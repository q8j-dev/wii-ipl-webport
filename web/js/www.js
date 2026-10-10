// Opera (RVL WWW) for Wii Settings, done by the host browser.
//
// The System Menu drives a browser through the WWW* API (web/hal/www_web.c):
// it opens "marc:" URLs (files of the settings archive, served by the menu's
// own protocol handler), forwards Wii Remote pointer events, and expects the
// page as pixels in a raster buffer, which it converts to a texture and
// composites with its own layouts and page transitions.
//
// Here pages live in a hidden same-origin iframe. Their resources are fetched
// through the menu's protocol handler and inlined (stylesheets, scripts,
// images as data: URLs), scripts run in the iframe with the menu's JavaScript
// plugins ("wiiSetting", "wiiTrasition") bridged to their C implementations,
// and the page is rasterized by drawing it as an SVG foreignObject image.
//
// Page code only runs while the menu is inside a WWW call (event dispatch,
// WWWRunSlice): timers are queued and run from WWWRunSlice, so plugin calls
// always happen on the browser thread, like with Opera.

const EVT_URL_CHANGE = 5, EVT_LOADING_START = 6, EVT_LOADING_FINISHED = 7, EVT_IMECREATE = 30,
  EVT_PROTOCOL_LOADING_START = 44;
const MOUSE_MOVE = 0, MOUSE_TRIGGER = 1, MOUSE_RELEASE = 2;
const TY_OBJECT = 0, TY_STRING = 1, TY_NUMBER = 2;

// The settings pages name the Wii system fonts, which only Opera had: every
// font-family becomes a rounded sans-serif available on the host.
const FONT_FACE = `@font-face { font-family: "WiiSans"; src: local("Arial Rounded MT Bold"), local("Nunito"),
  local("Arial"), local("Helvetica Neue"), local("Helvetica"), local("Liberation Sans"), local("DejaVu Sans"),
  local("Roboto"); }`;
const FONT_FAMILY = 'font-family: "WiiSans", Arial, Helvetica, sans-serif';

function mimeOf(path) {
  const ext = path.slice(path.lastIndexOf('.') + 1).toLowerCase();
  return {gif: 'image/gif', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', css: 'text/css',
    js: 'text/javascript', html: 'text/html', htm: 'text/html'}[ext] || 'application/octet-stream';
}

