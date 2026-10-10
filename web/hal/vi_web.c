// Video interface. Retraces happen at 60 Hz of real time (paced by the
// browser's animation frames, see __web_poll); at each retrace the frame
// buffer most recently flushed with VIFlush is presented on the canvas.

#include "web.h"

#include <revolution/gx.h>
#include <revolution/os.h>
#include <revolution/vi.h>

#define RETRACE_MS (1000.0 / 60.0)

static OSThreadQueue sRetraceQueue;
static volatile u32 sRetraceCount;
static VIRetraceCallback sPreCallback;
static VIRetraceCallback sPostCallback;
static double sNextRetrace;
static int sInitialized;

static void* sNextFb;
static void* sCurrentFb;
static int sFlushed;
static BOOL sBlack = TRUE;
static BOOL sNextBlack = TRUE;
static u16 sFbWidth = 640, sFbHeight = 456;

double __web_vi_next_retrace(void) {
    if (!sInitialized)
        return 1e300;
    return sNextRetrace;
}

void __web_vi_retrace(void) {
    double now = webjs_now();
    // Keep a steady 60 Hz grid; skip ahead if we fell behind.
    sNextRetrace += RETRACE_MS;
    if (sNextRetrace < now)
        sNextRetrace = now + RETRACE_MS * 0.5;

    sRetraceCount++;
    if (sPreCallback)
        sPreCallback(sRetraceCount);

    if (sFlushed) {
        sCurrentFb = sNextFb;
        sBlack = sNextBlack;
        sFlushed = 0;
    }
    webjs_vi_present((u32)sCurrentFb, sFbWidth, sFbHeight, sBlack || sCurrentFb == NULL);

    OSWakeupThread(&sRetraceQueue);
    if (sPostCallback)
        sPostCallback(sRetraceCount);
}

void VIInit(void) {
    if (sInitialized)
        return;
    sInitialized = 1;
    OSInitThreadQueue(&sRetraceQueue);
    sNextRetrace = webjs_now() + RETRACE_MS;
}

void __VIInit(VITVMode mode) {}
void __VIInit3in1(VITVMode mode) {}

void VIWaitForRetrace(void) {
    BOOL enabled = OSDisableInterrupts();
    u32 count = sRetraceCount;
    do {
        OSSleepThread(&sRetraceQueue);
    } while (count == sRetraceCount);
    OSRestoreInterrupts(enabled);
}

void VIConfigure(const GXRenderModeObj* rm) {
    sFbWidth = rm->fbWidth;
    sFbHeight = rm->xfbHeight;
}

void VIConfigurePan(u16 xOrg, u16 yOrg, u16 width, u16 height) {}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = sPreCallback;
    sPreCallback = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = sPostCallback;
    sPostCallback = cb;
    return old;
}

void VIFlush(void) { sFlushed = 1; }

void VISetNextFrameBuffer(void* fb) { sNextFb = fb; }

void VISetBlack(BOOL black) { sNextBlack = black; }

void VISetTrapFilter(BOOL setTrap) {}
void VIResetDimmingCount(void) {}
void VIEnableDimming(BOOL enableDim) {}
u32 VIGetRetraceCount(void) { return sRetraceCount; }
u32 VIGetDTVStatus(void) { return 0; }
u32 VIGetScanMode(void) { return 0; }  // interlaced
u32 VIGetTvFormat(void) { return 0; }  // NTSC
u32 VIGetCurrentLine(void) { return 0; }
void* VIGetCurrentFrameBuffer(void) { return sCurrentFb; }
void* VIGetNextFrameBuffer(void) { return sNextFb; }
u32 VIGetNextField(void) { return 1; }
void __VISetAdjustingValues(s16 x, s16 y) {}
void __VISetRGBModeImm(void) {}
void __VISetVolume(u8 l, u8 r) {}
void VISetVolume(u8 l, u8 r) {}
