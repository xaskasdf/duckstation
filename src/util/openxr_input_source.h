// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "input_source.h"
#include "vr/openxr_loader.h"

#include <array>
#include <span>

struct VRRawInputState
{
  float right_stick_x = 0.0f;
  bool left_stick_clicked = false;
  bool right_stick_clicked = false;
};

class OpenXRInputSource final : public InputSource
{
public:
  static const VRRawInputState& GetRawVRState() { return s_vr_raw_state; }
  enum : u32
  {
    NUM_CONTROLLERS = 1, // single virtual gamepad combining both hands
  };

  // Button indices (boolean actions + derived from analog thresholds)
  enum : u32
  {
    BUTTON_A,
    BUTTON_B,
    BUTTON_X,
    BUTTON_Y,
    BUTTON_MENU,
    BUTTON_LEFT_STICK,
    BUTTON_RIGHT_STICK,
    BUTTON_LEFT_TRIGGER,  // derived: trigger >= 0.5
    BUTTON_RIGHT_TRIGGER, // derived: trigger >= 0.5
    BUTTON_LEFT_GRIP,     // derived: grip >= 0.5
    BUTTON_RIGHT_GRIP,    // derived: grip >= 0.5
    NUM_BUTTONS,
  };

  // Axis indices
  enum : u32
  {
    AXIS_LEFT_STICK_X,
    AXIS_LEFT_STICK_Y,
    AXIS_RIGHT_STICK_X,
    AXIS_RIGHT_STICK_Y,
    AXIS_LEFT_TRIGGER,
    AXIS_RIGHT_TRIGGER,
    AXIS_LEFT_GRIP,
    AXIS_RIGHT_GRIP,
    NUM_AXES,
  };

  OpenXRInputSource();
  ~OpenXRInputSource();

  bool Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::optional<float> GetCurrentValue(InputBindingKey key) override;
  bool ContainsDevice(std::string_view device) const override;
  InputManager::DeviceList EnumerateDevices() override;
  InputManager::DeviceEffectList EnumerateEffects(std::optional<InputBindingInfo::Type> type,
                                                  std::optional<InputBindingKey> for_device) override;
  u32 GetPollableDeviceCount() const override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateLEDState(InputBindingKey key, float intensity) override;
  void SetSubclassPollDeviceList(InputSubclass subclass, const std::span<const InputBindingKey>* devices) override;
  std::unique_ptr<ForceFeedbackDevice> CreateForceFeedbackDevice(std::string_view device, Error* error) override;

  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper) override;

private:
  static constexpr float TRIGGER_BUTTON_THRESHOLD = 0.5f;

  bool CreateActionSet();
  void DestroyActionSet();
  bool SuggestBindings();
  bool TryAttachActionSet();
  void HandleConnected();
  void HandleDisconnected();

  // OpenXR action handles
  XrActionSet m_action_set = XR_NULL_HANDLE;

  // 7 boolean click actions: A, B, X, Y, Menu, LeftStickBtn, RightStickBtn
  static constexpr u32 NUM_BOOLEAN_ACTIONS = 7;
  XrAction m_button_actions[NUM_BOOLEAN_ACTIONS] = {};

  // 2 float actions: left trigger, right trigger
  XrAction m_trigger_actions[2] = {};

  // 2 float actions: left grip, right grip
  XrAction m_grip_actions[2] = {};

  // 2 vector2f actions: left thumbstick, right thumbstick
  XrAction m_thumbstick_actions[2] = {};

  // Hand subaction paths
  XrPath m_hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};

  // Previous-frame state for change detection
  bool m_prev_button_states[NUM_BUTTONS] = {};
  float m_prev_axis_states[NUM_AXES] = {};

  // State tracking
  bool m_connected = false;
  bool m_attached = false;
  bool m_init_failed = false;
  bool m_logged_not_focused = false;

  static VRRawInputState s_vr_raw_state;
};
