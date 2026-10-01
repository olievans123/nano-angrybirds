/* Device frame-cadence measurement without an on-screen overlay.
 *
 * Sampling starts 2 s after a level loads and survives level changes. A short report is
 * saved after 5 measured seconds and replaced once at 32 s (two writes per launch at
 * most, each before the next frame's GL work).
 *
 * The report separates frames by how long the platform took to call back (<= 20 ms or
 * longer) so the cost of slow frames can be compared with the rest, and it alternates
 * the renderer's "opaque sprites unblended" switch every 4 s so one play session
 * measures that change against the old behaviour. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nano_perf.h"
#include "nano_alloc.h"
#include "../src/gfx.h"
#include "../src/platform.h"

#define SAMPLE_US   32000000u
#define EARLY_US    5000000u
#define WINDOW_US   4000000u      /* blend A/B window */
#define SLOW_US     20000u

typedef struct {
    uint32_t n, steps;
    uint64_t interval, cpu, game, submit, gap, script, native, writeback, physics, draws, quads, states;
} Class;

typedef struct {
    uint32_t frames, worst, cpu_worst, arena_min_free;
    uint64_t elapsed;
    uint32_t interval_hist[42], gap_hist[32], cpu_hist[42];
    Class speed[2];               /* 0: interval <= 20 ms, 1: slower */
    Class blend[2];               /* 0: everything blended, 1: opaque sprites unblended */
    uint32_t blend_slow[2];
    uint32_t alloc_failures, masks;
} Samples;

uint32_t port_perf_entry_us;
static Samples sTotal;
static uint32_t sWarmupUs;
static int sActive, sSaved, sEarlySaved, sReportBytes, sSkipSample;
static char sReport[2048];

void port_perf_level_loaded(void)
{
    if (sSaved) return;
    sWarmupUs = 2000000;  /* exclude the load and its texture uploads */
    sActive = 1;
}

static unsigned percentile(const uint32_t *hist, unsigned bins, unsigned percent)
{
    unsigned target = (sTotal.frames * percent + 99u) / 100u, seen = 0, b = 0;
    for (; b + 1 < bins; b++) {
        seen += hist[b];
        if (seen >= target) break;
    }
    return b;
}

