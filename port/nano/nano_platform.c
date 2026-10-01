/* nano_platform.c — iPod nano 7G platform layer for the Angry Birds port (ARM state, VFP).
 *
 * Provides src/platform.h on top of the NanoApps SDK: time, files, touch, the landscape
 * frame, and the per-frame driver called from the app's GL surface entry
 * (nano/app/ab_entry.c, Thumb). Modelled on the Mario Kart 64 port's nano_platform.c. */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_surface_input.h"
#include "gl.h"
#include "../src/platform.h"
#include "../src/gfx.h"
#include "../src/assets.h"
#include "nano_perf.h"

#define AB_DIR   "/Apps/Data/AngryBirds"
#define LOG_PATH AB_DIR "/log.txt"

/* The 320-pixel-high game is drawn on the 240-pixel-high landscape panel: 0.75 panel
 * pixels per game pixel (textures may use a lower resolution), so the visible
 * game screen is 576 x 320. */
#define GAME_SCALE 0.75f

/* Textures live in the OS heap (the GL driver's), which must never run out: the driver
 * reboots the iPod instead of failing. */
#define TEXTURE_BUDGET (3u << 20)

extern void port_log_flush(const char *path);
extern void port_crumb(const char *tag, uint32_t a, uint32_t b);
extern void *memalign(size_t align, size_t size);
extern uint32_t port_arena_largest_free(void);
extern uint32_t port_arena_size(void);

static unsigned sCheckpoints, sLevelTraceFrames;
static uint32_t sIoUs;

void port_checkpoint(const char *stage, const char *detail, uint32_t value) {
    if (!strcmp(stage, "level-ready")) port_perf_level_loaded();
    if (!sCheckpoints) return;
    printf("CHECK %s %s value=%u heap=%u largest=%u arena=%u texture=%u\n",
           stage, detail, (unsigned)value, (unsigned)hb_os_heap_free(),
           (unsigned)hb_os_heap_largest(), (unsigned)port_arena_largest_free(),
           (unsigned)gfx_stat_tex_bytes);
    if (!strcmp(stage, "level-ready")) sLevelTraceFrames = 2;
    /* The next upload/draw checkpoint saves these too. Avoid three synced
     * writes per page in frames that load several images at once. */
    if (!strcmp(stage, "texture-read") || !strcmp(stage, "texture-ready")) return;
    sCheckpoints--;
    port_log_flush(LOG_PATH);  /* aligned buffer; SDK syncs and closes before returning */
}

/* ---- time: hb_time_uptime_us wraps every ~71 minutes; extend to 64 bits ---- */
uint64_t plat_time_us(void) {
    static uint32_t last;
    static uint64_t high;
    uint32_t now = hb_time_uptime_us();
    if (now < last) high += 1ull << 32;
    last = now;
    return high | now;
}

/* ---- files ----
 * The OS file read is only asked for 4 KB multiples into 64-byte-aligned buffers that can
 * take more than the file holds (the pattern proven on hardware by the MK64 port), and the
 * OS size query is avoided: data file sizes come from files.lst, written by the converter. */
#define MAX_FILES 200
typedef struct { char name[48]; uint32_t size; } FileEnt;
static FileEnt *sFiles;
static int sFileCount;

static void *read_capped(const char *full, uint32_t expect, uint32_t *got) {
    uint32_t cap = ((expect + 1u + 4095u) & ~4095u) + 4096u;
    uint8_t *b = memalign(64, cap);
    if (!b) return NULL;
    uint64_t start = plat_time_us();
    uint32_t n = hb_fs_read(full, b, cap);          /* always more than the file holds */
    sIoUs += (uint32_t)(plat_time_us() - start);
    if (!n) { free(b); return NULL; }
    if (n > expect) n = expect;
    b[n] = 0;
    *got = n;
    return b;
}

