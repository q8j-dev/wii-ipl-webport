// Browser front end: loads the NAND image and the program, wires the
// emulated hardware (GPU, input, audio, NAND persistence) and runs it.

import {Nand} from './nand.js';
import {Gpu} from './gpu.js';
import {Input} from './input.js';
import {Audio} from './audio.js';

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
const DB = 'wii-menu-nand', STORE = 'files';
function openDb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE);
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
}

const params = new URLSearchParams(location.search);

async function boot() {
  const gl = canvas.getContext('webgl2', {antialias: false, alpha: false, depth: false, stencil: false,
    preserveDrawingBuffer: false, powerPreference: 'high-performance'});
  if (!gl) throw new Error('WebGL 2 is not available in this browser.');

  const nand = new Nand();
  nand.loadImage(await fetchWithProgress('build/nand.bin', 'Loading System Menu data'));
  let db = null;
  try {
    db = await openDb();
    nand.applySaved(await loadSaved(db));
  } catch (e) {
    console.warn('NAND persistence unavailable:', e);
  }
  let saveTimer = 0;
  nand.onDirty = () => {
    if (!db || saveTimer) return;
    saveTimer = setTimeout(() => { saveTimer = 0; saveChanges(db, nand.takeChanges()); }, 1000);
  };

  const input = new Input(canvas);
  const audio = new Audio();
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
    get gpu() { return gpu; },
    log(bytes) {
      for (const b of bytes) {
        if (b === 10) { console.log(lineBuf); lineBuf = ''; } else lineBuf += String.fromCharCode(b);
      }
    },
    fatal(msg) {
      console.error('FATAL:', msg);
      setStatus(msg);
      document.body.classList.add('halted');
    },
    wait(ms, forFrame) {
      wii.waits = (wii.waits || 0) + 1;
      if (forFrame) return new Promise((r) => requestAnimationFrame(() => r()));
      return new Promise((r) => setTimeout(r, Math.max(0, ms)));
    },
  };

  setStatus('Starting…');
  // JSPI (stack switching) where available; the Asyncify build elsewhere.
  const jspi = typeof WebAssembly.Suspending === 'function' && typeof WebAssembly.promising === 'function';
  const {default: createWii} = await import(jspi ? '../build/wiimenu.js' : '../build/wiimenu-asyncify.js');
  mod = await createWii({wii, locateFile: (p) => 'build/' + p});
  audio.attach(mod);
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
  window.wii = {mod, gpu, nand, host: wii};
  gpu.noDraw = params.has('nodraw');
  if (params.has('heartbeat')) {
    setInterval(() => console.log(`heartbeat waits=${wii.waits} frames=${gpu.frameCount} draws=${gpu.stats.draws}`), 1000);
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
