/* gfx.c — batched 2D quads over OpenGL ES 1.1 (see gfx.h). */
#include <string.h>
#include "gl.h"
#include "gfx.h"

#define MAX_TEX   64          /* the nano's GL driver reboots somewhere past ~80 live textures */
#define MAX_VERTS 1536        /* 256 quads per batch */

typedef struct { float x, y, u, v; uint32_t c; } Vert;

static struct { unsigned gl; int w, h; uint32_t bytes; } sTex[MAX_TEX + 1];
static Vert sVerts[MAX_VERTS];
static int sCount, sCurTex = -1, sBlend = -1, sWantBlend;
static int sClipOn;
static float sClip[4];
static int sPanelW, sPanelH, sRot;
static float sLogW, sLogH;
static int sDraws, sQuads;

int gfx_stat_draws, gfx_stat_quads, gfx_stat_textures;
uint32_t gfx_stat_tex_bytes;

int gfx_tex_create(int w, int h, int fmt, const void *pixels, int linear, int repeat)
{
    int id;
    GLuint name = 0;
    GLenum glfmt, type;
    for (id = 1; id <= MAX_TEX && sTex[id].gl; id++) {}
    if (id > MAX_TEX)
        return 0;
    gfx_flush();
    glGenTextures(1, &name);
    if (!name)
        return 0;
    glBindTexture(GL_TEXTURE_2D, name);
    sCurTex = -1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (fmt == GFX_FMT_RGB565) {
        glfmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5;
    } else if (fmt == GFX_FMT_RGBA4444) {
        glfmt = GL_RGBA; type = GL_UNSIGNED_SHORT_4_4_4_4;
    } else {
        glfmt = GL_RGBA; type = GL_UNSIGNED_BYTE;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)glfmt, w, h, 0, glfmt, type, pixels);
    sTex[id].gl = name;
    sTex[id].w = w;
    sTex[id].h = h;
    sTex[id].bytes = (uint32_t)w * (uint32_t)h * (fmt == GFX_FMT_RGBA8888 ? 4u : 2u);
    gfx_stat_textures++;
    gfx_stat_tex_bytes += sTex[id].bytes;
    return id;
}

void gfx_tex_destroy(int tex)
{
    GLuint name;
    if (tex <= 0 || tex > MAX_TEX || !sTex[tex].gl)
        return;
    gfx_flush();
    name = sTex[tex].gl;
    glDeleteTextures(1, &name);
    gfx_stat_textures--;
    gfx_stat_tex_bytes -= sTex[tex].bytes;
    sTex[tex].gl = 0;
    sCurTex = -1;
}

void gfx_forget_all(void)
{
    memset(sTex, 0, sizeof(sTex));
    gfx_stat_textures = 0;
    gfx_stat_tex_bytes = 0;
    sCount = 0;
    sCurTex = -1;
    sBlend = -1;
}

static void apply_clip(void)
{
    if (!sClipOn) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    {
        /* logical -> panel pixels (GL window coordinates have y up) */
        float sx = (float)(sRot ? sPanelH : sPanelW) / sLogW;
        float sy = (float)(sRot ? sPanelW : sPanelH) / sLogH;
        int x0 = (int)(sClip[0] * sx), y0 = (int)(sClip[1] * sy);
        int x1 = (int)((sClip[0] + sClip[2]) * sx + 0.999f), y1 = (int)((sClip[1] + sClip[3]) * sy + 0.999f);
        glEnable(GL_SCISSOR_TEST);
        if (sRot)   /* logical x runs down the panel, logical y right to left */
            glScissor(y0, sPanelH - x1, y1 - y0, x1 - x0);
        else
            glScissor(x0, sPanelH - y1, x1 - x0, y1 - y0);
    }
}

