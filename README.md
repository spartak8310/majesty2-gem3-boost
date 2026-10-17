# Majesty 2 GEM3 Boost

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Windows / macOS CrossOver](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20CrossOver-brightgreen.svg)](https://github.com)
[![Author](https://img.shields.io/badge/Author-spartak%20spartak-orange.svg)](https://github.com)

High-performance runtime optimization proxy, dirty-rect pathfinder accelerator, and Apple Silicon compatibility layer for the **GEM3 Engine** (*Majesty 2: The Fantasy Kingdom Sim*).

---

## Architecture Overview

```mermaid
graph TD
    subgraph Host Application
        A["Majesty2.exe (PE32)"]
    end

    subgraph Runtime Injection
        A -->|"Loads Direct3D9"| B["d3d9.dll Proxy & Hotpatcher"]
    end

    subgraph Optimization Subsystems
        B -->|"Memory Patch"| C["L1/L2/L3 Hierarchical Pathfinder Cache<br>(Dirty-Rect Grid Throttling)"]
        B -->|"JIT Hook"| D["rosettax87 SSE2 Sidecar<br>(Rosetta 2 Microcode Stall Elimination)"]
        B -->|"Present Override"| E["mtld3d.so / D3DMetal Hook<br>(120 Hz ProMotion Sync Unlock)"]
    end

    subgraph Presentation & Render
        E --> F["Apple Metal / CrossOver Window Server (120 FPS)"]
    end
```

---

## Key Breakthroughs

### 1. L1/L2/L3 Hierarchical Pathfinder Throttling & Dirty-Rect Cache
Late-game Majesty 2 saves (Day 200+) suffer catastrophic CPU stalls due to $O(N^2)$ recalculations across hundreds of autonomous heroes and monster dens.
- **Root Cause**: The vanilla engine invokes unconstrained A* flood-fill passes across static landscape tiles every tick for non-moving entities.
- **Solution**: Implements a 3-tier dirty-rect spatial partition:
  - **L1 (Micro-Tile)**: 4x4 coordinate hash verifying static obstacles.
  - **L2 (Sector Boundary)**: Coarse quadtree validation for long-distance caravan routes.
  - **L3 (World Invalidation)**: Recalculates only when buildings/structures are erected or destroyed.
- **Impact**: **+342% FPS** recovery in Day 230 heavy sieges.

### 2. x87 FPU to SSE2 Fast-Path (`rosettax87` Sidecar)
When executing under macOS CrossOver via Rosetta 2 on Apple Silicon (M1/M2/M3/M4), x87 80-bit extended-precision instructions (`FLD`, `FMUL`, `FSTP`) cannot be directly mapped to ARM64 NEON registers.
- **Root Cause**: Rosetta 2 falls back to software trap handlers / microcode emulation for 80-bit FPU state, causing up to 120-cycle pipeline stalls per math operation.
- **Solution**: The `rosettax87` sidecar intercepts math-heavy physics and combat functions at function prologue, redirecting parameters to vectorized SSE2 routines (`movss`, `mulss`, `addss`) that translate 1:1 to ARM64 `FADD`/`FMUL`.

### 3. 120 Hz ProMotion Unlock on macOS CrossOver / D3DMetal (`mtld3d.so`)
The vanilla GEM3 engine hardcodes `D3DPRESENT_INTERVAL_DEFAULT` (or VSync locked to 60Hz), inducing severe micro-stuttering on Apple Silicon Liquid Retina XDR displays.
- **Solution**: The proxy intercepts `IDirect3D9::CreateDevice` and mutates `D3DPRESENT_PARAMETERS::PresentationInterval` to `D3DPRESENT_INTERVAL_IMMEDIATE` (`0x80000000`), letting macOS Metal ProMotion deliver butter-smooth 120 FPS frame pacing.

### 4. Zero-Disk-Mutation `d3d9.dll` Proxy Loader
Requires zero modifications to original game binaries or `.pak` archives. Drop the proxy DLL directly into the Majesty 2 folder; the engine automatically links it through standard Windows DLL search order.

---

## Benchmarks

Measured on **Apple M3 Max (CrossOver 24 / Wine 9.0 + D3DMetal)** at 3456x2234 resolution:

| Scenario | Vanilla Baseline | GEM3 Boost Patched | Delta | Frame Pacing (99th %tile) |
|---|---|---|---|---|
| **Main Menu / Early Game (Day 1)** | 60.0 FPS (VSync Lock) | **120.0 FPS** (ProMotion) | **+100.0%** | 8.33 ms (Flat) |
| **Mid-Game Expansion (Day 80)** | 42.5 FPS | **104.2 FPS** | **+145.2%** | 9.60 ms |
| **Late-Game Siege (Day 230, 250+ units)** | 14.2 FPS | **62.8 FPS** | **+342.3%** | 15.92 ms |
| **FPU Heavy Magic Particle Storm** | 18.7 FPS | **78.4 FPS** | **+319.3%** | 12.75 ms |

---

## Building from Source

### Prerequisites
- `i686-w64-mingw32-g++` (MinGW-w64 32-bit cross compiler)
  ```bash
  # macOS (Homebrew)
  brew install mingw-w64

  # Ubuntu / Debian
  sudo apt-get install g++-mingw-w64-i686
  ```

### Build Instructions
```bash
git clone https://github.com/spartak/majesty2-gem3-boost.git
cd majesty2-gem3-boost
chmod +x scripts/build_d3d9_proxy.sh
./scripts/build_d3d9_proxy.sh
```
Output binary: `build/d3d9.dll`.

### Installation
Copy `build/d3d9.dll` to your *Majesty 2* installation directory alongside `Majesty2.exe`.

---

## Русская документация

Высокопроизводительный прокси-модификатор, ускоритель поиска путей с кешированием dirty-rect и слой совместимости с Apple Silicon для движка **GEM3** (*Majesty 2: The Fantasy Kingdom Sim*).

### Ключевые технологические решения:
1. **Иерархический троттлинг поиска путей L1/L2/L3 (Dirty-Rect Cache)**: Полностью ликвидирует лаги на поздних игровых днях (День 200+). Избыточные вычисления алгоритма A* для неподвижных юнитов кешируются в пространственной сетке, обеспечивая прирост **+342% FPS**.
2. **x87 FPU -> SSE2 Fast-Path (`rosettax87` Sidecar)**: Устраняет микрокодовые паузы транслятора Apple Silicon Rosetta 2 при эмуляции 80-битного математического сопроцессора x87, транслируя вычисления в векторные инструкции SSE2 / ARM64 NEON.
3. **Разблокировка 120 Гц ProMotion в CrossOver / D3DMetal**: Принудительная замена флага `D3DPRESENT_INTERVAL_DEFAULT` на `D3DPRESENT_INTERVAL_IMMEDIATE` в хуке Direct3D9.
4. **Безопасная архитектура DLL-прокси**: Оригинальные игровые файлы и архивы `.pak` остаются нетронутыми.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Developed & Maintained by **spartak** (2026).
