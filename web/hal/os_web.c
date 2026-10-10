// OS hardware layer for the web build.
//
// The decompiled OSThread/OSAlarm/OSInterrupt/OSTime sources run unchanged
// except for their context switches (see os_thread_web.c). This file provides
// what the hardware did underneath them:
//
//  * "interrupts": the CPU runs cooperatively, so pending events (alarm
//    deadlines, IOS replies, video retrace) are delivered at interrupt points:
//    when interrupts get re-enabled and while the scheduler idles. Delivery
//    mirrors the hardware: handlers run with a temporary context and the
//    scheduler disabled, then the interrupted thread may be preempted.
//  * the time base (OSGetTime) from performance.now(),
//  * low memory globals, SRAM and RTC.

#include "web.h"

#include <private/os.h>
#include <private/os/OSSram.h>
#include <revolution/base/PPCArch.h>
#include <revolution/os.h>
#include <revolution/sc.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TIMER_CLOCK (243000000 / 4)

// ---------------------------------------------------------------------------
// Low memory and arenas

// The default thread's stack bounds, normally from the linker script.
u8 _stack_addr[32];
u8 _stack_end[32];

BOOL __OSInIPL = TRUE;
u32 __OSFpscrEnableBits = 0;

static void w32(u32 off, u32 v) { *(u32*)off = v; }

static void init_low_memory(void) {
    memset((void*)0, 0, 0x4000);
    // Boot info (physical addresses: wasm addresses are Wii physical addresses)
    memcpy((void*)0, "HAEA", 4);  // System Menu title code as "game code"
    memcpy((void*)4, "01", 2);
    w32(0x18, 0x5D1C9EA3);  // Wii magic
    w32(0x1C, 0);
    w32(0x20, 0x0D15EA5E);
    w32(0x24, 1);
    w32(0x28, 0x01800000);  // physical MEM1 size
    w32(0x2C, 0x10000006);  // console type: retail Wii
    w32(0xCC, 0);           // NTSC
    w32(0xF0, 0x01800000);  // simulated memory size
    w32(0xF8, 243000000);   // bus clock
    w32(0xFC, 729000000);   // core clock
    w32(0x3100, 0x01800000);
    w32(0x3104, 0x01800000);
    w32(0x310C, 0x00004000);  // MEM1 arena (physical)
    w32(0x3110, 0x01800000);
    w32(0x3118, 0x04000000);  // MEM2 size
    w32(0x311C, 0x04000000);
    w32(0x3120, 0x13400000);
    w32(0x3124, 0x10000800);  // MEM2 arena
    w32(0x3128, 0x133E0000);
    w32(0x3130, 0x133E0000);  // IPC buffer
    w32(0x3134, 0x13400000);
    w32(0x3138, 0x00000011);  // Hollywood revision
    w32(0x3140, 0x00501F20);  // IOS80 v7968
    w32(0x3144, 0x00062507);
    w32(0x3158, 0x0000FF16);
    w32(0x3188, 0x00501F20);
}

void __web_os_early_init(void) { init_low_memory(); }

// ---------------------------------------------------------------------------
// Time

static double sTimeBaseMs;  // performance.now() at time base zero
static s64 sTimeOffset;     // ticks added by __OSSetTime

OSTime OSGetTime(void) {
    double ms = webjs_now() - sTimeBaseMs;
    return (OSTime)(ms * (TIMER_CLOCK / 1000.0)) + sTimeOffset;
}

OSTick OSGetTick(void) { return (OSTick)OSGetTime(); }

void __web_set_time(OSTime time) {
    double ms = webjs_now() - sTimeBaseMs;
    sTimeOffset = time - (OSTime)(ms * (TIMER_CLOCK / 1000.0));
}

static double ticks_to_ms(s64 t) { return (double)t / (TIMER_CLOCK / 1000.0); }

// ---------------------------------------------------------------------------
// Interrupts

static int sIrqEnabled = 0;
static int sInPoll = 0;
volatile int __web_irq_pending = 0;

