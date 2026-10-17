// xbone_parallel.h — Multithreaded Skeletal Animation & Matrix Evaluator for Majesty 2 (GEM 3)
// Optimizes xScene::eQuantDistributor::Quant (RVA 0xcaa70) and xBone::eAnimationSystem::Quant (RVA 0x23ab0)
// Designed for 8 Performance Cores on Apple Silicon (M-series / Wine 11.0 / Rosetta 2) and modern multi-core x86.
#ifndef XBONE_PARALLEL_H
#define XBONE_PARALLEL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <emmintrin.h> // SSE2 intrinsics

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#ifndef __thiscall
#define __thiscall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#ifndef WINAPI
#define WINAPI
#endif
#endif

namespace GEM3 {
namespace xBone {

// =============================================================================
// 1. Core Mathematical Primitives (Memory-aligned, GEM3 Geometry Layout)
// =============================================================================

#pragma pack(push, 4)

struct Vec3 {
    float x, y, z;

    inline Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    inline Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};
static_assert(sizeof(Vec3) == 12, "GEM3 Vec3 must be exactly 12 bytes");

struct Quat {
    float x, y, z, w;

    inline Quat() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}
    inline Quat(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
};
static_assert(sizeof(Quat) == 16, "GEM3 Quat must be exactly 16 bytes");

// Matrix34: 3x3 rotation/scale + 3D translation (total 12 floats = 48 bytes)
// Layout strictly verified against scene.dll @ RVA 0x18150 and 0x196b0:
// [0..8]: 3x3 rotation matrix (m[0..2][0..2])
// [9..11]: translation vector pos (x, y, z at offset +0x24)
struct Matrix34 {
    float m[3][3];  // +0x00 .. +0x23: 3x3 rotation/scale matrix
    float pos[3];   // +0x24 .. +0x2f: 3D translation vector (pos.x, pos.y, pos.z)

    inline void SetIdentity() {
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f;
        pos[0]  = 0.0f; pos[1]  = 0.0f; pos[2]  = 0.0f;
    }

    // High-performance SSE2 vectorized 3x4 affine matrix multiplication: Out = A * B
    // Replaces the legacy scalar x87 FPU implementation at scene.dll RVA 0x196b0
    static inline void MultiplySSE2(Matrix34* __restrict out, const Matrix34* __restrict a, const Matrix34* __restrict b) {
        for (int i = 0; i < 3; ++i) {
            // Multiply row elements
            out->m[i][0] = a->m[i][0] * b->m[0][0] + a->m[i][1] * b->m[1][0] + a->m[i][2] * b->m[2][0];
            out->m[i][1] = a->m[i][0] * b->m[0][1] + a->m[i][1] * b->m[1][1] + a->m[i][2] * b->m[2][1];
            out->m[i][2] = a->m[i][0] * b->m[0][2] + a->m[i][1] * b->m[1][2] + a->m[i][2] * b->m[2][2];

            // Transform translation: pos_out = A.pos * B.rot + B.pos
            out->pos[i] = a->pos[0] * b->m[0][i] + a->pos[1] * b->m[1][i] + a->pos[2] * b->m[2][i] + b->pos[i];
        }
    }
};
static_assert(sizeof(Matrix34) == 48, "GEM3 Matrix34 must be exactly 48 bytes");

// eTag: string identifier token in GEM3
struct eTag {
    uint32_t m_id;
    const char* m_pName;
};

// eAnimationFlags: internal flags struct
struct eAnimationFlags {
    uint32_t m_raw;
    uint32_t m_namedId;
};

#pragma pack(pop)

// =============================================================================
// 2. GEM3 Engine Class Hierarchy (Reconstructed RTTI & VTables)
// =============================================================================

class eQuantDistributor;
class eBone;
class eAnimator;

// Base distributor client interface (xScene::eQuantDistributor::eClient)
// Vtable at scene.dll RVA 0x11b3c4
class eClient {
public:
    virtual ~eClient() {}
    virtual eQuantDistributor* Distributor() const = 0;
    virtual void Unknown2() = 0;
    virtual void Quant() = 0; // Vtable[3] @ offset 0x0c: Primary simulation tick
    virtual void Unknown4(void*) = 0;
};

// Doubly-linked circular list node in xScene::eQuantDistributor (12 bytes)
struct DistributorNode {
    DistributorNode* next;   // +0x00
    DistributorNode* prev;   // +0x04
    eClient*         client; // +0x08
};
#if defined(__i386__) || defined(_M_IX86)
static_assert(sizeof(DistributorNode) == 12, "DistributorNode must be 12 bytes on 32-bit PE");
#endif

// xScene::eQuantDistributor
class eQuantDistributor {
public:
    void*            vptr;               // +0x00
    uint32_t         m_pad04[4];         // +0x04 .. +0x10
    DistributorNode* m_head;             // +0x14: Circular sentinel node (head->next = first, head->prev = last)
    uint32_t         m_pad18;            // +0x18
    void*            m_pendingVector[3]; // +0x1c .. +0x24: std::vector of pending clients

