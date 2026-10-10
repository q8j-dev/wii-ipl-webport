// Audio output. The DSP emulation (web/hal/ax_web.c) hands over each 3 ms
// frame the audio interface would play: 96 stereo samples at 32 kHz, 16-bit
// big-endian, right channel first. Frames are batched and streamed into an
// AudioWorklet ring buffer running at 32 kHz.

const RATE = 32000;
const BATCH = 384;  // frames per message (12 ms)

const WORKLET = `
class WiiAudio extends AudioWorkletProcessor {
  constructor(options) {
    super();
    // Input samples per output sample (1 unless the context rate differs).
    this.step = (options.processorOptions && options.processorOptions.step) || 1;
    this.frac = 0;
    this.size = 16384;
    this.l = new Float32Array(this.size);
    this.r = new Float32Array(this.size);
    this.read = 0;
    this.fill = 0;
    this.primed = false;
    this.port.onmessage = (e) => {
      const d = e.data, n = d.length >> 1;
      // Too far ahead (the producer's clock runs fast): drop the oldest.
      const max = ${Math.round(RATE * 0.15)};
      if (this.fill + n > max) {
        const drop = this.fill + n - max;
        this.read = (this.read + drop) % this.size;
        this.fill -= drop;
      }
      let w = (this.read + this.fill) % this.size;
      for (let i = 0; i < n; i++) {
        this.l[w] = d[i * 2];
        this.r[w] = d[i * 2 + 1];
        w = (w + 1) % this.size;
      }
      this.fill += n;
    };
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length;
    if (!this.primed) {
      if (this.fill < ${Math.round(RATE * 0.03)}) return true;
      this.primed = true;
    }
    for (let i = 0; i < n; i++) {
      if (this.fill > 1) {
        const a = this.read, b = (a + 1) % this.size, f = this.frac;
        L[i] = this.l[a] + (this.l[b] - this.l[a]) * f;
        R[i] = this.r[a] + (this.r[b] - this.r[a]) * f;
        this.frac += this.step;
        while (this.frac >= 1) {
          this.frac -= 1;
          this.read = (this.read + 1) % this.size;
          this.fill--;
        }
      } else {
        L[i] = R[i] = 0;
        this.primed = false;
      }
    }
    return true;
  }
}
registerProcessor('wii-audio', WiiAudio);
`;

export class Audio {
  constructor() {
    this.ctx = null;
    this.node = null;
    this.mod = null;
    this.batch = new Float32Array(BATCH * 2);
    this.batchLen = 0;
    this.starting = false;
    this.stats = {frames: 0, peak: 0, nonzero: 0};
  }

  attach(mod) { this.mod = mod; }

  // Browsers start audio only after a user gesture.
  async resume() {
    if (this.ctx) {
      if (this.ctx.state === 'suspended') this.ctx.resume();
      return;
    }
    if (this.starting) return;
    this.starting = true;
    try {
      const AC = window.AudioContext || window.webkitAudioContext;
      try {
        this.ctx = new AC({sampleRate: RATE, latencyHint: 'interactive'});
      } catch (e) {
        this.ctx = new AC({latencyHint: 'interactive'});
      }
      const url = URL.createObjectURL(new Blob([WORKLET], {type: 'text/javascript'}));
      await this.ctx.audioWorklet.addModule(url);
      this.node = new AudioWorkletNode(this.ctx, 'wii-audio', {
        numberOfInputs: 0, outputChannelCount: [2], processorOptions: {step: RATE / this.ctx.sampleRate}});
      this.node.connect(this.ctx.destination);
      if (this.ctx.state === 'suspended') await this.ctx.resume();
    } catch (e) {
      console.warn('audio unavailable:', e);
    }
  }

  queued() { return 0; }

  push(ptr, frames) {
    const heap = this.mod.HEAPU8;
    if (this.debug) {
      const st = this.stats;
      st.frames += frames;
      for (let i = 0; i < frames * 2; i++) {
        const v = Math.abs((heap[ptr + i * 2] << 24 >> 16) | heap[ptr + i * 2 + 1]);
        if (v) { st.nonzero++; if (v > st.peak) st.peak = v; }
      }
    }
    if (!this.node || this.ctx.state !== 'running') return;
    let b = this.batch, n = this.batchLen;
    for (let i = 0; i < frames; i++) {
      const o = ptr + i * 4;
      const r = (heap[o] << 24 >> 16) | heap[o + 1];
      const l = (heap[o + 2] << 24 >> 16) | heap[o + 3];
      b[n * 2] = l / 32768;
      b[n * 2 + 1] = r / 32768;
      if (++n === BATCH) {
        this.node.port.postMessage(b, [b.buffer]);
        b = this.batch = new Float32Array(BATCH * 2);
        n = 0;
      }
    }
    this.batchLen = n;
  }
}
