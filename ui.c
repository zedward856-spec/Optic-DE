/* Optic DE — shared drawing (see ui.h). */
#include "ui.h"
#include <X11/Xutil.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

int ui_open(UI *ui, Display *dpy, const char *fontdir) {
    ui->dpy = dpy;
    ui->scr = DefaultScreen(dpy);
    ui->vis = DefaultVisual(dpy, ui->scr);
    ui->cmap = DefaultColormap(dpy, ui->scr);
    ui->depth = DefaultDepth(dpy, ui->scr);
    char path[512], name[96];
    const char *sc = getenv("OPTIC_SCALE");
    ui->scale = sc && atoi(sc) > 0 ? atoi(sc) : 1;
    snprintf(path, sizeof path, "%s/bpreplay_bold.otf", fontdir);
    FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)path);
    const int px[3] = { 15, 12, 19 };
    XftFont **slot[3] = { &ui->font, &ui->small, &ui->big };
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof name, "BPreplay:bold:pixelsize=%d", px[i] * ui->scale);
        *slot[i] = XftFontOpenName(dpy, ui->scr, name);
        if (!*slot[i]) { snprintf(name, sizeof name, "sans:bold:pixelsize=%d", px[i] * ui->scale); *slot[i] = XftFontOpenName(dpy, ui->scr, name); }
    }
    return ui->font != NULL;
}

Canvas *cv_new(int w, int h) {
    Canvas *c = malloc(sizeof *c);
    c->w = w; c->h = h;
    c->px = calloc((size_t)w * h, 4);
    return c;
}

void cv_free(Canvas *c) { if (c) { free(c->px); free(c); } }

void cv_clear(Canvas *c, uint32_t rgb) {
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = rgb;
}

void cv_blit(Canvas *c, const Canvas *src, int sx, int sy) {
    for (int y = 0; y < c->h; y++) {
        int yy = sy + y;
        if (yy < 0 || yy >= src->h) continue;
        for (int x = 0; x < c->w; x++) {
            int xx = sx + x;
            if (xx >= 0 && xx < src->w) c->px[y * c->w + x] = src->px[yy * src->w + xx];
        }
    }
}

static inline void blend(Canvas *c, int x, int y, uint32_t rgb, float a) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || a <= 0) return;
    uint32_t *p = &c->px[y * c->w + x];
    if (a >= 1) { *p = rgb; return; }
    int r = (*p >> 16) & 255, g = (*p >> 8) & 255, b = *p & 255;
    int R = (rgb >> 16) & 255, G = (rgb >> 8) & 255, B = rgb & 255;
    r += (int)((R - r) * a); g += (int)((G - g) * a); b += (int)((B - b) * a);
    *p = (uint32_t)(r << 16 | g << 8 | b);
}

/* Signed distance to a rounded box centred at (cx,cy) with half-size (hx,hy) and radius r. */
static float sd_rbox(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float qx = fabsf(px - cx) - hx + r, qy = fabsf(py - cy) - hy + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = fminf(fmaxf(qx, qy), 0);
    return sqrtf(ox * ox + oy * oy) + in - r;
}

void cv_rrect(Canvas *c, float x, float y, float w, float h, float r, uint32_t rgb, float a) {
    float cx = x + w / 2, cy = y + h / 2;
    r = fminf(r, fminf(w, h) / 2);
    for (int py = (int)floorf(y) - 1; py <= (int)ceilf(y + h); py++)
        for (int px = (int)floorf(x) - 1; px <= (int)ceilf(x + w); px++) {
            float d = sd_rbox(px + .5f, py + .5f, cx, cy, w / 2, h / 2, r);
            blend(c, px, py, rgb, a * clampf(.5f - d, 0, 1));
        }
}

void cv_pill(Canvas *c, float x, float y, float w, float h, uint32_t rgb, float a) { cv_rrect(c, x, y, w, h, h / 2, rgb, a); }

void cv_rrect_outline(Canvas *c, float x, float y, float w, float h, float r, float t, uint32_t rgb, float a) {
    float cx = x + w / 2, cy = y + h / 2;
    r = fminf(r, fminf(w, h) / 2);
    for (int py = (int)floorf(y - t); py <= (int)ceilf(y + h + t); py++)
        for (int px = (int)floorf(x - t); px <= (int)ceilf(x + w + t); px++) {
            float d = fabsf(sd_rbox(px + .5f, py + .5f, cx, cy, w / 2, h / 2, r)) - t / 2;
            blend(c, px, py, rgb, a * clampf(.5f - d, 0, 1));
        }
}

