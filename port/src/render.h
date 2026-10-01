/* render.h — the engine's 2D render state (gr::RenderState2D in the original) and the
 * sprite/string drawing that goes through it. */
#ifndef AB_RENDER_H
#define AB_RENDER_H

#include "assets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float m00, m10, m01, m11;     /* rotation */
    float tx, ty;                 /* translation (pre-scale units) */
    float sx, sy;                 /* scale, applied last */
    float px, py;                 /* rotation pivot, relative to the quad's top-left */
} RenderState2D;

extern RenderState2D gRS;
extern float gScreenW, gScreenH;  /* logical game screen */

void rs_reset(void);

enum { V_TOP, V_CENTER, V_BOTTOM, V_BASELINE, V_PIVOT };
enum { H_LEFT, H_CENTER, H_RIGHT, H_PIVOT };
typedef struct { int v, h; } Anchor;
/* Parse "TOP", "HCENTER", ... into whichever axis the word names. Unknown words are ignored. */
void anchor_parse(Anchor *a, const char *s);

/* Draw a sprite's rectangle with its top-left corner at (left, top), through gRS. */
void render_image(const AbSprite *s, float left, float top);
/* Draw part of a sprite: source rectangle (sx, sy, sw, sh) in game pixels inside the sprite. */
void render_image_part(const AbSprite *s, float left, float top, float sx, float sy, float sw, float sh);
void render_sprite(const AbSprite *s, float x, float y, Anchor a);

/* Screen position of the corner (cx, cy) of a quad whose top-left is (left, top). */
void rs_corner(float left, float top, float cx, float cy, float *x, float *y);
/* A w x h quad at (left, top) through gRS, textured with the texel rectangle u0,v0..u1,v1 of
 * `tex` (0 = solid colour). argb has premultiplied alpha. */
void render_quad(int tex, int tex_w, int tex_h, float left, float top, float w, float h,
                 float u0, float v0, float u1, float v1, uint32_t argb);

#ifdef __cplusplus
}
#endif

#endif
