/* nano_libc.c — the C library pieces the Angry Birds port needs on the nano, over the SDK
 * (adapted from the Mario Kart 64 port's file of the same name).
 *
 * newlib (ARM-mode softfp multilib) supplies string/ctype/setjmp; this file replaces the
 * parts that would drag in newlib's reentrant stdio and sbrk heap:
 *   - malloc family in one arena the resident frees on unload (see below);
 *   - printf family formatting into a log ring written to /Apps/Data/AngryBirds/log.txt;
 *   - the few stdio entry points Lua's libraries reference (never used for real files:
 *     the port loads everything through plat_read_file). */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hb_sdk.h"
#include "hb_heap.h"

/* newlib exposes these as macros as well as functions. */
#undef ferror
#undef feof

#define AB_DIR "/Apps/Data/AngryBirds"

/* ---- heap ---- */
/* All of the app's allocations come from ONE OS block registered as the app's aux block
 * (hb_heap.h): the resident frees that block when it unloads the app (switching to
 * another app), and nothing else. Separate hb_os_alloc blocks outlived the app and left
 * the next one (Mario 64) short of memory. Exhaustion returns NULL; it must not consume
 * the GL reserve or leave untracked allocations behind when the app unloads. */
#ifndef PORT_ARENA_SIZE
#define PORT_ARENA_SIZE 0x400000u   /* Lua + physics + sheet indexes + temporary ground-mask pages */
#endif
#define PORT_ARENA_MIN 0x200000u
#define GL_RESERVE     0x600000u   /* OS heap left for the GL driver and the game's textures (budget in nano_platform.c) */
typedef struct { uint32_t size, used, pad[2]; } ArenaBlk;   /* 16 bytes; size includes it */
static uint8_t *sArena;
static uint32_t sArenaSize, sHeapFreeAtInit;

static void arena_init(void) {
    if (sArena) return;
    if (HB_APP_AUX_BLOCK) {             /* shouldn't happen: the resident clears it */
        hb_os_free(HB_APP_AUX_BLOCK);
        HB_APP_AUX_BLOCK = NULL;
    }
    /* Leave GL_RESERVE of the OS heap for the GL driver, which also holds the textures
     * (the driver reboots the iPod when it runs out). Below PORT_ARENA_MIN the game can't
     * fit anyway, so take at least that. */
    uint32_t size = PORT_ARENA_SIZE, freeb = hb_os_heap_free();
    if (freeb > GL_RESERVE + PORT_ARENA_MIN && freeb - GL_RESERVE < size)
        size = (freeb - GL_RESERVE) & ~0xFFFFu;
    else if (freeb <= GL_RESERVE + PORT_ARENA_MIN)
        size = PORT_ARENA_MIN;
    while (!(sArena = hb_os_alloc(size)) && size > 0x100000u)
        size -= 0x80000u;
    if (!sArena) return;
    sArenaSize = size & ~15u;
    HB_APP_AUX_BLOCK = sArena;
    ArenaBlk *b = (ArenaBlk *)sArena;
    b->size = sArenaSize;
    b->used = 0;
    sHeapFreeAtInit = freeb;   /* logged later: no logging from inside the allocator */
}

uint32_t port_arena_size(void) { return sArenaSize; }
uint32_t port_heap_free_at_arena_init(void) { return sHeapFreeAtInit; }

static void *arena_alloc(uint32_t n) {
    arena_init();
    if (!sArena) return NULL;
    n = ((n + 15u) & ~15u) + sizeof(ArenaBlk);
    uint32_t off = 0;
    while (off < sArenaSize) {
        ArenaBlk *b = (ArenaBlk *)(sArena + off);
        if (!b->used) {
            /* merge the free blocks that follow (frees don't coalesce) */
            while (off + b->size < sArenaSize) {
                ArenaBlk *nb = (ArenaBlk *)(sArena + off + b->size);
                if (nb->used) break;
                b->size += nb->size;
            }
            if (b->size >= n) {
                if (b->size - n >= 64) {
                    ArenaBlk *rest = (ArenaBlk *)(sArena + off + n);
                    rest->size = b->size - n;
                    rest->used = 0;
                    b->size = n;
                }
                b->used = 1;
                return b + 1;
            }
        }
        off += b->size;
    }
    return NULL;
}

/* Largest free run in the arena (coalescing as it goes), for sizing caches. */
uint32_t port_arena_largest_free(void) {
    uint32_t off = 0, best = 0;
    if (!sArena) return 0;
    while (off < sArenaSize) {
        ArenaBlk *b = (ArenaBlk *)(sArena + off);
        if (!b->used) {
            while (off + b->size < sArenaSize) {
                ArenaBlk *nb = (ArenaBlk *)(sArena + off + b->size);
                if (nb->used) break;
                b->size += nb->size;
            }
            if (b->size > best) best = b->size;
        }
        off += b->size;
    }
    return best;
}

static int in_arena(const void *p) {
    return sArena && (const uint8_t *)p >= sArena && (const uint8_t *)p < sArena + sArenaSize;
}

