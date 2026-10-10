#!/usr/bin/env python3
"""Build the initial virtual NAND image (web/build/nand.bin) from the decrypted
System Menu contents in orig/<version>/ (00000000.app ... 00000008.app, the
same files the matching build uses).

Image format (all big-endian):
    "WNAND1\\0\\0"  u32 entryCount
    entries: u16 pathLen, path (UTF-8), u8 type (1 = file, 2 = directory),
             u32 size, data
"""

import hashlib
import os
import struct
import sys

SYSMENU = (0x00000001, 0x00000002)

# 4.3U: (content index, content id, type)
CONTENTS_43U = [
    (0, 0x96, 0x0001),
    (1, 0x97, 0x0001),
    (2, 0x09, 0x8001),
    (3, 0x0A, 0x8001),
    (4, 0x0B, 0x8001),
    (5, 0x0C, 0x8001),
    (6, 0x59, 0x8001),
    (7, 0x5A, 0x8001),
    (8, 0x98, 0x0001),
]
TITLE_VERSION_43U = 513
BOOT_INDEX = 8
IOS_43U = 0x0000000100000050

DIRS = [
    "/sys", "/ticket", "/ticket/00000001", "/title", "/title/00000001", "/title/00000001/00000002",
    "/title/00000001/00000002/content", "/title/00000001/00000002/data", "/title/00010001", "/title/00010002",
    "/title/00010004", "/title/00010005", "/title/00010008", "/shared1", "/shared2", "/shared2/sys",
    "/shared2/menu", "/shared2/menu/FaceLib", "/shared2/wc24", "/shared2/wc24/mbox", "/shared2/test2", "/tmp",
    "/import", "/meta", "/wfs",
]

SETTING_TXT = (
    "AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\n"
    "SERNO=100000000\r\nVIDEO=NTSC\r\nGAME=US\r\n"
)


def encrypt_setting(text):
    plain = text.encode("ascii").ljust(0x100, b"\0")
    key = 0x73B5DBFA
    out = bytearray()
    for b in plain:
        out.append(b ^ (key & 0xFF))
        key = ((key << 1) | (key >> 31)) & 0xFFFFFFFF
    return bytes(out)


def make_tmd(contents):
    sig = struct.pack(">I", 0x00010001) + bytes(256) + bytes(60)
    issuer = b"Root-CA00000001-CP00000004".ljust(64, b"\0")
    head = struct.pack(">BBBB", 0, 0, 0, 0)
    head += struct.pack(">Q", IOS_43U)
    head += struct.pack(">QIH", (SYSMENU[0] << 32) | SYSMENU[1], 1, 0)
    head += bytes(0x3E)  # reserved (region, ratings, ipc mask, ...)
    head += struct.pack(">IHHHH", 0, TITLE_VERSION_43U, len(contents), BOOT_INDEX, 0)
    assert len(sig + issuer + head) == 0x1E4, hex(len(sig + issuer + head))
    body = b""
    for index, cid, ctype, data in contents:
        body += struct.pack(">IHHQ", cid, index, ctype, len(data)) + hashlib.sha1(data).digest()
    return sig + issuer + head + body


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    version = sys.argv[1] if len(sys.argv) > 1 else "43U"
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "web", "build", "nand.bin")
    orig = os.path.join(root, "orig", version)

    contents = []
    for index, cid, ctype in CONTENTS_43U:
        path = os.path.join(orig, "%08x.app" % index)
        if not os.path.exists(path):
            sys.exit(f"missing {path}: extract the System Menu WAD contents into orig/{version}/")
        contents.append((index, cid, ctype, open(path, "rb").read()))

    tdir = "/title/%08x/%08x" % SYSMENU
    files = {}
    for index, cid, ctype, data in contents:
        files[f"{tdir}/content/{cid:08x}.app"] = data
    files[f"{tdir}/content/title.tmd"] = make_tmd(contents)
    files[f"{tdir}/data/setting.txt"] = encrypt_setting(SETTING_TXT)

    entries = []
    for d in DIRS:
        entries.append((d, 2, b""))
    for p, data in sorted(files.items()):
        entries.append((p, 1, data))

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path + ".tmp", "wb") as f:
        f.write(b"WNAND1\0\0" + struct.pack(">I", len(entries)))
        for p, t, data in entries:
            pb = p.encode()
            f.write(struct.pack(">H", len(pb)) + pb + struct.pack(">BI", t, len(data)) + data)
    os.replace(out_path + ".tmp", out_path)
    print(f"wrote {out_path} ({len(entries)} entries)")


if __name__ == "__main__":
    main()
