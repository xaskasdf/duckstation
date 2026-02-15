// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#include "vr_player_tracker.h"

#ifdef ENABLE_OPENXR

#include "common/log.h"
#include "core/cpu_core.h"
#include "core/gte_types.h"
#include "core/controller.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/screenshot_3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <unordered_map>

LOG_CHANNEL(VR);

// Math constants
static constexpr float PI = 3.14159265358979323846f;
static constexpr float TWO_PI = 2.0f * PI;

namespace VR {

std::unique_ptr<PlayerTracker> g_vr_player_tracker;

// Static helper methods

float PlayerTracker::SmoothValue(float current, float target, float alpha)
{
  // Exponential Moving Average: new = old + alpha * (target - old)
  return current + alpha * (target - current);
}

void PlayerTracker::SmoothPosition(float* current, const float* target, float alpha)
{
  current[0] = SmoothValue(current[0], target[0], alpha);
  current[1] = SmoothValue(current[1], target[1], alpha);
  current[2] = SmoothValue(current[2], target[2], alpha);
}

float PlayerTracker::NormalizeAngle(float angle)
{
  // Normalize to -PI to PI range
  while (angle > PI)
    angle -= TWO_PI;
  while (angle < -PI)
    angle += TWO_PI;
  return angle;
}

float PlayerTracker::SmoothRotation(float current, float target, float alpha)
{
  // Handle wraparound: find shortest path between angles
  float diff = NormalizeAngle(target - current);
  return NormalizeAngle(current + alpha * diff);
}

const char* PlayerTracker::GetMethodName(DetectionMethod method)
{
  switch (method)
  {
    case DetectionMethod::CameraAnalysis: return "CameraAnalysis";
    case DetectionMethod::MemoryDirect: return "MemoryDirect";
    case DetectionMethod::InputCorrelation: return "InputCorrelation";
    case DetectionMethod::ManualSelection: return "ManualSelection";
    case DetectionMethod::Fallback: return "Fallback";
    default: return "Unknown";
  }
}

std::string PlayerTracker::GetStatusString() const
{
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2);
  ss << "Method: " << GetMethodName(m_active_method);
  ss << " | Valid: " << (m_player_state.valid ? "Yes" : "No");
  ss << " | Conf: " << m_player_state.confidence;
  if (m_player_state.valid)
  {
    ss << " | Pos: (" << m_player_state.position[0] << ", "
       << m_player_state.position[1] << ", " << m_player_state.position[2] << ")";
    ss << " | Rot: " << (m_player_state.rotation_y * 180.0f / PI) << " deg";
  }
  if (m_camera_state.is_valid)
  {
    ss << " | Stable: " << m_camera_state.stable_frame_count << " frames";
  }
  if (m_centroid_valid)
  {
    ss << " | Centroid: (" << m_smoothed_centroid_cam[0] << ", "
       << m_smoothed_centroid_cam[1] << ", " << m_smoothed_centroid_cam[2] << ")";
    ss << " | CStable: " << m_centroid_stable_frames;
  }
  else
  {
    ss << " | Centroid: Not detected";
  }
  return ss.str();
}

PlayerTracker::PlayerTracker() = default;
PlayerTracker::~PlayerTracker() = default;

bool PlayerTracker::Initialize()
{
  if (m_initialized)
    return true;

  // Load settings
  m_first_person_enabled = g_settings.vr_first_person_enable;
  m_eye_height = g_settings.vr_eye_height;
  m_world_scale = g_settings.vr_world_scale;

  // Load known game memory layouts
  LoadMemoryLayouts();

  // Initialize state
  std::memset(&m_player_state, 0, sizeof(m_player_state));
  std::memset(m_last_camera_pos, 0, sizeof(m_last_camera_pos));
  std::memset(m_camera_velocity, 0, sizeof(m_camera_velocity));

  // Reset camera state
  m_camera_state = CameraState{};
  m_camera_state.is_valid = false;
  m_camera_state.stable_frame_count = 0;
  m_has_established_position = false;  // Will be set true on first valid camera data
  m_consecutive_skips = 0;

  // Reset hysteresis state for first-person mode
  m_first_person_active = false;
  m_mode_switch_counter = 0;

  m_input_history.clear();
  m_input_history.reserve(INPUT_HISTORY_SIZE);

  // Reset input correlation state
  m_tracked_objects.clear();
  m_tracked_objects.reserve(MAX_TRACKED_OBJECTS);
  m_best_candidate = nullptr;
  m_best_correlation_score = 0.0f;
  m_correlation_stable_frames = 0;

  // Reset centroid detection state
  std::memset(m_player_centroid_cam, 0, sizeof(m_player_centroid_cam));
  std::memset(m_smoothed_centroid_cam, 0, sizeof(m_smoothed_centroid_cam));
  m_centroid_valid = false;
  m_centroid_stable_frames = 0;
  m_centroid_skip_count = 0;

  m_current_layout = nullptr;
  m_current_game_serial.clear();
  m_last_update_frame = 0;

  m_initialized = true;
  INFO_LOG("VR Player Tracker initialized (first_person={}, eye_height={:.3f}, world_scale={:.6f})",
                 m_first_person_enabled ? "true" : "false", m_eye_height, m_world_scale);
  return true;
}

void PlayerTracker::Shutdown()
{
  if (!m_initialized)
    return;

  m_input_history.clear();
  m_tracked_objects.clear();
  m_best_candidate = nullptr;
  m_memory_layouts.clear();
  m_current_layout = nullptr;
  m_initialized = false;

  INFO_LOG("VR Player Tracker shutdown");
}

