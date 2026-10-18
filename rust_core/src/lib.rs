//! m2_rust_core: High-performance 32-bit Win32 SIMD/Pacing Core for Majesty 2 d3d9.dll proxy
//! Target: i686-pc-windows-gnu (SSE2, SSE3, SSSE3, SSE4.1, SSE4.2)
//!
//! Provides:
//! 1. SIMD frustum + AABB/sphere batch culler (`m2_rust_cull_aabb_sse2`, `m2_rust_cull_sphere_sse2`)
//! 2. Lock-free 256-entry D3D9 state/texture/shader hash & dirty-bit tracker (`m2_rust_state_filter`)
//! 3. Precision PI/Kalman frame-pacing controller (`m2_rust_pace_compute_target_us`)
//! 4. Fast Mat34 / Mat44 / Quat batch transforms (`m2_rust_batch_transform_mat34_sse4`, `m2_rust_batch_transform_mat44_sse4`, `m2_rust_batch_rotate_quat_sse4`)

#![no_std]

#[cfg(target_arch = "x86")]
use core::arch::x86::*;

use core::sync::atomic::{AtomicU32, Ordering};

// -------------------------------------------------------------------------------------------------
// Panic Handler for #![no_std] staticlib
// -------------------------------------------------------------------------------------------------
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

// -------------------------------------------------------------------------------------------------
// 1. Ultra-fast SIMD 4-plane/6-plane Frustum + AABB / Sphere Batch Culler
// -------------------------------------------------------------------------------------------------

/// Frustum plane: ax + by + cz + d = 0 (stored as [nx, ny, nz, d])
/// An AABB is defined by min: [min_x, min_y, min_z] and max: [max_x, max_y, max_z]
///
/// For each plane, compute the p-vertex (positive corner along plane normal).
/// If dot(normal, p_vertex) + d < 0, the AABB is completely outside the frustum.
///
/// Output:
/// `out_visible_mask`: array of u32 bitmasks. Bit `(i % 32)` in word `(i / 32)` is 1 if visible.
/// Returns: total count of visible AABBs.
#[no_mangle]
pub unsafe extern "C" fn m2_rust_cull_aabb_sse2(
    frustum_planes_ptr: *const f32,
    num_planes: u32,
    aabb_min_ptr: *const f32,
    aabb_max_ptr: *const f32,
    count: u32,
    out_visible_mask_ptr: *mut u32,
) -> u32 {
    if frustum_planes_ptr.is_null()
        || aabb_min_ptr.is_null()
        || aabb_max_ptr.is_null()
        || out_visible_mask_ptr.is_null()
        || count == 0
    {
        return 0;
    }

    let planes_count = if num_planes > 6 { 6 } else { num_planes as usize };
    let zero = _mm_setzero_ps();

    // Preload plane vectors: plane_xyzw and sign mask for p-vertex selection
    let mut p_normals: [__m128; 6] = [zero; 6];
    let mut p_d: [f32; 6] = [0.0; 6];
    let mut p_masks: [__m128; 6] = [zero; 6];

    for i in 0..planes_count {
        let p_ptr = frustum_planes_ptr.add(i * 4);
        let nx = *p_ptr;
        let ny = *p_ptr.add(1);
        let nz = *p_ptr.add(2);
        let d = *p_ptr.add(3);
        p_d[i] = d;

        let norm = _mm_set_ps(0.0, nz, ny, nx);
        p_normals[i] = norm;
        // mask is all-ones where normal > 0
        p_masks[i] = _mm_cmpgt_ps(norm, zero);
    }

    // Initialize bitmask output buffer to zero
    let mask_words = ((count + 31) / 32) as usize;
    for w in 0..mask_words {
        *out_visible_mask_ptr.add(w) = 0;
    }

    let mut visible_count: u32 = 0;

    for i in 0..count {
        let min_p = aabb_min_ptr.add((i * 3) as usize);
        let max_p = aabb_max_ptr.add((i * 3) as usize);

        let min_v = _mm_set_ps(0.0, *min_p.add(2), *min_p.add(1), *min_p);
        let max_v = _mm_set_ps(0.0, *max_p.add(2), *max_p.add(1), *max_p);

        let mut is_visible = true;

        for p_idx in 0..planes_count {
            let mask = p_masks[p_idx];
            // p-vertex: if mask (normal > 0) choose max, else min
            // (mask & max) | (~mask & min)
            let p_vert = _mm_or_ps(_mm_and_ps(mask, max_v), _mm_andnot_ps(mask, min_v));

            // Dot product: nx*px + ny*py + nz*pz
            let prod = _mm_mul_ps(p_vert, p_normals[p_idx]);

            // Sum lanes: lane0 + lane1 + lane2
            // SSE2 horizontal sum
            let shuf = _mm_shuffle_ps(prod, prod, 0b01_00_11_10); // [y, x, w, z]
            let sums = _mm_add_ps(prod, shuf);
            let shuf2 = _mm_shuffle_ps(sums, sums, 0b00_00_00_01); // [x, x, x, y]
            let dot_xyz = _mm_add_ss(sums, shuf2);

            let mut dot_val: f32 = 0.0;
            _mm_store_ss(&mut dot_val, dot_xyz);

            if dot_val + p_d[p_idx] < 0.0 {
                is_visible = false;
                break;
            }
        }

        if is_visible {
            let word_idx = (i / 32) as usize;
            let bit_idx = i % 32;
            let word_ptr = out_visible_mask_ptr.add(word_idx);
            *word_ptr |= 1u32 << bit_idx;
            visible_count += 1;
        }
    }

    visible_count
}

