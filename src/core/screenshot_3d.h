#pragma once
#include "types.h"
#include "gpu_types.h"
#include "util/image.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Screenshot3D {

//////////////////////////////////////////////////////////////////////////////
// VR Data Types
//////////////////////////////////////////////////////////////////////////////

/// Vertex with screen coords, color, and texture coords.
/// Self-contained — no dependency on GPU backend command types.
struct VRVertex
{
  s32 x, y;
  u8 r, g, b, a;
  u8 u, v;
};

/// Captured polygon with 3D vertex positions
struct Poly
{
  VRVertex v[4];
  std::array<float, 3> v_3d[4]; // 3D vert positions from GTE

  u8 is_quad : 1;
  u8 is_rect : 1;
  u8 texture_enable : 1;
  u8 transparency_enable : 1;
  u8 has_3d_verts : 1;

  GPUTransparencyMode transparency_mode;

  u32 texture_index;

  int NumVerts() const { return is_quad ? 4 : 3; }
};

/// Texture page state for a polygon
struct TextureState
{
  u16 texture_page_x_base : 4;
  u16 texture_page_y_base : 1;
  u16 transparency_enable : 1;
  u16 texture_mode : 2;

  u16 palette;

  GPUTextureWindow window;

  ALWAYS_INLINE u16 GetTexturePageBaseX() const { return ZeroExtend16(texture_page_x_base) * 64; }
  ALWAYS_INLINE u16 GetTexturePageBaseY() const { return ZeroExtend16(texture_page_y_base) * 256; }
  GPUTextureMode GetTextureMode() const { return GPUTextureMode(texture_mode); }
  GPUTexturePaletteReg GetPaletteReg() const
  {
    GPUTexturePaletteReg reg;
    reg.bits = palette;
    return reg;
  }

  // For use as unordered_map key
  u64 PackIntoU64() const
  {
    return (u64(texture_page_x_base) << 0) | (u64(texture_page_y_base) << 4) |
           (u64(transparency_enable) << 5) | (u64(texture_mode) << 6) | (u64(palette) << 16) |
           (u64(window.and_x) << 32) | (u64(window.and_y) << 40) | (u64(window.or_x) << 48) |
           (u64(window.or_y) << 56);
  }
};

/// UV bounding box of polys drawn with the same TextureState
struct UVBlob
{
  u8 min_u, max_u;
  u8 min_v, max_v;

  u32 Width() const { return u32(max_u) - u32(min_u) + 1; }
  u32 Height() const { return u32(max_v) - u32(min_v) + 1; }

  void Init(const Poly& poly);
  void UpdateBoundingBox(const Poly& poly);
  bool Collides(const UVBlob& other) const;
};

/// Texture image data
struct TextureImage
{
  Image pixbuf;
  std::string filename;
  bool semitransparency; // uses Alpha=50% for texels with the semitransparent bit?
  bool is_written = false;

  void HashAndAssignFileName();
};

/// Captured texture with state and pixel data
struct Texture
{
  TextureState tstate;
  UVBlob blob;
  // Shared because multiple textures can be packed into one image
  std::shared_ptr<TextureImage> image;
};

//////////////////////////////////////////////////////////////////////////////
// Core Lifecycle
//////////////////////////////////////////////////////////////////////////////

void NextFrame();
void Shutdown();
void DrawGuiWindow();

//////////////////////////////////////////////////////////////////////////////
// VR Geometry Access API
//////////////////////////////////////////////////////////////////////////////

bool IsGeometryReady();
const std::vector<Poly>& GetPolygonBuffer();
const std::vector<Texture>& GetTextureBuffer();
u32 GetFrameCounter();
bool IsRunning();
void SetVRContinuousMode(bool enabled);
bool IsVRContinuousModeEnabled();
bool GetCameraTransform(float* out_tr, float* out_rt);
void GetCacheStats(u32& cache_size, u32& attempts, u32& failures);

} // namespace Screenshot3D