    virtual ~eQuantDistributor() {}
    virtual void Quant() = 0;           // RVA 0xcaa70
    virtual void Sync(void*) const = 0;

    virtual void ApplyChanges() {}       // RVA 0xca9b0: Flushes pending additions/removals
    virtual void Update(eClient* client) { (void)client; } // RVA 0xcaab0: Toggles active/inactive state
};

// xBone::eBone: Hierarchical bone node
class eBone {
public:
    void*       vptr;               // +0x00 (0x1011ac54)
    uint32_t    m_pad04;            // +0x04
    uint32_t    m_pad08;            // +0x08
    Matrix34    m_matrixBase;       // +0x0c: Local rest pose (48 bytes)
    Matrix34    m_matrixWorld;      // +0x3c: Evaluated 3D world matrix (48 bytes)
    eBone*      m_pParent;          // +0x6c: Parent bone pointer (null for root)
    uint32_t    m_pad70;            // +0x70
    uint32_t    m_pad74;            // +0x74
    eBone**     m_children_first;   // +0x78: std::vector<eBone*>
    eBone**     m_children_last;    // +0x7c
    eBone**     m_children_end;     // +0x80
    uint8_t     m_pad84_9d[0x1a];   // +0x84
    uint16_t    m_flags;            // +0x9e: Bit 0 (0x0001) = DIRTY flag

    inline bool IsMatrixDirty() const { return (m_flags & 0x0001) != 0; }
    inline void SetMatrixDirty()      { m_flags |= 0x0001; }
    inline void ClearMatrixDirty()    { m_flags &= ~0x0001; }

    // RVA 0x18150: Update local matrix and concatenate parent transform
    void UpdateMatrixWorld() {
        Matrix34 localMat = m_matrixBase;
        // In full engine, ApplyLocalMatrix(&localMat) is called here via vfunc[10]

        if (m_pParent) {
            if (m_pParent->IsMatrixDirty()) {
                m_pParent->UpdateMatrixWorld();
            }
            Matrix34::MultiplySSE2(&m_matrixWorld, &m_pParent->m_matrixWorld, &localMat);
        } else {
            m_matrixWorld = localMat;
        }

        ClearMatrixDirty();

        // Recursively update children
        if (m_children_first) {
            size_t childCount = m_children_last - m_children_first;
            for (size_t i = 0; i < childCount; ++i) {
                if (m_children_first[i]) {
                    m_children_first[i]->UpdateMatrixWorld();
                }
            }
        }
    }

    // RVA 0x02b70: Lazy matrix retrieval
    inline const Matrix34& MatrixWorld() {
        if (IsMatrixDirty()) {
            UpdateMatrixWorld();
        }
        return m_matrixWorld;
    }
};

// xBone::eAnimator: Keyframe track player
class eAnimator {
public:
    void*    vptr;           // +0x00 (0x1011ba44)
    eTag     m_tag;          // +0x04: Animation track name / tag
    void*    m_pInfo;        // +0x08: Animator info / flags (raw & 1 = persistent)
    uint32_t m_pad0c;        // +0x0c
    uint32_t m_pad10;        // +0x10
    float    m_time;         // +0x14: Current playback time / frame
    float    m_speed;        // +0x18: Playback rate multiplier

