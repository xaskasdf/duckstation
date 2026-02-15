// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#include "openxr_input_source.h"
#include "input_manager.h"
#include "vr/vr_system.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"

#include <cstring>

LOG_CHANNEL(VR);

// --- Name tables ---

static const char* s_button_names[] = {
  "A",             // BUTTON_A
  "B",             // BUTTON_B
  "X",             // BUTTON_X
  "Y",             // BUTTON_Y
  "Menu",          // BUTTON_MENU
  "LeftStickBtn",  // BUTTON_LEFT_STICK
  "RightStickBtn", // BUTTON_RIGHT_STICK
  "LeftTriggerBtn",  // BUTTON_LEFT_TRIGGER (derived)
  "RightTriggerBtn", // BUTTON_RIGHT_TRIGGER (derived)
  "LeftGripBtn",     // BUTTON_LEFT_GRIP (derived)
  "RightGripBtn",    // BUTTON_RIGHT_GRIP (derived)
};
static_assert(std::size(s_button_names) == OpenXRInputSource::NUM_BUTTONS);

static const char* s_axis_names[] = {
  "LeftStickX",    // AXIS_LEFT_STICK_X
  "LeftStickY",    // AXIS_LEFT_STICK_Y
  "RightStickX",   // AXIS_RIGHT_STICK_X
  "RightStickY",   // AXIS_RIGHT_STICK_Y
  "LeftTrigger",   // AXIS_LEFT_TRIGGER
  "RightTrigger",  // AXIS_RIGHT_TRIGGER
  "LeftGrip",      // AXIS_LEFT_GRIP
  "RightGrip",     // AXIS_RIGHT_GRIP
};
static_assert(std::size(s_axis_names) == OpenXRInputSource::NUM_AXES);

// --- Generic binding mappings ---

static const GenericInputBinding s_generic_binding_button_mapping[] = {
  GenericInputBinding::Cross,    // BUTTON_A
  GenericInputBinding::Circle,   // BUTTON_B
  GenericInputBinding::Square,   // BUTTON_X
  GenericInputBinding::Triangle, // BUTTON_Y
  GenericInputBinding::Start,    // BUTTON_MENU
  GenericInputBinding::L3,       // BUTTON_LEFT_STICK
  GenericInputBinding::R3,       // BUTTON_RIGHT_STICK
  GenericInputBinding::Unknown,  // BUTTON_LEFT_TRIGGER (axis handles L2)
  GenericInputBinding::Unknown,  // BUTTON_RIGHT_TRIGGER (axis handles R2)
  GenericInputBinding::Unknown,  // BUTTON_LEFT_GRIP (axis handles L1)
  GenericInputBinding::Unknown,  // BUTTON_RIGHT_GRIP (axis handles R1)
};
static_assert(std::size(s_generic_binding_button_mapping) == OpenXRInputSource::NUM_BUTTONS);

static const GenericInputBinding s_generic_binding_axis_mapping[][2] = {
  {GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight},   // AXIS_LEFT_STICK_X
  {GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown},      // AXIS_LEFT_STICK_Y
  {GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight}, // AXIS_RIGHT_STICK_X
  {GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown},    // AXIS_RIGHT_STICK_Y
  {GenericInputBinding::Unknown, GenericInputBinding::L2},                     // AXIS_LEFT_TRIGGER
  {GenericInputBinding::Unknown, GenericInputBinding::R2},                     // AXIS_RIGHT_TRIGGER
  {GenericInputBinding::Unknown, GenericInputBinding::L1},                     // AXIS_LEFT_GRIP
  {GenericInputBinding::Unknown, GenericInputBinding::R1},                     // AXIS_RIGHT_GRIP
};
static_assert(std::size(s_generic_binding_axis_mapping) == OpenXRInputSource::NUM_AXES);

// --- Helper: create XrAction ---

