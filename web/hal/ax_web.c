// DSP HLE: the AX microcode's command list processor, and the audio
// interface (AI) DMA that paces it.
//
// AX (unchanged decompiled code) builds a command list each 3 ms audio frame
// and mails its address to the DSP. Here the list is executed right away:
// voices are decoded (DSP-ADPCM, PCM16, PCM8), resampled, enveloped and mixed
// into the main and aux buses, and the final stereo frame is written where the
// list says, as on hardware. Parameter blocks are updated in memory so AX sees
// the same state the DSP would leave (positions, decoder history, volumes).
//
// The AI "DMA interrupt" fires on a 3 ms grid of wall-clock time from
// __web_poll; each one hands the buffer AX queued to the browser (audio.js)
// and calls AX's callback, which starts the next frame.

#include "web.h"

#include <private/ax.h>
#include <private/dsp.h>
#include <revolution/ai.h>
#include <revolution/ax.h>
#include <revolution/dsp.h>

#include <string.h>

#define N AX_SAMPLES_PER_FRAME  // 96 samples, 3 ms at 32 kHz
#define FRAME_MS 3.0

// ---------------------------------------------------------------------------
// DSP mailbox and tasks

static int sDspInit;
static DSPTaskInfo* sTask;
static u32 sMailState;     // 1: next mail is a command list address
static int sResumePending;  // a command list ran; the task's resume callback is due

static void run_command_list(const u16* cl);

void DSPInit(void) { sDspInit = 1; }
BOOL DSPCheckInit(void) { return sDspInit; }
u32 DSPCheckMailToDSP(void) { return 0; }
u32 DSPCheckMailFromDSP(void) { return 0; }
u32 DSPReadMailFromDSP(void) { return 0; }
u32 DSPReadCPUToDSPMbox(void) { return 0; }
void DSPAssertInt(void) {}
void DSPHalt(void) {}
void DSPReset(void) {}
void DSPUnhalt(void) {}
u32 DSPGetDMAStatus(void) { return 0; }

static void resume_task(void) {
    if (sResumePending && sTask && sTask->res_cb) {
        sResumePending = 0;
        sTask->res_cb(sTask);
    }
}

static void resume_irq(void* arg) { resume_task(); }

void DSPSendMailToDSP(u32 mail) {
    if (sMailState == 1) {
        sMailState = 0;
        run_command_list((const u16*)(mail & 0x3FFFFFFF));
        sResumePending = 1;
        __web_queue_irq(resume_irq, NULL);
        return;
    }
    if ((mail >> 16) == 0xBABE)
        sMailState = 1;
}

DSPTaskInfo* DSPAddTask(DSPTaskInfo* task) {
    sTask = task;
    if (task->init_cb)
        task->init_cb(task);
    return task;
}

DSPTaskInfo* DSPAssertTask(DSPTaskInfo* task) { return task; }

// ---------------------------------------------------------------------------
// Audio interface

static AIDCallback sDmaCallback;
static u32 sDmaAddr, sDmaLen;
static int sAiRunning;
static double sAiNext;

void AIInit(void* stack) {}
void AIReset(void) {}
AIDCallback AIRegisterDMACallback(AIDCallback cb) {
    AIDCallback old = sDmaCallback;
    sDmaCallback = cb;
    return old;
}
void AIInitDMA(u32 addr, u32 length) {
    sDmaAddr = addr & 0x3FFFFFFF;
    sDmaLen = length;
}
void AIStartDMA(void) {
    sAiRunning = 1;
    sAiNext = 0;
}
void AIStopDMA(void) { sAiRunning = 0; }
u32 AIGetDMABytesLeft(void) { return 0; }
u32 AIGetDMAStartAddr(void) { return sDmaAddr; }
u32 AIGetDMALength(void) { return sDmaLen; }
BOOL AICheckInit(void) { return TRUE; }
void AISetDSPSampleRate(u32 rate) {}
u32 AIGetDSPSampleRate(void) { return 1; }

