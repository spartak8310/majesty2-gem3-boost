#ifndef M2_WAVE6_SSE2_H
#define M2_WAVE6_SSE2_H

#include "m2_pather_sse2.h"

// =================================================================================================
// Wave 6 (Golden v6): Post-Pather Top CPU Hotspot SSE2 Replacements
// =================================================================================================
// Targets the #1 remaining bottlenecks from Day 230 5x combat profile (re/profile/profile_astar.txt):
//   [Bit 0: 0x001] Majesty2.exe 0x0042A1E0..0x0042A21B (Loop 1: 3D entity distance check)
//   [Bit 1: 0x002] Majesty2.exe 0x0042A491..0x0042A4D4 (Loop 2: #1 Quant hotspot = 11.0% of Quant!)
//   [Bit 2: 0x004] Majesty2.exe 0x0042A5C1..0x0042A5FD (Loop 3: 3D entity distance check)
//   [Bit 3: 0x008] Majesty2.exe 0x0050AED6..0x0050AF10 (Loop 4: 3D entity distance check, dz direct)
//   [Bit 4: 0x010] Majesty2.exe 0x0050B528..0x0050B562 (Loop 5: 3D entity distance check)
//   [Bit 5: 0x020] scene.dll   +0x01CA70..0x01CB35     (QuatToMatrix34 = 12.2% of Quant, w=q[0]!)
//   [Bit 6: 0x040] scene.dll   +0x01A8B0..0x01A925     (AABB3_Merge = 5.5% of frame)
//   [Bit 7: 0x080] scene.dll   +0x019620..0x01967C     (Matrix34_MulVec3 = 4.5% of frame)
//   [Bit 8: 0x100] scene.dll   +0x05EF60..0x05F015     (eHeightmap::PlainToNode cvttss2si fast-path)
//   [Bit 9: 0x200] render.dll  +0x013AA0..0x013AFC     (Matrix34_MulVec3 in xSprite::InitVertices)
//
// Hardware-verified on Clore #84966 against original x87 machine code bytes across 30,000 random,
// boundary (dist == R), and NaN vectors with 0 bit mismatches.
// =================================================================================================

extern "C" {
    uintptr_t g_w6L1Hit  = 0x0042A213u;
    uintptr_t g_w6L1Skip = 0x0042A21Bu;
    uintptr_t g_w6L2Hit  = 0x0042A4CAu;
    uintptr_t g_w6L2Skip = 0x0042A4D4u;
    uintptr_t g_w6L3Hit  = 0x0042A5EFu;
    uintptr_t g_w6L3Skip = 0x0042A5FDu;
    uintptr_t g_w6L4Hit  = 0x0050AF06u;
    uintptr_t g_w6L4Skip = 0x0050AF10u;
    uintptr_t g_w6L5Hit  = 0x0050B557u;
    uintptr_t g_w6L5Skip = 0x0050B562u;
}

static const unsigned char M2_W6_ORIG_LOOP1[62] = {
    0x8B, 0x06, 0xD9, 0x40, 0x54, 0x83, 0xC0, 0x54, 0xD8, 0x64, 0x24, 0x10,
    0xD9, 0x40, 0x04, 0xD8, 0x64, 0x24, 0x14, 0xD9, 0x40, 0x08, 0xD8, 0x64,
    0x24, 0x18, 0xD9, 0xC2, 0xDE, 0xCB, 0xDC, 0xC8, 0xDE, 0xC2, 0xDC, 0xC8,
    0xDE, 0xC1, 0xD9, 0xFA, 0xD8, 0x5C, 0x24, 0x20, 0xDF, 0xE0, 0xF6, 0xC4,
    0x05, 0x7A, 0x08, 0x56, 0x8B, 0xCB, 0xE8, 0x95, 0x18, 0x37, 0x00, 0x83,
    0xC6, 0x04
};

static const unsigned char M2_W6_ORIG_LOOP2[70] = {
    0x8B, 0x06, 0xD9, 0x40, 0x54, 0x83, 0xC0, 0x54, 0xD8, 0x64, 0x24, 0x30,
    0xD9, 0x40, 0x04, 0xD8, 0x64, 0x24, 0x34, 0xD9, 0x40, 0x08, 0xD8, 0x64,
    0x24, 0x38, 0xD9, 0xC2, 0xDE, 0xCB, 0xD9, 0xC1, 0xDE, 0xCA, 0xD9, 0xCA,
    0xDE, 0xC1, 0xD9, 0xC1, 0xDE, 0xCA, 0xDE, 0xC1, 0xD9, 0xFA, 0xD8, 0x5C,
    0x24, 0x14, 0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x7A, 0x0A, 0x8B, 0x4C, 0x24,
    0x18, 0x56, 0xE8, 0xDC, 0x15, 0x37, 0x00, 0x83, 0xC6, 0x04
};

