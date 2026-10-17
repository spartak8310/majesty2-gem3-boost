
#ifndef M2_PATHER_SSE2_H
#define M2_PATHER_SSE2_H
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
extern "C" uintptr_t g_sceneBase;
// L1 continuation targets, written by M2_PatherSSE_Install.  The original
// slice exits by FALL-THROUGH; the replacement must not clobber ANY register
// on its way out (W3e: the old `mov eax,[g_sceneBase]; add; jmp eax` tail
// left EAX = jump target, breaking the exit register contract).
extern "C" uintptr_t g_l1DoneTgt;
extern "C" uintptr_t g_l1SkipTgt;

static const unsigned char M2_ORIG_BYTES_L3[78] = { 0x8B, 0x54, 0x24, 0x04, 0x3B, 0x51, 0x24, 0x75, 0x08, 0x89, 0x51, 0x28, 0xB0, 0x01, 0xC2, 0x04, 0x00, 0xD9, 0x41, 0x1C, 0xD8, 0x62, 0x24, 0xD9, 0x41, 0x20, 0xD8, 0x62, 0x28, 0xDC, 0xC8, 0xD9, 0xC1, 0xDE, 0xCA, 0xDE, 0xC1, 0xD8, 0x51, 0x2C, 0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x7A, 0x08, 0xD9, 0x59, 0x2C, 0x89, 0x51, 0x28, 0xEB, 0x02, 0xDD, 0xD8, 0x8B, 0x41, 0x18, 0xD9, 0x40, 0x08, 0xD8, 0x5A, 0x10, 0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x7B, 0xC3, 0x32, 0xC0, 0xC2, 0x04, 0x00 };
static const unsigned char M2_ORIG_BYTES_L2[172] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0x39, 0x3B, 0xF7, 0x0F, 0x8D, 0x97, 0x00, 0x00, 0x00, 0x53, 0x55, 0x8D, 0x5C, 0x36, 0x01, 0x3B, 0xDF, 0x8B, 0xD6, 0x8D, 0x6C, 0x36, 0x02, 0x7D, 0x12, 0x8B, 0x41, 0x08, 0xD9, 0x04, 0xF0, 0xD8, 0x1C, 0xD8, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x02, 0x8B, 0xD3, 0x3B, 0xEF, 0x7D, 0x12, 0x8B, 0x41, 0x08, 0xD9, 0x04, 0xD0, 0xD8, 0x1C, 0xE8, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x02, 0x8B, 0xD5, 0x3B, 0xD6, 0x74, 0x59, 0x8B, 0x41, 0x08, 0x8B, 0x3C, 0xF0, 0x8B, 0x2C, 0xD0, 0x8B, 0x5C, 0xF0, 0x04, 0x89, 0x2C, 0xF0, 0x8B, 0x6C, 0xD0, 0x04, 0x89, 0x6C, 0xF0, 0x04, 0x89, 0x3C, 0xD0, 0x89, 0x5C, 0xD0, 0x04, 0x8B, 0x41, 0x08, 0x8D, 0x44, 0xF0, 0x04, 0x8B, 0x00, 0x8B, 0x78, 0x18, 0x33, 0xFE, 0x81, 0xE7, 0xFF, 0xFF, 0x0F, 0x00, 0x31, 0x78, 0x18, 0x8B, 0x41, 0x08, 0x8D, 0x44, 0xD0, 0x04, 0x8B, 0x00, 0x8B, 0x70, 0x18, 0x33, 0xF2, 0x81, 0xE6, 0xFF, 0xFF, 0x0F, 0x00, 0x31, 0x70, 0x18, 0x8B, 0x39, 0x3B, 0xD7, 0x8B, 0xF2, 0x0F, 0x8C, 0x6D, 0xFF, 0xFF, 0xFF, 0x5D, 0x5B, 0x5F, 0x5E, 0xC2, 0x04, 0x00 };
static const unsigned char M2_ORIG_BYTES_L1[196] = { 0xD9, 0x05, 0xA4, 0x69, 0x12, 0x10, 0xC7, 0x43, 0x18, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x40, 0x38, 0xD9, 0x5B, 0x10, 0xC1, 0xE0, 0x16, 0x89, 0x43, 0x18, 0x8B, 0x0D, 0x04, 0x22, 0x1B, 0x10, 0x8B, 0x54, 0x24, 0x48, 0x0F, 0xBE, 0x42, 0x14, 0x8B, 0x54, 0x24, 0x18, 0xD9, 0x44, 0x85, 0x08, 0xD8, 0x42, 0x10, 0xD8, 0x53, 0x10, 0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x0F, 0x8A, 0x90, 0x03, 0x00, 0x00, 0xD9, 0x44, 0x24, 0x24, 0xD8, 0x5B, 0x24, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x0F, 0x8A, 0x7E, 0x03, 0x00, 0x00, 0xD9, 0x44, 0x24, 0x2C, 0xD8, 0x5B, 0x24, 0xDF, 0xE0, 0xF6, 0xC4, 0x01, 0x0F, 0x85, 0x6C, 0x03, 0x00, 0x00, 0xD9, 0x44, 0x24, 0x28, 0xD8, 0x5B, 0x28, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x0F, 0x8A, 0x5A, 0x03, 0x00, 0x00, 0xD9, 0x44, 0x24, 0x30, 0xD8, 0x5B, 0x28, 0xDF, 0xE0, 0xF6, 0xC4, 0x01, 0x0F, 0x85, 0x48, 0x03, 0x00, 0x00, 0x80, 0x7C, 0x24, 0x40, 0x00, 0xD9, 0x53, 0x10, 0x8B, 0x44, 0x24, 0x44, 0xD9, 0x54, 0x24, 0x14, 0x8B, 0x08, 0x89, 0x4B, 0x14, 0x74, 0x22, 0x8B, 0x44, 0x24, 0x1C, 0xD9, 0x43, 0x24, 0xD8, 0x60, 0x24, 0xD9, 0x43, 0x28, 0xD8, 0x60, 0x28, 0xDC, 0xC8, 0xD9, 0xC1, 0xDE, 0xCA, 0xDE, 0xC1, 0xD9, 0xFA, 0xDE, 0xC1, 0xD9, 0x5C, 0x24, 0x14, 0xEB, 0x02, 0xDD, 0xD8, 0x8B };

