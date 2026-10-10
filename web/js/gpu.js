// GX graphics processor emulation on WebGL2.
//
// The GX library (unchanged decompiled code) writes the command stream that
// the Wii's GPU executes; this file executes it: CP/XF/BP register writes,
// display lists and primitives. Render state is turned into generated GLSL
// (vertex transform/lighting/texgen and the TEV combiner stages), similar to
// what Dolphin does.
//
// The embedded frame buffer (EFB, 640x528 on hardware) is a framebuffer
// rendered at `scale` times the native resolution. Copies to the external
// frame buffer (XFB) become textures keyed by their address, which the video
// interface presents; copies to textures become GL textures keyed by address
// too, so later draws that sample that address use them.

import {TF, decodeTexture, textureSize} from './gx_texture.js';
import {buildShaders} from './gx_shader.js';

const PRIM_QUADS = 0, PRIM_QUADS2 = 1, PRIM_TRIS = 2, PRIM_TRISTRIP = 3, PRIM_TRIFAN = 4,
  PRIM_LINES = 5, PRIM_LINESTRIP = 6, PRIM_POINTS = 7;

const EFB_W = 640, EFB_H = 528;

// Vertex layout written by the vertex loader (floats):
// pos(3) pnmtx(1) nrm(3) tangent(3) binormal(3) col0(4) col1(4) tex0-7(2 each = 16) texmtx(8)
const V_POS = 0, V_PNMTX = 3, V_NRM = 4, V_TAN = 7, V_BIN = 10, V_COL0 = 13, V_COL1 = 17, V_TEX = 21, V_TEXMTX = 37;
const VSTRIDE = 45;

function f32(dv, o) { return dv.getFloat32(o, false); }

export class Gpu {
  constructor(gl, opts) {
    this.gl = gl;
    this.heap = opts.heap;       // () => Uint8Array of wasm memory
    this.onToken = opts.onToken;  // (token) => void
    this.hwregBase = opts.hwregBase || 0;
    this.scale = opts.scale || 2;

    // Register state
    this.cp = new Uint32Array(256);
    this.bp = new Uint32Array(256);
    this.xfMem = new Float32Array(0x1000);   // matrices/lights as floats
    this.xfMemU = new Uint32Array(this.xfMem.buffer);
    this.xfReg = new Uint32Array(0x100);     // 0x1000..0x10FF
    this.bpMask = 0xFFFFFF;
    this.tmem = new Uint8Array(0x100000);     // texture memory (TLUTs)
    this.tevRegs = new Float32Array(16);
    this.tevKonst = new Float32Array(16);

    this.vtxData = new Float32Array(VSTRIDE * 65536);
    this.vtxCount = 0;
    this.indices = new Uint32Array(65536 * 6);
    this.indexCount = 0;
    this.batchKey = null;

    this.texCache = new Map();  // key -> {tex, addr, size}
    this.copyTextures = new Map();  // addr -> {tex, w, h, fmt}
    this.xfbs = new Map();  // addr -> {tex, w, h}
    this.programs = new Map();
    this.stats = {draws: 0, verts: 0};

    this.initGL();
    this.resetState();
  }

  resetState() {
    this.bp[0x00] = 0;
  }

  // ---------------------------------------------------------------- GL setup

