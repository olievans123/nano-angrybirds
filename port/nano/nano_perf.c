/* Device frame-cadence measurement and frame pacing, without an on-screen overlay.
 *
 * What the first reports showed: the platform calls back about 8 ms after the game's
 * frame work ends, or about 25 ms after it (one display refresh later), and the long
 * case happened on a third of the frames whatever the game's own cost. So this build
 * tries holding each frame back to a fixed spacing instead of finishing as early as
 * possible: after 5 s of free running in a level it tries a list of spacings for 3 s
 * each, reports them all, and keeps the best for the rest of the session.
 *
 * Sampling starts 2 s after a level loads and survives level changes. The report is saved
 * after the free-running part and once more after the sweep (two writes per launch). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nano_perf.h"
#include "nano_alloc.h"
#include "../src/gfx.h"
#include "../src/platform.h"

#define FREE_US     5000000u      /* free-running part */
#define PHASE_US    3000000u      /* per spacing */
#define SLOW_US     24000u        /* an interval this long skipped a refresh */
#define SEQ_FRAMES  120

static const uint16_t kTargets[] = { 14000, 15000, 16000, 16500, 16700, 17000, 17500, 18000, 19000, 20000 };
#define NTARGETS ((int)(sizeof(kTargets) / sizeof(kTargets[0])))

typedef struct {
    uint32_t n, slow, min, max;
    uint64_t interval, cpu, gap, game, submit, script, draws;
} Phase;

typedef struct {
    uint32_t frames, worst, cpu_worst, arena_min_free;
    uint64_t elapsed;
    uint32_t interval_hist[42], gap_hist[32], cpu_hist[42];   /* free-running part */
    Phase phase[1 + NTARGETS];                                /* 0 = free running */
    uint16_t seq[SEQ_FRAMES][3];                              /* interval, cpu, gap in 0.1 ms */
    uint32_t alloc_failures, masks;
} Samples;

uint32_t port_perf_entry_us;
static Samples sTotal;
static uint32_t sWarmupUs, sGapEstimate = 7800, sPaceUs;
static int sActive, sSaved, sEarlySaved, sReportBytes, sSkipSample, sPhase, sBest = -1;
static uint64_t sPhaseStart;
static char sReport[4096];

uint32_t port_perf_pace_us(void) { return sPaceUs; }
uint32_t port_perf_gap_estimate(void) { return sGapEstimate; }

void port_perf_level_loaded(void)
{
    if (sSaved) return;
    sWarmupUs = 2000000;  /* exclude the load and its texture uploads */
    sActive = 1;
}

