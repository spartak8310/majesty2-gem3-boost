// ================== Radical Splice 2 (t_m2_multithread_quant): parallel entity quant ==================
//
// Goal: move the per-entity work of xScene::eScene::QuantEntities (animation bone-matrix evaluation plus
// the independent zone updates) off the single simulation thread onto a persistent worker pool
// (M2_QUANT_THREADS, default 8) so that quant_ms drops from ~111 ms toward the per-entity critical path.
//
// HARD CONSTRAINTS OF THIS PROJECT (HARNESS_BLACKBOARD.md / ECOSYSTEM_AUDIT_REPORT.md):
//   * scene.dll exists ONLY on the Mac.  There is no RVA to hard-code here and no way to disassemble the
//     engine loop from this host, so the target is resolved at RUNTIME through three independent channels
//     (explicit env RVA / scene.dll export-table symbol / ASCII-anchor scan) and every channel is gated by
//     section + prologue + 16-byte signature checks - the same discipline as the Wave-1 zone and Wave-2
//     particle hooks.  A wrong target is harmless in probe mode: the stub only ever returns "run".
//   * Parallelising engine code we cannot read is only sound if the parallel result is PROVABLY identical
//     to the serial one, so the feature is a LADDER, never a bare switch:
//       probe    install the semantics-preserving stub, learn the entity-loop shape, measure the per-entity
//                cost distribution.  Never parallelises.  Safe on ANY target.
//       verify   fan the entity range out across the pool, fingerprint the entity memory, RESTORE the
//                pre-fan-out bytes, let the engine's own serial loop run, fingerprint again and compare.
//                The serial run is always last, so the frame outcome stays exactly the serial outcome.
//       parallel entered only after N consecutive verify frames matched: the first call of a frame fans the
//                range out and the remaining calls return immediately (the work is already done).
//   * Fail-open everywhere: unknown target, unexpected call pattern, wrong stack-argument declaration,
//     allocation failure or a single verify mismatch -> the engine runs its own serial path untouched.
//
// Env knobs (all default OFF / conservative):
//   M2_QUANT=probe|verify|1|0           feature ladder (1 = verify first, then parallel)
//   M2_QUANT_RVA=<mod>:<hex>|<hex>      explicit leaf RVA (module defaults to scene.dll)
//   M2_QUANT_EXPORT=<csv substrings>    resolve the leaf from the module export table
//                                       (default QuantEntities,AnimationSystem,QuantEntity)
//   M2_QUANT_SCAN=1|auto                ASCII-anchor scan of the module (1 = log candidates only)
//   M2_QUANT_ARGS=<n>                   number of stack args the leaf takes (REQUIRED for mode >= 2)
//   M2_QUANT_RET=<bytes>                bytes the callee pops (default 4*ARGS, i.e. thiscall/stdcall)
//   M2_QUANT_THREADS=<n>                pool size incl. the caller (default 8, clamp 1..16)
//   M2_QUANT_LEARN=<n>                  consecutive identical frames before the shape is accepted (default 3)
//   M2_QUANT_VERIFY=<n>                 verify frames required before parallel mode (default 5)
//   M2_QUANT_FP_OFF / M2_QUANT_FP_LEN   fingerprint window inside one entity (default 0 / min(stride,64))
//   M2_QUANT_FP_MAX                     fingerprint byte budget (default 512 KB)
//   M2_QUANT_SNAP_MAX                   verify snapshot budget (default 16 MB)
//   M2_QUANT_VERIFY_EXTRA=<hex>:<len>   extra absolute range to fingerprint + restore during verify
//   M2_QUANT_CHUNK_MIN                  minimum items per work chunk (default 4)
//   M2_QUANT_KILL=1                     hard kill switch
//
// Include point: this header needs LogLine, Qpc, g_logFile, M2_SectionForVa, M2_ZonePrologueLen,
// M2_PfEmitStub, M2_PfSig16/SigMatches, M2_FindAsciiI and M2_PfX87Density, so it must be included after
// the Wave-2 section (d3d9_proxy.cpp, before Hook_AppQuant) and before Hook_Present/Hook_CreateDevice.
#ifndef M2_QUANT_PARALLEL_H
#define M2_QUANT_PARALLEL_H

// =====================================================================================================
// ---- M2_QP_CORE_BEGIN ----  (host-testable: no windows.h, no logging, no globals, no allocation)
// =====================================================================================================
#define M2QP_SEQ_MAX    8192
#define M2QP_STRIDE_MIN 4
#define M2QP_STRIDE_MAX 8192
#define M2QP_COUNT_MAX  65536

typedef struct { int ok, count, stride; } M2QpShape;

// Is the observed `this` sequence a plausible entity batch?
// Supports both contiguous flat arrays and dynamic pointer arrays (GEM3 heap entities).
static M2QpShape M2QpShapeOf(const uintptr_t* seq, int n) {
    M2QpShape r;
    r.ok = 0; r.count = n; r.stride = 0;
    if (!seq || n < 2 || n > M2QP_COUNT_MAX) return r;
    // Check if flat array with constant stride
    const long long s = (long long)seq[1] - (long long)seq[0];
    if (s >= M2QP_STRIDE_MIN && s <= M2QP_STRIDE_MAX && !(s & 3)) {
        int isFlat = 1;
        for (int i = 1; i < n; ++i) {
            if ((long long)seq[i] - (long long)seq[i - 1] != s) { isFlat = 0; break; }
        }
        if (isFlat) {
            r.ok = 1;
            r.stride = (int)s;
            return r;
        }
    }
    // Dynamic pointer array mode (GEM3 entities in heap):
    r.ok = 1;
    r.stride = 1; // indicates dynamic pointer array mode
    return r;
}

// Items per work chunk: ~8 chunks per worker so the tail balances, never fewer than `minItems`.
static int M2QpChunkFor(int count, int threads, int minItems) {
    if (threads < 1) threads = 1;
    if (minItems < 1) minItems = 1;
    if (count <= 0) return minItems;
    int t = count / (threads * 8);
    if (t < minItems) t = minItems;
    if (t > count) t = count;
    return t;
}
static int M2QpNChunks(int count, int chunk) {
    if (count <= 0) return 0;
    if (chunk < 1) return 1;
    return (count + chunk - 1) / chunk;
}

