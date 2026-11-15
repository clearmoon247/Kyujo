/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* WebAssembly build: the first object of the link. host_be.c reads the pointers in section `be_fixups` that lie between this marker and be_end.c's
 * (wasm-ld has no __start_/__stop_ symbols; same-named sections are joined in link order). Not part of the normal platform glob (own folder). */
typedef void (*be_fix_fn)(void);
const be_fix_fn gBeFixupsBegin __attribute__((section("be_fixups"), used)) = 0;