  initGL() {
    const gl = this.gl;
    this.vbo = gl.createBuffer();
    this.ibo = gl.createBuffer();
    this.vao = gl.createVertexArray();
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ibo);
    const attr = (loc, n, off) => {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, n, gl.FLOAT, false, VSTRIDE * 4, off * 4);
    };
    attr(0, 3, V_POS);
    attr(1, 1, V_PNMTX);
    attr(2, 3, V_NRM);
    attr(3, 3, V_TAN);
    attr(4, 3, V_BIN);
    attr(5, 4, V_COL0);
    attr(6, 4, V_COL1);
    for (let i = 0; i < 4; i++) attr(7 + i, 4, V_TEX + i * 4);
    attr(11, 4, V_TEXMTX);
    attr(12, 4, V_TEXMTX + 4);
    gl.bindVertexArray(null);

    this.efbW = EFB_W * this.scale;
    this.efbH = EFB_H * this.scale;
    this.efbColor = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.efbColor);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, this.efbW, this.efbH);
    this.efbDepth = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, this.efbDepth);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, this.efbW, this.efbH);
    this.efbFbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.efbFbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.efbColor, 0);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, this.efbDepth);
    gl.clearColor(0, 0, 0, 1);
    gl.clearDepth(1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    this.readFbo = gl.createFramebuffer();
    this.drawFbo = gl.createFramebuffer();

    // Blit program for copies and presentation.
    this.blitProg = this.makeProgram(`#version 300 es
      layout(location=0) in vec2 p;
      uniform vec4 uSrc; // x, y, w, h in texture UV
      out vec2 uv;
      void main() { uv = uSrc.xy + (p * 0.5 + 0.5) * uSrc.zw; gl_Position = vec4(p, 0.0, 1.0); }`,
    `#version 300 es
      precision highp float;
      in vec2 uv;
      uniform sampler2D t;
      uniform int uMode; // 0 rgba, 1 intensity, 2 alpha->rgba, 3 no alpha
      uniform vec4 uMask;
      out vec4 o;
      void main() {
        vec4 c = texture(t, uv);
        if (uMode == 1) { float i = dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0/255.0; o = vec4(i, i, i, c.a); }
        else if (uMode == 3) o = vec4(c.rgb, 1.0);
        else o = c;
      }`);
    this.blitVbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.blitVbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    this.blitVao = gl.createVertexArray();
    gl.bindVertexArray(this.blitVao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);

    this.dummyTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.dummyTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 255]));

    this.samplers = [];
    for (let i = 0; i < 8; i++) this.samplers.push(gl.createSampler());
  }

  makeProgram(vs, fs) {
    const gl = this.gl;
    const compile = (type, src) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        console.error(src);
        throw new Error('shader compile failed: ' + gl.getShaderInfoLog(s));
      }
      return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, compile(gl.VERTEX_SHADER, vs));
    gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fs));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error('link failed: ' + gl.getProgramInfoLog(p));
    return p;
  }

  // ---------------------------------------------------------- command stream

  process(ptr, len) {
    const heap = this.heap();
    const dv = new DataView(heap.buffer, heap.byteOffset);
    const used = this.run(dv, ptr, ptr + len, false);
    return used - ptr;
  }

  // Executes commands in [p, end). Returns the position after the last
  // complete command (incomplete trailing commands are left for later).
  run(dv, p, end, inDL) {
    while (p < end) {
      const start = p;
      const cmd = dv.getUint8(p++);
      switch (cmd) {
        case 0x00:  // NOP
          break;
        case 0x08: {  // CP register
          if (p + 5 > end) return start;
          const reg = dv.getUint8(p);
          const val = dv.getUint32(p + 1, false);
          p += 5;
          this.writeCP(reg, val);
          break;
        }
        case 0x10: {  // XF registers
          if (p + 4 > end) return start;
          const hdr = dv.getUint32(p, false);
          const n = ((hdr >>> 16) & 0xFFFF) + 1;
          const addr = hdr & 0xFFFF;
          if (p + 4 + n * 4 > end) return start;
          p += 4;
          this.writeXF(dv, addr, n, p);
          p += n * 4;
          break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {  // indexed XF load
          if (p + 4 > end) return start;
          const v = dv.getUint32(p, false);
          p += 4;
          this.loadIndexedXF(dv, (cmd >> 3) & 3, v);
          break;
        }
        case 0x40: {  // call display list
          if (p + 8 > end) return start;
          const addr = dv.getUint32(p, false) & 0x3FFFFFFF;
          const size = dv.getUint32(p + 4, false);
          p += 8;
          if (!inDL) this.run(dv, addr, addr + size, true);
          break;
        }
        case 0x44:  // invalidate vertex cache
          break;
        case 0x48:  // call... (unused)
          break;
        case 0x61: {  // BP register
          if (p + 4 > end) return start;
          const v = dv.getUint32(p, false);
          p += 4;
          this.writeBP(v >>> 24, v & 0xFFFFFF);
          break;
        }
        default:
          if (cmd & 0x80) {
            if (p + 2 > end) return start;
            const count = dv.getUint16(p, false);
            const vat = cmd & 7;
            const prim = (cmd >> 3) & 7;
            const vsize = this.vertexSize(vat);
            if (p + 2 + count * vsize > end) return start;
            p += 2;
            this.drawPrimitive(dv, prim, vat, count, p, vsize);
            p += count * vsize;
          } else {
            console.warn('GX: unknown command 0x' + cmd.toString(16) + ' at ' + start.toString(16));
            return end;
          }
      }
    }
    return p;
  }

  writeCP(reg, val) {
    this.cp[reg] = val >>> 0;
  }

  writeXF(dv, addr, n, p) {
    for (let i = 0; i < n; i++) {
      const a = addr + i;
      const raw = dv.getUint32(p + i * 4, false);
      if (a < 0x1000) {
        if (this.xfMemU[a] !== raw) {
          this.flushIfNeeded();
          this.xfMemU[a] = raw;
        }
      } else if (a < 0x1100) {
        const r = a - 0x1000;
        if (this.xfReg[r] !== raw) {
          this.flushIfNeeded();
          this.xfReg[r] = raw;
        }
      }
    }
  }

  loadIndexedXF(dv, which, v) {
    const index = v >>> 16;
    const n = ((v >>> 12) & 0xF) + 1;
    const addr = v & 0xFFF;
    const base = this.cp[0xAC + which] & 0x3FFFFFFF;
    const stride = this.cp[0xBC + which];
    this.writeXF(dv, addr, n, base + index * stride);
  }

  writeBP(reg, val) {
    if (reg !== 0xFE) {
      const mask = this.bpMask;
      this.bpMask = 0xFFFFFF;
      val = (this.bp[reg] & ~mask) | (val & mask);
    }
    switch (reg) {
      case 0xFE:
        this.bpMask = val;
        return;
      case 0x45:  // draw done
        this.flush();
        this.bp[reg] = val;
        return;
      case 0x47:  // token
      case 0x48:  // token + interrupt
        this.flush();
        this.bp[reg] = val;
        this.writeHwToken(val & 0xFFFF);
        if (reg === 0x48 && this.onToken) this.onToken(val & 0xFFFF);
        return;
      case 0x52:  // EFB copy
        this.flush();
        this.bp[reg] = val;
        this.efbCopy(val);
        return;
      case 0x64:  // TLUT load address
        this.bp[reg] = val;
        return;
      case 0x65:  // TLUT load
        this.bp[reg] = val;
        this.loadTlut(val);
        return;
      case 0x66:  // invalidate texture cache region
        this.bp[reg] = val;
        return;
    }
    if (this.bp[reg] !== val) {
      this.flushIfNeeded();
      this.bp[reg] = val;
    }
    if (reg >= 0xE0 && reg <= 0xE7) {
      this.flushIfNeeded();
      this.writeTevReg(reg, val);
    }
  }

  writeHwToken(token) {
    // PE token register (u16 at PE + 0x0E) for GXReadDrawSync.
    if (!this.hwregBase) return;
    const heap = this.heap();
    const o = this.hwregBase + 0x100E;
    heap[o] = token >> 8;
    heap[o + 1] = token & 0xFF;
  }

  loadTlut(val) {
    const heap = this.heap();
    const src = (this.bp[0x64] & 0xFFFFFF) << 5;
    const tmemOff = (val & 0x3FF) << 9;
    const bytes = ((val >> 10) & 0x7FF) * 32;
    this.tmem.set(heap.subarray(src, src + bytes), tmemOff & 0xFFFFF);
    this.tlutDirty = true;
  }

  // ---------------------------------------------------------- vertex loader

  vatFields(vat) {
    return [this.cp[0x70 + vat], this.cp[0x80 + vat], this.cp[0x90 + vat]];
  }

  // Returns the per-vertex byte size for VAT `vat` with the current VCD.
  vertexSize(vat) {
    const lo = this.cp[0x50], hi = this.cp[0x60];
    const [a, b, c] = this.vatFields(vat);
    const compSize = [1, 1, 2, 2, 4];
    let size = 0;
    if (lo & 1) size++;  // PNMTXIDX
    for (let i = 0; i < 8; i++) if (lo & (2 << i)) size++;
    const idxSize = (t, direct) => t === 0 ? 0 : t === 1 ? direct : t === 2 ? 1 : 2;
    // position
    {
      const t = (lo >> 9) & 3;
      const cnt = (a & 1) ? 3 : 2, fmt = (a >> 1) & 7;
      size += idxSize(t, cnt * compSize[fmt]);
    }
    // normal
    {
      const t = (lo >> 11) & 3;
      const nbt = (a >> 9) & 1, fmt = (a >> 10) & 7, idx3 = (a >> 31) & 1;
      if (t) {
        const n = nbt ? 9 : 3;
        if (t === 1) size += n * compSize[fmt];
        else size += idxSize(t, 0) * (nbt && idx3 ? 3 : 1);
      }
    }
    // colors
    for (let ci = 0; ci < 2; ci++) {
      const t = (lo >> (13 + ci * 2)) & 3;
      const fmt = ci ? (a >> 18) & 7 : (a >> 14) & 7;
      const csz = [2, 3, 4, 2, 3, 4][fmt] || 4;
      size += idxSize(t, csz);
    }
    // texcoords
    const tex = this.texFormats(a, b, c);
    for (let i = 0; i < 8; i++) {
      const t = (hi >> (i * 2)) & 3;
      const [cnt, fmt] = tex[i];
      size += idxSize(t, cnt * compSize[fmt]);
    }
    return size;
  }

  texFormats(a, b, c) {
    return [
      [(a >> 21) & 1 ? 2 : 1, (a >> 22) & 7, (a >> 25) & 31],
      [(b >> 0) & 1 ? 2 : 1, (b >> 1) & 7, (b >> 4) & 31],
      [(b >> 9) & 1 ? 2 : 1, (b >> 10) & 7, (b >> 13) & 31],
      [(b >> 18) & 1 ? 2 : 1, (b >> 19) & 7, (b >> 22) & 31],
      [(b >> 27) & 1 ? 2 : 1, (b >> 28) & 7, c & 31],
      [(c >> 5) & 1 ? 2 : 1, (c >> 6) & 7, (c >> 9) & 31],
      [(c >> 14) & 1 ? 2 : 1, (c >> 15) & 7, (c >> 18) & 31],
      [(c >> 23) & 1 ? 2 : 1, (c >> 24) & 7, (c >> 27) & 31],
    ];
  }

  readComp(dv, p, fmt, scale) {
    switch (fmt) {
      case 0: return dv.getUint8(p) * scale;
      case 1: return dv.getInt8(p) * scale;
      case 2: return dv.getUint16(p, false) * scale;
      case 3: return dv.getInt16(p, false) * scale;
      default: return dv.getFloat32(p, false);
    }
  }

  readColor(dv, p, fmt, out, o) {
    switch (fmt) {
      case 0: {  // RGB565
        const v = dv.getUint16(p, false);
        out[o] = ((v >> 11) & 31) / 31; out[o + 1] = ((v >> 5) & 63) / 63; out[o + 2] = (v & 31) / 31; out[o + 3] = 1;
        return 2;
      }
      case 1:  // RGB888
        out[o] = dv.getUint8(p) / 255; out[o + 1] = dv.getUint8(p + 1) / 255; out[o + 2] = dv.getUint8(p + 2) / 255; out[o + 3] = 1;
        return 3;
      case 2:  // RGB888x
        out[o] = dv.getUint8(p) / 255; out[o + 1] = dv.getUint8(p + 1) / 255; out[o + 2] = dv.getUint8(p + 2) / 255; out[o + 3] = 1;
        return 4;
      case 3: {  // RGBA4444
        const v = dv.getUint16(p, false);
        out[o] = ((v >> 12) & 15) / 15; out[o + 1] = ((v >> 8) & 15) / 15; out[o + 2] = ((v >> 4) & 15) / 15; out[o + 3] = (v & 15) / 15;
        return 2;
      }
      case 4: {  // RGBA6
        const v = (dv.getUint8(p) << 16) | (dv.getUint8(p + 1) << 8) | dv.getUint8(p + 2);
        out[o] = ((v >> 18) & 63) / 63; out[o + 1] = ((v >> 12) & 63) / 63; out[o + 2] = ((v >> 6) & 63) / 63; out[o + 3] = (v & 63) / 63;
        return 3;
      }
      default:  // RGBA8
        out[o] = dv.getUint8(p) / 255; out[o + 1] = dv.getUint8(p + 1) / 255; out[o + 2] = dv.getUint8(p + 2) / 255; out[o + 3] = dv.getUint8(p + 3) / 255;
        return 4;
    }
  }

  // Decodes `count` vertices into this.vtxData; returns the first vertex index.
  loadVertices(dv, vat, count, p, vsize) {
    const lo = this.cp[0x50], hi = this.cp[0x60];
    const [a, b, c] = this.vatFields(vat);
    const tex = this.texFormats(a, b, c);
    const compSize = [1, 1, 2, 2, 4];
    const posCnt = (a & 1) ? 3 : 2, posFmt = (a >> 1) & 7, posScale = 1 / (1 << ((a >> 4) & 31));
    const nrmNbt = (a >> 9) & 1, nrmFmt = (a >> 10) & 7, nrmIdx3 = (a >> 31) & 1;
    const nrmScale = nrmFmt === 1 ? 1 / 64 : nrmFmt === 3 ? 1 / 16384 : 1;
    const col0Fmt = (a >> 14) & 7, col1Fmt = (a >> 18) & 7;
    const mtxA = this.cp[0x30], mtxB = this.cp[0x40];
    const defPn = mtxA & 63;
    const defTex = [(mtxA >> 6) & 63, (mtxA >> 12) & 63, (mtxA >> 18) & 63, (mtxA >> 24) & 63,
      mtxB & 63, (mtxB >> 6) & 63, (mtxB >> 12) & 63, (mtxB >> 18) & 63];

    if (this.vtxCount + count > 65536) this.flush();
    const first = this.vtxCount;
    const out = this.vtxData;
    const arrayAddr = (i, idx) => (this.cp[0xA0 + i] & 0x3FFFFFFF) + idx * this.cp[0xB0 + i];

    for (let v = 0; v < count; v++) {
      const o = (first + v) * VSTRIDE;
      let q = p + v * vsize;
      // defaults
      out[o + V_PNMTX] = defPn;
      for (let i = 0; i < 8; i++) out[o + V_TEXMTX + i] = defTex[i];
      out[o + V_NRM] = 0; out[o + V_NRM + 1] = 0; out[o + V_NRM + 2] = 1;
      out[o + V_COL0] = out[o + V_COL0 + 1] = out[o + V_COL0 + 2] = out[o + V_COL0 + 3] = 1;
      out[o + V_COL1] = out[o + V_COL1 + 1] = out[o + V_COL1 + 2] = out[o + V_COL1 + 3] = 1;
      for (let i = 0; i < 16; i++) out[o + V_TEX + i] = 0;

      if (lo & 1) out[o + V_PNMTX] = dv.getUint8(q++) & 63;
      for (let i = 0; i < 8; i++) if (lo & (2 << i)) out[o + V_TEXMTX + i] = dv.getUint8(q++) & 63;

      // position
      {
        const t = (lo >> 9) & 3;
        let src = q;
        if (t === 1) q += posCnt * compSize[posFmt];
        else if (t === 2) src = arrayAddr(0, dv.getUint8(q++));
        else if (t === 3) { src = arrayAddr(0, dv.getUint16(q, false)); q += 2; }
        if (t) {
          const cs = compSize[posFmt];
          out[o + V_POS] = this.readComp(dv, src, posFmt, posScale);
          out[o + V_POS + 1] = this.readComp(dv, src + cs, posFmt, posScale);
          out[o + V_POS + 2] = posCnt === 3 ? this.readComp(dv, src + cs * 2, posFmt, posScale) : 0;
        }
      }
      // normal (+ binormal/tangent)
      {
        const t = (lo >> 11) & 3;
        if (t) {
          const cs = compSize[nrmFmt];
          const readVec = (src, dst) => {
            out[o + dst] = this.readComp(dv, src, nrmFmt, nrmScale);
            out[o + dst + 1] = this.readComp(dv, src + cs, nrmFmt, nrmScale);
            out[o + dst + 2] = this.readComp(dv, src + cs * 2, nrmFmt, nrmScale);
          };
          if (t === 1) {
            readVec(q, V_NRM); q += cs * 3;
            if (nrmNbt) { readVec(q, V_TAN); q += cs * 3; readVec(q, V_BIN); q += cs * 3; }
          } else {
            const reads = nrmNbt ? (nrmIdx3 ? 3 : 1) : 1;
            const idx = [];
            for (let k = 0; k < reads; k++) {
              if (t === 2) idx.push(dv.getUint8(q++));
              else { idx.push(dv.getUint16(q, false)); q += 2; }
            }
            const base = arrayAddr(1, idx[0]);
            readVec(base, V_NRM);
            if (nrmNbt) {
              if (nrmIdx3) { readVec(arrayAddr(1, idx[1]) + cs * 3, V_TAN); readVec(arrayAddr(1, idx[2]) + cs * 6, V_BIN); }
              else { readVec(base + cs * 3, V_TAN); readVec(base + cs * 6, V_BIN); }
            }
          }
        }
      }
      // colors
      for (let ci = 0; ci < 2; ci++) {
        const t = (lo >> (13 + ci * 2)) & 3;
        if (!t) continue;
        const fmt = ci ? col1Fmt : col0Fmt;
        const dst = ci ? V_COL1 : V_COL0;
        if (t === 1) q += this.readColor(dv, q, fmt, out, o + dst);
        else {
          let idx;
          if (t === 2) idx = dv.getUint8(q++);
          else { idx = dv.getUint16(q, false); q += 2; }
          this.readColor(dv, arrayAddr(2 + ci, idx), fmt, out, o + dst);
        }
      }
      // texcoords
      for (let i = 0; i < 8; i++) {
        const t = (hi >> (i * 2)) & 3;
        if (!t) continue;
        const [cnt, fmt, shift] = tex[i];
        const sc = 1 / (1 << shift);
        const cs = compSize[fmt];
        let src = q;
        if (t === 1) q += cnt * cs;
        else if (t === 2) src = arrayAddr(4 + i, dv.getUint8(q++));
        else { src = arrayAddr(4 + i, dv.getUint16(q, false)); q += 2; }
        out[o + V_TEX + i * 2] = this.readComp(dv, src, fmt, sc);
        out[o + V_TEX + i * 2 + 1] = cnt === 2 ? this.readComp(dv, src + cs, fmt, sc) : 0;
      }
    }
    this.vtxCount += count;
    return first;
  }

  drawPrimitive(dv, prim, vat, count, p, vsize) {
    if (count === 0) return;
    const key = this.stateKey(prim);
    if (this.batchKey !== null && key !== this.batchKey) this.flush();
    if (this.indexCount + count * 6 > this.indices.length) this.flush();
    this.batchKey = key;
    this.batchLines = prim >= PRIM_LINES;
    const f = this.loadVertices(dv, vat, count, p, vsize);
    const idx = this.indices;
    let n = this.indexCount;
    switch (prim) {
      case PRIM_QUADS:
      case PRIM_QUADS2:
        for (let i = 0; i + 3 < count; i += 4) {
          idx[n++] = f + i; idx[n++] = f + i + 1; idx[n++] = f + i + 2;
          idx[n++] = f + i; idx[n++] = f + i + 2; idx[n++] = f + i + 3;
        }
        break;
      case PRIM_TRIS:
        for (let i = 0; i + 2 < count; i += 3) { idx[n++] = f + i; idx[n++] = f + i + 1; idx[n++] = f + i + 2; }
        break;
      case PRIM_TRISTRIP:
        for (let i = 2; i < count; i++) {
          if (i & 1) { idx[n++] = f + i - 1; idx[n++] = f + i - 2; idx[n++] = f + i; }
          else { idx[n++] = f + i - 2; idx[n++] = f + i - 1; idx[n++] = f + i; }
        }
        break;
      case PRIM_TRIFAN:
        for (let i = 2; i < count; i++) { idx[n++] = f; idx[n++] = f + i - 1; idx[n++] = f + i; }
        break;
      case PRIM_LINES:
        for (let i = 0; i + 1 < count; i += 2) { idx[n++] = f + i; idx[n++] = f + i + 1; }
        break;
      case PRIM_LINESTRIP:
        for (let i = 1; i < count; i++) { idx[n++] = f + i - 1; idx[n++] = f + i; }
        break;
      case PRIM_POINTS:
        for (let i = 0; i < count; i++) idx[n++] = f + i;
        break;
    }
    this.indexCount = n;
    this.batchPrim = prim;
  }

  stateKey(prim) {
    return prim >= PRIM_LINES ? prim : 0;
  }

  flushIfNeeded() {
    if (this.indexCount) this.flush();
  }

  // ---------------------------------------------------------- draw

  flush() {
    if (this.indexCount === 0) {
      this.vtxCount = 0;
      this.batchKey = null;
      return;
    }
    const gl = this.gl;
    const shader = this.getProgram();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.efbFbo);
    gl.useProgram(shader.prog);
    this.applyRenderState();
    this.setUniforms(shader);
    this.bindTextures(shader);

    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, this.vtxData.subarray(0, this.vtxCount * VSTRIDE), gl.STREAM_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, this.indices.subarray(0, this.indexCount), gl.STREAM_DRAW);
    const mode = this.batchPrim === PRIM_POINTS ? gl.POINTS : this.batchPrim >= PRIM_LINES ? gl.LINES : gl.TRIANGLES;
    gl.drawElements(mode, this.indexCount, gl.UNSIGNED_INT, 0);
    gl.bindVertexArray(null);
    this.stats.draws++;
    this.stats.verts += this.vtxCount;
    this.indexCount = 0;
    this.vtxCount = 0;
    this.batchKey = null;
  }

  // Render state that is not part of the shader.
  applyRenderState() {
    const gl = this.gl, bp = this.bp, s = this.scale;
    // viewport/scissor from XF viewport and BP scissor
    gl.viewport(0, 0, this.efbW, this.efbH);
    const tl = bp[0x20], br = bp[0x21];
    const y0 = (tl & 0x7FF) - 342, x0 = ((tl >> 12) & 0x7FF) - 342;
    const y1 = (br & 0x7FF) - 342 + 1, x1 = ((br >> 12) & 0x7FF) - 342 + 1;
    gl.enable(gl.SCISSOR_TEST);
    const sx = Math.max(0, x0), sy = Math.max(0, y0);
    const sw = Math.max(0, Math.min(EFB_W, x1) - sx), sh = Math.max(0, Math.min(EFB_H, y1) - sy);
    gl.scissor(sx * s, this.efbH - (sy + sh) * s, sw * s, sh * s);

    // cull mode (GX: 0 none, 1 front(cw on screen), 2 back, 3 all)
    const cull = (bp[0x00] >> 14) & 3;
    if (cull === 0 || this.batchPrim >= PRIM_LINES) gl.disable(gl.CULL_FACE);
    else {
      gl.enable(gl.CULL_FACE);
      // GX front faces are clockwise in screen space; our y flip keeps winding,
      // so GX "front" == GL CW.
      gl.frontFace(gl.CCW);
      gl.cullFace(cull === 1 ? gl.BACK : cull === 2 ? gl.FRONT : gl.FRONT_AND_BACK);
    }

    // depth
    const zm = bp[0x40];
    const funcs = [gl.NEVER, gl.LESS, gl.EQUAL, gl.LEQUAL, gl.GREATER, gl.NOTEQUAL, gl.GEQUAL, gl.ALWAYS];
    if (zm & 1) {
      gl.enable(gl.DEPTH_TEST);
      gl.depthFunc(funcs[(zm >> 1) & 7]);
    } else {
      gl.disable(gl.DEPTH_TEST);
    }
    gl.depthMask(!!(zm & 0x10));
    const vp = this.viewportParams();
    const far = vp.oz / 16777215, near = (vp.oz - vp.sz) / 16777215;
    gl.depthRange(Math.min(Math.max(near, 0), 1), Math.min(Math.max(far, 0), 1));

    // blending
    const cm = bp[0x41];
    const pixFmt = bp[0x43] & 7;
    const hasAlpha = pixFmt === 1;  // RGBA6_Z24
    const factors = (f, isSrc) => {
      switch (f) {
        case 0: return gl.ZERO;
        case 1: return gl.ONE;
        case 2: return isSrc ? gl.DST_COLOR : gl.SRC_COLOR;
        case 3: return isSrc ? gl.ONE_MINUS_DST_COLOR : gl.ONE_MINUS_SRC_COLOR;
        case 4: return gl.SRC_ALPHA;
        case 5: return gl.ONE_MINUS_SRC_ALPHA;
        case 6: return hasAlpha ? gl.DST_ALPHA : gl.ONE;
        case 7: return hasAlpha ? gl.ONE_MINUS_DST_ALPHA : gl.ZERO;
      }
      return gl.ONE;
    };
    if (cm & 1) {
      gl.enable(gl.BLEND);
      if (cm & 0x800) {
        gl.blendEquation(gl.FUNC_REVERSE_SUBTRACT);
        gl.blendFunc(gl.ONE, gl.ONE);
      } else {
        gl.blendEquation(gl.FUNC_ADD);
        gl.blendFuncSeparate(factors((cm >> 8) & 7, true), factors((cm >> 5) & 7, false),
          factors((cm >> 8) & 7, true), factors((cm >> 5) & 7, false));
      }
    } else if (cm & 2) {
      // logic ops: approximate the common ones
      const op = (cm >> 12) & 15;
      gl.enable(gl.BLEND);
      gl.blendEquation(gl.FUNC_ADD);
      if (op === 0) gl.blendFunc(gl.ZERO, gl.ZERO);           // clear
      else if (op === 5) gl.blendFunc(gl.ZERO, gl.ONE);       // noop
      else if (op === 15) gl.blendFunc(gl.ONE, gl.ONE);       // set (approx)
      else gl.blendFunc(gl.ONE, gl.ZERO);                     // copy and others
    } else {
      gl.disable(gl.BLEND);
    }
    const colorUpdate = !!(cm & 8), alphaUpdate = !!(cm & 0x10) && hasAlpha;
    gl.colorMask(colorUpdate, colorUpdate, colorUpdate, alphaUpdate);
  }

  viewportParams() {
    const r = this.xfReg, f = this.xfMem;
    const fl = (i) => { const u = new Uint32Array([r[i]]); return new Float32Array(u.buffer)[0]; };
    return {sx: fl(0x1A), sy: fl(0x1B), sz: fl(0x1C), ox: fl(0x1D), oy: fl(0x1E), oz: fl(0x1F)};
  }

  // ---------------------------------------------------------- shaders

  getProgram() {
    const key = this.shaderKey();
    let s = this.programs.get(key);
    if (!s) {
      const src = buildShaders(this.shaderConfig());
      const prog = this.makeProgram(src.vs, src.fs);
      s = {prog, uni: {}};
      const gl = this.gl;
      const n = gl.getProgramParameter(prog, gl.ACTIVE_UNIFORMS);
      for (let i = 0; i < n; i++) {
        const info = gl.getActiveUniform(prog, i);
        const name = info.name.replace(/\[0\]$/, '');
        s.uni[name] = gl.getUniformLocation(prog, info.name);
      }
      this.programs.set(key, s);
    }
    return s;
  }

  shaderConfig() {
    const bp = this.bp, xr = this.xfReg;
    const gen = bp[0x00];
    const numTev = ((gen >> 10) & 15) + 1;
    const numTexGen = xr[0x3F] & 15;
    const numChan = xr[0x09] & 3;
    const stages = [];
    for (let i = 0; i < numTev; i++) {
      const order = bp[0x28 + (i >> 1)] >> ((i & 1) * 12);
      const ksel = bp[0xF6 + (i >> 1)] >> ((i & 1) * 10);
      stages.push({
        color: bp[0xC0 + i * 2] & 0xFFFFFF,
        alpha: bp[0xC1 + i * 2] & 0xFFFFFF,
        texmap: order & 7, texcoord: (order >> 3) & 7, texEnable: (order >> 6) & 1, chan: (order >> 7) & 7,
        kcsel: (ksel >> 4) & 31, kasel: (ksel >> 9) & 31,
        ind: bp[0x10 + i] & 0x1FFFFF,
      });
    }
    const swaps = [];
    for (let i = 0; i < 4; i++) {
      const lo = bp[0xF6 + i * 2], hi = bp[0xF7 + i * 2];
      swaps.push([lo & 3, (lo >> 2) & 3, hi & 3, (hi >> 2) & 3]);
    }
    const texgens = [];
    for (let i = 0; i < numTexGen; i++) texgens.push({info: xr[0x40 + i], post: xr[0x50 + i]});
    const chans = [];
    for (let i = 0; i < 2; i++) chans.push({color: xr[0x0E + i], alpha: xr[0x10 + i]});
    const numInd = (gen >> 16) & 7;
    return {
      numTev, stages, swaps, texgens, numChan, chans, numInd,
      indOrder: bp[0x27], alphaFunc: bp[0xF3] & 0xFFFFFF,
      dualTex: xr[0x12] & 1,
      proj: xr[0x26] & 1,
      fog: (bp[0xF2] >> 21) & 7,
      zfreeze: 0,
      pixFmt: bp[0x43] & 7,
    };
  }

  shaderKey() {
    const c = this.shaderConfig();
    return JSON.stringify(c);
  }

  setUniforms(shader) {
    const gl = this.gl, u = shader.uni;
    if (u.uPosTex) gl.uniform4fv(u.uPosTex, this.xfMem.subarray(0, 256));
    if (u.uNrm) gl.uniform3fv(u.uNrm, this.xfMem.subarray(0x400, 0x460));
    if (u.uPost) gl.uniform4fv(u.uPost, this.xfMem.subarray(0x500, 0x600));
    const r = this.xfReg;
    const fl = (i) => { const t = new Uint32Array([r[i]]); return new Float32Array(t.buffer)[0]; };
    if (u.uProj) gl.uniform4f(u.uProj, fl(0x20), fl(0x21), fl(0x22), fl(0x23));
    if (u.uProj2) gl.uniform2f(u.uProj2, fl(0x24), fl(0x25));
    const vp = this.viewportParams();
    if (u.uViewport) gl.uniform4f(u.uViewport, vp.sx, vp.sy, vp.ox - 342, vp.oy - 342);
    if (u.uEfbSize) gl.uniform2f(u.uEfbSize, EFB_W, EFB_H);
    const col = (v) => [((v >>> 24) & 255) / 255, ((v >>> 16) & 255) / 255, ((v >>> 8) & 255) / 255, (v & 255) / 255];
    if (u.uChanMat) gl.uniform4fv(u.uChanMat, [...col(r[0x0C]), ...col(r[0x0D])]);
    if (u.uChanAmb) gl.uniform4fv(u.uChanAmb, [...col(r[0x0A]), ...col(r[0x0B])]);
    if (u.uLightColor) {
      const lc = [], lp = [], ld = [], la = [], lk = [];
      const m = this.xfMem, mu = this.xfMemU;
      for (let l = 0; l < 8; l++) {
        const b = 0x600 + l * 16;
        lc.push(...col(mu[b + 3]));
        la.push(m[b + 4], m[b + 5], m[b + 6]);
        lk.push(m[b + 7], m[b + 8], m[b + 9]);
        lp.push(m[b + 10], m[b + 11], m[b + 12]);
        ld.push(m[b + 13], m[b + 14], m[b + 15]);
      }
      gl.uniform4fv(u.uLightColor, lc);
      if (u.uLightPos) gl.uniform3fv(u.uLightPos, lp);
      if (u.uLightDir) gl.uniform3fv(u.uLightDir, ld);
      if (u.uLightCos) gl.uniform3fv(u.uLightCos, la);
      if (u.uLightDist) gl.uniform3fv(u.uLightDist, lk);
    }
    // TEV registers
    const bp = this.bp;
    const s11 = (v) => ((v & 0x7FF) << 21 >> 21) / 255;
    if (u.uTevReg) gl.uniform4fv(u.uTevReg, this.tevRegs);
    if (u.uKonst) gl.uniform4fv(u.uKonst, this.tevKonst);
    const af = bp[0xF3];
    if (u.uAlphaRef) gl.uniform2f(u.uAlphaRef, (af & 255) / 255, ((af >> 8) & 255) / 255);
    // texture sizes for texcoord normalization
    const sizes = [];
    for (let i = 0; i < 8; i++) {
      const img0 = this.texReg(i, 0x88);
      sizes.push((img0 & 0x3FF) + 1, ((img0 >> 10) & 0x3FF) + 1);
    }
    if (u.uTexSize) gl.uniform2fv(u.uTexSize, sizes);
    // indirect
    const indMtx = [];
    for (let i = 0; i < 3; i++) {
      const a = bp[0x06 + i * 3], b = bp[0x07 + i * 3], c = bp[0x08 + i * 3];
      const s11m = (v) => ((v & 0x7FF) << 21 >> 21) / 1024;
      const sc = ((a >> 22) & 3) | (((b >> 22) & 3) << 2) | (((c >> 22) & 1) << 4);
      const scale = Math.pow(2, sc - 17);
      indMtx.push(s11m(a) * 1024 * scale, s11m(b) * 1024 * scale, s11m(c) * 1024 * scale, 0,
        s11m(a >> 11) * 1024 * scale, s11m(b >> 11) * 1024 * scale, s11m(c >> 11) * 1024 * scale, 0);
    }
    if (u.uIndMtx) gl.uniform4fv(u.uIndMtx, indMtx);
    const indScale = [];
    for (let i = 0; i < 4; i++) {
      const reg = bp[0x25 + (i >> 1)] >> ((i & 1) * 8);
      indScale.push(1 / (1 << (reg & 15)), 1 / (1 << ((reg >> 4) & 15)));
    }
    if (u.uIndScale) gl.uniform2fv(u.uIndScale, indScale);
  }

  // TEV color registers / konstants: BP 0xE0-0xE7 hold R,A (even) and B,G
  // (odd) of register i; bit 23 selects the konstant instead of the register.
  writeTevReg(reg, val) {
    const i = (reg - 0xE0) >> 1;
    const s11 = (v) => ((v & 0x7FF) << 21 >> 21) / 255;
    const dst = (val >> 23) & 1 ? this.tevKonst : this.tevRegs;
    if (reg & 1) {
      dst[i * 4 + 2] = s11(val);
      dst[i * 4 + 1] = s11(val >> 12);
    } else {
      dst[i * 4 + 0] = s11(val);
      dst[i * 4 + 3] = s11(val >> 12);
    }
  }

  texReg(map, base) {
    return this.bp[base + (map < 4 ? map : 0x20 + map - 4)];
  }

  // ---------------------------------------------------------- textures

  bindTextures(shader) {
    const gl = this.gl;
    for (let i = 0; i < 8; i++) {
      const uname = 'uTex' + i;
      if (!shader.uni[uname]) continue;
      gl.activeTexture(gl.TEXTURE0 + i);
      const t = this.getTexture(i);
      gl.bindTexture(gl.TEXTURE_2D, t ? t.tex : this.dummyTex);
      gl.uniform1i(shader.uni[uname], i);
      const mode0 = this.texReg(i, 0x80);
      const wrap = (w) => w === 0 ? gl.CLAMP_TO_EDGE : w === 1 ? gl.REPEAT : gl.MIRRORED_REPEAT;
      const smp = this.samplers[i];
      gl.samplerParameteri(smp, gl.TEXTURE_WRAP_S, wrap(mode0 & 3));
      gl.samplerParameteri(smp, gl.TEXTURE_WRAP_T, wrap((mode0 >> 2) & 3));
      const mag = (mode0 >> 4) & 1, min = (mode0 >> 5) & 7;
      gl.samplerParameteri(smp, gl.TEXTURE_MAG_FILTER, mag ? gl.LINEAR : gl.NEAREST);
      const minf = [gl.NEAREST, gl.NEAREST_MIPMAP_NEAREST, gl.NEAREST_MIPMAP_LINEAR, gl.NEAREST,
        gl.LINEAR, gl.LINEAR_MIPMAP_NEAREST, gl.LINEAR_MIPMAP_LINEAR, gl.LINEAR][min];
      gl.samplerParameteri(smp, gl.TEXTURE_MIN_FILTER, t && t.mips ? minf : (min >= 4 ? gl.LINEAR : gl.NEAREST));
      gl.bindSampler(i, smp);
    }
    gl.activeTexture(gl.TEXTURE0);
  }

  getTexture(map) {
    const img0 = this.texReg(map, 0x88), img3 = this.texReg(map, 0x94), tlutReg = this.texReg(map, 0x98);
    const mode1 = this.texReg(map, 0x84);
    const w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    const addr = (img3 & 0xFFFFFF) << 5;
    if (!addr) return null;
    const copy = this.copyTextures.get(addr);
    if (copy) return copy;
    const isPal = fmt === TF.C4 || fmt === TF.C8 || fmt === TF.C14X2;
    const tlutOff = (tlutReg & 0x3FF) << 9, tlutFmt = (tlutReg >> 10) & 3;
    const maxLod = (mode1 >> 8) & 0xFF;
    const mips = maxLod > 0 && !isPal ? Math.min(Math.floor(maxLod / 16) + 1, 11) : 1;
    let key = addr + ':' + fmt + ':' + w + ':' + h + ':' + mips;
    let tlutHash = 0;
    if (isPal) {
      const n = fmt === TF.C4 ? 16 : 256;
      const t = this.tmem.subarray(tlutOff & 0xFFFFF, (tlutOff & 0xFFFFF) + n * 2);
      for (let i = 0; i < t.length; i++) tlutHash = (Math.imul(tlutHash, 31) + t[i]) | 0;
      key += ':' + tlutFmt + ':' + tlutHash;
    }
    let ent = this.texCache.get(key);
    if (ent) return ent;
    // decode
    const heap = this.heap();
    const gl = this.gl;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    let size = 0, lw = w, lh = h, off = addr;
    const tlut = isPal ? this.tmem.subarray(tlutOff & 0xFFFFF) : null;
    for (let level = 0; level < mips; level++) {
      const bytes = textureSize(fmt, lw, lh);
      const rgba = decodeTexture(heap.subarray(off, off + bytes), fmt, lw, lh, tlut, tlutFmt);
      gl.texImage2D(gl.TEXTURE_2D, level, gl.RGBA8, lw, lh, 0, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
      off += bytes;
      size += bytes;
      if (lw === 1 && lh === 1) break;
      lw = Math.max(1, lw >> 1);
      lh = Math.max(1, lh >> 1);
    }
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAX_LEVEL, mips - 1);
    ent = {tex, addr, size, mips: mips > 1, key};
    this.texCache.set(key, ent);
    return ent;
  }

  invalidate(addr, len) {
    addr &= 0x3FFFFFFF;
    const end = addr + len;
    for (const [k, t] of this.texCache) {
      if (t.addr < end && t.addr + t.size > addr) {
        this.gl.deleteTexture(t.tex);
        this.texCache.delete(k);
      }
    }
    for (const [a, t] of this.copyTextures) {
      if (a >= addr && a < end) {
        // The CPU rewrote an EFB copy destination: use memory contents again.
        this.gl.deleteTexture(t.tex);
        this.copyTextures.delete(a);
      }
    }
  }

  // ---------------------------------------------------------- EFB copies

  efbCopy(val) {
    const gl = this.gl, bp = this.bp, s = this.scale;
    const tl = bp[0x49], wh = bp[0x4A];
    const x = tl & 0x3FF, y = (tl >> 10) & 0x3FF;
    const w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    const dst = (bp[0x4B] & 0xFFFFFF) << 5;
    const toXfb = (val >> 14) & 1;
    const clear = (val >> 11) & 1;
    const half = (val >> 9) & 1;

    if (toXfb) {
      const yScale = (bp[0x4E] & 0x1FF) / 256;
      const dh = Math.max(1, Math.round(h * (yScale || 1)));
      this.copyToTexture(this.xfbs, dst, x, y, w, h, w * s, dh * s, 0);
    } else {
      const fmt = ((val >> 4) & 7) | ((val >> 15) & 1) << 3;
      const intensity = (val >> 15) & 1;
      const tw = half ? w >> 1 : w, th = half ? h >> 1 : h;
      this.copyToTexture(this.copyTextures, dst, x, y, w, h, tw * s, th * s, intensity ? 1 : 0, fmt);
      // drop stale decoded textures at that address
      for (const [k, t] of this.texCache) if (t.addr === dst) { gl.deleteTexture(t.tex); this.texCache.delete(k); }
    }

    if (clear) {
      const ar = bp[0x4F], gb = bp[0x50];
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.efbFbo);
      gl.enable(gl.SCISSOR_TEST);
      gl.scissor(x * s, this.efbH - (y + h) * s, w * s, h * s);
      const cm = bp[0x41];
      gl.colorMask(!!(cm & 8), !!(cm & 8), !!(cm & 8), !!(cm & 0x10));
      gl.clearColor(((ar) & 255) / 255, ((gb >> 8) & 255) / 255, (gb & 255) / 255, ((ar >> 8) & 255) / 255);
      gl.depthMask(!!(bp[0x40] & 0x10));
      gl.clearDepth((bp[0x51] & 0xFFFFFF) / 16777215);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      gl.colorMask(true, true, true, true);
    }
  }

  copyToTexture(map, addr, x, y, w, h, tw, th, mode, fmt) {
    const gl = this.gl, s = this.scale;
    let ent = map.get(addr);
    if (!ent || ent.w !== tw || ent.h !== th) {
      if (ent) gl.deleteTexture(ent.tex);
      const tex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, tw, th);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      ent = {tex, w: tw, h: th, addr, size: 0};
      map.set(addr, ent);
    }
    ent.fmt = fmt;
    // draw the EFB region into the texture (handles scaling and conversion)
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.drawFbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, ent.tex, 0);
    gl.viewport(0, 0, tw, th);
    gl.disable(gl.SCISSOR_TEST);
    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);
    gl.colorMask(true, true, true, true);
    gl.useProgram(this.blitProg);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.efbColor);
    gl.bindSampler(0, null);
    gl.uniform1i(gl.getUniformLocation(this.blitProg, 't'), 0);
    // EFB y=0 is the top; texture v=1 is the top row of the EFB texture.
    // Copied textures keep GX orientation (row 0 = top) => v flipped.
    // Texture row 0 (sampled at t = 0) receives the top of the copied region.
    const u0 = x / EFB_W, v0 = 1 - y / EFB_H, uw = w / EFB_W, vh = -h / EFB_H;
    gl.uniform1i(gl.getUniformLocation(this.blitProg, 'uMode'), mode);
    gl.bindVertexArray(this.blitVao);
    gl.uniform4f(gl.getUniformLocation(this.blitProg, 'uSrc'), u0, v0, uw, vh);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindVertexArray(null);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.efbFbo);
  }

  // ---------------------------------------------------------- presentation

  present(xfb, width, height, black) {
    const gl = this.gl;
    const canvas = gl.canvas;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.disable(gl.SCISSOR_TEST);
    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);
    gl.colorMask(true, true, true, true);
    const ent = this.xfbs.get(xfb & 0x3FFFFFFF);
    if (black || !ent) {
      gl.clearColor(0, 0, 0, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      this.frameCount = (this.frameCount || 0) + 1;
      return;
    }
    gl.useProgram(this.blitProg);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, ent.tex);
    gl.bindSampler(0, null);
    gl.uniform1i(gl.getUniformLocation(this.blitProg, 't'), 0);
    // XFB texture: row 0 = top of the picture.
    gl.uniform4f(gl.getUniformLocation(this.blitProg, 'uSrc'), 0, 1, 1, -1);
    gl.uniform1i(gl.getUniformLocation(this.blitProg, 'uMode'), 3);
    gl.bindVertexArray(this.blitVao);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindVertexArray(null);
    this.frameCount = (this.frameCount || 0) + 1;
  }
}

export {VSTRIDE};
