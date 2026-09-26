/* Home's text renderer. vita2d's font cache put slivers of neighbouring glyphs
 * beside letters ("Fi|es", "c|hoose", a ghost on "Play"), seen on the device
 * (2026-09-24). This renders each glyph with FreeType at exactly the
 * size asked for, keeps a pixel of empty space around it in the atlas, honours
 * FreeType's row pitch, and draws at whole pixels with point sampling.
 * y is the baseline, as it was with vita2d_font_draw_text. */
#ifndef HOME_TEXT_H
#define HOME_TEXT_H

typedef struct UiFont UiFont;

UiFont *uifont_load(const char *path);
int uifont_draw(UiFont *f, int x, int y, unsigned int color, unsigned int size, const char *text);
int uifont_width(UiFont *f, unsigned int size, const char *text);

#endif
