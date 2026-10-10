// GX command FIFO for the web build (replaces GXFifo.c, GXMisc.c, GXPerf.c).
//
// The GX library itself is unchanged: it encodes register writes and vertices
// into the FIFO through __GXW_* (web/include/web/gxfifo.h). When the CPU FIFO
// is also the GP FIFO (immediate mode) the bytes are handed to the GPU
// emulator in web/js/gpu.js on flush; display-list FIFOs just collect bytes.

#include "web.h"

#include <private/gx.h>
#include <private/os.h>
#include <revolution/gx.h>
#include <revolution/os.h>

#include <string.h>

u8* __GXFifoCur;
u8* __GXFifoLimit;
volatile PPCWGPipe GXWGFifo;

static u8 sBootFifo[0x10000];
static __GXFifoObj sBootFifoObj;
static __GXFifoObj* sCPUFifo;
static __GXFifoObj* sGPFifo;

static GXDrawSyncCallback sTokenCB;
static GXDrawDoneCallback sDrawDoneCB;
static GXBreakPtCallback sBreakPtCB;
static volatile u8 sDrawDone;
static OSThreadQueue sFinishQueue;

static void sync_cpu_ptr(void) {
    if (sCPUFifo) {
        sCPUFifo->wrPtr = __GXFifoCur;
        sCPUFifo->count = (s32)(__GXFifoCur - sCPUFifo->base);
    }
}

static void process_gp(void) {
    __GXFifoObj* f = sCPUFifo;
    if (f == NULL || f != sGPFifo)
        return;
    u32 len = (u32)(__GXFifoCur - f->base);
    if (len == 0)
        return;
    u32 used = webjs_gpu_process(f->base, len);
    if (used < len)
        memmove(f->base, f->base + used, len - used);
    __GXFifoCur = f->base + (len - used);
    sync_cpu_ptr();
}

void __GXFifoFull(void) {
    __GXFifoObj* f = sCPUFifo;
    if (f && f == sGPFifo) {
        process_gp();
        if (__GXFifoCur >= __GXFifoLimit)
            __web_fatal("GX FIFO: command larger than the FIFO");
        return;
    }
    // Display list overflow: the hardware wraps; GXEndDisplayList reports 0.
    if (f) {
        f->wrap = GX_TRUE;
        __GXFifoCur = f->base;
    } else {
        __GXFifoCur = sBootFifo;
    }
}

void __GXFifoInit(void) {
    OSInitThreadQueue(&sFinishQueue);
}

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
    __GXFifoObj* f = (__GXFifoObj*)fifo;
    f->base = (u8*)base;
    f->top = (u8*)base + size - 4;
    f->size = size;
    f->count = 0;
    f->wrap = GX_FALSE;
    GXInitFifoLimits(fifo, size - 0x4000, (size >> 1) & ~31);
    GXInitFifoPtrs(fifo, base, base);
}

void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) {
    __GXFifoObj* f = (__GXFifoObj*)fifo;
    f->rdPtr = readPtr;
    f->wrPtr = writePtr;
    f->count = (s32)((u8*)writePtr - (u8*)readPtr);
    if (f->count < 0)
        f->count += f->size;
}

void GXInitFifoLimits(GXFifoObj* fifo, u32 hiWatermark, u32 loWatermark) {
    __GXFifoObj* f = (__GXFifoObj*)fifo;
    f->hiWatermark = hiWatermark;
    f->loWatermark = loWatermark;
}

void GXSetCPUFifo(GXFifoObj* fifo) {
    sync_cpu_ptr();
    if (sCPUFifo)
        sCPUFifo->bind_cpu = GX_FALSE;
    __GXFifoObj* f = (__GXFifoObj*)fifo;
    sCPUFifo = f;
    f->bind_cpu = GX_TRUE;
    f->wrap = GX_FALSE;
    __GXFifoCur = (u8*)f->wrPtr;
    __GXFifoLimit = f->top;
}

void GXSetGPFifo(GXFifoObj* fifo) {
    process_gp();
    if (sGPFifo)
        sGPFifo->bind_gp = GX_FALSE;
    sGPFifo = (__GXFifoObj*)fifo;
    sGPFifo->bind_gp = GX_TRUE;
}

GXBool GXGetCPUFifo(GXFifoObj* fifo) {
    sync_cpu_ptr();
    if (!sCPUFifo)
        return GX_FALSE;
    memcpy(fifo, sCPUFifo, sizeof(__GXFifoObj));
    return GX_TRUE;
}

GXFifoObj* GXGetGPFifo(void) { return (GXFifoObj*)sGPFifo; }

u32 GXGetFifoCount(const GXFifoObj* fifo) { return ((const __GXFifoObj*)fifo)->count; }
GXBool GXGetFifoWrap(const GXFifoObj* fifo) { return ((const __GXFifoObj*)fifo)->wrap; }

void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    *overhi = GX_FALSE;
    *underlow = GX_TRUE;
    *readIdle = GX_TRUE;
    *cmdIdle = GX_TRUE;
    *brkpt = GX_FALSE;
}

GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) {
    GXBreakPtCallback old = sBreakPtCB;
    sBreakPtCB = cb;
    return old;
}

void GXEnableBreakPt(void* breakPtr) {}
void GXDisableBreakPt(void) {}
void __GXCleanGPFifo(void) {}
GXBool __GXIsGPFifoReady(void) { return GX_TRUE; }
void GXCPInterruptHandler(__OSInterrupt interrupt, OSContext* context) {}

// ---- GXMisc ----

void GXSetMisc(GXMiscToken token, u32 val) {
    switch (token) {
        case GX_MT_XF_FLUSH:
            __GXData->vNum = val;
            __GXData->vNumNot = !__GXData->vNum;
            __GXData->bpSentNot = GX_TRUE;
            if (__GXData->vNum != 0)
                __GXData->dirtyState |= 8;
            break;
        case GX_MT_DL_SAVE_CONTEXT:
            __GXData->dlSaveContext = (val != 0);
            break;
        case GX_MT_ABORT_WAIT_COPYOUT:
            __GXData->abtWaitPECopy = (val != 0);
            break;
        default:
            break;
    }
}

void GXFlush(void) {
    if (__GXData->dirtyState)
        __GXSetDirtyState();
    process_gp();
}

void GXResetWriteGatherPipe(void) {}

void __GXAbort(void) {
    if (sGPFifo && sCPUFifo == sGPFifo)
        __GXFifoCur = sGPFifo->base;
}

void GXAbortFrame(void) { __GXAbort(); }

void GXSetDrawDone(void) {
    BOOL enabled = OSDisableInterrupts();
    GX_WRITE_RAS_REG(0x45000002);
    GXFlush();
    // The emulator executed everything synchronously.
    sDrawDone = 1;
    OSRestoreInterrupts(enabled);
    if (sDrawDoneCB)
        sDrawDoneCB();
}

void GXWaitDrawDone(void) {
    BOOL enabled = OSDisableInterrupts();
    while (!sDrawDone)
        OSSleepThread(&sFinishQueue);
    OSRestoreInterrupts(enabled);
}

void GXDrawDone(void) {
    GXSetDrawDone();
    GXWaitDrawDone();
}

void GXPixModeSync(void) {
    GX_WRITE_RAS_REG(__GXData->peCtrl);
    __GXData->bpSentNot = GX_FALSE;
}

void GXTexModeSync(void) { GX_WRITE_RAS_REG(0x63000000); }

void GXPokeAlphaMode(GXCompare func, u8 threshold) {}
void GXPokeAlphaRead(GXAlphaReadMode mode) {}
void GXPokeAlphaUpdate(GXBool update_enable) {}
void GXPokeBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {}
void GXPokeColorUpdate(GXBool update_enable) {}
void GXPokeDstAlpha(GXBool enable, u8 alpha) {}
void GXPokeDither(GXBool dither) {}
void GXPokeZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) {
    GXDrawSyncCallback old = sTokenCB;
    sTokenCB = cb;
    return old;
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) {
    GXDrawDoneCallback old = sDrawDoneCB;
    sDrawDoneCB = cb;
    return old;
}

static u16 sLastToken;

static void token_irq(void* arg) {
    if (sTokenCB)
        sTokenCB((u16)(u32)arg);
}

// Called by the GPU emulator when it executes a PE token interrupt write.
void webx_gx_token(u32 token) {
    sLastToken = (u16)token;
    __web_queue_irq(token_irq, (void*)token);
}

u16 GXReadDrawSync(void) { return sLastToken; }

void __GXPEInit(void) {}

// ---- GXPerf ----

void GXSetGPMetric(GXPerf0 perf0, GXPerf1 perf1) {}
void GXClearGPMetric(void) {}
void GXReadGPMetric(u32* cnt0, u32* cnt1) { *cnt0 = *cnt1 = 0; }
u32 GXReadGP0Metric(void) { return 0; }
u32 GXReadGP1Metric(void) { return 0; }
void GXReadMemMetric(u32* cpReq, u32* tcReq, u32* cpuRdReq, u32* cpuWrReq, u32* dspReq, u32* ioReq, u32* viReq, u32* peReq,
                     u32* rfReq, u32* fiReq) {}
void GXClearMemMetric(void) {}
void GXReadPixMetric(u32* topIn, u32* topOut, u32* bottomIn, u32* bottomOut, u32* clearIn, u32* copyClocks) {}
void GXClearPixMetric(void) {}
void GXSetVCacheMetric(GXVCachePerf attr) {}
void GXReadVCacheMetric(u32* check, u32* miss, u32* stall) {}
void GXClearVCacheMetric(void) {}

void __web_gx_boot(void) {
    GXInitFifoBase((GXFifoObj*)&sBootFifoObj, sBootFifo, sizeof(sBootFifo));
    __GXFifoCur = sBootFifo;
    __GXFifoLimit = sBootFifo + sizeof(sBootFifo) - 4;
}

u32 webx_hwreg(void) {
    extern u8 __wii_hwreg[];
    return (u32)__wii_hwreg;
}
