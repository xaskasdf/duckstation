// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#include "vr_system.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/screenshot_3d.h"

#include <cstring>

LOG_CHANNEL(VR);

namespace VR {

std::unique_ptr<System> g_vr_system;

bool Initialize()
{
  if (g_vr_system)
    return true;

  if (!OpenXR::LoadLoader())
  {
    ERROR_LOG("Failed to load OpenXR loader");
    return false;
  }

  g_vr_system = std::make_unique<System>();
  if (!g_vr_system->Initialize())
  {
    g_vr_system.reset();
    OpenXR::UnloadLoader();
    return false;
  }

  return true;
}

void Shutdown()
{
  if (g_vr_system)
  {
    g_vr_system->Shutdown();
    g_vr_system.reset();
  }
  OpenXR::UnloadLoader();
}

bool IsAvailable()
{
  // If VR system is already initialized, it's obviously available
  if (g_vr_system)
    return true;

  // Cache the result to avoid repeatedly loading/unloading the loader
  static bool s_checked = false;
  static bool s_available = false;

  if (s_checked)
    return s_available;

  // Try to load the loader to check availability
  if (!OpenXR::LoadLoader())
  {
    s_checked = true;
    s_available = false;
    return false;
  }

  // Check if we can enumerate extensions (basic availability check)
  u32 extension_count = 0;
  XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);

  // Only unload if we're just checking - don't unload if it was already loaded before
  OpenXR::UnloadLoader();

  s_checked = true;
  s_available = XR_SUCCEEDED(result);
  return s_available;
}

System::System() = default;

System::~System()
{
  Shutdown();
}

bool System::Initialize()
{
  if (m_initialized)
    return true;

  if (!CreateInstance())
    return false;

  if (!GetSystem())
  {
    Shutdown();
    return false;
  }

  if (!EnumerateViewConfigurations())
  {
    Shutdown();
    return false;
  }

  m_initialized = true;
  INFO_LOG("VR System initialized: {}", m_system_name.c_str());
  return true;
}

void System::Shutdown()
{
  if (m_session != XR_NULL_HANDLE)
    DestroySession();

  if (m_instance != XR_NULL_HANDLE)
  {
    xrDestroyInstance(m_instance);
    m_instance = XR_NULL_HANDLE;
  }

  m_system_id = XR_NULL_SYSTEM_ID;
  m_initialized = false;
}

bool System::CreateInstance()
{
  // Enumerate available extensions
  u32 extension_count = 0;
  XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr));

  std::vector<XrExtensionProperties> extensions(extension_count, {XR_TYPE_EXTENSION_PROPERTIES});
  XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data()));

  INFO_LOG("Available OpenXR extensions ({}):", extension_count);
  bool has_vulkan_enable = false;
  bool has_vulkan_enable2 = false;

  for (const auto& ext : extensions)
  {
    INFO_LOG("  {} (v{})", ext.extensionName, ext.extensionVersion);
    if (std::strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
      has_vulkan_enable = true;
    if (std::strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
      has_vulkan_enable2 = true;
  }

  if (!has_vulkan_enable && !has_vulkan_enable2)
  {
    ERROR_LOG("OpenXR runtime does not support Vulkan graphics binding");
    return false;
  }

  // Request required extensions - prefer vulkan_enable for broader compatibility
  std::vector<const char*> enabled_extensions;
  if (has_vulkan_enable)
  {
    enabled_extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    m_using_vulkan_enable2 = false;
    INFO_LOG("Using XR_KHR_vulkan_enable extension");
  }
  else if (has_vulkan_enable2)
  {
    enabled_extensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    m_using_vulkan_enable2 = true;
    INFO_LOG("Using XR_KHR_vulkan_enable2 extension");
  }

  // Create instance
  XrInstanceCreateInfo create_info = {XR_TYPE_INSTANCE_CREATE_INFO};
  std::strcpy(create_info.applicationInfo.applicationName, "DuckStation VR");
  create_info.applicationInfo.applicationVersion = 1;
  std::strcpy(create_info.applicationInfo.engineName, "DuckStation");
  create_info.applicationInfo.engineVersion = 1;
  // Use OpenXR 1.0 for maximum compatibility with runtimes
  create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create_info.enabledExtensionCount = static_cast<u32>(enabled_extensions.size());
  create_info.enabledExtensionNames = enabled_extensions.data();

  XrResult result = xrCreateInstance(&create_info, &m_instance);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to create OpenXR instance: {}", static_cast<int>(result));
    return false;
  }

  // Load instance functions
  if (!OpenXR::LoadInstanceFunctions(m_instance))
  {
    xrDestroyInstance(m_instance);
    m_instance = XR_NULL_HANDLE;
    return false;
  }

  // Get instance properties
  XrInstanceProperties instance_props = {XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &instance_props)))
  {
    INFO_LOG("OpenXR Runtime: {} (v{}.{}.{})", instance_props.runtimeName,
                   XR_VERSION_MAJOR(instance_props.runtimeVersion),
                   XR_VERSION_MINOR(instance_props.runtimeVersion),
                   XR_VERSION_PATCH(instance_props.runtimeVersion));
  }

  return true;
}

