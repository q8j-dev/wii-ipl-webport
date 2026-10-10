// Controllers: one Wii Remote on channel 0 driven by the mouse, keyboard or
// a gamepad (web/js/input.js). KPAD is implemented directly (pointer position
// in KPAD's [-1, 1] units) instead of synthesizing IR camera data; WPAD
// answers status queries for a connected remote; PAD reports no GameCube
// controllers.

#include "web.h"

#include <revolution/kpad.h>
#include <revolution/pad.h>
#include <revolution/wpad.h>

#include <string.h>

// Written by webjs_input_poll (big-endian, like all memory).
typedef struct WebPadState {
    u32 buttons;  // WPAD core button bits
    f32 x;        // pointer, KPAD units
    f32 y;
    u32 pointerValid;
    u32 connected;
} WebPadState;

static WebPadState sState;
static u32 sPrevButtons;
static f32 sPrevX, sPrevY;
static int sDpdEnabled = 1;

static WPADConnectCallback sConnectCB[4];
static WPADExtensionCallback sExtensionCB[4];
static WPADSamplingCallback sSamplingCB[4];
static KPADCallback* sControlDpdCB[4];

// ---- KPAD ----

void KPADInit(void) {}
void KPADSetSensorHeight(s32 chan, f32 sensorHeight) {}
void KPADSetPosParam(s32 chan, f32 x, f32 y) {}
void KPADSetBtnRepeat(s32 chan, f32 a, f32 b) {}
void KPADEnableDPD(s32 chan) { sDpdEnabled = 1; }
void KPADDisableDPD(s32 chan) { sDpdEnabled = 0; }
void KPADSetControlDpdCallback(s32 chan, KPADCallback* cb) { sControlDpdCB[chan & 3] = cb; }

void KPADGetProjectionPos(Vec2* dest, Vec2* src, const Rect* rect, f32 scale) {
    f32 halfHeight = (rect->bottom - rect->top) * 0.5f;
    dest->x = (f32)(1.2f * (src->x * halfHeight) * (0.908 * scale));
    dest->y = 1.2f * (src->y * halfHeight);
}

// The ring buffers are only used for diagnostics.
static WPADStatus sRing[16];
void* KPADGetWPADRingBuffer(s32 chan) { return sRing; }
void* KPADGetWPADFSRingBuffer(s32 chan) { return sRing; }
void* KPADGetWPADCLRingBuffer(s32 chan) { return sRing; }

s32 KPADRead(s32 chan, KPADStatus* status, u32 length) {
    if (length == 0)
        return 0;
    if (chan != 0 || !sState.connected) {
        memset(status, 0, sizeof(*status));
        status->wpad_err = WPAD_ERR_NO_CONTROLLER;
        status->dev_type = WPAD_DEV_NOT_FOUND;
        return 0;
    }
    webjs_input_poll(&sState);
    u32 b = sState.buttons;
    memset(status, 0, sizeof(*status));
    status->hold = b;
    status->trig = b & ~sPrevButtons;
    status->release = sPrevButtons & ~b;
    sPrevButtons = b;

    // Remote held level, pointing at the screen.
    status->acc.x = 0.0f;
    status->acc.y = -1.0f;
    status->acc.z = 0.0f;
    status->acc_value = 1.0f;
    status->acc_speed = 0.0f;
    status->acc_vertical.x = 1.0f;
    status->acc_vertical.y = 0.0f;
    status->horizon.x = 1.0f;
    status->horizon.y = 0.0f;
    status->dist = 2.0f;

    if (sDpdEnabled && sState.pointerValid) {
        status->pos.x = sState.x;
        status->pos.y = sState.y;
        status->vec.x = sState.x - sPrevX;
        status->vec.y = sState.y - sPrevY;
        f32 dx = status->vec.x, dy = status->vec.y;
        status->speed = __builtin_sqrtf(dx * dx + dy * dy);
        status->dpd_valid_fg = 2;
        sPrevX = sState.x;
        sPrevY = sState.y;
    } else {
        status->pos.x = sPrevX;
        status->pos.y = sPrevY;
        status->dpd_valid_fg = 0;
    }
    status->dev_type = WPAD_DEV_CORE;
    status->wpad_err = WPAD_ERR_OK;
    status->data_format = WPAD_FMT_CORE_BTN_ACC_DPD;
    return 1;
}

