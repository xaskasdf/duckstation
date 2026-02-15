# DuckStation VR Project - Technical Brief

## Objetivo
Crear un fork de DuckStation que permita jugar juegos de PS1 en VR usando OpenXR, aprovechando la geometría 3D real del GTE (Geometry Transformation Engine).

---

## Arquitectura PS1 Relevante

### Pipeline Gráfico Original
```
CPU (MIPS R3000A) → GTE (Coprocessor 2) → GPU (2D Rasterizer) → Framebuffer
     │                    │                      │
     │              Transforma 3D→2D        Solo dibuja
     │              Matrices, proyección    primitivas 2D
     │                    │
     └────────────────────┘
        Vértices 3D entran aquí
```

### El Problema
- El GPU de PS1 es **puramente 2D** - recibe polígonos ya proyectados
- No hay Z-buffer nativo
- Para VR necesitamos geometría 3D **antes** de la proyección

### La Solución
Interceptar en el GTE, donde aún tenemos:
- Vértices en espacio 3D (V0, V1, V2)
- Matrices de rotación y traslación
- Información de cámara implícita

---

## Trabajo Previo Relevante

### duckstation-3D-Screenshot (scurest)
- Fork que exporta escenas 3D a OBJ
- **Ya resolvió** la captura de geometría pre-proyección
- Usa PGXP para trackear vértices del GTE al GPU
- GitHub: https://github.com/scurest/duckstation-3D-Screenshot

### PGXP (Parallel/Precision Geometry Transform Pipeline)
- Sistema existente en DuckStation para mejorar precisión geométrica
- Transporta coordenadas de alta precisión y depth desde GTE a GPU
- Ya tiene tracking maduro de outputs del GTE
- **Base ideal** para extender hacia VR

### BotW-BetterVR (Cemu)
- Demuestra arquitectura VR para emuladores
- Usa Vulkan ↔ D3D12 interop para OpenXR
- Intercepta comandos Vulkan para capturar frames

---

## Estructura de Código DuckStation

### Archivos Clave
```
src/core/
├── gte.cpp/h              # GTE - donde capturar vértices 3D
├── cpu_pgxp.cpp/h         # PGXP - tracking de precisión existente
├── gpu.cpp/h              # GPU base class
├── gpu_hw.cpp/h           # Hardware renderer base
├── gpu_hw_vulkan.cpp      # Backend Vulkan
├── gpu_hw_shadergen.cpp   # Generación de shaders
├── system.cpp             # Sistema principal
└── settings.cpp           # Configuraciones (incluye PGXP settings)
```

### Comandos GTE Relevantes
```cpp
// En gte.cpp - comandos de transformación
RTPS   // Rotation, Translation, Perspective Single (1 vértice)
RTPT   // Rotation, Translation, Perspective Triple (3 vértices)
MVMVA  // Multiply Vector by Matrix and Add Vector
NCLIP  // Normal Clipping (para backface culling)
```

### Registros GTE Importantes
```cpp
// Input (vértices 3D)
V0, V1, V2          // Vectores de entrada (x, y, z cada uno)
RT                  // Matriz de rotación 3x3
TR                  // Vector de traslación

// Output (coordenadas 2D)
SXY0, SXY1, SXY2    // Screen coordinates después de proyección
SZ0, SZ1, SZ2, SZ3  // Z values para ordering table
```

---

## Arquitectura Propuesta VR

