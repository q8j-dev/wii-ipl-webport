// Paired-single matrix entry points, forwarded to the SDK's C versions.

#include <revolution/mtx.h>

void PSMTXIdentity(Mtx m) { C_MTXIdentity(m); }
void PSMTXCopy(const Mtx src, Mtx dst) { C_MTXCopy(src, dst); }
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) { C_MTXConcat(a, b, ab); }
u32 PSMTXInverse(const Mtx src, Mtx inv) { return C_MTXInverse(src, inv); }
u32 PSMTXInvXpose(const Mtx src, Mtx invX) { return C_MTXInvXpose(src, invX); }
void PSMTXRotRad(Mtx m, char axis, f32 rad) { C_MTXRotRad(m, axis, rad); }
void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) { C_MTXRotTrig(m, axis, sinA, cosA); }
void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) { C_MTXTrans(m, xT, yT, zT); }
void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) { C_MTXTransApply(src, dst, xT, yT, zT); }
void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) { C_MTXScale(m, xS, yS, zS); }
void PSMTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) { C_MTXScaleApply(src, dst, xS, yS, zS); }
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVec(m, src, dst); }
void PSVECNormalize(const Vec* src, Vec* dst) { C_VECNormalize(src, dst); }
void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) { C_VECCrossProduct(a, b, axb); }
