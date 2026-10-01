/* platform.h — what the engine needs from the machine it runs on. */
#ifndef AB_PLATFORM_H
#define AB_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t plat_time_us(void);

/* Read a whole file. `path` is relative to the game's data directory, or to the save
 * directory when `save` is set. Returns a malloc'd buffer (caller frees) with one extra
 * zero byte after the data, or NULL if the file is missing. */
void *plat_read_file(const char *path, uint32_t *size, int save);
int   plat_write_file(const char *path, const void *data, uint32_t size);   /* save directory */

void plat_log(const char *fmt, ...);

/* Bounded, explicitly enabled device bring-up checkpoints; ordinary runs do no I/O. */
#ifdef AB_NANO
void port_checkpoint(const char *stage, const char *detail, uint32_t value);
extern uint32_t game_stat_physics_us;
#else
#define port_checkpoint(stage, detail, value) ((void)0)
#endif

/* Audio. `file` is relative to the data directory ("SFX/x.wav", "Music/x.mp3").
 * plat_audio_play returns the clip's length in seconds (0 if nothing is played), which is
 * what res.isAudioPlaying reports on. plat_audio_stop(NULL) stops everything. */
float plat_audio_play(const char *file, float volume, int loop, int track);
void  plat_audio_stop(const char *file);
void  plat_audio_track_volume(int track, float volume);
void  plat_audio_clip_volume(const char *file, float volume);

/* Fingers currently on the screen, in logical game pixels. */
#define PLAT_MAX_TOUCHES 2
typedef struct {
    int count;
    struct { float x, y; } pt[PLAT_MAX_TOUCHES];
} PlatTouches;

/* ---- the game, as the platform drives it ---- */
int  game_init(float logical_w, float logical_h);                 /* 0 on success */
void game_frame(float dt, const PlatTouches *touches);            /* update + draw (inside a gfx frame) */
void game_gl_lost(void);                                           /* GL objects are gone */
uint32_t game_bg_color(void);                                      /* 0xRRGGBB to clear with */
int  game_wants_exit(void);
int  game_lua_kb(void);
void game_debug_dump(const char *names);                           /* test hook: log script globals */

#ifdef __cplusplus
}
#endif

#endif
