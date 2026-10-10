# Wii Menu for the web

The decompiled Wii System Menu 4.3U running in a browser: WebAssembly for the
program, WebGL 2 for the graphics, WebAudio for the sound, IndexedDB for the
console's NAND. The menu's own code runs unchanged (the original sources only
gain `#ifdef RVL_WEB` blocks, so the matching build is unaffected); what the
Wii hardware and IOS did is emulated at a high level.

What works: the Health and Safety screen, the channel menu (Disc Channel,
message board with memos and the address book, SD card menu, Wii button), Data
Management, Wii Settings (its HTML pages, with settings written back to
SYSCONF and text entry through the menu's keyboard), the HOME Menu, sound,
16:9 output. Settings and
saved data persist between visits.

Not emulated: discs, SD cards (the slot is empty), USB keyboards, the network
(WiiConnect24, Internet settings connection tests), Wii Remote speakers. Only
the System Menu title is installed, so other channels (Mii Channel, Shop, …)
do not appear.

## Running

Serve the `web/` directory with any static HTTP server and open `index.html`:

```
python3 -m http.server -d web 8000     # then open http://localhost:8000/
```

The page does not include Nintendo's files. On first start it asks for the
System Menu 4.3U from your Wii, as one of:

- the System Menu 4.3U WAD (title 00000001-00000002 v513), or a zip containing
  it, together with the Wii common key;
- the nine decrypted contents `00000000.app` … `00000008.app` (the files the
  decompilation keeps in `orig/43U/`), or a zip of them;
- `nand.bin` made by `web/tools/mknand.py`.

The NAND image is built in the browser and kept in IndexedDB. If
`web/build/nand.bin` exists on the server it is used instead.
`index.html?reset` forgets the imported System Menu and everything saved.

Controls: the mouse is the Wii Remote pointer; left click / Enter / Space / Z
is A, right click / Esc / Backspace / X is B, H is HOME, + and − are Plus and
Minus, 1 and 2 are the 1 and 2 buttons, arrow keys are the D-pad. A gamepad
works too (left stick moves the pointer).

Browser support: the default build uses WebAssembly JavaScript Promise
Integration (JSPI) for the menu's threads: Chrome and Edge 137 or later. Other
browsers load `wiimenu-asyncify.js` (built separately, see below), which is
larger and slower. WebGL 2 is required.

Query parameters: `scale=N` sets the internal resolution (default: from the
window size, 640×528 × 1–3), `heartbeat` logs diagnostics to the console.

## Building

Requirements: Python 3, Ninja, Clang/LLVM 18 with development headers
(`clang-18`, `opt-18`, `llvm-link-18`, `llvm-config-18`, `llvm-18-dev`), a C++
compiler for the LLVM pass plugin, and Emscripten (built with 6.0.12; activate
it with `source emsdk_env.sh`).

```
python3 web/build.py                 # web/build/wiimenu.js + .wasm (JSPI)
ninja -C web/build wiimenu-asyncify.js   # optional fallback build (slow to link)
python3 web/tools/mknand.py          # optional: web/build/nand.bin from orig/43U/*.app
```

## How it works

**Compilation.** Every unit of the DOL is compiled by clang for big-endian
PowerPC (`powerpc-unknown-eabi`) to LLVM IR. An LLVM plugin
(`tools/beswap/BESwap.cpp`) then retargets the IR to wasm32 while keeping the
Wii's big-endian memory: loads and stores get byte swaps, global initializers
are re-encoded big-endian, pointers in data are swapped by a startup
constructor, variadic arguments are passed pre-swapped, and Wii addresses
(cached/uncached MEM1/MEM2, hardware registers) are translated so that a wasm
address equals the Wii's physical address. This way code that overlays structs
on big-endian files, puns colors and words, or uses MSB-first bitfields works
as written. Further passes repair code clang treats differently from MWCC
(falling off the end of functions, member function pointers to odd table
indices, trapping deleting destructors, C library names), and the whole
program is linked with Emscripten. `patches.py` holds the few source fixups
needed only for clang, applied to copies at build time.

**HLE layer (`hal/`).** Replaces the hardware drivers and IOS:

- OS: the original thread scheduler runs on JSPI stacks (one wasm stack per OS
  thread), interrupts are delivered at interrupt points, alarms and the video
  retrace run on wall-clock time;
- IOS: `/dev/fs` on a virtual NAND (`js/nand.js`), `/dev/es` titles and
  contents, `/dev/di` without a disc, network configuration, the
  WiiConnect24 daemon without a network (the console gets a user ID, so
  memos post to the message board), STM;
- GX: the command FIFO is executed by a GPU emulator in JavaScript (`js/gpu.js`:
  CP/XF/BP registers, vertex loader, generated GLSL for transform, lighting,
  texgen and the TEV combiners, texture decoding, EFB copies);
- audio: the DSP's AX microcode command lists are executed in C (`hal/ax_web.c`:
  ADPCM/PCM voices, resampling, envelopes, main and aux buses) and the output is
  streamed to an AudioWorklet;
- input: KPAD/WPAD from the mouse, keyboard and gamepads; the SDK's KPR
  character processing (dead keys, romaji) for the software keyboard;
- Wii Settings: the menu renders these HTML pages with Opera, an RSO module of
  PowerPC code. Here the browser does it (`hal/www_web.c`, `js/www.js`): pages
  load through the menu's own archive protocol into a hidden iframe, their
  scripts talk to the menu's JavaScript plugins, and the page is rasterized
  into the surface the menu composites, transitions included.