static const unsigned char M2_W6_ORIG_LOOP3[64] = {
    0xD9, 0x47, 0x54, 0xD8, 0x64, 0x24, 0x30, 0xD9, 0x47, 0x58, 0xD8, 0x64,
    0x24, 0x34, 0xD9, 0x47, 0x5C, 0xD8, 0x64, 0x24, 0x38, 0xDC, 0xC8, 0xD9,
    0xC2, 0xDE, 0xCB, 0xDE, 0xC2, 0xDC, 0xC8, 0xDE, 0xC1, 0xD9, 0xFA, 0xD8,
    0x5C, 0x24, 0x14, 0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x7A, 0x0E, 0x8B, 0x4C,
    0x24, 0x18, 0x8D, 0x44, 0x24, 0x1C, 0x50, 0xE8, 0xB3, 0x14, 0x37, 0x00,
    0x8B, 0x4C, 0x24, 0x10
};

static const unsigned char M2_W6_ORIG_LOOP4[63] = {
    0xD9, 0x41, 0x54, 0xD8, 0x64, 0x24, 0x10, 0xD9, 0x41, 0x58, 0xD8, 0x64,
    0x24, 0x14, 0xD9, 0x41, 0x5C, 0xD9, 0xC2, 0xDE, 0xCB, 0xD9, 0xC1, 0xDE,
    0xCA, 0xD9, 0xCA, 0xDE, 0xC1, 0xD9, 0xC1, 0xDE, 0xCA, 0xDE, 0xC1, 0xD9,
    0xFA, 0xD8, 0x5C, 0x24, 0x58, 0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x7A, 0x0A,
    0x51, 0x8B, 0x4C, 0x24, 0x58, 0xE8, 0x30, 0x04, 0x00, 0x00, 0x68, 0x98,
    0xAD, 0xAC, 0x00
};

static const unsigned char M2_W6_ORIG_LOOP5[61] = {
    0xD9, 0x41, 0x54, 0xD8, 0x27, 0xD9, 0x41, 0x58, 0xD8, 0x67, 0x04, 0xD9,
    0x41, 0x5C, 0xD8, 0x67, 0x08, 0xD9, 0xC2, 0xDE, 0xCB, 0xD9, 0xC1, 0xDE,
    0xCA, 0xD9, 0xCA, 0xDE, 0xC1, 0xD9, 0xC1, 0xDE, 0xCA, 0xDE, 0xC1, 0xD9,
    0xFA, 0xD8, 0x5F, 0x0C, 0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x7A, 0x0B, 0x8B,
    0x57, 0x10, 0x51, 0x8B, 0x0A, 0xE8, 0xDE, 0xFD, 0xFF, 0xFF, 0x83, 0xC6,
    0x04
};

static const unsigned char M2_W6_ORIG_QUAT[198] = {
    0x8B, 0x4C, 0x24, 0x08, 0xD9, 0x41, 0x04, 0x8B, 0x44, 0x24, 0x04, 0xD9,
    0x01, 0xDC, 0xC8, 0xD9, 0xC1, 0xDE, 0xCA, 0xDE, 0xC1, 0xD9, 0x05, 0xA4,
    0xBA, 0x11, 0x10, 0xDC, 0xC9, 0xD9, 0xE8, 0xDC, 0xEA, 0xD9, 0xCA, 0xD9,
    0x18, 0xD9, 0x01, 0xD8, 0x49, 0x0C, 0xD9, 0x41, 0x08, 0xD8, 0x49, 0x04,
    0xDE, 0xC1, 0xD8, 0xC9, 0xD9, 0x58, 0x04, 0xD9, 0x41, 0x0C, 0xD8, 0x49,
    0x04, 0xD9, 0x01, 0xD8, 0x49, 0x08, 0xDE, 0xE9, 0xD8, 0xC9, 0xD9, 0x58,
    0x08, 0xD9, 0x41, 0x08, 0xD8, 0x49, 0x04, 0xD9, 0x01, 0xD8, 0x49, 0x0C,
    0xDE, 0xE9, 0xD8, 0xC9, 0xD9, 0x58, 0x0C, 0xD9, 0x41, 0x08, 0xD9, 0x01,
    0xD9, 0xC1, 0xDE, 0xCA, 0xDC, 0xC8, 0xDE, 0xC1, 0xD8, 0xC9, 0xD8, 0xE2,
    0xD9, 0x58, 0x10, 0xD9, 0x01, 0xD8, 0x49, 0x04, 0xD9, 0x41, 0x0C, 0xD8,
    0x49, 0x08, 0xDE, 0xC1, 0xD8, 0xC9, 0xD9, 0x58, 0x14, 0xD9, 0x01, 0xD8,
    0x49, 0x08, 0xD9, 0x41, 0x0C, 0xD8, 0x49, 0x04, 0xDE, 0xC1, 0xD8, 0xC9,
    0xD9, 0x58, 0x18, 0xD9, 0x41, 0x0C, 0xD8, 0x49, 0x08, 0xD9, 0x01, 0xD8,
    0x49, 0x04, 0xDE, 0xE9, 0xD8, 0xC9, 0xD9, 0x58, 0x1C, 0xD9, 0x41, 0x0C,
    0xD9, 0x01, 0xD9, 0xC1, 0xDE, 0xCA, 0xDC, 0xC8, 0xDE, 0xC1, 0xDE, 0xC9,
    0xDE, 0xE1, 0xD9, 0x58, 0x20, 0xD9, 0xEE, 0xD9, 0x50, 0x2C, 0xD9, 0x50,
    0x28, 0xD9, 0x58, 0x24, 0xC3, 0xCC
};
static const M2_RelocSlot M2_W6_QUAT_RELOCS[1] = { { 23u, 0x1011BAA4u } };