static int put(int n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int put(int n, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (n < 0 || n >= (int)sizeof(sReport) - 1) return n;
    va_start(ap, fmt);
    k = vsnprintf(sReport + n, sizeof(sReport) - (size_t)n, fmt, ap);
    va_end(ap);
    if (k < 0) return n;
    n += k;
    return n >= (int)sizeof(sReport) ? (int)sizeof(sReport) - 1 : n;
}

static unsigned avg(uint64_t sum, uint32_t n) { return n ? (unsigned)(sum / n) : 0; }

static int put_class(int n, const char *label, const Class *c)
{
    return put(n, "%s n=%u interval=%u cpu=%u game=%u script=%u native=%u writeback=%u physics=%u submit=%u "
                  "gap=%u draws=%u quads=%u states=%u step%%=%u\n",
               label, (unsigned)c->n, avg(c->interval, c->n), avg(c->cpu, c->n), avg(c->game, c->n),
               avg(c->script, c->n), avg(c->native, c->n), avg(c->writeback, c->n), avg(c->physics, c->n),
               avg(c->submit, c->n), avg(c->gap, c->n), avg(c->draws, c->n), avg(c->quads, c->n),
               avg(c->states, c->n), c->n ? (unsigned)(100u * c->steps / c->n) : 0);
}

static void prepare_report(void)
{
    int n = 0, b;
    n = put(n, "AB profile v8; us unless noted; warmup=2s/level\n");
    n = put(n, "frames=%u elapsed=%u fps10=%u mean=%u worst=%u\n", (unsigned)sTotal.frames,
            (unsigned)sTotal.elapsed, (unsigned)(10000000ull * sTotal.frames / (sTotal.elapsed ? sTotal.elapsed : 1)),
            avg(sTotal.elapsed, sTotal.frames), (unsigned)sTotal.worst);
    n = put(n, "interval_ms 8..41+:");
    for (b = 8; b < 42; b++) n = put(n, " %u", (unsigned)sTotal.interval_hist[b]);
    n = put(n, "\ngap_ms 0..31+:");
    for (b = 0; b < 32; b++) n = put(n, " %u", (unsigned)sTotal.gap_hist[b]);
    n = put(n, "\ncpu_ms p50=%u p95=%u p99=%u worst_us=%u entry=%u\n", percentile(sTotal.cpu_hist, 42, 50),
            percentile(sTotal.cpu_hist, 42, 95), percentile(sTotal.cpu_hist, 42, 99), (unsigned)sTotal.cpu_worst,
            (unsigned)port_perf_entry_us);
    n = put_class(n, "fast(<=20ms)", &sTotal.speed[0]);
    n = put_class(n, "slow(>20ms)", &sTotal.speed[1]);
    n = put_class(n, "blended", &sTotal.blend[0]);
    n = put_class(n, "opaque-unblended", &sTotal.blend[1]);
    n = put(n, "slow frames: blended %u, opaque-unblended %u\n", (unsigned)sTotal.blend_slow[0],
            (unsigned)sTotal.blend_slow[1]);
    n = put(n, "mem: arena_min_free=%u lua_peak_KiB=%u lua_fail=%u lua_recover=%u tex_KiB=%u textures=%d "
               "alloc_fail=%u mask_builds=%u\n",
            (unsigned)sTotal.arena_min_free, (unsigned)(game_stat_lua_cost_peak / 1024u),
            (unsigned)game_stat_lua_failures, (unsigned)game_stat_lua_recoveries,
            (unsigned)(gfx_stat_tex_bytes / 1024u), gfx_stat_textures, (unsigned)sTotal.alloc_failures,
            (unsigned)sTotal.masks);
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

static void add(Class *c, uint32_t interval, uint32_t cpu, uint32_t game, uint32_t submit, uint32_t gap)
{
    c->n++;
    c->interval += interval;
    c->cpu += cpu;
    c->game += game;
    c->submit += submit;
    c->gap += gap;
    c->script += game_prof_us[PROF_SCRIPT];
    c->native += game_prof_us[PROF_NATIVE];
    c->writeback += game_prof_us[PROF_WRITEBACK];
    c->physics += game_prof_us[PROF_PHYSICS];
    c->draws += (uint32_t)gfx_stat_draws;
    c->quads += (uint32_t)gfx_stat_quads;
    c->states += (uint32_t)gfx_stat_state_calls;
    c->steps += game_stat_steps != 0;
}

static uint32_t bin(uint32_t us, uint32_t bins)
{
    uint32_t b = (us + 999u) / 1000u;
    return b >= bins ? bins - 1 : b;
}

void port_perf_frame(uint32_t interval, uint32_t cpu, uint32_t game,
                     uint32_t submit, uint32_t physics, uint32_t io, uint32_t upload,
                     uint32_t gap)
{
    (void)physics; (void)io; (void)upload;
    if (!interval) return;  /* first callback has no previous timestamp */
    if (sSkipSample) { sSkipSample = 0; return; }
    if (!sActive || sSaved) return;
    if (sWarmupUs) {
        sWarmupUs = interval >= sWarmupUs ? 0 : sWarmupUs - interval;
        return;
    }
    /* this frame was drawn with the switch as it stood; choose it for the next one */
    int mode = gfx_opaque_fast != 0;
    sTotal.frames++;
    sTotal.elapsed += interval;
    if (interval > sTotal.worst) sTotal.worst = interval;
    if (cpu > sTotal.cpu_worst) sTotal.cpu_worst = cpu;
    sTotal.interval_hist[bin(interval, 42)]++;
    sTotal.gap_hist[bin(gap, 32)]++;
    sTotal.cpu_hist[bin(cpu, 42)]++;
    add(&sTotal.speed[interval > SLOW_US], interval, cpu, game, submit, gap);
    add(&sTotal.blend[mode], interval, cpu, game, submit, gap);
    sTotal.blend_slow[mode] += interval > SLOW_US;
    sTotal.masks += game_stat_mask_builds;
    sTotal.alloc_failures += port_alloc_stats().failures;
    {
        uint32_t arena_free = port_arena_free_bytes();
        if (sTotal.frames == 1 || arena_free < sTotal.arena_min_free) sTotal.arena_min_free = arena_free;
    }
    gfx_opaque_fast = (int)((sTotal.elapsed / WINDOW_US) & 1u);
    if (!sEarlySaved && sTotal.elapsed >= EARLY_US) {
        sEarlySaved = 1;
        prepare_report();
    }
    if (sTotal.elapsed >= SAMPLE_US) {
        sSaved = 1;
        sActive = 0;
        gfx_opaque_fast = 1;      /* measurement over: keep the cheaper path */
        prepare_report();
    }
}