// Audio is produced ahead of real time: when less than AI_LOW_MS is buffered,
// periods run back to back (a burst) until AI_HIGH_MS is, so that wakeups for
// audio stay rare. Each interrupt runs one period; the scheduler then lets the
// sound thread (woken by AX's callback) process it before the next one.
#define AI_LOW_MS 24.0
#define AI_HIGH_MS 42.0

static int sBurst;

double __web_ai_next(void) {
    if (!sAiRunning || !sDmaCallback)
        return 1e300;
    return sAiNext - (sBurst ? AI_HIGH_MS : AI_LOW_MS);
}

// Called by __web_poll in interrupt context when __web_ai_next() is due.
int __web_ai_interrupt(double now) {
    if (!sAiRunning || !sDmaCallback)
        return 0;
    if (sAiNext == 0 || now - sAiNext > 250.0)
        sAiNext = now;  // first period, or resynchronize after a stall
    if (sAiNext > now + AI_HIGH_MS) {
        sBurst = 0;
        return 0;
    }
    sBurst = 1;
    if (sDmaAddr && sDmaLen)
        webjs_audio_push((const s16*)sDmaAddr, sDmaLen / 4);
    sAiNext += FRAME_MS;
    sDmaCallback();
    // The DSP finishes long before the next period.
    resume_task();
    if (sAiNext > now + AI_HIGH_MS)
        sBurst = 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Mixer

enum { BUF_L, BUF_R, BUF_S, BUF_AL, BUF_AR, BUF_AS, BUF_BL, BUF_BR, BUF_BS, BUF_CL, BUF_CR, BUF_CS, BUF_COUNT };
static s32 sBuf[BUF_COUNT][N];

static inline u32 hilo(u16 hi, u16 lo) { return ((u32)hi << 16) | lo; }
static inline s32 clamp16(s32 v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; }
static inline u8 mem8(u32 addr) { return *(const u8*)(addr & 0x3FFFFFFF); }

// Sample fetch through the DSP's "accelerator": decodes one sample at the
// current address and handles the end of the sample (loop or stop).
typedef struct {
    AXPB* pb;
    u32 cur, end, loop;
    int running;
} Acc;

static s16 acc_sample(Acc* a) {
    if (!a->running)
        return 0;
    AXPB* pb = a->pb;
    u32 at = a->cur;
    s32 val;
    switch (pb->addr.format) {
        case AX_SAMPLE_FORMAT_DSP_ADPCM: {
            if ((a->cur & 15) == 0) {
                pb->adpcm.pred_scale = mem8(a->cur >> 1);
                a->cur += 2;
                at = a->cur;
            }
            u16 ps = pb->adpcm.pred_scale;
            s32 scale = 1 << (ps & 15);
            int idx = (ps >> 4) & 7;
            s32 c1 = (s16)pb->adpcm.a[idx][0], c2 = (s16)pb->adpcm.a[idx][1];
            u8 b = mem8(a->cur >> 1);
            s32 nib = (a->cur & 1) ? (b & 15) : (b >> 4);
            if (nib >= 8)
                nib -= 16;
            val = scale * nib + ((0x400 + c1 * (s16)pb->adpcm.yn1 + c2 * (s16)pb->adpcm.yn2) >> 11);
            val = clamp16(val);
            pb->adpcm.yn2 = pb->adpcm.yn1;
            pb->adpcm.yn1 = (u16)val;
            a->cur++;
            break;
        }
        case AX_SAMPLE_FORMAT_PCM_S16:
            val = (s16)((mem8(a->cur * 2) << 8) | mem8(a->cur * 2 + 1));
            a->cur++;
            break;
        case AX_SAMPLE_FORMAT_PCM_S8:
            val = (s16)(mem8(a->cur) << 8);
            a->cur++;
            break;
        default:
            a->running = 0;
            return 0;
    }
    if (at >= a->end) {
        if (pb->addr.loopFlag) {
            if (pb->type != AX_VOICE_STREAM) {
                pb->adpcm.pred_scale = pb->adpcmLoop.loop_pred_scale;
                pb->adpcm.yn1 = pb->adpcmLoop.loop_yn1;
                pb->adpcm.yn2 = pb->adpcmLoop.loop_yn2;
            }
            a->cur = a->loop;
        } else {
            a->running = 0;
        }
    }
    return (s16)val;
}

static void mix_add(s32* out, const s16* in, u16* vol, u16* delta, s16* dpop, int ramp) {
    u16 v = *vol;
    s16 d = ramp ? (s16)*delta : 0;
    if (v == 0 && d == 0)
        return;
    s32 s = 0;
    for (int i = 0; i < N; i++) {
        s = clamp16(((s32)in[i] * v) >> 15);
        out[i] += s;
        v = (u16)(v + d);
    }
    *vol = v;
    *dpop = (s16)s;
}

static void process_voice(AXPB* pb) {
    if (pb->state != AX_VOICE_RUN)
        return;

    Acc a;
    a.pb = pb;
    a.cur = hilo(pb->addr.currentAddressHi, pb->addr.currentAddressLo);
    a.end = hilo(pb->addr.endAddressHi, pb->addr.endAddressLo);
    a.loop = hilo(pb->addr.loopAddressHi, pb->addr.loopAddressLo);
    a.running = 1;

    // Sample rate conversion (linear interpolation over the last two input
    // samples; the 4-tap filters are approximated).
    s16 in[N];
    u32 ratio = hilo(pb->src.ratioHi, pb->src.ratioLo);
    u32 frac = pb->src.currentAddressFrac;
    s16 h0 = (s16)pb->src.last_samples[2], h1 = (s16)pb->src.last_samples[3];
    s16 hist0 = (s16)pb->src.last_samples[0], hist1 = (s16)pb->src.last_samples[1];
    if (pb->srcSelect == AX_SRC_TYPE_NONE)
        ratio = 0x10000;
    for (int i = 0; i < N; i++) {
        in[i] = (s16)(h0 + (((s32)(h1 - h0) * (s32)frac) >> 16));
        frac += ratio;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            hist0 = hist1;
            hist1 = h0;
            h0 = h1;
            h1 = acc_sample(&a);
        }
    }
    pb->src.currentAddressFrac = (u16)frac;
    pb->src.last_samples[0] = (u16)hist0;
    pb->src.last_samples[1] = (u16)hist1;
    pb->src.last_samples[2] = (u16)h0;
    pb->src.last_samples[3] = (u16)h1;

    pb->addr.currentAddressHi = (u16)(a.cur >> 16);
    pb->addr.currentAddressLo = (u16)a.cur;
    if (!a.running)
        pb->state = AX_VOICE_STOP;

    // Volume envelope.
    u16 ve = pb->ve.currentVolume;
    s16 ved = pb->ve.currentDelta;
    for (int i = 0; i < N; i++) {
        in[i] = (s16)clamp16(((s32)in[i] * ve) >> 15);
        ve = (u16)(ve + ved);
    }
    pb->ve.currentVolume = ve;

    // One-pole low pass filter.
    if (pb->lpf.on) {
        s32 y = (s16)pb->lpf.yn1;
        s32 a0 = pb->lpf.a0, b0 = pb->lpf.b0;
        for (int i = 0; i < N; i++) {
            y = clamp16((in[i] * a0 + y * b0) >> 15);
            in[i] = (s16)y;
        }
        pb->lpf.yn1 = (u16)y;
    }

    u32 mc = pb->mixerCtrl;
    AXPBMIX* m = &pb->mix;
    AXPBDPOP* dp = &pb->dpop;
    if (mc & AX_MIXER_CTRL_L)
        mix_add(sBuf[BUF_L], in, &m->vL, &m->vDeltaL, &dp->aL, mc & AX_MIXER_CTRL_DELTA);
    if (mc & AX_MIXER_CTRL_R)
        mix_add(sBuf[BUF_R], in, &m->vR, &m->vDeltaR, &dp->aR, mc & AX_MIXER_CTRL_DELTA);
    if (mc & AX_MIXER_CTRL_S)
        mix_add(sBuf[BUF_S], in, &m->vS, &m->vDeltaS, &dp->aS, mc & AX_MIXER_CTRL_DELTA_S);
    if (mc & AX_MIXER_CTRL_AL)
        mix_add(sBuf[BUF_AL], in, &m->vAuxAL, &m->vDeltaAuxAL, &dp->aAuxAL, mc & AX_MIXER_CTRL_DELTA_A);
    if (mc & AX_MIXER_CTRL_AR)
        mix_add(sBuf[BUF_AR], in, &m->vAuxAR, &m->vDeltaAuxAR, &dp->aAuxAR, mc & AX_MIXER_CTRL_DELTA_A);
    if (mc & AX_MIXER_CTRL_AS)
        mix_add(sBuf[BUF_AS], in, &m->vAuxAS, &m->vDeltaAuxAS, &dp->aAuxAS, mc & AX_MIXER_CTRL_DELTA_AS);
    if (mc & AX_MIXER_CTRL_BL)
        mix_add(sBuf[BUF_BL], in, &m->vAuxBL, &m->vDeltaAuxBL, &dp->aAuxBL, mc & AX_MIXER_CTRL_DELTA_B);
    if (mc & AX_MIXER_CTRL_BR)
        mix_add(sBuf[BUF_BR], in, &m->vAuxBR, &m->vDeltaAuxBR, &dp->aAuxBR, mc & AX_MIXER_CTRL_DELTA_B);
    if (mc & AX_MIXER_CTRL_BS)
        mix_add(sBuf[BUF_BS], in, &m->vAuxBS, &m->vDeltaAuxBS, &dp->aAuxBS, mc & AX_MIXER_CTRL_DELTA_BS);
    if (mc & AX_MIXER_CTRL_CL)
        mix_add(sBuf[BUF_CL], in, &m->vAuxCL, &m->vDeltaAuxCL, &dp->aAuxCL, mc & AX_MIXER_CTRL_DELTA_C);
    if (mc & AX_MIXER_CTRL_CR)
        mix_add(sBuf[BUF_CR], in, &m->vAuxCR, &m->vDeltaAuxCR, &dp->aAuxCR, mc & AX_MIXER_CTRL_DELTA_C);
    if (mc & AX_MIXER_CTRL_CS)
        mix_add(sBuf[BUF_CS], in, &m->vAuxCS, &m->vDeltaAuxCS, &dp->aAuxCS, mc & AX_MIXER_CTRL_DELTA_CS);
}