static XrAction CreateXrAction(XrActionSet action_set, XrActionType type, const char* name, const char* localized_name,
                                const XrPath* subaction_paths = nullptr, u32 subaction_count = 0)
{
  XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
  info.actionType = type;
  info.countSubactionPaths = subaction_count;
  info.subactionPaths = subaction_paths;

  // OpenXR requires name to be lowercase alphanumeric + underscores
  std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
  info.actionName[XR_MAX_ACTION_NAME_SIZE - 1] = '\0';
  std::strncpy(info.localizedActionName, localized_name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
  info.localizedActionName[XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1] = '\0';

  XrAction action = XR_NULL_HANDLE;
  XrResult result = xrCreateAction(action_set, &info, &action);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to create action '{}': {}", name, static_cast<int>(result));
    return XR_NULL_HANDLE;
  }
  return action;
}

// --- Raw VR state for navigation ---

VRRawInputState OpenXRInputSource::s_vr_raw_state = {};

// --- Implementation ---

OpenXRInputSource::OpenXRInputSource() = default;
OpenXRInputSource::~OpenXRInputSource() = default;

bool OpenXRInputSource::Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  // If VR system is available and has an instance, create actions now.
  // Otherwise stay dormant and try in PollEvents (lazy init).
  if (VR::g_vr_system && VR::g_vr_system->IsInitialized())
  {
    if (!CreateActionSet())
    {
      WARNING_LOG("OpenXR action set creation failed during init, will retry later.");
    }
  }
  else
  {
    INFO_LOG("VR system not yet available, OpenXR input source will initialize lazily.");
  }

  return true;
}

void OpenXRInputSource::UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

bool OpenXRInputSource::ReloadDevices()
{
  return false;
}

void OpenXRInputSource::Shutdown()
{
  if (m_connected)
    HandleDisconnected();

  DestroyActionSet();
}

