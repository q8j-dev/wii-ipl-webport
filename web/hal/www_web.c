// The Opera browser library (RVL WWW) for the web build.
//
// On the Wii, Wii Settings is a set of HTML pages rendered by Opera, which the
// System Menu loads as an RSO module (compiled PowerPC code). Here the host
// browser does Opera's job (web/js/www.js): pages are laid out in a hidden
// iframe, their scripts run there, and they are rasterized into the surface
// buffer the System Menu gave to WWWSurfaceInit; the menu then converts and
// composites that buffer exactly as it does with Opera's output.
//
// This file implements the WWW* entry points the menu calls (resolved by name
// in place of the RSO's exports) and the JavaScript plugin callbacks (the
// "wiiSetting" and "wiiTrasition" objects), and exports the functions that
// www.js uses to call back into the menu's C code.

#include "web.h"

#include <revolution/mem.h>
#include <revolution/os.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    int x, y, w, h;
} WebRect;

typedef void (*FlushCallback)(WebRect*, int);
typedef BOOL (*NotifyCallback)(void* browser, void* window, int event, void* data);
typedef int (*GetterSetter)(void* obj, const char* name, void* value);
typedef int (*CallFn)(void* thisObj, void* funcObj, int argc, void* argv, void* result);

// JavaScript imports (lib_wii.js -> www.js)
void webjs_www_surface(int w, int h, u32 rowSize, void* raster);
u32 webjs_www_create_browser(void);
void webjs_www_terminate(void);
void webjs_www_create_window(void);
void webjs_www_close_window(void);
void webjs_www_set_rect(int x, int y, int w, int h);
void webjs_www_open(const char* url);
void webjs_www_back(void);
void webjs_www_run_slice(void);
int webjs_www_take_frame(void);
void webjs_www_mouse(int cmd, int x, int y, u32 button);
void webjs_www_key(int cmd, int sym);
void webjs_www_commit_ime(int id, const char* text);
void webjs_www_add_plugin(const void* cap);
void webjs_www_protocol_write(void* request, const void* data, u32 len);
void webjs_www_protocol_done(void* request, int ok);
u32 webjs_www_make_function(u32 call, u32 construct);
void webjs_www_make_object(void* priv, u32 getter, u32 setter);

static u32 sBrowser[4];
static u32 sWindow[4];
static NotifyCallback sNotify;
static FlushCallback sFlush;
static int sFlushData;
static int sWidth, sHeight;

// ---------------------------------------------------------------------------
// Calls from JavaScript into the menu run on the calling OS thread's stack,
// nested in a JavaScript frame, where switching threads is impossible: the
// scheduler is held off until they return (interrupts are still delivered).

int __web_nested_js;

static void nested_enter(void) {
    OSDisableScheduler();
    __web_nested_js++;
}

static void nested_leave(void) {
    __web_nested_js--;
    OSEnableScheduler();
}

int webx_www_getset(u32 fn, u32 obj, const char* name, void* value) {
    nested_enter();
    int r = ((GetterSetter)fn)((void*)obj, name, value);
    nested_leave();
    return r;
}

int webx_www_call(u32 fn, u32 thisObj, u32 funcObj, int argc, void* argv, void* result) {
    nested_enter();
    int r = ((CallFn)fn)((void*)thisObj, (void*)funcObj, argc, argv, result);
    nested_leave();
    return r;
}

void webx_www_notify(int event, void* data) {
    if (!sNotify)
        return;
    nested_enter();
    sNotify(sBrowser, sWindow, event, data);
    nested_leave();
}

void* webx_www_malloc(u32 size) { return malloc(size); }
void webx_www_free(void* p) { free(p); }

// ---------------------------------------------------------------------------
// Surface

static int www_SurfaceInit(int w, int h, u32 rowSize, int unused, void* raster) {
    sWidth = w;
    sHeight = h;
    webjs_www_surface(w, h, rowSize, raster);
    return 0;
}