```
┌─────────────────────────────────────────────────────────────────┐
│                        DuckStation VR Fork                       │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────┐    ┌──────────┐    ┌──────────────────────────┐  │
│  │   GTE    │───▶│   PGXP   │───▶│  VR Geometry Collector   │  │
│  │ gte.cpp  │    │ cpu_pgxp │    │  - Captura vértices 3D   │  │
│  │          │    │ .cpp     │    │  - Matrices de transform │  │
│  └──────────┘    └──────────┘    └───────────┬──────────────┘  │
│                                               │                  │
│  ┌──────────────────────────────────────────▼──────────────┐   │
│  │                   VR Render Backend                      │   │
│  │  - Reconstruye escena 3D desde comandos GTE             │   │
│  │  - Aplica stereo offset para cada ojo                   │   │
│  │  - Renderiza con perspectiva correcta                   │   │
│  └─────────────────────────┬───────────────────────────────┘   │
│                            │                                     │
│  ┌─────────────────────────▼───────────────────────────────┐   │
│  │                   OpenXR Integration                     │   │
│  │  - XrSession, XrSwapchain (stereo array)                │   │
│  │  - Head tracking → cámara virtual                       │   │
│  │  - Frame timing/prediction                               │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Archivos VR Implementados

```
src/util/vr/
├── vr_system.cpp/h        # OpenXR init, session, swapchains, frame timing
├── vr_renderer.cpp/h      # Vulkan stereo rendering, texture management
├── vr_shader.vert         # Vertex shader (GLSL source)
├── vr_shader.frag         # Fragment shader (GLSL source)
├── vr_shader_vert.spv.h   # Generated C header (SPIR-V byte array) - committed
├── vr_shader_frag.spv.h   # Generated C header (SPIR-V byte array) - committed
├── compile_shaders.bat    # GLSL→SPIR-V→C header build script (Windows)
└── spv_to_header.cmake    # SPIR-V→C header conversion (CMake/cross-platform)

src/util/
└── openxr_input_source.cpp/h  # Quest Touch controller input (InputSource)

src/core/
├── screenshot_3d.cpp/h    # 3D geometry capture (from scurest fork)
└── system.cpp             # VR integration hooks
```

---

## Puntos de Hook en Código Existente

### 1. Hook en GTE (gte.cpp)
```cpp
// En Execute_RTPS / Execute_RTPT
void GTE::Execute_RTPT(u32 instruction) {
    // HOOK: Capturar vértices ANTES de proyección
    if (g_vr_system && g_vr_system->IsEnabled()) {
        VRVertex vertices[3] = {
            {m_regs.V0[0], m_regs.V0[1], m_regs.V0[2]},
            {m_regs.V1[0], m_regs.V1[1], m_regs.V1[2]},
            {m_regs.V2[0], m_regs.V2[1], m_regs.V2[2]}
        };
        g_vr_system->GetGeometryCollector()->AddTriangle(
            vertices,
            GetCurrentRotationMatrix(),
            GetCurrentTranslationVector()
        );
    }
    
    // ... código original de transformación ...
}
```

### 2. Extender PGXP (cpu_pgxp.cpp)
```cpp
// Extender PGXPValue para preservar coordenadas 3D originales
struct PGXPValue {
    float x, y, z;                    // Existente: posición 2D + depth
    float world_x, world_y, world_z;  // NUEVO: posición 3D original
    u32 flags;
};
```

### 3. Nuevo Backend VR (vr_renderer.cpp)
```cpp
class VRRenderer {
public:
    void BeginFrame() {
        XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
        xrWaitFrame(m_session, &frameWaitInfo, &m_frameState);
        xrBeginFrame(m_session, nullptr);
    }
    
    void RenderStereo(const VRGeometryBuffer& geometry) {
        // Obtener poses de cada ojo
        XrView views[2];
        xrLocateViews(m_session, &viewLocateInfo, &viewState, 2, &viewCount, views);
        
        for (int eye = 0; eye < 2; eye++) {
            // Bind swapchain image para este ojo
            uint32_t imageIndex;
            xrAcquireSwapchainImage(m_swapchains[eye], nullptr, &imageIndex);
            xrWaitSwapchainImage(m_swapchains[eye], &waitInfo);
            
            // Calcular view matrix con offset de ojo
            Matrix4x4 viewMatrix = PoseToMatrix(views[eye].pose);
            Matrix4x4 projMatrix = FovToProjection(views[eye].fov);
            
            // Renderizar geometría capturada
            RenderGeometry(geometry, viewMatrix, projMatrix);
            
            xrReleaseSwapchainImage(m_swapchains[eye], nullptr);
        }
    }
    
