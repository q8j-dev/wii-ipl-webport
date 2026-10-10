// Headless test run of the web build in Node: no graphics output, just the
// emulated machine. Usage: node web/tools/run_node.mjs [seconds]
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {Nand} from '../js/nand.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const build = path.join(here, '..', 'build');
const seconds = Number(process.argv[2] || 10);
const {default: createWii} = await import(path.join(build, 'wiimenu.js'));

const nand = new Nand();
const img = fs.readFileSync(path.join(build, 'nand.bin'));
nand.loadImage(img.buffer.slice(img.byteOffset, img.byteOffset + img.length));

let line = '';
const start = Date.now();
let frames = 0, gpuBytes = 0;
const wii = {
  nand,
  log(bytes) {
    for (const b of bytes) {
      if (b === 10) { console.log('[wii] ' + line); line = ''; } else line += String.fromCharCode(b);
    }
  },
  fatal(msg) { console.log('FATAL: ' + msg); process.exit(1); },
  wait(ms, forFrame) {
    if ((Date.now() - start) / 1000 > seconds) {
      console.log(`stopping after ${seconds}s: ${frames} frames, ${gpuBytes} GPU bytes`);
      process.exit(0);
    }
    return new Promise(r => setTimeout(r, Math.max(0, ms)));
  },
  gpu: {
    process(ptr, len) { gpuBytes += len; return len; },
    invalidate() {},
    present() { frames++; },
  },
  input: {state() { return {buttons: 0, x: 0, y: 0, pointerValid: false}; }},
  audio: {push() {}, queued() { return 0; }},
};

const mod = await createWii({wii, print: s => console.log(s), printErr: s => console.error(s)});
try {
  mod.callMain([]);
} catch (e) {
  console.log('exception: ' + (e.stack || e));
}
