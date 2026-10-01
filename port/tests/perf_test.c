/* The counter must measure callback cadence, not the much shorter CPU work. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nano/nano_perf.h"
#include "../src/gfx.h"

int gfx_stat_draws, gfx_stat_quads, gfx_stat_textures;
uint32_t gfx_stat_tex_bytes;
static char report[4096];
static int writes;
int plat_write_file(const char *path, const void *data, uint32_t size) {
    assert(!strcmp(path, "perf.txt") && size < sizeof(report));
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
    assert(writes == 1);
    assert(strstr(report, "frames=600 elapsed_us=30000000 fps_tenths=200\n"));
    assert(strstr(report, "cpu_avg_us=6000 game_avg_us=3000 submit_avg_us=2000"));
    assert(strstr(report, jitter ? "frame_p95_ms=84 frame_worst_us=83333" : "frame_p95_ms=50 frame_worst_us=50000"));
    assert(strstr(report, jitter ? "frames_over_33ms=300 frames_over_50ms=300" : "frames_over_33ms=600 frames_over_50ms=0"));
    port_perf_level_loaded();
    for (int i = 0; i < 1000; i++) port_perf_frame(50000, 6000, 3000, 2000, 1000, 0, 0);
    assert(writes == 1); /* Restarting a level must not trigger repeated disk writes. */
    puts(jitter ? "jitter profile: PASS" : "steady profile: PASS");
}
