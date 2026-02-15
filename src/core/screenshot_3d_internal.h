#pragma once
#include "screenshot_3d.h"
#include "video_thread_commands.h"

namespace Screenshot3D {

// GTE hooks
bool WantsModifyNCLIP();
void ModifyNCLIP(s64& value);
void PushVertex(float x, float y, float z, s32& Sx, s32& Sy);
bool ShouldUsePGXP();

// GPU polygon capture
bool WantsPolygon();
void DrawPolygon(GPURenderCommand rc, const GPUBackendDrawPolygonCommand::Vertex verts[4],
                 GPUDrawModeReg mode_reg, u16 palette_reg, GPUTextureWindow texture_window,
                 const std::array<float, 3> pgxp_verts[4]);

// GPU rectangle capture (2D mode)
bool WantsRectangle();
void DrawRectangle(GPURenderCommand rc, GPUBackendDrawPolygonCommand::Vertex vert, u16 width,
                   u16 height, GPUDrawModeReg mode_reg, u16 palette_reg,
                   GPUTextureWindow texture_window);

// VRAM texture readback
bool WantsUpdateFromVRAM();
void UpdateFromVRAM(const u16* vram_ptr);

// Input blocking during multi-exposure captures
bool BlockingInput();

} // namespace Screenshot3D
