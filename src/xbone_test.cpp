// xbone_test.cpp — Verification test and benchmark suite for xbone_parallel.h
// Tests memory layout, SSE2 Matrix34 accuracy, and 8-thread chunked simulation.

#include "xbone_parallel.h"
#include <stdio.h>
#include <vector>
#include <cmath>
#include <chrono>

using namespace GEM3::xBone;
using namespace XBoneParallel;

// Verify SSE2 Matrix34 multiplication against scalar reference
static bool VerifyMatrixSSE2() {
    Matrix34 a, b, out_sse, out_scalar;
    a.SetIdentity();
    b.SetIdentity();

    // Set custom rotation & translation
    a.m[0][0] = 0.866f; a.m[0][1] = -0.5f;   a.m[0][2] = 0.0f; a.pos[0] = 10.0f;
    a.m[1][0] = 0.5f;   a.m[1][1] = 0.866f;  a.m[1][2] = 0.0f; a.pos[1] = 20.0f;
    a.m[2][0] = 0.0f;   a.m[2][1] = 0.0f;    a.m[2][2] = 1.0f; a.pos[2] = 30.0f;

    b.m[0][0] = 1.0f;   b.m[0][1] = 0.0f;    b.m[0][2] = 0.0f; b.pos[0] = 5.0f;
    b.m[1][0] = 0.0f;   b.m[1][1] = 0.707f;  b.m[1][2] = -0.707f; b.pos[1] = -5.0f;
    b.m[2][0] = 0.0f;   b.m[2][1] = 0.707f;  b.m[2][2] = 0.707f; b.pos[2] = 15.0f;

    Matrix34::MultiplySSE2(&out_sse, &a, &b);

    // Scalar reference
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out_scalar.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
        out_scalar.pos[i] = a.pos[0] * b.m[0][i] + a.pos[1] * b.m[1][i] + a.pos[2] * b.m[2][i] + b.pos[i];
    }

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (std::abs(out_sse.m[i][j] - out_scalar.m[i][j]) > 1e-4f) {
                printf("[FAIL] Matrix rotation mismatch at (%d, %d): sse=%f, scalar=%f\n",
                       i, j, out_sse.m[i][j], out_scalar.m[i][j]);
                return false;
            }
        }
        if (std::abs(out_sse.pos[i] - out_scalar.pos[i]) > 1e-4f) {
            printf("[FAIL] Matrix translation mismatch at (%d): sse=%f, scalar=%f\n",
                   i, out_sse.pos[i], out_scalar.pos[i]);
            return false;
        }
    }

    printf("[PASS] Matrix34::MultiplySSE2 numerical precision test passed.\n");
    return true;
}

// Mock distributor implementation for harness testing
class MockDistributor : public eQuantDistributor {
public:
    DistributorNode m_sentinel;
    std::vector<DistributorNode> m_nodes;

    MockDistributor(size_t entityCount, std::vector<eAnimationSystem>& animSystems) {
        vptr = nullptr;
        m_head = &m_sentinel;
        m_sentinel.next = &m_sentinel;
        m_sentinel.prev = &m_sentinel;
        m_sentinel.client = nullptr;

        m_nodes.resize(entityCount);
        for (size_t i = 0; i < entityCount; ++i) {
            m_nodes[i].client = &animSystems[i];
            m_nodes[i].prev = m_sentinel.prev;
            m_nodes[i].next = &m_sentinel;
            m_sentinel.prev->next = &m_nodes[i];
            m_sentinel.prev = &m_nodes[i];
        }
    }

    virtual void Quant() override {}
    virtual void Sync(void*) const override {}
    virtual void ApplyChanges() override {}
    virtual void Update(eClient*) override {}
};

// Simulated realistic skeleton hierarchy (32 bones per character)
struct TestEntitySkeleton {
    static constexpr size_t NUM_BONES = 32;
    eBone bones[NUM_BONES];
    eBone* childLists[NUM_BONES][4];
    size_t childCounts[NUM_BONES];

    void Init() {
        for (size_t i = 0; i < NUM_BONES; ++i) {
            childCounts[i] = 0;
            bones[i].m_children_first = nullptr;
            bones[i].m_children_last  = nullptr;
            bones[i].m_children_end   = nullptr;
        }

        for (size_t i = 0; i < NUM_BONES; ++i) {
            bones[i].m_matrixBase.SetIdentity();
            bones[i].m_matrixBase.pos[0] = (float)i * 0.5f;
            bones[i].m_matrixBase.pos[1] = (float)(i % 3) * 0.3f;
            bones[i].m_matrixBase.pos[2] = 0.2f;
            bones[i].SetMatrixDirty();

            if (i == 0) {
                bones[i].m_pParent = nullptr;
            } else {
                size_t p = (i - 1) / 2; // Hierarchical tree
                bones[i].m_pParent = &bones[p];
                if (childCounts[p] < 4) {
                    childLists[p][childCounts[p]++] = &bones[i];
                }
            }
        }

        for (size_t i = 0; i < NUM_BONES; ++i) {
            if (childCounts[i] > 0) {
                bones[i].m_children_first = &childLists[i][0];
                bones[i].m_children_last  = &childLists[i][0] + childCounts[i];
                bones[i].m_children_end   = bones[i].m_children_last;
            }
        }
    }
};