bool OpenXRInputSource::CreateActionSet()
{
  if (!VR::g_vr_system || !VR::g_vr_system->IsInitialized())
    return false;

  // We need the XrInstance to create paths and actions. Get it via xrStringToPath
  // which requires the instance. Since openxr_loader provides global function pointers
  // and VR::System creates the instance, the functions should be available.
  if (!xrStringToPath || !xrCreateActionSet || !xrCreateAction)
  {
    ERROR_LOG("OpenXR function pointers not loaded.");
    return false;
  }

  // Get the XrInstance - we need it for xrStringToPath.
  // Access it through the openxr_loader's global function pointers which are
  // already bound to the instance created by VR::System.

  // Create hand subaction paths
  // Note: xrStringToPath uses the instance internally through the loaded function pointers.
  // We need to get the instance handle from VR::System. Since it's private, we'll use
  // xrStringToPath which has already been loaded as a global function pointer.
  // But xrStringToPath requires an XrInstance parameter...
  // We need access to the instance. Let's check if we can get it.

  // The VR::System stores m_instance as private. We need a way to access it.
  // For now, we'll store a reference or add an accessor.
  // Actually, looking at the code, the openxr_loader function pointers are global and
  // already bound to the instance. But xrStringToPath still needs the instance handle.
  // We need to add a public accessor to VR::System.

  // For this implementation, we'll need VR::System to expose its XrInstance.
  // Let me check if there's already a way...
  // There isn't one, so we need to add GetInstance() to VR::System.
  // This is handled by adding the accessor below.

  INFO_LOG("Creating OpenXR action set for controller input...");

  // We actually need the XrInstance handle. The function pointers are loaded but
  // xrStringToPath, xrCreateActionSet, etc. all take XrInstance as first parameter.
  // We need to expose it from VR::System.
  // For now, let's use the instance that's accessible. We'll add a getter.
  // NOTE: This will be compiled after we add GetInstance() to vr_system.h.

  XrInstance instance = VR::g_vr_system->GetInstance();
  if (instance == XR_NULL_HANDLE)
  {
    ERROR_LOG("XrInstance is null.");
    return false;
  }

  // Create hand paths
  XrResult result = xrStringToPath(instance, "/user/hand/left", &m_hand_paths[0]);
  if (!XR_SUCCEEDED(result))
    return false;
  result = xrStringToPath(instance, "/user/hand/right", &m_hand_paths[1]);
  if (!XR_SUCCEEDED(result))
    return false;

  // Create action set
  XrActionSetCreateInfo as_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(as_info.actionSetName, "duckstation_gamepad", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(as_info.localizedActionSetName, "DuckStation Gamepad", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  as_info.priority = 0;

  result = xrCreateActionSet(instance, &as_info, &m_action_set);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to create action set: {}", static_cast<int>(result));
    return false;
  }

  // --- Boolean button actions ---
  struct BooleanActionDef
  {
    const char* name;
    const char* localized;
  };
  static constexpr BooleanActionDef boolean_defs[NUM_BOOLEAN_ACTIONS] = {
    {"btn_a", "Button A"},
    {"btn_b", "Button B"},
    {"btn_x", "Button X"},
    {"btn_y", "Button Y"},
    {"btn_menu", "Menu Button"},
    {"btn_lstick", "Left Stick Click"},
    {"btn_rstick", "Right Stick Click"},
  };

  for (u32 i = 0; i < NUM_BOOLEAN_ACTIONS; i++)
  {
    m_button_actions[i] =
      CreateXrAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT, boolean_defs[i].name, boolean_defs[i].localized);
    if (m_button_actions[i] == XR_NULL_HANDLE)
    {
      DestroyActionSet();
      return false;
    }
  }

  // --- Float trigger actions ---
  m_trigger_actions[0] =
    CreateXrAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT, "trigger_left", "Left Trigger");
  m_trigger_actions[1] =
    CreateXrAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT, "trigger_right", "Right Trigger");

  // --- Float grip actions ---
  m_grip_actions[0] =
    CreateXrAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT, "grip_left", "Left Grip");
  m_grip_actions[1] =
    CreateXrAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT, "grip_right", "Right Grip");

  // --- Vector2f thumbstick actions ---
  m_thumbstick_actions[0] =
    CreateXrAction(m_action_set, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_left", "Left Thumbstick");
  m_thumbstick_actions[1] =
    CreateXrAction(m_action_set, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_right", "Right Thumbstick");

  // Verify all actions created
  for (u32 i = 0; i < 2; i++)
  {
    if (m_trigger_actions[i] == XR_NULL_HANDLE || m_grip_actions[i] == XR_NULL_HANDLE ||
        m_thumbstick_actions[i] == XR_NULL_HANDLE)
    {
      DestroyActionSet();
      return false;
    }
  }

  // Suggest interaction profile bindings
  if (!SuggestBindings())
  {
    DestroyActionSet();
    return false;
  }

  INFO_LOG("OpenXR action set created successfully.");
  return true;
}

void OpenXRInputSource::DestroyActionSet()
{
  // xrDestroyActionSet destroys all child actions too
  if (m_action_set != XR_NULL_HANDLE)
  {
    xrDestroyActionSet(m_action_set);
    m_action_set = XR_NULL_HANDLE;
  }

  std::memset(m_button_actions, 0, sizeof(m_button_actions));
  std::memset(m_trigger_actions, 0, sizeof(m_trigger_actions));
  std::memset(m_grip_actions, 0, sizeof(m_grip_actions));
  std::memset(m_thumbstick_actions, 0, sizeof(m_thumbstick_actions));
  m_hand_paths[0] = XR_NULL_PATH;
  m_hand_paths[1] = XR_NULL_PATH;
  m_attached = false;
}

bool OpenXRInputSource::SuggestBindings()
{
  XrInstance instance = VR::g_vr_system->GetInstance();

  // Build binding list for Oculus Touch controller
  // NOTE: The correct path is /interaction_profiles/oculus/touch_controller (no _profile suffix).
  XrPath profile_path;
  XrResult result =
    xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile_path);
  if (!XR_SUCCEEDED(result))
    return false;

  // Helper lambda to convert string to XrPath
  auto to_path = [&](const char* str) -> XrPath {
    XrPath p;
    xrStringToPath(instance, str, &p);
    return p;
  };

  XrActionSuggestedBinding bindings[] = {
    // Boolean buttons
    {m_button_actions[0], to_path("/user/hand/right/input/a/click")},
    {m_button_actions[1], to_path("/user/hand/right/input/b/click")},
    {m_button_actions[2], to_path("/user/hand/left/input/x/click")},
    {m_button_actions[3], to_path("/user/hand/left/input/y/click")},
    {m_button_actions[4], to_path("/user/hand/left/input/menu/click")},
    {m_button_actions[5], to_path("/user/hand/left/input/thumbstick/click")},
    {m_button_actions[6], to_path("/user/hand/right/input/thumbstick/click")},
    // Float triggers
    {m_trigger_actions[0], to_path("/user/hand/left/input/trigger/value")},
    {m_trigger_actions[1], to_path("/user/hand/right/input/trigger/value")},
    // Float grips
    {m_grip_actions[0], to_path("/user/hand/left/input/squeeze/value")},
    {m_grip_actions[1], to_path("/user/hand/right/input/squeeze/value")},
    // Vec2 thumbsticks
    {m_thumbstick_actions[0], to_path("/user/hand/left/input/thumbstick")},
    {m_thumbstick_actions[1], to_path("/user/hand/right/input/thumbstick")},
  };

  XrInteractionProfileSuggestedBinding suggestion = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  suggestion.interactionProfile = profile_path;
  suggestion.suggestedBindings = bindings;
  suggestion.countSuggestedBindings = static_cast<u32>(std::size(bindings));

  result = xrSuggestInteractionProfileBindings(instance, &suggestion);
  if (!XR_SUCCEEDED(result))
  {
    ERROR_LOG("Failed to suggest interaction profile bindings: {}", static_cast<int>(result));
    return false;
  }

  INFO_LOG("Suggested Oculus Touch controller bindings.");
  return true;
}

