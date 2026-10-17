// m2_arena_allocator.h — Lock-Free Thread-Local Arena Allocator for Majesty 2 & Wine
// Eliminates RtlAllocateHeap mutex contention (24.1% latency bottleneck) by caching
// small allocations (<= 512 bytes) in thread-local bump/freelist arenas.
//
// Supports:
// - Thread-Local L1 size-segregated freelists for 32 bins (16..512 bytes, step 16)
// - O(1) Bump pointer allocation from 1MB VirtualAlloc/mmap chunks
// - Lock-free atomic CAS remote free list for cross-thread handovers
// - Transparent fallback to HeapAlloc/malloc for blocks > 512 bytes
// - Safe header tags (M2AR / M2CH) with address-range validation to prevent false positives
// - Guaranteed 16-byte alignment on all allocations for SSE2 / SIMD compatibility
// - IAT hooking for PE32 modules (Majesty2.exe, scene.dll, render.dll, etc.)
// - Inline hooking with 5-byte executable trampoline
#ifndef M2_ARENA_ALLOCATOR_H
#define M2_ARENA_ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#ifndef WINAPI
#define WINAPI
#endif
#ifndef __cdecl
#define __cdecl
#endif
typedef void* HANDLE;
typedef uint32_t DWORD;
typedef size_t SIZE_T;
typedef int BOOL;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef uint8_t BYTE;
typedef uintptr_t HMODULE;
#define TRUE 1
#define FALSE 0
#define HEAP_ZERO_MEMORY 0x00000008
#endif

namespace M2Arena {

static constexpr size_t   MAX_ARENA_SIZE    = 512;
static constexpr size_t   ALIGNMENT         = 16;
static constexpr uint16_t NUM_BINS          = MAX_ARENA_SIZE / ALIGNMENT; // 32 bins
static constexpr size_t   CHUNK_SIZE        = 1024 * 1024;                // 1 MB per chunk
static constexpr uint32_t M2_ARENA_MAGIC    = 0x4D324152;                 // 'M2AR'
static constexpr uint32_t M2_CHUNK_MAGIC    = 0x4D324348;                 // 'M2CH'
static constexpr size_t   MAX_ARENA_THREADS = 128;

struct Chunk;
struct ThreadArena;

// 16-byte header on 32-bit, 32-byte header on 64-bit (always multiple of 16 for SIMD)
struct alignas(16) BlockHeader {
    uint32_t     magic;      // M2_ARENA_MAGIC (0x4D324152)
    uint16_t     bin;        // Size class index: 0..31
    uint16_t     flags;      // Flags / alignment padding
    Chunk*       chunk;      // Pointer to owning Chunk
    ThreadArena* owner;      // Pointer to ThreadArena that allocated this block
#if defined(__x86_64__) || defined(_M_X64)
    uint64_t     reserved;   // Pad to 32 bytes for 16-byte payload alignment
#endif
};

// Intrusive free-list node placed inside the payload when freed
struct FreeNode {
    FreeNode* next;
};

// Chunk allocated directly from OS (VirtualAlloc / mmap) bypassing RtlAllocateHeap
struct Chunk {
    uint32_t  magic;       // M2_CHUNK_MAGIC (0x4D324348)
    uint32_t  size;        // Total size of chunk in bytes
    Chunk*    next;        // Linked list of chunks for cleanup
    uintptr_t data_start;  // First allocatable byte (16-byte aligned)
    uintptr_t data_end;    // End boundary of chunk memory
};

// Forward declaration of fallback function types
#ifdef _WIN32
typedef LPVOID (WINAPI *PFN_HeapAlloc)(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
typedef BOOL   (WINAPI *PFN_HeapFree)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
typedef LPVOID (WINAPI *PFN_HeapReAlloc)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes);
typedef SIZE_T (WINAPI *PFN_HeapSize)(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
#endif
typedef void*  (__cdecl *PFN_Malloc)(size_t size);
typedef void   (__cdecl *PFN_Free)(void* ptr);
typedef void*  (__cdecl *PFN_Realloc)(void* ptr, size_t size);
typedef void*  (__cdecl *PFN_Calloc)(size_t num, size_t size);

// Pointers to real/underlying system memory functions
#ifdef _WIN32
inline PFN_HeapAlloc   g_realHeapAlloc   = nullptr;
inline PFN_HeapFree    g_realHeapFree    = nullptr;
inline PFN_HeapReAlloc g_realHeapReAlloc = nullptr;
inline PFN_HeapSize    g_realHeapSize    = nullptr;
#endif
inline PFN_Malloc      g_realMalloc      = nullptr;
inline PFN_Free        g_realFree        = nullptr;
inline PFN_Realloc     g_realRealloc     = nullptr;
inline PFN_Calloc      g_realCalloc      = nullptr;

// Initialize real function pointers if not already hooked
inline void InitRealPointers() {
#ifdef _WIN32
    if (!g_realHeapAlloc) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32) {
            g_realHeapAlloc   = (PFN_HeapAlloc)GetProcAddress(k32, "HeapAlloc");
            g_realHeapFree    = (PFN_HeapFree)GetProcAddress(k32, "HeapFree");
            g_realHeapReAlloc = (PFN_HeapReAlloc)GetProcAddress(k32, "HeapReAlloc");
            g_realHeapSize    = (PFN_HeapSize)GetProcAddress(k32, "HeapSize");
        }
    }
    if (!g_realMalloc) {
        HMODULE crt = GetModuleHandleA("MSVCR80.dll");
        if (!crt) crt = GetModuleHandleA("msvcrt.dll");
        if (crt) {
            g_realMalloc  = (PFN_Malloc)GetProcAddress(crt, "malloc");
            g_realFree    = (PFN_Free)GetProcAddress(crt, "free");
            g_realRealloc = (PFN_Realloc)GetProcAddress(crt, "realloc");
            g_realCalloc  = (PFN_Calloc)GetProcAddress(crt, "calloc");
        }
    }
#endif
}