static int www_SurfaceSetFlushCallback(FlushCallback cb, int data) {
    sFlush = cb;
    sFlushData = data;
    return 0;
}

static void www_SurfaceAddFont(const char* name) {}

// A new picture in the raster buffer is announced through the flush callback.
static void www_SurfaceUpdateScreen(u32 flags) {
    if (webjs_www_take_frame() && sFlush) {
        WebRect r = {0, 0, sWidth, sHeight};
        sFlush(&r, sFlushData);
    }
}

static void www_SurfaceMouseEvt(int cmd, int x, int y, int attrib, u32 button, u32 options) {
    webjs_www_mouse(cmd, x, y, button);
}

static void www_SurfaceKeyboardEvt(int cmd, int sym, u32 options) { webjs_www_key(cmd, sym); }

// ---------------------------------------------------------------------------
// Browser and window

static void www_CreateBrowser(void** browser, NotifyCallback notify, const char** fonts, const char* arc) {
    sNotify = notify;
    webjs_www_create_browser();
    *browser = sBrowser;
}

static BOOL www_TerminateBrowser(void* browser) {
    webjs_www_terminate();
    sNotify = NULL;
    sFlush = NULL;
    return FALSE;
}

static BOOL www_RunSlice(void* browser) {
    webjs_www_run_slice();
    return TRUE;
}

// Returns FALSE on success.
static BOOL www_CreateBrowserWindow(void* browser, void** window, int flags) {
    webjs_www_create_window();
    *window = sWindow;
    return FALSE;
}

static BOOL www_CloseBrowserWindow(void* browser, void* window) {
    webjs_www_close_window();
    return FALSE;
}

static void www_SetBrowserWindowRect(void* window, WebRect* r) { webjs_www_set_rect(r->x, r->y, r->w, r->h); }
static void www_SetImageMode(void* window, int fmt) {}
static void www_SetRenderingMode(void* window, int mode) {}
static void www_WindowNop(void* window) {}
static void www_PrevPage(void* window) { webjs_www_back(); }
static void www_OpenUrl(void* window, const char* url) { webjs_www_open(url); }
static void www_CommitIme(int id, const char* text) { webjs_www_commit_ime(id, text); }
static void www_UpdateIme(int id, const char* text, u32 options) {}

// ---------------------------------------------------------------------------
// Memory: Opera's heap. The menu only passes these functions on.

static MEMHeapHandle sHeap;

static void* www_alloc(u32 size) { return sHeap ? MEMAllocFromExpHeapEx(sHeap, size, 4) : NULL; }
static void www_free(void* p) {
    if (sHeap && p)
        MEMFreeToExpHeap(sHeap, p);
}
static int www_avail(void) { return sHeap ? (int)MEMGetAllocatableSizeForExpHeap(sHeap) : 0; }

static void www_GetBrowserAllocationFunctions(void* buf, u32 size, void** allocOut, void (**freeOut)(void*), int (**availOut)(void)) {
    sHeap = MEMCreateExpHeap(buf, size);
    *allocOut = (void*)www_alloc;
    *freeOut = www_free;
    *availOut = www_avail;
}

static void www_SetAllocationFunctions(void* a, void* b, void* c, void* d, void* e, void* f, void* g) {}

// ---------------------------------------------------------------------------
// JavaScript plugins (Opera's jsplugin interface)

typedef union {
    int i;
    f64 d;
    void* p;
} ValueData;

// Creates a function object; `call`/`construct` are called when JavaScript
// calls it or uses it with `new`.
static int js_globalGet(void* obj, int a, int b, u32 call, u32 construct, const char* name, int c, ValueData* out) {
    out->i = (int)webjs_www_make_function(call, construct);
    return 0;
}

// Creates an object whose property accesses go to getter/setter.
static int js_construct(void* obj, u32 getter, u32 setter, int a, ValueData* out) {
    u32* priv = (u32*)malloc(16);
    memset(priv, 0, 16);
    webjs_www_make_object(priv, getter, setter);
    out->p = priv;
    return 0;
}