bool OpenXRInputSource::TryAttachActionSet()
{
  if (m_attached || m_action_set == XR_NULL_HANDLE)
    return false;

  if (!VR::g_vr_system || !VR::g_vr_system->IsSessionRunning())
    return false;

  XrSession session = VR::g_vr_system->GetSession();
  if (session == XR_NULL_HANDLE)
    return false;

  XrSessionActionSetsAttachInfo attach_info = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach_info.countActionSets = 1;
  attach_info.actionSets = &m_action_set;

  XrResult result = xrAttachSessionActionSets(session, &attach_info);
  if (!XR_SUCCEEDED(result))
  {
    // XR_ERROR_ACTIONSETS_ALREADY_ATTACHED (-47) means the action set was attached to
    // this session before it stopped and restarted. The runtime remembers the attachment,
    // so we can treat this as success and proceed with polling.
    if (result == -47) // XR_ERROR_ACTIONSETS_ALREADY_ATTACHED
    {
      INFO_LOG("OpenXR action set already attached (session restart), continuing.");
      m_attached = true;
      return true;
    }
    ERROR_LOG("Failed to attach action sets: {}", static_cast<int>(result));
    return false;
  }

  m_attached = true;
  INFO_LOG("OpenXR action set attached to session.");
  return true;
}

void OpenXRInputSource::HandleConnected()
{
  if (m_connected)
    return;

  m_connected = true;
  std::memset(m_prev_button_states, 0, sizeof(m_prev_button_states));
  std::memset(m_prev_axis_states, 0, sizeof(m_prev_axis_states));

  InputManager::OnInputDeviceConnected(MakeGenericControllerDeviceKey(InputSourceType::OpenXR, 0), "OpenXR-0", "OpenXR VR Controller");
}

void OpenXRInputSource::HandleDisconnected()
{
  if (!m_connected)
    return;

  m_connected = false;
  m_attached = false;
  s_vr_raw_state = {};
  InputManager::OnInputDeviceDisconnected(MakeGenericControllerDeviceKey(InputSourceType::OpenXR, 0), "OpenXR-0");
}

