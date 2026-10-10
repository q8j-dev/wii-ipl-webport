"""Case-insensitive #include support.

The original code was built on Windows, so some #include lines differ in case
from the files on disk. For every include that only resolves when ignoring
case, a symlink with the spelling used in the source is created under the
output directory, which is then added to the include path.
"""
import os
import re

INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def build_index(dirs):
    """Map lower-case relative path -> real path, for each include dir."""
    idx = {}
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for dirpath, _, files in os.walk(d):
            for f in files:
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, d).replace(os.sep, "/")
                idx.setdefault((d, rel.lower()), full)
    return idx


def run(root, inc_dirs, src_dirs, out_dir):
    inc_dirs = [os.path.join(root, d) for d in inc_dirs]
    idx = build_index(inc_dirs)
    created = 0
    sources = []
    for sd in src_dirs + inc_dirs:
        sd = os.path.join(root, sd)
        for dirpath, _, files in os.walk(sd):
            for f in files:
                if f.endswith((".c", ".cpp", ".h", ".hpp", ".inc", "")) and "." in f:
                    sources.append(os.path.join(dirpath, f))
    for path in sources:
        try:
            text = open(path, "rb").read().decode("utf-8", errors="ignore")
        except OSError:
            continue
        for inc in INC_RE.findall(text):
            here = os.path.join(os.path.dirname(path), inc)
            if os.path.exists(here):
                continue
            if any(os.path.exists(os.path.join(d, inc)) for d in inc_dirs):
                continue
            target = None
            # same directory, ignoring case
            dl = os.path.dirname(path)
            cand = os.path.join(dl, inc)
            parent = os.path.dirname(cand)
            if os.path.isdir(parent):
                for f in os.listdir(parent):
                    if f.lower() == os.path.basename(inc).lower():
                        target = os.path.join(parent, f)
            if target is None:
                for d in inc_dirs:
                    t = idx.get((d, inc.lower()))
                    if t:
                        target = t
                        break
            if target is None:
                continue
            link = os.path.join(out_dir, inc)
            if os.path.lexists(link):
                continue
            os.makedirs(os.path.dirname(link), exist_ok=True)
            os.symlink(os.path.abspath(target), link)
            created += 1
    return created