// ---- M2_VERIFY_CORE_BEGIN (host-testable: no Win32 calls below this line) ----
// A HIGHLOW relocation slot inside a verified region: 4 operand bytes at `off`
// hold a VA that the loader rewrites from fileVA to fileVA + (modBase - preferredBase).
struct M2_RelocSlot { unsigned off; unsigned fileVA; };
// scene.dll .reloc inside L1 (verified against bin/scene.dll md5 d98b8057...):
//   +2   operand of  D9 05 <A4 69 12 10>   fld dword ptr [0x101269A4]
//   +27  operand of  8B 0D <04 22 1B 10>   mov ecx,[0x101B2204]
static const M2_RelocSlot M2_L1_RELOCS[2] = { { 2u, 0x101269A4u }, { 27u, 0x101B2204u } };

// Preferred (link-time) ImageBase read from the LOADED module's in-memory PE
// headers -- deliberately not hardcoded to 0x10000000.
static inline bool M2_ReadPreferredBase(uintptr_t modBase, uint32_t* outPref) {
    const unsigned char* m = (const unsigned char*)modBase;
    if (!m || m[0] != 'M' || m[1] != 'Z') return false;
    uint32_t lfanew;
    for (int i = 0; i < 4; ++i) ((unsigned char*)&lfanew)[i] = m[0x3C + i];
    if (lfanew < 0x40 || lfanew > 0x1000) return false;
    const unsigned char* nt = m + lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) return false;
    uint16_t magic = (uint16_t)(nt[24] | (nt[25] << 8));
    if (magic != 0x10B) return false;                       // PE32 only
    uint32_t imgBase;
    for (int i = 0; i < 4; ++i) ((unsigned char*)&imgBase)[i] = nt[24 + 28 + i];
    if (!imgBase) return false;
    *outPref = imgBase;
    return true;
}

