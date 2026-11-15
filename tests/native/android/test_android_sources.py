#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Source-level checks of the Android app that need no SDK, NDK or device (they run anywhere):

* every `native` method of the app's Java classes has a JNI function of the exact name and signature shape in shell/sdl/main_sdl.c (a typo only shows as an
  UnsatisfiedLinkError on the device, when the first touch arrives);
* the button bits of TouchOverlay.java (A, B, START, ... DRIGHT) equal the TB_* bits of main_sdl.c, one by one;
* the manifest names classes that exist, asks for no permission, and the game's activity is its own process;
* every Java file carries the SPDX tag.
Run: python3 tests/native/android/test_android_sources.py"""
import os
import re
import unittest
import xml.etree.ElementTree as ET

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
JAVA = os.path.join(REPO, "android", "java", "org", "kyujo", "app")
MAIN = os.path.join(REPO, "shell", "sdl", "main_sdl.c")
NS = "{http://schemas.android.com/apk/res/android}"


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


class AndroidSources(unittest.TestCase):
    def test_jni_names_match(self):
        c = read(MAIN)
        found = 0
        for fn in sorted(os.listdir(JAVA)):
            if not fn.endswith(".java"):
                continue
            cls = fn[:-5]
            for m in re.finditer(r"\bnative\s+(\w+)\s+(\w+)\s*\(([^)]*)\)", read(os.path.join(JAVA, fn))):
                ret, name, args = m.group(1), m.group(2), [a.strip() for a in m.group(3).split(",") if a.strip()]
                jni = "Java_org_kyujo_app_%s_%s" % (cls, name)
                d = re.search(r"JNIEXPORT\s+(\w+)\s+JNICALL\s+%s\s*\(([^)]*)\)" % jni, c)
                self.assertIsNotNone(d, "%s has no %s in main_sdl.c" % (name, jni))
                want_ret = {"void": "void", "int": "jint", "boolean": "jboolean"}[ret]
                self.assertEqual(d.group(1), want_ret, jni)
                params = [p.strip() for p in d.group(2).split(",")]
                self.assertEqual(params[:2], ["JNIEnv *env", "jclass cls"], jni)           # static native: (env, class, ...)
                kinds = [a.split()[0] for a in args]
                self.assertEqual(len(params) - 2, len(kinds), jni)
                for p, k in zip(params[2:], kinds):
                    self.assertEqual(p.split()[0], {"int": "jint", "boolean": "jboolean", "String": "jstring"}[k], "%s: %s" % (jni, p))
                self.assertIn("static", read(os.path.join(JAVA, fn))[max(0, m.start() - 40):m.start()], "%s must be static" % name)
                found += 1
        self.assertGreaterEqual(found, 2)

    def test_methods_the_game_calls(self):
        """Methods the native side looks up by name and signature string (GetMethodID): a mismatch is a failure only when the player opens the picker."""
        c = read(MAIN)
        looked_up = re.findall(r'GetMethodID\(env, k, "(\w+)", "([^"]+)"\)', c)
        self.assertTrue(looked_up)
        for name, sig in looked_up:
            cls = "GameActivity"                                                  # k is the class of SDL_AndroidGetActivity()
            j = read(os.path.join(JAVA, cls + ".java"))
            m = re.search(r"public\s+void\s+%s\(([^)]*)\)" % name, j)
            self.assertIsNotNone(m, name)
            kinds = [p.split()[-2] for p in m.group(1).split(",")]            # "final int port" -> "int"
            want = "(" + "".join({"int": "I", "String": "Ljava/lang/String;"}[k] for k in kinds) + ")V"
            self.assertEqual(sig, want, name)

    def test_button_bits_match(self):
        c = read(MAIN)
        enum = re.search(r"enum\s*\{\s*TB_A\b.*?\};", c, re.S)
        self.assertIsNotNone(enum)
        c_bits = {n: 1 << int(sh) for n, sh in re.findall(r"TB_(\w+)\s*=\s*1\s*<<\s*(\d+)", enum.group(0))}
        j = read(os.path.join(JAVA, "TouchOverlay.java"))
        block = re.search(r"static final int A = 1,(.*?);", j, re.S)
        self.assertIsNotNone(block)
        j_bits = {"A": 1}
        for n, sh in re.findall(r"(\w+)\s*=\s*1\s*<<\s*(\d+)", block.group(0)):
            j_bits[n] = 1 << int(sh)
        self.assertEqual(sorted(j_bits), sorted(c_bits))
        for n in c_bits:
            self.assertEqual(j_bits[n], c_bits[n], n)

    def test_manifest(self):
        root = ET.parse(os.path.join(REPO, "android", "AndroidManifest.xml")).getroot()
        self.assertEqual(root.findall("uses-permission"), [], "the app asks for no permission (files go through the system's picker)")
        acts = {a.get(NS + "name"): a for a in root.find("application").findall("activity")}
        self.assertEqual(sorted(acts), ["org.kyujo.app.GameActivity", "org.kyujo.app.LauncherActivity"])
        for name in acts:
            self.assertTrue(os.path.exists(os.path.join(JAVA, name.rsplit(".", 1)[1] + ".java")), name)
        self.assertEqual(acts["org.kyujo.app.GameActivity"].get(NS + "process"), ":game")
        self.assertEqual(acts["org.kyujo.app.GameActivity"].get(NS + "exported"), "false")
        self.assertEqual(acts["org.kyujo.app.LauncherActivity"].get(NS + "exported"), "true")
        self.assertEqual(root.find("application").get(NS + "allowBackup"), "false")

    def test_storage_pairing(self):
        """Name pairing and save placement (Storage.java) on a plain JVM; needs a JDK and the SDK's android.jar (ANDROID_SDK or the path of the build)."""
        import glob
        import shutil
        import subprocess
        import tempfile
        sdk = os.environ.get("ANDROID_SDK", "")
        jars = sorted(glob.glob(os.path.join(sdk, "platforms", "android-*", "android.jar"))) if sdk else []
        if not jars or not shutil.which("javac") or not shutil.which("java"):
            self.skipTest("no JDK or android.jar (set ANDROID_SDK)")
        out = tempfile.mkdtemp()
        try:
            srcs = [os.path.join(JAVA, "Storage.java"), os.path.join(REPO, "tests", "native", "android", "StorageTest.java")]
            subprocess.run(["javac", "--release", "8", "-Xlint:-options", "-cp", jars[-1], "-d", out] + srcs, check=True, capture_output=True)
            r = subprocess.run(["java", "-cp", out + os.pathsep + jars[-1], "org.kyujo.app.StorageTest"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("OK", r.stdout)
        finally:
            shutil.rmtree(out, ignore_errors=True)

    def test_tags(self):
        for fn in os.listdir(JAVA):
            self.assertIn("SPDX-License-Identifier: GPL-3.0-only", read(os.path.join(JAVA, fn))[:300], fn)


if __name__ == "__main__":
    unittest.main()