void PlayerTracker::Update()
{
  if (!m_initialized)
    return;

  // Check frame counter to avoid multiple updates per frame
  u32 current_frame = Screenshot3D::GetFrameCounter();
  if (current_frame == m_last_update_frame)
    return;
  m_last_update_frame = current_frame;

  // Update input history for correlation detection
  UpdateInputHistory();

  // Check if game changed and update memory layout
  std::string serial = System::GetGameSerial();
  if (serial != m_current_game_serial)
  {
    m_current_game_serial = serial;
    m_current_layout = FindLayoutForGame(serial);
    if (m_current_layout)
    {
      INFO_LOG("VR: Found memory layout for game {}", serial.c_str());
    }
  }

  // Detection runs in parallel layers:
  // 1. Camera analysis always runs → provides rotation data for game_to_world
  // 2. Centroid detection always runs → provides position offset for view matrix
  // 3. Memory override (if available) → most accurate position source

  // 1. Camera analysis (provides rotation and base validity)
  bool detected = DetectFromCameraTransform();
  if (detected)
    m_active_method = DetectionMethod::CameraAnalysis;

  // 2. Centroid detection from geometry (provides player position offset)
  if (Screenshot3D::IsGeometryReady())
  {
    ClusterPolygonsIntoCentroids();
    UpdateCorrelationScores();
    ScoreTrackedObjects();
    SelectBestPlayerCluster();
  }

  // 3. Memory-based override (highest accuracy when available)
  if (m_current_layout)
  {
    bool mem_detected = DetectFromMemory();
    if (mem_detected)
    {
      detected = true;
      m_active_method = DetectionMethod::MemoryDirect;
    }
  }

  if (!detected)
  {
    m_player_state.valid = false;
    m_active_method = DetectionMethod::Fallback;
  }

  // Apply hysteresis to first-person mode switching to prevent rapid flickering
  // between first-person and fixed camera modes when GTE data becomes momentarily invalid
  bool want_first_person = m_first_person_enabled && m_player_state.valid && m_player_state.confidence > 0.3f;

  if (want_first_person != m_first_person_active)
  {
    // Mode change requested - increment counter
    m_mode_switch_counter++;

    // Log every 30 frames (~0.5sec) when in transition
    if (m_mode_switch_counter % 30 == 1)
    {
      INFO_LOG("VR: Mode change pending: want={} active={} counter={}/{} (valid={} conf={:.2f})",
                     want_first_person ? "FPV" : "fixed",
                     m_first_person_active ? "FPV" : "fixed",
                     m_mode_switch_counter, MODE_SWITCH_FRAMES,
                     m_player_state.valid ? "Y" : "N",
                     m_player_state.confidence);
    }

    if (m_mode_switch_counter >= MODE_SWITCH_FRAMES)
    {
      // Enough consecutive frames - confirm the mode switch
      m_first_person_active = want_first_person;
      m_mode_switch_counter = 0;
      INFO_LOG("VR: First-person mode {} (after {} stable frames)",
                     m_first_person_active ? "ENABLED" : "DISABLED", MODE_SWITCH_FRAMES);
    }
  }
  else
  {
    // States match - reset counter (only log if we were counting)
    if (m_mode_switch_counter > 0)
    {
      INFO_LOG("VR: Mode change cancelled at counter={} (state now matches)", m_mode_switch_counter);
    }
    m_mode_switch_counter = 0;
  }
}

bool PlayerTracker::IsFirstPersonAvailable() const
{
  return m_initialized && m_player_state.valid && m_player_state.confidence > 0.3f;
}

void PlayerTracker::GetFirstPersonViewOffset(float* out_position, float* out_rotation_y)
{
  if (m_player_state.valid)
  {
    out_position[0] = m_player_state.position[0];
    out_position[1] = m_player_state.position[1];
    out_position[2] = m_player_state.position[2];
    *out_rotation_y = m_player_state.rotation_y;
  }
  else
  {
    out_position[0] = 0.0f;
    out_position[1] = 0.0f;
    out_position[2] = 0.0f;
    *out_rotation_y = 0.0f;
  }
}

bool PlayerTracker::GetPlayerCentroidCameraSpace(float* out_cx, float* out_cy, float* out_cz) const
{
  if (!m_centroid_valid)
    return false;

  *out_cx = m_smoothed_centroid_cam[0];
  *out_cy = m_smoothed_centroid_cam[1];
  *out_cz = m_smoothed_centroid_cam[2];
  return true;
}

void PlayerTracker::ExtractCameraFromGTE(float* out_position, float* out_rotation_matrix)
{
  // Access GTE registers from CPU state
  // TR = Translation vector (camera position in world space)
  // RT = Rotation matrix (3x3, camera orientation)

  const GTE::Regs& gte = CPU::g_state.gte_regs;

  // TR values are 32-bit signed integers representing world coordinates
  out_position[0] = static_cast<float>(gte.TR[0]);
  out_position[1] = static_cast<float>(gte.TR[1]);
  out_position[2] = static_cast<float>(gte.TR[2]);

  // RT is 3x3 rotation matrix stored as signed 16-bit values in 4.12 fixed point
  // Convert to float by dividing by 4096
  for (int row = 0; row < 3; row++)
  {
    for (int col = 0; col < 3; col++)
    {
      out_rotation_matrix[row * 3 + col] =
        static_cast<float>(gte.RT[row][col]) / 4096.0f;
    }
  }
}

bool PlayerTracker::ValidateCameraTransform(const float* position, const float* rotation_matrix)
{
  // Check 1: Zero position typically means GTE not active (menus, 2D sections)
  if (position[0] == 0.0f && position[1] == 0.0f && position[2] == 0.0f)
  {
    return false;
  }

  // Check 2: Validate rotation matrix (should have determinant close to +1 or -1)
  // For a valid rotation matrix, each row/column should be unit length
  // Quick check: sum of squares of first row should be close to 1.0
  float row0_len_sq = rotation_matrix[0] * rotation_matrix[0] +
                      rotation_matrix[1] * rotation_matrix[1] +
                      rotation_matrix[2] * rotation_matrix[2];

  // Allow some tolerance for fixed-point conversion errors
  if (row0_len_sq < 0.5f || row0_len_sq > 1.5f)
  {
    // Matrix not normalized - likely invalid or transitioning
    return false;
  }

  // Check 3: Position within reasonable bounds (avoid extreme values)
  // PS1 coordinate space is typically within +/- 1,000,000 units
  const float MAX_COORD = 1000000.0f;
  if (std::abs(position[0]) > MAX_COORD ||
      std::abs(position[1]) > MAX_COORD ||
      std::abs(position[2]) > MAX_COORD)
  {
    return false;
  }

  return true;
}