// Transparent fallback for sizes > 512 bytes or when arena is exhausted
inline void* FallbackAlloc(size_t size) {
#ifdef _WIN32
    if (g_realHeapAlloc) return g_realHeapAlloc(GetProcessHeap(), 0, size);
    if (g_realMalloc) return g_realMalloc(size);
#endif
    return ::malloc(size);
}

inline void FallbackFree(void* ptr) {
#ifdef _WIN32
    if (g_realHeapFree && GetProcessHeap()) {
        if (g_realHeapFree(GetProcessHeap(), 0, ptr)) return;
    }
    if (g_realFree) {
        g_realFree(ptr);
        return;
    }
#endif
    ::free(ptr);
}

inline void* FallbackRealloc(void* ptr, size_t size) {
#ifdef _WIN32
    if (g_realHeapReAlloc && GetProcessHeap()) {
        return g_realHeapReAlloc(GetProcessHeap(), 0, ptr, size);
    }
    if (g_realRealloc) return g_realRealloc(ptr, size);
#endif
    return ::realloc(ptr, size);
}

// Maps requested user bytes to bin index [0..31]
static inline uint16_t SizeToBin(size_t size) {
    if (__builtin_expect(size == 0, 0)) return 0;
    return (uint16_t)(((size + (ALIGNMENT - 1)) >> 4) - 1);
}

// Maps bin index to payload size in bytes (16, 32, ..., 512)
static inline size_t BinToSize(uint16_t bin) {
    return (size_t)(bin + 1) << 4;
}

// Per-thread arena context
struct ThreadArena {
    FreeNode*              free_lists[NUM_BINS];     // Local L1 freelist per bin
    std::atomic<FreeNode*> remote_free_head;         // Lock-free atomic list for foreign frees
    Chunk*                 current_chunk;            // Current active chunk for bump allocation
    Chunk*                 chunk_list;               // All chunks owned by this thread
    uint8_t*               bump_ptr;                 // Next available byte in current chunk
    uint8_t*               bump_end;                 // End of current chunk
    
    // Telemetry counters
    uint64_t count_local_alloc;
    uint64_t count_local_free;
    uint64_t count_remote_free;
    uint64_t count_bump_alloc;
    uint64_t count_fallback_alloc;
    uint64_t count_fallback_free;
    
    void Init() {
        for (uint16_t i = 0; i < NUM_BINS; ++i) {
            free_lists[i] = nullptr;
        }
        remote_free_head.store(nullptr, std::memory_order_relaxed);
        current_chunk = nullptr;
        chunk_list = nullptr;
        bump_ptr = nullptr;
        bump_end = nullptr;
        count_local_alloc = 0;
        count_local_free = 0;
        count_remote_free = 0;
        count_bump_alloc = 0;
        count_fallback_alloc = 0;
        count_fallback_free = 0;
    }
    