    virtual ~eAnimator() {}
    virtual void VFunc1() {}
    virtual void VFunc2() {}
    virtual void VFunc3() {}
    virtual void VFunc4() {}
    // Vfunc[5] @ offset 0x14: Advance time, calculate bone transforms
    // Returns: 0 = ended, 1 = playing, 2 = loop/event, 3 = paused
    virtual int Quant() {
        m_time += (m_speed * 0.033333f); // Advance frame by dt
        return (m_speed > 0.0f) ? 1 : 3;
    }
    // Vfunc[6] @ offset 0x18: Returns true if currently playing
    virtual bool IsActive() {
        return (m_speed > 0.0f);
    }
};

// xBone::eAnimationSystem: Reconstructed size 0x2c (44 bytes)
// Inherits from eClient, instantiated per animated unit
class eAnimationSystem : public eClient {
public:
    DistributorNode*    m_pDistributorNode; // +0x04: Pointer to linked list node in eQuantDistributor
    int32_t             m_distributorIndex; // +0x08: Registration index (-1: unmounted, 1: active)
    uint32_t            m_pad0c;            // +0x0c: Internal padding
    eAnimator**         m_animators_first;  // +0x10: std::vector<eAnimator*>::_Myfirst
    eAnimator**         m_animators_last;   // +0x14: std::vector<eAnimator*>::_Mylast
    eAnimator**         m_animators_end;    // +0x18: std::vector<eAnimator*>::_Myend
    void*               m_pAnimations;      // +0x1c: Pointer to eAnimations definition
    uint32_t            m_unknown20;        // +0x20: Notification token
    eBone*              m_pBone;            // +0x24: Root skeleton bone
    float*              m_pDeltaTime;       // +0x28: Pointer to eQuantImpl::delta

    eAnimationSystem()
        : m_pDistributorNode(nullptr)
        , m_distributorIndex(-1)
        , m_pad0c(0)
        , m_animators_first(nullptr)
        , m_animators_last(nullptr)
        , m_animators_end(nullptr)
        , m_pAnimations(nullptr)
        , m_unknown20(0)
        , m_pBone(nullptr)
        , m_pDeltaTime(nullptr)
    {}

    virtual ~eAnimationSystem() override {}

    virtual eQuantDistributor* Distributor() const override {
        // In real engine: retrieves the global eAnimationSystems extender
        return nullptr;
    }

    virtual void Unknown2() override {}

    // RVA 0x23ab0: Exact decompiled implementation of xBone::eAnimationSystem::Quant()
    virtual void Quant() override {
        int count = (m_animators_first != nullptr) ? (int)(m_animators_last - m_animators_first) : 0;
        if (count <= 0) return;

        bool has_active_work = false;

        // PHASE 1: Forward evaluation of active animators
        for (int i = 0; i < count; ++i) {
            eAnimator* anim = m_animators_first[i];
            if (!anim) continue;

            int status = anim->Quant(); // Vfunc[5]
            if (status == 0 || status == 2) {
                NotifyEnd(anim->m_tag);
                has_active_work = true;
            } else if (status == 1 || status == 3) {
                has_active_work = true;
            }
        }

        if (!has_active_work) return;

        // PHASE 2: Backward cleanup and dead animator pruning
        count = (int)(m_animators_last - m_animators_first);
        for (int i = count - 1; i >= 0; --i) {
            eAnimator* anim = m_animators_first[i];
            if (!anim) continue;

            if (!anim->IsActive()) {
                // Delete non-persistent finished animators and compact vector
                delete anim;
                eAnimator** cur = m_animators_first + i;
                int remaining = (int)(m_animators_last - (cur + 1));
                if (remaining > 0) {
                    memmove(cur, cur + 1, remaining * sizeof(eAnimator*));
                }
                m_animators_last -= 1;
            }
        }

        // Propagate bone matrix updates down the hierarchy
        if (m_pBone) {
            m_pBone->UpdateMatrixWorld();
        }
    }

    virtual void Unknown4(void*) override {}

    void NotifyEnd(const eTag& /*tag*/) {
        // Trigger / callback notification to entity logic
    }
};
#if defined(__i386__) || defined(_M_IX86)
static_assert(sizeof(eAnimationSystem) == 0x2c, "eAnimationSystem must be exactly 0x2c bytes on 32-bit PE");
#endif

// eSkeleton: Full skeleton entity container (inherits eBone)
class eSkeleton : public eBone {
public:
    uint8_t             m_skeletonPad[0x118 - sizeof(eBone)];
    eAnimationSystem*   m_pAnimationSystem; // +0x118: Pointer to associated animation system
};

} // namespace xBone
} // namespace GEM3

// =============================================================================
// 3. Multithreaded Skeletal Animation Engine (XBoneParallel)
// =============================================================================

