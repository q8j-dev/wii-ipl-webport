// Platform glue of the Metrowerks C library (MSL) and out-of-line copies of
// MWCC intrinsics for code that declares them extern.

#include "web.h"

#include <stdlib.h>
#include <string.h>

// emscripten's allocator backs MSL's malloc pools
void* emscripten_builtin_malloc(unsigned long size);
void emscripten_builtin_free(void* ptr);

void* __sys_alloc(u32 size) { return emscripten_builtin_malloc(size); }
void __sys_free(void* ptr) { emscripten_builtin_free(ptr); }

int __write_console(u32 handle, u8* buffer, u32* count, void* idle) {
    webjs_log((const char*)buffer, *count);
    return 0;
}

int __read_console(u32 handle, u8* buffer, u32* count, void* idle) {
    *count = 0;
    return 0;
}

int __close_console(u32 handle) { return 0; }
int InitializeUART(u32 baud) { return 0; }
int __isdst(void) { return 0; }

void (*__msl_stdio_exit)(void);