bool PlayerTracker::DetectFromCameraTransform()
{
  float cam_pos[3];
  float cam_rot[9];

  // Prefer Screenshot3D camera data (validated, captured at first GTE call per frame)
  // over direct GTE register reads (noisy, reads whichever object last used GTE)
  if (!Screenshot3D::GetCameraTransform(cam_pos, cam_rot))
  {
    // Fallback to direct GTE if Screenshot3D not running
    ExtractCameraFromGTE(cam_pos, cam_rot);
  }

  // Validate the camera transform
  if (!ValidateCameraTransform(cam_pos, cam_rot))
  {
    // Invalid camera data - could be menu, 2D section, or scene transition
    // If we have an established position, keep using it (don't fail)
    if (m_has_established_position)
    {
      // Just skip this frame's data, keep using previous smoothed values
      // Don't log every frame - too spammy
      return true;  // Return true because we still have valid smoothed data
    }

    // No established position yet - truly fail
    m_camera_state.is_valid = false;
    m_camera_state.stable_frame_count = 0;
    return false;
  }

  // Extract yaw rotation from rotation matrix
  // Forward vector from rotation matrix: RT[0][2], RT[1][2], RT[2][2]
  float forward_x = cam_rot[2];  // RT[0][2]
  float forward_z = cam_rot[8];  // RT[2][2]
  float raw_rotation_y = std::atan2(forward_x, forward_z);

  // First valid frame ever - initialize smoothed values directly
  // After this, we never reinitialize - just skip bad data
  if (!m_has_established_position)
  {
    m_camera_state.position[0] = cam_pos[0];
    m_camera_state.position[1] = cam_pos[1];
    m_camera_state.position[2] = cam_pos[2];
    m_camera_state.rotation_y = raw_rotation_y;
    m_camera_state.smoothed_position[0] = cam_pos[0];
    m_camera_state.smoothed_position[1] = cam_pos[1];
    m_camera_state.smoothed_position[2] = cam_pos[2];
    m_camera_state.smoothed_rotation_y = raw_rotation_y;
    m_camera_state.stable_frame_count = 0;
    m_camera_state.is_valid = true;
    m_has_established_position = true;  // Never reinitialize after this
    m_last_camera_pos[0] = cam_pos[0];
    m_last_camera_pos[1] = cam_pos[1];
    m_last_camera_pos[2] = cam_pos[2];
    INFO_LOG("VR: Camera ESTABLISHED at ({:.0f}, {:.0f}, {:.0f}) - will maintain this position",
                   cam_pos[0], cam_pos[1], cam_pos[2]);
    // Continue to output section below
  }
  else
  {
    // Check for position jumps FROM SMOOTHED position (not raw)
    // This prevents accepting alternate camera views that pass validation
    float dx = cam_pos[0] - m_camera_state.smoothed_position[0];
    float dy = cam_pos[1] - m_camera_state.smoothed_position[1];
    float dz = cam_pos[2] - m_camera_state.smoothed_position[2];
    float dist_sq = dx * dx + dy * dy + dz * dz;

    // Use tighter threshold: 500 PS1 units = 0.5m at default scale
    // This rejects alternate camera views while allowing normal movement
    const float POSITION_JUMP_THRESHOLD = 500.0f;

    if (dist_sq > POSITION_JUMP_THRESHOLD * POSITION_JUMP_THRESHOLD)
    {
      // Position jumped too far from smoothed - count consecutive skips
      m_consecutive_skips++;

      if (m_consecutive_skips >= MAX_CONSECUTIVE_SKIPS)
      {
        // Too many consecutive skips - established position is probably wrong
        // Re-establish at this new position
        INFO_LOG("VR: Too many skips ({}), RE-ESTABLISHING at ({:.0f}, {:.0f}, {:.0f})",
                       m_consecutive_skips, cam_pos[0], cam_pos[1], cam_pos[2]);
        m_camera_state.smoothed_position[0] = cam_pos[0];
        m_camera_state.smoothed_position[1] = cam_pos[1];
        m_camera_state.smoothed_position[2] = cam_pos[2];
        m_camera_state.position[0] = cam_pos[0];
        m_camera_state.position[1] = cam_pos[1];
        m_camera_state.position[2] = cam_pos[2];
        m_consecutive_skips = 0;
        m_camera_state.stable_frame_count = 0;
      }
      else
      {
        // Skip this update, keep using previous smoothed values
        static u32 s_skip_log_counter = 0;
        if (++s_skip_log_counter % 60 == 1) // Log once per second
        {
          INFO_LOG("VR: Skipping camera jump (dist={:.0f}, skips={}/{})",
                         std::sqrt(dist_sq), m_consecutive_skips, MAX_CONSECUTIVE_SKIPS);
        }
      }
      // Still return true - we have valid data
    }
    else
    {
      // Position is within threshold - reset skip counter
      m_consecutive_skips = 0;

      // Check for rotation jumps
      float rot_diff = std::abs(NormalizeAngle(raw_rotation_y - m_camera_state.smoothed_rotation_y));
      bool rotation_jumped = (rot_diff > m_max_rotation_jump);

      if (rotation_jumped)
      {
        // Rotation jumped - skip rotation update but allow position
        static u32 s_rot_skip_counter = 0;
        if (++s_rot_skip_counter % 60 == 1)
        {
          INFO_LOG("VR: Skipping rotation jump (diff={:.1f} deg)", rot_diff * 180.0f / PI);
        }
      }

      // Update raw values
      m_camera_state.position[0] = cam_pos[0];
      m_camera_state.position[1] = cam_pos[1];
      m_camera_state.position[2] = cam_pos[2];
      if (!rotation_jumped)
      {
        m_camera_state.rotation_y = raw_rotation_y;
      }

      // Calculate velocity
      float dt = 1.0f / 60.0f; // Assume 60fps
      for (int i = 0; i < 3; i++)
      {
        m_camera_state.velocity[i] = (cam_pos[i] - m_last_camera_pos[i]) / dt;
        m_last_camera_pos[i] = cam_pos[i];
      }

      // Apply smoothing with ramp-up
      m_camera_state.stable_frame_count++;

      float smoothing_factor = 1.0f;
      if (m_camera_state.stable_frame_count < STABILITY_FRAMES)
      {
        smoothing_factor = static_cast<float>(m_camera_state.stable_frame_count) /
                           static_cast<float>(STABILITY_FRAMES);
      }

      float pos_alpha = 1.0f - (1.0f - m_position_smoothing) * smoothing_factor;
      float rot_alpha = 1.0f - (1.0f - m_rotation_smoothing) * smoothing_factor;

      // Apply EMA smoothing
      SmoothPosition(m_camera_state.smoothed_position, cam_pos, pos_alpha);
      if (!rotation_jumped)
      {
        m_camera_state.smoothed_rotation_y = SmoothRotation(
          m_camera_state.smoothed_rotation_y, raw_rotation_y, rot_alpha);
      }
    }
  }

  // Convert smoothed PS1 units to meters for output
  m_player_state.position[0] = m_camera_state.smoothed_position[0] * m_world_scale;
  m_player_state.position[1] = m_camera_state.smoothed_position[1] * m_world_scale;
  m_player_state.position[2] = -m_camera_state.smoothed_position[2] * m_world_scale; // Flip Z for OpenXR

  m_player_state.rotation_y = m_camera_state.smoothed_rotation_y;
  m_player_state.valid = true;

  // Confidence based on stability
  if (m_camera_state.stable_frame_count >= STABILITY_FRAMES)
  {
    m_player_state.confidence = 0.8f; // Good confidence when stable
  }
  else
  {
    // Lower confidence during ramp-up
    m_player_state.confidence = 0.5f + 0.3f *
      (static_cast<float>(m_camera_state.stable_frame_count) / static_cast<float>(STABILITY_FRAMES));
  }

  // Debug logging
  if (m_debug_logging && (m_last_update_frame % 60 == 0))
  {
    INFO_LOG("VR Camera: pos=({:.0f},{:.0f},{:.0f}) smooth=({:.0f},{:.0f},{:.0f}) rot={:.1f} deg stable={}",
                   cam_pos[0], cam_pos[1], cam_pos[2],
                   m_camera_state.smoothed_position[0],
                   m_camera_state.smoothed_position[1],
                   m_camera_state.smoothed_position[2],
                   m_camera_state.smoothed_rotation_y * 180.0f / PI,
                   m_camera_state.stable_frame_count);
  }

  return true;
}

