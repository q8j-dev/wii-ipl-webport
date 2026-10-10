#ifndef WEB_GXFIFO_H
#define WEB_GXFIFO_H

// Web build: the GX write-gather pipe. Writes are appended (big-endian, like
// the hardware) to the current CPU FIFO buffer; web/hal/gx_fifo.c hands the
// buffer to the GPU emulator in web/js when it fills up or on GXFlush.

#include <revolution/types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern u8* __GXFifoCur;
extern u8* __GXFifoLimit;
void __GXFifoFull(void);

#define __GXW_DEF(T, N)                                                                                                \
    static inline void __GXW_##T(T v) {                                                                                \
        __builtin_memcpy(__GXFifoCur, &v, N);                                                                          \
        __GXFifoCur += N;                                                                                              \
        if (__GXFifoCur >= __GXFifoLimit)                                                                              \
            __GXFifoFull();                                                                                            \
    }

__GXW_DEF(u8, 1)
__GXW_DEF(s8, 1)
__GXW_DEF(u16, 2)
__GXW_DEF(s16, 2)
__GXW_DEF(u32, 4)
__GXW_DEF(s32, 4)
__GXW_DEF(f32, 4)

#undef __GXW_DEF

#ifdef __cplusplus
}
#endif

#endif
