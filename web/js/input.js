// Mouse / keyboard / gamepad -> Wii Remote on channel 0.
//
// The pointer is reported as a normalized screen position (see input_web.c).

const BTN = {
  LEFT: 0x0001, RIGHT: 0x0002, DOWN: 0x0004, UP: 0x0008, PLUS: 0x0010,
  TWO: 0x0100, ONE: 0x0200, B: 0x0400, A: 0x0800, MINUS: 0x1000, HOME: 0x8000,
};

// Every press lasts at least this many polls (frames), like a real button
// press: some screens act only on a button held for several frames (the
// message board opens a letter after five), which a quick click or a tap of
// a key would otherwise not reach.
const MIN_HOLD_POLLS = 6;

const KEYMAP = {
  ArrowLeft: BTN.LEFT, ArrowRight: BTN.RIGHT, ArrowUp: BTN.UP, ArrowDown: BTN.DOWN,
  Enter: BTN.A, Space: BTN.A, KeyZ: BTN.A, KeyX: BTN.B, Backspace: BTN.B, Escape: BTN.B,
  Equal: BTN.PLUS, NumpadAdd: BTN.PLUS, Minus: BTN.MINUS, NumpadSubtract: BTN.MINUS,
  Digit1: BTN.ONE, Digit2: BTN.TWO, KeyH: BTN.HOME, Home: BTN.HOME,
};

export class Input {
  constructor(canvas) {
    this.canvas = canvas;
    this.mouseButtons = 0;
    this.keyButtons = 0;
    // Remaining polls for which each recently pressed button stays down.
    this.minHold = new Map();
    // Console buttons pressed since the last poll: 1 POWER, 2 RESET.
    this.console = 0;
    this.x = 0;
    this.y = 0;
    this.inside = false;
    this.lastMove = 0;
    const move = (e) => {
      const r = canvas.getBoundingClientRect();
      const u = (e.clientX - r.left) / r.width, v = (e.clientY - r.top) / r.height;
      // Normalized screen position in [-1, 1]; input_web.c converts it to
      // KPAD units for the current aspect ratio.
      this.x = u * 2 - 1;
      this.y = v * 2 - 1;
      this.inside = u >= 0 && u <= 1 && v >= 0 && v <= 1;
      this.lastMove = performance.now();
    };
    canvas.addEventListener('pointermove', move);
    canvas.addEventListener('pointerdown', (e) => {
      move(e);
      canvas.setPointerCapture?.(e.pointerId);
      if (e.button === 0) this.mouseButtons |= BTN.A;
      if (e.button === 2) this.mouseButtons |= BTN.B;
      if (e.button === 1) this.mouseButtons |= BTN.HOME;
      this.press(this.mouseButtons);
      e.preventDefault();
    });
    canvas.addEventListener('pointerup', (e) => {
      if (e.button === 0) this.mouseButtons &= ~BTN.A;
      if (e.button === 2) this.mouseButtons &= ~BTN.B;
      if (e.button === 1) this.mouseButtons &= ~BTN.HOME;
    });
    canvas.addEventListener('pointerleave', () => { this.inside = false; });
    canvas.addEventListener('pointerenter', (e) => move(e));
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    window.addEventListener('keydown', (e) => {
      if (!e.repeat && e.code === 'KeyP') this.console |= 1;
      if (!e.repeat && e.code === 'KeyR') this.console |= 2;
      const b = KEYMAP[e.code];
      if (b) { this.keyButtons |= b; this.press(b); e.preventDefault(); }
    });
    window.addEventListener('keyup', (e) => {
      const b = KEYMAP[e.code];
      if (b) { this.keyButtons &= ~b; e.preventDefault(); }
    });
    window.addEventListener('blur', () => { this.keyButtons = 0; this.mouseButtons = 0; });
  }

  press(bits) {
    for (let b = 1; b <= bits; b <<= 1) if (bits & b) this.minHold.set(b, MIN_HOLD_POLLS);
  }

  gamepadButtons() {
    let b = 0;
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const p of pads) {
      if (!p) continue;
      const pr = (i) => p.buttons[i] && p.buttons[i].pressed;
      if (pr(0)) b |= BTN.A;
      if (pr(1)) b |= BTN.B;
      if (pr(2)) b |= BTN.ONE;
      if (pr(3)) b |= BTN.TWO;
      if (pr(8)) b |= BTN.MINUS;
      if (pr(9)) b |= BTN.PLUS;
      if (pr(16)) b |= BTN.HOME;
      if (pr(12)) b |= BTN.UP;
      if (pr(13)) b |= BTN.DOWN;
      if (pr(14)) b |= BTN.LEFT;
      if (pr(15)) b |= BTN.RIGHT;
      // left stick moves the pointer
      const ax = p.axes[0] || 0, ay = p.axes[1] || 0;
      if (Math.abs(ax) > 0.15 || Math.abs(ay) > 0.15) {
        this.x = Math.max(-1, Math.min(1, this.x + ax * 0.02));
        this.y = Math.max(-1, Math.min(1, this.y + ay * 0.03));
        this.inside = true;
      }
    }
    return b;
  }

  state() {
    let buttons = this.mouseButtons | this.keyButtons | this.gamepadButtons();
    for (const [b, n] of this.minHold) {
      buttons |= b;
      if (n <= 1) this.minHold.delete(b); else this.minHold.set(b, n - 1);
    }
    const consoleButtons = this.console;
    this.console = 0;
    return {buttons, x: this.x, y: this.y, pointerValid: this.inside, console: consoleButtons};
  }
}
