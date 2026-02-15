// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0

#include "vr_camera_injector.h"
#include <cmath>
#include <cstring>

namespace VR {

CameraInjector g_camera_injector;

void CameraInjector::ApplyToVertex(s64& x, s64& y, s64& z) const
{
  if (!enabled)
    return;

  const s64 ox = x, oy = y, oz = z;
  x = (delta_rotation[0][0] * ox + delta_rotation[0][1] * oy + delta_rotation[0][2] * oz) >> 12;
  y = (delta_rotation[1][0] * ox + delta_rotation[1][1] * oy + delta_rotation[1][2] * oz) >> 12;
  z = (delta_rotation[2][0] * ox + delta_rotation[2][1] * oy + delta_rotation[2][2] * oz) >> 12;
}

void CameraInjector::SetVROrientation(float qx, float qy, float qz, float qw)
{
  // OpenXR pose is view-to-world; NCLIP needs world-to-view, so conjugate.
  // Note: this only affects screen coord projection for culling.
  // Screenshot3D captures pre-injection positions, so no double-rotation.
  qx = -qx; qy = -qy; qz = -qz;

  // Convert quaternion to 3x3 rotation matrix (row-major)
  float r[9];
  r[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
  r[1] = 2.0f * (qx * qy - qz * qw);
  r[2] = 2.0f * (qx * qz + qy * qw);
  r[3] = 2.0f * (qx * qy + qz * qw);
  r[4] = 1.0f - 2.0f * (qx * qx + qz * qz);
  r[5] = 2.0f * (qy * qz - qx * qw);
  r[6] = 2.0f * (qx * qz - qy * qw);
  r[7] = 2.0f * (qy * qz + qx * qw);
  r[8] = 1.0f - 2.0f * (qx * qx + qy * qy);

  // Convert from OpenXR (Y-up, right-handed, -Z forward)
  // to PS1 GTE (Y-down, left-handed, +Z forward)
  // Transformation: flip Y axis, negate Z axis
  // R_ps1 = F * R_xr * F where F = diag(1, -1, -1)
  // R_ps1[i][j] = sign[i] * sign[j] * R_xr[i][j]
  const float sign[3] = {1.0f, -1.0f, -1.0f};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      m_vr_rotation[i * 3 + j] = sign[i] * sign[j] * r[i * 3 + j];

  m_vr_valid = true;
}

void CameraInjector::SetGameCameraRotation(const float* rt)
{
  std::memcpy(m_game_rotation, rt, sizeof(float) * 9);
  m_game_valid = true;
}

void CameraInjector::ComputeDelta()
{
  if (!m_vr_valid || !m_game_valid)
  {
    Reset();
    return;
  }

  // delta = R_vr * R_game_transpose (transpose = inverse for rotation matrices)
  // R_game is row-major [row*3+col], so R_game_transpose[i][j] = R_game[j*3+i]
  float delta[9];
  for (int i = 0; i < 3; i++)
  {
    for (int j = 0; j < 3; j++)
    {
      delta[i * 3 + j] = 0.0f;
      for (int k = 0; k < 3; k++)
        delta[i * 3 + j] += m_vr_rotation[i * 3 + k] * m_game_rotation[j * 3 + k]; // note: j*3+k = transpose
    }
  }

  // Convert to s64 scaled by 4096 for GTE fixed-point math
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      delta_rotation[i][j] = static_cast<s64>(std::roundf(delta[i * 3 + j] * 4096.0f));
}

void CameraInjector::Reset()
{
  // Identity matrix scaled by 4096
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      delta_rotation[i][j] = (i == j) ? 4096 : 0;

  m_vr_valid = false;
  m_game_valid = false;
}

} // namespace VR