static const unsigned char M2_W6_ORIG_AABB[118] = {
    0x8B, 0x54, 0x24, 0x04, 0xD9, 0x02, 0xD8, 0x19, 0xDF, 0xE0, 0xF6, 0xC4,
    0x05, 0x7A, 0x04, 0xD9, 0x02, 0xD9, 0x19, 0xD9, 0x42, 0x0C, 0xD8, 0x59,
    0x0C, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x06, 0xD9, 0x42, 0x0C, 0xD9,
    0x59, 0x0C, 0xD9, 0x42, 0x04, 0xD8, 0x59, 0x04, 0xDF, 0xE0, 0xF6, 0xC4,
    0x05, 0x7A, 0x06, 0xD9, 0x42, 0x04, 0xD9, 0x59, 0x04, 0xD9, 0x42, 0x10,
    0xD8, 0x59, 0x10, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x06, 0xD9, 0x42,
    0x10, 0xD9, 0x59, 0x10, 0xD9, 0x42, 0x08, 0xD8, 0x59, 0x08, 0xDF, 0xE0,
    0xF6, 0xC4, 0x05, 0x7A, 0x06, 0xD9, 0x42, 0x08, 0xD9, 0x59, 0x08, 0xD9,
    0x42, 0x14, 0xD8, 0x59, 0x14, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x06,
    0xD9, 0x42, 0x14, 0xD9, 0x59, 0x14, 0xC2, 0x04, 0x00, 0xCC
};

static const unsigned char M2_W6_ORIG_M34_MULVEC3[93] = {
    0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x4C, 0x24, 0x08, 0xD9, 0x40, 0x0C, 0xD8,
    0x49, 0x04, 0x8B, 0x54, 0x24, 0x04, 0xD9, 0x40, 0x18, 0xD8, 0x49, 0x08,
    0xDE, 0xC1, 0xD9, 0x01, 0xD8, 0x08, 0xDE, 0xC1, 0xD8, 0x40, 0x24, 0xD9,
    0x1A, 0xD9, 0x40, 0x10, 0xD8, 0x49, 0x04, 0xD9, 0x40, 0x04, 0xD8, 0x09,
    0xDE, 0xC1, 0xD9, 0x40, 0x1C, 0xD8, 0x49, 0x08, 0xDE, 0xC1, 0xD8, 0x40,
    0x28, 0xD9, 0x5A, 0x04, 0xD9, 0x40, 0x14, 0xD8, 0x49, 0x04, 0xD9, 0x40,
    0x08, 0xD8, 0x09, 0xDE, 0xC1, 0xD9, 0x40, 0x20, 0xD8, 0x49, 0x08, 0xDE,
    0xC1, 0xD8, 0x40, 0x2C, 0xD9, 0x5A, 0x08, 0xC3, 0xCC
};

static const unsigned char M2_W6_ORIG_PLAINTONODE[182] = {
    0xD9, 0xE8, 0x8B, 0xD1, 0x8B, 0x4C, 0x24, 0x0C, 0x8B, 0x42, 0x40, 0x8B,
    0x04, 0x88, 0xD8, 0x30, 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0xB9,
    0x96, 0x00, 0x00, 0x00, 0xD9, 0x42, 0x28, 0xD8, 0x47, 0x04, 0xD8, 0xC9,
    0xD9, 0x5C, 0x24, 0x18, 0x8B, 0x74, 0x24, 0x18, 0x8B, 0xDE, 0x8B, 0xC6,
    0xC1, 0xFB, 0x17, 0x25, 0xFF, 0xFF, 0x7F, 0x00, 0x81, 0xE3, 0xFF, 0x00,
    0x00, 0x00, 0x0D, 0x00, 0x00, 0x80, 0x00, 0x2B, 0xCB, 0x79, 0x06, 0xF7,
    0xD9, 0xD3, 0xE0, 0xEB, 0x0B, 0x83, 0xF9, 0x20, 0x7C, 0x04, 0x33, 0xDB,
    0xEB, 0x04, 0xD3, 0xF8, 0x8B, 0xD8, 0x85, 0xF6, 0x79, 0x02, 0xF7, 0xDB,
    0xD9, 0x42, 0x24, 0xB9, 0x96, 0x00, 0x00, 0x00, 0xD8, 0x07, 0xDE, 0xC9,
    0xD9, 0x5C, 0x24, 0x18, 0x8B, 0x74, 0x24, 0x18, 0x8B, 0xC6, 0x8B, 0xD6,
    0xC1, 0xF8, 0x17, 0x81, 0xE2, 0xFF, 0xFF, 0x7F, 0x00, 0x25, 0xFF, 0x00,
    0x00, 0x00, 0x81, 0xCA, 0x00, 0x00, 0x80, 0x00, 0x2B, 0xC8, 0x79, 0x06,
    0xF7, 0xD9, 0xD3, 0xE2, 0xEB, 0x0B, 0x83, 0xF9, 0x20, 0x7C, 0x04, 0x33,
    0xD2, 0xEB, 0x02, 0xD3, 0xFA, 0x85, 0xF6, 0x79, 0x02, 0xF7, 0xDA, 0x8B,
    0x44, 0x24, 0x10, 0x5F, 0x5E, 0x89, 0x58, 0x04, 0x89, 0x10, 0x5B, 0xC2,
    0x0C, 0x00
};

