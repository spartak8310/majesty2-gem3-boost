#ifndef M2_TURBO_HUD_H
#define M2_TURBO_HUD_H

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "m2_wave6_sse2.h"

static HWND g_m2HudWnd = NULL;
static int  g_m2HudEnabled = -1;
static int  g_m2KeyPrev[5] = {0, 0, 0, 0, 0}; // F8, F9, F10, F11, F12
static double g_m2LastFps = 0.0, g_m2LastAvgMs = 0.0, g_m2LastQuantMs = 0.0, g_m2LastGameX = 1.0;

// [F8] Color & Contrast Profile (with mtld3d.conf color.space=accurate + 16x Aniso)
// 0 = Natural sRGB Accurate (Parallels-Exact Default, no Gamma LUT distortion)
// 1 = Apple XDR Crisp Contrast + 16x Aniso (Mip -0.65)
// 2 = Warm Golden Fantasy + 16x Aniso (Mip -0.95)
// 3 = Neutral 16x Aniso Only
static int g_m2HdrMode = 0;
static int g_m2HdrDirty = 0;
static volatile uint32_t g_m2AnisoUpgrades = 0;

// [F10] Sub-Tick Quant+RenderRC Decoupler (decouples BOTH eApplication::Quant and eApplication::RenderRC via GPU StretchRect Replay)
// 0 = OFF (1:1 stock), 1 = 3x Adaptive 120Hz (Default), 2 = 4x Ultra 120Hz
static int g_m2DecoupleMode = 1;
static int g_m2DecoupleSkipPhase = 0;
static int g_m2DecoupleAccumMs = 0;
// g_m2LastRealQuantMs forward-declared in d3d9_proxy.cpp
static volatile uint32_t g_m2DecoupleSkipped = 0;
static uint32_t g_m2DecoupleSkippedLastSec = 0;
static volatile LONG g_m2SubFrameSkipRender = 0;
static volatile LONG g_m2DidSkipRenderRC = 0;
static volatile uint32_t g_m2DecoupleSkippedRenders = 0;
static uint32_t g_m2DecoupleSkippedRendersLastSec = 0;
static void* g_m2CachedSurf = NULL;
static void* g_m2LastDevice = NULL;
static int g_m2CachedW = 0;
static int g_m2CachedH = 0;
static DWORD g_m2CachedFmt = 0;
// g_m2CachedValid and g_m2PostLoadWarmup forward-declared in d3d9_proxy.cpp
// Render-skip pacing (Quant runs every tick when <9ms; RenderScreen is independently decoupled 1:K)
static int g_m2RenderPhase = 0;
static int g_m2RenderSkipK = 4;
static double g_m2LastRenderMs = 0.0;
static double s_emaRealRenderMs = 0.0;
static int g_m2LastEffK = 1;
static volatile uint32_t g_m2RealFrames = 0;
static volatile uint32_t g_m2ReplayFrames = 0;
static uint32_t g_m2RealFramesLastSec = 0;
static uint32_t g_m2ReplayFramesLastSec = 0;
static LARGE_INTEGER g_m2QpcFreq = {0};

// 120 FPS Unlocker state (app.dll Sleep(3)->NOP + pro_sleepRender=0 + r_waitForRenderDone=0 + VSync OFF + RenderScreen Hook)
static int g_m2Unlock120Installed = 0;
static int g_m2RenderRCHookInstalled = 0;
static int g_m2AppSleepPatched = 0;
static HMODULE g_m2AppMod = NULL;
static uint32_t g_m2ToastFrames = 180; // Show visual indicator bar for first 180 frames and on every F8-F11 keypress

struct M2_D3DSURFACE_DESC_V10 {
    DWORD Format; DWORD Type; DWORD Usage; DWORD Pool;
    DWORD MultiSampleType; DWORD MultiSampleQuality;
    UINT Width; UINT Height;
};

typedef void (__thiscall *PFN_M2RealRenderScreen)(void*);
static PFN_M2RealRenderScreen g_m2RealRenderScreen = NULL;

