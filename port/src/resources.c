/* resources.c — the Lua table `res` (game::LuaResources / game::Resources in the original):
 * sprite sheets, bitmap fonts, localised text and audio.
 *
 * Behaviour follows analysis/resources-spec.md. Differences on purpose: argument types are
 * converted the Lua way instead of throwing, a character the font lacks is skipped
 * instead of throwing, and audio goes through the platform hooks (plat_audio_*). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "resources.h"
#include "render.h"
#include "assets.h"
#include "platform.h"

/* ---- names ---- */

/* "800X480/INGAME.dat" -> "INGAME" */
static void resource_name(const char *path, char *out, size_t n)
{
    const char *base = strrchr(path, '/'), *dot;
    size_t len;
    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= n) len = n - 1;
    memcpy(out, base, len);
    out[len] = 0;
}

/* ---- fonts ---- */
#define MAX_FONTS 8
typedef struct {
    char name[32];
    AbSheet *sheet;
    int leading, tracking, max_asc, max_desc;
    const AbSprite *glyph[256];
} Font;

static Font sFonts[MAX_FONTS];
static Font *sFont;

static Font *font_find(const char *name)
{
    int i;
    for (i = 0; i < MAX_FONTS; i++)
        if (sFonts[i].sheet && !strcmp(sFonts[i].name, name))
            return &sFonts[i];
    return NULL;
}

static int font_create(const char *name)
{
    Font *f = font_find(name);
    char path[64];
    uint8_t *d;
    uint32_t size = 0;
    int i, code;
    if (f)
        return 1;
    for (i = 0; i < MAX_FONTS && sFonts[i].sheet; i++) {}
    if (i == MAX_FONTS)
        return 0;
    f = &sFonts[i];
    memset(f, 0, sizeof(*f));
    f->sheet = assets_load_sheet(name);
    if (!f->sheet)
        return 0;
    strncpy(f->name, name, sizeof(f->name) - 1);
    snprintf(path, sizeof path, "%s.fnt", name);
    d = (uint8_t *)plat_read_file(path, &size, 0);
    if (d && size >= 4) {
        f->leading = (int16_t)(d[0] | (d[1] << 8));
        f->tracking = (int16_t)(d[2] | (d[3] << 8));
    }
    free(d);
    for (code = 0; code < 256; code++) {
        char key[8];
        const AbSprite *g;
        snprintf(key, sizeof key, "%d", code);
        g = assets_find_sprite(f->sheet, key);
        f->glyph[code] = g;
        if (g) {
            if (g->pivot_y > f->max_asc) f->max_asc = g->pivot_y;
            if ((int)g->h - g->pivot_y > f->max_desc) f->max_desc = (int)g->h - g->pivot_y;
        }
    }
    return 1;
}

/* Next UTF-16 code unit of a UTF-8 string (invalid bytes are skipped); 0 at the end. */
static unsigned next_code(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    for (;;) {
        unsigned c = *p;
        if (!c) { *pp = p; return 0; }
        if (c < 0x80) { *pp = p + 1; return c; }
        if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *pp = p + 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
        if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            *pp = p + 3;
            return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        }
        p++;
    }
}

int res_utf16_length(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    int n = 0;
    while (next_code(&p)) n++;
    return n;
}

const char *res_utf16_offset(const char *text, int index)
{
    const unsigned char *p = (const unsigned char *)text;
    while (index-- > 0 && next_code(&p)) {}
    return (const char *)p;
}

int res_string_width(const char *text, int start, int count)
{
    const unsigned char *p = (const unsigned char *)text;
    int w = 0, n = 0, i = 0;
    unsigned c;
    if (!sFont)
        return 0;
    while ((c = next_code(&p)) != 0) {
        if (i++ < start) continue;
        if (n >= count) break;
        if (c < 256 && sFont->glyph[c]) w += sFont->glyph[c]->w;
        n++;
    }
    return n ? w + sFont->tracking * (n - 1) : 0;
}

