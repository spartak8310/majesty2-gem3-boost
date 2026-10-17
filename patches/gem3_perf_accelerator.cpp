/**
 * GEM 3 Engine Performance Accelerator & D3D9 Stall Eliminator (`gem3_perf_accelerator.cpp`)
 * Target: Majesty 2 Collection v1.5.356 (32-bit x86 PE / Wine / Wine/Whisky / Rosetta 2 / Proton)
 *
 * Root Causes Addressed:
 * 1. `geometry.dll` has 16,973 legacy x87 FPU instructions and 0 SSE instructions.
 *    Its hottest functions (`xGeometry::Construct(eBoxGeneric<Vec3>*, ...)`,
 *    `xGeometry::Xform(eBoxGeneric<Vec3>&, const Matrix34&)`, `Vec3::Xform(const Matrix34&)`,
 *    `Matrix33::Xform(const Vec3&)`, `xGeometry::Intersecting(const eFrustum&, const eBoxGeneric<Vec3>&)`)
 *    execute 140+ x87 stack instructions and 9 out-of-line `call fabsf` (`0x10002790`) per AABB.
 *    Under modern x86 CPUs and especially Apple Silicon Rosetta 2, x87 stack juggling (`fxch`,
 *    `fnstsw ax`) is 5-8x slower than SIMD SSE2.
 * 2. `render.dll` defaults `dbg_occlusionWaitResult` (`0x100A64A0`) to `1` (`true`), which forces
 *    a synchronous CPU spin-wait (`VA 0x1004AB06..0x1004AC44`) on `IDirect3DQuery9::GetData`
 *    whenever hardware occlusion culling is active (`r_occlusionCullingNVOnly 0`).
 * 3. `render.dll` defaults `r_FogmapDisableSystemCopy` (`0x100A4DA0`) to `0` (`false`), causing
 *    `IDirect3DDevice9::GetRenderTargetData` (`VA 0x1003F452`) to stall the GPU pipeline every
 *    single frame copying the 1024x1024 fog-of-war render target from VRAM to System RAM.
 * 4. `scene.dll` `ed_alwaysEnableLandEditor` (`0x101B17D8`) is accidentally enabled (`1`) in
 *    `resource_patch.pak/interface/cs/developer.con`, preventing static grass buffer locking
 *    (`VA 0x100B1F5A`) and clearing baked terrain stamp flags (`VA 0x100B2C8E`).
 *
 * Build (MinGW-w64 i686 on macOS/Linux):
 *   i686-w64-mingw32-g++ -O3 -msse2 -shared -static-libgcc -static-libstdc++ \
 *       -o gem3_perf_accelerator.dll gem3_perf_accelerator.cpp
 *
 * Deployment:
 *   Can be loaded as an ASI/plugin DLL or injected via `ps.dll` `DummyFunction` (`VA 0x100065A0`).
 */

#if defined(_WIN32) || defined(__MINGW32__)
#include <windows.h>
#include <emmintrin.h> // SSE2
#include <xmmintrin.h> // SSE

#pragma pack(push, 1)
struct Vec3 {
    float x, y, z;
};

struct Matrix33 {
    Vec3 x; // +0x00 (row 0)
    Vec3 y; // +0x0C (row 1)
    Vec3 z; // +0x18 (row 2)
};

struct Matrix34 {
    Vec3 x; // +0x00 (row 0)
    Vec3 y; // +0x0C (row 1)
    Vec3 z; // +0x18 (row 2)
    Vec3 t; // +0x24 (translation row 3)
};

struct eBox3 {
    Vec3 min; // +0x00
    Vec3 max; // +0x0C
};

struct ePlane3 {
    Vec3  normal; // +0x00
    float d;      // +0x0C
};

struct eFrustum {
    Vec3    origin;         // +0x00
    uint8_t _pad[0x34];     // camera basis / cached vertices
    int32_t plane_Mask_or_pad;
    // Frustum planes start at offset +0x38, stride 0x10, count at +0x98
};
#pragma pack(pop)

