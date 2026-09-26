/* Workbench UI layer: fonts, colours, input with key repeat and touch, and the
 * few widgets every screen shares (header, footer, lists, bars, dialogs). */
#ifndef WB_UI_H
#define WB_UI_H

#include <vita2d.h>
#include "text.h"

#define W 960
#define H 544

/* Not near-black: the original Vita's OLED smears bright things moving over
 * near-black (the "ghosting" in the 2026-09-25 playtest). */
#define C_BG      RGBA8(21, 24, 33, 255)
#define C_PANEL   RGBA8(22, 25, 34, 255)
#define C_LINE    RGBA8(40, 45, 58, 255)
#define C_ACCENT_DEFAULT RGBA8(88, 166, 255, 255)
/* The accent follows the art in focus (dynamic theming); it eases there. */
extern unsigned int ui_accent;
#define C_ACCENT  ui_accent
#define C_SEL     RGBA8(38, 79, 140, 255)
#define C_SEL_DIM RGBA8(34, 42, 58, 255)
#define C_MARK    RGBA8(255, 196, 0, 255)
#define C_TEXT    RGBA8(236, 239, 244, 255)
#define C_DIM     RGBA8(236, 239, 244, 150)
#define C_FAINT   RGBA8(236, 239, 244, 70)
#define C_OK      RGBA8(80, 200, 120, 255)
#define C_BAD     RGBA8(240, 90, 90, 255)

extern UiFont *font, *bold;

/* Buttons that are "pressed" fire once on the press and then repeat while
 * held (directions only), so long lists scroll at a steady pace. */
typedef struct {
    unsigned int held, pressed;
    int touching, tx, ty;       /* finger on the front panel, screen pixels */
    int tapped, tap_x, tap_y;   /* a short touch that did not move: a tap */
    int drag_dx, drag_dy;       /* drag since the last frame */
    int released;               /* a touch ended this frame (after a drag or a tap) */
} Input;

void ui_init(void);

/* Where the main thread is, for the stall watchdog in main.c. */
extern volatile const char *ui_where;
extern volatile int ui_agent_active;   /* an agent drove the console in the last ~2 minutes */
/* How the console has just been tilted, -1..1 each way; eases back to 0 when
 * it is held still. Backgrounds shift with it (parallax), depth by depth. */
extern float ui_tilt_x, ui_tilt_y;
#define STAGE(name) (ui_where = (name))

/* Frames go through these so a dialog opened mid-frame can close the frame,
 * run its own, and reopen it (vita2d cannot nest drawing). */
void ui_begin_frame(void);
void ui_end_frame(void);
void ui_read_input(Input *in);

/* Text. fit() shortens with "..." to a pixel width. */
int text_w(UiFont *f, int size, const char *s);
void text(UiFont *f, int x, int y, unsigned int color, int size, const char *s);
void text_fit(UiFont *f, int x, int y, unsigned int color, int size, const char *s, int max_w);
void text_right(UiFont *f, int right, int y, unsigned int color, int size, const char *s);

void human_size(unsigned long long n, char *out, int max);

/* context: a short line for the current screen, shown before the clock. */
void draw_header(const char *const tabs[], int ntabs, int active, const char *context);
void draw_footer(const char *hint);
int draw_hints(int x, int cy, const char *hint, unsigned int color, int max_x);   /* "X play  O back" with button glyphs */
int hints_width(const char *hint);
void draw_hints_centered(int cx, int cy, const char *hint, unsigned int color);
void draw_action_button(float x, float y, float w, float h, const char *hint, int on, unsigned int fill);
void draw_bar(int x, int y, int w, int h, float frac, unsigned int color);
void draw_wrapped_text(const char *s, int x, int y, int width, int size, int max_lines, unsigned int color);

/* Motion: a slow 0..1 breathing value, the shared focus glow and ring, and a
 * springy ease for tiles growing into focus. */
float ui_pulse(void);
unsigned int ui_frames(void);

typedef struct { float top, vel; int touch; } GridScroll;
void grid_scroll(GridScroll *g, int *sel, int cols, int count, int rows_visible, float cell_h, const Input *in);

/* Soft shapes, tinted by color's alpha: a flat elliptical shadow and a round glow. */
vita2d_texture *ui_soft_shadow(void);
vita2d_texture *ui_glow(void);
void draw_soft(vita2d_texture *t, float cx, float cy, float w, float h, unsigned int color);
void ui_ambient(float strength);   /* PS5-style drifting glow, ribbons, motes */

/* A picture from the card, decoded off the main thread: NULL until it is ready
 * (or if the file is missing). Cached; the least recently used are dropped. */
vita2d_texture *ui_image(const char *path);
void ui_image_forget(const char *path);
int ui_image_pending(const char *path);
void draw_shimmer(float x, float y, float w, float h);
void draw_gradient(float x, float y, float w, float h, unsigned int tl, unsigned int tr, unsigned int bl, unsigned int br);
void draw_made_cover(const char *name, const char *sub, float x, float y, float s, int dim);
void draw_round_rect(float x, float y, float w, float h, float r, unsigned int c);
void draw_round_ring(float x, float y, float w, float h, float r, float t, unsigned int c);
void draw_round_texture(vita2d_texture *t, float x, float y, float w, float h, float r, unsigned int tint);
void draw_round_texture_uv(vita2d_texture *t, float x, float y, float w, float h, float r,
                           float u0, float v0, float u1, float v1, unsigned int tint);
void draw_round_cover(vita2d_texture *t, float x, float y, float s, float r, unsigned int tint);   /* crop to square */
void draw_round_gradient(float x, float y, float w, float h, float r, unsigned int left, unsigned int right);
float ui_corner(float w, float h);        /* the focus ring's radius: give tiles inside it the same */
void draw_app_icon(const char *path, float x, float y, float size, const char *name, int dim);

/* Write a file from a background thread (the data is copied). Never touch
 * the card from the main thread: one small write there once waited 53 s. */
void ui_save(const char *path, const void *data, int len, int append);

/* Dynamic theming: take the accent from a cover or poster (cached per
 * texture), or go back to the default blue. Call every frame you want it. */
void ui_theme_from(vita2d_texture *art);
void ui_theme_color(unsigned int rgba);
void ui_theme_default(void);

/* A notice in the top-right corner for a few seconds; safe from any thread. */
void ui_toast(const char *text, unsigned int color);
void ui_draw_toasts(void);

/* A blurred copy of the last finished frame, for overlays (between frames). */
void ui_blur_capture(void);
void ui_blur_draw(void);
void draw_focus(float x, float y, float w, float h, float lift);
void draw_focus_r(float x, float y, float w, float h, float lift, float r);
float ease_back(float t);

/* Modal dialogs: block and run their own frames. */
int ui_confirm(const char *title, const char *body);                 /* 1 = yes */
int ui_menu(const char *title, const char *const items[], int n);    /* index or -1 */
int ui_ask_text(const char *title, char *text, int max);             /* 1 = entered */
void ui_message(const char *title, const char *body);

/* Which header tab a tap landed on, or -1. */
int header_tab_at(int x, int y, const char *const tabs[], int ntabs);

#endif
