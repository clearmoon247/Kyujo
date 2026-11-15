#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds a native shell around the sandboxed core: wasm2c turns build/kyujo.wasm into C, which is compiled for the shell's target.

    build_shell.py --wasm bw/kyujo.wasm --wabt wabt-checkout --out bw/shell-linux --front headless [--cc clang] [--opt O1] [--jobs 2]

Fronts: `headless` (a desktop Linux test shell, no window, no sound) and `sdl` (window, controllers and sound through SDL2: the shell for the Nintendo
Switch, Android, and desktop Linux for development). Targets: `linux` (the host compiler), `switch` (devkitA64: needs --devkitpro DIR; produces an .nro
with elf2nro) and `android` (the NDK's clang: needs --ndk DIR, --abi, --sdl-include and --sdl-lib; produces libmain.so, the game library of the app that
tools/android/build_apk.py packs). Everything under shell/core is shared by all of them.

    build_shell.py --wasm bw/kyujo.wasm --wabt wabt-checkout --out bw/shell-switch --front sdl --target switch --devkitpro /opt/devkitpro --gpu
The wasm2c translator and runtime are wabt's (Apache-2.0): `--wabt` is a checkout whose build/wasm2c exists and wasm2c/ holds the runtime sources.
Memory is checked with explicit bounds tests (WASM_RT_USE_MMAP=0): no reserved address space is needed, which Horizon and Android limit.
Standard library only."""
import argparse
import glob
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def read_version():
    """The version in VERSION (first line that is not a comment); the Android app and the Switch show it."""
    try:
        for ln in open(os.path.join(REPO, "VERSION"), encoding="utf-8"):
            if ln.strip() and not ln.startswith("#"):
                return ln.strip()
    except OSError:
        pass
    return "dev"



ANDROID_ABIS = {"arm64-v8a": "aarch64-linux-android", "x86_64": "x86_64-linux-android"}


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("FAILED: %s\n  %s\n%s" % (what, " ".join(cmd)[:400], (r.stderr or r.stdout)[-3000:]))
    return r


LENIENT_NOTE = """/* PORT: indirect calls are lenient. The decomp (like most C of its time) calls function pointers whose type differs from the function's own
 * (a callback that returns void called through a pointer that returns s32, a different number of arguments). On the console, and in the
 * Linux and Windows builds, that is harmless: the extra arguments are ignored and an unused result is junk. WebAssembly checks the function type
 * at every indirect call and would trap. wasm2c's check is therefore reduced to "the entry exists" (null and out-of-range still trap); the
 * call itself is the C call through the caller's declared type, which behaves on x86-64 and AArch64 exactly as the MIPS call did. */
#define CHECK_CALL_INDIRECT(table, ft, x) \
  (LIKELY((x) < table.size && table.data[x].func) || func_types_eq_slowpath_missing(&(table), x))