void OpenXRInputSource::PollEvents()
{
  // Lazy initialization: if action set not created yet and VR now available, create it
  if (m_action_set == XR_NULL_HANDLE)
  {
    if (m_init_failed)
      return; // Don't retry after permanent failure

    if (VR::g_vr_system && VR::g_vr_system->IsInitialized())
    {
      if (!CreateActionSet())
      {
        m_init_failed = true;
        ERROR_LOG("OpenXR input source initialization failed permanently.");
        return;
      }
    }
    else
    {
      return;
    }
  }

  // If session not running, mark disconnected and reset attach state
  if (!VR::g_vr_system || !VR::g_vr_system->IsSessionRunning())
  {
    if (m_connected)
      HandleDisconnected();
    return;
  }

  // Try to attach if not yet attached
  if (!m_attached)
  {
    if (!TryAttachActionSet())
      return;
  }

  XrSession session = VR::g_vr_system->GetSession();

  // Sync actions
  XrActiveActionSet active_set = {};
  active_set.actionSet = m_action_set;
  active_set.subactionPath = XR_NULL_PATH;

  XrActionsSyncInfo sync_info = {XR_TYPE_ACTIONS_SYNC_INFO};
  sync_info.countActiveActionSets = 1;
  sync_info.activeActionSets = &active_set;

  XrResult result = xrSyncActions(session, &sync_info);
  if (!XR_SUCCEEDED(result))
  {
    // XR_SESSION_NOT_FOCUSED is normal when another app/overlay has focus.
    // Input is only available in FOCUSED state (session state 6).
    if (result == XR_SESSION_NOT_FOCUSED)
    {
      if (!m_logged_not_focused)
      {
        INFO_LOG("OpenXR session not focused - controller input unavailable until session gains focus.");
        m_logged_not_focused = true;
      }
      return;
    }
    WARNING_LOG("xrSyncActions failed: {}", static_cast<int>(result));
    return;
  }

  // Mark connected on first successful sync (session is now FOCUSED)
  if (!m_connected)
  {
    INFO_LOG("OpenXR session now focused - controller input active.");
    m_logged_not_focused = false;
    HandleConnected();
  }

  // --- Query boolean button states ---
  for (u32 i = 0; i < NUM_BOOLEAN_ACTIONS; i++)
  {
    XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
    XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = m_button_actions[i];

    result = xrGetActionStateBoolean(session, &get_info, &state);
    if (!XR_SUCCEEDED(result))
      continue;

    const bool pressed = state.isActive && state.currentState;

    // Cache raw stick click state for VR navigation mode cycling
    if (i == BUTTON_LEFT_STICK)
      s_vr_raw_state.left_stick_clicked = pressed;
    else if (i == BUTTON_RIGHT_STICK)
      s_vr_raw_state.right_stick_clicked = pressed;

    if (pressed != m_prev_button_states[i])
    {
      m_prev_button_states[i] = pressed;
      const float value = pressed ? 1.0f : 0.0f;
      const GenericInputBinding generic = s_generic_binding_button_mapping[i];
      InputManager::InvokeEvents(
        MakeGenericControllerButtonKey(InputSourceType::OpenXR, 0, static_cast<s32>(i)), value, generic);
    }
  }

  // --- Query float trigger values ---
  for (u32 side = 0; side < 2; side++)
  {
    XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
    XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = m_trigger_actions[side];

    result = xrGetActionStateFloat(session, &get_info, &state);
    if (!XR_SUCCEEDED(result))
      continue;

    const float value = state.isActive ? state.currentState : 0.0f;
    const u32 axis_index = AXIS_LEFT_TRIGGER + side;

    // Fire axis event
    if (value != m_prev_axis_states[axis_index])
    {
      m_prev_axis_states[axis_index] = value;
      InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::OpenXR, 0, static_cast<s32>(axis_index)),
                                 value, GenericInputBinding::Unknown);
    }

    // Fire derived button event (threshold crossing)
    const u32 btn_index = BUTTON_LEFT_TRIGGER + side;
    const bool btn_pressed = value >= TRIGGER_BUTTON_THRESHOLD;
    if (btn_pressed != m_prev_button_states[btn_index])
    {
      m_prev_button_states[btn_index] = btn_pressed;
      InputManager::InvokeEvents(
        MakeGenericControllerButtonKey(InputSourceType::OpenXR, 0, static_cast<s32>(btn_index)),
        btn_pressed ? 1.0f : 0.0f, GenericInputBinding::Unknown);
    }
  }

  // --- Query float grip values ---
  for (u32 side = 0; side < 2; side++)
  {
    XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
    XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = m_grip_actions[side];

    result = xrGetActionStateFloat(session, &get_info, &state);
    if (!XR_SUCCEEDED(result))
      continue;

    const float value = state.isActive ? state.currentState : 0.0f;
    const u32 axis_index = AXIS_LEFT_GRIP + side;

    // Fire axis event
    if (value != m_prev_axis_states[axis_index])
    {
      m_prev_axis_states[axis_index] = value;
      InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::OpenXR, 0, static_cast<s32>(axis_index)),
                                 value, GenericInputBinding::Unknown);
    }

    // Fire derived button event
    const u32 btn_index = BUTTON_LEFT_GRIP + side;
    const bool btn_pressed = value >= TRIGGER_BUTTON_THRESHOLD;
    if (btn_pressed != m_prev_button_states[btn_index])
    {
      m_prev_button_states[btn_index] = btn_pressed;
      InputManager::InvokeEvents(
        MakeGenericControllerButtonKey(InputSourceType::OpenXR, 0, static_cast<s32>(btn_index)),
        btn_pressed ? 1.0f : 0.0f, GenericInputBinding::Unknown);
    }
  }

  // --- Query vec2 thumbstick values ---
  for (u32 side = 0; side < 2; side++)
  {
    XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
    XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = m_thumbstick_actions[side];

    result = xrGetActionStateVector2f(session, &get_info, &state);
    if (!XR_SUCCEEDED(result))
      continue;

    const float x = state.isActive ? state.currentState.x : 0.0f;
    const float y = state.isActive ? state.currentState.y : 0.0f;

    // Cache raw right stick X for VR navigation
    if (side == 1)
      s_vr_raw_state.right_stick_x = x;

    const u32 x_axis = (side == 0) ? AXIS_LEFT_STICK_X : AXIS_RIGHT_STICK_X;
    const u32 y_axis = (side == 0) ? AXIS_LEFT_STICK_Y : AXIS_RIGHT_STICK_Y;

    if (x != m_prev_axis_states[x_axis])
    {
      m_prev_axis_states[x_axis] = x;
      InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::OpenXR, 0, static_cast<s32>(x_axis)), x,
                                 GenericInputBinding::Unknown);
    }

    // OpenXR: Y+ is up. DuckStation convention for sticks: negative = up, positive = down.
    // We negate Y to match the convention used by XInput/SDL in DuckStation.
    const float y_negated = -y;
    if (y_negated != m_prev_axis_states[y_axis])
    {
      m_prev_axis_states[y_axis] = y_negated;
      InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::OpenXR, 0, static_cast<s32>(y_axis)),
                                 y_negated, GenericInputBinding::Unknown);
    }
  }
}