bool System::GetSystem()
{
  XrSystemGetInfo system_info = {XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  XrResult result = xrGetSystem(m_instance, &system_info, &m_system_id);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to get OpenXR system (HMD): {}. Is your headset connected?", static_cast<int>(result));
    return false;
  }

  // Get system properties
  m_system_properties = {XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_system_id, &m_system_properties)))
  {
    m_system_name = m_system_properties.systemName;
    INFO_LOG("OpenXR System: {} (vendorId={})", m_system_properties.systemName, m_system_properties.vendorId);
    INFO_LOG("  Max layers: {}, Max swapchain size: {}x{}",
                   m_system_properties.graphicsProperties.maxLayerCount,
                   m_system_properties.graphicsProperties.maxSwapchainImageWidth,
                   m_system_properties.graphicsProperties.maxSwapchainImageHeight);
  }

  return true;
}

bool System::EnumerateViewConfigurations()
{
  // Get supported view configurations
  u32 config_count = 0;
  XR_CHECK(xrEnumerateViewConfigurations(m_instance, m_system_id, 0, &config_count, nullptr));

  std::vector<XrViewConfigurationType> configs(config_count);
  XR_CHECK(xrEnumerateViewConfigurations(m_instance, m_system_id, config_count, &config_count, configs.data()));

  // Look for stereo view configuration
  bool found_stereo = false;
  for (auto config : configs)
  {
    if (config == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
      m_view_config_type = config;
      found_stereo = true;
      break;
    }
  }

  if (!found_stereo)
  {
    ERROR_LOG("OpenXR system does not support stereo view configuration");
    return false;
  }

  // Get view configuration views (one per eye)
  u32 view_count = 0;
  XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system_id, m_view_config_type, 0, &view_count, nullptr));

  std::vector<XrViewConfigurationView> config_views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system_id, m_view_config_type, view_count, &view_count,
                                              config_views.data()));

  m_views.resize(view_count);
  for (u32 i = 0; i < view_count; i++)
  {
    m_views[i].config_view = config_views[i];
    m_views[i].view = {XR_TYPE_VIEW};
    INFO_LOG("View {}: recommended {}x{}, max {}x{}, samples {}", i,
                   config_views[i].recommendedImageRectWidth, config_views[i].recommendedImageRectHeight,
                   config_views[i].maxImageRectWidth, config_views[i].maxImageRectHeight,
                   config_views[i].recommendedSwapchainSampleCount);
  }

  // Get blend modes
  u32 blend_count = 0;
  XR_CHECK(xrEnumerateEnvironmentBlendModes(m_instance, m_system_id, m_view_config_type, 0, &blend_count, nullptr));

  std::vector<XrEnvironmentBlendMode> blend_modes(blend_count);
  XR_CHECK(xrEnumerateEnvironmentBlendModes(m_instance, m_system_id, m_view_config_type, blend_count, &blend_count,
                                             blend_modes.data()));

  // Prefer opaque
  m_blend_mode = blend_modes[0];
  for (auto mode : blend_modes)
  {
    if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
    {
      m_blend_mode = mode;
      break;
    }
  }

  return true;
}