// Byte-compare a region against the file image, skipping the 4 operand bytes of
// each known relocation slot and checking those dwords as fileVA+delta instead.
// On the FIRST mismatch fills: *outFirstBad (region offset), *outDumpStart,
// act16/exp16 (16 actual vs expected bytes; expected has delta applied).
static inline bool M2_VerifyRegionRel(uintptr_t addr, const unsigned char* expected, size_t len,
                                      const M2_RelocSlot* relocs, size_t nReloc,
                                      uintptr_t modBase,
                                      unsigned* outFirstBad, unsigned* outDumpStart,
                                      unsigned char* act16, unsigned char* exp16) {
    uint32_t pref = 0;
    if (!M2_ReadPreferredBase(modBase, &pref)) {
        if (outFirstBad) *outFirstBad = 0xFFFFFFFFu;        // unreadable PE header
        if (outDumpStart) *outDumpStart = 0;
        return false;
    }
    uint32_t delta = (uint32_t)((uint32_t)modBase - pref);
    const unsigned char* mem = (const unsigned char*)addr;
    unsigned bad = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        size_t k = nReloc;
        for (size_t r = 0; r < nReloc; ++r)
            if (i >= relocs[r].off && i < relocs[r].off + 4) { k = r; break; }
        if (k < nReloc) {
            if (i != relocs[k].off) continue;               // operand body: checked as dword
            uint32_t got, want = relocs[k].fileVA + delta;
            for (int b = 0; b < 4; ++b) ((unsigned char*)&got)[b] = mem[i + b];
            if (got != want) { bad = (unsigned)i; break; }
            continue;
        }
        if (mem[i] != expected[i]) { bad = (unsigned)i; break; }
    }
    if (bad == 0xFFFFFFFFu) return true;
    unsigned ds = (len >= 16) ? (unsigned)((bad + 16 <= len) ? bad : len - 16) : 0;
    if (outFirstBad) *outFirstBad = bad;
    if (outDumpStart) *outDumpStart = ds;
    if (act16) for (int j = 0; j < 16; ++j) act16[j] = mem[ds + j];
    if (exp16) {
        for (int j = 0; j < 16; ++j) {
            unsigned off = ds + j;
            size_t k = nReloc;
            for (size_t r = 0; r < nReloc; ++r)
                if (off >= relocs[r].off && off < relocs[r].off + 4) { k = r; break; }
            if (k < nReloc) {
                uint32_t v = relocs[k].fileVA + delta;
                exp16[j] = ((unsigned char*)&v)[off - relocs[k].off];
            } else {
                exp16[j] = expected[off];
            }
        }
    }
    return false;
}

// Plain byte compare (regions with no relocations).
static inline bool M2_VerifyRegion(uintptr_t addr, const unsigned char* expected, size_t len) {
    for (size_t i = 0; i < len; ++i) if (((const unsigned char*)addr)[i] != expected[i]) return false;
    return true;
}

// One-line diagnostic: region name+rva, first mismatch offset, 16 actual vs
// expected bytes, real scene.dll base. Written into the proxy log by the caller.
static inline void M2_FormatVerifyDiag(char* buf, size_t cap, const char* name, unsigned rva,
                                       unsigned firstBad, unsigned dumpStart, uintptr_t modBase,
                                       uint32_t prefBase,
                                       const unsigned char* act16, const unsigned char* exp16) {
    char a[33], e[33];
    for (int i = 0; i < 16; ++i) {
        a[i * 2] = "0123456789ABCDEF"[act16[i] >> 4];
        a[i * 2 + 1] = "0123456789ABCDEF"[act16[i] & 15];
        e[i * 2] = "0123456789ABCDEF"[exp16[i] >> 4];
        e[i * 2 + 1] = "0123456789ABCDEF"[exp16[i] & 15];
    }
    a[32] = e[32] = 0;
    if (firstBad == 0xFFFFFFFFu) {
        snprintf(buf, cap,
                 "pather_sse: verify_fail %s rva=0x%08X off=pe-header scene_base=0x%08llX",
                 name, rva, (unsigned long long)modBase);
        return;
    }
    snprintf(buf, cap,
             "pather_sse: verify_fail %s rva=0x%08X off=+0x%X(+0x%X) scene_base=0x%08llX pref=0x%08X actual=%s expected=%s",
             name, rva, firstBad, dumpStart, (unsigned long long)modBase, prefBase, a, e);
}
// ---- M2_VERIFY_CORE_END ----

