#ifndef GLOBAL_HEADERS_DECOMP_SHUT_UP_IDE_H
#define GLOBAL_HEADERS_DECOMP_SHUT_UP_IDE_H

// Web build replacement for include/global/decomp/ide.h: MWCC intrinsics as
// plain C. Memory is big-endian (see web/tools/beswap), so the byte-reversed
// load/store intrinsics swap explicitly.

#define asm
#define register
// __declspec(weak) keeps its meaning; section placement and the rest are
// irrelevant for the web build.
#define __declspec(x) __web_declspec_##x
#define __web_declspec_weak __attribute__((weak))
#define __web_declspec_novtable
#define __web_declspec_export
#define __web_declspec_align(n) __attribute__((aligned(n)))
#define __web_declspec_section __web_declspec_drop_string
#define __web_declspec_drop_string(x)

#ifdef __cplusplus
extern "C" {
#endif

void* memset(void*, int, unsigned long);

static inline void __sync(void) {}
static inline void __isync(void) {}
static inline void __eieio(void) {}
static inline unsigned int __cntlzw(unsigned int v) { return v ? (unsigned int)__builtin_clz(v) : 32u; }
static inline void __memclr(void* buffer, int len) { memset(buffer, 0, (unsigned long)len); }
static inline unsigned int __ppc_rotl(unsigned int v, unsigned int s) {
    s &= 31;
    return s ? (v << s) | (v >> (32 - s)) : v;
}
static inline unsigned int __ppc_mask(unsigned int mb, unsigned int me) {
    unsigned int a = 0xFFFFFFFFu >> mb;
    unsigned int b = me >= 31 ? 0u : 0xFFFFFFFFu >> (me + 1);
    return mb <= me ? (a & ~b) : (a | ~b);
}
static inline unsigned int __rlwinm(unsigned int v, unsigned int sh, unsigned int mb, unsigned int me) {
    return __ppc_rotl(v, sh) & __ppc_mask(mb, me);
}
static inline unsigned int __rlwnm(unsigned int v, unsigned int sh, unsigned int mb, unsigned int me) {
    return __ppc_rotl(v, sh) & __ppc_mask(mb, me);
}
static inline unsigned int __rlwimi(unsigned int a, unsigned int v, unsigned int sh, unsigned int mb, unsigned int me) {
    unsigned int m = __ppc_mask(mb, me);
    return (__ppc_rotl(v, sh) & m) | (a & ~m);
}
#define __alloca(n) __builtin_alloca(n)
static inline int __abs(int n) { return n < 0 ? -n : n; }
static inline double __fabs(double x) { return __builtin_fabs(x); }
static inline float __fabsf(float x) { return __builtin_fabsf(x); }
static inline double __fnabs(double x) { return -__builtin_fabs(x); }
static inline double __fsel(double c, double a, double b) { return c >= 0.0 ? a : b; }
static inline unsigned int __mulhwu(unsigned int a, unsigned int b) {
    return (unsigned int)(((unsigned long long)a * b) >> 32);
}
static inline int __mulhw(int a, int b) { return (int)(((long long)a * b) >> 32); }
static inline unsigned int __lwbrx(const void* base, int off) {
    return __builtin_bswap32(*(const unsigned int*)((const char*)base + off));
}
static inline unsigned short __lhbrx(const void* base, int off) {
    return __builtin_bswap16(*(const unsigned short*)((const char*)base + off));
}
static inline void __stwbrx(unsigned int v, void* base, int off) {
    *(unsigned int*)((char*)base + off) = __builtin_bswap32(v);
}
static inline void __sthbrx(unsigned short v, void* base, int off) {
    *(unsigned short*)((char*)base + off) = __builtin_bswap16(v);
}
static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); }
static inline float __fres(float x) { return 1.0f / x; }
static inline void __dcbz(void* base, int off) { memset((char*)base + off, 0, 32); }
static inline void __dcbf(void* base, int off) {}
static inline void __dcbst(void* base, int off) {}
static inline void __dcbi(void* base, int off) {}
static inline void __icbi(void* base, int off) {}

#ifdef __cplusplus
}
#endif

#endif  // GLOBAL_HEADERS_DECOMP_SHUT_UP_IDE_H
