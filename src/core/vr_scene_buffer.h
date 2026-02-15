// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once
#include "types.h"
#include "gpu_types.h"
#include <array>
#include <unordered_map>
#include <vector>

// Forward declarations
namespace Screenshot3D {
struct Poly;
struct Texture;
}

namespace VR {

/// Polygon stored in world space for accumulation across frames
struct WorldPoly
{
  // World-space vertex positions (up to 4 vertices for quads)
  std::array<float, 3> world_pos[4];

  // Vertex colors (RGBA, normalized 0-1)
  std::array<float, 4> color[4];

  // UV coordinates (original texture UVs, will be remapped to atlas in renderer)
  std::array<float, 2> uv[4];

  // Texture index from Screenshot3D (used for atlas cell mapping)
  u32 texture_index;

  // Polygon properties
  u8 num_verts;           // 3 for triangle, 4 for quad
  u8 texture_enable : 1;
  u8 transparency_enable : 1;
  GPUTransparencyMode transparency_mode;

  // Tracking
  u32 last_seen_frame;    // Frame counter when last seen
  u64 hash;               // For deduplication

  /// Compute hash from world-space vertex positions
  void ComputeHash();
};

/// World-space scene buffer that accumulates geometry across frames
/// Allows VR to show previously-seen geometry when looking away
class SceneBuffer
{
public:
  SceneBuffer();
  ~SceneBuffer();

  /// Set the camera transform for the current frame (from Screenshot3D::GetCameraTransform)
  /// rt is row-major float[9], tr is float[3]
  void SetCameraTransform(const float* rt, const float* tr);

  /// Add polygons from the current frame, transforming to world space
  /// polygons: camera-space polygons from Screenshot3D
  /// textures: texture array from Screenshot3D (for texture_index validation)
  /// frame_counter: current frame number for tracking
  void AddPolygonsFromFrame(const std::vector<Screenshot3D::Poly>& polygons,
                            const std::vector<Screenshot3D::Texture>& textures,
                            u32 frame_counter);

  /// Get all accumulated polygons
  const std::vector<WorldPoly>& GetPolygons() const { return m_polygons; }

  /// Evict polygons not seen for more than max_age frames
  void EvictOld(u32 current_frame, u32 max_age);

  /// Clear all cached geometry (e.g., on scene transition)
  void Clear();

  /// Check if scene transition occurred (large camera movement)
  /// Returns true if cache was cleared
  bool CheckSceneTransition(u32 current_frame);

  /// Get polygon count for debugging
  size_t GetPolygonCount() const { return m_polygons.size(); }

  /// Enable/disable accumulation
  void SetEnabled(bool enabled) { m_enabled = enabled; }
  bool IsEnabled() const { return m_enabled; }

private:
  /// Transform a camera-space position to world space
  void TransformToWorld(const float* cam_pos, float* world_pos) const;

  /// Compute camera world position for scene transition detection
  void ComputeCameraWorldPosition();

  // Camera transform (row-major)
  float m_camera_rt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // Rotation matrix
  float m_camera_tr[3] = {0, 0, 0};                     // Translation
  float m_camera_rt_inv[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // RT inverse (transpose)
  bool m_camera_valid = false;

  // Camera world position for scene transition detection
  float m_camera_world_pos[3] = {0, 0, 0};
  float m_last_camera_world_pos[3] = {0, 0, 0};
  u32 m_last_transition_check_frame = 0;

  // Accumulated polygons
  std::vector<WorldPoly> m_polygons;

  // Hash map for deduplication: hash -> index in m_polygons
  std::unordered_map<u64, size_t> m_polygon_hash_map;

  // Configuration
  bool m_enabled = true;
  float m_scene_transition_threshold = 5000.0f;  // Distance in PS1 units
  u32 m_max_polygon_age = 300;                   // ~5 seconds at 60fps
  size_t m_max_polygons = 100000;                // Memory limit
};

/// Global scene buffer instance
extern SceneBuffer g_scene_buffer;

} // namespace VR