static inline double M2_QpcMs(void) {
    LARGE_INTEGER t;
    if (g_m2QpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_m2QpcFreq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_m2QpcFreq.QuadPart;
}

// Adaptive real-frame skip factor effK
static inline int M2_RenderSkipK(void) {
    if (g_m2PostLoadWarmup > 0 || !g_levelUp || g_m2DecoupleMode <= 0) return 1;
    if (g_m2DecoupleMode == 1) {
        return (s_emaRealRenderMs <= 6.5) ? 1 :
               (s_emaRealRenderMs <= 10.5) ? 2 :
               (s_emaRealRenderMs <= 14.5) ? 4 :
               (s_emaRealRenderMs <= 19.0) ? 6 : 8;
    }
    return (s_emaRealRenderMs <= 6.5) ? 2 :
           (s_emaRealRenderMs <= 12.0) ? 4 :
           (s_emaRealRenderMs <= 18.0) ? 6 : 8;
}

static void __fastcall M2_HookedRenderScreen(void* thisPtr, void* edx) {
    (void)edx;
    int effK = M2_RenderSkipK();
    g_m2LastEffK = effK;
    int canReplay = g_levelUp && g_m2PostLoadWarmup <= 0 && g_m2CachedValid &&
                    g_m2CachedSurf != NULL && g_m2LastDevice != NULL && g_m2DecoupleMode > 0 && effK > 1;
    if (canReplay) {
        if (++g_m2RenderPhase < effK) {
            g_m2DidSkipRenderRC = 1;
            g_m2DecoupleSkippedRenders++;
            g_m2ReplayFrames++;
            Hook_Present(g_m2LastDevice, NULL, NULL, NULL, NULL);
            g_m2DidSkipRenderRC = 0;
            g_m2SubFrameSkipRender = 0;
            return;
        }
    }
    g_m2RenderPhase = 0;
    g_m2SubFrameSkipRender = 0;
    g_m2DidSkipRenderRC = 0;
    g_m2RealFrames++;
    double t0 = M2_QpcMs();
    g_inRenderScreen = 1;
    if (g_m2RealRenderScreen) {
        g_m2RealRenderScreen(thisPtr);
    } else {
        void** vtbl = *(void***)thisPtr;
        ((PFN_M2RealRenderScreen)vtbl[5])(thisPtr);
    }
    g_inRenderScreen = 0;
    double r = M2_QpcMs() - t0;
    if (r > 0.05 && r < 200.0) {
        double cr = r > 50.0 ? 50.0 : r;
        s_emaRealRenderMs = (s_emaRealRenderMs <= 0.1) ? cr : (s_emaRealRenderMs * 0.8 + cr * 0.2);
        g_m2LastRenderMs = s_emaRealRenderMs;
    }
}

static inline void M2_TurboV10_OnBeforeReset(void) {
    if (g_m2CachedSurf) {
        void** svtbl = *(void***)g_m2CachedSurf;
        typedef ULONG (STDMETHODCALLTYPE *PFN_Release)(void*);
        if (svtbl && svtbl[2]) ((PFN_Release)svtbl[2])(g_m2CachedSurf);
        g_m2CachedSurf = NULL;
    }
    g_m2CachedValid = 0;
    g_m2SubFrameSkipRender = 0;
    g_m2DidSkipRenderRC = 0;
    g_m2RenderPhase = 0;
}

static inline void M2_TurboV10_OnBeforePresent(void* device) {
    if (!device) return;
    static double s_lastPresentMs = 0.0;
    double now = M2_QpcMs();
    if (s_lastPresentMs > 0.0 && g_levelUp && g_m2DidSkipRenderRC) {
        double targetFloorMs = (s_emaRealRenderMs > 14.0) ? 4.2 : 7.8;
        double target = s_lastPresentMs + targetFloorMs;
        if (target - now > 2.2) { Sleep(1); now = M2_QpcMs(); }
        while (now < target && target - now < 8.0) { YieldProcessor(); now = M2_QpcMs(); }
    }
    s_lastPresentMs = M2_QpcMs();
    g_m2LastDevice = device;
    // No cache surface / StretchRect copy during loading or warmup: eliminates GPU texture upload / RT stalls in GameStateLoadLevel
    if (!g_levelUp || g_m2PostLoadWarmup > 0 || g_m2DecoupleMode <= 0) {
        g_m2CachedValid = 0;
        return;
    }
    void** vtbl = *(void***)device;
    if (!vtbl || !vtbl[18] || !vtbl[28] || !vtbl[34]) return;
    typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBackBuffer)(void*, UINT, UINT, DWORD, void**);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRenderTarget)(void*, UINT, UINT, DWORD, DWORD, DWORD, BOOL, void**, HANDLE*);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_StretchRect)(void*, void*, const void*, void*, const void*, DWORD);
    typedef ULONG   (STDMETHODCALLTYPE *PFN_Release)(void*);
    typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc)(void*, M2_D3DSURFACE_DESC_V10*);
    void* bb = NULL;
    if (FAILED(((PFN_GetBackBuffer)vtbl[18])(device, 0, 0, 0, &bb)) || !bb) return;
    void** bvtbl = *(void***)bb;
    M2_D3DSURFACE_DESC_V10 desc;
    memset(&desc, 0, sizeof(desc));
    if (bvtbl && bvtbl[12] && SUCCEEDED(((PFN_GetDesc)bvtbl[12])(bb, &desc)) && desc.Width > 0 && desc.Height > 0) {
        if (!g_m2CachedSurf || g_m2CachedW != (int)desc.Width || g_m2CachedH != (int)desc.Height || g_m2CachedFmt != desc.Format) {
            M2_TurboV10_OnBeforeReset();
            if (SUCCEEDED(((PFN_CreateRenderTarget)vtbl[28])(device, desc.Width, desc.Height, desc.Format, 0, 0, FALSE, &g_m2CachedSurf, NULL))) {
                g_m2CachedW = (int)desc.Width;
                g_m2CachedH = (int)desc.Height;
                g_m2CachedFmt = desc.Format;
            }
        }
        if (g_m2CachedSurf) {
            if (g_m2DidSkipRenderRC && g_m2CachedValid) {
                ((PFN_StretchRect)vtbl[34])(device, g_m2CachedSurf, NULL, bb, NULL, 0);
            } else if (!g_m2DidSkipRenderRC) {
                if (SUCCEEDED(((PFN_StretchRect)vtbl[34])(device, bb, NULL, g_m2CachedSurf, NULL, 0))) {
                    g_m2CachedValid = 1;
                }
            }
        }
    }
    if (bvtbl && bvtbl[2]) ((PFN_Release)bvtbl[2])(bb);
}

