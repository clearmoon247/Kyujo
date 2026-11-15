/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_x11.c - a small X11 client that speaks the X protocol directly. See host_x11.h for why.
 *
 * The protocol, as far as it is used here (X Window System Protocol, X11R7; every number below is from that specification):
 *   - A request is  opcode(1) data(1) length(2, in 4-byte units, the header included)  followed by its body. Byte order is chosen by the client in
 *     the first bytes it sends ('l' = little endian, used here). The server numbers the requests 1, 2, 3 ... (16 bits on the wire).
 *   - The server sends 32-byte packets: 0 = error, 1 = reply (followed by 4 * (the length field at byte 4) more bytes), 2..127 = events.
 *     A reply carries the sequence number of its request in bytes 2..3.
 *   - Resource ids (window, graphics context ...) are numbers the client makes up from the range the server gave it (base + 0 .. mask).
 * Requests used: CreateWindow(1) ChangeWindowAttributes(2) MapWindow(8) InternAtom(16) ChangeProperty(18) GetProperty(20) SendEvent(25)
 *   SetInputFocus(42) QueryKeymap(44) CreatePixmap(53) FreePixmap(54) CreateGC(55) FreeGC(60) PutImage(72) PolyFillRectangle(70)
 *   GetKeyboardMapping(101) CreatePixmapCursor(93).
 * Events used: KeyPress/Release are NOT used (the keyboard is read with QueryKeymap, which gives the state of all keys at once and has no
 *   auto-repeat flicker); FocusIn(9) FocusOut(10) Expose(12) MapNotify(19) ConfigureNotify(22) PropertyNotify(28) ClientMessage(33).
 *
 * Authority: the server only talks to clients that send its "MIT-MAGIC-COOKIE-1" (16 bytes). It is read from the X authority file
 * ($XAUTHORITY, ~/.Xauthority, or the file named by the "-auth" argument of the running X server, which is how a display started by a
 * start script, as on Batocera, can be reached by a program started from elsewhere). If no cookie is found the connection is tried without.
 *
 * HOST-side file (Linux only). Tested against Xvfb (tests/native/x11/).
 */
#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <dirent.h>

#include "host_x11.h"
#include "host_ui.h"

struct X11Win {
    int fd;
    int dead;                                   /* the connection failed: every call does nothing from now on */
    uint32_t seq;                               /* requests sent so far (the last request's number; the wire carries the low 16 bits) */
    uint32_t rid_base, rid_mask, rid_next;      /* resource ids this client may use */
    uint32_t root, root_visual;
    unsigned root_depth, screen_w, screen_h;
    unsigned max_req_bytes;                     /* the largest request the server takes (no BIG-REQUESTS extension is used) */
    unsigned min_kc, max_kc;
    uint32_t win, gc;
    uint32_t a_wm_protocols, a_wm_delete, a_net_wm_state, a_net_wm_state_fullscreen, a_net_wm_name, a_utf8;
    unsigned cw, ch;                            /* the client area now */
    int focused, mapped, fullscreen, redraw, want_close;
    int focus_taken;                            /* SetInputFocus was sent once the window became visible */
    int state_changed;                          /* _NET_WM_STATE changed: read it again (done in x11win_pump, never inside an event handler: see handle_event) */
    uint32_t blank_cursor;
    unsigned char kc_hk[256];                   /* X keycode -> HK_* (0 = not a key the game uses) */
    unsigned char sendbuf[65536];
    unsigned sendlen;
    unsigned errors_logged;
    const char *transport;
};

/* ---- little-endian helpers ---- */
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static unsigned get16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static unsigned pad4(unsigned n) { return (n + 3u) & ~3u; }

/* ---- the socket ---- */

