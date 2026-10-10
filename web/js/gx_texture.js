// GX texture formats -> RGBA8.
//
// Textures are stored in 4x4/8x4/8x8 blocks (32 bytes per block, 64 for
// RGBA8), big-endian. TLUT (palette) entries come from emulated TMEM.

export const TF = {
  I4: 0x0, I8: 0x1, IA4: 0x2, IA8: 0x3, RGB565: 0x4, RGB5A3: 0x5, RGBA8: 0x6,
  C4: 0x8, C8: 0x9, C14X2: 0xA, CMPR: 0xE,
};

export const TLUT_FMT = {IA8: 0, RGB565: 1, RGB5A3: 2};

// Block dimensions (width, height) per format.
export function blockSize(fmt) {
  switch (fmt) {
    case TF.I4: case TF.C4: case TF.CMPR: return [8, 8];
    case TF.I8: case TF.IA4: case TF.C8: return [8, 4];
    default: return [4, 4];
  }
}

export function bytesPerBlock(fmt) {
  return fmt === TF.RGBA8 ? 64 : 32;
}

export function textureSize(fmt, w, h) {
  const [bw, bh] = blockSize(fmt);
  return Math.ceil(w / bw) * Math.ceil(h / bh) * bytesPerBlock(fmt);
}

function rgb565(v, out, o) {
  const r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  out[o] = (r << 3) | (r >> 2);
  out[o + 1] = (g << 2) | (g >> 4);
  out[o + 2] = (b << 3) | (b >> 2);
  out[o + 3] = 255;
}

function rgb5a3(v, out, o) {
  if (v & 0x8000) {
    const r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    out[o] = (r << 3) | (r >> 2);
    out[o + 1] = (g << 3) | (g >> 2);
    out[o + 2] = (b << 3) | (b >> 2);
    out[o + 3] = 255;
  } else {
    const a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    out[o] = r * 17;
    out[o + 1] = g * 17;
    out[o + 2] = b * 17;
    out[o + 3] = (a << 5) | (a << 2) | (a >> 1);
  }
}

function ia8(v, out, o) {
  const i = v & 0xFF;
  out[o] = out[o + 1] = out[o + 2] = i;
  out[o + 3] = v >> 8;
}

function tlutColor(tlut, tlutFmt, index, out, o) {
  const v = (tlut[index * 2] << 8) | tlut[index * 2 + 1];
  if (tlutFmt === TLUT_FMT.RGB565) rgb565(v, out, o);
  else if (tlutFmt === TLUT_FMT.RGB5A3) rgb5a3(v, out, o);
  else ia8(v, out, o);
}

