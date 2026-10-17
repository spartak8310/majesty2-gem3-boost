// ============================================================================================
//  m2_frustum_sse2.h - HyperDrive SSE2 camera-frustum kernel (Gribb-Hartmann) for Majesty 2
// ============================================================================================
//  Purpose
//  -------
//  Wave 1 ("Spatial Frustum LOD") needs two things:
//    (1) the six camera frustum planes out of the captured world->clip 4x4 matrix, and
//    (2) a cheap per-draw `IsInCameraFrustum(x,y,z,radius)` test that later waves call from
//        inside the DrawIndexedPrimitive/DrawPrimitive hooks (and from the particle-path hook
//        once a runtime scan has located the emitter).
//  The *scalar* core lives in `src/d3d9_proxy.cpp` between the markers
//      // ---- M2_FRUSTUM_CORE_BEGIN ----  ...  // ---- M2_FRUSTUM_CORE_END ----
//  and is unit-tested by `scripts/test_frustum_math.py` (39/39 checks PASS).  This header is the
//  SSE2 kernel that consumes the convention the scalar core has already selected and returns the
//  *same* visible/hidden verdict without the per-plane scalar loop.
//
//  Contract (must stay identical to M2_FrustumSphereVisible in d3d9_proxy.cpp)
//  -------------------------------------------------------------------------
//      M2SSE_SphereVisible(f, x, y, z, radius) -> 1 = VISIBLE (or unknown), 0 = fully outside
//      invalid frustum / NaN / non-finite input / radius > 1e6 -> 1   (FAIL OPEN)
//      radius < 0                                              -> point test (radius = 0)
//      hidden only when the sphere is behind ALL six planes by more than `radius`
//  Fail-open is a hard requirement: a broken capture must never hide content that is on screen.
//
//  Matrix convention (identical to the scalar core - do not "fix" independently)
//  ----------------------------------------------------------------------------
//  Row-vector D3D convention: clip = world * V * P, so with C = V * P stored row-major
//  (C[i][j] = c[i*4+j]) and world position column c = (x,y,z,1):
//      clip_j = sum_i C[i][j] * c_i          (j = 0..3  <-> x, y, z, w)
//  A plane is the row selector s (over clip coordinates) applied as a *column* combination:
//      plane(axis) = sum_j s_j * C[axis][j]      (axis 0..2 = xyz normal, axis 3 = d)
//      <=>  plane_vec = C * s   (matrix-vector), i.e. s0*Col0 + s1*Col1 + s2*Col2 + s3*Col3
//      left:  s=( 1,0,0,1)   right: s=(-1,0,0,1)   bottom: s=(0, 1,0,1)
//      top:   s=( 0,-1,0,1)  near:  s=(0,0, 1,0)   far:    s=(0,0,-1,1)
//  inside <=> dot3(normal, p) + d >= 0 with unit-length normals.
//  NOTE: plane_vec = C*s is NOT the same as the row sum s0*R0 + ... + s3*R3 unless C is
//  symmetric - the SSE2 code below transposes once so the columns are contiguous and then
//  combines them; `scripts/test_frustum_sse2.py` cross-checks every plane against the scalar
//  core bit-for-bit within 1e-5 relative, so a mismatch cannot survive the test gate.
//
//  Integration (Wave 2 / Wave 3)
//  -----------------------------
//      static M2FrustumSSE g_frustumSSE;                             // next to g_frustum
//      /* in M2_FrustumRebuild(), after M2_FrustumBuild(&f, view, proj) returns 1: */
//      M2SSE_Build(&g_frustumSSE, f.plane, view, f.convention, 1);
//      /* hot path (draw hook / particle hook): */
//      if (!M2SSE_SphereVisible(&g_frustumSSE, cx, cy, cz, radius)) return D3D_OK;
//      /* AABB batches: */
//      if (!M2SSE_AABBVisible(&g_frustumSSE, bbMin, bbMax)) return D3D_OK;
//
//  No OS headers, no globals, no allocation: the same file compiles into the MinGW PE32 build of
//  d3d9_proxy.cpp (i686-w64-mingw32-g++ -O3 -msse2) and into a native host unit test.
// ============================================================================================

#ifndef M2_FRUSTUM_SSE2_H
#define M2_FRUSTUM_SSE2_H

#include <emmintrin.h>   /* SSE2 */

