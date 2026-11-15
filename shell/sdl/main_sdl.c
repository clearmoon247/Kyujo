/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * main_sdl.c - the shell for systems that have SDL2: Nintendo Switch (devkitPro's SDL2 port), Android (SDL's own Android port, later) and desktop Linux
 * (for development: the same code runs here, with the offscreen or the dummy video driver on a machine without a display).
 *
 * What the shell does, and what it leaves to others (docs/switch-android-plan.md section 9: nothing is written here that a library already does):
 *   window, OpenGL ES 3 context, game controllers, keyboard, audio device ........ SDL2
 *   the game itself .............................................................. the sandboxed core (shell/core/core_host.c)
 *   the RDP on the GPU, and the picture on the screen .............................. shell/core/gpu_gles.c (OpenGL ES 3)
 *
 * THREADS. The main thread owns the window: it pumps SDL's events and publishes the input. The game (core_run) runs on a thread of its own with a big
 * stack. The OpenGL context belongs to the back end's render thread (gpu_gles.c) for good: the main thread creates it and lets go of it before the
 * render thread takes it. SDL's audio callback runs on SDL's thread and reads a ring buffer that the game's audio call fills.
 *
 *   core_sdl [--root DIR] [--home PATH] [--window WxH] [game arguments, e.g. --rom FILE]
 * On the Switch the folder is sdmc:/switch/kyujo (ROM, settings, saves, log); without --rom the first ROM file in it (extension z64, n64 or v64) is used.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <malloc.h>
#endif
#include <dirent.h>

#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "../core/core_host.h"
#include "../core/gpu_gles.h"

extern char **environ;

#ifdef __SWITCH__
/* libnx calls this on a CPU exception of any thread (a crash of the program): it writes where the program was into the log, so that the crash can be found
 * afterwards. Addresses are relative to the start of the program (__start__), which the system places differently each time; with core_sdl.elf, `aarch64-none-elf-addr2line -f -e`
 * turns them back into functions. Then it returns, and the system ends the program as it would anyway. */
extern char __start__[];

/* libnx: how the program ends the application's own process. 1 = through the applet service's own exit (the way hbmenu ends itself), which is what makes the system
 * treat it as a normal close. A bare svcExitProcess (tests 33 and 34) is shown as "The software was closed because an error occurred". The default (0) returns to
 * hbloader, which loads hbmenu again in the same process. */
u32 __nx_applet_exit_mode = 0;

/* The address space as the system sees it (every mapped block: where, what kind, permissions, attributes), written to the log at start, before leaving and when the
 * program crashes: a start right after another program in the same loader process (hbloader keeps one process for hbmenu and every .nro) crashed on a page of the C
 * heap that was not mapped (tests 32 and 33), and this is what shows which pages and since when. */
static void mem_map_log(const char *tag)
{
    u64 a = 0;
    unsigned n = 0;
    fprintf(stderr, "sdl: memory map (%s):\n", tag);
    while (n < 400) {
        MemoryInfo mi;
        u32 pi;
        u64 next;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
        next = mi.addr + mi.size;
        if (mi.type != MemType_Unmapped) {
            fprintf(stderr, "sdl:   %09llX-%09llX type %02X perm %u attr %X ipc %u device %u\n", (unsigned long long)mi.addr, (unsigned long long)next, (unsigned)mi.type, (unsigned)mi.perm,
                    (unsigned)mi.attr, (unsigned)mi.ipc_refcount, (unsigned)mi.device_refcount);
            n++;
        }
        if (next <= a) break;
        a = next;
    }
    fprintf(stderr, "sdl: memory map (%s): %u blocks\n", tag, n);
}

/* The C heap. libnx's own start-up code gives malloc the whole heap the loader handed over; this looks at the pages first and gives malloc the largest stretch of ordinary
 * pages (heap type, read/write, not locked). hbloader runs every program in the same process and leaves the heap as the previous program left it, and the graphics
 * driver (Mesa/nouveau on the nv service) leaves about fourteen 2 MiB buffers locked after the program has closed it (tests 33 to 35: "perm none, locked", a data abort
 * "translation fault" the moment the next program's malloc runs into one). The first program in a process finds one stretch (the whole heap, as before); later ones start
 * after the locked blocks. The result is kept for the log (nothing can be printed this early). */
static char *sHeapBase, *sHeapStart;
static unsigned long long sHeapSize, sHeapGood, sHeapBadBytes, sHeapFirstBad;
static int sHeapDamaged;
static unsigned sHeapBadType, sHeapBadPerm, sHeapBadAttr, sHeapBlocks, sHeapBadBlocks;

#define HEAP_ENOUGH (768ull << 20)                              /* the least a run needs (a start uses about 320 MiB, the game's own needs come on top) */

/* Walks [base, base + size) block by block and finds the longest stretch of ordinary heap pages. */
static void heap_runs(unsigned long long base, unsigned long long size, unsigned long long *best_at, unsigned long long *best_len, unsigned *blocks, unsigned *bad_blocks,
                      unsigned long long *bad_bytes, unsigned long long *first_bad, unsigned *bad_type, unsigned *bad_perm, unsigned *bad_attr)
{
    MemoryInfo mi;
    u32 pi;
    u64 a = base, end = base + size, run_at = 0, run_len = 0;
    *best_at = 0; *best_len = 0; *blocks = 0; *bad_blocks = 0; *bad_bytes = 0; *first_bad = 0;
    while (a < end && *blocks < 4096) {
        u64 next, len;
        int good;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
        next = mi.addr + mi.size;
        if (next > end) next = end;
        len = next - a;
        good = mi.type == MemType_Heap && mi.perm == Perm_Rw && !(mi.attr & 1);
        (*blocks)++;
        if (good) {
            if (!run_len) run_at = a;
            run_len += len;
            if (run_len > *best_len) { *best_len = run_len; *best_at = run_at; }
        } else {
            run_len = 0;
            (*bad_blocks)++;
            *bad_bytes += len;
            if (!*first_bad) { *first_bad = a; *bad_type = mi.type; *bad_perm = mi.perm; *bad_attr = mi.attr; }
        }
        if (next <= a) break;
        a = next;
    }
}

void __libnx_initheap(void);
void __libnx_initheap(void)
{
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    void *addr = NULL;
    u64 size = 0;
    unsigned long long at, len;
    if (envHasHeapOverride()) {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        u64 avail = 0, used = 0;
        svcGetInfo(&avail, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        if (avail > used + 0x200000) size = (avail - used - 0x200000) & ~0x1FFFFFull;
        if (size == 0) size = 0x2000000ull * 16;
        svcSetHeapSize(&addr, size);
    }
    sHeapBase = (char *)addr;
    sHeapSize = size;
    heap_runs((unsigned long long)(uintptr_t)addr, size, &at, &len, &sHeapBlocks, &sHeapBadBlocks, &sHeapBadBytes, &sHeapFirstBad, &sHeapBadType, &sHeapBadPerm, &sHeapBadAttr);
    if (!len) { at = (unsigned long long)(uintptr_t)addr; len = size; }          /* nothing recognisable: do what libnx would */
    sHeapStart = (char *)(uintptr_t)at;
    sHeapGood = len;
    fake_heap_start = sHeapStart;
    fake_heap_end = sHeapStart + len;
}

void __libnx_exception_handler(ThreadExceptionDump *c);
void __libnx_exception_handler(ThreadExceptionDump *c)
{
    unsigned long long base = (unsigned long long)(uintptr_t)__start__, fp = c->fp.x, lo = c->sp.x, i;
    fprintf(stderr, "sdl: CRASH: exception %u (esr %08X) pc +0x%llX lr +0x%llX sp %llX fault address %llX; program starts at %llX\n", (unsigned)c->error_desc, (unsigned)c->esr,
            c->pc.x - base, c->lr.x - base, c->sp.x, c->far.x, base);
    fprintf(stderr, "sdl: CRASH: x0 %llX x1 %llX x2 %llX x3 %llX x19 %llX x20 %llX x21 %llX\n", c->cpu_gprs[0].x, c->cpu_gprs[1].x, c->cpu_gprs[2].x, c->cpu_gprs[3].x, c->cpu_gprs[19].x, c->cpu_gprs[20].x, c->cpu_gprs[21].x);
    for (i = 0; i < 24 && fp > lo && fp < lo + (8u << 20) && !(fp & 7); i++) {      /* the frame-pointer chain: {previous fp, return address} */
        const unsigned long long *fr = (const unsigned long long *)(uintptr_t)fp;
        fprintf(stderr, "sdl: CRASH: caller +0x%llX\n", fr[1] - base);
        if (fr[0] <= fp) break;
        fp = fr[0];
    }
    {   /* what the system says about the page that was touched, and where the C heap ends: a data abort with "translation fault" (esr ...47) is a page that is not mapped */
        MemoryInfo mi;
        u32 pi;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, c->far.x)))
            fprintf(stderr, "sdl: CRASH: the page at the fault address: %llX-%llX type %02X perm %u attr %X ipc %u device %u\n", (unsigned long long)mi.addr, (unsigned long long)(mi.addr + mi.size),
                    (unsigned)mi.type, (unsigned)mi.perm, (unsigned)mi.attr, (unsigned)mi.ipc_refcount, (unsigned)mi.device_refcount);
        mem_map_log("crash");
    }
    fflush(stderr);
}
#endif

