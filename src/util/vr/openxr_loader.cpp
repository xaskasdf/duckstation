// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#include "openxr_loader.h"

#include "common/assert.h"
#include "common/log.h"

#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <dlfcn.h>
#endif

LOG_CHANNEL(VR);

extern "C" {

// Define function pointer storage
#define OPENXR_MODULE_ENTRY_POINT(name, required) PFN_##name name = nullptr;
#define OPENXR_INSTANCE_ENTRY_POINT(name, required) PFN_##name name = nullptr;
#include "openxr_entry_points.inl"
#undef OPENXR_INSTANCE_ENTRY_POINT
#undef OPENXR_MODULE_ENTRY_POINT

} // extern "C"

namespace OpenXR {

void ResetFunctionPointers()
{
#define OPENXR_MODULE_ENTRY_POINT(name, required) name = nullptr;
#define OPENXR_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#include "openxr_entry_points.inl"
#undef OPENXR_INSTANCE_ENTRY_POINT
#undef OPENXR_MODULE_ENTRY_POINT
}

bool CheckResult(XrResult result, const char* func_name)
{
  if (XR_SUCCEEDED(result))
    return true;

  ERROR_LOG("OpenXR call {} failed with result {}", func_name, static_cast<int>(result));
  return false;
}

#if defined(_WIN32)

static HMODULE s_openxr_module = nullptr;

bool IsLoaderLoaded()
{
  return s_openxr_module != nullptr;
}

bool LoadLoader()
{
  // If already loaded, just return success
  if (s_openxr_module)
    return true;

  // Try to load the OpenXR loader
  s_openxr_module = LoadLibraryA("openxr_loader.dll");
  if (!s_openxr_module)
  {
    ERROR_LOG("Failed to load openxr_loader.dll");
    return false;
  }

  bool required_functions_missing = false;
  auto LoadFunction = [&](FARPROC* func_ptr, const char* name, bool is_required) {
    *func_ptr = GetProcAddress(s_openxr_module, name);
    if (!(*func_ptr) && is_required)
    {
      ERROR_LOG("OpenXR: Failed to load required module function {}", name);
      required_functions_missing = true;
    }
  };

#define OPENXR_MODULE_ENTRY_POINT(name, required) \
  LoadFunction(reinterpret_cast<FARPROC*>(&name), #name, required);
#include "openxr_entry_points.inl"
#undef OPENXR_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetFunctionPointers();
    FreeLibrary(s_openxr_module);
    s_openxr_module = nullptr;
    return false;
  }

  INFO_LOG("OpenXR loader library loaded successfully");
  return true;
}

void UnloadLoader()
{
  ResetFunctionPointers();
  if (s_openxr_module)
  {
    FreeLibrary(s_openxr_module);
    s_openxr_module = nullptr;
  }
}

#else // POSIX

static void* s_openxr_module = nullptr;

bool IsLoaderLoaded()
{
  return s_openxr_module != nullptr;
}

bool LoadLoader()
{
  // If already loaded, just return success
  if (s_openxr_module)
    return true;

  // Try different library names
  static const char* search_lib_names[] = {
    "libopenxr_loader.so.1",
    "libopenxr_loader.so",
#ifdef __APPLE__
    "libopenxr_loader.dylib",
#endif
  };

  for (const char* lib_name : search_lib_names)
  {
    s_openxr_module = dlopen(lib_name, RTLD_NOW);
    if (s_openxr_module)
    {
      INFO_LOG("Loaded OpenXR loader from {}", lib_name);
      break;
    }
  }

  if (!s_openxr_module)
  {
    ERROR_LOG("Failed to load OpenXR loader library");
    return false;
  }

  bool required_functions_missing = false;
  auto LoadFunction = [&](void** func_ptr, const char* name, bool is_required) {
    *func_ptr = dlsym(s_openxr_module, name);
    if (!(*func_ptr) && is_required)
    {
      ERROR_LOG("OpenXR: Failed to load required module function {}", name);
      required_functions_missing = true;
    }
  };

#define OPENXR_MODULE_ENTRY_POINT(name, required) \
  LoadFunction(reinterpret_cast<void**>(&name), #name, required);
#include "openxr_entry_points.inl"
#undef OPENXR_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetFunctionPointers();
    dlclose(s_openxr_module);
    s_openxr_module = nullptr;
    return false;
  }

  INFO_LOG("OpenXR loader library loaded successfully");
  return true;
}

void UnloadLoader()
{
  ResetFunctionPointers();
  if (s_openxr_module)
  {
    dlclose(s_openxr_module);
    s_openxr_module = nullptr;
  }
}

#endif // _WIN32

bool LoadInstanceFunctions(XrInstance instance)
{
  if (!xrGetInstanceProcAddr)
  {
    ERROR_LOG("xrGetInstanceProcAddr not loaded");
    return false;
  }

  bool required_functions_missing = false;
  auto LoadFunction = [&](PFN_xrVoidFunction* func_ptr, const char* name, bool is_required) {
    XrResult result = xrGetInstanceProcAddr(instance, name, func_ptr);
    if (!XR_SUCCEEDED(result) || !(*func_ptr))
    {
      if (is_required)
      {
        ERROR_LOG("OpenXR: Failed to load required instance function {} (result={})", name,
                        static_cast<int>(result));
        required_functions_missing = true;
      }
      else
      {
        WARNING_LOG("OpenXR: Optional function {} not available", name);
      }
    }
  };

#define OPENXR_INSTANCE_ENTRY_POINT(name, required) \
  LoadFunction(reinterpret_cast<PFN_xrVoidFunction*>(&name), #name, required);
#include "openxr_entry_points.inl"
#undef OPENXR_INSTANCE_ENTRY_POINT

  if (required_functions_missing)
  {
    ERROR_LOG("Failed to load required OpenXR instance functions");
    return false;
  }

  INFO_LOG("OpenXR instance functions loaded successfully");
  return true;
}

} // namespace OpenXR