void cv_circle(Canvas *c, float cx, float cy, float r, uint32_t rgb, float a) {
    for (int py = (int)(cy - r - 1); py <= (int)(cy + r + 1); py++)
        for (int px = (int)(cx - r - 1); px <= (int)(cx + r + 1); px++) {
            float d = hypotf(px + .5f - cx, py + .5f - cy) - r;
            blend(c, px, py, rgb, a * clampf(.5f - d, 0, 1));
        }
}

void cv_ring(Canvas *c, float cx, float cy, float r, float t, uint32_t rgb, float a) {
    for (int py = (int)(cy - r - t); py <= (int)(cy + r + t); py++)
        for (int px = (int)(cx - r - t); px <= (int)(cx + r + t); px++) {
            float d = fabsf(hypotf(px + .5f - cx, py + .5f - cy) - r) - t / 2;
            blend(c, px, py, rgb, a * clampf(.5f - d, 0, 1));
        }
}

void cv_line(Canvas *c, float x0, float y0, float x1, float y1, float t, uint32_t rgb, float a) {
    float minx = fminf(x0, x1) - t, maxx = fmaxf(x0, x1) + t, miny = fminf(y0, y1) - t, maxy = fmaxf(y0, y1) + t;
    float dx = x1 - x0, dy = y1 - y0, L = dx * dx + dy * dy;
    for (int py = (int)miny; py <= (int)maxy; py++)
        for (int px = (int)minx; px <= (int)maxx; px++) {
            float qx = px + .5f - x0, qy = py + .5f - y0;
            float k = L > 0 ? clampf((qx * dx + qy * dy) / L, 0, 1) : 0;
            float d = hypotf(qx - dx * k, qy - dy * k) - t / 2;
            blend(c, px, py, rgb, a * clampf(.5f - d, 0, 1));
        }
}

void cv_mask(Canvas *c, const uint8_t *m, int mw, int mh, float x, float y, float w, float h, uint32_t rgb) {
    if (!m) return;
    float s = fminf(w / mw, h / mh);
    int dw = (int)(mw * s), dh = (int)(mh * s);
    int ox = (int)(x + (w - dw) / 2), oy = (int)(y + (h - dh) / 2);
    /* Box filter: each destination pixel averages the source pixels it covers. */
    for (int j = 0; j < dh; j++)
        for (int i = 0; i < dw; i++) {
            int sx0 = (int)(i / s), sx1 = (int)((i + 1) / s), sy0 = (int)(j / s), sy1 = (int)((j + 1) / s);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            int sum = 0, n = 0;
            for (int v = sy0; v < sy1 && v < mh; v++)
                for (int u = sx0; u < sx1 && u < mw; u++) { sum += m[v * mw + u]; n++; }
            if (n) blend(c, ox + i, oy + j, rgb, sum / (255.f * n));
        }
}

void cv_put(UI *ui, Canvas *c, Drawable d, GC gc, int dx, int dy) {
    XImage *img = XCreateImage(ui->dpy, ui->vis, ui->depth, ZPixmap, 0, (char *)c->px, c->w, c->h, 32, 0);
    XPutImage(ui->dpy, d, gc, img, 0, 0, dx, dy, c->w, c->h);
    img->data = NULL; /* the canvas owns the pixels */
    XDestroyImage(img);
}

int ui_text_w(UI *ui, XftFont *f, const char *s) {
    XGlyphInfo g;
    XftTextExtentsUtf8(ui->dpy, f, (const FcChar8 *)s, (int)strlen(s), &g);
    return g.xOff;
}

void ui_text(UI *ui, XftDraw *xd, XftFont *f, int x, int baseline, uint32_t rgb, const char *s) {
    XRenderColor rc = { (unsigned short)(((rgb >> 16) & 255) * 257), (unsigned short)(((rgb >> 8) & 255) * 257),
                        (unsigned short)((rgb & 255) * 257), 0xFFFF };
    XftColor col;
    XftColorAllocValue(ui->dpy, ui->vis, ui->cmap, &rc, &col);
    XftDrawStringUtf8(xd, &col, f, x, baseline, (const FcChar8 *)s, (int)strlen(s));
    XftColorFree(ui->dpy, ui->vis, ui->cmap, &col);
}

