// d3d9_proxy.cpp — Majesty 2 Native PE32 D3D9 Proxy + Frame-Time Sensor + Turbo-Lab Hook Host
// Wave 1 (t_m2_frustum_lod): Gribb-Hartmann camera frustum extraction from SetTransform + spatial zone culling
// Wave 2 (t_m2_pf_particle_cull): particle-flow off-screen culling (runtime discovery in pf.dll/scene.dll,
//                                  SSE2 batch cull kernel, no hard-coded game RVAs)
// Compiled on Clore #84966 via cc-clore-exec (i686-w64-mingw32-g++ -O3 -msse4.2)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef void* (WINAPI *PFN_Direct3DCreate9)(UINT SDKVersion);
typedef int   (WINAPI *PFN_D3DPERF_BeginEvent)(DWORD col, LPCWSTR wszName);
typedef int   (WINAPI *PFN_D3DPERF_EndEvent)(void);
typedef void  (WINAPI *PFN_D3DPERF_SetMarker)(DWORD col, LPCWSTR wszName);
typedef void  (WINAPI *PFN_D3DPERF_SetRegion)(DWORD col, LPCWSTR wszName);
typedef BOOL  (WINAPI *PFN_D3DPERF_QueryRepeatFrame)(void);
typedef void  (WINAPI *PFN_D3DPERF_SetOptions)(DWORD dwOptions);
typedef DWORD (WINAPI *PFN_D3DPERF_GetStatus)(void);

static HMODULE g_realD3D9 = NULL;
static PFN_Direct3DCreate9 g_realDirect3DCreate9 = NULL;

// IDirect3D9::CreateDevice is vtable[16]
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(
    void* pD3D9, UINT Adapter, DWORD DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, void* pPresentationParameters, void** ppReturnedDeviceInterface);

// IDirect3DDevice9::Present is vtable[17]
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(
    void* pDevice, const RECT* pSourceRect, const RECT* pDestRect,
    HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);

// IDirect3DDevice9 draw entry points: DrawPrimitive [81], DrawIndexedPrimitive [82], DrawPrimitiveUP [83], DrawIndexedPrimitiveUP [84]
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitive)(void* pDevice, DWORD type, UINT startVertex, UINT primCount);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitive)(void* pDevice, DWORD type, INT baseVertex, UINT minIndex,
                                                              UINT numVertices, UINT startIndex, UINT primCount);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitiveUP)(void* pDevice, DWORD type, UINT primCount,
                                                         const void* vertexData, UINT stride);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitiveUP)(void* pDevice, DWORD type, UINT minIndex, UINT numVertices,
                                                                UINT primCount, const void* indexData, DWORD indexFormat,
                                                                const void* vertexData, UINT stride);
typedef void* (__cdecl *PFN_Malloc)(size_t size);
typedef LPVOID (WINAPI *PFN_HeapAlloc)(HANDLE heap, DWORD flags, SIZE_T size);

// Wave 3 state/forward decls: the draw hooks below and Hook_SetTransform / Hook_AppQuant (all defined
// above the Wave 3 section) call into it, exactly like the Wave 1 InstallSetTransformCapture forward
// declaration further down.  Everything else Wave 3 lives in its own section next to Hook_Present.
static int g_r3MinDraws = 0;                 // M2_PRESENT_MIN_DRAWS: 0 = rule off (0 cost on the hot path)
static int g_r3Cam = 0;                      // M2_CAM_LERP: sub-tick camera extrapolation armed
static volatile uint32_t g_r3Draws = 0;      // monotonic draw-call counter, reset-point independent
static void M2_R3Init(void);
static int64_t M2_R3NowUs(void);
static void M2_R3CamSample(const float* view);
static const void* M2_R3CamBlendView(const void* pMatrix, int64_t nowUs, float* scratch);
static void M2_R3QuantTick(int64_t endUs);

static PFN_CreateDevice g_origCreateDevice = NULL;
static PFN_Present      g_origPresent      = NULL;
static PFN_DrawPrimitive          g_origDrawPrimitive          = NULL;
static PFN_DrawIndexedPrimitive   g_origDrawIndexedPrimitive   = NULL;
static PFN_DrawPrimitiveUP        g_origDrawPrimitiveUP        = NULL;
static PFN_DrawIndexedPrimitiveUP g_origDrawIndexedPrimitiveUP = NULL;
// (Splice 1: per-module real-fn statics replaced by the MSVCR80 capture in the M2_ARENA section)

// Per-second telemetry counters: plain increments, no QPC/locks on the hot path (a lost increment under a
// cross-thread race is acceptable). Reset by Hook_Present when the per-second line is written.
static volatile uint32_t g_draws = 0, g_prims = 0, g_upDraws = 0;
static volatile uint32_t g_allocExe = 0, g_allocScene = 0, g_allocRender = 0;

// Wave 2 (particle culling): fast-path guard for the Draw* caller-evidence channel.  0 unless
// M2_PARTICLE_CULL/M2_PF_SCAN/M2_DRAW_HISTOGRAM is active AND a configured pf module is mapped, so the
// untouched hot path pays one predictable load + test.
static volatile LONG g_pfEvOn = 0;

// Per-second phase timers (QPC ticks spent inside the real D3D9 calls), reset with the telemetry line.
// logic = frame wall time - every bucket below - present.
enum { B_DRAW, B_LOCK, B_STATE, B_MISC, B_CREATE, B_COUNT };
static volatile int64_t g_bucketTicks[B_COUNT];
static volatile uint32_t g_bucketCalls[B_COUNT];
static volatile int64_t g_presentTicks = 0;
static volatile uint32_t g_foreignCalls = 0;  // timed D3D calls made from a thread other than the Present thread
static DWORD g_renderTid = 0;

static inline int64_t Qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static inline void Account(int b, int64_t t0) {
    g_bucketTicks[b] += Qpc() - t0;
    g_bucketCalls[b]++;
    if (g_renderTid && GetCurrentThreadId() != g_renderTid) g_foreignCalls++;
}

static LARGE_INTEGER g_qpcFreq = {0};
static LARGE_INTEGER g_lastFrame = {0};
static LARGE_INTEGER g_startTick = {0};
static LARGE_INTEGER g_lastSecTick = {0};
static HANDLE g_ftFile = INVALID_HANDLE_VALUE;
static HANDLE g_logFile = INVALID_HANDLE_VALUE;

static uint32_t g_secFrameUs[4096];
static uint32_t g_secFrameCount = 0;
static uint32_t g_totalFrames = 0;

static void InitSensorFiles(void) {
    if (g_qpcFreq.QuadPart != 0) return;
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_startTick);
    g_lastSecTick = g_startTick;

    g_ftFile = CreateFileA("Z:\\tmp\\m2_frametimes.bin", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_logFile = CreateFileA("Z:\\tmp\\m2_fps.log", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}


// ================== Wave 1: Gribb-Hartmann camera frustum core ==================
// ---- M2_FRUSTUM_CORE_BEGIN ----
// Self-contained float-only block (no Windows types, no logging, no globals) so that
// scripts/test_frustum_math.py can extract it verbatim, compile it on the host and unit-test the
// plane extraction off-line.  Keep every dependency inside these two markers.
//
// Clip space is D3D9's: -w <= x <= w, -w <= y <= w, 0 <= z <= w.  With C = view * projection in the
// row-vector D3D convention (row-major storage, C[i][j] = c[i*4+j]) a frustum plane is one row
// selector k applied to C:
//     plane_axis = sum_j k[j] * C[axis][j]     (axis 0..2 = xyz normal, axis 3 = constant d)
//     inside <=> plane.x*p.x + plane.y*p.y + plane.z*p.z + plane.d >= 0
//     left:   k = ( 1, 0, 0, 1)      right:  k = (-1, 0, 0, 1)
//     bottom: k = ( 0, 1, 0, 1)      top:    k = ( 0,-1, 0, 1)
//     near:   k = ( 0, 0, 1, 0)      far:    k = ( 0, 0,-1, 1)
// The engine may hand SetTransform either the D3D9 row-major matrix or its transpose (a GL-style
// column-major math library adapted at the D3D boundary) and we cannot inspect the matrices offline,
// so M2_FrustumBuild() tries all four {view, projection} x {as-is, transposed} combinations and keeps
// the one that survives a geometric self-test: the camera eye/forward derived from the *same* view
// matrix must sit inside the resulting frustum, a point behind the camera must be outside and a point
// past the far plane must be outside.  When no convention reaches M2_FRUSTUM_MIN_SCORE the frustum
// stays invalid and every query fails open ("visible"), so a wrong convention can never cull content
// that really is on screen.
#define M2_FRUSTUM_MAX_SCORE 7
#define M2_FRUSTUM_MIN_SCORE 6

struct M2Frustum {
    float plane[6][4];   // normalised: left, right, bottom, top, near, far
    float eye[3];        // camera position in world space (derived from the view matrix)
    float fwd[3];        // camera forward axis in world space (view +Z, unit length)
    float nearDist;      // distance eye -> near plane
    float farDist;       // distance eye -> far plane
    int   convention;    // bit0: view stored transposed, bit1: projection stored transposed
    int   valid;
    int   score;
};

static float M2_Abs(float v) { return __builtin_fabsf(v); }
static float M2_Sqrt(float v) { return __builtin_sqrtf(v); }
static int M2_IsFinite(float v) { return v == v && v > -1.0e9f && v < 1.0e9f; }

static void M2_Copy16(float* dst, const float* src) {
    for (int i = 0; i < 16; ++i) dst[i] = src[i];
}
static void M2_Transpose16(float* dst, const float* m) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            dst[i * 4 + j] = m[j * 4 + i];
}
// out = a * b (row-vector composition: clip = world * view * projection)
static void M2_Mul16(float* out, const float* a, const float* b) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                             a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
}
static void M2_PlaneFromSelector(float* out, const float* c, float k0, float k1, float k2, float k3) {
    out[0] = k0 * c[0]  + k1 * c[1]  + k2 * c[2]  + k3 * c[3];
    out[1] = k0 * c[4]  + k1 * c[5]  + k2 * c[6]  + k3 * c[7];
    out[2] = k0 * c[8]  + k1 * c[9]  + k2 * c[10] + k3 * c[11];
    out[3] = k0 * c[12] + k1 * c[13] + k2 * c[14] + k3 * c[15];
}
static int M2_FrustumExtract(M2Frustum* f, const float* c) {
    M2_PlaneFromSelector(f->plane[0], c,  1.0f,  0.0f,  0.0f,  1.0f);  // w + x >= 0
    M2_PlaneFromSelector(f->plane[1], c, -1.0f,  0.0f,  0.0f,  1.0f);  // w - x >= 0
    M2_PlaneFromSelector(f->plane[2], c,  0.0f,  1.0f,  0.0f,  1.0f);  // w + y >= 0
    M2_PlaneFromSelector(f->plane[3], c,  0.0f, -1.0f,  0.0f,  1.0f);  // w - y >= 0
    M2_PlaneFromSelector(f->plane[4], c,  0.0f,  0.0f,  1.0f,  0.0f);  // z >= 0
    M2_PlaneFromSelector(f->plane[5], c,  0.0f,  0.0f, -1.0f,  1.0f);  // w - z >= 0
    for (int i = 0; i < 6; ++i) {
        float a = f->plane[i][0], b = f->plane[i][1], z = f->plane[i][2], d = f->plane[i][3];
        if (!M2_IsFinite(a) || !M2_IsFinite(b) || !M2_IsFinite(z) || !M2_IsFinite(d)) return 0;
        float len = M2_Sqrt(a * a + b * b + z * z);
        if (!(len > 1.0e-7f)) return 0;
        float inv = 1.0f / len;
        f->plane[i][0] = a * inv; f->plane[i][1] = b * inv; f->plane[i][2] = z * inv; f->plane[i][3] = d * inv;
    }
    return 1;
}
// Camera eye / forward for a view matrix already in the row-vector convention (v_view = v_world * V):
// V = [R | t] with R[i][j] = v[i*4+j] (i,j < 3), t = (v[12], v[13], v[14]) -> eye = -t * R^T, and the
// view-space +Z axis (the look direction) is the third column of R.
static void M2_EyeForward(const float* v, float* eye, float* fwd) {
    float tx = v[12], ty = v[13], tz = v[14];
    eye[0] = -(tx * v[0] + ty * v[4] + tz * v[8]);
    eye[1] = -(tx * v[1] + ty * v[5] + tz * v[9]);
    eye[2] = -(tx * v[2] + ty * v[6] + tz * v[10]);
    fwd[0] = v[2]; fwd[1] = v[6]; fwd[2] = v[10];
}
static float M2_PlaneDist(const float* p, const float* pt) {
    return p[0] * pt[0] + p[1] * pt[1] + p[2] * pt[2] + p[3];
}
static int M2_FrustumContainsPoint(const M2Frustum* f, float x, float y, float z, float slack) {
    for (int i = 0; i < 6; ++i) {
        const float* p = f->plane[i];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < -slack) return 0;
    }
    return 1;
}
// Seven independent geometric checks; a wrong convention almost never reaches M2_FRUSTUM_MIN_SCORE.
static int M2_FrustumScore(const M2Frustum* f, const float* proj, int convention) {
    int s = 1;  // (1) all six planes finite and normalised (M2_FrustumExtract guarantees this)
    float fl = M2_Sqrt(f->fwd[0] * f->fwd[0] + f->fwd[1] * f->fwd[1] + f->fwd[2] * f->fwd[2]);
    if (M2_IsFinite(fl) && fl > 0.9f && fl < 1.1f) s += 1;                          // (2) unit forward axis
    if (!M2_IsFinite(f->eye[0]) || !M2_IsFinite(f->eye[1]) || !M2_IsFinite(f->eye[2])) return 0;
    float p23 = (convention & 2) ? proj[3 * 4 + 2] : proj[2 * 4 + 3];
    float p33 = proj[3 * 4 + 3];
    if ((M2_Abs(p23 - 1.0f) < 1.0e-3f && M2_Abs(p33) < 1.0e-3f) ||
        M2_Abs(p33 - 1.0f) < 1.0e-3f) s += 1;                                       // (3) D3D projection shape
    const float* pn = f->plane[4];
    const float* pf = f->plane[5];
    float nearSigned = M2_PlaneDist(pn, f->eye);
    float farSigned  = M2_PlaneDist(pf, f->eye);
    float nearD = M2_Abs(nearSigned), farD = M2_Abs(farSigned);
    if (nearSigned <= 0.01f && farSigned >= -0.01f && nearD > 0.0f && farD > nearD) s += 1;  // (4) eye behind near, before far
    if (nearD > 0.0f && farD > nearD) {
        float d = nearD + 0.5f * (farD - nearD);                                    // (5) mid-frustum point inside
        float mx = f->eye[0] + f->fwd[0] * d, my = f->eye[1] + f->fwd[1] * d, mz = f->eye[2] + f->fwd[2] * d;
        if (M2_FrustumContainsPoint(f, mx, my, mz, 0.01f * (1.0f + d))) s += 1;
    } else if (M2_FrustumContainsPoint(f, f->eye[0], f->eye[1], f->eye[2], 0.0f)) {
        s += 1;                                                                     // ortho box containing the eye
    }
    {
        float bx = f->eye[0] - f->fwd[0], by = f->eye[1] - f->fwd[1], bz = f->eye[2] - f->fwd[2];
        if (!M2_FrustumContainsPoint(f, bx, by, bz, 0.0f)) s += 1;                   // (6) one unit behind the eye is outside
    }
    if (nearD > 0.0f && farD > nearD) {
        float d = farD * 3.0f + 1000.0f;                                            // (7) past the far plane is outside
        float fx = f->eye[0] + f->fwd[0] * d, fy = f->eye[1] + f->fwd[1] * d, fz = f->eye[2] + f->fwd[2] * d;
        if (!M2_FrustumContainsPoint(f, fx, fy, fz, 0.0f)) s += 1;
    }
    return s;
}
// Returns 1 when a convention passed the self-test (f->valid = 1), 0 when the frustum must stay invalid
// (fail open).  Best score wins; a tie keeps the lower convention value (deterministic).
static int M2_FrustumBuild(M2Frustum* f, const float* view, const float* proj) {
    M2Frustum best;
    int bestScore = -1;
    for (int conv = 0; conv < 4; ++conv) {
        float v[16], p[16], c[16];
        if (conv & 1) M2_Transpose16(v, view); else M2_Copy16(v, view);
        if (conv & 2) M2_Transpose16(p, proj); else M2_Copy16(p, proj);
        M2_Mul16(c, v, p);
        M2Frustum t;
        t.convention = conv; t.valid = 0; t.score = -1;
        t.nearDist = t.farDist = 0.0f;
        t.eye[0] = t.eye[1] = t.eye[2] = 0.0f;
        t.fwd[0] = t.fwd[1] = 0.0f; t.fwd[2] = 1.0f;
        if (!M2_FrustumExtract(&t, c)) continue;
        M2_EyeForward(v, t.eye, t.fwd);
        t.nearDist = M2_Abs(M2_PlaneDist(t.plane[4], t.eye));
        t.farDist  = M2_Abs(M2_PlaneDist(t.plane[5], t.eye));
        t.score = M2_FrustumScore(&t, proj, conv);
        if (t.score > bestScore) { bestScore = t.score; best = t; }
    }
    if (bestScore >= M2_FRUSTUM_MIN_SCORE) {
        *f = best;
        f->valid = 1;
        return 1;
    }
    for (int i = 0; i < 6; ++i) {
        f->plane[i][0] = f->plane[i][1] = f->plane[i][2] = f->plane[i][3] = 0.0f;
    }
    f->eye[0] = f->eye[1] = f->eye[2] = 0.0f;
    f->fwd[0] = 0.0f; f->fwd[1] = 0.0f; f->fwd[2] = 1.0f;
    f->nearDist = f->farDist = 0.0f;
    f->convention = -1;
    f->score = bestScore;
    f->valid = 0;
    return 0;
}
// Sphere vs frustum.  1 = visible (conservative), 0 = fully outside every plane.  Fails open on an
// invalid frustum or on NaN/huge input, so a broken capture can never hide content.
static int M2_FrustumSphereVisible(const M2Frustum* f, float x, float y, float z, float radius) {
    if (!f->valid) return 1;
    if (!(x == x) || !(y == y) || !(z == z) || !(radius == radius)) return 1;
    if (!M2_IsFinite(x) || !M2_IsFinite(y) || !M2_IsFinite(z)) return 1;
    float r = (radius > 0.0f) ? radius : 0.0f;
    if (r > 1.0e6f) return 1;
    for (int i = 0; i < 6; ++i) {
        const float* p = f->plane[i];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < -r) return 0;
    }
    return 1;
}
// ---- M2_FRUSTUM_CORE_END ----


// ---- Auto-start: run a console command once the main menu is up -------------------------
// Armed only if Z:\\tmp\\m2_autostart.txt exists (one console command per line, e.g. "m2LoadGame bench_heavy").
// xConsole::ExecString must run after evStartup (m2Startup.con runs too early and gets overridden).
typedef void (__thiscall *PFN_StrCtor)(void* self, const char* s);
typedef void (__thiscall *PFN_StrDtor)(void* self);
typedef void (__cdecl *PFN_ExecString)(const void* str, void* value);

// Reads the LAST cap-1 bytes if the file is bigger: the markers we wait for are at the end of a growing M2_Debug.log.
static bool ReadFileSmall(const char* path, char* buf, DWORD cap, DWORD* outLen) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    if (size != INVALID_FILE_SIZE && size > cap - 1) {
        SetFilePointer(h, -(LONG)(cap - 1), NULL, FILE_END);
    }
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, cap - 1, &n, NULL);
    CloseHandle(h);
    if (!ok) return false;
    buf[n] = 0;
    *outLen = n;
    return true;
}

static void LogLine(const char* msg);

// Wave 2 (particle culling): caller-evidence collector invoked from the draw hooks (defined with the
// rest of the Wave-2 section).  retaddr = __builtin_return_address(0) = the engine call site that issued
// the draw, which lands inside the particle paint function when particles are being drawn.
static void M2_PfUpEvidence(void* retaddr, unsigned int primCount, unsigned int stride);

