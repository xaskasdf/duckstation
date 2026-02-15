// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#include "vr_integration.h"
#include "vr_system.h"
#include "vr_renderer.h"
#include "vr_player_tracker.h"

#include "util/gpu_device.h"

#ifdef ENABLE_VULKAN
#include "util/vulkan_device.h"
#include "util/vulkan_loader.h"
#endif

#include "common/log.h"

#include "imgui.h"

LOG_CHANNEL(VR);

namespace VR {

// Current frame state
static FrameState s_current_frame_state = {};
static bool s_frame_in_progress = false;
static GPUDevice* s_gpu_device = nullptr;

bool IsHardwareAvailable()
{
#ifdef ENABLE_OPENXR
  return VR::IsAvailable();
#else
  return false;
#endif
}

bool InitializeSystem()
{
#ifdef ENABLE_OPENXR
  INFO_LOG("Initializing VR system...");
  return VR::Initialize();
#else
  WARNING_LOG("VR support not compiled in (ENABLE_OPENXR=0)");
  return false;
#endif
}

bool CreateSession(GPUDevice* gpu_device)
{
#ifdef ENABLE_OPENXR
  if (!g_vr_system || !g_vr_system->IsInitialized())
  {
    ERROR_LOG("VR system not initialized");
    return false;
  }

  // VR requires Vulkan
  if (gpu_device->GetRenderAPI() != RenderAPI::Vulkan)
  {
    ERROR_LOG("VR requires Vulkan backend, current: {}",
                    GPUDevice::RenderAPIToString(gpu_device->GetRenderAPI()));
    return false;
  }

#ifdef ENABLE_VULKAN
  VulkanDevice* vk_device = static_cast<VulkanDevice*>(gpu_device);

  VkInstance vk_instance = VulkanLoader::GetVulkanInstance();
  VkPhysicalDevice vk_physical_device = vk_device->GetVulkanPhysicalDevice();
  VkDevice vk_logical_device = vk_device->GetVulkanDevice();
  u32 queue_family = vk_device->GetGraphicsQueueFamilyIndex();

  INFO_LOG("Creating VR session with Vulkan device...");

  if (!g_vr_system->CreateSession(vk_instance, vk_physical_device, vk_logical_device, queue_family))
  {
    ERROR_LOG("Failed to create VR session");
    return false;
  }

  INFO_LOG("VR session created successfully");
  INFO_LOG("VR render resolution: {}x{} per eye",
                 g_vr_system->GetRenderWidth(),
                 g_vr_system->GetRenderHeight());

  // Create renderer
  s_gpu_device = gpu_device;
  if (!CreateRenderer(gpu_device, g_vr_system.get()))
  {
    WARNING_LOG("Failed to create VR renderer, stereo rendering disabled");
  }

  // Create player tracker for first-person VR
  if (!CreatePlayerTracker())
  {
    WARNING_LOG("Failed to create VR player tracker, first-person mode disabled");
  }

  return true;
#else
  ERROR_LOG("Vulkan support not compiled in");
  return false;
#endif

#else
  return false;
#endif
}

void DestroySession()
{
#ifdef ENABLE_OPENXR
  // Destroy player tracker first
  DestroyPlayerTracker();

  // Then destroy renderer
  DestroyRenderer();
  s_gpu_device = nullptr;

  if (g_vr_system)
  {
    INFO_LOG("Destroying VR session...");
    g_vr_system->DestroySession();
  }
#endif
}

// Note: VR::Shutdown() is defined in vr_system.cpp

bool IsActive()
{
#ifdef ENABLE_OPENXR
  return g_vr_system && g_vr_system->IsSessionRunning();
#else
  return false;
#endif
}

bool ShouldRender()
{
#ifdef ENABLE_OPENXR
  return s_frame_in_progress && s_current_frame_state.should_render;
#else
  return false;
#endif
}

bool BeginFrame()
{
#ifdef ENABLE_OPENXR
  if (!g_vr_system)
    return false;

  // Process pending events FIRST - this handles session state transitions
  // ProcessEvents must be called even when session isn't running to receive
  // the READY state and start the session
  g_vr_system->ProcessEvents();

  // Now check if session is running (may have just started from ProcessEvents)
  if (!g_vr_system->IsSessionRunning())
    return false;

  // Begin frame
  if (!g_vr_system->BeginFrame(s_current_frame_state))
    return false;

  s_frame_in_progress = true;

  // Locate views for this frame
  if (s_current_frame_state.should_render)
  {
    g_vr_system->LocateViews(s_current_frame_state.predicted_display_time);
  }

  return true;
#else
  return false;
#endif
}

s32 AcquireEyeImage(u32 eye)
{
#ifdef ENABLE_OPENXR
  if (!g_vr_system || eye >= 2)
    return -1;

  if (!g_vr_system->AcquireSwapchainImage(eye))
    return -1;

  return static_cast<s32>(g_vr_system->GetSwapchain(eye).current_image_index);
#else
  return -1;
#endif
}

bool ReleaseEyeImage(u32 eye)
{
#ifdef ENABLE_OPENXR
  if (!g_vr_system || eye >= 2)
    return false;

  return g_vr_system->ReleaseSwapchainImage(eye);
#else
  return false;
#endif
}

bool EndFrame()
{
#ifdef ENABLE_OPENXR
  if (!g_vr_system || !s_frame_in_progress)
    return false;

  // Get quad layer params from renderer (if display was blitted this frame)
  bool submit_quad = false;
  XrPosef quad_pose = {};
  XrExtent2Df quad_size = {};
  if (g_vr_renderer)
    submit_quad = g_vr_renderer->GetQuadLayerParams(&quad_pose, &quad_size);

  bool result = g_vr_system->EndFrame(s_current_frame_state, submit_quad, &quad_pose, &quad_size);
  s_frame_in_progress = false;
  return result;
#else
  return false;
#endif
}

void GetRenderDimensions(u32* width, u32* height)
{
#ifdef ENABLE_OPENXR
  if (g_vr_system)
  {
    *width = g_vr_system->GetRenderWidth();
    *height = g_vr_system->GetRenderHeight();
  }
  else
#endif
  {
    *width = 0;
    *height = 0;
  }
}

void* GetEyeImage(u32 eye)
{
#ifdef ENABLE_OPENXR
  if (!g_vr_system || eye >= 2)
    return nullptr;

  const auto& swapchain = g_vr_system->GetSwapchain(eye);
  if (swapchain.images.empty())
    return nullptr;

  u32 index = swapchain.current_image_index;

  // CRITICAL: Bounds check before accessing images array
  if (index >= swapchain.images.size())
  {
    ERROR_LOG("VR: GetEyeImage - Image index {} out of bounds for eye {} (max: {})",
                    index, eye, swapchain.images.size());
    return nullptr;
  }

  return reinterpret_cast<void*>(swapchain.images[index].image);
#else
  return nullptr;
#endif
}

void ProcessEvents()
{
#ifdef ENABLE_OPENXR
  if (g_vr_system)
    g_vr_system->ProcessEvents();
#endif
}

bool RenderStereo()
{
#ifdef ENABLE_OPENXR
  if (!g_vr_renderer || !s_frame_in_progress || !s_current_frame_state.should_render)
    return false;

  return g_vr_renderer->RenderStereo();
#else
  return false;
#endif
}

void DrawDebugWindow()
{
  if (!ImGui::Begin("VR Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::End();
    return;
  }

#ifdef ENABLE_OPENXR
  // VR System status
  ImGui::Text("VR System Status:");
  ImGui::Separator();

  // Don't call IsHardwareAvailable() every frame - it loads/unloads the OpenXR loader
  // If VR is active, hardware is obviously available
  bool vr_active = IsActive();
  bool vr_available = vr_active || (g_vr_system != nullptr);
  ImGui::Text("Hardware Available: %s", vr_available ? "Yes" : "No");
  ImGui::Text("Session Active: %s", vr_active ? "Yes" : "No");
  ImGui::Text("Should Render: %s", ShouldRender() ? "Yes" : "No");

  if (g_vr_system)
  {
    u32 width = g_vr_system->GetRenderWidth();
    u32 height = g_vr_system->GetRenderHeight();
    ImGui::Text("Render Resolution: %ux%u per eye", width, height);
  }

  ImGui::Spacing();

  // Player Tracker status
  ImGui::Text("Player Tracker:");
  ImGui::Separator();

  if (g_vr_player_tracker)
  {
    const PlayerState& state = g_vr_player_tracker->GetPlayerState();
    DetectionMethod method = g_vr_player_tracker->GetActiveMethod();

    const char* method_names[] = {"Camera Analysis", "Memory Direct", "Input Correlation", "Manual Selection", "Fallback"};
    ImGui::Text("Detection Method: %s", method_names[static_cast<int>(method)]);
    ImGui::Text("First-Person Enabled: %s", g_vr_player_tracker->IsFirstPersonEnabled() ? "Yes" : "No");
    ImGui::Text("First-Person Available: %s", g_vr_player_tracker->IsFirstPersonAvailable() ? "Yes" : "No");

    ImGui::Spacing();
    ImGui::Text("Player State:");
    ImGui::Text("  Valid: %s", state.valid ? "Yes" : "No");
    ImGui::Text("  Confidence: %.1f%%", state.confidence * 100.0f);
    ImGui::Text("  Position: (%.2f, %.2f, %.2f)", state.position[0], state.position[1], state.position[2]);
    ImGui::Text("  Rotation Y: %.1f deg", state.rotation_y * 57.2957795f);

    ImGui::Spacing();
    ImGui::Text("Settings:");
    ImGui::Text("  Eye Height: %.3f m", g_vr_player_tracker->GetEyeHeight());
    ImGui::Text("  World Scale: %.6f", g_vr_player_tracker->GetWorldScale());

    // Toggle first-person mode button
    ImGui::Spacing();
    if (ImGui::Button(g_vr_player_tracker->IsFirstPersonEnabled() ? "Disable First-Person" : "Enable First-Person"))
    {
      g_vr_player_tracker->SetFirstPersonEnabled(!g_vr_player_tracker->IsFirstPersonEnabled());
    }
  }
  else
  {
    ImGui::Text("Player Tracker not initialized");
  }

  ImGui::Spacing();

  // Renderer status
  ImGui::Text("VR Renderer:");
  ImGui::Separator();

  if (g_vr_renderer)
  {
    ImGui::Text("Initialized: %s", g_vr_renderer->IsInitialized() ? "Yes" : "No");

    // Navigation mode
    const char* nav_names[] = {"Tank (Joystick)", "Camera Yaw", "Hybrid", "Snap Turn"};
    ImGui::Text("Nav Mode: %s", nav_names[static_cast<int>(g_vr_renderer->GetActiveNavigationMode())]);
    ImGui::Text("Facing Yaw: %.1f deg", g_vr_renderer->GetFacingYaw() * 57.2957795f);
    if (ImGui::Button("Cycle Nav Mode"))
      g_vr_renderer->CycleNavigationMode();
    ImGui::SameLine();
    if (ImGui::Button("Reset Yaw"))
      g_vr_renderer->ResetNavigationYaw();
  }
  else
  {
    ImGui::Text("Renderer not initialized");
  }

#else
  ImGui::Text("VR support not compiled (ENABLE_OPENXR=0)");
#endif

  ImGui::End();
}

} // namespace VR