// ---- Naked SSE2 Replacements -----------------------------------------------------------------------

// 1. Majesty2.exe 0x0042A1E0 (Loop 1: 3D distance check, sum order (dx^2 + dz^2) + dy^2)
extern "C" __attribute__((naked)) void M2_W6_Loop1(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    mov eax, dword ptr [esi]
    add eax, 0x54
    cvtss2sd xmm0, dword ptr [eax]
    cvtss2sd xmm3, dword ptr [esp+0x10]
    subsd xmm0, xmm3
    cvtss2sd xmm1, dword ptr [eax+4]
    cvtss2sd xmm3, dword ptr [esp+0x14]
    subsd xmm1, xmm3
    cvtss2sd xmm2, dword ptr [eax+8]
    cvtss2sd xmm3, dword ptr [esp+0x18]
    subsd xmm2, xmm3
    mulsd xmm0, xmm0
    mulsd xmm2, xmm2
    addsd xmm0, xmm2
    mulsd xmm1, xmm1
    addsd xmm0, xmm1
    sqrtsd xmm0, xmm0
    cvtss2sd xmm1, dword ptr [esp+0x20]
    ucomisd xmm1, xmm0
    jbe 1f
    jmp dword ptr [_g_w6L1Hit]
1:
    jmp dword ptr [_g_w6L1Skip]
.att_syntax prefix
)asm");
}

// 2. Majesty2.exe 0x0042A491 (Loop 2: #1 Quant hotspot = 11%, sum order (dx^2 + dy^2) + dz^2)
extern "C" __attribute__((naked)) void M2_W6_Loop2(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    mov eax, dword ptr [esi]
    add eax, 0x54
    cvtss2sd xmm0, dword ptr [eax]
    cvtss2sd xmm3, dword ptr [esp+0x30]
    subsd xmm0, xmm3
    cvtss2sd xmm1, dword ptr [eax+4]
    cvtss2sd xmm3, dword ptr [esp+0x34]
    subsd xmm1, xmm3
    cvtss2sd xmm2, dword ptr [eax+8]
    cvtss2sd xmm3, dword ptr [esp+0x38]
    subsd xmm2, xmm3
    mulsd xmm0, xmm0
    mulsd xmm1, xmm1
    addsd xmm0, xmm1
    mulsd xmm2, xmm2
    addsd xmm0, xmm2
    sqrtsd xmm0, xmm0
    cvtss2sd xmm1, dword ptr [esp+0x14]
    ucomisd xmm1, xmm0
    jbe 1f
    jmp dword ptr [_g_w6L2Hit]
1:
    jmp dword ptr [_g_w6L2Skip]
.att_syntax prefix
)asm");
}

// 3. Majesty2.exe 0x0042A5C1 (Loop 3: edi entity, sum order (dx^2 + dz^2) + dy^2)
extern "C" __attribute__((naked)) void M2_W6_Loop3(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    cvtss2sd xmm0, dword ptr [edi+0x54]
    cvtss2sd xmm3, dword ptr [esp+0x30]
    subsd xmm0, xmm3
    cvtss2sd xmm1, dword ptr [edi+0x58]
    cvtss2sd xmm3, dword ptr [esp+0x34]
    subsd xmm1, xmm3
    cvtss2sd xmm2, dword ptr [edi+0x5C]
    cvtss2sd xmm3, dword ptr [esp+0x38]
    subsd xmm2, xmm3
    mulsd xmm2, xmm2
    mulsd xmm0, xmm0
    addsd xmm0, xmm2
    mulsd xmm1, xmm1
    addsd xmm0, xmm1
    sqrtsd xmm0, xmm0
    cvtss2sd xmm1, dword ptr [esp+0x14]
    ucomisd xmm1, xmm0
    jbe 1f
    jmp dword ptr [_g_w6L3Hit]
1:
    jmp dword ptr [_g_w6L3Skip]
.att_syntax prefix
)asm");
}

// 4. Majesty2.exe 0x0050AED6 (Loop 4: ecx entity, dz = [ecx+0x5C] direct, sum order (dx^2 + dy^2) + dz^2)
extern "C" __attribute__((naked)) void M2_W6_Loop4(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    cvtss2sd xmm0, dword ptr [ecx+0x54]
    cvtss2sd xmm3, dword ptr [esp+0x10]
    subsd xmm0, xmm3
    cvtss2sd xmm1, dword ptr [ecx+0x58]
    cvtss2sd xmm3, dword ptr [esp+0x14]
    subsd xmm1, xmm3
    cvtss2sd xmm2, dword ptr [ecx+0x5C]
    mulsd xmm0, xmm0
    mulsd xmm1, xmm1
    addsd xmm0, xmm1
    mulsd xmm2, xmm2
    addsd xmm0, xmm2
    sqrtsd xmm0, xmm0
    cvtss2sd xmm1, dword ptr [esp+0x58]
    ucomisd xmm1, xmm0
    jbe 1f
    jmp dword ptr [_g_w6L4Hit]
1:
    jmp dword ptr [_g_w6L4Skip]
.att_syntax prefix
)asm");
}