static inline const char* M2_HdrModeName(int m) {
    switch (m & 3) {
        case 0: return "sRGB-Accurate(16xAniso)";
        case 1: return "AppleCrisp+16xAniso";
        case 2: return "WarmGold+UltraSharp";
        case 3: return "Neutral16xAniso";
    }
    return "sRGB-Accurate";
}

static inline const char* M2_DecoupleModeName(int m) {
    switch (m % 3) {
        case 0: return "1:1(OFF)";
        case 1: return "Auto-120Hz";
        case 2: return "Ultra-120Hz";
    }
    return "Auto-120Hz";
}

static inline void M2_TurboV9_TickUnlock(void) {
    if (g_m2AppSleepPatched || g_m2Unlock120Installed) {
        HMODULE hApp = g_m2AppMod ? g_m2AppMod : GetModuleHandleA("app.dll");
        if (hApp) {
            *(volatile uint32_t*)((uintptr_t)hApp + 0x31df0) = 0;
            *(volatile uint32_t*)((uintptr_t)hApp + 0x31e20) = 0;
        }
    }
}

static inline void M2_TurboHud_OnPresent(void* pDevice) {
    (void)pDevice;
    M2_TurboV9_TickUnlock();
}

static inline void M2_TurboV8_UnlockApp120Fps(void) {
    if (!g_m2AppMod) {
        g_m2AppMod = GetModuleHandleA("app.dll");
    }
    if (!g_m2AppMod) return;
    uint8_t* base = (uint8_t*)g_m2AppMod;

    // 1. Patch Majesty2.exe main loop 15ms (60 FPS) Sleep limiter at VA 0x0087ba9c (7c 07 52 ff 15 18 c1 87 00 -> eb 07 NOPs)
    //    AND app.dll!eApplication::Working RVA +0x13ba6 (Sleep(3) -> 8x NOP)
    if (!g_m2Unlock120Installed) {
        uint8_t* pExeSleep = (uint8_t*)0x0087ba9c;
        if (pExeSleep[0] == 0x7C && pExeSleep[1] == 0x07 && pExeSleep[2] == 0x52 && pExeSleep[3] == 0xFF && pExeSleep[4] == 0x15) {
            DWORD oldProtE = 0;
            if (VirtualProtect(pExeSleep, 16, PAGE_EXECUTE_READWRITE, &oldProtE)) {
                pExeSleep[0] = 0xEB; // jmp short +7 (unconditionally skip Sleep(15 - elapsed_ms)!)
                for (int k = 2; k < 9; ++k) pExeSleep[k] = 0x90;
                VirtualProtect(pExeSleep, 16, oldProtE, &oldProtE);
                FlushInstructionCache(GetCurrentProcess(), pExeSleep, 16);
                LogLine("turbo_v10: patched Majesty2.exe VA 0x0087ba9c (removed 15ms 60FPS Sleep limiter -> TRUE 120Hz unlocked!)");
            }
        }
        uint8_t* pSleep = base + 0x13ba6;
        if (pSleep[0] == 0x6A && pSleep[1] == 0x03 && pSleep[2] == 0xFF && pSleep[3] == 0x15) {
            DWORD oldProt = 0;
            if (VirtualProtect(pSleep, 8, PAGE_EXECUTE_READWRITE, &oldProt)) {
                for (int k = 0; k < 8; ++k) pSleep[k] = 0x90;
                VirtualProtect(pSleep, 8, oldProt, &oldProt);
                LogLine("turbo_v10: patched app.dll!eApplication::Working RVA +0x13ba6 (Sleep(3) -> 8x NOP)");
            }
        }
        g_m2Unlock120Installed = 1;
        g_m2AppSleepPatched = 1;
    }

    // 2. Hook Majesty2.exe IAT slot 0x0087cc44 (app.dll!?RenderScreen@eApplication@xApp@@UAEXXZ called at 0x0087bbb5)
    if (!g_m2RenderRCHookInstalled) {
        g_m2RealRenderScreen = (PFN_M2RealRenderScreen)GetProcAddress(g_m2AppMod, "?RenderScreen@eApplication@xApp@@UAEXXZ");
        void** pIatSlot = (void**)0x0087cc44;
        if (g_m2RealRenderScreen && *pIatSlot == (void*)g_m2RealRenderScreen) {
            DWORD oldProt2 = 0;
            if (VirtualProtect(pIatSlot, sizeof(void*), PAGE_READWRITE, &oldProt2)) {
                *pIatSlot = (void*)&M2_HookedRenderScreen;
                VirtualProtect(pIatSlot, sizeof(void*), oldProt2, &oldProt2);
                LogLine("turbo_v10: hooked Majesty2.exe IAT 0x0087cc44 app.dll!eApplication::RenderScreen (Sub-Frame RenderScreen Skip + StretchRect Replay)");
            }
        }
        g_m2RenderRCHookInstalled = 1;
    }

    // 3. Clear pro_sleepRender (+0x31df0) / r_waitForRenderDone (+0x31e20). VirtualProtect once only:
    //    per-tick VirtualProtect is a wineserver round trip and showed up as load-time stalls.
    static int s_cvarsRW = 0;
    if (!s_cvarsRW) {
        DWORD oldProt = 0;
        VirtualProtect(base + 0x31df0, 0x100, PAGE_READWRITE, &oldProt);
        s_cvarsRW = 1;
    }
    *(volatile uint32_t*)(base + 0x31df0) = 0;
    *(volatile uint32_t*)(base + 0x31e20) = 0;
    g_m2AppSleepPatched = 1;
}