static inline void M2_WriteJmp32(uintptr_t src, void* dst) {
    DWORD op; VirtualProtect((void*)src,5,PAGE_EXECUTE_READWRITE,&op);
    ((unsigned char*)src)[0]=0xE9;
    *(long*)((char*)src+1)=(long)((char*)dst-((char*)src+5));
    VirtualProtect((void*)src,5,op,&op);
    FlushInstructionCache(GetCurrentProcess(),(void*)src,5);
}

extern "C" __attribute__((naked)) void M2_Pather_L3(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
mov edx,[esp+4]
mov eax,[ecx+0x24]
cmp edx,eax
jne 1f
mov [ecx+0x28],edx
mov al,1
ret 4
1:
cvtss2sd xmm0,dword ptr [ecx+0x1C]
movss xmm1,dword ptr [edx+0x24]
cvtss2sd xmm1,xmm1
subsd xmm0,xmm1
cvtss2sd xmm2,dword ptr [ecx+0x20]
movss xmm3,dword ptr [edx+0x28]
cvtss2sd xmm3,xmm3
subsd xmm2,xmm3
mulsd xmm0,xmm0
mulsd xmm2,xmm2
addsd xmm0,xmm2
movss xmm1,dword ptr [ecx+0x2C]
cvtss2sd xmm1,xmm1
ucomisd xmm1,xmm0
jbe 2f
cvtsd2ss xmm2,xmm0
movss dword ptr [ecx+0x2C],xmm2
mov [ecx+0x28],edx
2:
mov eax,[ecx+0x18]
movss xmm4,dword ptr [eax+8]
cvtss2sd xmm4,xmm4
movss xmm5,dword ptr [edx+0x10]
cvtss2sd xmm5,xmm5
ucomisd xmm5,xmm4
ja 3f
xor al,al
ret 4
3:
mov al,1
ret 4
.att_syntax prefix
)asm");
}
extern "C" __attribute__((naked)) void M2_Pather_L2(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
push ebp
mov ebp,esp
push ebx
push esi
push edi
mov esi,ecx
mov edi,[ebp+8]
mov eax,[esi]
cmp edi,eax
jge l2exit
mov ebx,[esi+8]
l2loop:
mov ecx,edi
mov eax,edi
shl eax,1
inc eax
mov edx,eax
inc edx
cmp eax,[esi]
jge l2c2
movss xmm0,dword ptr [ebx+edi*8]
movss xmm1,dword ptr [ebx+eax*8]
ucomiss xmm0,xmm1
jbe l2c2
mov ecx,eax
l2c2:
mov eax,edi
shl eax,1
add eax,2
cmp eax,[esi]
jge l2cmp
movss xmm0,dword ptr [ebx+ecx*8]
movss xmm1,dword ptr [ebx+eax*8]
ucomiss xmm0,xmm1
jbe l2cmp
mov ecx,eax
l2cmp:
cmp ecx,edi
je l2exit
movq xmm0,qword ptr [ebx+edi*8]
movq xmm1,qword ptr [ebx+ecx*8]
movq qword ptr [ebx+edi*8],xmm1
movq qword ptr [ebx+ecx*8],xmm0
mov eax,[ebx+edi*8+4]
mov edx,[eax+0x18]
and edx,0xFFF00000
or edx,edi
mov [eax+0x18],edx
mov eax,[ebx+ecx*8+4]
mov edx,[eax+0x18]
and edx,0xFFF00000
or edx,ecx
mov [eax+0x18],edx
mov edi,ecx
jmp l2loop
l2exit:
pop edi
pop esi
pop ebx
pop ebp
ret 4
.att_syntax prefix
)asm");
}