static struct {
    void* globalGet;
    void* construct;
} sCallbacks = {(void*)js_globalGet, (void*)js_construct};

static void www_AddJSPlugin(const char* name, const void* cap, void** callbacks) {
    *callbacks = &sCallbacks;
    webjs_www_add_plugin(cap);
}

static int www_AddProtocol(const char* name) { return 0; }

static u32 www_ProtocolWrite(void* request, const void* data, u32 len) {
    webjs_www_protocol_write(request, data, len);
    return 0;
}

static u32 www_ProtocolFinished(void* request) {
    webjs_www_protocol_done(request, 1);
    return 0;
}

static u32 www_ProtocolFailed(void* request) {
    webjs_www_protocol_done(request, 0);
    return 0;
}

// ---------------------------------------------------------------------------

static int www_unimplemented(void) {
    __web_report("[www] unimplemented WWW function called");
    return 0;
}

static const struct {
    const char* name;
    void* fn;
} kExports[] = {
    {"WWWSurfaceInit", (void*)www_SurfaceInit},
    {"WWWSurfaceSetFlushCallback", (void*)www_SurfaceSetFlushCallback},
    {"WWWSurfaceAddFont", (void*)www_SurfaceAddFont},
    {"WWWSurfaceUpdateScreen", (void*)www_SurfaceUpdateScreen},
    {"WWWSurfaceMouseEvt", (void*)www_SurfaceMouseEvt},
    {"WWWSurfaceKeyboardEvt", (void*)www_SurfaceKeyboardEvt},
    {"WWWCreateBrowser", (void*)www_CreateBrowser},
    {"WWWTerminateBrowser", (void*)www_TerminateBrowser},
    {"WWWRunSlice", (void*)www_RunSlice},
    {"WWWCreateBrowserWindow", (void*)www_CreateBrowserWindow},
    {"WWWCloseBrowserWindow", (void*)www_CloseBrowserWindow},
    {"WWWSetBrowserWindowRect", (void*)www_SetBrowserWindowRect},
    {"WWWSetImageMode", (void*)www_SetImageMode},
    {"WWWSetRenderingMode", (void*)www_SetRenderingMode},
    {"WWWRaiseBrowserWindow", (void*)www_WindowNop},
    {"WWWLowerBrowserWindow", (void*)www_WindowNop},
    {"WWWShowBrowserWindow", (void*)www_WindowNop},
    {"WWWHideBrowserWindow", (void*)www_WindowNop},
    {"WWWSetFocus", (void*)www_WindowNop},
    {"WWWNextPage", (void*)www_WindowNop},
    {"WWWPrevPage", (void*)www_PrevPage},
    {"WWWOpenUrl", (void*)www_OpenUrl},
    {"WWWCommitIme", (void*)www_CommitIme},
    {"WWWUpdateIme", (void*)www_UpdateIme},
    {"WWWGetBrowserAllocationFunctions", (void*)www_GetBrowserAllocationFunctions},
    {"WWWSetAllocationFunctions", (void*)www_SetAllocationFunctions},
    {"WWWAddJSPlugin", (void*)www_AddJSPlugin},
    {"WWWAddProtocol", (void*)www_AddProtocol},
    {"WWWProtocolWrite", (void*)www_ProtocolWrite},
    {"WWWProtocolFinished", (void*)www_ProtocolFinished},
    {"WWWProtocolFailed", (void*)www_ProtocolFailed},
};

// Replaces RSOFindExportSymbolAddr for the WWW module (www_surface.cpp).
u32 __web_www_lookup(const char* name) {
    for (u32 i = 0; i < sizeof(kExports) / sizeof(kExports[0]); i++)
        if (strcmp(kExports[i].name, name) == 0)
            return (u32)kExports[i].fn;
    return (u32)www_unimplemented;
}