/* ---- the window and the OpenGL context -------------------------------------------------------------------------------------------- */
static SDL_Window *sWin;
static SDL_GLContext sCtx;
static int sFullscreen;
static atomic_int sFullscreenReq = -1;

/* Android: the app can be sent to the background at any time. The system then destroys the window's surface (SDL frees its EGL surface) and makes a new one when
 * the app comes back; the OpenGL context itself survives. The game is held still while it is in the background (pause_gate, called where the game hands over a
 * picture or sound), the render thread does not swap into a surface that is gone, and after the return it lets go of the context and takes it again, which binds it
 * to the new surface (SDL_GL_MakeCurrent does nothing when the context is already current, so the release comes first). */
static atomic_int sPaused;                                      /* the app is in the background */
static atomic_uint sSurfaceEpoch;                               /* counts the returns from the background */
static unsigned sBoundEpoch;                                    /* render thread only */
static void pause_gate(void)
{
    if (!atomic_load(&sPaused)) return;
    while (atomic_load(&sPaused)) SDL_Delay(20);
}

static int gl_make_current(void *user) { (void)user; return SDL_GL_MakeCurrent(sWin, sCtx) == 0 ? 0 : -1; }
static void gl_release(void *user) { (void)user; SDL_GL_MakeCurrent(sWin, NULL); }
static void gl_swap(void *user)
{
    unsigned e = atomic_load(&sSurfaceEpoch);
    (void)user;
    if (atomic_load(&sPaused)) return;                           /* no surface to show it on */
    if (e != sBoundEpoch) {
        SDL_GL_MakeCurrent(sWin, NULL);
        if (SDL_GL_MakeCurrent(sWin, sCtx) != 0) fprintf(stderr, "sdl: the OpenGL context could not be bound to the new surface: %s\n", SDL_GetError());
        else fprintf(stderr, "sdl: the OpenGL context is bound to the new surface (return %u from the background)\n", e);
        sBoundEpoch = e;
    }
    SDL_GL_SwapWindow(sWin);
}
static void gl_size(void *user, int *w, int *h) { (void)user; SDL_GL_GetDrawableSize(sWin, w, h); }

/* ---- audio: a ring buffer between the game's audio call and SDL's callback ---------------------------------------------------------- */
#define RING_SIZE (1u << 18)
#define RING_KEEP_MAX 38400u                    /* bytes (200 ms of 48 kHz 16-bit stereo): the most the ring may hold, so that the sound cannot lag far behind */
static uint8_t sRing[RING_SIZE];
static atomic_uint sRingRd, sRingWr;
static SDL_AudioDeviceID sAudio;
static atomic_uint sAudioDropped, sAudioUnderruns;            /* for the log: bytes the game wrote that did not fit / callbacks that found the ring empty */

#define RING_PRIME 8192u                        /* bytes (about 43 ms) the ring must hold before playing starts, and again after it ran dry */
static int sPrimed;                             /* audio callback only */

static void audio_callback(void *user, Uint8 *out, int len)
{
    unsigned rd = atomic_load_explicit(&sRingRd, memory_order_relaxed), wr = atomic_load_explicit(&sRingWr, memory_order_acquire);
    unsigned avail = wr - rd, n, i;
    (void)user;
    if (!sPrimed && avail >= RING_PRIME) sPrimed = 1;
    if (!sPrimed) { memset(out, 0, (size_t)len); return; }                       /* still filling: silence */
    n = avail < (unsigned)len ? avail : (unsigned)len;
    for (i = 0; i < n; i++) out[i] = sRing[(rd + i) & (RING_SIZE - 1)];
    if (n < (unsigned)len) {                                                     /* ran dry: the rest is silence, and playing waits until the ring has filled again */
        memset(out + n, 0, (size_t)len - n);
        atomic_fetch_add(&sAudioUnderruns, 1);
        sPrimed = 0;
    }
    atomic_store_explicit(&sRingRd, rd + n, memory_order_release);
}

static void on_audio(const void *pcm, unsigned bytes, void *user)
{
    const uint8_t *p = (const uint8_t *)pcm;
    pause_gate();
    unsigned rd = atomic_load_explicit(&sRingRd, memory_order_acquire), wr = atomic_load_explicit(&sRingWr, memory_order_relaxed), i;
    unsigned fill = wr - rd;
    (void)user;
    if (!sAudio) return;
    if (fill + bytes > RING_KEEP_MAX) {                                         /* too much queued: drop the new samples (never block the game) */
        atomic_fetch_add(&sAudioDropped, fill + bytes > RING_KEEP_MAX ? fill + bytes - RING_KEEP_MAX : 0);
        if (fill >= RING_KEEP_MAX) return;
        bytes = RING_KEEP_MAX - fill;
    }
    for (i = 0; i < bytes; i++) sRing[(wr + i) & (RING_SIZE - 1)] = p[i];
    atomic_store_explicit(&sRingWr, wr + bytes, memory_order_release);
}

static void audio_start(void)
{
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    sAudio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!sAudio) { fprintf(stderr, "sdl: no audio device (%s): the game runs without sound\n", SDL_GetError()); return; }
    if (have.freq != 48000 || have.format != AUDIO_S16LSB || have.channels != 2)
        fprintf(stderr, "sdl: audio device is %d Hz format 0x%X %d channels (asked 48000 Hz 16-bit stereo)\n", have.freq, (unsigned)have.format, (int)have.channels);
    SDL_PauseAudioDevice(sAudio, 0);
}

/* ---- Android: the on-screen controls (android/java/org/kyujo/app/TouchOverlay.java) ----------------------------------------------------------
 * The Java view draws the controls and follows the fingers; it reports the state here: a bit mask of buttons and the left stick (-32767..32767, up positive).
 * read_input turns it into one more Xbox-layout pad called "Touch controls", so the game's own button assignment (the controller menu) applies to it, as to a
 * physical pad. The view reports "off" while a physical gamepad is connected. */
#ifdef __ANDROID__
enum { TB_A = 1 << 0, TB_B = 1 << 1, TB_START = 1 << 2, TB_L = 1 << 3, TB_R = 1 << 4, TB_Z = 1 << 5, TB_MENU = 1 << 6, TB_CUP = 1 << 7, TB_CDOWN = 1 << 8, TB_CLEFT = 1 << 9,
       TB_CRIGHT = 1 << 10, TB_DUP = 1 << 11, TB_DDOWN = 1 << 12, TB_DLEFT = 1 << 13, TB_DRIGHT = 1 << 14 };
static atomic_uint sTouchMask;
static atomic_int sTouchLX, sTouchLY, sTouchOn;
JNIEXPORT void JNICALL Java_org_kyujo_app_TouchOverlay_nativeTouch(JNIEnv *env, jclass cls, jint mask, jint lx, jint ly)
{
    (void)env; (void)cls;
    atomic_store(&sTouchLX, lx);
    atomic_store(&sTouchLY, ly);
    atomic_store(&sTouchMask, (unsigned)mask);
}
JNIEXPORT void JNICALL Java_org_kyujo_app_TouchOverlay_nativeActive(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env; (void)cls;
    atomic_store(&sTouchOn, on ? 1 : 0);
    if (!on) atomic_store(&sTouchMask, 0u);
}
#endif /* __ANDROID__ (the touch controls) */