// Decrementer: armed by OSAlarm through PPCMtdec.
static int sDecArmed = 0;
static double sDecDeadline;
void __web_alarm_interrupt(OSContext* context);

void PPCMtdec(u32 ticks) {
    sDecArmed = 1;
    sDecDeadline = webjs_now() + ticks_to_ms((s32)ticks < 0 ? 0 : ticks);
}

typedef struct {
    void (*fn)(void*);
    void* arg;
} IrqItem;

#define IRQ_QUEUE_SIZE 256
static IrqItem sIrqQueue[IRQ_QUEUE_SIZE];
static u32 sIrqHead, sIrqTail;

void __web_queue_irq(void (*fn)(void*), void* arg) {
    u32 next = (sIrqTail + 1) % IRQ_QUEUE_SIZE;
    if (next == sIrqHead)
        __web_fatal("interrupt queue overflow");
    sIrqQueue[sIrqTail].fn = fn;
    sIrqQueue[sIrqTail].arg = arg;
    sIrqTail = next;
    __web_irq_pending = 1;
}

static OSContext sIrqContext;

void __web_irq_enter(OSContext* saved[1]) {
    saved[0] = OSGetCurrentContext();
    OSDisableScheduler();
    OSClearContext(&sIrqContext);
    OSSetCurrentContext(&sIrqContext);
}

void __web_irq_leave(OSContext* saved[1]) {
    OSSetCurrentContext(saved[0]);
    OSEnableScheduler();
}

static void deliver(int idle) {
    OSContext* ctx = OSGetCurrentContext();
    int any = 0;
    double now = webjs_now();

    while (sIrqHead != sIrqTail) {
        IrqItem it = sIrqQueue[sIrqHead];
        sIrqHead = (sIrqHead + 1) % IRQ_QUEUE_SIZE;
        OSContext* saved[1];
        __web_irq_enter(saved);
        it.fn(it.arg);
        __web_irq_leave(saved);
        any = 1;
    }
    __web_irq_pending = 0;

    // Alarms: the decrementer callback fires one alarm and re-arms.
    int guard = 0;
    while (sDecArmed && now >= sDecDeadline && guard++ < 64) {
        sDecArmed = 0;
        __web_alarm_interrupt(ctx);
        any = 1;
    }

    // Video retrace.
    if (now >= __web_vi_next_retrace()) {
        OSContext* saved[1];
        __web_irq_enter(saved);
        __web_vi_retrace();
        __web_irq_leave(saved);
        any = 1;
    }

    if (any && !idle)
        __OSReschedule();
}

// Interrupt point. With idle set the scheduler has nothing to run: wait for
// the next event, letting the browser run meanwhile.
void __web_poll(int idle) {
    if (sInPoll)
        return;
    sInPoll = 1;
    BOOL enabled = sIrqEnabled;
    sIrqEnabled = 0;

    deliver(idle);

    if (idle) {
        double now = webjs_now();
        double next = __web_vi_next_retrace();
        if (sDecArmed && sDecDeadline < next)
            next = sDecDeadline;
        if (!__web_irq_pending && next > now) {
            int forFrame = next == __web_vi_next_retrace();
            webjs_wait(next - now, forFrame);
        }
        deliver(1);
    } else {
        // A thread that never blocks must still let frames happen.
        double late = webjs_now() - __web_vi_next_retrace();
        if (late > 50.0) {
            webjs_wait(0, 1);
            deliver(0);
        }
    }

    sIrqEnabled = enabled;
    sInPoll = 0;
}

void __web_idle(void) { __web_poll(1); }

BOOL OSDisableInterrupts(void) {
    BOOL prev = sIrqEnabled;
    sIrqEnabled = 0;
    return prev;
}

static int sPollCounter;

static inline void maybe_poll(void) {
    if (__web_irq_pending || (++sPollCounter & 63) == 0)
        __web_poll(0);
}

