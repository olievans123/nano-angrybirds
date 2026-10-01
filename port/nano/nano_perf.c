/* Device frame-cadence measurement. The small HUD uses one 64x16 texture.
 * Stats stay in RAM; a compact report is saved at the start of the callback after
 * measurement, before any new GL work. No file writes during draw submission. */
#include <stdio.h>
#include <string.h>
#include "nano_perf.h"
#include "../src/gfx.h"
#include "../src/platform.h"

typedef struct {
    uint64_t interval, cpu, game, submit, physics, io, upload;
    uint32_t frames, worst, slow33, slow50, histogram[201];
    uint32_t masks;
} Samples;
static Samples sTotal;
static uint64_t sWindowUs, sWarmupUs;
static uint32_t sWindowFrames, sFps10;
static int sActive, sSaved, sFont, sReportBytes;
static uint16_t sPixels[64 * 16];
static char sReport[512];

void port_perf_level_loaded(void)
{
    if (sSaved) return;
    memset(&sTotal, 0, sizeof(sTotal));
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
        "AB profile v2; times us except p95_ms; 30s after 2s warmup\n"
        "frames=%u elapsed=%u fps10=%u\n"
        "mean=%u p95_ms=%u worst=%u slow33=%u slow50=%u\n"
        "cpu=%u game=%u submit=%u physics=%u io=%u upload=%u\n"
        "mask_builds=%u draws=%d quads=%d textures=%d tex_KiB=%u\n",
        (unsigned)sTotal.frames, (unsigned)sTotal.interval,
        (unsigned)(10000000ull * sTotal.frames / sTotal.interval),
        average(sTotal.interval), p95, (unsigned)sTotal.worst,
        (unsigned)sTotal.slow33, (unsigned)sTotal.slow50,
        average(sTotal.cpu), average(sTotal.game), average(sTotal.submit),
        average(sTotal.physics), average(sTotal.io), average(sTotal.upload),
        (unsigned)sTotal.masks, gfx_stat_draws, gfx_stat_quads, gfx_stat_textures,
        (unsigned)(gfx_stat_tex_bytes / 1024u));
    if (n > 0 && (unsigned)n < sizeof(sReport))
        sReportBytes = n;
}

void port_perf_flush(void)
{
    if (!sReportBytes) return;
    int bytes = sReportBytes;
    sReportBytes = 0; /* never retry repeatedly on an I/O failure */
    plat_write_file("perf.txt", sReport, (uint32_t)bytes);
}

void port_perf_frame(uint32_t interval, uint32_t cpu, uint32_t game,
                     uint32_t submit, uint32_t physics, uint32_t io, uint32_t upload)
{
    if (!interval) return;  /* first callback has no previous timestamp */
    sWindowUs += interval;
    sWindowFrames++;
    if (sWindowUs >= 1000000) {
        sFps10 = (uint32_t)(10000000ull * sWindowFrames / sWindowUs);
        sWindowUs = 0;
        sWindowFrames = 0;
    }
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
    if (interval > sTotal.worst) sTotal.worst = interval;
    sTotal.slow33 += interval > 33334u;
    sTotal.slow50 += interval > 50000u;
    uint32_t bin = (interval + 999u) / 1000u;
    if (bin > 200) bin = 200;
    sTotal.histogram[bin]++;
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
    char text[16];
    if (sFps10) snprintf(text, sizeof(text), "%u.%u FPS", value / 10, value % 10);
    else strcpy(text, "--.- FPS");
    float x = width - (float)strlen(text) * 10.f - 6.f, y = height - 19.f;
    gfx_set_clip(0, 0, 0, 0);
    gfx_set_blend(GFX_BLEND_ALPHA);
    quad(0, x-3, y-2, (float)strlen(text)*10.f+4, 19, 0,0,0,0, 0xc0000000u);
    for (const char *p = text; *p; p++, x += 10.f) {
        int g = *p >= '0' && *p <= '9' ? *p - '0' :
                *p == '.' ? 10 : *p == 'F' ? 11 : *p == 'P' ? 12 : *p == 'S' ? 13 : 14;
        if (*p != ' ') quad(sFont, x, y, 9,15, (float)(g*4),0,3,5, 0xffffffffu);
    }
}
