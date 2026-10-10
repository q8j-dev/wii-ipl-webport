#ifndef MSL_STDARG_H
#define MSL_STDARG_H

// Web build: the BESwap pass converts the pointer stored by va_start into
// big-endian and pre-swaps variadic arguments, so va_arg is a plain aligned
// pointer walk over the wasm variadic buffer.

#include <stddef.h>

typedef __builtin_va_list va_list;

#ifdef __cplusplus
extern "C" {
#endif
static inline void* __web_va_arg(void* ap, unsigned long size, unsigned long align) {
    char** p = (char**)ap;
    char* a = (char*)(((unsigned long)*p + align - 1) & ~(align - 1));
    *p = a + size;
    return a;
}
#ifdef __cplusplus
}
#endif

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#define va_arg(ap, T) (*(T*)__web_va_arg((void*)(ap), sizeof(T), __alignof__(T) < 4 ? 4 : __alignof__(T)))

#endif  // MSL_STDARG_H
