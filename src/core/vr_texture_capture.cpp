// SPDX-FileCopyrightText: 2024 DuckStation VR Contributors
// SPDX-License-Identifier: GPL-3.0
//
// PS1 VRAM texture extraction — reads paletted/direct-color texels from VRAM
// and fills TextureImage pixel buffers for VR rendering.

#include "screenshot_3d.h"
#include "gpu_types.h"
#include "gpu_helpers.h"

#include "common/log.h"

#include "util/image.h"

#include "xxhash.h"
#if defined(CPU_ARCH_X86) || defined(CPU_ARCH_X64)
#include "xxh_x86dispatch.h"
#endif

#include "fmt/format.h"

#include <vector>

LOG_CHANNEL(VR);

namespace Screenshot3D {

//////////////////////////////////////////////////////////////////////////////
// Pixel-level VRAM readback
//////////////////////////////////////////////////////////////////////////////

void FillTextureFromVRAM(Texture& texture, const u16* vram_ptr, u32 dst_x, u32 dst_y)
{
#define GetPixel(x, y) (vram_ptr[VRAM_WIDTH * (y) + (x)])
  const auto draw_mode = texture.tstate;
  const auto palette = texture.tstate.GetPaletteReg();
  const auto texture_mode = texture.tstate.GetTextureMode();
  const auto& window = texture.tstate.window;
  auto& pixbuf = texture.image->pixbuf;

  for (u32 ofs_y = 0; ofs_y < texture.blob.Height(); ++ofs_y)
  {
    for (u32 ofs_x = 0; ofs_x < texture.blob.Width(); ++ofs_x)
    {
      u32 image_x = dst_x + ofs_x;
      u32 image_y = dst_y + ofs_y;
      u8 texcoord_x = texture.blob.min_u + ofs_x;
      u8 texcoord_y = texture.blob.min_v + ofs_y;

      // Apply texture window
      texcoord_x = (texcoord_x & window.and_x) | window.or_x;
      texcoord_y = (texcoord_y & window.and_y) | window.or_y;

      u16 texture_color;
      if (texture_mode == GPUTextureMode::Palette4Bit)
      {
        const u16 palette_value = GetPixel(
          (draw_mode.GetTexturePageBaseX() + ZeroExtend32(texcoord_x / 4)) % VRAM_WIDTH,
          (draw_mode.GetTexturePageBaseY() + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT
        );
        const u16 palette_index = (palette_value >> ((texcoord_x % 4) * 4)) & 0x0Fu;
        texture_color = GetPixel(
          (palette.GetXBase() + ZeroExtend32(palette_index)) % VRAM_WIDTH,
          palette.GetYBase()
        );
      }
      else if (texture_mode == GPUTextureMode::Palette8Bit)
      {
        const u16 palette_value = GetPixel(
          (draw_mode.GetTexturePageBaseX() + ZeroExtend32(texcoord_x / 2)) % VRAM_WIDTH,
          (draw_mode.GetTexturePageBaseY() + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT
        );
        const u16 palette_index = (palette_value >> ((texcoord_x % 2) * 8)) & 0xFFu;
        texture_color = GetPixel(
          (palette.GetXBase() + ZeroExtend32(palette_index)) % VRAM_WIDTH,
          palette.GetYBase()
        );
      }
      else
      {
        texture_color = GetPixel(
          (draw_mode.GetTexturePageBaseX() + ZeroExtend32(texcoord_x)) % VRAM_WIDTH,
          (draw_mode.GetTexturePageBaseY() + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT
        );
      }

      u32* row = reinterpret_cast<u32*>(pixbuf.GetRowPixels(image_y));

      // Black with transparent bit clear is always fully transparent
      if (texture_color == 0)
      {
        row[image_x] = 0;
        continue;
      }

      // Semitransparency: 50% alpha when both poly and texel have transparent bit set
      u8 alpha;
      if (draw_mode.transparency_enable && (texture_color & 0x8000u))
        alpha = 128;
      else
        alpha = 255;

      const u32 rgb = VRAMRGBA5551ToRGBA8888(texture_color) & 0x00FFFFFFu;
      row[image_x] = rgb | (alpha << 24);
    }
  }
#undef GetPixel
}

//////////////////////////////////////////////////////////////////////////////
// Texture packing
//////////////////////////////////////////////////////////////////////////////

bool AnyCollidesWith(const std::vector<Texture*>& list, const Texture& t)
{
  for (const Texture* tp : list)
  {
    if (tp->blob.Collides(t.blob))
      return true;
  }
  return false;
}

void PackTexturesIntoImages(const std::vector<Texture*>& textures, const u16* vram_ptr)
{
  // Group textures into non-overlapping lists. Each list shares one 256x256 image.
  std::vector<std::vector<Texture*>> lists;

  for (Texture* texture_ptr : textures)
  {
    bool found = false;
    for (auto& list : lists)
    {
      if (!AnyCollidesWith(list, *texture_ptr))
      {
        list.push_back(texture_ptr);
        found = true;
        break;
      }
    }

    if (!found)
      lists.push_back({texture_ptr});
  }

  for (const auto& list : lists)
  {
    auto image = std::make_shared<TextureImage>();
    image->pixbuf = Image(256, 256, ImageFormat::RGBA8);
    image->semitransparency = list[0]->tstate.transparency_enable;

    for (Texture* texture_ptr : list)
    {
      texture_ptr->image = image;
      FillTextureFromVRAM(*texture_ptr, vram_ptr, texture_ptr->blob.min_u, texture_ptr->blob.min_v);

      // Expand to full 256x256 space
      texture_ptr->blob.min_u = 0;
      texture_ptr->blob.min_v = 0;
      texture_ptr->blob.max_u = 255;
      texture_ptr->blob.max_v = 255;
    }
  }
}

//////////////////////////////////////////////////////////////////////////////
// Texture hashing (for deduplication)
//////////////////////////////////////////////////////////////////////////////

void TextureImage::HashAndAssignFileName()
{
  if (!filename.empty())
    return;

  if (!pixbuf.IsValid())
    return;

  const u32 width = pixbuf.GetWidth();
  const u32 height = pixbuf.GetHeight();
  const size_t size = width * height * sizeof(u32);
  XXH128_hash_t hash = XXH3_128bits(pixbuf.GetPixels(), size);

  filename = fmt::format(
    "{:016X}{:016X}{}",
    hash.high64,
    hash.low64 ^ width, // add dependence on dimensions
    semitransparency ? "t" : ""
  );
}

} // namespace Screenshot3D