int ui_baseline(XftFont *f, int top, int h) { return top + (h - (f->ascent + f->descent)) / 2 + f->ascent; }

Canvas *png_load(const char *path) {
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path)) return NULL;
    im.format = PNG_FORMAT_BGRA; /* little-endian 0xAARRGGBB words */
    Canvas *c = cv_new((int)im.width, (int)im.height);
    if (!png_image_finish_read(&im, NULL, c->px, 0, NULL)) { cv_free(c); return NULL; }
    for (int i = 0; i < c->w * c->h; i++) c->px[i] &= 0xFFFFFF;
    return c;
}

Canvas *cv_cover(const Canvas *src, int w, int h) {
    Canvas *c = cv_new(w, h);
    float s = fmaxf((float)w / src->w, (float)h / src->h);
    float ox = (src->w * s - w) / 2, oy = (src->h * s - h) / 2;
    for (int y = 0; y < h; y++) {
        float fy = (y + oy + .5f) / s - .5f;
        int y0 = (int)clampf(floorf(fy), 0, src->h - 1), y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        float ty = clampf(fy - y0, 0, 1);
        for (int x = 0; x < w; x++) {
            float fx = (x + ox + .5f) / s - .5f;
            int x0 = (int)clampf(floorf(fx), 0, src->w - 1), x1 = x0 + 1 < src->w ? x0 + 1 : x0;
            float tx = clampf(fx - x0, 0, 1);
            uint32_t a = src->px[y0 * src->w + x0], b = src->px[y0 * src->w + x1];
            uint32_t cc = src->px[y1 * src->w + x0], d = src->px[y1 * src->w + x1];
            uint32_t out = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                float top = ((a >> sh) & 255) * (1 - tx) + ((b >> sh) & 255) * tx;
                float bot = ((cc >> sh) & 255) * (1 - tx) + ((d >> sh) & 255) * tx;
                out |= (uint32_t)(top * (1 - ty) + bot * ty + .5f) << sh;
            }
            c->px[y * w + x] = out;
        }
    }
    return c;
}

uint8_t *a8_load(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hd[4];
    if (fread(hd, 1, 4, f) != 4) { fclose(f); return NULL; }
    *w = hd[0] | hd[1] << 8; *h = hd[2] | hd[3] << 8;
    uint8_t *m = malloc((size_t)*w * *h);
    if (fread(m, 1, (size_t)*w * *h, f) != (size_t)*w * *h) { free(m); m = NULL; }
    fclose(f);
    return m;
}

/* ---- config ---------------------------------------------------------------------------------- */

#define MAXKV 64
static char keys[MAXKV][64], vals[MAXKV][256];
static int nkv;

static const char *conf_path(void) {
    static char p[512];
    const char *home = getenv("HOME");
    snprintf(p, sizeof p, "%s/.config/optic-de.conf", home ? home : "/root");
    return p;
}

void conf_load(void) {
    nkv = 0;
    FILE *f = fopen(conf_path(), "r");
    if (!f) return;
    char line[400];
    while (nkv < MAXKV && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(keys[nkv], sizeof keys[0], "%s", line);
        snprintf(vals[nkv], sizeof vals[0], "%s", v);
        nkv++;
    }
    fclose(f);
}

const char *conf_get(const char *key, const char *def) {
    for (int i = 0; i < nkv; i++) if (!strcmp(keys[i], key)) return vals[i];
    return def;
}

void conf_set(const char *key, const char *val) {
    int i;
    for (i = 0; i < nkv; i++) if (!strcmp(keys[i], key)) break;
    if (i == nkv) { if (nkv == MAXKV) return; nkv++; snprintf(keys[i], sizeof keys[0], "%s", key); }
    snprintf(vals[i], sizeof vals[0], "%s", val);
    char dir[512];
    snprintf(dir, sizeof dir, "%s", conf_path());
    *strrchr(dir, '/') = 0;
    char cmd[600];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd)) {}
    FILE *f = fopen(conf_path(), "w");
    if (!f) return;
    for (int k = 0; k < nkv; k++) fprintf(f, "%s=%s\n", keys[k], vals[k]);
    fclose(f);
}
