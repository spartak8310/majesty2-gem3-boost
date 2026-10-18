#ifndef M2_RUST_BRIDGE_H
#define M2_RUST_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1. Ultra-fast SIMD Frustum + AABB / Sphere Batch Culler
uint32_t m2_rust_cull_aabb_sse2(
    const float* frustum_planes_ptr,
    uint32_t num_planes,
    const float* aabb_min_ptr,
    const float* aabb_max_ptr,
    uint32_t count,
    uint32_t* out_visible_mask_ptr
);

uint32_t m2_rust_cull_sphere_sse2(
    const float* frustum_planes_ptr,
    uint32_t num_planes,
    const float* spheres_ptr,
    uint32_t count,
    uint32_t* out_visible_mask_ptr
);

// 2. Lock-free 256-entry D3D9 State / Texture / Shader Hash & Dirty-Bit Tracker
int32_t m2_rust_state_filter(uint32_t state_type, uint32_t state_idx, uint32_t state_val);
void m2_rust_state_filter_reset(void);

// 3. Precision PI/Kalman Frame-Pacing Controller
uint64_t m2_rust_pace_compute_target_us(uint64_t last_frame_us, uint32_t target_hz, float ema_alpha);
void m2_rust_pace_reset(uint32_t target_hz);

// 4. Fast Mat34 / Mat44 / Quat Batch Transforms (SSE4.2)
void m2_rust_batch_transform_mat34_sse4(const float* mat_ptr, const float* in_vecs_ptr, float* out_vecs_ptr, uint32_t count);
void m2_rust_batch_transform_mat44_sse4(const float* mat_ptr, const float* in_vecs_ptr, float* out_vecs_ptr, uint32_t count);
void m2_rust_batch_rotate_quat_sse4(const float* quat_ptr, const float* in_vecs_ptr, float* out_vecs_ptr, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // M2_RUST_BRIDGE_H
