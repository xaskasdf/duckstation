// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#include "vr_scene_buffer.h"
#include "screenshot_3d.h"
#include "common/log.h"
#include <cmath>
#include <cstring>

LOG_CHANNEL(VR);

namespace VR {

SceneBuffer g_scene_buffer;

void WorldPoly::ComputeHash()
{
  // Simple hash based on quantized world positions
  // Quantize to ~1 PS1 unit resolution to handle floating point imprecision
  hash = 0;
  for (int i = 0; i < num_verts; i++)
  {
    s32 qx = static_cast<s32>(world_pos[i][0]);
    s32 qy = static_cast<s32>(world_pos[i][1]);
    s32 qz = static_cast<s32>(world_pos[i][2]);

    // FNV-1a style hash
    hash ^= static_cast<u64>(qx) * 0x100000001b3ULL;
    hash ^= static_cast<u64>(qy) * 0x100000001b3ULL;
    hash ^= static_cast<u64>(qz) * 0x100000001b3ULL;
  }
  // Include texture index in hash for proper deduplication
  hash ^= static_cast<u64>(texture_index) * 0x100000001b3ULL;
}

SceneBuffer::SceneBuffer()
{
  m_polygons.reserve(10000);
}

SceneBuffer::~SceneBuffer() = default;

void SceneBuffer::SetCameraTransform(const float* rt, const float* tr)
{
  std::memcpy(m_camera_rt, rt, sizeof(float) * 9);
  std::memcpy(m_camera_tr, tr, sizeof(float) * 3);

  // Compute RT inverse = RT transpose (rotation matrices are orthogonal)
  // RT is row-major: RT[row][col] = rt[row*3 + col]
  // RT_inv[i][j] = RT[j][i]
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      m_camera_rt_inv[i * 3 + j] = m_camera_rt[j * 3 + i];

  m_camera_valid = true;
  ComputeCameraWorldPosition();
}

void SceneBuffer::ComputeCameraWorldPosition()
{
  // Camera world position = -RT_inv * TR
  for (int i = 0; i < 3; i++)
  {
    m_camera_world_pos[i] = 0.0f;
    for (int j = 0; j < 3; j++)
      m_camera_world_pos[i] -= m_camera_rt_inv[i * 3 + j] * m_camera_tr[j];
  }
}

void SceneBuffer::TransformToWorld(const float* cam_pos, float* world_pos) const
{
  // world = RT_inv * (cam - TR)
  float cam_minus_tr[3];
  cam_minus_tr[0] = cam_pos[0] - m_camera_tr[0];
  cam_minus_tr[1] = cam_pos[1] - m_camera_tr[1];
  cam_minus_tr[2] = cam_pos[2] - m_camera_tr[2];

  for (int i = 0; i < 3; i++)
  {
    world_pos[i] = 0.0f;
    for (int j = 0; j < 3; j++)
      world_pos[i] += m_camera_rt_inv[i * 3 + j] * cam_minus_tr[j];
  }
}

void SceneBuffer::AddPolygonsFromFrame(const std::vector<Screenshot3D::Poly>& polygons,
                                       const std::vector<Screenshot3D::Texture>& textures,
                                       u32 frame_counter)
{
  if (!m_enabled || !m_camera_valid)
    return;

  // Check for scene transition first
  if (CheckSceneTransition(frame_counter))
  {
    INFO_LOG("VR Scene Buffer: Scene transition detected, cleared cache");
  }

  // Process each polygon
  for (const auto& poly : polygons)
  {
    // Skip polygons without valid 3D data
    if (!poly.has_3d_verts)
      continue;

    WorldPoly wp;
    wp.num_verts = poly.NumVerts();
    wp.texture_index = poly.texture_index;
    wp.texture_enable = poly.texture_enable;
    wp.transparency_enable = poly.transparency_enable;
    wp.transparency_mode = poly.transparency_mode;
    wp.last_seen_frame = frame_counter;

    // Copy vertices - keep in camera space for now
    // NOTE: World-space transform is disabled because RT/TR changes per-object,
    // not per-frame. Transforming with a single RT/TR causes geometry corruption.
    // The camera injection (Phase 3) handles viewpoint changes in the GTE directly.
    for (int i = 0; i < wp.num_verts; i++)
    {
      // Keep camera-space position as-is (no transform)
      wp.world_pos[i][0] = poly.v_3d[i][0];
      wp.world_pos[i][1] = poly.v_3d[i][1];
      wp.world_pos[i][2] = poly.v_3d[i][2];

      // Copy vertex color (normalize from 0-255 to 0-1)
      wp.color[i][0] = poly.v[i].r / 255.0f;
      wp.color[i][1] = poly.v[i].g / 255.0f;
      wp.color[i][2] = poly.v[i].b / 255.0f;
      wp.color[i][3] = 1.0f;

      // Copy UVs (original texture UVs, will be remapped to atlas in renderer)
      wp.uv[i][0] = static_cast<float>(poly.v[i].u);
      wp.uv[i][1] = static_cast<float>(poly.v[i].v);
    }

    // Fill unused vertices for triangles
    for (int i = wp.num_verts; i < 4; i++)
    {
      wp.world_pos[i] = wp.world_pos[0];
      wp.color[i] = wp.color[0];
      wp.uv[i] = wp.uv[0];
    }

    // Compute hash for deduplication
    wp.ComputeHash();

    // Check if polygon already exists
    auto it = m_polygon_hash_map.find(wp.hash);
    if (it != m_polygon_hash_map.end())
    {
      // Update last seen frame for existing polygon
      m_polygons[it->second].last_seen_frame = frame_counter;
    }
    else
    {
      // Add new polygon
      if (m_polygons.size() < m_max_polygons)
      {
        size_t idx = m_polygons.size();
        m_polygons.push_back(wp);
        m_polygon_hash_map[wp.hash] = idx;
      }
    }
  }
}

void SceneBuffer::EvictOld(u32 current_frame, u32 max_age)
{
  if (m_polygons.empty())
    return;

  // Mark polygons for removal
  std::vector<size_t> to_remove;
  for (size_t i = 0; i < m_polygons.size(); i++)
  {
    if (current_frame - m_polygons[i].last_seen_frame > max_age)
      to_remove.push_back(i);
  }

  if (to_remove.empty())
    return;

  // Remove in reverse order to preserve indices
  for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it)
  {
    size_t idx = *it;

    // Remove from hash map
    m_polygon_hash_map.erase(m_polygons[idx].hash);

    // Swap with last element and pop (O(1) removal)
    if (idx != m_polygons.size() - 1)
    {
      // Update hash map for swapped element
      m_polygon_hash_map[m_polygons.back().hash] = idx;
      m_polygons[idx] = std::move(m_polygons.back());
    }
    m_polygons.pop_back();
  }

