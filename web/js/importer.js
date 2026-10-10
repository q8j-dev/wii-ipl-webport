// Builds the System Menu's NAND image in the browser from files the user
// provides, so the page itself carries no Nintendo data. Accepted inputs:
//   - nand.bin made by web/tools/mknand.py
//   - the decrypted contents 00000000.app ... 00000008.app (files or a zip),
//     the same files the decompilation's orig/43U directory holds
//   - the System Menu 4.3U WAD (or a zip containing it), decrypted with a
//     Wii common key the user supplies
// The result is the same image mknand.py writes.

const SYSMENU_TID_HI = 0x00000001, SYSMENU_TID_LO = 0x00000002;
const TITLE_VERSION_43U = 513;
const BOOT_INDEX = 8;
const IOS_43U_HI = 0x00000001, IOS_43U_LO = 0x00000050;

// 4.3U: [content index, content id, type]
export const CONTENTS_43U = [
  [0, 0x96, 0x0001], [1, 0x97, 0x0001], [2, 0x09, 0x8001], [3, 0x0A, 0x8001], [4, 0x0B, 0x8001],
  [5, 0x0C, 0x8001], [6, 0x59, 0x8001], [7, 0x5A, 0x8001], [8, 0x98, 0x0001],
];

const DIRS = [
  '/sys', '/ticket', '/ticket/00000001', '/title', '/title/00000001', '/title/00000001/00000002',
  '/title/00000001/00000002/content', '/title/00000001/00000002/data', '/title/00010001', '/title/00010002',
  '/title/00010004', '/title/00010005', '/title/00010008', '/shared1', '/shared2', '/shared2/sys',
  '/shared2/menu', '/shared2/menu/FaceLib', '/shared2/wc24', '/shared2/wc24/mbox', '/shared2/test2', '/tmp',
  '/import', '/meta', '/wfs',
];

const SETTING_TXT = 'AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\n' +
  'SERNO=100000000\r\nVIDEO=NTSC\r\nGAME=US\r\n';

const hex8 = (n) => n.toString(16).padStart(8, '0');

function encryptSetting(text) {
  const out = new Uint8Array(0x100);
  let key = 0x73B5DBFA;
  for (let i = 0; i < 0x100; i++) {
    const b = i < text.length ? text.charCodeAt(i) : 0;
    out[i] = b ^ (key & 0xFF);
    key = ((key << 1) | (key >>> 31)) >>> 0;
  }
  return out;
}

async function sha1(data) {
  return new Uint8Array(await crypto.subtle.digest('SHA-1', data));
}

// A TMD as IOS stores it (unsigned): the menu reads its contents list.
async function makeTmd(contents) {
  const head = 0x1E4;
  const out = new Uint8Array(head + contents.length * 36);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, 0x00010001);
  const issuer = 'Root-CA00000001-CP00000004';
  for (let i = 0; i < issuer.length; i++) out[0x140 + i] = issuer.charCodeAt(i);
  dv.setUint32(0x184, IOS_43U_HI);
  dv.setUint32(0x188, IOS_43U_LO);
  dv.setUint32(0x18C, SYSMENU_TID_HI);
  dv.setUint32(0x190, SYSMENU_TID_LO);
  dv.setUint32(0x194, 1);
  dv.setUint16(0x1DC, TITLE_VERSION_43U);
  dv.setUint16(0x1DE, contents.length);
  dv.setUint16(0x1E0, BOOT_INDEX);
  for (let i = 0; i < contents.length; i++) {
    const [index, cid, type, data] = contents[i];
    const o = head + i * 36;
    dv.setUint32(o, cid);
    dv.setUint16(o + 4, index);
    dv.setUint16(o + 6, type);
    dv.setUint32(o + 8, 0);
    dv.setUint32(o + 12, data.length);
    out.set(await sha1(data), o + 16);
  }
  return out;
}

