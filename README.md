# Majesty 2: GEM 3 Engine Turbo Boost (`Golden v10`)

A high-performance Direct3D 9 proxy, SSE2 vectorizer, and sub-tick presentation decoupler for 1C:Ino-Co's GEM 3 engine (*Majesty 2: The Fantasy Kingdom Sim* and all expansions). Engineered specifically for Apple Silicon via native Metal 4 translation.

---

## Reference System Specifications

| Component | Specification |
| :--- | :--- |
| **Reference Hardware** | **Apple M3 Pro** (`MacBook Pro 14-inch, Mac15,6`) |
| **CPU** | 11-core Apple Silicon (5 Performance + 6 Efficiency cores) |
| **GPU** | 14-core Apple Silicon Metal 4 GPU |
| **Memory** | 18 GB Unified Memory (150 GB/s memory bandwidth) |
| **Display** | Liquid Retina XDR (`3024x1964` native, `1512x982 @ 120Hz ProMotion`) |
| **Operating System** | macOS 27.0 |
| **Runtime Stack** | **Custom Wine 11.0 (WOW64)** |
| **Direct3D Backend** | **`mtld3d v0.13.0`** (Direct3D 9 -> Native Apple Metal Backend) |
| **Engine Hook** | **`d3d9_proxy.dll` (`Golden v10`)** |

---

## Key Subsystems & Architectural Innovations

### 1. 120Hz ProMotion Unlock & Engine Frame-Limiter Removal
In vanilla Majesty 2, the GEM 3 engine couples display refresh to hardcoded spin-waits and presentation throttles:
- **Presentation Override**: Intercepts `IDirect3D9::CreateDevice` and `IDirect3DDevice9::Reset` (`vtbl[16]`) to force `D3DPRESENT_PARAMETERS::PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE` (`0x80000000`), bypassing engine VSync caps.
- **Sleep Elimination**: NOPs out the legacy hardcoded `Sleep(3)` (`6A 03 FF 15 ...`) inside `app.dll!eApplication::Working` (`RVA +0x13ba6`).
- **Spin-Wait Zeroing**: Clears the post-present QPC spin-wait rate limiter variables (`pro_sleepRender` at `app.dll + 0x31df0` and `r_waitForRenderDone` at `app.dll + 0x31e20`), delivering continuous, stutter-free 120 FPS ProMotion pacing.

### 2. Sub-Tick Quant-Render Decoupler (`F10`)
Vanilla GEM 3 rigidly locks world logic simulation (`eApplication::Quant`) with frame rendering (`eApplication::RenderScreen`), causing severe framerate dips when 100+ entities pathfind simultaneously:
- **Simulation/Render Decoupling**: Allows the presentation loop to run at full 120Hz refresh while quantizing game simulation at dynamic intervals (`2x Auto 120Hz` / `3x Force 120Hz`).
- **Dynamic 3D Level Detection**: Features automatic `g_levelUp` auto-latching to engage sub-tick interpolation only when active 3D world geometry and entities are loaded, preventing menu/loading desync.
- **Toggle Control**: Real-time mode switching via hotkey `F10`.

### 3. Authentic sRGB Color Pipeline & 16x Anisotropic Filtering (`F8` + `mtld3d.conf`)
Standard translation layers frequently apply wide-gamut DisplayP3 passthrough and aggressive inverse tone-mapping, washing out the original fantasy aesthetic:
- **Accurate Color Pipeline**: Enforces strict sRGB transfer functions and disables `mtld3d`'s default DisplayP3 passthrough and `BT.2446 ICtCp` inverse tone-mapping (`color.hdr.enable = false`, `color.space = accurate`, `present.maxFps = 0`).
- **Texture Clarity**: Forces 16x Anisotropic Filtering across all 8 sampler stages with a MipMap LOD Bias of `-0.65f` for crystal-clear terrain, unit details, and spell particles.
- **HUD OSD Status Bar**: Provides an unobtrusive top-left 3-pill HUD display showing active pipeline states:
  - `F8`: Color Pipeline (sRGB Accurate) / 16x Aniso status
  - `F9`: Audio Latency & SSE2 Hook vectorization status
  - `F10`: 120Hz Decoupler mode (`Auto` / `Decouple` / `Direct`)
  - `F11`: Free Camera & Pitch / Zoom Unlock

### 4. 13 Hot-Spot x87 -> SSE2 Vector Hooks & Dirty-Rect Pathfinder Cache
Rosetta 2 incurs significant overhead when translating legacy x87 80-bit floating point instructions:
- **SSE2 Vectorization (`m2_wave6_sse2.h`, `m2_pather_sse2.h`)**: Replaces 13 critical math hot-spots (matrix transforms, entity distance culling, frustum testing) with 128-bit SSE2 vector intrinsics.
- **Hierarchical Dirty-Rect Spatial Cache**: Introduces an L1/L2/L3 pathfinding cache that tracks terrain modification zones, eliminating redundant A* node evaluations.
- **A* Heap Budgeting**: Restricts open-heap pops to `96` iterations during active gameplay ticks, smoothing out pathfinding spikes during large monster waves.