BOOL OSEnableInterrupts(void) {
    BOOL prev = sIrqEnabled;
    sIrqEnabled = 1;
    if (!prev)
        maybe_poll();
    return prev;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL prev = sIrqEnabled;
    sIrqEnabled = level ? 1 : 0;
    if (level && !prev)
        maybe_poll();
    return prev;
}

// ---------------------------------------------------------------------------
// Contexts and exceptions

void OSSetCurrentContext(OSContext* context) { __OSCurrentContext = context; }
OSContext* OSGetCurrentContext(void) { return (OSContext*)__OSCurrentContext; }
void OSClearContext(OSContext* context) {}
void OSFillFPUContext(OSContext* context) {}
void __OSContextInit(void) {}

int OSSwitchFiber(u32 pc, u32 newsp) {
    ((void (*)(void))pc)();
    return 0;
}

void OSSwitchFiberEx(u32 r3, u32 r4, u32 r5, u32 r6, void* pc, void* newsp) {
    ((void (*)(u32, u32, u32, u32))pc)(r3, r4, r5, r6);
}

static __OSExceptionHandler sExceptionHandlers[16];

__OSExceptionHandler __OSSetExceptionHandler(__OSException type, __OSExceptionHandler handler) {
    __OSExceptionHandler old = sExceptionHandlers[type & 15];
    sExceptionHandlers[type & 15] = handler;
    return old;
}

__OSExceptionHandler __OSGetExceptionHandler(__OSException type) { return sExceptionHandlers[type & 15]; }

void OSDefaultExceptionHandler(u8 type, OSContext* context) { __web_fatal("exception %d", type); }

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) { return NULL; }

void OSProtectRange(u32 chan, void* addr, u32 nBytes, u32 control) {}
void __OSFPRInit(void) {}

// ---------------------------------------------------------------------------
// Caches and processor registers

void DCFlushRange(void* addr, u32 len) { webjs_gpu_invalidate((u32)addr, len); }
void DCFlushRangeNoSync(void* addr, u32 len) { webjs_gpu_invalidate((u32)addr, len); }
void DCStoreRange(void* addr, u32 len) { webjs_gpu_invalidate((u32)addr, len); }
void DCStoreRangeNoSync(void* addr, u32 len) { webjs_gpu_invalidate((u32)addr, len); }
void DCInvalidateRange(void* addr, u32 len) {}
void DCZeroRange(void* addr, u32 len) { memset(addr, 0, len); }
void DCTouchRange(void* addr, u32 len) {}
void DCEnable(void) {}
void ICInvalidateRange(void* addr, u32 len) {}
void ICFlashInvalidate(void) {}
void ICEnable(void) {}
void LCEnable(void) {}
void LCDisable(void) {}
void __OSCacheInit(void) {}

static u32 sMsr = 0x8000;
u32 PPCMfmsr(void) { return sMsr; }
void PPCMtmsr(u32 val) { sMsr = val; }
static u32 sHid2;
u32 PPCMfhid2(void) { return sHid2; }
void PPCMthid2(u32 val) { sHid2 = val; }
void PPCMtwpar(u32 val) {}
u32 PPCMfwpar(void) { return 0; }
void PPCMthid4(u32 val) {}
u32 PPCMfhid4(void) { return 0; }
void PPCHalt(void) { __web_fatal("PPCHalt"); }
void PPCSync(void) {}
void PPCMtmmcr0(u32 val) {}
void PPCMtmmcr1(u32 val) {}
void PPCMtpmc1(u32 val) {}
void PPCMtpmc2(u32 val) {}
void PPCMtpmc3(u32 val) {}
void PPCMtpmc4(u32 val) {}
u32 PPCMfpmc1(void) { return 0; }
u32 PPCMfpmc2(void) { return 0; }
u32 PPCMfpmc3(void) { return 0; }
u32 PPCMfpmc4(void) { return 0; }
void PPCDisableSpeculation(void) {}
void PPCSetFpNonIEEEMode(void) {}
void PPCSetFpIEEEMode(void) {}
u32 PPCMfdec(void) { return 0; }