// Register contract of the ORIGINAL slice exits (established by disassembly of
// scene.dll continuations 0xF6497 / 0xF67A6, W3e):
//   EAX  reset=gen<<22, fresh=profile(movsx), body=[esp+0x1C]/[esp+0x44]
//   ECX  reset=[0x101B2204](relocated), fresh=caller value, body=*([esp+0x44])
//   EDX  = [esp+0x18]  (set at 0xF63FB, both entries)
//   ESI, EDI, EBP, EBX = caller values (slice never writes them)
//   x87 stack = empty on both exits (0xF67A4's fstp st0 pops the original's
//   leftover g; the SSE2 replacement never pushes, so it must enter the skip
//   continuation at 0xF67A6, AFTER that pop, to keep the stack balanced).
// The replacement below reproduces every one of these exactly.
extern "C" __attribute__((naked)) void M2_Pather_L1_Normal(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
cvtss2sd xmm4,dword ptr [esp+0x24]
cvtss2sd xmm5,dword ptr [esp+0x28]
cvtss2sd xmm6,dword ptr [esp+0x2C]
cvtss2sd xmm7,dword ptr [esp+0x30]
mov edx,[esp+0x48]
movsx eax,byte ptr [edx+0x14]
mov edx,[esp+0x18]
movss xmm0,dword ptr [edx+0x10]
cvtss2sd xmm0,xmm0
movss xmm1,dword ptr [ebp+eax*4+8]
cvtss2sd xmm1,xmm1
addsd xmm0,xmm1
movss xmm2,dword ptr [ebx+0x10]
cvtss2sd xmm2,xmm2
ucomisd xmm2,xmm0
ja l1body
jmp l1skip
l1body:
movss xmm3,dword ptr [ebx+0x24]
cvtss2sd xmm3,xmm3
ucomisd xmm3,xmm4
jb l1skip
ucomisd xmm6,xmm3
jb l1skip
movss xmm3,dword ptr [ebx+0x28]
cvtss2sd xmm3,xmm3
ucomisd xmm3,xmm5
jb l1skip
ucomisd xmm7,xmm3
jb l1skip
cvtsd2ss xmm1,xmm0
movss dword ptr [ebx+0x10],xmm1
mov eax,[esp+0x44]
mov ecx,[eax]
mov [ebx+0x14],ecx
movss dword ptr [esp+0x14],xmm1
cmp byte ptr [esp+0x40],0
je l1done
movss xmm2,dword ptr [ebx+0x24]
cvtss2sd xmm2,xmm2
mov eax,[esp+0x1C]
movss xmm3,dword ptr [eax+0x24]
cvtss2sd xmm3,xmm3
subsd xmm2,xmm3
movss xmm3,dword ptr [ebx+0x28]
cvtss2sd xmm3,xmm3
mov eax,[esp+0x1C]
movss xmm4,dword ptr [eax+0x28]
cvtss2sd xmm4,xmm4
subsd xmm3,xmm4
mulsd xmm2,xmm2
mulsd xmm3,xmm3
addsd xmm2,xmm3
sqrtsd xmm2,xmm2
addsd xmm0,xmm2
cvtsd2ss xmm0,xmm0
movss dword ptr [esp+0x14],xmm0
l1done:
jmp dword ptr [_g_l1DoneTgt]
l1skip:
jmp dword ptr [_g_l1SkipTgt]
.att_syntax prefix
)asm");
}