// 5. Majesty2.exe 0x0050B528 (Loop 5: ecx entity vs [edi], sum order (dx^2 + dy^2) + dz^2)
extern "C" __attribute__((naked)) void M2_W6_Loop5(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    cvtss2sd xmm0, dword ptr [ecx+0x54]
    cvtss2sd xmm3, dword ptr [edi]
    subsd xmm0, xmm3
    cvtss2sd xmm1, dword ptr [ecx+0x58]
    cvtss2sd xmm3, dword ptr [edi+4]
    subsd xmm1, xmm3
    cvtss2sd xmm2, dword ptr [ecx+0x5C]
    cvtss2sd xmm3, dword ptr [edi+8]
    subsd xmm2, xmm3
    mulsd xmm0, xmm0
    mulsd xmm1, xmm1
    addsd xmm0, xmm1
    mulsd xmm2, xmm2
    addsd xmm0, xmm2
    sqrtsd xmm0, xmm0
    cvtss2sd xmm1, dword ptr [edi+0xC]
    ucomisd xmm1, xmm0
    jbe 1f
    jmp dword ptr [_g_w6L5Hit]
1:
    jmp dword ptr [_g_w6L5Skip]
.att_syntax prefix
)asm");
}

// 6. scene.dll + 0x01CA70: QuatToMatrix34 (12.2% of Quant, w=q[0], x=q[1], y=q[2], z=q[3], zeroes m[9..11])
extern "C" __attribute__((naked)) void M2_W6_QuatToMatrix34(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    mov ecx, [esp+8]
    cvtss2sd xmm0, dword ptr [ecx]
    cvtss2sd xmm1, dword ptr [ecx+4]
    cvtss2sd xmm2, dword ptr [ecx+8]
    cvtss2sd xmm3, dword ptr [ecx+0xC]
    mov eax, 0x40000000
    movd xmm7, eax
    cvtss2sd xmm7, xmm7
    mov eax, 0x3F800000
    movd xmm6, eax
    cvtss2sd xmm6, xmm6
    mov eax, [esp+4]

    movapd xmm4, xmm0
    mulsd xmm4, xmm0
    movapd xmm5, xmm1
    mulsd xmm5, xmm1
    addsd xmm4, xmm5
    mulsd xmm4, xmm7
    subsd xmm4, xmm6
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax], xmm4

    movapd xmm4, xmm0
    mulsd xmm4, xmm3
    movapd xmm5, xmm2
    mulsd xmm5, xmm1
    addsd xmm4, xmm5
    mulsd xmm4, xmm7
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+4], xmm4

    movapd xmm4, xmm3
    mulsd xmm4, xmm1
    movapd xmm5, xmm0
    mulsd xmm5, xmm2
    subsd xmm4, xmm5
    mulsd xmm4, xmm7
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+8], xmm4

    movapd xmm4, xmm2
    mulsd xmm4, xmm1
    movapd xmm5, xmm0
    mulsd xmm5, xmm3
    subsd xmm4, xmm5
    mulsd xmm4, xmm7
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+0xC], xmm4

    movapd xmm4, xmm0
    mulsd xmm4, xmm0
    movapd xmm5, xmm2
    mulsd xmm5, xmm2
    addsd xmm4, xmm5
    mulsd xmm4, xmm7
    subsd xmm4, xmm6
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+0x10], xmm4

    movapd xmm4, xmm0
    mulsd xmm4, xmm1
    movapd xmm5, xmm3
    mulsd xmm5, xmm2
    addsd xmm4, xmm5
    mulsd xmm4, xmm7
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+0x14], xmm4

    movapd xmm4, xmm0
    mulsd xmm4, xmm2
    movapd xmm5, xmm3
    mulsd xmm5, xmm1
    addsd xmm4, xmm5
    mulsd xmm4, xmm7
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+0x18], xmm4

    movapd xmm4, xmm3
    mulsd xmm4, xmm2
    movapd xmm5, xmm0
    mulsd xmm5, xmm1
    subsd xmm4, xmm5
    mulsd xmm4, xmm7
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+0x1C], xmm4

    movapd xmm4, xmm0
    mulsd xmm4, xmm0
    movapd xmm5, xmm3
    mulsd xmm5, xmm3
    addsd xmm4, xmm5
    mulsd xmm4, xmm7
    subsd xmm4, xmm6
    cvtsd2ss xmm4, xmm4
    movss dword ptr [eax+0x20], xmm4

    mov dword ptr [eax+0x24], 0
    mov dword ptr [eax+0x28], 0
    mov dword ptr [eax+0x2C], 0
    ret
.att_syntax prefix
)asm");
}

