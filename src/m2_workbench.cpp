// ==============================================================================
// GEM 3 Isolated Subsystem Micro-Workbench (Clore RAM Fast-Feedback Stand)
// ==============================================================================
// Tests math correctness, SIMD vectorization, and algorithmic throughput
// in ~50ms on 32-thread Ryzen 9 5950X without launching the heavy 40s game client.
// ==============================================================================

#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <xmmintrin.h>
#include <emmintrin.h>

struct Vec3 {
    float x, y, z;
};

struct AABB {
    Vec3 min;
    Vec3 max;
};

struct FrustumPlane {
    float a, b, c, d;
};

struct Frustum {
    FrustumPlane planes[6];
};

// 1. Reference Scalar Frustum Culling
bool CullAABB_Scalar(const Frustum& f, const AABB& box) {
    for (int i = 0; i < 6; ++i) {
        float px = (f.planes[i].a > 0.0f) ? box.max.x : box.min.x;
        float py = (f.planes[i].b > 0.0f) ? box.max.y : box.min.y;
        float pz = (f.planes[i].c > 0.0f) ? box.max.z : box.min.z;
        if (f.planes[i].a * px + f.planes[i].b * py + f.planes[i].c * pz + f.planes[i].d < 0.0f) {
            return false; // fully outside
        }
    }
    return true; // inside or intersecting
}

// 2. Optimized SSE2 Frustum Culling
bool CullAABB_SSE2(const Frustum& f, const AABB& box) {
    __m128 bmin = _mm_set_ps(0.0f, box.min.z, box.min.y, box.min.x);
    __m128 bmax = _mm_set_ps(0.0f, box.max.z, box.max.y, box.max.x);

    for (int i = 0; i < 6; ++i) {
        __m128 p = _mm_set_ps(0.0f, f.planes[i].c, f.planes[i].b, f.planes[i].a);
        __m128 mask = _mm_cmpgt_ps(p, _mm_setzero_ps());
        __m128 v = _mm_or_ps(_mm_and_ps(mask, bmax), _mm_andnot_ps(mask, bmin));

        __m128 dot = _mm_mul_ps(p, v);
        // Horizontal add x + y + z
        __m128 shuf = _mm_shuffle_ps(dot, dot, _MM_SHUFFLE(2, 3, 0, 1));
        __m128 sums = _mm_add_ps(dot, shuf);
        shuf = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(0, 0, 2, 2));
        sums = _mm_add_ss(sums, shuf);

        float d_val;
        _mm_store_ss(&d_val, sums);
        if (d_val + f.planes[i].d < 0.0f) {
            return false;
        }
    }
    return true;
}

// 3. Differential Test & Benchmark
int main() {
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist_pos(-500.0f, 500.0f);
    std::uniform_real_distribution<float> dist_size(1.0f, 20.0f);

    Frustum f;
    // Set up a realistic perspective camera frustum
    for (int i = 0; i < 6; ++i) {
        f.planes[i].a = dist_pos(rng);
        f.planes[i].b = dist_pos(rng);
        f.planes[i].c = dist_pos(rng);
        f.planes[i].d = dist_pos(rng);
        float len = std::sqrt(f.planes[i].a * f.planes[i].a + f.planes[i].b * f.planes[i].b + f.planes[i].c * f.planes[i].c);
        if (len > 1e-4f) {
            f.planes[i].a /= len; f.planes[i].b /= len; f.planes[i].c /= len; f.planes[i].d /= len;
        }
    }

    const int NUM_ENTITIES = 5000;
    std::vector<AABB> entities(NUM_ENTITIES);
    for (int i = 0; i < NUM_ENTITIES; ++i) {
        float cx = dist_pos(rng);
        float cy = dist_pos(rng);
        float cz = dist_pos(rng);
        float sx = dist_size(rng);
        float sy = dist_size(rng);
        float sz = dist_size(rng);
        entities[i] = { {cx - sx, cy - sy, cz - sz}, {cx + sx, cy + sy, cz + sz} };
    }

    // Step 1: Correctness verification (Differential Testing)
    int mismatches = 0;
    for (int i = 0; i < NUM_ENTITIES; ++i) {
        bool ref = CullAABB_Scalar(f, entities[i]);
        bool opt = CullAABB_SSE2(f, entities[i]);
        if (ref != opt) mismatches++;
    }

    // Step 2: Throughput Benchmark (2,000 iterations over 5,000 entities = 10,000,000 culling tests)
    const int ITERS = 1000;
    volatile int sink_scalar = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int it = 0; it < ITERS; ++it) {
        for (int i = 0; i < NUM_ENTITIES; ++i) {
            if (CullAABB_Scalar(f, entities[i])) sink_scalar = sink_scalar + 1;
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    volatile int sink_opt = 0;
    auto t2 = std::chrono::high_resolution_clock::now();
    for (int it = 0; it < ITERS; ++it) {
        for (int i = 0; i < NUM_ENTITIES; ++i) {
            if (CullAABB_SSE2(f, entities[i])) sink_opt = sink_opt + 1;
        }
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    double ms_scalar = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ms_sse2 = std::chrono::duration<double, std::milli>(t3 - t2).count();
    double speedup = (ms_sse2 > 0.0001) ? (ms_scalar / ms_sse2) : 1.0;

    std::cout << "{\n"
              << "  \"test\": \"FrustumCullBench\",\n"
              << "  \"mismatches\": " << mismatches << ",\n"
              << "  \"status\": \"" << (mismatches == 0 ? "PASS" : "FAIL") << "\",\n"
              << "  \"scalar_ms\": " << ms_scalar << ",\n"
              << "  \"sse2_ms\": " << ms_sse2 << ",\n"
              << "  \"speedup\": " << speedup << ",\n"
              << "  \"sink\": " << (sink_scalar + sink_opt) << ",\n"
              << "  \"total_tests\": " << (NUM_ENTITIES * ITERS) << "\n"
              << "}" << std::endl;

    return (mismatches == 0) ? 0 : 1;
}