static void load_manifest(void) {
    uint32_t n = 0;
    char *text = read_capped(AB_DIR "/files.lst", 16384, &n), *p;
    sFiles = malloc(sizeof(FileEnt) * MAX_FILES);
    sFileCount = 0;
    if (!text || !sFiles) { printf("files.lst is missing\n"); free(text); return; }
    for (p = text; *p && sFileCount < MAX_FILES;) {
        FileEnt *e = &sFiles[sFileCount];
        int k = 0;
        uint32_t size = 0;
        while (*p && *p != ' ' && *p != '\n' && k < (int)sizeof(e->name) - 1) e->name[k++] = *p++;
        e->name[k] = 0;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') size = size * 10u + (uint32_t)(*p++ - '0');
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (k) { e->size = size; sFileCount++; }
    }
    free(text);
    printf("files.lst: %d files\n", sFileCount);
}

void *plat_read_file(const char *path, uint32_t *size, int save) {
    char full[96];
    uint32_t expect = 65536, got = 0;       /* saved tables are small */
    void *b;
    if (*path == '/') path++;
    snprintf(full, sizeof full, AB_DIR "/%s", path);
    if (!save) {
        int i;
        for (i = 0; i < sFileCount; i++)
            if (!strcmp(sFiles[i].name, path)) break;
        if (i == sFileCount) return NULL;
        expect = sFiles[i].size;
    }
    b = read_capped(full, expect, &got);
    if (b && size) *size = got;
    return b;
}

int plat_write_file(const char *path, const void *data, uint32_t size) {
    char full[96];
    if (*path == '/') path++;
    if (!size) return 0;
    if (size > UINT32_MAX - 4095u) return -1;
    void *aligned = memalign(64, (size + 4095u) & ~4095u);
    if (!aligned) return -1;
    memcpy(aligned, data, size);
    snprintf(full, sizeof full, AB_DIR "/%s", path);
    hb_fs_mkdir("/Apps/Data");
    hb_fs_mkdir(AB_DIR);
    uint64_t start = plat_time_us();
    int result = hb_fs_write(full, aligned, size) ? 0 : -1;
    sIoUs += (uint32_t)(plat_time_us() - start);
    free(aligned);
    return result;
}

void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- audio: not yet ---- */
float plat_audio_play(const char *file, float volume, int loop, int track) {
    (void)file; (void)volume; (void)loop; (void)track;
    return 0.f;
}
void plat_audio_stop(const char *file) { (void)file; }
void plat_audio_track_volume(int track, float volume) { (void)track; (void)volume; }
void plat_audio_clip_volume(const char *file, float volume) { (void)file; (void)volume; }

/* ---- input ----
 * The device is held landscape with the Home button on the right (the NanoApps default):
 * logical x runs down the panel, logical y from right to left. The primary finger comes
 * from the resident's latched mailbox (never misses a tap); a second finger, for pinch
 * zoom, from the OS lists, where status 1 means lifted. */
static int sPanelW = 240, sPanelH = 432;
static float sLogW, sLogH;

static void to_logical(int px, int py, float *lx, float *ly) {
    *lx = (float)py * sLogW / (float)sPanelH;
    *ly = (float)(sPanelW - 1 - px) * sLogH / (float)sPanelW;
}

static void read_touches(PlatTouches *t) {
    hb_spoint_t p;
    hb_touch_t fingers[HB_MAX_FINGERS];
    int n, i, best = -1, bestd = 40 * 40;
    t->count = 0;
    hb_surface_touch_read(&p);
    if (!p.down) return;
    to_logical(p.x, p.y, &t->pt[0].x, &t->pt[0].y);
    t->count = 1;
    hb_touch_drain_all();
    n = hb_touch_poll_multi(fingers);
    for (i = 0; i < n; i++) {
        int dx, dy;
        if (fingers[i].status == 1) continue;
        dx = (int)fingers[i].x - p.x;
        dy = (int)fingers[i].y - p.y;
        if (dx * dx + dy * dy > bestd) { bestd = dx * dx + dy * dy; best = i; }
    }
    if (best >= 0) {
        to_logical(fingers[best].x, fingers[best].y, &t->pt[1].x, &t->pt[1].y);
        t->count = 2;
    }
}

/* Keep the screen lit: the OS dims after ~20-40 s without touches and doesn't know a game
 * is running. Reset its idle clock like a touch every 10 s (firmware 39579dba addresses,
 * as the MK64 port does). */
static void keep_awake(void) {
    static uint64_t last;
    uint64_t now = plat_time_us();
    if (last && now - last < 10000000ull) return;
    last = now;
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(0x0842ae80u | 1u))();
    if (m) ((se_t)(0x084069d8u | 1u))(m, 4);
}