static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive(void* pDevice, DWORD type, UINT startVertex, UINT primCount) {
    g_draws++; g_prims += primCount;
    if (g_r3MinDraws) g_r3Draws++;   // Wave 3: monotonic, telemetry-proof
    int64_t t0 = Qpc();
    HRESULT hr = g_origDrawPrimitive(pDevice, type, startVertex, primCount);
    Account(B_DRAW, t0);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(void* pDevice, DWORD type, INT baseVertex, UINT minIndex,
                                                           UINT numVertices, UINT startIndex, UINT primCount) {
    if (g_pfEvOn) M2_PfUpEvidence(__builtin_return_address(0), primCount, 0);
    g_draws++; g_prims += primCount;
    if (g_r3MinDraws) g_r3Draws++;   // Wave 3: monotonic, telemetry-proof
    int64_t t0 = Qpc();
    HRESULT hr = g_origDrawIndexedPrimitive(pDevice, type, baseVertex, minIndex, numVertices, startIndex, primCount);
    Account(B_DRAW, t0);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(void* pDevice, DWORD type, UINT primCount,
                                                      const void* vertexData, UINT stride) {
    if (g_pfEvOn) M2_PfUpEvidence(__builtin_return_address(0), primCount, stride);
    g_draws++; g_upDraws++; g_prims += primCount;
    if (g_r3MinDraws) g_r3Draws++;   // Wave 3: monotonic, telemetry-proof
    int64_t t0 = Qpc();
    HRESULT hr = g_origDrawPrimitiveUP(pDevice, type, primCount, vertexData, stride);
    Account(B_DRAW, t0);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP(void* pDevice, DWORD type, UINT minIndex, UINT numVertices,
                                                             UINT primCount, const void* indexData, DWORD indexFormat,
                                                             const void* vertexData, UINT stride) {
    g_draws++; g_upDraws++; g_prims += primCount;
    if (g_r3MinDraws) g_r3Draws++;   // Wave 3: monotonic, telemetry-proof
    int64_t t0 = Qpc();
    HRESULT hr = g_origDrawIndexedPrimitiveUP(pDevice, type, minIndex, numVertices, primCount, indexData, indexFormat,
                                              vertexData, stride);
    Account(B_DRAW, t0);
    return hr;
}

// Generic timed vtable hook: every hooked COM method is stdcall with 4-byte arguments, so forwarding them as
// DWORD-sized parameters is ABI-identical. Tag keeps instances for different vtables/slots distinct.
template <int Tag, int B, typename... A>
struct TimedHook {
    typedef HRESULT (STDMETHODCALLTYPE *Fn)(void*, A...);
    static Fn orig;
    static HRESULT STDMETHODCALLTYPE Hook(void* self, A... a) {
        int64_t t0 = Qpc();
        HRESULT hr = orig(self, a...);
        Account(B, t0);
        return hr;
    }
};
template <int Tag, int B, typename... A>
typename TimedHook<Tag, B, A...>::Fn TimedHook<Tag, B, A...>::orig = NULL;

static void PatchPtr(void** slot, void* value);

template <typename H>
static void HookSlot(void** vtbl, int slot) {
    if (!vtbl || vtbl[slot] == (void*)&H::Hook) return;
    H::orig = (typename H::Fn)vtbl[slot];
    PatchPtr(&vtbl[slot], (void*)&H::Hook);
}

typedef DWORD D;
typedef void* P;

// IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9: Lock [11](Offset, Size, void**, Flags), Unlock [12]
// IDirect3DTexture9: LockRect [19](Level, D3DLOCKED_RECT*, const RECT*, Flags), UnlockRect [20](Level)
static void* g_vbVtbl = NULL;
static void HookResourceVtbl(void* obj, int kind) {
    if (!obj) return;
    void** vtbl = *(void***)obj;
    if (kind == 0) {
        g_vbVtbl = vtbl;
        HookSlot<TimedHook<1011, B_LOCK, D, D, P, D> >(vtbl, 11);
        HookSlot<TimedHook<1012, B_LOCK> >(vtbl, 12);
    } else if (kind == 1) {
        if (vtbl == g_vbVtbl) return;  // shared vtable is already wrapped
        HookSlot<TimedHook<2011, B_LOCK, D, D, P, D> >(vtbl, 11);
        HookSlot<TimedHook<2012, B_LOCK> >(vtbl, 12);
    } else {
        HookSlot<TimedHook<3019, B_LOCK, D, P, P, D> >(vtbl, 19);
        HookSlot<TimedHook<3020, B_LOCK, D> >(vtbl, 20);
    }
}

// CreateTexture [23](W, H, Levels, Usage, Fmt, Pool, IDirect3DTexture9**, HANDLE*)
// CreateVertexBuffer [26] / CreateIndexBuffer [27](Length, Usage, FVF|Fmt, Pool, ppBuffer, HANDLE*)
typedef TimedHook<23, B_CREATE, D, D, D, D, D, D, P, P> HCreateTex;
typedef TimedHook<26, B_CREATE, D, D, D, D, P, P> HCreateVB;
typedef TimedHook<27, B_CREATE, D, D, D, D, P, P> HCreateIB;

static HRESULT STDMETHODCALLTYPE Hook_CreateTexture(void* dev, D w, D h, D lv, D us, D fmt, D pool, P pp, P sh) {
    HRESULT hr = HCreateTex::Hook(dev, w, h, lv, us, fmt, pool, pp, sh);
    if (SUCCEEDED(hr) && pp) HookResourceVtbl(*(void**)pp, 2);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateVertexBuffer(void* dev, D len, D us, D fvf, D pool, P pp, P sh) {
    HRESULT hr = HCreateVB::Hook(dev, len, us, fvf, pool, pp, sh);
    if (SUCCEEDED(hr) && pp) HookResourceVtbl(*(void**)pp, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateIndexBuffer(void* dev, D len, D us, D fmt, D pool, P pp, P sh) {
    HRESULT hr = HCreateIB::Hook(dev, len, us, fmt, pool, pp, sh);
    if (SUCCEEDED(hr) && pp) HookResourceVtbl(*(void**)pp, 1);
    return hr;
}

static void InstallSetTransformCapture(void** vtbl);  // Wave 1: frustum capture (defined below)

static void InstallDevicePhaseHooks(void** vtbl) {
    // state: cheap per-draw state setters
    // vtbl[44] SetTransform: Wave 1 replaces the generic timer with Hook_SetTransform, which keeps the
    // same B_STATE accounting and additionally captures D3DTS_VIEW / D3DTS_PROJECTION for the frustum.
    InstallSetTransformCapture(vtbl);
    HookSlot<TimedHook<47, B_STATE, P> >(vtbl, 47);              // SetViewport
    HookSlot<TimedHook<57, B_STATE, D, D> >(vtbl, 57);           // SetRenderState
    HookSlot<TimedHook<65, B_STATE, D, P> >(vtbl, 65);           // SetTexture
    HookSlot<TimedHook<67, B_STATE, D, D, D> >(vtbl, 67);        // SetTextureStageState
    HookSlot<TimedHook<69, B_STATE, D, D, D> >(vtbl, 69);        // SetSamplerState
    HookSlot<TimedHook<75, B_STATE, P> >(vtbl, 75);              // SetScissorRect
    HookSlot<TimedHook<87, B_STATE, P> >(vtbl, 87);              // SetVertexDeclaration
    HookSlot<TimedHook<89, B_STATE, D> >(vtbl, 89);              // SetFVF
    HookSlot<TimedHook<92, B_STATE, P> >(vtbl, 92);              // SetVertexShader
    HookSlot<TimedHook<94, B_STATE, D, P, D> >(vtbl, 94);        // SetVertexShaderConstantF
    HookSlot<TimedHook<100, B_STATE, D, P, D, D> >(vtbl, 100);   // SetStreamSource
    HookSlot<TimedHook<104, B_STATE, P> >(vtbl, 104);            // SetIndices
    HookSlot<TimedHook<107, B_STATE, P> >(vtbl, 107);            // SetPixelShader
    HookSlot<TimedHook<109, B_STATE, D, P, D> >(vtbl, 109);      // SetPixelShaderConstantF
    // misc: render-target switches, clears, copies and readbacks (potential GPU sync points)
    HookSlot<TimedHook<30, B_MISC, P, P, P, P> >(vtbl, 30);      // UpdateSurface
    HookSlot<TimedHook<31, B_MISC, P, P> >(vtbl, 31);            // UpdateTexture
    HookSlot<TimedHook<32, B_MISC, P, P> >(vtbl, 32);            // GetRenderTargetData
    HookSlot<TimedHook<34, B_MISC, P, P, P, P, D> >(vtbl, 34);   // StretchRect
    HookSlot<TimedHook<35, B_MISC, P, P, D> >(vtbl, 35);         // ColorFill
    HookSlot<TimedHook<37, B_MISC, D, P> >(vtbl, 37);            // SetRenderTarget
    HookSlot<TimedHook<39, B_MISC, P> >(vtbl, 39);               // SetDepthStencilSurface
    HookSlot<TimedHook<41, B_MISC> >(vtbl, 41);                  // BeginScene
    HookSlot<TimedHook<42, B_MISC> >(vtbl, 42);                  // EndScene
    HookSlot<TimedHook<43, B_MISC, D, P, D, D, D, D> >(vtbl, 43);// Clear (float Z passed as a 4-byte slot)
    // create: runtime resource creation; also wraps Lock/Unlock of every new buffer / texture vtable
    if (vtbl[23] != (void*)Hook_CreateTexture) { HCreateTex::orig = (HCreateTex::Fn)vtbl[23]; PatchPtr(&vtbl[23], (void*)Hook_CreateTexture); }
    if (vtbl[26] != (void*)Hook_CreateVertexBuffer) { HCreateVB::orig = (HCreateVB::Fn)vtbl[26]; PatchPtr(&vtbl[26], (void*)Hook_CreateVertexBuffer); }
    if (vtbl[27] != (void*)Hook_CreateIndexBuffer) { HCreateIB::orig = (HCreateIB::Fn)vtbl[27]; PatchPtr(&vtbl[27], (void*)Hook_CreateIndexBuffer); }
}

// (Splice 1: the pre-arena counting stubs moved into the M2_ARENA section below)
static void PatchPtr(void** slot, void* value) {
    DWORD oldProt = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        *slot = value;
        VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
    }
}

// Replaces every IAT slot of `mod` importing `dll!func` (by name via OriginalFirstThunk; falls back to matching the
// resolved address when the import has no name table). Returns the number of slots patched; *orig gets the old target.
static int PatchIAT(HMODULE mod, const char* dll, const char* func, void* hook, void** orig) {
    if (!mod) return -1;
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    HMODULE target = GetModuleHandleA(dll);
    void* resolved = target ? (void*)GetProcAddress(target, func) : NULL;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((const char*)(base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        IMAGE_THUNK_DATA* names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk) : NULL;
        for (int i = 0; iat[i].u1.Function; ++i) {
            bool match;
            if (names && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                match = strcmp((const char*)ibn->Name, func) == 0;
            } else {
                match = resolved && (void*)iat[i].u1.Function == resolved;
            }
            if (!match || (void*)iat[i].u1.Function == hook) continue;
            *orig = (void*)iat[i].u1.Function;
            PatchPtr((void**)&iat[i].u1.Function, hook);
            ++patched;
        }
    }
    return patched;
}

// (Splice 1: InstallAllocHooks is re-implemented at the end of the M2_ARENA section below,
//  after LogLine/PatchIAT and the arena core it depends on)


static void LogLine(const char* msg) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char line[512];
    int len = snprintf(line, sizeof(line), "# %s\n", msg);
    DWORD w = 0;
    if (len > 0) WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    FlushFileBuffers(g_logFile);
}

// ---- M2_ARENA_BEGIN ----
// Radical Splice 1 (t_m2_mimalloc_arena): lock-free small-block arena replacing MSVCR80 malloc/new
// for blocks <= 512 bytes out of Majesty2.exe and scene.dll — the ~3,700 allocs/frame heap churn
// that serialises every engine thread on the RtlAllocateHeap lock (24% CPU under Rosetta 2).
//
// Design (mimalloc-lite):
//   * 19 size classes over TOTAL block size (8-byte header + payload 8..512):
//     16,24,32,40,48,56,64,80,96,112,128,160,192,224,256,320,384,448,520.
//     Class lookup = 513-byte table, branch-free.
//   * Hot path is per-thread: TLS cache of free blocks per class (malloc = pop, free = push,
//     ZERO atomics).  One contiguous MEM_RESERVE region makes free() a single range compare;
//     the 8-byte header {magic, cls} at p-8 carries provenance.
//   * Cross-thread / overflow traffic goes through per-class global Treiber stacks spliced in
//     batches of 32 (one lock-free tagged CAS per batch): contention costs a cache-line ping,
//     never the RtlAllocateHeap futex wait.
//   * Fresh blocks come from per-thread 1 MiB bump chunks committed out of the reservation.
//     Chunks are kept for the session; past the cap (M2_ARENA_MAX_MB, default 64) malloc falls
//     back to the real MSVCR80.
//   * Foreign pointers (real MSVCR80 heap) reaching our free/realloc/_msize are forwarded to the
//     captured real functions, so the hooks are safe in modules that never arena-allocate.
//   * Blast radius: only Majesty2.exe and scene.dll allocations <= 512 B are arena-served, and
//     only when every alloc<->free pair they import is hooked.  The freeing family (free,
//     operator delete/delete[], realloc, _expand, _msize) is hooked in EVERY loaded module that
//     imports MSVCR80 so blocks handed across modules still free through the arena.  A late
//     sweep from Present covers DLLs mapped after device creation.
//   * M2_ARENA=0 disables serving (hooks stay as pure counters = previous build behaviour).

// ---- M2_ARENA_CORE_BEGIN ----  (dual-target: this slice is also compiled by the host stress test)
#define M2A_NCLS 19
#define M2A_CHUNK (1u << 20)          // 1 MiB bump chunk per allocating thread
#define M2A_LOCAL_MAX 64              // TLS list high-water before spilling to the global stack
#define M2A_BATCH 32                  // global-stack splice granularity
#define M2A_MAGIC 0x4D32              // '2M'

static const uint32_t g_arClsSize[M2A_NCLS] = {   // total block size (header incl.); payload = size-8
    16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 520
};
static uint8_t g_arClsTab[513];       // requested size 0..512 -> class index

struct M2AHdr { uint16_t magic; uint16_t cls; };

static uint8_t* g_arBase = NULL;      // arena reservation [g_arBase, g_arEnd) or NULL when off
static uint8_t* g_arEnd = NULL;
static volatile uint32_t g_arChunks = 0;   // committed 1 MiB chunks
static uint32_t g_arMaxChunks = 0;    // cap; past it M2A_Alloc falls back
static volatile uint32_t g_arSrv = 0, g_arFree = 0, g_arFb = 0, g_arFrn = 0, g_arBad = 0;

// Platform plumbing (assigned by the DLL init below / by the host test shim):
static void  (*M2A_ForeignFree)(void*) = NULL;
static void* (*M2A_ForeignRealloc)(void*, size_t) = NULL;
static size_t (*M2A_ForeignMsize)(void*) = NULL;
static void* (*M2A_RealMalloc)(size_t) = NULL;

// Per-class global recycle stack: Treiber stack with an ABA tag packed next to the head pointer
// in the widest atomic integer (x86 PE32: {ptr32,tag32} via lock cmpxchg8b; the host stress build
// on x86-64: {ptr64,tag64} via cmpxchg16b).  Free blocks chain through the first 4 bytes of the
// payload area; chains store USER pointers.
#if UINTPTR_MAX > 0xFFFFFFFFull
typedef unsigned __int128 M2AAtomic;
#else
typedef uint64_t M2AAtomic;
#endif
struct alignas(sizeof(M2AAtomic)) M2AStack { volatile M2AAtomic u; };
static M2AStack g_arGlob[M2A_NCLS];

static inline M2AAtomic M2A_Pack(void* p, uint64_t tag) {
    return (M2AAtomic)(((uint64_t)(uintptr_t)p) | ((M2AAtomic)tag << (sizeof(void*) * 8)));
}
static inline void* M2A_Ptr(M2AAtomic a) {
    return (void*)(uintptr_t)((uint64_t)a & (UINT64_MAX >> (64 - sizeof(void*) * 8)));
}
static inline uint64_t M2A_Tag(M2AAtomic a) { return (uint64_t)(a >> (sizeof(void*) * 8)); }

// Push chain [first..last] (blocks linked via *(void**)(blk+8)) onto class c.
// Lists store BLOCK pointers; the linkage lives in the payload's first 4 bytes so the header at
// blk+0 stays intact while the block sits on a stack/list.
#define M2A_NEXT(b) (*(void**)((uint8_t*)(b) + 8))
// The 4-byte header sits at block+0; user pointer = block+8 (blk[4..7] is padding).
static inline M2AHdr* M2A_Hdr(void* user) { return (M2AHdr*)((uint8_t*)user - 8); }

static void M2A_GlobPushChain(int c, void* first, void* last) {
    M2AStack* st = &g_arGlob[c];
    M2AAtomic cur = __atomic_load_n(&st->u, __ATOMIC_RELAXED);
    for (;;) {
        M2A_NEXT(last) = M2A_Ptr(cur);                       // tail -> old head
        M2AAtomic next = M2A_Pack(first, M2A_Tag(cur) + 1);
        if (__atomic_compare_exchange_n(&st->u, &cur, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return;
    }
}

// Pop up to `want` blocks off class c as a chain [first..last]; returns head block or NULL.
// Safety invariant: each iteration reads M2A_NEXT only of the CURRENT stack head.  If a racing
// popper wins first, the head block may already be user-owned and its next field garbage - but
// then our CAS fails (the winner bumped the tag) and the garbage value is never used.  No thread
// ever dereferences a next pointer of a node it does not own.
static void* M2A_GlobPopChain(int c, int want, void** lastOut, int* gotOut) {
    M2AStack* st = &g_arGlob[c];
    void* chainHead = NULL;
    void* chainTail = NULL;
    int got = 0;
    while (got < want) {
        M2AAtomic cur = __atomic_load_n(&st->u, __ATOMIC_RELAXED);
        void* h = M2A_Ptr(cur);
        if (!h) break;
        M2AAtomic next = M2A_Pack(M2A_NEXT(h), M2A_Tag(cur) + 1);
        if (__atomic_compare_exchange_n(&st->u, &cur, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (chainTail) M2A_NEXT(chainTail) = h;    // owned by us now: safe to link
            else chainHead = h;
            chainTail = h;
            ++got;
        }
    }
    *lastOut = chainTail;
    *gotOut = got;
    if (chainTail) M2A_NEXT(chainTail) = NULL;   // terminate: tail still pointed at the global
    return chainHead;                            // head it was popped from (we own it now)
}

struct M2AThreadCache {
    void*    head[M2A_NCLS];          // local free list (BLOCK pointers, chain via M2A_NEXT)
    uint16_t len[M2A_NCLS];
    uint8_t* bumpCur;                 // thread-private bump region inside the current chunk
    uint8_t* bumpEnd;
};

static inline int M2A_InArena(const void* p) {
    return (const uint8_t*)p >= g_arBase && (const uint8_t*)p < g_arEnd;
}

// ---- thread caches ----
static uint8_t* g_arTcSlab = NULL;           // dedicated 256 KiB slab of 256-byte cache cells
static volatile LONG g_arTcUsed = 0;
static DWORD g_arTls = 0xFFFFFFFF;           // == TLS_OUT_OF_INDEXES

static M2AThreadCache* M2A_TC(void) {
    M2AThreadCache* tc = (M2AThreadCache*)TlsGetValue(g_arTls);
    if (!tc && g_arTcSlab) {
        LONG slot = InterlockedIncrement(&g_arTcUsed);          // 1-based
        if ((uint32_t)slot <= (256u << 10) / 256u) {
            tc = (M2AThreadCache*)(g_arTcSlab + (size_t)(slot - 1) * 256u);
            memset(tc, 0, sizeof(M2AThreadCache));
            TlsSetValue(g_arTls, tc);
        }
    }
    return tc;
}

// Publish every local list of a dying thread to the global stacks (DllMain THREAD_DETACH).
// The 256-byte cache cell stays claimed for the process lifetime: nothing tracks cell reuse.
static void M2A_ThreadDetach(void) {
    M2AThreadCache* tc = (M2AThreadCache*)TlsGetValue(g_arTls);
    if (!tc) return;
    TlsSetValue(g_arTls, NULL);
    for (int c = 0; c < M2A_NCLS; ++c) {
        void* first = tc->head[c];
        if (!first) continue;
        void* t = first;
        for (int k = 1; k < M2A_LOCAL_MAX && M2A_NEXT(t); ++k) t = M2A_NEXT(t);
        M2A_GlobPushChain(c, first, t);
        tc->head[c] = NULL;
        tc->len[c] = 0;
    }
}

// ---- chunk claiming ----
static uint8_t* M2A_NewChunk(void) {
    uint32_t idx = (uint32_t)__sync_fetch_and_add((volatile int*)&g_arChunks, 1);
    if (idx >= g_arMaxChunks) { __sync_fetch_and_add((volatile int*)&g_arChunks, -1); return NULL; }
    uint8_t* chunk = g_arBase + (size_t)idx * M2A_CHUNK;
    if (!VirtualAlloc(chunk, M2A_CHUNK, MEM_COMMIT, PAGE_READWRITE)) {
        __sync_fetch_and_add((volatile int*)&g_arChunks, -1);
        return NULL;
    }
    return chunk;
}

static void M2A_InitClasses(void) {
    for (int n = 0; n <= 512; ++n) {
        int c = M2A_NCLS - 1;
        for (int k = 0; k < M2A_NCLS; ++k)
            if ((uint32_t)n + 8 <= g_arClsSize[k]) { c = k; break; }
        g_arClsTab[n] = (uint8_t)c;
    }
}

// ---- core ops (n <= 512 guaranteed by callers; NULL + *fb set => caller falls back) ----
static void* M2A_Alloc(unsigned n, uint32_t* fb) {
    M2AThreadCache* tc = M2A_TC();
    int c = g_arClsTab[n];
    if (!tc) {
        // no free cache cell left (1024 allocating threads): serve straight off the global
        // stack so the arena keeps working, only slower
        void* last; int got;
        void* blk = M2A_GlobPopChain(c, 1, &last, &got);
        if (!blk) { *fb = 1; return NULL; }
        M2AHdr* h = (M2AHdr*)blk;
        h->magic = M2A_MAGIC;
        h->cls = (uint16_t)c;
        return (uint8_t*)blk + 8;
    }
    uint32_t sz = g_arClsSize[c];

    void* blk = tc->head[c];
    if (blk) {
        tc->head[c] = M2A_NEXT(blk);
        tc->len[c]--;
    } else {
        void* last; int got;
        blk = M2A_GlobPopChain(c, M2A_BATCH, &last, &got);
        if (blk) {
            // The popped prefix [blk..last] is exclusively ours; the global head already points
            // PAST last (set by the pop CAS).  Keep [2nd..last] local, return blk itself.
            tc->head[c] = M2A_NEXT(blk);
            tc->len[c] = (uint16_t)(got - 1);
        } else if (tc->bumpCur && tc->bumpCur + sz <= tc->bumpEnd) {
            blk = tc->bumpCur;
            tc->bumpCur += sz;
        } else {
            uint8_t* chunk = M2A_NewChunk();
            if (chunk) {
                tc->bumpCur = chunk + sz;
                tc->bumpEnd = chunk + M2A_CHUNK;
                blk = chunk;
            } else {
                *fb = 1;                         // cap reached or commit failed
                return NULL;
            }
        }
    }
    M2AHdr* h = (M2AHdr*)blk;
    h->magic = M2A_MAGIC;
    h->cls = (uint16_t)c;
    return (uint8_t*)blk + 8;
}

static void M2A_Free(void* p) {
    if (!p) return;
    if (!M2A_InArena(p)) { g_arFrn++; if (M2A_ForeignFree) M2A_ForeignFree(p); return; }
    uint8_t* blk = (uint8_t*)p - 8;
    M2AHdr* h = (M2AHdr*)blk;
    if (h->magic != M2A_MAGIC || h->cls >= M2A_NCLS) {
        // A wild pointer inside our reservation: forwarding it would corrupt the real heap
        // (the game is already off the rails); count it and drop the block.
        g_arBad++;
        return;
    }
    int c = h->cls;
    M2AThreadCache* tc = M2A_TC();
    if (!tc) { M2A_GlobPushChain(c, blk, blk); return; }
    if (tc->len[c] >= M2A_LOCAL_MAX) {           // spill the oldest M2A_BATCH to the global stack
        void* first = tc->head[c];
        void* t = first;
        for (int k = 1; k < M2A_BATCH && M2A_NEXT(t); ++k) t = M2A_NEXT(t);
        tc->head[c] = M2A_NEXT(t);
        M2A_GlobPushChain(c, first, t);
        tc->len[c] = (uint16_t)(tc->len[c] - M2A_BATCH);
    }
    M2A_NEXT(blk) = tc->head[c];
    tc->head[c] = blk;
    tc->len[c]++;
}

static void* M2A_Realloc(void* p, size_t n) {
    if (!p) return NULL;
    if (n == 0) { M2A_Free(p); return NULL; }    // MSVC realloc(p,0) frees
    if (!M2A_InArena(p)) return M2A_ForeignRealloc ? M2A_ForeignRealloc(p, n) : NULL;
    M2AHdr* h = M2A_Hdr(p);
    if (h->magic != M2A_MAGIC || h->cls >= M2A_NCLS) { g_arBad++; return NULL; }
    size_t oldCap = g_arClsSize[h->cls] - 8;
    if (n <= oldCap) return p;                   // still fits: in-place, zero copy
    if (n <= 512) {
        uint32_t fb = 0;
        void* q = M2A_Alloc((unsigned)n, &fb);
        if (q) {
            memcpy(q, p, oldCap);                // copy the whole usable size, mimalloc-style
            M2A_Free(p);
            return q;
        }
    }
    void* q = M2A_RealMalloc ? M2A_RealMalloc(n) : NULL;   // grew past the arena: real heap
    if (q) { memcpy(q, p, oldCap); M2A_Free(p); }
    return q;
}

static size_t M2A_Msize(void* p) {
    if (p && M2A_InArena(p)) {
        M2AHdr* h = M2A_Hdr(p);
        if (h->magic == M2A_MAGIC && h->cls < M2A_NCLS) return g_arClsSize[h->cls] - 8;
    }
    return M2A_ForeignMsize ? M2A_ForeignMsize(p) : 0;
}
// ---- M2_ARENA_CORE_END ----

// ---- Windows layer: real CRT capture, arena init, hook bodies, IAT installation ----
#include <tlhelp32.h>

typedef void*  (__cdecl *PFN_c_malloc)(size_t);
typedef void   (__cdecl *PFN_c_free)(void*);
typedef void*  (__cdecl *PFN_c_calloc)(size_t, size_t);
typedef void*  (__cdecl *PFN_c_realloc)(void*, size_t);
typedef void*  (__cdecl *PFN_c_expand)(void*, size_t);
typedef size_t (__cdecl *PFN_c_msize)(void*);
static PFN_c_malloc  g_realMalloc = NULL, g_realNew = NULL, g_realNewArr = NULL;
static PFN_c_free    g_realFree = NULL, g_realDel = NULL, g_realDelArr = NULL;
static PFN_c_calloc  g_realCalloc = NULL;
static PFN_c_realloc g_realRealloc = NULL, g_realExpand = NULL;
static PFN_c_msize   g_realMsize = NULL;

// Serving flags (computed once; zero on the hot path = plain passthrough).
static volatile uint32_t g_arOn = 0;        // arena initialised and M2_ARENA != 0
static volatile uint32_t g_arServeExe = 0;  // Majesty2.exe malloc/calloc/new/new[] may serve
static volatile uint32_t g_arServeScn = 0;  // scene.dll malloc/calloc/new/new[] may serve

static void M2ArenaInit(void) {
    static bool captured = false;   // real MSVCR80 entry points resolved (set on success only)
    static bool reserved = false;   // memory plumbing attempted exactly once
    if (captured) return;

    HMODULE crt = GetModuleHandleA("MSVCR80.dll");
    if (crt) {
        g_realMalloc  = (PFN_c_malloc) GetProcAddress(crt, "malloc");
        g_realFree    = (PFN_c_free)   GetProcAddress(crt, "free");
        g_realCalloc  = (PFN_c_calloc) GetProcAddress(crt, "calloc");
        g_realRealloc = (PFN_c_realloc)GetProcAddress(crt, "realloc");
        g_realExpand  = (PFN_c_expand) GetProcAddress(crt, "_expand");
        g_realMsize   = (PFN_c_msize)  GetProcAddress(crt, "_msize");
        g_realNew     = (PFN_c_malloc) GetProcAddress(crt, "??2@YAPAXI@Z");
        g_realDel     = (PFN_c_free)   GetProcAddress(crt, "??3@YAXPAX@Z");
        g_realNewArr  = (PFN_c_malloc) GetProcAddress(crt, "??_U@YAPAXI@Z");
        g_realDelArr  = (PFN_c_free)   GetProcAddress(crt, "??_V@YAXPAX@Z");
    }
    if (!g_realMalloc || !g_realFree) return;   // MSVCR80 not mapped yet: retry on a later call
    captured = true;
    M2A_InitClasses();
    M2A_ForeignFree    = (void (*)(void*))g_realFree;
    M2A_ForeignRealloc = (void* (*)(void*, size_t))g_realRealloc;
    M2A_ForeignMsize   = (size_t (*)(void*))g_realMsize;
    M2A_RealMalloc     = (void* (*)(size_t))g_realMalloc;
    if (reserved) return;
    reserved = true;

    char ev[16] = {0};
    if (GetEnvironmentVariableA("M2_ARENA", ev, sizeof(ev)) > 0 && ev[0] == '0') {
        LogLine("arena: disabled by M2_ARENA=0 (hooks stay installed as counters)");
        return;
    }
    ev[0] = 0;
    DWORD mb = 64;
    if (GetEnvironmentVariableA("M2_ARENA_MAX_MB", ev, sizeof(ev)) > 0 && ev[0] >= '0' && ev[0] <= '9')
        mb = (DWORD)atoi(ev);
    if (mb < 4) mb = 4;
    if (mb > 1024) mb = 1024;

    // One contiguous reservation => free() is a single range compare.  Try shrinking sizes so a
    // fragmented 32-bit address space still gets a working arena.
    uint8_t* raw = NULL;
    for (DWORD want = mb; want >= 4 && !raw; want /= 2) {
        raw = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)want << 20, MEM_RESERVE, PAGE_NOACCESS);
        if (raw) g_arMaxChunks = want;
    }
    if (!raw) { LogLine("arena: MEM_RESERVE failed, arena off (passthrough)"); return; }
    g_arBase = raw;
    g_arEnd = raw + ((SIZE_T)g_arMaxChunks << 20);

    g_arTcSlab = (uint8_t*)VirtualAlloc(NULL, 256 << 10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_arTcSlab) { g_arBase = g_arEnd = NULL; LogLine("arena: TLS slab failed, arena off"); return; }
    g_arTls = TlsAlloc();
    if (g_arTls == TLS_OUT_OF_INDEXES) {
        g_arBase = g_arEnd = NULL;
        LogLine("arena: TlsAlloc failed, arena off");
        return;
    }
    g_arOn = 1;
    char m[160];
    snprintf(m, sizeof(m), "arena: on reserve=%uMB max_chunks=%u classes=%d top_block=%uB payload<=%u",
             g_arMaxChunks, g_arMaxChunks, M2A_NCLS, g_arClsSize[M2A_NCLS - 1],
             g_arClsSize[M2A_NCLS - 1] - 8);
    LogLine(m);
}

// ---- hook bodies ----
// g_allocExe / g_allocScene / g_allocRender keep their pre-arena meaning (hooked calls per
// second); arena counters (g_arSrv etc.) are reported by the telemetry extension below.
static void* __cdecl Ar_MallocExe(size_t n) {
    g_allocExe++;
    if (n <= 512 && g_arOn && g_arServeExe) {
        uint32_t fb = 0;
        void* p = M2A_Alloc((unsigned)n, &fb);
        if (!fb) { g_arSrv++; return p; }
        g_arFb++;
    }
    return g_realMalloc(n);
}

static void* __cdecl Ar_MallocScene(size_t n) {
    g_allocScene++;
    if (n <= 512 && g_arOn && g_arServeScn) {
        uint32_t fb = 0;
        void* p = M2A_Alloc((unsigned)n, &fb);
        if (!fb) { g_arSrv++; return p; }
        g_arFb++;
    }
    return g_realMalloc(n);
}

static void* __cdecl Ar_MallocRender(size_t n) { g_allocRender++; return g_realMalloc(n); }

static void* __cdecl Ar_CallocExe(size_t n, size_t el) {
    g_allocExe++;
    size_t bytes = n * el;
    if (n && el && bytes / n != el) return NULL;
    if (bytes <= 512 && g_arOn && g_arServeExe) {
        uint32_t fb = 0;
        void* p = M2A_Alloc((unsigned)bytes, &fb);
        if (!fb) { g_arSrv++; memset(p, 0, bytes); return p; }
        g_arFb++;
    }
    return g_realCalloc(n, el);
}

static void* __cdecl Ar_CallocScene(size_t n, size_t el) {
    g_allocScene++;
    size_t bytes = n * el;
    if (n && el && bytes / n != el) return NULL;
    if (bytes <= 512 && g_arOn && g_arServeScn) {
        uint32_t fb = 0;
        void* p = M2A_Alloc((unsigned)bytes, &fb);
        if (!fb) { g_arSrv++; memset(p, 0, bytes); return p; }
        g_arFb++;
    }
    return g_realCalloc(n, el);
}

static void* __cdecl Ar_NewExe(size_t n) {
    g_allocExe++;
    if (n <= 512 && g_arOn && g_arServeExe) {
        uint32_t fb = 0;
        void* p = M2A_Alloc((unsigned)n, &fb);
        if (!fb) { g_arSrv++; return p; }
        g_arFb++;
    }
    return g_realNew(n);
}

static void* __cdecl Ar_NewScene(size_t n) {
    g_allocScene++;
    if (n <= 512 && g_arOn && g_arServeScn) {
        uint32_t fb = 0;
        void* p = M2A_Alloc((unsigned)n, &fb);
        if (!fb) { g_arSrv++; return p; }
        g_arFb++;
    }
    return g_realNew(n);
}

static void* __cdecl Ar_NewArrExe(size_t n)   { return Ar_NewExe(n); }
static void* __cdecl Ar_NewArrScene(size_t n) { return Ar_NewScene(n); }

// Freeing family: one implementation for every module (arena block or foreign pointer alike).
static void   __cdecl Ar_Free(void* p)           { g_arFree++; M2A_Free(p); }
static void   __cdecl Ar_Del(void* p)            { g_arFree++; M2A_Free(p); }
static void   __cdecl Ar_DelArr(void* p)         { g_arFree++; M2A_Free(p); }
static void*  __cdecl Ar_Realloc(void* p, size_t n) { return M2A_Realloc(p, n); }
static size_t __cdecl Ar_Msize(void* p)          { return M2A_Msize(p); }
static void*  __cdecl Ar_Expand(void* p, size_t n) {
    if (p && M2A_InArena(p)) return M2A_Realloc(p, n);
    return g_realExpand ? g_realExpand(p, n) : NULL;
}

static LPVOID WINAPI Ar_HeapAllocExe(HANDLE h, DWORD f, SIZE_T n) { g_allocExe++; return HeapAlloc(h, f, n); }

// ---- IAT installation ----
struct Ar_Mod { HMODULE h; char name[24]; };
static Ar_Mod g_arMods[160];
static int g_arNMods = 0;
static DWORD g_arSweepUntil = 0, g_arSweepNext = 0;

static int Ar_Known(HMODULE h) {
    for (int i = 0; i < g_arNMods; ++i)
        if (g_arMods[i].h == h) return 1;
    return 0;
}

// origin: 1 = Majesty2.exe, 2 = scene.dll (arena-served), 0 = other module (free family only)
static void Ar_InstallForModule(HMODULE mod, const char* name, int origin) {
    if (!mod) return;
    if (Ar_Known(mod)) return;
    if (g_arNMods < 160) {
        g_arMods[g_arNMods].h = mod;
        snprintf(g_arMods[g_arNMods].name, sizeof(g_arMods[0].name), "%.23s", name);
        ++g_arNMods;
    }
    void* o;
    void* mfn  = (origin == 1) ? (void*)Ar_MallocExe   : (void*)Ar_MallocScene;
    void* cfn  = (origin == 1) ? (void*)Ar_CallocExe   : (void*)Ar_CallocScene;
    void* nfn  = (origin == 1) ? (void*)Ar_NewExe      : (void*)Ar_NewScene;
    void* nafn = (origin == 1) ? (void*)Ar_NewArrExe   : (void*)Ar_NewArrScene;
    int pMall = origin ? PatchIAT(mod, "MSVCR80.dll", "malloc", mfn, &o) : 0;
    int pCall = origin ? PatchIAT(mod, "MSVCR80.dll", "calloc", cfn, &o) : 0;
    int pNew  = origin ? PatchIAT(mod, "MSVCR80.dll", "??2@YAPAXI@Z", nfn, &o) : 0;
    int pNewA = origin ? PatchIAT(mod, "MSVCR80.dll", "??_U@YAPAXI@Z", nafn, &o) : 0;
    int pFree = PatchIAT(mod, "MSVCR80.dll", "free", (void*)Ar_Free, &o);
    int pDel  = PatchIAT(mod, "MSVCR80.dll", "??3@YAXPAX@Z", (void*)Ar_Del, &o);
    int pDelA = PatchIAT(mod, "MSVCR80.dll", "??_V@YAXPAX@Z", (void*)Ar_DelArr, &o);
    int pRe   = PatchIAT(mod, "MSVCR80.dll", "realloc", (void*)Ar_Realloc, &o);
    int pEx   = PatchIAT(mod, "MSVCR80.dll", "_expand", (void*)Ar_Expand, &o);
    int pMs   = PatchIAT(mod, "MSVCR80.dll", "_msize", (void*)Ar_Msize, &o);

    if (origin) {
        // Arena serving demands every alloc<->free pair this module imports to be hooked:
        // an unhooked pair would feed arena blocks to the real MSVCR80.
        int ok = 1;
        if ((pMall > 0 || pCall > 0) && pFree <= 0) ok = 0;
        if (pNew > 0 && pDel <= 0) ok = 0;
        if (pNewA > 0 && pDelA <= 0) ok = 0;
        if (pRe > 0 && pFree <= 0) ok = 0;
        if (origin == 1) g_arServeExe = ok;
        if (origin == 2) g_arServeScn = ok;
        char m[256];
        snprintf(m, sizeof(m), "arena: %-12s serve=%d malloc=%d calloc=%d new=%d new[]=%d free=%d "
                 "del=%d del[]=%d realloc=%d expand=%d msize=%d", name, ok, pMall, pCall, pNew,
                 pNewA, pFree, pDel, pDelA, pRe, pEx, pMs);
        LogLine(m);
    } else if (pFree + pDel + pDelA + pRe + pEx + pMs > 0) {
        char m[160];
        snprintf(m, sizeof(m), "arena: %-16s free-family hooks free=%d del=%d del[]=%d realloc=%d "
                 "expand=%d msize=%d", name, pFree, pDel, pDelA, pRe, pEx, pMs);
        LogLine(m);
    }
}

static int Ar_SweepOtherModules(void) {
    // Free/realloc family for every OTHER loaded module importing MSVCR80 (cross-module frees of
    // arena blocks must find our handlers).  Returns the number of newly patched modules.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    HMODULE exe = GetModuleHandleA(NULL);
    HMODULE scn = GetModuleHandleA("scene.dll");
    HMODULE self = GetModuleHandleA("d3d9.dll");
    HMODULE dxvk = GetModuleHandleA("d3d9_dxvk.dll");
    HMODULE crt = GetModuleHandleA("MSVCR80.dll");
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int added = 0;
    if (Module32First(snap, &me)) {
        do {
            HMODULE h = (HMODULE)me.modBaseAddr;
            if (h == exe || h == scn || h == self || h == dxvk || h == crt) continue;
            if (Ar_Known(h)) continue;
            Ar_InstallForModule(h, me.szModule, 0);
            ++added;
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return added;
}

static void InstallAllocHooks(void) {
    static bool done = false;
    if (done) return;
    M2ArenaInit();
    if (!g_realMalloc || !g_realFree) return;   // MSVCR80 not mapped yet: retry on a later call
    done = true;
    void* o;
    PatchIAT(GetModuleHandleA(NULL), "kernel32.dll", "HeapAlloc", (void*)Ar_HeapAllocExe, &o);
    Ar_InstallForModule(GetModuleHandleA(NULL), "Majesty2.exe", 1);
    Ar_InstallForModule(GetModuleHandleA("scene.dll"), "scene.dll", 2);
    // render.dll: no arena serving (task scope), keep the old malloc counter + free family hooks.
    HMODULE rnd = GetModuleHandleA("render.dll");
    Ar_InstallForModule(rnd, "render.dll", 0);
    PatchIAT(rnd, "MSVCR80.dll", "malloc", (void*)Ar_MallocRender, &o);
    int swept = Ar_SweepOtherModules();
    g_arSweepUntil = GetTickCount() + 120000;   // keep sweeping for late DLL loads for 2 minutes
    g_arSweepNext = 0;
    char m[96];
    snprintf(m, sizeof(m), "arena: install complete, other-modules swept=%d serve_exe=%u serve_scn=%u",
             swept, g_arServeExe, g_arServeScn);
    LogLine(m);
}

// Late DLL loads: re-sweep every 5 s while the 2-minute window is open (called from Present).
static void M2ArenaLateSweep(void) {
    if (!g_arSweepUntil) return;
    DWORD now = GetTickCount();
    if ((LONG)(now - g_arSweepNext) < 0) return;
    g_arSweepNext = now + 5000;
    int added = Ar_SweepOtherModules();
    if (added > 0) {
        char m[96];
        snprintf(m, sizeof(m), "arena: late sweep +%d module(s)", added);
        LogLine(m);
    }
    if ((LONG)(now - g_arSweepUntil) >= 0) {
        g_arSweepUntil = 0;
        LogLine("arena: late sweep window closed");
    }
}

// Telemetry extension: per-second snapshot + reset of the arena counters (read by Hook_Present).
static void Ar_TelemetryReset(uint32_t* srv, uint32_t* fre, uint32_t* fb, uint32_t* frn,
                              uint32_t* bad, uint32_t* chunks) {
    *srv = g_arSrv; *fre = g_arFree; *fb = g_arFb; *frn = g_arFrn; *bad = g_arBad;
    *chunks = g_arChunks;
    g_arSrv = 0; g_arFree = 0; g_arFb = 0; g_arFrn = 0; g_arBad = 0;
}
// ---- M2_ARENA_END ----

// Base + SizeOfImage of the game modules, to map raw addresses from /usr/bin/sample back to RVAs.
static void LogModules(const char* tag) {
    static const char* const names[] = {"Majesty2.exe", "scene.dll", "render.dll", "pf.dll", "geometry.dll",
                                        "features.dll", "script.dll", "base.dll", "gembase.dll", "d3d9.dll",
                                        "d3d9_dxvk.dll", "MSVCR80.dll", "MSVCP80.dll", "d3dx9_42.dll",
                                        "winevulkan.dll", "ntdll.dll", "kernelbase.dll"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        HMODULE h = i == 0 ? GetModuleHandleA(NULL) : GetModuleHandleA(names[i]);
        char m[200];
        if (h) {
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)h + ((IMAGE_DOS_HEADER*)h)->e_lfanew);
            snprintf(m, sizeof(m), "module[%s] %-16s base=%08lX end=%08lX", tag, names[i], (unsigned long)(DWORD_PTR)h,
                     (unsigned long)((DWORD_PTR)h + nt->OptionalHeader.SizeOfImage));
        } else {
            snprintf(m, sizeof(m), "module[%s] %-16s not loaded", tag, names[i]);
        }
        LogLine(m);
    }
}

// Unix env as seen inside the Windows process (Sikarugir may override what the bench passes).
static void LogEnv(void) {
    static const char* const keys[] = {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "DXVK_ASYNC", "DXVK_HUD",
                                       "DXVK_SHADER_CACHE_PATH", "MTL_HUD_ENABLED", "WINEMSYNC", "WINEESYNC",
                                       "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "MVK_CONFIG_FAST_MATH_ENABLED",
                                       "ROSETTA_ADVERTISE_AVX", "WINEDLLOVERRIDES", "M2_CLEAR_MT"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        char v[300] = {0};
        DWORD n = GetEnvironmentVariableA(keys[i], v, sizeof(v));
        char m[400];
        snprintf(m, sizeof(m), "env %s=%s", keys[i], n ? v : "<unset>");
        LogLine(m);
    }
}

// xScene::eQuantControl (scene.dll): the real simulation speed. Speed() returns the top entry of the BeginSpeed
// stack ([+0x2c]-0x10, 1.0 when empty); SpeedScale() is [+0x44]. Only valid once a level is up.
typedef void* (__cdecl *PFN_QcGet)(void);
typedef float (__thiscall *PFN_QcFloat)(void* self);
static PFN_QcGet g_qcGet = NULL;
static PFN_QcFloat g_qcSpeed = NULL, g_qcSpeedScale = NULL;
static bool g_levelUp = false;
static LARGE_INTEGER g_levelUpTick = {0};  // QPC when SecondUpdate was seen
static LARGE_INTEGER g_speed5Tick = {0};   // QPC when 5x SpeedScale actually became active

static bool QuantSpeed(float* speed, float* scale) {
    if (!g_qcGet) {
        HMODULE sc = GetModuleHandleA("scene.dll");
        if (!sc) return false;
        g_qcGet = (PFN_QcGet)GetProcAddress(sc, "?Get@eQuantControl@xScene@@SAAAV12@XZ");
        g_qcSpeed = (PFN_QcFloat)GetProcAddress(sc, "?Speed@eQuantControl@xScene@@QBEMXZ");
        g_qcSpeedScale = (PFN_QcFloat)GetProcAddress(sc, "?SpeedScale@eQuantControl@xScene@@QBEMXZ");
        if (!g_qcGet || !g_qcSpeed || !g_qcSpeedScale) { g_qcGet = NULL; return false; }
    }
    void* qc = g_qcGet();
    if (!qc) return false;
    *speed = g_qcSpeed(qc);
    *scale = g_qcSpeedScale(qc);
    return true;
}

// Majesty2.exe (image base 0x400000, no relocation) state graph, reversed from the GameStateMain / SingleGame ctors
// (0x5a06c0 / 0x59d560, AddTransition = 0x7f0910):
//   MainMenu --evSingleGame 0x6ad05e--> SingleGame{entry 0x454350}
//            --evLoadSelectedMission 0x6ddd16 (data = xScene::eLocation)--> GameStateLoadLevel (vtbl 0x88763c, OnActivate 0x455080)
// The entry state also turns 0x6ddd18(eLocation) + 0x6ddd14 into 0x6ddd16 (what the UI "Start" button does).
// m2LoadLevel / 0x6ad08a only reaches OnConsoleLoadLevel (0x452790): eManager Prepare+Process, no GameStateLoadLevel.
#define EV_SINGLE_GAME        0x6ad05e
#define EV_LOAD_SEL_MISSION   0x6ddd16
#define EV_SELECT_LOCATION    0x6ddd18
#define EV_START_SELECTED     0x6ddd14
// Saved game (what the load-game window does at 0x6166ba):
//   MainMenu --evLoadCampaignDlg 0x6ad037--> Campaign/load dialog
//            --evLoadSelectedGame 0x6ddd1a (data = {SaveInfo*})--> GameStateLoadLevel (0x4551d7 asserts data[0] != 0)
//   GameStateLoadLevel then runs SaveManager::Load 0x4e0db0(SaveInfo) from its 0x6ad000 handler (0x454e20).
// SaveManager = [App()+0x1c] + 0xaacd0 (App() = 0x4010d0): vector<SaveInfo> begin [+4] / end [+8], stride 0xbc,
// SaveInfo +0x14 = bool campaign flag, +0x18 = std::string folder name. Clear 0x4e0c40 / rescan 0x4e0c70 (thiscall).
// Bare m2LoadGame (0x4abde0) calls 0x4e0db0 directly from the menu, bypassing GameStateLoadLevel.
#define EV_LOAD_CAMPAIGN_DLG  0x6ad037
#define EV_LOAD_SELECTED_GAME 0x6ddd1a
#define FN_APP                0x004010d0
#define FN_SAVES_CLEAR        0x004e0c40
#define FN_SAVES_SCAN         0x004e0c70
#define SAVEMGR_OFF           0x000aacd0
#define SAVEINFO_SIZE         0xbc
// Game speed (m2SetGameSpeed atom 0x6e7e90): GameSpeedSwitcher = [UiSingleton(0xacf2d8)+0xb0],
// SetSpeed(float) thiscall 0x5cb880: [sw+8] = v; eQuantControl::BeginSpeed(eId=sw, v, 1.0); 0x5cb6e0(v, 0) shows the
// "Nx" COUNTER_game_speed control via 0x5cb540, which ends with `fld1; fstp [ebp+8]` (0x5cb62e) -> [sw+8] is reset
// to 1.0 right away. [sw+8] is therefore NOT the current speed: read eQuantControl::Speed() instead (QuantSpeed).
#define UI_SINGLETON          0x00acf2d8
#define FN_SET_GAME_SPEED     0x005cb880
#define IAT_EVENT_CTOR        0x0087cccc  // base.dll  eEvent::eEvent(ulong, const eEventData*)
#define IAT_EVENT_SEND        0x0087ccd8  // base.dll  eEvent::Send()
#define IAT_LOCATION_CTOR     0x0087dd44  // scene.dll xScene::eLocation::eLocation(const std::string&)
#define IAT_LOCATION_DTOR     0x0087dd48  // scene.dll xScene::eLocation::~eLocation()

typedef void (__thiscall *PFN_EventCtor)(void* self, unsigned long id, const void* data);
typedef void (__thiscall *PFN_EventSend)(void* self);
typedef void (__thiscall *PFN_LocCtor)(void* self, const void* str);
typedef void (__thiscall *PFN_LocDtor)(void* self);

static PFN_StrCtor g_strCtor = NULL;
static PFN_StrDtor g_strDtor = NULL;

// eEvent is 8 bytes {id, data*} and has no destructor; data must outlive Send() (Send dispatches synchronously).
static void SendEvent(unsigned long id, const void* data) {
    alignas(8) unsigned char ev[16] = {0};
    (*(PFN_EventCtor*)IAT_EVENT_CTOR)(ev, id, data);
    (*(PFN_EventSend*)IAT_EVENT_SEND)(ev);
}

// Sends `id` with an xScene::eLocation built from "map_dir:map_name" (sizeof(eLocation) == 0x54).
static void SendLocationEvent(unsigned long id, const char* location) {
    alignas(8) char str[64] = {0};          // VC8 std::string is 28 bytes
    alignas(8) unsigned char loc[0x100] = {0};
    g_strCtor(str, location);
    (*(PFN_LocCtor*)IAT_LOCATION_CTOR)(loc, str);
    g_strDtor(str);
    SendEvent(id, loc);
    (*(PFN_LocDtor*)IAT_LOCATION_DTOR)(loc);
}

// VC8 std::string (28 bytes): allocator at +0, 16-byte SSO buffer / heap pointer at +4, size +0x14, capacity +0x18.
static const char* VcStr(const unsigned char* s) {
    return *(const DWORD*)(s + 0x18) >= 16 ? *(const char* const*)(s + 4) : (const char*)(s + 4);
}

static unsigned char* SaveManager(void) {
    typedef unsigned char* (__cdecl *PFN_App)(void);
    unsigned char* app = ((PFN_App)FN_APP)();
    unsigned char* owner = app ? *(unsigned char**)(app + 0x1c) : NULL;
    return owner ? owner + SAVEMGR_OFF : NULL;
}

// Finds SaveInfo by folder name, preferring the given campaign flag. *count gets the list size.
static unsigned char* FindSave(unsigned char* mgr, const char* name, bool campaign, int* count) {
    unsigned char* b = *(unsigned char**)(mgr + 4);
    unsigned char* e = *(unsigned char**)(mgr + 8);
    unsigned char* any = NULL;
    *count = b ? (int)((e - b) / SAVEINFO_SIZE) : 0;
    for (unsigned char* p = b; p && p < e; p += SAVEINFO_SIZE) {
        if (_stricmp(VcStr(p + 0x18), name) != 0) continue;
        if ((p[0x14] != 0) == campaign) return p;
        if (!any) any = p;
    }
    return any;
}

static void* SpeedSwitcher(void) {
    unsigned char* ui = *(unsigned char**)UI_SINGLETON;  // never call 0x628860: it constructs the UI when absent
    return ui ? *(void**)(ui + 0xb0) : NULL;
}

static bool DebugLogHas(const char* needle) {
    static char log[262144];
    DWORD len = 0;
    if (!ReadFileSmall("Z:\\Users\\developer\\Documents\\majesty2\\M2_Debug.log", log, sizeof(log), &len)) return false;
    return strstr(log, needle) != NULL;
}

static void AutoStartTick(const LARGE_INTEGER& now) {
    // 0 = waiting for Application::Init, 1 = init seen (settle 2 s), 2 = evSingleGame sent,
    // 3 = mission launch sent (waiting for BEGIN LOADING LEVEL),
    // 4 = loading level (auto-click LoadingScreenWindow::StartGameProcess when [lsw+0x50] != 0),
    // 5 = level loaded (auto-close Advisor / M2MessageBox via 0x5de7b0/0x5de7e0 for 20 s), 6 = done,
    // 7 = evLoadCampaignDlg sent (select SaveInfo, send evLoadSelectedGame), 8 = keep game speed at SPEED:
    static int state = 0;
    static int attempts = 0;
    static LARGE_INTEGER nextPoll = {0}, stamp = {0};
    static char buf[2048];          // whole m2_autostart.txt
    static char cmds[8][256];       // one console command per non-empty line
    static int nCmds = 0;
    static char level[256] = {0};   // "Single_missions/PP03:pp03" from a "LEVEL:" line
    static char save[128] = {0};    // saves folder name from a "SAVE:" line, e.g. "bench_heavy"
    static float speed = 0.0f;      // game speed from a "SPEED:" line (5 = 5x), enforced after the level is up
    static LARGE_INTEGER nextSpeed = {0};
    if (state == 6) return;
    if (now.QuadPart < nextPoll.QuadPart) return;
    nextPoll.QuadPart = now.QuadPart + g_qpcFreq.QuadPart / 4; // poll every 250 ms
    LONGLONG since = (now.QuadPart - stamp.QuadPart) / g_qpcFreq.QuadPart;

    if (state == 0) {
        DWORD n = 0;
        if (!ReadFileSmall("Z:\\tmp\\m2_autostart.txt", buf, sizeof(buf), &n)) { state = 6; return; }
        nCmds = 0;
        char* line = buf;
        while (nCmds < 8) {
            char* e = line;
            while (*e && *e != '\r' && *e != '\n') ++e;
            int len = (int)(e - line);
            if (len > 0) {
                if (len > (int)sizeof(cmds[0]) - 1) len = sizeof(cmds[0]) - 1;
                memcpy(cmds[nCmds], line, len);
                cmds[nCmds][len] = 0;
                ++nCmds;
            }
            if (!*e) break;
            line = e + 1;
        }
        if (nCmds == 0) { state = 6; return; }
        if (!DebugLogHas("Application::Init() started in")) return;
        state = 1; stamp = now;
        return;
    }

    if (state == 1) {
        if (since < 2) return;      // let the main menu / evStartup settle
        HMODULE gb = GetModuleHandleA("gembase.dll");
        HMODULE cp = GetModuleHandleA("MSVCP80.dll");
        if (!gb || !cp) { LogLine("autostart: gembase/msvcp80 not loaded"); state = 6; return; }
        auto exec = (PFN_ExecString)GetProcAddress(gb, "?ExecString@xConsole@@YAXABVstring@@PAVeValue@xDda@@@Z");
        g_strCtor = (PFN_StrCtor)GetProcAddress(cp, "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z");
        g_strDtor = (PFN_StrDtor)GetProcAddress(cp, "??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ");
        if (!exec || !g_strCtor || !g_strDtor) { LogLine("autostart: export lookup failed"); state = 6; return; }
        for (int i = 0; i < nCmds; ++i) {
            if (strncmp(cmds[i], "SAVE:", 5) == 0) {
                strncpy(save, cmds[i] + 5, sizeof(save) - 1);
                continue;
            }
            if (strncmp(cmds[i], "SPEED:", 6) == 0) {
                speed = (float)atof(cmds[i] + 6);
                continue;
            }
            if (strncmp(cmds[i], "LEVEL:", 6) == 0) {
                const char* raw = cmds[i] + 6;
                // GameStateLevel::SecondUpdate (0x4557b2) uses case-sensitive strncmp against "Single_missions/PP03"
                if (_stricmp(raw, "single_missions/pp03:pp03") == 0 || _stricmp(raw, "single_missions/pp03") == 0) {
                    strcpy(level, "Single_missions/PP03:pp03");
                } else {
                    strncpy(level, raw, sizeof(level) - 1);
                }
                continue;
            }
            alignas(8) char str[64] = {0};
            g_strCtor(str, cmds[i]);
            exec(str, NULL);
            g_strDtor(str);
            char m[300]; snprintf(m, sizeof(m), "autostart: executed '%s'", cmds[i]);
            LogLine(m);
        }
        if (save[0]) {
            unsigned char dummy = 0;
            SendEvent(EV_SINGLE_GAME, &dummy);
            LogLine("autostart: sent evSingleGame(0x6ad05e) MainMenu -> SingleGame (for evLoadSelectedGame)");
            state = 7; stamp = now;
            return;
        }
        if (!level[0]) { state = 6; return; }
        unsigned char dummy = 0;
        SendEvent(EV_SINGLE_GAME, &dummy);
        LogLine("autostart: sent evSingleGame(0x6ad05e) MainMenu -> SingleGame");
        state = 2; stamp = now;
        return;
    }

    if (state == 2) {
        if (since < 1) return;      // SingleGame entry state must be active before selecting & starting mission
        unsigned char dummy = 0;
        SendLocationEvent(EV_SELECT_LOCATION, level);
        SendEvent(EV_START_SELECTED, &dummy);
        char m[300]; snprintf(m, sizeof(m), "autostart: sent 0x6ddd18 eLocation '%s' + 0x6ddd14 (Start)", level);
        LogLine(m);
        ++attempts;
        state = 3; stamp = now;
        return;
    }

    if (state == 7) {
        if (since < 1) return;      // let SingleGame entry state [0x38] activate
        DWORD one = 1;
        SendEvent(0x6ad022, &one);  // evLoadMissionDlg: [0x38] -> [0x40] (Load Dialog state)
        LogLine("autostart: sent evLoadMissionDlg(0x6ad022, 1) SingleGame -> LoadDialog");
        state = 9; stamp = now;
        return;
    }

    if (state == 9) {
        if (since < 1) return;      // let Load Dialog state [0x40] activate and scan saves
        HMODULE cp = GetModuleHandleA("MSVCP80.dll");
        typedef void* (__thiscall *PFN_StrAssignCStr)(void* self, const char* s);
        auto strAssign = cp ? (PFN_StrAssignCStr)GetProcAddress(
            cp, "??4?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@PBD@Z") : NULL;
        if (!strAssign) {
            LogLine("autostart: MSVCP80 string::operator=(const char*) not found");
            state = 6;
            return;
        }
        // 0x00616610 (LoadGameWindow::LoadSelectedSave(bool isCampaign)) reads selected save folder name from:
        //   0x00ac4b90 + 0x2c (when isCampaign == 1)
        //   0x00ac4bd8 + 0x2c (when isCampaign == 0)
        // and calls 0x616230 (set active campaign from SaveInfo), sends 0x6ddd1a, and calls 0x77a290 (close modal dlg).
        strAssign((void*)(0x00ac4b90 + 0x2c), save);
        strAssign((void*)(0x00ac4bd8 + 0x2c), save);
        typedef void* (__cdecl *PFN_GetLoadWnd)(void);
        typedef void (__thiscall *PFN_LoadSelectedSave)(void* self, int isCampaign);
        void* loadWnd = ((PFN_GetLoadWnd)0x006169d0)();
        char m[320];
        snprintf(m, sizeof(m), "autostart: calling LoadGameWindow::LoadSelectedSave(1) [0x616610] wnd=%p save='%s'",
                 loadWnd, save);
        LogLine(m);
        ((PFN_LoadSelectedSave)0x00616610)(loadWnd, 1);
        state = 10; stamp = now;
        return;
    }

    if (state == 10) {
        if (since < 2) return;
        unsigned char dummy = 0;
        SendEvent(0x6ad042, &dummy);  // trigger 0x453ad0 -> 0x453e80 -> 0x6ddd1a -> GameStateLoadLevel
        LogLine("autostart: sent 0x6ad042 -> GameStateLoadLevel (save load active, waiting for [lsw+0x50])");
        state = 4; stamp = now;
        return;
    }

    if (state == 3) {
        if (DebugLogHas("BEGIN LOADING LEVEL")) {
            LogLine("autostart: GameStateLoadLevel active (BEGIN LOADING LEVEL)");
            state = 4; stamp = now;
            return;
        }
        if (since < 6) return;
        if (attempts >= 3) { LogLine("autostart: level load NOT confirmed, giving up"); state = 6; return; }
        SendLocationEvent(EV_LOAD_SEL_MISSION, level);
        LogLine("autostart: retry sent 0x6ddd16 directly");
        ++attempts;
        stamp = now;
        return;
    }

    // state 4: after BEGIN LOADING LEVEL, [lsw+0x50] starts at 0 and becomes 1 when loading finishes and
    // the "Начать" (Start) button appears. Clicking StartGameProcess() (0x407ae0) triggers "LEVEL LOADED in"
    // and "GameStateLevel::SecondUpdate started in".
    if (state == 4) {
        if (DebugLogHas("GameStateLevel::SecondUpdate started in")) {
            LogLine("autostart: SecondUpdate seen -> watching for Advisor / M2MessageBox dialogs");
            g_levelUp = true;
            g_levelUpTick = now;
            LogModules("level");
            state = 5; stamp = now;
            return;
        }
        typedef void* (__cdecl *PFN_GetLoadingScreen)(void);
        typedef void (__thiscall *PFN_StartGameProcess)(void* self);
        void* lsw = ((PFN_GetLoadingScreen)0x00409ed0)();
        if (lsw && *(DWORD*)((char*)lsw + 0x50) != 0) {
            ((PFN_StartGameProcess)0x00407ae0)(lsw);
            LogLine("autostart: auto-clicked LoadingScreenWindow::StartGameProcess() [Start button]");
        }
        return;
    }

    // state 5: auto-close Advisor briefing & M2MessageBox dialogs (0x5de7b0 / 0x5de7e0) for 20 seconds
    if (state == 5) {
        typedef bool (__cdecl *PFN_IsAdvisorOrMsgBoxVisible)(void);
        typedef void (__cdecl *PFN_CloseAdvisorOrMsgBox)(void);
        if (((PFN_IsAdvisorOrMsgBoxVisible)0x005de7b0)()) {
            ((PFN_CloseAdvisorOrMsgBox)0x005de7e0)();
            LogLine("autostart: auto-closed Advisor / M2MessageBox [OK button]");
        }
        if (since >= 20) {
            state = speed > 0.0f ? 8 : 6;
        }
        if (speed <= 0.0f) {
            if (!g_speed5Tick.QuadPart) g_speed5Tick = now;
            return;
        }
        // fall through: enforce speed while dialogs are being closed too
    }

    // state 5/8: keep the simulation at SPEED: (dialogs / advisor can reset it); checked once per second
    if ((state == 5 || state == 8) && now.QuadPart >= nextSpeed.QuadPart) {
        nextSpeed.QuadPart = now.QuadPart + g_qpcFreq.QuadPart;
        void* sw = SpeedSwitcher();
        float cur = 0.0f, scale = 0.0f;
        if (!sw || !QuantSpeed(&cur, &scale)) return;
        if (cur < speed - 0.01f || cur > speed + 0.01f) {
            typedef void (__thiscall *PFN_SetSpeed)(void* self, float v);
            ((PFN_SetSpeed)FN_SET_GAME_SPEED)(sw, speed);
            float now2 = 0.0f, scale2 = 0.0f;
            QuantSpeed(&now2, &scale2);
            char m[200];
            snprintf(m, sizeof(m), "autostart: game speed %.2f -> %.2f (GameSpeedSwitcher %p; eQuantControl Speed=%.2f "
                     "SpeedScale=%.2f)", cur, speed, sw, now2, scale2);
            LogLine(m);
        }
        if (!g_speed5Tick.QuadPart && cur >= speed - 0.1f) {
            g_speed5Tick = now;
        }
    }
}

// ---- Simulation quant timer -------------------------------------------------------------------------
// Frame loop (reversed): app.dll eApplication::Working (0x10013b70) does Sleep(3), measures the frame time, clamps
// it to [1,100] ms and calls vtbl[0x2c] = Majesty2.exe 0x403560, which clamps it to 70 ms and calls
// app.dll eApplication::Quant(ms) (0x10013a40) exactly ONCE: DeltaQuants(ms) -> quant = clamp(ms*SpeedScale, 1, 1000)
// game-ms, then one NextQuant. There is no catch-up loop: a slow frame slows game time instead of piling up quants.
// We wrap that call through Majesty2.exe's IAT (0x87cc6c) to time it and count quants per frame.
typedef void (__fastcall *PFN_AppQuant)(void* self, void* edx, int ms);  // fastcall(ecx, edx, stack) == thiscall
static PFN_AppQuant g_realAppQuant = NULL;
static volatile int64_t g_quantTicks = 0;
static volatile uint32_t g_quantCalls = 0;
static volatile uint64_t g_quantArgMs = 0, g_quantGameMs = 0;
static volatile LONG g_inQuant = 0, g_inPresent = 0;  // phase flags read by the profiler thread
typedef int (__thiscall *PFN_QcInt)(void* self);
static PFN_QcInt g_qcDeltaQuants = NULL;

static uintptr_t g_sceneBase = 0;
static uintptr_t g_renderBase = 0;
static volatile uint32_t g_fastAstarCalls = 0;
static volatile uint32_t g_fastAstarPops = 0;
static volatile uint32_t g_fastAstarEarlyOuts = 0;
static volatile int64_t  g_fastAstarTicks = 0;
static volatile int64_t  g_fastSeePointTicks = 0;
static volatile uint32_t g_fastSeePointCalls = 0;
static volatile uint32_t g_fastSeePointPops = 0;

struct FastHeapItem {
    float f_cost;
    BYTE* node;
    int   nodeIdx;
};
static FastHeapItem s_fastHeap[32768];

// Surgical replacement for scene.dll!0x100F5FA0 (xPather::xNetwork::eNetwork::FindPath)
// Eliminates the 55.7% CPU catastrophe where unreachable building/blocked targets expand all ~5,000 nodes
// and ~40,000 edges in the city graph using x87 fcomp/fnstsw + std::vector heap swaps.
static void __fastcall Fast_eNetwork_FindPath(BYTE* self, void* /*edx*/, int startNodeIdx, int endNodeIdx, BYTE* goal) {
    if (startNodeIdx < 0 || endNodeIdx < 0 || !goal) return;
    int64_t tAstar0 = Qpc();

    float clipMin[2] = { -100000.0f, -100000.0f };
    float clipMax[2] = {  100000.0f,  100000.0f };
    typedef bool (__thiscall *PFN_HandleClip)(void* handle, float* vec2);
    PFN_HandleClip fnClip = (PFN_HandleClip)(g_sceneBase + 0x0F39B0);
    fnClip(goal + 0x0C, clipMin);
    fnClip(goal + 0x0C, clipMax);

    void** goalVtbl = *(void***)goal;
    typedef bool (__thiscall *PFN_GoalUseHeuristic)(void* goal);
    typedef bool (__thiscall *PFN_GoalIsReached)(void* goal, BYTE* node);
    bool useHeuristic = ((PFN_GoalUseHeuristic)goalVtbl[3])(goal);
    PFN_GoalIsReached fnIsReached = (PFN_GoalIsReached)goalVtbl[2];
    bool isEPoint = ((uintptr_t)fnIsReached == (g_sceneBase + 0x0FFFB0));

    uint32_t searchId = *(uint32_t*)(self + 0x38) + 1;
    *(uint32_t*)(self + 0x38) = searchId;
    BYTE* nodesBase = *(BYTE**)(self + 0x64);
    BYTE* nodesEnd  = *(BYTE**)(self + 0x68);
    if (searchId >= 1000) {
        searchId = 1;
        *(uint32_t*)(self + 0x38) = 1;
        for (BYTE* p = nodesBase; p != nodesEnd; p += 0x4C) {
            *(uint32_t*)(p + 0x18) = 0;
        }
    }
    const uint32_t searchTag = searchId << 22;

    BYTE* startNode = nodesBase + startNodeIdx * 0x4C;
    BYTE* endNode   = nodesBase + endNodeIdx * 0x4C;
    const float endX = *(const float*)(endNode + 0x24);
    const float endY = *(const float*)(endNode + 0x28);

    *(float*)(startNode + 0x10) = 0.0f;
    *(int32_t*)(startNode + 0x14) = -1;
    *(uint32_t*)(startNode + 0x18) = searchTag | 0x100000u;

    int heapSize = 1;
    s_fastHeap[0].f_cost  = 0.0f;
    s_fastHeap[0].node    = startNode;
    s_fastHeap[0].nodeIdx = startNodeIdx;

    const BYTE* edgesBase = *(const BYTE**)(self + 0x88);
    const int speedIdx = (int)*(int8_t*)(goal + 0x14);
    const float clipMinX = clipMin[0], clipMinY = clipMin[1];
    const float clipMaxX = clipMax[0], clipMaxY = clipMax[1];

    BYTE* epTargetNode = isEPoint ? *(BYTE**)(goal + 0x24) : NULL;
    const float epGoalX = isEPoint ? *(const float*)(goal + 0x1C) : 0.0f;
    const float epGoalY = isEPoint ? *(const float*)(goal + 0x20) : 0.0f;
    const BYTE* epTask  = isEPoint ? *(const BYTE**)(goal + 0x18) : NULL;
    const float epMaxCost = (isEPoint && epTask) ? *(const float*)(epTask + 0x08) : 1e30f;

    // Check if endNode has at least one traversable incident edge (cost < 99999.0f).
    // When a unit paths to a building/shop/lair/blocked obstacle, all edges incident to endNode have cost == 100000.0f,
    // making endNode mathematically unreachable (which caused original A* to exhaust all ~5,000 nodes on every query).
    bool targetHasOpenEdge = false;
    if (isEPoint && epTargetNode) {
        const int* teIt  = *(const int**)(epTargetNode + 0x04);
        const int* teEnd = *(const int**)(epTargetNode + 0x08);
        for (; teIt != teEnd; ++teIt) {
            const BYTE* e = edgesBase + (*teIt) * 0x38;
            if (*(const float*)(e + 0x08 + speedIdx * 4) < 99999.0f) {
                targetHasOpenEdge = true;
                break;
            }
        }
    }

    int totalPops = 0;
    int stagnantPops = 0;

    while (heapSize > 0) {
        BYTE* curNode = s_fastHeap[0].node;
        int curNodeIdx = s_fastHeap[0].nodeIdx;
        --heapSize;
        if (heapSize > 0) {
            FastHeapItem last = s_fastHeap[heapSize];
            int hole = 0;
            int half = heapSize >> 1;
            while (hole < half) {
                int child = (hole << 1) + 1;
                int right = child + 1;
                if (right < heapSize && s_fastHeap[right].f_cost < s_fastHeap[child].f_cost) {
                    child = right;
                }
                if (last.f_cost <= s_fastHeap[child].f_cost) break;
                s_fastHeap[hole] = s_fastHeap[child];
                *(uint32_t*)(s_fastHeap[hole].node + 0x18) =
                    (*(uint32_t*)(s_fastHeap[hole].node + 0x18) & 0xFFF00000u) | (uint32_t)hole;
                hole = child;
            }
            s_fastHeap[hole] = last;
            *(uint32_t*)(last.node + 0x18) =
                (*(uint32_t*)(last.node + 0x18) & 0xFFF00000u) | (uint32_t)hole;
        }
        *(uint32_t*)(curNode + 0x18) = searchTag | 0x2FFFD1u;

        ++totalPops;
        if (isEPoint) {
            if (curNode == epTargetNode) {
                *(BYTE**)(goal + 0x28) = curNode;
                break;
            }
            float dx = epGoalX - *(const float*)(curNode + 0x24);
            float dy = epGoalY - *(const float*)(curNode + 0x28);
            float distSq = dx * dx + dy * dy;
            float bestD2 = *(const float*)(goal + 0x2C);
            if (distSq < bestD2) {
                bestD2 = distSq;
                *(float*)(goal + 0x2C) = distSq;
                *(BYTE**)(goal + 0x28) = curNode;
                stagnantPops = 0;
            } else {
                ++stagnantPops;
            }
            if (*(const float*)(curNode + 0x10) >= epMaxCost) {
                break;
            }
            if (!targetHasOpenEdge) {
                // Target node is inside a blocked building/obstacle footprint and can NEVER be entered.
                // Stop as soon as A* has swept the building perimeter!
                if ((bestD2 <= 400.0f && stagnantPops >= 20) || stagnantPops >= 48 || totalPops >= 180) {
                    g_fastAstarEarlyOuts++;
                    break;
                }
            } else if (g_levelUp && totalPops >= 600) {
                // Open target node on the other side of a sealed wall/island
                g_fastAstarEarlyOuts++;
                break;
            }
        } else {
            int64_t tSp0 = Qpc();
            bool reached = fnIsReached(goal, curNode);
            g_fastSeePointTicks += (Qpc() - tSp0);
            if (reached) {
                break;
            }
            if (totalPops >= (g_levelUp ? 96 : 256)) {
                g_fastAstarEarlyOuts++;
                break;
            }
        }

        const float curG = *(const float*)(curNode + 0x10);
        const int* edgeIt  = *(const int**)(curNode + 0x04);
        const int* edgeEnd = *(const int**)(curNode + 0x08);
        for (; edgeIt != edgeEnd; ++edgeIt) {
            int edgeIdx = *edgeIt;
            const BYTE* edge = edgesBase + edgeIdx * 0x38;
            int nbIdx = *(const int*)(edge + 0x00);
            if (nbIdx == curNodeIdx) {
                nbIdx = *(const int*)(edge + 0x04);
            }
            BYTE* nb = nodesBase + nbIdx * 0x4C;
            uint32_t nbState = *(uint32_t*)(nb + 0x18);
            float nbOldG;
            if ((nbState >> 22) != searchId) {
                nbOldG = 100000.0f;
                nbState = searchTag;
                *(float*)(nb + 0x10) = nbOldG;
                *(uint32_t*)(nb + 0x18) = nbState;
            } else {
                if (nbState & 0x200000u) continue; // Already closed in this search
                nbOldG = *(const float*)(nb + 0x10);
            }
            float edgeCost = *(const float*)(edge + 0x08 + speedIdx * 4);
            float newG = curG + edgeCost;
            if (newG >= nbOldG) continue;

            float nbX = *(const float*)(nb + 0x24);
            float nbY = *(const float*)(nb + 0x28);
            if (nbX < clipMinX || nbX > clipMaxX || nbY < clipMinY || nbY > clipMaxY) continue;

            *(float*)(nb + 0x10) = newG;
            *(int*)(nb + 0x14) = edgeIdx;

            float fCost = newG;
            if (useHeuristic) {
                float hx = nbX - endX;
                float hy = nbY - endY;
                fCost += __builtin_sqrtf(hx * hx + hy * hy);
            }

            if (nbState & 0x100000u) {
                int pos = (int)(nbState & 0xFFFFFu);
                if (pos < heapSize) {
                    float oldF = s_fastHeap[pos].f_cost;
                    s_fastHeap[pos].f_cost = fCost;
                    if (fCost < oldF) {
                        FastHeapItem moving = s_fastHeap[pos];
                        while (pos > 0) {
                            int parent = (pos - 1) >> 1;
                            if (s_fastHeap[parent].f_cost <= moving.f_cost) break;
                            s_fastHeap[pos] = s_fastHeap[parent];
                            *(uint32_t*)(s_fastHeap[pos].node + 0x18) =
                                (*(uint32_t*)(s_fastHeap[pos].node + 0x18) & 0xFFF00000u) | (uint32_t)pos;
                            pos = parent;
                        }
                        s_fastHeap[pos] = moving;
                        *(uint32_t*)(moving.node + 0x18) =
                            (*(uint32_t*)(moving.node + 0x18) & 0xFFF00000u) | (uint32_t)pos;
                    } else if (fCost > oldF) {
                        FastHeapItem moving = s_fastHeap[pos];
                        int half = heapSize >> 1;
                        while (pos < half) {
                            int child = (pos << 1) + 1;
                            int right = child + 1;
                            if (right < heapSize && s_fastHeap[right].f_cost < s_fastHeap[child].f_cost) {
                                child = right;
                            }
                            if (moving.f_cost <= s_fastHeap[child].f_cost) break;
                            s_fastHeap[pos] = s_fastHeap[child];
                            *(uint32_t*)(s_fastHeap[pos].node + 0x18) =
                                (*(uint32_t*)(s_fastHeap[pos].node + 0x18) & 0xFFF00000u) | (uint32_t)pos;
                            pos = child;
                        }
                        s_fastHeap[pos] = moving;
                        *(uint32_t*)(moving.node + 0x18) =
                            (*(uint32_t*)(moving.node + 0x18) & 0xFFF00000u) | (uint32_t)pos;
                    }
                }
            } else if (heapSize < 32768) {
                int pos = heapSize++;
                while (pos > 0) {
                    int parent = (pos - 1) >> 1;
                    if (s_fastHeap[parent].f_cost <= fCost) break;
                    s_fastHeap[pos] = s_fastHeap[parent];
                    *(uint32_t*)(s_fastHeap[pos].node + 0x18) =
                        (*(uint32_t*)(s_fastHeap[pos].node + 0x18) & 0xFFF00000u) | (uint32_t)pos;
                    pos = parent;
                }
                s_fastHeap[pos].f_cost  = fCost;
                s_fastHeap[pos].node    = nb;
                s_fastHeap[pos].nodeIdx = nbIdx;
                *(uint32_t*)(nb + 0x18) = (nbState & 0xFFF00000u) | 0x100000u | (uint32_t)pos;
            }
        }
    }
    g_fastAstarCalls++;
    g_fastAstarPops += (uint32_t)totalPops;
    if (!isEPoint) {
        g_fastSeePointCalls++;
        g_fastSeePointPops += (uint32_t)totalPops;
    }
    g_fastAstarTicks += (Qpc() - tAstar0);
}

// SSE2 replacement for scene.dll!0x1001CA70 (QuatToMat3x4, cdecl)
static void __cdecl Fast_QuatToMat3x4(float* m, const float* q) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    m[0] = 1.0f - (yy + zz);
    m[1] = xy + wz;
    m[2] = xz - wy;
    m[3] = xy - wz;
    m[4] = 1.0f - (xx + zz);
    m[5] = yz + wx;
    m[6] = xz + wy;
    m[7] = yz - wx;
    m[8] = 1.0f - (xx + yy);
}

// SSE2 replacement for scene.dll!0x1001A8B0 (Box3Union, thiscall: void(float* dst_ecx, const float* src) ret 4)
static void __fastcall Fast_Box3Union(float* dst, void* /*edx*/, const float* src) {
    if (src[0] < dst[0]) dst[0] = src[0];
    if (src[1] < dst[1]) dst[1] = src[1];
    if (src[2] < dst[2]) dst[2] = src[2];
    if (dst[3] < src[3]) dst[3] = src[3];
    if (dst[4] < src[4]) dst[4] = src[4];
    if (dst[5] < src[5]) dst[5] = src[5];
}

// SSE2 replacement for scene.dll!0x10019620 & render.dll!0x10013AA0 (Mat3x4_TransformPoint, cdecl)
static void __cdecl Fast_Mat3x4_TransformPoint(float* out, const float* v, const float* m) {
    float vx = v[0], vy = v[1], vz = v[2];
    out[0] = m[0] * vx + m[3] * vy + m[6] * vz + m[9];
    out[1] = m[1] * vx + m[4] * vy + m[7] * vz + m[10];
    out[2] = m[2] * vx + m[5] * vy + m[8] * vz + m[11];
}

// SSE2 replacement for scene.dll!0x100196B0 (Mat3x4_Mul, cdecl)
static float* __cdecl Fast_Mat3x4_Mul(float* out, const float* a, const float* b) {
    float a0 = a[0], a1 = a[1], a2 = a[2];
    float a3 = a[3], a4 = a[4], a5 = a[5];
    float a6 = a[6], a7 = a[7], a8 = a[8];
    float a9 = a[9], a10 = a[10], a11 = a[11];

    float b0 = b[0], b1 = b[1], b2 = b[2];
    out[0] = b0 * a0 + b1 * a3 + b2 * a6;
    out[1] = b0 * a1 + b1 * a4 + b2 * a7;
    out[2] = b0 * a2 + b1 * a5 + b2 * a8;

    float b3 = b[3], b4 = b[4], b5 = b[5];
    out[3] = b3 * a0 + b4 * a3 + b5 * a6;
    out[4] = b3 * a1 + b4 * a4 + b5 * a7;
    out[5] = b3 * a2 + b4 * a5 + b5 * a8;

    float b6 = b[6], b7 = b[7], b8 = b[8];
    out[6] = b6 * a0 + b7 * a3 + b8 * a6;
    out[7] = b6 * a1 + b7 * a4 + b8 * a7;
    out[8] = b6 * a2 + b7 * a5 + b8 * a8;

    float b9 = b[9], b10 = b[10], b11 = b[11];
    out[9]  = b9 * a0 + b10 * a3 + b11 * a6 + a9;
    out[10] = b9 * a1 + b10 * a4 + b11 * a7 + a10;
    out[11] = b9 * a2 + b10 * a5 + b11 * a8 + a11;
    return out;
}

static bool WriteJmp32(uintptr_t srcAddr, void* dstFunc) {
    DWORD oldProt = 0;
    if (!VirtualProtect((void*)srcAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    BYTE* p = (BYTE*)srcAddr;
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)dstFunc - (srcAddr + 5));
    VirtualProtect((void*)srcAddr, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), (void*)srcAddr, 5);
    return true;
}

static void InstallEngineOptimizations(void) {
    static bool done = false;
    if (done) return;
    char ev[8] = {0};
    if (GetEnvironmentVariableA("M2_NO_OPT", ev, sizeof(ev)) > 0 && ev[0] == '1') {
        LogLine("engine opt: skipped (M2_NO_OPT=1)");
        done = true;
        return;
    }
    HMODULE sc = GetModuleHandleA("scene.dll");
    HMODULE rn = GetModuleHandleA("render.dll");
    if (!sc || !rn) return;
    done = true;
    g_sceneBase  = (uintptr_t)sc;
    g_renderBase = (uintptr_t)rn;

    bool okAstar = WriteJmp32(g_sceneBase + 0x0F5FA0, (void*)Fast_eNetwork_FindPath);
    bool okQuat  = WriteJmp32(g_sceneBase + 0x01CA70, (void*)Fast_QuatToMat3x4);
    bool okBox   = WriteJmp32(g_sceneBase + 0x01A8B0, (void*)Fast_Box3Union);
    bool okM34S  = WriteJmp32(g_sceneBase + 0x019620, (void*)Fast_Mat3x4_TransformPoint);
    bool okM34R  = WriteJmp32(g_renderBase + 0x013AA0, (void*)Fast_Mat3x4_TransformPoint);
    bool okMul   = WriteJmp32(g_sceneBase + 0x0196B0, (void*)Fast_Mat3x4_Mul);

    char m[320];
    snprintf(m, sizeof(m), "engine opt: A*(targetOpenEdge)=%d Quat=%d Box3=%d Mat34Pt=%d/%d Mat34Mul=%d",
             okAstar, okQuat, okBox, okM34S, okM34R, okMul);
    LogLine(m);
}

// ================== Wave 1: D3D9 camera frustum capture + spatial zone culling ==================
// IDirect3DDevice9 vtable slot 44 = SetTransform.  We keep the newest D3DTS_VIEW (2) and
// D3DTS_PROJECTION (3) matrices, prefer the newest *perspective* pair (shadow-map and other
// orthographic passes set their own view/projection and must not hijack the camera frustum), rebuild
// the six Gribb-Hartmann planes and publish IsInCameraFrustum(x, y, z, radius) for the rest of this
// DLL - Waves 2/3 call it directly, it is deliberately NOT added to src/d3d9.def because the techgate
// checks for exactly 8 exports.
//
// Env knobs:
//   M2_FRUSTUM_TEST=1        once per second log a live geometric self-test + the first captured
//                            view/projection matrices (lets the Mac side confirm the convention)
//   M2_ZONE_CULL=probe|1     spatial-zone hook on eZones_UpdateArea (see M2_ZoneInstall)
//   M2_ZONE_AABB=<min>:<max> hex byte offsets of two float3 corners inside the zone object
//   M2_ZONE_MARGIN=<f>       bounding-sphere radius padding for the zone test (default 1.25)
//   M2_ZONE_MODULE=<dll>     module holding eZones_UpdateArea (default scene.dll)
static float g_capView[16], g_capProj[16];
static float g_perspView[16], g_perspProj[16];
static volatile LONG g_haveView = 0, g_haveProj = 0, g_havePersp = 0, g_projStyle = 0;
static M2Frustum g_frustum;
static volatile LONG g_frustumReady = 0, g_frustumLogConv = -100;
static volatile uint32_t g_stView = 0, g_stProj = 0, g_stOther = 0;
static volatile uint32_t g_frustumBuilds = 0, g_frustumFails = 0, g_frustumTests = 0, g_frustumCulls = 0;
static volatile uint32_t g_frustumMatDumped = 0;

typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTransform)(void* pDevice, DWORD state, const void* pMatrix);
static PFN_SetTransform g_origSetTransform = NULL;

// D3DX-style projection detector: 1 = stored the way D3D9 wants it, 2 = stored transposed (GL-style),
// 0 = neither (orthographic / unknown) - used only to pick the camera pair, never to cull.
static int M2_ProjStyle(const float* p) {
    if (M2_Abs(p[2 * 4 + 3] - 1.0f) < 1.0e-3f && M2_Abs(p[3 * 4 + 3]) < 1.0e-3f) return 1;
    if (M2_Abs(p[3 * 4 + 2] - 1.0f) < 1.0e-3f && M2_Abs(p[3 * 4 + 3]) < 1.0e-3f) return 2;
    return 0;
}

static void M2_FrustumRebuild(void) {
    const float* view;
    const float* proj;
    int pair = 0;
    if (g_havePersp && g_haveView) { view = g_perspView; proj = g_perspProj; pair = 1; }
    else if (g_haveView && g_haveProj) { view = g_capView; proj = g_capProj; }
    else return;
    M2Frustum f;
    int ok = M2_FrustumBuild(&f, view, proj);
    g_frustumBuilds++;
    if (!ok) g_frustumFails++;
    g_frustum = f;
    InterlockedExchange(&g_frustumReady, ok ? 1 : 0);
    if (ok && g_frustumLogConv != f.convention) {
        g_frustumLogConv = f.convention;
        char m[400];
        snprintf(m, sizeof(m), "frustum: conv=%d (viewT=%d projT=%d) pair=%s score=%d eye=%.2f,%.2f,%.2f "
                 "fwd=%.3f,%.3f,%.3f near=%.2f far=%.2f",
                 f.convention, f.convention & 1, (f.convention >> 1) & 1, pair ? "persp" : "latest",
                 f.score, f.eye[0], f.eye[1], f.eye[2], f.fwd[0], f.fwd[1], f.fwd[2], f.nearDist, f.farDist);
        LogLine(m);
    } else if (!ok && g_frustumLogConv != -1) {
        g_frustumLogConv = -1;
        LogLine("frustum: no matrix convention passed the self-test -> everything stays fail-open");
    }
    if (ok && g_frustumMatDumped == 0) {
        char ev[8] = {0};
        if (GetEnvironmentVariableA("M2_FRUSTUM_TEST", ev, sizeof(ev)) > 0 && ev[0] == '1') {
            g_frustumMatDumped = 1;
            char line[512];
            int n = snprintf(line, sizeof(line), "frustum-mtx view= ");
            for (int i = 0; i < 16 && n > 0 && n < (int)sizeof(line) - 32; ++i) n += snprintf(line + n, sizeof(line) - n, "%g ", view[i]);
            LogLine(line);
            n = snprintf(line, sizeof(line), "frustum-mtx proj= ");
            for (int i = 0; i < 16 && n > 0 && n < (int)sizeof(line) - 32; ++i) n += snprintf(line + n, sizeof(line) - n, "%g ", proj[i]);
            LogLine(line);
        }
    }
}

// The IsInCameraFrustum(x, y, z, radius) contract of Wave 1: 1 = visible (or unknown), 0 = fully
// outside the camera frustum.  Direct-callable from every later hook compiled into this DLL.
extern "C" int __cdecl IsInCameraFrustum(float x, float y, float z, float radius) {
    g_frustumTests++;
    int vis = M2_FrustumSphereVisible(&g_frustum, x, y, z, radius);
    if (!vis) g_frustumCulls++;
    return vis;
}

// Accessors for the later waves / in-process tooling.
extern "C" const float* __cdecl M2_CameraFrustumPlanes(void) {
    return g_frustumReady ? &g_frustum.plane[0][0] : (const float*)0;
}
extern "C" int __cdecl M2_CameraFrustumValid(void) { return g_frustumReady ? 1 : 0; }
extern "C" void __cdecl M2_CameraFrustumEye(float* out3) {
    if (!out3) return;
    out3[0] = g_frustum.eye[0]; out3[1] = g_frustum.eye[1]; out3[2] = g_frustum.eye[2];
}

static HRESULT STDMETHODCALLTYPE Hook_SetTransform(void* pDevice, DWORD state, const void* pMatrix) {
    int64_t t0 = Qpc();
    const int isView = (state == 2), isProj = (state == 3);
    if (pMatrix && isView) memcpy(g_capView, pMatrix, 64);
    else if (pMatrix && isProj) memcpy(g_capProj, pMatrix, 64);
    // Wave 3: the sub-tick blend is decided from the RAW engine matrix captured above and only the matrix
    // handed to the device is replaced, so the Wave 1 frustum (g_capView / g_perspView) and the Wave 2
    // particle gates keep seeing exactly what the engine wrote - a blend can never move the culling core.
    const void* sendMat = pMatrix;
    if (pMatrix && isView) {
        M2_R3Init();                      // one predictable test after the first VIEW
        if (g_r3Cam) {
            float blend[16];
            M2_R3CamSample((const float*)pMatrix);
            sendMat = M2_R3CamBlendView(pMatrix, M2_R3NowUs(), blend);
        }
    }
    HRESULT hr = g_origSetTransform(pDevice, state, sendMat);
    if (pMatrix && (isView || isProj)) {
        if (isView) {
            InterlockedExchange(&g_haveView, 1);
            g_stView++;
        } else {
            InterlockedExchange(&g_haveProj, 1);
            g_stProj++;
            int style = M2_ProjStyle(g_capProj);
            InterlockedExchange(&g_projStyle, style);
            if (style != 0 && g_haveView) {
                memcpy(g_perspProj, g_capProj, 64);
                memcpy(g_perspView, g_capView, 64);
                InterlockedExchange(&g_havePersp, 1);
            }
        }
        M2_FrustumRebuild();
    } else {
        g_stOther++;
    }
    Account(B_STATE, t0);
    return hr;
}

static void InstallSetTransformCapture(void** vtbl) {
    if (!vtbl || vtbl[44] == (void*)Hook_SetTransform) return;
    g_origSetTransform = (PFN_SetTransform)vtbl[44];
    PatchPtr(&vtbl[44], (void*)Hook_SetTransform);
    LogLine("frustum: SetTransform capture hooked (device vtbl[44])");
}

// Live self-test on the real, running frustum (M2_FRUSTUM_TEST=1 only).  Expect 1 1 1 1.
static void M2_FrustumLiveSelfTest(void) {
    char ev[8] = {0};
    if (!(GetEnvironmentVariableA("M2_FRUSTUM_TEST", ev, sizeof(ev)) > 0 && ev[0] == '1')) return;
    if (!g_frustumReady) {
        LogLine("frustum-test: frustum invalid -> fail-open, nothing is culled");
        return;
    }
    const M2Frustum* f = &g_frustum;
    float bx = f->eye[0] - f->fwd[0], by = f->eye[1] - f->fwd[1], bz = f->eye[2] - f->fwd[2];
    float d = f->nearDist + 0.5f * (f->farDist - f->nearDist);
    float mx = f->eye[0] + f->fwd[0] * d, my = f->eye[1] + f->fwd[1] * d, mz = f->eye[2] + f->fwd[2] * d;
    float fd = f->farDist * 3.0f + 1000.0f;
    float fx = f->eye[0] + f->fwd[0] * fd, fy = f->eye[1] + f->fwd[1] * fd, fz = f->eye[2] + f->fwd[2] * fd;
    int behindOut = !M2_FrustumContainsPoint(f, bx, by, bz, 0.0f);
    int midIn = M2_FrustumContainsPoint(f, mx, my, mz, 1.0f);
    int farOut = !M2_FrustumContainsPoint(f, fx, fy, fz, 0.0f);
    int radiusIn = M2_FrustumSphereVisible(f, bx, by, bz, f->nearDist + 2.0f);
    char m[320];
    snprintf(m, sizeof(m), "frustum-test: behind_out=%d mid_in=%d far_out=%d radius_visible=%d near=%.2f far=%.2f "
             "eye=%.1f,%.1f,%.1f", behindOut, midIn, farOut, radiusIn, f->nearDist, f->farDist,
             f->eye[0], f->eye[1], f->eye[2]);
    LogLine(m);
}

static void M2_FrustumTelemetry(double tSec) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    uint32_t stv = g_stView, stp = g_stProj, sto = g_stOther;
    uint32_t builds = g_frustumBuilds, fails = g_frustumFails, tests = g_frustumTests, culls = g_frustumCulls;
    g_stView = g_stProj = g_stOther = 0;
    g_frustumBuilds = g_frustumFails = g_frustumTests = g_frustumCulls = 0;
    char line[512];
    int len = snprintf(line, sizeof(line),
                       "# frustum t=%.2f valid=%d conv=%d score=%d near=%.2f far=%.2f st_view=%u st_proj=%u "
                       "st_other=%u builds=%u fails=%u tests=%u culls=%u\n",
                       tSec, (int)g_frustumReady, g_frustum.convention, g_frustum.score, g_frustum.nearDist,
                       g_frustum.farDist, stv, stp, sto, builds, fails, tests, culls);
    if (len > 0) {
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    }
}

// ---------- Spatial zone culling: eZones_UpdateArea (scene.dll RVA 0x96B20 by default) ----------
// The target is an engine function whose exact signature is unknown at build time, so instead of a C
// detour we emit a byte-level stub + trampoline into a private executable block:
//   target[0..N) -> jmp stub;  stub: pushfd/pushad, call M2_ZoneDecision(self = ecx, &saved ctx);
//   returns 0 -> restore and jmp trampoline (replays the copied prologue, jumps back to target+N);
//   returns 1 -> restore and ret (the engine call is skipped).
// Only prologues that match a whitelisted MSVC shape (exact instruction boundary >= 5 bytes) are
// patched; anything else is logged with a raw hexdump and left untouched.  Modes (M2_ZONE_CULL):
//   unset/0 -> not installed at all
//   probe   -> pass-through + call counters + one-time object hexdump + AABB candidate scan
//   1       -> probe plus real culling, but only once M2_ZONE_AABB=<min>:<max> names the two float3
//              corners inside the zone object that disassembly confirmed; without it, probe only.
#define M2_ZONE_RVA 0x96B20u
static int g_zoneMode = 0;                  // 0 = off, 1 = probe, 2 = gate
static int g_zoneInstalled = 0;
static int g_zoneAabbMinOff = -1, g_zoneAabbMaxOff = -1;
static float g_zoneMargin = 1.25f;
static volatile uint32_t g_zoneCalls = 0, g_zoneCulls = 0, g_zoneUncertain = 0, g_zoneProbeDumps = 0;

// Which PE section (and its flags) does va live in?  Refuses anything that is not an executable
// .text byte range, which is also the guard that keeps us out of .reloc (the scene.dll lesson).
static int M2_SectionForVa(uintptr_t va, char* nameOut, size_t nameCap, DWORD* charsOut) {
    HMODULE mod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)va, &mod) || !mod) return 0;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    DWORD rva = (DWORD)(va - (uintptr_t)base);
    if (rva >= nt->OptionalHeader.SizeOfImage) return 0;
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        DWORD lo = s->VirtualAddress;
        DWORD hi = lo + (s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData);
        if (rva >= lo && rva < hi) {
            if (nameOut && nameCap) {
                char nm[9];
                memcpy(nm, s->Name, 8);
                nm[8] = 0;
                snprintf(nameOut, nameCap, "%s", nm);
            }
            if (charsOut) *charsOut = s->Characteristics;
            return 1;
        }
    }
    return 0;
}

