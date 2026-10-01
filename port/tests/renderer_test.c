/* Compare this fixture against a previous gfx.c using cmp on the RGBA output.
 * Covers transparent overlap, independent draws, clipping/blend transitions,
 * vertex offsets after merges, texture retirement, and state reset per frame. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <OpenGL/OpenGL.h>
#include "gl.h"
#include "gfx.h"

uint64_t plat_time_us(void) { return 0; }
void plat_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    assert(!"unexpected renderer diagnostic");
}
static void quad(int tex, float x, float y, float w, float h, uint32_t colour) {
    float xy[8] = {x,y,x+w,y,x+w,y+h,x,y+h};
    const float uv[8] = {0,0,2,0,2,2,0,2};
    gfx_quad(tex, 2, 2, xy, uv, colour);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    CGLPixelFormatAttribute attrs[] = { kCGLPFAColorSize, (CGLPixelFormatAttribute)24, 0 };
    CGLPixelFormatObj pf; CGLContextObj ctx; GLint count;
    assert(!CGLChoosePixelFormat(attrs, &pf, &count) && pf);
    assert(!CGLCreateContext(pf, NULL, &ctx));
    assert(!CGLSetCurrentContext(ctx));
    GLuint fbo, rb;
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 128, 128);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    uint32_t pixels[4] = {0xffffffffu, 0xff808080u, 0xff808080u, 0xffffffffu};
    int a = gfx_tex_create(2, 2, GFX_FMT_RGBA8888, pixels, 1, 0);
    int b = gfx_tex_create(2, 2, GFX_FMT_RGBA8888, pixels, 1, 0);
    assert(a && b);
    FILE *out = fopen(argv[1], "wb"); assert(out);
    uint8_t rgba[128 * 128 * 4];
    for (int frame = 0; frame < 8; frame++) {
        /* Simulate the resident changing GL state between app callbacks. */
        glBindTexture(GL_TEXTURE_2D, 0); glDisable(GL_TEXTURE_2D);
        glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 1, 1);
        gfx_begin_frame(128, 128, 128, 128, frame & 1, .1f, .2f, .3f);
        quad(a, 2, 2, 28, 28, 0x80800000u);
        quad(b, 74, 2, 28, 28, 0x80000080u);
        quad(a, 4, 4, 28, 28, 0x80008000u); /* can cross disjoint b */
        quad(b, 4, 4, 28, 28, 0x80000080u);
        quad(a, 6, 6, 28, 28, 0x80800000u); /* must not cross overlap */
        gfx_set_clip(10, 10, 76, 76);
        quad(a, 0, 0, 128, 128, 0x40400000u);
        gfx_set_clip(28, 28, 40, 40);
        gfx_set_blend(GFX_BLEND_ADD);
        quad(0, 20, 20, 70, 70, 0x40000040u);
        gfx_set_blend(GFX_BLEND_NONE);
        quad(a, 34, 34, 12, 12, 0xff00ff00u);
        gfx_set_clip(0, 0, 0, 0);
        gfx_set_blend(GFX_BLEND_ALPHA);
        /* Hundreds of interleaved draws exercise VBO offsets and copy limits. */
        for (int i = 0; i < (frame & 2 ? 540 : 300); i++)
            quad(frame & 2 || i % 3 ? a : b, (float)(i * 17 % 120), (float)(i * 13 % 120), 3, 3, 0x80808080u);
        int retired = gfx_tex_create(2, 2, GFX_FMT_RGBA8888, pixels, 0, 0);
        quad(retired, 90, 90, 16, 16, 0x80808000u);
        gfx_tex_destroy(retired); /* pixels must live until draw submission */
        quad(0, 94, 94, 20, 20, 0x80008080u);
        gfx_end_frame();
        assert(glGetError() == GL_NO_ERROR);
        glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        assert(fwrite(rgba, 1, sizeof rgba, out) == sizeof rgba);
    }
    assert(!fclose(out));
    gfx_tex_destroy(a); gfx_tex_destroy(b);
    puts("renderer fixture: PASS");
    return 0;
}
