# Vec3::Xform(const Matrix33&) thiscall: ecx = Vec3* (in/out), [esp+4] = Matrix (3x3 part of Matrix34)
# v' = c0*x + c1*y + c2*z    register-only (xmm0..xmm5), no FPU, no stack temporaries
.intel_syntax noprefix
.text
    mov     edx, dword ptr [esp+4]
    movss   xmm0, dword ptr [ecx]
    movss   xmm1, dword ptr [ecx+4]
    movss   xmm2, dword ptr [ecx+8]
    shufps  xmm0, xmm0, 0
    shufps  xmm1, xmm1, 0
    shufps  xmm2, xmm2, 0
    movups  xmm3, xmmword ptr [edx]
    mulps   xmm3, xmm0
    movups  xmm4, xmmword ptr [edx+0x0c]
    mulps   xmm4, xmm1
    addps   xmm3, xmm4
    movlps  xmm5, qword ptr [edx+0x18]
    movss   xmm6, dword ptr [edx+0x20]
    movlhps xmm5, xmm6
    mulps   xmm5, xmm2
    addps   xmm3, xmm5
    movlps  qword ptr [ecx], xmm3
    movhlps xmm3, xmm3
    movss   dword ptr [ecx+8], xmm3
    mov     eax, ecx
    ret     4
