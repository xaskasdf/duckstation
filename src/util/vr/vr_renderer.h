// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/types.h"
#include "core/types.h"

#ifdef ENABLE_OPENXR

#include "vr_system.h"

#ifdef ENABLE_VULKAN
#include "util/vulkan_loader.h"
#endif

#include <array>
#include <chrono>
#include <vector>

// Forward declarations
namespace Screenshot3D {
struct Poly;
struct Texture;
}

class GPUDevice;

namespace VR {

/// Vertex format for VR rendering (matches PS1 3D geometry)
struct VRVertex
{
  float position[3];    // World position from GTE
  float color[4];       // RGBA color
  float texcoord[2];    // UV coordinates
  u32 texture_index;    // Index into texture array (UINT32_MAX = untextured)
};

#ifdef ENABLE_VULKAN
/// VR texture resource
struct VRTexture
{
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  u32 width = 0;
  u32 height = 0;
};
#endif

/// Push constants for VR shader
struct PushConstants
{
  float view_proj[16];  // Combined view-projection matrix
  float scale;          // World scale
  float padding[3];     // Padding to 16-byte alignment
};

/// VR Stereo Renderer
/// Renders captured 3D geometry to OpenXR swapchains
class Renderer
{
public:
  Renderer();
  ~Renderer();

  /// Initialize the VR renderer
  bool Initialize(GPUDevice* gpu_device, System* vr_system);

  /// Shutdown and release resources
  void Shutdown();

  /// Check if initialized
  bool IsInitialized() const { return m_initialized; }

  /// Render the current frame's 3D geometry to both eyes
  /// Should be called after VR::BeginFrame() and before VR::EndFrame()
  bool RenderStereo();

  /// Set the virtual screen distance from the player
  void SetScreenDistance(float distance) { m_screen_distance = distance; }

  /// Set the virtual screen scale
  void SetScreenScale(float scale) { m_screen_scale = scale; }

  /// Set whether to render captured 3D geometry or fallback to 2D screen
  void SetRender3DEnabled(bool enabled) { m_render_3d = enabled; }

  /// Save a screenshot of the VR view (for debugging)
  void SaveScreenshot();

  /// Get the active VR navigation mode
  VRNavigationMode GetActiveNavigationMode() const { return m_active_nav_mode; }

  /// Get the current facing yaw in radians
  float GetFacingYaw() const { return m_facing_yaw_world; }

  /// Cycle through navigation modes (Tank -> CameraYaw -> Hybrid -> Tank)
  void CycleNavigationMode();

  /// Reset navigation yaw (re-initialize from camera yaw next frame)
  void ResetNavigationYaw();

  /// Blit emulator display to quad swapchain for 2D overlay
  bool BlitDisplayToQuadSwapchain();

  /// Get quad layer pose and size for EndFrame submission
  /// Returns false if no quad blit happened this frame
  bool GetQuadLayerParams(XrPosef* out_pose, XrExtent2Df* out_size) const;

private:
  /// Build vertex and index buffers from captured geometry
  bool BuildGeometryBuffers(const std::vector<Screenshot3D::Poly>& polygons,
                            const std::vector<Screenshot3D::Texture>& textures);

  /// Render to a single eye
  bool RenderEye(u32 eye_index);

  /// Render the 2D fallback screen (when no 3D geometry available)
  bool RenderFallbackScreen(u32 eye_index);

  /// Calculate view and projection matrices for an eye
  void CalculateViewProjection(u32 eye_index, float* out_view_matrix, float* out_proj_matrix);

  /// Convert OpenXR pose to view matrix
  static void PoseToViewMatrix(const XrPosef& pose, float* out_matrix);

  /// Convert OpenXR FoV to projection matrix
  static void FovToProjectionMatrix(const XrFovf& fov, float near_z, float far_z, float* out_matrix);

  /// Multiply two 4x4 matrices
  static void MultiplyMatrices(const float* a, const float* b, float* out);

#ifdef ENABLE_VULKAN
  /// Create render pass and framebuffers for VR swapchain images
  bool CreateRenderResources();
  void DestroyRenderResources();

  /// Create graphics pipeline
  bool CreatePipeline();
  void DestroyPipeline();

  /// Create command pool and buffers
  bool CreateCommandResources();
  void DestroyCommandResources();

  /// Upload geometry to GPU buffers
  bool UploadGeometryBuffers(const std::vector<VRVertex>& vertices, const std::vector<u32>& indices);
  void DestroyGeometryBuffers();

