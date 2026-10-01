/* resources.h — the Lua table `res` and what the rest of the engine uses from it. */
#ifndef AB_RESOURCES_H
#define AB_RESOURCES_H

#include "assets.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lua_State;
void res_register(struct lua_State *L);
void res_audio_tick(float dt);
int  res_audio_is_playing(const char *name);
void res_audio_play(const char *name, float volume, int loop, int track);
void res_audio_stop(const char *name);
void res_audio_set_volume(const char *name, float volume);

const AbSprite *res_find_sprite(const char *sheet, const char *sprite);   /* sheet "" = any */
const char *res_get_string(const char *group, const char *id);
/* Width in game pixels of `count` characters from character `start` in the current font. */
int res_string_width(const char *text, int start, int count);
int res_utf16_length(const char *text);
const char *res_utf16_offset(const char *text, int index);

#ifdef __cplusplus
}
#endif

#endif