std::vector<std::string> System::GetRequiredVulkanInstanceExtensions() const
{
  std::vector<std::string> extensions;

  if (!xrGetVulkanInstanceExtensionsKHR)
    return extensions;

  u32 buffer_size = 0;
  xrGetVulkanInstanceExtensionsKHR(m_instance, m_system_id, 0, &buffer_size, nullptr);

  if (buffer_size == 0)
    return extensions;

  std::string extension_string;
  extension_string.resize(buffer_size);
  xrGetVulkanInstanceExtensionsKHR(m_instance, m_system_id, buffer_size, &buffer_size, extension_string.data());

  // Parse space-separated extension names
  auto views = StringUtil::SplitString(extension_string.c_str(), ' ', true);
  for (const auto& view : views)
    extensions.emplace_back(view);
  return extensions;
}

std::vector<std::string> System::GetRequiredVulkanDeviceExtensions() const
{
  std::vector<std::string> extensions;

  if (!xrGetVulkanDeviceExtensionsKHR)
    return extensions;

  u32 buffer_size = 0;
  xrGetVulkanDeviceExtensionsKHR(m_instance, m_system_id, 0, &buffer_size, nullptr);

  if (buffer_size == 0)
    return extensions;

  std::string extension_string;
  extension_string.resize(buffer_size);
  xrGetVulkanDeviceExtensionsKHR(m_instance, m_system_id, buffer_size, &buffer_size, extension_string.data());

  auto views = StringUtil::SplitString(extension_string.c_str(), ' ', true);
  for (const auto& view : views)
    extensions.emplace_back(view);
  return extensions;
}

bool System::GetVulkanGraphicsDevice(VkInstance vk_instance, VkPhysicalDevice* out_physical_device) const
{
  if (xrGetVulkanGraphicsDevice2KHR)
  {
    XrVulkanGraphicsDeviceGetInfoKHR get_info = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    get_info.systemId = m_system_id;
    get_info.vulkanInstance = vk_instance;

    return XR_SUCCEEDED(xrGetVulkanGraphicsDevice2KHR(m_instance, &get_info, out_physical_device));
  }
  else if (xrGetVulkanGraphicsDeviceKHR)
  {
    return XR_SUCCEEDED(xrGetVulkanGraphicsDeviceKHR(m_instance, m_system_id, vk_instance, out_physical_device));
  }

  return false;
}