/* ---- frame driver ---- */
static int sState, sDead;        /* 0: first frame, 1: start the game, 2: running */
static jmp_buf sFatal;

void port_fatal(int code) {
    (void)code;
    longjmp(sFatal, 1);
}

void ab_nano_frame(int w, int h, uint32_t frame) {
    static uint64_t last;
    static uint32_t n;
    (void)frame;
    sPanelW = w;
    sPanelH = h;
    sLogW = (float)h / GAME_SCALE;
    sLogH = (float)w / GAME_SCALE;
    if (sDead || setjmp(sFatal)) {
        /* a fatal error: stop the game and show red rather than freeze the iPod's UI task */
        if (!sDead) port_log_flush(LOG_PATH);
        sDead = 1;
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.6f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    if (sState < 2) {
        /* Render something every frame (a GL view that goes quiet may be reset), and give
         * the UI task a frame before the slow start-up. */
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (sState == 1) {
            uint64_t t0 = plat_time_us();
            int rc;
            printf("Angry Birds start: panel %dx%d, heap free %u, largest %u\n", w, h,
                   (unsigned)hb_os_heap_free(), (unsigned)hb_os_heap_largest());
            printf("memory: arena %u, texture cache %u\n", (unsigned)port_arena_size(), TEXTURE_BUDGET);
            load_manifest();
            /* Opt-in trace for one launch, capped at 96 events. Consume the flag
             * before starting so subsequent normal play never writes these checkpoints. */
            if (hb_fs_exists(AB_DIR "/trace.once") && hb_fs_remove(AB_DIR "/trace.once"))
                sCheckpoints = 96;
            assets_set_texture_budget(TEXTURE_BUDGET);
            rc = game_init(sLogW, sLogH);
            printf("game_init: %d, %u ms, Lua heap %d KB, heap free %u\n", rc,
                   (unsigned)((plat_time_us() - t0) / 1000u), game_lua_kb(), (unsigned)hb_os_heap_free());
            port_crumb("init", (uint32_t)rc, hb_os_heap_free());
            port_log_flush(LOG_PATH);
            if (rc) { sDead = 1; return; }
        }
        sState++;
        return;
    }
    {
        PlatTouches t;
        uint64_t now = plat_time_us(), t0, game_start, game_end, finish;
        uint32_t interval = last ? (uint32_t)(now - last) : 0;
        float dt = last ? (float)(now - last) * 1e-6f : 1.f / 60.f;
        uint32_t bg = game_bg_color(), us;
        last = now;
        if (dt > 0.1f) dt = 0.1f;            /* as the original's main loop does */
        keep_awake();
        read_touches(&t);
        sIoUs = 0;
        t0 = plat_time_us();
        gfx_begin_frame(w, h, sLogW, sLogH, 1, ((bg >> 16) & 255) / 255.f, ((bg >> 8) & 255) / 255.f, (bg & 255) / 255.f);
        game_start = plat_time_us();
        game_frame(dt, &t);
        game_end = plat_time_us();
        if (sLevelTraceFrames) port_checkpoint("level-updated", "", n);
        port_perf_draw(sLogW, sLogH);
        gfx_end_frame();
        finish = plat_time_us();
        port_perf_frame(interval, (uint32_t)(finish - now),
                        (uint32_t)(game_end - game_start), (uint32_t)(finish - game_end),
                        game_stat_physics_us, sIoUs, gfx_stat_upload_us);
        if (sLevelTraceFrames) {
            port_checkpoint("level-drawn", "", n);
            sLevelTraceFrames--;
        }
        us = (uint32_t)(plat_time_us() - t0);
        if (n < 4 || (n % 300) == 0) {
            printf("frame %u: %u us, %d draws, %d quads, %d tex %u KB, Lua %d KB, heap free %u\n", (unsigned)n,
                   (unsigned)us, gfx_stat_draws, gfx_stat_quads, gfx_stat_textures,
                   (unsigned)(gfx_stat_tex_bytes / 1024u), game_lua_kb(), (unsigned)hb_os_heap_free());
        }
        n++;
    }
}