/* ---- the system's file picker for Game Boy files: Android (GameActivity.pickGb), and a fake one for tests on the other systems -----------------------------
 * The game's Game Pak menu asks for it (core front pick_gb): on Android the activity opens the system's picker, copies the chosen ROM (and save) into the app's folder and
 * answers with nativeGbPicked; the menu loop polls (pick_gb_poll). One request at a time. Elsewhere the picker exists only when KYUJO_FAKE_GB_PICK="<game path>|<line>"
 * is set (a test of the menu's side of it: the answer comes at once; KYUJO_FAKE_GB_PICK_SAVE answers the save item). */
#if defined(__ANDROID__) || !defined(__SWITCH__)
static pthread_mutex_t sPickLock = PTHREAD_MUTEX_INITIALIZER;
static int sPickDone, sPickPort, sPickKind;
static char sPickPath[1100], sPickMsg[400];

#ifdef __ANDROID__
JNIEXPORT void JNICALL Java_org_kyujo_app_GameActivity_nativeGbPicked(JNIEnv *env, jclass cls, jint port, jint kind, jstring path, jstring msg, jstring detail)
{
    const char *p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    const char *m = msg ? (*env)->GetStringUTFChars(env, msg, NULL) : NULL;
    const char *d = detail ? (*env)->GetStringUTFChars(env, detail, NULL) : NULL;
    (void)cls;
    fprintf(stderr, "sdl: Game Boy file picker (port %d, kind %d): path '%s', %s\n", (int)port + 1, (int)kind, p ? p : "", d ? d : (m ? m : ""));
    pthread_mutex_lock(&sPickLock);
    sPickPort = (int)port;
    sPickKind = (int)kind;
    snprintf(sPickPath, sizeof sPickPath, "%s", p ? p : "");
    snprintf(sPickMsg, sizeof sPickMsg, "%s", m ? m : "");
    sPickDone = 1;
    pthread_mutex_unlock(&sPickLock);
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    if (m) (*env)->ReleaseStringUTFChars(env, msg, m);
    if (d) (*env)->ReleaseStringUTFChars(env, detail, d);
}

static int on_pick_gb(int port, int kind, const char *rom, void *user)
{
    JNIEnv *env;
    jobject act;
    jclass k;
    jmethodID mid;
    jstring jrom = NULL;
    (void)user;
    if (port < 0) return 1;                                      /* only the question: is there a picker */
    env = (JNIEnv *)SDL_AndroidGetJNIEnv();                      /* attaches the calling thread (the game's) to the VM */
    act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act) return 0;
    k = (*env)->GetObjectClass(env, act);
    mid = k ? (*env)->GetMethodID(env, k, "pickGb", "(IILjava/lang/String;)V") : NULL;
    if (!mid) {
        (*env)->ExceptionClear(env);
        fprintf(stderr, "sdl: GameActivity.pickGb was not found\n");
        (*env)->DeleteLocalRef(env, act);
        return 0;
    }
    pthread_mutex_lock(&sPickLock);
    sPickDone = 0;
    pthread_mutex_unlock(&sPickLock);
    if (rom) jrom = (*env)->NewStringUTF(env, rom);
    (*env)->CallVoidMethod(env, act, mid, (jint)port, (jint)kind, jrom);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fprintf(stderr, "sdl: GameActivity.pickGb threw\n"); }
    if (jrom) (*env)->DeleteLocalRef(env, jrom);
    (*env)->DeleteLocalRef(env, k);
    (*env)->DeleteLocalRef(env, act);
    return 1;
}

#elif !defined(__SWITCH__)
static int on_pick_gb(int port, int kind, const char *rom, void *user)
{
    const char *env = getenv(kind == 1 && getenv("KYUJO_FAKE_GB_PICK_SAVE") ? "KYUJO_FAKE_GB_PICK_SAVE" : "KYUJO_FAKE_GB_PICK"), *bar;
    (void)user; (void)rom;
    if (!env) return 0;
    if (port < 0) return 1;
    bar = strchr(env, '|');
    pthread_mutex_lock(&sPickLock);
    sPickPort = port;
    sPickKind = kind;
    snprintf(sPickPath, sizeof sPickPath, "%.*s", bar ? (int)(bar - env) : (int)strlen(env), env);
    snprintf(sPickMsg, sizeof sPickMsg, "%s", bar ? bar + 1 : "");
    sPickDone = 1;
    pthread_mutex_unlock(&sPickLock);
    return 1;
}
#endif

static int on_pick_gb_poll(int *port, int *kind, char *path, unsigned path_size, char *msg, unsigned msg_size, void *user)
{
    int done;
    (void)user;
    pthread_mutex_lock(&sPickLock);
    done = sPickDone;
    if (done) {
        *port = sPickPort;
        *kind = sPickKind;
        snprintf(path, path_size, "%s", sPickPath);
        snprintf(msg, msg_size, "%s", sPickMsg);
        sPickDone = 0;
    }
    pthread_mutex_unlock(&sPickLock);
    return done;
}
#endif /* the file picker: not on the Switch */

#ifdef __ANDROID__
/* Fills `p` (a free slot) from the touch state. */
static void touch_pad(HostRawPad *p)
{
    unsigned m = atomic_load(&sTouchMask);
    memset(p, 0, sizeof *p);
    p->connected = 1;
    p->generic = 0;
    snprintf(p->name, sizeof p->name, "Touch controls");
    p->down[PB_A] = (m & TB_A) != 0; p->down[PB_B] = (m & TB_B) != 0; p->down[PB_START] = (m & TB_START) != 0;
    p->down[PB_LB] = (m & TB_L) != 0; p->down[PB_RB] = (m & TB_R) != 0; p->down[PB_BACK] = (m & TB_MENU) != 0;
    p->down[PB_DUP] = (m & TB_DUP) != 0; p->down[PB_DDOWN] = (m & TB_DDOWN) != 0; p->down[PB_DLEFT] = (m & TB_DLEFT) != 0; p->down[PB_DRIGHT] = (m & TB_DRIGHT) != 0;
    p->lt = (m & TB_Z) ? 255 : 0;
    p->lx = (short)atomic_load(&sTouchLX);
    p->ly = (short)atomic_load(&sTouchLY);
    p->rx = (short)((m & TB_CRIGHT ? 32767 : 0) - (m & TB_CLEFT ? 32767 : 0));       /* the C buttons are the right stick (the game's default assignment) */
    p->ry = (short)((m & TB_CUP ? 32767 : 0) - (m & TB_CDOWN ? 32767 : 0));
}
#endif

/* ---- input: SDL's keyboard and game controllers into the game's raw input snapshot --------------------------------------------------- */
static SDL_GameController *sPad[4];
static SDL_JoystickID sPadId[4];