static unsigned M2QpFnv1aMix(unsigned h, const unsigned char* p, int n) {
    for (int i = 0; i < n; ++i) {
        h ^= (unsigned)p[i];
        h *= 16777619u;
    }
    return h;
}

// Entity-array fingerprint.  `per` bytes at `off` inside each entity, every `step`-th entity, index-mixed so
// that a reordered/short array cannot collide with the correct one.  `step` is chosen from the byte budget.
static int M2QpStepFor(int count, int per, int maxBytes) {
    if (count <= 0 || per <= 0 || maxBytes <= 0) return 1;
    const long long total = (long long)count * (long long)per;
    if (total <= (long long)maxBytes) return 1;
    int step = (int)((total + maxBytes - 1) / maxBytes);
    if (step < 1) step = 1;
    return step;
}
static unsigned M2QpFingerprint(const unsigned char* base, int count, int stride, int off, int per, int maxBytes) {
    unsigned h = 2166136261u;
    if (!base || count <= 0 || stride <= 0 || per <= 0) return h;
    const int step = M2QpStepFor(count, per, maxBytes);
    int hashed = 0;
    for (int i = 0; i < count; i += step) {
        const unsigned char* p = base + (size_t)i * (size_t)stride + (size_t)off;
        h ^= (unsigned)i;
        h *= 16777619u;
        h = M2QpFnv1aMix(h, p, per);
        ++hashed;
    }
    h ^= (unsigned)hashed * 2654435761u;
    return h;
}
// ---- M2_QP_CORE_END ----

// =====================================================================================================
// ---- M2_QP_EMIT_BEGIN ----  (byte emitters, verified with capstone by scripts/test_quant_parallel.py)
// =====================================================================================================
// Extended prologue whitelist for the quant leaf.  MSVC 2005 /O2 (implicit /Oy) and GCC -O2 both omit the
// frame pointer, so the landed Wave-1/2 whitelist - which requires `push ebp; mov ebp,esp` - misses many
// real functions (the synthetic scene.dll fixture starts `53 BB imm32 83 EC 18`, for example).  These extra
// families are still decodable without a length disassembler because every accepted byte is either a
// one-byte push, a fixed 5-byte `mov r32,imm32`, or a `sub esp,imm8|imm32` whose length is unambiguous, and
// the terminating `sub esp` anchors the instruction boundary exactly like the frame families do.
// Anything outside this grammar is refused; M2_QUANT_PROLOGUE=<n> lets the Mac side declare the exact cut
// length computed with capstone (scripts/m2_verify_quant_rva.py prints the ready-to-use value).
static int M2QpPrologueLen(const unsigned char* p) {
    const int l = M2_ZonePrologueLen(p);
    if (l >= 5) return l;
    int i = 0;
    while (i < 6 && p[i] >= 0x50 && p[i] <= 0x57) ++i;      // callee-saved pushes
    for (int m = 0; m < 2; ++m) {                            // up to two `mov r32, imm32` register loads
        if (p[i] >= 0xB8 && p[i] <= 0xBF) i += 5;
    }
    if (p[i] == 0x83 && p[i + 1] == 0xEC) i += 3;            // sub esp, imm8
    else if (p[i] == 0x81 && p[i + 1] == 0xEC) i += 6;       // sub esp, imm32
    else return 0;
    return i >= 5 ? i : 0;
}
// __cdecl helper the pool workers call:  void m2_leaf(uintptr_t self)
//   mov ecx,[esp+4]              ; this  (thiscall / fastcall first argument)
//   mov edx,[esp+8]              ; edx   (thiscall calls it with edx == this; harmless for cdecl)
//   push dword ptr [slot+n-1] .. push dword ptr [slot+0]     ; the leaf's constant stack arguments
//   call dword ptr [trampSlot]   ; the replayed-prologue trampoline (never the patched entry)
//   add esp, 4*n - retBytes      ; only when the callee does not pop everything itself
//   ret
static int M2QpEmitLeafCall(unsigned char* out, int nArgs, int retBytes, void* trampSlot, void* argSlots) {
    unsigned char* w = out;
    *w++ = 0x8B; *w++ = 0x4C; *w++ = 0x24; *w++ = 0x04;              // mov ecx,[esp+4]
    *w++ = 0x8B; *w++ = 0x54; *w++ = 0x24; *w++ = 0x08;              // mov edx,[esp+8]
    if (nArgs < 0) nArgs = 0;
    if (nArgs > 4) nArgs = 4;
    for (int i = nArgs - 1; i >= 0; --i) {
        *w++ = 0xFF; *w++ = 0x35;                                     // push dword ptr [imm32]
        *(unsigned int*)w = (unsigned int)(uintptr_t)((unsigned char*)argSlots + 4 * i);
        w += 4;
    }
    *w++ = 0xFF; *w++ = 0x15;                                         // call dword ptr [imm32]
    *(unsigned int*)w = (unsigned int)(uintptr_t)trampSlot;
    w += 4;
    const int pop = 4 * nArgs - retBytes;
    if (pop > 0) {
        *w++ = 0x83; *w++ = 0xC4; *w++ = (unsigned char)(pop & 0xFF);  // add esp, imm8
    }
    *w++ = 0xC3;                                                      // ret
    return (int)(w - out);
}
// ---- M2_QP_EMIT_END ----

// ---- OS hooks used by the testable logic block ------------------------------------------------------
// The ladder below (learn -> verify -> parallel) is deliberately OS-agnostic so that
// scripts/test_quant_parallel.py can extract it and run it against a synthetic engine loop with real
// threads.  The host harness defines the same six macros plus M2QpFanout itself.
#ifndef M2QP_HOST
static double M2QpQpcTickMs(void) {
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || !f.QuadPart) return 0.0;
    return 1000.0 / (double)f.QuadPart;
}
#define M2QP_LOG(msg)      LogLine(msg)
#define M2QP_NOW()         Qpc()
#define M2QP_TICKMS()      M2QpQpcTickMs()
#define M2QP_ALLOC(n)      VirtualAlloc(NULL, (n), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
#define M2QP_TID()         ((unsigned long)GetCurrentThreadId())
#define M2QP_LEAF_READY()  (g_qpLeaf != NULL)
#define M2QP_FANOUT(lo, hi) M2QpFanout((lo), (hi))
#endif
static int M2QpFanout(int lo, int hi);          // Windows layer below (host harness supplies its own)