// Reset entry (E9 at sceneBase+0xF63D4). Replaces, in order:
//   fld dword ptr [0x101269A4]  + fstp dword ptr [ebx+0x10]  -> [ebx+0x10] = 100000.0f
//     (constant verified in bin/scene.dll: 0x47C35000; relocated VA read via
//      g_sceneBase, so the constant store is also base-independent)
//   mov [ebx+0x18],0 ; mov eax,[eax+0x38] ; shl eax,0x16 ; mov [ebx+0x18],eax
//     -> flags = generation<<22 (bit 20 "in heap" cleared; the transient 0 is
//        overwritten before any read and is not part of the contract)
//   mov ecx,[0x101B2204] -> ecx = [g_sceneBase+0x1B2204] (relocated global)
// Exit state: EAX=gen<<22 (flags from the same shl), ECX=global, EDX/ESI/EDI/
// EBP/EBX and the x87 stack exactly as the original leaves them.
extern "C" __attribute__((naked)) void M2_Pather_L1_Reset(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
mov edx,[_g_sceneBase]
mov ecx,[edx+0x1B2204]
mov dword ptr [ebx+0x10],0x47C35000
mov eax,[eax+0x38]
shl eax,0x16
mov [ebx+0x18],eax
jmp _M2_Pather_L1_Normal
.att_syntax prefix
)asm");
}

static inline void M2_PatherSSE_Install(uintptr_t sceneBase, int mask, char* statusBuf,
                                        char* diagBuf, size_t diagCap) {
    if (!sceneBase) return;
    int okL3=0, okL2=0, okL1=0;
    // verify (L1 relocation-aware: two HIGHLOW slots inside the region)
    unsigned bad=0, ds=0; unsigned char act[16], exp[16];
    bool v3 = M2_VerifyRegionRel(sceneBase+0x0FFFB0, M2_ORIG_BYTES_L3, 78, 0, 0, sceneBase, &bad, &ds, act, exp);
    bool v2 = M2_VerifyRegionRel(sceneBase+0x0FBA50, M2_ORIG_BYTES_L2, 172, 0, 0, sceneBase, &bad, &ds, act, exp);
    uint32_t prefBase = 0;
    M2_ReadPreferredBase(sceneBase, &prefBase);
    bool v1 = M2_VerifyRegionRel(sceneBase+0x0F63D4, M2_ORIG_BYTES_L1, 196, M2_L1_RELOCS, 2, sceneBase, &bad, &ds, act, exp);
    if (diagBuf && diagCap) diagBuf[0] = 0;
    if ((mask & 4) && !v1 && diagBuf && diagCap) {
        M2_FormatVerifyDiag(diagBuf, diagCap, "L1", 0x0F63D4u, bad, ds, sceneBase, prefBase, act, exp);
    } else if ((mask & 2) && !v2 && diagBuf && diagCap) {
        M2_FormatVerifyDiag(diagBuf, diagCap, "L2", 0x0FBA50u, bad, ds, sceneBase, prefBase, act, exp);
    } else if ((mask & 1) && !v3 && diagBuf && diagCap) {
        M2_FormatVerifyDiag(diagBuf, diagCap, "L3", 0x0FFFB0u, bad, ds, sceneBase, prefBase, act, exp);
    }
    if ((mask & 1) && v3) {
        M2_WriteJmp32(sceneBase+0x0FFFB0, (void*)M2_Pather_L3); okL3=1;
    }
    if ((mask & 2) && v2) {
        M2_WriteJmp32(sceneBase+0x0FBA50, (void*)M2_Pather_L2); okL2=1;
    }
    if ((mask & 4) && v1) {
        g_l1DoneTgt = sceneBase+0x0F6497;
        g_l1SkipTgt = sceneBase+0x0F67A6;
        M2_WriteJmp32(sceneBase+0x0F63D4, (void*)M2_Pather_L1_Reset);
        M2_WriteJmp32(sceneBase+0x0F63D4+0x1F, (void*)M2_Pather_L1_Normal);
        okL1=1;
    }
    snprintf(statusBuf, 128, "pather_sse: mask=%d L3=%s L2=%s L1=%s", mask,
        (mask&1)?(okL3?"ok":"verify"):"off",
        (mask&2)?(okL2?"ok":"verify"):"off",
        (mask&4)?(okL1?"ok":"verify"):"off");
}
#endif