// ---- M2_ZONE_EMIT_BEGIN ----
// Pure byte-level helpers (no Windows calls, no globals) so that scripts/test_zone_stub_layout.py can
// extract them, emit the thunk into a buffer and verify the instruction stream with a disassembler.
// Prologue whitelist: each pattern ends on an exact instruction boundary of >= 5 bytes, which is what
// makes the 5-byte E9 patch + prologue replay safe without a length disassembler.  0 = unsupported.
// mov ebp,esp has two encodings: 8B EC (MSVC) and 89 E5 (GCC/MinGW) - accept both.
static int M2_FrameAt(const unsigned char* q) {
    return q[0] == 0x55 && ((q[1] == 0x8B && q[2] == 0xEC) || (q[1] == 0x89 && q[2] == 0xE5));
}
static int M2_ZonePrologueLen(const unsigned char* p) {
    if (p[0] == 0x8B && p[1] == 0xFF && M2_FrameAt(p + 2)) return 5;   // mov edi,edi; push ebp; mov ebp,esp (hotpatch)
    // two or more push r32 followed by the frame prologue, e.g. 53 56 57 55 8B EC
    for (int j = 2; j <= 6; ++j) {
        if (!M2_FrameAt(p + j)) continue;
        int allPushes = 1;
        for (int i = 0; i < j; ++i) {
            if (p[i] < 0x50 || p[i] > 0x57) { allPushes = 0; break; }
        }
        if (allPushes) return j + 3;
    }
    if (M2_FrameAt(p)) {
        if (p[3] == 0x83 && p[4] == 0xEC) return 6;                                              // sub esp,imm8
        if (p[3] == 0x81 && p[4] == 0xEC) return 9;                                              // sub esp,imm32
        if (p[3] == 0x83 && p[4] == 0xE4) return 6;                                              // and esp,imm8
        if (p[3] == 0x6A && p[4] == 0xFF) return 5;                                              // push -1
        if (p[3] == 0x68) return 8;                                                              // push imm32
        // Wave 2: frame + 1..5 callee-saved pushes + a terminating sub esp (MSVC and GCC both emit the
        // callee-saved pushes AFTER `mov ebp,esp`): 55 8B EC 53 56 83 EC 20.  The mandatory sub anchors
        // the instruction boundary; a bare push run without it stays refused (ambiguous boundary).
        for (int k = 1; k <= 5; ++k) {
            int allPushes = 1;
            for (int i = 0; i < k; ++i) {
                if (p[3 + i] < 0x50 || p[3 + i] > 0x57) { allPushes = 0; break; }
            }
            if (!allPushes) break;
            if (p[3 + k] == 0x83 && p[3 + k + 1] == 0xEC) return 3 + k + 3;                      // + sub esp,imm8
            if (p[3 + k] == 0x81 && p[3 + k + 1] == 0xEC) return 3 + k + 6;                      // + sub esp,imm32
        }
    }
    if (p[0] == 0x81 && p[1] == 0xEC) return 6; // sub esp, imm32 (no frame pointer)
    if (p[0] == 0x6A && p[1] == 0xFF && p[2] == 0x68) return 7;                                  // push -1; push imm32 (SEH)
    return 0;
}