// ============================================================================
// 1. SSE2 Replacement for `xGeometry::Xform(eBoxGeneric<Vec3>&, const Matrix34&)`
//    Original VA in `geometry.dll`: 0x1001F940 (RVA 0x0001F940)
//    Original cost: 140 x87 instructions + 9 out-of-line `call 0x10002790` (fabsf)
//    SSE2 cost: ~22 instructions, 0 calls, 0 branches
// ============================================================================
extern "C" void __cdecl SSE2_Geometry_Xform_Box3(eBox3& box, const Matrix34& m) {
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));

    // Load box.min and box.max safely (unaligned 3 floats + 1 float overlap)
    __m128 bmin = _mm_setr_ps(box.min.x, box.min.y, box.min.z, 0.0f);
    __m128 bmax = _mm_setr_ps(box.max.x, box.max.y, box.max.z, 0.0f);

    __m128 center  = _mm_mul_ps(_mm_add_ps(bmax, bmin), half);
    __m128 extents = _mm_mul_ps(_mm_sub_ps(bmax, bmin), half);

    // Load Matrix34 rows
    __m128 row0 = _mm_setr_ps(m.x.x, m.x.y, m.x.z, 0.0f);
    __m128 row1 = _mm_setr_ps(m.y.x, m.y.y, m.y.z, 0.0f);
    __m128 row2 = _mm_setr_ps(m.z.x, m.z.y, m.z.z, 0.0f);
    __m128 row3 = _mm_setr_ps(m.t.x, m.t.y, m.t.z, 0.0f);

    // Broadcast center components
    __m128 cx = _mm_shuffle_ps(center, center, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 cy = _mm_shuffle_ps(center, center, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 cz = _mm_shuffle_ps(center, center, _MM_SHUFFLE(2, 2, 2, 2));

    // new_center = cx * row0 + cy * row1 + cz * row2 + row3
    __m128 new_center = _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(cx, row0), _mm_mul_ps(cy, row1)),
        _mm_add_ps(_mm_mul_ps(cz, row2), row3)
    );

    // Broadcast extent components
    __m128 ex = _mm_shuffle_ps(extents, extents, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 ey = _mm_shuffle_ps(extents, extents, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 ez = _mm_shuffle_ps(extents, extents, _MM_SHUFFLE(2, 2, 2, 2));

    // new_extents = |ex * row0| + |ey * row1| + |ez * row2|
    __m128 a0 = _mm_and_ps(_mm_mul_ps(ex, row0), abs_mask);
    __m128 a1 = _mm_and_ps(_mm_mul_ps(ey, row1), abs_mask);
    __m128 a2 = _mm_and_ps(_mm_mul_ps(ez, row2), abs_mask);
    __m128 new_extents = _mm_add_ps(_mm_add_ps(a0, a1), a2);

    __m128 out_min = _mm_sub_ps(new_center, new_extents);
    __m128 out_max = _mm_add_ps(new_center, new_extents);

    alignas(16) float tmp_min[4], tmp_max[4];
    _mm_store_ps(tmp_min, out_min);
    _mm_store_ps(tmp_max, out_max);

    box.min.x = tmp_min[0]; box.min.y = tmp_min[1]; box.min.z = tmp_min[2];
    box.max.x = tmp_max[0]; box.max.y = tmp_max[1]; box.max.z = tmp_max[2];
}

// ============================================================================
// 2. SSE2 Replacement for `xGeometry::Construct(eBoxGeneric<Vec3>*, const eBoxGeneric<Vec3>&, const Matrix34&)`
//    Original VA in `geometry.dll`: 0x10013CF0 (RVA 0x00013CF0)
// ============================================================================
extern "C" void __cdecl SSE2_Geometry_Construct_Box3(eBox3* dst, const eBox3& src, const Matrix34& m) {
    *dst = src;
    SSE2_Geometry_Xform_Box3(*dst, m);
}

// ============================================================================
// 3. SSE2 Replacement for `Vec3::Xform(const Matrix34&)` (__thiscall)
//    Original VA in `geometry.dll`: 0x10008800 (RVA 0x00008800)
// ============================================================================
extern "C" Vec3* __fastcall SSE2_Vec3_Xform_Mat34(Vec3* self, void* /*edx*/, const Matrix34& m) {
    __m128 v = _mm_setr_ps(self->x, self->y, self->z, 0.0f);
    __m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));

    __m128 row0 = _mm_setr_ps(m.x.x, m.x.y, m.x.z, 0.0f);
    __m128 row1 = _mm_setr_ps(m.y.x, m.y.y, m.y.z, 0.0f);
    __m128 row2 = _mm_setr_ps(m.z.x, m.z.y, m.z.z, 0.0f);
    __m128 row3 = _mm_setr_ps(m.t.x, m.t.y, m.t.z, 0.0f);

    __m128 res = _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(vx, row0), _mm_mul_ps(vy, row1)),
        _mm_add_ps(_mm_mul_ps(vz, row2), row3)
    );

    alignas(16) float out[4];
    _mm_store_ps(out, res);
    self->x = out[0];
    self->y = out[1];
    self->z = out[2];
    return self;
}