/// Ultra-fast SIMD Frustum Sphere Batch Culler
/// Spheres format: [cx, cy, cz, radius] contiguous
#[no_mangle]
pub unsafe extern "C" fn m2_rust_cull_sphere_sse2(
    frustum_planes_ptr: *const f32,
    num_planes: u32,
    spheres_ptr: *const f32,
    count: u32,
    out_visible_mask_ptr: *mut u32,
) -> u32 {
    if frustum_planes_ptr.is_null()
        || spheres_ptr.is_null()
        || out_visible_mask_ptr.is_null()
        || count == 0
    {
        return 0;
    }

    let planes_count = if num_planes > 6 { 6 } else { num_planes as usize };
    let mut p_normals: [__m128; 6] = [_mm_setzero_ps(); 6];
    let mut p_d: [f32; 6] = [0.0; 6];

    for i in 0..planes_count {
        let p_ptr = frustum_planes_ptr.add(i * 4);
        p_normals[i] = _mm_set_ps(0.0, *p_ptr.add(2), *p_ptr.add(1), *p_ptr);
        p_d[i] = *p_ptr.add(3);
    }

    let mask_words = ((count + 31) / 32) as usize;
    for w in 0..mask_words {
        *out_visible_mask_ptr.add(w) = 0;
    }

    let mut visible_count: u32 = 0;

    for i in 0..count {
        let s_ptr = spheres_ptr.add((i * 4) as usize);
        let center = _mm_set_ps(0.0, *s_ptr.add(2), *s_ptr.add(1), *s_ptr);
        let radius = *s_ptr.add(3);

        let mut is_visible = true;

        for p_idx in 0..planes_count {
            let prod = _mm_mul_ps(center, p_normals[p_idx]);
            let shuf = _mm_shuffle_ps(prod, prod, 0b01_00_11_10);
            let sums = _mm_add_ps(prod, shuf);
            let shuf2 = _mm_shuffle_ps(sums, sums, 0b00_00_00_01);
            let dot_xyz = _mm_add_ss(sums, shuf2);

            let mut dot_val: f32 = 0.0;
            _mm_store_ss(&mut dot_val, dot_xyz);

            // If center is further behind the plane than radius, it's outside
            if dot_val + p_d[p_idx] < -radius {
                is_visible = false;
                break;
            }
        }

        if is_visible {
            let word_idx = (i / 32) as usize;
            let bit_idx = i % 32;
            let word_ptr = out_visible_mask_ptr.add(word_idx);
            *word_ptr |= 1u32 << bit_idx;
            visible_count += 1;
        }
    }

    visible_count
}

