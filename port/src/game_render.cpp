/* game_render.cpp — the drawing natives of GameLua (analysis/gamelua-render-spec.md):
 * the object passes, background / foreground parallax, trajectories, rectangles, lines
 * and text wrapping. Addresses in comments are those of the original functions. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

static const float kWorld = 20.0f;      /* world pixels per physics unit (hard-coded in the original) */
static const Anchor kPivot = { V_PIVOT, H_PIVOT };
uint32_t game_stat_rotation_builds, game_stat_rotation_hits;

static const char *arg_s(lua_State *L, int i) { const char *s = lua_tostring(L, i); return s ? s : ""; }
static float arg_n(lua_State *L, int i) { return (float)lua_tonumber(L, i); }

static void rs_rotation(float angle)
{
    /* Small exact-key cache shared by objects and script render states. Keeping
     * it outside ObjData preserves the object pool's allocation sizes. Float
     * bits distinguish +0 from -0 and never use a tolerance for moving objects. */
    struct Rotation { uint32_t bits; float c, s; bool valid; };
    static Rotation cache[32][2];
    static uint8_t victim[32];
    uint32_t bits;
    __builtin_memcpy(&bits, &angle, sizeof bits);
    unsigned slot = (bits ^ (bits >> 13) ^ (bits >> 23)) & 31u, way;
    Rotation *pair = cache[slot], *r;
    if (pair[0].valid && pair[0].bits == bits) way = 0;
    else if (pair[1].valid && pair[1].bits == bits) way = 1;
    else way = victim[slot];
    r = &pair[way];
    if (!r->valid || r->bits != bits) {
        r->c = cosf(angle); r->s = sinf(angle);
        r->bits = bits; r->valid = true;
        game_stat_rotation_builds++;
    } else {
        game_stat_rotation_hits++;
    }
    victim[slot] = (uint8_t)(way ^ 1u);
    gRS.m00 = r->c; gRS.m11 = r->c; gRS.m10 = r->s; gRS.m01 = -r->s;
}

static void rs_identity(void) { gRS.m00 = 1.f; gRS.m10 = 0.f; gRS.m01 = 0.f; gRS.m11 = 1.f; }

/* setRenderState(tx, ty [, sx, sy [, angle [, px, py]]]) @0000a960: fields not covered by the
 * argument count keep their values. */
static int l_setRenderState(lua_State *L)
{
    int n = lua_gettop(L);
    if (n >= 2) {
        gRS.tx = arg_n(L, 1);
        gRS.ty = arg_n(L, 2);
        if (n >= 4) {
            gRS.sx = arg_n(L, 3);
            gRS.sy = arg_n(L, 4);
        }
    }
    if (n >= 5) rs_rotation(arg_n(L, 5));
    if (n >= 7) {
        gRS.px = arg_n(L, 6);
        gRS.py = arg_n(L, 7);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Theme                                                                                */
/* ------------------------------------------------------------------------------------ */

static struct {
    float sky[3], ground[3];
    char sheetA[16], sheetB[16];
    char names[5][40];           /* <image>_LAYER_1..4, <image>_LAYER_GRASS */
    bool set;
} sTheme;

static float tbl_num(lua_State *L, const char *k)
{
    lua_getfield(L, -1, k);
    float v = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

/* setTheme(name) @000092b8: blockTable.themes[name] */
static int l_setTheme(lua_State *L)
{
    const char *name = arg_s(L, 1);
    char image[32] = "";
    int index = 1, top = lua_gettop(L);
    env_push();
    lua_getfield(L, -1, "blockTable");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "themes");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, name);
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "color");
                if (lua_istable(L, -1)) { sTheme.sky[0] = tbl_num(L, "r"); sTheme.sky[1] = tbl_num(L, "g"); sTheme.sky[2] = tbl_num(L, "b"); }
                lua_pop(L, 1);
                lua_getfield(L, -1, "groundColor");
                if (lua_istable(L, -1)) { sTheme.ground[0] = tbl_num(L, "r"); sTheme.ground[1] = tbl_num(L, "g"); sTheme.ground[2] = tbl_num(L, "b"); }
                lua_pop(L, 1);
                lua_getfield(L, -1, "image");
                if (lua_isstring(L, -1)) strncpy(image, lua_tostring(L, -1), sizeof(image) - 1);
                lua_pop(L, 1);
                index = (int)tbl_num(L, "index");
                snprintf(sTheme.sheetA, sizeof sTheme.sheetA, "INGAME_%d", index + 2);
                strcpy(sTheme.sheetB, "INGAME_2");
                for (int i = 0; i < 4; i++)
                    snprintf(sTheme.names[i], sizeof sTheme.names[i], "%s_LAYER_%d", image, i + 1);
                snprintf(sTheme.names[4], sizeof sTheme.names[4], "%s_LAYER_GRASS", image);
                sTheme.set = true;
            }
        }
    }
    lua_settop(L, top);
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Rectangles and lines                                                                 */
/* ------------------------------------------------------------------------------------ */

