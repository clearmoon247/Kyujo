/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* WebAssembly build: the last object of the link; see be_begin.c. */
typedef void (*be_fix_fn)(void);
const be_fix_fn gBeFixupsEnd __attribute__((section("be_fixups"), used)) = 0;