// contents: [index, cid, type, Uint8Array] for every 4.3U content.
export async function buildImage(contents, tmd) {
  const tdir = '/title/00000001/00000002';
  const files = new Map();
  for (const [, cid, , data] of contents) files.set(`${tdir}/content/${hex8(cid)}.app`, data);
  files.set(`${tdir}/content/title.tmd`, tmd || await makeTmd(contents));
  files.set(`${tdir}/data/setting.txt`, encryptSetting(SETTING_TXT));
  const entries = DIRS.map((d) => [d, 2, new Uint8Array(0)]);
  for (const p of [...files.keys()].sort()) entries.push([p, 1, files.get(p)]);

  const enc = new TextEncoder();
  let size = 12;
  const paths = entries.map(([p]) => enc.encode(p));
  entries.forEach(([, , d], i) => { size += 2 + paths[i].length + 5 + d.length; });
  const out = new Uint8Array(size);
  const dv = new DataView(out.buffer);
  out.set(enc.encode('WNAND1'), 0);
  dv.setUint32(8, entries.length);
  let pos = 12;
  entries.forEach(([, type, data], i) => {
    dv.setUint16(pos, paths[i].length); pos += 2;
    out.set(paths[i], pos); pos += paths[i].length;
    out[pos++] = type;
    dv.setUint32(pos, data.length); pos += 4;
    out.set(data, pos); pos += data.length;
  });
  return out.buffer;
}

// ---------------------------------------------------------------- zip