static int key_from_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (int)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (int)(sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return HK_F1 + (int)(sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return HK_NUM1 + (int)(sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_KP_0: return HK_NUM0;
    case SDL_SCANCODE_RETURN: return HK_ENTER;
    case SDL_SCANCODE_ESCAPE: return HK_ESC;
    case SDL_SCANCODE_AC_BACK: return HK_ESC;                 /* Android's Back button opens the game's menu (SDL passes it on instead of closing the activity: see the hints in main) */
    case SDL_SCANCODE_BACKSPACE: return HK_BACKSPACE;
    case SDL_SCANCODE_TAB: return HK_TAB;
    case SDL_SCANCODE_SPACE: return HK_SPACE;
    case SDL_SCANCODE_UP: return HK_UP;
    case SDL_SCANCODE_DOWN: return HK_DOWN;
    case SDL_SCANCODE_LEFT: return HK_LEFT;
    case SDL_SCANCODE_RIGHT: return HK_RIGHT;
    case SDL_SCANCODE_LSHIFT: return HK_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return HK_RSHIFT;
    case SDL_SCANCODE_LCTRL: return HK_LCTRL;
    case SDL_SCANCODE_RCTRL: return HK_RCTRL;
    case SDL_SCANCODE_LALT: return HK_LALT;
    case SDL_SCANCODE_RALT: return HK_RALT;
    case SDL_SCANCODE_INSERT: return HK_INSERT;
    case SDL_SCANCODE_DELETE: return HK_DELETE;
    case SDL_SCANCODE_HOME: return HK_HOME;
    case SDL_SCANCODE_END: return HK_END;
    case SDL_SCANCODE_PAGEUP: return HK_PGUP;
    case SDL_SCANCODE_PAGEDOWN: return HK_PGDN;
    case SDL_SCANCODE_KP_PLUS: return HK_NUMADD;
    case SDL_SCANCODE_KP_MINUS: return HK_NUMSUB;
    case SDL_SCANCODE_KP_MULTIPLY: return HK_NUMMUL;
    case SDL_SCANCODE_KP_DIVIDE: return HK_NUMDIV;
    case SDL_SCANCODE_KP_PERIOD: return HK_NUMDOT;
    case SDL_SCANCODE_KP_ENTER: return HK_NUMENTER;
    case SDL_SCANCODE_SEMICOLON: return HK_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE: return HK_QUOTE;
    case SDL_SCANCODE_COMMA: return HK_COMMA;
    case SDL_SCANCODE_PERIOD: return HK_PERIOD;
    case SDL_SCANCODE_SLASH: return HK_SLASH;
    case SDL_SCANCODE_LEFTBRACKET: return HK_LBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET: return HK_RBRACKET;
    case SDL_SCANCODE_BACKSLASH: return HK_BACKSLASH;
    case SDL_SCANCODE_MINUS: return HK_MINUS;
    case SDL_SCANCODE_EQUALS: return HK_EQUALS;
    case SDL_SCANCODE_GRAVE: return HK_BACKQUOTE;
    default: return HK_NONE;
    }
}

static void pad_open(int device_index)
{
    int slot;
    SDL_GameController *c;
    if (!SDL_IsGameController(device_index)) return;
    for (slot = 0; slot < 4 && sPad[slot]; slot++) {}
    if (slot == 4) return;
    c = SDL_GameControllerOpen(device_index);
    if (!c) { fprintf(stderr, "sdl: cannot open controller %d: %s\n", device_index, SDL_GetError()); return; }
    sPad[slot] = c;
    sPadId[slot] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    fprintf(stderr, "sdl: controller %d: %s\n", slot, SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?");
}

static void pad_close(SDL_JoystickID id)
{
    int slot;
    for (slot = 0; slot < 4; slot++) {
        if (sPad[slot] && sPadId[slot] == id) { SDL_GameControllerClose(sPad[slot]); sPad[slot] = NULL; }
    }
}

static short axis16(SDL_GameController *c, SDL_GameControllerAxis a) { return (short)SDL_GameControllerGetAxis(c, a); }

static void read_input(HostRawInput *raw)
{
    static const struct { SDL_GameControllerButton sdl; int pb; } map[] = {
#ifdef __SWITCH__
        /* SDL names the buttons by position (A = bottom), the Switch's own labels are the other way round: the right-hand button is "A". The game's
         * "A" (accept) must be the one labelled A on the controller, and B the one labelled B. */
        { SDL_CONTROLLER_BUTTON_B, PB_A }, { SDL_CONTROLLER_BUTTON_A, PB_B }, { SDL_CONTROLLER_BUTTON_Y, PB_X }, { SDL_CONTROLLER_BUTTON_X, PB_Y },
#else
        { SDL_CONTROLLER_BUTTON_A, PB_A }, { SDL_CONTROLLER_BUTTON_B, PB_B }, { SDL_CONTROLLER_BUTTON_X, PB_X }, { SDL_CONTROLLER_BUTTON_Y, PB_Y },
#endif
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PB_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PB_RB },
        { SDL_CONTROLLER_BUTTON_BACK, PB_BACK }, { SDL_CONTROLLER_BUTTON_START, PB_START },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, PB_LSTICK }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PB_RSTICK },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, PB_DUP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PB_DDOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PB_DLEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PB_DRIGHT },
    };
    int i, k, nkeys = 0;
    const Uint8 *ks = SDL_GetKeyboardState(&nkeys);
    memset(raw, 0, sizeof *raw);
    {   /* the window's size, so that the game's menu can be drawn as wide as the window */
        int vw = 0, vh = 0;
        if (sWin) SDL_GL_GetDrawableSize(sWin, &vw, &vh);
        raw->view_w = (unsigned short)(vw > 0 && vw < 65536 ? vw : 0); raw->view_h = (unsigned short)(vh > 0 && vh < 65536 ? vh : 0);
    }
#ifdef __SWITCH__
    raw->console = 1;                                            /* the menu words buttons the console's way (L, R, ZL, Minus) */
#endif
#ifdef __ANDROID__
    raw->touch = 1;                                              /* the menu talks about the Menu button (or Back), not about keys */