// ============================================================================
// 4. SSE2 Replacement for `Vec3::Xform(const Matrix33&)` (__thiscall)
//    Original VA in `geometry.dll`: 0x1000EBA0 (RVA 0x0000EBA0)
// ============================================================================
extern "C" Vec3* __fastcall SSE2_Vec3_Xform_Mat33(Vec3* self, void* /*edx*/, const Matrix33& m) {
    __m128 v = _mm_setr_ps(self->x, self->y, self->z, 0.0f);
    __m128 vx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 vy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 vz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));

    __m128 row0 = _mm_setr_ps(m.x.x, m.x.y, m.x.z, 0.0f);
    __m128 row1 = _mm_setr_ps(m.y.x, m.y.y, m.y.z, 0.0f);
    __m128 row2 = _mm_setr_ps(m.z.x, m.z.y, m.z.z, 0.0f);

    __m128 res = _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(vx, row0), _mm_mul_ps(vy, row1)),
        _mm_mul_ps(vz, row2)
    );

    alignas(16) float out[4];
    _mm_store_ps(out, res);
    self->x = out[0];
    self->y = out[1];
    self->z = out[2];
    return self;
}

// ============================================================================
// 5. SSE2 Replacement for `xGeometry::Intersecting(const eFrustum&, const eBoxGeneric<Vec3>&)`
//    Original VA in `geometry.dll`: 0x100151B0 (RVA 0x000151B0)
//    Replaces 135 x87 instructions and `fnstsw ax` branching per plane with SSE2 min/max dot products
// ============================================================================
extern "C" bool __cdecl SSE2_Geometry_Intersecting_Frustum_Box3(const uint8_t* frustum, const eBox3& box) {
    int32_t plane_count = *reinterpret_cast<const int32_t*>(frustum + 0x98);
    if (plane_count <= 0) {
        return true;
    }

    __m128 bmin = _mm_setr_ps(box.min.x, box.min.y, box.min.z, 0.0f);
    __m128 bmax = _mm_setr_ps(box.max.x, box.max.y, box.max.z, 0.0f);

    const ePlane3* planes = reinterpret_cast<const ePlane3*>(frustum + 0x38);
    for (int32_t i = 0; i < plane_count; ++i) {
        __m128 n = _mm_setr_ps(planes[i].normal.x, planes[i].normal.y, planes[i].normal.z, 0.0f);
        __m128 d0 = _mm_mul_ps(n, bmin);
        __m128 d1 = _mm_mul_ps(n, bmax);
        // For outward-pointing frustum normals in GEM 3 (`fnstsw ax; test ah, 0x41`),
        // a box is completely outside the plane if the minimum signed distance is > 0:
        __m128 min_prod = _mm_min_ps(d0, d1);
        alignas(16) float c[4];
        _mm_store_ps(c, min_prod);
        float min_dist = c[0] + c[1] + c[2] + planes[i].d;
        if (min_dist > 0.0f) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Binary Patching Helpers
// ============================================================================
static void WriteJump32(uintptr_t src_va, uintptr_t dst_va) {
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(src_va), 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        uint8_t* p = reinterpret_cast<uint8_t*>(src_va);
        p[0] = 0xE9; // JMP rel32
        *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(dst_va - (src_va + 5));
        VirtualProtect(reinterpret_cast<void*>(src_va), 5, old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(src_va), 5);
    }
}

static void PatchByte(uintptr_t va, uint8_t value) {
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(va), 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *reinterpret_cast<uint8_t*>(va) = value;
        VirtualProtect(reinterpret_cast<void*>(va), 1, old_protect, &old_protect);
    }
}