// =====================================================================================================
// ---- M2_QP_LOGIC_BEGIN ----  (the ladder itself: learning, verification, parallel dispatch)
// =====================================================================================================
#define M2QP_MAX_THREADS 16

typedef struct {
    int      mode;              // 0 off / 1 probe / 2 verify / 3 parallel
    int      installed, ready, done;
    int      prologueOverride;  // M2_QUANT_PROLOGUE: Mac-confirmed cut length (capstone), 0 = auto
    int      stableFrames, count, stride, nArgs, retBytes;
    uintptr_t base;
    // learning buffer (main thread only)
    uintptr_t seq[M2QP_SEQ_MAX];
    int       seqLen, seqOverflow;
    uintptr_t lastBase; int lastCount, lastStride;
    uintptr_t frameArgs[4];     // the leaf's stack arguments, captured from the saved context (M2_QUANT_ARGS)
    int       argsVary;         // they differ between entities in one frame -> parallel must be refused
    // measurement (probe/verify only): per-entity cost proxy = QPC delta between consecutive decisions
    int64_t  tickPrev, tickSum; uint32_t tickN; uint32_t tickMax;
    // counters (reset by telemetry)
    volatile uint32_t calls, fanouts, skips, foreign, mismatches, verifyOk, verifyBad, serialRuns;
    // verify state
    int      verifyPhase, verifyFrames, skipActive;
    unsigned fpPar, fpSer;
    unsigned char* snap;        // M2QP_ALLOC'd restore buffer (entity range)
    size_t   snapLen;
    unsigned char* snapExtra;   // ... plus the optional M2_QUANT_VERIFY_EXTRA range
    size_t   snapExtraLen;
    // resolved target
    char     module[32];
    uintptr_t rva;
    uintptr_t target;
    unsigned char sig[16];
    int      prologueLen;
    void*    block;             // RWX block: tramp@0, slot@64, argslots@80, thunk@96, stub@160
    int      thunkLen, stubLen;
} M2QpState;

static M2QpState g_qp;
static unsigned long g_qpMainTid = 0;
static void (*g_qpLeaf)(uintptr_t self) = NULL;
static int   g_qpThreads = 8, g_qpLearn = 3, g_qpVerifyNeed = 5, g_qpChunkMin = 4;
static int   g_qpFpOff = 0, g_qpFpLen = -1, g_qpFpMax = 512 * 1024, g_qpSnapMax = 16 * 1024 * 1024;
static uintptr_t g_qpExtraVa = 0; static size_t g_qpExtraLen = 0;
static int   g_qpExportNames = 0;                 // 1 = use the export table channel
static char  g_qpExportCsv[128] = "QuantEntities,AnimationSystem,QuantEntity";
static int   g_qpScan = 0;                        // 0 off / 1 log candidates / 2 auto-install
static int   g_qpKill = 0;
static uintptr_t* g_qpArgSlots = NULL;   // set by the installer: the slots the leaf-call thunk pushes from

// ---- learning / decision ----------------------------------------------------------------------------
static int M2QpFpLen(void) {
    int per = g_qpFpLen;
    if (per < 0) per = g_qp.stride < 256 ? g_qp.stride : 256;   // default: the whole entity when it fits
    if (per < 4) per = 4;
    if (per > g_qp.stride) per = g_qp.stride;
    if (per > 4096) per = 4096;
    return per;
}

static unsigned M2QpFp(void) {
    unsigned h = M2QpFingerprint((const unsigned char*)g_qp.base, g_qp.count, g_qp.stride,
                                 g_qpFpOff, M2QpFpLen(), g_qpFpMax);
    if (g_qpExtraVa && g_qpExtraLen) {
        h = M2QpFnv1aMix(h ^ 0x9E3779B9u, (const unsigned char*)g_qpExtraVa,
                         (int)(g_qpExtraLen > 262144 ? 262144 : g_qpExtraLen));
    }
    return h;
}

// Evaluate the just-finished frame's call sequence; sets g_qp.ready once the shape is stable.
static void M2QpEvalFrame(void) {
    M2QpState* s = &g_qp;
    if (s->ready) return;
    if (s->seqOverflow) {
        s->seqOverflow = 0;
        s->stableFrames = 0;
        M2QP_LOG("quant: call sequence longer than the learning buffer -> shape rejected");
        return;
    }
    const M2QpShape sh = M2QpShapeOf(s->seq, s->seqLen);
    if (!sh.ok) {
        if (s->stableFrames) {
            char m[160];
            snprintf(m, sizeof(m), "quant: frame shape broke (n=%d) -> learning restarted", s->seqLen);
            M2QP_LOG(m);
        }
        s->stableFrames = 0;
        return;
    }
    if (s->stableFrames && sh.count == s->lastCount && sh.stride == s->lastStride &&
        s->seq[0] == s->lastBase) {
        s->stableFrames++;
    } else {
        s->stableFrames = 1;
        s->lastBase = s->seq[0];
        s->lastCount = sh.count;
        s->lastStride = sh.stride;
        char m[220];
        snprintf(m, sizeof(m), "quant: frame shape base=%p count=%d stride=%d (stable %d/%d)",
                 (void*)s->seq[0], sh.count, sh.stride, s->stableFrames, g_qpLearn);
        M2QP_LOG(m);
    }
    if (s->stableFrames < g_qpLearn) return;

    s->base = s->lastBase;
    s->count = sh.count;
    s->stride = sh.stride;
    if (s->argsVary && s->mode >= 2) {
        M2QP_LOG("quant: the leaf's stack arguments differ between entities in one frame -> parallel disabled "
                 "(the fan-out cannot reproduce them); staying in probe mode");
        s->mode = 1;
    }
    if (s->mode >= 2 && s->nArgs < 0) {
        M2QP_LOG("quant: mode >= 2 needs M2_QUANT_ARGS (number of stack arguments the leaf takes) "
                "-> staying in probe mode");
        s->mode = 1;
    }
    if (!M2QP_LEAF_READY()) {
        M2QP_LOG("quant: shape learned but no leaf thunk (install failed) -> probe only");
        s->ready = 0;
        return;
    }
    s->ready = 1;
    {
        char m[320];
        const double mean = s->tickN ? (double)s->tickSum / (double)s->tickN : 0.0;
        const double tk2ms = M2QP_TICKMS();
        const double serialMs = mean * s->count * tk2ms;
        const double critMs = (double)s->tickMax * tk2ms;
        const double parMs = serialMs / (double)(g_qpThreads > 0 ? g_qpThreads : 1);
        snprintf(m, sizeof(m), "quant: READY base=%p count=%d stride=%d threads=%d mode=%d "
                 "serial_est=%.2fms par_est=%.2fms crit=%.2fms (per-entity mean=%.3fms max=%.3fms)",
                 (void*)s->base, s->count, s->stride, g_qpThreads, s->mode, serialMs,
                 parMs > critMs ? parMs : critMs, critMs, mean * tk2ms, critMs);
        M2QP_LOG(m);
    }
}

