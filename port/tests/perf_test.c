/* The counter must measure callback cadence, not the much shorter CPU work. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nano/nano_perf.h"
#include "../src/gfx.h"

int gfx_stat_draws, gfx_stat_quads, gfx_stat_textures;
uint32_t gfx_stat_tex_bytes;
uint32_t game_stat_mask_builds;
static char report[4096];
static int writes;
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
    port_perf_level_loaded();
    for (int i = 0; i < 640; i++) {
        uint32_t interval = jitter ? (i & 1 ? 83333 : 16667) : 50000;
        port_perf_frame(interval, 6000, 3000, 2000, 1000, 0, 0);
    }
    assert(writes == 0); /* no storage I/O after draw submission */
    port_perf_flush();
    assert(writes == 1);
    assert(strstr(report, "frames=600 elapsed=30000000 fps10=200\n"));
    assert(strstr(report, "cpu=6000 game=3000 submit=2000"));
    assert(strstr(report, jitter ? "p95_ms=84 worst=83333" : "p95_ms=50 worst=50000"));
    assert(strstr(report, jitter ? "slow33=300 slow50=300" : "slow33=600 slow50=0"));
    port_perf_level_loaded();
    for (int i = 0; i < 1000; i++) port_perf_frame(50000, 6000, 3000, 2000, 1000, 0, 0);
    port_perf_flush();
    assert(writes == 1); /* Restarting a level must not trigger repeated disk writes. */
    puts(jitter ? "jitter profile: PASS" : "steady profile: PASS");
}
