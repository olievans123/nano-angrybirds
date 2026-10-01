/* assets.c — converted sprite sheets with lazily loaded, LRU-evicted texture pages. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "assets.h"
#include "gfx.h"
#include "platform.h"

#define MAX_SHEETS 32
#define MAX_PAGES  8
#define HASH_SIZE  2048        /* > 2x the ~460 sprites of the game */

typedef struct {
    uint16_t w, h;
    uint8_t  fmt, repeat;
    int      tex;              /* gfx texture, 0 = not loaded */
    uint32_t last_used;        /* frame */
    uint8_t  failed;
} Page;

struct AbSheet {
    char      name[32];
    int       nsprites, npages;
    int       image_w, image_h;
    float     scale;
    Page      pages[MAX_PAGES];
    AbSprite *sprites;
};

static AbSheet sSheets[MAX_SHEETS];
static const AbSprite *sHash[HASH_SIZE];
static uint32_t sFrame = 1;
static uint32_t sBudget = 3u << 20;   /* leave space for driver allocations */

static uint32_t hash_name(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void hash_rebuild(void)
{
    int s, i;
    memset(sHash, 0, sizeof(sHash));
    for (s = 0; s < MAX_SHEETS; s++) {
        AbSheet *sh = &sSheets[s];
        if (!sh->sprites)
            continue;
        for (i = 0; i < sh->nsprites; i++) {
            uint32_t h = hash_name(sh->sprites[i].name) % HASH_SIZE;
            /* first sheet created wins when two sheets define the same name */
            while (sHash[h] && strcmp(sHash[h]->name, sh->sprites[i].name))
                h = (h + 1) % HASH_SIZE;
            if (!sHash[h])
                sHash[h] = &sh->sprites[i];
        }
    }
}

AbSheet *assets_find_sheet(const char *name)
{
    int s;
    for (s = 0; s < MAX_SHEETS; s++)
        if (sSheets[s].sprites && !strcmp(sSheets[s].name, name))
            return &sSheets[s];
    return NULL;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

AbSheet *assets_load_sheet(const char *name)
{
    AbSheet *sh = assets_find_sheet(name);
    char path[64];
    uint8_t *d;
    const uint8_t *p;
    uint32_t size = 0;
    int s, i;
    if (sh)
        return sh;
    for (s = 0; s < MAX_SHEETS && sSheets[s].sprites; s++) {}
    if (s == MAX_SHEETS)
        return NULL;
    sh = &sSheets[s];
    snprintf(path, sizeof path, "%s.sht", name);
    d = (uint8_t *)plat_read_file(path, &size, 0);
    if (!d || size < 16 || memcmp(d, "ABS1", 4)) {
        plat_log("sheet %s: missing or bad", path);
        free(d);
        return NULL;
    }
    memset(sh, 0, sizeof(*sh));
    strncpy(sh->name, name, sizeof(sh->name) - 1);
    sh->nsprites = rd16(d + 4);
    sh->npages = rd16(d + 6);
    sh->image_w = rd16(d + 8);
    sh->image_h = rd16(d + 10);
    memcpy(&sh->scale, d + 12, 4);
    if (sh->npages > MAX_PAGES || size < 16u + (uint32_t)sh->npages * 8u + (uint32_t)sh->nsprites * 62u) {
        plat_log("sheet %s: truncated", path);
        free(d);
        return NULL;
    }
    p = d + 16;
    for (i = 0; i < sh->npages; i++, p += 8) {
        sh->pages[i].w = rd16(p);
        sh->pages[i].h = rd16(p + 2);
        sh->pages[i].fmt = p[4];
        sh->pages[i].repeat = p[5];
    }
    sh->sprites = (AbSprite *)calloc((size_t)sh->nsprites, sizeof(AbSprite));
    if (!sh->sprites) {
        free(d);
        return NULL;
    }
    for (i = 0; i < sh->nsprites; i++, p += 62) {
        AbSprite *sp = &sh->sprites[i];
        memcpy(sp->name, p, 40);
        sp->name[39] = 0;
        sp->x = (int16_t)rd16(p + 40);
        sp->y = (int16_t)rd16(p + 42);
        sp->w = rd16(p + 44);
        sp->h = rd16(p + 46);
        sp->pivot_x = (int16_t)rd16(p + 48);
        sp->pivot_y = (int16_t)rd16(p + 50);
        sp->page = rd16(p + 52);
        sp->tx = rd16(p + 54);
        sp->ty = rd16(p + 56);
        sp->tw = rd16(p + 58);
        sp->th = rd16(p + 60);
        sp->sheet = sh;
    }
    free(d);
    hash_rebuild();
    return sh;
}

void assets_release_sheet(const char *name)
{
    AbSheet *sh = assets_find_sheet(name);
    int i;
    if (!sh)
        return;
    for (i = 0; i < sh->npages; i++)
        if (sh->pages[i].tex)
            gfx_tex_destroy(sh->pages[i].tex);
    free(sh->sprites);
    memset(sh, 0, sizeof(*sh));
    hash_rebuild();
}

const char *assets_sheet_name(const AbSheet *sheet) { return sheet->name; }

void assets_sheet_image_size(const AbSheet *sheet, int *w, int *h)
{
    *w = sheet->image_w;
    *h = sheet->image_h;
}

const AbSprite *assets_find_sprite(const AbSheet *sheet, const char *sprite)
{
    uint32_t h;
    int i;
    if (sheet) {
        /* usually the global table already points into this sheet */
        h = hash_name(sprite) % HASH_SIZE;
        while (sHash[h]) {
            if (!strcmp(sHash[h]->name, sprite)) {
                if (sHash[h]->sheet == sheet)
                    return sHash[h];
                break;
            }
            h = (h + 1) % HASH_SIZE;
        }
        for (i = 0; i < sheet->nsprites; i++)
            if (!strcmp(sheet->sprites[i].name, sprite))
                return &sheet->sprites[i];
        return NULL;
    }
    h = hash_name(sprite) % HASH_SIZE;
    while (sHash[h]) {
        if (!strcmp(sHash[h]->name, sprite))
            return sHash[h];
        h = (h + 1) % HASH_SIZE;
    }
    return NULL;
}

/* Drop least-recently-used pages (never one used this frame) until `need` more bytes fit. */
int assets_make_room(uint32_t need)
{
    while (gfx_stat_tex_bytes + need > sBudget) {
        Page *victim = NULL;
        int s, i;
        for (s = 0; s < MAX_SHEETS; s++) {
            AbSheet *sh = &sSheets[s];
            if (!sh->sprites)
                continue;
            for (i = 0; i < sh->npages; i++) {
                Page *pg = &sh->pages[i];
                if (pg->tex && pg->last_used != sFrame && (!victim || pg->last_used < victim->last_used))
                    victim = pg;
            }
        }
        if (!victim)
            return 0;           /* everything loaded is in use this frame */
        gfx_tex_destroy(victim->tex);
        victim->tex = 0;
    }
    return 1;
}

/* Mark the sprites of a just-read page that have no transparent texel, so they can be
 * drawn without blending. */
static void mark_opaque(AbSheet *sh, int page, const Page *pg, const uint16_t *px)
{
    int i, x, y;
    for (i = 0; i < sh->nsprites; i++) {
        AbSprite *sp = &sh->sprites[i];
        if (sp->page != page || sp->tx + sp->tw > pg->w || sp->ty + sp->th > pg->h)
            continue;
        sp->opaque = 1;
        if (!pg->fmt)
            continue;                           /* RGB565: no alpha at all */
        for (y = 0; y < sp->th && sp->opaque; y++) {
            const uint16_t *row = px + (size_t)(sp->ty + y) * pg->w + sp->tx;
            for (x = 0; x < sp->tw; x++)
                if ((row[x] & 15u) != 15u) { sp->opaque = 0; break; }
        }
    }
}

int assets_sprite_texture(const AbSprite *sprite, int *tex_w, int *tex_h)
{
    AbSheet *sh = sprite->sheet;
    Page *pg = &sh->pages[sprite->page];
    *tex_w = pg->w;
    *tex_h = pg->h;
    pg->last_used = sFrame;
    if (!pg->tex && !pg->failed) {
        char path[64];
        uint32_t size = 0, want = (uint32_t)pg->w * pg->h * 2u;
        void *px;
        if (!assets_make_room(want)) {
            plat_log("texture budget: cannot fit %s.p%d", sh->name, (int)sprite->page);
            return 0;
        }
        snprintf(path, sizeof path, "%s.p%d", sh->name, (int)sprite->page);
        port_checkpoint("texture-read", path, want);
        px = plat_read_file(path, &size, 0);
        if (px && size >= want) {
            pg->tex = gfx_tex_create(pg->w, pg->h, pg->fmt ? GFX_FMT_RGBA4444 : GFX_FMT_RGB565, px, 1, pg->repeat);
            if (pg->tex)
                mark_opaque(sh, sprite->page, pg, (const uint16_t *)px);
        }
        if (!pg->tex) {
            plat_log("texture page %s: load failed", path);
            pg->failed = 1;
        }
        free(px);
    }
    return pg->tex;
}

void assets_page_size(const AbSprite *sprite, int *tex_w, int *tex_h)
{
    const Page *pg = &sprite->sheet->pages[sprite->page];
    *tex_w = pg->w;
    *tex_h = pg->h;
}

const AbSprite *assets_first_sprite(const AbSheet *sheet)
{
    return sheet && sheet->nsprites ? &sheet->sprites[0] : NULL;
}

/* Test hook: which page each loaded texture is. */
void assets_debug_pages(void)
{
    int s, i;
    for (s = 0; s < MAX_SHEETS; s++)
        for (i = 0; sSheets[s].sprites && i < sSheets[s].npages; i++)
            if (sSheets[s].pages[i].tex)
                plat_log("texture %u = %s.p%d %dx%d %s", gfx_tex_gl_name(sSheets[s].pages[i].tex), sSheets[s].name, i,
                         sSheets[s].pages[i].w, sSheets[s].pages[i].h, sSheets[s].pages[i].fmt ? "4444" : "565");
}

void assets_begin_frame(void) { sFrame++; }

void assets_gl_lost(void)
{
    int s, i;
    for (s = 0; s < MAX_SHEETS; s++)
        for (i = 0; i < MAX_PAGES; i++) {
            sSheets[s].pages[i].tex = 0;
            sSheets[s].pages[i].failed = 0;
        }
}

void assets_set_texture_budget(uint32_t bytes) {
    sBudget = bytes;
    gfx_set_texture_budget(bytes);
}