#include <stdio.h>
W2C_COLD_FUNC static bool func_types_eq_slowpath_missing(const wasm_rt_funcref_table_t* table, uint32_t index) {
  fprintf(stderr, "core: call through function-table slot %u failed: %s (table size %u)\\n", index, index < table->size ? "the slot is empty" : "out of range", (unsigned)table->size);
  fflush(stderr);
  TRAP(CALL_INDIRECT);
  return false;
}
"""


YIELD_DECL = ("extern volatile int gYieldReq;\nvoid core_yield_point(void);\n"
              "#define YIELD_POINT() do { if (__builtin_expect(gYieldReq, 0)) core_yield_point(); } while (0)\n")


def add_yield_points(gen, trace):
    """Edits wasm2c's output: (1) a YIELD_POINT at every loop header, so that a thread that spins without calling the OS hands the core's lock to a
    thread that has waited for it (shell/core/core_host.c, "fairness of the lock"); (2) with `trace`, every function leaves its index in a ring
    (gFnRing, core_host.c), so that a stall can say which game code it is stuck in."""
    head = re.compile(r"^[a-z0-9_]+ w2c_kyujo_f(\d+)\(.*\) \{$")
    label = re.compile(r"^\s+var_L\d+: $")
    for path in sorted(glob.glob(os.path.join(gen, "kyujo_*.c"))):
        out, cur = [], None
        for line in open(path).read().split("\n"):
            m = head.match(line)
            if m: cur = m.group(1)
            out.append(line)
            if label.match(line):
                out.append("    YIELD_POINT();")
            if trace and cur is not None and line.strip() == "FUNC_PROLOGUE;":
                out.append("  gFnRing[gFnPos++ & 63u] = %s;" % cur)
                cur = None
        text = "\n".join(out)
        text = text.replace("\n", "\n" + YIELD_DECL + ("extern volatile unsigned int gFnRing[64], gFnPos;\n" if trace else ""), 1)
        open(path, "w").write(text)


def patch_lenient_calls(path):
    """Replaces wasm2c's strict indirect-call check in the generated header by the lenient one (see LENIENT_NOTE). Fails if wasm2c's text changed."""
    import re
    text = open(path).read()
    pat = re.compile(r"#define CHECK_CALL_INDIRECT\(table, ft, x\)[^\n]*\n[^\n]*\n[^\n]*\n")
    if not pat.search(text):
        sys.exit("wasm2c's CHECK_CALL_INDIRECT macro has another form than expected; update patch_lenient_calls in build_shell.py")
    open(path, "w").write(pat.sub(lambda m: LENIENT_NOTE, text, count=1))


# the renderer runs in the shell, not in the core (docs/switch-android-plan.md section 11); rdp_client.c is the core-side stand-in
RENDER_SOURCES = ["rdp.c", "vi_out.c"]
RSP_SOURCES = ["rsp_cpu.c"]                    # src/rsp: the audio microcode interpreter, native side (rsp_client.c is its wasm side)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wasm", required=True)
    ap.add_argument("--wabt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--front", default="headless", choices=["headless", "sdl"])
    ap.add_argument("--target", default="linux", choices=["linux", "switch", "android"])
    ap.add_argument("--devkitpro", default=os.environ.get("DEVKITPRO", "/opt/devkitpro"), help="devkitPro folder (target switch)")
    ap.add_argument("--sdl-include", default="", help="folder that holds SDL.h (targets linux and android, front sdl)")
    ap.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME", ""), help="Android NDK folder (target android)")
    ap.add_argument("--abi", default="arm64-v8a", choices=sorted(ANDROID_ABIS), help="Android ABI (target android)")
    ap.add_argument("--api", type=int, default=24, help="Android API level the library is built for (target android)")
    ap.add_argument("--sdl-lib", default="", help="folder that holds libSDL2.so built for the ABI (target android)")
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--opt", default="O1")
    ap.add_argument("--outputs", type=int, default=16, help="number of C files wasm2c splits the module into (compile in parallel, bounded memory)")
    ap.add_argument("--trace-fn", action="store_true", help="diagnostics: every generated function records its index in a ring (core_fn_ring), for stall reports")
    ap.add_argument("--gpu", action="store_true", help="build the OpenGL ES renderer (shell/core/gpu_gles.c) and the --gpu option of the headless front")
    ap.add_argument("--gl-headers", nargs="*", default=[], help="folder with GLES3/ and EGL/ and KHR/ headers (desktop Linux without a system copy)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    rt = os.path.join(os.path.abspath(a.wabt), "wasm2c")
    w2c = os.path.join(os.path.abspath(a.wabt), "build", "wasm2c")
    gen = os.path.join(out, "w2c")
    os.makedirs(gen, exist_ok=True)
    wasm = os.path.abspath(a.wasm)
    stamp = os.path.join(gen, "kyujo.stamp")
    want_stamp = "yield3" + (":trace-fn" if a.trace_fn else "")
    have_stamp = open(stamp).read() if os.path.exists(stamp) else None
    if have_stamp != want_stamp or os.path.getmtime(stamp) < os.path.getmtime(wasm):
        for old in glob.glob(os.path.join(gen, "*.c")) + glob.glob(os.path.join(gen, "*.o")):
            os.remove(old)
        run([w2c, wasm, "--num-outputs=%d" % a.outputs, "-n", "kyujo", "-o", os.path.join(gen, "kyujo.c")], "wasm2c")
        patch_lenient_calls(os.path.join(gen, "kyujo-impl.h"))
        add_yield_points(gen, a.trace_fn)
        open(stamp, "w").write(want_stamp)
    switch = a.target == "switch"
    android = a.target == "android"
    dkp = os.path.abspath(a.devkitpro)
    if android:
        if not a.ndk or not a.sdl_include or not a.sdl_lib:
            sys.exit("target android needs --ndk, --sdl-include and --sdl-lib")
        if a.front != "sdl":
            sys.exit("target android needs --front sdl")
        ndk_bin = os.path.join(os.path.abspath(a.ndk), "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin")
        triple = ANDROID_ABIS[a.abi]
        a.cc = os.path.join(ndk_bin, "%s%d-clang" % (triple, a.api))
        if not os.path.exists(a.cc):
            sys.exit("the NDK's compiler is not at %s" % a.cc)
        arch = ["-fPIC", "-fvisibility=default", "-D_REENTRANT"]
        quiet = "-Wno-everything"
        sdl_inc = ["-I" + a.sdl_include]
    elif switch:
        os.environ["DEVKITPRO"] = dkp                      # switch.specs and the gcc driver read it
        a.cc = os.path.join(dkp, "devkitA64", "bin", "aarch64-none-elf-gcc")
        arch = ["-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE", "-ftls-model=local-exec", "-D__SWITCH__", "-D_REENTRANT",
                "-isystem", os.path.join(dkp, "libnx", "include"), "-isystem", os.path.join(dkp, "portlibs", "switch", "include"),
                "-I" + os.path.join(REPO, "shell", "switch", "compat"), "-include", os.path.join(REPO, "shell", "switch", "compat", "switch_compat.h")]
        quiet = "-w"
        sdl_inc = ["-isystem", os.path.join(dkp, "portlibs", "switch", "include", "SDL2")]
    else:
        arch = []
        quiet = "-Wno-everything"
        sdl_inc = ["-I" + a.sdl_include] if a.sdl_include else []
    flags = ["-" + a.opt, "-g0", "-DWASM_RT_USE_MMAP=0", "-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1", "-I" + gen, "-I" + rt,
             "-I" + os.path.join(REPO, "shell", "core"), "-I" + os.path.join(REPO, "src", "render"), "-I" + os.path.join(REPO, "src", "platform"),
             "-DRDP_SHELL_NATIVE", quiet] + arch
    if a.front == "sdl":
        flags += sdl_inc
    if a.gpu or a.front == "sdl":
        flags += ["-DRDP_GPU", "-DUSE_GPU"]
        flags += ["-I" + h for h in a.gl_headers]
    if a.front == "sdl" and not a.gpu:
        sys.exit("the sdl front needs --gpu (it only shows what the GPU renderer draws)")
    front_sources = [os.path.join(REPO, "shell", a.front, "main_%s.c" % a.front)]
    if a.gpu and a.front == "headless":
        front_sources.append(os.path.join(REPO, "shell", "headless", "egl_headless.c"))
    if switch:
        front_sources.append(os.path.join(REPO, "shell", "switch", "compat", "switch_compat.c"))
    sources = sorted(glob.glob(os.path.join(gen, "kyujo_*.c"))) + [os.path.join(rt, "wasm-rt-impl.c"), os.path.join(rt, "wasm-rt-mem-impl.c"), os.path.join(rt, "wasm-rt-exceptions-impl.c")] + \
        [f for f in sorted(glob.glob(os.path.join(REPO, "shell", "core", "*.c"))) if a.gpu or os.path.basename(f) != "gpu_gles.c"] + front_sources + \
        [os.path.join(REPO, "src", "render", n) for n in RENDER_SOURCES] + [os.path.join(REPO, "src", "rsp", n) for n in RSP_SOURCES]
    objs = []
    render_abs = set(os.path.join(REPO, "src", "render", n) for n in RENDER_SOURCES) | set(os.path.join(REPO, "src", "rsp", n) for n in RSP_SOURCES)

    # an object is stale when its source or any header of the shell, the renderer, the platform layer, the generated code or the wasm2c run-time is newer (headers are not tracked one by one)
    hdr_time = max(os.path.getmtime(h) for d in (os.path.join(REPO, "shell"), os.path.join(REPO, "src", "render"), os.path.join(REPO, "src", "platform"), gen, rt)
                   for h in glob.glob(os.path.join(d, "**", "*.h"), recursive=True))

    def build(src):
        o = os.path.join(out, os.path.basename(src)[:-2] + ".o")
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(src), hdr_time):
            f = flags if src not in render_abs else [x for x in flags if x != "-" + a.opt] + ["-O2"]     # the renderer is hot code: always -O2 (the generated C is big and compiled at -O1)
            if (switch or android) and src.endswith("main_sdl.c"):
                f = [x for x in f if x != "-w"] + ["-Wall"]
            run([a.cc] + f + ["-c", src, "-o", o], "compile " + os.path.basename(src))
        return o
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        objs = list(pool.map(build, sources))
    exe = os.path.join(out, "core_" + a.front + (".elf" if switch else ""))
    if android:
        lib = os.path.join(out, "libmain.so")
        # 16 KiB page alignment: the devices of Android 15 and later may use that page size; strip: the symbols are not needed on a device
        run([a.cc, "-shared", "-o", lib] + objs + ["-L" + os.path.abspath(a.sdl_lib), "-lSDL2", "-lEGL", "-lGLESv3", "-llog", "-landroid", "-lm",
                                                  "-Wl,-z,max-page-size=16384", "-Wl,--build-id=sha1", "-Wl,--strip-all", "-Wl,--no-undefined"], "link")
        print("built %s (%d bytes)" % (lib, os.path.getsize(lib)))
    elif switch:
        libs = ["-L" + os.path.join(dkp, "portlibs", "switch", "lib"), "-L" + os.path.join(dkp, "libnx", "lib"),
                "-lSDL2", "-lEGL", "-lGLESv2", "-lglapi", "-ldrm_nouveau", "-lnx", "-lm", "-lpthread", "-lstdc++"]
        run([a.cc, "-specs=" + os.path.join(dkp, "libnx", "switch.specs"), "-g", "-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE",
             "-o", exe] + objs + libs, "link")
        nacp = os.path.join(out, "kyujo.nacp")
        nro = os.path.join(out, "kyujo.nro")
        run([os.path.join(dkp, "tools", "bin", "nacptool"), "--create", "Kyujo", "Dan B. Cooper", read_version(), nacp], "nacptool")
        cmd = [os.path.join(dkp, "tools", "bin", "elf2nro"), exe, nro, "--nacp=" + nacp]
        icon = os.path.join(HERE, "..", "..", "assets", "icon.jpg")      # the application icon (256 x 256 baseline JPEG), the same picture as on Android and Windows
        if not os.path.exists(icon):
            sys.exit("build_shell: assets/icon.jpg is missing (the icon of the .nro)")
        cmd.append("--icon=" + icon)
        run(cmd, "elf2nro")
        print("built %s (%d bytes)" % (nro, os.path.getsize(nro)))
    else:
        if a.front == "sdl":
            libs = ["-l:libSDL2-2.0.so.0", "-l:libEGL.so.1", "-l:libGLESv2.so.2"]
        else:
            libs = ["-l:libEGL.so.1", "-l:libGLESv2.so.2"] if a.gpu else []
        run([a.cc, "-o", exe] + objs + ["-lpthread", "-lm"] + libs, "link")
        print("built %s (%d bytes)" % (exe, os.path.getsize(exe)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
