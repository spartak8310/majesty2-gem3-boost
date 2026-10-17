#ifndef M2_QUANT_DISTRIBUTOR_H
#define M2_QUANT_DISTRIBUTOR_H

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

// W3a kill switch (read once).
static int g_qdistEnabled = 0;   // W3b: default OFF; enabled only by M2_QDIST=1

// ==============================================================================
// m2_quant_distributor.h — Parallel Entity Simulation Distributor for GEM 3 (scene.dll)
// Hooks xScene::eQuantDistributor::Quant (scene.dll RVA 0xCAA70).
//
// Target Hardware: Apple Silicon M3 Pro (5 Performance Cores).
// Worker Setup: 4 worker threads + 1 calling main thread = 5 parallel P-cores.
// Work Distribution: Dynamic lock-free chunking (16 entities/chunk) via atomic add.
// ==============================================================================

#define M2_DIST_WORKERS 4
#define M2_CLIENT_MAX   8192

static void* s_distClients[M2_CLIENT_MAX];
static volatile LONG s_distNextIndex = 0;
static volatile LONG s_distTotalClients = 0;
static volatile LONG s_distWorkersRemaining = 0;

static HANDLE s_distWorkerWake[M2_DIST_WORKERS] = {NULL};
static HANDLE s_distDoneEvent = NULL;
static int s_distPoolReady = 0;

static uint32_t s_distFrames = 0;
static uint32_t s_distTotalEntities = 0;
static uint64_t s_distTotalTicks = 0;
static uint32_t s_distLastCount = 0;
static double   s_distLastMs = 0.0;

typedef void (__thiscall *PFN_M2ClientQuant)(void* client);
typedef void (__fastcall *PFN_M2ApplyChanges)(void* self, void* edx);
static PFN_M2ApplyChanges s_origApplyChanges = NULL;

static void DistRunWorkerChunks(void) {
    const int total = (int)s_distTotalClients;
    const int chunkSize = 16;
    for (;;) {
        const LONG idx = InterlockedExchangeAdd(&s_distNextIndex, chunkSize);
        if (idx >= total) return;
        int end = idx + chunkSize;
        if (end > total) end = total;
        for (int i = idx; i < end; ++i) {
            void* client = s_distClients[i];
            if (client) {
                void** vtbl = *(void***)client;
                if (vtbl) {
                    PFN_M2ClientQuant pfn = (PFN_M2ClientQuant)vtbl[3];
                    if (pfn) pfn(client);
                }
            }
        }
    }
}

static DWORD WINAPI DistWorkerThread(LPVOID param) {
    const int id = (int)(uintptr_t)param;
    for (;;) {
        if (WaitForSingleObject(s_distWorkerWake[id], INFINITE) != WAIT_OBJECT_0) return 0;
        DistRunWorkerChunks();
        if (InterlockedDecrement(&s_distWorkersRemaining) == 0) {
            SetEvent(s_distDoneEvent);
        }
    }
    return 0;
}

static void InitDistPool(void) {
    if (s_distPoolReady) return;
    s_distDoneEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < M2_DIST_WORKERS; ++i) {
        s_distWorkerWake[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        HANDLE th = CreateThread(NULL, 0, DistWorkerThread, (LPVOID)(uintptr_t)i, 0, NULL);
        if (th) {
            SetThreadPriority(th, THREAD_PRIORITY_HIGHEST);
            CloseHandle(th);
        }
    }
    s_distPoolReady = 1;
}