// ---- WPAD ----

static WPADAllocFunc sAlloc;
static WPADFreeFunc sFree;
static u8 sSpeakerVolume = 0x40;
static u8 sDpdSensitivity = 3;
static BOOL sMotorEnabled = TRUE;
static u32 sDataFormat[4];

void WPADInit(void) {}
void WPADShutdown(void) {}
s32 WPADGetStatus(void) { return 3; }  // WPAD_STATE_SETUP
void WPADRegisterAllocator(WPADAllocFunc a, WPADFreeFunc f) {
    sAlloc = a;
    sFree = f;
}
u32 WPADGetWorkMemorySize(void) { return 0x400; }
u8 WPADGetSensorBarPosition(void) { return 0; }

s32 WPADProbe(s32 chan, u32* type) {
    if (chan == 0 && sState.connected) {
        if (type)
            *type = WPAD_DEV_CORE;
        return WPAD_ERR_OK;
    }
    if (type)
        *type = WPAD_DEV_NOT_FOUND;
    return WPAD_ERR_NO_CONTROLLER;
}

static void connect_irq(void* arg) {
    s32 chan = (s32)(u32)arg;
    if (sConnectCB[chan])
        sConnectCB[chan](chan, WPAD_ERR_OK);
}

WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback cb) {
    WPADConnectCallback old = sConnectCB[chan & 3];
    sConnectCB[chan & 3] = cb;
    if (cb && chan == 0 && sState.connected)
        __web_queue_irq(connect_irq, (void*)0);
    return old;
}

WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback cb) {
    WPADExtensionCallback old = sExtensionCB[chan & 3];
    sExtensionCB[chan & 3] = cb;
    return old;
}

WPADSamplingCallback WPADSetSamplingCallback(s32 chan, WPADSamplingCallback cb) {
    WPADSamplingCallback old = sSamplingCB[chan & 3];
    sSamplingCB[chan & 3] = cb;
    return old;
}

u32 WPADGetDataFormat(s32 chan) { return sDataFormat[chan & 3]; }
s32 WPADSetDataFormat(s32 chan, u32 format) {
    sDataFormat[chan & 3] = format;
    return WPAD_ERR_OK;
}

typedef struct {
    s32 chan;
    WPADCallback cb;
    s32 result;
} CbArg;

static CbArg sCbArgs[32];
static int sCbNext;

static void cb_irq(void* p) {
    CbArg* a = (CbArg*)p;
    if (a->cb)
        a->cb(a->chan, a->result);
}

static void later(s32 chan, WPADCallback cb, s32 result) {
    if (!cb)
        return;
    CbArg* a = &sCbArgs[sCbNext++ & 31];
    a->chan = chan;
    a->cb = cb;
    a->result = result;
    __web_queue_irq(cb_irq, a);
}

s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback cb) {
    if (chan != 0 || !sState.connected) {
        later(chan, cb, WPAD_ERR_NO_CONTROLLER);
        return WPAD_ERR_NO_CONTROLLER;
    }
    memset(info, 0, sizeof(*info));
    info->dpd = TRUE;
    info->speaker = FALSE;
    info->battery = 4;
    info->led = 1;
    later(chan, cb, WPAD_ERR_OK);
    return WPAD_ERR_OK;
}

void WPADGetAccGravityUnit(s32 chan, u32 type, WPADAccGravityUnit* acc) {
    memset(acc, 0, sizeof(*acc));
}

