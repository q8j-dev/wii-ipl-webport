// Browser front end: loads the NAND image and the program, wires the
// emulated hardware (GPU, input, audio, NAND persistence) and runs it.

import {Nand} from './nand.js';
import {Gpu} from './gpu.js';
import {Input} from './input.js';
import {Audio} from './audio.js';
import {Www} from './www.js';
import {importFiles} from './importer.js';

const status = document.getElementById('status');
const canvas = document.getElementById('screen');
const setStatus = (t) => { if (status) status.textContent = t; };

async function fetchWithProgress(url, label) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  const total = Number(res.headers.get('content-length')) || 0;
  if (!res.body || !total) return await res.arrayBuffer();
  const reader = res.body.getReader();
  const buf = new Uint8Array(total);
  let got = 0;
  for (;;) {
    const {done, value} = await reader.read();
    if (done) break;
    buf.set(value, got);
    got += value.length;
    setStatus(`${label} ${Math.floor(got * 100 / total)}%`);
  }
  return buf.buffer;
}

// ---- NAND persistence (IndexedDB) ----
// "files": changes made by the System Menu, keyed by path (null = deleted).
// "base": the imported NAND image (when the server does not provide one).
const DB = 'wii-menu-nand', STORE = 'files', BASE = 'base';
function openDb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 2);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE);
      if (!db.objectStoreNames.contains(BASE)) db.createObjectStore(BASE);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