    void EndFrame() {
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        // ... configurar layer views ...
        xrEndFrame(m_session, &frameEndInfo);
    }
};
```

---

## Desafíos y Soluciones

| Desafío | Solución |
|---------|----------|
| Elementos 2D (sprites, HUD) | Renderizar como quad layer de OpenXR frente al jugador |
| Cámara fija del juego | Desacoplar y reemplazar con head tracking |
| 30fps vs 90fps VR | Reprojection/ASW, interpolación de frames |
| Juegos sin geometría GTE | Fallback a pantalla virtual plana |
| Z-fighting | Usar PGXP depth buffer emulation |

---

## Dependencias Requeridas

- OpenXR SDK (loader + headers)
- Vulkan SDK (DuckStation ya lo usa)
- Opcional: OpenXR validation layers para debug

### CMake additions
```cmake
find_package(OpenXR REQUIRED)
target_link_libraries(duckstation-core PRIVATE OpenXR::openxr_loader)
```

---

## Referencias

1. **DuckStation original**: https://github.com/stenzek/duckstation
2. **3D Screenshot fork**: https://github.com/scurest/duckstation-3D-Screenshot
3. **Documentación GTE**: https://psx-spx.consoledev.net/geometrytransformationenginegte/
4. **OpenXR + Vulkan example**: https://github.com/janhsimon/openxr-vulkan-example
5. **BotW VR (referencia)**: https://github.com/Crementif/BotW-BetterVR
6. **OpenXR Tutorial**: https://openxr-tutorial.com/

---

## Fases de Implementación

### Fase 1: Captura de Geometría (base de 3D-Screenshot) ✅ COMPLETADA
- [x] Fork duckstation-3D-Screenshot
- [x] Estudiar implementación de captura existente
- [x] Modificar para buffer continuo en vez de exportación

### Fase 2: OpenXR Básico ✅ COMPLETADA
- [x] Integrar OpenXR loader
- [x] Crear XrInstance, XrSession
- [x] Setup de swapchains estéreo (Vulkan)
- [x] Renderizar frame capturado a ambos ojos
- [x] **Textures rendering** - Texture index remapping (40K+ → 256 slots)
- [x] **Batched uploads** - Single command buffer submission to avoid GPU timeout

**Archivos VR creados:**
```
src/util/vr/
├── vr_system.cpp/h        # OpenXR session, swapchains, frame management
├── vr_renderer.cpp/h      # Stereo rendering, texture upload, Vulkan pipeline
├── vr_player_tracker.cpp/h # Player position detection for first-person VR
├── vr_integration.cpp/h   # Integration API for system initialization
├── vr_shader.vert/frag    # GLSL shaders (compiled to embedded SPIR-V)
```

**Commits clave:**
- `vr: Add OpenXR VR rendering with pre-compiled SPIR-V shaders`
- `vr: Fix texture rendering and shader index handling`
- `vr: Add texture index remapping for proper texture rendering`
- `vr: Batch texture uploads to fix VK_ERROR_DEVICE_LOST crash`
- `vr: Add player tracker for first-person VR (Phase 3.1)`
- `vr: Add VR settings to configuration system`
- `vr: Add VR debug window for testing and diagnostics`
- `vr: Add camera injection for full-scene VR rendering`
- `vr: Add OpenXR controller input source for Quest Touch controllers`
- `vr: Add OpenXR quad layer for 2D content (menus, loading, HUD)`
- `vr: Add navigation modes and fix GTE camera injection crash`

### Fase 3: First-Person VR ← **ACTUAL**
#### Fase 3.1: Camera Analysis ✅ COMPLETADA
- [x] PlayerTracker class with multi-method detection
- [x] Extract camera position from GTE TR register
- [x] Extract rotation from GTE RT matrix
- [x] Convert PS1 coordinates to meters (configurable scale)
- [x] Integrate with renderer's view matrix

#### Fase 3.5: Camera Stability ✅ COMPLETADA
- [x] Hysteresis for FPV mode switching (15 frames threshold)
- [x] Position jump filtering from smoothed position (500 unit threshold)
- [x] Auto re-establishment after 60 consecutive skips
- [x] EMA smoothing for position and rotation
- [x] Discovered: Screenshot3D captures geometry in CAMERA SPACE
- [x] VR headset pose used directly (no offset needed)

#### Fase 3.2: Memory Direct
- [ ] Build database of known game memory layouts
- [ ] SafeReadMemoryWord for player position addresses
- [ ] Per-game address discovery (Crash, Spyro, Tomb Raider, etc.)

#### Fase 3.3: Input Correlation
- [ ] Track geometry centroids over time
- [ ] Correlate movement with controller input
- [ ] Identify player object from correlation score

#### Fase 3.4: UI & Calibration ✅ COMPLETADA
- [x] Debug overlay (detection method, confidence, position)
- [x] Eye height slider in settings
- [x] World scale setting
- [x] VR enable/first-person mode toggles
- [x] Screen distance and scale settings
- [ ] Manual object selection fallback
- [x] HUD as separate overlay layer (via quad layer, Phase 3.10)

**Settings añadidas (Advanced Settings > Tweaks):**
- Enable VR Mode
- VR First-Person Mode
- Show VR Debug Window
- VR Eye Height (0.0-0.5m)
- VR World Scale (0.0001-0.01)
- VR Screen Distance (0.5-10.0m)
- VR Screen Scale (0.5-5.0x)
- VR Navigation Mode (Tank / Camera Yaw / Hybrid)
- VR Rotation Speed (0.5-5.0 rad/s)
- VR Stick Deadzone (0.0-0.5)
- VR Drift Correction (0.001-0.1)

---

## ✅ RESOLVED: Meta/Oculus OpenXR Swapchain Bug

**Status**: FIXED - VR rendering now works on Quest 1

**Root Causes Found**:
1. `ProcessEvents()` was called AFTER checking `IsSessionRunning()`, but ProcessEvents is what transitions the session to RUNNING state
2. VR frame loop (BeginFrame/RenderStereo/EndFrame) was not integrated into the main render loop
3. `IsAvailable()` was loading/unloading OpenXR loader every frame in debug window

**Fixes Applied**:
- Reordered ProcessEvents() to be called BEFORE IsSessionRunning() check
- Integrated VR frame loop into system.cpp PresentDisplay()
- Cached IsAvailable() result to avoid loader spam

**Key Learning**: Screenshot3D captures geometry in CAMERA SPACE (after GTE transform), meaning the game camera is already at origin. No additional position offset should be applied - VR headset pose directly views the geometry.

#### Fase 3.6: Geometry Cache & World-Space Rendering (ATTEMPTED - SUPERSEDED)
- [x] GeometryCache class (`vr_scene_buffer.cpp/h`) - accumulates world-space polygons
- [x] Camera-to-world transform: `world_pos = RT_inv * (cam_pos - TR)`
- [x] Spatial hash deduplication for polygon matching
- [x] Scene transition detection (large camera jumps clear cache)
- [x] Eviction of stale polygons (configurable TTL)
- **SUPERSEDED**: World-space accumulation causes geometry explosion with moving objects.
  Camera injection (Phase 3.7) is the better solution.

#### Fase 3.7: Camera Injection for Full-Scene Rendering ✅ COMPLETADA
- [x] `vr_camera_injector.cpp/h` - Rotates GTE output for VR headset direction
- [x] Hook in `gte.cpp:RTPS` applies delta rotation post-transform
- [x] Computes delta = VR_rotation * game_camera_rotation_inverse
- [x] Gets VR orientation from OpenXR quaternion each frame
- [x] Gets game camera rotation from Screenshot3D::GetCameraTransform
- [x] Proper coordinate conversion (OpenXR Y-up → PS1 Y-down)
- [x] VR screenshot menu option for debugging
- [x] Textures working correctly with 256-texture array approach
- [x] Pre-injection capture: PushVertex uses original positions, not injected
- [x] View matrix includes game camera inverse rotation (camera→world→VR view)
- [x] PoseToViewMatrix: partial quaternion conjugation (only -qy) for Y-down space
- [x] **CRITICAL FIX**: Injection only affects SXY screen coords, NOT MAC/IR/SZ registers

**How it works**: The camera injector makes the game's own CPU-side culling work
with the VR view direction. By rotating GTE vertex output, geometry that would be
visible from the VR headset passes NCLIP culling, so the game renders the full
scene visible from the VR viewpoint - not just what the original game camera sees.

**Two-part system**:
1. **Camera injector** (NCLIP culling): `delta = VR_rot_ps1 * game_rot^T` applied
   to GTE output after RT*V+TR. Only affects screen coords (SXY) for culling.
   PushVertex captures PRE-injection positions (game camera space).
2. **View matrix** (rendering): `V = hmd_view * Fz * game_rot^T * Fz` where
   `Fz = diag(1,1,-1)` matches the vertex shader Z flip. First converts
   camera-space geometry to world space, then applies VR headset rotation.

**PoseToViewMatrix coordinate handling**: The vertex shader flips Z but NOT Y.
Geometry is in Y-down space. This means pitch (X-axis) and roll (Z-axis) rotation
directions are reversed by the Y flip, so only yaw (Y-axis) needs conjugation.
Result: `(qx, -qy, qz, qw)` from OpenXR quaternion.

**GTE injection safety (crash fix)**: The original camera injection modified x/y/z
BEFORE they were written to MAC/IR/SZ registers. Games read MAC/IR back for gameplay
logic (collision detection, pointer computation). When the delta rotation was large
(e.g. fast-spinning pickup item), corrupted values like `0x3E635C5A` (a float bit
pattern) were used as memory addresses, causing crashes. Fix: MAC/IR/SZ use original
pre-injection values; injection computes separate `ir1_for_screen`/`ir2_for_screen`
used only in the Sx/Sy screen coordinate projection.

**Key files:**
```
src/core/vr_camera_injector.cpp/h  # Delta rotation computation and vertex transform
src/core/vr_scene_buffer.cpp/h     # World-space accumulation (for future use)
src/core/gte.cpp:681               # Hook point: injection only affects SXY, not MAC/IR/SZ
src/util/vr/vr_renderer.cpp:1012   # View matrix: hmd_view * game_to_world
```

#### Fase 3.8: Color Correction & Transparency ✅ COMPLETADA
- [x] VR screenshot capture (swapchain readback to PNG with matching emulator screenshot)
- [x] sRGB→linear conversion in fragment shader (`pow(color.rgb, vec3(2.2))`)
- [x] PS1-correct color modulation: `* 2.0` for textured polygons (PS1 uses color/128, not color/255)
- [x] Alpha blending enabled (`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`)
- [x] Two-pass rendering: opaque first, then textured semi-transparent
- [x] Skip untextured transparent polygons (PS1 lighting overlays cause fog without depth sorting)
- [x] Display background blit: emulator 2D framebuffer behind 3D geometry (LOAD_OP_LOAD render pass)
- [x] Smart background disable: blit skipped when camera injection active (wrong perspective)
- [x] Geometry diagnostics: per-frame polygon category counts, z-range, cache stats
- [x] Vertex cache tuning: VR mode uses longer recycle period (4 frames vs 2)
- [ ] Missing lighting: untextured transparent overlays provide scene lighting (RE uses these heavily)
- [ ] Proper PS1 transparency modes (B/2+F/2, B+F, B-F, B+F/4) not yet implemented

**Key discoveries:**
- PS1 vertex color 128 = 1.0x neutral modulation (hardware does `(texel * color) >> 7`)
- Dividing by 255 made 128 → 0.5 → pow(2.2) → 0.22 (extremely dark). Fix: multiply by 2.0 in shader.
- RE environment geometry is drawn with `transparency_enable` (blends over pre-rendered BG).
  Skipping ALL transparent polys removes the entire room; must keep textured transparent ones.
- Untextured transparent polygons are PS1 lighting/shadow overlays that stack and create fog
  in VR without proper depth-sorted blending. Skipping them loses scene lighting but avoids fog.
- Display background blit shows emulator 2D view behind 3D geometry gaps. When camera injection
  is active, this creates visual garbage because the 2D view is from the original game camera
  direction which doesn't match the VR headset rotation. Disabled automatically in that case.
- Polygons without `has_3d_verts` (2D sprites, rectangles, pre-rendered BG elements) are skipped
  in VR rendering. Outdoor scenes visible through windows (trees, sky) are often these 2D elements.

**Key files modified:**
```
src/util/vr/vr_shader.frag          # sRGB correction + PS1 color modulation * 2.0
src/util/vr/vr_renderer.cpp         # Screenshot capture, display blit, diagnostics, two-pass rendering
src/util/vr/vr_renderer.h           # m_screenshot_requested, m_vk_render_pass_load
src/core/gpu.h                      # GetDisplayTextureView{X,Y,Width,Height} accessors
src/core/screenshot_3d.cpp          # VR vertex cache recycle period tuning
src/core/screenshot_3d.h            # GetCacheStats() API
```

#### Fase 3.9: VR Controller Input ✅ COMPLETADA
- [x] `OpenXRInputSource` class implementing DuckStation's `InputSource` interface
- [x] OpenXR action set with boolean, float, and vector2f actions
- [x] Suggested interaction profile bindings for Oculus Touch controller
- [x] Both Quest Touch controllers exposed as single virtual gamepad ("OpenXR-0")
- [x] 11 buttons: A, B, X, Y, Menu, LeftStickBtn, RightStickBtn + derived trigger/grip buttons
- [x] 8 axes: LeftStick X/Y, RightStick X/Y, LeftTrigger, RightTrigger, LeftGrip, RightGrip
- [x] Default PS1 mapping: A→Cross, B→Circle, X→Square, Y→Triangle, Menu→Start
- [x] Triggers→L2/R2 (analog), Grips→L1/R1 (analog), StickBtns→L3/R3
- [x] Derived button events from analog thresholds (trigger/grip ≥ 0.5)
- [x] Thumbstick Y negated for DuckStation convention (negative=up)
- [x] Lazy initialization: dormant until VR system available
- [x] Session restart resilience: treats `XR_ERROR_ACTIONSETS_ALREADY_ATTACHED` as success
- [x] Registered in InputManager with `InputSourceType::OpenXR` (guarded by `ENABLE_OPENXR`)

**Interaction profile**: `/interaction_profiles/oculus/touch_controller` (not `_profile` suffix)

**Session restart handling**: OpenXR action sets are instance-level objects attached to
sessions via `xrAttachSessionActionSets` (once per session). When the runtime restarts the
session (states FOCUSED→STOPPING→IDLE→READY→SYNCHRONIZED→VISIBLE), the action set remains
attached. `TryAttachActionSet` treats error -47 (`XR_ERROR_ACTIONSETS_ALREADY_ATTACHED`) as
success, avoiding the need to destroy and recreate the action set.

**Key files:**
```
src/util/openxr_input_source.cpp/h  # Full InputSource implementation
src/util/input_manager.h            # InputSourceType::OpenXR enum
src/util/input_manager.cpp          # Registration in ReloadSources()
src/util/input_source.h             # CreateOpenXRSource() factory
src/util/vr/openxr_entry_points.inl # Added xrGetActionStateVector2f
src/util/vr/vr_system.h             # Added GetInstance()/GetSession() accessors
```

#### Fase 3.10: OpenXR Quad Layer for 2D Content ✅ COMPLETADA
- [x] `XrCompositionLayerQuad` for 2D emulator display as floating virtual screen
- [x] Quad swapchain (640x480, TRANSFER_DST | COLOR_ATTACHMENT | SAMPLED)
- [x] Blit emulator display to quad swapchain each frame (utility CB #2)
- [x] Quad layer submitted BEHIND projection layer in EndFrame
- [x] Projection layer uses `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`
- [x] RenderFallbackScreen clears to transparent (alpha=0) so quad shows through
- [x] During 3D gameplay, opaque projection layer covers the quad
- [x] During menus/loading (no 3D geometry), transparent projection lets quad show
- [x] Quad positioned at `(0, eye_height, -screen_distance)` in LOCAL reference space
- [x] Size: `1.5 * screen_scale` width, 4:3 aspect ratio
- [x] Fixed m_screen_scale initialization (was `vr_world_scale * 1000`, now `vr_screen_scale`)
- [x] Non-fatal: VR continues without quad if swapchain creation fails

**How it works**: Every frame, the emulator's 2D framebuffer is blitted to a separate
OpenXR quad swapchain. EndFrame submits two layers: quad (behind) and projection (in front).
The projection layer has alpha blending enabled. When 3D geometry is present, RenderEye
clears with opaque black (alpha=1.0), fully covering the quad. When no 3D geometry exists
(menus, loading, BIOS), RenderFallbackScreen clears with transparent black (alpha=0.0),
allowing the quad layer to show through as a floating virtual screen.

**Key files:**
```
src/util/vr/vr_system.h             # SwapchainInfo m_quad_swapchain, EndFrame quad params
src/util/vr/vr_system.cpp           # CreateQuadSwapchain, Acquire/Release, EndFrame quad+projection
src/util/vr/vr_renderer.h           # BlitDisplayToQuadSwapchain, GetQuadLayerParams
src/util/vr/vr_renderer.cpp         # Quad blit implementation, transparent fallback clear
src/util/vr/vr_integration.cpp      # Pass quad params from renderer to system EndFrame
```

#### Fase 3.11: VR Navigation Modes ✅ COMPLETADA
- [x] Three selectable navigation modes: Tank, CameraYaw, Hybrid
- [x] `VRNavigationMode` enum in `types.h`
- [x] Settings: `vr_navigation_mode`, `vr_rotation_speed`, `vr_stick_deadzone`, `vr_drift_correction_alpha`
- [x] Settings Load/Save with Parse/Get functions following GPUWireframeMode pattern
- [x] Raw VR input exposure: `VRRawInputState` struct in `openxr_input_source.h`
- [x] Right stick X cached for yaw rotation (left stick occupied by PS1 controls)
- [x] Both stick buttons pressed simultaneously cycles mode (debounced onset detection)
- [x] Tank mode: pure joystick-driven rotation with deadzone remapping
- [x] CameraYaw mode: VR forward = game camera forward direction
- [x] Hybrid mode: joystick accumulates yaw, 10-frame idle triggers drift correction toward camera yaw
- [x] Angle-aware lerp: normalize diff to [-PI, PI] to avoid wrapping artifacts
- [x] Mode switch re-initializes accumulated_yaw from current facing for seamless transitions
- [x] Reset sets yaw_initialized=false to re-init from camera yaw next frame
- [x] OSD messages on mode cycle and yaw reset
- [x] Debug window: mode display, facing yaw in degrees, Cycle/Reset buttons
- [x] Qt Advanced Settings: Navigation Mode dropdown + 3 float sliders
- [x] `vr_rotation_attached` fully replaced with `vr_first_person_enable` condition

**Replaces**: The old `vr_rotation_attached` setting which used centroid-based yaw
that drifted as the game camera orbited independently of the player's facing direction.

**Key files:**
```
src/core/types.h                           # VRNavigationMode enum
src/core/settings.h/cpp                    # 4 new settings + Parse/Get functions
src/util/openxr_input_source.h/cpp         # VRRawInputState for right stick + stick clicks
src/util/vr/vr_renderer.h/cpp             # 3-mode yaw computation, cycling, reset
src/util/vr/vr_integration.cpp            # Debug window nav mode display + buttons
src/duckstation-qt/advancedsettingswidget.cpp  # Mode dropdown + sliders
```

**Settings added:**
- VR Navigation Mode (Tank / Camera Yaw / Hybrid)
- VR Rotation Speed (0.5-5.0 rad/s, default 2.0)
- VR Stick Deadzone (0.0-0.5, default 0.15)
- VR Drift Correction (0.001-0.1, default 0.01)

#### Fase 3.12: VR HUD Overlay ← **NEXT**
- [ ] Render 2D polygons (has_3d_verts=false) as head-locked floating panels
- [ ] Position HUD elements at fixed distance in front of player
- [ ] PS1 text/sprites visible during 3D gameplay (item pickups, etc.)
- [ ] Transparent background so only text/sprites are visible

**Problem**: During 3D gameplay, the projection layer is opaque (alpha=1.0 clear),
covering the quad layer. 2D elements (text, HUD, sprites) have `has_3d_verts=false`
and are skipped in VR 3D rendering. Full 2D screens (menus, loading) work because
`RenderFallbackScreen` uses alpha=0.0, but mixed scenes (3D room + 2D text overlay)
lose the text. Making the clear transparent doesn't work — quad layer bleeds through
every gap in the 3D geometry (dark areas, floor, ceiling).

### Fase 4: Optimización
- [ ] Multiview rendering
- [ ] Reprojection para timing
- [ ] Configuración de usuario (IPD, world scale, etc.)

---

## VR Vulkan Pipeline Details

### Shader Architecture
- **Vertex shader**: Transforms `a_position` by `pc.u_scale * 0.001`, negates Z, multiplies by `pc.u_view_proj`
- **Fragment shader**: For textured polygons, applies PS1-correct `* 2.0` color modulation (PS1 uses color/128 not color/255, so vertex color 128 = neutral 1.0x). Applies `pow(color.rgb, vec3(2.2))` sRGB→linear conversion for correct output to `VK_FORMAT_*_SRGB` swapchains. Falls back to vertex color only when `v_texture_index >= 256`.
- **SPIR-V build pipeline**: Shaders are compiled to SPIR-V and stored as C byte arrays in generated `.spv.h` headers, `#include`d by `vr_renderer.cpp`. Headers are committed so builds work without Vulkan SDK. Regeneration is automatic via `compile_shaders.bat` (MSVC PreBuildEvent) or CMake `add_custom_command`, and skips when outputs are newer than sources.

