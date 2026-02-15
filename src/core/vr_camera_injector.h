// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once
#include "types.h"

namespace VR {

/// Camera injection for VR - rotates GTE output so the game's culling
/// works with the VR headset direction instead of the original game camera.
///
/// Applied post-transform in GTE::RTPS, same hook point as Freecam.
/// The delta rotation = VR_rotation * game_camera_rotation_inverse
/// makes objects visible from the VR viewpoint pass NCLIP.
struct CameraInjector
{
  bool enabled = false;

  /// Delta rotation matrix (s64, scaled by 4096).
  /// Applied to post-transform camera-space coordinates.
  /// Identity when VR direction matches game camera.
  s64 delta_rotation[3][3];

  /// Apply the delta rotation to a post-transform vertex.
  /// Called from GTE::RTPS after RT*V+TR but before perspective projection.
  void ApplyToVertex(s64& x, s64& y, s64& z) const;

  /// Set the VR headset orientation from OpenXR quaternion.
  /// Converts from OpenXR coordinates (Y-up, right-handed) to
  /// PS1 GTE coordinates (Y-down, left-handed).
  void SetVROrientation(float qx, float qy, float qz, float qw);

  /// Set the game camera rotation matrix (from Screenshot3D::GetCameraTransform).
  /// rt is row-major float[9], already divided by 4096.
  void SetGameCameraRotation(const float* rt);

  /// Compute the delta rotation from the stored VR and game camera rotations.
  /// Must be called after SetVROrientation and SetGameCameraRotation.
  /// delta = R_vr * R_game_transpose
  void ComputeDelta();

  /// Reset to identity (no correction).
  void Reset();

private:
  float m_vr_rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float m_game_rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool m_vr_valid = false;
  bool m_game_valid = false;
};

extern CameraInjector g_camera_injector;

} // namespace VR