bool PlayerTracker::DetectFromMemory()
{
  if (!m_current_layout)
    return false;

  // Verify game version if validation address is set
  if (m_current_layout->validation_addr != 0)
  {
    u32 validation_val = 0;
    if (!CPU::SafeReadMemoryWord(m_current_layout->validation_addr, &validation_val) ||
        validation_val != m_current_layout->validation_value)
    {
      // Wrong game version or memory not ready
      return false;
    }
  }

  // Determine which addresses to use (camera or player)
  u32 x_addr = m_current_layout->use_camera_position ?
               m_current_layout->camera_x_addr : m_current_layout->player_x_addr;
  u32 y_addr = m_current_layout->use_camera_position ?
               m_current_layout->camera_y_addr : m_current_layout->player_y_addr;
  u32 z_addr = m_current_layout->use_camera_position ?
               m_current_layout->camera_z_addr : m_current_layout->player_z_addr;
  u32 angle_addr = m_current_layout->use_camera_position ?
                   m_current_layout->camera_angle_addr : m_current_layout->player_angle_addr;

  if (x_addr == 0 || y_addr == 0 || z_addr == 0)
    return false;

  u32 raw_x = 0, raw_y = 0, raw_z = 0;

  // Read position based on address size
  bool read_ok = false;
  if (m_current_layout->addr_size == GameMemoryLayout::AddressSize::HalfWord16)
  {
    u16 hx = 0, hy = 0, hz = 0;
    read_ok = CPU::SafeReadMemoryHalfWord(x_addr, &hx) &&
              CPU::SafeReadMemoryHalfWord(y_addr, &hy) &&
              CPU::SafeReadMemoryHalfWord(z_addr, &hz);
    // Sign extend 16-bit values
    raw_x = static_cast<u32>(static_cast<s16>(hx));
    raw_y = static_cast<u32>(static_cast<s16>(hy));
    raw_z = static_cast<u32>(static_cast<s16>(hz));
  }
  else
  {
    read_ok = CPU::SafeReadMemoryWord(x_addr, &raw_x) &&
              CPU::SafeReadMemoryWord(y_addr, &raw_y) &&
              CPU::SafeReadMemoryWord(z_addr, &raw_z);
  }

  if (!read_ok)
    return false;

  // Convert from game format
  float x, y, z;
  if (m_current_layout->uses_fixed_point)
  {
    float divisor = static_cast<float>(1 << m_current_layout->fixed_point_shift);
    x = static_cast<float>(static_cast<s32>(raw_x)) / divisor;
    y = static_cast<float>(static_cast<s32>(raw_y)) / divisor;
    z = static_cast<float>(static_cast<s32>(raw_z)) / divisor;
  }
  else
  {
    x = static_cast<float>(static_cast<s32>(raw_x));
    y = static_cast<float>(static_cast<s32>(raw_y));
    z = static_cast<float>(static_cast<s32>(raw_z));
  }

  // Apply scale and convert to meters
  m_player_state.position[0] = x * m_current_layout->scale_factor * m_world_scale;
  m_player_state.position[1] = y * m_current_layout->scale_factor * m_world_scale;
  m_player_state.position[2] = -z * m_current_layout->scale_factor * m_world_scale; // Flip Z

  // Read rotation if available
  if (angle_addr != 0)
  {
    u32 raw_angle = 0;
    bool angle_ok = false;

    if (m_current_layout->addr_size == GameMemoryLayout::AddressSize::HalfWord16)
    {
      u16 ha = 0;
      angle_ok = CPU::SafeReadMemoryHalfWord(angle_addr, &ha);
      raw_angle = static_cast<u32>(static_cast<s16>(ha));
    }
    else
    {
      angle_ok = CPU::SafeReadMemoryWord(angle_addr, &raw_angle);
    }

    if (angle_ok)
    {
      // PS1 angles: 0-4096 = 0-360 degrees (4.12 fixed point for full rotation)
      float angle_deg = static_cast<float>(static_cast<s16>(raw_angle)) * 360.0f / 4096.0f;
      m_player_state.rotation_y = angle_deg * (PI / 180.0f);
    }
  }

  m_player_state.valid = true;
  m_player_state.confidence = 0.95f; // High confidence for memory-based detection

  if (m_debug_logging && (m_last_update_frame % 60 == 0))
  {
    INFO_LOG("VR Memory [{}]: pos=({:.2f},{:.2f},{:.2f}) rot={:.1f} deg",
                   m_current_layout->game_name.c_str(),
                   m_player_state.position[0], m_player_state.position[1],
                   m_player_state.position[2],
                   m_player_state.rotation_y * 180.0f / PI);
  }

  return true;
}

void PlayerTracker::GetAverageInputDirection(float* out_dir_x, float* out_dir_z, size_t frames)
{
  *out_dir_x = 0.0f;
  *out_dir_z = 0.0f;

  if (m_input_history.empty())
    return;

  size_t count = std::min(frames, m_input_history.size());
  size_t start = m_input_history.size() - count;

  for (size_t i = start; i < m_input_history.size(); i++)
  {
    *out_dir_x += m_input_history[i].left_x;
    *out_dir_z += m_input_history[i].left_y; // Y stick maps to Z movement
  }

  *out_dir_x /= static_cast<float>(count);
  *out_dir_z /= static_cast<float>(count);
}

