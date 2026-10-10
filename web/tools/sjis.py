"""Prepare a source file for clang.

* UTF-16 sources (with BOM) are re-encoded as UTF-8.
* With --sjis, non-ASCII characters inside narrow string and character
  literals are replaced by octal escapes of their Shift-JIS bytes, which is
  what MWCC produces with -enc SJIS. Wide literals keep their characters so
  that clang stores them as UTF-16.
"""
import os
import sys


def read_text(path):
    raw = open(path, "rb").read()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16")
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    return raw.decode("utf-8")


def escape_literals(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j])
            i = j
        elif c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(text[i:j])
            i = j
        elif c in "\"'":
            wide = i > 0 and text[i - 1] in "LuU" and (i < 2 or not (text[i - 2].isalnum() or text[i - 2] == "_"))
            j = i + 1
            buf = [c]
            while j < n and text[j] != c and text[j] != "\n":
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                ch = text[j]
                if ord(ch) >= 0x80 and not wide:
                    for b in ch.encode("cp932", errors="replace"):
                        buf.append("\\%03o" % b)
                else:
                    buf.append(ch)
                j += 1
            if j < n and text[j] == c:
                buf.append(c)
                j += 1
            out.append("".join(buf))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def apply_patches(src, text):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    from patches import PATCHES
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    rel = os.path.relpath(os.path.abspath(src), root)
    for p in PATCHES.get(rel, []):
        old, new = p[0], p[1]
        count = p[2] if len(p) > 2 else 1
        if text.count(old) != count:
            sys.exit(f"patch for {rel} does not apply ({text.count(old)} matches): {old[:60]!r}")
        text = text.replace(old, new)
    return text


def main():
    args = sys.argv[1:]
    sjis = "--sjis" in args
    args = [a for a in args if a != "--sjis"]
    src, dst = args
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    text = read_text(src)
    text = apply_patches(src, text)
    if sjis:
        text = escape_literals(text)
    open(dst, "w", encoding="utf-8").write(text)


main()