typedef struct { void *base; uint32_t size; uint32_t pad[2]; } AllocHdr;   /* 16 bytes */

void *memalign(size_t align, size_t size) {
    if (align < 16) align = 16;
    uint32_t n = size + align + sizeof(AllocHdr);
    uint8_t *raw = arena_alloc(n);
    if (!raw) {
        printf("OOM: %u bytes\n", (unsigned)size);
        return NULL;
    }
    uintptr_t p = ((uintptr_t)raw + sizeof(AllocHdr) + align - 1) & ~(uintptr_t)(align - 1);
    AllocHdr *h = (AllocHdr *)p - 1;
    h->base = raw;
    h->size = size;
    return (void *)p;
}
void *malloc(size_t size) { return memalign(16, size); }
void free(void *p) {
    if (!p) return;
    void *base = ((AllocHdr *)p - 1)->base;
    if (in_arena(base)) ((ArenaBlk *)base - 1)->used = 0;
    else hb_os_free(base);
}
void *calloc(size_t n, size_t sz) {
    void *p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}
void *realloc(void *p, size_t size) {
    if (!p) return malloc(size);
    void *q = malloc(size);
    if (q) {
        uint32_t old = ((AllocHdr *)p - 1)->size;
        memcpy(q, p, old < size ? old : size);
        free(p);
    }
    return q;
}

/* ---- log ring + printf ---- */
#define LOG_SIZE 8192
static char sLog[LOG_SIZE];
static uint32_t sLogHead;   /* total bytes ever written */

static void log_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        sLog[(sLogHead++) % LOG_SIZE] = s[i];
}

void port_log_flush(const char *path) {
    uint32_t n = sLogHead < LOG_SIZE ? sLogHead : LOG_SIZE;
    if (!n) return;
    /* As for reads, use a heap buffer aligned for the OS storage path. An
     * aligned declaration in a relocatable image is only as aligned as its base. */
    char *out = memalign(64, (n + 4095u) & ~4095u);
    if (!out) return;
    uint32_t start = sLogHead - n;
    for (uint32_t i = 0; i < n; i++)
        out[i] = sLog[(start + i) % LOG_SIZE];
    hb_fs_mkdir("/Apps/Data");
    hb_fs_mkdir(AB_DIR);
    hb_fs_write(path, out, n);
    free(out);
}

static void put_u(char **o, char *end, unsigned long long v, int base, int upper, int width, char padc, int neg) {
    char t[24];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { t[n++] = dig[v % base]; v /= base; } while (v);
    if (neg) t[n++] = '-';
    if (neg && padc == '0') { n--; if (*o < end) *(*o)++ = '-'; width--; }
    while (width-- > n) if (*o < end) *(*o)++ = padc;
    while (n) if (*o < end) *(*o)++ = t[--n]; else n--;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    char *o = buf, *end = buf + (size ? size - 1 : 0);
    size_t total = 0;
    char tmp[64];
    for (; *fmt; fmt++) {
        if (*fmt != '%') { if (o < end) *o++ = *fmt; total++; continue; }
        fmt++;
        char padc = ' ';
        int width = 0, prec = -1, lng = 0, left = 0;
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' || *fmt == '#') {
            if (*fmt == '-') left = 1;
            if (*fmt == '0') padc = '0';
            fmt++;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0; fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; fmt++; }
        char *t = tmp, *tend = tmp + sizeof(tmp) - 1;
        switch (*fmt) {
            case 'd': case 'i': {
                long long v = lng > 1 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
                put_u(&t, tend, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, 0, left ? 0 : width, padc, v < 0);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                unsigned long long v = lng > 1 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                put_u(&t, tend, v, *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16, *fmt == 'X', left ? 0 : width, padc, 0);
                break;
            }
            case 'p':
                put_u(&t, tend, (uintptr_t)va_arg(ap, void *), 16, 0, 8, '0', 0);
                break;
            case 'c':
                *t++ = (char)va_arg(ap, int);
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int n = (int)strlen(s);
                if (prec >= 0 && n > prec) n = prec;
                if (!left) for (int k = n; k < width; k++) { if (o < end) *o++ = ' '; total++; }
                for (int k = 0; k < n; k++) { if (o < end) *o++ = s[k]; total++; }
                if (left) for (int k = n; k < width; k++) { if (o < end) *o++ = ' '; total++; }
                continue;
            }
            case 'f': case 'g': case 'e': {
                double v = va_arg(ap, double);
                int p = prec < 0 ? 3 : prec;
                int neg = v < 0;
                if (neg) v = -v;
                unsigned long long ip = (unsigned long long)v;
                double fr = v - (double)ip;
                put_u(&t, tend, ip, 10, 0, 0, ' ', neg);
                if (p) {
                    *t++ = '.';
                    for (int k = 0; k < p && t < tend; k++) { fr *= 10; int d = (int)fr; *t++ = (char)('0' + d); fr -= d; }
                }
                break;
            }
            case '%': *t++ = '%'; break;
            default: *t++ = '?'; break;
        }
        int n = (int)(t - tmp);
        if (!left) for (int k = n; k < width && *fmt != 'd' && *fmt != 'i' && *fmt != 'u' && *fmt != 'x' && *fmt != 'X'; k++) { if (o < end) *o++ = ' '; total++; }
        for (int k = 0; k < n; k++) { if (o < end) *o++ = tmp[k]; total++; }
        if (left) for (int k = n; k < width; k++) { if (o < end) *o++ = ' '; total++; }
    }
    if (size) *o = 0;
    return (int)total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, size, fmt, ap); va_end(ap); return n;
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, 0x7fffffff, fmt, ap); va_end(ap); return n;
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }
/* While set (first launch, until the game has drawn a few frames), every log line is
 * written to disk immediately so a crash-reboot still leaves the log. */