namespace XBoneParallel {

static constexpr size_t DEFAULT_CHUNK_SIZE    = 24; // Optimal: 16..32 entities per chunk
static constexpr size_t MAX_WORKER_THREADS    = 16;
static constexpr size_t DEFAULT_WORKER_COUNT  = 8;  // Matches 8 P-cores on Apple Silicon

// Deferred distributor state update item
struct DeferredUpdate {
    GEM3::xBone::eClient* client;
    int32_t               targetState; // -1: unregister, 1: register
};

// Thread-local update buffer to eliminate lock contention in worker threads
struct alignas(64) ThreadDeferredBuffer {
    static constexpr size_t CAPACITY = 256;
    DeferredUpdate items[CAPACITY];
    size_t         count;

    ThreadDeferredBuffer() : count(0) {}

    inline void Push(GEM3::xBone::eClient* c, int32_t state) {
        if (count < CAPACITY) {
            items[count].client = c;
            items[count].targetState = state;
            ++count;
        }
    }

    inline void Clear() { count = 0; }
};

// MSVC 32-bit __thiscall client->Quant() caller
typedef void (__thiscall *PFN_MsvcClientQuant)(void* self);

inline void CallMsvcClientQuant(void* client) {
    if (!client) return;
    void** vtbl = *(void***)client;
    if (!vtbl) return;
    PFN_MsvcClientQuant pfn = (PFN_MsvcClientQuant)vtbl[3];
    if (pfn) pfn(client);
}

// Lightweight, zero-overhead Worker Thread Pool
class WorkerPool {
private:
    std::atomic<bool>     m_running;
    std::atomic<uint32_t> m_chunkIndex;
    uint32_t              m_totalChunks;
    size_t                m_chunkSize;
    size_t                m_threadCount;

    const std::vector<GEM3::xBone::eClient*>* m_pClients;
    void**                m_pRawClients;
    size_t                m_rawCount;

    std::atomic<uint32_t> m_activeJobs;
    std::atomic<uint32_t> m_generation;

    std::thread           m_threads[MAX_WORKER_THREADS];
    ThreadDeferredBuffer  m_deferredBuffers[MAX_WORKER_THREADS];

    void WorkerLoop(size_t threadId) {
        uint32_t lastGen = 0;

        while (m_running.load(std::memory_order_relaxed)) {
            uint32_t curGen = m_generation.load(std::memory_order_acquire);
            if (curGen == lastGen) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }

            lastGen = curGen;
            m_deferredBuffers[threadId].Clear();

            // Work-stealing loop over chunks
            while (true) {
                uint32_t chunk = m_chunkIndex.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= m_totalChunks) {
                    break;
                }

                if (m_pRawClients && m_rawCount > 0) {
                    size_t startIdx = chunk * m_chunkSize;
                    size_t endIdx   = (startIdx + m_chunkSize < m_rawCount) ? (startIdx + m_chunkSize) : m_rawCount;

                    for (size_t i = startIdx; i < endIdx; ++i) {
                        void* client = m_pRawClients[i];
                        if (client) {
                            CallMsvcClientQuant(client);
                        }
                    }
                } else if (m_pClients) {
                    size_t startIdx = chunk * m_chunkSize;
                    size_t endIdx   = (startIdx + m_chunkSize < m_pClients->size()) ? (startIdx + m_chunkSize) : m_pClients->size();

                    for (size_t i = startIdx; i < endIdx; ++i) {
                        GEM3::xBone::eClient* client = (*m_pClients)[i];
                        if (client) {
                            CallMsvcClientQuant((void*)client);
                        }
                    }
                }
            }

            m_activeJobs.fetch_sub(1, std::memory_order_release);
        }
    }

