/* Run the actual bare-metal ARM library under qemu-arm using Linux file syscalls.
 * Graphics calls are inert: this checks the nano allocator, math, Lua and physics,
 * not the iPod GL driver. Run from port/ so build/data-small-pages exists. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include "hb_shared.h"
#include "platform.h"
#include "gfx.h"
#include "../../nano/nano_perf.h"
#undef port_checkpoint
void port_checkpoint(const char *stage, const char *detail, uint32_t value) {
    if (!strcmp(stage, "level-ready")) port_perf_level_loaded();
    printf("CHECK %s %s %u\n", stage, detail, (unsigned)value);
}

hb_shared_t shared;
hb_shared_t *hb_shared = &shared;
#ifndef TEST_HEAP_BYTES
#define TEST_HEAP_BYTES (16 * 1024 * 1024)
#endif
#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "build/data-small-pages"
#endif
static uint64_t heap[TEST_HEAP_BYTES / 8];
static uint32_t heap_used;
static uint64_t now;
extern void port_log_flush(const char *path);
extern uint32_t port_arena_largest_free(void);
extern uint32_t port_arena_size(void);
extern void *memalign(size_t align, size_t size);

static int sys3(int nr, int a, int b, int c) {
    register int r0 __asm__("r0") = a;
    register int r1 __asm__("r1") = b;
    register int r2 __asm__("r2") = c;
    register int r7 __asm__("r7") = nr;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

void port_fatal(int code) { port_log_flush("fatal"); sys3(1, code, 0, 0); for (;;) {} }
uint32_t hb_os_heap_free(void) { return sizeof(heap) - heap_used; }
uint32_t hb_os_heap_largest(void) { return hb_os_heap_free(); }
void *hb_os_alloc(uint32_t n) {
    n = (n + 63u) & ~63u;
    if (n > hb_os_heap_free()) return NULL;
    void *p = (char *)heap + heap_used;
    heap_used += n;
    return p;
}
void hb_os_free(void *p) { (void)p; }
void port_crumb(const char *tag, uint32_t a, uint32_t b) { (void)tag; (void)a; (void)b; }
bool hb_fs_mkdir(const char *path) { (void)path; return true; }
bool hb_fs_write(const char *path, const void *p, uint32_t n) {
    (void)path;
    return sys3(4, 1, (int)p, n) == (int)n;
}

uint64_t plat_time_us(void) { return now; }
void *plat_read_file(const char *name, uint32_t *size, int save) {
    char path[160];
    if (save) return NULL;
    snprintf(path, sizeof path, "%s/%s", TEST_DATA_DIR, name);
    int fd = sys3(5, (int)path, 0, 0);
    if (fd < 0) return NULL;
    int n = sys3(19, fd, 0, 2);
    sys3(19, fd, 0, 0);
    /* Match the device's aligned, over-sized whole-file read allocation. */
    uint32_t cap = (((uint32_t)n + 1u + 4095u) & ~4095u) + 4096u;
    char *p = n >= 0 ? memalign(64, cap) : NULL;
    if (p && sys3(3, fd, (int)p, n) != n) { free(p); p = NULL; }
    sys3(6, fd, 0, 0);
    if (p) { p[n] = 0; if (size) *size = n; }
    return p;
}
int plat_write_file(const char *path, const void *p, uint32_t n) {
    (void)p; printf("SAVE %s %u bytes\n", path, n); return 0;
}
void plat_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}
float plat_audio_play(const char *s, float v, int l, int t) { return 0; }
void plat_audio_stop(const char *s) {}
void plat_audio_track_volume(int t, float v) {}
void plat_audio_clip_volume(const char *s, float v) {}

struct Event { int frame, down, x, y; };
#include "events.h"

int main(void) {
    printf("nano ARM test start; heap %u\n", hb_os_heap_free());
    if (game_init(576, 320)) return 2;
    printf("arena %u bytes\n", port_arena_size());
    PlatTouches touch = {0};
    for (int frame = 0; frame < 2300; frame++) {
        now += 33333;
        for (unsigned i = 0; i < sizeof(events)/sizeof(events[0]); i++) {
            if (events[i].frame == frame) {
                touch.count = events[i].down;
                touch.pt[0].x = events[i].x;
                touch.pt[0].y = events[i].y;
            }
        }
        gfx_begin_frame(240, 432, 576, 320, 1, 0, 0, 0);
        game_frame(1.f/30.f, &touch);
        port_perf_draw(576, 320);
        gfx_end_frame();
        port_perf_frame(33333, 0, 0, 0, 0, 0, 0); /* simulated cadence, never a device benchmark */
        if (frame % 100 == 0) {
            printf("frame %d, Lua %d KiB, texture %u KiB, arena largest %u\n", frame,
                   game_lua_kb(), gfx_stat_tex_bytes/1024, port_arena_largest_free());
            game_debug_dump("score,levelCompleted,currentBirdName");
        }
    }
    port_log_flush("test");
    return 0;
}

__asm__(".text\n.global _start\n_start:\n bl main\n mov r7,#1\n svc 0\n");