static void process_pb_list(u32 addr) {
    for (int guard = 0; addr && guard < AX_VOICE_MAX * 2; guard++) {
        AXPB* pb = (AXPB*)(addr & 0x3FFFFFFF);
        process_voice(pb);
        addr = hilo(pb->nextHi, pb->nextLo);
    }
}

static void setup(u32 addr) {
    // Initial values (depop fade) of each bus: s32 value, s16 delta.
    const u8* p = (const u8*)(addr & 0x3FFFFFFF);
    for (int b = 0; b < BUF_COUNT; b++) {
        const u16* w = (const u16*)p + b * 3;  // AXSPB: dpopXHi, dpopXLo, dpopXDelta
        s32 v = (s32)hilo(w[0], w[1]);
        s16 d = (s16)w[2];
        for (int i = 0; i < N; i++) {
            sBuf[b][i] = v;
            v += d;
        }
    }
}

static void add_to_lr(u32 addr, int neg) {
    const s32* p = (const s32*)(addr & 0x3FFFFFFF);
    for (int i = 0; i < N; i++) {
        s32 v = p[i];
        if (neg)
            v = -v;
        sBuf[BUF_L][i] += v;
        sBuf[BUF_R][i] += v;
    }
}

// Uploads an aux bus for the CPU's effect processing and mixes the processed
// result of the previous frame back into the main buses.
static void mix_aux(int bus, u16 volume, u32 upload, u32 download) {
    if (upload) {
        s32* p = (s32*)(upload & 0x3FFFFFFF);
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < N; i++)
                *p++ = sBuf[bus + c][i];
    }
    if (download) {
        const s32* p = (const s32*)(download & 0x3FFFFFFF);
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < N; i++)
                sBuf[BUF_L + c][i] += (s32)(((s64)*p++ * volume) >> 15);
    }
}

