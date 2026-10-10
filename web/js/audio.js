// Audio output: the AX mixer (web/hal/ax_web.c) produces 32 kHz stereo
// frames which are queued into an AudioWorklet-free ring of AudioBuffers.

export class Audio {
  constructor() {
    this.ctx = null;
    this.mod = null;
    this.nextTime = 0;
    this.pending = 0;
  }

  attach(mod) { this.mod = mod; }

  resume() {
    if (!this.ctx) {
      try {
        this.ctx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: 32000, latencyHint: 'interactive'});
      } catch (e) {
        this.ctx = new (window.AudioContext || window.webkitAudioContext)();
      }
    }
    if (this.ctx.state === 'suspended') this.ctx.resume();
  }

  // Frames still queued ahead of the playback position.
  queued() {
    if (!this.ctx || this.ctx.state !== 'running') return 0x7FFFFFFF;
    const ahead = this.nextTime - this.ctx.currentTime;
    return ahead > 0 ? Math.floor(ahead * 32000) : 0;
  }

  // Interleaved stereo s16 in wasm memory (native byte order).
  push(ptr, frames) {
    if (!this.ctx || this.ctx.state !== 'running') return;
    const heap = this.mod.HEAPU8;
    const s16 = new Int16Array(heap.buffer, ptr, frames * 2);
    const buf = this.ctx.createBuffer(2, frames, 32000);
    const l = buf.getChannelData(0), r = buf.getChannelData(1);
    for (let i = 0; i < frames; i++) {
      l[i] = s16[i * 2] / 32768;
      r[i] = s16[i * 2 + 1] / 32768;
    }
    const src = this.ctx.createBufferSource();
    src.buffer = buf;
    src.connect(this.ctx.destination);
    const now = this.ctx.currentTime;
    if (this.nextTime < now + 0.02) this.nextTime = now + 0.05;
    src.start(this.nextTime);
    this.nextTime += frames / 32000;
  }
}