function toBase64(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

export class Www {
  constructor() {
    this.mod = null;
    this.raster = 0;
    this.width = 608;
    this.height = 456;
    this.rowSize = 608 * 4;
    this.iframe = null;
    this.plugins = [];
    this.functions = new Map();  // id -> {call, construct, fn}
    this.objects = new Map();    // private pointer -> proxy
    this.handleOf = new WeakMap();  // proxy/function -> handle
    this.nextFn = 1;
    this.requests = new Map();
    this.nextReq = 1;
    this.cache = new Map();  // path -> bytes
    this.dataUrls = new Map();
    this.page = null;
    this.history = [];
    this.pendingUrl = null;
    this.loading = false;
    this.frame = null;
    this.frameSerial = 0;
    this.dirty = false;
    this.rasterizing = false;
    this.timers = [];
    this.timerId = 1;
    this.hover = null;
    this.down = null;
    this.ime = new Map();
    this.nextIme = 1;
  }

  attach(mod) { this.mod = mod; }

  get dv() {
    if (!this._dv || this._dv.buffer !== this.mod.HEAPU8.buffer) this._dv = new DataView(this.mod.HEAPU8.buffer);
    return this._dv;
  }

  cstr(ptr) {
    if (!ptr) return '';
    const h = this.mod.HEAPU8;
    let e = ptr;
    while (h[e]) e++;
    return new TextDecoder().decode(h.subarray(ptr, e));
  }

  allocStr(s) {
    const b = new TextEncoder().encode(s);
    const p = this.mod._webx_www_malloc(b.length + 1);
    this.mod.HEAPU8.set(b, p);
    this.mod.HEAPU8[p + b.length] = 0;
    return p;
  }

  // ------------------------------------------------------------ WWW API

  surface(w, h, rowSize, raster) {
    this.width = w;
    this.height = h;
    this.rowSize = rowSize;
    this.raster = raster;
  }

  createBrowser() {
    this.plugins = [];
    this.history = [];
    this.page = null;
  }

  terminate() {
    this.closeWindow();
  }

  createWindow() {
    if (this.iframe) return;
    const f = document.createElement('iframe');
    f.setAttribute('aria-hidden', 'true');
    f.tabIndex = -1;
    f.style.cssText = `position:fixed;left:-20000px;top:0;width:${this.width}px;height:${this.height}px;border:0;` +
      'opacity:0;pointer-events:none;';
    document.body.appendChild(f);
    this.iframe = f;
    this.scratch = this.mod._webx_www_malloc(64);
    this.eventData = this.mod._webx_www_malloc(32);
    this.mod.HEAPU8.fill(0, this.eventData, this.eventData + 32);
  }

  closeWindow() {
    if (this.iframe) this.iframe.remove();
    this.iframe = null;
    this.timers = [];
    this.frame = null;
    this.loading = false;
    this.pendingUrl = null;
  }

  setRect(x, y, w, h) {
    if (this.iframe) {
      this.iframe.style.width = w + 'px';
      this.iframe.style.height = h + 'px';
    }
  }

  open(urlPtr) {
    this.pendingUrl = this.cstr(urlPtr).replace(/^marc:\/*/, '');
  }

  back() {
    if (this.history.length > 1) {
      this.history.pop();
      this.pendingUrl = this.history.pop();
    }
  }

  runSlice() {
    try {
      if (this.pendingUrl !== null && !this.loading) {
        const url = this.pendingUrl;
        this.pendingUrl = null;
        this.load(url);
      }
      this.runTimers();
      this.fixUrls();
      if (this.loading && this.frame && this.frame.serial >= this.loadSerial) {
        this.writeRaster();
        this.loading = false;
        this.notify(EVT_LOADING_FINISHED, this.eventData);
      } else if (!this.loading && this.dirty && !this.rasterizing) {
        this.rasterize();
      }
    } catch (e) {
      console.error('www:', e);
    }
  }

  // Returns 1 when a new picture was written to the raster buffer.
  takeFrame() {
    if (this.loading || !this.frame || this.frame.written) return 0;
    this.writeRaster();
    return 1;
  }

  mouse(cmd, x, y, button) {
    const doc = this.doc();
    if (!doc || this.loading) return;
    try {
      const win = this.iframe.contentWindow;
      const el = x >= 0 && y >= 0 ? doc.elementFromPoint(x, y) : null;
      const opts = (extra) => Object.assign({bubbles: true, cancelable: true, view: win, clientX: x, clientY: y,
        button: 0}, extra);
      if (el !== this.hover) {
        if (this.hover && this.hover.isConnected) this.hover.dispatchEvent(new win.MouseEvent('mouseout', opts({relatedTarget: el})));
        const prev = this.hover;
        this.hover = el;
        if (el) el.dispatchEvent(new win.MouseEvent('mouseover', opts({relatedTarget: prev})));
      }
      if (!el) return;
      if (cmd === MOUSE_MOVE) {
        el.dispatchEvent(new win.MouseEvent('mousemove', opts({})));
      } else if (cmd === MOUSE_TRIGGER) {
        this.down = el;
        el.dispatchEvent(new win.MouseEvent('mousedown', opts({buttons: 1})));
      } else if (cmd === MOUSE_RELEASE) {
        el.dispatchEvent(new win.MouseEvent('mouseup', opts({})));
        if (this.down === el) this.click(el, opts);
        this.down = null;
      }
    } catch (e) {
      console.error('www:', e);
    }
    this.fixUrls();
  }

  key(cmd, sym) {}

  click(el, opts) {
    const win = this.iframe.contentWindow;
    let pageCanceled = false;
    const catcher = (ev) => { pageCanceled = ev.defaultPrevented; ev.preventDefault(); };
    win.addEventListener('click', catcher);
    el.dispatchEvent(new win.MouseEvent('click', opts({})));
    win.removeEventListener('click', catcher);
    const input = el.closest('input, textarea');
    if (input && /^(text|password|)$/i.test(input.type || '') || (input && input.tagName === 'TEXTAREA')) {
      this.startIme(input);
      return;
    }
    const a = el.closest('a[href]');
    if (!a || pageCanceled || !this.iframe) return;
    const href = a.getAttribute('href');
    if (/^javascript:/i.test(href)) {
      this.eval(decodeURIComponent(href.slice(11)));
    } else if (href && !href.startsWith('#')) {
      this.pendingUrl = this.resolve(href, this.page);
      this.runSlice();
    }
  }

  // Text fields are edited with the System Menu's keyboard.
  startIme(input) {
    const id = this.nextIme++;
    this.ime.set(id, input);
    const p = this.mod._webx_www_malloc(32);
    const text = this.allocStr(input.value || '');
    const dv = this.dv;
    for (let i = 0; i < 32; i += 4) dv.setUint32(p + i, 0, false);
    dv.setUint32(p, id, false);
    dv.setUint32(p + 4, text, false);
    dv.setUint32(p + 0x1C, input.maxLength > 0 ? input.maxLength : 0, false);
    this.notify(EVT_IMECREATE, p);
    this.mod._webx_www_free(text);
    this.mod._webx_www_free(p);
  }

  commitIme(id, textPtr) {
    const input = this.ime.get(id);
    this.ime.delete(id);
    if (!input || !input.isConnected) return;
    if (textPtr) input.value = this.cstr(textPtr);
    const win = this.iframe.contentWindow;
    try {
      input.dispatchEvent(new win.Event('input', {bubbles: true}));
      input.dispatchEvent(new win.Event('change', {bubbles: true}));
    } catch (e) {
      console.error('www:', e);
    }
    this.dirty = true;
  }

  // ------------------------------------------------------------ resources

  notify(event, data) {
    this.mod._webx_www_notify(event, data);
  }

  resolve(rel, base) {
    const u = new URL(rel, 'http://marc/' + (base || ''));
    return decodeURIComponent(u.pathname.slice(1));
  }

  // Reads a file through the menu's "marc" protocol handler.
  fetch(path) {
    if (this.cache.has(path)) return this.cache.get(path);
    const id = this.nextReq++;
    const req = {chunks: [], ok: false};
    this.requests.set(id, req);
    const pathPtr = this.allocStr('marc:/' + path);
    const protoPtr = this.allocStr('marc');
    const data = this.mod._webx_www_malloc(12);
    const dv = this.dv;
    dv.setUint32(data, pathPtr, false);
    dv.setUint32(data + 4, protoPtr, false);
    dv.setUint32(data + 8, id, false);
    this.notify(EVT_PROTOCOL_LOADING_START, data);
    this.mod._webx_www_free(data);
    this.mod._webx_www_free(pathPtr);
    this.mod._webx_www_free(protoPtr);
    this.requests.delete(id);
    if (!req.ok) {
      console.warn('www: cannot load', path);
      this.cache.set(path, null);
      return null;
    }
    let n = 0;
    for (const c of req.chunks) n += c.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const c of req.chunks) { out.set(c, o); o += c.length; }
    this.cache.set(path, out);
    return out;
  }

  protocolWrite(id, ptr, len) {
    const req = this.requests.get(id);
    if (req) req.chunks.push(this.mod.HEAPU8.slice(ptr, ptr + len));
  }

  protocolDone(id, ok) {
    const req = this.requests.get(id);
    if (req) req.ok = !!ok;
  }

  text(path) {
    const b = this.fetch(path);
    return b ? new TextDecoder().decode(b) : '';
  }

  dataUrl(path) {
    if (this.dataUrls.has(path)) return this.dataUrls.get(path);
    const b = this.fetch(path);
    const url = b ? `data:${mimeOf(path)};base64,${toBase64(b)}` : 'data:,';
    this.dataUrls.set(path, url);
    return url;
  }

  // Inlines @imports and turns url(...) into data: URLs.
  css(text, base, depth = 0) {
    text = text.replace(/@import\s+(?:url\()?\s*["']?([^"')]+)["']?\s*\)?\s*;/gi, (m, href) =>
      depth < 4 ? this.css(this.text(this.resolve(href, base)), this.resolve(href, base), depth + 1) : '');
    return text.replace(/url\(\s*["']?([^"')]+)["']?\s*\)/gi, (m, href) =>
      /^data:/i.test(href) ? m : `url("${this.dataUrl(this.resolve(href, base))}")`)
      .replace(/font-family\s*:[^;}]*/gi, FONT_FAMILY);
  }

  // ------------------------------------------------------------ pages

  doc() {
    return this.iframe && this.page !== null ? this.iframe.contentDocument : null;
  }

  load(path) {
    this.notify(EVT_URL_CHANGE, this.eventData);
    this.notify(EVT_LOADING_START, this.eventData);
    this.page = path;
    this.history.push(path);
    if (this.history.length > 64) this.history.shift();
    this.loading = true;
    this.loadSerial = ++this.frameSerial;
    this.timers = [];
    this.hover = null;
    this.down = null;

    const src = new DOMParser().parseFromString(this.text(path) || '<html><body></body></html>', 'text/html');
    for (const link of [...src.querySelectorAll('link')]) {
      if (!/stylesheet/i.test(link.getAttribute('rel') || '')) continue;
      const href = this.resolve(link.getAttribute('href'), path);
      const style = src.createElement('style');
      style.textContent = this.css(this.text(href), href);
      link.replaceWith(style);
    }
    for (const st of [...src.querySelectorAll('style')]) st.textContent = this.css(st.textContent, path);
    for (const sc of [...src.querySelectorAll('script[src]')]) {
      sc.textContent = this.text(this.resolve(sc.getAttribute('src'), path));
      sc.removeAttribute('src');
    }
    const fonts = src.createElement('style');
    fonts.textContent = FONT_FACE + `\nhtml, body { overflow: hidden; ${FONT_FAMILY}; }`;
    src.head.prepend(fonts);
    const onload = src.body.getAttribute('onload');
    src.body.removeAttribute('onload');
    this.fixUrls(src);

    const win = this.iframe.contentWindow;
    const doc = this.iframe.contentDocument;
    doc.open();
    this.installGlobals(win);
    doc.write('<!DOCTYPE html>' + src.documentElement.outerHTML);
    doc.close();
    if (onload) this.eval(onload, doc.body);
    this.fixUrls();
    if (this.observer) this.observer.disconnect();
    this.observer = new win.MutationObserver(() => { this.dirty = true; });
    this.observer.observe(doc, {subtree: true, childList: true, attributes: true, characterData: true});
    this.dirty = false;
    this.rasterize();
  }

  eval(code, thisObj) {
    const win = this.iframe.contentWindow;
    try {
      new win.Function(code).call(thisObj || win);
    } catch (e) {
      console.error('www page script:', e);
    }
  }

  // Resolves image and background URLs set by markup or page scripts.
  fixUrls(root) {
    const doc = root || this.doc();
    if (!doc || !this.page) return;
    const base = this.page;
    for (const img of doc.querySelectorAll('img[src], input[src]')) {
      const s = img.getAttribute('src');
      if (s && !/^data:/i.test(s)) img.setAttribute('src', this.dataUrl(this.resolve(s, base)));
    }
    for (const el of doc.querySelectorAll('[background]')) {
      const s = el.getAttribute('background');
      if (/^data:/i.test(s)) continue;
      el.setAttribute('background', this.dataUrl(this.resolve(s, base)));
      el.style.backgroundImage = `url("${el.getAttribute('background')}")`;
    }
    for (const el of doc.querySelectorAll('[style*="url("]')) {
      const s = el.getAttribute('style');
      const f = s.replace(/url\(\s*["']?([^"')]+)["']?\s*\)/gi, (m, href) =>
        /^data:/i.test(href) ? m : `url("${this.dataUrl(this.resolve(href, base))}")`);
      if (f !== s) el.setAttribute('style', f);
    }
  }

  // Globals of the page: the plugins' constructors and timers that run from
  // WWWRunSlice.
  installGlobals(win) {
    // Image sources set by page scripts are archive paths relative to the page.
    const www = this;
    const proto = win.HTMLImageElement.prototype;
    const desc = this.imgSrc || (this.imgSrc = Object.getOwnPropertyDescriptor(proto, 'src'));
    Object.defineProperty(proto, 'src', {
      configurable: true,
      get() { return desc.get.call(this); },
      set(v) {
        v = String(v);
        if (!/^(data|blob):/i.test(v) && www.page) v = www.dataUrl(www.resolve(v, www.page));
        desc.set.call(this, v);
      },
    });
    for (const p of this.plugins) {
      for (const name of p.names) {
        let value;
        Object.defineProperty(win, name, {
          configurable: true,
          get: () => {
            if (value === undefined) value = this.getProperty(p.getter, 1, name).value;
            return value;
          },
        });
      }
    }
    win.setTimeout = (fn, ms, ...args) => this.addTimer(fn, ms, args, false);
    win.setInterval = (fn, ms, ...args) => this.addTimer(fn, ms, args, true);
    win.clearTimeout = win.clearInterval = (id) => { this.timers = this.timers.filter((t) => t.id !== id); };
    win.alert = (m) => console.log('www alert:', m);
    win.open = () => null;
  }

  addTimer(fn, ms, args, repeat) {
    const id = this.timerId++;
    this.timers.push({id, fn, args, at: performance.now() + (ms || 0), ms: Math.max(ms || 0, 1), repeat});
    return id;
  }

  runTimers() {
    const now = performance.now();
    const due = this.timers.filter((t) => t.at <= now);
    if (!due.length) return;
    this.timers = this.timers.filter((t) => t.at > now || t.repeat);
    for (const t of due) {
      if (t.repeat) t.at = now + t.ms;
      if (typeof t.fn === 'function') {
        try { t.fn(...t.args); } catch (e) { console.error('www timer:', e); }
      } else {
        this.eval(String(t.fn));
      }
    }
  }

  // ------------------------------------------------------------ JS plugins

  addPlugin(cap) {
    const dv = this.dv;
    const namesPtr = dv.getUint32(cap, false);
    const names = [];
    for (let i = 0; i < 16; i++) {
      const p = dv.getUint32(namesPtr + i * 4, false);
      if (!p) break;
      names.push(this.cstr(p));
    }
    this.plugins.push({names, getter: dv.getUint32(cap + 8, false)});
  }

  makeFunction(call, construct) {
    const id = this.nextFn++;
    const www = this;
    const fn = function(...args) {
      const self = new.target ? 0 : (www.handleOf.get(this) || 0);
      const target = new.target ? construct : call;
      if (!target) return undefined;
      return www.invoke(target, self, id, args);
    };
    this.functions.set(id, {call, construct, fn});
    this.handleOf.set(fn, id);
    return id;
  }

  makeObject(priv, getter, setter) {
    const www = this;
    const proxy = new Proxy({}, {
      get(t, prop) {
        if (typeof prop !== 'string' || !getter) return t[prop];
        const r = www.getProperty(getter, priv, prop);
        return r.found ? r.value : t[prop];
      },
      set(t, prop, v) {
        if (typeof prop === 'string' && setter && www.setProperty(setter, priv, prop, v)) return true;
        t[prop] = v;
        return true;
      },
    });
    this.objects.set(priv, proxy);
    this.handleOf.set(proxy, priv);
  }

  writeValue(p, v, temps) {
    const dv = this.dv;
    dv.setUint32(p + 4, 0, false);
    dv.setFloat64(p + 8, 0, false);
    if (typeof v === 'number' || typeof v === 'boolean') {
      dv.setInt32(p, TY_NUMBER, false);
      dv.setFloat64(p + 8, Number(v), false);
    } else if (typeof v === 'string') {
      const s = this.allocStr(v);
      temps.push(s);
      dv.setInt32(p, TY_STRING, false);
      dv.setUint32(p + 8, s, false);
    } else if (v && this.handleOf.has(v)) {
      dv.setInt32(p, TY_OBJECT, false);
      dv.setUint32(p + 8, this.handleOf.get(v), false);
    } else {
      dv.setInt32(p, -1, false);
    }
  }

  readValue(p) {
    const dv = this.dv;
    const ty = dv.getInt32(p, false);
    if (ty === TY_STRING) return this.cstr(dv.getUint32(p + 8, false));
    if (ty === TY_NUMBER) return dv.getFloat64(p + 8, false);
    if (ty === TY_OBJECT) {
      const h = dv.getUint32(p + 8, false);
      if (this.functions.has(h)) return this.functions.get(h).fn;
      return this.objects.get(h);
    }
    return undefined;
  }

  getProperty(getter, obj, name) {
    const v = this.scratch;
    this.dv.setInt32(v, -1, false);
    const n = this.allocStr(name);
    const r = this.mod._webx_www_getset(getter, obj, n, v);
    this.mod._webx_www_free(n);
    if (r !== 6 && r !== 7) return {found: false};
    return {found: true, value: this.readValue(v)};
  }

  setProperty(setter, obj, name, value) {
    const temps = [];
    const v = this.scratch + 16;
    this.writeValue(v, value, temps);
    const n = this.allocStr(name);
    const r = this.mod._webx_www_getset(setter, obj, n, v);
    this.mod._webx_www_free(n);
    for (const t of temps) this.mod._webx_www_free(t);
    this.dirty = true;
    return r === 11;
  }

  invoke(fn, self, id, args) {
    const temps = [];
    const argv = this.mod._webx_www_malloc(16 * (args.length + 1));
    args.forEach((a, i) => this.writeValue(argv + i * 16, a, temps));
    const res = this.scratch + 32;
    this.dv.setInt32(res, -1, false);
    this.mod._webx_www_call(fn, self, id, args.length, argv, res);
    const out = this.readValue(res);
    this.mod._webx_www_free(argv);
    for (const t of temps) this.mod._webx_www_free(t);
    this.dirty = true;
    return out;
  }

  // ------------------------------------------------------------ rendering

  async rasterize() {
    const doc = this.doc();
    if (!doc) return;
    this.rasterizing = true;
    this.dirty = false;
    const serial = ++this.frameSerial;
    try {
      const w = this.width, h = this.height;
      const html = new XMLSerializer().serializeToString(doc.documentElement)
        .replace(/<script\b[\s\S]*?<\/script>/gi, '');
      const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}">` +
        `<foreignObject x="0" y="0" width="${w}" height="${h}">${html}</foreignObject></svg>`;
      const img = new Image(w, h);
      img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
      await img.decode();
      if (!this.canvas) {
        this.canvas = document.createElement('canvas');
        this.ctx = this.canvas.getContext('2d', {willReadFrequently: true});
      }
      if (this.canvas.width !== w || this.canvas.height !== h) {
        this.canvas.width = w;
        this.canvas.height = h;
      }
      this.ctx.fillStyle = '#fff';
      this.ctx.fillRect(0, 0, w, h);
      this.ctx.drawImage(img, 0, 0);
      const pixels = this.ctx.getImageData(0, 0, w, h).data;
      if (!this.frame || serial > this.frame.serial) this.frame = {pixels, serial, written: false, w, h};
    } catch (e) {
      console.error('www: rasterization failed', e);
      if (!this.frame || serial > this.frame.serial) {
        this.frame = {pixels: new Uint8ClampedArray(this.width * this.height * 4).fill(255), serial, written: false,
          w: this.width, h: this.height};
      }
    }
    this.rasterizing = false;
  }

  // Raster pixels are 32-bit ARGB, big-endian.
  writeRaster() {
    const f = this.frame;
    if (!f || !this.raster) return;
    const heap = this.mod.HEAPU8;
    const w = Math.min(f.w, this.width), h = Math.min(f.h, this.height);
    for (let y = 0; y < h; y++) {
      let o = this.raster + y * this.rowSize, i = y * f.w * 4;
      for (let x = 0; x < w; x++, o += 4, i += 4) {
        heap[o] = 255;
        heap[o + 1] = f.pixels[i];
        heap[o + 2] = f.pixels[i + 1];
        heap[o + 3] = f.pixels[i + 2];
      }
    }
    f.written = true;
  }
}