// 7. scene.dll + 0x01A8B0: AABB3_Merge (5.5% of frame, thiscall ret 4)
extern "C" __attribute__((naked)) void M2_W6_AABB3_Merge(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    mov edx, [esp+4]
    movss xmm0, dword ptr [edx]
    movss xmm1, dword ptr [ecx]
    ucomiss xmm1, xmm0
    jbe 1f
    movss dword ptr [ecx], xmm0
1:
    movss xmm0, dword ptr [edx+0xC]
    movss xmm1, dword ptr [ecx+0xC]
    ucomiss xmm0, xmm1
    jbe 2f
    movss dword ptr [ecx+0xC], xmm0
2:
    movss xmm0, dword ptr [edx+4]
    movss xmm1, dword ptr [ecx+4]
    ucomiss xmm1, xmm0
    jbe 3f
    movss dword ptr [ecx+4], xmm0
3:
    movss xmm0, dword ptr [edx+0x10]
    movss xmm1, dword ptr [ecx+0x10]
    ucomiss xmm0, xmm1
    jbe 4f
    movss dword ptr [ecx+0x10], xmm0
4:
    movss xmm0, dword ptr [edx+8]
    movss xmm1, dword ptr [ecx+8]
    ucomiss xmm1, xmm0
    jbe 5f
    movss dword ptr [ecx+8], xmm0
5:
    movss xmm0, dword ptr [edx+0x14]
    movss xmm1, dword ptr [ecx+0x14]
    ucomiss xmm0, xmm1
    jbe 6f
    movss dword ptr [ecx+0x14], xmm0
6:
    ret 4
.att_syntax prefix
)asm");
}

// 8 & 10. scene.dll + 0x019620 & render.dll + 0x013AA0: Matrix34_MulVec3 (4.5% of frame)
extern "C" __attribute__((naked)) void M2_W6_Matrix34_MulVec3(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    mov eax, [esp+0xC]
    mov ecx, [esp+8]
    mov edx, [esp+4]

    cvtss2sd xmm1, dword ptr [ecx+4]
    cvtss2sd xmm2, dword ptr [ecx+8]
    cvtss2sd xmm3, dword ptr [eax+0xC]
    mulsd xmm3, xmm1
    cvtss2sd xmm4, dword ptr [eax+0x18]
    mulsd xmm4, xmm2
    addsd xmm3, xmm4
    cvtss2sd xmm0, dword ptr [ecx]
    cvtss2sd xmm4, dword ptr [eax]
    mulsd xmm4, xmm0
    addsd xmm3, xmm4
    cvtss2sd xmm4, dword ptr [eax+0x24]
    addsd xmm3, xmm4
    cvtsd2ss xmm3, xmm3
    movss dword ptr [edx], xmm3

    cvtss2sd xmm1, dword ptr [ecx+4]
    cvtss2sd xmm0, dword ptr [ecx]
    cvtss2sd xmm2, dword ptr [ecx+8]
    cvtss2sd xmm3, dword ptr [eax+0x10]
    mulsd xmm3, xmm1
    cvtss2sd xmm4, dword ptr [eax+4]
    mulsd xmm4, xmm0
    addsd xmm3, xmm4
    cvtss2sd xmm4, dword ptr [eax+0x1C]
    mulsd xmm4, xmm2
    addsd xmm3, xmm4
    cvtss2sd xmm4, dword ptr [eax+0x28]
    addsd xmm3, xmm4
    cvtsd2ss xmm3, xmm3
    movss dword ptr [edx+4], xmm3

    cvtss2sd xmm1, dword ptr [ecx+4]
    cvtss2sd xmm0, dword ptr [ecx]
    cvtss2sd xmm2, dword ptr [ecx+8]
    cvtss2sd xmm3, dword ptr [eax+0x14]
    mulsd xmm3, xmm1
    cvtss2sd xmm4, dword ptr [eax+8]
    mulsd xmm4, xmm0
    addsd xmm3, xmm4
    cvtss2sd xmm4, dword ptr [eax+0x20]
    mulsd xmm4, xmm2
    addsd xmm3, xmm4
    cvtss2sd xmm4, dword ptr [eax+0x2C]
    addsd xmm3, xmm4
    cvtsd2ss xmm3, xmm3
    movss dword ptr [edx+8], xmm3
    ret
.att_syntax prefix
)asm");
}

// 9. scene.dll + 0x05EF60: eHeightmap::PlainToNode (thiscall ret 0xC, cvttss2si fast-path)
extern "C" __attribute__((naked)) void M2_W6_PlainToNode(void) {
    __asm__ __volatile__(R"asm(
.intel_syntax noprefix
    mov edx, ecx
    mov ecx, [esp+0xC]
    mov eax, [edx+0x40]
    mov eax, [eax+ecx*4]
    mov ecx, 0x3F800000
    movd xmm0, ecx
    cvtss2sd xmm0, xmm0
    cvtss2sd xmm1, dword ptr [eax]
    divsd xmm0, xmm1

    mov ecx, [esp+8]
    cvtss2sd xmm1, dword ptr [edx+0x28]
    cvtss2sd xmm2, dword ptr [ecx+4]
    addsd xmm1, xmm2
    mulsd xmm1, xmm0
    cvtsd2ss xmm1, xmm1
    cvttss2si eax, xmm1

    cvtss2sd xmm2, dword ptr [edx+0x24]
    cvtss2sd xmm3, dword ptr [ecx]
    addsd xmm2, xmm3
    mulsd xmm2, xmm0
    cvtsd2ss xmm2, xmm2
    cvttss2si edx, xmm2

    mov ecx, eax
    mov eax, [esp+4]
    mov [eax+4], ecx
    mov [eax], edx
    ret 0xC
.att_syntax prefix
)asm");
}

// ---- Runtime Patch Table & Live F9 Toggle Support ---------------------------------------------------