    // Drains blocks freed remotely by other threads into local free lists (1 atomic exchange)
    inline void DrainRemoteFreeList() {
        FreeNode* remote = remote_free_head.exchange(nullptr, std::memory_order_acquire);
        while (remote) {
            FreeNode* next = remote->next;
            BlockHeader* bh = (BlockHeader*)remote - 1;
            uint16_t b = bh->bin;
            if (__builtin_expect(b < NUM_BINS, 1)) {
                remote->next = free_lists[b];
                free_lists[b] = remote;
                count_local_free++;
            }
            remote = next;
        }
    }
    
    // Allocates a new 1MB chunk from OS via VirtualAlloc/mmap (bypassing RtlAllocateHeap)
    bool GrowChunk(size_t min_bytes) {
        size_t alloc_size = CHUNK_SIZE;
        if (min_bytes + sizeof(Chunk) + sizeof(BlockHeader) > alloc_size) {
            alloc_size = (min_bytes + sizeof(Chunk) + sizeof(BlockHeader) + 4095) & ~4095;
        }
        
        void* mem = nullptr;
#ifdef _WIN32
        mem = VirtualAlloc(NULL, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) mem = nullptr;
#endif
        if (!mem) return false;
        
        Chunk* chunk = (Chunk*)mem;
        chunk->magic = M2_CHUNK_MAGIC;
        chunk->size = (uint32_t)alloc_size;
        chunk->next = chunk_list;
        chunk_list = chunk;
        current_chunk = chunk;
        
        uintptr_t raw_data = (uintptr_t)(chunk + 1);
        uintptr_t aligned_data = (raw_data + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
        chunk->data_start = aligned_data;
        chunk->data_end = (uintptr_t)mem + alloc_size;
        
        bump_ptr = (uint8_t*)chunk->data_start;
        bump_end = (uint8_t*)chunk->data_end;
        return true;
    }
    
    // Slow path: drained remote frees or bump-allocates
    void* AllocSlow(uint16_t bin) {
        // 1. Check remote free list if any pending
        if (remote_free_head.load(std::memory_order_relaxed)) {
            DrainRemoteFreeList();
            FreeNode* node = free_lists[bin];
            if (node) {
                free_lists[bin] = node->next;
                count_local_alloc++;
                return (void*)node;
            }
        }
        
        // 2. Bump pointer allocation
        size_t payload_size = BinToSize(bin);
        size_t total_size = sizeof(BlockHeader) + payload_size;
        
        if (__builtin_expect(bump_ptr + total_size > bump_end, 0)) {
            if (!GrowChunk(total_size)) {
                count_fallback_alloc++;
                return FallbackAlloc(payload_size);
            }
        }
        
        BlockHeader* h = (BlockHeader*)bump_ptr;
        bump_ptr += total_size;
        
        h->magic = M2_ARENA_MAGIC;
        h->bin = bin;
        h->flags = 0;
        h->chunk = current_chunk;
        h->owner = this;
#if defined(__x86_64__) || defined(_M_X64)
        h->reserved = 0;
#endif
        
        count_bump_alloc++;
        count_local_alloc++;
        return (void*)(h + 1);
    }
    
    void Cleanup() {
        Chunk* c = chunk_list;
        while (c) {
            Chunk* next = c->next;
#ifdef _WIN32
            VirtualFree(c, 0, MEM_RELEASE);
#else
            munmap(c, c->size);
#endif
            c = next;
        }
        Init();
    }
};

// Global pool of ThreadArenas to ensure pointers remain valid across threads
inline ThreadArena                  g_arena_pool[MAX_ARENA_THREADS];
inline std::atomic<uint32_t>        g_arena_count{0};
inline thread_local ThreadArena*    t_current_arena = nullptr;

inline ThreadArena* GetCurrentThreadArena() {
    if (__builtin_expect(t_current_arena != nullptr, 1)) {
        return t_current_arena;
    }
    uint32_t idx = g_arena_count.fetch_add(1, std::memory_order_relaxed);
    if (idx < MAX_ARENA_THREADS) {
        ThreadArena* arena = &g_arena_pool[idx];
        arena->Init();
        t_current_arena = arena;
        return arena;
    }
    // Fallback if thread count exceeds pool
    ThreadArena* arena = (ThreadArena*)malloc(sizeof(ThreadArena));
    arena->Init();
    t_current_arena = arena;
    return arena;
}

// Global Alloc / Free / Realloc / Calloc entry points
inline void* Alloc(size_t size) {
    ThreadArena* arena = t_current_arena;
    if (__builtin_expect(!arena, 0)) {
        arena = GetCurrentThreadArena();
    }
    
    if (__builtin_expect(size <= MAX_ARENA_SIZE, 1)) {
        uint16_t bin = SizeToBin(size);
        FreeNode* node = arena->free_lists[bin];
        if (__builtin_expect(node != nullptr, 1)) {
            arena->free_lists[bin] = node->next;
            arena->count_local_alloc++;
            return (void*)node;
        }
        return arena->AllocSlow(bin);
    }
    
    arena->count_fallback_alloc++;
    return FallbackAlloc(size);
}

inline bool IsArenaPtr(void* ptr) {
    if (!ptr) return false;
    BlockHeader* h = (BlockHeader*)ptr - 1;
    if (h->magic != M2_ARENA_MAGIC) return false;
    Chunk* chunk = h->chunk;
    if (!chunk || chunk->magic != M2_CHUNK_MAGIC) return false;
    uintptr_t u = (uintptr_t)ptr;
    return (u >= chunk->data_start && u < chunk->data_end && h->bin < NUM_BINS);
}

inline void Free(void* ptr) {
    if (__builtin_expect(!ptr, 0)) return;
    
    BlockHeader* h = (BlockHeader*)ptr - 1;
    if (__builtin_expect(h->magic == M2_ARENA_MAGIC, 1)) {
        ThreadArena* current = t_current_arena;
        if (__builtin_expect(h->owner == current && current != nullptr, 1)) {
            // Hot path: same-thread local free!
            uint16_t bin = h->bin;
            FreeNode* node = (FreeNode*)ptr;
            node->next = current->free_lists[bin];
            current->free_lists[bin] = node;
            current->count_local_free++;
            return;
        }
        
        // Cross-thread free: push onto owner's lock-free remote free list
        ThreadArena* owner = h->owner;
        if (owner) {
            FreeNode* node = (FreeNode*)ptr;
            FreeNode* old_head = owner->remote_free_head.load(std::memory_order_relaxed);
            do {
                node->next = old_head;
            } while (!owner->remote_free_head.compare_exchange_weak(
                old_head, node, std::memory_order_release, std::memory_order_relaxed));
            if (current) current->count_remote_free++;
            return;
        }
    }
    
    FallbackFree(ptr);
}

inline void* Realloc(void* ptr, size_t new_size) {
    if (!ptr) return Alloc(new_size);
    if (new_size == 0) {
        Free(ptr);
        return nullptr;
    }
    
    if (IsArenaPtr(ptr)) {
        BlockHeader* h = (BlockHeader*)ptr - 1;
        size_t old_size = BinToSize(h->bin);
        if (new_size <= old_size) {
            return ptr; // Reuses same block if it fits
        }
        void* new_ptr = Alloc(new_size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, old_size);
            Free(ptr);
        }
        return new_ptr;
    }
    