### 5. 4-DLC Campaign Selector Restoration (`Majesty2.exe` `0x004D46A6`)
- **Native Campaign Chooser**: Restores the original 4-campaign startup selector (*The Fantasy Kingdom Sim*, *Kingmaker*, *Battles of Ardania*, *Monster Kingdom*) in `Majesty2.exe` at address `0x004D46A6` during standard gameplay.
- **Intelligent Benchmark Bypass**: Automatically bypasses the UI prompt when launched with `-autostart` or automated test flags for headless CI and performance profiling.

---

## Repository Structure

```
├── README.md                      # Architecture documentation & guide
├── RE_FINDINGS.md                 # Deep reverse engineering notes & RVA map
├── mtld3d.conf                    # Metal translation & color configuration
├── sdk/
│   ├── GEM3_ENGINE_API.md         # GEM 3 internal engine structures & vtables
│   └── XBONE_MULTITHREAD_DESIGN.md# Multi-threaded quant/render architecture
└── src/
    ├── d3d9_proxy.cpp             # Primary D3D9 hook & proxy implementation
    ├── d3d9.def                   # DLL export definitions
    ├── m2_turbo_hud.h             # 3-pill top-left OSD overlay
    ├── m2_wave6_sse2.h            # SSE2 vectorization hooks for GEM 3
    ├── m2_pather_sse2.h           # SSE2-accelerated A* pathfinder
    ├── m2_quant_parallel.h        # Parallel quant dispatching primitives
    ├── m2_quant_distributor.h     # Dynamic work distributor for quants
    ├── m2_frustum_sse2.h          # SSE2 frustum culling routines
    ├── m2_arena_allocator.h       # High-speed memory arena for frame temps
    └── xbone_parallel.h           # Thread pool abstraction for worker cores
```

---

## Configuration (`mtld3d.conf`)

Place `mtld3d.conf` in the game root alongside `Majesty2.exe` to configure the Metal backend:

```ini
# mtld3d Configuration for Majesty 2 Golden v10
[color]
color.hdr.enable = false
color.space = accurate

[present]
present.maxFps = 0
present.presentationInterval = 0

[sampler]
sampler.anisotropy = 16
sampler.lodBias = -0.65
```

---

## In-Game Controls & Hotkeys

| Key | Subsystem | Action |
| :---: | :--- | :--- |
| **`F8`** | Color / Sampler | Toggle accurate sRGB pipeline & 16x Anisotropic filtering |
| **`F9`** | Audio / Vector | Toggle audio latency modes & SSE2 vector hooks |
| **`F10`** | Decoupler | Cycle 120Hz Decoupler (`Auto 120Hz` / `Force 120Hz` / `Direct 1:1`) |
| **`F11`** | Camera Unlock | Toggle pitch limits, maximum zoom range, and free camera |

---

## Building from Source

To cross-compile `d3d9.dll` for 32-bit Windows / Wine:

```bash
i686-w64-mingw32-g++ -shared -O3 -s -std=c++17     -static -static-libgcc -static-libstdc++     src/d3d9_proxy.cpp -o d3d9.dll src/d3d9.def     -lkernel32 -luser32 -lgdi32 -ld3d9
```

Copy the generated `d3d9.dll` and `mtld3d.conf` directly into your Majesty 2 root directory.


## Golden v10: RenderRC Hook & GPU Sub-Frame Replay Architecture

### 1. Engine Render Hook (`RenderRC` at `app.dll + 0x14a55`)
- Hooks into Majesty 2's primary rendering callback `RenderRC` at offset `app.dll + 0x14a55`.
- Intercepts viewport and scene rendering passes before frame presentation, preventing engine race conditions and ensuring deterministic HUD/overlay compositing.

### 2. `StretchRect` GPU Sub-Frame Replay
- Transparently captures and resolves intermediate offscreen render targets via hardware-accelerated `IDirect3DDevice9::StretchRect`.
- Enables high-frequency GPU sub-frame replay across multi-threaded render state changes, eliminating HUD flicker and depth buffer synchronization stalls during Turbo mode speedups.

### 3. Sampler State Shadow Cache
- Implements a hardware-level shadow cache for D3D9 sampler states (`D3DSAMP_MINFILTER`, `D3DSAMP_MAGFILTER`, `D3DSAMP_MIPFILTER`, `D3DSAMP_ADDRESSU`/`V`).
- Redundant driver-level state transitions are filtered out in hot render paths, yielding substantial draw call latency reduction under high unit-count scenarios.


### 4. Verified Day 230 Heavy-Combat Telemetry (`golden_v10_final`)
Measured on **Apple M3 Pro** (`Mac15,6`, macOS 27.0, `1512x982 @ 120Hz ProMotion`, Wine 11.0 WOW64 + `mtld3d v0.13.0`) on the heavy late-game save (`Save22` / Day 230, 300+ active entities at 5x simulation speed):

