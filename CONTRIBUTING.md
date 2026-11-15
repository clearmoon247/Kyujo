<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Dan B. Cooper -->
# Contributing to Kyujo

Thank you for helping. Bug reports, testing on more hardware and patches are all welcome.

## Reporting a problem

Open an [issue](../../issues/new/choose), say which system you use and what happened, and attach the log (`kyujo-log.txt`, or `KYUJO-SDL-LOG.TXT` on Switch and Android). The log contains no game data.

## The rules that keep the project safe

- **Never commit or attach** a game file, anything generated from one (dumps of graphics or audio), a built program (`.exe`, `.nro`, `.apk`, AppImage, the Linux zip) or Game Boy games and saves. `.gitignore` blocks the usual types; please don't work around it. A built program contains the game's code, which isn't ours to give away.
- **Don't copy the decompilation into this repository.** It is fetched from Team Hippodrome's repository (the `ext/decomp` submodule). Changes to it are *port patches* in `port/patches/` that hold only the lines you add; make one with `tools/native/mkpatch.py` and explain the reason at the top. See [`docs/port-patches.md`](docs/port-patches.md).
- **Keep the Windows, Linux, Switch and Android behaviour you didn't touch the same.** If you change shared code, say which systems you tested.

## Building and testing

[`BUILDING.md`](BUILDING.md) builds for each system. Before sending a change:

```
tools/native/check_build.sh ext/decomp <work> build      # Linux and Windows builds and every check that needs no Windows
python3 tests/native/srctree/test_srctree.py
```

## Code and licence

Write comments that explain *why*. New files start with `SPDX-License-Identifier: GPL-3.0-only` and `Copyright (C) 2026 Dan B. Cooper` (`REUSE.toml` covers files that can't carry a header). By contributing you agree that your contribution is licensed GPL-3.0-only, like the rest of the project.
