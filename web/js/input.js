// Mouse / keyboard / gamepad -> Wii Remote on channel 0.
//
// Pointer: the canvas shows the 16:9 picture, whose horizontal extent is the
// System Menu's wide projection (+-416 x +-228); KPAD positions are in units
// of 1.2 * half the screen height (see KPADGetProjectionPos).

const BTN = {
  LEFT: 0x0001, RIGHT: 0x0002, DOWN: 0x0004, UP: 0x0008, PLUS: 0x0010,
  TWO: 0x0100, ONE: 0x0200, B: 0x0400, A: 0x0800, MINUS: 0x1000, HOME: 0x8000,
};

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
    this.x = 0;
    this.y = 0;
    this.inside = false;
    this.lastMove = 0;
    const unit = 1.2 * 228;
    const move = (e) => {
      const r = canvas.getBoundingClientRect();
      const u = (e.clientX - r.left) / r.width, v = (e.clientY - r.top) / r.height;
      this.x = (u * 2 - 1) * 416 / unit;
      this.y = (v * 2 - 1) * 228 / unit;
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
      const b = KEYMAP[e.code];
      if (b) { this.keyButtons |= b; e.preventDefault(); }
    });
    window.addEventListener('keyup', (e) => {
      const b = KEYMAP[e.code];
      if (b) { this.keyButtons &= ~b; e.preventDefault(); }
    });
    window.addEventListener('blur', () => { this.keyButtons = 0; this.mouseButtons = 0; });
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
        this.x = Math.max(-1.6, Math.min(1.6, this.x + ax * 0.03));
        this.y = Math.max(-1.0, Math.min(1.0, this.y + ay * 0.03));
        this.inside = true;
      }
    }
    return b;
  }

  state() {
    const buttons = this.mouseButtons | this.keyButtons | this.gamepadButtons();
    return {buttons, x: this.x, y: this.y, pointerValid: this.inside};
  }
}
