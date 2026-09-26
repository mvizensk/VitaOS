#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "text.h"

#define ATLAS 1024
#define PAD 1
#define SLOTS 4096               /* glyph cache entries, power of two */

struct UiFont { FT_Face face; int id; };

typedef struct {
    int used, font, size, cp;
    short x, y, w, h, left, top, adv;
} Glyph;

static FT_Library lib;
static vita2d_texture *atlas;
static unsigned char *pixels;
static unsigned int stride;
static int shelf_x, shelf_y, shelf_h, fonts;
static Glyph cache[SLOTS];

static void atlas_reset(void) {
    vita2d_wait_rendering_done();            /* the GPU may still be reading it */
    memset(pixels, 0, stride * ATLAS);
    memset(cache, 0, sizeof(cache));
    shelf_x = shelf_y = shelf_h = 0;
}

UiFont *uifont_load(const char *path) {
    if (!lib && FT_Init_FreeType(&lib)) return NULL;
    if (!atlas) {
        atlas = vita2d_create_empty_texture_format(ATLAS, ATLAS, SCE_GXM_TEXTURE_FORMAT_U8_R111);
        if (!atlas) return NULL;
        vita2d_texture_set_filters(atlas, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        pixels = vita2d_texture_get_datap(atlas);
        stride = vita2d_texture_get_stride(atlas);
        atlas_reset();
    }
    UiFont *f = calloc(1, sizeof(UiFont));
    /* The whole file in memory: FreeType otherwise reads each glyph from the
     * card as it is first drawn, and a card read can block for seconds. */
    FILE *fp = fopen(path, "rb");
    long n = 0;
    unsigned char *data = NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
        data = n > 0 ? malloc(n) : NULL;
        if (data && fread(data, 1, n, fp) != (size_t)n) { free(data); data = NULL; }
        fclose(fp);
    }
    if (!f || !data || FT_New_Memory_Face(lib, data, n, 0, &f->face)) { free(f); free(data); return NULL; }
    f->id = ++fonts;
    return f;
}

/* Next code point from UTF-8; advances *s. */
static int next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp, n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = (p[0] & 0x1F) << 6 | (p[1] & 0x3F); n = 2; }
    else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) { cp = (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); n = 3; }
    else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = (p[0] & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F); n = 4;
    } else { cp = '?'; n = 1; }
    *s += n;
    return cp;
}

static Glyph *glyph(UiFont *f, int size, int cp) {
    unsigned int h = ((unsigned)cp * 2654435761u ^ (unsigned)size * 40503u ^ (unsigned)f->id * 97u) & (SLOTS - 1);
    for (int probe = 0; probe < SLOTS; ++probe, h = (h + 1) & (SLOTS - 1)) {
        Glyph *g = &cache[h];
        if (g->used && g->font == f->id && g->size == size && g->cp == cp) return g;
        if (g->used) continue;
        /* Not cached: render it. */
        FT_Set_Pixel_Sizes(f->face, 0, size);
        if (FT_Load_Char(f->face, cp, FT_LOAD_RENDER)) return NULL;
        FT_GlyphSlot s = f->face->glyph;
        int w = s->bitmap.width, bh = s->bitmap.rows;
        if (shelf_x + w + PAD > ATLAS) { shelf_x = 0; shelf_y += shelf_h + PAD; shelf_h = 0; }
        if (shelf_y + bh + PAD > ATLAS) { atlas_reset(); return glyph(f, size, cp); }
        for (int r = 0; r < bh; ++r)            /* pitch, not width: rows can be padded */
            memcpy(pixels + (shelf_y + r) * stride + shelf_x, s->bitmap.buffer + r * s->bitmap.pitch, w);
        g->used = 1; g->font = f->id; g->size = size; g->cp = cp;
        g->x = shelf_x; g->y = shelf_y; g->w = w; g->h = bh;
        g->left = s->bitmap_left; g->top = s->bitmap_top; g->adv = s->advance.x >> 6;
        shelf_x += w + PAD;
        if (bh > shelf_h) shelf_h = bh;
        return g;
    }
    atlas_reset();
    return NULL;
}

int uifont_draw(UiFont *f, int x, int y, unsigned int color, unsigned int size, const char *text) {
    if (!f || !text) return 0;
    int pen = x, line = y;
    while (*text) {
        int cp = next_cp(&text);
        if (cp == '\n') { pen = x; line += size + size / 4; continue; }
        Glyph *g = glyph(f, size, cp);
        if (!g) continue;
        if (g->w && g->h)
            vita2d_draw_texture_tint_part(atlas, pen + g->left, line - g->top, g->x, g->y, g->w, g->h, color);
        pen += g->adv;
    }
    return pen - x;
}

int uifont_width(UiFont *f, unsigned int size, const char *text) {
    if (!f || !text) return 0;
    int w = 0, best = 0;
    while (*text) {
        int cp = next_cp(&text);
        if (cp == '\n') { if (w > best) best = w; w = 0; continue; }
        Glyph *g = glyph(f, size, cp);
        if (g) w += g->adv;
    }
    return w > best ? w : best;
}
