/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * be_cases.c - GAME-side test program for tools/native/be_memory.py (compiled with the rewrite, no C library).
 *
 * Each check states, from the definition of big-endian byte order (most significant byte at the lowest address), what must be in
 * memory or what a read must return. The expected values are written out by hand below, NOT computed by the rewrite. A second,
 * independent reference is used for initialised data: tools/native/test_be_memory.py compiles be_data.c for 32-bit big-endian MIPS
 * (the console's own CPU) and compares the bytes.
 */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;

/* provided by the host test driver */
extern void be_check(const char *name, int ok);
extern void be_set_redirect(const void *from, const void *to);     /* the host's HostResolveCallTarget maps `from` to `to` */
extern unsigned int be_hook_calls(void);                           /* how many indirect calls the host's hook has seen */

#define CHECK(name, cond) be_check(name, (cond) ? 1 : 0)

/* ---- helpers that keep the compiler from folding the test away ---------------------------------------------------------------- */
static volatile u32 sink32;
static u32 opaque32(u32 v) { sink32 = v; return sink32; }
static u16 opaque16(u16 v) { return (u16)opaque32(v); }
static u64 opaque64(u64 v) { u64 r; u32 hi, lo; sink32 = (u32)(v >> 32); hi = sink32; sink32 = (u32)v; lo = sink32; r = ((u64)hi << 32) | lo; return r; }

static int bytes_are(const void *p, const u8 *want, int n)
{
    const u8 *q = (const u8 *)p;
    int i;
    for (i = 0; i < n; i++) {
        if (q[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---- 1. plain stores: the bytes in memory ---------------------------------------------------------------------------------------- */
static void test_store_bytes(void)
{
    u32 w;
    u16 h;
    s16 sh;
    u64 d;
    float f;
    double g;
    static const u8 w_be[4] = { 0x11, 0x22, 0x33, 0x44 };
    static const u8 h_be[2] = { 0x12, 0x34 };
    static const u8 sh_be[2] = { 0xFF, 0xFE };
    static const u8 d_be[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    static const u8 f_be[4] = { 0x3F, 0xC0, 0x00, 0x00 };            /* 1.5f */
    static const u8 g_be[8] = { 0x3F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };   /* 1.5 */

    w = 0x11223344; CHECK("store constant u32 -> bytes 11 22 33 44", bytes_are(&w, w_be, 4));
    w = opaque32(0x11223344); CHECK("store variable u32 -> bytes 11 22 33 44", bytes_are(&w, w_be, 4));
    h = 0x1234; CHECK("store constant u16 -> bytes 12 34", bytes_are(&h, h_be, 2));
    h = opaque16(0x1234); CHECK("store variable u16 -> bytes 12 34", bytes_are(&h, h_be, 2));
    sh = -2; CHECK("store s16 -2 -> bytes FF FE", bytes_are(&sh, sh_be, 2));
    d = 0x0102030405060708ULL; CHECK("store constant u64 -> bytes 01..08", bytes_are(&d, d_be, 8));
    d = opaque64(0x0102030405060708ULL); CHECK("store variable u64 -> bytes 01..08", bytes_are(&d, d_be, 8));
    f = 1.5f; CHECK("store constant float 1.5 -> 3F C0 00 00", bytes_are(&f, f_be, 4));
    f = (float)opaque32(3) / 2.0f; CHECK("store computed float 1.5 -> 3F C0 00 00", bytes_are(&f, f_be, 4));
    g = 1.5; CHECK("store constant double 1.5 -> 3F F8 00..", bytes_are(&g, g_be, 8));
    g = (double)opaque32(3) / 2.0; CHECK("store computed double 1.5 -> 3F F8 00..", bytes_are(&g, g_be, 8));
}

/* ---- 2. plain loads: reading big-endian bytes through typed pointers ----------------------------------------------------------- */
static void test_load_typed(void)
{
    u8 raw[16];
    u32 *w = (u32 *)raw;
    u16 *h = (u16 *)raw;
    float *f = (float *)(raw + 4);
    double *g = (double *)(raw + 8);
    int i;
    s16 *sh = (s16 *)raw;

    for (i = 0; i < 16; i++) {
        raw[i] = 0;
    }
    raw[0] = 0x00; raw[1] = 0x01; raw[2] = 0x02; raw[3] = 0x03;        /* u32 0x00010203, u16s 0x0001 0x0203 */
    raw[4] = 0x40; raw[5] = 0x49; raw[6] = 0x0F; raw[7] = 0xDB;        /* float 3.14159274 */
    raw[8] = 0x3F; raw[9] = 0xF0;                                       /* double 1.0 */
    CHECK("read u32 from bytes 00 01 02 03", w[0] == 0x00010203);
    CHECK("read u16 at offset 0", h[0] == 0x0001);
    CHECK("read u16 at offset 2", h[1] == 0x0203);
    CHECK("read float from bytes 40 49 0F DB", *f > 3.14159f && *f < 3.14160f);
    CHECK("read double from bytes 3F F0 00..", *g == 1.0);
    raw[0] = 0xFF; raw[1] = 0xFE;
    CHECK("read s16 FF FE = -2 (sign extension)", sh[0] == -2);
}

/* ---- 3. mixed-width access to one object (unions, casts) -------------------------------------------------------------------------- */
typedef union {
    u32 w;
    u16 h[2];
    u8 b[4];
    float f;
} Mix;

static void test_union(void)
{
    Mix m;
    m.w = 0xAABBCCDD;
    CHECK("union u32 -> u8[0] is the most significant byte", m.b[0] == 0xAA && m.b[3] == 0xDD);
    CHECK("union u32 -> u16[0] is the upper half", m.h[0] == 0xAABB && m.h[1] == 0xCCDD);
    m.h[0] = 0x1234;
    CHECK("union write u16[0] changes the upper half of u32", m.w == 0x1234CCDD);
    m.b[3] = 0x99;
    CHECK("union write u8[3] changes the least significant byte", m.w == 0x1234CC99);
    m.f = 1.0f;
    CHECK("union float 1.0 as u32 is 0x3F800000", m.w == 0x3F800000);
}

/* ---- 4. structs, arrays, 64-bit, pointers stored in memory --------------------------------------------------------------------------- */
typedef struct {
    u8 a;
    u16 b;
    u32 c;
    float f;
    double d;
    void *p;
    s16 e[3];
    u64 q;
} Rec;

static Rec g_rec_out;

static void test_struct_and_pointer(void)
{
    Rec r;
    Rec copy;
    u8 *rb = (u8 *)&r;
    u32 pv;
    int i;
    for (i = 0; i < (int)sizeof r; i++) {
        rb[i] = 0;
    }
    r.a = 1; r.b = 0x0203; r.c = 0x04050607; r.f = 1.5f; r.d = 1.5; r.p = &g_rec_out;
    r.e[0] = 0x0A0B; r.e[1] = -1; r.e[2] = 3; r.q = 0x1112131415161718ULL;
    CHECK("struct: u16 field bytes", rb[2] == 0x02 && rb[3] == 0x03);
    CHECK("struct: u32 field bytes", rb[4] == 0x04 && rb[7] == 0x07);
    CHECK("struct: float field bytes", rb[8] == 0x3F && rb[9] == 0xC0);
    CHECK("struct: s16 array element 1 = FF FF", rb[(int)((u8 *)&r.e[1] - rb)] == 0xFF && rb[(int)((u8 *)&r.e[1] - rb) + 1] == 0xFF);
    CHECK("struct: u64 field bytes", ((u8 *)&r.q)[0] == 0x11 && ((u8 *)&r.q)[7] == 0x18);
    pv = (u32)(unsigned long)&g_rec_out;
    CHECK("pointer in memory is stored big-endian",
          ((u8 *)&r.p)[0] == (u8)(pv >> 24) && ((u8 *)&r.p)[1] == (u8)(pv >> 16) && ((u8 *)&r.p)[2] == (u8)(pv >> 8) && ((u8 *)&r.p)[3] == (u8)pv);
    CHECK("pointer read back equals the original", r.p == (void *)&g_rec_out);
    copy = r;                                                          /* aggregate copy moves bytes */
    CHECK("struct copy keeps every field", copy.a == 1 && copy.b == 0x0203 && copy.c == 0x04050607 && copy.f == 1.5f && copy.d == 1.5 &&
          copy.p == (void *)&g_rec_out && copy.e[0] == 0x0A0B && copy.e[1] == -1 && copy.e[2] == 3 && copy.q == 0x1112131415161718ULL);
}

/* ---- 5. initialised data (reversed once at start-up) ------------------------------------------------------------------------------------ */
const u32 k_tab[3] = { 0x01020304, 5, 6 };
const u16 k_half[8] = { 0x0102, 0x0304, 0x0506, 0x0708, 0x090A, 0x0B0C, 0x0D0E, 0x0F10 };    /* more than 4: loop path */
u32 k_grid[3][5] = { { 1, 2, 3, 4, 5 }, { 6, 7, 8, 9, 10 }, { 11, 12, 13, 14, 15 } };
const Rec k_rec = { 1, 0x0203, 0x04050607, 1.5f, 1.5, (void *)&g_rec_out, { 0x0A0B, -1, 3 }, 0x1112131415161718ULL };
const Rec k_recs[6] = { { 1, 2, 3, 4.0f, 5.0, 0, { 6, 7, 8 }, 9 }, { 10, 20, 30, 40.0f, 50.0, 0, { 60, 70, 80 }, 90 } };
const char *const k_names[3] = { "ab", "cd", "ef" };
static int k_func_a(int x) { return x + 1; }
static int k_func_b(int x) { return x * 2; }
int (*const k_funcs[2])(int) = { k_func_a, k_func_b };
typedef union { struct { u32 w0, w1; } w; s64 force_structure_alignment; } Gfx;                  /* shape of libultra's Gfx */
Gfx k_dl[3] = { { { 0xE7000000, 0 } }, { { 0xFC121824, 0xFF33FFFF } }, { { 0xDF000000, 0 } } };
u16 k_big[1000] = { 1, 2, 3 };
float k_fl[4] = { 1.0f, -2.5f, 0.0f, 3.25f };
double k_db[2] = { 1.0, -2.5 };
u8 k_bytes[5] = { 9, 8, 7, 6, 5 };
u32 k_scalar = 0xCAFEBABE;
u32 k_rw = 0xCAFEBABE;                                              /* the one the test writes to, so k_scalar stays pristine for the dump */
u32 k_zero;                                                 /* stays zero, must stay in .bss */

static void test_initialised_data(void)
{
    const u8 *p;
    int i, ok;
    CHECK("static u32 table: logical values", k_tab[0] == 0x01020304 && k_tab[1] == 5 && k_tab[2] == 6);
    p = (const u8 *)k_tab;
    CHECK("static u32 table: bytes 01 02 03 04", p[0] == 1 && p[1] == 2 && p[2] == 3 && p[3] == 4);
    CHECK("static u16 table (loop path)", k_half[0] == 0x0102 && k_half[7] == 0x0F10 && ((const u8 *)k_half)[15] == 0x10);
    CHECK("static 2-D table", k_grid[0][0] == 1 && k_grid[1][2] == 8 && k_grid[2][4] == 15);
    CHECK("static struct: scalar fields", k_rec.a == 1 && k_rec.b == 0x0203 && k_rec.c == 0x04050607 && k_rec.q == 0x1112131415161718ULL);
    CHECK("static struct: float/double", k_rec.f == 1.5f && k_rec.d == 1.5);
    CHECK("static struct: pointer to another global", k_rec.p == (void *)&g_rec_out);
    CHECK("static struct: s16 array", k_rec.e[0] == 0x0A0B && k_rec.e[1] == -1 && k_rec.e[2] == 3);
    CHECK("static struct array: element 1", k_recs[1].c == 30 && k_recs[1].f == 40.0f && k_recs[1].d == 50.0 && k_recs[1].e[2] == 80 && k_recs[1].q == 90);
    CHECK("static string pointer table", k_names[1][0] == 'c' && k_names[2][1] == 'f');
    CHECK("static function pointer table", k_funcs[0](5) == 6 && k_funcs[1](5) == 10);
    CHECK("display-list-shaped union: word 0 bytes E7 00 00 00", ((const u8 *)&k_dl[0])[0] == 0xE7 && ((const u8 *)&k_dl[0])[3] == 0x00);
    CHECK("display-list-shaped union: w0/w1 values", k_dl[1].w.w0 == 0xFC121824 && k_dl[1].w.w1 == 0xFF33FFFF);
    CHECK("display-list-shaped union: 64-bit view is (w0 << 32) | w1", (u64)k_dl[1].force_structure_alignment == 0xFC121824FF33FFFFULL);
    ok = k_big[0] == 1 && k_big[1] == 2 && k_big[2] == 3;
    for (i = 3; i < 1000; i++) {
        ok = ok && k_big[i] == 0;
    }
    CHECK("static 1000-element u16 array (long loop)", ok);
    CHECK("static float array", k_fl[0] == 1.0f && k_fl[1] == -2.5f && k_fl[2] == 0.0f && k_fl[3] == 3.25f);
    CHECK("static double array", k_db[0] == 1.0 && k_db[1] == -2.5);
    CHECK("static byte array untouched", k_bytes[0] == 9 && k_bytes[4] == 5);
    CHECK("static scalar", k_scalar == 0xCAFEBABE && ((const u8 *)&k_scalar)[0] == 0xCA);
    CHECK("zero-initialised global is zero", k_zero == 0);
    CHECK("static scalar before writing", k_rw == 0xCAFEBABE);
    k_rw = 0x01020304;
    CHECK("static scalar can be written", ((const u8 *)&k_rw)[0] == 1 && k_rw == 0x01020304);
}

/* ---- 6. local initialisers (the compiler copies them from a constant) -------------------------------------------------------------- */
static void test_local_init(void)
{
    u32 loc[4] = { 0x01020304, 2, 3, 4 };
    u16 lh[3] = { 0x0A0B, 0x0C0D, 0x0E0F };
    Rec r = { 1, 2, 3, 4.0f, 5.0, 0, { 6, 7, 8 }, 9 };
    CHECK("local u32 array initialiser", loc[0] == 0x01020304 && loc[3] == 4 && ((u8 *)loc)[0] == 1);
    CHECK("local u16 array initialiser", lh[1] == 0x0C0D && ((u8 *)lh)[2] == 0x0C);
    CHECK("local struct initialiser", r.c == 3 && r.f == 4.0f && r.d == 5.0 && r.e[1] == 7 && r.q == 9);
}

/* ---- 7. variable arguments -------------------------------------------------------------------------------------------------------------- */
static int va_sum_ints(int n, ...)
{
    __builtin_va_list ap;
    int t = 0;
    __builtin_va_start(ap, n);
    while (n--) {
        t += __builtin_va_arg(ap, int);
    }
    __builtin_va_end(ap);
    return t;
}

static double va_mixed(const char *fmt, ...)
{
    __builtin_va_list ap;
    double t = 0;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt == 'i') {
            t += __builtin_va_arg(ap, int);
        } else if (*fmt == 'd') {
            t += __builtin_va_arg(ap, double);
        } else if (*fmt == 'p') {
            t += *__builtin_va_arg(ap, int *);
        } else if (*fmt == 'l') {
            t += (double)__builtin_va_arg(ap, s64);
        } else if (*fmt == 's') {
            t += (s16)__builtin_va_arg(ap, int);
        }
    }
    __builtin_va_end(ap);
    return t;
}

static int va_take_two(__builtin_va_list ap)
{
    int a = __builtin_va_arg(ap, int);
    int b = __builtin_va_arg(ap, int);
    return a * 100 + b;
}

static int va_forward(int n, ...)
{
    __builtin_va_list ap;
    int r;
    __builtin_va_start(ap, n);
    r = va_take_two(ap);
    __builtin_va_end(ap);
    return r;
}

static int va_copy_test(int n, ...)
{
    __builtin_va_list ap, ap2;
    int a, b;
    __builtin_va_start(ap, n);
    __builtin_va_copy(ap2, ap);
    a = __builtin_va_arg(ap, int);
    b = __builtin_va_arg(ap2, int);
    __builtin_va_end(ap);
    __builtin_va_end(ap2);
    return a == b ? a : -1;
}

static void test_varargs(void)
{
    int x = 42;
    CHECK("varargs: ints", va_sum_ints(4, 1, 2, 3, 4) == 10);
    CHECK("varargs: negative ints", va_sum_ints(2, -5, 3) == -2);
    CHECK("varargs: mixed int/double/pointer/long long/short", va_mixed("idpls", 1, 2.5, &x, (s64)1000000000000LL, 7) == 1.0 + 2.5 + 42 + 1e12 + 7);
    CHECK("varargs: va_list passed to another function", va_forward(0, 7, 9) == 709);
    CHECK("varargs: va_copy", va_copy_test(0, 33) == 33);
}

/* ---- 8. values that cross calls in registers and in memory ------------------------------------------------------------------------------------ */
typedef struct { u16 a, b; } Pair;
typedef struct { u32 x[6]; } Big;

static Pair swap_pair(Pair p) { Pair r; r.a = p.b; r.b = p.a; return r; }
static Big grow_big(Big b) { int i; for (i = 0; i < 6; i++) { b.x[i] += i; } return b; }
static u64 add64(u64 a, u64 b) { return a + b; }
static double half_of(float f) { return f / 2.0f; }

static void test_calls(void)
{
    Pair p, q;
    Big b, c;
    int i;
    p.a = 1; p.b = 2;
    q = swap_pair(p);
    CHECK("small struct passed and returned by value", q.a == 2 && q.b == 1 && ((u8 *)&q)[1] == 2);
    for (i = 0; i < 6; i++) { b.x[i] = (u32)(i * 10); }
    c = grow_big(b);
    CHECK("large struct passed and returned by value", c.x[0] == 0 && c.x[5] == 55 && ((u8 *)&c.x[5])[3] == 55);
    CHECK("u64 arguments", add64(0x00000001FFFFFFFFULL, 1) == 0x0000000200000000ULL);
    CHECK("float -> double return", half_of(3.0f) == 1.5);
}

/* ---- 9. byte pointers walking big-endian structures (what DMA'd ROM data looks like) ------------------------------------------------------------ */
static void test_rom_like_walk(void)
{
    /* A header as it is in the ROM: a 4-byte magic, a 32-bit size, a 16-bit count, a 16-bit flags field. */
    static const u8 rom[12] = { 'P', 'E', 'R', 'S', 0x00, 0x00, 0x12, 0x34, 0x00, 0x05, 0x80, 0x01 };
    typedef struct { u32 magic0; u32 size; u16 count; u16 flags; } Hdr;
    u8 buf[12];
    Hdr *h = (Hdr *)buf;
    int i;
    for (i = 0; i < 12; i++) {
        buf[i] = rom[i];                                           /* what a DMA does: copies bytes */
    }
    CHECK("header: magic compares with the multi-character constant 'PERS'", h->magic0 == 'PERS');
    CHECK("header: size field", h->size == 0x1234);
    CHECK("header: count and flags", h->count == 5 && h->flags == 0x8001);
}

/* ---- 10. indirect calls go through HostResolveCallTarget -------------------------------------------------------------------- */
static int plus_hundred(int x) { return x + 100; }
static int plus_one(int x) { return x + 1; }
static u8 dead_code_block[8];                       /* stands for an overlay copy in the pool: never executed */

static void test_indirect_call_hook(void)
{
    int (*volatile fp)(int);
    unsigned int before;

    fp = plus_one;
    before = be_hook_calls();
    CHECK("indirect call to a normal function still works", fp(5) == 6);
    CHECK("an indirect call is seen by the hook exactly once", be_hook_calls() == before + 1);

    be_set_redirect(dead_code_block, plus_hundred);
    fp = (int (*)(int))(void *)dead_code_block;
    CHECK("an indirect call to the 'pool copy' runs the function the hook names", fp(5) == 105);

    before = be_hook_calls();
    CHECK("a direct call is not routed through the hook", plus_one(1) == 2 && be_hook_calls() == before);
}

int be_run_tests(void)
{
    test_store_bytes();
    test_load_typed();
    test_union();
    test_struct_and_pointer();
    test_initialised_data();
    test_local_init();
    test_varargs();
    test_calls();
    test_rom_like_walk();
    test_indirect_call_hook();
    return 0;
}

/* ---- 10. the initialised data, offered to the host driver --------------------------------------------------------------------------------------- */
/* tools/native/test_be_memory.py compiles THIS file also for big-endian MIPS and compares these globals byte by byte with what the
 * rewritten i386 program holds after host_swap_game_data(). Pointer slots (k_rec.p, k_names, k_funcs, k_dl is not one) differ by
 * nature and are masked there. */
extern void be_emit(const char *name, const void *data, unsigned int size);

void be_dump(void)
{
    be_emit("k_tab", k_tab, sizeof k_tab);
    be_emit("k_half", k_half, sizeof k_half);
    be_emit("k_grid", k_grid, sizeof k_grid);
    be_emit("k_rec", &k_rec, sizeof k_rec);
    be_emit("k_recs", k_recs, sizeof k_recs);
    be_emit("k_names", k_names, sizeof k_names);
    be_emit("k_funcs", k_funcs, sizeof k_funcs);
    be_emit("k_dl", k_dl, sizeof k_dl);
    be_emit("k_big", k_big, sizeof k_big);
    be_emit("k_fl", k_fl, sizeof k_fl);
    be_emit("k_db", k_db, sizeof k_db);
    be_emit("k_bytes", k_bytes, sizeof k_bytes);
    be_emit("k_scalar", &k_scalar, sizeof k_scalar);
}