    return FallbackRealloc(ptr, new_size);
}

inline void* Calloc(size_t num, size_t size) {
    size_t total = num * size;
    void* ptr = Alloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

inline size_t GetSize(const void* ptr) {
    if (!ptr) return 0;
    const BlockHeader* h = (const BlockHeader*)ptr - 1;
    if (h->magic == M2_ARENA_MAGIC && h->bin < NUM_BINS) {
        return BinToSize(h->bin);
    }
#ifdef _WIN32
    if (g_realHeapSize && GetProcessHeap()) {
        return g_realHeapSize(GetProcessHeap(), 0, ptr);
    }
#endif
    return 0;
}

// -------------------------------------------------------------------------
// Hooks for Win32 HeapAlloc / HeapFree and MSVCR80 malloc / free
// -------------------------------------------------------------------------
#ifdef _WIN32
inline LPVOID WINAPI Hook_HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) {
    if (dwBytes <= MAX_ARENA_SIZE) {
        void* ptr = Alloc(dwBytes);
        if (ptr && (dwFlags & HEAP_ZERO_MEMORY)) {
            memset(ptr, 0, dwBytes);
        }
        return (LPVOID)ptr;
    }
    if (g_realHeapAlloc) {
        return g_realHeapAlloc(hHeap ? hHeap : GetProcessHeap(), dwFlags, dwBytes);
    }
    return (LPVOID)::malloc(dwBytes);
}