| Metric | Value | Notes |
| :--- | :--- | :--- |
| **Sustained Frame Rate (`fps`)** | **`123.64 FPS`** | Exceeds 120Hz ProMotion display target (`989 frames` in `8.00s` window) |
| **Average Frame Time (`avg_ms`)** | **`8.09 ms`** | `p50 = 1.97 ms`, `p95 = 28.05 ms` |
| **Simulation Quants (`quant_calls`)** | **`152` calls** | `4.66 ms` amortized per visual frame (`0x0087ba9c` 15ms limiter unlocked) |
| **Post-Load Warmup Guard** | **`120` frames (`1:1`)** | Zero frame/quant skipping when `q_ms > 60 ms` (level load) or `q_ms < 6 ms` (Main Menu / DLC selector) |
| **CPU / Memory Footprint** | **`85.7% CPU` / `750.1 MB RSS`** | 4 parallel worker threads across 5 Performance cores |

---

## 5. Golden v11: «Проект Возрождение» (Project Rebirth v1.5.5), Apple MetalFX 2× Retina & 120 FPS Lock

### Mod Base Compatibility: Majesty 2 Collection — «Проект Возрождение» v1.5.5
- Built and verified against the **Majesty 2 Collection «Проект Возрождение» (Project Rebirth v1.5.5)** overhaul (`resource.mod`, `Vozrozhdeniye.ogg`, all 4 campaigns: Base Human Campaign, *Kingmaker / Трон Ардании*, *Battles of Ardania / Битвы Ардании*, and *Monster Kingdom / Королевство Монстров*).
- **Launcher Thread Race Condition Fix (`Majesty2.exe` `0x40de21..0x40dede`)**:
  - In stock `Majesty2.exe`, `LauncherWindow::OnMouseUp` (`0x40ddf0`, running on background thread `0x40e640`) set `launcher->selected_expansion = 0` (`0x40de1a`) and immediately set `launcher->closed = 1` (`0x40de21`) **before** calling `ShowWindow(SW_HIDE)` and comparing `"but_mp1"` / `"but_mp2"` / `"but_mp3"`. Under Wine/macOS, the main thread (`0x40e605`) woke from `Sleep(100)` during `ShowWindow(SW_HIDE)`, observed `closed == 1` while `selected_expansion` was still `0`, and mounted `original` (`Campaign_01`) instead of `expansion_3` (*Monster Kingdom*, `Campaign_Exp_03`).
  - Our binary patch NOPs out the premature `movb $1, 0xc(%esi)` at `0x40de21` and relocates `movb $1, 0xc(%esi)` to `0x40ded9` strictly **after** `0x10(%esi)` is written across all button branches (`m2_closebutton`, `but_m2`, `but_mp1`, `but_mp2`, `but_mp3`).

### Apple Silicon Modern Gaming Stack (`MetalFX` + `120 Hz` Pacing + Native Game Mode)
- **Apple `MetalFX` Spatial Upscaling (`1512×982 -> 3024×1964` Retina 2×)**:
  - Raising Majesty 2's internal D3D9 resolution above `1512×982` shrinks the 2D RTS interface.
  - `src/m2_gamemode.m` (`libm2_gamemode.dylib`) promotes `WineContentView`'s `CAMetalLayer` (`d3dmetal_client_surface`) to `contentsScale = 2.0` (`drawableSize = 3024×1964` integer 2× Retina scale) while keeping the guest D3D9 backbuffer at `1512×982`. This automatically engages `mtld3d v0.13.0`'s native **Apple `MTLFXSpatialScaler`** (`1512×982 -> 3024×1964`) so the 3D scene and edges are reconstructed at full Retina resolution while the 2D UI remains large and readable.
- **1:1 FullScreen Cursor Geometry (Zero Vertical Offset)**:
  - Swizzles `-[WineWindow contentRectForFrameRect:]`, `-[WineWindow frameRectForContentRect:]`, and `-[WineWindow constrainFrameRect:toScreen:]` alongside `NSWindowStyleMaskFullSizeContentView` (without `NSWindowStyleMaskTitled`) so entering native AppKit FullScreen Space never shrinks the D3D9 backbuffer to `1504×917` or shifts mouse hitboxes vertically below the MacBook Pro camera notch (`safeAreaInsets.top = 32pt`).
- **120.0 FPS Precision Frame Pacing Lock & Low Input Latency (`config/mtld3d.conf`)**:
  - Locks presentation to **120.0 FPS (`8.333 ms`)** via `present.maxFps = 120`, bounds swapchain queue depth via `present.maxFrameLatency = 1` + `displaySyncEnabled = YES`, and keeps hardware `NSCursor` (`cursor.software = false`) for crisp 120Hz cursor & camera response.
- **Real-Time Apple Metal Performance HUD (`MTL_HUD_ENABLED=1`)**:
  - Enabled automatically by `libm2_gamemode.dylib` constructor with `MTL_HUD_STATE_REPORTER_DOMAINS=mtld3d` to display live FPS, frame-time graph, and `mtld3d v0.13.0` telemetry on screen (disable anytime with `M2_FAST_HUD=0`).