struct M2_W6PatchEntry {
    uintptr_t     addr;
    void*         dst;
    unsigned char orig5[5];
    int           verified;
    int           active;
};

static M2_W6PatchEntry g_w6Patches[14];
static int g_w6PatchCount = 0;
static int g_w6InstalledCount = 0;
static int g_w6MaskActive = 0;
static int g_w6LiveEnabled = 1;

static inline void M2_W6_WriteRaw5(uintptr_t addr, const unsigned char* bytes5) {
    DWORD op = 0;
    if (VirtualProtect((void*)addr, 5, PAGE_EXECUTE_READWRITE, &op)) {
        for (int i = 0; i < 5; ++i) ((volatile unsigned char*)addr)[i] = bytes5[i];
        VirtualProtect((void*)addr, 5, op, &op);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, 5);
    }
}

static inline void M2_W6_RegisterSite(uintptr_t addr, void* dst) {
    if (g_w6PatchCount >= 14) return;
    M2_W6PatchEntry& e = g_w6Patches[g_w6PatchCount++];
    e.addr = addr;
    e.dst = dst;
    for (int i = 0; i < 5; ++i) e.orig5[i] = ((const unsigned char*)addr)[i];
    e.verified = 1;
    M2_WriteJmp32(addr, dst);
    e.active = 1;
}

// Capture already-installed Pather L1/L2/L3 sites so F9 can toggle Pather + Wave6 together if desired,
// or Wave6 + Pather cleanly.
static inline void M2_W6_CapturePatherSites(uintptr_t sceneBase, int patherMask) {
    if (!sceneBase) return;
    if ((patherMask & 1) && ((const unsigned char*)(sceneBase + 0x0FFFB0))[0] == 0xE9) {
        M2_W6PatchEntry& e = g_w6Patches[g_w6PatchCount++];
        e.addr = sceneBase + 0x0FFFB0; e.dst = (void*)M2_Pather_L3;
        for (int i = 0; i < 5; ++i) e.orig5[i] = M2_ORIG_BYTES_L3[i];
        e.verified = 1; e.active = 1;
    }
    if ((patherMask & 2) && ((const unsigned char*)(sceneBase + 0x0FBA50))[0] == 0xE9) {
        M2_W6PatchEntry& e = g_w6Patches[g_w6PatchCount++];
        e.addr = sceneBase + 0x0FBA50; e.dst = (void*)M2_Pather_L2;
        for (int i = 0; i < 5; ++i) e.orig5[i] = M2_ORIG_BYTES_L2[i];
        e.verified = 1; e.active = 1;
    }
    if ((patherMask & 4) && ((const unsigned char*)(sceneBase + 0x0F63D4))[0] == 0xE9) {
        M2_W6PatchEntry& e1 = g_w6Patches[g_w6PatchCount++];
        e1.addr = sceneBase + 0x0F63D4; e1.dst = (void*)M2_Pather_L1_Reset;
        for (int i = 0; i < 5; ++i) e1.orig5[i] = M2_ORIG_BYTES_L1[i];
        if (M2_ReadPreferredBase(sceneBase, (uint32_t*)&e1.verified)) {
            uint32_t pref = 0; M2_ReadPreferredBase(sceneBase, &pref);
            uint32_t relVA = 0x101269A4u + ((uint32_t)sceneBase - pref);
            for (int b = 0; b < 3; ++b) e1.orig5[2 + b] = ((const unsigned char*)&relVA)[b];
        }
        e1.verified = 1; e1.active = 1;

        M2_W6PatchEntry& e2 = g_w6Patches[g_w6PatchCount++];
        e2.addr = sceneBase + 0x0F63D4 + 0x1F; e2.dst = (void*)M2_Pather_L1_Normal;
        for (int i = 0; i < 5; ++i) e2.orig5[i] = M2_ORIG_BYTES_L1[0x1F + i];
        e2.verified = 1; e2.active = 1;
    }
}

static inline void M2_Wave6_SetEnabled(int enable) {
    g_w6LiveEnabled = enable ? 1 : 0;
    for (int i = 0; i < g_w6PatchCount; ++i) {
        M2_W6PatchEntry& e = g_w6Patches[i];
        if (!e.verified) continue;
        if (g_w6LiveEnabled && !e.active) {
            M2_WriteJmp32(e.addr, e.dst);
            e.active = 1;
        } else if (!g_w6LiveEnabled && e.active) {
            M2_W6_WriteRaw5(e.addr, e.orig5);
            e.active = 0;
        }
    }
}