/* straight-alpha colour -> premultiplied, for the renderer's single blend mode */
static uint32_t premultiply(uint32_t argb)
{
    uint32_t a = argb >> 24, r = (argb >> 16) & 255u, g = (argb >> 8) & 255u, b = argb & 255u;
    r = (r * a + 127u) / 255u; g = (g * a + 127u) / 255u; b = (b * a + 127u) / 255u;
    return a << 24 | r << 16 | g << 8 | b;
}

/* gr::Context::fillRect2D @000ab0e0 */
static void fill_rect(uint32_t argb, int x, int y, int w, int h)
{
    render_quad(0, 0, 0, (float)x, (float)y, (float)w, (float)h, 0.f, 0.f, 0.f, 0.f, premultiply(argb));
}

/* drawRect(r, g, b, a, x1, y1, x2, y2, keepState) @00007898: colour 0..1; without keepState the
 * render state is reset (and stays reset) and the rectangle is in screen pixels. */
static void draw_rect(float r, float g, float b, float a, float x1, float y1, float x2, float y2, bool keepState)
{
    if (!keepState)
        rs_reset();
    uint32_t argb = (uint32_t)(int)(a * 255.f) << 24 | ((uint32_t)(int)(r * 255.f) & 255u) << 16 |
                    ((uint32_t)(int)(g * 255.f) & 255u) << 8 | ((uint32_t)(int)(b * 255.f) & 255u);
    fill_rect(argb, (int)x1, (int)y1, (int)(x2 - x1), (int)(y2 - y1));
}

static int l_drawRect(lua_State *L)
{
    draw_rect(arg_n(L, 1), arg_n(L, 2), arg_n(L, 3), arg_n(L, 4), arg_n(L, 5), arg_n(L, 6), arg_n(L, 7), arg_n(L, 8),
              lua_toboolean(L, 9) != 0);
    return 0;
}

/* drawLine2D(x1, y1, x2, y2, width, r, g, b, a) @00006044 -> gr::Context::drawLine2D @000ab444.
 * The original draws a GL line of width (int)(width * scale); here it is a quad of that
 * thickness, which is how a wide line looks anyway. */