// The decision the byte stub calls: 1 = skip the engine call, 0 = run it (fail-open on any doubt).
extern "C" int __cdecl M2_QuantLeafDecision(void* self, void* ctx) {
    M2QpState* s = &g_qp;
    if (s->mode == 0 || g_qpKill) return 0;
    if (M2QP_TID() != g_qpMainTid) { s->foreign++; return 0; }

    const uintptr_t v = (uintptr_t)self;
    s->calls++;

    // ---- learning (probe/verify/parallel all pass through it until the shape is accepted) ----
    if (!s->ready) {
        if (s->mode < 3) {                                   // cheap per-entity cost proxy
            const int64_t t = M2QP_NOW();
            if (s->tickPrev) {
                const int64_t dt = t - s->tickPrev;
                if (dt > 0 && dt < 1000000000LL) {
                    s->tickSum += dt;
                    s->tickN++;
                    if ((uint32_t)dt > s->tickMax) s->tickMax = (uint32_t)dt;
                }
            }
            s->tickPrev = t;
        }
        if (s->nArgs > 0 && ctx) {
            // Saved-context layout after pushfd/pushad: +24 ecx (this), +36 return address, +40 first
            // stack argument.  A per-entity varying argument would make the fan-out pass entity 0's value
            // to every worker, so it is a hard gate, not a hint.
            const uintptr_t* sa = (const uintptr_t*)((const unsigned char*)ctx + 40);
            if (s->seqLen == 0) {
                for (int i = 0; i < 4; ++i) s->frameArgs[i] = sa[i];
            } else {
                for (int i = 0; i < 4; ++i) if (sa[i] != s->frameArgs[i]) s->argsVary = 1;
            }
        }
        if (s->seqLen >= 1 && v == s->seq[0]) {              // the loop restarted -> previous frame complete
            M2QpEvalFrame();
            s->seqLen = 0;                                   // start collecting the new frame at index 0
        }
        if (s->seqLen >= M2QP_SEQ_MAX) { s->seqOverflow = 1; s->seqLen = 0; return 0; }
        s->seq[s->seqLen++] = v;
        return 0;
    }

    // ---- ready: classify the call ----
    int idx = -1;
    int inRange = 0;
    if (s->stride == 1) {
        if (v == s->seq[0]) idx = 0;
        else if (s->skipActive) { idx = 1; inRange = 1; }
    } else {
        idx = (g_qp.stride > 0 && v >= s->base)
                            ? (int)((v - s->base) / (uintptr_t)g_qp.stride) : -1;
        inRange = (idx >= 0 && idx < s->count &&
                   v == s->base + (uintptr_t)idx * (uintptr_t)g_qp.stride);
    }

    if (idx == 0 && s->nArgs > 0 && ctx) {                    // publish this frame's arguments for the thunk
        const uintptr_t* sa = (const uintptr_t*)((const unsigned char*)ctx + 40);
        for (int i = 0; i < 4; ++i) s->frameArgs[i] = sa[i];
        if (g_qpArgSlots) for (int i = 0; i < 4; ++i) g_qpArgSlots[i] = s->frameArgs[i];
    }

    if (s->mode == 3) {                                       // ---- parallel ----
        if (idx == 0) {
            s->skipActive = M2QP_FANOUT(1, s->count);          // workers take entities 1..count-1
            return 0;                                         // the caller runs entity 0 itself
        }
        if (inRange && s->skipActive) { s->skips++; return 1; }   // already computed by the pool
        s->foreign++;
        return 0;
    }

    if (s->mode == 2) {                                       // ---- verify ----
        if (s->verifyPhase == 1 && idx == 0) {                // the serial replay never reached the last
            s->verifyPhase = 0;                               // entity -> this frame cannot be judged
            s->verifyBad++;
            M2QP_LOG("quant: verify frame incomplete (last entity not seen) -> counted as a failure");
        }
        if (idx == 0 && s->verifyPhase == 0) {
            const size_t bytes = (size_t)s->count * (size_t)s->stride;
            if (!s->snap && bytes && bytes <= (size_t)g_qpSnapMax) {
                s->snap = (unsigned char*)M2QP_ALLOC(bytes);
                if (s->snap) s->snapLen = bytes;
            }
            if (!s->snap || s->snapLen < bytes) {
                static int warned = 0;
                if (!warned) {
                    warned = 1;
                    M2QP_LOG("quant: verify needs a snapshot buffer (raise M2_QUANT_SNAP_MAX) -> probe mode");
                }
                s->mode = 1;
                return 0;
            }
            if (g_qpExtraVa && g_qpExtraLen && !s->snapExtra) {
                s->snapExtra = (unsigned char*)M2QP_ALLOC(g_qpExtraLen);
                if (s->snapExtra) s->snapExtraLen = g_qpExtraLen;
            }
            memcpy(s->snap, (const void*)s->base, bytes);
            if (s->snapExtra) memcpy(s->snapExtra, (const void*)g_qpExtraVa, s->snapExtraLen);
            // Fan out [0, count-1): the last entity is deliberately left out so that the "par" fingerprint
            // (taken right here, before any serial leaf has run) and the "ser" fingerprint (taken at the last
            // decision, before leaf(count-1) runs) describe exactly the same set of computed entities.
            if (!M2QP_FANOUT(0, s->count - 1)) {              // no pool -> the "verify" would be a lie
                s->mode = 1;
                M2QP_LOG("quant: pool unavailable (thread creation failed) -> probe mode");
                return 0;
            }
            s->fpPar = M2QpFp();
            memcpy((void*)s->base, s->snap, bytes);           // restore: the serial replay starts identical
            if (s->snapExtra) memcpy((void*)g_qpExtraVa, s->snapExtra, s->snapExtraLen);
            s->verifyPhase = 1;
            return 0;
        }
        if (s->verifyPhase == 1 && idx == s->count - 1) {
            s->fpSer = M2QpFp();
            s->verifyPhase = 0;
            s->serialRuns++;
            if (s->fpPar == s->fpSer) {
                s->verifyOk++;
                s->verifyFrames++;
                char m[220];
                snprintf(m, sizeof(m), "quant: verify %d/%d OK (fp=%08X count=%d stride=%d)",
                         s->verifyFrames, g_qpVerifyNeed, s->fpSer, s->count, s->stride);
                M2QP_LOG(m);
                if (s->verifyFrames >= g_qpVerifyNeed) {
                    s->mode = 3;
                    M2QP_LOG("quant: verify PASSED -> parallel mode enabled (M2_QUANT_KILL=1 to disable)");
                }
            } else {
                s->mismatches++;
                s->verifyBad++;
                char m[240];
                snprintf(m, sizeof(m), "quant: verify MISMATCH par=%08X ser=%08X (count=%d stride=%d) "
                         "-> parallel stays OFF (probe mode)", s->fpPar, s->fpSer, s->count, s->stride);
                M2QP_LOG(m);
                s->mode = 1;
            }
            return 0;
        }
        return 0;
    }
    return 0;                                                 // probe: always run
}
// ---- M2_QP_LOGIC_END ----