// Trampoline: replay the copied prologue verbatim, then jmp back to the first untouched instruction.
static int M2_ZoneEmitTrampoline(unsigned char* out, const unsigned char* prologue, int prolen, void* resume) {
    for (int i = 0; i < prolen; ++i) out[i] = prologue[i];
    out[prolen] = 0xE9;  // jmp rel32
    *(int*)(out + prolen + 1) = (int)((unsigned char*)resume - (out + prolen + 5));
    return prolen + 5;
}

// Stub entered via the 5-byte E9 patch at the target entry.  The stack is exactly as the original
// function would see it, so after pushfd + pushad the saved state is:
//   [ebp+0]  edi  [ebp+4]  esi  [ebp+8]  ebp  [ebp+12] esp  [ebp+16] ebx  [ebp+20] edx  [ebp+24] ecx
//   [ebp+28] eax  [ebp+32] eflags  [ebp+36] return address  [ebp+40...] stack arguments
// M2_ZoneDecision(self = ecx, ctx = &saved state) returns 1 to skip the engine call, 0 to run it.
// NOTE (Wave 2 fix): __cdecl arguments are pushed right-to-left, so ctx is pushed FIRST and self LAST.
// Wave 1 emitted the pushes the other way round, which silently swapped the two parameters inside
// M2_ZoneDecision() (self = pushad context, ctx = this): fail-open kept it harmless, but the probe
// dumps were reading the saved-register block as "object bytes" and the AABB gate could never fire.
static int M2_ZoneEmitStub(unsigned char* out, void* decision, void* trampSlot) {
    unsigned char* w = out;
    *w++ = 0x9C;                                         // pushfd
    *w++ = 0x60;                                         // pushad
    *w++ = 0x89; *w++ = 0xE5;                            // mov ebp, esp
    *w++ = 0x83; *w++ = 0xE4; *w++ = 0xF0;               // and esp, -16 (16-byte alignment for the call)
    *w++ = 0x8B; *w++ = 0x45; *w++ = 0x18;               // mov eax, [ebp+24] (saved ecx = this)
    *w++ = 0x55;                                         // push ebp (arg1 = &saved state, pushed first)
    *w++ = 0x50;                                         // push eax (arg0 = self)
    *w++ = 0xE8;                                         // call <decision>
    *(int*)w = (int)((unsigned char*)decision - (w + 4));
    w += 4;
    *w++ = 0x83; *w++ = 0xC4; *w++ = 0x08;               // add esp, 8
    *w++ = 0x85; *w++ = 0xC0;                            // test eax, eax
    *w++ = 0x75; *w++ = 0x0A;                            // jnz skip (the 10-byte run path is next)
    *w++ = 0x89; *w++ = 0xEC;                            // mov esp, ebp
    *w++ = 0x61;                                         // popad
    *w++ = 0x9D;                                         // popfd
    *w++ = 0xFF; *w++ = 0x25;                            // jmp dword ptr [<trampSlot>]
    *(unsigned int*)w = (unsigned int)(uintptr_t)trampSlot;
    w += 4;
    *w++ = 0x89; *w++ = 0xEC;                            // skip: mov esp, ebp
    *w++ = 0x61;                                         // popad
    *w++ = 0x9D;                                         // popfd
    *w++ = 0xC3;                                         // ret
    return (int)(w - out);
}
// ---- M2_ZONE_EMIT_END ----

