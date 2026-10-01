/* The counter must measure callback cadence, not the much shorter CPU work. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nano/nano_perf.h"
#include "../nano/nano_alloc.h"
#include "../src/gfx.h"

int gfx_stat_draws, gfx_stat_quads, gfx_stat_textures;
uint32_t gfx_stat_tex_bytes;
uint32_t game_stat_mask_builds;
static char report[4096];
static int writes;
PortAllocStats port_alloc_stats(void) { return (PortAllocStats){10, 50, 8, 0}; }
int plat_write_file(const char *path, const void *data, uint32_t size) {
    assert(!strcmp(path, "perf.txt") && size < 512);
    memcpy(report, data, size); report[size] = 0; writes++;
    return 0;
}
int gfx_tex_create(int w, int h, int fmt, const void *p, int linear, int repeat) { return 1; }
void gfx_set_clip(float x, float y, float w, float h) {}
void gfx_set_blend(int mode) {}
void gfx_quad(int tex, int w, int h, const float xy[8], const float uv[8], uint32_t c) {}

int main(int argc, char **argv) {
    int jitter = argc > 1 && !strcmp(argv[1], "jitter");
    int transitions = argc > 1 && !strcmp(argv[1], "transitions");
    port_perf_level_loaded();
    /* Short levels must not discard previous measurements. Simulate 3-second
     * measured segments separated by the 2-second load warmup. */
    int frame = 0, measured = 0;
    for (int segment = 0; segment < (transitions ? 10 : 1); segment++) {
        if (segment) port_perf_level_loaded();
        for (int warm = 0; warm < 40; warm++, frame++) {
            uint32_t interval = jitter ? (frame & 1 ? 83333 : 16667) : 50000;
            port_perf_frame(interval, 6000, 3000, 2000, 1000, 0, 0);
            port_perf_flush();
        }
        for (int i = 0; i < (transitions ? 60 : 600); i++, frame++) {
            uint32_t interval = jitter ? (frame & 1 ? 83333 : 16667) : 50000;
            int before = writes;
            port_perf_frame(interval, 6000, 3000, 2000, 1000, 0, 0);
            measured++;
            assert(writes == before); /* no writes during draw submission */
            port_perf_flush();
            if (measured == 100) {
                assert(writes == 1);
                assert(strstr(report, "frames=100 elapsed=5000000 fps10=200\n"));
            }
            assert(writes == (measured < 100 ? 0 : measured < 600 ? 1 : 2));
            if (writes > before && writes == 1) {
                /* The next callback includes the recorder's own I/O stall;
                 * it must not inflate the reported gameplay frame times. */
                port_perf_frame(800000, 6000, 3000, 2000, 1000, 0, 0);
                port_perf_flush();
                assert(writes == 1);
            }
        }
    }
    assert(writes == 2);
    assert(strstr(report, "frames=600 elapsed=30000000 fps10=200\n"));
    assert(strstr(report, "cpu=6000 game=3000 submit=2000"));
    assert(strstr(report, "alloc_avg=10 probes_avg=50 copy_B_avg=8 alloc_failures=0"));
    assert(strstr(report, jitter ? "p95_ms=84 worst=83333" : "p95_ms=50 worst=50000"));
    assert(strstr(report, jitter ? "slow33=300 slow50=300" : "slow33=600 slow50=0"));
    port_perf_level_loaded();
    for (int i = 0; i < 1000; i++) port_perf_frame(50000, 6000, 3000, 2000, 1000, 0, 0);
    port_perf_flush();
    assert(writes == 2); /* At most two report writes in a whole launch. */
    puts(transitions ? "short-level transitions: PASS" : jitter ? "jitter profile: PASS" : "steady profile: PASS");
}