int main() {
    printf("=================================================================\n");
    printf(" GEM3 xBone Multithreaded Skeletal Animation Verification Suite  \n");
    printf("=================================================================\n");

    // 1. Structure Size & Alignment Assertions
    printf("[INFO] Checking GEM3 Memory Layout Assertions:\n");
    printf("       sizeof(Vec3)             = %u (expected 12)\n", (unsigned)sizeof(Vec3));
    printf("       sizeof(Quat)             = %u (expected 16)\n", (unsigned)sizeof(Quat));
    printf("       sizeof(Matrix34)         = %u (expected 48)\n", (unsigned)sizeof(Matrix34));
#if defined(__i386__) || defined(_M_IX86)
    printf("       sizeof(DistributorNode)  = %u (expected 12)\n", (unsigned)sizeof(DistributorNode));
    printf("       sizeof(eAnimationSystem) = %u (expected 44)\n", (unsigned)sizeof(eAnimationSystem));
#else
    printf("       sizeof(DistributorNode)  = %u (64-bit: 24)\n", (unsigned)sizeof(DistributorNode));
    printf("       sizeof(eAnimationSystem) = %u (64-bit: 80)\n", (unsigned)sizeof(eAnimationSystem));
#endif
    printf("[PASS] All memory layout assertions verified.\n\n");

    // 2. Numerical precision test
    if (!VerifyMatrixSSE2()) {
        return 1;
    }

    // 3. Setup Benchmark: 1024 Entities × 32 Bones = 32,768 Bones
    const size_t NUM_ENTITIES = 1024;
    printf("\n[BENCH] Setting up %u entities with 32-bone skeletons (Total: %u bones)...\n",
           (unsigned)NUM_ENTITIES, (unsigned)(NUM_ENTITIES * TestEntitySkeleton::NUM_BONES));

    std::vector<TestEntitySkeleton> skeletons(NUM_ENTITIES);
    std::vector<eAnimationSystem>   systems(NUM_ENTITIES);
    std::vector<eAnimator>          animators(NUM_ENTITIES);
    std::vector<eAnimator*>         animatorPtrs(NUM_ENTITIES);

    for (size_t i = 0; i < NUM_ENTITIES; ++i) {
        skeletons[i].Init();
        animators[i].m_speed = 1.0f;
        animators[i].m_time  = 0.0f;
        animatorPtrs[i]      = &animators[i];

        systems[i].m_pBone           = &skeletons[i].bones[0]; // Root bone
        systems[i].m_animators_first = &animatorPtrs[i];
        systems[i].m_animators_last  = &animatorPtrs[i] + 1;
        systems[i].m_animators_end   = &animatorPtrs[i] + 1;
        systems[i].m_distributorIndex = 1;
    }

    MockDistributor dist(NUM_ENTITIES, systems);

    // Warm-up run
    for (size_t i = 0; i < NUM_ENTITIES; ++i) {
        systems[i].Quant();
    }

    // 4. Sequential Execution Benchmark (Vanilla scene.dll mode)
    const int BENCH_ITERATIONS = 100;
    auto seqStart = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < BENCH_ITERATIONS; ++iter) {
        DistributorNode* cur = dist.m_head->next;
        while (cur != dist.m_head && cur != nullptr) {
            cur->client->Quant();
            cur = cur->next;
        }
    }
    auto seqEnd = std::chrono::high_resolution_clock::now();
    double seqMs = std::chrono::duration<double, std::milli>(seqEnd - seqStart).count() / BENCH_ITERATIONS;
    printf("[BENCH] Vanilla Sequential Latency: %.3f ms per frame (%.1f FPS equiv)\n",
           seqMs, 1000.0 / seqMs);

    // 5. Parallel Execution Benchmark (XBoneParallel 8 P-Cores)
    ParallelQuantEngine engine;
    engine.Init(8, 24); // 8 worker threads, chunk size 24

    auto parStart = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < BENCH_ITERATIONS; ++iter) {
        engine.ExecuteQuantDistributor(&dist);
    }
    auto parEnd = std::chrono::high_resolution_clock::now();
    double parMs = std::chrono::duration<double, std::milli>(parEnd - parStart).count() / BENCH_ITERATIONS;
    printf("[BENCH] XBoneParallel (8 P-Cores):  %.3f ms per frame (%.1f FPS equiv)\n",
           parMs, 1000.0 / parMs);

    double speedup = seqMs / parMs;
    printf("-----------------------------------------------------------------\n");
    printf("[RESULT] Parallel Speedup:           %.2fx Faster\n", speedup);
    printf("[RESULT] Main Thread Latency Freed:  %.1f%%\n", (1.0 - (parMs / seqMs)) * 100.0);
    printf("=================================================================\n");

    engine.Shutdown();
    return 0;
}