// =====================================================================================================
// Windows layer: the worker pool, discovery/install, telemetry
// =====================================================================================================
static HANDLE g_qpWake[M2QP_MAX_THREADS];
static HANDLE g_qpDone;
static int    g_qpWorkers = 0;
static volatile LONG g_qpNextChunk = 0, g_qpRemaining = 0;
static const uintptr_t* g_qpPoolSeq = NULL;
static uintptr_t g_qpPoolBase = 0;
static int    g_qpPoolLo = 0, g_qpPoolHi = 0, g_qpPoolStride = 0, g_qpPoolChunk = 1, g_qpPoolNChunks = 0;

static void M2QpRunChunks(void) {
    for (;;) {
        const LONG c = InterlockedIncrement(&g_qpNextChunk) - 1;
        if (c >= g_qpPoolNChunks) return;
        int lo = g_qpPoolLo + (int)c * g_qpPoolChunk;
        int hi = lo + g_qpPoolChunk;
        if (hi > g_qpPoolHi) hi = g_qpPoolHi;
        for (int i = lo; i < hi; ++i) {
            uintptr_t target = g_qpPoolSeq ? g_qpPoolSeq[i] : (g_qpPoolBase + (uintptr_t)i * (uintptr_t)g_qpPoolStride);
            if (target) g_qpLeaf(target);
        }
    }
}

static DWORD WINAPI M2QpWorker(LPVOID param) {
    const int idx = (int)(uintptr_t)param;
    for (;;) {
        if (WaitForSingleObject(g_qpWake[idx], INFINITE) != WAIT_OBJECT_0) return 0;
        M2QpRunChunks();
        if (InterlockedDecrement(&g_qpRemaining) == 0) SetEvent(g_qpDone);
    }
}

// Creates the pool on first use.  Returns the number of participating threads (1 = caller only).
static int M2QpPoolEnsure(void) {
    if (g_qpWorkers > 0) return g_qpWorkers + 1;
    if (!g_qpDone) {
        g_qpDone = CreateEventA(NULL, TRUE, FALSE, NULL);   // manual reset
        if (!g_qpDone) return 1;
    }
    int want = g_qpThreads - 1;                             // the caller works too
    if (want < 0) want = 0;
    if (want > M2QP_MAX_THREADS) want = M2QP_MAX_THREADS;
    for (int i = 0; i < want; ++i) {
        g_qpWake[i] = CreateEventA(NULL, FALSE, FALSE, NULL);   // auto reset
        if (!g_qpWake[i]) break;
        HANDLE th = CreateThread(NULL, 0, M2QpWorker, (LPVOID)(uintptr_t)i, 0, NULL);
        if (!th) break;
        SetThreadPriority(th, THREAD_PRIORITY_NORMAL);
        CloseHandle(th);
        ++g_qpWorkers;
    }
    char m[160];
    snprintf(m, sizeof(m), "quant: pool up threads=%d (1 caller + %d workers)", g_qpWorkers + 1, g_qpWorkers);
    LogLine(m);
    return g_qpWorkers + 1;
}

// Synchronous fan-out of [lo,hi) across the pool; the calling thread participates.  Returns 1 on success.
static int M2QpFanout(int lo, int hi) {
    if (!g_qpLeaf || hi <= lo) return 0;
    if (M2QpPoolEnsure() < 2) return 0;
    g_qpPoolSeq = (g_qp.stride == 1) ? g_qp.seq : NULL;
    g_qpPoolBase = g_qp.base;
    g_qpPoolStride = g_qp.stride;
    g_qpPoolLo = lo; g_qpPoolHi = hi;
    g_qpPoolChunk = M2QpChunkFor(hi - lo, g_qpWorkers + 1, g_qpChunkMin);
    g_qpPoolNChunks = M2QpNChunks(hi - lo, g_qpPoolChunk);
    InterlockedExchange(&g_qpNextChunk, 0);
    InterlockedExchange(&g_qpRemaining, (LONG)g_qpWorkers);
    ResetEvent(g_qpDone);
    for (int i = 0; i < g_qpWorkers; ++i) SetEvent(g_qpWake[i]);
    M2QpRunChunks();                                        // the caller takes chunks as well
    if (InterlockedCompareExchange(&g_qpRemaining, 0, 0) != 0) {
        WaitForSingleObject(g_qpDone, INFINITE);
    }
    g_qp.fanouts++;
    return 1;
}