  DEV_LOG("VR Scene Buffer: Evicted {} old polygons, {} remaining",
                to_remove.size(), m_polygons.size());
}

void SceneBuffer::Clear()
{
  m_polygons.clear();
  m_polygon_hash_map.clear();
  m_camera_valid = false;
  INFO_LOG("VR Scene Buffer: Cleared all cached geometry");
}

bool SceneBuffer::CheckSceneTransition(u32 current_frame)
{
  // Only check once per frame
  if (current_frame == m_last_transition_check_frame)
    return false;

  m_last_transition_check_frame = current_frame;

  // Skip if this is the first frame
  if (m_last_camera_world_pos[0] == 0.0f &&
      m_last_camera_world_pos[1] == 0.0f &&
      m_last_camera_world_pos[2] == 0.0f)
  {
    std::memcpy(m_last_camera_world_pos, m_camera_world_pos, sizeof(float) * 3);
    return false;
  }

  // Compute distance moved
  float dx = m_camera_world_pos[0] - m_last_camera_world_pos[0];
  float dy = m_camera_world_pos[1] - m_last_camera_world_pos[1];
  float dz = m_camera_world_pos[2] - m_last_camera_world_pos[2];
  float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

  // Update last position
  std::memcpy(m_last_camera_world_pos, m_camera_world_pos, sizeof(float) * 3);

  // Check for scene transition (large jump in camera position)
  if (distance > m_scene_transition_threshold)
  {
    Clear();
    return true;
  }

  return false;
}

} // namespace VR