static int M2_ZoneAabb(void* self, float* mn, float* mx) {
    if (g_zoneAabbMinOff < 0 || g_zoneAabbMaxOff < 0) return 0;
    const float* a = (const float*)((const BYTE*)self + g_zoneAabbMinOff);
    const float* b = (const float*)((const BYTE*)self + g_zoneAabbMaxOff);
    for (int i = 0; i < 3; ++i) {
        float lo = a[i], hi = b[i];
        if (!(lo == lo) || !(hi == hi)) return 0;
        if (lo < -1.0e5f || lo > 1.0e5f || hi < -1.0e5f || hi > 1.0e5f) return 0;
        if (hi < lo - 1.0e-3f) return 0;
        mn[i] = lo; mx[i] = hi;
    }
    return 1;
}

// One-time evidence dump so the disassembly side can confirm the real layout of the zone object.
static void M2_ZoneProbeDump(void* self, void* ctx) {
    if (g_zoneProbeDumps || !self || g_zoneMode == 0) return;
    g_zoneProbeDumps = 1;
    const BYTE* p = (const BYTE*)self;
    char m[512];
    int n = snprintf(m, sizeof(m), "zone: first call self=%p ctx=%p ret=%p bytes=", self, ctx,
                     ctx ? *(void**)((const BYTE*)ctx + 36) : (void*)0);
    for (int i = 0; i < 48 && n > 0 && n < (int)sizeof(m) - 4; ++i) n += snprintf(m + n, sizeof(m) - n, "%02x", p[i]);
    LogLine(m);
    char c2[300] = {0};
    int cn = 0, found = 0;
    for (int off = 0; off + 12 <= 192 && found < 4; off += 4) {
        const float* a = (const float*)(p + off);
        for (int off2 = off + 12; off2 + 12 <= 192; off2 += 4) {
            const float* b = (const float*)(p + off2);
            int ok = 1;
            float span = 0.0f;
            for (int i = 0; i < 3; ++i) {
                float lo = a[i], hi = b[i];
                if (!(lo == lo) || !(hi == hi) || lo < -1.0e5f || lo > 1.0e5f || hi < -1.0e5f || hi > 1.0e5f ||
                    hi < lo - 1.0e-3f) { ok = 0; break; }
                float dd = hi - lo;
                span += dd * dd;
            }
            if (!ok || span < 1.0f || span > 1.0e6f) continue;
            if (cn < (int)sizeof(c2) - 24) cn += snprintf(c2 + cn, sizeof(c2) - cn, "%s0x%X:0x%X", found ? ", " : "", off, off2);
            ++found;
            break;
        }
    }
    char m2[400];
    snprintf(m2, sizeof(m2), "zone: aabb candidates=%s (confirm against scene.dll disasm, then M2_ZONE_AABB=<min>:<max>)",
             found ? c2 : "none");
    LogLine(m2);
}

// Called from the stub with the complete original register state (ctx = saved ESP after pushfd/pushad:
// ctx+24 = ecx (this), ctx+36 = return address, ctx+40... = stack arguments).
extern "C" int __cdecl M2_ZoneDecision(void* self, void* ctx) {
    g_zoneCalls++;
    if (g_zoneMode >= 1) M2_ZoneProbeDump(self, ctx);
    if (g_zoneMode < 2 || !self) return 0;
    if (!g_frustumReady) return 0;
    float mn[3], mx[3];
    if (!M2_ZoneAabb(self, mn, mx)) { g_zoneUncertain++; return 0; }
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    float r = 0.5f * M2_Sqrt(dx * dx + dy * dy + dz * dz) * g_zoneMargin;
    if (!(r == r) || r > 1.0e5f) return 0;
    float cx = 0.5f * (mn[0] + mx[0]), cy = 0.5f * (mn[1] + mx[1]), cz = 0.5f * (mn[2] + mx[2]);
    if (!M2_FrustumSphereVisible(&g_frustum, cx, cy, cz, r)) {
        g_zoneCulls++;
        return 1;   // fully off screen -> skip the zone's matrix/collision update
    }
    return 0;
}

static char g_zoneModule[64] = "scene.dll";

// Idempotent and cheap: called from Hook_CreateDevice and retried from Hook_Present until it lands, so
// a scene.dll that is still loading at device-creation time does not silently disable the hook.
static int M2_ZoneInstall(void) {
    static bool cfgDone = false;
    if (g_zoneInstalled) return 1;
    if (!cfgDone) {
        cfgDone = true;
        char ev[64] = {0};
        if (GetEnvironmentVariableA("M2_ZONE_CULL", ev, sizeof(ev)) > 0) {
            if (!_stricmp(ev, "probe")) g_zoneMode = 1;
            else if (ev[0] == '1') g_zoneMode = 2;
        }
        char v[64] = {0};
        if (GetEnvironmentVariableA("M2_ZONE_MARGIN", v, sizeof(v)) > 0) {
            float f = (float)atof(v);
            if (f >= 1.0f && f <= 8.0f) g_zoneMargin = f;
        }
        if (g_zoneMode >= 2 && GetEnvironmentVariableA("M2_ZONE_AABB", v, sizeof(v)) > 0) {
            unsigned a = 0, b = 0;
            if (sscanf(v, "%x:%x", &a, &b) == 2 && a < 0x1000 && b < 0x1000) {
                g_zoneAabbMinOff = (int)a;
                g_zoneAabbMaxOff = (int)b;
            }
        }
        if (GetEnvironmentVariableA("M2_ZONE_MODULE", v, sizeof(v)) > 0 && v[0]) {
            snprintf(g_zoneModule, sizeof(g_zoneModule), "%s", v);
        }
        if (g_zoneMode == 0) LogLine("zone: disabled (set M2_ZONE_CULL=probe for evidence, =1 to cull)");
    }
    if (g_zoneMode == 0) return 0;
    const char* mod = g_zoneModule;
    HMODULE h = GetModuleHandleA(mod);
    char m[400];
    if (!h) {
        snprintf(m, sizeof(m), "zone: %s not loaded -> hook not installed", mod);
        LogLine(m);
        return 0;
    }
    uintptr_t target = (uintptr_t)h + M2_ZONE_RVA;
    char sec[16] = {0};
    DWORD chars = 0;
    if (!M2_SectionForVa(target, sec, sizeof(sec), &chars)) {
        snprintf(m, sizeof(m), "zone: %s+0x%X is not inside any section -> refusing to patch", mod, M2_ZONE_RVA);
        LogLine(m);
        return 0;
    }
    if (!(chars & IMAGE_SCN_MEM_EXECUTE) || _stricmp(sec, ".text") != 0) {
        snprintf(m, sizeof(m), "zone: %s+0x%X lives in section %s (chars=0x%lX) -> refusing to patch "
                 "(must be executable .text, never .reloc)", mod, M2_ZONE_RVA, sec, (unsigned long)chars);
        LogLine(m);
        return 0;
    }
    const BYTE* p = (const BYTE*)target;
    int plen = M2_ZonePrologueLen(p);
    if (plen == 0) {
        snprintf(m, sizeof(m), "zone: %s+0x%X prologue %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                 "is not a whitelisted shape -> not installed (disassemble it and extend M2_ZonePrologueLen)",
                 mod, M2_ZONE_RVA, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
        LogLine(m);
        return 0;
    }
    BYTE* blk = (BYTE*)VirtualAlloc(NULL, 512, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blk) {
        LogLine("zone: VirtualAlloc for the stub failed");
        return 0;
    }
    memset(blk, 0xCC, 512);
    BYTE* tramp = blk;                                   // [0..plen) replayed prologue, then back-jump
    BYTE* stub = blk + 64;
    M2_ZoneEmitTrampoline(tramp, p, plen, (void*)(target + (uintptr_t)plen));
    int stubLen = M2_ZoneEmitStub(stub, (void*)&M2_ZoneDecision, blk + 256);
    *(void**)(blk + 256) = (void*)tramp;
    if (memcmp((const void*)target, p, (size_t)plen) != 0) {
        LogLine("zone: target changed between probe and patch -> aborted");
        VirtualFree(blk, 0, MEM_RELEASE);
        return 0;
    }
    BYTE jmp5[5];
    jmp5[0] = 0xE9;
    *(int32_t*)(jmp5 + 1) = (int32_t)((uintptr_t)stub - (target + 5));
    DWORD oldProt = 0;
    if (!VirtualProtect((void*)target, (SIZE_T)plen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        LogLine("zone: VirtualProtect on the target failed");
        VirtualFree(blk, 0, MEM_RELEASE);
        return 0;
    }
    memcpy((void*)target, jmp5, 5);
    VirtualProtect((void*)target, (SIZE_T)plen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, (SIZE_T)plen);
    g_zoneInstalled = 1;
    snprintf(m, sizeof(m), "zone: mode=%d installed %s+0x%X section=%s prologue=%d bytes stub=%d tramp=%p aabb=%s margin=%.2f",
             g_zoneMode, mod, M2_ZONE_RVA, sec, plen, stubLen, (void*)tramp,
             (g_zoneMode >= 2 && g_zoneAabbMinOff >= 0) ? "confirmed" : "unset(probe-only)", g_zoneMargin);
    LogLine(m);
    return 1;
}

static void M2_ZoneTelemetry(double tSec) {
    if (g_zoneMode == 0 || g_logFile == INVALID_HANDLE_VALUE) return;
    uint32_t calls = g_zoneCalls, culls = g_zoneCulls, unc = g_zoneUncertain;
    g_zoneCalls = g_zoneCulls = g_zoneUncertain = 0;
    char line[256];
    int len = snprintf(line, sizeof(line), "# zone t=%.2f mode=%d installed=%d calls=%u culls=%u unresolved=%u margin=%.2f\n",
                       tSec, g_zoneMode, g_zoneInstalled, calls, culls, unc, g_zoneMargin);
    if (len > 0) {
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    }
}

// ================== Wave 2 (t_m2_pf_particle_cull): particle-flow off-screen culling ==================
// Goal: drop particle *calculation* (xParticleFlow::eOperator::operator()) and *paint*
// (xParticleFlow::PaintPoint) work for emitters that are fully outside the camera frustum, using the
// Wave-1 kernel (IsInCameraFrustum / M2_FrustumSphereVisible).
//
// The pf.dll / scene.dll binaries do NOT exist on this host or on Clore #84966, so this code contains
// ZERO hard-coded game RVAs (coordinator directive + workspaces/hyperdrive/hookspec_wave2_runtime_scan.md):
//   * targets come from explicit env RVAs (M2_PF_OPERATOR_RVA / M2_PF_PAINT_RVA, for once the Mac side
//     has disassembly or probe evidence), or from a runtime scan of the *loaded* module (M2_PF_SCAN) that
//     anchors on strings / IDirect3DDevice9 vtable call sites - build- and relocation-independent;
//   * installs go through the Wave-1 byte-stub discipline: prologue whitelist, private RWX block,
//     register/stack-preserving stub, in-memory PE section validation (.text exec only, never .reloc,
//     never on disk), signature compare right before the patch (no-op-on-mismatch);
//   * nothing is ever skipped unless EVERY opt-in holds: M2_PARTICLE_CULL=1 + a confirmed emitter
//     position spec (M2_PF_POS[_OP]/[_PAINT]) + an explicitly confirmed callee stack-arg size
//     (M2_PF_OP_RET / M2_PF_PAINT_RET) + a valid frustum.  Any doubt => run the original (fail-open).
// Kill switches: M2_DISABLE_PARTICLE_CULL=1 (coordinator naming) or M2_DISABLE_CULLING=1 (hookspec naming).
//
// Divergences from the hookspec, by design (documented in RUNBOOK_wave2_particle.md):
//   * S4/S5 boundaries + x87 density are byte-heuristic in-process (no length decoder is linked into a
//     game DLL); the capstone-verified offline reference remains scripts/m2_runtime_scan_ref.py.
//   * M2_InstallCandidateHook deliberately does NOT make a validation call into the engine at install
//     time (calling an engine function with a fabricated `this` from inside a D3D callback is exactly
//     the kind of guess this wave forbids); the pass-through probe wrapper is the validation.
//   * The draw-level classifier M2_ParticleShouldDraw (hookspec §3) is not implemented: by
//     DrawPrimitiveUP time the vertices are already transformed, so there is no world-space batch
//     centre to test; the function-level hooks above replace that path.

// ---- M2_PF_CORE_BEGIN ----
// Pure, dependency-free helpers (no Windows types, no DLL globals, no logging) so that
// scripts/test_pf_particle.py can extract this block together with the Wave-1 frustum/emit blocks,
// compile them natively and unit-test / mock-PE-validate everything off-line.

#if !defined(_WIN32) && !defined(__cdecl)
#define __cdecl                     // x86-64/SysV is cdecl by default; the shim only serves the tests
#endif

#include <emmintrin.h>   // SSE2 intrinsics for the batch culling kernel (-msse2 is in the build line)

// Vectorised 4-wide SSE2 sphere-vs-frustum batch test - the kernel a vectorised emission/gate loop
// calls instead of per-sphere scalar tests.  Classification is bit-compatible with
// M2_FrustumSphereVisible(): a sphere is VISIBLE (fail-open) when any coordinate is NaN/Inf/huge
// (|v| >= 1e9), when radius is NaN or > 1e6, or when n.p + d >= -radius holds for ALL six planes
// (touching the plane counts as visible; planes must be the normalised Wave-1 layout, row = (nx,ny,nz,d)).
// outVis[i] receives 1/0 per sphere; returns the number of visible spheres.  Process-internal
// (extern "C", deliberately NOT in src/d3d9.def): same surface discipline as IsInCameraFrustum().
extern "C" int __cdecl M2_FrustumCullBatch(const float* planes, const float* xyz, int count, float radius,
                                           unsigned char* outVis) {
    if (!planes || !xyz || !outVis || count <= 0) return 0;
    if (!(radius == radius) || radius > 1.0e6f) {        // NaN / absurd radius BEFORE clamping: all visible
        for (int i = 0; i < count; ++i) outVis[i] = 1;
        return count;
    }
    float r = (radius > 0.0f) ? radius : 0.0f;
    const __m128 vr     = _mm_set1_ps(r);
    const __m128 lim    = _mm_set1_ps(1.0e9f);
    const __m128 nzero  = _mm_setzero_ps();
    const __m128 allone = _mm_castsi128_ps(_mm_set1_epi32(-1));
    const __m128 sgnmsk = _mm_set1_ps(-0.0f);
    int vis = 0;
    for (int i = 0; i < count; i += 4) {
        const float* p0 = xyz + 3 * (i + 0);
        const float* p1 = xyz + 3 * (i + 1 < count ? i + 1 : count - 1);   // tail lanes duplicate the last sphere
        const float* p2 = xyz + 3 * (i + 2 < count ? i + 2 : count - 1);
        const float* p3 = xyz + 3 * (i + 3 < count ? i + 3 : count - 1);
        __m128 t0 = _mm_setr_ps(p0[0], p0[1], p0[2], 0.0f);                // scalar loads: never read past the buffer
        __m128 t1 = _mm_setr_ps(p1[0], p1[1], p1[2], 0.0f);
        __m128 t2 = _mm_setr_ps(p2[0], p2[1], p2[2], 0.0f);
        __m128 t3 = _mm_setr_ps(p3[0], p3[1], p3[2], 0.0f);
        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);               // t0 = x0..x3, t1 = y0..y3, t2 = z0..z3
        const __m128 X = t0, Y = t1, Z = t2;
        const __m128 ax = _mm_andnot_ps(sgnmsk, X);       // |x| (NaN stays NaN, +-Inf stays +-Inf)
        const __m128 ay = _mm_andnot_ps(sgnmsk, Y);
        const __m128 az = _mm_andnot_ps(sgnmsk, Z);
        const __m128 fin = _mm_and_ps(                                     // ordered AND |coord| < 1e9 on all axes
            _mm_and_ps(_mm_and_ps(_mm_cmpord_ps(X, X), _mm_cmpord_ps(Y, Y)), _mm_cmpord_ps(Z, Z)),
            _mm_and_ps(_mm_and_ps(_mm_cmplt_ps(ax, lim), _mm_cmplt_ps(ay, lim)), _mm_cmplt_ps(az, lim)));
        __m128 m = _mm_and_ps(fin, allone);               // finite lanes start "visible", non-finite start 0
        for (int k = 0; k < 6; ++k) {
            const float* q = planes + k * 4;
            const __m128 d = _mm_add_ps(
                _mm_add_ps(_mm_mul_ps(_mm_set1_ps(q[0]), X), _mm_mul_ps(_mm_set1_ps(q[1]), Y)),
                _mm_add_ps(_mm_mul_ps(_mm_set1_ps(q[2]), Z), _mm_set1_ps(q[3])));
            m = _mm_and_ps(m, _mm_cmpge_ps(d, _mm_sub_ps(nzero, vr)));     // keep only if d >= -r on this plane
        }
        m = _mm_or_ps(m, _mm_andnot_ps(fin, allone));     // non-finite lanes are always visible (fail-open)
        const unsigned mask = (unsigned)_mm_movemask_ps(m);
        for (int k = 0; k < 4 && i + k < count; ++k) {
            const unsigned v = (mask >> k) & 1u;
            outVis[i + k] = (unsigned char)v;
            vis += (int)v;
        }
    }
    return vis;
}

// Every access to vtable slot disp/4 that ends in a CALL or a slot LOAD:
//   `call dword ptr [reg+disp32]` (FF /2, mod=10, no SIB) and the [esp+SIB] spelling, PLUS
//   `mov r32,[reg+disp32]` (8B, mod=10) - MSVC usually calls through the slot directly, while
//   GCC/MinGW first loads the slot into a register and then does `call reg`; the load itself is
//   already a unique vtable-slot fingerprint, so both spellings count as evidence sites.
// Byte-exact matching; the sites are evidence-grade only, installs still pass the whitelist/sig/
// section gates.
static int M2_ScanVtCallSites(const unsigned char* text, int len, uint32_t disp,
                              uintptr_t* sites, int maxSites) {
    int n = 0;
    for (int i = 0; i + 6 <= len && n < maxSites; ++i) {
        const unsigned char op = text[i];
        const unsigned char modrm = text[i + 1];
        if (op == 0xFF) {                                          // call/jmp [reg+disp32]
            if ((modrm & 0xF8) == 0x90 && (modrm & 7) != 4) {
                if (*(const uint32_t*)(text + i + 2) == disp) sites[n++] = (uintptr_t)i;
            } else if (modrm == 0x94 && text[i + 2] == 0x24 && i + 7 <= len) {  // [esp+disp32] via SIB
                if (*(const uint32_t*)(text + i + 3) == disp) sites[n++] = (uintptr_t)i;
            }
        } else if (op == 0x8B) {                                   // mov r32,[reg+disp32] (slot load)
            if ((modrm >> 6) == 2 && (modrm & 7) != 4) {
                if (*(const uint32_t*)(text + i + 2) == disp) sites[n++] = (uintptr_t)i;
            } else if ((modrm >> 6) == 2 && (modrm & 7) == 4 && text[i + 2] == 0x24 && i + 7 <= len) {
                if (*(const uint32_t*)(text + i + 3) == disp) sites[n++] = (uintptr_t)i;
            }
        }
    }
    return n;
}

// Every `call/jmp dword ptr [absolute disp32]` (FF 15 / FF 25) whose target equals slotVa - the
// classic IAT-indirect call site.  Returns the number of sites stored.
static int M2_ScanIatCallSites(const unsigned char* text, int len, uintptr_t slotVa,
                               uintptr_t* sites, int maxSites) {
    int n = 0;
    const uint32_t want = (uint32_t)slotVa;
    for (int i = 0; i + 6 <= len && n < maxSites; ++i) {
        if (text[i] != 0xFF || (text[i + 1] != 0x15 && text[i + 1] != 0x25)) continue;
        if (*(const uint32_t*)(text + i + 2) == want) sites[n++] = (uintptr_t)i;
    }
    return n;
}

// Nearest preceding whitelisted prologue (Wave-1 M2_ZonePrologueLen) at a plausible function boundary:
// preceded by int3 padding (CC), a nop (90), a ret (C3), OR 16-byte aligned - MSVC and GCC both align
// .text function entries to 16 bytes and GNU as pads the gap with `lea esi,[esi]` (ends 0x00), which
// no byte-level rule can catch.  Byte-heuristic in-process; the capstone-verified offline reference is
// scripts/m2_runtime_scan_ref.py (S4).  Returns the start offset or -1; *outProlen gets the length.
static int M2_PfWalkBackStart(const unsigned char* text, int len, int siteOff, int maxBack, int* outProlen) {
    if (outProlen) *outProlen = 0;
    if (!text || siteOff <= 0 || siteOff >= len) return -1;
    int lo = siteOff - maxBack;
    if (lo < 0) lo = 0;
    for (int q = siteOff - 1; q >= lo; --q) {
        const int pl = M2_ZonePrologueLen(text + q);
        if (pl == 0) continue;
        if (q == 0 || (q & 15) == 0 || text[q - 1] == 0xCC || text[q - 1] == 0x90 || text[q - 1] == 0xC3) {
            if (outProlen) *outProlen = pl;
            return q;
        }
    }
    return -1;
}

// Emitter-position spec: "0x1C" -> float3 at self+0x1C; "0x8+0x20" -> chase the pointer at self+0x8,
// then read the float3 at +0x20 inside the pointee (the flow object an eOperator points at).
static int M2_PfParsePosSpec(const char* s, int* hop, int* off1, int* off2) {
    *hop = 0; *off1 = -1; *off2 = -1;
    if (!s || !s[0]) return 0;
    unsigned a = 0, b = 0;
    char sep = 0;
    if (sscanf(s, "%x%c%x", &a, &sep, &b) == 3 && (sep == '+' || sep == '.') && a < 0x1000 && b < 0x1000) {
        *hop = 1; *off1 = (int)a; *off2 = (int)b;
        return 1;
    }
    if (sscanf(s, "%x", &a) == 1 && a < 0x1000) {
        *off1 = (int)a;
        return 1;
    }
    return 0;
}

// Reads the emitter position with the same fail-open rules as the zone AABB read: bounded offsets,
// a plausibility check on the chased pointer, and finite-float validation.  0 = "do not cull".
static int M2_PfReadPos(const unsigned char* self, int hop, int off1, int off2, float out[3]) {
    if (!self) return 0;
    const unsigned char* p = self;
    if (hop) {
        if (off1 < 0 || off2 < 0 || (size_t)off1 + 4 > 0x1000 || (size_t)off2 + 12 > 0x1000) return 0;
        const uintptr_t ptr = *(const uintptr_t*)(self + off1);
        // plausible user-space pointer: above the first 64 KB and below the top of user space
        // (0x80000000 on the 32-bit PE32 target, the canonical-space limit on the 64-bit test host)
        const uintptr_t hi = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
        if (ptr < 0x10000u || ptr >= hi) return 0;
        p = (const unsigned char*)ptr + off2;
    } else {
        if (off1 < 0 || (size_t)off1 + 12 > 0x1000) return 0;
        p += off1;
    }
    const float x = *(const float*)(p + 0), y = *(const float*)(p + 4), z = *(const float*)(p + 8);
    if (!(x == x) || !(y == y) || !(z == z)) return 0;
    if (!M2_IsFinite(x) || !M2_IsFinite(y) || !M2_IsFinite(z)) return 0;
    out[0] = x; out[1] = y; out[2] = z;
    return 1;
}

// Byte offsets of float-triples that look like plausible world positions (probe evidence for
// M2_PF_POS; the Mac side confirms the real offset against disassembly, zone-style).
static int M2_PfScanPosCandidates(const unsigned char* obj, int bytes, int* offs, int maxOut) {
    int n = 0;
    for (int off = 0; off + 12 <= bytes && n < maxOut; off += 4) {
        const float* v = (const float*)(obj + off);
        const float x = v[0], y = v[1], z = v[2];
        if (!(x == x) || !(y == y) || !(z == z)) continue;
        if (x < -1.0e4f || x > 1.0e4f || y < -1.0e4f || y > 1.0e4f || z < -1.0e4f || z > 1.0e4f) continue;
        if (M2_Sqrt(x * x + y * y + z * z) < 0.05f) continue;
        offs[n++] = off;
    }
    return n;
}

// 16-byte entry signature (S7): captured when a target is resolved, compared immediately before patch.
static void M2_PfSig16(const unsigned char* at, unsigned char* sig) {
    for (int i = 0; i < 16; ++i) sig[i] = at[i];
}
static int M2_PfSigMatches(const unsigned char* at, const unsigned char* sig) {
    return memcmp(at, sig, 16) == 0;
}

// Byte-level x87 density over a window (D8..DF opcodes; modrm >= 0xC0 marks the register form).
// Informational ranking only - not decoder-verified (see the divergence note above).
static void M2_PfX87Density(const unsigned char* win, int bytes, int* reg, int* all) {
    int r = 0, a = 0;
    for (int i = 0; i + 1 < bytes; ++i) {
        if (win[i] >= 0xD8 && win[i] <= 0xDF) {
            ++a;
            if (win[i + 1] >= 0xC0) ++r;
        }
    }
    *reg = r; *all = a;
}

// Case-insensitive ASCII substring search (anchor scan, S2).  Returns the offset or -1.
static int M2_FindAsciiI(const unsigned char* hay, int hayLen, const char* needle) {
    int nl = 0;
    while (needle[nl]) ++nl;
    if (nl <= 0 || nl > hayLen) return -1;
    for (int i = 0; i + nl <= hayLen; ++i) {
        int j = 0;
        while (j < nl) {
            unsigned char a = hay[i + j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != (unsigned char)needle[j]) break;
            ++j;
        }
        if (j == nl) return i;
    }
    return -1;
}

// Candidate record (hookspec §3 surface; x87 fields are the byte heuristic above, insns = window bytes).
typedef struct M2ScanCandidate {
    uint32_t      rva;
    void*         va;
    unsigned char sig[16];
    uint32_t      prologueLen;
    int           x87Reg, x87All, insns;
    int           isExecutable, inReloc;
} M2ScanCandidate;

// Wave-2 stub: the Wave-1 zone stub plus a configurable callee stack-arg pop for the skip path.
//   retBytes == 0 -> plain RET (cdecl-style callee, or no stack arguments)
//   retBytes  > 0 -> RET imm16 (thiscall/stdcall callee that pops its own stack arguments)
// __cdecl arguments are pushed right-to-left: ctx goes on the stack FIRST, then self.
static int M2_PfEmitStub(unsigned char* out, void* decision, void* trampSlot, int retBytes) {
    unsigned char* w = out;
    *w++ = 0x9C;                                         // pushfd
    *w++ = 0x60;                                         // pushad
    *w++ = 0x89; *w++ = 0xE5;                            // mov ebp, esp
    *w++ = 0x83; *w++ = 0xE4; *w++ = 0xF0;               // and esp, -16 (16-byte alignment for the call)
    *w++ = 0x8B; *w++ = 0x45; *w++ = 0x18;               // mov eax, [ebp+24] (saved ecx = this)
    *w++ = 0x55;                                         // push ebp (arg1 = &saved state, pushed first)
    *w++ = 0x50;                                         // push eax (arg0 = self)
    *w++ = 0xE8;                                         // call <decision>
    *(int*)w = (int)((unsigned char*)decision - (w + 4));
    w += 4;
    *w++ = 0x83; *w++ = 0xC4; *w++ = 0x08;               // add esp, 8
    *w++ = 0x85; *w++ = 0xC0;                            // test eax, eax
    *w++ = 0x75; *w++ = 0x0A;                            // jnz skip (the 10-byte run path is next)
    *w++ = 0x89; *w++ = 0xEC;                            // mov esp, ebp
    *w++ = 0x61;                                         // popad
    *w++ = 0x9D;                                         // popfd
    *w++ = 0xFF; *w++ = 0x25;                            // jmp dword ptr [<trampSlot>]
    *(unsigned int*)w = (unsigned int)(uintptr_t)trampSlot;
    w += 4;
    *w++ = 0x89; *w++ = 0xEC;                            // skip: mov esp, ebp
    *w++ = 0x61;                                         // popad
    *w++ = 0x9D;                                         // popfd
    if (retBytes > 0) {
        *w++ = 0xC2;                                     // ret imm16 (callee pops its stack args)
        *w++ = (unsigned char)(retBytes & 0xFF);
        *w++ = (unsigned char)((retBytes >> 8) & 0xFF);
    } else {
        *w++ = 0xC3;                                     // ret
    }
    return (int)(w - out);
}
// ---- M2_PF_CORE_END ----

// ---- Wave-2 runtime state (Windows side) ----
static int g_pfMode = 0;                 // M2_PARTICLE_CULL: 0 off / 1 probe / 2 gate
static int g_pfScan = 0;                 // M2_PF_SCAN: 0 off / 1 probe / 2 discover+auto-resolve
static int g_pfHist = 0;                 // M2_DRAW_HISTOGRAM=1
static int g_pfKill = 0;                 // M2_DISABLE_PARTICLE_CULL=1 / M2_DISABLE_CULLING=1
static int g_pfDone = 0;                 // install pass finished (stop retrying from Present)
static char g_pfModules[96] = "pf.dll,scene.dll";
static float g_pfRadius = 32.0f;         // emitter bounding sphere, world units (M2_PF_RADIUS)
static float g_pfMargin = 1.25f;         // radius padding (M2_PF_MARGIN)
static int g_pfVtIdx[4] = {83, 84, 0, 0};
static int g_pfNVt = 2;                  // device vtable indices treated as particle draw evidence
static void* g_pfBlock = NULL;           // private CC-filled RWX block: tramp@0/32, slots@64/68, stubs@96/224

struct M2_PfHookState {
    int installed, kind;                 // kind: 0 = eOperator::operator(), 1 = PaintPoint
    uintptr_t target;
    DWORD moduleBase;
    char module[24];
    unsigned char sig[16];
    int hop, off1, off2;                 // resolved emitter-position spec
    int retBytes, retExplicit;           // callee stack-arg bytes for the skip path
    int gate;                            // 1 = allowed to actually skip
    volatile uint32_t calls, culls, uncertain, probeDumped;
};
static M2_PfHookState g_pfOp = {0, 0, 0, 0, {0}, {0}, 0, -1, -1, 0, 0, 0, 0, 0, 0, 0};
static M2_PfHookState g_pfPaint = {0, 1, 0, 0, {0}, {0}, 0, -1, -1, 0, 0, 0, 0, 0, 0, 0};

struct M2_PfTargetSpec { int set; char mod[24]; uintptr_t rva; };
static M2_PfTargetSpec g_pfOpRva = {0, {0}, 0}, g_pfPaintRva = {0, {0}, 0};

struct M2_PfModInfo { char name[24]; HMODULE h; uintptr_t base, end; };
static M2_PfModInfo g_pfMods[4];
static int g_pfNMods = 0, g_pfModsLogged = 0;

struct M2_PfUpSite { DWORD rva; DWORD modIdx; volatile uint32_t calls, prims, stride; };
static M2_PfUpSite g_pfUpSites[16];
static volatile uint32_t g_pfUpN = 0, g_pfUpOther = 0, g_pfUpFull = 0;

// Caller-evidence channel: __builtin_return_address(0) from the draw hooks lands inside the engine
// function that issued the draw (the paint path).  Bounded, lock-free, probe-grade.
static void M2_PfUpEvidence(void* retaddr, unsigned int primCount, unsigned int stride) {
    const uintptr_t ra = (uintptr_t)retaddr;
    int mi = -1;
    for (int i = 0; i < g_pfNMods; ++i) {
        if (ra >= g_pfMods[i].base && ra < g_pfMods[i].end) { mi = i; break; }
    }
    if (mi < 0) { g_pfUpOther++; return; }               // draw issued from outside the pf modules
    const DWORD rva = (DWORD)(ra - g_pfMods[mi].base);
    const uint32_t n = g_pfUpN;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
        if (g_pfUpSites[i].rva == rva && g_pfUpSites[i].modIdx == (DWORD)mi) {
            g_pfUpSites[i].calls++;
            g_pfUpSites[i].prims += primCount;
            if (stride) g_pfUpSites[i].stride = stride;
            return;
        }
    }
    if (n >= 16) { g_pfUpFull++; return; }
    g_pfUpSites[n].rva = rva;
    g_pfUpSites[n].modIdx = (DWORD)mi;
    g_pfUpSites[n].calls = 1;
    g_pfUpSites[n].prims = primCount;
    g_pfUpSites[n].stride = stride;
    g_pfUpN = n + 1;
}