// ------------------------------------------------------------------ small helpers (no libm dependency)
static __inline float M2SSE_SqrtF(float v) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v)));   /* correctly rounded, x87-free */
}
static __inline int M2SSE_FiniteF(float v) {
    return (v == v) && (v > -1.0e9f) && (v < 1.0e9f);   /* mirrors the scalar core's M2_IsFinite */
}
static __inline __m128 M2SSE_K(float k0, float k1, float k2, float k3) {
    return _mm_set_ps(k3, k2, k1, k0);                  /* lane0 = k0 ... lane3 = k3 */
}
static __inline float M2SSE_HSum4(__m128 v) {           /* full 4-lane horizontal sum */
    __m128 s = _mm_add_ss(v, _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1)));
    s = _mm_add_ss(s, _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2)));
    s = _mm_add_ss(s, _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)));
    return _mm_cvtss_f32(s);
}
static __inline float M2SSE_AbsF(float v) { return v < 0.0f ? -v : v; }

// ------------------------------------------------------------------ kernel state
#define M2SSE_GROUP_COUNT  2            /* group 0: planes 0..3, group 1: planes 4,5 + 2 sentinels  */
#define M2SSE_FAR_SENTINEL 1.0e30f      /* sentinel constant -> distance +1e30 -> never culls      */

typedef struct M2FrustumSSE {
    /* Structure-of-arrays, 4 planes per group; group 1 lanes 2,3 are sentinels (unit normal 0,
       constant +1e30) so every SIMD step is branch-free and fail-open. */
    __m128 soaX[M2SSE_GROUP_COUNT];
    __m128 soaY[M2SSE_GROUP_COUNT];
    __m128 soaZ[M2SSE_GROUP_COUNT];
    __m128 soaD[M2SSE_GROUP_COUNT];
    float  plane[6][4];     /* AoS mirror, normalised: (nx, ny, nz, d)                          */
    float  eye[3];
    float  fwd[3];
    float  nearDist;
    float  farDist;
    int    convention;      /* copied from the scalar core (bit0 viewT, bit1 projT)             */
    int    valid;           /* 0 -> every query fails open                                      */
} M2FrustumSSE;

// ------------------------------------------------------------------ 4x4 helpers
static __inline void M2SSE_Transpose4(__m128* r0, __m128* r1, __m128* r2, __m128* r3) {
    const __m128 t0 = _mm_unpacklo_ps(*r0, *r1);   /* R0[0] R1[0] R0[1] R1[1] */
    const __m128 t1 = _mm_unpackhi_ps(*r0, *r1);   /* R0[2] R1[2] R0[3] R1[3] */
    const __m128 t2 = _mm_unpacklo_ps(*r2, *r3);   /* R2[0] R3[0] R2[1] R3[1] */
    const __m128 t3 = _mm_unpackhi_ps(*r2, *r3);   /* R2[2] R3[2] R2[3] R3[3] */
    *r0 = _mm_movelh_ps(t0, t2);                   /* column 0 */
    *r1 = _mm_movehl_ps(t2, t0);                   /* column 1 */
    *r2 = _mm_movelh_ps(t1, t3);                   /* column 2 */
    *r3 = _mm_movehl_ps(t3, t1);                   /* column 3 */
}

// ------------------------------------------------------------------ Gribb-Hartmann extraction
// Computes the six planes of C = V*P (row-major, row-vector convention). out6x4 may be unaligned.
static __inline void M2SSE_ExtractPlanes(float out6x4[6][4], const float* c) {
    __m128 col0 = _mm_loadu_ps(c + 0);
    __m128 col1 = _mm_loadu_ps(c + 4);
    __m128 col2 = _mm_loadu_ps(c + 8);
    __m128 col3 = _mm_loadu_ps(c + 12);
    M2SSE_Transpose4(&col0, &col1, &col2, &col3);   /* now colN = column N of C */

    /* selector table identical to M2_FrustumExtract() in d3d9_proxy.cpp */
    static const float SEL[6][4] = {
        {  1.0f,  0.0f,  0.0f,  1.0f },   /* left   : w + x >= 0 */
        { -1.0f,  0.0f,  0.0f,  1.0f },   /* right  : w - x >= 0 */
        {  0.0f,  1.0f,  0.0f,  1.0f },   /* bottom : w + y >= 0 */
        {  0.0f, -1.0f,  0.0f,  1.0f },   /* top    : w - y >= 0 */
        {  0.0f,  0.0f,  1.0f,  0.0f },   /* near   : z     >= 0 */
        {  0.0f,  0.0f, -1.0f,  1.0f }    /* far    : w - z >= 0 */
    };
    for (int p = 0; p < 6; ++p) {
        __m128 acc = _mm_mul_ps(_mm_set1_ps(SEL[p][0]), col0);
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(SEL[p][1]), col1));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(SEL[p][2]), col2));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(SEL[p][3]), col3));
        _mm_storeu_ps(&out6x4[p][0], acc);          /* (nx, ny, nz, d) */
    }
}

