// OSThread contexts as coroutines.
//
// OSThread.c keeps its scheduler (run queues, priorities, mutex priority
// inheritance); where the hardware saved and loaded register contexts it calls
// __web_switch_thread, which swaps to the coroutine of the selected thread.
// Each thread has its own C stack (allocated here, larger than the Wii stacks
// since wasm frames are laid out differently).
//
// Two stack-switching mechanisms are supported, chosen by the link:
// - JSPI: every thread runs on its own wasm stack, entered through the
//   promising export webx_thread_entry; switching suspends the current stack
//   and resumes the target (webjs_thread_switch in lib_wii.js).
// - Asyncify: Emscripten fibers (each with an Asyncify unwind buffer).

#include "web.h"

#include <private/os.h>
#include <revolution/os.h>

#include <stdlib.h>
#include <string.h>

// emscripten/fiber.h, declared here because the HAL is compiled against the
// SDK headers. The structure is only touched by Emscripten's runtime (which
// sees it in its own byte order), so its contents are opaque here.
typedef struct {
    u32 words[8];
} emscripten_fiber_t;
void emscripten_fiber_init(emscripten_fiber_t* fiber, void (*entry)(void*), void* arg, void* cStack, unsigned long cStackSize,
                           void* asyncStack, unsigned long asyncStackSize);
void emscripten_fiber_init_from_current_context(emscripten_fiber_t* fiber, void* asyncStack, unsigned long asyncStackSize);
void emscripten_fiber_swap(emscripten_fiber_t* oldFiber, emscripten_fiber_t* newFiber);

#define FIBER_C_STACK (256 * 1024)
#define FIBER_ASYNC_STACK (128 * 1024)
#define MAX_THREADS 128

typedef struct WebThread {
    OSThread* thread;
    emscripten_fiber_t fiber;
    void* (*func)(void*);
    void* param;
    void* cStack;
    void* asyncStack;
    int started;
} WebThread;

static WebThread sThreads[MAX_THREADS];
static int sJspi;
static WebThread sMainThread;
static WebThread* sRunning;  // fiber currently executing
static WebThread* sDead;     // exited thread whose fiber can be reclaimed

static WebThread* find(OSThread* t) {
    if (sMainThread.thread == t)
        return &sMainThread;
    for (int i = 0; i < MAX_THREADS; i++)
        if (sThreads[i].thread == t)
            return &sThreads[i];
    return NULL;
}

static void reclaim(WebThread* w) {
    free(w->cStack);
    free(w->asyncStack);
    memset(w, 0, sizeof(*w));
}

static void thread_entry(void* arg);

// JSPI: first entry of a thread, on its own stack (see webjs_thread_switch).
void webx_thread_entry(int index) {
    thread_entry(&sThreads[index]);
}

static void thread_entry(void* arg) {
    WebThread* w = (WebThread*)arg;
    void* ret = w->func(w->param);
    OSExitThread(ret);
    __web_fatal("thread returned from OSExitThread");
}

void __web_threads_init(void) {
    static char mainAsync[512 * 1024];
    memset(&sMainThread, 0, sizeof(sMainThread));
    sJspi = webjs_jspi();
    if (!sJspi)
        emscripten_fiber_init_from_current_context(&sMainThread.fiber, mainAsync, sizeof(mainAsync));
    sMainThread.started = 1;
    sRunning = &sMainThread;
}

void __web_thread_create(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize) {
    if (sMainThread.thread == NULL && thread == OSGetCurrentThread()) {
        // never happens: the default thread is never created
    }
    WebThread* w = find(thread);
    if (w && w == sRunning)
        __web_fatal("OSCreateThread on the running thread");
    if (w) {
        if (w == sDead)
            sDead = NULL;
        reclaim(w);
    } else {
        for (int i = 0; i < MAX_THREADS && !w; i++)
            if (sThreads[i].thread == NULL)
                w = &sThreads[i];
        if (!w)
            __web_fatal("too many threads");
    }
    w->thread = thread;
    w->func = func;
    w->param = param;
    w->cStack = malloc(FIBER_C_STACK);
    if (!w->cStack)
        __web_fatal("out of memory for thread stacks");
    if (sJspi) {
        webjs_thread_reset(w - sThreads);
        return;
    }
    w->asyncStack = malloc(FIBER_ASYNC_STACK);
    if (!w->asyncStack)
        __web_fatal("out of memory for thread stacks");
    emscripten_fiber_init(&w->fiber, thread_entry, w, w->cStack, FIBER_C_STACK, w->asyncStack, FIBER_ASYNC_STACK);
}

// The default thread (OSThread.c's DefaultThread) runs on the main context.
static void bind_main(void) {
    if (sMainThread.thread == NULL)
        sMainThread.thread = OSGetCurrentThread();
}

void __web_switch_thread(OSThread* to) {
    bind_main();
    WebThread* target = find(to);
    if (!target)
        __web_fatal("switch to unknown thread %p", to);
    if (target == sRunning)
        return;

    WebThread* from = sRunning;
    OSThread* fromThread = from->thread;
    if (fromThread && (fromThread->state == OS_THREAD_STATE_DEAD || fromThread->state == OS_THREAD_STATE_UNINITIALIZED) &&
        from != &sMainThread) {
        // Exited: its fiber never runs again. Free it once we are off its stack.
        sDead = from;
    }
    sRunning = target;
    if (sJspi) {
        int toIndex = target == &sMainThread ? MAX_THREADS : (int)(target - sThreads);
        int fromIndex = from == &sMainThread ? MAX_THREADS : (int)(from - sThreads);
        u32 base = (u32)target->cStack + FIBER_C_STACK;
        webjs_thread_switch(fromIndex, toIndex, base & ~15u, (u32)target->cStack);
    } else {
        emscripten_fiber_swap(&from->fiber, &target->fiber);
    }

    // Back on this fiber.
    if (sDead && sDead != sRunning) {
        WebThread* d = sDead;
        sDead = NULL;
        if (d->thread && (d->thread->state == OS_THREAD_STATE_DEAD || d->thread->state == OS_THREAD_STATE_UNINITIALIZED)) {
            OSThread* t = d->thread;
            reclaim(d);
            // Keep the record so that a later OSCreateThread on t starts afresh.
            (void)t;
        }
    }
}