static inline void M2_TurboV8_ForcePresentParams120Hz(void* pPresentationParameters, const char* caller) {
    if (caller && caller[0] == 'R') {
        M2_TurboV10_OnBeforeReset();
    }
    if (!pPresentationParameters) return;
    DWORD* pp = (DWORD*)pPresentationParameters;
    DWORD origInterval = pp[13];
    DWORD origHz = pp[12];
    char ev[8] = {0};
    if (!(GetEnvironmentVariableA("M2_VSYNC", ev, sizeof(ev)) > 0 && ev[0] == '1')) {
        pp[13] = 0x80000000UL; // D3DPRESENT_INTERVAL_IMMEDIATE
        if (pp[5] < 2) pp[5] = 2; // BackBufferCount = 2 (triple buffering to avoid CAMetalLayer drawable wait)
        if (pp[8] == 0 && pp[12] > 0 && pp[12] < 120) {
            pp[12] = 0;
        }
        char m[192];
        snprintf(m, sizeof(m), "turbo_v10: %s unlocked 120Hz (presentInterval 0x%lX -> 0x%lX IMMEDIATE, refreshHz %lu -> %lu, backbuffers=%lu)",
                 caller, (unsigned long)origInterval, (unsigned long)pp[13], (unsigned long)origHz, (unsigned long)pp[12], (unsigned long)pp[5]);
        LogLine(m);
    }
}

