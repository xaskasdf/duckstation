// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/types.h"

// Forward declarations
class GPUDevice;

namespace VR {

/// Check if VR hardware is available
bool IsHardwareAvailable();

/// Initialize VR system (call early, before GPU device creation)
bool InitializeSystem();

/// Create VR session using the current GPU device (call after GPU device creation)
/// Requires Vulkan backend
bool CreateSession(GPUDevice* gpu_device);

/// Destroy VR session (call before GPU device destruction)
void DestroySession();

// Note: Use VR::Shutdown() from vr_system.h to shutdown the VR system completely

/// Check if VR is currently active and rendering
bool IsActive();

/// Check if we should render this frame
bool ShouldRender();

/// Begin VR frame - call at start of frame rendering
/// Returns false if frame should be skipped
bool BeginFrame();

/// Acquire swapchain image for an eye
/// @param eye 0 = left, 1 = right
/// @return Image index or -1 on error
s32 AcquireEyeImage(u32 eye);

/// Release swapchain image after rendering
bool ReleaseEyeImage(u32 eye);

/// End VR frame - submit to compositor
bool EndFrame();

/// Get render dimensions for VR
void GetRenderDimensions(u32* width, u32* height);

/// Get the Vulkan image handle for an eye's current swapchain image
/// Useful for rendering to the VR swapchain
void* GetEyeImage(u32 eye);

/// Process VR events (call every frame)
void ProcessEvents();

/// Render the current frame in stereo to VR headset
/// Should be called between BeginFrame() and EndFrame()
bool RenderStereo();

/// Draw VR debug information window (ImGui)
void DrawDebugWindow();

} // namespace VR