static unsigned percentile(const uint32_t *hist, unsigned bins, unsigned percent, uint32_t total)
{
    unsigned target = (total * percent + 99u) / 100u, seen = 0, b = 0;
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

/* Lower is better: mean spacing, plus a charge for frames that skipped a refresh. */
static uint32_t phase_score(const Phase *p)
{
    if (p->n < 30) return 0xFFFFFFFFu;
    return avg(p->interval, p->n) + (uint32_t)(10000ull * p->slow / p->n);
}

static void prepare_report(void)
{
    int n = 0, b, i;
    const Phase *f = &sTotal.phase[0];
    n = put(n, "AB profile v9; us unless noted; warmup=2s/level\n");
    n = put(n, "frames=%u elapsed=%u fps10=%u worst=%u gap_estimate=%u\n", (unsigned)sTotal.frames,
            (unsigned)sTotal.elapsed, (unsigned)(10000000ull * sTotal.frames / (sTotal.elapsed ? sTotal.elapsed : 1)),
            (unsigned)sTotal.worst, (unsigned)sGapEstimate);
    n = put(n, "free-running interval_ms 8..41+:");
    for (b = 8; b < 42; b++) n = put(n, " %u", (unsigned)sTotal.interval_hist[b]);
    n = put(n, "\nfree-running gap_ms 0..31+:");
    for (b = 0; b < 32; b++) n = put(n, " %u", (unsigned)sTotal.gap_hist[b]);
    n = put(n, "\nfree-running cpu_ms p50=%u p95=%u p99=%u worst_us=%u\n", percentile(sTotal.cpu_hist, 42, 50, f->n),
            percentile(sTotal.cpu_hist, 42, 95, f->n), percentile(sTotal.cpu_hist, 42, 99, f->n),
            (unsigned)sTotal.cpu_worst);
    for (i = 0; i <= NTARGETS; i++) {
        const Phase *p = &sTotal.phase[i];
        if (!p->n) continue;
        n = put(n, "pace=%u n=%u mean=%u slow=%u min=%u max=%u cpu=%u gap=%u game=%u script=%u submit=%u draws=%u%s\n",
                i ? (unsigned)kTargets[i - 1] : 0u, (unsigned)p->n, avg(p->interval, p->n), (unsigned)p->slow,
                (unsigned)p->min, (unsigned)p->max, avg(p->cpu, p->n), avg(p->gap, p->n), avg(p->game, p->n),
                avg(p->script, p->n), avg(p->submit, p->n), avg(p->draws, p->n), i == sBest ? " <- kept" : "");
    }
    n = put(n, "mem: arena_min_free=%u lua_peak_KiB=%u lua_fail=%u lua_recover=%u tex_KiB=%u textures=%d "
               "alloc_fail=%u mask_builds=%u\n",
            (unsigned)sTotal.arena_min_free, (unsigned)(game_stat_lua_cost_peak / 1024u),
            (unsigned)game_stat_lua_failures, (unsigned)game_stat_lua_recoveries,
            (unsigned)(gfx_stat_tex_bytes / 1024u), gfx_stat_textures, (unsigned)sTotal.alloc_failures,
            (unsigned)sTotal.masks);
    n = put(n, "free-running sequence (interval,cpu,gap in 0.1 ms):");
    for (i = 0; i < SEQ_FRAMES && sTotal.seq[i][0]; i++)
        n = put(n, " %u,%u,%u", (unsigned)sTotal.seq[i][0], (unsigned)sTotal.seq[i][1], (unsigned)sTotal.seq[i][2]);
    n = put(n, "\n");
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

static uint32_t bin(uint32_t us, uint32_t bins)
{
    uint32_t b = (us + 999u) / 1000u;
    return b >= bins ? bins - 1 : b;
}

static uint16_t tenths(uint32_t us) { us = (us + 50u) / 100u; return us > 65535u ? 65535u : (uint16_t)us; }

void port_perf_frame(uint32_t interval, uint32_t cpu, uint32_t game,
                     uint32_t submit, uint32_t physics, uint32_t io, uint32_t upload,
                     uint32_t gap)
{
    (void)physics; (void)io; (void)upload;
    if (!interval) return;  /* first callback has no previous timestamp */
    /* the platform's own time between our frames, when it does not skip a refresh */
    if (gap > 2000u && gap < 16000u) sGapEstimate = (sGapEstimate * 7u + gap) / 8u;
    if (sSkipSample) { sSkipSample = 0; return; }
    if (!sActive || sSaved) return;
    if (sWarmupUs) {
        sWarmupUs = interval >= sWarmupUs ? 0 : sWarmupUs - interval;
        return;
    }
    Phase *p = &sTotal.phase[sPhase];
    /* the first frames of a phase still carry the previous spacing */
    if (sTotal.elapsed - sPhaseStart >= 100000u || sPhase == 0) {
        p->n++;
        p->interval += interval;
        p->cpu += cpu;
        p->gap += gap;
        p->game += game;
        p->submit += submit;
        p->script += game_prof_us[PROF_SCRIPT];
        p->draws += (uint32_t)gfx_stat_draws;
        p->slow += interval > SLOW_US;
        if (!p->min || interval < p->min) p->min = interval;
        if (interval > p->max) p->max = interval;
    }
    if (sPhase == 0) {
        if (sTotal.frames < SEQ_FRAMES) {
            sTotal.seq[sTotal.frames][0] = tenths(interval);
            sTotal.seq[sTotal.frames][1] = tenths(cpu);
            sTotal.seq[sTotal.frames][2] = tenths(gap);
        }
        sTotal.interval_hist[bin(interval, 42)]++;
        sTotal.gap_hist[bin(gap, 32)]++;
        sTotal.cpu_hist[bin(cpu, 42)]++;
        if (cpu > sTotal.cpu_worst) sTotal.cpu_worst = cpu;
    }
    sTotal.frames++;
    sTotal.elapsed += interval;
    if (interval > sTotal.worst) sTotal.worst = interval;
    sTotal.masks += game_stat_mask_builds;
    sTotal.alloc_failures += port_alloc_stats().failures;
    {
        uint32_t arena_free = port_arena_free_bytes();
        if (sTotal.frames == 1 || arena_free < sTotal.arena_min_free) sTotal.arena_min_free = arena_free;
    }
    if (sTotal.elapsed - sPhaseStart >= (sPhase == 0 ? FREE_US : PHASE_US)) {
        if (sPhase == 0 && !sEarlySaved) {
            sEarlySaved = 1;
            prepare_report();
        }
        sPhaseStart = sTotal.elapsed;
        if (++sPhase > NTARGETS) {
            /* sweep finished: keep the spacing with the best score */
            uint32_t best = 0xFFFFFFFFu;
            for (int i = 0; i <= NTARGETS; i++) {
                uint32_t sc = phase_score(&sTotal.phase[i]);
                if (sc < best) { best = sc; sBest = i; }
            }
            sPaceUs = sBest > 0 ? kTargets[sBest - 1] : 0;
            sSaved = 1;
            sActive = 0;
            prepare_report();
        } else {
            sPaceUs = kTargets[sPhase - 1];
        }
    }
}