static void PatchFloat(uintptr_t va, float value) {
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(va), 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *reinterpret_cast<float*>(va) = value;
        VirtualProtect(reinterpret_cast<void*>(va), 4, old_protect, &old_protect);
    }
}

extern "C" __declspec(dllexport) void __cdecl ApplyGem3PerformancePatches() {
    // 1. Hook `geometry.dll` hot x87 functions with SSE2 implementations
    if (HMODULE hGeom = GetModuleHandleA("geometry.dll")) {
        uintptr_t base = reinterpret_cast<uintptr_t>(hGeom);
        // RVA 0x0001F940: xGeometry::Xform(eBoxGeneric<Vec3>&, const Matrix34&)
        WriteJump32(base + 0x0001F940, reinterpret_cast<uintptr_t>(&SSE2_Geometry_Xform_Box3));
        // RVA 0x00013CF0: xGeometry::Construct(eBoxGeneric<Vec3>*, const eBoxGeneric<Vec3>&, const Matrix34&)
        WriteJump32(base + 0x00013CF0, reinterpret_cast<uintptr_t>(&SSE2_Geometry_Construct_Box3));
        // RVA 0x00008800: Vec3::Xform(const Matrix34&)
        WriteJump32(base + 0x00008800, reinterpret_cast<uintptr_t>(&SSE2_Vec3_Xform_Mat34));
        // RVA 0x0000EBA0: Vec3::Xform(const Matrix33&)
        WriteJump32(base + 0x0000EBA0, reinterpret_cast<uintptr_t>(&SSE2_Vec3_Xform_Mat33));
        // RVA 0x000151B0: xGeometry::Intersecting(const eFrustum&, const eBoxGeneric<Vec3>&)
        WriteJump32(base + 0x000151B0, reinterpret_cast<uintptr_t>(&SSE2_Geometry_Intersecting_Frustum_Box3));
    }

    // 2. Patch `render.dll` synchronous D3D9 pipeline stalls
    if (HMODULE hRender = GetModuleHandleA("render.dll")) {
        uintptr_t base = reinterpret_cast<uintptr_t>(hRender);
        // dbg_occlusionWaitResult (RVA 0x000A64A0): 1 -> 0 (disable CPU spin-wait on D3D9 occlusion query)
        PatchByte(base + 0x000A64A0, 0);
        // r_FogmapDisableSystemCopy (RVA 0x000A4DA0): 0 -> 1 (disable per-frame VRAM->RAM GetRenderTargetData stall)
        PatchByte(base + 0x000A4DA0, 1);
    }

    // 3. Patch `scene.dll` LandEditor & grass culling defaults
    if (HMODULE hScene = GetModuleHandleA("scene.dll")) {
        uintptr_t base = reinterpret_cast<uintptr_t>(hScene);
        // ed_alwaysEnableLandEditor (RVA 0x001B17D8): force 0 so static grass & stamp buffers lock properly
        PatchByte(base + 0x001B17D8, 0);
        // r_grassCull (RVA 0x001B12F4): force 1
        PatchByte(base + 0x001B12F4, 1);
    }

    // 4. Patch `Multiplayer.dll` quant rate-limiting (fixes 0.0 spam from resource_patch.pak)
    if (HMODULE hMp = GetModuleHandleA("Multiplayer.dll")) {
        uintptr_t base = reinterpret_cast<uintptr_t>(hMp);
        // m2TimeBetweenQuantReports (RVA 0x00093170) -> 0.033f
        PatchFloat(base + 0x00093170, 0.033f);
        // m2TimeBetweenQuantCommands (RVA 0x000931D0) -> 0.033f
        PatchFloat(base + 0x000931D0, 0.033f);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        ApplyGem3PerformancePatches();
    }
    return TRUE;
}
#endif