  /// Texture resources
  bool CreateTextureResources();
  void DestroyTextureResources();
  bool UploadTextures(const std::vector<Screenshot3D::Texture>& textures);
  bool UploadTexturesRemapped(const std::vector<Screenshot3D::Texture>& textures,
                               const std::vector<u32>& used_indices);
  void DestroyTextures();

  /// Vulkan-specific rendering
  bool RenderEyeVulkan(u32 eye_index, VkImage target_image, u32 width, u32 height);

  // Vulkan device resources
  VkDevice m_vk_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_vk_physical_device = VK_NULL_HANDLE;
  u32 m_vk_queue_family = 0;
  VmaAllocator m_vk_allocator = VK_NULL_HANDLE;

  // Render pass and framebuffers
  VkRenderPass m_vk_render_pass = VK_NULL_HANDLE;       // LOAD_OP_CLEAR (default)
  VkRenderPass m_vk_render_pass_load = VK_NULL_HANDLE;  // LOAD_OP_LOAD (for display background)
  std::array<std::vector<VkFramebuffer>, 2> m_vk_framebuffers;
  std::array<std::vector<VkImageView>, 2> m_vk_image_views;

  // Graphics pipeline
  VkPipeline m_vk_pipeline = VK_NULL_HANDLE;
  VkPipeline m_vk_pipeline_additive = VK_NULL_HANDLE;  // Additive blend for lighting overlays
  VkPipelineLayout m_vk_pipeline_layout = VK_NULL_HANDLE;

  // Command resources
  // Index 0/1 = eye 0/1 rendering, Index 2 = utility operations (texture upload)
  VkCommandPool m_vk_command_pool = VK_NULL_HANDLE;
  VkCommandBuffer m_vk_command_buffers[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkFence m_vk_fences[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};

  // Geometry buffers
  VkBuffer m_vk_vertex_buffer = VK_NULL_HANDLE;
  VmaAllocation m_vk_vertex_allocation = VK_NULL_HANDLE;
  VkBuffer m_vk_index_buffer = VK_NULL_HANDLE;
  VmaAllocation m_vk_index_allocation = VK_NULL_HANDLE;

  u32 m_vertex_count = 0;
  u32 m_index_count = 0;

  // Texture resources
  static constexpr u32 MAX_VR_TEXTURES = 256;
  std::vector<VRTexture> m_vr_textures;
  VkSampler m_vk_sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_vk_descriptor_set_layout = VK_NULL_HANDLE;
  VkDescriptorPool m_vk_descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet m_vk_descriptor_set = VK_NULL_HANDLE;
  VkImage m_vk_placeholder_image = VK_NULL_HANDLE;
  VmaAllocation m_vk_placeholder_allocation = VK_NULL_HANDLE;
  VkImageView m_vk_placeholder_view = VK_NULL_HANDLE;
#endif

  GPUDevice* m_gpu_device = nullptr;
  System* m_vr_system = nullptr;

  // Rendering settings
  float m_screen_distance = 2.0f;   // Distance from player in meters
  float m_screen_scale = 1.5f;      // Scale of virtual screen (quad layer size)
  float m_world_scale_factor = 1.0f; // Scale for 3D geometry push constants
  bool m_render_3d = true;          // Render 3D geometry or fallback screen

  // Navigation state
  float m_facing_yaw_world = 0.0f;
  float m_accumulated_yaw = 0.0f;
  bool m_yaw_initialized = false;
  u32 m_stick_idle_frames = 0;
  bool m_prev_both_sticks_clicked = false;
  VRNavigationMode m_active_nav_mode = VRNavigationMode::Hybrid;

  // Snap turn state
  bool m_snap_turn_armed = true;

  // Lighting overlay geometry (Pass 3: untextured transparent polygons with additive blend)
  u32 m_lighting_index_offset = 0;
  u32 m_lighting_index_count = 0;

  // Frame timing for rotation speed
  std::chrono::steady_clock::time_point m_last_time_point;
  bool m_time_initialized = false;

  // State
  bool m_initialized = false;
  bool m_pipeline_creation_attempted = false;
  bool m_screenshot_requested = false;
  bool m_quad_blitted_this_frame = false;
  u32 m_last_frame_counter = 0;
};

/// Global VR renderer instance
extern std::unique_ptr<Renderer> g_vr_renderer;

/// Create and initialize the global VR renderer
bool CreateRenderer(GPUDevice* gpu_device, System* vr_system);

/// Destroy the global VR renderer
void DestroyRenderer();

} // namespace VR

#endif // ENABLE_OPENXR