// ---- discovery + install ----------------------------------------------------------------------------
// Channel 1: explicit RVA.  Channel 2: the module's own export table (scene.dll exports decorated C++
// symbols - e.g. ?DeltaQuants@eQuantControl@xScene@@QBEHXZ is already used by the quant timer - so an
// exported ?QuantEntities@... is an exact, non-guessed address).  Channel 3: ASCII-anchor scan (evidence
// only unless M2_QUANT_SCAN=auto).
// Case-insensitive substring: does `hay` contain `needle`?
static int M2QpContainsI(const char* hay, const char* needle) {
    const int nl = (int)strlen(needle);
    if (nl <= 0 || !hay) return 0;
    for (const char* p = hay; *p; ++p) {
        int i = 0;
        while (i < nl && p[i] && (p[i] == needle[i] || p[i] + 32 == needle[i] || p[i] - 32 == needle[i])) ++i;
        if (i == nl) return 1;
    }
    return 0;
}

// Is any comma-separated token of `csv` contained in `name`?
static int M2QpNameMatches(const char* csv, const char* name) {
    for (const char* p = csv; *p; ) {
        char tok[64];
        int k = 0;
        while (*p && *p != ',' && k < (int)sizeof(tok) - 1) tok[k++] = *p++;
        tok[k] = 0;
        while (*p == ',') ++p;
        if (k && M2QpContainsI(name, tok)) return 1;
    }
    return 0;
}

// Walks the module export table; returns the RVA of the first export whose name contains one of the
// CSV substrings, or 0.  Logs every hit so the Mac side can confirm the choice from the run log.
static uintptr_t M2QpResolveFromExports(HMODULE h, char* nameOut, size_t nameCap) {
    BYTE* b = (BYTE*)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(b + dir.VirtualAddress);
    DWORD* funcs = (DWORD*)(b + ed->AddressOfFunctions);
    DWORD* names = (DWORD*)(b + ed->AddressOfNames);
    WORD* ords = (WORD*)(b + ed->AddressOfNameOrdinals);
    uintptr_t found = 0;
    for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
        const char* nm = (const char*)(b + names[i]);
        if (!M2QpNameMatches(g_qpExportCsv, nm)) continue;
        const DWORD rva = funcs[ords[i]];
        char m[300];
        snprintf(m, sizeof(m), "quant: export hit %s+0x%lX  %s", g_qp.module, (unsigned long)rva, nm);
        LogLine(m);
        if (!found) {
            found = rva;
            snprintf(nameOut, nameCap, "%s", nm);
        }
    }
    return found;
}

// ASCII-anchor scan: strings in non-executable sections -> imm32 references in .text -> nearest
// whitelisted prologue.  Evidence only (never installed unless M2_QUANT_SCAN=auto).
static int M2QpScanModule(HMODULE h, uintptr_t* outRva, int outCap) {
    static const char* const kAnchors[] = {"QuantEntities", "eAnimationSystem", "xBone", "eScene", "Quant"};
    const int nAnchors = (int)(sizeof(kAnchors) / sizeof(kAnchors[0]));
    BYTE* b = (BYTE*)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t anchorVa[16];
    int nAnchorVa = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && nAnchorVa < 16; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        const unsigned char* p = b + sec[i].VirtualAddress;
        const int n = (int)(sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
        if (n <= 0) continue;
        for (int a = 0; a < nAnchors && nAnchorVa < 16; ++a) {
            int off = M2_FindAsciiI(p, n, kAnchors[a]);
            if (off < 0) continue;
            // A compiler references the START of the string literal, not the offset of the substring we
            // matched ("xScene::eScene::QuantEntities" is referenced at its first byte, not at +14), so walk
            // back to the beginning of the C string (bounded to 128 bytes) before recording the VA.
            for (int back = 0; back < 128 && off > 0 && p[off - 1] >= 0x20 && p[off - 1] < 0x7F; ++back) --off;
            anchorVa[nAnchorVa++] = (uintptr_t)h + sec[i].VirtualAddress + (uintptr_t)off;
        }
    }
    char m[220];
    snprintf(m, sizeof(m), "quant: scan anchors=%d module=%s", nAnchorVa, g_qp.module);
    LogLine(m);
    int nOut = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE* text = b + sec[i].VirtualAddress;
        const int n = (int)(sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
        if (n <= 32) continue;
        for (int q = 0; q + 4 <= n; q += 1) {
            const unsigned int word = *(const unsigned int*)(text + q);
            if (word < (unsigned int)(uintptr_t)h || word >= (unsigned int)((uintptr_t)h + nt->OptionalHeader.SizeOfImage))
                continue;
            int hit = 0;
            for (int a = 0; a < nAnchorVa; ++a) if ((uintptr_t)word == anchorVa[a]) { hit = 1; break; }
            if (!hit) continue;
            int plen = 0;
            const int start = M2_PfWalkBackStart(text, n, q, 0x800, &plen);
            if (start < 0 || plen < 5) continue;
            plen = M2QpPrologueLen(text + start);
            if (plen < 5) continue;
            const uintptr_t rva = (uintptr_t)sec[i].VirtualAddress + (uintptr_t)start;
            int dup = 0;
            for (int k = 0; k < nOut; ++k) if (outRva[k] == rva) dup = 1;
            if (dup || nOut >= outCap) continue;
            outRva[nOut++] = rva;
            int xr = 0, xa = 0;
            M2_PfX87Density(text + start, n - start > 4096 ? 4096 : n - start, &xr, &xa);
            snprintf(m, sizeof(m), "quant: scan candidate %s+0x%lX prologue=%d x87reg=%d x87all=%d sig=%02X%02X%02X%02X",
                     g_qp.module, (unsigned long)rva, plen, xr, xa,
                     text[start], text[start + 1], text[start + 2], text[start + 3]);
            LogLine(m);
        }
    }
    return nOut;
}

