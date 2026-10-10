"""Extract the compilation units of the matching build from build.ninja."""
import json, re, shlex, sys

def parse(path):
    text = open(path).read().replace("$\n", "")
    units = []
    cur = None
    for line in text.split("\n"):
        m = re.match(r"# (.*): (\S+) \(linked (True|False)\)", line)
        if m:
            pending = (m.group(1), m.group(2), m.group(3) == "True")
            continue
        m = re.match(r"build (\S+\.o):\s+(mwcc\S*)\s+(\S+)", line)
        if m:
            cur = {"obj": m.group(1), "rule": m.group(2), "src": m.group(3), "linked": pending[2], "lib": pending[1]}
            units.append(cur)
            continue
        if cur is not None:
            m = re.match(r"\s+cflags = (.*)", line)
            if m:
                fl = shlex.split(m.group(1))
                defs, incs = [], []
                i = 0
                while i < len(fl):
                    f = fl[i]
                    if f.startswith("-D"):
                        defs.append(f[2:])
                    elif f in ("-i", "-ir", "-I"):
                        incs.append(fl[i + 1]); i += 1
                    elif f.startswith("-lang"):
                        cur["lang"] = fl[i + 1] if f == "-lang" else f
                    i += 1
                cur["defines"] = defs
                cur["includes"] = incs
                cur["cflags"] = fl
            if not line.startswith(" "):
                cur = None if not line.startswith("build") else cur
    return units

if __name__ == "__main__":
    u = parse(sys.argv[1])
    json.dump(u, open(sys.argv[2], "w"), indent=1)
    print(len(u), "units,", sum(x["linked"] for x in u), "linked")