void gfx_begin_frame(int panel_w, int panel_h, float logical_w, float logical_h, int rot90,
                     float r, float g, float b)
{
    sPanelW = panel_w; sPanelH = panel_h; sRot = rot90;
    sLogW = logical_w; sLogH = logical_h;
    sDraws = sQuads = 0;
    sCount = 0;
    sCurTex = -1;
    sBlend = -1;
    sWantBlend = GFX_BLEND_ALPHA;
    sClipOn = 0;

    glViewport(0, 0, panel_w, panel_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (rot90)
        glRotatef(-90.f, 0.f, 0.f, 1.f);    /* logical right -> physical down */
    glOrthof(0.f, logical_w, logical_h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glShadeModel(GL_SMOOTH);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
}

void gfx_flush(void)
{
    if (!sCount)
        return;
    if (sBlend != sWantBlend) {
        sBlend = sWantBlend;
        if (sBlend == GFX_BLEND_NONE) {
            glDisable(GL_BLEND);
        } else {
            glEnable(GL_BLEND);
            /* the game's images (and the colours passed in) are premultiplied */
            glBlendFunc(GL_ONE, sBlend == GFX_BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        }
    }
    glVertexPointer(2, GL_FLOAT, sizeof(Vert), &sVerts[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vert), &sVerts[0].c);
    if (sCurTex > 0) {
        glEnable(GL_TEXTURE_2D);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vert), &sVerts[0].u);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDrawArrays(GL_TRIANGLES, 0, sCount);
    sDraws++;
    sCount = 0;
}

void gfx_end_frame(void)
{
    gfx_flush();
    glDisable(GL_SCISSOR_TEST);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    gfx_stat_draws = sDraws;
    gfx_stat_quads = sQuads;
}

void gfx_set_clip(float x, float y, float w, float h)
{
    int on = w > 0.f && h > 0.f;
    if (on == sClipOn && (!on || (x == sClip[0] && y == sClip[1] && w == sClip[2] && h == sClip[3])))
        return;
    gfx_flush();
    sClipOn = on;
    sClip[0] = x; sClip[1] = y; sClip[2] = w; sClip[3] = h;
    apply_clip();
}

void gfx_set_blend(int mode)
{
    if (mode == sWantBlend)
        return;
    gfx_flush();
    sWantBlend = mode;
}

static void bind(int tex)
{
    if (tex == sCurTex)
        return;
    gfx_flush();
    sCurTex = tex;
    if (tex > 0)
        glBindTexture(GL_TEXTURE_2D, sTex[tex].gl);
}

/* GL wants RGBA byte order in memory. */
static uint32_t pack(uint32_t argb)
{
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
}

/* The nano's GL driver reboots the iPod on zero-area triangles. */
static int degenerate(const float *a, const float *b, const float *c)
{
    float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    return area > -1e-6f && area < 1e-6f;
}

static void emit(const float *p, float u, float v, uint32_t c)
{
    Vert *o = &sVerts[sCount++];
    o->x = p[0]; o->y = p[1]; o->u = u; o->v = v; o->c = c;
}

void gfx_quad(int tex, int tex_w, int tex_h, const float xy[8], const float uv[8], uint32_t argb)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    uint32_t c = pack(argb);
    float iu = 0.f, iv = 0.f;
    int t, k;
    if (tex < 0 || tex > MAX_TEX || (tex && !sTex[tex].gl))
        return;
    bind(tex);
    if (sCount + 6 > MAX_VERTS)
        gfx_flush();
    if (tex) {
        iu = 1.f / (float)tex_w;
        iv = 1.f / (float)tex_h;
    }
    for (t = 0; t < 2; t++) {
        const int *o = &order[t * 3];
        if (degenerate(&xy[o[0] * 2], &xy[o[1] * 2], &xy[o[2] * 2]))
            continue;
        for (k = 0; k < 3; k++) {
            int i = o[k];
            emit(&xy[i * 2], tex ? uv[i * 2] * iu : 0.f, tex ? uv[i * 2 + 1] * iv : 0.f, c);
        }
    }
    sQuads++;
}

void gfx_tri(const float xy[6], uint32_t argb)
{
    uint32_t c = pack(argb);
    int k;
    if (degenerate(&xy[0], &xy[2], &xy[4]))
        return;
    bind(0);
    if (sCount + 3 > MAX_VERTS)
        gfx_flush();
    for (k = 0; k < 3; k++)
        emit(&xy[k * 2], 0.f, 0.f, c);
}