static void draw_string(const char *text, float x, float y, Anchor a)
{
    const unsigned char *p = (const unsigned char *)text;
    const Anchor pivot = { V_PIVOT, H_PIVOT };
    unsigned c;
    if (!sFont || !*text)
        return;
    switch (a.v) {
        case V_TOP:    y += (float)sFont->max_asc; break;
        case V_CENTER: y += (float)(sFont->max_asc - ((sFont->max_asc + sFont->max_desc) >> 1)); break;
        case V_BOTTOM: y -= (float)sFont->max_desc; break;
        default: break;
    }
    if (a.h == H_CENTER || a.h == H_RIGHT) {
        int w = res_string_width(text, 0, 0x7fffffff);
        x -= (float)(a.h == H_CENTER ? w >> 1 : w);
    }
    while ((c = next_code(&p)) != 0) {
        const AbSprite *g = c < 256 ? sFont->glyph[c] : NULL;
        if (!g)
            continue;                                   /* the original throws here */
        render_sprite(g, x, y, pivot);
        x += (float)(g->w + sFont->tracking);
    }
}

/* ---- localised text ---- */
#define MAX_TEXT_SETS 2
typedef struct {
    char name[32], file[48];
    int nids;
    char **ids, **text;          /* text of the loaded locale, parallel to ids */
    char *blob;                  /* storage for both */
    char locale[8];
    /* literals drawn through this group that are not ids (the original caches id -> id) */
} TextSet;

static TextSet sTextSets[MAX_TEXT_SETS];
static char sLocale[8] = "en_EN";