static void output(u16 volume, u32 surround, u32 lr) {
    if (surround) {
        s32* s = (s32*)(surround & 0x3FFFFFFF);
        for (int i = 0; i < N; i++)
            s[i] = sBuf[BUF_S][i];
    }
    // Interleaved R, L (as the DSP writes it for the AI).
    s16* out = (s16*)(lr & 0x3FFFFFFF);
    for (int i = 0; i < N; i++) {
        out[i * 2] = (s16)clamp16((s32)(((s64)sBuf[BUF_R][i] * volume) >> 15));
        out[i * 2 + 1] = (s16)clamp16((s32)(((s64)sBuf[BUF_L][i] * volume) >> 15));
    }
}

enum {
    CMD_SETUP,
    CMD_ADD_TO_LR,
    CMD_SUB_TO_LR,
    CMD_ADD_SUB_TO_LR,
    CMD_PROCESS,
    CMD_MIX_AUXA,
    CMD_MIX_AUXB,
    CMD_MIX_AUXC,
    CMD_UPL_AUXA_MIX_LRSC,
    CMD_UPL_AUXB_MIX_LRSC,
    CMD_COMPRESSOR,
    CMD_OUTPUT,
    CMD_OUTPUT_DPL2,
    CMD_WM_OUTPUT,
    CMD_END
};