InputManager::DeviceList OpenXRInputSource::EnumerateDevices()
{
  InputManager::DeviceList ret;
  if (m_connected)
  {
    const InputBindingKey key = MakeGenericControllerDeviceKey(InputSourceType::OpenXR, 0);
    ret.emplace_back(key, "OpenXR-0", "OpenXR VR Controller");
  }
  return ret;
}

InputManager::DeviceEffectList OpenXRInputSource::EnumerateEffects(std::optional<InputBindingInfo::Type> type,
                                                                  std::optional<InputBindingKey> for_device)
{
  // Quest Touch controllers have haptics, but we don't expose them yet.
  return {};
}

bool OpenXRInputSource::GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping)
{
  if (!device.starts_with("OpenXR-"))
    return false;

  const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(7));
  if (!player_id.has_value() || player_id.value() != 0)
    return false;

  // Axis mappings
  for (u32 i = 0; i < std::size(s_generic_binding_axis_mapping); i++)
  {
    const GenericInputBinding negative = s_generic_binding_axis_mapping[i][0];
    const GenericInputBinding positive = s_generic_binding_axis_mapping[i][1];

    if (negative != GenericInputBinding::Unknown)
      mapping->emplace_back(negative, fmt::format("OpenXR-0/-{}", s_axis_names[i]));
    if (positive != GenericInputBinding::Unknown)
      mapping->emplace_back(positive, fmt::format("OpenXR-0/+{}", s_axis_names[i]));
  }

  // Button mappings
  for (u32 i = 0; i < std::size(s_generic_binding_button_mapping); i++)
  {
    const GenericInputBinding binding = s_generic_binding_button_mapping[i];
    if (binding != GenericInputBinding::Unknown)
      mapping->emplace_back(binding, fmt::format("OpenXR-0/{}", s_button_names[i]));
  }

  return true;
}

void OpenXRInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
  // Haptics not implemented yet
}

std::optional<InputBindingKey> OpenXRInputSource::ParseKeyString(std::string_view device,
                                                                  std::string_view binding)
{
  if (!device.starts_with("OpenXR-") || binding.empty())
    return std::nullopt;

  const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(7));
  if (!player_id.has_value() || player_id.value() < 0)
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::OpenXR;
  key.source_index = static_cast<u32>(player_id.value());

  if (binding[0] == '+' || binding[0] == '-')
  {
    // Axis
    const std::string_view axis_name(binding.substr(1));
    for (u32 i = 0; i < std::size(s_axis_names); i++)
    {
      if (axis_name == s_axis_names[i])
      {
        key.source_subtype = InputSubclass::ControllerAxis;
        key.data = i;
        key.modifier = (binding[0] == '-') ? InputModifier::Negate : InputModifier::None;
        return key;
      }
    }
  }
  else
  {
    // Button
    for (u32 i = 0; i < std::size(s_button_names); i++)
    {
      if (binding == s_button_names[i])
      {
        key.source_subtype = InputSubclass::ControllerButton;
        key.data = i;
        return key;
      }
    }
  }

  return std::nullopt;
}

TinyString OpenXRInputSource::ConvertKeyToString(InputBindingKey key)
{
  TinyString ret;

  if (key.source_type == InputSourceType::OpenXR)
  {
    if (key.source_subtype == InputSubclass::ControllerAxis && key.data < std::size(s_axis_names))
    {
      const char modifier = (key.modifier == InputModifier::Negate) ? '-' : '+';
      ret.format("OpenXR-{}/{}{}", static_cast<u32>(key.source_index), modifier, s_axis_names[key.data]);
    }
    else if (key.source_subtype == InputSubclass::ControllerButton && key.data < std::size(s_button_names))
    {
      ret.format("OpenXR-{}/{}", static_cast<u32>(key.source_index), s_button_names[key.data]);
    }
  }

  return ret;
}

TinyString OpenXRInputSource::ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper)
{
  // No icon font mappings for Quest controllers yet
  return {};
}

std::optional<float> OpenXRInputSource::GetCurrentValue(InputBindingKey key)
{
  std::optional<float> ret;
  if (key.source_type != InputSourceType::OpenXR)
    return ret;

  if (key.source_subtype == InputSubclass::ControllerAxis && key.data < NUM_AXES)
    ret = m_prev_axis_states[key.data];
  else if (key.source_subtype == InputSubclass::ControllerButton && key.data < NUM_BUTTONS)
    ret = m_prev_button_states[key.data] ? 1.0f : 0.0f;

  return ret;
}

bool OpenXRInputSource::ContainsDevice(std::string_view device) const
{
  return device.starts_with("OpenXR-");
}

u32 OpenXRInputSource::GetPollableDeviceCount() const
{
  return m_connected ? 1 : 0;
}

void OpenXRInputSource::UpdateLEDState(InputBindingKey key, float intensity)
{
  // No LEDs on Quest controllers
}

void OpenXRInputSource::SetSubclassPollDeviceList(InputSubclass subclass, const std::span<const InputBindingKey>* devices)
{
  // No optional subclasses
}

std::unique_ptr<ForceFeedbackDevice> OpenXRInputSource::CreateForceFeedbackDevice(std::string_view device, Error* error)
{
  // No force feedback support yet
  return {};
}

std::unique_ptr<InputSource> InputSource::CreateOpenXRSource()
{
  return std::make_unique<OpenXRInputSource>();
}