async function loadSaved(db) {
  return new Promise((resolve) => {
    const out = [];
    const tx = db.transaction(STORE, 'readonly');
    const req = tx.objectStore(STORE).openCursor();
    req.onsuccess = () => {
      const c = req.result;
      if (!c) { resolve(out); return; }
      out.push([c.key, c.value]);
      c.continue();
    };
    req.onerror = () => resolve(out);
  });
}
function saveChanges(db, changes) {
  const tx = db.transaction(STORE, 'readwrite');
  const st = tx.objectStore(STORE);
  for (const [path, node] of changes) st.put(node, path);
  return new Promise((resolve) => { tx.oncomplete = tx.onerror = tx.onabort = resolve; });
}
function dbGet(db, store, key) {
  return new Promise((resolve) => {
    const req = db.transaction(store, 'readonly').objectStore(store).get(key);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => resolve(undefined);
  });
}
function dbPut(db, store, key, value) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(store, 'readwrite');
    tx.objectStore(store).put(value, key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

// The System Menu's NAND image: from the server (build/nand.bin), from an
// earlier import, or imported now from the user's files.
async function loadBaseImage(db) {
  try {
    const head = await fetch('build/nand.bin', {method: 'HEAD'});
    if (head.ok) return await fetchWithProgress('build/nand.bin', 'Loading System Menu data');
  } catch (e) {
    // no server copy
  }
  if (db) {
    const saved = await dbGet(db, BASE, 'image');
    if (saved) return saved;
  }
  const image = await askForImage();
  if (db) {
    try {
      await dbPut(db, BASE, 'image', image);
    } catch (e) {
      console.warn('cannot keep the imported System Menu:', e);
    }
  }
  return image;
}

function askForImage() {
  const panel = document.getElementById('import');
  const files = document.getElementById('import-files');
  const key = document.getElementById('import-key');
  const go = document.getElementById('import-go');
  const err = document.getElementById('import-error');
  setStatus('');
  document.body.classList.add('importing');
  let picked = [];
  const show = () => {
    const names = picked.map((f) => f.name);
    document.getElementById('import-picked').textContent = names.length ? names.join(', ') : '';
    const needKey = picked.some((f) => /\.(wad|zip)$/i.test(f.name));
    key.parentElement.style.display = needKey ? '' : 'none';
    go.disabled = !picked.length;
  };
  files.addEventListener('change', () => { picked = [...files.files]; show(); });
  panel.addEventListener('dragover', (e) => { e.preventDefault(); panel.classList.add('drag'); });
  panel.addEventListener('dragleave', () => panel.classList.remove('drag'));
  panel.addEventListener('drop', (e) => {
    e.preventDefault();
    panel.classList.remove('drag');
    picked = [...e.dataTransfer.files];
    show();
  });
  show();
  return new Promise((resolve) => {
    go.addEventListener('click', async () => {
      err.textContent = '';
      go.disabled = true;
      try {
        const image = await importFiles(picked, key.value, (m) => { err.textContent = m; });
        document.body.classList.remove('importing');
        resolve(image);
      } catch (e) {
        err.textContent = e.message || String(e);
        go.disabled = false;
      }
    });
  });
}

const params = new URLSearchParams(location.search);

// ?reset forgets the imported System Menu and everything it saved.
if (params.has('reset')) {
  indexedDB.deleteDatabase(DB);
  history.replaceState(null, '', location.pathname);
}

async function boot() {
  const gl = canvas.getContext('webgl2', {antialias: false, alpha: false, depth: false, stencil: false,
    preserveDrawingBuffer: false, powerPreference: 'high-performance'});
  if (!gl) throw new Error('WebGL 2 is not available in this browser.');

  let db = null;
  try {
    db = await openDb();
  } catch (e) {
    console.warn('NAND persistence unavailable:', e);
  }
  const nand = new Nand();
  nand.loadImage(await loadBaseImage(db));
  if (db) {
    nand.applySaved(await loadSaved(db));
    nand.clearTmp();
  }
  let saveTimer = 0;
  nand.onDirty = () => {
    if (!db || saveTimer) return;
    saveTimer = setTimeout(() => { saveTimer = 0; saveChanges(db, nand.takeChanges()); }, 1000);
  };
  // Write pending changes when the page goes away.
  const saveNow = () => {
    if (!db) return;
    clearTimeout(saveTimer);
    saveTimer = 0;
    const changes = nand.takeChanges();
    if (changes.length) saveChanges(db, changes);
  };
  window.addEventListener('pagehide', saveNow);
  document.addEventListener('visibilitychange', () => { if (document.hidden) saveNow(); });

  const input = new Input(canvas);
  const audio = new Audio();
  const www = new Www();
  window.addEventListener('pointerdown', () => audio.resume(), {once: false});
  window.addEventListener('keydown', () => audio.resume(), {once: false});

  let mod = null;
  let gpu = null;
  let lineBuf = '';
  const fps = {frames: 0, last: performance.now(), el: document.getElementById('fps')};

  const wii = {
    nand,
    input,
    audio,
    www,
    get gpu() { return gpu; },
    log(bytes) {
      for (const b of bytes) {
        if (b === 10) { console.log(lineBuf); lineBuf = ''; } else lineBuf += String.fromCharCode(b);
      }
    },
    // The menu restarts itself (Return to the Wii Menu, settings that need a
    // restart) or switches the console off. Neither returns.
    async power(mode) {
      if (audio.ctx) audio.ctx.suspend();
      if (db) {
        clearTimeout(saveTimer);
        saveTimer = 0;
        const changes = nand.takeChanges();
        if (changes.length) await saveChanges(db, changes);
      }
      if (mode === 1) {
        setStatus('');
        location.reload();
      } else {
        canvas.style.visibility = 'hidden';
        setStatus('The Wii is off. Click to turn it on.');
        document.addEventListener('pointerdown', () => location.reload(), {once: true});
        document.addEventListener('keydown', () => location.reload(), {once: true});
      }
      return new Promise(() => {});
    },
    fatal(msg) {
      console.error('FATAL:', msg);
      setStatus(msg);
      document.body.classList.add('halted');
    },
    wait(ms, forFrame, limitMs) {
      wii.waits = (wii.waits || 0) + 1;
      if (forFrame) {
        return new Promise((r) => {
          let done = false;
          const fin = () => { if (!done) { done = true; r(); } };
          requestAnimationFrame(fin);
          if (limitMs < 1e8) setTimeout(fin, Math.max(0, limitMs));
        });
      }
      return new Promise((r) => setTimeout(r, Math.max(0, ms)));
    },
  };

  setStatus('Starting…');
  // JSPI (stack switching) where available; the Asyncify build elsewhere.
  const jspi = typeof WebAssembly.Suspending === 'function' && typeof WebAssembly.promising === 'function' &&
    !params.has('asyncify');
  const {default: createWii} = await import(jspi ? '../build/wiimenu.js' : '../build/wiimenu-asyncify.js');
  mod = await createWii({wii, locateFile: (p) => 'build/' + p});
  audio.attach(mod);
  www.attach(mod);
  gpu = new Gpu(gl, {
    heap: () => mod.HEAPU8,
    hwregBase: mod._webx_hwreg(),
    onToken: (t) => mod._webx_gx_token(t),
    scale: Number(params.get('scale')) || Math.min(3, Math.max(1, Math.ceil(window.devicePixelRatio * canvas.clientHeight / 456))),
  });
  const present = gpu.present.bind(gpu);
  gpu.present = (...a) => {
    present(...a);
    fps.frames++;
    const now = performance.now();
    if (now - fps.last >= 1000) {
      if (fps.el) fps.el.textContent = `${Math.round(fps.frames * 1000 / (now - fps.last))} fps`;
      fps.frames = 0;
      fps.last = now;
    }
  };
  document.body.classList.add('running');
  setStatus('');
  window.wii = {mod, gpu, nand, input, host: wii};
  gpu.noDraw = params.has('nodraw');
  audio.debug = params.has('heartbeat');
  gpu.debugShaders = params.has('heartbeat');
  www.debug = params.has('heartbeat');
  window.wii.audio = audio;
  if (params.has('heartbeat')) {
    setInterval(() => console.log(`heartbeat waits=${wii.waits} frames=${gpu.frameCount} draws=${gpu.stats.draws} audio=${JSON.stringify(audio.stats)} ctx=${audio.ctx && audio.ctx.state}`), 1000);
  }
  const crashed = (e) => {
    if (String(e).includes('unwind')) return;
    console.error(e);
    wii.fatal('Crashed: ' + (e && e.message || e));
  };
  try {
    const r = mod.callMain([]);
    if (r && typeof r.then === 'function') r.catch(crashed);
  } catch (e) {
    crashed(e);
  }
}

function resize() {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.floor(canvas.clientWidth * dpr), h = Math.floor(canvas.clientHeight * dpr);
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
}
window.addEventListener('resize', resize);
resize();

boot().catch((e) => {
  console.error(e);
  setStatus('Error: ' + e.message);
});