### Descriptor Layout
- **Set 0, Binding 0**: `sampler2D u_textures[256]` - array of combined image samplers
- **Push constants**: 84 bytes (mat4 view_proj + float scale + float[3] padding + uint texture_index)
- **Push constant range**: `VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT`

### Per-Texture Draw Calls
Instead of non-uniform descriptor indexing (which requires `VK_EXT_descriptor_indexing`),
geometry is sorted by texture index and drawn in ranges:
```
for (range in draw_ranges):
    push_constants.texture_index = range.texture_index
    vkCmdPushConstants(...)
    vkCmdDrawIndexed(range.index_count, 1, range.first_index, 0, 0)
```

### Texture Upload
- Textures captured by Screenshot3D as raw RGBA pixel data
- Remapped from 40K+ potential textures to max 256 VkImage slots
- Uploaded in a single batched command buffer to avoid VK_ERROR_DEVICE_LOST
- Unused slots filled with 1x1 magenta placeholder
- All images must be transitioned to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`

### Display Background Blit
- Before 3D geometry rendering, the emulator's 2D framebuffer can be blitted to the VR swapchain
- Uses `LOAD_OP_LOAD` render pass to preserve the blitted background behind 3D geometry
- Automatically disabled when camera injection is active (2D view from wrong perspective)
- Accesses emulator display via `g_gpu->GetDisplayTextureHandle()` and view rect accessors

### Command Buffer Strategy
- 3 command buffers: eye 0, eye 1, utility (texture upload)
- 3 fences for synchronization
- Each eye rendered with full fence wait before OpenXR swapchain release

---

## Comandos Útiles para Desarrollo

```bash
# Clonar fork base
git clone https://github.com/scurest/duckstation-3D-Screenshot.git duckstation-vr
cd duckstation-vr

# Build (Linux)
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Build con OpenXR
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_VR=ON ..
```

### Build (Windows - MSVC)
```bash
# Build duckstation-qt (Release x64)
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
  "C:\Users\xasko\duckstation-vr\duckstation.sln" \
  /t:duckstation-qt /p:Configuration=Release /p:Platform=x64 /p:VcpkgEnabled=false /m

# Binary output
C:\Users\xasko\duckstation-vr\bin\x64\duckstation-qt-x64-Release.exe

# Compile SPIR-V shaders (automatic during build if VULKAN_SDK is set)
# Manual: run from src/util/vr/ directory
src\util\vr\compile_shaders.bat
# Skips if .spv.h outputs are newer than .vert/.frag sources
```