static int l_drawLine2D(lua_State *L)
{
    int x1 = (int)arg_n(L, 1), y1 = (int)arg_n(L, 2), x2 = (int)arg_n(L, 3), y2 = (int)arg_n(L, 4);
    int width = (int)arg_n(L, 5);
    uint32_t argb = (uint32_t)(int)arg_n(L, 9) << 24 | ((uint32_t)(int)arg_n(L, 6) & 255u) << 16 |
                    ((uint32_t)(int)arg_n(L, 7) & 255u) << 8 | ((uint32_t)(int)arg_n(L, 8) & 255u);
    float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
    if (dx == 0.f && dy == 0.f)
        return 0;
    float ax, ay, bx, by;
    rs_corner((float)x1, (float)y1, 0.f, 0.f, &ax, &ay);
    rs_corner((float)x1, (float)y1, dx, dy, &bx, &by);
    float len = sqrtf(dx * dx + dy * dy), nx = dx / len, ny = dy / len;
    float w = (float)(int)(ny * ny * (float)width * gRS.sx + nx * nx * (float)width * gRS.sy);
    if (w < 1.f) w = 1.f;
    /* perpendicular in screen space */
    float sx = bx - ax, sy = by - ay, sl = sqrtf(sx * sx + sy * sy);
    if (sl <= 0.f)
        return 0;
    float px = -sy / sl * w * 0.5f, py = sx / sl * w * 0.5f;
    float xy[8] = { ax + px, ay + py, bx + px, by + py, bx - px, by - py, ax - px, ay - py };
    static const float uv[8] = { 0 };
    gfx_quad(0, 0, 0, xy, uv, premultiply(argb));
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Background and foreground                                                            */
/* ------------------------------------------------------------------------------------ */

static uint32_t clamp_byte(float v) { return v < 0.f ? 0u : v > 255.f ? 255u : (uint32_t)(int)v; }

/* drawBackgroundNative() @0000a5d8 */
static int l_drawBackground(lua_State *L)
{
    (void)L;
    if (!sTheme.set)
        return 0;
    float ws = G.worldScale, W = gScreenW, H = gScreenH;
    const AbSprite *l1 = res_find_sprite(sTheme.sheetB, sTheme.names[0]);
    int h1 = l1 ? l1->h : 0, py1 = l1 ? l1->pivot_y : 0;

    G.bgColor = clamp_byte(sTheme.sky[0]) << 16 | clamp_byte(sTheme.sky[1]) << 8 | clamp_byte(sTheme.sky[2]);

    /* ground colour below the foreground ground strip */
    float yTop = -((G.topLeftY - (float)(h1 - py1) * 1.5f) * ws);
    draw_rect(sTheme.ground[0] / 255.f, sTheme.ground[1] / 255.f, sTheme.ground[2] / 255.f, 1.f, 0.f, yTop, W, H + 1.f, false);

    /* two parallax layers at twice the sprite size: LAYER_4 (1/8 scroll), LAYER_2 (1/2 scroll) */
    gRS.sx = gRS.sy = ws + ws;
    float tyAll = G.topLeftY * -0.5f;
    float halfW = (W * 0.5f) / ws;
    for (int pass = 0; pass < 2; pass++) {
        const char *sheet = pass == 0 ? sTheme.sheetB : sTheme.sheetA;
        const char *layer = sTheme.names[pass == 0 ? 3 : 1];
        float f = pass == 0 ? 0.125f : 0.5f;
        const AbSprite *s = res_find_sprite(sheet, layer);
        if (!s) continue;
        float limit = (float)s->pivot_x + halfW;
        float wf = (float)s->w, period = wf + wf;
        float m = fmodf(f * G.topLeftX, period);
        float v = (period + m) * -0.5f;
        int i = (int)v;
        float x = (float)i;
        gRS.ty = tyAll;
        gRS.tx = v - x;
        while (limit > x) {
            render_sprite(s, x, 0.f, kPivot);
            x += wf;
        }
    }
    return 0;
}

/* drawForegroundNative() @00006ba0: the ground strip and the grass, at 1.5x, over the objects */
static int l_drawForeground(lua_State *L)
{
    (void)L;
    if (!sTheme.set)
        return 0;
    const AbSprite *l1 = res_find_sprite(sTheme.sheetB, sTheme.names[0]);
    const AbSprite *grass = res_find_sprite(sTheme.sheetA, sTheme.names[4]);
    if (!l1)
        return 0;
    float s = G.worldScale * 1.5f;
    gRS.sx = gRS.sy = s;
    rs_identity();
    int w = l1->w, px = l1->pivot_x;
    float limit = gScreenW / s + (float)px;
    float period = (float)w * 1.5f;
    float m = fmodf(G.topLeftX, period);
    gRS.ty = -G.topLeftY / 1.5f;
    float v = -(period + m) / 1.5f;
    int i = (int)v;
    gRS.tx = v - (float)i;
    if (limit > (float)i) {
        for (int x = i; limit > (float)x; x += w)
            render_sprite(l1, (float)x, 0.f, kPivot);
        if (grass)
            for (int x = i; limit > (float)x; x += w)
                render_sprite(grass, (float)x, 0.f, kPivot);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Trajectories                                                                         */
/* ------------------------------------------------------------------------------------ */

struct Trail { float *pts; int size, cap; };
static Trail sTrails[6];
static int sTrailBase;

/* startNewTrajectory() @0000aa64: two groups of three trails; the previous shot is kept */
static int l_startNewTrajectory(lua_State *L)
{
    (void)L;
    sTrailBase += 3;
    if (sTrailBase > 3) sTrailBase = 0;
    for (int i = 0; i < 3; i++) sTrails[sTrailBase + i].size = 0;
    return 0;
}

/* addToTrajectory(which, x, y) @00008ff8 */
static int l_addToTrajectory(lua_State *L)
{
    int idx = (sTrailBase - 1) + (int)arg_n(L, 1);
    if (idx < 0 || idx > 5)
        return 0;
    Trail *t = &sTrails[idx];
    if (t->size == t->cap) {
        int cap = t->cap ? t->cap * 2 : 16;
        float *p = (float *)realloc(t->pts, sizeof(float) * 2 * (size_t)cap);
        if (!p) return 0;
        t->pts = p;
        t->cap = cap;
    }
    t->pts[t->size * 2] = arg_n(L, 2);
    t->pts[t->size * 2 + 1] = arg_n(L, 3);
    t->size++;
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Textured static blocks (MaskedImage in the original)                                 */
/* ------------------------------------------------------------------------------------ */

/* The original draws these with two texture units: the tiling ground texture, anchored to
 * the world, with the block sprite's alpha as a mask. They never move, so the port bakes
 * each one once into small shared pages and draws it as an ordinary sprite.
 * Generated textures must obey the same device limits as converted asset pages. */
#define MASK_ATLAS_W 256
#define MASK_ATLAS_H 128
#define MASK_PAGES   8
#define MAX_MASKS    64

struct MaskRect { int x, y, w, h, page; };
static MaskRect sMasks[MAX_MASKS];
static int sMaskCount, sMaskTextures[MASK_PAGES];
static bool sMaskDirty;
uint32_t game_stat_mask_builds;

void render_object_removed(ObjData *d)
{
    if (d->maskSlot >= 0) {
        d->maskSlot = -1;
        /* its atlas space is reclaimed at the next level load */
    }
    if (d->texture) sMaskDirty = true;
}

static void masks_drop(void)
{
    for (int i = 0; i < MASK_PAGES; i++) {
        if (sMaskTextures[i]) gfx_tex_destroy(sMaskTextures[i]);
        sMaskTextures[i] = 0;
    }
    sMaskCount = 0;
    ObjIter it;
    for (ObjNode *n = obj_first(&it); n; n = obj_next(&it))
        n->d.maskSlot = -1;
}

struct PageCache { char file[48]; uint16_t *px; int w, h; };

static uint16_t *page_load(PageCache *c, const AbSprite *s)
{
    char file[48];
    int tex_w, tex_h;
    snprintf(file, sizeof file, "%s.p%d", assets_sheet_name(s->sheet), (int)s->page);
    assets_page_size(s, &tex_w, &tex_h);
    if (!c->px || strcmp(c->file, file)) {
        uint32_t size = 0;
        free(c->px);
        c->px = (uint16_t *)plat_read_file(file, &size, 0);
        if (c->px && size < (uint32_t)tex_w * (uint32_t)tex_h * 2u) { free(c->px); c->px = NULL; }
        strcpy(c->file, file);
        c->w = tex_w;
        c->h = tex_h;
    }
    return c->px;
}

static void mask_upload(int page, const uint16_t *pixels)
{
    if (assets_make_room(MASK_ATLAS_W * MASK_ATLAS_H * 2u))
        sMaskTextures[page] = gfx_tex_create(MASK_ATLAS_W, MASK_ATLAS_H,
                                            GFX_FMT_RGBA4444, pixels, 1, 0);
}

/* Build the pages for every textured static block of the level. Only one page's
 * pixels are kept in CPU memory at a time, even for levels with many hills. */
static void masks_build(void)
{
    game_stat_mask_builds++;
    masks_drop();
    sMaskDirty = false;
    uint16_t *atlas = NULL;
    PageCache mask = { "", NULL, 0, 0 }, ground = { "", NULL, 0, 0 };
    int shelfX = 0, shelfY = 0, shelfH = 0, page = 0;
    ObjIter it;
    for (ObjNode *n = obj_first(&it); n; n = obj_next(&it)) {
        ObjData *d = &n->d;
        if (!d->texture || !d->sprite || d->body->GetMass() != 0.0f || sMaskCount >= MAX_MASKS)
            continue;
        const AbSprite *s = d->sprite;
        const AbSprite *g = assets_first_sprite(d->texture);   /* the ground texture fills its sheet */
        if (!g) continue;
        int w = s->tw, h = s->th;
        /* Also support assets converted with a larger host page profile. */
        if (w > MASK_ATLAS_W - 2 || h > MASK_ATLAS_H - 2) {
            float scaleX = (float)(MASK_ATLAS_W - 2) / w;
            float scaleY = (float)(MASK_ATLAS_H - 2) / h;
            float scale = scaleX < scaleY ? scaleX : scaleY;
            w = (int)(w * scale); h = (int)(h * scale);
        }
        if (w < 1 || h < 1 || !g->tw || !g->th) continue;
        if (shelfX + w + 2 > MASK_ATLAS_W) { shelfX = 0; shelfY += shelfH; shelfH = 0; }
        if (shelfY + h + 2 > MASK_ATLAS_H) {
            if (page + 1 == MASK_PAGES) continue; /* no room: drawn untextured */
            if (atlas) {
                mask_upload(page, atlas);
                memset(atlas, 0, MASK_ATLAS_W * MASK_ATLAS_H * 2u);
            }
            page++;
            shelfX = shelfY = shelfH = 0;
        }
        uint16_t *mp = page_load(&mask, s), *gp = page_load(&ground, g);
        if (!mp || !gp) continue;
        if (!atlas) {
            atlas = (uint16_t *)calloc((size_t)MASK_ATLAS_W * MASK_ATLAS_H, 2);
            if (!atlas) break;
        }
        /* Key the bake on the actual static body pose. Interpolated render
         * coordinates can vary by an ULP even when both snapshots are identical. */
        const b2Vec2 pos = d->body->GetPosition();
        const float angle = d->body->GetAngle();
        float c = cosf(angle), sn = sinf(angle);
        float wx0 = pos.x * kWorld, wy0 = pos.y * kWorld;
        float gsx = (float)g->tw / (float)g->w, gsy = (float)g->th / (float)g->h;   /* ground texels per world px */
        int ox0 = shelfX + 1, oy0 = shelfY + 1;
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                int mx = i * s->tw / w, my = j * s->th / h;
                uint16_t m = mp[(s->ty + my) * mask.w + s->tx + mx];
                uint32_t a = m & 15u;
                if (!a) continue;
                /* texel centre -> sprite-local game pixels -> world pixels */
                float lx = ((float)i + 0.5f) * (float)s->w / (float)w - (float)s->pivot_x;
                float ly = ((float)j + 0.5f) * (float)s->h / (float)h - (float)s->pivot_y;
                float wx = wx0 + c * lx - sn * ly, wy = wy0 + sn * lx + c * ly;
                int gx = (int)floorf(wx * gsx), gy = (int)floorf(wy * gsy);
                gx = ((gx % g->tw) + g->tw) % g->tw;
                gy = ((gy % g->th) + g->th) % g->th;
                uint16_t t = gp[(g->ty + gy) * ground.w + g->tx + gx];        /* RGB565 */
                uint32_t r = (t >> 11) & 31u, gr = (t >> 5) & 63u, b = t & 31u;
                /* -> premultiplied RGBA4444 */
                r = (r * a * 15u + 232u) / 465u;
                gr = (gr * a * 15u + 472u) / 945u;
                b = (b * a * 15u + 232u) / 465u;
                atlas[(oy0 + j) * MASK_ATLAS_W + ox0 + i] = (uint16_t)(r << 12 | gr << 8 | b << 4 | a);
            }
        }
        sMasks[sMaskCount].x = ox0;
        sMasks[sMaskCount].y = oy0;
        sMasks[sMaskCount].w = w;
        sMasks[sMaskCount].h = h;
        sMasks[sMaskCount].page = page;
        d->maskSlot = sMaskCount++;
        d->maskX = pos.x;
        d->maskY = pos.y;
        d->maskAngle = angle;
        shelfX += w + 2;
        if (h + 2 > shelfH) shelfH = h + 2;
    }
    free(mask.px);
    free(ground.px);
    if (atlas) {
        mask_upload(page, atlas);
        free(atlas);
    }
}

void render_level_reset(void)
{
    for (int i = 0; i < 6; i++) sTrails[i].size = 0;
    masks_drop();
    sMaskDirty = true;
}

/* ------------------------------------------------------------------------------------ */
/* drawGameNative() @00009ad4                                                           */
/* ------------------------------------------------------------------------------------ */

static void object_xform(const ObjData *e)
{
    rs_rotation(e->renderAngle);
    gRS.px = (float)e->sprite->pivot_x;
    gRS.py = (float)e->sprite->pivot_y;
}

static int l_drawGame(lua_State *L)
{
    (void)L;
    ObjIter it;
    gRS.tx = -G.topLeftX;
    gRS.ty = -G.topLeftY;
    gRS.sx = gRS.sy = G.worldScale;

    /* pass 1: static objects; the textured ones after all the plain ones */
    ObjData *masked[MAX_MASKS];
    int nmasked = 0;
    bool stale = sMaskDirty;
    for (ObjNode *n = obj_first(&it); n; n = obj_next(&it)) {
        ObjData *e = &n->d;
        if (!e->sprite || e->body->GetMass() != 0.0f) continue;
        if (!e->texture) {
            object_xform(e);
            render_sprite(e->sprite, e->renderX * kWorld, e->renderY * kWorld, kPivot);
        } else if (nmasked < MAX_MASKS) {
            masked[nmasked++] = e;
            const b2Vec2 pos = e->body->GetPosition();
            if (e->maskSlot >= 0 && (e->maskX != pos.x || e->maskY != pos.y || e->maskAngle != e->body->GetAngle()))
                stale = true;                   /* moved (level editor): bake again */
        }
    }
    if (nmasked) {
        if (stale) masks_build();
        for (int i = 0; i < nmasked; i++) {
            ObjData *e = masked[i];
            const AbSprite *s = e->sprite;
            object_xform(e);
            float left = e->renderX * kWorld - (float)s->pivot_x, top = e->renderY * kWorld - (float)s->pivot_y;
            if (e->maskSlot >= 0 && sMaskTextures[sMasks[e->maskSlot].page]) {
                const MaskRect *r = &sMasks[e->maskSlot];
                render_quad(sMaskTextures[r->page], MASK_ATLAS_W, MASK_ATLAS_H, left, top, (float)s->w, (float)s->h,
                            (float)r->x, (float)r->y, (float)(r->x + r->w), (float)(r->y + r->h), 0xFFFFFFFFu);
            } else {
                render_image(s, left, top);
            }
        }
    }

    /* pass 2: dynamic objects that are neither birds nor "front" */
    for (ObjNode *n = obj_first(&it); n; n = obj_next(&it)) {
        ObjData *e = &n->d;
        if (e->sprite && !e->controllable && e->body->GetMass() != 0.0f && !e->front) {
            object_xform(e);
            render_sprite(e->sprite, e->renderX * kWorld, e->renderY * kWorld, kPivot);
        }
    }

    /* trajectories of the current and the previous shot */
    rs_identity();
    static const char *const dots[3] = { "SMOKE_TRAIL_1", "SMOKE_TRAIL_2", "SMOKE_TRAIL_3" };
    const AbSprite *dot[3];
    for (int i = 0; i < 3; i++) dot[i] = res_find_sprite("INGAME", dots[i]);
    for (int t = 0; t < 6; t++)
        for (int i = 0; i < sTrails[t].size; i++)
            if (dot[i % 3])
                render_sprite(dot[i % 3], sTrails[t].pts[i * 2], sTrails[t].pts[i * 2 + 1], kPivot);

    /* pass 3: birds and "front" objects (pigs) */
    for (ObjNode *n = obj_first(&it); n; n = obj_next(&it)) {
        ObjData *e = &n->d;
        if (e->sprite && (e->controllable || e->front)) {
            object_xform(e);
            render_sprite(e->sprite, e->renderX * kWorld, e->renderY * kWorld, kPivot);
        }
    }

    particles_draw();
    return 0;
}

static int l_drawParticles(lua_State *L) { (void)L; particles_draw(); return 0; }

/* ------------------------------------------------------------------------------------ */
/* clipText(group, id, maxWidth) @00006da4                                              */
/* ------------------------------------------------------------------------------------ */

static unsigned code_at(const char *text, int index)
{
    const unsigned char *p = (const unsigned char *)res_utf16_offset(text, index);
    unsigned c = *p;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) return ((c & 0x1F) << 6) | (p[1] & 0x3F);
    return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
}

static int l_clipText(lua_State *L)
{
    const char *text = res_get_string(arg_s(L, 1), arg_s(L, 2));
    float maxWidth = arg_n(L, 3);
    int n = res_utf16_length(text), widest = 0, lineStart = 0, pos = 0, nlines = 0;

    lua_newtable(L);                            /* lines */
    while (pos < n) {
        int lineEnd = n, wordCount = 0, count = 0;
        for (;;) {
            int i = pos;
            unsigned ch = 0;
            while (i < n && (ch = code_at(text, i)) != '\n' && ch != ' ' && ch != '-') i++;
            if (i < n && code_at(text, i) == '-') i++;          /* a hyphen stays with its word */
            int wordEnd = i;
            count = wordEnd - lineStart;
            if (!((float)res_string_width(text, lineStart, count) < maxWidth)) {
                if (wordCount == 0) {
                    /* a single word wider than the box: break it by characters */
                    int k = lineStart;
                    do { k++; } while (k <= n && maxWidth > (float)res_string_width(text, lineStart, k - lineStart));
                    k--;
                    if (k <= lineStart) k = lineStart + 1;       /* the original would loop forever */
                    count = k - lineStart;
                    lineEnd = k;
                    pos = k;
                } else {
                    count = lineEnd - lineStart;                 /* up to the end of the previous word */
                }
                break;
            }
            if (i >= n) { pos = wordEnd; lineEnd = wordEnd; break; }
            if (code_at(text, i) == '\n') { pos = wordEnd; lineEnd = wordEnd; break; }
            pos = i;
            while (pos < n && code_at(text, pos) == ' ') pos++;
            lineEnd = wordEnd;
            if (pos >= n) break;
            wordCount++;
        }
        int w = res_string_width(text, lineStart, count);
        if (w > widest) widest = w;
        const char *a = res_utf16_offset(text, lineStart), *b = res_utf16_offset(text, lineEnd);
        lua_pushlstring(L, a, (size_t)(b - a));
        lua_rawseti(L, -2, ++nlines);
        if (pos < n && code_at(text, pos) == '\n') pos++;
        if (pos >= n) break;
        lineStart = pos;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, G.clippedTextRef);
    lua_pushnumber(L, (lua_Number)widest);
    lua_setfield(L, -2, "widestLine");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "lines");
    lua_pop(L, 2);
    return 0;
}

static int l_nop(lua_State *L) { (void)L; return 0; }

static const luaL_Reg kRenderNatives[] = {
    { "setRenderState", l_setRenderState },
    { "drawGameNative", l_drawGame },
    { "drawBackgroundNative", l_drawBackground },
    { "drawForegroundNative", l_drawForeground },
    { "drawParticlesNative", l_drawParticles },
    { "setTheme", l_setTheme },
    { "drawRect", l_drawRect },
    { "drawTexturedRect", l_nop },              /* not used by the 1.0 scripts */
    { "drawLine2D", l_drawLine2D },
    { "clipText", l_clipText },
    { "startNewTrajectory", l_startNewTrajectory },
    { "addToTrajectory", l_addToTrajectory },
    { NULL, NULL }
};

/* The environment table is on the stack top. */
void render_register(lua_State *L)
{
    luaL_register(L, NULL, kRenderNatives);
}