#endif
    if (ks) {
        for (i = 0; i < nkeys && i < SDL_NUM_SCANCODES; i++) {
            if (ks[i]) { k = key_from_scancode((SDL_Scancode)i); if (k > 0 && k < HK_COUNT) raw->key[k] = 1; }
        }
        raw->keyboards = 1;
    }
    for (i = 0; i < 4; i++) {
        HostRawPad *p = &raw->pad[i];
        SDL_GameController *c = sPad[i];
        unsigned j;
        if (!c) continue;
        p->connected = 1;
        p->generic = 0;
        snprintf(p->name, sizeof p->name, "%.40s #%d", SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "Controller", i + 1);
        for (j = 0; j < sizeof map / sizeof map[0]; j++) p->down[map[j].pb] = SDL_GameControllerGetButton(c, map[j].sdl) ? 1 : 0;
        p->lx = axis16(c, SDL_CONTROLLER_AXIS_LEFTX);
        p->ly = (short)(-(int)axis16(c, SDL_CONTROLLER_AXIS_LEFTY) - (axis16(c, SDL_CONTROLLER_AXIS_LEFTY) == -32768 ? 1 : 0));    /* up positive */
        p->rx = axis16(c, SDL_CONTROLLER_AXIS_RIGHTX);
        p->ry = (short)(-(int)axis16(c, SDL_CONTROLLER_AXIS_RIGHTY) - (axis16(c, SDL_CONTROLLER_AXIS_RIGHTY) == -32768 ? 1 : 0));
        k = axis16(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7; p->lt = (unsigned char)(k < 0 ? 0 : k > 255 ? 255 : k);
        k = axis16(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7; p->rt = (unsigned char)(k < 0 ? 0 : k > 255 ? 255 : k);
    }
#ifdef __ANDROID__
    if (atomic_load(&sTouchOn) && !sPad[0] && !sPad[1] && !sPad[2] && !sPad[3]) touch_pad(&raw->pad[0]);      /* the on-screen controls are controller 1 while no gamepad is connected */
#endif
}


/* A tap that starts and ends between two frames of the game is never seen: the game reads the controller once per frame, and in the Game Boy Tower (and wherever the
 * game runs slowly) a frame can take 100 ms or more (test 21: "menus wouldn't respond to every directional press"). Every digital input and stick direction that went
 * down is therefore reported as down until the game has shown at least two pictures since (but at most 300 ms), so the game's next reads see it.
 * (Tests 24-27 tried replaying presses that arrive while one is still held up as separate taps; with the Game Boy processor fixed that queue is gone again:
 * a press during the hold simply extends it.) */
#define LATCH_N (4 * (PB_COUNT + 8) + HK_COUNT)
typedef struct { unsigned char on, phys; unsigned frames; unsigned long t; short axis; } Latch;
static volatile int sInputUnpushed;                              /* the last state change has not reached the game yet (core_push_input found the core busy) */
static int latch_step(Latch *l, int phys, unsigned frames, unsigned long now)
{
    if (phys && !l->phys) { l->on = 1; l->frames = frames; l->t = now; }     /* a new press (also while the last one is still being held up) */
    l->phys = (unsigned char)phys;
    if (phys) l->on = 1;
    else if (l->on && !sInputUnpushed && (frames - l->frames >= 2 || now - l->t >= 300)) l->on = 0;     /* (a press the game has not been given yet stays down) */
    return l->on;
}
static void latch_input(HostRawInput *raw, unsigned frames, unsigned long now)
{
    static Latch st[LATCH_N];
    int pad, i, n = 0;
    for (pad = 0; pad < 4; pad++) {
        HostRawPad *p = &raw->pad[pad];
        short *ax[4] = { &p->lx, &p->ly, &p->rx, &p->ry };
        if (!p->connected) { n += PB_COUNT + 8; continue; }
        for (i = 0; i < PB_COUNT; i++, n++) p->down[i] = (unsigned char)latch_step(&st[n], p->down[i], frames, now);
        for (i = 0; i < 8; i++, n++) {              /* the four sticks' two directions each */
            short v = *ax[i >> 1];
            int dir = (i & 1) ? 1 : -1, big = (dir > 0 ? v > 16000 : v < -16000);
            if (big) st[n].axis = v;
            if (latch_step(&st[n], big, frames, now)) { if (!big) *ax[i >> 1] = st[n].axis; }
            else if (big) *ax[i >> 1] = 0;
        }
    }
    for (i = 0; i < HK_COUNT; i++, n++) raw->key[i] = (unsigned char)latch_step(&st[n], raw->key[i], frames, now);
}

/* ---- the game thread, leaving ------------------------------------------------------------------------------------------------------- */
static int sGameExit = -1;
static pthread_t sGameThread;
static int sArgc;
static char **sArgv;
static atomic_int sQuit;

static atomic_int sRestart;                                     /* "Restart game": after the clean-up the program starts itself again */

static void on_exit_hook(int code, void *user)
{
    SDL_Event e;
    (void)user;
    fprintf(stderr, "sdl: the game ended with code %d\n", code);
    sGameExit = code;
    atomic_store(&sQuit, 1);
    SDL_zero(e);
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
    pthread_exit(NULL);                                         /* the game's thread ends here; the main thread cleans up and returns to the launcher */
}

/* "Restart game" (menu, after a Game Boy cartridge or the game save changed). The game has flushed its files; the game's thread ends, main() cleans up
 * (window, GPU, sound) and then starts the new copy: Switch = libnx envSetNextLoad (hbmenu's loader runs the same .nro again when this one returns),
 * Linux = execv of the same program. Returns 0 if that is not possible here (the game then says so). */
static char *sSelfPath;

#ifdef __SWITCH__
/* ---- CPU clock (Switch) -------------------------------------------------------------------------------------------------------------------------
 * Homebrew starts at the CPU's normal 1020 MHz. The game's RSP audio interpreter and the display-list work keep the cores busy (the log's audio line shows the
 * audio task at 60-70 % of a core in battles), so the program asks for 1785 MHz, the top step of the same CPU (what the system itself uses for loading, and what
 * emulators such as RetroArch ask for), and puts the previous rate back when it ends. Creating the file switch/kyujo/no-cpu-boost turns this off. */
static ClkrstSession sClk;
static int sClkOpen, sClkPcv;
static u32 sClkBefore;
static u32 cpu_rate(void)
{
    u32 hz = 0;
    if (sClkOpen) clkrstGetClockRate(&sClk, &hz);
    else if (sClkPcv) pcvGetClockRate(PcvModule_CpuBus, &hz);
    return hz;
}
static void cpu_boost(const char *hostdir)
{
    char flag[1200];
    FILE *f;
    snprintf(flag, sizeof flag, "%s/no-cpu-boost", hostdir);
    if ((f = fopen(flag, "rb")) != NULL) { fclose(f); fprintf(stderr, "sdl: cpu clock left alone (no-cpu-boost file)\n"); return; }
    if (hosversionAtLeast(8, 0, 0)) {
        if (R_SUCCEEDED(clkrstInitialize())) {
            if (R_SUCCEEDED(clkrstOpenSession(&sClk, PcvModuleId_CpuBus, 3))) sClkOpen = 1; else clkrstExit();
        }
    } else if (R_SUCCEEDED(pcvInitialize())) {
        sClkPcv = 1;
    }
    if (!sClkOpen && !sClkPcv) { fprintf(stderr, "sdl: cpu clock: the clock service is not available\n"); return; }
    sClkBefore = cpu_rate();
    {
        Result rc = sClkOpen ? clkrstSetClockRate(&sClk, 1785000000u) : pcvSetClockRate(PcvModule_CpuBus, 1785000000u);
        fprintf(stderr, "sdl: cpu clock: %u MHz -> request 1785 MHz: result 0x%X, now %u MHz\n", sClkBefore / 1000000u, (unsigned)rc, cpu_rate() / 1000000u);
    }
}
/* Test 20's log: the clock was back at 1020 MHz partway through the session (the system puts its own clock back at times; the cause is not in the log), and the
 * battles after that ran with the audio task at 8-10 ms instead of 3 ms. Called about once a second: asks for 1785 MHz again when the rate is lower. */
static void cpu_keep(unsigned long now_ms)
{
    static unsigned long last;
    static int logged;
    u32 hz;
    if (!sClkOpen && !sClkPcv) return;
    if (now_ms - last < 1000) return;
    last = now_ms;
    hz = cpu_rate();
    if (hz && hz < 1785000000u) {
        Result rc = sClkOpen ? clkrstSetClockRate(&sClk, 1785000000u) : pcvSetClockRate(PcvModule_CpuBus, 1785000000u);
        if (logged < 20) { logged++; fprintf(stderr, "sdl: cpu clock was %u MHz: asked for 1785 MHz again: result 0x%X, now %u MHz\n", hz / 1000000u, (unsigned)rc, cpu_rate() / 1000000u); }
    }
}
static void cpu_restore(void)
{
    if (sClkBefore) {
        if (sClkOpen) clkrstSetClockRate(&sClk, sClkBefore); else if (sClkPcv) pcvSetClockRate(PcvModule_CpuBus, sClkBefore);
    }
    if (sClkOpen) { clkrstCloseSession(&sClk); clkrstExit(); sClkOpen = 0; }
    else if (sClkPcv) { pcvExit(); sClkPcv = 0; }
}
#endif
static atomic_int sGameGone;                                    /* the game's thread asked for a restart and has ended */
static int on_restart(void *user)
{
    (void)user;
    fprintf(stderr, "sdl: restart requested\n");
    atomic_store(&sRestart, 1);
    atomic_store(&sGameGone, 1);
    pthread_exit(NULL);                                         /* the main thread starts the game again (restart_in_process), or, if it cannot, the whole program */
    return 1;
}
static atomic_int sGameUp;                                      /* the game has shown its first picture: it is running and takes input */
static atomic_uint sPresents;
static atomic_int sInputOn;                                    /* a picture has been shown (the first-start page counts): the game takes input */
static atomic_int sGraphicsGo;                                  /* the game's first-start page is over: from here on its pictures mean "the game is running" */
static void on_present(const unsigned char *rgb, unsigned w, unsigned h, void *user) { (void)user; pause_gate(); atomic_store(&sInputOn, 1); if (atomic_load(&sGraphicsGo)) atomic_store(&sGameUp, 1); atomic_fetch_add(&sPresents, 1); gles_present(rgb, w, h); }
static void on_fullscreen(int on, void *user) { (void)user; atomic_store(&sFullscreenReq, on ? 1 : 0); }
static void on_graphics_go(void *user) { (void)user; atomic_store(&sGraphicsGo, 1); gles_graphics_go(); }
static void on_backdrop(const unsigned char *rgb, unsigned w, unsigned h, void *user) { (void)user; gles_set_backdrop(rgb, w, h); }
static void on_widescreen(int on, void *user) { (void)user; gles_set_stretch(on & 1); gles_set_filter((on >> 1) & 3); }

static void *game_main(void *arg)
{
    int code = core_run(sArgc, sArgv);
    (void)arg;
    on_exit_hook(code, NULL);
    return NULL;
}


static int sProfStarted;
static int start_game_thread(void)
{
    /* Right after a previous run in the same loader process the heap can still be short ("error 12: Not enough space", test 21: a black first launch). Try again
     * after a short wait, with a smaller stack on the later tries (a smaller stack is only a fallback: it is better than a black screen, and the log says which one was used). */
    static const unsigned sizes[] = { 8u << 20, 8u << 20, 4u << 20, 4u << 20, 2u << 20, 2u << 20, 1u << 20, 1u << 20 };
    unsigned i;
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        pthread_attr_t attr;
        int rc;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, sizes[i]);
        rc = pthread_create(&sGameThread, &attr, game_main, NULL);
        pthread_attr_destroy(&attr);
        if (rc == 0) {
            if (i) fprintf(stderr, "sdl: the game thread started on try %u (stack %u KiB)\n", i + 1, sizes[i] >> 10);
            return 1;
        }
        fprintf(stderr, "sdl: cannot start the game thread (try %u, stack %u KiB, error %d: %s)\n", i + 1, sizes[i] >> 10, rc, strerror(rc));
        fflush(stderr);
        SDL_Delay(250);
    }
    return 0;
}

