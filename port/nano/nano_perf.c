/* Device frame-cadence measurement. The small HUD uses one 64x16 texture.
 * Stats survive level changes. Save a short sample after 5 measured seconds and
 * replace it once at 30 seconds. Writes happen before the next frame's GL work. */
#include <stdio.h>
#include <string.h>
#include "nano_perf.h"
#include "nano_alloc.h"
#include "../src/gfx.h"
#include "../src/platform.h"

typedef struct {
    uint64_t interval, cpu, game, submit, physics, io, upload;
    uint32_t frames, worst, slow33, slow50, histogram[201];
    uint32_t masks;
    uint64_t allocations, probes, copied, alloc_failures;
    uint64_t draws, merges, state_calls;
    uint64_t gap, buffer;
    uint32_t minimum, fast60;
} Samples;
static Samples sTotal;
static uint64_t sWindowUs, sWarmupUs;
static uint32_t sWindowFrames, sFps10;
static int sActive, sSaved, sEarlySaved, sFont, sReportBytes, sSkipSample;
static uint16_t sPixels[64 * 16];
static char sReport[512];

void port_perf_level_loaded(void)
{
    if (sSaved) return;
    /* Static storage starts empty. Keep earlier samples when the player moves
     * on quickly, while still excluding each level's load and first uploads. */
    sWarmupUs = 2000000;  /* exclude the first load and its texture uploads */
    sActive = 1;
}

static unsigned average(uint64_t sum)
{
    return sTotal.frames ? (unsigned)(sum / sTotal.frames) : 0;
}

static void prepare_report(void)
{
    unsigned target = (sTotal.frames * 95u + 99u) / 100u, seen = 0, p95 = 0;
    for (; p95 < 200; p95++) {
        seen += sTotal.histogram[p95];
        if (seen >= target) break;
    }
    int n = snprintf(sReport, sizeof(sReport),
        "AB profile v5; us except p95_ms; warmup=2s/level\n"
        "frames=%u elapsed=%u fps10=%u\n"
        "mean=%u p95_ms=%u worst=%u slow33=%u slow50=%u\n"
        "cpu=%u game=%u submit=%u physics=%u io=%u upload=%u\n"
        "mask_builds=%u draws=%d quads=%d textures=%d tex_KiB=%u\n"
        "alloc_avg=%u probes_avg=%u copy_B_avg=%u alloc_failures=%u\n"
        "draw_avg=%u merge_avg=%u state_avg=%u\n"
        "gap=%u vbo=%u min=%u at60=%u\n",
        (unsigned)sTotal.frames, (unsigned)sTotal.interval,
        (unsigned)(10000000ull * sTotal.frames / sTotal.interval),
        average(sTotal.interval), p95, (unsigned)sTotal.worst,
        (unsigned)sTotal.slow33, (unsigned)sTotal.slow50,
        average(sTotal.cpu), average(sTotal.game), average(sTotal.submit),
        average(sTotal.physics), average(sTotal.io), average(sTotal.upload),
        (unsigned)sTotal.masks, gfx_stat_draws, gfx_stat_quads, gfx_stat_textures,
        (unsigned)(gfx_stat_tex_bytes / 1024u),
        average(sTotal.allocations), average(sTotal.probes), average(sTotal.copied),
        (unsigned)sTotal.alloc_failures, average(sTotal.draws),
        average(sTotal.merges), average(sTotal.state_calls),
        average(sTotal.gap), average(sTotal.buffer),
        (unsigned)sTotal.minimum, (unsigned)sTotal.fast60);
    if (n > 0 && (unsigned)n < sizeof(sReport))
        sReportBytes = n;
}

void port_perf_flush(void)
{
    if (!sReportBytes) return;
    int bytes = sReportBytes;
    sReportBytes = 0; /* never retry repeatedly on an I/O failure */
    plat_write_file("perf.txt", sReport, (uint32_t)bytes);
    sSkipSample = 1; /* exclude the callback interval containing our own disk write */
}

