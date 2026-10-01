#include <string.h>
/*
 * ab_entry.c — GL surface entry for the Angry Birds port (Thumb, like every NanoApp).
 *
 * The game itself (the engine re-implementation, Lua and Box2D) is built separately into
 * libab_nano.a in ARM state with hardware float; see AB_DIR in the Makefile. This file is
 * the Mario Kart 64 port's entry with the game-specific parts replaced.
 *
 * Every launch is a fresh start (Home quits the game): the resident frees the app's aux
 * block (our malloc arena) on every launch, so nothing from the last run may survive.
 * .bss is zeroed, and on a relaunch of a still-loaded image .data is restored from the
 * app's own file.
 */
#include "hb_sdk.h"
#include "hb_shared.h"
#include "hb_heap.h"
#include "gl.h"

extern void ab_nano_frame(int w, int h, uint32_t frame);
extern uint32_t port_perf_entry_us;

typedef void (*gl_frame_fn)(int w, int h, uint32_t frame);

/* Breadcrumbs: a small RAM ring tagged with a magic. RAM survives a panic reboot, so the
 * next launch finds the previous run's ring and saves it as prevboot.txt. Written to disk
 * only on request (never per frame: that corrupted the nano's FAT across crash-reboots). */
#define CRUMB_MAGIC0 0x41424952u   /* 'ABIR' */
#define CRUMB_MAGIC1 0x43524D42u   /* 'CRMB' */
#define DATA_DIR "/Apps/Data/AngryBirds"
typedef struct { uint32_t magic0, magic1, len, seq; char text[8176]; } crumb_ring_t;
static crumb_ring_t *s_ring;

void port_crumb(const char *tag, uint32_t a, uint32_t b)
{
    static const char hex[] = "0123456789abcdef";
    static uint32_t s_t0;
    char line[40];
    int n = 0, i;
    if (!s_t0) s_t0 = hb_time_uptime_us();
    if (b == 0) b = (hb_time_uptime_us() - s_t0) / 1000u;   /* ms since the first crumb */
    while (*tag && n < 12) line[n++] = *tag++;
    line[n++] = ' ';
    for (i = 28; i >= 0; i -= 4) line[n++] = hex[(a >> i) & 15];
    line[n++] = ' ';
    for (i = 28; i >= 0; i -= 4) line[n++] = hex[(b >> i) & 15];
    line[n++] = '\n';
    if (!s_ring) {
        extern void *malloc(unsigned int);   /* the game's arena (freed with the app) */
        s_ring = (crumb_ring_t *)malloc(sizeof(crumb_ring_t));
        if (!s_ring) return;
        s_ring->magic0 = CRUMB_MAGIC0;
        s_ring->magic1 = CRUMB_MAGIC1;
        s_ring->len = 0;
        s_ring->seq = hb_time_uptime_us();
    }
    if (s_ring->len > sizeof(s_ring->text)) s_ring->len = 0;   /* corrupted */
    if (s_ring->len + (uint32_t)n > sizeof(s_ring->text)) {
        /* full: drop the oldest half (at a line start) so the newest crumbs survive */
        uint32_t cut = s_ring->len / 2;
        while (cut < s_ring->len && s_ring->text[cut - 1] != '\n') cut++;
        memmove(s_ring->text, s_ring->text + cut, s_ring->len - cut);
        s_ring->len -= cut;
    }
    for (i = 0; i < n; i++) s_ring->text[s_ring->len++] = line[i];
    __asm__ volatile("dsb" ::: "memory");
}

void port_crumb_flush(void)
{
    if (!s_ring) return;
    hb_fs_mkdir("/Apps/Data");
    hb_fs_mkdir(DATA_DIR);
    hb_fs_write(DATA_DIR "/boot.txt", s_ring->text, s_ring->len);
}

/* Previous run's ring: scan the heap region for the magic (16-byte aligned blocks). */
static void save_previous_crumbs(void)
{
    uint32_t a;
    for (a = 0x08800000u; a < 0x0B800000u; a += 16u) {
        const crumb_ring_t *r = (const crumb_ring_t *)(uintptr_t)a;
        if (r->magic0 == CRUMB_MAGIC0 && r->magic1 == CRUMB_MAGIC1 && r != s_ring &&
            r->len <= sizeof(r->text)) {
            hb_fs_mkdir("/Apps/Data");
            hb_fs_mkdir(DATA_DIR);
            hb_fs_write(DATA_DIR "/prevboot.txt", r->text, r->len);
            ((crumb_ring_t *)r)->magic0 = 0;   /* consumed */
            return;
        }
    }
}