/* "Restart game" without leaving the program: a chain-loaded second copy of the .nro in the Switch's loader process crashed inside the graphics driver (test 16-18
 * logs: the first 19 shaders built, then a data abort) whatever the first copy closed. So the game is made again here: its threads end, the renderer is drained and
 * its caches dropped, the wasm instance is thrown away and instantiated again, and the game's thread starts over. Window, GL context, sound and controllers stay.
 * Returns 0 if that did not work (the caller then leaves the program and restarts it the old way). */
static int restart_in_process(void)
{
    int left, n;
    fprintf(stderr, "sdl: restarting the game inside the program\n");
    for (n = 0; !atomic_load(&sGameGone) && n < 500; n++) SDL_Delay(2);
    left = core_threads_stop(1500);
    fprintf(stderr, "sdl: game threads stopped (%d did not leave in time)\n", left);
    fflush(stderr);
    if (left != 0 || !atomic_load(&sGameGone)) return 0;
    pthread_join(sGameThread, NULL);
    gles_quiesce();
    atomic_store(&sRingWr, atomic_load(&sRingRd));              /* no sound of the old game is left to play */
    if (core_reset() != 0) { fprintf(stderr, "sdl: the core could not be reset\n"); return 0; }
    atomic_store(&sGameUp, 0);
    atomic_store(&sInputOn, 0);
    atomic_store(&sGraphicsGo, 0);
    atomic_store(&sPresents, 0);
    atomic_store(&sGameGone, 0);
    atomic_store(&sRestart, 0);
    sProfStarted = 0;
    if (!start_game_thread()) return 0;
    fprintf(stderr, "sdl: the game starts again\n");
    fflush(stderr);
    return 1;
}

/* ---- files -------------------------------------------------------------------------------------------------------------------------- */
static int has_rom_ext(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".z64") || !strcasecmp(dot, ".n64") || !strcasecmp(dot, ".v64"));
}

/* The name of the first ROM file in `dir` (host path) into out; 0 if there is none. */
static int find_rom(const char *dir, char *out, size_t size)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.' && has_rom_ext(e->d_name)) { snprintf(out, size, "%s", e->d_name); found = 1; }
    }
    closedir(d);
    return found;
}

static void make_dirs(const char *path)
{
    char tmp[1100];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    }
    mkdir(tmp, 0777);
}

int main(int argc, char **argv)
{
    CoreFront fr;
    GlesHost host;
    const char *root = ".", *home = "/";
    int i, gargc = 1, have_rom = 0, win_w = 1280, win_h = 720, exit_code = 0;
    char **gargv = calloc((size_t)argc + 4, sizeof *gargv);
    char hostdir[1100], rom_name[256], rom_arg[600], log_path[1200];
    static HostRawInput raw, prev;
    unsigned long last_push = 0, last_stat = 0;
    SDL_Event ev;

#ifdef __SWITCH__
    root = "sdmc:/";
    home = "/switch/kyujo";
    socketInitializeDefault();
    nxlinkStdio();
#endif
    gargv[0] = "kyujo";
    sSelfPath = argc > 0 ? argv[0] : NULL;                      /* hbmenu: the .nro's own path; Linux: how the program was started */
#ifdef __SWITCH__
    argc = 1;                                                   /* hbmenu and nxlink pass their own arguments (the file's path ...): none of them is meant for the game */
#endif
#ifdef __ANDROID__
    {   /* the app's own folder (GameActivity.getArguments passes --root and --home); without them the files folder of the app is used */
        const char *d = SDL_AndroidGetInternalStoragePath();
        if (d) root = d;
    }
#endif
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) { sscanf(argv[++i], "%dx%d", &win_w, &win_h); }
        else if (!strcmp(argv[i], "--")) { while (++i < argc) gargv[gargc++] = argv[i]; }
        else { if (!strcmp(argv[i], "--rom")) have_rom = 1; gargv[gargc++] = argv[i]; }
    }
    if (getenv("KYUJO_ROM")) have_rom = 1;

    /* the game's folder: log file, then the ROM */
    snprintf(hostdir, sizeof hostdir, "%s%s%s", root, (root[strlen(root) - 1] == '/' || home[0] == '/') ? "" : "/", home[0] == '/' ? home + 1 : home);
    make_dirs(hostdir);
    snprintf(log_path, sizeof log_path, "%s/kyujo-sdl-log.txt", hostdir);