static TextSet *textset_find(const char *name)
{
    int i;
    for (i = 0; i < MAX_TEXT_SETS; i++)
        if (sTextSets[i].name[0] && !strcmp(sTextSets[i].name, name))
            return &sTextSets[i];
    return NULL;
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void textset_unload(TextSet *ts)
{
    free(ts->ids);
    free(ts->text);
    free(ts->blob);
    ts->ids = ts->text = NULL;
    ts->blob = NULL;
    ts->nids = 0;
    ts->locale[0] = 0;
}

/* TEXTS_*.dat: u8 version, s32 localeBlockSize, [u8 nLocales, names], s16 nIds, ids,
 * s32 offset[nLocales] (each relative to the end of its own entry), then per locale nIds strings. */
static void textset_load(TextSet *ts, const char *locale)
{
    uint32_t size = 0, pos, nloc, i, k = 0xFFFFFFFFu, nids, blob_len = 0, ids_pos;
    uint8_t *d = (uint8_t *)plat_read_file(ts->file, &size, 0);
    char *out;
    textset_unload(ts);
    if (!d || size < 8) { free(d); return; }
    pos = 5;
    nloc = d[pos++];
    for (i = 0; i < nloc && pos + 2 <= size; i++) {
        uint32_t len = be16(d + pos);
        if (len == strlen(locale) && !memcmp(d + pos + 2, locale, len)) k = i;
        pos += 2 + len;
    }
    if (k == 0xFFFFFFFFu) {
        plat_log("text set %s has no locale %s", ts->name, locale);
        free(d);
        return;
    }
    nids = be16(d + pos);
    pos += 2;
    ids_pos = pos;
    for (i = 0; i < nids; i++) { uint32_t len = be16(d + pos); blob_len += len + 1; pos += 2 + len; }
    {
        uint32_t tpos = pos + 4 * k, off = be32(d + tpos), p2;
        tpos += 4 + off;
        p2 = tpos;
        for (i = 0; i < nids && p2 + 2 <= size; i++) { uint32_t len = be16(d + p2); blob_len += len + 1; p2 += 2 + len; }
        ts->ids = (char **)calloc(nids, sizeof(char *));
        ts->text = (char **)calloc(nids, sizeof(char *));
        ts->blob = (char *)malloc(blob_len + 1);
        if (!ts->ids || !ts->text || !ts->blob) { textset_unload(ts); free(d); return; }
        out = ts->blob;
        pos = ids_pos;
        for (i = 0; i < nids; i++) {
            uint32_t len = be16(d + pos);
            memcpy(out, d + pos + 2, len); out[len] = 0;
            ts->ids[i] = out; out += len + 1; pos += 2 + len;
        }
        for (i = 0; i < nids; i++) {
            uint32_t len = tpos + 2 <= size ? be16(d + tpos) : 0;
            memcpy(out, d + tpos + 2, len); out[len] = 0;
            ts->text[i] = out; out += len + 1; tpos += 2 + len;
        }
    }
    ts->nids = (int)nids;
    strncpy(ts->locale, locale, sizeof(ts->locale) - 1);
    free(d);
}

const char *res_get_string(const char *group, const char *id)
{
    TextSet *ts = textset_find(group);
    int i;
    if (!ts)
        return id;
    if (strcmp(ts->locale, sLocale))
        textset_load(ts, sLocale);          /* the original throws if the locale isn't loaded */
    for (i = 0; i < ts->nids; i++)
        if (!strcmp(ts->ids[i], id))
            return ts->text[i];
    return id;
}

/* ---- audio ---- */
#define MAX_CLIPS 160
typedef struct { char name[40], file[56]; float until; int looping; } Clip;
static Clip sClips[MAX_CLIPS];
static int sClipCount;
static float sAudioClock;

static Clip *clip_find(const char *name)
{
    int i;
    for (i = 0; i < sClipCount; i++)
        if (!strcmp(sClips[i].name, name))
            return &sClips[i];
    return NULL;
}

void res_audio_tick(float dt) { sAudioClock += dt; }

int res_audio_is_playing(const char *name)
{
    Clip *c = clip_find(name);
    return c && (c->looping || c->until > sAudioClock);
}

void res_audio_play(const char *name, float volume, int loop, int track)
{
    Clip *c = clip_find(name);
    if (!c)
        return;
    c->looping = loop;
    c->until = sAudioClock + plat_audio_play(c->file, volume, loop, track);
}

void res_audio_stop(const char *name)
{
    Clip *c = clip_find(name);
    if (c && (c->looping || c->until > sAudioClock)) {
        c->looping = 0;
        c->until = 0.f;
        plat_audio_stop(c->file);
    }
}

void res_audio_set_volume(const char *name, float volume)
{
    Clip *c = clip_find(name);
    if (c)
        plat_audio_clip_volume(c->file, volume);
}

/* ---- Lua bindings ---- */
static const char *arg_str(lua_State *L, int i)
{
    const char *s = lua_tostring(L, i);
    return s ? s : "";
}

static Anchor arg_anchor(lua_State *L, int first, Anchor a)
{
    int top = lua_gettop(L);
    if (top >= first) anchor_parse(&a, lua_tostring(L, first));
    if (top >= first + 1) anchor_parse(&a, lua_tostring(L, first + 1));
    return a;
}

static const AbSprite *find_sprite(const char *sheet, const char *sprite)
{
    if (!*sheet)
        return assets_find_sprite(NULL, sprite);
    {
        AbSheet *sh = assets_find_sheet(sheet);
        return sh ? assets_find_sprite(sh, sprite) : NULL;
    }
}

const AbSprite *res_find_sprite(const char *sheet, const char *sprite) { return find_sprite(sheet, sprite); }

static int l_createSpriteSheet(lua_State *L)
{
    char name[32];
    resource_name(arg_str(L, 1), name, sizeof name);
    assets_load_sheet(name);
    return 0;
}

static int l_releaseSpriteSheet(lua_State *L)
{
    char name[32];
    resource_name(arg_str(L, 1), name, sizeof name);
    assets_release_sheet(name);
    return 0;
}

static int l_createBitmapFont(lua_State *L)
{
    char name[32];
    resource_name(arg_str(L, 1), name, sizeof name);
    if (!font_create(name))
        plat_log("font %s: load failed", name);
    return 0;
}

static int l_createTextGroupSet(lua_State *L)
{
    const char *path = arg_str(L, 1);
    char name[32];
    int i;
    resource_name(path, name, sizeof name);
    if (textset_find(name))
        return 0;
    for (i = 0; i < MAX_TEXT_SETS; i++)
        if (!sTextSets[i].name[0]) {
            memset(&sTextSets[i], 0, sizeof(sTextSets[i]));
            strncpy(sTextSets[i].name, name, sizeof(sTextSets[i].name) - 1);
            snprintf(sTextSets[i].file, sizeof(sTextSets[i].file), "%s.dat", name);
            break;
        }
    return 0;
}

static int l_loadLocale(lua_State *L)
{
    TextSet *ts = textset_find(arg_str(L, 1));
    if (ts)
        textset_load(ts, arg_str(L, 2));
    return 0;
}

static int l_useLocale(lua_State *L)
{
    strncpy(sLocale, arg_str(L, 1), sizeof(sLocale) - 1);
    return 0;
}

static int l_useFont(lua_State *L)
{
    Font *f = font_find(arg_str(L, 1));
    if (f)
        sFont = f;                 /* unknown name: the previous font stays current */
    return 0;
}

static int l_drawSprite(lua_State *L)
{
    const AbSprite *s = find_sprite(arg_str(L, 1), arg_str(L, 2));
    Anchor a = { V_PIVOT, H_PIVOT };
    if (s)
        render_sprite(s, (float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4), arg_anchor(L, 5, a));
    return 0;
}

static int l_drawString(lua_State *L)
{
    Anchor a = { V_TOP, H_LEFT };
    draw_string(res_get_string(arg_str(L, 1), arg_str(L, 2)), (float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4),
                arg_anchor(L, 5, a));
    return 0;
}

static int l_getString(lua_State *L)
{
    lua_pushstring(L, res_get_string(arg_str(L, 1), arg_str(L, 2)));
    return 1;
}

static int l_getSpriteBounds(lua_State *L)
{
    const AbSprite *s = find_sprite(arg_str(L, 1), arg_str(L, 2));
    lua_pushnumber(L, s ? (lua_Number)s->w : 0);
    lua_pushnumber(L, s ? (lua_Number)s->h : 0);
    return 2;
}

static int l_getSpritePivot(lua_State *L)
{
    const AbSprite *s = find_sprite(arg_str(L, 1), arg_str(L, 2));
    lua_pushnumber(L, s ? (lua_Number)s->pivot_x : 0);
    lua_pushnumber(L, s ? (lua_Number)s->pivot_y : 0);
    return 2;
}

static int l_getStringWidth(lua_State *L)
{
    lua_pushnumber(L, (lua_Number)res_string_width(arg_str(L, 1), 0, 0x7fffffff));
    return 1;
}

static int l_getFontHeight(lua_State *L)        { lua_pushnumber(L, sFont ? (lua_Number)(sFont->max_asc + sFont->max_desc) : 0); return 1; }
static int l_getFontMaxAscending(lua_State *L)  { lua_pushnumber(L, sFont ? (lua_Number)sFont->max_asc : 0); return 1; }
static int l_getFontMaxDescending(lua_State *L) { lua_pushnumber(L, sFont ? (lua_Number)sFont->max_desc : 0); return 1; }
static int l_getFontLeading(lua_State *L)       { lua_pushnumber(L, sFont ? (lua_Number)sFont->leading : 0); return 1; }
static int l_getFontTracking(lua_State *L)      { lua_pushnumber(L, sFont ? (lua_Number)sFont->tracking : 0); return 1; }

/* createAudio(file, name) */
static int l_createAudio(lua_State *L)
{
    const char *file = arg_str(L, 1), *name = arg_str(L, 2);
    Clip *c = clip_find(name);
    if (!c && sClipCount < MAX_CLIPS)
        c = &sClips[sClipCount++];
    if (c) {
        memset(c, 0, sizeof(*c));
        strncpy(c->name, name, sizeof(c->name) - 1);
        strncpy(c->file, file, sizeof(c->file) - 1);
    }
    return 0;
}

/* playAudio(name [, volume = 1 [, loop = false [, track = 0]]]) */
static int l_playAudio(lua_State *L)
{
    int top = lua_gettop(L);
    res_audio_play(arg_str(L, 1), top >= 2 ? (float)lua_tonumber(L, 2) : 1.f,
                   top >= 3 ? lua_toboolean(L, 3) : 0, top >= 4 ? (int)lua_tonumber(L, 4) : 0);
    return 0;
}

static int l_stopAudio(lua_State *L)
{
    Clip *c = clip_find(arg_str(L, 1));
    if (c) {
        c->looping = 0;
        c->until = 0.f;
        plat_audio_stop(c->file);
    }
    return 0;
}

static int l_stopAllAudio(lua_State *L)
{
    int i;
    (void)L;
    for (i = 0; i < sClipCount; i++) {
        sClips[i].looping = 0;
        sClips[i].until = 0.f;
    }
    plat_audio_stop(NULL);
    return 0;
}

static int l_isAudioPlaying(lua_State *L)
{
    lua_pushboolean(L, res_audio_is_playing(arg_str(L, 1)));
    return 1;
}

/* setTrackVolume(volume, track) */
static int l_setTrackVolume(lua_State *L)
{
    float v = (float)lua_tonumber(L, 1);
    int track = (int)lua_tonumber(L, 2);
    if (!(v > 0.f)) v = 0.f;
    if (v > 1.f) v = 1.f;
    if (track >= 0 && track < 8)
        plat_audio_track_volume(track, v);
    return 0;
}

static int l_nop(lua_State *L) { (void)L; return 0; }
static int l_openURL(lua_State *L) { lua_pushboolean(L, 0); return 1; }

static const luaL_Reg reslib[] = {
    { "setPath", l_nop },
    { "createSpriteSheet", l_createSpriteSheet },
    { "createBitmapFont", l_createBitmapFont },
    { "createSystemFont", l_nop },
    { "createTextGroupSet", l_createTextGroupSet },
    { "createAudioOutput", l_nop },
    { "createAudioInput", l_nop },
    { "createAudio", l_createAudio },
    { "releaseSpriteSheet", l_releaseSpriteSheet },
    { "loadLocale", l_loadLocale },
    { "useLocale", l_useLocale },
    { "useFont", l_useFont },
    { "drawSprite", l_drawSprite },
    { "drawString", l_drawString },
    { "getString", l_getString },
    { "playAudio", l_playAudio },
    { "stopAudio", l_stopAudio },
    { "stopAllAudio", l_stopAllAudio },
    { "isAudioPlaying", l_isAudioPlaying },
    { "getSpriteBounds", l_getSpriteBounds },
    { "getSpritePivot", l_getSpritePivot },
    { "getStringWidth", l_getStringWidth },
    { "getFontMaxAscending", l_getFontMaxAscending },
    { "getFontMaxDescending", l_getFontMaxDescending },
    { "getFontLeading", l_getFontLeading },
    { "getFontTracking", l_getFontTracking },
    { "getFontHeight", l_getFontHeight },
    { "startAudioOutput", l_nop },
    { "stopAudioOutput", l_nop },
    { "startAudioInput", l_nop },
    { "stopAudioInput", l_nop },
    { "setMasterVolume", l_nop },
    { "setTrackVolume", l_setTrackVolume },
    { "openURL", l_openURL },
    { NULL, NULL }
};

void res_register(lua_State *L)
{
    luaL_register(L, "res", reslib);
    lua_pop(L, 1);
}