// ---------------------------------------------------------------------------
// Reporting

static void write_text(const char* s, u32 n) { webjs_log(s, n); }

void OSVReport(const char* msg, va_list list) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), msg, list);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    write_text(buf, n);
}

void OSReport(const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);
    OSVReport(msg, ap);
    va_end(ap);
}

void __web_report(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 3)
        n = sizeof(buf) - 3;
    buf[n++] = '\n';
    write_text(buf, n);
}

void __web_fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    webjs_fatal(buf, n);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    __web_fatal("OSPanic %s:%d: %s", file, line, buf);
}

void OSRegisterVersion(const char* id) {}

static const char* sUnimplementedSeen[512];
static int sUnimplementedCount;

void __web_unimplemented(const char* name) {
    for (int i = 0; i < sUnimplementedCount; i++)
        if (sUnimplementedSeen[i] == name)
            return;
    if (sUnimplementedCount < 512)
        sUnimplementedSeen[sUnimplementedCount++] = name;
    __web_report("[web] unimplemented: %s", name);
}

// ---------------------------------------------------------------------------
// Shutdown functions (OSReset.c keeps the list on hardware)

static OSShutdownFunctionInfo* sShutdownHead;

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) {
    OSShutdownFunctionInfo** p = &sShutdownHead;
    while (*p && (*p)->priority <= info->priority)
        p = &(*p)->next;
    info->next = *p;
    info->prev = NULL;
    *p = info;
}

BOOL __OSCallShutdownFunctions(BOOL final, u32 event) {
    BOOL ok = TRUE;
    for (OSShutdownFunctionInfo* i = sShutdownHead; i; i = i->next)
        ok &= i->func(final, event) != 0;
    return ok;
}

void __OSShutdownDevices(u32 id) { __OSCallShutdownFunctions(TRUE, id); }
void __OSStopAudioSystem(void) {}
void __OSInitAudioSystem(void) {}

void __OSHotResetForError(void) { __web_fatal("The Wii was reset (error)."); }
void OSRestart(u32 resetCode) { __web_fatal("The Wii was restarted."); }

void __OSLaunchTitlevForSystem(u64 titleId, u32 launchCode, const char** argv) {
    __web_fatal("Launching title %08x-%08x is not supported.", (u32)(titleId >> 32), (u32)titleId);
}

void __OSLaunchTitlelForSystem(u64 titleId, u32 launchCode, const char* arg0, ...) {
    __OSLaunchTitlevForSystem(titleId, launchCode, NULL);
}

// ---------------------------------------------------------------------------
// System information

u32 OSGetConsoleType(void) { return 0x10000006; }
u32 OSGetPhysicalMem2Size(void) { return 0x04000000; }
u32 OSGetConsoleSimulatedMem2Size(void) { return 0x04000000; }
u32 OSGetPhysicalMem1Size(void) { return 0x01800000; }
u32 __OSGetHollywoodRev(void) { return 0x11; }

void __OSGetIOSRev(OSIOSRev* rev) {
    rev->reserved = 0;
    rev->major = 80;
    rev->minor = 0x1F;
    rev->micro = 0x20;
    rev->month = 3;
    rev->date = 6;
    rev->year = 0x2012;
}

u8 OSGetAppType(void) { return 0x40; }
const char* OSGetAppGamename(void) { return "HAEA"; }
BOOL __OSIsReturnToIdle(void) { return FALSE; }

// ---------------------------------------------------------------------------
// SRAM and RTC

static OSSram sSram;
static OSSramEx sSramEx;

void __OSInitSram(void) {
    memset(&sSram, 0, sizeof(sSram));
    memset(&sSramEx, 0, sizeof(sSramEx));
    sSram.language = 0;  // English
    sSram.flags = 0x04;  // stereo
}

OSSram* __OSLockSram(void) { return &sSram; }
OSSramEx* __OSLockSramEx(void) { return &sSramEx; }
BOOL __OSUnlockSram(int commit) { return TRUE; }
BOOL __OSUnlockSramEx(int commit) { return TRUE; }
BOOL __OSSyncSram(void) { return TRUE; }
BOOL __OSCheckSram(void) { return TRUE; }