static inline void M2_TurboV7_InitEnv(void) {
    static int s_init = 0;
    if (s_init) return;
    s_init = 1;
    char ev[16] = {0};
    if (GetEnvironmentVariableA("M2_HDR_MODE", ev, sizeof(ev)) > 0 && ev[0] >= '0' && ev[0] <= '3') {
        g_m2HdrMode = ev[0] - '0';
        if (g_m2HdrMode != 0) g_m2HdrDirty = 1;
    }
    ev[0] = 0;
    if (GetEnvironmentVariableA("M2_DECOUPLE", ev, sizeof(ev)) > 0 && ev[0] <= '2' && ev[0] >= '0') {
        g_m2DecoupleMode = ev[0] - '0';
    }
    if (g_m2DecoupleMode > 0) {
        g_r3Cam = 1;
    }
    M2_TurboV8_UnlockApp120Fps();
}

static inline int M2_GetHdrMode(void) {
    return (g_m2HdrMode == 0) ? 1 : g_m2HdrMode;
}

static inline void M2_OnAnisoUpgrade(void) {
    g_m2AnisoUpgrades++;
}

// g_m2PostLoadWarmup forward-declared in d3d9_proxy.cpp
static int g_m2DidSkipQuant = 0;

static inline int M2_DecoupleFilterQuant(int origMs, int* outEffMs) {
    M2_TurboV7_InitEnv();
    if (!g_m2Unlock120Installed || !g_m2RenderRCHookInstalled) M2_TurboV8_UnlockApp120Fps();
    if (origMs < 1) origMs = 1;
    if (origMs > 100) origMs = 100;
    *outEffMs = origMs;
    static double s_lastExecQuantWallMs = 0.0;
    if (!g_levelUp || g_m2PostLoadWarmup > 0 || !g_m2CachedValid || g_m2DecoupleMode <= 0) {
        s_lastExecQuantWallMs = 0.0;
        g_m2DecoupleAccumMs = 0;
        g_m2DecoupleSkipPhase = 0;
        return 0;
    }

    int effK = M2_RenderSkipK();
    if (effK <= 1) {
        s_lastExecQuantWallMs = 0.0;
        *outEffMs = origMs;
        return 0;
    }

    int phase = g_m2RenderPhase;
    int quantPhase = (effK >> 1) - 1;
    if (quantPhase < 0) quantPhase = 0;
    bool isQuantTick = (phase == quantPhase);

    if (!isQuantTick) {
        g_m2DecoupleSkipped++;
        return 1;
    }

    double nowWall = M2_QpcMs();
    int passMs = origMs;
    if (s_lastExecQuantWallMs > 0.0 && (nowWall - s_lastExecQuantWallMs) < 250.0) {
        passMs = (int)(nowWall - s_lastExecQuantWallMs + 0.5);
    }
    s_lastExecQuantWallMs = nowWall;
    double s_decoupleLastQuantMs = g_m2LastRealQuantMs;
    int maxPass = (s_decoupleLastQuantMs > 25.0) ? 42 : 75;
    if (passMs > maxPass) passMs = maxPass;
    if (passMs < 1) passMs = 1;
    *outEffMs = passMs;
    return 0;
}