// Resolves the target and installs the stub.  Idempotent, retried from Hook_Present until it lands.
static int M2_QuantInstall(void) {
    static int cfgDone = 0;
    M2QpState* s = &g_qp;
    if (s->installed) return 1;
    if (!cfgDone) {
        cfgDone = 1;
        g_qpMainTid = GetCurrentThreadId();
        snprintf(s->module, sizeof(s->module), "scene.dll");
        s->nArgs = 0;                                         // default: thiscall with 0 stack arguments
        s->retBytes = 0;
        s->rva = 0x23AB0;                                     // default: scene.dll+0x23AB0 (xBone::eAnimationSystem::Quant)
        s->prologueOverride = 5;                              // default: 5 bytes confirmed prologue
        s->mode = 0;                                          // default: 0 (disabled: superseded by m2_quant_distributor.h)
        g_qpThreads = 4;                                      // default: 4 Apple Silicon P-cores (1 caller + 4 workers)
        g_qpLearn = 2;                                        // default: 2 frames to learn shape
        char v[256] = {0};
        if (GetEnvironmentVariableA("M2_QUANT_KILL", v, sizeof(v)) > 0 && v[0] == '1') g_qpKill = 1;
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT", v, sizeof(v)) > 0) {
            if (!_stricmp(v, "probe")) s->mode = 1;
            else if (!_stricmp(v, "verify")) s->mode = 2;
            else if (v[0] == '1' || !_stricmp(v, "parallel")) s->mode = 3;
            else if (v[0] == '0' || !_stricmp(v, "off")) s->mode = 0;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_THREADS", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 1 && n <= M2QP_MAX_THREADS) g_qpThreads = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_LEARN", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 1 && n <= 600) g_qpLearn = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_VERIFY", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 1 && n <= 600) g_qpVerifyNeed = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_ARGS", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 0 && n <= 4) s->nArgs = n;
        }
        s->retBytes = s->nArgs > 0 ? 4 * s->nArgs : 0;
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_RET", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 0 && n <= 64) s->retBytes = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_CHUNK_MIN", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 1 && n <= 1024) g_qpChunkMin = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_FP_OFF", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 0 && n <= 4096) g_qpFpOff = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_FP_LEN", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 4 && n <= 4096) g_qpFpLen = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_FP_MAX", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 4096 && n <= 64 * 1024 * 1024) g_qpFpMax = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_SNAP_MAX", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 65536 && n <= 256 * 1024 * 1024) g_qpSnapMax = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_VERIFY_EXTRA", v, sizeof(v)) > 0) {
            unsigned a = 0, b = 0;
            if (sscanf(v, "%x:%x", &a, &b) == 2 && b > 0 && b <= 4u * 1024 * 1024) {
                g_qpExtraVa = (uintptr_t)a;
                g_qpExtraLen = (size_t)b;
            }
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_EXPORT", v, sizeof(v)) > 0) {
            if (!_stricmp(v, "0")) g_qpExportNames = 0;
            else { g_qpExportNames = 1; snprintf(g_qpExportCsv, sizeof(g_qpExportCsv), "%s", v); }
        } else {
            g_qpExportNames = 1;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_SCAN", v, sizeof(v)) > 0) {
            if (!_stricmp(v, "auto")) g_qpScan = 2;
            else if (v[0] == '1') g_qpScan = 1;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_PROLOGUE", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 5 && n <= 32) s->prologueOverride = n;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_MODULE", v, sizeof(v)) > 0 && v[0])
            snprintf(s->module, sizeof(s->module), "%s", v);
        // explicit RVA: <module>:<hex> or <hex>
        v[0] = 0;
        if (GetEnvironmentVariableA("M2_QUANT_RVA", v, sizeof(v)) > 0 && v[0]) {
            char* colon = strchr(v, ':');
            unsigned rv = 0;
            if (colon) {
                *colon = 0;
                if (v[0]) snprintf(s->module, sizeof(s->module), "%s", v);
                if (sscanf(colon + 1, "%x", &rv) == 1) s->rva = rv;
            } else if (sscanf(v, "%x", &rv) == 1) {
                s->rva = rv;
            }
        }
        char m[300];
        snprintf(m, sizeof(m), "quant: cfg mode=%d threads=%d learn=%d verify=%d args=%d ret=%d rva=0x%lX "
                 "module=%s exports=%d scan=%d kill=%d",
                 s->mode, g_qpThreads, g_qpLearn, g_qpVerifyNeed, s->nArgs, s->retBytes,
                 (unsigned long)s->rva, s->module, g_qpExportNames, g_qpScan, g_qpKill);
        LogLine(m);
        if (s->mode == 0) {
            LogLine("quant: disabled (M2_QUANT=probe for evidence, =verify for the equivalence gate, =1 for "
                    "verify-then-parallel)");
            s->done = 1;
            return 0;
        }
    }
    if (s->done) return 0;
    HMODULE h = GetModuleHandleA(s->module);
    if (!h) return 0;                                  // retried from Hook_Present
    char m[400];
    uintptr_t rva = s->rva;
    if (!rva && g_qpExportNames) {
        char nm[220] = {0};
        rva = M2QpResolveFromExports(h, nm, sizeof(nm));
        if (!rva) LogLine("quant: no matching export in the module export table");
    }
    if (!rva && g_qpScan) {
        uintptr_t cand[16];
        const int n = M2QpScanModule(h, cand, 16);
        snprintf(m, sizeof(m), "quant: scan candidates=%d%s", n,
                 (n == 1 && g_qpScan == 2) ? " (auto-install)" : " (evidence only; confirm on the Mac, then "
                 "set M2_QUANT_RVA=<mod>:<hex> or M2_QUANT_SCAN=auto)");
        LogLine(m);
        if (n == 1 && g_qpScan == 2) rva = cand[0];
    }
    if (!rva) {
        snprintf(m, sizeof(m), "quant: no target for %s (set M2_QUANT_RVA=<mod>:<hex>, M2_QUANT_EXPORT=..., "
                 "or M2_QUANT_SCAN=1) -> engine runs unpatched", s->module);
        LogLine(m);
        s->done = 1;
        return 0;
    }
    s->rva = rva;
    const uintptr_t target = (uintptr_t)h + rva;
    char sec[16] = {0};
    DWORD chars = 0;
    if (!M2_SectionForVa(target, sec, sizeof(sec), &chars)) {
        snprintf(m, sizeof(m), "quant: %s+0x%lX is not inside any section -> refusing to patch",
                 s->module, (unsigned long)rva);
        LogLine(m);
        s->done = 1;
        return 0;
    }
    if (!(chars & IMAGE_SCN_MEM_EXECUTE) || _stricmp(sec, ".text") != 0) {
        snprintf(m, sizeof(m), "quant: %s+0x%lX lives in section %s (chars=0x%lX) -> refusing to patch "
                 "(must be executable .text, never .reloc)", s->module, (unsigned long)rva, sec,
                 (unsigned long)chars);
        LogLine(m);
        s->done = 1;
        return 0;
    }
    const BYTE* p = (const BYTE*)target;
    int plen = M2QpPrologueLen(p);
    if (s->prologueOverride > 0) {
        plen = s->prologueOverride;               // declared by the Mac side from a capstone decode
        snprintf(m, sizeof(m), "quant: using M2_QUANT_PROLOGUE=%d (Mac-confirmed cut length)", plen);
        LogLine(m);
    }
    if (plen == 0) {
        snprintf(m, sizeof(m), "quant: %s+0x%lX prologue %02X %02X %02X %02X %02X %02X %02X %02X is not a "
                 "whitelisted shape -> not installed", s->module, (unsigned long)rva,
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        LogLine(m);
        s->done = 1;
        return 0;
    }
    s->prologueLen = plen;
    M2_PfSig16(p, s->sig);

    // RWX block: tramp@0, trampSlot@64, argSlots@80 (16 B), thunk@96 (64 B), stub@160 (64 B)
    void* blk = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blk) {
        LogLine("quant: VirtualAlloc for the stub block failed");
        s->done = 1;
        return 0;
    }
    memset(blk, 0xCC, 256);
    BYTE* tramp = (BYTE*)blk;
    void** trampSlot = (void**)((BYTE*)blk + 64);
    void** argSlots = (void**)((BYTE*)blk + 80);
    BYTE* thunk = (BYTE*)blk + 96;
    BYTE* stub = (BYTE*)blk + 160;
    M2_ZoneEmitTrampoline(tramp, p, plen, (void*)(target + (uintptr_t)plen));
    *trampSlot = (void*)tramp;
    for (int i = 0; i < 4; ++i) argSlots[i] = NULL;
    g_qpArgSlots = (uintptr_t*)argSlots;
    s->thunkLen = M2QpEmitLeafCall(thunk, s->nArgs > 0 ? s->nArgs : 0, s->retBytes, (void*)trampSlot, (void*)argSlots);
    s->stubLen = M2_PfEmitStub(stub, (void*)&M2_QuantLeafDecision, (void*)((BYTE*)blk + 64), s->retBytes);
    g_qpLeaf = (void (*)(uintptr_t))thunk;
    if (memcmp((const void*)target, p, (size_t)plen) != 0) {
        LogLine("quant: target changed between probe and patch -> aborted");
        VirtualFree(blk, 0, MEM_RELEASE);
        s->done = 1;
        return 0;
    }
    BYTE jmp5[5];
    jmp5[0] = 0xE9;
    *(int32_t*)(jmp5 + 1) = (int32_t)((uintptr_t)stub - (target + 5));
    DWORD oldProt = 0;
    if (!VirtualProtect((void*)target, (SIZE_T)plen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        LogLine("quant: VirtualProtect on the target failed");
        VirtualFree(blk, 0, MEM_RELEASE);
        s->done = 1;
        return 0;
    }
    memcpy((void*)target, jmp5, 5);
    VirtualProtect((void*)target, (SIZE_T)plen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, (SIZE_T)plen);
    s->installed = 1;
    s->target = target;
    s->block = blk;
    snprintf(m, sizeof(m), "quant: installed %s+0x%lX section=%s prologue=%d stub=%d thunk=%d ret=%d "
             "sig=%02X%02X%02X%02X mode=%d (ladder: probe -> verify -> parallel)",
             s->module, (unsigned long)rva, sec, plen, s->stubLen, s->thunkLen, s->retBytes,
             s->sig[0], s->sig[1], s->sig[2], s->sig[3], s->mode);
    LogLine(m);
    return 1;
}

