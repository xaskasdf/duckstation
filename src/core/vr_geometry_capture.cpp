// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0
//
// VR geometry capture — intercepts GTE vertex output and GPU draw commands
// to build per-frame polygon buffers for VR stereo rendering.

#include "screenshot_3d_internal.h"
#include "cpu_core.h"
#include "gte_types.h"
#include "gpu.h"
#include "host.h"
#include "system.h"

#include "common/hash_combine.h"
#include "common/log.h"

#include "util/image.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

LOG_CHANNEL(VR);

namespace Screenshot3D {

//////////////////////////////////////////////////////////////////////////////
// Internal types
//////////////////////////////////////////////////////////////////////////////

namespace {

enum
{
  TEXTURE_MODE_FULL = 0,
  TEXTURE_MODE_CROPPED = 1,
  TEXTURE_MODE_PACKED = 2
};

struct Config
{
  int num_frames_to_capture = 1;
  int texture_mode = TEXTURE_MODE_FULL;
  bool disable_culling = true;
  bool shoot_front_and_back = false;
  bool in_2d_mode = false;
  bool use_pgxp = false;
  bool is_dry_run = false;
  bool vr_continuous_mode = false;
};

// Internal vertex cache entry — maps screen coords to 3D positions
struct Vertex
{
  s16 Sx, Sy;
  float x, y, z;
  u32 generation;
};

} // namespace

//////////////////////////////////////////////////////////////////////////////
// UVBlob methods (public struct in header)
//////////////////////////////////////////////////////////////////////////////

void UVBlob::Init(const Poly& poly)
{
  min_u = max_u = poly.v[0].u;
  min_v = max_v = poly.v[0].v;
  UpdateBoundingBox(poly);
}

void UVBlob::UpdateBoundingBox(const Poly& poly)
{
  for (int i = 0; i < poly.NumVerts(); i++)
  {
    min_u = std::min(min_u, poly.v[i].u);
    max_u = std::max(max_u, poly.v[i].u);
    min_v = std::min(min_v, poly.v[i].v);
    max_v = std::max(max_v, poly.v[i].v);
  }
}

bool UVBlob::Collides(const UVBlob& other) const
{
  return !(
    (other.min_u > max_u || other.max_u < min_u) ||
    (other.min_v > max_v || other.max_v < min_v)
  );
}

//////////////////////////////////////////////////////////////////////////////
// Constants and state
//////////////////////////////////////////////////////////////////////////////

static constexpr u32 WARMUP_PERIOD = 2;
static constexpr u32 VERTEX_RECYCLE_PERIOD = 2;
static constexpr u32 VR_VERTEX_RECYCLE_PERIOD = 4;

static Config s_config;

static bool s_running = false;
static int s_shots_taken = 0;
static u32 s_frame_counter = 0;

// Per-frame camera RT/TR capture
static float s_frame_camera_tr[3] = {0, 0, 0};
static float s_frame_camera_rt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static bool s_frame_camera_valid = false;
static bool s_need_capture_camera = false;

using ScreenXYKey = u32;
using TextureKey = u64;
using TextureIndex = u32;

static std::vector<Poly> s_poly_buffer;
static std::vector<Texture> s_textures;
static std::unordered_map<ScreenXYKey, Vertex> s_vertex_cache;
static std::unordered_map<TextureKey, TextureIndex> s_texture_cache;

// VR double-buffering
static std::vector<Poly> s_vr_poly_buffer;
static std::vector<Texture> s_vr_textures;
static bool s_vr_continuous_mode = false;

// Front/back capture (original screenshot feature, kept for compatibility)
struct FrontShot
{
  std::vector<Poly> poly_buffer;
  std::vector<Texture> textures;
};
static std::vector<FrontShot> s_front_shots;
static bool s_in_front_phase = false;
// Note: MemorySaveState removed — front/back capture disabled in VR-only build

// UI state
static Config s_ui_config;
static bool s_run_requested = false;

// Texture extraction functions (defined in vr_texture_capture.cpp)
extern void FillTextureFromVRAM(Texture& texture, const u16* vram_ptr, u32 dst_x, u32 dst_y);
extern void PackTexturesIntoImages(const std::vector<Texture*>& textures, const u16* vram_ptr);

//////////////////////////////////////////////////////////////////////////////
// Lifecycle
//////////////////////////////////////////////////////////////////////////////

static void TurnOff()
{
  s_running = false;
  s_shots_taken = 0;
  s_frame_counter = 0;

  s_vertex_cache.clear();
  s_poly_buffer.clear();
  s_textures.clear();
  s_texture_cache.clear();

  s_vr_poly_buffer.clear();
  s_vr_textures.clear();

  s_frame_camera_valid = false;
  s_need_capture_camera = false;

  s_front_shots.clear();
  s_in_front_phase = false;
}

static void TurnOn()
{
  TurnOff();
  s_running = true;
  s_need_capture_camera = true;

  // Note: shoot_front_and_back disabled in VR-only build (no MemorySaveState)
  s_config.shoot_front_and_back = false;
}

void Shutdown()
{
  TurnOff();
}

//////////////////////////////////////////////////////////////////////////////
// VR Geometry Access API
//////////////////////////////////////////////////////////////////////////////

bool IsGeometryReady()
{
  return s_running && s_frame_counter >= WARMUP_PERIOD && !s_vr_poly_buffer.empty();
}

const std::vector<Poly>& GetPolygonBuffer()
{
  if (s_vr_continuous_mode)
    return s_vr_poly_buffer;
  return s_poly_buffer;
}

const std::vector<Texture>& GetTextureBuffer()
{
  if (s_vr_continuous_mode)
    return s_vr_textures;
  return s_textures;
}

u32 GetFrameCounter()
{
  return s_frame_counter;
}

bool IsRunning()
{
  return s_running;
}

void SetVRContinuousMode(bool enabled)
{
  if (enabled == s_vr_continuous_mode)
    return;

  s_vr_continuous_mode = enabled;

  if (enabled)
  {
    if (!s_running)
    {
      s_config.vr_continuous_mode = true;
      s_config.disable_culling = true;
      s_config.in_2d_mode = false;
      s_config.is_dry_run = true;
      s_config.num_frames_to_capture = 999999;
      TurnOn();
    }
  }
  else
  {
    if (s_running && s_config.vr_continuous_mode)
      TurnOff();
    s_config.vr_continuous_mode = false;
  }
}

bool IsVRContinuousModeEnabled()
{
  return s_vr_continuous_mode;
}

bool GetCameraTransform(float* out_tr, float* out_rt)
{
  if (!s_running || !s_frame_camera_valid)
    return false;

  std::memcpy(out_tr, s_frame_camera_tr, sizeof(float) * 3);
  std::memcpy(out_rt, s_frame_camera_rt, sizeof(float) * 9);
  return true;
}

void GetCacheStats(u32& cache_size, u32& attempts, u32& failures);

//////////////////////////////////////////////////////////////////////////////
// Internal helpers
//////////////////////////////////////////////////////////////////////////////

static bool IsWarmedUp()
{
  return s_running && s_frame_counter >= WARMUP_PERIOD;
}

static bool IsDoneShooting()
{
  return s_shots_taken >= s_config.num_frames_to_capture;
}

static void EnterBackPhase()
{
  s_shots_taken = 0;
  s_frame_counter = 0;
  s_vertex_cache.clear();
  s_poly_buffer.clear();
  s_textures.clear();
  s_texture_cache.clear();

  s_in_front_phase = false;
  // Note: LoadMemoryState removed — front/back capture disabled in VR-only build
}

bool BlockingInput()
{
  return s_running && s_config.shoot_front_and_back;
}

//////////////////////////////////////////////////////////////////////////////
// GTE hooks
//////////////////////////////////////////////////////////////////////////////

bool WantsModifyNCLIP()
{
  return s_running && (s_config.disable_culling || s_config.shoot_front_and_back);
}

void ModifyNCLIP(s64& value)
{
  if (value == 0) value = 1;

  if (s_config.disable_culling)
    if (value < 0) value = -value;

  if (s_config.shoot_front_and_back && !s_in_front_phase)
    value = -value;
}

bool ShouldUsePGXP()
{
  return s_running && g_settings.gpu_pgxp_enable && s_config.use_pgxp && !s_config.in_2d_mode;
}

//////////////////////////////////////////////////////////////////////////////
// Vertex / RTPS tracking
//////////////////////////////////////////////////////////////////////////////

static u32 PackSXYIntoU32(s32 Sx, s32 Sy)
{
  return u32(u16(Sx)) | (u32(u16(Sy)) << 16);
}

static Vertex& FindFreeScreenCoord(s32& Sx, s32& Sy)
{
  const u32 key = PackSXYIntoU32(Sx, Sy);
  const auto [it, success] = s_vertex_cache.try_emplace(key);
  if (success)
    return it->second;

  std::size_t seed = 0xb0ba7ea;
  hash_combine(seed, s_frame_counter, s_vertex_cache.size());
  u32 rng = seed;

  for (u32 attempts = 0; true; ++attempts)
  {
    rng *= 0x343fdU;
    rng += 0x269ec3U;

    const bool axis = (rng >> 20) & 1;
    const bool sign = (rng >> 21) & 1;
    const s32 dist = 1 + ((rng >> 22) & (attempts < 5 ? 3 : 7));
    (axis ? Sx : Sy) += sign ? dist : -dist;

    Sx = Sx < -1024 ? -1024 : Sx > 1023 ? 1023 : Sx;
    Sy = Sy < -1024 ? -1024 : Sy > 1023 ? 1023 : Sy;

    const u32 key2 = PackSXYIntoU32(Sx, Sy);
    const auto [it2, success2] = s_vertex_cache.try_emplace(key2);
    if (success2)
      return it2->second;
  }
}

void PushVertex(float x, float y, float z, s32& Sx, s32& Sy)
{
  if (!s_running || s_config.in_2d_mode)
    return;

  // Capture camera RT/TR on the first GTE call per frame
  if (s_need_capture_camera)
  {
    const GTE::Regs& gte = CPU::g_state.gte_regs;

    s_frame_camera_tr[0] = static_cast<float>(gte.TR[0]);
    s_frame_camera_tr[1] = static_cast<float>(gte.TR[1]);
    s_frame_camera_tr[2] = static_cast<float>(gte.TR[2]);

    for (int row = 0; row < 3; row++)
      for (int col = 0; col < 3; col++)
        s_frame_camera_rt[row * 3 + col] = static_cast<float>(gte.RT[row][col]) / 4096.0f;

    bool tr_nonzero = (s_frame_camera_tr[0] != 0.0f || s_frame_camera_tr[1] != 0.0f || s_frame_camera_tr[2] != 0.0f);
    float row0_len_sq = s_frame_camera_rt[0] * s_frame_camera_rt[0] +
                        s_frame_camera_rt[1] * s_frame_camera_rt[1] +
                        s_frame_camera_rt[2] * s_frame_camera_rt[2];
    bool rot_valid = (row0_len_sq >= 0.5f && row0_len_sq <= 1.5f);

    s_frame_camera_valid = tr_nonzero && rot_valid;
    s_need_capture_camera = false;
  }

  Sx = Sx < -1024 ? -1024 : Sx > 1023 ? 1023 : Sx;
  Sy = Sy < -1024 ? -1024 : Sy > 1023 ? 1023 : Sy;

  Vertex& v = FindFreeScreenCoord(Sx, Sy);
  v.Sx = static_cast<s16>(Sx);
  v.Sy = static_cast<s16>(Sy);
  v.x = x;
  v.y = y;
  v.z = z;
  v.generation = s_frame_counter;
}

//////////////////////////////////////////////////////////////////////////////
// Polygon tracking
//////////////////////////////////////////////////////////////////////////////

bool WantsPolygon()
{
  return s_running;
}

static TextureIndex AssignPolyToTexture(
  const Poly& poly,
  GPUDrawModeReg mode_reg,
  u16 palette_reg,
  GPUTextureWindow texture_window)
{
  TextureState tstate;

  tstate.texture_page_x_base = mode_reg.texture_page_x_base;
  tstate.texture_page_y_base = mode_reg.texture_page_y_base;
  tstate.texture_mode = u16(mode_reg.texture_mode.GetValue());
  tstate.transparency_enable = poly.transparency_enable;
  tstate.palette = palette_reg;
  tstate.window = texture_window;

  const u64 key = tstate.PackIntoU64();
  const u32 next_texture_index = s_textures.size();
  const auto [it, inserted] = s_texture_cache.insert({key, next_texture_index});
  if (!inserted)
  {
    const u32 texture_index = it->second;
    s_textures[texture_index].blob.UpdateBoundingBox(poly);
    return texture_index;
  }

  Texture texture;
  texture.tstate = tstate;
  texture.blob.Init(poly);
  s_textures.push_back(texture);

  return next_texture_index;
}

static u32 s_lookup_attempts = 0;
static u32 s_lookup_failures = 0;

static bool Lookup3DVertsForPoly(const Vertex* v3d[4], const Poly& poly)
{
  s_lookup_attempts++;

  for (int i = 0; i < poly.NumVerts(); ++i)
  {
    const s32 sx = poly.v[i].x;
    const s32 sy = poly.v[i].y;

    const u32 key = PackSXYIntoU32(sx, sy);
    const auto find_result = s_vertex_cache.find(key);

    if (find_result == s_vertex_cache.end())
    {
      s_lookup_failures++;
      return false;
    }

    v3d[i] = &find_result->second;
  }

  return true;
}

void GetCacheStats(u32& cache_size, u32& attempts, u32& failures)
{
  cache_size = static_cast<u32>(s_vertex_cache.size());
  attempts = s_lookup_attempts;
  failures = s_lookup_failures;
}

void DrawPolygon(
  GPURenderCommand rc,
  const GPUBackendDrawPolygonCommand::Vertex verts[4],
  GPUDrawModeReg mode_reg,
  u16 palette_reg,
  GPUTextureWindow texture_window,
  const std::array<float, 3> pgxp_v[4])
{
  Poly poly;

  poly.is_quad = rc.quad_polygon;
  poly.is_rect = false;
  poly.texture_enable = rc.texture_enable;
  poly.transparency_enable = rc.transparency_enable;
  poly.transparency_mode = mode_reg.transparency_mode;
  poly.has_3d_verts = false;

  for (int i = 0; i < poly.NumVerts(); i++)
  {
    poly.v[i].x = verts[i].x; poly.v[i].y = verts[i].y;
    poly.v[i].r = verts[i].r; poly.v[i].g = verts[i].g;
    poly.v[i].b = verts[i].b; poly.v[i].a = verts[i].a;
    poly.v[i].u = verts[i].u; poly.v[i].v = verts[i].v;

    if (rc.texture_enable && rc.raw_texture_enable)
    {
      poly.v[i].r = 128;
      poly.v[i].g = 128;
      poly.v[i].b = 128;
    }
  }

  if (poly.texture_enable)
    poly.texture_index = AssignPolyToTexture(poly, mode_reg, palette_reg, texture_window);

  // Try to get 3D vert positions
  if (!s_config.in_2d_mode)
  {
    if (!ShouldUsePGXP())
    {
      const Vertex* v_3d[4];
      if (Lookup3DVertsForPoly(v_3d, poly))
      {
        poly.has_3d_verts = true;
        for (int i = 0; i < poly.NumVerts(); i++)
        {
          poly.v_3d[i][0] = v_3d[i]->x;
          poly.v_3d[i][1] = v_3d[i]->y;
          poly.v_3d[i][2] = v_3d[i]->z;
        }
      }
    }
    else if (pgxp_v)
    {
      poly.has_3d_verts = true;
      for (int i = 0; i < poly.NumVerts(); i++)
        poly.v_3d[i] = pgxp_v[i];
    }
  }

  s_poly_buffer.push_back(poly);
}

bool WantsRectangle()
{
  return s_running && s_config.in_2d_mode;
}

void DrawRectangle(
  GPURenderCommand rc,
  GPUBackendDrawPolygonCommand::Vertex vert,
  u16 width,
  u16 height,
  GPUDrawModeReg mode_reg,
  u16 palette_reg,
  GPUTextureWindow texture_window)
{
  Poly poly;

  if (rc.texture_enable && rc.raw_texture_enable)
  {
    vert.r = 128;
    vert.g = 128;
    vert.b = 128;
  }

  {
    VRVertex vr_vert;
    vr_vert.x = vert.x; vr_vert.y = vert.y;
    vr_vert.r = vert.r; vr_vert.g = vert.g;
    vr_vert.b = vert.b; vr_vert.a = vert.a;
    vr_vert.u = vert.u; vr_vert.v = vert.v;
    poly.v[0] = poly.v[1] = poly.v[2] = poly.v[3] = vr_vert;
  }

  if (rc.texture_enable)
  {
    if (u32(vert.u) + width - 1 > 255)
      width = 256 - vert.u;
    if (u32(vert.v) + height - 1 > 255)
      height = 256 - vert.v;
  }

  poly.v[1].y += height;
  poly.v[2].x += width;
  poly.v[3].x += width;
  poly.v[3].y += height;

  if (rc.texture_enable)
  {
    poly.v[1].v += height - 1;
    poly.v[2].u += width - 1;
    poly.v[3].u += width - 1;
    poly.v[3].v += height - 1;
  }

  poly.is_quad = true;
  poly.is_rect = true;
  poly.texture_enable = rc.texture_enable;
  poly.transparency_enable = rc.transparency_enable;
  poly.transparency_mode = mode_reg.transparency_mode;
  poly.has_3d_verts = false;

  if (poly.texture_enable)
    poly.texture_index = AssignPolyToTexture(poly, mode_reg, palette_reg, texture_window);

  s_poly_buffer.push_back(poly);
}

//////////////////////////////////////////////////////////////////////////////
// VRAM texture readback orchestration
//////////////////////////////////////////////////////////////////////////////

bool WantsUpdateFromVRAM()
{
  if (!s_running || !IsWarmedUp())
    return false;

  // VR continuous mode needs textures even with is_dry_run
  if (s_config.is_dry_run && !s_config.vr_continuous_mode)
    return false;

  if (s_config.shoot_front_and_back && s_in_front_phase)
    return false;

  return true;
}

void UpdateFromVRAM(const u16* vram_ptr)
{
  if (s_config.texture_mode == TEXTURE_MODE_PACKED)
  {
    std::unordered_map<u64, std::vector<Texture*>> groups;

    for (auto& texture : s_textures)
    {
      if (texture.image)
        continue;

      auto tstate = texture.tstate;
      tstate.palette = 0;
      tstate.window.and_x = 0;
      tstate.window.and_y = 0;
      tstate.window.or_x = 0;
      tstate.window.or_y = 0;
      groups[tstate.PackIntoU64()].push_back(&texture);
    }

    for (const auto& kv : groups)
      PackTexturesIntoImages(kv.second, vram_ptr);
  }
  else
  {
    for (auto& texture : s_textures)
    {
      if (texture.image)
        continue;

      if (s_config.texture_mode == TEXTURE_MODE_FULL)
      {
        texture.blob.min_u = 0;
        texture.blob.min_v = 0;
        texture.blob.max_u = 255;
        texture.blob.max_v = 255;
      }

      texture.image = std::make_shared<TextureImage>();
      texture.image->pixbuf = Image(texture.blob.Width(), texture.blob.Height(), ImageFormat::RGBA8);
      texture.image->semitransparency = texture.tstate.transparency_enable;
      FillTextureFromVRAM(texture, vram_ptr, 0, 0);
    }
  }

  // In VR continuous mode, keep texture cache for dedup across frames
  if (!s_config.vr_continuous_mode)
    s_texture_cache.clear();
}

//////////////////////////////////////////////////////////////////////////////
// Frame lifecycle
//////////////////////////////////////////////////////////////////////////////

static void RecycleVertsAtFrameEnd()
{
  const u32 recycle_period = s_config.vr_continuous_mode ? VR_VERTEX_RECYCLE_PERIOD : VERTEX_RECYCLE_PERIOD;
  if (s_frame_counter < recycle_period)
    return;

  std::erase_if(s_vertex_cache, [recycle_period](const auto& kv) {
    return kv.second.generation <= s_frame_counter - recycle_period;
  });
}

static void FinishFrame()
{
  // In VR continuous mode, double-buffer the completed frame
  if (s_config.vr_continuous_mode && IsWarmedUp())
  {
    s_vr_poly_buffer = s_poly_buffer;
    s_vr_textures = s_textures;

    RecycleVertsAtFrameEnd();
    s_poly_buffer.clear();
    s_frame_counter++;
    return;
  }

  // Non-VR path: check if capture is done
  if (!s_config.is_dry_run && IsWarmedUp() && !IsDoneShooting())
  {
    s_shots_taken++;

    if (IsDoneShooting())
    {
      if (s_config.shoot_front_and_back && s_in_front_phase)
        EnterBackPhase();
      else
        TurnOff();
      return;
    }
  }

  RecycleVertsAtFrameEnd();

  s_poly_buffer.clear();
  s_textures.clear();
  s_texture_cache.clear();
  s_frame_counter++;
}

static void StartFrame()
{
  if (s_config.shoot_front_and_back && !s_in_front_phase)
  {
    if (s_frame_counter < s_front_shots.size())
    {
      s_poly_buffer = s_front_shots[s_frame_counter].poly_buffer;
      s_textures = s_front_shots[s_frame_counter].textures;

      for (u32 i = 0; i != s_textures.size(); i++)
        s_texture_cache[s_textures[i].tstate.PackIntoU64()] = i;
    }
  }
}

void NextFrame()
{
  if (s_running)
  {
    FinishFrame();
    StartFrame();
    s_need_capture_camera = true;
  }

  if (s_run_requested && !s_running)
  {
    s_config = s_ui_config;
    TurnOn();
    s_run_requested = false;
  }
}

//////////////////////////////////////////////////////////////////////////////
// GUI (stub — original 3D screenshot UI removed)
//////////////////////////////////////////////////////////////////////////////

void DrawGuiWindow()
{
  // Original OBJ export GUI removed. VR uses its own debug window.
}

} // namespace Screenshot3D