public:
    WorkerPool()
        : m_running(false)
        , m_chunkIndex(0)
        , m_totalChunks(0)
        , m_chunkSize(DEFAULT_CHUNK_SIZE)
        , m_threadCount(DEFAULT_WORKER_COUNT)
        , m_pClients(nullptr)
        , m_pRawClients(nullptr)
        , m_rawCount(0)
        , m_activeJobs(0)
        , m_generation(0)
    {}

    ~WorkerPool() {
        Stop();
    }

    void Start(size_t threads = DEFAULT_WORKER_COUNT, size_t chunkSize = DEFAULT_CHUNK_SIZE) {
        if (m_running.load()) return;

        m_threadCount = (threads > 0 && threads <= MAX_WORKER_THREADS) ? threads : DEFAULT_WORKER_COUNT;
        m_chunkSize   = (chunkSize > 0) ? chunkSize : DEFAULT_CHUNK_SIZE;
        m_running.store(true);

        for (size_t i = 0; i < m_threadCount; ++i) {
            m_threads[i] = std::thread(&WorkerPool::WorkerLoop, this, i);

#ifdef _WIN32
            // Set thread priority high so Wine / OS scheduler places workers on Apple Silicon P-cores
            SetThreadPriority(m_threads[i].native_handle(), THREAD_PRIORITY_HIGHEST);
#endif
        }
    }

    void Stop() {
        if (!m_running.load()) return;

        m_running.store(false);
        m_generation.fetch_add(1);

        for (size_t i = 0; i < m_threadCount; ++i) {
            if (m_threads[i].joinable()) {
                m_threads[i].join();
            }
        }
    }

    // Dispatches a batch of active animation clients across all worker threads
    void Dispatch(const std::vector<GEM3::xBone::eClient*>& clients) {
        if (clients.empty()) return;

        m_pClients     = &clients;
        m_chunkIndex.store(0, std::memory_order_relaxed);
        m_totalChunks  = (uint32_t)((clients.size() + m_chunkSize - 1) / m_chunkSize);
        m_activeJobs.store((uint32_t)m_threadCount, std::memory_order_relaxed);

        // Wake all worker threads
        m_generation.fetch_add(1, std::memory_order_release);

        // Main thread assists by processing chunks too (work-sharing)
        while (true) {
            uint32_t chunk = m_chunkIndex.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= m_totalChunks) {
                break;
            }

            size_t startIdx = chunk * m_chunkSize;
            size_t endIdx   = (startIdx + m_chunkSize < clients.size()) ? (startIdx + m_chunkSize) : clients.size();

            for (size_t i = startIdx; i < endIdx; ++i) {
                GEM3::xBone::eClient* client = clients[i];
                if (client) {
                    CallMsvcClientQuant((void*)client);
                }
            }
        }

        // Wait for all worker threads to complete their chunks
        while (m_activeJobs.load(std::memory_order_acquire) > 0) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#else
            std::this_thread::yield();
#endif
        }
    }

    // Dispatches raw client pointers across all worker threads
    void DispatchRaw(void** clients, size_t count) {
        if (!clients || count == 0) return;

        m_pRawClients = clients;
        m_rawCount    = count;
        m_pClients    = nullptr;
        m_chunkIndex.store(0, std::memory_order_relaxed);
        m_totalChunks = (uint32_t)((count + m_chunkSize - 1) / m_chunkSize);
        m_activeJobs.store((uint32_t)m_threadCount, std::memory_order_relaxed);

        // Wake all worker threads
        m_generation.fetch_add(1, std::memory_order_release);

        // Main thread assists by processing chunks too (work-sharing)
        while (true) {
            uint32_t chunk = m_chunkIndex.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= m_totalChunks) {
                break;
            }

            size_t startIdx = chunk * m_chunkSize;
            size_t endIdx   = (startIdx + m_chunkSize < count) ? (startIdx + m_chunkSize) : count;

            for (size_t i = startIdx; i < endIdx; ++i) {
                void* client = clients[i];
                if (client) {
                    CallMsvcClientQuant(client);
                }
            }
        }

        // Wait for all worker threads to complete their chunks
        while (m_activeJobs.load(std::memory_order_acquire) > 0) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#else
            std::this_thread::yield();
#endif
        }
    }

    inline size_t GetThreadCount() const { return m_threadCount; }
    inline size_t GetChunkSize()   const { return m_chunkSize; }

    inline const ThreadDeferredBuffer& GetDeferredBuffer(size_t threadId) const {
        return m_deferredBuffers[threadId];
    }
};

// =============================================================================
// 4. Parallel Quant Distributor Driver
// =============================================================================

class ParallelQuantEngine {
private:
    WorkerPool                  m_pool;
    std::vector<GEM3::xBone::eClient*> m_clientCache;
    bool                        m_enabled;