bool System::CreateSession(VkInstance vk_instance, VkPhysicalDevice vk_physical_device, VkDevice vk_device,
                           u32 vk_queue_family_index)
{
  if (m_session != XR_NULL_HANDLE)
  {
    WARNING_LOG("Session already exists");
    return true;
  }

  INFO_LOG("CreateSession: vk_instance={}, vk_physical_device={}, vk_device={}, queue_family={}",
                 static_cast<void*>(vk_instance), static_cast<void*>(vk_physical_device), static_cast<void*>(vk_device), vk_queue_family_index);
  INFO_LOG("CreateSession: m_instance={}, m_system_id={}",
                 reinterpret_cast<void*>(m_instance), static_cast<unsigned long long>(m_system_id));

  // Log the required Vulkan extensions that OpenXR wants
  if (xrGetVulkanInstanceExtensionsKHR)
  {
    u32 buffer_size = 0;
    XrResult ext_result = xrGetVulkanInstanceExtensionsKHR(m_instance, m_system_id, 0, &buffer_size, nullptr);
    if (XR_SUCCEEDED(ext_result) && buffer_size > 0)
    {
      std::string ext_string;
      ext_string.resize(buffer_size);
      xrGetVulkanInstanceExtensionsKHR(m_instance, m_system_id, buffer_size, &buffer_size, ext_string.data());
      INFO_LOG("OpenXR required Vulkan INSTANCE extensions: {}", ext_string.c_str());
    }
  }

  if (xrGetVulkanDeviceExtensionsKHR)
  {
    u32 buffer_size = 0;
    XrResult ext_result = xrGetVulkanDeviceExtensionsKHR(m_instance, m_system_id, 0, &buffer_size, nullptr);
    if (XR_SUCCEEDED(ext_result) && buffer_size > 0)
    {
      std::string ext_string;
      ext_string.resize(buffer_size);
      xrGetVulkanDeviceExtensionsKHR(m_instance, m_system_id, buffer_size, &buffer_size, ext_string.data());
      INFO_LOG("OpenXR required Vulkan DEVICE extensions: {}", ext_string.c_str());
    }
  }

  // Check if xrGetVulkanGraphicsDeviceKHR is available and verify the physical device
  if (xrGetVulkanGraphicsDeviceKHR)
  {
    VkPhysicalDevice recommended_device = VK_NULL_HANDLE;
    XrResult dev_result = xrGetVulkanGraphicsDeviceKHR(m_instance, m_system_id, vk_instance, &recommended_device);
    if (XR_SUCCEEDED(dev_result))
    {
      INFO_LOG("OpenXR recommended physical device: {}", static_cast<void*>(recommended_device));
      if (recommended_device != vk_physical_device)
      {
        WARNING_LOG("Physical device mismatch! DuckStation: {}, OpenXR recommends: {}",
                          static_cast<void*>(vk_physical_device), static_cast<void*>(recommended_device));
        WARNING_LOG("This may cause issues with some OpenXR runtimes");
      }
      else
      {
        INFO_LOG("Physical device matches OpenXR recommendation");
      }
    }
    else
    {
      WARNING_LOG("xrGetVulkanGraphicsDeviceKHR failed: {}", static_cast<int>(dev_result));
    }
  }
  else
  {
    INFO_LOG("xrGetVulkanGraphicsDeviceKHR not available, skipping device verification");
  }

  // REQUIRED: Check graphics requirements BEFORE creating session
  // The spec requires this call to be made before xrCreateSession
  if (m_using_vulkan_enable2 && xrGetVulkanGraphicsRequirements2KHR)
  {
    XrVulkanGraphicsDeviceGetInfoKHR device_info = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    device_info.systemId = m_system_id;
    device_info.vulkanInstance = vk_instance;

    XrGraphicsRequirementsVulkan2KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XrResult req_result = xrGetVulkanGraphicsRequirements2KHR(m_instance, m_system_id, &requirements);
    if (!XR_SUCCEEDED(req_result))
    {
      ERROR_LOG("Failed to get Vulkan graphics requirements (v2): {}", static_cast<int>(req_result));
      return false;
    }
    INFO_LOG("Vulkan requirements (v2): min={}.{}.{}, max={}.{}.{}",
                   XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                   XR_VERSION_MINOR(requirements.minApiVersionSupported),
                   XR_VERSION_PATCH(requirements.minApiVersionSupported),
                   XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                   XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                   XR_VERSION_PATCH(requirements.maxApiVersionSupported));
  }
  else if (xrGetVulkanGraphicsRequirementsKHR)
  {
    INFO_LOG("Calling xrGetVulkanGraphicsRequirementsKHR (function ptr: {})...",
                   reinterpret_cast<void*>(xrGetVulkanGraphicsRequirementsKHR));

    XrGraphicsRequirementsVulkanKHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    requirements.next = nullptr;
    requirements.minApiVersionSupported = 0;
    requirements.maxApiVersionSupported = 0;

    XrResult req_result = xrGetVulkanGraphicsRequirementsKHR(m_instance, m_system_id, &requirements);
    INFO_LOG("xrGetVulkanGraphicsRequirementsKHR returned: {}", static_cast<int>(req_result));

    if (!XR_SUCCEEDED(req_result))
    {
      ERROR_LOG("Failed to get Vulkan graphics requirements: {}", static_cast<int>(req_result));
      return false;
    }
    INFO_LOG("Vulkan requirements: min=0x{:x} ({}.{}.{}), max=0x{:x} ({}.{}.{})",
                   static_cast<unsigned long long>(requirements.minApiVersionSupported),
                   XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                   XR_VERSION_MINOR(requirements.minApiVersionSupported),
                   XR_VERSION_PATCH(requirements.minApiVersionSupported),
                   static_cast<unsigned long long>(requirements.maxApiVersionSupported),
                   XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                   XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                   XR_VERSION_PATCH(requirements.maxApiVersionSupported));
  }
  else
  {
    ERROR_LOG("Neither xrGetVulkanGraphicsRequirementsKHR nor xrGetVulkanGraphicsRequirements2KHR is available");
    return false;
  }

  // Create Vulkan graphics binding
  XrGraphicsBindingVulkanKHR graphics_binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  graphics_binding.instance = vk_instance;
  graphics_binding.physicalDevice = vk_physical_device;
  graphics_binding.device = vk_device;
  graphics_binding.queueFamilyIndex = vk_queue_family_index;
  graphics_binding.queueIndex = 0;

  INFO_LOG("Creating OpenXR session with Vulkan device (queueFamilyIndex={})", vk_queue_family_index);

  XrSessionCreateInfo session_create_info = {XR_TYPE_SESSION_CREATE_INFO};
  session_create_info.next = &graphics_binding;
  session_create_info.systemId = m_system_id;

  XrResult result = xrCreateSession(m_instance, &session_create_info, &m_session);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to create OpenXR session: {}", static_cast<int>(result));
    return false;
  }

  INFO_LOG("OpenXR session created");

  if (!CreateReferenceSpace())
  {
    DestroySession();
    return false;
  }

  if (!CreateSwapchains())
  {
    DestroySession();
    return false;
  }

  return true;
}