async function inflateRaw(data) {
  const ds = new DecompressionStream('deflate-raw');
  const stream = new Blob([data]).stream().pipeThrough(ds);
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Returns [{name, data: () => Promise<Uint8Array>}].
export function readZip(buf) {
  const u8 = new Uint8Array(buf);
  const dv = new DataView(buf);
  let eocd = -1;
  for (let i = u8.length - 22; i >= Math.max(0, u8.length - 70000); i--) {
    if (dv.getUint32(i, true) === 0x06054B50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('not a zip file');
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (dv.getUint32(p, true) !== 0x02014B50) break;
    const method = dv.getUint16(p + 10, true);
    const csize = dv.getUint32(p + 20, true);
    const nlen = dv.getUint16(p + 28, true), xlen = dv.getUint16(p + 30, true), clen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = new TextDecoder().decode(u8.subarray(p + 46, p + 46 + nlen));
    p += 46 + nlen + xlen + clen;
    const lnlen = dv.getUint16(local + 26, true), lxlen = dv.getUint16(local + 28, true);
    const start = local + 30 + lnlen + lxlen;
    const raw = u8.subarray(start, start + csize);
    out.push({name, data: () => method === 0 ? Promise.resolve(raw.slice()) : inflateRaw(raw)});
  }
  return out;
}

// ---------------------------------------------------------------- WAD

async function aesCbcDecrypt(keyBytes, iv, data) {
  // WebCrypto insists on PKCS#7 padding: append a block that decrypts to a
  // full padding block (the encryption of an empty message chained on the
  // last ciphertext block).
  const key = await crypto.subtle.importKey('raw', keyBytes, 'AES-CBC', false, ['encrypt', 'decrypt']);
  const last = data.subarray(data.length - 16);
  const pad = new Uint8Array(await crypto.subtle.encrypt({name: 'AES-CBC', iv: last}, key, new Uint8Array(0)));
  const all = new Uint8Array(data.length + 16);
  all.set(data);
  all.set(pad.subarray(0, 16), data.length);
  return new Uint8Array(await crypto.subtle.decrypt({name: 'AES-CBC', iv}, key, all));
}

const align = (n, a) => (n + a - 1) & ~(a - 1);

export function parseKey(text) {
  const h = String(text || '').replace(/[^0-9a-fA-F]/g, '');
  if (h.length !== 32) throw new Error('the Wii common key is 16 bytes (32 hexadecimal digits)');
  const k = new Uint8Array(16);
  for (let i = 0; i < 16; i++) k[i] = parseInt(h.substr(i * 2, 2), 16);
  return k;
}

export async function contentsFromWad(buf, commonKey) {
  const u8 = new Uint8Array(buf);
  const dv = new DataView(buf);
  const hdr = dv.getUint32(0), certSize = dv.getUint32(8), tikSize = dv.getUint32(16), tmdSize = dv.getUint32(20);
  if (hdr !== 0x20) throw new Error('not a WAD file');
  const tikOff = align(hdr, 0x40) + align(certSize, 0x40);
  const tmdOff = tikOff + align(tikSize, 0x40);
  let dataOff = tmdOff + align(tmdSize, 0x40);
  const tik = u8.subarray(tikOff, tikOff + tikSize);
  const tmd = u8.slice(tmdOff, tmdOff + tmdSize);
  const tdv = new DataView(tmd.buffer);
  if (tdv.getUint32(0x18C) !== SYSMENU_TID_HI || tdv.getUint32(0x190) !== SYSMENU_TID_LO) {
    throw new Error('this WAD is not the Wii System Menu');
  }
  const version = tdv.getUint16(0x1DC);
  if (version !== TITLE_VERSION_43U) throw new Error(`this is System Menu version ${version}; version 513 (4.3U) is required`);
  const iv = new Uint8Array(16);
  iv.set(tik.subarray(0x1DC, 0x1E4));
  const titleKey = (await aesCbcDecrypt(commonKey, iv, tik.slice(0x1BF, 0x1BF + 16))).subarray(0, 16);
  const n = tdv.getUint16(0x1DE);
  const contents = [];
  for (let i = 0; i < n; i++) {
    const o = 0x1E4 + i * 36;
    const cid = tdv.getUint32(o), index = tdv.getUint16(o + 4), type = tdv.getUint16(o + 6);
    const size = Number(tdv.getBigUint64(o + 8));
    const hash = tmd.subarray(o + 16, o + 36);
    const encSize = align(size, 16);
    const civ = new Uint8Array(16);
    civ[0] = index >> 8;
    civ[1] = index & 0xFF;
    const plain = (await aesCbcDecrypt(titleKey, civ, u8.subarray(dataOff, dataOff + encSize))).slice(0, size);
    const got = await sha1(plain);
    if (got.some((b, j) => b !== hash[j])) throw new Error('decryption failed: wrong common key?');
    contents.push([index, cid, type, plain]);
    dataOff += align(encSize, 0x40);
  }
  return {contents, tmd};
}

// ---------------------------------------------------------------- inputs

// files: File[] picked or dropped by the user. Returns a NAND image buffer.
export async function importFiles(files, keyText, progress = () => {}) {
  const items = [];
  for (const f of files) {
    const buf = await f.arrayBuffer();
    if (/\.zip$/i.test(f.name)) {
      for (const e of readZip(buf)) items.push({name: e.name.split('/').pop(), data: e.data});
    } else {
      items.push({name: f.name, data: async () => new Uint8Array(buf)});
    }
  }
  const nand = items.find((i) => /\.bin$/i.test(i.name));
  if (nand) {
    const d = await nand.data();
    if (new TextDecoder().decode(d.subarray(0, 6)) === 'WNAND1') return d.buffer.slice(d.byteOffset, d.byteOffset + d.length);
  }
  const wad = items.find((i) => /\.wad$/i.test(i.name));
  if (wad) {
    progress('Decrypting the WAD…');
    const d = await wad.data();
    const {contents, tmd} = await contentsFromWad(d.buffer.slice(d.byteOffset, d.byteOffset + d.length), parseKey(keyText));
    progress('Building the NAND…');
    return buildImage(contents, tmd);
  }
  const contents = [];
  for (const [index, cid, type] of CONTENTS_43U) {
    const name = hex8(index) + '.app';
    const item = items.find((i) => i.name.toLowerCase() === name || i.name.toLowerCase() === hex8(cid) + '.app');
    if (!item) throw new Error(`missing ${name}: select the System Menu 4.3U WAD, nand.bin, or all 9 decrypted .app contents`);
    contents.push([index, cid, type, await item.data()]);
  }
  progress('Building the NAND…');
  return buildImage(contents, null);
}
