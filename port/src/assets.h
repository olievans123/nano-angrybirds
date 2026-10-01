/* assets.h — sprite sheets in the converted format (tools/convert_assets.py).
 *
 * A sheet's index is loaded when the game creates the sheet; its texture pages are read
 * from disk the first time one of their sprites is drawn and dropped again, least
 * recently used first, when the texture budget is exceeded (the game creates every sheet
 * at start-up, which is more texture than the nano has memory for). */
#ifndef AB_ASSETS_H
#define AB_ASSETS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int assets_make_room(uint32_t bytes);

typedef struct AbSheet AbSheet;

typedef struct AbSprite {
    char     name[40];
    int16_t  x, y;            /* position in the original sheet image (game pixels) */
    uint16_t w, h;            /* size in game pixels */
    int16_t  pivot_x, pivot_y;
    uint16_t page;            /* texture page of the sheet */
    uint16_t tx, ty, tw, th;  /* texel rectangle in that page */
    AbSheet *sheet;
    uint8_t  opaque;          /* every texel is opaque (known once its page has been loaded) */
} AbSprite;

/* Load <name>.sht (name without extension, e.g. "INGAME"). Returns NULL if missing.
 * Loading a sheet twice returns the same sheet. */
AbSheet *assets_load_sheet(const char *name);
AbSheet *assets_find_sheet(const char *name);
void     assets_release_sheet(const char *name);
const char *assets_sheet_name(const AbSheet *sheet);
void     assets_sheet_image_size(const AbSheet *sheet, int *w, int *h);

/* Find a sprite by name: in `sheet` when given, else in every loaded sheet. */
const AbSprite *assets_find_sprite(const AbSheet *sheet, const char *sprite);

/* The gfx texture of a sprite's page (loads it if needed). Returns 0 if it can't be
 * loaded. *tex_w / *tex_h get the page size. */
int assets_sprite_texture(const AbSprite *sprite, int *tex_w, int *tex_h);

/* Size of the sprite's texture page (without loading it), and a sheet's first sprite. */
void assets_page_size(const AbSprite *sprite, int *tex_w, int *tex_h);
const AbSprite *assets_first_sprite(const AbSheet *sheet);

/* Call once per frame (before drawing): ages the pages for the LRU. */
void assets_begin_frame(void);
void assets_preload_sheet(const char *name);   /* load its missing pages now, into free budget only */
/* Textures are gone (new GL view): reload on next use. */
void assets_gl_lost(void);
void assets_set_texture_budget(uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif
