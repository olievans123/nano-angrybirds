/* Device frame-cadence measurement without an on-screen overlay.
 * Stats survive level changes. Save a short sample after 5 measured seconds and
 * replace it once at 30 seconds. Writes happen before the next frame's GL work. */
#include <stdio.h>
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
    uint64_t entry;
    uint32_t gap_histogram[201];
} Samples;
uint32_t port_perf_entry_us;
static Samples sTotal;
static uint64_t sWarmupUs;
static int sActive, sSaved, sEarlySaved, sReportBytes, sSkipSample;
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

static unsigned percentile(const uint32_t *histogram, unsigned percent)
{
    unsigned target = (sTotal.frames * percent + 99u) / 100u, seen = 0, bucket = 0;
    for (; bucket < 200; bucket++) {
        seen += histogram[bucket];
        if (seen >= target) break;
    }
    return bucket;
}

static void prepare_report(void)
{
    int n = snprintf(sReport, sizeof(sReport),
        "AB profile v6; us except *_ms; warmup=2s/level\n"
        "frames=%u elapsed=%u fps10=%u\n"
        "mean=%u p95_ms=%u worst=%u slow33=%u slow50=%u\n"
        "cpu=%u game=%u submit=%u physics=%u io=%u upload=%u\n"
        "mask_builds=%u draws=%d quads=%d textures=%d tex_KiB=%u\n"
        "alloc_avg=%u probes_avg=%u copy_B_avg=%u alloc_failures=%u\n"
        "draw_avg=%u merge_avg=%u state_avg=%u\n"
        "gap=%u vbo=%u min=%u at60=%u\n"
        "entry=%u gap50_ms=%u gap95_ms=%u\n",
        (unsigned)sTotal.frames, (unsigned)sTotal.interval,
        (unsigned)(10000000ull * sTotal.frames / sTotal.interval),
        average(sTotal.interval), percentile(sTotal.histogram, 95), (unsigned)sTotal.worst,
        (unsigned)sTotal.slow33, (unsigned)sTotal.slow50,
        average(sTotal.cpu), average(sTotal.game), average(sTotal.submit),
        average(sTotal.physics), average(sTotal.io), average(sTotal.upload),
        (unsigned)sTotal.masks, gfx_stat_draws, gfx_stat_quads, gfx_stat_textures,
        (unsigned)(gfx_stat_tex_bytes / 1024u),
        average(sTotal.allocations), average(sTotal.probes), average(sTotal.copied),
        (unsigned)sTotal.alloc_failures, average(sTotal.draws),
        average(sTotal.merges), average(sTotal.state_calls),
        average(sTotal.gap), average(sTotal.buffer),
        (unsigned)sTotal.minimum, (unsigned)sTotal.fast60,
        average(sTotal.entry), percentile(sTotal.gap_histogram, 50),
        percentile(sTotal.gap_histogram, 95));
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
    sTotal.entry += port_perf_entry_us;
    uint32_t gap_bin = gap / 1000u + (gap % 1000u != 0);
    if (gap_bin > 200) gap_bin = 200;
    sTotal.gap_histogram[gap_bin]++;
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