void System::DestroySession()
{
  DestroySwapchains();

  if (m_reference_space != XR_NULL_HANDLE)
  {
    xrDestroySpace(m_reference_space);
    m_reference_space = XR_NULL_HANDLE;
  }

  if (m_session != XR_NULL_HANDLE)
  {
    xrDestroySession(m_session);
    m_session = XR_NULL_HANDLE;
  }

  m_session_running = false;
  m_session_state = XR_SESSION_STATE_UNKNOWN;
}

bool System::CreateReferenceSpace()
{
  XrReferenceSpaceCreateInfo space_create_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  space_create_info.poseInReferenceSpace.orientation.w = 1.0f; // Identity quaternion

  XrResult result = xrCreateReferenceSpace(m_session, &space_create_info, &m_reference_space);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to create reference space: {}", static_cast<int>(result));
    return false;
  }

  return true;
}

bool System::CreateSwapchains()
{
  // Enumerate swapchain formats
  u32 format_count = 0;
  XR_CHECK(xrEnumerateSwapchainFormats(m_session, 0, &format_count, nullptr));

  std::vector<int64_t> formats(format_count);
  XR_CHECK(xrEnumerateSwapchainFormats(m_session, format_count, &format_count, formats.data()));

  // Validate we have at least one format
  if (formats.empty())
  {
    ERROR_LOG("VR: No swapchain formats available");
    return false;
  }

  // Prefer SRGB formats
  int64_t selected_format = formats[0];
  for (int64_t format : formats)
  {
    if (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB)
    {
      selected_format = format;
      break;
    }
  }

  INFO_LOG("Selected swapchain format: {}", static_cast<long long>(selected_format));

  // Create swapchain for each eye
  for (u32 eye = 0; eye < 2 && eye < m_views.size(); eye++)
  {
    const auto& view = m_views[eye];
    auto& swapchain = m_swapchains[eye];

    XrSwapchainCreateInfo swapchain_create_info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchain_create_info.format = selected_format;
    swapchain_create_info.sampleCount = view.config_view.recommendedSwapchainSampleCount;
    swapchain_create_info.width = view.config_view.recommendedImageRectWidth;
    swapchain_create_info.height = view.config_view.recommendedImageRectHeight;
    swapchain_create_info.faceCount = 1;
    swapchain_create_info.arraySize = 1;
    swapchain_create_info.mipCount = 1;

    XrResult result = xrCreateSwapchain(m_session, &swapchain_create_info, &swapchain.swapchain);
    if (!XR_SUCCEEDED(result))
    {
      ERROR_LOG("Failed to create swapchain for eye {}: {}", eye, static_cast<int>(result));
      return false;
    }

    swapchain.format = selected_format;
    swapchain.width = swapchain_create_info.width;
    swapchain.height = swapchain_create_info.height;
    swapchain.sample_count = swapchain_create_info.sampleCount;

    // Get swapchain images
    u32 image_count = 0;
    XR_CHECK(xrEnumerateSwapchainImages(swapchain.swapchain, 0, &image_count, nullptr));

    swapchain.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(swapchain.swapchain, image_count, &image_count,
                                         reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())));

    INFO_LOG("Eye {} swapchain: {}x{}, {} images", eye, swapchain.width, swapchain.height, image_count);
  }

  // Create quad swapchain for 2D content (menus, loading screens, HUD)
  if (!CreateQuadSwapchain(selected_format))
  {
    WARNING_LOG("Failed to create quad swapchain, 2D overlay disabled");
    // Non-fatal — VR still works without the quad layer
  }

  return true;
}