int port_log_sync = 0;
int vprintf(const char *fmt, va_list ap) {
    char b[256];
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    log_write(b, strlen(b));
    if (port_log_sync)
        port_log_flush(AB_DIR "/log.txt");
    return n;
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); va_end(ap); return n;
}
int puts(const char *s) { log_write(s, strlen(s)); log_write("\n", 1); return 0; }
int putchar(int c) { char ch = (char)c; log_write(&ch, 1); return c; }
void perror(const char *s) { printf("perror: %s\n", s); }
int fprintf(FILE *f, const char *fmt, ...) {
    (void)f; va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); va_end(ap); return n;
}

/* ---- safe whole-file reads ----
 * On hardware, an OS file read that asks for exactly the file's length crashed the nano
 * when the destination was in the app image (3/3 runs); asking for more than the file
 * holds never did. So reads always ask for a 4 KB-rounded length, into a buffer that can
 * take it: the caller's if it has room, else an aligned bounce buffer. */
uint32_t port_read_file(const char *path, void *dst, uint32_t size, uint32_t capacity) {
    uint32_t want = (size + 4095u) & ~4095u;
    if (capacity >= want && ((uintptr_t)dst & 63u) == 0)
        return hb_fs_read(path, dst, want);
    uint8_t *b = memalign(64, want);
    if (!b) return 0;
    uint32_t got = hb_fs_read(path, b, want);
    memcpy(dst, b, got < size ? got : size);
    free(b);
    return got < size ? got : size;
}

/* ---- stdio stubs ----
 * Lua's auxiliary and base libraries reference these (luaL_loadfile, print, the default
 * panic handler); the game never calls them with a real file. */
FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
FILE *freopen(const char *path, const char *mode, FILE *f) { (void)path; (void)mode; (void)f; return NULL; }
int fclose(FILE *f) { (void)f; return 0; }
size_t fread(void *b, size_t sz, size_t n, FILE *f) { (void)b; (void)sz; (void)n; (void)f; return 0; }
int getc(FILE *f) { (void)f; return EOF; }
int ungetc(int c, FILE *f) { (void)f; return c; }
int ferror(FILE *f) { (void)f; return 0; }
int feof(FILE *f) { (void)f; return 1; }
int fflush(FILE *f) { (void)f; return 0; }
int fputs(const char *s, FILE *f) { (void)f; log_write(s, strlen(s)); return 0; }
int fputc(int c, FILE *f) { char ch = (char)c; (void)f; log_write(&ch, 1); return c; }
size_t fwrite(const void *b, size_t sz, size_t n, FILE *f) { (void)f; log_write((const char *)b, sz * n); return n; }
char *strerror(int e) { (void)e; return (char *)"error"; }

/* ---- errno / reent ----
 * newlib's libm reports errors through __errno(), and a few libc helpers use _REENT.
 * Defining both here keeps newlib's impure.o, and with it its whole stdio, out of the
 * link. */
#include <reent.h>
static int sErrno;
int *__errno(void) { return &sErrno; }
static struct _reent sReent;
struct _reent *_impure_ptr = &sReent;

/* newlib's objects carry EABI unwind tables that reference these personality routines;
 * without them libgcc links its (Thumb, VFP-saving) exception unwinder, which the
 * no-Thumb-FP gate rightly rejects. Nothing throws, so they are never called. */
void __aeabi_unwind_cpp_pr0(void) { abort(); }
void __aeabi_unwind_cpp_pr1(void) { abort(); }
void __aeabi_unwind_cpp_pr2(void) { abort(); }

/* ---- misc ---- */
char *getenv(const char *name) { (void)name; return NULL; }
/* Fatal errors unwind to the frame driver (port_nano_frame), which stops the game and
 * shows a red screen; spinning here would freeze the iPod's UI task. */
extern void port_fatal(int code) __attribute__((noreturn));
void exit(int code) {
    printf("exit(%d)\n", code);
    port_log_flush(AB_DIR "/log.txt");
    port_fatal(code);
}
void abort(void) { exit(134); }
void __assert_func(const char *file, int line, const char *fn, const char *expr) {
    printf("assert %s:%d %s: %s\n", file, line, fn ? fn : "", expr);
    exit(1);
}