void WPADDisconnect(s32 chan) {}
void WPADControlMotor(s32 chan, u32 command) {}
void WPADEnableMotor(BOOL enable) { sMotorEnabled = enable; }
BOOL WPADIsMotorEnabled(void) { return sMotorEnabled; }
s32 WPADControlLed(s32 chan, u8 flags, WPADCallback cb) {
    later(chan, cb, WPAD_ERR_OK);
    return WPAD_ERR_OK;
}
BOOL WPADSaveConfig(WPADSaveCallback cb) {
    if (cb)
        cb(0);
    return TRUE;
}

void WPADRead(s32 chan, WPADStatus* status) {
    memset(status, 0, sizeof(*status));
    status->err = (chan == 0 && sState.connected) ? WPAD_ERR_OK : WPAD_ERR_NO_CONTROLLER;
    status->button = (u16)sState.buttons;
}

void WPADSetAutoSamplingBuf(s32 chan, void* buffer, u32 len) {}
u32 WPADGetLatestIndexInBuf(s32 chan) { return 0; }
BOOL WPADIsSpeakerEnabled(s32 chan) { return FALSE; }
s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback cb) {
    later(chan, cb, WPAD_ERR_OK);
    return WPAD_ERR_OK;
}
u8 WPADGetSpeakerVolume(void) { return sSpeakerVolume; }
void WPADSetSpeakerVolume(u8 volume) { sSpeakerVolume = volume; }
BOOL WPADCanSendStreamData(s32 chan) { return FALSE; }
s32 WPADSendStreamData(s32 chan, void* data, u16 len) { return WPAD_ERR_OK; }
void WPADSetDpdSensitivity(u8 s) { sDpdSensitivity = s; }
u8 WPADGetDpdSensitivity(void) { return sDpdSensitivity; }
BOOL WPADSetSensorBarPower(BOOL enable) { return TRUE; }
BOOL WPADIsDpdEnabled(s32 chan) { return sDpdEnabled; }
s32 WPADControlDpd(s32 chan, u32 command, WPADCallback cb) {
    later(chan, cb, WPAD_ERR_OK);
    return WPAD_ERR_OK;
}
void WPADRecalibrate(s32 chan) {}
s32 WPADReadFaceData(s32 chan, void* dst, u16 size, u16 src, WPADCallback cb) {
    later(chan, cb, WPAD_ERR_NO_CONTROLLER);
    return WPAD_ERR_NO_CONTROLLER;
}
s32 WPADWriteFaceData(s32 chan, void* dst, u16 size, u16 src, WPADCallback cb) {
    later(chan, cb, WPAD_ERR_NO_CONTROLLER);
    return WPAD_ERR_NO_CONTROLLER;
}
BOOL WPADStartSimpleSync(void) { return FALSE; }
BOOL WPADStartFastSimpleSync(void) { return FALSE; }
BOOL WPADStopSimpleSync(void) { return FALSE; }
BOOL WPADStartClearDevice(void) { return FALSE; }
WPADSyncDeviceCallback WPADSetSimpleSyncCallback(WPADSyncDeviceCallback cb) { return NULL; }
WPADClearDeviceCallback WPADSetClearDeviceCallback(WPADClearDeviceCallback cb) { return NULL; }
void __WPADReconnect(BOOL exec) {}

// ---- PAD (GameCube controllers) ----

BOOL PADInit(void) { return TRUE; }
BOOL PADReset(u32 mask) { return TRUE; }
BOOL PADRecalibrate(u32 mask) { return TRUE; }
BOOL PADSync(void) { return TRUE; }

u32 PADRead(PADStatus* status) {
    for (int i = 0; i < PAD_MAX_CONTROLLER; i++) {
        memset(&status[i], 0, sizeof(PADStatus));
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return 0;
}

void PADClamp(PADStatus* status) {}
void PADClampCircle(PADStatus* status) {}
void PADControlMotor(s32 chan, u32 command) {}
void PADSetSpec(u32 spec) {}
void __PADFixBits(void) {}
BOOL PADSetSamplingCallback(void* cb) { return TRUE; }

void __web_input_init(void) {
    sState.connected = 1;
    webjs_input_poll(&sState);
    sState.connected = 1;
}