PlayerTracker::TrackedObject* PlayerTracker::FindOrCreateTrackedObject(
  const float* centroid, u32 polygon_count, u32 frame)
{
  // Find existing tracked object near this centroid
  TrackedObject* best_match = nullptr;
  float best_dist_sq = CLUSTER_DISTANCE_THRESHOLD * CLUSTER_DISTANCE_THRESHOLD;

  for (auto& obj : m_tracked_objects)
  {
    float dx = centroid[0] - obj.centroid[0];
    float dy = centroid[1] - obj.centroid[1];
    float dz = centroid[2] - obj.centroid[2];
    float dist_sq = dx * dx + dy * dy + dz * dz;

    if (dist_sq < best_dist_sq)
    {
      best_dist_sq = dist_sq;
      best_match = &obj;
    }
  }

  if (best_match)
  {
    // Update existing object
    best_match->prev_centroid[0] = best_match->centroid[0];
    best_match->prev_centroid[1] = best_match->centroid[1];
    best_match->prev_centroid[2] = best_match->centroid[2];

    best_match->centroid[0] = centroid[0];
    best_match->centroid[1] = centroid[1];
    best_match->centroid[2] = centroid[2];

    // Calculate velocity
    float dt = 1.0f / 60.0f;
    best_match->velocity[0] = (best_match->centroid[0] - best_match->prev_centroid[0]) / dt;
    best_match->velocity[1] = (best_match->centroid[1] - best_match->prev_centroid[1]) / dt;
    best_match->velocity[2] = (best_match->centroid[2] - best_match->prev_centroid[2]) / dt;

    best_match->polygon_count = polygon_count;
    best_match->last_seen_frame = frame;
    return best_match;
  }

  // Create new tracked object if under limit
  if (m_tracked_objects.size() < MAX_TRACKED_OBJECTS)
  {
    TrackedObject new_obj;
    new_obj.centroid[0] = new_obj.prev_centroid[0] = centroid[0];
    new_obj.centroid[1] = new_obj.prev_centroid[1] = centroid[1];
    new_obj.centroid[2] = new_obj.prev_centroid[2] = centroid[2];
    new_obj.velocity[0] = new_obj.velocity[1] = new_obj.velocity[2] = 0.0f;
    new_obj.correlation_score = 0.0f;
    new_obj.polygon_count = polygon_count;
    new_obj.last_seen_frame = frame;
    new_obj.is_candidate = false;

    m_tracked_objects.push_back(new_obj);
    return &m_tracked_objects.back();
  }

  return nullptr;
}