static int write_all(X11Win *x, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        ssize_t n = send(x->fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { struct pollfd pf = {x->fd, POLLOUT, 0}; poll(&pf, 1, 100); continue; }
            x->dead = 1;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void flush_out(X11Win *x)
{
    if (x->sendlen && !x->dead) write_all(x, x->sendbuf, x->sendlen);
    x->sendlen = 0;
}

static void queue_out(X11Win *x, const void *buf, unsigned len)
{
    if (x->sendlen + len > sizeof x->sendbuf) flush_out(x);
    if (len > sizeof x->sendbuf) { write_all(x, buf, len); return; }
    memcpy(x->sendbuf + x->sendlen, buf, len);
    x->sendlen += len;
}

/* Reads exactly len bytes, waiting as long as needed (the server answers at once; a dead server closes the socket). */
static int read_all(X11Win *x, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        ssize_t n = recv(x->fd, p, len, 0);
        if (n == 0) { x->dead = 1; return -1; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { struct pollfd pf = {x->fd, POLLIN, 0}; poll(&pf, 1, 1000); continue; }
            x->dead = 1;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Queues a request. `body` is the part after the 4-byte header (padded here to a multiple of 4). Returns the request's sequence number. */
static uint32_t request(X11Win *x, unsigned opcode, unsigned data, const void *body, unsigned body_len)
{
    unsigned char hdr[4];
    unsigned total = 4 + pad4(body_len);
    static const unsigned char zeros[4] = {0, 0, 0, 0};
    hdr[0] = (unsigned char)opcode;
    hdr[1] = (unsigned char)data;
    put16(hdr + 2, total / 4);
    queue_out(x, hdr, 4);
    if (body_len) queue_out(x, body, body_len);
    if (pad4(body_len) != body_len) queue_out(x, zeros, pad4(body_len) - body_len);
    return ++x->seq;
}

static uint32_t new_id(X11Win *x)
{
    /* The ids are base | (n & mask-aligned): the server gave the range as a base and a mask of the usable low bits (a contiguous run of ones
     * starting at some bit); the next id is the base plus the next value of that bit field. */
    uint32_t lowbit = x->rid_mask & (~x->rid_mask + 1u);
    uint32_t id = x->rid_base | (x->rid_next * lowbit);
    x->rid_next++;
    return id;
}

/* ---- events ---- */

static void log_error(X11Win *x, const unsigned char *e)
{
    if (x->errors_logged++ < 20)
        fprintf(stderr, "kyujo: x11: the server reported an error: code %u, request opcode %u.%u, bad value 0x%x (request number %u)\n",
                e[1], e[10], get16(e + 8), (unsigned)get32(e + 4), get16(e + 2));
}

static void read_fullscreen_state(X11Win *x);

static void handle_event(X11Win *x, const unsigned char *e)
{
    switch (e[0] & 0x7f) {
    case 0: log_error(x, e); break;
    case 9:                                     /* FocusIn: detail at byte 1 (2 = NotifyInferior: a child got it; not the window itself) */
        if (e[1] != 2) x->focused = 1;
        break;
    case 10:
        if (e[1] != 2) x->focused = 0;
        break;
    case 12:                                    /* Expose: count (bytes 16..17) is the number of Expose events still to come; paint once, on the last */
        if (get16(e + 16) == 0) x->redraw = 1;
        break;
    case 19:                                    /* MapNotify */
        x->mapped = 1;
        x->redraw = 1;
        break;
    case 22: {                                  /* ConfigureNotify: width at 20, height at 22 */
        unsigned w = get16(e + 20), h = get16(e + 22);
        if (w && h && (w != x->cw || h != x->ch)) { x->cw = w; x->ch = h; x->redraw = 1; }
        break;
    }
    case 28:                                    /* PropertyNotify: window 4, atom 8 */
        /* Only a flag: reading the property sends a request and waits for its reply, and this handler also runs while another request waits for
         * ITS reply (wait_reply); a nested wait would swallow the outer reply as "a reply to something else" and the outer wait would never end. */
        if (get32(e + 8) == x->a_net_wm_state) x->state_changed = 1;
        break;
    case 33:                                    /* ClientMessage: window 4, type 8, data from 12 */
        if (get32(e + 8) == x->a_wm_protocols && get32(e + 12) == x->a_wm_delete) x->want_close = 1;
        break;
    default: break;
    }
}

/* Waits for the reply to request `seq`. Events that arrive meanwhile are handled. On success fills reply[32] and, if the reply has more data,
 * *extra (malloc'ed, the caller frees) with *extra_len bytes. Returns 0, or -1 if the server answered with an error or the connection died. */
static int wait_reply(X11Win *x, uint32_t seq, unsigned char reply[32], unsigned char **extra, unsigned *extra_len)
{
    flush_out(x);
    if (extra) { *extra = NULL; *extra_len = 0; }
    for (;;) {
        unsigned char p[32];
        if (x->dead || read_all(x, p, 32) != 0) return -1;
        if (p[0] == 1) {
            unsigned more = get32(p + 4) * 4u;
            unsigned char *buf = NULL;
            if (more) {
                buf = (unsigned char *)malloc(more);
                if (!buf || read_all(x, buf, more) != 0) { free(buf); return -1; }
            }
            if ((get16(p + 2)) == (seq & 0xFFFF)) {
                memcpy(reply, p, 32);
                if (extra) { *extra = buf; *extra_len = more; } else free(buf);
                return 0;
            }
            free(buf);                          /* the reply to something else: not expected, ignored */
        } else if (p[0] == 0) {
            if (get16(p + 2) == (seq & 0xFFFF)) { log_error(x, p); return -1; }
            log_error(x, p);
        } else {
            if ((p[0] & 0x7f) == 35) {          /* GenericEvent has extra data: skip it */
                unsigned more = get32(p + 4) * 4u;
                unsigned char tmp[256];
                while (more) { unsigned n = more > sizeof tmp ? (unsigned)sizeof tmp : more; if (read_all(x, tmp, n)) return -1; more -= n; }
            }
            handle_event(x, p);
        }
    }
}

static uint32_t intern_atom(X11Win *x, const char *name)
{
    unsigned char body[64];
    unsigned char rep[32];
    unsigned n = (unsigned)strlen(name);
    uint32_t seq;
    if (n > 48) return 0;
    put16(body, n);
    put16(body + 2, 0);
    memcpy(body + 4, name, n);
    seq = request(x, 16, 0, body, 4 + n);       /* data byte = only_if_exists: 0 = create the atom if it is new */
    if (wait_reply(x, seq, rep, NULL, NULL) != 0) return 0;
    return get32(rep + 8);
}

/* Sets a property made of bytes (format 8: `n` bytes) or 32-bit values (format 32: `n` values) on our window. */
static void set_property(X11Win *x, uint32_t atom, uint32_t type, unsigned format, const void *data, unsigned n)
{
    unsigned bytes = n * (format / 8);
    unsigned char *b = (unsigned char *)calloc(1, 20 + pad4(bytes));
    if (!b) return;
    put32(b, x->win);
    put32(b + 4, atom);
    put32(b + 8, type);
    b[12] = (unsigned char)format;               /* bytes 13..15 are padding */
    put32(b + 16, n);                            /* length in units of `format` bits */
    memcpy(b + 20, data, bytes);
    request(x, 18, 0, b, 20 + bytes);            /* data byte = mode: 0 = Replace */
    free(b);
}

static void read_fullscreen_state(X11Win *x)
{
    unsigned char body[24], rep[32], *extra = NULL;
    unsigned extra_len = 0, i, n, full = 0;
    uint32_t seq;
    put32(body, x->win);
    put32(body + 4, x->a_net_wm_state);
    put32(body + 8, 0);                          /* type: AnyPropertyType */
    put32(body + 12, 0);                         /* offset */
    put32(body + 16, 64);                        /* length in 4-byte units */
    seq = request(x, 20, 0, body, 20);
    if (wait_reply(x, seq, rep, &extra, &extra_len) != 0) return;
    n = get32(rep + 16);                         /* value length in format units */
    if (rep[1] == 32 && extra) for (i = 0; i < n && i * 4 + 4 <= extra_len; i++) if (get32(extra + i * 4) == x->a_net_wm_state_fullscreen) full = 1;
    free(extra);
    if ((int)full != x->fullscreen) {
        x->fullscreen = (int)full;
        x->redraw = 1;
        if (x->blank_cursor) {                   /* no mouse pointer over the full screen picture; back to the default when the window is small again */
            unsigned char b[12];
            put32(b, x->win);
            put32(b + 4, 0x4000);                /* value mask: CWCursor */
            put32(b + 8, full ? x->blank_cursor : 0);
            request(x, 2, 0, b, 12);             /* ChangeWindowAttributes */
        }
    }
}

/* ---- keys ---- */

static int keysym_to_hk(uint32_t ks)
{
    if (ks >= 'a' && ks <= 'z') return (int)(ks - 'a' + 'A');
    if (ks >= 'A' && ks <= 'Z') return (int)ks;
    if (ks >= '0' && ks <= '9') return (int)ks;
    if (ks >= 0xffbe && ks <= 0xffc9) return HK_F1 + (int)(ks - 0xffbe);
    if (ks >= 0xffb0 && ks <= 0xffb9) return HK_NUM0 + (int)(ks - 0xffb0);          /* KP_0 .. KP_9 */
    switch (ks) {
    case 0xff08: return HK_BACKSPACE;
    case 0xff09: return HK_TAB;
    case 0xff0d: return HK_ENTER;
    case 0xff1b: return HK_ESC;
    case 0x0020: return HK_SPACE;
    case 0xff52: return HK_UP;
    case 0xff54: return HK_DOWN;
    case 0xff51: return HK_LEFT;
    case 0xff53: return HK_RIGHT;
    case 0xffe1: return HK_LSHIFT;
    case 0xffe2: return HK_RSHIFT;
    case 0xffe3: return HK_LCTRL;
    case 0xffe4: return HK_RCTRL;
    case 0xffe9: case 0xffe7: return HK_LALT;                                         /* Alt_L, Meta_L */
    case 0xffea: case 0xffe8: case 0xfe03: return HK_RALT;                            /* Alt_R, Meta_R, ISO_Level3_Shift (AltGr) */
    case 0xff63: return HK_INSERT;
    case 0xffff: return HK_DELETE;
    case 0xff50: return HK_HOME;
    case 0xff57: return HK_END;
    case 0xff55: return HK_PGUP;
    case 0xff56: return HK_PGDN;
    case 0xffab: return HK_NUMADD;
    case 0xffad: return HK_NUMSUB;
    case 0xffaa: return HK_NUMMUL;
    case 0xffaf: return HK_NUMDIV;
    case 0xffae: return HK_NUMDOT;
    case 0xff8d: return HK_NUMENTER;
    case 0x003b: return HK_SEMICOLON;
    case 0x0027: return HK_QUOTE;
    case 0x002c: return HK_COMMA;
    case 0x002e: return HK_PERIOD;
    case 0x002f: return HK_SLASH;
    case 0x005b: return HK_LBRACKET;
    case 0x005d: return HK_RBRACKET;
    case 0x005c: return HK_BACKSLASH;
    case 0x002d: return HK_MINUS;
    case 0x003d: return HK_EQUALS;
    case 0x0060: return HK_BACKQUOTE;
    default: return 0;
    }
}

/* Reads the server's keyboard table and builds keycode -> HK_*. A keycode has several keysyms (plain, shifted, ...); the first one that is a
 * key of ours decides. On the numeric pad with NumLock off the plain keysym is KP_Home etc. and the shifted one is KP_7, so scanning the first
 * four columns finds the digit. */
static void load_keymap(X11Win *x)
{
    unsigned char body[4], rep[32], *extra = NULL;
    unsigned extra_len = 0, per, kc, col;
    uint32_t seq;
    body[0] = (unsigned char)x->min_kc;
    body[1] = (unsigned char)(x->max_kc - x->min_kc + 1);
    body[2] = body[3] = 0;
    seq = request(x, 101, 0, body, 4);
    if (wait_reply(x, seq, rep, &extra, &extra_len) != 0) { fprintf(stderr, "kyujo: x11: could not read the keyboard table; keys will not work\n"); return; }
    per = rep[1];
    for (kc = x->min_kc; kc <= x->max_kc; kc++) {
        for (col = 0; col < per && col < 4; col++) {
            unsigned idx = ((kc - x->min_kc) * per + col) * 4;
            int hk;
            if (idx + 4 > extra_len) break;
            hk = keysym_to_hk(get32(extra + idx));
            if (hk) { x->kc_hk[kc] = (unsigned char)hk; break; }
        }
    }
    free(extra);
}

void x11win_keys(X11Win *x, unsigned char key[256])
{
    unsigned char rep[32], *extra = NULL;
    unsigned extra_len = 0, kc;
    uint32_t seq;
    if (!x || x->dead) return;
    seq = request(x, 44, 0, NULL, 0);
    if (wait_reply(x, seq, rep, &extra, &extra_len) != 0) { free(extra); return; }
    /* The 32 key bytes start at byte 8 of the reply: 24 inside the 32-byte packet, the rest in `extra`. */
    {
        unsigned char keys[32];
        memcpy(keys, rep + 8, 24);
        if (extra && extra_len >= 8) memcpy(keys + 24, extra, 8); else memset(keys + 24, 0, 8);
        for (kc = 0; kc < 256; kc++) {
            if ((keys[kc >> 3] >> (kc & 7)) & 1) if (x->kc_hk[kc]) key[x->kc_hk[kc]] = 1;
        }
    }
    free(extra);
}

/* ---- connecting ---- */

static int connect_unix(const char *path, int abstract_name)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    socklen_t len;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (abstract_name) { a.sun_path[0] = 0; snprintf(a.sun_path + 1, sizeof a.sun_path - 1, "%s", path); len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(path)); }
    else { snprintf(a.sun_path, sizeof a.sun_path, "%s", path); len = (socklen_t)sizeof a; }
    if (connect(fd, (struct sockaddr *)&a, len) != 0) { close(fd); return -1; }
    return fd;
}

static int connect_tcp(const char *host, int port)
{
    struct addrinfo hints, *res, *r;
    char ps[16];
    int fd = -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    for (r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Splits $DISPLAY ("[host]:number[.screen]") */
static int parse_display(const char *d, char *host, unsigned hostsz, int *num)
{
    const char *colon = strrchr(d, ':');
    size_t n;
    if (!colon || colon[1] < '0' || colon[1] > '9') return -1;
    n = (size_t)(colon - d);
    if (n >= hostsz) return -1;
    memcpy(host, d, n);
    host[n] = 0;
    *num = atoi(colon + 1);
    return 0;
}

typedef struct AuthEntry { unsigned family; char addr[128]; char number[16]; char name[32]; unsigned char data[64]; unsigned data_len; } AuthEntry;

/* Reads one entry of an X authority file (all numbers big endian, strings with a 16-bit length in front). Returns 0 at the end of the file. */
static int read_auth_entry(FILE *f, AuthEntry *e)
{
    unsigned char b[2];
    unsigned len;
    memset(e, 0, sizeof *e);
    if (fread(b, 1, 2, f) != 2) return 0;
    e->family = (unsigned)b[0] << 8 | b[1];
#define STR(dst, sz) do { if (fread(b, 1, 2, f) != 2) return 0; len = (unsigned)b[0] << 8 | b[1]; if (len >= (sz)) { fseek(f, (long)len, SEEK_CUR); (dst)[0] = 0; } else { if (fread(dst, 1, len, f) != len) return 0; (dst)[len] = 0; } } while (0)
    STR(e->addr, sizeof e->addr);
    STR(e->number, sizeof e->number);
    STR(e->name, sizeof e->name);
    if (fread(b, 1, 2, f) != 2) return 0;
    len = (unsigned)b[0] << 8 | b[1];
    if (len > sizeof e->data) { fseek(f, (long)len, SEEK_CUR); e->data_len = 0; return 1; }
    if (fread(e->data, 1, len, f) != len) return 0;
    e->data_len = len;
    return 1;
#undef STR
}

/* Looks for the cookie of display `num` in the file. Preference: an entry for this machine's name, then the "any host" entry, then any entry for
 * that display number. Returns 1 and fills cookie[16] when found. */
static int find_cookie_in(const char *path, int num, unsigned char cookie[16])
{
    FILE *f = fopen(path, "rb");
    AuthEntry e;
    char me[128] = "";
    int best = 0;
    char ns[16];
    if (!f) return 0;
    gethostname(me, sizeof me - 1);
    snprintf(ns, sizeof ns, "%d", num);
    while (read_auth_entry(f, &e)) {
        int rank;
        if (strcmp(e.name, "MIT-MAGIC-COOKIE-1") != 0 || e.data_len != 16) continue;
        if (e.number[0] && strcmp(e.number, ns) != 0) continue;
        if (e.family == 256 && strcmp(e.addr, me) == 0) rank = 3;
        else if (e.family == 65535) rank = 2;
        else rank = 1;
        if (rank > best) { best = rank; memcpy(cookie, e.data, 16); }
    }
    fclose(f);
    return best > 0;
}

/* The "-auth <file>" argument of a running X server (/proc/<pid>/cmdline of a process called X or Xorg). */
static int find_server_auth_file(char *out, size_t outsz)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    int found = 0;
    if (!d) return 0;
    while (!found && (de = readdir(d)) != NULL) {
        char path[64], cmd[4096];
        FILE *f;
        size_t n, i;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/cmdline", de->d_name);
        f = fopen(path, "rb");
        if (!f) continue;
        n = fread(cmd, 1, sizeof cmd - 1, f);
        fclose(f);
        if (n == 0) continue;
        cmd[n] = 0;
        {
            const char *base = strrchr(cmd, '/');
            base = base ? base + 1 : cmd;
            if (strcmp(base, "X") != 0 && strcmp(base, "Xorg") != 0) continue;
        }
        for (i = 0; i < n; i += strlen(cmd + i) + 1) {
            if (strcmp(cmd + i, "-auth") == 0 && i + 6 < n) { snprintf(out, outsz, "%s", cmd + i + 6); found = 1; break; }
        }
    }
    closedir(d);
    return found;
}

/* Sends the connection set-up request and reads the answer. On success the server's description is in *x. */
static int do_setup(X11Win *x, const unsigned char *cookie, char *err, unsigned errsz)
{
    unsigned char req[64], head[8], *body;      /* 12 + 20 (name, padded) + 16 (cookie) = 48 at most */
    unsigned name_len = cookie ? 18 : 0, data_len = cookie ? 16 : 0, n = 12, add, i;
    memset(req, 0, sizeof req);
    req[0] = 'l';                                /* little endian */
    put16(req + 2, 11);                          /* protocol 11.0 */
    put16(req + 4, 0);
    put16(req + 6, name_len);
    put16(req + 8, data_len);
    if (cookie) {
        memcpy(req + 12, "MIT-MAGIC-COOKIE-1", 18);          /* padded to 20 */
        memcpy(req + 12 + 20, cookie, 16);
        n = 12 + 20 + 16;
    }
    if (write_all(x, req, n) != 0) { snprintf(err, errsz, "cannot send to the X server"); return -1; }
    if (read_all(x, head, 8) != 0) { snprintf(err, errsz, "the X server closed the connection during set-up"); return -2; }
    add = get16(head + 6) * 4u;
    body = (unsigned char *)malloc(add + 8);
    if (!body || read_all(x, body, add) != 0) { free(body); snprintf(err, errsz, "short set-up reply"); return -1; }
    if (head[0] != 1) {
        unsigned rl = head[1];
        snprintf(err, errsz, "the X server refused the connection: %.*s", (int)(rl < add ? rl : add), (const char *)body);
        free(body);
        return -2;
    }
    {
        unsigned vendor_len = get16(body + 16), nfmt = body[21], nscr = body[20], off;
        unsigned image_order = body[22];
        x->rid_base = get32(body + 4);
        x->rid_mask = get32(body + 8);
        x->max_req_bytes = get16(body + 18) * 4u;
        x->min_kc = body[26];
        x->max_kc = body[27];
        off = 32 + pad4(vendor_len);
        /* the pixmap formats: (depth, bits per pixel, scanline pad), 8 bytes each. Needed: the one for the root depth must be 32 bits per pixel. */
        x->root_depth = 0;
        {
            unsigned fmt_off = off;
            off += nfmt * 8;
            if (nscr < 1 || off + 40 > add) { free(body); snprintf(err, errsz, "the X server describes no screen"); return -1; }
            x->root = get32(body + off);
            x->screen_w = get16(body + off + 20);
            x->screen_h = get16(body + off + 22);
            x->root_visual = get32(body + off + 32);
            x->root_depth = body[off + 38];
            {
                unsigned ndepths = body[off + 39], doff = off + 40, d, v;
                unsigned red = 0, green = 0, blue = 0;
                for (d = 0; d < ndepths && doff + 8 <= add; d++) {
                    unsigned depth = body[doff], nvis = get16(body + doff + 2);
                    doff += 8;
                    for (v = 0; v < nvis && doff + 24 <= add; v++, doff += 24) {
                        if (get32(body + doff) == x->root_visual && depth == x->root_depth) {
                            red = get32(body + doff + 8); green = get32(body + doff + 12); blue = get32(body + doff + 16);
                        }
                    }
                }
                for (i = 0; i < nfmt; i++) {
                    const unsigned char *f = body + fmt_off + i * 8;
                    if (f[0] == x->root_depth && f[1] != 32) {
                        snprintf(err, errsz, "the screen uses %u bits per pixel at depth %u; only 32 is supported", f[1], f[0]);
                        free(body);
                        return -1;
                    }
                }
                if (red != 0xff0000 || green != 0x00ff00 || blue != 0x0000ff || image_order != 0) {
                    snprintf(err, errsz, "the screen's pixel layout (red %06x green %06x blue %06x, byte order %u) is not the usual 24-bit one", red, green, blue, image_order);
                    free(body);
                    return -1;
                }
            }
        }
    }
    free(body);
    return 0;
}

/* Opens the socket to the server named by DISPLAY (the file in /tmp/.X11-unix, else the same name in the abstract socket namespace, else TCP). */
static int open_display_socket(const char *host, int num)
{
    char path[128];
    int fd;
    if (host[0] == 0 || strcmp(host, "unix") == 0) {
        snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", num);
        fd = connect_unix(path, 0);
        if (fd < 0) fd = connect_unix(path, 1);
        return fd;
    }
    return connect_tcp(host, 6000 + num);
}

static char sAuthSource[160];                    /* where the cookie that worked came from (for the log) */

/* Connects and runs the set-up. The cookie is looked for in $XAUTHORITY, ~/.Xauthority and the "-auth" file of the running server, in that
 * order; the last attempt is without a cookie (a server started with access control off). A refused set-up closes the connection, so each
 * attempt opens a new one. */
static int connect_and_setup(X11Win *x, char *err, unsigned errsz)
{
    const char *display = getenv("DISPLAY"), *home = getenv("HOME");
    char host[128], file[512], first_err[256] = "";
    unsigned char cookie[16];
    int num, attempt, tried = 0;
    if (!display || !*display) { snprintf(err, errsz, "DISPLAY is not set"); return -1; }
    if (parse_display(display, host, sizeof host, &num) != 0) { snprintf(err, errsz, "cannot understand DISPLAY=%s", display); return -1; }
    for (attempt = 0; attempt < 4; attempt++) {
        int have_cookie = 0, rc;
        const char *src = "no cookie";
        file[0] = 0;
        if (attempt == 0 && getenv("XAUTHORITY")) { snprintf(file, sizeof file, "%s", getenv("XAUTHORITY")); src = "$XAUTHORITY"; }
        else if (attempt == 1 && home) { snprintf(file, sizeof file, "%s/.Xauthority", home); src = "~/.Xauthority"; }
        else if (attempt == 2 && find_server_auth_file(file, sizeof file)) src = "the -auth file of the X server";
        if (attempt < 3) {
            if (!file[0] || !find_cookie_in(file, num, cookie)) continue;
            have_cookie = 1;
        }
        x->fd = open_display_socket(host, num);
        if (x->fd < 0) { snprintf(err, errsz, "cannot connect to the X server at DISPLAY=%s (%s)", display, strerror(errno)); return -1; }
        x->dead = 0;
        tried++;
        rc = do_setup(x, have_cookie ? cookie : NULL, err, errsz);
        if (rc == 0) {
            snprintf(sAuthSource, sizeof sAuthSource, "%s%s%s", src, have_cookie ? ": " : "", have_cookie ? file : "");
            return 0;
        }
        close(x->fd);
        x->fd = -1;
        if (!first_err[0]) snprintf(first_err, sizeof first_err, "%s", err);
        if (rc != -2) return -1;                 /* not an authorization problem: another cookie would not help */
    }
    if (!tried) snprintf(err, errsz, "no X authority cookie found (tried $XAUTHORITY, ~/.Xauthority and the X server's -auth file)");
    return -1;
}

const char *x11win_auth_source(void) { return sAuthSource; }

/* ---- the window ---- */

X11Win *x11win_open(const char *title, unsigned w, unsigned h, int fullscreen, char *err, unsigned errsz)
{
    X11Win *x = (X11Win *)calloc(1, sizeof *x);
    unsigned char body[64];
    if (!x) { snprintf(err, errsz, "out of memory"); return NULL; }
    x->fd = -1;
    x->transport = "PutImage";
    if (connect_and_setup(x, err, errsz) != 0) { free(x); return NULL; }
    x->cw = w; x->ch = h;
    x->win = new_id(x);
    x->gc = new_id(x);

    x->a_wm_protocols = intern_atom(x, "WM_PROTOCOLS");
    x->a_wm_delete = intern_atom(x, "WM_DELETE_WINDOW");
    x->a_net_wm_state = intern_atom(x, "_NET_WM_STATE");
    x->a_net_wm_state_fullscreen = intern_atom(x, "_NET_WM_STATE_FULLSCREEN");
    x->a_net_wm_name = intern_atom(x, "_NET_WM_NAME");
    x->a_utf8 = intern_atom(x, "UTF8_STRING");
    if (x->dead) { snprintf(err, errsz, "the X server stopped answering"); close(x->fd); free(x); return NULL; }

    /* CreateWindow: depth, window id, parent = root, x y, width height, border width, class InputOutput(1), visual, value mask, values.
     * Value mask 0x800 = event mask: Exposure(0x8000) | StructureNotify(0x20000) | FocusChange(0x200000) | PropertyChange(0x400000).
     * No background is given on purpose (None): the server does not clear the window when it is uncovered, so a repaint does not flash. */
    memset(body, 0, sizeof body);
    put32(body, x->win);
    put32(body + 4, x->root);
    put16(body + 8, 0); put16(body + 10, 0);
    put16(body + 12, w); put16(body + 14, h);
    put16(body + 16, 0);
    put16(body + 18, 1);
    put32(body + 20, x->root_visual);
    put32(body + 24, 0x800);
    put32(body + 28, 0x8000u | 0x20000u | 0x200000u | 0x400000u);
    request(x, 1, x->root_depth, body, 32);

    x11win_set_title(x, title);
    {   /* WM_PROTOCOLS = [WM_DELETE_WINDOW]: the manager sends a message instead of killing the connection when the window is closed */
        unsigned char v[4];
        put32(v, x->a_wm_delete);
        set_property(x, x->a_wm_protocols, 4 /* ATOM */, 32, v, 1);
    }
    {   /* WM_HINTS: input = true (flags 1 = InputHint), so that the manager gives the window the keyboard focus */
        unsigned char hints[36];
        memset(hints, 0, sizeof hints);
        put32(hints, 1);
        put32(hints + 4, 1);
        set_property(x, 35 /* WM_HINTS */, 35 /* WM_HINTS */, 32, hints, 9);
    }
    {   /* WM_CLASS: instance and class name, two NUL-terminated strings */
        static const char cls[] = "kyujo\0Kyujo";
        set_property(x, 67 /* WM_CLASS */, 31 /* STRING */, 8, cls, sizeof cls);
    }
    if (fullscreen && x->a_net_wm_state && x->a_net_wm_state_fullscreen) {
        unsigned char v[4];
        put32(v, x->a_net_wm_state_fullscreen);
        set_property(x, x->a_net_wm_state, 4, 32, v, 1);          /* the initial state, read by the manager when the window is first shown */
    }
    /* a graphics context to draw with; GraphicsExposures off (bit 0x10000, value 0) so that drawing never generates exposure events */
    put32(body, x->gc);
    put32(body + 4, x->win);
    put32(body + 8, 0x10000);
    put32(body + 12, 0);
    request(x, 55, 0, body, 16);
    /* an invisible mouse pointer for full screen: a 1x1 bitmap that is all zero used as both the shape and the mask */
    {
        uint32_t pix = new_id(x), cgc = new_id(x);
        put32(body, pix); put32(body + 4, x->win); put16(body + 8, 1); put16(body + 10, 1);
        request(x, 53, 1, body, 12);                                   /* CreatePixmap depth 1 */
        put32(body, cgc); put32(body + 4, pix); put32(body + 8, 0);
        request(x, 55, 0, body, 12);                                   /* a GC for it (foreground 0 by default) */
        put32(body, pix); put32(body + 4, cgc); put16(body + 8, 0); put16(body + 10, 0); put16(body + 12, 1); put16(body + 14, 1);
        request(x, 70, 0, body, 16);                                   /* PolyFillRectangle: make the pixel 0 */
        x->blank_cursor = new_id(x);
        memset(body, 0, sizeof body);
        put32(body, x->blank_cursor); put32(body + 4, pix); put32(body + 8, pix);   /* colours (fore/back r g b) and hot spot stay 0 */
        request(x, 93, 0, body, 28);                                   /* CreatePixmapCursor: id, source, mask, 2 x (r g b), hot spot x y */
        put32(body, cgc); request(x, 60, 0, body, 4);                  /* FreeGC */
        put32(body, pix); request(x, 54, 0, body, 4);                  /* FreePixmap (the cursor keeps its own copy) */
    }
    load_keymap(x);
    put32(body, x->win);
    request(x, 8, 0, body, 4);                                          /* MapWindow */
    flush_out(x);
    /* wait (up to 2 s) for the window to be shown, so that the first picture is not lost */
    {
        int i;
        for (i = 0; i < 200 && !x->mapped && !x->dead; i++) {
            struct pollfd pf = {x->fd, POLLIN, 0};
            if (poll(&pf, 1, 10) > 0) x11win_pump(x);
        }
    }
    if (x->dead) { snprintf(err, errsz, "the X server closed the connection"); close(x->fd); free(x); return NULL; }
    if (!x->mapped) fprintf(stderr, "kyujo: x11: the window was not reported as shown after 2 s (is there a window manager?); going on\n");
    return x;
}

void x11win_close(X11Win *x)
{
    if (!x) return;
    if (x->fd >= 0) close(x->fd);
    free(x);
}

int x11win_fd(const X11Win *x) { return x ? x->fd : -1; }

int x11win_pump(X11Win *x)
{
    int bits = 0;
    if (!x || x->dead) return X11W_CLOSE;
    flush_out(x);
    for (;;) {
        struct pollfd pf = {x->fd, POLLIN, 0};
        unsigned char p[32];
        if (poll(&pf, 1, 0) <= 0) break;
        if (read_all(x, p, 32) != 0) break;
        if (p[0] == 1) {                                  /* a reply nobody waits for */
            unsigned more = get32(p + 4) * 4u;
            unsigned char tmp[256];
            while (more) { unsigned n = more > sizeof tmp ? (unsigned)sizeof tmp : more; if (read_all(x, tmp, n)) break; more -= n; }
            continue;
        }
        if ((p[0] & 0x7f) == 35) {
            unsigned more = get32(p + 4) * 4u;
            unsigned char tmp[256];
            while (more) { unsigned n = more > sizeof tmp ? (unsigned)sizeof tmp : more; if (read_all(x, tmp, n)) break; more -= n; }
        }
        handle_event(x, p);
        if (x->mapped && !x->focus_taken) {               /* once visible, ask for the keyboard focus (a manager that does not do it by itself) */
            unsigned char b[8];
            x->focus_taken = 1;
            put32(b, x->win);
            put32(b + 4, 0);                              /* revert-to is in the data byte; time 0 = CurrentTime */
            request(x, 42, 2 /* RevertToParent */, b, 8);
            flush_out(x);
        }
    }
    flush_out(x);
    if (x->state_changed && !x->dead) { x->state_changed = 0; read_fullscreen_state(x); }
    if (x->dead || x->want_close) bits |= X11W_CLOSE;
    if (x->redraw) { bits |= X11W_REDRAW; x->redraw = 0; }
    return bits;
}

void x11win_size(const X11Win *x, unsigned *w, unsigned *h) { *w = x ? x->cw : 0; *h = x ? x->ch : 0; }
int x11win_has_focus(const X11Win *x) { return x && x->focused; }
int x11win_is_fullscreen(const X11Win *x) { return x && x->fullscreen; }
unsigned x11win_screen_width(const X11Win *x) { return x ? x->screen_w : 0; }
unsigned x11win_screen_height(const X11Win *x) { return x ? x->screen_h : 0; }
const char *x11win_transport(const X11Win *x) { return x ? x->transport : ""; }

void x11win_set_title(X11Win *x, const char *title)
{
    if (!x || x->dead) return;
    set_property(x, 39 /* WM_NAME */, 31 /* STRING */, 8, title, (unsigned)strlen(title));
    if (x->a_net_wm_name && x->a_utf8) set_property(x, x->a_net_wm_name, x->a_utf8, 8, title, (unsigned)strlen(title));
    flush_out(x);
}

void x11win_set_fullscreen(X11Win *x, int on)
{
    unsigned char ev[32 + 12];
    if (!x || x->dead || !x->a_net_wm_state || !x->a_net_wm_state_fullscreen) return;
    /* A ClientMessage sent to the root window, which is how a window asks its manager to change _NET_WM_STATE (EWMH): data = action (0 remove,
     * 1 add), the state, a second state (none), source indication (1 = a normal application). */
    memset(ev, 0, sizeof ev);
    put32(ev, x->root);                                   /* SendEvent body: destination */
    put32(ev + 4, 0x100000u | 0x80000u);                  /* event mask: SubstructureRedirect | SubstructureNotify */
    ev[8] = 33;                                           /* ClientMessage */
    ev[9] = 32;                                           /* format */
    put32(ev + 12, x->win);
    put32(ev + 16, x->a_net_wm_state);
    put32(ev + 20, on ? 1u : 0u);
    put32(ev + 24, x->a_net_wm_state_fullscreen);
    put32(ev + 28, 0);
    put32(ev + 32, 1);
    request(x, 25, 0, ev, 40);
    flush_out(x);
}

void x11win_present(X11Win *x, const unsigned char *bgrx, unsigned w, unsigned h)
{
    unsigned row, rows_per, y;
    if (!x || x->dead || !w || !h) return;
    row = w * 4u;
    if (x->max_req_bytes <= 32 + row) return;
    rows_per = (x->max_req_bytes - 32) / row;
    for (y = 0; y < h; y += rows_per) {
        unsigned n = (h - y < rows_per) ? h - y : rows_per;
        unsigned char b[20];
        unsigned char hdr[4];
        /* PutImage: format ZPixmap(2) in the data byte; drawable, gc, width, height, dst x, dst y, left pad 0, depth, then the rows */
        put32(b, x->win);
        put32(b + 4, x->gc);
        put16(b + 8, w);
        put16(b + 10, n);
        put16(b + 12, 0);
        put16(b + 14, y);
        b[16] = 0;
        b[17] = (unsigned char)x->root_depth;
        b[18] = b[19] = 0;
        hdr[0] = 72; hdr[1] = 2;
        put16(hdr + 2, (4 + 20 + n * row) / 4);
        queue_out(x, hdr, 4);
        queue_out(x, b, 20);
        flush_out(x);
        write_all(x, bgrx + (size_t)y * row, (size_t)n * row);
        ++x->seq;
        if (x->dead) return;
    }
}

#endif /* __linux__ */
