#ifndef WEB_HAL_H
#define WEB_HAL_H

// Shared declarations of the web HLE layer (web/hal) and the JavaScript
// imports it calls (web/js/lib_wii.js).
//
// Memory is big-endian everywhere (web/tools/beswap). The JavaScript side
// therefore reads structures with DataView big-endian accessors; values passed
// as arguments are ordinary numbers.

#include <revolution/types.h>
#include <revolution/os.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- JavaScript imports ----
double webjs_now(void);                          // milliseconds, monotonic
void webjs_log(const char* text, u32 len);       // console output
// async: yields to the browser for `ms`, or until the next animation frame
// (forFrame) but at most `limitMs`
void webjs_wait(double ms, int forFrame, double limitMs);
void webjs_fatal(const char* text, u32 len);     // stops the program
double webjs_now_wall_local(void);               // ms since 1970, local time

// Threads (JSPI builds; see os_thread_web.c)
int webjs_jspi(void);
void webjs_thread_reset(int index);
void webjs_thread_switch(int from, int to, u32 stackBase, u32 stackEnd);  // async

// GPU (web/js/gpu.js)
u32 webjs_gpu_process(const void* data, u32 len);  // returns consumed bytes
void webjs_gpu_invalidate(u32 addr, u32 len);
void webjs_vi_present(u32 xfbAddr, u32 width, u32 height, int black);

// NAND file system (web/js/nand.js). Paths are NUL-terminated.
s32 webjs_fs_stat(const char* path, u32* out);  // out: [type, size]; type 1=file 2=dir
s32 webjs_fs_read(const char* path, u32 offset, void* dst, u32 len);
s32 webjs_fs_write(const char* path, u32 offset, const void* src, u32 len, int truncate);
s32 webjs_fs_create(const char* path, int isDir);
s32 webjs_fs_delete(const char* path);
s32 webjs_fs_rename(const char* from, const char* to);
s32 webjs_fs_readdir(const char* path, char* out, u32 outLen);  // NUL-separated names, returns count
s32 webjs_fs_truncate(const char* path, u32 size);

// Input (web/js/input.js): fills a WebPadState (big-endian, see input_web.c)
void webjs_input_poll(void* state);

// Audio (web/js/audio.js)
void webjs_audio_push(const s16* samples, u32 frames);  // interleaved stereo, native order
u32 webjs_audio_queued(void);                            // frames buffered

// ---- HAL internals ----
void __web_poll(int idle);
void __web_irq_enter(OSContext* saved[1]);
void __web_irq_leave(OSContext* saved[1]);
void __web_report(const char* fmt, ...);
void __web_fatal(const char* fmt, ...);

extern volatile int __web_irq_pending;

// Deferred "interrupt" work: run fn(arg) at the next interrupt point, in
// interrupt context (like an IOS reply or a DSP interrupt on hardware).
void __web_queue_irq(void (*fn)(void*), void* arg);

// Audio interface (ax_web.c): time of the next due DMA period, and the
// interrupt that runs due periods (returns how many ran).
double __web_ai_next(void);
int __web_ai_interrupt(double now);

// Called by the VI emulation once per retrace from __web_poll.
void __web_vi_retrace(void);
double __web_vi_next_retrace(void);

#ifdef __cplusplus
}
#endif

#endif
