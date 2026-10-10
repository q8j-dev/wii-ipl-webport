// Virtual NAND file system for the IOS emulation (web/hal/ios_web.c).
//
// The base image (nand.bin, built by web/tools/mknand.py) holds the System
// Menu contents and the initial directory tree. Everything the menu writes is
// kept in an overlay that is saved to IndexedDB, so settings, the channel
// layout, Miis and messages survive reloads.

export class Nand {
  constructor() {
    this.nodes = new Map();  // path -> {dir, data, size}
    this.children = new Map();  // dir path -> Set of names
    this.dirty = new Set();
    this.onDirty = null;
    this.addDir('/');
  }

  static parent(path) {
    const i = path.lastIndexOf('/');
    return i <= 0 ? '/' : path.slice(0, i);
  }

  static norm(path) {
    if (path.length > 1 && path.endsWith('/')) path = path.slice(0, -1);
    return path;
  }

  link(path) {
    if (path === '/') return;
    const p = Nand.parent(path);
    let set = this.children.get(p);
    if (!set) this.children.set(p, set = new Set());
    set.add(path.slice(p === '/' ? 1 : p.length + 1));
  }

  unlink(path) {
    const p = Nand.parent(path);
    const set = this.children.get(p);
    if (set) set.delete(path.slice(p === '/' ? 1 : p.length + 1));
  }

  addDir(path) {
    this.nodes.set(path, {dir: true, data: null, size: 0});
    if (!this.children.has(path)) this.children.set(path, new Set());
    this.link(path);
  }

  addFile(path, data) {
    this.nodes.set(path, {dir: false, data, size: data.length});
    this.link(path);
  }

  loadImage(buffer) {
    const dv = new DataView(buffer);
    const magic = new TextDecoder().decode(new Uint8Array(buffer, 0, 6));
    if (magic !== 'WNAND1') throw new Error('bad NAND image');
    const count = dv.getUint32(8);
    let pos = 12;
    for (let i = 0; i < count; i++) {
      const plen = dv.getUint16(pos); pos += 2;
      const path = new TextDecoder().decode(new Uint8Array(buffer, pos, plen)); pos += plen;
      const type = dv.getUint8(pos); pos += 1;
      const size = dv.getUint32(pos); pos += 4;
      if (type === 2) this.addDir(path);
      else this.addFile(path, new Uint8Array(buffer, pos, size));
      pos += size;
    }
  }

  markDirty(path) {
    this.dirty.add(path);
    if (this.onDirty) this.onDirty();
  }

  stat(path) {
    path = Nand.norm(path);
    const n = this.nodes.get(path);
    if (!n) return null;
    return {dir: n.dir, size: n.size};
  }

  read(path, offset, len) {
    const n = this.nodes.get(Nand.norm(path));
    if (!n || n.dir) return null;
    if (offset >= n.size) return new Uint8Array(0);
    return n.data.subarray(offset, Math.min(n.size, offset + len));
  }

  write(path, offset, bytes, truncate) {
    path = Nand.norm(path);
    const n = this.nodes.get(path);
    if (!n || n.dir) return -106;
    const end = offset + bytes.length;
    if (!n.owned || end > n.data.length) {
      const cap = Math.max(end, Math.min(n.size * 2, end + 0x100000), 64);
      const nd = new Uint8Array(cap);
      nd.set(n.data.subarray(0, n.size));
      n.data = nd;
      n.owned = true;
    }
    n.data.set(bytes, offset);
    n.size = truncate ? end : Math.max(n.size, end);
    this.markDirty(path);
    return bytes.length;
  }

  truncate(path, size) {
    const n = this.nodes.get(Nand.norm(path));
    if (!n || n.dir) return -106;
    n.size = Math.min(size, n.size);
    this.markDirty(Nand.norm(path));
    return 0;
  }

  create(path, isDir) {
    path = Nand.norm(path);
    if (this.nodes.has(path)) return -105;
    if (!this.nodes.has(Nand.parent(path))) return -106;
    if (isDir) this.addDir(path);
    else {
      this.addFile(path, new Uint8Array(64));
      this.nodes.get(path).size = 0;
      this.nodes.get(path).owned = true;
    }
    this.markDirty(path);
    return 0;
  }

  remove(path) {
    path = Nand.norm(path);
    const n = this.nodes.get(path);
    if (!n) return -106;
    if (n.dir) {
      for (const name of [...(this.children.get(path) || [])]) this.remove(path === '/' ? '/' + name : path + '/' + name);
      this.children.delete(path);
    }
    this.nodes.delete(path);
    this.unlink(path);
    this.markDirty(path);
    return 0;
  }

  rename(from, to) {
    from = Nand.norm(from);
    to = Nand.norm(to);
    const n = this.nodes.get(from);
    if (!n) return -106;
    if (!this.nodes.has(Nand.parent(to))) return -106;
    if (this.nodes.has(to)) this.remove(to);
    if (n.dir) {
      this.addDir(to);
      for (const name of [...(this.children.get(from) || [])]) this.rename(from + '/' + name, to + '/' + name);
      this.remove(from);
    } else {
      this.nodes.delete(from);
      this.unlink(from);
      this.nodes.set(to, n);
      this.link(to);
      this.markDirty(from);
    }
    this.markDirty(to);
    return 0;
  }

  readdir(path) {
    path = Nand.norm(path);
    const n = this.nodes.get(path);
    if (!n || !n.dir) return null;
    return [...(this.children.get(path) || [])].sort();
  }

  // ---- persistence ----

  // Returns the changed entries since the last call: [path, node|null].
  takeChanges() {
    const out = [];
    for (const path of this.dirty) {
      const n = this.nodes.get(path);
      out.push([path, n ? {dir: n.dir, data: n.dir ? null : n.data.slice(0, n.size)} : null]);
    }
    this.dirty.clear();
    return out;
  }

  applySaved(entries) {
    // Parents first, deletions last.
    entries.sort((a, b) => a[0].length - b[0].length);
    for (const [path, n] of entries) {
      if (n === null) continue;
      if (this.nodes.has(path)) {
        const cur = this.nodes.get(path);
        if (cur.dir !== n.dir) this.remove(path);
      }
      if (!this.nodes.has(Nand.parent(path))) continue;
      if (n.dir) { if (!this.nodes.has(path)) this.addDir(path); }
      else this.addFile(path, n.data);
    }
    for (const [path, n] of entries) if (n === null && this.nodes.has(path)) this.remove(path);
    this.dirty.clear();
  }
}
