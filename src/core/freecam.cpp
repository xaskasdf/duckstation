#include "freecam.h"
#include "settings.h"

#include "imgui.h"
#include "util/imgui_manager.h"

#include <cmath>

namespace Freecam {
namespace {

struct Config
{
  bool enabled = true;
  s32 angle_x = 0;
  s32 angle_y = 0;
  s32 angle_z = 0;
  s32 move_x = 0;
  s32 move_y = 0;
  s32 move_z = 0;
};

} // namespace

static Config s_config;

// Cached sine/cosine values
// Precomputed once at frame start
s64 s_sin_angle_x, s_cos_angle_x;
s64 s_sin_angle_y, s_cos_angle_y;
s64 s_sin_angle_z, s_cos_angle_z;

static void PrecomputeSineAndCosine(s32 angle, s64& sine, s64& cosine)
{
  constexpr float PI = 3.14159265f;
  const float radians = angle * (2.f * PI / 360.f);
  sine = roundf(sinf(radians) * 4096.f);
  cosine = roundf(cosf(radians) * 4096.f);
}

static void Rotate2D(s64 sine, s64 cosine, s64& x, s64& y)
{
  const s64 x_ = x, y_ = y;
  x = (cosine * x_ + sine * y_) >> 12;
  y = (-sine * x_ + cosine * y_) >> 12;
}

// x,y,z are in high precision, before the 12 fractional bits are
// shifted off.
void ApplyToVertex(s64& x, s64& y, s64& z)
{
  if (!s_config.enabled)
    return;

  x += (s64)s_config.move_x << 12;
  y -= (s64)s_config.move_y << 12;
  z -= (s64)s_config.move_z << 12;

  Rotate2D(s_sin_angle_x, s_cos_angle_x, y, z);
  Rotate2D(s_sin_angle_y, s_cos_angle_y, x, z);
  Rotate2D(s_sin_angle_z, s_cos_angle_z, x, y);
}

// Config is double-buffered so changes don't "tear" the frame.
// This is the back-buffer the UI modifies.
static Config s_ui_config;

void NextFrame()
{
  s_config = s_ui_config;

  PrecomputeSineAndCosine(s_config.angle_x, s_sin_angle_x, s_cos_angle_x);
  PrecomputeSineAndCosine(s_config.angle_y, s_sin_angle_y, s_cos_angle_y);
  PrecomputeSineAndCosine(s_config.angle_z, s_sin_angle_z, s_cos_angle_z);

  // Freecam is always available when called (controlled by caller)
}

void DrawGuiWindow()
{
  const float framebuffer_scale = ImGuiManager::OSDScale(1.0f);

  ImGui::SetNextWindowSize(
    ImVec2(500.0f * framebuffer_scale, 233.0f * framebuffer_scale),
    ImGuiCond_Once
  );
  ImGui::SetNextWindowPos(
    ImVec2(480.0f * framebuffer_scale, 60.0f * framebuffer_scale),
    ImGuiCond_Once
  );

  if (!ImGui::Begin("Freecam", nullptr))
  {
    ImGui::End();
    return;
  }

  ImGui::Checkbox("Enabled", &s_ui_config.enabled);

  if (!s_ui_config.enabled)
    ImGui::BeginDisabled();

  ImGui::SliderInt("Rotation X", &s_ui_config.angle_x, -180, 180);
  ImGui::SliderInt("Rotation Y", &s_ui_config.angle_y, -180, 180);
  ImGui::SliderInt("Rotation Z", &s_ui_config.angle_z, -180, 180);

  ImGui::SliderInt("Move X", &s_ui_config.move_x, -4000, 4000);
  ImGui::SliderInt("Move Y", &s_ui_config.move_y, -4000, 4000);
  ImGui::SliderInt("Move Z", &s_ui_config.move_z, -4000, 4000);

  if (!s_ui_config.enabled)
    ImGui::EndDisabled();

  if (ImGui::Button("Reset Rotation"))
    s_ui_config.angle_x = s_ui_config.angle_y = s_ui_config.angle_z = 0;
  ImGui::SameLine();
  if (ImGui::Button("Reset Move"))
    s_ui_config.move_x = s_ui_config.move_y = s_ui_config.move_z = 0;

  ImGui::End();
}

} // namespace Freecam
