/*
 * Optic DE — shared drawing: a 32-bit canvas in client memory with anti-aliased shapes
 * (signed-distance coverage, so pill ends and icon strokes are smooth without a compositor),
 * copied to an X pixmap, then text on top with Xft in the MinUI font.
 */
#ifndef OPTIC_UI_H
#define OPTIC_UI_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <stdint.h>

/* MinUI palette */
#define C_BLACK     0x000000
#define C_WHITE     0xFFFFFF
#define C_DARK      0x262626   /* pills */
#define C_GRAY      0x999999
#define C_DIM       0x7C7C7C
#define C_TEXT      0xE8E8E8   /* OSINT's text */

typedef struct {
    int w, h;
    uint32_t *px;              /* 0x00RRGGBB, row-major */
} Canvas;

typedef struct {
    Display *dpy;
    int scr;
    Visual *vis;
    Colormap cmap;
    int depth;
    XftFont *font, *small, *big;
    int scale;                 /* OPTIC_SCALE: 2 on a phone's native-resolution desktop */
} UI;

int  ui_open(UI *ui, Display *dpy, const char *fontdir);
Canvas *cv_new(int w, int h);
void cv_free(Canvas *c);
void cv_clear(Canvas *c, uint32_t rgb);
/* Copies [src] (same size or larger) region into c at 0,0. */
void cv_blit(Canvas *c, const Canvas *src, int sx, int sy);

/* Anti-aliased shapes; alpha 0..1 blends over what's there. */
void cv_rrect(Canvas *c, float x, float y, float w, float h, float r, uint32_t rgb, float a);
void cv_pill(Canvas *c, float x, float y, float w, float h, uint32_t rgb, float a);
void cv_circle(Canvas *c, float cx, float cy, float r, uint32_t rgb, float a);
void cv_ring(Canvas *c, float cx, float cy, float r, float t, uint32_t rgb, float a);
void cv_line(Canvas *c, float x0, float y0, float x1, float y1, float t, uint32_t rgb, float a);
void cv_rrect_outline(Canvas *c, float x, float y, float w, float h, float r, float t, uint32_t rgb, float a);
/* An 8-bit alpha mask (e.g. the Optic logo), scaled to fit w×h, centred, tinted. */
void cv_mask(Canvas *c, const uint8_t *m, int mw, int mh, float x, float y, float w, float h, uint32_t rgb);

/* Canvas → drawable. */
void cv_put(UI *ui, Canvas *c, Drawable d, GC gc, int dx, int dy);

/* Text (after cv_put, on the same drawable). */
int  ui_text_w(UI *ui, XftFont *f, const char *s);
void ui_text(UI *ui, XftDraw *xd, XftFont *f, int x, int baseline, uint32_t rgb, const char *s);
/* Baseline that centres a line of [f] in a box from top..top+h. */
int  ui_baseline(XftFont *f, int top, int h);

/* Loads a PNG to a canvas (libpng). */
Canvas *png_load(const char *path);
/* Scales [src] to cover w×h (centre crop), bilinear. */
Canvas *cv_cover(const Canvas *src, int w, int h);
uint8_t *a8_load(const char *path, int *w, int *h);

/* Config: ~/.config/optic-de.conf, key=value lines. */
const char *conf_get(const char *key, const char *def);
void conf_set(const char *key, const char *val);
void conf_load(void);

#endif