static void __fastcall Hook_QuantDistributor(void* self, void* edx) {
    if (!self) return;
    int64_t t0 = Qpc();

    // 1. Drain pending modifications before tick
    if (s_origApplyChanges) {
        s_origApplyChanges(self, NULL);
    }

    // 2. Gather active clients from circular linked list [self + 0x14]
    void* head = *(void**)((char*)self + 0x14);
    if (!head) return;

    int count = 0;
    void* cur = *(void**)head;
    while (cur && cur != head && count < M2_CLIENT_MAX) {
        void* client = *(void**)((char*)cur + 8);
        if (client) {
            s_distClients[count++] = client;
        }
        cur = *(void**)cur;
    }

    // 3. Evaluate entities
    if (count > 0) {
        if (count < 8) {
            // Sequential fallback for small batches (main menu, early preload)
            for (int i = 0; i < count; ++i) {
                void* client = s_distClients[i];
                if (client) {
                    void** vtbl = *(void***)client;
                    if (vtbl) {
                        PFN_M2ClientQuant pfn = (PFN_M2ClientQuant)vtbl[3];
                        if (pfn) pfn(client);
                    }
                }
            }
        } else {
            // Parallel execution across 4 workers + 1 caller (5 threads = 5 P-cores)
            InitDistPool();
            InterlockedExchange(&s_distTotalClients, count);
            InterlockedExchange(&s_distNextIndex, 0);
            InterlockedExchange(&s_distWorkersRemaining, M2_DIST_WORKERS);
            ResetEvent(s_distDoneEvent);

            for (int i = 0; i < M2_DIST_WORKERS; ++i) {
                SetEvent(s_distWorkerWake[i]);
            }

            // Caller thread participates in chunk execution
            DistRunWorkerChunks();

            // Wait for all workers to finish
            if (InterlockedCompareExchange(&s_distWorkersRemaining, 0, 0) != 0) {
                WaitForSingleObject(s_distDoneEvent, INFINITE);
            }
        }
    }

    // 4. Drain pending modifications after tick
    if (s_origApplyChanges) {
        s_origApplyChanges(self, NULL);
    }

    int64_t dt = Qpc() - t0;
    s_distTotalTicks += dt;
    s_distTotalEntities += count;
    s_distFrames++;
    s_distLastCount = count;
    if (g_qpcFreq.QuadPart > 0) {
        s_distLastMs = (double)dt * 1000.0 / (double)g_qpcFreq.QuadPart;
    }
}

static bool InstallQuantDistributorHook(void) {
    static bool cfgDone = false;
    if (!cfgDone) {
        cfgDone = true;
        char ev[16] = {0};
        if (GetEnvironmentVariableA("M2_QDIST", ev, sizeof(ev)) > 0 && ev[0] == '1') {
            g_qdistEnabled = 1;
            LogLine("qdist: enabled by M2_QDIST=1 (parallel distributor active)");
        } else {
            g_qdistEnabled = 0;
            LogLine("qdist: disabled by default (set M2_QDIST=1 to enable; M2_QDIST=0 also disables)");
        }
    }
    if (!g_qdistEnabled) {
        static bool done = false;
        if (!done) { done = true; }
        return false;
    }
    static bool installed = false;
    if (installed) return true;

    HMODULE hScene = GetModuleHandleA("scene.dll");
    if (!hScene) return false;

    s_origApplyChanges = (PFN_M2ApplyChanges)((uintptr_t)hScene + 0xCA9B0);

    void* target = (void*)((uintptr_t)hScene + 0xCAA70);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    // 5-byte JMP rel32: 0xE9 <target - source - 5>
    uint8_t* p = (uint8_t*)target;
    p[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)Hook_QuantDistributor - ((uintptr_t)target + 5));
    *(int32_t*)(p + 1) = rel;

    VirtualProtect(target, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    installed = true;
    LogLine("quant: hooked xScene::eQuantDistributor::Quant @ scene.dll+0xCAA70 (5 P-cores parallel distributor active!)");
    return true;
}

static void M2_DistTelemetry(double tSec) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char line[512];
    uint32_t frames = s_distFrames;
    uint32_t entities = s_distTotalEntities;
    uint64_t ticks = s_distTotalTicks;
    s_distFrames = 0; s_distTotalEntities = 0; s_distTotalTicks = 0;
    double ms = (g_qpcFreq.QuadPart > 0 && frames > 0) ? ((double)ticks * 1000.0 / (double)g_qpcFreq.QuadPart / (double)frames) : s_distLastMs;
    double epf = frames > 0 ? (double)entities / (double)frames : (double)s_distLastCount;
    int len = snprintf(line, sizeof(line),
                       "# distributor t=%.2f frames=%u entities_pf=%.1f parallel_ms=%.2f workers=%d p_cores=5\n",
                       tSec, frames, epf, ms, M2_DIST_WORKERS);
    if (len > 0) {
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    }
}

#endif // M2_QUANT_DISTRIBUTOR_H
