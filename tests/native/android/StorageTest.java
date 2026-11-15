// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Dan B. Cooper
package org.kyujo.app;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

/** Checks of Storage's name pairing and save placement (plain JVM; run by test_android_sources.py when a JDK and android.jar are there). Prints "OK" or fails with an exception. */
public final class StorageTest {
    static void eq(Object a, Object b, String what) {
        if (!a.equals(b)) throw new AssertionError(what + ": expected <" + b + "> but was <" + a + ">");
    }

    public static void main(String[] args) throws IOException {
        eq(Storage.stem("a.b.gb"), "a.b", "stem");
        eq(Storage.saveName("Pokemon Red") + "|" + Storage.saveName("a.SRM") + "|" + Storage.saveName("a.sav"), "Pokemon Red.sav|a.SRM|a.sav", "save names always end in .sav or .srm");
        eq(Storage.isGbRom("X.GBC") && Storage.isGbSave("x.SRM") && !Storage.isGbRom("x.sav"), true, "kinds");

        File d = Files.createTempDirectory("storagetest").toFile();
        File target = new File(d, "game.sav");
        // no save there yet: the incoming one is moved in
        File in1 = new File(d, "game.sav.incoming");
        Files.write(in1.toPath(), new byte[] {1, 2, 3});
        eq(Storage.placeSave(in1, target), "", "first place");
        eq(target.length(), 3L, "first size");
        eq(in1.exists(), false, "incoming gone");
        // an identical save: nothing is backed up
        File in2 = new File(d, "game.sav.incoming");
        Files.write(in2.toPath(), new byte[] {1, 2, 3});
        eq(Storage.placeSave(in2, target), "", "identical");
        eq(new File(d, "game-backup-1.sav").exists(), false, "no backup for identical");
        // a different save: the old progress is kept
        File in3 = new File(d, "game.sav.incoming");
        Files.write(in3.toPath(), new byte[] {9, 9});
        String note = Storage.placeSave(in3, target);
        eq(note.contains("game-backup-1.sav"), true, "note names the backup");
        eq(Files.readAllBytes(new File(d, "game-backup-1.sav").toPath()).length, 3, "old kept");
        eq(Files.readAllBytes(target.toPath()).length, 2, "new in use");
        // and once more: the next backup number
        File in4 = new File(d, "game.sav.incoming");
        Files.write(in4.toPath(), new byte[] {7});
        Storage.placeSave(in4, target);
        eq(new File(d, "game-backup-2.sav").exists(), true, "second backup");
        for (File f : d.listFiles()) f.delete();
        d.delete();
        System.out.println("OK");
    }
}