#define APP_FILE "/Apps/Executables/Angry Birds.hbapp"
static uint32_t s_ran_before __attribute__((section(".noinit")));

void *payload_entry(int op, void *fb, int w, int h, hb_shared_t *sh);

/* Relaunch of a still-loaded image: put .data back as the loader left it (the original
 * bytes from the app's file, with the relocations inside .data re-applied). */
static int restore_data_from_file(void)
{
    extern uint8_t __data_start[], _edata[];
    uint32_t size = hb_fs_size(APP_FILE), cap, i;
    uint8_t *buf;
    const uint32_t *hdr;
    int ok = 0;
    if (size < 24u)
        return 0;
    cap = (size + 4096u + 4095u) & ~4095u;   /* ask for more than the file holds */
    buf = (uint8_t *)hb_os_alloc(cap);
    if (!buf)
        return 0;
    if (hb_fs_read(APP_FILE, buf, cap) >= 24u) {
        hdr = (const uint32_t *)buf;          /* magic, entry, image, span, nreloc, align */
        uint8_t *base = (uint8_t *)(((uintptr_t)payload_entry & ~(uintptr_t)1) - hdr[1]);
        uint32_t d0 = (uint32_t)(__data_start - base), d1 = (uint32_t)(_edata - base);
        if (hdr[0] == 0x314c5248u && d0 <= d1 && d1 <= hdr[2] &&
            24u + hdr[2] + hdr[4] * 4u <= size && ((uintptr_t)base & 3u) == 0) {
            const uint32_t *rel = (const uint32_t *)(buf + 24u + hdr[2]);
            memcpy(__data_start, buf + 24u + d0, d1 - d0);
            for (i = 0; i < hdr[4]; i++)
                if (rel[i] >= d0 && rel[i] + 4u <= d1)
                    *(uint32_t *)(base + rel[i]) += (uint32_t)(uintptr_t)base;
            ok = 1;
        }
    }
    hb_os_free(buf);
    return ok;
}

/* The game is ARM code using the hardware FPU. The iPod OS enables VFP per task and
 * reboots on a VFP instruction from a task without an FP context, so attach (or reuse) the
 * current task's context here, in Thumb soft-float code, before any game code runs.
 * Idempotent and cheap, so done every frame. */
static void ab_frame(int w, int h, uint32_t frame)
{
    static uint32_t s_logged;
    uint32_t entry_start = hb_time_uptime_us();
    hb_fpu_status_t st;
    int r = hb_fpu_attach_current_task(&st);
    if (s_logged < 1 || r < 0) {
        s_logged++;
        port_crumb("fpu", (uint32_t)r, st.task);
        if (r < 0) port_crumb_flush();
    }
    if (r < 0)
        return;          /* no FP context: never run float code */
    port_perf_entry_us = hb_time_uptime_us() - entry_start;
    ab_nano_frame(w, h, frame);
}

__attribute__((section(".text.entry"), used, noinline))
void *payload_entry(int op, void *fb, int w, int h, hb_shared_t *sh)
{
    unsigned int *p;
    extern unsigned int __bss_start__[], __bss_end__[];
    (void)fb; (void)w; (void)h;
    if (op != 0 || !sh || sh->magic != HB_SHARED_MAGIC)
        return 0;
    if (s_ran_before && !restore_data_from_file())
        return 0;                   /* can't restore: refuse rather than run half-reset */
    s_ran_before = 1;
    for (p = __bss_start__; p < __bss_end__; p++)
        *p = 0;
    hb_shared = sh;
    save_previous_crumbs();
    hb_trace_log("ABENT   ", s_ran_before, (uint32_t)(uintptr_t)__bss_end__);
    port_crumb("entry", hb_os_heap_free(), hb_os_heap_largest());
    return (void *)(gl_frame_fn)ab_frame;
}