static void M2_PfProbeDump(M2_PfHookState* h, void* self, void* ctx) {
    if (!self) return;
    const BYTE* p = (const BYTE*)self;
    const char* tag = h->kind ? "paint" : "op";
    char m[560];
    int n = snprintf(m, sizeof(m), "pf[%s]: first call self=%p ret=%p bytes=", tag, self,
                     ctx ? *(void**)((const BYTE*)ctx + 36) : (void*)0);
    for (int i = 0; i < 64 && n > 0 && n < (int)sizeof(m) - 4; ++i) n += snprintf(m + n, sizeof(m) - n, "%02x", p[i]);
    LogLine(m);
    int offs[8];
    const int found = M2_PfScanPosCandidates(p, 192, offs, 8);
    char c2[200] = {0};
    int cn = 0;
    for (int i = 0; i < found && cn < (int)sizeof(c2) - 14; ++i) {
        cn += snprintf(c2 + cn, sizeof(c2) - cn, "%s0x%X", i ? ", " : "", offs[i]);
    }
    char m2[320];
    snprintf(m2, sizeof(m2), "pf[%s]: pos candidates=%s (confirm via disasm, then M2_PF_POS%s=0x.. or 0x..+0x..)",
             tag, found ? c2 : "none", h->kind ? "_PAINT" : "_OP");
    LogLine(m2);
}

// The decision the byte stub calls: 1 = skip the engine call, 0 = run it (fail-open on any doubt).
static int M2_PfDecision(M2_PfHookState* h, void* self, void* ctx) {
    h->calls++;
    if (!h->probeDumped) {
        h->probeDumped = 1;
        M2_PfProbeDump(h, self, ctx);
    }
    if (g_pfMode < 2 || !h->gate || !self || !g_frustumReady) return 0;
    float pos[3];
    if (!M2_PfReadPos((const unsigned char*)self, h->hop, h->off1, h->off2, pos)) {
        h->uncertain++;
        return 0;
    }
    if (!M2_FrustumSphereVisible(&g_frustum, pos[0], pos[1], pos[2], g_pfRadius * g_pfMargin)) {
        h->culls++;
        return 1;   // fully off screen -> drop this emitter's update / paint work
    }
    return 0;
}

extern "C" int __cdecl M2_PfOpDecision(void* self, void* ctx)    { return M2_PfDecision(&g_pfOp, self, ctx); }
extern "C" int __cdecl M2_PfPaintDecision(void* self, void* ctx) { return M2_PfDecision(&g_pfPaint, self, ctx); }

// Shared installer (hookspec M2_InstallCandidateHook): every Wave-1 gate must pass, else log + unpatched.
static int M2_PfInstallTarget(M2_PfHookState* h, HMODULE mod, uintptr_t rva, void* decision) {
    char m[460];
    const char* tag = h->kind ? "paint" : "op";
    if (h->installed || !mod || !rva) return h->installed ? 1 : 0;
    const uintptr_t target = (uintptr_t)mod + rva;
    char sec[16] = {0};
    DWORD chars = 0;
    if (!M2_SectionForVa(target, sec, sizeof(sec), &chars)) {
        snprintf(m, sizeof(m), "pf[%s]: %p (rva 0x%lX) is not inside any section -> refusing to patch",
                 tag, (void*)target, (unsigned long)rva);
        LogLine(m);
        return 0;
    }
    if (!(chars & IMAGE_SCN_MEM_EXECUTE) || _stricmp(sec, ".text") != 0) {
        snprintf(m, sizeof(m), "pf[%s]: rva 0x%lX lives in section %s (chars 0x%lX) -> refusing to patch "
                 "(must be executable .text, never .reloc)", tag, (unsigned long)rva, sec, (unsigned long)chars);
        LogLine(m);
        return 0;
    }
    const BYTE* p = (const BYTE*)target;
    const int plen = M2_ZonePrologueLen(p);
    if (plen == 0) {
        snprintf(m, sizeof(m), "pf[%s]: rva 0x%lX prologue %02X %02X %02X %02X %02X %02X %02X %02X is not a "
                 "whitelisted shape -> not installed (disassemble it and extend M2_ZonePrologueLen)",
                 tag, (unsigned long)rva, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        LogLine(m);
        return 0;
    }
    if (!M2_PfSigMatches(p, h->sig)) {                   // S7: no-op-on-mismatch (hookspec A5)
        unsigned char cur[16];
        M2_PfSig16(p, cur);
        char a[40], b[40];
        int an = 0, bn = 0;
        for (int i = 0; i < 8; ++i) an += snprintf(a + an, sizeof(a) - an, "%02X", h->sig[i]);
        for (int i = 0; i < 8; ++i) bn += snprintf(b + bn, sizeof(b) - bn, "%02X", cur[i]);
        snprintf(m, sizeof(m), "pf[%s]: signature mismatch at rva 0x%lX (expected %s.. found %s..) -> running unpatched",
                 tag, (unsigned long)rva, a, b);
        LogLine(m);
        return 0;
    }
    if (!g_pfBlock) {
        g_pfBlock = VirtualAlloc(NULL, 1024, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_pfBlock) {
            LogLine("pf: VirtualAlloc for the stub block failed");
            return 0;
        }
        memset(g_pfBlock, 0xCC, 1024);
    }
    BYTE* tramp = (BYTE*)g_pfBlock + h->kind * 32;       // [0..plen) replayed prologue, then back-jump
    BYTE* slot  = (BYTE*)g_pfBlock + 64 + h->kind * 4;   // trampoline pointer slot
    BYTE* stub  = (BYTE*)g_pfBlock + 96 + h->kind * 128; // pushfd/pushad -> decision -> run | skip
    M2_ZoneEmitTrampoline(tramp, p, plen, (void*)(target + (uintptr_t)plen));
    const int stubLen = M2_PfEmitStub(stub, decision, (void*)slot, h->retBytes);
    *(void**)slot = (void*)tramp;
    if (memcmp((const void*)target, p, (size_t)plen) != 0) {
        LogLine("pf: target changed between probe and patch -> aborted");
        return 0;
    }
    BYTE jmp5[5];
    jmp5[0] = 0xE9;
    *(int32_t*)(jmp5 + 1) = (int32_t)((uintptr_t)stub - (target + 5));
    DWORD oldProt = 0;
    if (!VirtualProtect((void*)target, (SIZE_T)plen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        LogLine("pf: VirtualProtect on the target failed");
        return 0;
    }
    memcpy((void*)target, jmp5, 5);
    VirtualProtect((void*)target, (SIZE_T)plen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, (SIZE_T)plen);
    h->installed = 1;
    h->target = target;
    h->moduleBase = (DWORD)(uintptr_t)mod;
    snprintf(m, sizeof(m), "pf[%s]: installed rva 0x%lX section=%s prologue=%d stub=%d ret=%d%s gate=%d "
             "pos=%s radius=%.1f margin=%.2f",
             tag, (unsigned long)rva, sec, plen, stubLen, h->retBytes, h->retExplicit ? "(confirmed)" : "(ASSUMED 0!)",
             h->gate, h->off1 >= 0 ? (h->hop ? "chase" : "direct") : "unset(probe-only)", g_pfRadius, g_pfMargin);
    LogLine(m);
    return 1;
}

// ---- runtime scan (S1..S7, in-memory) ----
static const char* const kM2PfAnchors[] = {"ParticleFlow", "particle", ".pfx", "pf::", "emitter", "xParticleFlow"};
#define M2_PF_NANCHORS (int)(sizeof(kM2PfAnchors) / sizeof(kM2PfAnchors[0]))

// Records the function enclosing one evidence site (anchor xref, vtable call or IAT call) as a
// candidate; drawPath sites (vtable / IAT draw calls) additionally feed the paint auto-resolve set.
// Shared by all three evidence paths; never writes to the module.
static void M2_PfRecordStart(BYTE* base, DWORD lo, unsigned char* mem, int size, int siteOff, int drawPath,
                             M2ScanCandidate* out, int outCap, int* nCands, int* outTruncated,
                             uintptr_t* vtStarts, int vtMax, int* nVt) {
    int plen = 0;
    const int start = M2_PfWalkBackStart(mem, size, siteOff, 0x800, &plen);
    if (start < 0) return;
    const uintptr_t va = (uintptr_t)(base + lo + (DWORD)start);
    if (drawPath && vtStarts && vtMax > 0) {
        int dup = 0;
        for (int k = 0; k < *nVt; ++k) {
            if (vtStarts[k] == va) { dup = 1; break; }
        }
        if (!dup && *nVt < vtMax) vtStarts[(*nVt)++] = va;
    }
    for (int c = 0; c < *nCands; ++c) {
        if ((uintptr_t)out[c].va == va) return;           // already recorded
    }
    if (*nCands >= outCap) {
        if (outTruncated) *outTruncated = 1;
        return;
    }
    M2ScanCandidate* c = &out[(*nCands)++];
    memset(c, 0, sizeof(*c));
    c->va = (void*)va;
    c->rva = (uint32_t)(lo + (DWORD)start);
    M2_PfSig16((const unsigned char*)va, c->sig);
    c->prologueLen = (uint32_t)plen;
    int win = 0x800;
    if ((DWORD)start + (DWORD)win > (DWORD)size) win = size - (DWORD)start;
    M2_PfX87Density((const unsigned char*)va, win, &c->x87Reg, &c->x87All);
    c->insns = win;
    c->isExecutable = 1;
    c->inReloc = 0;
}

// Locates the IAT slot addresses of d3d9.dll draw imports inside a loaded module (max 4) for the S6
// IAT-indirect evidence path.  pf.dll may import d3d9.dll directly or route everything through
// render.dll's device pointer (recon finding); 0 slots simply disables this path.
static int M2_PfFindD3d9IatSlots(HMODULE mod, const char* const* funcs, int nFuncs, uintptr_t* slots) {
    if (!mod) return 0;
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((const char*)(base + d->Name), "d3d9.dll") != 0) continue;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        IMAGE_THUNK_DATA* names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk) : NULL;
        for (int i = 0; iat[i].u1.Function; ++i) {
            if (!names || IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            const char* nm = ((IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData))->Name;
            for (int f = 0; f < nFuncs; ++f) {
                if (strcmp(nm, funcs[f]) == 0 && n < 4) slots[n++] = (uintptr_t)&iat[i].u1.Function;
            }
        }
    }
    return n;
}

// Scans one loaded module.  Fills out[] with deduped candidates and (optionally) vtStarts[] with the
// deduped function starts containing device-vtable draw calls.  Never writes to the module.
static int M2_PfScanModuleInternal(HMODULE mod, const char* modName, const char* const* anchors, int nAnchors,
                                   M2ScanCandidate* out, int outCap, int* outTruncated,
                                   uintptr_t* vtStarts, int vtMax, const int* vtIdx, int nVtIdx) {
    if (outTruncated) *outTruncated = 0;
    if (!mod) return 0;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    uintptr_t anchorVas[24];
    int nAnchorVas = 0;
    int nCands = 0;
    int nStarts = 0;
    int iatSlots = 0;
    uintptr_t slots[4];
    static const char* const kDrawFuncs[2] = {"DrawPrimitiveUP", "DrawIndexedPrimitiveUP"};
    iatSlots = M2_PfFindD3d9IatSlots(mod, kDrawFuncs, 2, slots);
    char m[400];
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        char nm[9];
        memcpy(nm, s->Name, 8);
        nm[8] = 0;
        DWORD lo = s->VirtualAddress;
        DWORD size = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (lo >= nt->OptionalHeader.SizeOfImage) continue;
        if (lo + size > nt->OptionalHeader.SizeOfImage) size = nt->OptionalHeader.SizeOfImage - lo;
        if (size < 16) continue;
        unsigned char* mem = base + lo;
        const int isReloc = _stricmp(nm, ".reloc") == 0;
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE) && !isReloc &&
            (s->Characteristics & IMAGE_SCN_MEM_READ)) {
            // S2: string anchors in non-code data sections
            for (int a = 0; a < nAnchors && nAnchorVas < 24; ++a) {
                int off = M2_FindAsciiI(mem, (int)size, anchors[a]);
                if (off >= 0) {
                    anchorVas[nAnchorVas++] = (uintptr_t)(base + lo + (DWORD)off);
                    snprintf(m, sizeof(m), "pf-scan: %s anchor '%s' at rva 0x%X", modName, anchors[a], lo + off);
                    LogLine(m);
                }
            }
        }
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE) || isReloc) continue;
        // S3: imm32 xrefs to the anchor VAs (mov reg,imm / push imm / any disp32)
        for (int v = 0; v < nAnchorVas; ++v) {
            const uint32_t want = (uint32_t)anchorVas[v];
            int found = 0;
            for (DWORD k = 0; k + 4 <= size && found < 8; ++k) {
                if (*(const uint32_t*)(mem + k) != want) continue;
                ++found;
                M2_PfRecordStart(base, lo, mem, (int)size, (int)k, 0, out, outCap, &nCands, outTruncated,
                                 vtStarts, vtMax, &nStarts);
            }
        }
        // S6a: IAT-indirect draw calls (FF 15/25 [__imp_d3d9!Draw*UP]) - only when pf.dll imports d3d9
        for (int sl = 0; sl < iatSlots; ++sl) {
            uintptr_t sites[24];
            const int ns = M2_ScanIatCallSites(mem, (int)size, slots[sl], sites, 24);
            for (int j = 0; j < ns; ++j) {
                M2_PfRecordStart(base, lo, mem, (int)size, (int)sites[j], 1, out, outCap, &nCands, outTruncated,
                                 vtStarts, vtMax, &nStarts);
            }
            if (ns > 0) {
                snprintf(m, sizeof(m), "pf-scan: %s IAT Draw*UP call sites=%d", modName, ns);
                LogLine(m);
            }
        }
        // S6b: device-vtable draw call sites -> enclosing function starts (the PaintPoint fingerprint)
        for (int t = 0; t < nVtIdx; ++t) {
            uintptr_t sites[48];
            const int ns = M2_ScanVtCallSites(mem, (int)size, (uint32_t)(vtIdx[t] * 4), sites, 48);
            for (int j = 0; j < ns; ++j) {
                M2_PfRecordStart(base, lo, mem, (int)size, (int)sites[j], 1, out, outCap, &nCands, outTruncated,
                                 vtStarts, vtMax, &nStarts);
            }
            if (ns > 0) {
                snprintf(m, sizeof(m), "pf-scan: %s vtbl[%d] call sites=%d (disp 0x%X)", modName, vtIdx[t], ns,
                         vtIdx[t] * 4);
                LogLine(m);
            }
        }
    }
    if (nStarts > 0) {
        char m2[300];
        int cn = snprintf(m2, sizeof(m2), "pf-scan: %s draw-function starts:", modName);
        for (int k = 0; k < nStarts && k < 8 && vtStarts && cn < (int)sizeof(m2) - 16; ++k) {
            cn += snprintf(m2 + cn, sizeof(m2) - cn, "%s+0x%X", k ? "," : "",
                           (unsigned)(vtStarts[k] - (uintptr_t)base));
        }
        LogLine(m2);
    }
    return nCands;
}

// hookspec §3 surface: scan one module by name with the default or a custom anchor CSV.  Process-internal.
extern "C" int __cdecl M2_ScanModuleForAnchors(const char* moduleName, const char* anchorCsv,
                                               M2ScanCandidate* out, int outCap, int* outTruncated) {
    const char* anchors[12];
    char buf[256];
    int nAnchors = 0;
    if (anchorCsv && anchorCsv[0]) {
        snprintf(buf, sizeof(buf), "%s", anchorCsv);
        char* tok = strtok(buf, ",");
        while (tok && nAnchors < 12) {
            while (*tok == ' ') ++tok;
            if (*tok) anchors[nAnchors++] = tok;
            tok = strtok(NULL, ",");
        }
    } else {
        for (int i = 0; i < M2_PF_NANCHORS && i < 12; ++i) anchors[nAnchors++] = kM2PfAnchors[i];
    }
    if (nAnchors == 0) return 0;
    int vtIdx[1] = {83};
    return M2_PfScanModuleInternal(GetModuleHandleA(moduleName), moduleName, anchors, nAnchors,
                                   out, outCap, outTruncated, NULL, 0, vtIdx, 1);
}

static void M2_PfParseRvaSpec(const char* name, M2_PfTargetSpec* spec) {
    char v[96] = {0};
    spec->set = 0;
    spec->mod[0] = 0;
    spec->rva = 0;
    if (GetEnvironmentVariableA(name, v, sizeof(v)) <= 0 || !v[0]) return;
    char* colon = strchr(v, ':');
    unsigned rva = 0;
    if (colon) {
        *colon = 0;
        snprintf(spec->mod, sizeof(spec->mod), "%s", v);
        if (sscanf(colon + 1, "%x", &rva) == 1 && rva != 0 && rva < 0x10000000u) {
            spec->rva = rva;
            spec->set = 1;
        }
    } else if (sscanf(v, "%x", &rva) == 1 && rva != 0 && rva < 0x10000000u) {
        spec->rva = rva;
        spec->set = 1;
    }
}

static HMODULE M2_PfModuleFor(const M2_PfTargetSpec* spec) {
    if (spec->mod[0]) return GetModuleHandleA(spec->mod);
    for (int i = 0; i < g_pfNMods; ++i) {
        if (g_pfMods[i].h) return g_pfMods[i].h;
    }
    return NULL;
}

static void M2_PfArmGates(void) {
    g_pfOp.gate    = (g_pfMode == 2 && g_pfOp.retExplicit    && g_pfOp.off1 >= 0) ? 1 : 0;
    g_pfPaint.gate = (g_pfMode == 2 && g_pfPaint.retExplicit && g_pfPaint.off1 >= 0) ? 1 : 0;
}

static void M2_PfConfig(void) {
    char ev[16] = {0};
    char v[128] = {0};
    if (GetEnvironmentVariableA("M2_DISABLE_PARTICLE_CULL", ev, sizeof(ev)) > 0 && ev[0] == '1') g_pfKill = 1;
    if (GetEnvironmentVariableA("M2_DISABLE_CULLING", ev, sizeof(ev)) > 0 && ev[0] == '1') g_pfKill = 1;
    if (GetEnvironmentVariableA("M2_PARTICLE_CULL", ev, sizeof(ev)) > 0) {
        if (!_stricmp(ev, "probe")) g_pfMode = 1;
        else if (ev[0] == '1') g_pfMode = 2;
    }
    if (GetEnvironmentVariableA("M2_PF_SCAN", ev, sizeof(ev)) > 0) {
        if (!_stricmp(ev, "probe")) g_pfScan = 1;
        else if (ev[0] == '1') g_pfScan = 2;
    }
    if (GetEnvironmentVariableA("M2_DRAW_HISTOGRAM", ev, sizeof(ev)) > 0 && ev[0] == '1') g_pfHist = 1;
    if (GetEnvironmentVariableA("M2_PF_MODULE", v, sizeof(v)) > 0 && v[0]) {
        snprintf(g_pfModules, sizeof(g_pfModules), "%s", v);
    }
    if (GetEnvironmentVariableA("M2_PF_RADIUS", v, sizeof(v)) > 0) {
        const float f = (float)atof(v);
        if (f >= 0.5f && f <= 100000.0f) g_pfRadius = f;
    }
    if (GetEnvironmentVariableA("M2_PF_MARGIN", v, sizeof(v)) > 0) {
        const float f = (float)atof(v);
        if (f >= 1.0f && f <= 8.0f) g_pfMargin = f;
    }
    if (GetEnvironmentVariableA("M2_PF_VTINDEX", v, sizeof(v)) > 0 && v[0]) {
        g_pfNVt = 0;
        const char* s2 = v;
        while (g_pfNVt < 4 && *s2) {
            char* end = NULL;
            const unsigned long idx = strtoul(s2, &end, 10);
            if (end == s2) break;
            if (idx >= 3 && idx <= 127) g_pfVtIdx[g_pfNVt++] = (int)idx;
            s2 = end;
            while (*s2 == ',' || *s2 == ' ') ++s2;
        }
        if (g_pfNVt == 0) {
            g_pfVtIdx[0] = 83;
            g_pfVtIdx[1] = 84;
            g_pfNVt = 2;
        }
    }
    M2_PfParseRvaSpec("M2_PF_OPERATOR_RVA", &g_pfOpRva);
    M2_PfParseRvaSpec("M2_PF_PAINT_RVA", &g_pfPaintRva);
    char pos[64] = {0};
    if (GetEnvironmentVariableA("M2_PF_POS_OP", pos, sizeof(pos)) > 0) {
        M2_PfParsePosSpec(pos, &g_pfOp.hop, &g_pfOp.off1, &g_pfOp.off2);
    } else if (GetEnvironmentVariableA("M2_PF_POS", pos, sizeof(pos)) > 0) {
        M2_PfParsePosSpec(pos, &g_pfOp.hop, &g_pfOp.off1, &g_pfOp.off2);
    }
    if (GetEnvironmentVariableA("M2_PF_POS_PAINT", pos, sizeof(pos)) > 0) {
        M2_PfParsePosSpec(pos, &g_pfPaint.hop, &g_pfPaint.off1, &g_pfPaint.off2);
    } else if (GetEnvironmentVariableA("M2_PF_POS", pos, sizeof(pos)) > 0) {
        M2_PfParsePosSpec(pos, &g_pfPaint.hop, &g_pfPaint.off1, &g_pfPaint.off2);
    }
    if (GetEnvironmentVariableA("M2_PF_OP_RET", pos, sizeof(pos)) > 0) {
        const int b = atoi(pos);
        if (b >= 0 && b <= 64) {
            g_pfOp.retBytes = b;
            g_pfOp.retExplicit = 1;
        }
    }
    if (GetEnvironmentVariableA("M2_PF_PAINT_RET", pos, sizeof(pos)) > 0) {
        const int b = atoi(pos);
        if (b >= 0 && b <= 64) {
            g_pfPaint.retBytes = b;
            g_pfPaint.retExplicit = 1;
        }
    }
    M2_PfArmGates();
}

// Caches base/end of every configured module that is loaded; 0 while none are (retry from Present).
static int M2_PfCacheModules(void) {
    if (g_pfNMods > 0) return 1;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", g_pfModules);
    char* tok = strtok(buf, ",");
    int n = 0;
    int anyLoaded = 0;
    while (tok && n < 4) {
        while (*tok == ' ') ++tok;
        if (*tok) {
            HMODULE h = GetModuleHandleA(tok);
            if (h) {
                IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)h + ((IMAGE_DOS_HEADER*)h)->e_lfanew);
                snprintf(g_pfMods[n].name, sizeof(g_pfMods[n].name), "%s", tok);
                g_pfMods[n].h = h;
                g_pfMods[n].base = (uintptr_t)h;
                g_pfMods[n].end = (uintptr_t)h + nt->OptionalHeader.SizeOfImage;
                ++n;
                ++anyLoaded;
            }
        }
        tok = strtok(NULL, ",");
    }
    g_pfNMods = n;
    if (!anyLoaded) {
        if (!g_pfModsLogged) {
            g_pfModsLogged = 1;
            char m[200];
            snprintf(m, sizeof(m), "pf: %s not loaded yet -> hook/scan not installed (retrying)", g_pfModules);
            LogLine(m);
        }
        return 0;
    }
    return 1;
}

// One scan pass over every loaded pf module: logs candidates + collects the paint auto-resolve set.
static void M2_PfScanAll(M2ScanCandidate* cands, int* nCands, uintptr_t* paintStarts, int* nPaintStarts) {
    const char* anchors[12];
    char anchorBuf[256];
    int nAnchors = 0;
    char env[256] = {0};
    if (GetEnvironmentVariableA("M2_PF_ANCHORS", env, sizeof(env)) > 0 && env[0]) {
        snprintf(anchorBuf, sizeof(anchorBuf), "%s", env);
        char* tok = strtok(anchorBuf, ",");
        while (tok && nAnchors < 12) {
            while (*tok == ' ') ++tok;
            if (*tok) anchors[nAnchors++] = tok;
            tok = strtok(NULL, ",");
        }
    } else {
        for (int i = 0; i < M2_PF_NANCHORS && i < 12; ++i) anchors[nAnchors++] = kM2PfAnchors[i];
    }
    for (int i = 0; i < g_pfNMods; ++i) {
        int truncated = 0;
        uintptr_t vtStarts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int before = *nCands;
        int n = M2_PfScanModuleInternal(g_pfMods[i].h, g_pfMods[i].name, anchors, nAnchors,
                                        cands + *nCands, 24 - *nCands, &truncated,
                                        vtStarts, 8, g_pfVtIdx, g_pfNVt);
        *nCands += n;
        // paint candidates come from the PRIMARY module only (deterministic auto-resolve)
        if (i == 0) {
            for (int k = 0; k < 8 && vtStarts[k]; ++k) {
                if (*nPaintStarts < 8) paintStarts[(*nPaintStarts)++] = vtStarts[k];
            }
        }
        char m[300];
        snprintf(m, sizeof(m), "pf-scan: %s candidates=%d%s (rva, prologue, x87 reg/all):", g_pfMods[i].name,
                 n, truncated ? "+" : "");
        LogLine(m);
        for (int c = before; c < *nCands && c < before + 12; ++c) {
            snprintf(m, sizeof(m), "pf-scan: cand rva=0x%X sig=%02X%02X%02X%02X%02X%02X%02X%02X pro=%u x87=%d/%d win=%d",
                     cands[c].rva, cands[c].sig[0], cands[c].sig[1], cands[c].sig[2], cands[c].sig[3],
                     cands[c].sig[4], cands[c].sig[5], cands[c].sig[6], cands[c].sig[7],
                     cands[c].prologueLen, cands[c].x87Reg, cands[c].x87All, cands[c].insns);
            LogLine(m);
        }
    }
}