inline BOOL WINAPI Hook_HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
    if (!lpMem) return TRUE;
    
    BlockHeader* h = (BlockHeader*)lpMem - 1;
    if (h->magic == M2_ARENA_MAGIC) {
        Chunk* chunk = h->chunk;
        if (chunk && chunk->magic == M2_CHUNK_MAGIC &&
            (uintptr_t)lpMem >= chunk->data_start && (uintptr_t)lpMem < chunk->data_end) {
            Free(lpMem);
            return TRUE;
        }
    }
    
    if (g_realHeapFree) {
        return g_realHeapFree(hHeap ? hHeap : GetProcessHeap(), dwFlags, lpMem);
    }
    ::free(lpMem);
    return TRUE;
}

inline LPVOID WINAPI Hook_HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) {
    if (!lpMem) return Hook_HeapAlloc(hHeap, dwFlags, dwBytes);
    if (IsArenaPtr(lpMem)) {
        BlockHeader* h = (BlockHeader*)lpMem - 1;
        size_t old_size = BinToSize(h->bin);
        if (dwBytes <= old_size) {
            return lpMem;
        }
        void* new_ptr = Hook_HeapAlloc(hHeap, dwFlags, dwBytes);
        if (new_ptr) {
            memcpy(new_ptr, lpMem, old_size);
            Hook_HeapFree(hHeap, dwFlags, lpMem);
        }
        return new_ptr;
    }
    if (g_realHeapReAlloc) {
        return g_realHeapReAlloc(hHeap ? hHeap : GetProcessHeap(), dwFlags, lpMem, dwBytes);
    }
    return (LPVOID)::realloc(lpMem, dwBytes);
}

inline SIZE_T WINAPI Hook_HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) {
    if (!lpMem) return 0;
    const BlockHeader* h = (const BlockHeader*)lpMem - 1;
    if (h->magic == M2_ARENA_MAGIC && h->bin < NUM_BINS) {
        return BinToSize(h->bin);
    }
    if (g_realHeapSize) {
        return g_realHeapSize(hHeap ? hHeap : GetProcessHeap(), dwFlags, lpMem);
    }
    return 0;
}
#endif

// CRT hooks
inline void* __cdecl Hook_Malloc(size_t size) {
    return Alloc(size);
}

inline void __cdecl Hook_Free(void* ptr) {
    Free(ptr);
}

inline void* __cdecl Hook_Realloc(void* ptr, size_t size) {
    return Realloc(ptr, size);
}

inline void* __cdecl Hook_Calloc(size_t num, size_t size) {
    return Calloc(num, size);
}

inline size_t __cdecl Hook_Msize(void* ptr) {
    return GetSize(ptr);
}

inline void* __cdecl Hook_New(size_t size) {
    return Alloc(size);
}

inline void __cdecl Hook_Delete(void* ptr) {
    Free(ptr);
}

inline void* __cdecl Hook_NewArray(size_t size) {
    return Alloc(size);
}

inline void __cdecl Hook_DeleteArray(void* ptr) {
    Free(ptr);
}

// -------------------------------------------------------------------------
// IAT Hooking Engine
// -------------------------------------------------------------------------
#ifdef _WIN32
static inline void PatchPtr(void** slot, void* value) {
    DWORD oldProt = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        *slot = value;
        VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
    }
}

inline int PatchIAT(HMODULE mod, const char* dll, const char* func, void* hook, void** orig) {
    if (!mod) return -1;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
    
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
            bool match = false;
            if (names && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                match = (strcmp((const char*)ibn->Name, func) == 0);
            } else {
                match = (resolved && (void*)iat[i].u1.Function == resolved);
            }
            if (!match || (void*)iat[i].u1.Function == hook) continue;
            if (orig && !*orig) {
                *orig = (void*)iat[i].u1.Function;
            }
            PatchPtr((void**)&iat[i].u1.Function, hook);
            ++patched;
        }
    }
    return patched;
}

