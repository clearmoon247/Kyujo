#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds the Android app (an .apk) of the native port, without Gradle: the Android SDK's own tools (aapt2, d8, zipalign, apksigner), the NDK's clang and SDL2's CMake build.

    build_apk.py --sdk SDK --ndk NDK --sdl SDL2-SOURCE --wasm bw/kyujo.wasm --wabt wabt-checkout --out bw/android [--abis arm64-v8a x86_64]

What goes into the package
    lib/<abi>/libmain.so   the game: tools/native/build_shell.py --target android (wasm2c of the game, the renderer, shell/sdl/main_sdl.c)
    lib/<abi>/libSDL2.so   SDL2 built from source (zlib licence) with the NDK
    classes.dex            SDL2's Java layer (android-project/.../org/libsdl/app) and android/java (the launcher, the game's activity, the on-screen controls, file import)
    res, manifest          the launcher icon (made from assets/icon.jpg) and android/AndroidManifest.xml
The game's wasm for this package must be built with `build_native.py --profile wasm32 --define HOST_SHELL_PICKER` (the Game Boy system picker in the menu; the Windows, Linux and Switch builds are built without it and are unchanged).
The ROM is not in the package: the player chooses it in the app (the launcher copies it into the app's private folder). Nothing of the game's data is in the package either.

Signing: with --keystore the given key; without it a debug key is generated in the output folder (keytool) and used, which is what a package for the player's own device needs. A release for
a store is signed by its owner with their own key: pass --keystore, --ks-pass and --key-alias.
Needs: python3, cmake, ninja, a JDK (javac, keytool), Pillow, and the Android SDK (build-tools, one platform) and NDK. With JDK 21 also pass --d8-jar (see there)."""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

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

ABIS = ("arm64-v8a", "x86_64")


def run(cmd, what, cwd=None, quiet=True):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if r.returncode != 0:
        sys.stderr.write((r.stdout or "")[-3000:] + (r.stderr or "")[-6000:])
        sys.exit("%s failed (%d)" % (what, r.returncode))
    return r.stdout


def newest(paths):
    return max(os.path.getmtime(p) for p in paths)


def sdl_stamp(sdl, ndk, abi, api):
    h = hashlib.sha256()
    h.update(open(os.path.join(sdl, "include", "SDL_version.h"), "rb").read())
    h.update(("%s|%s|%d|%s" % (abi, os.path.basename(os.path.abspath(ndk)), api, "v1")).encode())
    return h.hexdigest()


def build_sdl(a, abi, out):
    """SDL2 as a shared library for one ABI. Returns the folder with libSDL2.so."""
    bd = os.path.join(out, "sdl-" + abi)
    lib = os.path.join(bd, "libSDL2.so")
    stamp = os.path.join(bd, "stamp")
    want = sdl_stamp(a.sdl, a.ndk, abi, a.api)
    if os.path.exists(lib) and os.path.exists(stamp) and open(stamp).read() == want:
        return bd
    os.makedirs(bd, exist_ok=True)
    run(["cmake", "-S", a.sdl, "-B", bd, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(a.ndk, "build", "cmake", "android.toolchain.cmake"), "-DANDROID_ABI=" + abi, "-DANDROID_PLATFORM=android-%d" % a.api,
         "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST=OFF", "-DSDL_TESTS=OFF", "-DSDL_INSTALL=OFF", "-DSDL_WERROR=OFF",
         "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 -Wl,--strip-all"], "cmake configure SDL2 (%s)" % abi)
    run(["cmake", "--build", bd, "--parallel", str(os.cpu_count() or 2)], "build SDL2 (%s)" % abi)
    if not os.path.exists(lib):
        sys.exit("SDL2 did not produce %s" % lib)
    open(stamp, "w").write(want)
    return bd


def build_game(a, abi, out, sdl_bd):
    d = os.path.join(out, "shell-" + abi)
    run([sys.executable, os.path.join(REPO, "tools", "native", "build_shell.py"), "--wasm", a.wasm, "--wabt", a.wabt, "--out", d, "--front", "sdl", "--gpu", "--target", "android",
         "--ndk", a.ndk, "--abi", abi, "--api", str(a.api), "--sdl-include", os.path.join(a.sdl, "include"), "--sdl-lib", sdl_bd, "--opt", a.opt, "--jobs", str(a.jobs)],
        "build libmain.so (%s)" % abi)
    return os.path.join(d, "libmain.so")


def make_icons(res):
    from PIL import Image
    src = Image.open(os.path.join(REPO, "assets", "icon.jpg")).convert("RGB")
    for dpi, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(res, "mipmap-" + dpi)
        os.makedirs(d, exist_ok=True)
        src.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"), optimize=True)


def build_dex(a, out):
    cls = os.path.join(out, "classes")
    shutil.rmtree(cls, ignore_errors=True)
    os.makedirs(cls)
    java = glob.glob(os.path.join(a.sdl, "android-project", "app", "src", "main", "java", "org", "libsdl", "app", "*.java")) + \
        glob.glob(os.path.join(REPO, "android", "java", "org", "kyujo", "app", "*.java"))
    android_jar = os.path.join(a.sdk, "platforms", a.platform, "android.jar")
    run(["javac", "--release", "8", "-Xlint:-options", "-encoding", "UTF-8", "-cp", android_jar, "-d", cls] + java, "javac")
    classes = glob.glob(os.path.join(cls, "**", "*.class"), recursive=True)
    dex = os.path.join(out, "dex")
    shutil.rmtree(dex, ignore_errors=True)
    os.makedirs(dex)
    if a.d8_jar:
        d8 = ["java", "-cp", a.d8_jar, "com.android.tools.r8.D8"]
    else:
        d8 = [os.path.join(a.sdk, "build-tools", a.build_tools, "d8")]
    run(d8 + ["--release", "--min-api", str(a.api), "--lib", android_jar, "--output", dex] + classes, "d8")
    return os.path.join(dex, "classes.dex")


def ensure_key(a, out):
    if a.keystore:
        return a.keystore, a.ks_pass, a.key_alias
    ks = os.path.join(out, "debug.keystore")
    if not os.path.exists(ks):
        run(["keytool", "-genkeypair", "-keystore", ks, "-storepass", "android", "-keypass", "android", "-alias", "kyujo-debug", "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10000", "-dname", "CN=kyujo debug key,O=Dan B. Cooper"], "keytool")
    return ks, "android", "kyujo-debug"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk", required=True, help="Android SDK folder")
    ap.add_argument("--ndk", required=True, help="Android NDK folder")
    ap.add_argument("--sdl", required=True, help="SDL2 source folder (2.32.x)")
    ap.add_argument("--wasm", required=True)
    ap.add_argument("--wabt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--abis", nargs="+", default=["arm64-v8a"], choices=ABIS)
    ap.add_argument("--api", type=int, default=24, help="minimum Android version (24 = Android 7.0)")
    ap.add_argument("--target-api", type=int, default=34)
    ap.add_argument("--platform", default="android-34")
    ap.add_argument("--build-tools", default="34.0.0")
    ap.add_argument("--opt", default="O1")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--version-name", default=read_version())
    ap.add_argument("--version-code", type=int, default=1)
    ap.add_argument("--keystore", default="")
    ap.add_argument("--ks-pass", default="")
    ap.add_argument("--key-alias", default="")
    ap.add_argument("--name", default="kyujo-android.apk")
    ap.add_argument("--d8-jar", default="", help="R8 jar (com.android.tools.r8.D8) to use instead of the SDK's d8: the d8 of build-tools 34 (8.2.2) fails on the classes of JDK 21's javac "
                    "(anonymous classes carry a MethodParameters entry without a name); R8 8.13.19 from Google's Maven repository reads them")
    a = ap.parse_args()
    for k in ("sdk", "ndk", "sdl", "wasm", "wabt", "out"):
        setattr(a, k, os.path.abspath(getattr(a, k)))
    out = a.out
    os.makedirs(out, exist_ok=True)
    bt = os.path.join(a.sdk, "build-tools", a.build_tools)
    android_jar = os.path.join(a.sdk, "platforms", a.platform, "android.jar")
    for p in (bt, android_jar, os.path.join(a.ndk, "build", "cmake", "android.toolchain.cmake"), os.path.join(a.sdl, "include", "SDL_version.h")):
        if not os.path.exists(p):
            sys.exit("missing: %s" % p)

    libs = {}
    for abi in a.abis:
        sd = build_sdl(a, abi, out)
        libs[abi] = (os.path.join(sd, "libSDL2.so"), build_game(a, abi, out, sd))

    dex = build_dex(a, out)

    res = os.path.join(out, "res")
    shutil.rmtree(res, ignore_errors=True)
    make_icons(res)
    flat = os.path.join(out, "res.flata")
    run([os.path.join(bt, "aapt2"), "compile", "--dir", res, "-o", flat], "aapt2 compile")
    base = os.path.join(out, "base.apk")
    run([os.path.join(bt, "aapt2"), "link", "-o", base, "-I", android_jar, "--manifest", os.path.join(REPO, "android", "AndroidManifest.xml"), "--min-sdk-version", str(a.api),
         "--target-sdk-version", str(a.target_api), "--version-code", str(a.version_code), "--version-name", a.version_name, "-R", flat, "--auto-add-overlay"], "aapt2 link")

    unsigned = os.path.join(out, "unsigned.apk")
    with zipfile.ZipFile(base) as zin, zipfile.ZipFile(unsigned, "w", zipfile.ZIP_DEFLATED) as z:
        for info in zin.infolist():
            z.writestr(info, zin.read(info.filename), compress_type=zipfile.ZIP_DEFLATED if info.filename != "resources.arsc" else zipfile.ZIP_STORED)
        z.write(dex, "classes.dex")
        for abi, (sdl_so, main_so) in sorted(libs.items()):
            z.write(sdl_so, "lib/%s/libSDL2.so" % abi)
            z.write(main_so, "lib/%s/libmain.so" % abi)

    aligned = os.path.join(out, "aligned.apk")
    if os.path.exists(aligned):
        os.remove(aligned)
    run([os.path.join(bt, "zipalign"), "-f", "-p", "4", unsigned, aligned], "zipalign")
    ks, kp, alias = ensure_key(a, out)
    apk = os.path.join(out, a.name)
    run([os.path.join(bt, "apksigner"), "sign", "--ks", ks, "--ks-pass", "pass:" + kp, "--key-pass", "pass:" + kp, "--ks-key-alias", alias, "--out", apk, aligned], "apksigner sign")
    print(run([os.path.join(bt, "apksigner"), "verify", "--verbose", "--print-certs", apk], "apksigner verify"))
    print(run([os.path.join(bt, "aapt2"), "dump", "badging", apk], "aapt2 dump badging")[:1500])
    print("built %s (%d bytes)" % (apk, os.path.getsize(apk)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
