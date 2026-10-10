// DSP and audio interface. (Mixing of AX voices is added in ax_web.c.)

#include "web.h"

#include <private/dsp.h>
#include <revolution/ai.h>
#include <revolution/dsp.h>

static int sDspInit;

void DSPInit(void) { sDspInit = 1; }
BOOL DSPCheckInit(void) { return sDspInit; }
u32 DSPCheckMailToDSP(void) { return 0; }
u32 DSPCheckMailFromDSP(void) { return 0; }
u32 DSPReadMailFromDSP(void) { return 0; }
u32 DSPReadCPUToDSPMbox(void) { return 0; }
void DSPSendMailToDSP(u32 mail) {}
void DSPAssertInt(void) {}
void DSPHalt(void) {}
void DSPReset(void) {}
void DSPUnhalt(void) {}
u32 DSPGetDMAStatus(void) { return 0; }

DSPTaskInfo* DSPAddTask(DSPTaskInfo* task) {
    if (task->init_cb)
        task->init_cb(task);
    return task;
}

DSPTaskInfo* DSPAssertTask(DSPTaskInfo* task) { return task; }

static AIDCallback sDmaCallback;
static u32 sDmaAddr, sDmaLen;

void AIInit(void* stack) {}
void AIReset(void) {}
AIDCallback AIRegisterDMACallback(AIDCallback cb) {
    AIDCallback old = sDmaCallback;
    sDmaCallback = cb;
    return old;
}
void AIInitDMA(u32 addr, u32 length) {
    sDmaAddr = addr;
    sDmaLen = length;
}
void AIStartDMA(void) {}
void AIStopDMA(void) {}
u32 AIGetDMABytesLeft(void) { return 0; }
u32 AIGetDMAStartAddr(void) { return sDmaAddr; }
u32 AIGetDMALength(void) { return sDmaLen; }
BOOL AICheckInit(void) { return TRUE; }
void AISetDSPSampleRate(u32 rate) {}
u32 AIGetDSPSampleRate(void) { return 1; }