void port_perf_frame(uint32_t interval, uint32_t cpu, uint32_t game,
                     uint32_t submit, uint32_t physics, uint32_t io, uint32_t upload,
                     uint32_t gap)
{
    if (!interval) return;  /* first callback has no previous timestamp */
    sWindowUs += interval;
    sWindowFrames++;
    if (sWindowUs >= 1000000) {
        sFps10 = (uint32_t)(10000000ull * sWindowFrames / sWindowUs);
        sWindowUs = 0;
        sWindowFrames = 0;
    }
    if (sSkipSample) { sSkipSample = 0; return; }
    if (!sActive || sSaved) return;
    if (sWarmupUs) {
        sWarmupUs = interval >= sWarmupUs ? 0 : sWarmupUs - interval;
        return;
    }
    sTotal.frames++;
    sTotal.interval += interval;
    sTotal.cpu += cpu;
    sTotal.game += game;
    sTotal.submit += submit;
    sTotal.physics += physics;
    sTotal.io += io;
    sTotal.upload += upload;
    sTotal.masks += game_stat_mask_builds;
    sTotal.draws += gfx_stat_draws;
    sTotal.merges += gfx_stat_merges;
    sTotal.state_calls += gfx_stat_state_calls;
    sTotal.gap += gap;
    sTotal.buffer += gfx_stat_buffer_us;
    if (!sTotal.minimum || interval < sTotal.minimum) sTotal.minimum = interval;
    sTotal.fast60 += interval <= 16667u;
    PortAllocStats ast = port_alloc_stats();
    sTotal.allocations += ast.allocations;
    sTotal.probes += ast.probes;
    sTotal.copied += ast.copied;
    sTotal.alloc_failures += ast.failures;
    if (interval > sTotal.worst) sTotal.worst = interval;
    sTotal.slow33 += interval > 33334u;
    sTotal.slow50 += interval > 50000u;
    uint32_t bin = (interval + 999u) / 1000u;
    if (bin > 200) bin = 200;
    sTotal.histogram[bin]++;
    if (!sEarlySaved && sTotal.interval >= 5000000) {
        sEarlySaved = 1;
        prepare_report();
    }
    if (sTotal.interval >= 30000000) {
        sSaved = 1;
        sActive = 0;
        prepare_report();
    }
}

static void quad(int tex, float x, float y, float w, float h,
                 float u, float v, float uw, float vh, uint32_t colour)
{
    float xy[8] = {x,y, x+w,y, x+w,y+h, x,y+h};
    float uv[8] = {u,v, u+uw,v, u+uw,v+vh, u,v+vh};
    gfx_quad(tex, 64, 16, xy, uv, colour);
}

void port_perf_draw(float width, float height)
{
    /* Five rows of 3 bits, top row first: digits, dot, F, P, S, dash. */
    static const uint8_t glyphs[15][5] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
        {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
        {0,0,0,0,2}, {7,4,6,4,4}, {6,5,6,4,4}, {7,4,7,1,7}, {0,0,7,0,0}
    };
    if (!sFont) {
        for (int g = 0; g < 15; g++)
            for (int y = 0; y < 5; y++)
                for (int x = 0; x < 3; x++)
                    sPixels[y * 64 + g * 4 + x] = (glyphs[g][y] & (4 >> x)) ? 0xffff : 0;
        sFont = gfx_tex_create(64, 16, GFX_FMT_RGBA4444, sPixels, 0, 0);
        if (!sFont) return;
    }
    unsigned value = sFps10 > 9999 ? 9999 : sFps10;
    static unsigned cached;
    static char text[16];
    static int length;
    if (!length || cached != value) {
        cached = value;
        if (value) snprintf(text, sizeof(text), "%u.%u FPS", value / 10, value % 10);
        else strcpy(text, "--.- FPS");
        length = (int)strlen(text);
    }
    float x = width - (float)length * 10.f - 6.f, y = height - 19.f;
    gfx_set_clip(0, 0, 0, 0);
    gfx_set_blend(GFX_BLEND_ALPHA);
    quad(0, x-3, y-2, (float)length*10.f+4, 19, 0,0,0,0, 0xc0000000u);
    for (const char *p = text; *p; p++, x += 10.f) {
        int g = *p >= '0' && *p <= '9' ? *p - '0' :
                *p == '.' ? 10 : *p == 'F' ? 11 : *p == 'P' ? 12 : *p == 'S' ? 13 : 14;
        if (*p != ' ') quad(sFont, x, y, 9,15, (float)(g*4),0,3,5, 0xffffffffu);
    }
}
