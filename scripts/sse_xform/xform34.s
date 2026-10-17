# Vec3::Xform(const Matrix34&)  thiscall: ecx = Vec3* (in/out), [esp+4] = Matrix34*
# Matrix34 is column-major: c0=[0x00..0x08], c1=[0x0c..0x14], c2=[0x18..0x20], t=[0x24..0x2c]
# v' = c0*x + c1*y + c2*z + t    register-only (xmm0..xmm6), no FPU, no stack temporaries
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
    movups  xmm5, xmmword ptr [edx+0x18]
    mulps   xmm5, xmm2
    addps   xmm3, xmm5
    movups  xmm6, xmmword ptr [edx+0x20]
    shufps  xmm6, xmm6, 0x39
    addps   xmm3, xmm6
    movlps  qword ptr [ecx], xmm3
    movhlps xmm3, xmm3
    movss   dword ptr [ecx+8], xmm3
    mov     eax, ecx
    ret     4
