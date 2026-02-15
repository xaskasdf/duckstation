// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/types.h"

#ifdef ENABLE_OPENXR

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace VR {

/// Detected player position and orientation
struct PlayerState
{
  float position[3] = {0, 0, 0};  // World position (x, y, z) in meters
  float rotation_y = 0.0f;        // Facing angle (radians)
  float confidence = 0.0f;        // Detection confidence 0.0-1.0
  bool valid = false;
};

/// Detection method used
enum class DetectionMethod
{
  CameraAnalysis,   // Using GTE camera transform
  MemoryDirect,     // Reading from known RAM addresses
  InputCorrelation, // Correlating geometry with input
  ManualSelection,  // User selected object
  Fallback          // No detection, default position
};

/// Camera state for smoothing and validation
struct CameraState
{
  float position[3] = {0, 0, 0};           // Raw position from GTE
  float smoothed_position[3] = {0, 0, 0};  // EMA-smoothed position
  float rotation_y = 0.0f;                 // Raw rotation
  float smoothed_rotation_y = 0.0f;        // EMA-smoothed rotation
  float velocity[3] = {0, 0, 0};           // Position velocity
  bool is_valid = false;                   // Whether current reading is valid
  u32 stable_frame_count = 0;              // Frames since last position jump
};

/// Game-specific memory layout for direct player position reading
struct GameMemoryLayout
{
  std::string serial;
  std::string game_name;                   // Human-readable name
  u32 player_x_addr = 0;
  u32 player_y_addr = 0;
  u32 player_z_addr = 0;
  u32 player_angle_addr = 0;
  u32 camera_x_addr = 0;                   // Alternative: camera position
  u32 camera_y_addr = 0;
  u32 camera_z_addr = 0;
  u32 camera_angle_addr = 0;
  bool use_camera_position = false;        // Use camera instead of player
  bool uses_fixed_point = true;
  int fixed_point_shift = 12;
  float scale_factor = 1.0f;
  enum class AddressSize { Word32, HalfWord16 } addr_size = AddressSize::Word32;
  u32 validation_addr = 0;                 // Address to verify game version
  u32 validation_value = 0;
};

/// VR Player Tracker
/// Detects and tracks the player character's position for first-person VR
class PlayerTracker
{
public:
  PlayerTracker();
  ~PlayerTracker();

  /// Initialize the tracker
  bool Initialize();

  /// Shutdown and release resources
  void Shutdown();

  /// Update tracking (call every frame)
  void Update();

  /// Get the current player state
  const PlayerState& GetPlayerState() const { return m_player_state; }

  /// Get the active detection method
  DetectionMethod GetActiveMethod() const { return m_active_method; }

  /// Enable/disable first-person mode
  void SetFirstPersonEnabled(bool enabled) { m_first_person_enabled = enabled; }
  bool IsFirstPersonEnabled() const { return m_first_person_enabled; }

  /// Check if first-person mode is available (valid detection)
  bool IsFirstPersonAvailable() const;

  /// Check if first-person mode is active with hysteresis (stable switching)
  /// Use this instead of IsFirstPersonAvailable() to avoid rapid mode switching
  bool IsFirstPersonActiveStable() const { return m_first_person_active; }

  /// Get the first-person view offset to apply
  /// Returns position (in meters) and rotation (in radians)
  void GetFirstPersonViewOffset(float* out_position, float* out_rotation_y);

  /// Set/get eye height offset (meters)
  void SetEyeHeight(float height) { m_eye_height = height; }
  float GetEyeHeight() const { return m_eye_height; }

  /// Set/get world scale factor
  void SetWorldScale(float scale) { m_world_scale = scale; }
  float GetWorldScale() const { return m_world_scale; }

  /// Check if initialized
  bool IsInitialized() const { return m_initialized; }

  /// Get player centroid in camera space (PS1 units)
  /// Returns false if no valid centroid is detected
  bool GetPlayerCentroidCameraSpace(float* out_cx, float* out_cy, float* out_cz) const;

  /// Enable debug logging to console
  void SetDebugLogging(bool enabled) { m_debug_logging = enabled; }
  bool IsDebugLoggingEnabled() const { return m_debug_logging; }

  /// Get human-readable status string for debugging
  std::string GetStatusString() const;

  /// Get name of detection method
  static const char* GetMethodName(DetectionMethod method);

private:
  // Forward declare types used by methods
  struct TrackedObject;

  /// Camera-based detection using GTE TR/RT registers
  bool DetectFromCameraTransform();

  /// Memory-based detection for known games
  bool DetectFromMemory();

  /// Input correlation detection (future)
  bool DetectFromInputCorrelation();

  /// Extract camera position and rotation from GTE registers
  void ExtractCameraFromGTE(float* out_position, float* out_rotation_matrix);

  /// Validate camera transform for scene transitions and invalid data
  bool ValidateCameraTransform(const float* position, const float* rotation_matrix);

  /// Apply EMA smoothing to a single value
  static float SmoothValue(float current, float target, float alpha);

  /// Apply EMA smoothing to a 3D position
  static void SmoothPosition(float* current, const float* target, float alpha);

  /// Normalize angle to -PI to PI range
  static float NormalizeAngle(float angle);

  /// Smooth rotation handling wraparound at +/- PI
  static float SmoothRotation(float current, float target, float alpha);

  /// Update input history for correlation
  void UpdateInputHistory();

  /// Get average input direction over recent frames
  void GetAverageInputDirection(float* out_dir_x, float* out_dir_z, size_t frames = 5);

  /// Cluster polygons into tracked objects
  void ClusterPolygonsIntoCentroids();

  /// Find or create tracked object nearest to given centroid
  TrackedObject* FindOrCreateTrackedObject(const float* centroid, u32 polygon_count, u32 frame);

  /// Update correlation scores for all tracked objects
  void UpdateCorrelationScores();

  /// Score tracked objects using weighted heuristics (screen center, size, depth, correlation)
  void ScoreTrackedObjects();

  /// Select the best player cluster from scored tracked objects and apply smoothing
  void SelectBestPlayerCluster();

  /// Load game-specific memory layouts
  void LoadMemoryLayouts();

  /// Find layout for current game
  const GameMemoryLayout* FindLayoutForGame(const std::string& serial);

  // State
  PlayerState m_player_state;
  DetectionMethod m_active_method = DetectionMethod::Fallback;
  bool m_first_person_enabled = false;
  bool m_initialized = false;

  // Hysteresis for stable first-person mode switching
  // Requires multiple consecutive frames before switching modes to prevent flickering
  bool m_first_person_active = false;      // Current effective mode (with hysteresis)
  int m_mode_switch_counter = 0;           // Frames since mode change was requested
  static constexpr int MODE_SWITCH_FRAMES = 15; // ~0.25 seconds at 60fps before confirming switch

  // Camera tracking state (improved with smoothing)
  CameraState m_camera_state;
  bool m_has_established_position = false;  // True once we have a stable smoothed position to keep
  u32 m_consecutive_skips = 0;              // Count of consecutive position skips
  static constexpr u32 MAX_CONSECUTIVE_SKIPS = 60; // Re-establish position after this many skips (~1 second)
  float m_last_camera_pos[3] = {0, 0, 0};
  float m_camera_velocity[3] = {0, 0, 0};

  // Smoothing parameters
  float m_position_smoothing = 0.15f;        // EMA alpha for position (0 = no smooth, 1 = instant)
  float m_rotation_smoothing = 0.2f;         // EMA alpha for rotation
  float m_max_position_jump = 5000.0f;       // Max position change before reset (PS1 units)
  float m_max_rotation_jump = 1.5f;          // Max rotation change before reset (radians, ~86 deg)
  static constexpr u32 STABILITY_FRAMES = 10; // Frames after reset before applying full smoothing

  // Input correlation state
  struct InputSample
  {
    float left_x = 0, left_y = 0;
    u32 button_bits = 0;
    u32 frame = 0;
  };
  std::vector<InputSample> m_input_history;
  static constexpr size_t INPUT_HISTORY_SIZE = 30;

  // Tracked object for input correlation and player detection
  struct TrackedObject
  {
    float centroid[3] = {0, 0, 0};      // Current centroid position (camera-space PS1 units)
    float prev_centroid[3] = {0, 0, 0}; // Previous frame centroid
    float velocity[3] = {0, 0, 0};      // Movement velocity
    float correlation_score = 0.0f;      // Correlation with input
    float avg_screen_x = 0.0f;          // Average screen-space X (normalized 0-1)
    float avg_screen_y = 0.0f;          // Average screen-space Y (normalized 0-1)
    float combined_score = 0.0f;        // Weighted heuristic score for player detection
    u32 polygon_count = 0;               // Number of polygons in cluster
    u32 last_seen_frame = 0;             // Frame when last updated
    bool is_candidate = false;           // Marked as player candidate
  };

  // Input correlation tracking
  std::vector<TrackedObject> m_tracked_objects;
  TrackedObject* m_best_candidate = nullptr;
  float m_best_correlation_score = 0.0f;
  u32 m_correlation_stable_frames = 0;

  static constexpr size_t MAX_TRACKED_OBJECTS = 50;
  static constexpr float CORRELATION_THRESHOLD = 0.6f;
  static constexpr u32 CORRELATION_STABILITY_THRESHOLD = 30;
  static constexpr float CLUSTER_DISTANCE_THRESHOLD = 500.0f; // PS1 units

  // Player centroid detection from geometry clustering
  float m_player_centroid_cam[3] = {0, 0, 0};    // Raw detected centroid (camera-space PS1 units)
  float m_smoothed_centroid_cam[3] = {0, 0, 0};   // EMA-smoothed centroid
  bool m_centroid_valid = false;
  u32 m_centroid_stable_frames = 0;
  u32 m_centroid_skip_count = 0;                   // Consecutive frames where best candidate was rejected (too far)

  // Game-specific memory layouts
  std::vector<GameMemoryLayout> m_memory_layouts;
  const GameMemoryLayout* m_current_layout = nullptr;
  std::string m_current_game_serial;

  // Configuration
  float m_eye_height = 0.15f;    // 15cm default eye height offset (in meters)
  float m_world_scale = 0.001f;  // Convert PS1 units to meters

  // Frame tracking
  u32 m_last_update_frame = 0;

  // Debug
  bool m_debug_logging = false;
};

/// Global player tracker instance
extern std::unique_ptr<PlayerTracker> g_vr_player_tracker;

/// Create and initialize the global player tracker
bool CreatePlayerTracker();

/// Destroy the global player tracker
void DestroyPlayerTracker();

} // namespace VR

#endif // ENABLE_OPENXR