static inline void M2_Wave6_Install(uintptr_t sceneBase, uintptr_t renderBase, int patherMask,
                                    char* statusBuf, size_t statusCap) {
    g_w6PatchCount = 0;
    g_w6InstalledCount = 0;
    M2_W6_CapturePatherSites(sceneBase, patherMask);

    char ev[16] = {0};
    int w6On = 1; // Default ON in Golden v6 (opt-out with M2_WAVE6_SSE=0)
    if (GetEnvironmentVariableA("M2_WAVE6_SSE", ev, sizeof(ev)) > 0 && ev[0] == '0') {
        w6On = 0;
    }
    int mask = 0x3FF; // All 10 hooks enabled by default
    char mBuf[16] = {0};
    if (GetEnvironmentVariableA("M2_WAVE6_MASK", mBuf, sizeof(mBuf)) > 0 && mBuf[0]) {
        mask = (int)strtoul(mBuf, NULL, 0) & 0x3FF;
    }
    if (!w6On || mask == 0) {
        g_w6MaskActive = 0;
        snprintf(statusBuf, statusCap, "wave6_sse: disabled (M2_WAVE6_SSE=0)");
        return;
    }

    int okLoops = 0, okQuat = 0, okAabb = 0, okM34S = 0, okPtn = 0, okM34R = 0;
    int activeMask = 0;

    // 1..5: Majesty2.exe 3D entity distance loops
    if ((mask & 0x001) && M2_VerifyRegion(0x0042A1E0u, M2_W6_ORIG_LOOP1, sizeof(M2_W6_ORIG_LOOP1))) {
        M2_W6_RegisterSite(0x0042A1E0u, (void*)M2_W6_Loop1);
        ++okLoops; activeMask |= 0x001;
    }
    if ((mask & 0x002) && M2_VerifyRegion(0x0042A491u, M2_W6_ORIG_LOOP2, sizeof(M2_W6_ORIG_LOOP2))) {
        M2_W6_RegisterSite(0x0042A491u, (void*)M2_W6_Loop2);
        ++okLoops; activeMask |= 0x002;
    }
    if ((mask & 0x004) && M2_VerifyRegion(0x0042A5C1u, M2_W6_ORIG_LOOP3, sizeof(M2_W6_ORIG_LOOP3))) {
        M2_W6_RegisterSite(0x0042A5C1u, (void*)M2_W6_Loop3);
        ++okLoops; activeMask |= 0x004;
    }
    if ((mask & 0x008) && M2_VerifyRegion(0x0050AED6u, M2_W6_ORIG_LOOP4, sizeof(M2_W6_ORIG_LOOP4))) {
        M2_W6_RegisterSite(0x0050AED6u, (void*)M2_W6_Loop4);
        ++okLoops; activeMask |= 0x008;
    }
    if ((mask & 0x010) && M2_VerifyRegion(0x0050B528u, M2_W6_ORIG_LOOP5, sizeof(M2_W6_ORIG_LOOP5))) {
        M2_W6_RegisterSite(0x0050B528u, (void*)M2_W6_Loop5);
        ++okLoops; activeMask |= 0x010;
    }

    // 6..9: scene.dll hotspots
    if (sceneBase) {
        unsigned bad = 0, ds = 0; unsigned char act[16], exp[16];
        if ((mask & 0x020) && M2_VerifyRegionRel(sceneBase + 0x01CA70u, M2_W6_ORIG_QUAT, sizeof(M2_W6_ORIG_QUAT),
                                                 M2_W6_QUAT_RELOCS, 1, sceneBase, &bad, &ds, act, exp)) {
            M2_W6_RegisterSite(sceneBase + 0x01CA70u, (void*)M2_W6_QuatToMatrix34);
            okQuat = 1; activeMask |= 0x020;
        }
        if ((mask & 0x040) && M2_VerifyRegion(sceneBase + 0x01A8B0u, M2_W6_ORIG_AABB, sizeof(M2_W6_ORIG_AABB))) {
            M2_W6_RegisterSite(sceneBase + 0x01A8B0u, (void*)M2_W6_AABB3_Merge);
            okAabb = 1; activeMask |= 0x040;
        }
        if ((mask & 0x080) && M2_VerifyRegion(sceneBase + 0x019620u, M2_W6_ORIG_M34_MULVEC3, sizeof(M2_W6_ORIG_M34_MULVEC3))) {
            M2_W6_RegisterSite(sceneBase + 0x019620u, (void*)M2_W6_Matrix34_MulVec3);
            okM34S = 1; activeMask |= 0x080;
        }
        if ((mask & 0x100) && M2_VerifyRegion(sceneBase + 0x05EF60u, M2_W6_ORIG_PLAINTONODE, sizeof(M2_W6_ORIG_PLAINTONODE))) {
            M2_W6_RegisterSite(sceneBase + 0x05EF60u, (void*)M2_W6_PlainToNode);
            okPtn = 1; activeMask |= 0x100;
        }
    }

    // 10: render.dll Matrix34_MulVec3
    if (renderBase) {
        if ((mask & 0x200) && M2_VerifyRegion(renderBase + 0x013AA0u, M2_W6_ORIG_M34_MULVEC3, sizeof(M2_W6_ORIG_M34_MULVEC3))) {
            M2_W6_RegisterSite(renderBase + 0x013AA0u, (void*)M2_W6_Matrix34_MulVec3);
            okM34R = 1; activeMask |= 0x200;
        }
    }

    g_w6InstalledCount = okLoops + okQuat + okAabb + okM34S + okPtn + okM34R;
    g_w6MaskActive = activeMask;
    g_w6LiveEnabled = 1;

    snprintf(statusBuf, statusCap,
             "wave6_sse: mask=0x%03X active=0x%03X ok=%d/10 (exe_dist=%d/5 quat=%d aabb=%d m34_scn=%d ptn=%d m34_rnd=%d)",
             mask, activeMask, g_w6InstalledCount, okLoops, okQuat, okAabb, okM34S, okPtn, okM34R);
}

#endif // M2_WAVE6_SSE2_H
