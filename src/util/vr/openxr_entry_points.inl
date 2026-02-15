// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

// OpenXR entry points for dynamic loading
// Usage: Define OPENXR_MODULE_ENTRY_POINT and OPENXR_INSTANCE_ENTRY_POINT macros before including

// Module-level functions (loaded from openxr_loader)
#ifdef OPENXR_MODULE_ENTRY_POINT
OPENXR_MODULE_ENTRY_POINT(xrGetInstanceProcAddr, true)
OPENXR_MODULE_ENTRY_POINT(xrEnumerateApiLayerProperties, true)
OPENXR_MODULE_ENTRY_POINT(xrEnumerateInstanceExtensionProperties, true)
OPENXR_MODULE_ENTRY_POINT(xrCreateInstance, true)
#endif

// Instance-level functions (loaded via xrGetInstanceProcAddr)
#ifdef OPENXR_INSTANCE_ENTRY_POINT
OPENXR_INSTANCE_ENTRY_POINT(xrDestroyInstance, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetInstanceProperties, true)
OPENXR_INSTANCE_ENTRY_POINT(xrPollEvent, true)
OPENXR_INSTANCE_ENTRY_POINT(xrResultToString, true)
OPENXR_INSTANCE_ENTRY_POINT(xrStructureTypeToString, true)

// System functions
OPENXR_INSTANCE_ENTRY_POINT(xrGetSystem, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetSystemProperties, true)
OPENXR_INSTANCE_ENTRY_POINT(xrEnumerateEnvironmentBlendModes, true)

// Session functions
OPENXR_INSTANCE_ENTRY_POINT(xrCreateSession, true)
OPENXR_INSTANCE_ENTRY_POINT(xrDestroySession, true)
OPENXR_INSTANCE_ENTRY_POINT(xrBeginSession, true)
OPENXR_INSTANCE_ENTRY_POINT(xrEndSession, true)
OPENXR_INSTANCE_ENTRY_POINT(xrRequestExitSession, true)
OPENXR_INSTANCE_ENTRY_POINT(xrEnumerateReferenceSpaces, true)
OPENXR_INSTANCE_ENTRY_POINT(xrCreateReferenceSpace, true)
OPENXR_INSTANCE_ENTRY_POINT(xrDestroySpace, true)
OPENXR_INSTANCE_ENTRY_POINT(xrLocateSpace, true)

// View functions
OPENXR_INSTANCE_ENTRY_POINT(xrEnumerateViewConfigurations, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetViewConfigurationProperties, true)
OPENXR_INSTANCE_ENTRY_POINT(xrEnumerateViewConfigurationViews, true)
OPENXR_INSTANCE_ENTRY_POINT(xrLocateViews, true)

// Swapchain functions
OPENXR_INSTANCE_ENTRY_POINT(xrEnumerateSwapchainFormats, true)
OPENXR_INSTANCE_ENTRY_POINT(xrCreateSwapchain, true)
OPENXR_INSTANCE_ENTRY_POINT(xrDestroySwapchain, true)
OPENXR_INSTANCE_ENTRY_POINT(xrEnumerateSwapchainImages, true)
OPENXR_INSTANCE_ENTRY_POINT(xrAcquireSwapchainImage, true)
OPENXR_INSTANCE_ENTRY_POINT(xrWaitSwapchainImage, true)
OPENXR_INSTANCE_ENTRY_POINT(xrReleaseSwapchainImage, true)

// Frame functions
OPENXR_INSTANCE_ENTRY_POINT(xrWaitFrame, true)
OPENXR_INSTANCE_ENTRY_POINT(xrBeginFrame, true)
OPENXR_INSTANCE_ENTRY_POINT(xrEndFrame, true)

// Action functions
OPENXR_INSTANCE_ENTRY_POINT(xrCreateActionSet, true)
OPENXR_INSTANCE_ENTRY_POINT(xrDestroyActionSet, true)
OPENXR_INSTANCE_ENTRY_POINT(xrCreateAction, true)
OPENXR_INSTANCE_ENTRY_POINT(xrDestroyAction, true)
OPENXR_INSTANCE_ENTRY_POINT(xrSuggestInteractionProfileBindings, true)
OPENXR_INSTANCE_ENTRY_POINT(xrAttachSessionActionSets, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetCurrentInteractionProfile, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetActionStateBoolean, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetActionStateFloat, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetActionStateVector2f, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetActionStatePose, true)
OPENXR_INSTANCE_ENTRY_POINT(xrSyncActions, true)
OPENXR_INSTANCE_ENTRY_POINT(xrEnumerateBoundSourcesForAction, true)
OPENXR_INSTANCE_ENTRY_POINT(xrGetInputSourceLocalizedName, true)
OPENXR_INSTANCE_ENTRY_POINT(xrCreateActionSpace, true)

// Path functions
OPENXR_INSTANCE_ENTRY_POINT(xrStringToPath, true)
OPENXR_INSTANCE_ENTRY_POINT(xrPathToString, true)

// Vulkan extension functions (loaded if XR_KHR_vulkan_enable2 is available)
OPENXR_INSTANCE_ENTRY_POINT(xrGetVulkanInstanceExtensionsKHR, false)
OPENXR_INSTANCE_ENTRY_POINT(xrGetVulkanDeviceExtensionsKHR, false)
OPENXR_INSTANCE_ENTRY_POINT(xrGetVulkanGraphicsDeviceKHR, false)
OPENXR_INSTANCE_ENTRY_POINT(xrGetVulkanGraphicsRequirementsKHR, false)
OPENXR_INSTANCE_ENTRY_POINT(xrCreateVulkanInstanceKHR, false)
OPENXR_INSTANCE_ENTRY_POINT(xrCreateVulkanDeviceKHR, false)
OPENXR_INSTANCE_ENTRY_POINT(xrGetVulkanGraphicsDevice2KHR, false)
OPENXR_INSTANCE_ENTRY_POINT(xrGetVulkanGraphicsRequirements2KHR, false)
#endif