static inline void M2_DecoupleRecordQuantMs(double q_ms) {
    double clamped_q = q_ms > 40.0 ? 40.0 : q_ms;
    g_m2LastRealQuantMs = (g_m2LastRealQuantMs <= 0.1) ? clamped_q : (g_m2LastRealQuantMs * 0.7 + clamped_q * 0.3);
}

typedef void    (STDMETHODCALLTYPE *PFN_M2SetGammaRamp)(void* pDevice, UINT iSwapChain, DWORD Flags, const void* pRamp);
typedef HRESULT (STDMETHODCALLTYPE *PFN_M2GetBackBuffer)(void* pDevice, UINT iSwapChain, UINT iBackBuffer, DWORD Type, void** ppBackBuffer);
typedef HRESULT (STDMETHODCALLTYPE *PFN_M2ColorFill)(void* pDevice, void* pSurface, const RECT* pRect, DWORD color);
typedef ULONG   (STDMETHODCALLTYPE *PFN_M2Release)(void* pUnknown);

static inline void M2_TurboHud_ApplyHdrRamp(void* pDevice) {
    if (!pDevice) return;
    void** vtbl = *(void***)pDevice;
    if (!vtbl || !vtbl[21]) return;
    struct { WORD r[256]; WORD g[256]; WORD b[256]; } ramp;
    int m = g_m2HdrMode & 3;
    for (int i = 0; i < 256; ++i) {
        float x = (float)i / 255.0f;
        float rf = x, gf = x, bf = x;
        if (m == 1) {
            float s = x * x * (3.0f - 2.0f * x);
            float c = s * 0.28f + x * 0.72f;
            rf = c; gf = c; bf = c;
        } else if (m == 2) {
            float y = powf(x, 0.92f);
            float s = y * y * (3.0f - 2.0f * y);
            float c = s * 0.35f + y * 0.65f;
            rf = c * 1.02f; gf = c * 1.005f; bf = c * 0.98f;
        }
        if (rf < 0.0f) rf = 0.0f; else if (rf > 1.0f) rf = 1.0f;
        if (gf < 0.0f) gf = 0.0f; else if (gf > 1.0f) gf = 1.0f;
        if (bf < 0.0f) bf = 0.0f; else if (bf > 1.0f) bf = 1.0f;
        ramp.r[i] = (WORD)(rf * 65535.0f + 0.5f);
        ramp.g[i] = (WORD)(gf * 65535.0f + 0.5f);
        ramp.b[i] = (WORD)(bf * 65535.0f + 0.5f);
    }
    ((PFN_M2SetGammaRamp)vtbl[21])(pDevice, 0, 0, &ramp);
    g_m2HdrDirty = 0;
}

static inline void M2_TurboHud_DrawToast(void* pDevice) {
    if (g_m2ToastFrames == 0 || !pDevice) return;
    g_m2ToastFrames--;
    void** vtbl = *(void***)pDevice;
    if (!vtbl || !vtbl[18] || !vtbl[35]) return;
    void* pBack = NULL;
    if (FAILED(((PFN_M2GetBackBuffer)vtbl[18])(pDevice, 0, 0, 0, &pBack)) || !pBack) return;

    PFN_M2ColorFill fnFill = (PFN_M2ColorFill)vtbl[35];
    RECT rBg = { 10, 10, 206, 26 };
    fnFill(pDevice, pBack, &rBg, 0xFF141820UL);

    DWORD cF8 = (g_m2HdrMode == 0) ? 0xFF2ECC71UL : (g_m2HdrMode == 1 ? 0xFF00D2FFUL : (g_m2HdrMode == 2 ? 0xFFF1C40FUL : 0xFF95A5A6UL));
    RECT r1 = { 14, 13, 72, 23 };
    fnFill(pDevice, pBack, &r1, cF8);

    DWORD cF9 = g_w6LiveEnabled ? 0xFF00E676UL : 0xFFE74C3CUL;
    RECT r2 = { 78, 13, 136, 23 };
    fnFill(pDevice, pBack, &r2, cF9);

    DWORD cF10 = (g_m2DecoupleMode == 0) ? 0xFF7F8C8DUL : (g_m2DecoupleMode == 1 ? 0xFF00E5FFUL : 0xFFFFAB00UL);
    RECT r3 = { 142, 13, 202, 23 };
    fnFill(pDevice, pBack, &r3, cF10);

    void** surfVtbl = *(void***)pBack;
    if (surfVtbl && surfVtbl[2]) ((PFN_M2Release)surfVtbl[2])(pBack);
}