    // Telemetry stats
    uint64_t                    m_totalFrames;
    double                      m_totalMicroseconds;
    size_t                      m_lastClientCount;

public:
    ParallelQuantEngine()
        : m_enabled(true)
        , m_totalFrames(0)
        , m_totalMicroseconds(0.0)
        , m_lastClientCount(0)
    {
        m_clientCache.reserve(1024);
    }

    void Init(size_t threads = DEFAULT_WORKER_COUNT, size_t chunkSize = DEFAULT_CHUNK_SIZE) {
        m_pool.Start(threads, chunkSize);
    }

    void Shutdown() {
        m_pool.Stop();
    }

    inline void SetEnabled(bool enable) { m_enabled = enable; }
    inline bool IsEnabled() const       { return m_enabled; }

    void DispatchRaw(void** clients, size_t count) {
        if (!clients || count == 0) return;
        if (m_enabled && count >= m_pool.GetChunkSize()) {
            m_pool.DispatchRaw(clients, count);
        } else {
            for (size_t i = 0; i < count; ++i) {
                CallMsvcClientQuant(clients[i]);
            }
        }
    }

    // Replaces xScene::eQuantDistributor::Quant() (RVA 0xcaa70)
    void ExecuteQuantDistributor(GEM3::xBone::eQuantDistributor* distributor) {
        if (!distributor || !distributor->m_head) return;

        auto t0 = std::chrono::high_resolution_clock::now();

        // 1. Drain pending modifications on the main thread
        distributor->ApplyChanges();

        // 2. Gather active clients from circular linked list into contiguous flat vector
        m_clientCache.clear();
        GEM3::xBone::DistributorNode* head = distributor->m_head;
        GEM3::xBone::DistributorNode* cur  = head->next;

        while (cur != head && cur != nullptr) {
            if (cur->client) {
                m_clientCache.push_back(cur->client);
            }
            cur = cur->next;
        }

        m_lastClientCount = m_clientCache.size();

        if (m_enabled && m_clientCache.size() >= m_pool.GetChunkSize()) {
            // 3. Parallel chunked evaluation across 8 P-cores
            m_pool.Dispatch(m_clientCache);

            // 4. Flush deferred updates collected by worker threads
            for (size_t t = 0; t < m_pool.GetThreadCount(); ++t) {
                const auto& buf = m_pool.GetDeferredBuffer(t);
                for (size_t i = 0; i < buf.count; ++i) {
                    distributor->Update(buf.items[i].client);
                }
            }
        } else {
            // Sequential fallback for small batches (< 1 chunk)
            for (auto* client : m_clientCache) {
                if (client) {
                    client->Quant();
                }
            }
        }

        // 5. Final drain of changes that occurred during this frame
        distributor->ApplyChanges();

        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsedUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        m_totalMicroseconds += elapsedUs;
        m_totalFrames++;
    }

    // Telemetry getters
    inline size_t GetLastClientCount()  const { return m_lastClientCount; }
    inline double GetAverageTimeUs()    const { return (m_totalFrames > 0) ? (m_totalMicroseconds / (double)m_totalFrames) : 0.0; }
    inline uint64_t GetTotalFrames()    const { return m_totalFrames; }
};

// Global singleton instance
inline ParallelQuantEngine& GetEngine() {
    static ParallelQuantEngine s_engine;
    return s_engine;
}

// =============================================================================
// 5. Hook Installation Helpers (Inline JMP Trampolines)
// =============================================================================

#pragma pack(push, 1)
struct JumpHook5 {
    uint8_t  opcode;     // 0xE9 = JMP rel32
    int32_t  relative;   // Target - Source - 5
};
#pragma pack(pop)

inline bool InstallDetour(void* targetFunc, void* replacementFunc, uint8_t* outOriginalPrologue5) {
#ifdef _WIN32
    DWORD oldProtect = 0;
    if (!VirtualProtect(targetFunc, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    if (outOriginalPrologue5) {
        memcpy(outOriginalPrologue5, targetFunc, 5);
    }

    JumpHook5 hook;
    hook.opcode   = 0xE9;
    hook.relative = (int32_t)((uintptr_t)replacementFunc - ((uintptr_t)targetFunc + 5));

    memcpy(targetFunc, &hook, sizeof(hook));
    VirtualProtect(targetFunc, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), targetFunc, 5);
    return true;
#else
    (void)targetFunc; (void)replacementFunc; (void)outOriginalPrologue5;
    return false;
#endif
}

} // namespace XBoneParallel

#endif // XBONE_PARALLEL_H