BOOL __OSReadROM(void* buffer, s32 length, s32 offset) {
    memset(buffer, 0, length);
    return TRUE;
}

static u32 rtc_now(void) {
    // Seconds since 2000-01-01 in local time; the Wii clock shows local time.
    double ms = webjs_now_wall_local();
    return (u32)(ms / 1000.0 - 946684800.0);
}

BOOL __OSGetRTC(u32* rtc) {
    *rtc = rtc_now();
    return TRUE;
}

BOOL __OSSetRTC(u32 rtc) { return TRUE; }
BOOL __OSGetRTCFlags(u32* flags) {
    *flags = 0;
    return TRUE;
}
BOOL __OSClearRTCFlags(void) { return TRUE; }

u32 OSGetSoundMode(void) { return (sSram.flags & 4) ? 1 : 0; }
void OSSetSoundMode(u32 mode) { sSram.flags = (sSram.flags & ~4) | ((mode << 2) & 4); }
u32 OSGetProgressiveMode(void) { return (sSram.flags & 0x80) >> 7; }
void OSSetProgressiveMode(u32 mode) { sSram.flags = (sSram.flags & ~0x80) | ((mode << 7) & 0x80); }
void OSSetVideoMode(u32 mode) { sSram.flags = (sSram.flags & ~3) | (mode > 2 ? 0 : mode); }
u8 OSGetLanguage(void) { return sSram.language; }
void OSSetLanguage(u8 language) { sSram.language = language; }
u32 OSGetEuRgb60Mode(void) { return (sSram.ntd & 0x40) >> 6; }
void OSSetEuRgb60Mode(u32 mode) { sSram.ntd = (sSram.ntd & ~0x40) | ((mode << 6) & 0x40); }
u16 OSGetWirelessID(s32 chan) { return sSramEx.wirelessPadID[chan & 3]; }
void OSSetWirelessID(s32 chan, u16 id) { sSramEx.wirelessPadID[chan & 3] = id; }
u16 OSGetGbsMode(void) { return sSramEx.gbs; }
void OSSetGbsMode(u16 mode) { sSramEx.gbs = mode; }

// ---------------------------------------------------------------------------
// Initialization

#include <private/os/OSStateTM.h>

void __OSInitIPCBuffer(void);
void __OSThreadInit(void);
void __OSInitAlarm(void);
void __OSInterruptInit(void);
void __OSInitNet(void);

OSTime __OSStartTime;
void __web_sc_defaults(void);

static BOOL sInitialized;

void OSInit(void) {
    if (sInitialized)
        return;
    sInitialized = TRUE;
    sTimeBaseMs = webjs_now();

    __OSStartTime = __OSGetSystemTime();
    OSDisableInterrupts();

    OSSetMEM1ArenaLo((void*)*(u32*)0x310C);
    OSSetMEM1ArenaHi((void*)*(u32*)0x3110);
    OSSetMEM2ArenaLo((void*)*(u32*)0x3124);
    OSSetMEM2ArenaHi((void*)*(u32*)0x3128);

    __OSInitIPCBuffer();
    __OSInitAlarm();
    __OSInterruptInit();
    __OSInitSram();
    __OSThreadInit();

    OSEnableInterrupts();
    IPCCltInit();
    __OSInitSTM();
    SCInit();
    while (SCCheckStatus() == SC_STATUS_BUSY) {
        __web_poll(0);
    }
    __web_sc_defaults();
    __OSInitNet();
}

// ---------------------------------------------------------------------------
// Hardware register window (see web/tools/beswap: fixed register addresses
// are redirected here; the HLE layer does not use them).

u8 __wii_hwreg[0x30000] __attribute__((aligned(32)));

OSErrorHandler __OSErrorTable[OS_EXCEPTION_MAX];

void __cxa_pure_virtual(void) { __web_fatal("pure virtual function called"); }