static inline void M2_TurboHud_SetWindow(HWND hFocus, const void* pPresentationParams) {
    M2_TurboV7_InitEnv();
    if (hFocus && IsWindow(hFocus)) {
        g_m2HudWnd = hFocus;
        return;
    }
    if (pPresentationParams) {
        HWND hDev = ((const HWND*)pPresentationParams)[7];
        if (hDev && IsWindow(hDev)) {
            g_m2HudWnd = hDev;
            return;
        }
    }
}

static inline int M2_TurboHud_IsEnabled(void) {
    if (g_m2HudEnabled < 0) {
        char ev[8] = {0};
        if (GetEnvironmentVariableA("M2_HUD", ev, sizeof(ev)) > 0 && ev[0] == '0') {
            g_m2HudEnabled = 0;
        } else {
            g_m2HudEnabled = 1;
        }
    }
    return g_m2HudEnabled;
}

static inline void M2_TurboHud_RefreshTitleNow(void) {
    if (!M2_TurboHud_IsEnabled()) return;
    HWND wnd = g_m2HudWnd;
    if (!wnd || !IsWindow(wnd)) return;
    char title[352];
    snprintf(title, sizeof(title),
             "Majesty 2 [TURBO v10 120Hz] | %.1f FPS (real:%u rep:%u K:%d) | Quant: %.1fms (%.1fx) | [F8] %s | [F9] SSE2:%s | [F10] Decouple:%s | [F11] Cam:%s",
             g_m2LastFps, g_m2RealFramesLastSec, g_m2ReplayFramesLastSec, g_m2LastEffK,
             g_m2LastQuantMs, g_m2LastGameX > 0.0 ? g_m2LastGameX : 1.0,
             M2_HdrModeName(g_m2HdrMode),
             g_w6LiveEnabled ? "ON(13)" : "OFF",
             M2_DecoupleModeName(g_m2DecoupleMode),
             g_r3Cam ? "ON" : "OFF");
    SetWindowTextA(wnd, title);
}