// -------------------------------------------------------------------------------------------------
// 2. Lock-free 256-entry D3D9 State / Texture / Shader Hash & Dirty-Bit Tracker
// -------------------------------------------------------------------------------------------------

const STATE_CACHE_SIZE: usize = 256;
const STATE_EMPTY_TAG: u32 = 0xFFFF_FFFF;

struct StateEntry {
    tag: AtomicU32, // (state_type << 16) | (state_idx & 0xFFFF)
    val: AtomicU32, // cached state_val
}

static STATE_CACHE: [StateEntry; STATE_CACHE_SIZE] = {
    // const init for AtomicU32
    const EMPTY_ENTRY: StateEntry = StateEntry {
        tag: AtomicU32::new(STATE_EMPTY_TAG),
        val: AtomicU32::new(0),
    };
    [EMPTY_ENTRY; STATE_CACHE_SIZE]
};

/// Lock-free 256-entry D3D9 state/texture/shader hash & dirty-bit tracker
///
/// Returns:
///   0 if state value matches cached value (redundant call -> skip/filter)
///   1 if state changed or new entry (dirty -> dispatch to D3D9)
#[no_mangle]
pub extern "C" fn m2_rust_state_filter(state_type: u32, state_idx: u32, state_val: u32) -> i32 {
    let tag = ((state_type & 0xFFFF) << 16) | (state_idx & 0xFFFF);
    // Knuth multiplicative hash for uniform distribution into 256 bins
    let hash = (((tag.wrapping_mul(2654435761u32)) ^ (tag >> 8)) & 0xFF) as usize;

    let entry = &STATE_CACHE[hash];
    let cur_tag = entry.tag.load(Ordering::Acquire);
    let cur_val = entry.val.load(Ordering::Acquire);

    if cur_tag == tag && cur_val == state_val {
        // Redundant call: state is identical
        return 0;
    }

    // State changed or hash collided: store new state
    entry.tag.store(tag, Ordering::Release);
    entry.val.store(state_val, Ordering::Release);
    1
}

/// Reset state cache (e.g. on device Reset or scene transition)
#[no_mangle]
pub extern "C" fn m2_rust_state_filter_reset() {
    for entry in STATE_CACHE.iter() {
        entry.tag.store(STATE_EMPTY_TAG, Ordering::Release);
        entry.val.store(0, Ordering::Release);
    }
}

// -------------------------------------------------------------------------------------------------
// 3. Precision PI/Kalman Frame-Pacing Controller
// -------------------------------------------------------------------------------------------------

static PACER_SMOOTHED_US: AtomicU32 = AtomicU32::new(8333); // default 120Hz (8.33ms)
static PACER_INTEGRAL_ERR: AtomicU32 = AtomicU32::new(0);
static PACER_FRAME_COUNTER: AtomicU32 = AtomicU32::new(0);