#ifdef __SWITCH__
    {   /* A log that does not end with "sdl: returning to the launcher" belongs to a run that did not end the normal way. It is also kept, under a name later runs leave
         * alone, so that it can still be sent after the next launch has taken its place as the "previous" log: as kyujo-sdl-log.close.txt when the run was ending the
         * software ("sdl: closing the software" is its last line: whether the system then took it down quietly is not known from here), as kyujo-sdl-log.crash.txt
         * otherwise (a crash, or a close from the Home menu). */
        FILE *f = fopen(log_path, "rb");
        if (f) {
            char tail[4096], keep[1200];
            long sz;
            size_t n;
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fseek(f, sz > (long)sizeof tail - 1 ? sz - ((long)sizeof tail - 1) : 0, SEEK_SET);
            n = fread(tail, 1, sizeof tail - 1, f);
            tail[n] = 0;
            fclose(f);
            if (sz > 0 && !strstr(tail, "sdl: returning to the launcher")) {
                FILE *in = fopen(log_path, "rb"), *out;
                snprintf(keep, sizeof keep, "%s/kyujo-sdl-log.%s.txt", hostdir, strstr(tail, "sdl: closing the software") ? "close" : "crash");
                out = in ? fopen(keep, "wb") : NULL;
                if (in && out) { char buf[8192]; size_t k; while ((k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, out); }
                if (out) fclose(out);
                if (in) fclose(in);
            }
        }
    }
    {   /* the log of the run before stays as kyujo-sdl-log.prev.txt (a "Restart game" starts a second run: the first one's log is the interesting one) */
        char prev[1200];
        snprintf(prev, sizeof prev, "%s/kyujo-sdl-log.prev.txt", hostdir);
        remove(prev);
        rename(log_path, prev);
    }
    freopen(log_path, "w", stderr);                              /* nothing else shows what happens on a console: the file is what to send back */
#elif defined(__ANDROID__)
    {   /* the log of the run before stays as kyujo-sdl-log.prev.txt; the launcher's "Save log" button hands both over */
        char prev[1200];
        snprintf(prev, sizeof prev, "%s/kyujo-sdl-log.prev.txt", hostdir);
        remove(prev);
        rename(log_path, prev);
        freopen(log_path, "w", stderr);
    }
#else
    if (getenv("KYUJO_SDL_LOG")) freopen(log_path, "w", stderr);
#endif
    setvbuf(stderr, NULL, _IOLBF, 0);
    fprintf(stderr, "sdl: build %s %s\n", __DATE__, __TIME__);
#ifdef __SWITCH__
    {   /* what the system gave the program: an error at launch may be a memory shortage (started over a running game or from the album gives far less than a title takeover) */
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        {   /* the C heap at start: a start right after another program in the same loader process may find it in a different state */
            struct mallinfo mi = mallinfo();
            fprintf(stderr, "sdl: heap at start: arena %u KiB, in use %u KiB, free %u KiB\n", (unsigned)mi.arena >> 10, (unsigned)mi.uordblks >> 10, (unsigned)mi.fordblks >> 10);
        }
        fprintf(stderr, "sdl: memory: %llu MiB for the program, %llu MiB in use at start; applet type %d\n", (unsigned long long)(total >> 20), (unsigned long long)(used >> 20), (int)appletGetAppletType());
        fprintf(stderr, "sdl: C heap: %p, %llu MiB (the largest stretch of ordinary pages) of the %llu MiB the loader gave at %p; %u block(s) in all, %u skipped (%llu MiB)\n", (void *)sHeapStart,
                sHeapGood >> 20, sHeapSize >> 20, (void *)sHeapBase, sHeapBlocks, sHeapBadBlocks, sHeapBadBytes >> 20);
        if (sHeapBadBlocks)
            fprintf(stderr, "sdl: C HEAP: pages left locked by the program before this one are skipped; the first at %llX (%llu MiB in) is type %02X perm %u attr %X\n", sHeapFirstBad,
                    (unsigned long long)((sHeapFirstBad - (unsigned long long)(uintptr_t)sHeapBase) >> 20), sHeapBadType, sHeapBadPerm, sHeapBadAttr);
        mem_map_log("start");
    }
#endif
#if defined(__SWITCH__) || defined(__ANDROID__)
    setenv("KYUJO_FIXED_WINDOW", "1", 1);
#endif
#ifdef __SWITCH__
    cpu_boost(hostdir);
#endif
    if (!have_rom && find_rom(hostdir, rom_name, sizeof rom_name)) {
        snprintf(rom_arg, sizeof rom_arg, "%s%s%s", home, home[strlen(home) - 1] == '/' ? "" : "/", rom_name);
        gargv[gargc++] = "--rom";
        gargv[gargc++] = rom_arg;
        have_rom = 1;
        fprintf(stderr, "sdl: ROM: %s\n", rom_arg);
    }
    if (!have_rom) fprintf(stderr, "sdl: no ROM found in %s (put a .z64 file there, or pass --rom)\n", hostdir);

    /* SDL: video (OpenGL ES 3), audio, controllers */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");               /* Back opens the game's menu instead of closing the activity */
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");                 /* the event loop goes on in the background: it holds the game still (pause_gate) and lets the audio stop */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "sdl: SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    sWin = SDL_CreateWindow("Pokemon Stadium", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_w, win_h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
#ifdef __ANDROID__
                                           | SDL_WINDOW_FULLSCREEN                       /* immersive: no status or navigation bar */
#endif
                                           );
    if (!sWin) { fprintf(stderr, "sdl: cannot create the window: %s\n", SDL_GetError()); return 1; }
    sCtx = SDL_GL_CreateContext(sWin);
    if (!sCtx) { fprintf(stderr, "sdl: cannot create an OpenGL ES 3 context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_GL_MakeCurrent(sWin, NULL);                              /* the render thread takes the context */
    audio_start();
    for (i = 0; i < SDL_NumJoysticks(); i++) pad_open(i);

    memset(&fr, 0, sizeof fr);
    fr.present = on_present;
    fr.audio = on_audio;
    fr.set_fullscreen = on_fullscreen;
    fr.set_widescreen = on_widescreen;
    fr.graphics_go = on_graphics_go;
    fr.set_backdrop = on_backdrop;
#if defined(__ANDROID__) || !defined(__SWITCH__)
    fr.pick_gb = on_pick_gb;                                     /* Android: the system's picker; elsewhere only the test's fake one (the Switch has none) */
    fr.pick_gb_poll = on_pick_gb_poll;
#endif
    fr.exit_hook = on_exit_hook;
    fr.restart = on_restart;
    fr.root_dir = root;
    fr.home = home;
    if (core_init(&fr) != 0) { fprintf(stderr, "sdl: cannot start the core\n"); return 1; }
    core_pass_environment(environ);
    memset(&host, 0, sizeof host);
    host.make_current = gl_make_current;
    host.cache_dir = hostdir;
    host.release = gl_release;
    host.swap = gl_swap;
    host.window_size = gl_size;
    if (gles_start(&host) != 0) { fprintf(stderr, "sdl: the GPU renderer could not be started\n"); return 2; }

    sArgc = gargc;
    sArgv = gargv;
    if (!start_game_thread()) {
        /* leave through the normal end below (the renderer thread is running: returning from main here crashed in test 21) */
        fprintf(stderr, "sdl: the game could not be started\n");
        fflush(stderr);
        atomic_store(&sQuit, 1);
    }

    /* the main thread: events and input until the game ends or the system asks us to quit */
    while (!atomic_load(&sQuit)) {
        int req;
        unsigned long now;
        if (atomic_load(&sRestart) && !restart_in_process()) { sGameExit = 0; atomic_store(&sQuit, 1); break; }       /* (the old way: leave and start the program again) */
#ifndef __SWITCH__
        {   /* development: KYUJO_FAKE_TRAP=<seconds> ends the program as if the game had trapped, to see the stop message */
            static long at = -2;
            if (at == -2) { const char *e = getenv("KYUJO_FAKE_TRAP"); at = e ? (long)SDL_GetTicks() + 1000L * atol(e) : -1; }
            if (at > 0 && (long)SDL_GetTicks() >= at) { sGameExit = 70; atomic_store(&sQuit, 1); break; }
        }
#endif
        if (SDL_WaitEventTimeout(&ev, 8)) {
            do {
                switch (ev.type) {
                case SDL_QUIT: atomic_store(&sQuit, 1); break;
                case SDL_APP_WILLENTERBACKGROUND: atomic_store(&sPaused, 1); fprintf(stderr, "sdl: the app is in the background: the game is held\n"); break;
                case SDL_APP_DIDENTERFOREGROUND:
                    atomic_fetch_add(&sSurfaceEpoch, 1);
                    atomic_store(&sPaused, 0);
                    fprintf(stderr, "sdl: the app is back in the foreground\n");
                    break;
                case SDL_CONTROLLERDEVICEADDED: pad_open(ev.cdevice.which); break;
                case SDL_CONTROLLERDEVICEREMOVED: pad_close(ev.cdevice.which); break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT)) atomic_store(&sFullscreenReq, !sFullscreen);
                    break;
                default: break;
                }
            } while (SDL_PollEvent(&ev));
        }
        now = (unsigned long)SDL_GetTicks();
#ifdef __SWITCH__
        cpu_keep(now);
#endif
        {   /* the game stands still: a picture does not come and nothing is delivered to the game either (the menu pausing the game also looks like this) */
            static unsigned last_n; static unsigned long last_change, last_report; static int reports;
            unsigned n = atomic_load(&sPresents) + (unsigned)gles_pictures_shown();
            if (n) atomic_store(&sInputOn, 1);
            if (n && atomic_load(&sGraphicsGo)) atomic_store(&sGameUp, 1);                          /* pictures shown straight from the GPU do not pass through on_present */
            if (atomic_load(&sGameUp)) n += core_dbg_progress() * 7u;
            if (n != last_n || !last_change) { last_n = n; last_change = now; reports = 0; }
            else if (atomic_load(&sGameUp) && now - last_change > 2000 && now - last_report > 3000 && reports < 12) {
                char d[200]; unsigned age; const char *note = core_last_note(&age);
                reports++; last_report = now;
                gles_describe(d, sizeof d);
                { char m[1800]; core_dbg_describe(m, sizeof m); fprintf(stderr, "sdl: STALL: scheduler: %s\n", m); }
                if (reports <= 2) { char m[1500]; mips_native_describe(m, sizeof m); fprintf(stderr, "sdl: STALL: native MIPS interpreter: %s\n", m); }
                if (reports <= 2) { char h[400]; core_fn_history(h, sizeof h); fprintf(stderr, "sdl: STALL: game functions entered last (newest first): %s\n", h); }
                fprintf(stderr, "sdl: STALL: nothing new for %lu ms; wasm lock %s; %s; last file/host call '%s' %u ms ago\n", now - last_change, core_lock_is_busy() ? "held" : "free", d, note, age);
            }
        }
        if (now - last_stat > 10000) {
            last_stat = now;
            if (!sProfStarted) { sProfStarted = 1; core_profile_start(); }
            { char pr[400]; core_profile_report(pr, sizeof pr); if (pr[0]) fprintf(stderr, "sdl: profile: %s\n", pr); }
            { char gs[400]; gles_period_stats(gs, sizeof gs); fprintf(stderr, "sdl: %s\n", gs); }
            fprintf(stderr, "sdl: audio: %u underruns, %u bytes dropped in total; core memory %llu MiB"
#ifdef __SWITCH__
                    "; cpu %u MHz; C heap used up to %llu of %llu MiB"
#endif
                    "\n", atomic_load(&sAudioUnderruns), atomic_load(&sAudioDropped), core_memory_bytes() >> 20
#ifdef __SWITCH__
                    , cpu_rate() / 1000000u, (unsigned long long)(((char *)sbrk(0) - sHeapStart) >> 20), sHeapGood >> 20
#endif
                    );
        }
        req = atomic_exchange(&sFullscreenReq, -1);
#if defined(__SWITCH__) || defined(__ANDROID__)
        req = -1;               /* the screen is always full: switching the window mode on a console broke the picture (and a saved fullscreen=1 made it unrecoverable) */
#endif
        if (req >= 0 && req != sFullscreen) { sFullscreen = req; SDL_SetWindowFullscreen(sWin, req ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
        read_input(&raw);
        now = (unsigned long)SDL_GetTicks();
        if (atomic_load(&sInputOn)) latch_input(&raw, atomic_load(&sPresents) + (unsigned)gles_pictures_shown(), now);
        if (atomic_load(&sInputOn) && !getenv("KYUJO_SDL_NOINPUT") && (memcmp(&raw, &prev, sizeof raw) != 0 || now - last_push > 100)) {
            if (core_push_input((const HostRawInput *)&raw)) {
                memcpy(&prev, &raw, sizeof raw);
                last_push = now;
                sInputUnpushed = 0;
            } else sInputUnpushed = 1;
            { static unsigned fails, tries; static unsigned long since; tries++; fails += sInputUnpushed;
              if (now - since > 10000) { if (fails) fprintf(stderr, "sdl: input: %u of %u pushes found the core busy in 10 s\n", fails, tries); since = now; fails = tries = 0; } }
        }
    }
    exit_code = sGameExit >= 0 ? sGameExit : 0;
    fprintf(stderr, "sdl: leaving (exit code %d)\n", exit_code);
    fflush(stderr);
    {   /* every thread of the game must be gone before the process returns to the Switch's loader (see core_threads_stop); elsewhere it is just tidy */
        int left = core_threads_stop(1500);
        fprintf(stderr, "sdl: game threads stopped (%d did not leave in time)\n", left);
        fflush(stderr);
        if (sGameExit >= 0) pthread_join(sGameThread, NULL);      /* the game's own thread has ended: frees its stack and kernel handle */
    }
    atomic_store(&sPaused, 0);
    if (sAudio) SDL_CloseAudioDevice(sAudio);
    if (exit_code == 70) {   /* the game trapped (an invalid memory access in the game's own code): say so on the screen, it used to just vanish to the launcher */
        uint32_t t0 = SDL_GetTicks();
        SDL_Event ev;
        gles_show_notice("THE GAME STOPPED", "AN INTERNAL ERROR: CODE 70", "DETAILS: KYUJO-SDL-LOG.TXT");
        fprintf(stderr, "sdl: the stop message is on the screen\n");
        while (SDL_GetTicks() - t0 < 8000u) {
            int pressed = 0;
            while (SDL_PollEvent(&ev)) if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_KEYDOWN || ev.type == SDL_FINGERDOWN) pressed = 1;
            if (pressed && SDL_GetTicks() - t0 > 1500u) break;             /* a button after a moment: on to the launcher */
            SDL_Delay(30);
        }
    }
#ifdef __SWITCH__
    mem_map_log("leaving, before the graphics teardown");
#endif
    gles_stop();
    fprintf(stderr, "sdl: render thread stopped\n");
    fflush(stderr);
    SDL_GL_DeleteContext(sCtx);
    SDL_DestroyWindow(sWin);
    SDL_Quit();
    fprintf(stderr, "sdl: window and driver closed\n");
    fflush(stderr);
#ifdef __SWITCH__
    {   /* The graphics driver (Mesa/nouveau on libnx's nv service) opens the nv service with an 8 MB transfer memory taken from this program's heap. If the service is
         * still open when the program returns to hbloader, those pages stay locked, and the next program loaded into the same process (a restart) crashes the
         * moment it touches them. Close it here whatever the driver's own reference count says (the loop stops when the service is no longer open). */
        int n = 0;
        while (serviceIsActive(nvGetServiceSession()) && n < 16) { nvExit(); n++; }
        fprintf(stderr, "sdl: graphics service closed (%d reference(s) left open by the driver)\n", n);
        fflush(stderr);
    }
    cpu_restore();
    socketExit();
    mem_map_log("leaving, after the graphics teardown and the services");
    {   /* What the next program in this loader process will find: the graphics driver leaves some blocks of the heap locked (see __libnx_initheap), and the next start
         * skips them. The process only ends here instead of going back to the loader when too little ordinary heap is left in one piece for another start (the next
         * start is then a fresh process, and the system goes to its Home menu instead of hbmenu). */
        unsigned long long at, len, bad_bytes, first_bad;
        unsigned blocks, bad_blocks, t = 0, p = 0, a2 = 0;
        heap_runs((unsigned long long)(uintptr_t)sHeapBase, sHeapSize, &at, &len, &blocks, &bad_blocks, &bad_bytes, &first_bad, &t, &p, &a2);
        sHeapDamaged = len < HEAP_ENOUGH;
        fprintf(stderr, "sdl: heap check at the end: largest stretch of ordinary pages %llu MiB at %llX, %u block(s) in all, %u locked or otherwise changed (%llu MiB); %s\n", len >> 20, at, blocks,
                bad_blocks, bad_bytes >> 20, sHeapDamaged ? "TOO LITTLE LEFT for another start in this process" : "enough for another start");
        if (sHeapDamaged) exit_code = 100;
    }
    if ((exit_code == 100 && !atomic_load(&sRestart)) || sHeapDamaged) {
        /* "Quit and close the software" (or a heap that cannot be reused): end the process through the applet service's own exit, like hbmenu does. */
        __nx_applet_exit_mode = 1;
        fprintf(stderr, "sdl: closing the software\n");
        fflush(stderr);
        exit(0);
    }
#endif
    if (atomic_load(&sRestart)) {
#ifdef __SWITCH__
        char arg[1100];
        snprintf(arg, sizeof arg, "\"%s\"", sSelfPath);
        envSetNextLoad(sSelfPath, arg);                         /* the loader starts it when this process has ended (the exit below) */
        fprintf(stderr, "sdl: next load: %s\n", sSelfPath);
        fflush(stderr);
        exit_code = 0;
#else
        fflush(stderr);
#ifdef __ANDROID__
        fprintf(stderr, "sdl: the game could not be restarted inside the program; leaving (start it again from the launcher)\n");     /* (/proc/self/exe is the system's app process here) */
#else
        execv("/proc/self/exe", argv);
        fprintf(stderr, "sdl: restart failed: %s\n", strerror(errno));
#endif
#endif
    }
    if (exit_code == 100) exit_code = 0;                            /* (only the Switch ends the process differently for "quit and close"; elsewhere both are the same) */
    fprintf(stderr, "sdl: returning to the launcher\n");           /* the last line of a run that ended the normal way (the next start looks for it) */
    fflush(stderr);
#ifdef __ANDROID__
    return exit_code;                                            /* SDL ends the activity (GameActivity.onDestroy then ends the process, so the next start finds no old state) */
#endif
    exit(exit_code);                                             /* other game threads may still be running: leave the process as a whole */
}