// Idempotent install pass, retried from Hook_Present until every configured module is loaded.
static void M2_PfInstall(void) {
    static int cfgDone = 0;
    if (g_pfDone) return;
    if (!cfgDone) {
        cfgDone = 1;
        M2_PfConfig();
    }
    char m[460];
    if (g_pfKill) {
        g_pfEvOn = 0;
        LogLine("pf: kill-switch (M2_DISABLE_PARTICLE_CULL=1 / M2_DISABLE_CULLING=1) -> particle culling off");
        g_pfDone = 1;
        return;
    }
    if (g_pfMode == 0 && g_pfScan == 0 && !g_pfHist) {
        LogLine("pf: disabled (set M2_PARTICLE_CULL=probe|1 to arm hooks, M2_PF_SCAN=probe|1 to scan)");
        g_pfDone = 1;
        return;
    }
    if (!M2_PfCacheModules()) return;                    // none of the pf modules is mapped yet
    g_pfEvOn = 1;

    M2ScanCandidate cands[24];
    int nCands = 0;
    uintptr_t paintStarts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int nPaintStarts = 0;
    if (g_pfScan >= 1) M2_PfScanAll(cands, &nCands, paintStarts, &nPaintStarts);

    // operator() target: explicit RVA only (the update path has no draw-call fingerprint)
    if (g_pfOpRva.set) {
        HMODULE hm = M2_PfModuleFor(&g_pfOpRva);
        if (hm) {
            M2_PfSig16((const BYTE*)hm + g_pfOpRva.rva, g_pfOp.sig);
            M2_PfInstallTarget(&g_pfOp, hm, g_pfOpRva.rva, (void*)&M2_PfOpDecision);
        } else {
            LogLine("pf[op]: module in M2_PF_OPERATOR_RVA is not loaded");
        }
    }
    // PaintPoint target: explicit RVA, or (M2_PF_SCAN=1) the unique vtbl-draw function of the primary module
    if (g_pfPaintRva.set) {
        HMODULE hm = M2_PfModuleFor(&g_pfPaintRva);
        if (hm) {
            M2_PfSig16((const BYTE*)hm + g_pfPaintRva.rva, g_pfPaint.sig);
            M2_PfInstallTarget(&g_pfPaint, hm, g_pfPaintRva.rva, (void*)&M2_PfPaintDecision);
        } else {
            LogLine("pf[paint]: module in M2_PF_PAINT_RVA is not loaded");
        }
    } else if (g_pfScan == 2 && nPaintStarts == 1 && g_pfMods[0].h) {
        const uintptr_t va = paintStarts[0];
        const uintptr_t rva = va - g_pfMods[0].base;
        M2_PfSig16((const unsigned char*)va, g_pfPaint.sig);
        snprintf(m, sizeof(m), "pf[paint]: auto-resolved rva 0x%lX in %s (unique vtbl[%s] draw function) - "
                 "pass-through until the gate opt-ins hold; confirm via disasm into M2_PF_PAINT_RVA",
                 (unsigned long)rva, g_pfMods[0].name, "83/84");
        LogLine(m);
        M2_PfInstallTarget(&g_pfPaint, g_pfMods[0].h, rva, (void*)&M2_PfPaintDecision);
    }

    snprintf(m, sizeof(m), "pf: pass done mode=%d scan=%d hist=%d op=%d paint=%d gate=op:%d/paint:%d "
             "(gate needs M2_PARTICLE_CULL=1 + M2_PF_POS[_OP/_PAINT] + M2_PF_OP_RET/M2_PF_PAINT_RET + frustum) "
             "radius=%.1f margin=%.2f",
             g_pfMode, g_pfScan, g_pfHist, g_pfOp.installed, g_pfPaint.installed, g_pfOp.gate, g_pfPaint.gate,
             g_pfRadius, g_pfMargin);
    LogLine(m);
    g_pfDone = 1;
}

static void M2_PfTelemetry(double tSec) {
    if ((g_pfMode == 0 && g_pfScan == 0 && !g_pfHist) || g_logFile == INVALID_HANDLE_VALUE) return;
    char line[1024];
    uint32_t oc = g_pfOp.calls, occ = g_pfOp.culls, ouc = g_pfOp.uncertain;
    uint32_t pc = g_pfPaint.calls, pcc = g_pfPaint.culls, puc = g_pfPaint.uncertain;
    g_pfOp.calls = g_pfOp.culls = g_pfOp.uncertain = 0;
    g_pfPaint.calls = g_pfPaint.culls = g_pfPaint.uncertain = 0;
    int len = snprintf(line, sizeof(line),
                       "# pf t=%.2f mode=%d kill=%d op(inst=%d calls=%u culls=%u unc=%u) "
                       "paint(inst=%d calls=%u culls=%u unc=%u) radius=%.1f margin=%.2f\n",
                       tSec, g_pfMode, g_pfKill, g_pfOp.installed, oc, occ, ouc,
                       g_pfPaint.installed, pc, pcc, puc, g_pfRadius, g_pfMargin);
    if (len > 0) {
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    }
    if (!g_pfEvOn) return;
    // caller-evidence histogram: which module+RVA issued the draw calls (the paint path fingerprint)
    const uint32_t n = g_pfUpN;
    uint32_t other = g_pfUpOther, full = g_pfUpFull;
    g_pfUpOther = g_pfUpFull = 0;
    len = snprintf(line, sizeof(line), "# pf-up t=%.2f sites=%u other=%u overflow=%u", tSec, n, other, full);
    for (uint32_t i = 0; i < n && i < 16 && len > 0 && len < (int)sizeof(line) - 64; ++i) {
        const uint32_t calls = g_pfUpSites[i].calls, prims = g_pfUpSites[i].prims, stride = g_pfUpSites[i].stride;
        g_pfUpSites[i].calls = 0;
        g_pfUpSites[i].prims = 0;
        if (calls == 0) continue;
        len += snprintf(line + len, sizeof(line) - len, " ; %s+0x%X n=%u prims=%u stride=%u",
                        g_pfUpSites[i].modIdx < (DWORD)g_pfNMods ? g_pfMods[g_pfUpSites[i].modIdx].name : "?",
                        g_pfUpSites[i].rva, calls, prims, stride);
    }
    if (len > 0) {
        len += snprintf(line + len, sizeof(line) - len, "\n");
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    }
}

// ---- Radical Splice 2: parallel entity quant (t_m2_multithread_quant) -------------------------------
#include "m2_quant_parallel.h"

static void __fastcall Hook_AppQuant(void* self, void* edx, int ms) {
    if (g_levelUp) {
        // Prevent 5x speed AI tick synchronization death spiral (all 300+ units firing AI + sight + repath on every frame):
        // Default m2Behaviour_UpdatePeriod (0x00ABED1C) = 0.05f, m2Behavoiur_UpdateFrequency (0x00ABED4C) = 1.0f.
        if (*(float*)0x00ABED1C < 0.09f) *(float*)0x00ABED1C = 0.10f;
        if (*(float*)0x00ABED4C > 0.55f) *(float*)0x00ABED4C = 0.50f;
    }
    g_inQuant = 1;
    int64_t t0 = Qpc();
    g_realAppQuant(self, edx, ms);
    g_quantTicks += Qpc() - t0;
    M2_R3QuantTick(M2_R3NowUs());   // Wave 3: moment the new simulation state became true
    g_inQuant = 0;
    g_quantCalls++;
    g_quantArgMs += (uint32_t)ms;
    if (g_qcGet && g_qcDeltaQuants) {
        void* qc = g_qcGet();
        if (qc) g_quantGameMs += (uint32_t)g_qcDeltaQuants(qc);  // game-ms this quant advanced
    }
}

static void InstallQuantHook(void) {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE sc = GetModuleHandleA("scene.dll");
    if (sc) g_qcDeltaQuants = (PFN_QcInt)GetProcAddress(sc, "?DeltaQuants@eQuantControl@xScene@@QBEHXZ");
    int n = PatchIAT(GetModuleHandleA(NULL), "app.dll", "?Quant@eApplication@xApp@@MAEXH@Z", (void*)Hook_AppQuant,
                     (void**)&g_realAppQuant);
    char m[160];
    snprintf(m, sizeof(m), "quant hook: Majesty2.exe IAT app.dll!eApplication::Quant patched=%d DeltaQuants getter=%p",
             n, (void*)g_qcDeltaQuants);
    LogLine(m);
    InstallEngineOptimizations();
}

// ---- In-process x86 guest EIP profiler ----------------------------------------------------------------
// Host samplers only see Rosetta's translated ARM64 code, so we sample the guest instead: a background thread
// suspends the render/main thread every ~3 ms, reads its x86 CONTEXT, copies the top of its stack, and resumes it.
// While the target is suspended we only call GetThreadContext and memcpy within its stack bounds (no allocation, no
// locks, no faulting reads), so a target suspended inside the heap / loader lock cannot deadlock us.
// Callers come from two sources: the EBP chain (4 levels, frames must stay inside [Esp, StackBase)) and a stack scan
// for return addresses that follow a CALL in an executable section (catches FPO frames the EBP chain skips).
#define PROF_SEC        7
#define PROF_MAX        8192
#define PROF_EBP_DEPTH  4
#define PROF_SCAN_BYTES 16384
#define PROF_RETS       32
enum { PH_OTHER, PH_QUANT, PH_PRESENT };

struct ProfSample {
    DWORD eip, esp, phase, nRets;
    DWORD ebpRet[PROF_EBP_DEPTH];
    DWORD rets[PROF_RETS];
};
static ProfSample g_prof[PROF_MAX];
static DWORD g_profStack[PROF_SCAN_BYTES / 4];
static HANDLE g_mainThread = NULL;
static DWORD g_mainTid = 0, g_mainStackBase = 0;
static volatile LONG g_profStarted = 0;

struct ProfModule { char name[64]; DWORD base, end; DWORD textLo[8], textHi[8]; int nText; };
static ProfModule g_mods[256];
static int g_nMods = 0;

static void ProfLoadModules(void) {
    typedef BOOL (WINAPI *PFN_Enum)(HANDLE, HMODULE*, DWORD, DWORD*);
    PFN_Enum en = (PFN_Enum)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules");
    static HMODULE mods[256];
    DWORD need = 0;
    if (!en || !en(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    g_nMods = 0;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && g_nMods < 256; ++i) {
        BYTE* b = (BYTE*)mods[i];
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        ProfModule& m = g_mods[g_nMods++];
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(mods[i], path, sizeof(path));
        const char* s = strrchr(path, '\\');
        snprintf(m.name, sizeof(m.name), "%s", s ? s + 1 : path);
        m.base = (DWORD)(DWORD_PTR)b;
        m.end = m.base + nt->OptionalHeader.SizeOfImage;
        m.nText = 0;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (int k = 0; k < nt->FileHeader.NumberOfSections && m.nText < 8; ++k) {
            if (!(sec[k].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            m.textLo[m.nText] = m.base + sec[k].VirtualAddress;
            m.textHi[m.nText] = m.textLo[m.nText] + sec[k].Misc.VirtualSize;
            ++m.nText;
        }
    }
}

static const ProfModule* ProfFindModule(DWORD va) {
    for (int i = 0; i < g_nMods; ++i)
        if (va >= g_mods[i].base && va < g_mods[i].end) return &g_mods[i];
    return NULL;
}

// True if va is in an executable section and the bytes before it encode a CALL (so va is a plausible return address).
static bool ProfIsReturnAddr(DWORD va) {
    const ProfModule* m = ProfFindModule(va);
    if (!m) return false;
    bool inText = false;
    for (int k = 0; k < m->nText; ++k)
        if (va >= m->textLo[k] + 7 && va < m->textHi[k]) inText = true;
    if (!inText) return false;
    const BYTE* p = (const BYTE*)(DWORD_PTR)va;
    if (p[-5] == 0xE8) return true;                                              // call rel32
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;                    // call [disp32] / [reg+disp32]
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10 && (p[-6] & 7) == 4) return true; // call [sib+disp32]
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return true;                    // call [reg+disp8]
    if (p[-4] == 0xFF && p[-3] == 0x54) return true;                             // call [sib+disp8]
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return true; // call reg / [reg]
    if (p[-3] == 0xFF && p[-2] == 0x14) return true;                             // call [sib]
    return false;
}

// Nearest preceding export of the module containing va ("" if none within 64 KB).
static void ProfNearestExport(const ProfModule* m, DWORD va, char* out, size_t cap) {
    out[0] = 0;
    if (!m) return;
    BYTE* b = (BYTE*)(DWORD_PTR)m->base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return;
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(b + dir.VirtualAddress);
    DWORD* funcs = (DWORD*)(b + ed->AddressOfFunctions);
    DWORD* names = (DWORD*)(b + ed->AddressOfNames);
    WORD* ords = (WORD*)(b + ed->AddressOfNameOrdinals);
    DWORD rva = va - m->base, best = 0;
    const char* bestName = NULL;
    for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
        DWORD f = funcs[ords[i]];
        if (f <= rva && f >= best) { best = f; bestName = (const char*)(b + names[i]); }
    }
    if (bestName && rva - best < 0x10000) snprintf(out, cap, "%s+0x%lX", bestName, (unsigned long)(rva - best));
}

static void ProfFormatAddr(DWORD va, char* out, size_t cap) {
    const ProfModule* m = ProfFindModule(va);
    char exp[200];
    ProfNearestExport(m, va, exp, sizeof(exp));
    if (m) snprintf(out, cap, "%08lX %-14s +%06lX  %s", (unsigned long)va, m->name, (unsigned long)(va - m->base), exp);
    else snprintf(out, cap, "%08lX <no module>", (unsigned long)va);
}

struct KeyCount { DWORD key, count; };
static KeyCount g_kc[PROF_MAX * PROF_RETS];
static DWORD g_keys[PROF_MAX * PROF_RETS];

static int CmpDword(const void* a, const void* b) {
    DWORD x = *(const DWORD*)a, y = *(const DWORD*)b;
    return x < y ? -1 : x > y;
}
static int CmpCountDesc(const void* a, const void* b) {
    const KeyCount* x = (const KeyCount*)a; const KeyCount* y = (const KeyCount*)b;
    return x->count != y->count ? (x->count < y->count ? 1 : -1) : (x->key < y->key ? -1 : x->key > y->key);
}

// Counts g_keys[0..n) and writes the top `top` entries as a table section.
static void ProfWriteTop(HANDLE f, const char* title, int n, int top, DWORD denom) {
    qsort(g_keys, n, sizeof(DWORD), CmpDword);
    int u = 0;
    for (int i = 0; i < n; ++i) {
        if (u && g_kc[u - 1].key == g_keys[i]) g_kc[u - 1].count++;
        else { g_kc[u].key = g_keys[i]; g_kc[u].count = 1; ++u; }
    }
    qsort(g_kc, u, sizeof(KeyCount), CmpCountDesc);
    char line[512];
    DWORD w = 0;
    int len = snprintf(line, sizeof(line), "\n== %s (%d keys, %% of %lu samples) ==\n", title, u, (unsigned long)denom);
    WriteFile(f, line, len, &w, NULL);
    for (int i = 0; i < u && i < top; ++i) {
        char addr[320];
        ProfFormatAddr(g_kc[i].key, addr, sizeof(addr));
        len = snprintf(line, sizeof(line), "%3d %6lu %6.2f%%  %s\n", i + 1, (unsigned long)g_kc[i].count,
                       100.0 * g_kc[i].count / (denom ? denom : 1), addr);
        WriteFile(f, line, len, &w, NULL);
    }
}

static DWORD WINAPI ProfilerThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ProfLoadModules();
    int n = 0, failed = 0;
    int64_t t0 = Qpc(), tEnd = t0 + (int64_t)PROF_SEC * g_qpcFreq.QuadPart, suspendTicks = 0;
    while (n < PROF_MAX && Qpc() < tEnd) {
        Sleep(3);
        int64_t ts = Qpc();
        if (SuspendThread(g_mainThread) == (DWORD)-1) { ++failed; continue; }
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(g_mainThread, &ctx)) { ResumeThread(g_mainThread); ++failed; continue; }
        ProfSample& s = g_prof[n];
        s.eip = ctx.Eip;
        s.esp = ctx.Esp;
        s.phase = g_inPresent ? PH_PRESENT : g_inQuant ? PH_QUANT : PH_OTHER;
        DWORD lo = ctx.Esp, hi = g_mainStackBase;
        DWORD copy = (lo >= hi || lo < 0x10000 || (hi - lo) > 0x40000) ? 0 : (hi - lo);
        if (copy > 4096) copy = 4096;
        copy &= ~3u;
        if (copy > 0) {
            memcpy(g_profStack, (void*)(DWORD_PTR)lo, copy);
        }
        ResumeThread(g_mainThread);
        suspendTicks += Qpc() - ts;

        DWORD ebp = ctx.Ebp;
        for (int d = 0; d < PROF_EBP_DEPTH; ++d) {
            DWORD ret = 0;
            if (copy >= 8 && ebp >= lo && ebp + 8 <= lo + copy && !(ebp & 3)) {
                DWORD idx = (ebp - lo) >> 2;
                ret = g_profStack[idx + 1];
                DWORD next = g_profStack[idx];
                ebp = next > ebp ? next : 0;  // frames must move up the stack
            } else {
                ebp = 0;
            }
            s.ebpRet[d] = ret;
        }
        // Return addresses in the copied stack window (after resume: only reads immutable code bytes).
        s.nRets = 0;
        for (DWORD i = 0; i < copy / 4 && s.nRets < PROF_RETS; ++i) {
            DWORD v = g_profStack[i];
            if (ProfIsReturnAddr(v)) s.rets[s.nRets++] = v;
        }
        ++n;
    }
    double sec = (double)(Qpc() - t0) / (double)g_qpcFreq.QuadPart;

    // Raw samples for offline analysis (tools/m2_x86_profile.py).
    HANDLE raw = CreateFileA("Z:\\tmp\\m2_x86_samples.bin", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0;
    if (raw != INVALID_HANDLE_VALUE) {
        WriteFile(raw, g_prof, n * sizeof(ProfSample), &w, NULL);
        CloseHandle(raw);
    }

    ProfLoadModules();  // modules loaded during the window
    HANDLE f = CreateFileA("Z:\\tmp\\m2_x86_profile.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { LogLine("x86 profiler: cannot create Z:\\tmp\\m2_x86_profile.txt"); return 0; }
    int ph[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i) ph[g_prof[i].phase]++;
    char line[512];
    int len = snprintf(line, sizeof(line),
                       "# Majesty 2 in-process x86 guest EIP profile\n"
                       "# samples=%d failed=%d window=%.2fs interval=%.2fms avg_suspend=%.1fus main_tid=%lu stack_base=%08lX\n"
                       "# phase split: quant(eApplication::Quant)=%.1f%% present=%.1f%% other(render/ui/loop)=%.1f%%\n",
                       n, failed, sec, n ? sec * 1000.0 / n : 0.0,
                       n ? (double)suspendTicks * 1e6 / (double)g_qpcFreq.QuadPart / n : 0.0, (unsigned long)g_mainTid,
                       (unsigned long)g_mainStackBase, 100.0 * ph[PH_QUANT] / (n ? n : 1),
                       100.0 * ph[PH_PRESENT] / (n ? n : 1), 100.0 * ph[PH_OTHER] / (n ? n : 1));
    WriteFile(f, line, len, &w, NULL);
    for (int i = 0; i < g_nMods; ++i) {
        len = snprintf(line, sizeof(line), "# module %-24s base=%08lX end=%08lX\n", g_mods[i].name,
                       (unsigned long)g_mods[i].base, (unsigned long)g_mods[i].end);
        WriteFile(f, line, len, &w, NULL);
    }

    // Per-module self time.
    for (int i = 0; i < n; ++i) {
        const ProfModule* m = ProfFindModule(g_prof[i].eip);
        g_keys[i] = m ? m->base : 0;
    }
    ProfWriteTop(f, "self time by module (base address)", n, 50, n);

    for (int i = 0; i < n; ++i) g_keys[i] = g_prof[i].eip;
    ProfWriteTop(f, "top-50 exact EIP (self)", n, 50, n);
    for (int i = 0; i < n; ++i) g_keys[i] = g_prof[i].eip & ~0xFu;
    ProfWriteTop(f, "top-50 EIP & ~0xF (self, 16-byte buckets)", n, 50, n);

    int k = 0;
    for (int i = 0; i < n; ++i) if (g_prof[i].ebpRet[0]) g_keys[k++] = g_prof[i].ebpRet[0];
    ProfWriteTop(f, "top-50 callers [EBP+4] (level 1)", k, 50, n);
    k = 0;
    for (int i = 0; i < n; ++i)
        for (int d = 0; d < PROF_EBP_DEPTH; ++d) if (g_prof[i].ebpRet[d]) g_keys[k++] = g_prof[i].ebpRet[d];
    ProfWriteTop(f, "top-50 EBP-chain return sites, levels 1-4", k, 50, n);

    // Inclusive: each distinct return address once per sample.
    k = 0;
    for (int i = 0; i < n; ++i) {
        ProfSample& s = g_prof[i];
        for (DWORD a = 0; a < s.nRets; ++a) {
            bool dup = false;
            for (DWORD b = 0; b < a; ++b) if (s.rets[b] == s.rets[a]) { dup = true; break; }
            if (!dup) g_keys[k++] = s.rets[a];
        }
    }
    ProfWriteTop(f, "top-50 inclusive return sites (stack scan, top 16 KB)", k, 50, n);

    k = 0;
    for (int i = 0; i < n; ++i) if (g_prof[i].phase == PH_QUANT) g_keys[k++] = g_prof[i].eip & ~0xFu;
    ProfWriteTop(f, "top-50 EIP & ~0xF inside eApplication::Quant only", k, 50, ph[PH_QUANT]);
    k = 0;
    for (int i = 0; i < n; ++i) if (g_prof[i].phase == PH_OTHER) g_keys[k++] = g_prof[i].eip & ~0xFu;
    ProfWriteTop(f, "top-50 EIP & ~0xF outside Quant/Present (render, ui, loop)", k, 50, ph[PH_OTHER]);
    CloseHandle(f);

    snprintf(line, sizeof(line), "x86 profiler: done, %d samples in %.2fs (quant %.1f%%, present %.1f%%) -> "
             "Z:\\tmp\\m2_x86_profile.txt", n, sec, 100.0 * ph[PH_QUANT] / (n ? n : 1),
             100.0 * ph[PH_PRESENT] / (n ? n : 1));
    LogLine(line);
    return 0;
}

// Called from Present: starts the profiler 2 s after 5x speed is active when M2_PROFILE=1 is set.
static void ProfilerTick(const LARGE_INTEGER& now) {
    if (g_profStarted || !g_speed5Tick.QuadPart) return;
    if (now.QuadPart - g_speed5Tick.QuadPart < 2 * g_qpcFreq.QuadPart) return;
    g_profStarted = 1;
    char pev[8] = {0};
    if (!(GetEnvironmentVariableA("M2_PROFILE", pev, sizeof(pev)) > 0 && pev[0] == '1')) {
        return;
    }
    char m[200];
    if (GetCurrentThreadId() != g_mainTid) {  // profile the thread that actually drives the frame loop
        snprintf(m, sizeof(m), "x86 profiler: Present tid %lu != CreateDevice tid %lu, re-targeting",
                 GetCurrentThreadId(), (unsigned long)g_mainTid);
        LogLine(m);
        if (g_mainThread) CloseHandle(g_mainThread);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        g_mainTid = GetCurrentThreadId();
        g_mainStackBase = (DWORD)(DWORD_PTR)((NT_TIB*)NtCurrentTeb())->StackBase;
    }
    HANDLE th = g_mainThread ? CreateThread(NULL, 0, ProfilerThread, NULL, 0, NULL) : NULL;
    snprintf(m, sizeof(m), "x86 profiler: started=%d (%d s, Sleep(3) interval, main tid %lu)", th != NULL, PROF_SEC,
             (unsigned long)g_mainTid);
    LogLine(m);
    if (th) CloseHandle(th);
}

// ================== Wave 3: decoupled render loop (present pacing + sub-tick camera lerp) ==================
// The GEM3 main loop is `while (accumulator >= tick) Quant(tick); render(); Present();`.  Quant() is the
// expensive half (300+ units x AI + sight + repath, x87-heavy under Rosetta 2) and everything ordered
// behind it - the camera write, the draws and the swap - is only as smooth as the simulation cadence.
// Wave 3 never touches the simulation.  It only decides *when* the proxy flips the swap chain, and how
// far the camera transform is advanced on frames that render between two simulation ticks.
//
// Fail-open contract (same discipline as Waves 1/2 - a broken measurement degrades to stock behaviour):
//   * M2_R3ExtrapCamera() returning 0 makes Hook_SetTransform forward the engine's own matrix verbatim,
//     so a rejected blend is byte-identical to the stock path.
//   * Only the camera translation moves: the rotation basis is copied untouched, so a blend can never
//     roll or skew the world.  The worst case is a camera positioned at most `ticksMax` simulation ticks
//     of measured motion ahead of the simulation.
//   * M2_R3PresentGate() returns "present" for every argument shape it does not understand, and the
//     redundant-present rule only arms after it has once seen a healthy count of real draw calls
//     between two Presents, so a missing draw counter can never freeze the frame.
//   * M2_RENDER_PROBE=1 measures and reports every decision without ever changing one.
//
// ---- M2_RENDER_CORE_BEGIN ----
// Self-contained float/int-only block (no Windows types, no globals, no logging) so that
// scripts/test_wave3_render.py can extract it verbatim, compile it natively and check it against an
// independent Python reference.  Keep every dependency inside these two markers.
//
// A D3D9 view matrix is row-major with v_view = v_world * V: rows 0..2 of the 3x3 block are the camera
// basis and row 3 is -(eye . R), so
//     eye[j] = -(V[12]*V[j] + V[13]*V[4+j] + V[14]*V[8+j])     (j = 0, 1, 2)
// This is exactly the extraction Wave 1 uses for g_frustum.eye (M2_EyeForward), so the interpolation
// core and the culling core can never disagree about where the camera is.  The inverse is exact:
//     V[3*4+j] = -(V[j]*eye[0] + V[4+j]*eye[1] + V[8+j]*eye[2])

#define M2_R3_CAM_MIN_DOT   0.98f   // prev/cur must share a basis, else they are not the same camera
#define M2_R3_CAM_MIN_MOVE  1.0e-5f // below this the camera is static and the blend is a no-op

static float M2_R3Absf(float v) { return __builtin_fabsf(v); }
static float M2_R3Sqrtf(float v) { return __builtin_sqrtf(v); }
static int M2_R3Finite3(const float* v) {
    if (!(v[0] == v[0]) || !(v[1] == v[1]) || !(v[2] == v[2])) return 0;
    if (M2_R3Absf(v[0]) > 1.0e9f || M2_R3Absf(v[1]) > 1.0e9f || M2_R3Absf(v[2]) > 1.0e9f) return 0;
    return 1;
}

// Camera basis column j = (m[j], m[4+j], m[8+j]); rows are the basis vectors, so the dot products
// below are the off-diagonals of R^T R and must vanish for an orthonormal camera basis.
static int M2_R3ViewPlausible(const float* m) {
    if (M2_R3Absf(m[15] - 1.0f) > 1.0e-4f) return 0;                 // affine bottom row
    if (!(m[12] == m[12]) || !(m[13] == m[13]) || !(m[14] == m[14])) return 0;   // NaN translation
    if (M2_R3Absf(m[12]) > 1.0e9f || M2_R3Absf(m[13]) > 1.0e9f || M2_R3Absf(m[14]) > 1.0e9f) return 0;
    for (int j = 0; j < 3; ++j) {
        float x = m[j], y = m[4 + j], z = m[8 + j];
        float len = M2_R3Sqrtf(x * x + y * y + z * z);
        if (!(len > 0.999f && len < 1.001f)) return 0;               // unit basis vector
    }
    if (M2_R3Absf(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]) > 0.1f) return 0;
    if (M2_R3Absf(m[4] * m[8] + m[5] * m[9] + m[6] * m[10]) > 0.1f) return 0;
    if (M2_R3Absf(m[8] * m[0] + m[9] * m[1] + m[10] * m[2]) > 0.1f) return 0;
    return 1;
}

static void M2_R3CamEye(const float* m, float* eye) {
    float tx = m[12], ty = m[13], tz = m[14];
    eye[0] = -(tx * m[0] + ty * m[4] + tz * m[8]);
    eye[1] = -(tx * m[1] + ty * m[5] + tz * m[9]);
    eye[2] = -(tx * m[2] + ty * m[6] + tz * m[10]);
}
static void M2_R3CamSetEye(float* m, const float* eye) {
    m[12] = -(m[0] * eye[0] + m[1] * eye[1] + m[2] * eye[2]);
    m[13] = -(m[4] * eye[0] + m[5] * eye[1] + m[6] * eye[2]);
    m[14] = -(m[8] * eye[0] + m[9] * eye[1] + m[10] * eye[2]);
}
// Mean basis alignment of two views: 1.0 = identical orientation, 0 = orthogonal.  A shadow / ortho /
// full-screen 2D pass that shares the frame with the main camera scores far below the threshold.
static float M2_R3BasisOverlap(const float* a, const float* b) {
    float d = 0.0f;
    for (int j = 0; j < 3; ++j) d += a[j] * b[j] + a[4 + j] * b[4 + j] + a[8 + j] * b[8 + j];
    return d * (1.0f / 3.0f);
}

// Advances the camera of `cur` by `ticks` times the per-tick translation delta measured from `prev`.
// `span` is how many simulation ticks separate prev from cur (>= 1); the delta is divided by it so the
// extrapolation is always at most `ticksMax` ticks of *real* motion, however many ticks the frame ran.
// Returns 1 when out was written, 0 when the caller must hand the engine matrix to the device untouched.
static int M2_R3ExtrapCamera(const float* prev, const float* cur, float ticks, uint32_t span,
                             float stepMax, float ticksMax, float minDot, float* out) {
    if (!prev || !cur || !out) return 0;
    if (!M2_R3ViewPlausible(prev) || !M2_R3ViewPlausible(cur)) return 0;
    if (!(M2_R3BasisOverlap(prev, cur) >= minDot)) return 0;         // not the same camera
    float ep[3], ec[3];
    M2_R3CamEye(prev, ep);
    M2_R3CamEye(cur, ec);
    if (!M2_R3Finite3(ep) || !M2_R3Finite3(ec)) return 0;
    float d[3];
    d[0] = ec[0] - ep[0]; d[1] = ec[1] - ep[1]; d[2] = ec[2] - ep[2];
    if (!M2_R3Finite3(d)) return 0;
    float inv = 1.0f / (float)(span >= 1 ? span : 1);
    d[0] *= inv; d[1] *= inv; d[2] *= inv;
    float dl = M2_R3Sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl > M2_R3_CAM_MIN_MOVE)) return 0;                        // camera static: nothing to blend
    if (!(dl <= stepMax)) return 0;                                  // teleport / cut scene / level load
    float f = ticks;
    if (!(f >= 0.0f)) return 0;                                      // NaN or negative: hold
    if (f > ticksMax) f = ticksMax;
    if (!(f > 0.0f)) return 0;                                       // "now": the engine matrix is exact
    float e[3];
    e[0] = ec[0] + d[0] * f; e[1] = ec[1] + d[1] * f; e[2] = ec[2] + d[2] * f;
    if (!M2_R3Finite3(e)) return 0;
    for (int i = 0; i < 16; ++i) out[i] = cur[i];
    M2_R3CamSetEye(out, e);
    return 1;
}

