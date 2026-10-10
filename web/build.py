#!/usr/bin/env python3
"""Web build of the Wii Menu decompilation.

Every C/C++ unit is compiled by clang for big-endian PowerPC to LLVM IR, run
through the BESwap pass (web/tools/beswap) which retargets it to wasm32 while
keeping big-endian memory, then compiled and linked by Emscripten together
with the HLE layer in web/hal and the JavaScript runtime in web/js.

Usage:
    python3 web/build.py            # writes web/build/build.ninja and runs ninja
    python3 web/build.py --debug    # unoptimized wasm with assertions
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "web", "tools"))
import ninja_syntax  # noqa: E402
sys.path.insert(0, WEB)
from patches import PATCHES  # noqa: E402

# Units of the original DOL that are not compiled for the web: they drive
# hardware that the HLE layer in web/hal replaces, or are MWCC runtime support
# that clang provides itself.
EXCLUDE_LIBS = {
    "TRK_Hollywood_Revolution",  # debugger stub
    "Runtime.PPCEABI.H",  # MWCC runtime (clang provides the C++ ABI)
    "base",  # PPCArch: SPR access
    "exi", "si", "euart",  # serial buses
    "bte", "wud", "usb", "usbcmn", "kbd", "usbkbd", "kpr",  # bluetooth / usb
    "dsp", "ai", "vi", "pad", "wpad", "kpad",  # hardware drivers / controllers
    "ipc",  # replaced by IOS HLE
    "sdi",  # SD host controller
    "rso",  # dynamic PPC modules
}

EXCLUDE_PREFIXES = (
    "libs/RVL_SDK/src/db/",  # debugger (NW4R's "db" library is kept)
)

EXCLUDE_SRCS = {
    "src/BS2/BS2Start.c",  # __start: replaced by web/hal startup
    # os: hardware parts are replaced by web/hal/os
    "libs/RVL_SDK/src/os/OS.c",
    "libs/RVL_SDK/src/os/OSCache.c",
    "libs/RVL_SDK/src/os/OSContext.c",
    "libs/RVL_SDK/src/os/OSError.c",
    "libs/RVL_SDK/src/os/OSExec.c",
    "libs/RVL_SDK/src/os/OSIpc.c",
    "libs/RVL_SDK/src/os/OSLink.c",
    "libs/RVL_SDK/src/os/OSMemory.c",
    "libs/RVL_SDK/src/os/OSReset.c",
    "libs/RVL_SDK/src/os/OSRtc.c",
    "libs/RVL_SDK/src/os/OSSync.c",
    "libs/RVL_SDK/src/os/OSAudioSystem.c",
    "libs/RVL_SDK/src/os/__ppc_eabi_init.cpp",
    # gx: command processor / interrupt plumbing
    "libs/RVL_SDK/src/gx/GXFifo.c",
    "libs/RVL_SDK/src/gx/GXMisc.c",
    "libs/RVL_SDK/src/gx/GXPerf.c",
    # MSL string/memory routines: byte-oriented, so Emscripten's libc versions
    # are equivalent (and these rely on MWCC lvalue casts)
    "libs/MSL/src/MSL_Common/string.c",
    "libs/MSL/src/MSL_Common/mem_funcs.c",
    "libs/MSL/src/MSL_Common/mem.c",
    # MSL platform glue, provided by web/hal
    "libs/MSL/src/PPC_EABI/abort_exit_ppc_eabi.c",
    "libs/MSL/src/PPC_EABI/uart_console_io_gcn.c",
    "libs/MSL/src/PPC_EABI/sysenv.GCN.c",
    "libs/MSL/src/PPC_EABI/time.dolphin.c",
}

CLANG = "clang-18" if shutil.which("clang-18") else "clang"
OPT = "opt-18" if shutil.which("opt-18") else "opt"
LLVM_LINK = "llvm-link-18" if shutil.which("llvm-link-18") else "llvm-link"
LLVM_NM = "llvm-nm-18" if shutil.which("llvm-nm-18") else "llvm-nm"
LLVM_CONFIG = "llvm-config-18" if shutil.which("llvm-config-18") else "llvm-config"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--no-run", action="store_true")
    ap.add_argument("-j", type=int, default=0)
    ap.add_argument("targets", nargs="*")
    args = ap.parse_args()

    units = json.load(open(os.path.join(WEB, "units.json")))
    base_includes = units["includes"]

    bdir = os.path.join(WEB, "build")
    os.makedirs(bdir, exist_ok=True)
    out = open(os.path.join(bdir, "build.ninja"), "w")
    n = ninja_syntax.Writer(out)

    rel = lambda p: os.path.relpath(os.path.join(ROOT, p), bdir)

    n.variable("root", os.path.relpath(ROOT, bdir))
    n.variable("plugin", "beswap.so")
    n.variable("emcc", "emcc")

    # The pass plugin.
    n.rule("plugin", f"g++ -shared -fPIC -O2 $$({LLVM_CONFIG} --cxxflags) $in -o $out", description="PLUGIN $out")
    n.build("beswap.so", "plugin", rel("web/tools/beswap/BESwap.cpp"))

    common = [
        "--target=powerpc-unknown-eabi",
        "-mcpu=750",
        "-mlong-double-64",
        "-fsigned-char",
        "-fno-delete-null-pointer-checks",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-fno-strict-return",
        "-fno-discard-value-names",
        "-fshort-wchar",
        "-fno-common",
        "-femit-all-decls",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-threadsafe-statics",
        "-fno-vectorize",
        "-fno-slp-vectorize",
        "-ffp-contract=off",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-fno-strict-return",
        "-fno-discard-value-names",
        "-nostdinc",
        "-nostdinc++",
        "-w",
        "-Wno-error",
        "-O2",
        "-Xclang", "-disable-llvm-passes",
        "-DRVL_WEB=1",
        "-DNDEBUG=1",
    ]
    cxx = ["-std=gnu++03", "-fms-compatibility", "-fms-extensions", "-fno-delayed-template-parsing", "-fpermissive", "-Wno-c++11-narrowing",
           "-Wno-register", "-fno-cxx-modules", "-Wno-out-of-line-declaration", "-Wno-return-type"]
    cc = ["-std=gnu99", "-fgnu89-inline", "-Wno-implicit-function-declaration", "-Wno-int-conversion",
          "-Wno-incompatible-pointer-types", "-Wno-implicit-int", "-Wno-return-type"]

    n.rule("becc",
           f"{CLANG} $flags -MD -MF $out.d -emit-llvm -c $in -o $out.ppc.bc && "
           f"{OPT} -load-pass-plugin=./$plugin -passes='fixret,fixpmf,function(sroa,early-cse,instcombine<no-verify-fixpoint>,simplifycfg),beswap' $out.ppc.bc -o $out",
           depfile="$out.d", deps="gcc", description="CC $in")
    n.rule("sjis", f"python3 {rel('web/tools/sjis.py')} $sjisflag $in $out", description="SJIS $in")


    import casefix
    sys.path.insert(0, os.path.join(WEB, "tools"))
    cf_dir = os.path.join(bdir, "casefix")
    all_incs = list(dict.fromkeys(base_includes + [i for u in units["units"] for i in u.get("includes", [])]))
    casefix.run(ROOT, all_incs, ["src", "libs"], cf_dir)

    objs = []
    web_inc = ["web/include"]
    for u in units["units"]:
        if u["lib"] in EXCLUDE_LIBS or u["src"] in EXCLUDE_SRCS or u["src"].startswith(EXCLUDE_PREFIXES):
            continue
        objs.append(add_unit(n, u, base_includes, web_inc, common, cc, cxx, rel))

    # HLE layer.
    for dirpath, _, files in os.walk(os.path.join(WEB, "hal")):
        for f in sorted(files):
            if not f.endswith((".c", ".cpp")):
                continue
            src = os.path.relpath(os.path.join(dirpath, f), ROOT)
            u = {"src": src, "lib": "hal", "lang": "c++" if f.endswith(".cpp") else "c", "sjis": False,
                 "defines": ["VERSION_43U", "BUILD_VERSION=0", "SDK_IPL", "TARGET_RVL", "_REVOLUTION",
                             "MEM_MANAGER_DIRECT"]}
            objs.append(add_unit(n, u, base_includes, web_inc + ["web/hal"], common, cc, cxx, rel))

    # Whole-program link: one module, so that cross-unit prototype mismatches
    # can be adapted (befix) and unreachable MWCC leftovers dropped.
    bcs = objs
    with open(os.path.join(bdir, "bitcode.rsp"), "w") as f:
        f.write("\n".join(bcs) + "\n")
    n.rule("wplink", f"{LLVM_LINK} @bitcode.rsp -o $out", description="LLVM-LINK $out")
    n.build("all.bc", "wplink", [], implicit=objs + ["bitcode.rsp"])
    # Symbols the Emscripten runtime libraries define (everything else that is
    # still undefined gets a reporting stub).
    provided = os.path.join(bdir, "provided.txt")
    write_provided_symbols(provided)
    n.rule("wpopt",
           f"BEFIX_PROVIDED=provided.txt BEFIX_ALIASES={rel('web/aliases.txt')} {OPT} -load-pass-plugin=./$plugin -passes='befix,internalize,globaldce' "
           f"-internalize-public-api-list='main,webx_*' $in -o $out 2> befix.log",
           description="WPOPT $out")
    n.build("program.bc", "wpopt", "all.bc", implicit=["beswap.so", rel("web/aliases.txt")])

    js_lib = [rel(os.path.join("web/js", f)) for f in sorted(os.listdir(os.path.join(WEB, "js")))
              if f.startswith("lib_") and f.endswith(".js")]
    link_flags = [
        "-O2" if not args.debug else "-O1",
        "-Wno-override-module",
        "--profiling-funcs",
        f"-sGLOBAL_BASE={0x14000000}",
        "-sINITIAL_MEMORY=576MB",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sMAXIMUM_MEMORY=1GB",
        "-sSTACK_SIZE=1MB",
        "-sTABLE_BASE=65536",  # see FixPmfPass in web/tools/beswap
        "-sENVIRONMENT=web,node",
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','callMain']",
        "-sEXPORTED_FUNCTIONS=['_main','_webx_gx_token','_webx_hwreg','_webx_thread_entry','_webx_www_getset','_webx_www_call','_webx_www_notify','_webx_www_malloc','_webx_www_free']",
        "-sEXIT_RUNTIME=0",
        "-sERROR_ON_UNDEFINED_SYMBOLS=1",
        "-sINVOKE_RUN=0",
        "-sMODULARIZE=1",
        "-sEXPORT_ES6=1",
        "-sEXPORT_NAME=createWii",
    ]
    if args.debug:
        link_flags += ["-sASSERTIONS=1", "-g2"]
    for j in js_lib:
        link_flags += ["--js-library", j]
    n.rule("wasm", f"$emcc {' '.join(quote(f) for f in link_flags)} $mode $in -o $out", description="EMCC $out")
    # JSPI build (default): threads are separate wasm stacks. The Asyncify
    # build (fallback for browsers without JSPI) instruments the whole program
    # and takes far longer to link.
    n.build("wiimenu.js", "wasm", "program.bc", implicit=js_lib,
            variables={"mode": "-sJSPI=1 -sJSPI_EXPORTS=['webx_thread_entry']"})
    n.build("wiimenu-asyncify.js", "wasm", "program.bc", implicit=js_lib,
            variables={"mode": "-sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536"})
    n.default("wiimenu.js")

    out.close()
    if args.no_run:
        return
    cmd = ["ninja", "-C", bdir] + (["-j", str(args.j)] if args.j else []) + ["-k", "0"] + args.targets
    sys.exit(subprocess.call(cmd))


def write_provided_symbols(path):
    emcc = shutil.which("emcc")
    if not emcc:
        sys.exit("emcc not found: source emsdk_env.sh first")
    sysroot = os.path.join(os.path.dirname(os.path.realpath(emcc)), "cache", "sysroot", "lib", "wasm32-emscripten")
    names = set()
    for lib in ("libc.a", "libcompiler_rt.a", "libdlmalloc.a", "libc++abi-noexcept.a", "libstubs.a", "libnoexit.a",
                "libc-debug.a"):
        p = os.path.join(sysroot, lib)
        if not os.path.exists(p):
            continue
        res = subprocess.run([LLVM_NM, "--defined-only", "-g", "--format=just-symbols", p], capture_output=True,
                             text=True)
        names.update(l.strip() for l in res.stdout.splitlines() if l.strip() and not l.endswith(":"))
    if not names:
        sys.exit("could not read Emscripten's libc symbols; build a test program with emcc once")
    with open(path, "w") as f:
        f.write("\n".join(sorted(names)) + "\n")


def add_unit(n, u, base_includes, web_inc, common, cc, cxx, rel):
    src = u["src"]
    obj = "obj/" + src.rsplit(".", 1)[0] + ".bc"
    incs = web_inc + u.get("includes", base_includes)
    srcdir = os.path.dirname(src)
    flags = list(common)
    flags += ["-x", "c++" if u["lang"] == "c++" else "c"]
    flags += cxx if u["lang"] == "c++" else cc
    if u["lib"].startswith("MSL"):
        flags += ["-fno-builtin"]
    flags += ["-D" + d for d in u["defines"]]
    flags += ["-iquote", rel(srcdir)]
    for i in incs:
        flags += ["-I", rel(i)]
    flags += ["-I", "casefix"]
    inp = rel(src)
    if has_non_ascii(os.path.join(ROOT, src)) or src in PATCHES:
        conv = "conv/" + src
        n.build(conv, "sjis", inp, implicit=[rel("web/patches.py")], variables={"sjisflag": "--sjis" if u.get("sjis") else ""})
        inp = conv
    n.build(obj, "becc", inp, implicit=["beswap.so"], variables={"flags": " ".join(quote(f) for f in flags)})
    return obj


def has_non_ascii(path):
    with open(path, "rb") as f:
        return any(b >= 0x80 for b in f.read())


def quote(s):
    if any(c in s for c in " \"'$"):
        return "'" + s.replace("'", "'\\''") + "'"
    return s


if __name__ == "__main__":
    main()