static inline void M2_TurboHud_PollHotkeys(void* pDevice) {
    M2_TurboV7_InitEnv();
    M2_TurboV9_TickUnlock();
    M2_TurboHud_OnPresent(pDevice);
    if (g_m2HdrDirty && pDevice) {
        M2_TurboHud_ApplyHdrRamp(pDevice);
    }
    M2_TurboHud_DrawToast(pDevice);
    if (!M2_TurboHud_IsEnabled()) return;
    static uint32_t s_keyTick = 0;
    if ((s_keyTick++ & 15u) != 0) return;

    const int vks[5] = { VK_F8, VK_F9, VK_F10, VK_F11, VK_F12 };
    for (int i = 0; i < 5; ++i) {
        int down = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
        if (down && !g_m2KeyPrev[i]) {
            char msg[192];
            g_m2ToastFrames = 180;
            if (i == 0) {
                g_m2HdrMode = (g_m2HdrMode + 1) & 3;
                g_m2HdrDirty = 1;
                M2_TurboHud_ApplyHdrRamp(pDevice);
                MessageBeep(MB_OK);
                snprintf(msg, sizeof(msg), "turbo_hud: [F8] cycled Color/Aniso -> mode %d (%s)",
                         g_m2HdrMode, M2_HdrModeName(g_m2HdrMode));
                LogLine(msg);
                M2_TurboHud_RefreshTitleNow();
            } else if (i == 1) {
                int next = !g_w6LiveEnabled;
                M2_Wave6_SetEnabled(next);
                MessageBeep(MB_OK);
                snprintf(msg, sizeof(msg), "turbo_hud: [F9] toggled 13x SSE2 engine -> %s (pather=%d wave6=%d/10)",
                         next ? "ON" : "OFF", next ? g_patherSSEMask : 0, next ? g_w6InstalledCount : 0);
                LogLine(msg);
                M2_TurboHud_RefreshTitleNow();
            } else if (i == 2) {
                g_m2DecoupleMode = (g_m2DecoupleMode + 1) % 3;
                if (g_m2DecoupleMode > 0) g_r3Cam = 1;
                MessageBeep(MB_OK);
                snprintf(msg, sizeof(msg), "turbo_hud: [F10] cycled Sub-Tick Quant-Render Decoupler -> mode %d (%s)",
                         g_m2DecoupleMode, M2_DecoupleModeName(g_m2DecoupleMode));
                LogLine(msg);
                M2_TurboHud_RefreshTitleNow();
            } else if (i == 3) {
                g_r3Cam = !g_r3Cam;
                if (g_r3Cam) g_m2ToastFrames = 600;
                MessageBeep(MB_OK);
                snprintf(msg, sizeof(msg), "turbo_hud: [F11] toggled Sub-Tick Camera Lerp -> %s",
                         g_r3Cam ? "ON" : "OFF");
                LogLine(msg);
                M2_TurboHud_RefreshTitleNow();
            } else if (i == 4) {
                if (g_mainThread) {
                    HANDLE th = CreateThread(NULL, 0, ProfilerThread, NULL, 0, NULL);
                    if (th) CloseHandle(th);
                    LogLine("turbo_hud: [F12] triggered live 7s x86 EIP profile -> Z:\\tmp\\m2_x86_profile.txt");
                }
            }
        }
        g_m2KeyPrev[i] = down;
    }
}

static inline void M2_TurboHud_UpdateSec(void* pDevice, double total_el, double fps, double avg_ms, double quant_ms, double game_x) {
    M2_TurboV8_UnlockApp120Fps();
    g_m2LastFps = fps;
    g_m2LastAvgMs = avg_ms;
    g_m2LastQuantMs = quant_ms;
    g_m2LastGameX = game_x;
    uint32_t sk = g_m2DecoupleSkipped;
    uint32_t skr = g_m2DecoupleSkippedRenders;
    uint32_t rf = g_m2RealFrames;
    uint32_t repf = g_m2ReplayFrames;
    uint32_t an = g_m2AnisoUpgrades;
    g_m2DecoupleSkipped = 0;
    g_m2DecoupleSkippedRenders = 0;
    g_m2RealFrames = 0;
    g_m2ReplayFrames = 0;
    g_m2AnisoUpgrades = 0;
    g_m2DecoupleSkippedLastSec = sk;
    g_m2DecoupleSkippedRendersLastSec = skr;
    g_m2RealFramesLastSec = rf;
    g_m2ReplayFramesLastSec = repf;
    if (g_m2HdrMode == 1 || g_m2HdrMode == 2) {
        M2_TurboHud_ApplyHdrRamp(pDevice);
    }
    if (g_logFile != INVALID_HANDLE_VALUE) {
        char line[384];
        int len = snprintf(line, sizeof(line),
                           "# turbo_v8 [M2-TURBO-V10] t=%.2f fps=%.1f real_frames=%u replay_frames=%u effK=%d ema_render_ms=%.2f warmup=%d level_up=%d hdr_mode=%d(%s) aniso_upgrades=%u decouple_mode=%d(%s) decouple_skipped_quants=%u decouple_skipped_renders=%u last_real_quant_ms=%.2f unlock120=%d\n",
                           total_el, fps, rf, repf, g_m2LastEffK, s_emaRealRenderMs, g_m2PostLoadWarmup, (int)g_levelUp,
                           g_m2HdrMode, M2_HdrModeName(g_m2HdrMode), an,
                           g_m2DecoupleMode, M2_DecoupleModeName(g_m2DecoupleMode), sk, skr, g_m2LastRealQuantMs, g_m2Unlock120Installed);
        if (len > 0) {
            DWORD w = 0;
            WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
        }
    }
    M2_TurboHud_RefreshTitleNow();
}

#endif // M2_TURBO_HUD_H