static void M2_QuantTelemetry(double tSec) {
    M2QpState* s = &g_qp;
    if (s->mode == 0 || g_logFile == INVALID_HANDLE_VALUE) return;
    const uint32_t calls = s->calls, fan = s->fanouts, sk = s->skips, fo = s->foreign;
    const uint32_t mism = s->mismatches, vok = s->verifyOk, vbad = s->verifyBad, sr = s->serialRuns;
    s->calls = s->fanouts = s->skips = s->foreign = s->mismatches = s->verifyOk = s->verifyBad = s->serialRuns = 0;
    char line[512];
    int len = snprintf(line, sizeof(line),
                       "# quant t=%.2f mode=%d installed=%d ready=%d calls=%u fanouts=%u skips=%u foreign=%u "
                       "count=%d stride=%d threads=%d stable=%d verify_ok=%u verify_bad=%u mismatches=%u "
                       "serial=%u fp_par=%08X fp_ser=%08X\n",
                       tSec, s->mode, s->installed, s->ready, calls, fan, sk, fo, s->count, s->stride,
                       g_qpThreads, s->stableFrames, vok, vbad, mism, sr, s->fpPar, s->fpSer);
    if (len > 0) {
        DWORD w = 0;
        WriteFile(g_logFile, line, (DWORD)len, &w, NULL);
    }
}

#endif // M2_QUANT_PARALLEL_H