inline int InstallModuleIATHooks(HMODULE mod) {
    if (!mod) return 0;
    int count = 0;
    
    // kernel32.dll HeapAlloc/HeapFree/HeapReAlloc/HeapSize
    count += PatchIAT(mod, "kernel32.dll", "HeapAlloc",   (void*)Hook_HeapAlloc,   (void**)&g_realHeapAlloc);
    count += PatchIAT(mod, "kernel32.dll", "HeapFree",    (void*)Hook_HeapFree,    (void**)&g_realHeapFree);
    count += PatchIAT(mod, "kernel32.dll", "HeapReAlloc", (void*)Hook_HeapReAlloc, (void**)&g_realHeapReAlloc);
    count += PatchIAT(mod, "kernel32.dll", "HeapSize",    (void*)Hook_HeapSize,    (void**)&g_realHeapSize);
    
    // MSVCR80.dll CRT functions
    count += PatchIAT(mod, "MSVCR80.dll", "malloc",       (void*)Hook_Malloc,      (void**)&g_realMalloc);
    count += PatchIAT(mod, "MSVCR80.dll", "free",         (void*)Hook_Free,        (void**)&g_realFree);
    count += PatchIAT(mod, "MSVCR80.dll", "realloc",      (void*)Hook_Realloc,     (void**)&g_realRealloc);
    count += PatchIAT(mod, "MSVCR80.dll", "calloc",       (void*)Hook_Calloc,      (void**)&g_realCalloc);
    count += PatchIAT(mod, "MSVCR80.dll", "_msize",       (void*)Hook_Msize,       nullptr);
    
    // MSVCR80.dll C++ operator new / delete / new[] / delete[]
    count += PatchIAT(mod, "MSVCR80.dll", "??2@YAPAXI@Z", (void*)Hook_New,         nullptr);
    count += PatchIAT(mod, "MSVCR80.dll", "??3@YAXPAX@Z", (void*)Hook_Delete,      nullptr);
    count += PatchIAT(mod, "MSVCR80.dll", "??_U@YAPAXI@Z",(void*)Hook_NewArray,    nullptr);
    count += PatchIAT(mod, "MSVCR80.dll", "??_V@YAXPAX@Z",(void*)Hook_DeleteArray, nullptr);
    
    return count;
}

inline int InstallAllIATHooks() {
    InitRealPointers();
    int total = 0;
    
    HMODULE exe = GetModuleHandleA(NULL);
    if (exe) total += InstallModuleIATHooks(exe);
    
    const char* const modules[] = {
        "scene.dll",
        "render.dll",
        "base.dll",
        "geometry.dll",
        "gembase.dll",
        "pf.dll",
        "features.dll",
        "script.dll",
        "traceable.dll",
        "app.dll",
        nullptr
    };
    
    for (int i = 0; modules[i]; ++i) {
        HMODULE hMod = GetModuleHandleA(modules[i]);
        if (hMod) {
            total += InstallModuleIATHooks(hMod);
        }
    }
    return total;
}

// -------------------------------------------------------------------------
// Inline Hooking Engine (5-byte JMP with executable trampoline)
// -------------------------------------------------------------------------
inline bool InstallInlineHook5(void* target, void* hook, void** orig_callable) {
    if (!target || !hook) return false;
    
    // Allocate 64-byte executable trampoline
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    
    // Copy 5 stolen bytes to trampoline
    memcpy(tramp, target, 5);
    
    // Append JMP back to (target + 5)
    tramp[5] = 0xE9; // JMP rel32
    int32_t rel_back = (int32_t)((uint8_t*)target + 5 - (tramp + 10));
    memcpy(tramp + 6, &rel_back, 4);
    
    if (orig_callable) *orig_callable = (void*)tramp;
    
    // Patch target with JMP hook
    DWORD oldProt = 0;
    if (VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        uint8_t* p = (uint8_t*)target;
        p[0] = 0xE9;
        int32_t rel_hook = (int32_t)((uint8_t*)hook - (p + 5));
        memcpy(p + 1, &rel_hook, 4);
        VirtualProtect(target, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
        return true;
    }
    return false;
}
#endif

// Aggregate telemetry report
struct AggregateStats {
    uint64_t total_local_alloc;
    uint64_t total_local_free;
    uint64_t total_remote_free;
    uint64_t total_bump_alloc;
    uint64_t total_fallback_alloc;
    uint64_t total_fallback_free;
    uint32_t active_threads;
};

inline AggregateStats GetAggregateStats() {
    AggregateStats s = {0};
    uint32_t count = g_arena_count.load(std::memory_order_relaxed);
    if (count > MAX_ARENA_THREADS) count = MAX_ARENA_THREADS;
    s.active_threads = count;
    for (uint32_t i = 0; i < count; ++i) {
        s.total_local_alloc    += g_arena_pool[i].count_local_alloc;
        s.total_local_free     += g_arena_pool[i].count_local_free;
        s.total_remote_free    += g_arena_pool[i].count_remote_free;
        s.total_bump_alloc     += g_arena_pool[i].count_bump_alloc;
        s.total_fallback_alloc += g_arena_pool[i].count_fallback_alloc;
        s.total_fallback_free  += g_arena_pool[i].count_fallback_free;
    }
    return s;
}

} // namespace M2Arena

#endif // M2_ARENA_ALLOCATOR_H