static void run_command_list(const u16* cl) {
    const u16* end = cl + AX_CL_SIZE / 2;
#define W16() (*cl++)
#define W32() (cl += 2, hilo(cl[-2], cl[-1]))
    while (cl < end) {
        u16 cmd = W16();
        switch (cmd) {
            case CMD_SETUP:
                setup(W32());
                break;
            case CMD_ADD_TO_LR:
                add_to_lr(W32(), 0);
                break;
            case CMD_SUB_TO_LR:
                add_to_lr(W32(), 1);
                break;
            case CMD_ADD_SUB_TO_LR:
                add_to_lr(W32(), 0);
                break;
            case CMD_PROCESS:
                process_pb_list(W32());
                break;
            case CMD_MIX_AUXA:
            case CMD_MIX_AUXB:
            case CMD_MIX_AUXC: {
                u16 vol = W16();
                u32 up = W32();
                u32 down = W32();
                mix_aux(BUF_AL + (cmd - CMD_MIX_AUXA) * 3, vol, up, down);
                break;
            }
            case CMD_UPL_AUXA_MIX_LRSC:
            case CMD_UPL_AUXB_MIX_LRSC: {
                u16 vol = W16();
                u32 up = W32();
                W32();  // DPL2 input
                u32 down = W32();
                W32();
                W32();
                W32();
                mix_aux(BUF_AL + (cmd - CMD_UPL_AUXA_MIX_LRSC) * 3, vol, up, down);
                break;
            }
            case CMD_COMPRESSOR:
                W16();
                W16();
                W32();
                break;
            case CMD_WM_OUTPUT: {
                // Wii Remote speakers are not emulated: keep their buffers silent.
                for (int r = 0; r < 4; r++) {
                    u32 a = W32();
                    if (a)
                        memset((void*)(a & 0x3FFFFFFF), 0, AX_SAMPLES_PER_FRAME_RMT * 2);
                }
                break;
            }
            case CMD_OUTPUT:
            case CMD_OUTPUT_DPL2: {
                u16 vol = W16();
                u32 surround = W32();
                u32 lr = W32();
                output(vol, surround, lr);
                break;
            }
            case CMD_END:
                return;
            default:
                __web_report("[ax] unknown command %d", cmd);
                return;
        }
    }
#undef W16
#undef W32
}
