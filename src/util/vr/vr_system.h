// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "openxr_loader.h"

#include "common/types.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#ifdef ENABLE_VULKAN
#include "vulkan/vulkan.h"
#endif

namespace VR {

/// View configuration for stereo rendering
struct ViewInfo
{
  XrViewConfigurationView config_view;
  XrView view;
  XrPosef pose;
  XrFovf fov;
};

/// Per-eye swapchain info
struct SwapchainInfo
{
  XrSwapchain swapchain = XR_NULL_HANDLE;
  int64_t format = 0;
  u32 width = 0;
  u32 height = 0;
  u32 sample_count = 1;
  std::vector<XrSwapchainImageVulkanKHR> images;
  u32 current_image_index = 0;  // CRITICAL: Must be initialized
};

/// Frame state for rendering
struct FrameState
{
  XrTime predicted_display_time;
  XrDuration predicted_display_period;
  bool should_render;
};

/// VR System - Main OpenXR integration class
class System
{
public:
  System();
  ~System();

  // Initialization
  bool Initialize();
  void Shutdown();

  // State queries
  bool IsInitialized() const { return m_initialized; }
  bool IsSessionRunning() const { return m_session_running; }
  bool IsHmdPresent() const { return m_system_id != XR_NULL_SYSTEM_ID; }

  // Handle accessors (needed by OpenXR input source for action creation)
  XrInstance GetInstance() const { return m_instance; }
  XrSession GetSession() const { return m_session; }

  // Session management
  bool CreateSession(VkInstance vk_instance, VkPhysicalDevice vk_physical_device, VkDevice vk_device,
                     u32 vk_queue_family_index);
  void DestroySession();

  // Frame cycle
  bool BeginFrame(FrameState& out_frame_state);
  bool EndFrame(const FrameState& frame_state,
                bool submit_quad_layer = false,
                const XrPosef* quad_pose = nullptr,
                const XrExtent2Df* quad_size = nullptr);

  // View information
  u32 GetViewCount() const { return static_cast<u32>(m_views.size()); }
  const ViewInfo& GetView(u32 index) const { return m_views[index]; }
  bool LocateViews(XrTime display_time);

  // Swapchain access for rendering
  SwapchainInfo& GetSwapchain(u32 eye) { return m_swapchains[eye]; }
  const SwapchainInfo& GetSwapchain(u32 eye) const { return m_swapchains[eye]; }
  bool AcquireSwapchainImage(u32 eye);
  bool ReleaseSwapchainImage(u32 eye);

  // Quad swapchain for 2D content (menus, loading screens, HUD)
  bool HasQuadSwapchain() const { return m_quad_swapchain_valid; }
  SwapchainInfo& GetQuadSwapchain() { return m_quad_swapchain; }
  const SwapchainInfo& GetQuadSwapchain() const { return m_quad_swapchain; }
  bool AcquireQuadSwapchainImage();
  bool ReleaseQuadSwapchainImage();

  // Get recommended render dimensions
  u32 GetRenderWidth() const;
  u32 GetRenderHeight() const;

  // Event processing
  void ProcessEvents();

  // Vulkan requirements
  std::vector<std::string> GetRequiredVulkanInstanceExtensions() const;
  std::vector<std::string> GetRequiredVulkanDeviceExtensions() const;
  bool GetVulkanGraphicsDevice(VkInstance vk_instance, VkPhysicalDevice* out_physical_device) const;

private:
  bool CreateInstance();
  bool GetSystem();
  bool EnumerateViewConfigurations();
  bool CreateSwapchains();
  void DestroySwapchains();
  bool CreateQuadSwapchain(int64_t format);
  void DestroyQuadSwapchain();
  bool CreateReferenceSpace();
  void HandleSessionStateChange(XrEventDataSessionStateChanged* event);

  // OpenXR handles
  XrInstance m_instance = XR_NULL_HANDLE;
  XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
  XrSession m_session = XR_NULL_HANDLE;
  XrSpace m_reference_space = XR_NULL_HANDLE;

  // View configuration
  XrViewConfigurationType m_view_config_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  XrEnvironmentBlendMode m_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  std::vector<ViewInfo> m_views;

  // Swapchains (one per eye for stereo)
  std::array<SwapchainInfo, 2> m_swapchains;

  // Quad swapchain for 2D overlay content
  SwapchainInfo m_quad_swapchain;
  bool m_quad_swapchain_valid = false;

  // Session state
  XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
  bool m_session_running = false;
  bool m_exit_requested = false;

  // State flags
  bool m_initialized = false;
  bool m_using_vulkan_enable2 = false;

  // System properties
  XrSystemProperties m_system_properties = {};
  std::string m_system_name;
};

/// Global VR system instance
extern std::unique_ptr<System> g_vr_system;

/// Initialize the global VR system
bool Initialize();

/// Shutdown the global VR system
void Shutdown();

/// Check if VR is available on this system
bool IsAvailable();

} // namespace VR