/// Precision PI/Kalman frame-pacing controller
/// Computes per-tick target duration in microseconds to maintain smooth 120Hz pacing
/// without bimodal 60/120Hz oscillating jumps.
///
/// `last_frame_us`: duration of preceding frame in microseconds
/// `target_hz`: target refresh rate (typically 120, fallback 60)
/// `ema_alpha`: EMA smoothing weight in range (0.05..0.50), default 0.15
///
/// Returns: pacing target duration in microseconds
#[no_mangle]
pub extern "C" fn m2_rust_pace_compute_target_us(
    last_frame_us: u64,
    target_hz: u32,
    ema_alpha: f32,
) -> u64 {
    let hz = if target_hz == 0 { 120 } else { target_hz };
    let nominal_us = 1_000_000u64 / (hz as u64); // 8333 us for 120Hz

    let alpha = if ema_alpha <= 0.0 || ema_alpha > 1.0 {
        0.15f32
    } else {
        ema_alpha
    };

    PACER_FRAME_COUNTER.fetch_add(1, Ordering::Relaxed);

    // Read previous smoothed value
    let prev_smoothed = PACER_SMOOTHED_US.load(Ordering::Relaxed) as f32;
    let sample = (last_frame_us.min(100_000)) as f32; // clamp 100ms max

    // EMA update: S_t = alpha * Y_t + (1 - alpha) * S_{t-1}
    let new_smoothed = alpha * sample + (1.0 - alpha) * prev_smoothed;
    PACER_SMOOTHED_US.store(new_smoothed as u32, Ordering::Relaxed);

    // Proportional error: positive if running slow, negative if running fast
    let error = new_smoothed - (nominal_us as f32);

    // PI control constants: Kp = 0.25, Ki = 0.05
    let kp = 0.25f32;
    let ki = 0.05f32;

    let prev_int = (PACER_INTEGRAL_ERR.load(Ordering::Relaxed) as i32) as f32;
    // Anti-windup clamping on integral term (-2000us .. +2000us)
    let new_int = (prev_int + error * ki).max(-2000.0).min(2000.0);
    PACER_INTEGRAL_ERR.store((new_int as i32) as u32, Ordering::Relaxed);

    // Control adjustment
    let adjustment = error * kp + new_int;

    // Nominal target minus adjustment to maintain phase lock
    let target_f = (nominal_us as f32) - adjustment;

    // Hard bounds: clamp target between 1,000 us (1ms floor) and 2 * nominal_us (16.6ms)
    let final_target = (target_f as i64).max(1000).min((nominal_us * 2) as i64) as u64;

    final_target
}

/// Reset frame pacer state
#[no_mangle]
pub extern "C" fn m2_rust_pace_reset(target_hz: u32) {
    let hz = if target_hz == 0 { 120 } else { target_hz };
    let nominal_us = (1_000_000u32 / hz) as u32;
    PACER_SMOOTHED_US.store(nominal_us, Ordering::Release);
    PACER_INTEGRAL_ERR.store(0, Ordering::Release);
    PACER_FRAME_COUNTER.store(0, Ordering::Release);
}

// -------------------------------------------------------------------------------------------------
// 4. Fast Mat34 / Mat44 / Quat Batch Transforms (SSE4.1 / SSE4.2)
// -------------------------------------------------------------------------------------------------

/// Fast batch transformation of 3D vectors by 3x4 affine matrix
/// `mat_ptr`: pointer to 12 floats:
///   row0: [m00, m01, m02, tx]
///   row1: [m10, m11, m12, ty]
///   row2: [m20, m21, m22, tz]
/// `in_vecs_ptr`: count * 3 floats [x, y, z]
/// `out_vecs_ptr`: count * 3 floats [x', y', z']
#[no_mangle]
pub unsafe extern "C" fn m2_rust_batch_transform_mat34_sse4(
    mat_ptr: *const f32,
    in_vecs_ptr: *const f32,
    out_vecs_ptr: *mut f32,
    count: u32,
) {
    if mat_ptr.is_null() || in_vecs_ptr.is_null() || out_vecs_ptr.is_null() || count == 0 {
        return;
    }

    let r0 = _mm_loadu_ps(mat_ptr);
    let r1 = _mm_loadu_ps(mat_ptr.add(4));
    let r2 = _mm_loadu_ps(mat_ptr.add(8));

    for i in 0..(count as usize) {
        let in_p = in_vecs_ptr.add(i * 3);
        let out_p = out_vecs_ptr.add(i * 3);

        // Vector with w = 1.0: [x, y, z, 1.0]
        let v = _mm_set_ps(1.0, *in_p.add(2), *in_p.add(1), *in_p);

        // SSE4.1 Dot products: _mm_dp_ps with mask 0xF1 (dot 4 components, store in lane 0)
        let dp0 = _mm_dp_ps(r0, v, 0xF1);
        let dp1 = _mm_dp_ps(r1, v, 0xF1);
        let dp2 = _mm_dp_ps(r2, v, 0xF1);

        let mut x_out: f32 = 0.0;
        let mut y_out: f32 = 0.0;
        let mut z_out: f32 = 0.0;

        _mm_store_ss(&mut x_out, dp0);
        _mm_store_ss(&mut y_out, dp1);
        _mm_store_ss(&mut z_out, dp2);

        *out_p = x_out;
        *out_p.add(1) = y_out;
        *out_p.add(2) = z_out;
    }
}