void PlayerTracker::ClusterPolygonsIntoCentroids()
{
  u32 current_frame = Screenshot3D::GetFrameCounter();

  // Remove stale tracked objects (not seen for 10+ frames)
  m_tracked_objects.erase(
    std::remove_if(m_tracked_objects.begin(), m_tracked_objects.end(),
      [current_frame](const TrackedObject& obj) {
        return (current_frame - obj.last_seen_frame) > 10;
      }),
    m_tracked_objects.end());

  const auto& polygons = Screenshot3D::GetPolygonBuffer();
  if (polygons.empty())
    return;

  // Spatial hash grid: cell size 200 PS1 units
  static constexpr float CELL_SIZE = 200.0f;
  static constexpr float INV_CELL_SIZE = 1.0f / CELL_SIZE;
  static constexpr u32 MIN_POLYS_PER_CELL = 5;

  struct CellData
  {
    float sum_x = 0, sum_y = 0, sum_z = 0;     // Sum of 3D centroids
    float sum_sx = 0, sum_sy = 0;               // Sum of screen centroids
    u32 count = 0;
  };

  // Hash function for 3D grid cell keys
  struct GridKeyHash
  {
    size_t operator()(const std::tuple<s32, s32, s32>& k) const
    {
      size_t h = std::hash<s32>()(std::get<0>(k));
      h ^= std::hash<s32>()(std::get<1>(k)) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<s32>()(std::get<2>(k)) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  std::unordered_map<std::tuple<s32, s32, s32>, CellData, GridKeyHash> grid;

  for (const auto& poly : polygons)
  {
    if (!poly.has_3d_verts)
      continue;

    const int nv = poly.NumVerts();

    // Compute 3D centroid from v_3d
    float cx = 0, cy = 0, cz = 0;
    for (int i = 0; i < nv; i++)
    {
      cx += poly.v_3d[i][0];
      cy += poly.v_3d[i][1];
      cz += poly.v_3d[i][2];
    }
    cx /= static_cast<float>(nv);
    cy /= static_cast<float>(nv);
    cz /= static_cast<float>(nv);

    // Skip polygons too close to camera (Z < 100) or very far (Z > 5000)
    if (cz < 100.0f || cz > 5000.0f)
      continue;

    // Compute screen centroid from vertex screen coords
    // v[i].x and v[i].y are in PS1 screen coordinates (0-320, 0-240 typical)
    float sx = 0, sy = 0;
    for (int i = 0; i < nv; i++)
    {
      sx += static_cast<float>(poly.v[i].x);
      sy += static_cast<float>(poly.v[i].y);
    }
    sx /= static_cast<float>(nv);
    sy /= static_cast<float>(nv);
    // Normalize: PS1 screen is typically 320x240
    sx /= 320.0f;
    sy /= 240.0f;

    // Compute grid cell key
    s32 gx = static_cast<s32>(std::floor(cx * INV_CELL_SIZE));
    s32 gy = static_cast<s32>(std::floor(cy * INV_CELL_SIZE));
    s32 gz = static_cast<s32>(std::floor(cz * INV_CELL_SIZE));

    auto key = std::make_tuple(gx, gy, gz);
    auto& cell = grid[key];
    cell.sum_x += cx;
    cell.sum_y += cy;
    cell.sum_z += cz;
    cell.sum_sx += sx;
    cell.sum_sy += sy;
    cell.count++;
  }

  // Process grid cells with enough polygons into tracked objects
  for (const auto& [key, cell] : grid)
  {
    if (cell.count < MIN_POLYS_PER_CELL)
      continue;

    float inv_count = 1.0f / static_cast<float>(cell.count);
    float centroid[3] = {
      cell.sum_x * inv_count,
      cell.sum_y * inv_count,
      cell.sum_z * inv_count
    };

    TrackedObject* obj = FindOrCreateTrackedObject(centroid, cell.count, current_frame);
    if (obj)
    {
      obj->avg_screen_x = cell.sum_sx * inv_count;
      obj->avg_screen_y = cell.sum_sy * inv_count;
    }
  }
}

void PlayerTracker::ScoreTrackedObjects()
{
  for (auto& obj : m_tracked_objects)
  {
    // Weight 0.4: Screen center score — player character is typically near screen center
    // avg_screen_x/y are normalized 0-1, center is (0.5, 0.5)
    float dx = obj.avg_screen_x - 0.5f;
    float dy = obj.avg_screen_y - 0.5f;
    float angular_offset = std::sqrt(dx * dx + dy * dy);
    float screen_score = std::max(0.0f, 1.0f - angular_offset / 0.5f);

    // Weight 0.2: Size score — character models are typically 15-300 polygons
    float size_score = 0.0f;
    if (obj.polygon_count >= 15 && obj.polygon_count <= 300)
      size_score = 1.0f;
    else if (obj.polygon_count >= 5 && obj.polygon_count < 15)
      size_score = static_cast<float>(obj.polygon_count - 5) / 10.0f;
    else if (obj.polygon_count > 300 && obj.polygon_count <= 600)
      size_score = 1.0f - static_cast<float>(obj.polygon_count - 300) / 300.0f;

    // Weight 0.2: Depth score — player is typically 300-3000 PS1 units from camera
    float depth = obj.centroid[2]; // Z in camera space
    float depth_score = 0.0f;
    if (depth >= 300.0f && depth <= 3000.0f)
      depth_score = 1.0f;
    else if (depth >= 100.0f && depth < 300.0f)
      depth_score = (depth - 100.0f) / 200.0f;
    else if (depth > 3000.0f && depth <= 5000.0f)
      depth_score = 1.0f - (depth - 3000.0f) / 2000.0f;

    // Weight 0.2: Correlation score (from input correlation, grows over time)
    float corr_score = std::max(0.0f, obj.correlation_score);

    obj.combined_score = 0.4f * screen_score + 0.2f * size_score +
                         0.2f * depth_score + 0.2f * corr_score;
  }
}

void PlayerTracker::SelectBestPlayerCluster()
{
  static constexpr float MIN_COMBINED_SCORE = 0.3f;
  // Once centroid is established, reject candidates further than this from the smoothed
  // centroid. Camera injection changes which polygons are visible based on VR head
  // direction, causing different clusters to appear/disappear. Without this filter the
  // viewpoint teleports between clusters when the user looks around.
  static constexpr float CENTROID_JUMP_THRESHOLD = 400.0f; // PS1 units
  static constexpr float CENTROID_JUMP_THRESHOLD_SQ = CENTROID_JUMP_THRESHOLD * CENTROID_JUMP_THRESHOLD;
  static constexpr u32 CENTROID_LOCK_FRAMES = 15;          // Frames before jump filtering activates
  static constexpr u32 CENTROID_MAX_SKIPS = 600;            // ~10 seconds before re-establish

  // Quality criteria for re-establishment: must look like a character model,
  // not room geometry (walls, floor, ceiling).
  static constexpr float REESTABLISH_MIN_SCORE = 0.35f;
  static constexpr float REESTABLISH_MIN_Z = 150.0f;       // Not too close to camera
  static constexpr float REESTABLISH_MAX_Z = 4000.0f;      // Not too far from camera
  static constexpr u32 REESTABLISH_MIN_POLYS = 10;
  static constexpr u32 REESTABLISH_MAX_POLYS = 400;

  TrackedObject* best = nullptr;
  float best_score = MIN_COMBINED_SCORE;

  for (auto& obj : m_tracked_objects)
  {
    if (obj.combined_score > best_score)
    {
      best_score = obj.combined_score;
      best = &obj;
    }
  }

  if (!best)
  {
    // No cluster passes minimum score threshold — keep previous centroid
    return;
  }

  float candidate[3] = {best->centroid[0], best->centroid[1], best->centroid[2]};

  // Position-jump filtering: once established, reject far candidates
  if (m_centroid_valid && m_centroid_stable_frames >= CENTROID_LOCK_FRAMES)
  {
    float dx = candidate[0] - m_smoothed_centroid_cam[0];
    float dy = candidate[1] - m_smoothed_centroid_cam[1];
    float dz = candidate[2] - m_smoothed_centroid_cam[2];
    float dist_sq = dx * dx + dy * dy + dz * dz;

    if (dist_sq > CENTROID_JUMP_THRESHOLD_SQ)
    {
      // Candidate too far — count consecutive skips
      m_centroid_skip_count++;

      if (m_centroid_skip_count >= CENTROID_MAX_SKIPS)
      {
        // Candidate must pass quality check to re-establish — reject room geometry
        // (walls with huge polygon counts, geometry too close/far from camera)
        bool passes_quality = (best_score >= REESTABLISH_MIN_SCORE) &&
                              (candidate[2] >= REESTABLISH_MIN_Z) &&
                              (candidate[2] <= REESTABLISH_MAX_Z) &&
                              (best->polygon_count >= REESTABLISH_MIN_POLYS) &&
                              (best->polygon_count <= REESTABLISH_MAX_POLYS);

        if (passes_quality)
        {
          // Player probably moved (scene change, long walk). Re-establish.
          INFO_LOG("VR Centroid: Re-establishing after {} skips at ({:.0f},{:.0f},{:.0f}) score={:.2f} polys={}",
                         m_centroid_skip_count, candidate[0], candidate[1], candidate[2],
                         best_score, best->polygon_count);
          m_smoothed_centroid_cam[0] = candidate[0];
          m_smoothed_centroid_cam[1] = candidate[1];
          m_smoothed_centroid_cam[2] = candidate[2];
          m_player_centroid_cam[0] = candidate[0];
          m_player_centroid_cam[1] = candidate[1];
          m_player_centroid_cam[2] = candidate[2];
          m_centroid_stable_frames = 1;
          m_centroid_skip_count = 0;
        }
        // else: candidate doesn't look like a character model, keep skipping
      }
      // Otherwise keep previous smoothed centroid
      return;
    }

    // Within threshold — reset skip counter
    m_centroid_skip_count = 0;
  }

  // Store raw centroid
  m_player_centroid_cam[0] = candidate[0];
  m_player_centroid_cam[1] = candidate[1];
  m_player_centroid_cam[2] = candidate[2];

  // Apply EMA smoothing
  // Use faster alpha (0.3) during first 30 frames for initial convergence,
  // then slower alpha (0.1) for stable tracking
  float alpha = (m_centroid_stable_frames < 30) ? 0.3f : 0.1f;

  if (!m_centroid_valid)
  {
    // First detection — apply quality check before committing
    bool passes_quality = (best_score >= REESTABLISH_MIN_SCORE) &&
                          (candidate[2] >= REESTABLISH_MIN_Z) &&
                          (candidate[2] <= REESTABLISH_MAX_Z) &&
                          (best->polygon_count >= REESTABLISH_MIN_POLYS) &&
                          (best->polygon_count <= REESTABLISH_MAX_POLYS);
    if (!passes_quality)
      return;

    // Snap directly
    m_smoothed_centroid_cam[0] = candidate[0];
    m_smoothed_centroid_cam[1] = candidate[1];
    m_smoothed_centroid_cam[2] = candidate[2];
    m_centroid_valid = true;
    m_centroid_stable_frames = 1;
    m_centroid_skip_count = 0;
    INFO_LOG("VR Centroid: ESTABLISHED at ({:.0f},{:.0f},{:.0f}) score={:.2f} polys={}",
                   candidate[0], candidate[1], candidate[2], best_score, best->polygon_count);
  }
  else
  {
    SmoothPosition(m_smoothed_centroid_cam, m_player_centroid_cam, alpha);
    m_centroid_stable_frames++;
  }

  if (m_debug_logging && (m_last_update_frame % 60 == 0))
  {
    INFO_LOG("VR Centroid: raw=({:.0f},{:.0f},{:.0f}) smooth=({:.0f},{:.0f},{:.0f}) score={:.2f} polys={} stable={}",
                   m_player_centroid_cam[0], m_player_centroid_cam[1], m_player_centroid_cam[2],
                   m_smoothed_centroid_cam[0], m_smoothed_centroid_cam[1], m_smoothed_centroid_cam[2],
                   best_score, best->polygon_count, m_centroid_stable_frames);
  }
}

void PlayerTracker::UpdateCorrelationScores()
{
  // Get average input direction
  float input_x = 0, input_z = 0;
  GetAverageInputDirection(&input_x, &input_z, 5);

  // Normalize input direction
  float input_len = std::sqrt(input_x * input_x + input_z * input_z);
  if (input_len < 0.2f)
  {
    // No significant input - can't correlate
    return;
  }
  input_x /= input_len;
  input_z /= input_len;

  for (auto& obj : m_tracked_objects)
  {
    // Calculate object's movement direction (XZ plane)
    float vel_x = obj.velocity[0];
    float vel_z = obj.velocity[2];
    float vel_len = std::sqrt(vel_x * vel_x + vel_z * vel_z);

    if (vel_len < 10.0f) // Minimum velocity threshold
    {
      // Object not moving significantly - decrease correlation
      obj.correlation_score *= 0.95f;
      continue;
    }

    // Normalize velocity
    vel_x /= vel_len;
    vel_z /= vel_len;

    // Compute dot product (cosine similarity)
    float dot = input_x * vel_x + input_z * vel_z;

    // Update correlation score with EMA (70% old + 30% new)
    obj.correlation_score = 0.7f * obj.correlation_score + 0.3f * dot;

    // Mark as candidate if above threshold
    obj.is_candidate = (obj.correlation_score > CORRELATION_THRESHOLD);
  }
}

bool PlayerTracker::DetectFromInputCorrelation()
{
  // NOTE: Input correlation detection requires Screenshot3D to expose the Poly struct
  // via a public header. Currently the polygon geometry types are internal to
  // screenshot_3d.cpp, so this detection method is disabled.
  //
  // When the API is available, this method will:
  // 1. Cluster polygons into tracked objects based on spatial proximity
  // 2. Track movement of each cluster over time
  // 3. Correlate cluster movement direction with controller input
  // 4. Objects that move when player pushes stick are likely the player character

  // Need geometry to be available
  if (!Screenshot3D::IsGeometryReady())
    return false;

  // Step 1: Cluster polygons into tracked objects
  // (Currently stubbed - needs Screenshot3D::Poly exposure)
  ClusterPolygonsIntoCentroids();

  // Without polygon access, we won't have any tracked objects
  if (m_tracked_objects.empty())
    return false;

  // Step 2: Update correlation scores
  UpdateCorrelationScores();

  // Step 3: Find best candidate
  TrackedObject* best = nullptr;
  float best_score = CORRELATION_THRESHOLD;

  for (auto& obj : m_tracked_objects)
  {
    if (obj.is_candidate && obj.correlation_score > best_score)
    {
      best_score = obj.correlation_score;
      best = &obj;
    }
  }

  if (!best)
  {
    m_best_candidate = nullptr;
    m_correlation_stable_frames = 0;
    return false;
  }

  // Check if same candidate as before
  if (best == m_best_candidate)
  {
    m_correlation_stable_frames++;
  }
  else
  {
    m_best_candidate = best;
    m_best_correlation_score = best_score;
    m_correlation_stable_frames = 1;
  }

  // Only report as valid if stable for enough frames
  if (m_correlation_stable_frames < CORRELATION_STABILITY_THRESHOLD)
  {
    return false;
  }

  // Output player state from best candidate
  m_player_state.position[0] = best->centroid[0] * m_world_scale;
  m_player_state.position[1] = best->centroid[1] * m_world_scale;
  m_player_state.position[2] = -best->centroid[2] * m_world_scale; // Flip Z

  // Estimate rotation from velocity direction
  float vel_len = std::sqrt(best->velocity[0] * best->velocity[0] +
                            best->velocity[2] * best->velocity[2]);
  if (vel_len > 10.0f)
  {
    m_player_state.rotation_y = std::atan2(best->velocity[0], best->velocity[2]);
  }

  m_player_state.valid = true;
  m_player_state.confidence = 0.5f + 0.4f * best->correlation_score; // 0.5-0.9 based on correlation

  if (m_debug_logging && (m_last_update_frame % 60 == 0))
  {
    INFO_LOG("VR InputCorr: candidate at ({:.0f},{:.0f},{:.0f}) score={:.2f} polys={} stable={}",
                   best->centroid[0], best->centroid[1], best->centroid[2],
                   best->correlation_score, best->polygon_count,
                   m_correlation_stable_frames);
  }

  return true;
}

void PlayerTracker::UpdateInputHistory()
{
  Controller* controller = Pad::GetController(0);
  if (!controller)
    return;

  InputSample sample;
  sample.button_bits = controller->GetButtonStateBits();
  sample.frame = Screenshot3D::GetFrameCounter();

  // Get analog stick values (indices 0,1 for left stick typically)
  // These are 0.0-1.0 range, with 0.5 being center
  sample.left_x = controller->GetBindState(0) * 2.0f - 1.0f; // Convert to -1 to 1
  sample.left_y = controller->GetBindState(1) * 2.0f - 1.0f;

  // D-pad fallback if analog is centered
  if (std::abs(sample.left_x) < 0.2f && std::abs(sample.left_y) < 0.2f)
  {
    // PS1 button bits: Up=4, Right=5, Down=6, Left=7 (active low)
    u32 bits = sample.button_bits;
    if (!(bits & (1 << 5))) sample.left_x = 1.0f;   // Right
    if (!(bits & (1 << 7))) sample.left_x = -1.0f;  // Left
    if (!(bits & (1 << 6))) sample.left_y = 1.0f;   // Down
    if (!(bits & (1 << 4))) sample.left_y = -1.0f;  // Up
  }

  m_input_history.push_back(sample);
  if (m_input_history.size() > INPUT_HISTORY_SIZE)
  {
    m_input_history.erase(m_input_history.begin());
  }
}

void PlayerTracker::LoadMemoryLayouts()
{
  m_memory_layouts.clear();

  // Known game memory layouts
  // NOTE: These addresses are starting points from cheat databases and community research.
  // They need verification through actual testing on each game.
  // Memory addresses are for NTSC-U versions unless noted otherwise.

  // ============================================================
  // Crash Bandicoot (NTSC-U) - SCUS-94900
  // Player object position in world coordinates
  // ============================================================
  {
    GameMemoryLayout layout;
    layout.serial = "SCUS-94900";
    layout.game_name = "Crash Bandicoot";
    layout.player_x_addr = 0x8006E444;
    layout.player_y_addr = 0x8006E448;
    layout.player_z_addr = 0x8006E44C;
    layout.player_angle_addr = 0x8006E458;
    layout.use_camera_position = false;
    layout.uses_fixed_point = true;
    layout.fixed_point_shift = 8; // Crash uses 24.8 fixed point
    layout.scale_factor = 1.0f;
    layout.addr_size = GameMemoryLayout::AddressSize::Word32;
    m_memory_layouts.push_back(layout);
  }

  // ============================================================
  // Crash Bandicoot 2 (NTSC-U) - SCUS-94154
  // ============================================================
  {
    GameMemoryLayout layout;
    layout.serial = "SCUS-94154";
    layout.game_name = "Crash Bandicoot 2";
    layout.player_x_addr = 0x800A4D58;
    layout.player_y_addr = 0x800A4D5C;
    layout.player_z_addr = 0x800A4D60;
    layout.player_angle_addr = 0x800A4D6C;
    layout.use_camera_position = false;
    layout.uses_fixed_point = true;
    layout.fixed_point_shift = 8;
    layout.scale_factor = 1.0f;
    layout.addr_size = GameMemoryLayout::AddressSize::Word32;
    m_memory_layouts.push_back(layout);
  }

  // ============================================================
  // Spyro the Dragon (NTSC-U) - SCUS-94228
  // Spyro's position in level
  // ============================================================
  {
    GameMemoryLayout layout;
    layout.serial = "SCUS-94228";
    layout.game_name = "Spyro the Dragon";
    layout.player_x_addr = 0x80078BDC;
    layout.player_y_addr = 0x80078BE0;
    layout.player_z_addr = 0x80078BE4;
    layout.player_angle_addr = 0x80078BF8;
    layout.use_camera_position = false;
    layout.uses_fixed_point = false; // Spyro uses integer coords
    layout.fixed_point_shift = 0;
    layout.scale_factor = 1.0f;
    layout.addr_size = GameMemoryLayout::AddressSize::Word32;
    m_memory_layouts.push_back(layout);
  }

  // ============================================================
  // Tomb Raider (NTSC-U) - SLUS-00152
  // Lara's position in current level
  // ============================================================
  {
    GameMemoryLayout layout;
    layout.serial = "SLUS-00152";
    layout.game_name = "Tomb Raider";
    layout.player_x_addr = 0x8009A5E4;
    layout.player_y_addr = 0x8009A5E8;
    layout.player_z_addr = 0x8009A5EC;
    layout.player_angle_addr = 0x8009A5F8;
    layout.use_camera_position = false;
    layout.uses_fixed_point = false;
    layout.fixed_point_shift = 0;
    layout.scale_factor = 1.0f;
    layout.addr_size = GameMemoryLayout::AddressSize::Word32;
    m_memory_layouts.push_back(layout);
  }

  // ============================================================
  // Metal Gear Solid (NTSC-U) - SLUS-00594
  // Snake's position
  // ============================================================
  {
    GameMemoryLayout layout;
    layout.serial = "SLUS-00594";
    layout.game_name = "Metal Gear Solid";
    layout.player_x_addr = 0x800B3628;
    layout.player_y_addr = 0x800B362C;
    layout.player_z_addr = 0x800B3630;
    layout.player_angle_addr = 0x800B363C;
    layout.use_camera_position = false;
    layout.uses_fixed_point = false;
    layout.fixed_point_shift = 0;
    layout.scale_factor = 1.0f;
    layout.addr_size = GameMemoryLayout::AddressSize::Word32;
    m_memory_layouts.push_back(layout);
  }

  // ============================================================
  // Final Fantasy VII (NTSC-U) - SCUS-94163
  // Cloud's position in field scenes (16-bit coordinates)
  // Note: World map and battle have different address layouts
  // ============================================================
  {
    GameMemoryLayout layout;
    layout.serial = "SCUS-94163";
    layout.game_name = "Final Fantasy VII";
    layout.player_x_addr = 0x800916B2;
    layout.player_y_addr = 0x800916B4;
    layout.player_z_addr = 0x800916B6;
    layout.player_angle_addr = 0x800916BC;
    layout.use_camera_position = false;
    layout.uses_fixed_point = false;
    layout.fixed_point_shift = 0;
    layout.scale_factor = 1.0f;
    layout.addr_size = GameMemoryLayout::AddressSize::HalfWord16;
    m_memory_layouts.push_back(layout);
  }

  // ============================================================
  // Resident Evil 2 (NTSC-U) - SLUS-00421
  // Leon/Claire position
  // ============================================================
  {
    GameMemoryLayout layout;
    layout.serial = "SLUS-00421";
    layout.game_name = "Resident Evil 2";
    layout.player_x_addr = 0x800CC1B0;
    layout.player_y_addr = 0x800CC1B4;
    layout.player_z_addr = 0x800CC1B8;
    layout.player_angle_addr = 0x800CC1C0;
    layout.use_camera_position = false;
    layout.uses_fixed_point = false;
    layout.fixed_point_shift = 0;
    layout.scale_factor = 1.0f;
    layout.addr_size = GameMemoryLayout::AddressSize::Word32;
    m_memory_layouts.push_back(layout);
  }

  INFO_LOG("VR: Loaded {} game memory layouts", m_memory_layouts.size());
}

const GameMemoryLayout* PlayerTracker::FindLayoutForGame(const std::string& serial)
{
  for (const auto& layout : m_memory_layouts)
  {
    if (layout.serial == serial)
    {
      return &layout;
    }
  }
  return nullptr;
}

bool CreatePlayerTracker()
{
  if (g_vr_player_tracker)
  {
    WARNING_LOG("VR player tracker already exists");
    return g_vr_player_tracker->IsInitialized();
  }

  g_vr_player_tracker = std::make_unique<PlayerTracker>();
  if (!g_vr_player_tracker->Initialize())
  {
    g_vr_player_tracker.reset();
    return false;
  }

  return true;
}

void DestroyPlayerTracker()
{
  if (g_vr_player_tracker)
  {
    g_vr_player_tracker->Shutdown();
    g_vr_player_tracker.reset();
  }
}

} // namespace VR

#endif // ENABLE_OPENXR