void System::DestroySwapchains()
{
  DestroyQuadSwapchain();

  for (auto& swapchain : m_swapchains)
  {
    if (swapchain.swapchain != XR_NULL_HANDLE)
    {
      xrDestroySwapchain(swapchain.swapchain);
      swapchain.swapchain = XR_NULL_HANDLE;
    }
    swapchain.images.clear();
  }
}

bool System::CreateQuadSwapchain(int64_t format)
{
  XrSwapchainCreateInfo create_info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  create_info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                           XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                           XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  create_info.format = format;
  create_info.sampleCount = 1;
  create_info.width = 640;
  create_info.height = 480;
  create_info.faceCount = 1;
  create_info.arraySize = 1;
  create_info.mipCount = 1;

  XrResult result = xrCreateSwapchain(m_session, &create_info, &m_quad_swapchain.swapchain);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to create quad swapchain: {}", static_cast<int>(result));
    return false;
  }

  m_quad_swapchain.format = format;
  m_quad_swapchain.width = create_info.width;
  m_quad_swapchain.height = create_info.height;
  m_quad_swapchain.sample_count = create_info.sampleCount;

  u32 image_count = 0;
  XR_CHECK(xrEnumerateSwapchainImages(m_quad_swapchain.swapchain, 0, &image_count, nullptr));

  m_quad_swapchain.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  XR_CHECK(xrEnumerateSwapchainImages(m_quad_swapchain.swapchain, image_count, &image_count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(m_quad_swapchain.images.data())));

  m_quad_swapchain_valid = true;
  INFO_LOG("Quad swapchain created: {}x{}, {} images", create_info.width, create_info.height, image_count);
  return true;
}

void System::DestroyQuadSwapchain()
{
  if (m_quad_swapchain.swapchain != XR_NULL_HANDLE)
  {
    xrDestroySwapchain(m_quad_swapchain.swapchain);
    m_quad_swapchain.swapchain = XR_NULL_HANDLE;
  }
  m_quad_swapchain.images.clear();
  m_quad_swapchain_valid = false;
}

bool System::AcquireQuadSwapchainImage()
{
  if (!m_quad_swapchain_valid || m_quad_swapchain.swapchain == XR_NULL_HANDLE)
    return false;

  XrSwapchainImageAcquireInfo acquire_info = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  u32 image_index = 0;
  XrResult result = xrAcquireSwapchainImage(m_quad_swapchain.swapchain, &acquire_info, &image_index);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to acquire quad swapchain image: {}", static_cast<int>(result));
    return false;
  }

  if (image_index >= m_quad_swapchain.images.size())
  {
    ERROR_LOG("VR: Quad swapchain image index {} out of bounds (max {})", image_index, m_quad_swapchain.images.size());
    return false;
  }

  m_quad_swapchain.current_image_index = image_index;

  XrSwapchainImageWaitInfo wait_info = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;

  result = xrWaitSwapchainImage(m_quad_swapchain.swapchain, &wait_info);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to wait for quad swapchain image: {}", static_cast<int>(result));
    m_quad_swapchain.current_image_index = 0;
    return false;
  }

  return true;
}

bool System::ReleaseQuadSwapchainImage()
{
  if (!m_quad_swapchain_valid || m_quad_swapchain.swapchain == XR_NULL_HANDLE)
    return false;

  XrSwapchainImageReleaseInfo release_info = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  XrResult result = xrReleaseSwapchainImage(m_quad_swapchain.swapchain, &release_info);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to release quad swapchain image: {}", static_cast<int>(result));
    return false;
  }

  return true;
}