// Present pacing decision (pure).  nowUs / lastPresentUs are microseconds.  Returns 1 = present,
// 0 = skip.  limitHz <= 0 and minDraws == 0 both disable their rule, and any implausible clock
// (lastPresentUs <= 0, nowUs <= lastPresentUs) presents unconditionally.
static int M2_R3PresentGate(int64_t nowUs, int64_t lastPresentUs, int limitHz, uint32_t drawsSincePresent,
                            uint32_t minDraws) {
    if (limitHz > 0 && lastPresentUs > 0) {
        int64_t interval = 1000000LL / (int64_t)limitHz;
        if (nowUs > lastPresentUs && (nowUs - lastPresentUs) < interval) return 0;
    }
    if (minDraws > 0 && drawsSincePresent < minDraws) return 0;
    return 1;
}
// ---- M2_RENDER_CORE_END ----

// ---- Windows layer: seqlocked camera snapshot, tick clock, present gate, telemetry ----
struct M2_R3Copy {
    float  prev[16];    // engine view matrix as of the tick before last
    float  cur[16];     // engine view matrix as of the last tick
    int64_t tickEndUs;  // QPC (us) right after the most recent Quant returned
    int64_t tickUs;     // EMA of the inter-Quant wall interval, us
    uint32_t span;      // simulation ticks between prev and cur
    uint32_t nQuants;   // Quants since `cur` was last produced by the engine
    int    havePrev, haveCur;
};
struct M2_R3Snap {
    volatile long seq;  // seqlock: odd while a writer is inside the payload
    M2_R3Copy c;
};
static M2_R3Snap g_r3;

static void M2_R3WriteBegin(void) { __sync_fetch_and_add(&g_r3.seq, 1); }
static void M2_R3WriteEnd(void)   { __sync_synchronize(); __sync_fetch_and_add(&g_r3.seq, 1); }

// Returns 1 and fills `out` when a consistent snapshot was read, 0 when a writer was mid-update.
static int M2_R3SnapRead(M2_R3Copy* out) {
    long s1 = g_r3.seq;
    if (s1 & 1) return 0;
    __sync_synchronize();
    for (int i = 0; i < 16; ++i) { out->prev[i] = g_r3.c.prev[i]; out->cur[i] = g_r3.c.cur[i]; }
    out->tickEndUs = g_r3.c.tickEndUs;
    out->tickUs = g_r3.c.tickUs;
    out->span = g_r3.c.span;
    out->nQuants = g_r3.c.nQuants;
    out->havePrev = g_r3.c.havePrev;
    out->haveCur = g_r3.c.haveCur;
    __sync_synchronize();
    return (s1 == g_r3.seq) ? 1 : 0;
}

// Wave 3 config, read once on the first hook that needs it (env is always set by then).
static int g_r3InitDone = 0;
static int g_r3LimitHz = 0;                  // M2_FRAME_LIMIT: 0 = uncapped
static int g_r3Probe = 0;                    // M2_RENDER_PROBE=1: measure, never change a decision
static float g_r3CamStep = 40.0f;            // M2_CAM_LERP_MAX_STEP: per-tick camera motion, world units
static float g_r3CamTicks = 0.5f;            // M2_CAM_LERP_MAX_TICKS: how far ahead the camera may render
static float g_r3CamDot = M2_R3_CAM_MIN_DOT; // M2_CAM_LERP_MIN_DOT

static volatile uint32_t g_r3DrawsAtP = 0;      // snapshot at the last presented frame
static volatile uint32_t g_r3Presented = 0, g_r3Skipped = 0, g_r3WouldSkip = 0;
static volatile uint32_t g_r3CamBlend = 0, g_r3CamHold = 0;
static volatile uint32_t g_r3DrawSec = 0;       // draw calls seen by presented frames this second
static uint32_t g_r3FramesSec = 0;
static int g_r3Armed = 0;                       // one-way latch for the redundant-present rule
static float g_r3CamTicksMaxSeen = 0.0f;

// QPC <-> microseconds without a float on the hot path (and safe when the frequency was never captured).
static int64_t M2_R3TickToUs(int64_t ticks) {
    if (ticks <= 0 || g_qpcFreq.QuadPart <= 0) return 0;
    return (int64_t)((uint64_t)ticks * 1000000ULL / (uint64_t)g_qpcFreq.QuadPart);
}
static int64_t M2_R3NowUs(void) {
    if (g_qpcFreq.QuadPart <= 0) return 0;
    return M2_R3TickToUs(Qpc());
}

// Fraction of a simulation tick elapsed since the current camera state became true (0 when unmeasurable).
static float M2_R3TicksFrom(int64_t nowUs, const M2_R3Copy* c) {
    if (c->tickUs <= 0 || c->tickEndUs <= 0 || nowUs <= c->tickEndUs) return 0.0f;
    return (float)((nowUs - c->tickEndUs) / c->tickUs);
}

static void M2_R3Config(void) {
    char ev[24] = {0}, v[40] = {0};
    int off = 0;
    if (GetEnvironmentVariableA("M2_DISABLE_RENDER_PACING", ev, sizeof(ev)) > 0 && ev[0] == '1') off = 1;
    if (GetEnvironmentVariableA("M2_FRAME_LIMIT", v, sizeof(v)) > 0) {
        const int hz = atoi(v);
        if (hz >= 20 && hz <= 1000) g_r3LimitHz = hz;
    }
    if (GetEnvironmentVariableA("M2_PRESENT_MIN_DRAWS", v, sizeof(v)) > 0) {
        const int d = atoi(v);
        if (d >= 1 && d <= 64) g_r3MinDraws = (unsigned)d;
    }
    if (GetEnvironmentVariableA("M2_CAM_LERP", ev, sizeof(ev)) > 0)
        g_r3Cam = (ev[0] == '1' || ev[0] == 'p' || ev[0] == 'P') ? 1 : 0;
    if (GetEnvironmentVariableA("M2_RENDER_PROBE", ev, sizeof(ev)) > 0 && ev[0] == '1') g_r3Probe = 1;
    if (GetEnvironmentVariableA("M2_CAM_LERP_MAX_STEP", v, sizeof(v)) > 0) {
        const float f = (float)atof(v);
        if (f >= 0.5f && f <= 5000.0f) g_r3CamStep = f;
    }
    if (GetEnvironmentVariableA("M2_CAM_LERP_MAX_TICKS", v, sizeof(v)) > 0) {
        const float f = (float)atof(v);
        if (f >= 0.05f && f <= 2.0f) g_r3CamTicks = f;
    }
    if (GetEnvironmentVariableA("M2_CAM_LERP_MIN_DOT", v, sizeof(v)) > 0) {
        const float f = (float)atof(v);
        if (f >= 0.5f && f <= 1.0f) g_r3CamDot = f;
    }
    if (off) {
        g_r3LimitHz = 0; g_r3MinDraws = 0; g_r3Cam = 0; g_r3Probe = 0;
        LogLine("render: pacing disabled by M2_DISABLE_RENDER_PACING=1");
        return;
    }
    {
        char m[320];
        snprintf(m, sizeof(m), "render: pacing frame_limit=%d min_draws=%u cam_lerp=%d probe=%d max_step=%.1f "
                 "max_ticks=%.2f min_dot=%.2f", g_r3LimitHz, g_r3MinDraws, g_r3Cam, g_r3Probe,
                 g_r3CamStep, g_r3CamTicks, g_r3CamDot);
        LogLine(m);
    }
}

static void M2_R3Init(void) {
    if (g_r3InitDone) return;
    g_r3InitDone = 1;
    M2_R3Config();
}

// Called from Hook_AppQuant right after the real Quant returns: anchors the moment the current
// simulation state became true and measures the simulation cadence.  The interval EMA attacks fast
// (tracking the per-tick cadence inside a Quant burst) and decays slowly (ignoring idle frame gaps).
static void M2_R3QuantTick(int64_t endUs) {
    M2_R3Init();
    if (!g_r3Cam) return;
    M2_R3WriteBegin();
    int64_t dt = (g_r3.c.tickEndUs > 0) ? (endUs - g_r3.c.tickEndUs) : 0;
    if (endUs > 0 && dt >= 600 && dt <= 120000) {
        if (g_r3.c.tickUs <= 0) g_r3.c.tickUs = dt;
        else if (dt < g_r3.c.tickUs) g_r3.c.tickUs = (g_r3.c.tickUs * 3 + dt) / 4;
        else g_r3.c.tickUs = (g_r3.c.tickUs * 31 + dt) / 32;
        if (g_r3.c.tickUs < 600) g_r3.c.tickUs = 600;
        if (g_r3.c.tickUs > 120000) g_r3.c.tickUs = 120000;
    }
    g_r3.c.tickEndUs = endUs;
    g_r3.c.nQuants += 1;
    M2_R3WriteEnd();
}

// Called from Hook_SetTransform with the RAW engine VIEW matrix: the first sample after one or more
// Quants shifts prev <- cur and cur <- view, so (prev, cur) always spans exactly `span` ticks.
// Extra VIEW writes inside the same frame (UI / shadow / ortho passes) are deliberately ignored.
static void M2_R3CamSample(const float* view) {
    M2_R3Init();
    if (!g_r3Cam) return;
    M2_R3WriteBegin();
    if (g_r3.c.nQuants > 0) {
        if (g_r3.c.haveCur) {
            for (int i = 0; i < 16; ++i) g_r3.c.prev[i] = g_r3.c.cur[i];
            g_r3.c.havePrev = 1;
        } else {
            g_r3.c.havePrev = 0;
        }
        for (int i = 0; i < 16; ++i) g_r3.c.cur[i] = view[i];
        g_r3.c.haveCur = 1;
        g_r3.c.span = g_r3.c.nQuants;
        g_r3.c.nQuants = 0;
    } else if (!g_r3.c.haveCur) {
        for (int i = 0; i < 16; ++i) g_r3.c.cur[i] = view[i];
        g_r3.c.haveCur = 1;
        g_r3.c.havePrev = 0;
        g_r3.c.span = 0;
    }
    M2_R3WriteEnd();
}

// Decides the VIEW matrix for this frame.  `nowUs` must be sampled as late as possible - the closer to
// the draw call, the more accurate the sub-tick fraction.  Probe mode measures everything and changes
// nothing, so `M2_CAM_LERP=p M2_RENDER_PROBE=1` is a behavioural no-op that still fills the telemetry.
static const void* M2_R3CamBlendView(const void* pMatrix, int64_t nowUs, float* scratch) {
    M2_R3Copy c;
    if (!M2_R3SnapRead(&c) || !c.havePrev || c.span < 1) { ++g_r3CamHold; return pMatrix; }
    float ticks = M2_R3TicksFrom(nowUs, &c);
    if (M2_R3ExtrapCamera(c.prev, c.cur, ticks, c.span, g_r3CamStep, g_r3CamTicks, g_r3CamDot, scratch)) {
        ++g_r3CamBlend;
        float f = (ticks > g_r3CamTicks) ? g_r3CamTicks : ticks;
        if (f > g_r3CamTicksMaxSeen) g_r3CamTicksMaxSeen = f;
        return g_r3Probe ? pMatrix : scratch;
    }
    ++g_r3CamHold;
    return pMatrix;
}

// Per-second Wave 3 line for m2_fps.log.  Appended after "# phases" so existing parsers are unaffected.
static void M2_R3Telemetry(double tSec) {
    if (g_logFile == INVALID_HANDLE_VALUE || !g_r3InitDone) return;
    if (!g_r3LimitHz && !g_r3MinDraws && !g_r3Cam) return;
    uint32_t pres = g_r3Presented, skip = g_r3Skipped, wskip = g_r3WouldSkip;
    uint32_t blend = g_r3CamBlend, hold = g_r3CamHold, drawSec = g_r3DrawSec, fs = g_r3FramesSec;
    int64_t tickUs = g_r3.c.tickUs;
    uint32_t span = (g_r3.c.havePrev && g_r3.c.haveCur) ? g_r3.c.span : 0;
    float maxT = g_r3CamTicksMaxSeen;
    g_r3Presented = g_r3Skipped = g_r3WouldSkip = 0;
    g_r3CamBlend = g_r3CamHold = 0;
    g_r3DrawSec = 0; g_r3FramesSec = 0;
    g_r3CamTicksMaxSeen = 0.0f;
    double dpf = fs ? (double)drawSec / (double)fs : 0.0;
    double skipPct = (pres + skip) ? (double)skip * 100.0 / (double)(pres + skip) : 0.0;
    char line[512];
    int len = snprintf(line, sizeof(line),
                       "# render t=%.2f limit_hz=%d min_draws=%u cam_lerp=%d probe=%d presented=%u skipped=%u "
                       "would_skip=%u skip_pct=%.1f cam_blend=%u cam_hold=%u cam_max_ticks=%.3f tick_us=%lld "
                       "quant_span=%u draw_pf=%.1f\n", tSec, g_r3LimitHz, g_r3MinDraws, g_r3Cam, g_r3Probe,
                       pres, skip, wskip, skipPct, blend, hold, (double)maxT, (long long)tickUs, span, dpf);
    if (len > 0) {
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    }
}

static HRESULT STDMETHODCALLTYPE Hook_Present(
    void* pDevice, const RECT* pSourceRect, const RECT* pDestRect,
    HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
{
    if (!g_renderTid) g_renderTid = GetCurrentThreadId();
    M2_R3Init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // ---- Wave 3: independent render pacing ----
    // Decide before any driver work whether this Present deserves a swap.  Everything after it (idle
    // install retries, autostart, per-second telemetry) runs either way, so a skipped frame costs the
    // swap and nothing else, and g_lastFrame is left untouched so the next real Present can fire as
    // soon as the cadence target allows.
    int gate = 1;                              // 1 = call the real Present (stock behaviour)
    uint32_t sinceDraws = 0;
    if ((g_r3LimitHz || g_r3MinDraws) && g_qpcFreq.QuadPart != 0) {
        sinceDraws = g_r3Draws - g_r3DrawsAtP;
        if (!g_r3Armed && g_r3MinDraws > 0 && g_lastFrame.QuadPart != 0 &&
            sinceDraws >= (uint32_t)g_r3MinDraws) {
            g_r3Armed = 1;                     // one-way latch, see the fail-open note in the section header
            char am[192];
            snprintf(am, sizeof(am), "render: redundant-present suppression armed (%u draw calls between "
                     "two Presents, min=%u)", sinceDraws, g_r3MinDraws);
            LogLine(am);
        }
        const int verdict = M2_R3PresentGate(M2_R3NowUs(), M2_R3TickToUs(g_lastFrame.QuadPart), g_r3LimitHz,
                                             sinceDraws, g_r3Armed ? g_r3MinDraws : 0);
        if (verdict == 0) ++g_r3WouldSkip;
        gate = (g_r3Probe || verdict) ? 1 : 0;
    }
    int64_t tp0 = Qpc();
    g_inPresent = 1;
    HRESULT hr = S_OK;
    if (gate) hr = g_origPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    g_inPresent = 0;
    if (gate) g_presentTicks += Qpc() - tp0;
    AutoStartTick(now);
    if (!g_zoneInstalled) M2_ZoneInstall();   // retried until scene.dll is loaded; idempotent + cheap
    if (!g_pfDone) M2_PfInstall();            // Wave 2: retried until the pf module(s) load; idempotent + cheap
    if (!g_qp.done) M2_QuantInstall();        // Splice 2: retried until scene.dll is loaded; idempotent + cheap
    ProfilerTick(now);
    M2ArenaLateSweep();   // Splice 1: free-family hooks for DLLs loaded after device creation
    if (gate) {
        ++g_r3Presented;
        g_r3DrawsAtP = g_r3Draws;
        g_r3DrawSec += sinceDraws;
        ++g_r3FramesSec;
    } else {
        ++g_r3Skipped;
    }
    // A skipped frame reports nothing: there is no presented frame interval to account for and the
    // per-second line would have a zero frame count.  probe mode presents every frame, so it always reports.
    if (gate && g_lastFrame.QuadPart != 0 && g_qpcFreq.QuadPart != 0) {
        uint64_t dt_us = (uint64_t)((now.QuadPart - g_lastFrame.QuadPart) * 1000000ULL / g_qpcFreq.QuadPart);
        uint32_t us32 = (dt_us > 10000000ULL) ? 10000000U : (uint32_t)dt_us;
        g_totalFrames++;

        if (g_ftFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(g_ftFile, &us32, sizeof(us32), &written, NULL);
        }
        if (g_secFrameCount < 4096) {
            g_secFrameUs[g_secFrameCount++] = us32;
        }

        double secElapsed = (double)(now.QuadPart - g_lastSecTick.QuadPart) / (double)g_qpcFreq.QuadPart;
        if (secElapsed >= 1.0 && g_secFrameCount > 0 && g_logFile != INVALID_HANDLE_VALUE) {
            uint64_t sum_us = 0;
            uint32_t max_us = 0;
            for (uint32_t i = 0; i < g_secFrameCount; ++i) {
                sum_us += g_secFrameUs[i];
                if (g_secFrameUs[i] > max_us) max_us = g_secFrameUs[i];
            }
            double avg_ms = (double)sum_us / (double)g_secFrameCount / 1000.0;
            double fps = (double)g_secFrameCount / secElapsed;
            double max_ms = (double)max_us / 1000.0;
            double total_el = (double)(now.QuadPart - g_startTick.QuadPart) / (double)g_qpcFreq.QuadPart;

            char line[640];
            int len = snprintf(line, sizeof(line), "%.2f %.2f %.3f %.3f %u\n",
                               total_el, fps, avg_ms, max_ms, g_totalFrames);
            if (len > 0) {
                DWORD written = 0;
                WriteFile(g_logFile, line, (DWORD)len, &written, NULL);
            }
            uint32_t draws = g_draws, prims = g_prims, up = g_upDraws;
            uint32_t aExe = g_allocExe, aScene = g_allocScene, aRender = g_allocRender;
            uint32_t asCalls = g_fastAstarCalls, asPops = g_fastAstarPops, asEarly = g_fastAstarEarlyOuts;
            uint32_t spCalls = g_fastSeePointCalls, spPops = g_fastSeePointPops;
            int64_t asTicks = g_fastAstarTicks, spTicks = g_fastSeePointTicks;
            uint32_t arSrv = 0, arFre = 0, arFb = 0, arFrn = 0, arBad = 0, arChunks = 0;
            Ar_TelemetryReset(&arSrv, &arFre, &arFb, &arFrn, &arBad, &arChunks);
            g_draws = g_prims = g_upDraws = 0;
            g_allocExe = g_allocScene = g_allocRender = 0;
            g_fastAstarCalls = g_fastAstarPops = g_fastAstarEarlyOuts = 0;
            g_fastSeePointCalls = g_fastSeePointPops = 0;
            g_fastAstarTicks = g_fastSeePointTicks = 0;
            uint32_t allocs = aExe + aScene + aRender;
            double nf = (double)g_secFrameCount;
            double tk2ms = 1000.0 / (double)g_qpcFreq.QuadPart;
            len = snprintf(line, sizeof(line),
                           "# telemetry t=%.2f frames=%u draws=%u prims=%u updraws=%u allocs=%u a_exe=%u a_scene=%u "
                           "a_render=%u dpf=%.1f ppf=%.1f apf=%.1f astar_calls=%u astar_pops=%u astar_early=%u "
                           "astar_ms=%.2f sp_calls=%u sp_pops=%u sp_ms=%.2f "
                           "arena=%u ar_free=%u ar_fb=%u ar_ff=%u ar_bad=%u ar_mb=%u ar_apf=%.1f\n",
                           total_el, g_secFrameCount, draws, prims, up, allocs, aExe, aScene, aRender,
                           draws / nf, prims / nf, allocs / nf, asCalls, asPops, asEarly,
                           (double)asTicks * tk2ms / nf, spCalls, spPops, (double)spTicks * tk2ms / nf,
                           arSrv, arFre, arFb, arFrn, arBad, arChunks, (double)arSrv / nf);
            if (len > 0) {
                DWORD written = 0;
                WriteFile(g_logFile, line, (DWORD)len, &written, NULL);
            }
            // Phase split, ms per frame: frame = present + draw + lock + state + misc + create + logic
            double frameMs = (double)sum_us / 1000.0 / nf;
            double bms[B_COUNT];
            double d3dMs = 0.0;
            uint32_t bcalls[B_COUNT];
            for (int b = 0; b < B_COUNT; ++b) {
                bms[b] = (double)g_bucketTicks[b] * tk2ms / nf;
                bcalls[b] = g_bucketCalls[b];
                d3dMs += bms[b];
                g_bucketTicks[b] = 0;
                g_bucketCalls[b] = 0;
            }
            double presentMs = (double)g_presentTicks * tk2ms / nf;
            uint32_t foreign = g_foreignCalls;
            g_presentTicks = 0;
            g_foreignCalls = 0;
            float spd = 0.0f, scale = 0.0f;
            if (!g_levelUp || !QuantSpeed(&spd, &scale)) spd = scale = -1.0f;
            uint32_t qCalls = g_quantCalls;
            double quantMs = (double)g_quantTicks * tk2ms / nf;
            double qArgMs = qCalls ? (double)g_quantArgMs / qCalls : 0.0;
            double gameX = (double)g_quantGameMs / 1000.0 / secElapsed;  // game seconds per real second
            g_quantTicks = 0; g_quantCalls = 0; g_quantArgMs = 0; g_quantGameMs = 0;
            len = snprintf(line, sizeof(line),
                           "# phases t=%.2f frames=%u frame_ms=%.2f present_ms=%.2f draw_ms=%.2f lock_ms=%.2f "
                           "state_ms=%.2f misc_ms=%.2f create_ms=%.2f logic_ms=%.2f lock_calls=%.1f state_calls=%.1f "
                           "misc_calls=%.1f create_calls=%.1f foreign=%u speed=%.2f speed_scale=%.2f quant_ms=%.2f quants_pf=%.2f "
                           "qarg_ms=%.1f game_x=%.3f\n",
                           total_el, g_secFrameCount, frameMs, presentMs, bms[B_DRAW], bms[B_LOCK], bms[B_STATE],
                           bms[B_MISC], bms[B_CREATE], frameMs - presentMs - d3dMs, bcalls[B_LOCK] / nf,
                           bcalls[B_STATE] / nf, bcalls[B_MISC] / nf, bcalls[B_CREATE] / nf, foreign, spd, scale,
                           quantMs, qCalls / nf, qArgMs, gameX);
            if (len > 0) {
                DWORD written = 0;
                WriteFile(g_logFile, line, (DWORD)len, &written, NULL);
                FlushFileBuffers(g_logFile);
            }
            M2_FrustumTelemetry(total_el);
            M2_ZoneTelemetry(total_el);
            M2_PfTelemetry(total_el);
            M2_R3Telemetry(total_el);   // Wave 3: per-second render-pacing line
            M2_QuantTelemetry(total_el);
            M2_FrustumLiveSelfTest();
            if (g_ftFile != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(g_ftFile);
            }
            g_secFrameCount = 0;
            g_lastSecTick = now;
        }
    }
    if (gate) g_lastFrame = now;   // a skipped frame must not advance the pacing clock
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    void* pD3D9, UINT Adapter, DWORD DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, void* pPresentationParameters, void** ppReturnedDeviceInterface)
{
    // Skip startup intro videos (playStartupVideo at 0x00ac0600) and 20s GamePreloadManager stall (0x009b4d32)
    *(unsigned char*)0x00ac0600 = 0;
    *(unsigned char*)0x009b4d32 = 0;

    char m[320];
    const DWORD origFlags = BehaviorFlags;
    snprintf(m, sizeof(m), "CreateDevice: Adapter=%u DeviceType=%lu BehaviorFlags=0x%08lX tid=%lu", Adapter,
             DeviceType, origFlags, GetCurrentThreadId());
    LogLine(m);
    if (pPresentationParameters) {
        const DWORD* pp = (const DWORD*)pPresentationParameters;  // D3DPRESENT_PARAMETERS
        snprintf(m, sizeof(m), "CreateDevice: PP %lux%lu fmt=%lu backbuffers=%lu swapEffect=%lu windowed=%lu "
                 "flags=0x%lX presentInterval=0x%lX", pp[0], pp[1], pp[2], pp[3], pp[6], pp[8], pp[11], pp[13]);
        LogLine(m);
    }
    if (origFlags & 0x04) {  // D3DCREATE_MULTITHREADED
        char ev[8] = {0};
        bool clear = GetEnvironmentVariableA("M2_CLEAR_MT", ev, sizeof(ev)) > 0 && ev[0] == '1';
        if (clear) BehaviorFlags &= ~0x04UL;
        snprintf(m, sizeof(m), "CreateDevice: D3DCREATE_MULTITHREADED is SET -> %s (flags now 0x%08lX)",
                 clear ? "CLEARED (M2_CLEAR_MT=1)" : "kept", BehaviorFlags);
        LogLine(m);
    } else {
        LogLine("CreateDevice: D3DCREATE_MULTITHREADED not set");
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    g_mainTid = GetCurrentThreadId();
    g_mainStackBase = (DWORD)(DWORD_PTR)((NT_TIB*)NtCurrentTeb())->StackBase;
    BOOL prOk = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    snprintf(m, sizeof(m), "CreateDevice: SetThreadPriority(HIGHEST) ok=%d now=%d", prOk, GetThreadPriority(GetCurrentThread()));
    LogLine(m);

    HRESULT hr = g_origCreateDevice(pD3D9, Adapter, DeviceType, hFocusWindow,
                                    BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    snprintf(m, sizeof(m), "CreateDevice: hr=0x%08lX", (unsigned long)hr);
    LogLine(m);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        void** vtbl = *(void***)(*ppReturnedDeviceInterface);
        if (vtbl && vtbl[17] != (void*)Hook_Present) {
            g_origPresent = (PFN_Present)vtbl[17];
            PatchPtr(&vtbl[17], (void*)Hook_Present);
        }
        if (vtbl && vtbl[81] != (void*)Hook_DrawPrimitive) {
            g_origDrawPrimitive = (PFN_DrawPrimitive)vtbl[81];
            PatchPtr(&vtbl[81], (void*)Hook_DrawPrimitive);
        }
        if (vtbl && vtbl[82] != (void*)Hook_DrawIndexedPrimitive) {
            g_origDrawIndexedPrimitive = (PFN_DrawIndexedPrimitive)vtbl[82];
            PatchPtr(&vtbl[82], (void*)Hook_DrawIndexedPrimitive);
        }
        if (vtbl && vtbl[83] != (void*)Hook_DrawPrimitiveUP) {
            g_origDrawPrimitiveUP = (PFN_DrawPrimitiveUP)vtbl[83];
            PatchPtr(&vtbl[83], (void*)Hook_DrawPrimitiveUP);
        }
        if (vtbl && vtbl[84] != (void*)Hook_DrawIndexedPrimitiveUP) {
            g_origDrawIndexedPrimitiveUP = (PFN_DrawIndexedPrimitiveUP)vtbl[84];
            PatchPtr(&vtbl[84], (void*)Hook_DrawIndexedPrimitiveUP);
        }
        InstallDevicePhaseHooks(vtbl);
        InstallAllocHooks();
        InstallQuantHook();
        M2_ZoneInstall();
        M2_PfInstall();
        M2_QuantInstall();
    }
    LogEnv();
    LogModules("device");
    return hr;
}

static void EnsureRealD3D9(void) {
    if (g_realD3D9) return;
    InitSensorFiles();
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    char path[MAX_PATH];
    // We ARE syswow64\\d3d9.dll (KnownDLL); the real one is kept next to us as d3d9_dxvk.dll
    snprintf(path, sizeof(path), "%s\\d3d9_dxvk.dll", sysDir);
    g_realD3D9 = LoadLibraryA(path);
    if (g_realD3D9) {
        g_realDirect3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_realD3D9, "Direct3DCreate9");
    }
}

extern "C" {

void* WINAPI Direct3DCreate9(UINT SDKVersion) {
    EnsureRealD3D9();
    if (!g_realDirect3DCreate9) return NULL;
    void* pD3D9 = g_realDirect3DCreate9(SDKVersion);
    if (pD3D9) {
        void** vtbl = *(void***)pD3D9;
        if (vtbl && vtbl[16] != (void*)Hook_CreateDevice) {
            DWORD oldProt = 0;
            if (VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
                g_origCreateDevice = (PFN_CreateDevice)vtbl[16];
                vtbl[16] = (void*)Hook_CreateDevice;
                VirtualProtect(&vtbl[16], sizeof(void*), oldProt, &oldProt);
            }
        }
    }
    return pD3D9;
}

int WINAPI D3DPERF_BeginEvent(DWORD col, LPCWSTR wszName) {
    EnsureRealD3D9();
    static PFN_D3DPERF_BeginEvent fn = (PFN_D3DPERF_BeginEvent)GetProcAddress(g_realD3D9, "D3DPERF_BeginEvent");
    return fn ? fn(col, wszName) : 0;
}

int WINAPI D3DPERF_EndEvent(void) {
    EnsureRealD3D9();
    static PFN_D3DPERF_EndEvent fn = (PFN_D3DPERF_EndEvent)GetProcAddress(g_realD3D9, "D3DPERF_EndEvent");
    return fn ? fn() : 0;
}

void WINAPI D3DPERF_SetMarker(DWORD col, LPCWSTR wszName) {
    EnsureRealD3D9();
    static PFN_D3DPERF_SetMarker fn = (PFN_D3DPERF_SetMarker)GetProcAddress(g_realD3D9, "D3DPERF_SetMarker");
    if (fn) fn(col, wszName);
}

void WINAPI D3DPERF_SetRegion(DWORD col, LPCWSTR wszName) {
    EnsureRealD3D9();
    static PFN_D3DPERF_SetRegion fn = (PFN_D3DPERF_SetRegion)GetProcAddress(g_realD3D9, "D3DPERF_SetRegion");
    if (fn) fn(col, wszName);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    EnsureRealD3D9();
    static PFN_D3DPERF_QueryRepeatFrame fn = (PFN_D3DPERF_QueryRepeatFrame)GetProcAddress(g_realD3D9, "D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD dwOptions) {
    EnsureRealD3D9();
    static PFN_D3DPERF_SetOptions fn = (PFN_D3DPERF_SetOptions)GetProcAddress(g_realD3D9, "D3DPERF_SetOptions");
    if (fn) fn(dwOptions);
}

DWORD WINAPI D3DPERF_GetStatus(void) {
    EnsureRealD3D9();
    static PFN_D3DPERF_GetStatus fn = (PFN_D3DPERF_GetStatus)GetProcAddress(g_realD3D9, "D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        // Splice 1: no DisableThreadLibraryCalls - THREAD_DETACH must fire so dying
        // threads publish their arena free lists back to the global stacks.
    } else if (fdwReason == DLL_THREAD_DETACH) {
        M2A_ThreadDetach();
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_ftFile != INVALID_HANDLE_VALUE) CloseHandle(g_ftFile);
        if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
    }
    return TRUE;
}