/// Fast batch transformation of 4D vectors by 4x4 matrix
/// `mat_ptr`: pointer to 16 floats (row-major)
/// `in_vecs_ptr`: count * 4 floats [x, y, z, w]
/// `out_vecs_ptr`: count * 4 floats [x', y', z', w']
#[no_mangle]
pub unsafe extern "C" fn m2_rust_batch_transform_mat44_sse4(
    mat_ptr: *const f32,
    in_vecs_ptr: *const f32,
    out_vecs_ptr: *mut f32,
    count: u32,
) {
    if mat_ptr.is_null() || in_vecs_ptr.is_null() || out_vecs_ptr.is_null() || count == 0 {
        return;
    }

    let r0 = _mm_loadu_ps(mat_ptr);
    let r1 = _mm_loadu_ps(mat_ptr.add(4));
    let r2 = _mm_loadu_ps(mat_ptr.add(8));
    let r3 = _mm_loadu_ps(mat_ptr.add(12));

    for i in 0..(count as usize) {
        let in_p = in_vecs_ptr.add(i * 4);
        let out_p = out_vecs_ptr.add(i * 4);

        let v = _mm_loadu_ps(in_p);

        let dp0 = _mm_dp_ps(r0, v, 0xF1);
        let dp1 = _mm_dp_ps(r1, v, 0xF1);
        let dp2 = _mm_dp_ps(r2, v, 0xF1);
        let dp3 = _mm_dp_ps(r3, v, 0xF1);

        let mut x_out: f32 = 0.0;
        let mut y_out: f32 = 0.0;
        let mut z_out: f32 = 0.0;
        let mut w_out: f32 = 0.0;

        _mm_store_ss(&mut x_out, dp0);
        _mm_store_ss(&mut y_out, dp1);
        _mm_store_ss(&mut z_out, dp2);
        _mm_store_ss(&mut w_out, dp3);

        *out_p = x_out;
        *out_p.add(1) = y_out;
        *out_p.add(2) = z_out;
        *out_p.add(3) = w_out;
    }
}

/// Fast batch quaternion vector rotation: v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
/// `quat_ptr`: [qx, qy, qz, qw]
/// `in_vecs_ptr`: count * 3 floats [x, y, z]
/// `out_vecs_ptr`: count * 3 floats [x', y', z']
#[no_mangle]
pub unsafe extern "C" fn m2_rust_batch_rotate_quat_sse4(
    quat_ptr: *const f32,
    in_vecs_ptr: *const f32,
    out_vecs_ptr: *mut f32,
    count: u32,
) {
    if quat_ptr.is_null() || in_vecs_ptr.is_null() || out_vecs_ptr.is_null() || count == 0 {
        return;
    }

    let qx = *quat_ptr;
    let qy = *quat_ptr.add(1);
    let qz = *quat_ptr.add(2);
    let qw = *quat_ptr.add(3);

    for i in 0..(count as usize) {
        let in_p = in_vecs_ptr.add(i * 3);
        let out_p = out_vecs_ptr.add(i * 3);

        let vx = *in_p;
        let vy = *in_p.add(1);
        let vz = *in_p.add(2);

        // t = 2 * cross(q.xyz, v)
        let tx = 2.0 * (qy * vz - qz * vy);
        let ty = 2.0 * (qz * vx - qx * vz);
        let tz = 2.0 * (qx * vy - qy * vx);

        // v' = v + qw * t + cross(q.xyz, t)
        *out_p = vx + qw * tx + (qy * tz - qz * ty);
        *out_p.add(1) = vy + qw * ty + (qz * tx - qx * tz);
        *out_p.add(2) = vz + qw * tz + (qx * ty - qy * tx);
    }
}