bool System::AcquireSwapchainImage(u32 eye)
{
  if (eye >= 2)
    return false;

  auto& swapchain = m_swapchains[eye];

  // Validate swapchain is initialized
  if (swapchain.swapchain == XR_NULL_HANDLE || swapchain.images.empty())
  {
    ERROR_LOG("VR: Swapchain {} not initialized", eye);
    return false;
  }

  XrSwapchainImageAcquireInfo acquire_info = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  u32 image_index = 0;
  XrResult result = xrAcquireSwapchainImage(swapchain.swapchain, &acquire_info, &image_index);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to acquire swapchain image for eye {}: {}", eye, static_cast<int>(result));
    return false;
  }

  // CRITICAL: Bounds check before storing
  if (image_index >= swapchain.images.size())
  {
    ERROR_LOG("VR: Acquired image index {} out of bounds (max {})", image_index, swapchain.images.size());
    return false;
  }

  swapchain.current_image_index = image_index;

  XrSwapchainImageWaitInfo wait_info = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;

  result = xrWaitSwapchainImage(swapchain.swapchain, &wait_info);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to wait for swapchain image for eye {}: {}", eye, static_cast<int>(result));
    swapchain.current_image_index = 0;  // Reset to safe value
    return false;
  }

  return true;
}

bool System::ReleaseSwapchainImage(u32 eye)
{
  auto& swapchain = m_swapchains[eye];

  XrSwapchainImageReleaseInfo release_info = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  XrResult result = xrReleaseSwapchainImage(swapchain.swapchain, &release_info);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to release swapchain image for eye {}: {}", eye, static_cast<int>(result));
    return false;
  }

  return true;
}

u32 System::GetRenderWidth() const
{
  if (m_views.empty())
    return 0;
  return m_views[0].config_view.recommendedImageRectWidth;
}

u32 System::GetRenderHeight() const
{
  if (m_views.empty())
    return 0;
  return m_views[0].config_view.recommendedImageRectHeight;
}

void System::ProcessEvents()
{
  XrEventDataBuffer event_buffer = {XR_TYPE_EVENT_DATA_BUFFER};

  while (true)
  {
    event_buffer = {XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result = xrPollEvent(m_instance, &event_buffer);

    if (result == XR_EVENT_UNAVAILABLE)
      break;

    if (!XR_SUCCEEDED(result))
    {
      ERROR_LOG("xrPollEvent failed: {}", static_cast<int>(result));
      break;
    }

    switch (event_buffer.type)
    {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        HandleSessionStateChange(reinterpret_cast<XrEventDataSessionStateChanged*>(&event_buffer));
        break;

      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        WARNING_LOG("OpenXR instance loss pending");
        m_exit_requested = true;
        break;

      default:
        DEV_LOG("Unhandled OpenXR event type: {}", static_cast<int>(event_buffer.type));
        break;
    }
  }
}

void System::HandleSessionStateChange(XrEventDataSessionStateChanged* event)
{
  XrSessionState old_state = m_session_state;
  m_session_state = event->state;

  INFO_LOG("OpenXR session state changed: {} -> {}", static_cast<int>(old_state),
                 static_cast<int>(m_session_state));

  switch (m_session_state)
  {
    case XR_SESSION_STATE_READY:
    {
      XrSessionBeginInfo begin_info = {XR_TYPE_SESSION_BEGIN_INFO};
      begin_info.primaryViewConfigurationType = m_view_config_type;
      if (XR_SUCCEEDED(xrBeginSession(m_session, &begin_info)))
      {
        m_session_running = true;
        INFO_LOG("OpenXR session started");
      }
      break;
    }

    case XR_SESSION_STATE_FOCUSED:
      // Enable 3D geometry capture for VR when session becomes focused
      INFO_LOG("Enabling 3D geometry capture for VR");
      Screenshot3D::SetVRContinuousMode(true);
      break;

    case XR_SESSION_STATE_STOPPING:
      // Disable 3D geometry capture when session stops
      Screenshot3D::SetVRContinuousMode(false);
      xrEndSession(m_session);
      m_session_running = false;
      INFO_LOG("OpenXR session stopped");
      break;

    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
      Screenshot3D::SetVRContinuousMode(false);
      m_exit_requested = true;
      m_session_running = false;
      break;

    default:
      break;
  }
}

bool System::BeginFrame(FrameState& out_frame_state)
{
  if (!m_session_running)
    return false;

  XrFrameWaitInfo wait_info = {XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame_state = {XR_TYPE_FRAME_STATE};

  XrResult result = xrWaitFrame(m_session, &wait_info, &frame_state);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("xrWaitFrame failed: {}", static_cast<int>(result));
    return false;
  }

  out_frame_state.predicted_display_time = frame_state.predictedDisplayTime;
  out_frame_state.predicted_display_period = frame_state.predictedDisplayPeriod;
  out_frame_state.should_render = (frame_state.shouldRender == XR_TRUE);

  XrFrameBeginInfo begin_info = {XR_TYPE_FRAME_BEGIN_INFO};
  result = xrBeginFrame(m_session, &begin_info);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("xrBeginFrame failed: {}", static_cast<int>(result));
    return false;
  }

  return true;
}