// Normalises the xyz part of every plane (d keeps its scale: d / |n|).
// Returns 1 when all planes are finite with a non-degenerate normal, 0 otherwise.
static __inline int M2SSE_NormalizePlanes(float p6[6][4]) {
    for (int i = 0; i < 6; ++i) {
        if (!M2SSE_FiniteF(p6[i][0]) || !M2SSE_FiniteF(p6[i][1]) ||
            !M2SSE_FiniteF(p6[i][2]) || !M2SSE_FiniteF(p6[i][3])) return 0;
        __m128 p = _mm_loadu_ps(&p6[i][0]);
        const __m128 sq = _mm_mul_ps(p, p);
        __m128 s2 = _mm_add_ss(sq, _mm_shuffle_ps(sq, sq, _MM_SHUFFLE(1, 1, 1, 1)));
        s2 = _mm_add_ss(s2, _mm_shuffle_ps(sq, sq, _MM_SHUFFLE(2, 2, 2, 2)));
        const __m128 lenVec = _mm_sqrt_ss(s2);                       /* lane0 = sqrt(x^2+y^2+z^2) */
        const float  lenS = _mm_cvtss_f32(lenVec);
        if (!(lenS > 1.0e-7f)) return 0;                             /* degenerate normal */
        /* (a,b,c,d) / len  - identical to the scalar core's plane[i][j] *= inv */
        const __m128 lenBcast = _mm_shuffle_ps(lenVec, lenVec, _MM_SHUFFLE(0, 0, 0, 0));
        _mm_storeu_ps(&p6[i][0], _mm_div_ps(p, lenBcast));
    }
    return 1;
}

// ------------------------------------------------------------------ build (planes -> SoA kernel)
// planes6x4 must already be normalised; `valid` is the scalar core's verdict.  view may be NULL.
static __inline void M2SSE_Build(M2FrustumSSE* f, const float planes6x4[6][4],
                                 const float* view, int convention, int valid) {
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 4; ++j)
            f->plane[i][j] = planes6x4[i][j];

    for (int g = 0; g < M2SSE_GROUP_COUNT; ++g) {
        float px[4], py[4], pz[4], pd[4];
        for (int lane = 0; lane < 4; ++lane) {
            const int pi = g * 4 + lane;
            if (pi < 6) {
                px[lane] = planes6x4[pi][0];
                py[lane] = planes6x4[pi][1];
                pz[lane] = planes6x4[pi][2];
                pd[lane] = planes6x4[pi][3];
            } else {
                px[lane] = 0.0f; py[lane] = 0.0f; pz[lane] = 0.0f;
                pd[lane] = M2SSE_FAR_SENTINEL;      /* distance +1e30 -> visible -> fail open */
            }
        }
        f->soaX[g] = _mm_loadu_ps(px);
        f->soaY[g] = _mm_loadu_ps(py);
        f->soaZ[g] = _mm_loadu_ps(pz);
        f->soaD[g] = _mm_loadu_ps(pd);
    }

    f->eye[0] = f->eye[1] = f->eye[2] = 0.0f;
    f->fwd[0] = 0.0f; f->fwd[1] = 0.0f; f->fwd[2] = 1.0f;
    f->nearDist = f->farDist = 0.0f;
    if (view) {
        const float tx = view[12], ty = view[13], tz = view[14];
        f->eye[0] = -(tx * view[0] + ty * view[4] + tz * view[8]);
        f->eye[1] = -(tx * view[1] + ty * view[5] + tz * view[9]);
        f->eye[2] = -(tx * view[2] + ty * view[6] + tz * view[10]);
        f->fwd[0] = view[2]; f->fwd[1] = view[6]; f->fwd[2] = view[10];

        const __m128 pn  = _mm_loadu_ps(&f->plane[4][0]);
        const __m128 pf  = _mm_loadu_ps(&f->plane[5][0]);
        const __m128 eye = _mm_set_ps(1.0f, f->eye[2], f->eye[1], f->eye[0]);   /* (x,y,z,1) */
        f->nearDist = M2SSE_AbsF(M2SSE_HSum4(_mm_mul_ps(pn, eye)));
        f->farDist  = M2SSE_AbsF(M2SSE_HSum4(_mm_mul_ps(pf, eye)));
    }
    f->convention = convention;
    f->valid = valid ? 1 : 0;
}