// Decodes a texture; `src` is a Uint8Array starting at the texture data.
export function decodeTexture(src, fmt, w, h, tlut, tlutFmt) {
  const out = new Uint8Array(w * h * 4);
  const [bw, bh] = blockSize(fmt);
  const bx = Math.ceil(w / bw), by = Math.ceil(h / bh);
  let p = 0;
  const put = (x, y) => (y < h && x < w) ? (y * w + x) * 4 : -1;
  const tmp = new Uint8Array(4);
  for (let byi = 0; byi < by; byi++) {
    for (let bxi = 0; bxi < bx; bxi++) {
      const x0 = bxi * bw, y0 = byi * bh;
      switch (fmt) {
        case TF.I4:
          for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x += 2) {
            const b = src[p++];
            let o = put(x0 + x, y0 + y);
            if (o >= 0) { const i = (b >> 4) * 17; out[o] = out[o + 1] = out[o + 2] = out[o + 3] = i; }
            o = put(x0 + x + 1, y0 + y);
            if (o >= 0) { const i = (b & 15) * 17; out[o] = out[o + 1] = out[o + 2] = out[o + 3] = i; }
          }
          break;
        case TF.I8:
          for (let y = 0; y < 4; y++) for (let x = 0; x < 8; x++) {
            const i = src[p++];
            const o = put(x0 + x, y0 + y);
            if (o >= 0) out[o] = out[o + 1] = out[o + 2] = out[o + 3] = i;
          }
          break;
        case TF.IA4:
          for (let y = 0; y < 4; y++) for (let x = 0; x < 8; x++) {
            const b = src[p++];
            const o = put(x0 + x, y0 + y);
            if (o >= 0) { const i = (b & 15) * 17; out[o] = out[o + 1] = out[o + 2] = i; out[o + 3] = (b >> 4) * 17; }
          }
          break;
        case TF.IA8:
          for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) {
            const a = src[p++], i = src[p++];
            const o = put(x0 + x, y0 + y);
            if (o >= 0) { out[o] = out[o + 1] = out[o + 2] = i; out[o + 3] = a; }
          }
          break;
        case TF.RGB565:
          for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) {
            const v = (src[p] << 8) | src[p + 1]; p += 2;
            const o = put(x0 + x, y0 + y);
            if (o >= 0) rgb565(v, out, o);
          }
          break;
        case TF.RGB5A3:
          for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) {
            const v = (src[p] << 8) | src[p + 1]; p += 2;
            const o = put(x0 + x, y0 + y);
            if (o >= 0) rgb5a3(v, out, o);
          }
          break;
        case TF.RGBA8:
          for (let i = 0; i < 16; i++) {
            const o = put(x0 + (i & 3), y0 + (i >> 2));
            if (o >= 0) { out[o + 3] = src[p + i * 2]; out[o] = src[p + i * 2 + 1]; out[o + 1] = src[p + 32 + i * 2]; out[o + 2] = src[p + 32 + i * 2 + 1]; }
          }
          p += 64;
          break;
        case TF.C4:
          for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x += 2) {
            const b = src[p++];
            let o = put(x0 + x, y0 + y);
            if (o >= 0) tlutColor(tlut, tlutFmt, b >> 4, out, o);
            o = put(x0 + x + 1, y0 + y);
            if (o >= 0) tlutColor(tlut, tlutFmt, b & 15, out, o);
          }
          break;
        case TF.C8:
          for (let y = 0; y < 4; y++) for (let x = 0; x < 8; x++) {
            const b = src[p++];
            const o = put(x0 + x, y0 + y);
            if (o >= 0) tlutColor(tlut, tlutFmt, b, out, o);
          }
          break;
        case TF.C14X2:
          for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) {
            const v = ((src[p] << 8) | src[p + 1]) & 0x3FFF; p += 2;
            const o = put(x0 + x, y0 + y);
            if (o >= 0) tlutColor(tlut, tlutFmt, v, out, o);
          }
          break;
        case TF.CMPR:
          for (let sub = 0; sub < 4; sub++) {
            const sx = x0 + (sub & 1) * 4, sy = y0 + (sub >> 1) * 4;
            const c0 = (src[p] << 8) | src[p + 1], c1 = (src[p + 2] << 8) | src[p + 3];
            const pal = new Uint8Array(16);
            rgb565(c0, pal, 0);
            rgb565(c1, pal, 4);
            if (c0 > c1) {
              for (let k = 0; k < 3; k++) {
                pal[8 + k] = ((pal[k] * 5 + pal[4 + k] * 3) >> 3);
                pal[12 + k] = ((pal[k] * 3 + pal[4 + k] * 5) >> 3);
              }
              pal[11] = pal[15] = 255;
            } else {
              for (let k = 0; k < 3; k++) pal[8 + k] = (pal[k] + pal[4 + k]) >> 1;
              pal[11] = 255;
              pal[12] = pal[13] = pal[14] = pal[15] = 0;
            }
            for (let y = 0; y < 4; y++) {
              const bits = src[p + 4 + y];
              for (let x = 0; x < 4; x++) {
                const idx = (bits >> (6 - x * 2)) & 3;
                const o = put(sx + x, sy + y);
                if (o >= 0) { out[o] = pal[idx * 4]; out[o + 1] = pal[idx * 4 + 1]; out[o + 2] = pal[idx * 4 + 2]; out[o + 3] = pal[idx * 4 + 3]; }
              }
            }
            p += 8;
          }
          break;
        default:
          p += bytesPerBlock(fmt);
          for (let y = 0; y < bh; y++) for (let x = 0; x < bw; x++) {
            const o = put(x0 + x, y0 + y);
            if (o >= 0) { out[o] = 255; out[o + 1] = 0; out[o + 2] = 255; out[o + 3] = 255; }
          }
      }
    }
  }
  return out;
}
