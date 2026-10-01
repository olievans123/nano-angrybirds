/* render.c — sprites through the 2D render state (EGL_Image::draw @000b15c4). */
#include <string.h>
#include "render.h"
#include "gfx.h"

RenderState2D gRS = { 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f };
float gScreenW = 480.f, gScreenH = 320.f;

void rs_reset(void)
{
    gRS.m00 = 1.f; gRS.m10 = 0.f; gRS.m01 = 0.f; gRS.m11 = 1.f;
    gRS.tx = gRS.ty = 0.f;
    gRS.sx = gRS.sy = 1.f;
    gRS.px = gRS.py = 0.f;
}

void anchor_parse(Anchor *a, const char *s)
{
    if (!s || !*s) return;
    if (!strcmp(s, "TOP")) a->v = V_TOP;
    else if (!strcmp(s, "VCENTER")) a->v = V_CENTER;
    else if (!strcmp(s, "BOTTOM")) a->v = V_BOTTOM;
    else if (!strcmp(s, "BASELINE")) a->v = V_BASELINE;
    else if (!strcmp(s, "VPIVOT")) a->v = V_PIVOT;
    else if (!strcmp(s, "LEFT")) a->h = H_LEFT;
    else if (!strcmp(s, "HCENTER")) a->h = H_CENTER;
    else if (!strcmp(s, "RIGHT")) a->h = H_RIGHT;
    else if (!strcmp(s, "HPIVOT")) a->h = H_PIVOT;
}

/* screen = (R * (corner - pivot) + topLeft + pivot + translate) * scale   (EGL_Image::draw) */
void rs_corner(float left, float top, float cx, float cy, float *x, float *y)
{
    float ox = cx - gRS.px, oy = cy - gRS.py;
    *x = (left + gRS.px + gRS.tx + ox * gRS.m00 + oy * gRS.m01) * gRS.sx;
    *y = (top + gRS.py + gRS.ty + ox * gRS.m10 + oy * gRS.m11) * gRS.sy;
}

static int quad_geometry(float left, float top, float w, float h, float xy[8])
{
    static const float cornerx[4] = { 0.f, 1.f, 1.f, 0.f }, cornery[4] = { 0.f, 0.f, 1.f, 1.f };
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    int i;
    for (i = 0; i < 4; i++) {
        float x, y;
        rs_corner(left, top, cornerx[i] * w, cornery[i] * h, &x, &y);
        xy[i * 2] = x;
        xy[i * 2 + 1] = y;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    /* the original's cull test, which also rejects NaN coordinates */
    return maxx >= 0.f && maxy >= 0.f && minx < gScreenW && miny < gScreenH;
}

void render_quad(int tex, int tex_w, int tex_h, float left, float top, float w, float h,
                 float u0, float v0, float u1, float v1, uint32_t argb)
{
    float xy[8], uv[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
    if (!quad_geometry(left, top, w, h, xy)) return;
    gfx_quad(tex, tex_w, tex_h, xy, uv, argb);
}

void render_image_part(const AbSprite *s, float left, float top, float sx, float sy, float sw, float sh)
{
    float fu = (float)s->tw / (float)s->w, fv = (float)s->th / (float)s->h;   /* texels per game pixel */
    int tw, th, tex;
    float xy[8];
    /* cull before touching the texture, so sprites off screen never load a page */
    if (!quad_geometry(left, top, sw, sh, xy)) return;
    tex = assets_sprite_texture(s, &tw, &th);
    if (tex) {
        float u0 = (float)s->tx + sx * fu, v0 = (float)s->ty + sy * fv;
        float u1 = (float)s->tx + (sx + sw) * fu, v1 = (float)s->ty + (sy + sh) * fv;
        float uv[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
        if (s->opaque && gfx_opaque_fast) {
            gfx_set_blend(GFX_BLEND_NONE);
            gfx_quad(tex, tw, th, xy, uv, 0xFFFFFFFFu);
            gfx_set_blend(GFX_BLEND_ALPHA);
        } else {
            gfx_quad(tex, tw, th, xy, uv, 0xFFFFFFFFu);
        }
    }
}

void render_image(const AbSprite *s, float left, float top)
{
    render_image_part(s, left, top, 0.f, 0.f, (float)s->w, (float)s->h);
}

void render_sprite(const AbSprite *s, float x, float y, Anchor a)
{
    float left, top;
    switch (a.h) {
        case H_LEFT:   left = x; break;
        case H_CENTER: left = x - (float)(s->w >> 1); break;
        case H_RIGHT:  left = x - (float)s->w; break;
        default:       left = x - (float)s->pivot_x; break;
    }
    switch (a.v) {
        case V_CENTER: top = y - (float)(s->h >> 1); break;
        case V_BOTTOM: top = y - (float)s->h; break;
        case V_PIVOT:  top = y - (float)s->pivot_y; break;
        default:       top = y; break;
    }
    render_image(s, left, top);
}