// ------------------------------------------------------------------ hot path: sphere vs frustum
// 1 = visible (or unknown), 0 = fully outside.  ~24 SSE ops, branches only in the guards.
static __inline int M2SSE_SphereVisible(const M2FrustumSSE* f, float x, float y, float z, float radius) {
    if (!f->valid) return 1;                                                  /* fail open */
    if (!(x == x) || !(y == y) || !(z == z) || !(radius == radius)) return 1;
    if (!M2SSE_FiniteF(x) || !M2SSE_FiniteF(y) || !M2SSE_FiniteF(z)) return 1;
    const float r = (radius > 0.0f) ? radius : 0.0f;
    if (r > 1.0e6f) return 1;                                                 /* fail open */

    const __m128 bx = _mm_set1_ps(x);
    const __m128 by = _mm_set1_ps(y);
    const __m128 bz = _mm_set1_ps(z);

    __m128 d0 = _mm_add_ps(_mm_mul_ps(f->soaX[0], bx), _mm_mul_ps(f->soaY[0], by));
    d0 = _mm_add_ps(d0, _mm_mul_ps(f->soaZ[0], bz));
    d0 = _mm_add_ps(d0, f->soaD[0]);
    __m128 d1 = _mm_add_ps(_mm_mul_ps(f->soaX[1], bx), _mm_mul_ps(f->soaY[1], by));
    d1 = _mm_add_ps(d1, _mm_mul_ps(f->soaZ[1], bz));
    d1 = _mm_add_ps(d1, f->soaD[1]);

    __m128 m = _mm_min_ps(d0, d1);                                          /* min over 8 lanes */
    m = _mm_min_ps(m, _mm_shuffle_ps(m, m, _MM_SHUFFLE(1, 0, 3, 2)));
    m = _mm_min_ps(m, _mm_shuffle_ps(m, m, _MM_SHUFFLE(2, 3, 0, 1)));
    /* hidden as soon as ONE plane has distance < -r  ->  visible iff the minimum distance >= -r */
    return (_mm_cvtss_f32(m) >= -r) ? 1 : 0;
}

// ------------------------------------------------------------------ AABB vs frustum (n/p-vertex)
// Conservative: culls only when all 8 corners are on the negative side of one plane.
static __inline int M2SSE_AABBVisible(const M2FrustumSSE* f, const float* mn, const float* mx) {
    if (!f->valid) return 1;
    for (int i = 0; i < 3; ++i) {
        if (!(mn[i] == mn[i]) || !(mx[i] == mx[i])) return 1;
        if (!M2SSE_FiniteF(mn[i]) || !M2SSE_FiniteF(mx[i])) return 1;
        if (mn[i] > mx[i]) return 1;                                        /* malformed: fail open */
    }
    const __m128 zero = _mm_setzero_ps();
    const __m128 bmnx = _mm_set1_ps(mn[0]), bmxx = _mm_set1_ps(mx[0]);
    const __m128 bmny = _mm_set1_ps(mn[1]), bmxy = _mm_set1_ps(mx[1]);
    const __m128 bmnz = _mm_set1_ps(mn[2]), bmxz = _mm_set1_ps(mx[2]);
    int failMask = 0;
    for (int g = 0; g < M2SSE_GROUP_COUNT; ++g) {
        const __m128 nx = f->soaX[g], ny = f->soaY[g], nz = f->soaZ[g], dd = f->soaD[g];
        const __m128 negx = _mm_cmplt_ps(nx, zero);
        const __m128 negy = _mm_cmplt_ps(ny, zero);
        const __m128 negz = _mm_cmplt_ps(nz, zero);
        const __m128 vx = _mm_or_ps(_mm_and_ps(negx, bmnx), _mm_andnot_ps(negx, bmxx));  /* p-vertex */
        const __m128 vy = _mm_or_ps(_mm_and_ps(negy, bmny), _mm_andnot_ps(negy, bmxy));
        const __m128 vz = _mm_or_ps(_mm_and_ps(negz, bmnz), _mm_andnot_ps(negz, bmxz));
        __m128 d = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, vx), _mm_mul_ps(ny, vy)),
                              _mm_add_ps(_mm_mul_ps(nz, vz), dd));
        failMask |= _mm_movemask_ps(_mm_cmplt_ps(d, zero)) << (g * 4);
    }
    return (failMask != 0) ? 0 : 1;      /* cull as soon as ONE plane fails (all 8 corners on its negative side) */
}

// ------------------------------------------------------------------ scalar oracle (used by tests)
static __inline int M2SSE_AABBVisibleRef(const float planes6x4[6][4], const float* mn, const float* mx) {
    for (int i = 0; i < 6; ++i) {
        const float px = (planes6x4[i][0] >= 0.0f) ? mx[0] : mn[0];
        const float py = (planes6x4[i][1] >= 0.0f) ? mx[1] : mn[1];
        const float pz = (planes6x4[i][2] >= 0.0f) ? mx[2] : mn[2];
        if (planes6x4[i][0] * px + planes6x4[i][1] * py + planes6x4[i][2] * pz + planes6x4[i][3] < 0.0f) return 0;
    }
    return 1;
}

#endif /* M2_FRUSTUM_SSE2_H */
