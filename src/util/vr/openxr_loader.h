// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#define XR_NO_PROTOTYPES

// We only need Vulkan graphics binding, not platform-specific Windows extensions
// Don't define XR_USE_PLATFORM_WIN32 to avoid IUnknown dependencies from HoloLens extensions

// Graphics API bindings - Vulkan only
#ifdef ENABLE_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#include "../vulkan_loader.h"  // This already includes vulkan.h with proper setup
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifdef __cplusplus
extern "C" {
#endif

// Declare function pointers for all OpenXR functions
#define OPENXR_MODULE_ENTRY_POINT(name, required) extern PFN_##name name;
#define OPENXR_INSTANCE_ENTRY_POINT(name, required) extern PFN_##name name;
#include "openxr_entry_points.inl"
#undef OPENXR_INSTANCE_ENTRY_POINT
#undef OPENXR_MODULE_ENTRY_POINT

#ifdef __cplusplus
}
#endif

namespace OpenXR {

/// Returns true if the OpenXR loader library is currently loaded.
bool IsLoaderLoaded();

/// Attempts to load the OpenXR loader library.
bool LoadLoader();

/// Loads instance-level functions after XrInstance creation.
bool LoadInstanceFunctions(XrInstance instance);

/// Unloads the OpenXR loader library.
void UnloadLoader();

/// Resets all function pointers to nullptr.
void ResetFunctionPointers();

/// Helper to check XR_SUCCEEDED and log errors
bool CheckResult(XrResult result, const char* func_name);

/// Helper macro for checking OpenXR results
#define XR_CHECK(expr) OpenXR::CheckResult(expr, #expr)

} // namespace OpenXR
