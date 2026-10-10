// JavaScript imports of the web HLE layer (declared in web/hal/web.h).
//
// The program's memory is big-endian (see web/tools/beswap), so structures
// are accessed through DataView with littleEndian = false. The objects doing
// the actual work (NAND, GPU, input, audio) live in Module.wii, set up by
// web/js/main.js (browser) or web/tools/run_node.mjs (tests).

addToLibrary({
  $wiiView: () => {
    if (!Module.wiiDV || Module.wiiDV.buffer !== HEAPU8.buffer) Module.wiiDV = new DataView(HEAPU8.buffer);
    return Module.wiiDV;
  },
  $wiiString: (ptr) => {
    let end = ptr;
    while (HEAPU8[end]) end++;
    let s = '';
    for (let i = ptr; i < end; i++) s += String.fromCharCode(HEAPU8[i]);
    return s;
  },

  webjs_now: () => performance.now(),
  webjs_now_wall_local: () => Date.now() - new Date().getTimezoneOffset() * 60000,

  webjs_log: (ptr, len) => {
    Module.wii.log(HEAPU8.subarray(ptr, ptr + len));
  },

  webjs_fatal__deps: ['$wiiString'],
  webjs_fatal: (ptr, len) => {
    let s = '';
    for (let i = 0; i < len; i++) s += String.fromCharCode(HEAPU8[ptr + i]);
    Module.wii.fatal(s);
    throw new Error('wii fatal: ' + s);
  },

  webjs_wait__async: true,
  webjs_wait: (ms, forFrame) => Asyncify.handleAsync(() => Module.wii.wait(ms, forFrame)),

  // ---- threads (JSPI) ----
  webjs_jspi: () => {
#if JSPI
    return 1;
#else
    return 0;
#endif
  },
#if JSPI
  // Each OS thread runs on its own wasm stack. A switch suspends the calling
  // stack (this import returns a promise) and resumes the target, or enters
  // it through the promising export webx_thread_entry the first time. The
  // shadow stack pointer is per thread and restored after resumption.
  $wiiThreads: {},
  webjs_thread_reset__deps: ['$wiiThreads'],
  webjs_thread_reset: (index) => { delete wiiThreads[index]; },
  webjs_thread_switch__deps: ['$wiiThreads', '$stackSave', '$stackRestore', 'emscripten_stack_set_limits',
                              'emscripten_stack_get_base', 'emscripten_stack_get_end'],
  webjs_thread_switch__async: 'auto',
  webjs_thread_switch: async (from, to, stackBase, stackEnd) => {
    const me = wiiThreads[from] || (wiiThreads[from] = {});
    me.sp = stackSave();
    me.base = _emscripten_stack_get_base();
    me.end = _emscripten_stack_get_end();
    const target = wiiThreads[to];
    if (target && target.resume) {
      const r = target.resume;
      target.resume = null;
      r();
    } else {
      wiiThreads[to] = {};
      queueMicrotask(() => {
        _emscripten_stack_set_limits(stackBase, stackEnd);
        stackRestore(stackBase);
        Promise.resolve(Module['_webx_thread_entry'](to)).catch((e) => {
          Module.wii.fatal('thread ' + to + ' crashed: ' + (e && e.stack || e));
        });
      });
    }
    await new Promise((r) => { me.resume = r; });
    _emscripten_stack_set_limits(me.base, me.end);
    stackRestore(me.sp);
  },
#else
  webjs_thread_reset: (index) => {},
  webjs_thread_switch: (from, to, stackBase, stackEnd) => { abort('webjs_thread_switch without JSPI'); },
#endif

  // ---- GPU ----
  webjs_gpu_process: (ptr, len) => Module.wii.gpu.process(ptr, len),
  webjs_gpu_invalidate: (addr, len) => Module.wii.gpu.invalidate(addr, len),
  webjs_vi_present: (xfb, width, height, black) => Module.wii.gpu.present(xfb, width, height, black),

  // ---- NAND ----
  webjs_fs_stat__deps: ['$wiiString', '$wiiView'],
  webjs_fs_stat: (pathPtr, outPtr) => {
    const st = Module.wii.nand.stat(wiiString(pathPtr));
    if (!st) return -106;
    const dv = wiiView();
    dv.setUint32(outPtr, st.dir ? 2 : 1, false);
    dv.setUint32(outPtr + 4, st.size, false);
    return 0;
  },
  webjs_fs_read__deps: ['$wiiString'],
  webjs_fs_read: (pathPtr, offset, dst, len) => {
    const data = Module.wii.nand.read(wiiString(pathPtr), offset, len);
    if (data === null) return -106;
    HEAPU8.set(data, dst);
    return data.length;
  },
  webjs_fs_write__deps: ['$wiiString'],
  webjs_fs_write: (pathPtr, offset, src, len, truncate) => {
    return Module.wii.nand.write(wiiString(pathPtr), offset, HEAPU8.subarray(src, src + len), truncate);
  },
  webjs_fs_create__deps: ['$wiiString'],
  webjs_fs_create: (pathPtr, isDir) => Module.wii.nand.create(wiiString(pathPtr), isDir),
  webjs_fs_delete__deps: ['$wiiString'],
  webjs_fs_delete: (pathPtr) => Module.wii.nand.remove(wiiString(pathPtr)),
  webjs_fs_rename__deps: ['$wiiString'],
  webjs_fs_rename: (fromPtr, toPtr) => Module.wii.nand.rename(wiiString(fromPtr), wiiString(toPtr)),
  webjs_fs_truncate__deps: ['$wiiString'],
  webjs_fs_truncate: (pathPtr, size) => Module.wii.nand.truncate(wiiString(pathPtr), size),
  webjs_fs_readdir__deps: ['$wiiString'],
  webjs_fs_readdir: (pathPtr, out, outLen) => {
    const names = Module.wii.nand.readdir(wiiString(pathPtr));
    if (names === null) return -106;
    let pos = out;
    const end = out + outLen;
    let n = 0;
    for (const name of names) {
      if (pos + name.length + 1 > end) break;
      for (let i = 0; i < name.length; i++) HEAPU8[pos++] = name.charCodeAt(i);
      HEAPU8[pos++] = 0;
      n++;
    }
    return n;
  },

  // ---- input ----
  webjs_input_poll__deps: ['$wiiView'],
  webjs_input_poll: (ptr) => {
    const s = Module.wii.input.state();
    const dv = wiiView();
    dv.setUint32(ptr, s.buttons >>> 0, false);
    dv.setFloat32(ptr + 4, s.x, false);
    dv.setFloat32(ptr + 8, s.y, false);
    dv.setUint32(ptr + 12, s.pointerValid ? 1 : 0, false);
    dv.setUint32(ptr + 16, 1, false);
  },

  // ---- audio ----
  webjs_audio_push: (ptr, frames) => Module.wii.audio.push(ptr, frames),
  webjs_audio_queued: () => Module.wii.audio.queued(),
});