bool System::LocateViews(XrTime display_time)
{
  XrViewState view_state = {XR_TYPE_VIEW_STATE};
  XrViewLocateInfo locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = m_view_config_type;
  locate_info.displayTime = display_time;
  locate_info.space = m_reference_space;

  std::vector<XrView> views(m_views.size(), {XR_TYPE_VIEW});
  u32 view_count = 0;

  XrResult result = xrLocateViews(m_session, &locate_info, &view_state, static_cast<u32>(views.size()), &view_count,
                                   views.data());
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("xrLocateViews failed: {}", static_cast<int>(result));
    return false;
  }

  for (u32 i = 0; i < view_count && i < m_views.size(); i++)
  {
    m_views[i].view = views[i];
    m_views[i].pose = views[i].pose;
    m_views[i].fov = views[i].fov;
  }

  return true;
}

bool System::EndFrame(const FrameState& frame_state,
                      bool submit_quad_layer,
                      const XrPosef* quad_pose,
                      const XrExtent2Df* quad_size)
{
  if (!m_session_running)
    return false;

  // Build projection views
  std::vector<XrCompositionLayerProjectionView> projection_views(m_views.size());

  for (u32 i = 0; i < m_views.size() && i < 2; i++)
  {
    projection_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    projection_views[i].pose = m_views[i].pose;
    projection_views[i].fov = m_views[i].fov;
    projection_views[i].subImage.swapchain = m_swapchains[i].swapchain;
    projection_views[i].subImage.imageRect.offset = {0, 0};
    projection_views[i].subImage.imageRect.extent = {
      static_cast<int32_t>(m_swapchains[i].width),
      static_cast<int32_t>(m_swapchains[i].height)
    };
    projection_views[i].subImage.imageArrayIndex = 0;
  }

  // Projection layer (3D geometry) — with alpha blending so transparent areas show the quad behind
  XrCompositionLayerProjection projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  projection_layer.space = m_reference_space;
  projection_layer.viewCount = static_cast<u32>(projection_views.size());
  projection_layer.views = projection_views.data();

  // Build layers array: quad behind, projection in front
  // OpenXR composites layers in array order: first = bottom, last = top
  const XrCompositionLayerBaseHeader* layers[2] = {};
  u32 layer_count = 0;

  // Quad layer (behind) — shows 2D emulator display as floating virtual screen
  XrCompositionLayerQuad quad_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  if (submit_quad_layer && m_quad_swapchain_valid && quad_pose && quad_size)
  {
    quad_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad_layer.space = m_reference_space;
    quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_layer.subImage.swapchain = m_quad_swapchain.swapchain;
    quad_layer.subImage.imageRect.offset = {0, 0};
    quad_layer.subImage.imageRect.extent = {
      static_cast<int32_t>(m_quad_swapchain.width),
      static_cast<int32_t>(m_quad_swapchain.height)
    };
    quad_layer.subImage.imageArrayIndex = 0;
    quad_layer.pose = *quad_pose;
    quad_layer.size = *quad_size;

    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad_layer);
  }

  // Projection layer (in front)
  layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection_layer);

  XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = frame_state.predicted_display_time;
  end_info.environmentBlendMode = m_blend_mode;
  end_info.layerCount = frame_state.should_render ? layer_count : 0;
  end_info.layers = frame_state.should_render ? layers : nullptr;

  XrResult result = xrEndFrame(m_session, &end_info);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("xrEndFrame failed: {}", static_cast<int>(result));
    return false;
  }

  return true;
}

} // namespace VR
