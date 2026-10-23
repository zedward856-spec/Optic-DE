/*
 * Optic Settings — the desktop's settings app, in the MinUI look: a round back button and a
 * title pill top-left, one pill-high row per setting with its value in its own pill on the
 * right; the row you touch lights up white. Changes save to ~/.config/optic-de.conf and the
 * desktop picks them up at once (a root-window property it watches).
 */
#include "ui.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define SHARE "/opt/optic-de/share"
static int S = 1;
#define PAD (16 * S)
#define PILL (36 * S)
#define ROW (44 * S)

static Display *dpy;
static Window win;
static UI ui;
static GC gc;
static int W = 760, H = 460;
static Atom A_RELOAD, A_DELETE;

typedef struct { char title[64]; char value[96]; int kind; } Row; /* kind: 0 setting, 1 header, 2 info */
static Row rows[48];
static int nrows, pressedRow = -1, backPressed, scroll;
static int page; /* 0 main, 1 screen size, 2 about */
static char sizes[16][24];
static int nsizes;

static void reload(void) {
    long v = 1;
    XChangeProperty(dpy, DefaultRootWindow(dpy), A_RELOAD, XA_INTEGER, 32, PropModeReplace, (unsigned char *)&v, 1);
    XFlush(dpy);
}

static void run(const char *cmd, char *out, size_t n) {
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return;
    size_t k = fread(out, 1, n - 1, p);
    out[k] = 0;
    pclose(p);
    out[strcspn(out, "\n")] = 0;
}

static void add(const char *t, const char *v, int kind) {
    if (nrows >= 48) return;
    snprintf(rows[nrows].title, sizeof rows[0].title, "%s", t);
    snprintf(rows[nrows].value, sizeof rows[0].value, "%s", v ? v : "");
    rows[nrows].kind = kind;
    nrows++;
}

static void build(void) {
    nrows = 0;
    conf_load();
    if (page == 0) {
        add("Look", NULL, 1);
        add("Wallpaper", strcmp(conf_get("wallpaper", "stone"), "black") ? "Optic stone" : "Black", 0);
        add("Taskbar", strcmp(conf_get("taskbar", "autohide"), "always") ? "Hides itself" : "Always shown", 0);
        add("Clock seconds", strcmp(conf_get("seconds", "on"), "off") ? "On" : "Off", 0);
        add("Apps", NULL, 1);
        add("Preload Firefox", strcmp(conf_get("preload", "on"), "off") ? "On" : "Off", 0);
        add("Display", NULL, 1);
        char sz[32];
        snprintf(sz, sizeof sz, "%d x %d", DisplayWidth(dpy, DefaultScreen(dpy)), DisplayHeight(dpy, DefaultScreen(dpy)));
        add("Screen size", sz, 0);
        add("System", NULL, 1);
        add("About", ">", 0);
    } else if (page == 1) {
        char out[4096];
        run("xrandr 2>/dev/null | awk '/^ +[0-9]+x[0-9]+/{print $1}' | head -16 | tr '\\n' ' '", out, sizeof out);
        nsizes = 0;
        for (char *t = strtok(out, " "); t && nsizes < 16; t = strtok(NULL, " ")) snprintf(sizes[nsizes++], sizeof sizes[0], "%s", t);
        char cur[32];
        snprintf(cur, sizeof cur, "%dx%d", DisplayWidth(dpy, DefaultScreen(dpy)), DisplayHeight(dpy, DefaultScreen(dpy)));
        add("Sizes the display offers", NULL, 1);
        for (int i = 0; i < nsizes; i++) add(sizes[i], strcmp(sizes[i], cur) ? "" : "Current", 0);
        if (!nsizes) add("xrandr isn't available", NULL, 2);
    } else {
        char out[256];
        add("Optic DE", "1.0", 2);
        run(". /etc/os-release 2>/dev/null; echo \"$PRETTY_NAME\"", out, sizeof out);
        add("System", out, 2);
        struct utsname u;
        uname(&u);
        add("Kernel", u.release, 2);
        run("grep -m1 -i 'hardware\\|model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//'", out, sizeof out);
        if (!out[0]) snprintf(out, sizeof out, "%ld cores", sysconf(_SC_NPROCESSORS_ONLN));
        add("Processor", out, 2);
        long mb = sysconf(_SC_PHYS_PAGES) / 1024 * sysconf(_SC_PAGESIZE) / 1024;
        snprintf(out, sizeof out, "%.1f GB", mb / 1024.0);
        add("Memory", out, 2);
        snprintf(out, sizeof out, "%d x %d", DisplayWidth(dpy, DefaultScreen(dpy)), DisplayHeight(dpy, DefaultScreen(dpy)));
        add("Screen", out, 2);
        add("Window manager", "Optic", 2);
    }
}

static const char *title(void) { return page == 0 ? "Settings" : page == 1 ? "Screen size" : "About"; }

static int listTop(void) { return PAD + PILL + PAD; }

static void draw(void) {
    Canvas *c = cv_new(W, H);
    cv_clear(c, C_BLACK);
    /* Back (round) and the title pill. */
    cv_circle(c, PAD + PILL / 2.f, PAD + PILL / 2.f, PILL / 2.f, backPressed ? C_WHITE : C_DARK, 1);
    uint32_t chev = backPressed ? C_BLACK : C_WHITE;
    float bx = PAD + PILL / 2.f, by = PAD + PILL / 2.f;
    cv_line(c, bx + 3 * S, by - 7 * S, bx - 4 * S, by, 2.6f * S, chev, 1);
    cv_line(c, bx - 4 * S, by, bx + 3 * S, by + 7 * S, 2.6f * S, chev, 1);
    int tw = ui_text_w(&ui, ui.font, title());
    cv_pill(c, PAD + PILL + 8 * S, PAD, tw + 32 * S, PILL, C_DARK, 1);
    int top = listTop();
    for (int i = 0; i < nrows; i++) {
        int y = top + i * ROW - scroll;
        if (y + ROW < top || y > H) continue;
        Row *r = &rows[i];
        if (r->kind == 1) continue;
        if (i == pressedRow && r->kind == 0) {
            int w = ui_text_w(&ui, ui.font, r->title) + 32 * S;
            cv_pill(c, PAD, y + 4 * S, w, PILL, C_WHITE, 1);
        }
        if (r->value[0] && r->kind != 1) {
            int vw = ui_text_w(&ui, ui.small, r->value) + 28 * S;
            /* the value in its own pill, apart from the title's */
            cv_pill(c, W - PAD - vw, y + 7 * S, vw, PILL - 6 * S, C_DARK, 1);
        }
    }
    Pixmap pm = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H, (unsigned)ui.depth);
    cv_put(&ui, c, pm, gc, 0, 0);
    XftDraw *xd = XftDrawCreate(dpy, pm, ui.vis, ui.cmap);
    ui_text(&ui, xd, ui.font, PAD + PILL + 24 * S, ui_baseline(ui.font, PAD, PILL), C_WHITE, title());
    XRectangle clip = { 0, (short)top, (unsigned short)W, (unsigned short)(H - top) };
    XftDrawSetClipRectangles(xd, 0, 0, &clip, 1);
    for (int i = 0; i < nrows; i++) {
        int y = top + i * ROW - scroll;
        if (y + ROW < top || y > H) continue;
        Row *r = &rows[i];
        if (r->kind == 1) {
            char up[64]; int k = 0;
            for (; r->title[k] && k < 63; k++) up[k] = (char)(r->title[k] >= 'a' && r->title[k] <= 'z' ? r->title[k] - 32 : r->title[k]);
            up[k] = 0;
            ui_text(&ui, xd, ui.small, PAD + 16 * S, ui_baseline(ui.small, y + 10 * S, ROW - 10 * S), C_DIM, up);
            continue;
        }
        ui_text(&ui, xd, ui.font, PAD + 16 * S, ui_baseline(ui.font, y + 4 * S, PILL), i == pressedRow && r->kind == 0 ? C_BLACK : (r->kind == 2 ? C_GRAY : C_WHITE), r->title);
        if (r->value[0]) {
            int vw = ui_text_w(&ui, ui.small, r->value);
            ui_text(&ui, xd, ui.small, W - PAD - 14 * S - vw, ui_baseline(ui.small, y + 7 * S, PILL - 6 * S), C_WHITE, r->value);
        }
    }
    XftDrawDestroy(xd);
    XCopyArea(dpy, pm, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
    XFreePixmap(dpy, pm);
    cv_free(c);
}

static void openRow(int i) {
    Row *r = &rows[i];
    if (r->kind != 0) return;
    if (page == 0) {
        if (!strcmp(r->title, "Wallpaper")) conf_set("wallpaper", strcmp(conf_get("wallpaper", "stone"), "black") ? "black" : "stone");
        else if (!strcmp(r->title, "Taskbar")) conf_set("taskbar", strcmp(conf_get("taskbar", "autohide"), "always") ? "always" : "autohide");
        else if (!strcmp(r->title, "Clock seconds")) conf_set("seconds", strcmp(conf_get("seconds", "on"), "off") ? "off" : "on");
        else if (!strcmp(r->title, "Preload Firefox")) conf_set("preload", strcmp(conf_get("preload", "on"), "off") ? "off" : "on");
        else if (!strcmp(r->title, "Screen size")) { page = 1; scroll = 0; build(); return; }
        else if (!strcmp(r->title, "About")) { page = 2; scroll = 0; build(); return; }
        reload();
    } else if (page == 1) {
        char cmd[96];
        snprintf(cmd, sizeof cmd, "xrandr -s %s >/dev/null 2>&1", r->title);
        if (system(cmd)) {}
        XSync(dpy, False);
    }
    build();
}

static void back(void) {
    if (page) { page = 0; scroll = 0; build(); draw(); }
    else exit(0);
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    ui_open(&ui, dpy, SHARE);
    S = ui.scale;
    W *= S; H *= S;
    A_RELOAD = XInternAtom(dpy, "_OPTIC_DE_RELOAD", False);
    A_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)W, (unsigned)H, 0, 0, 0);
    XStoreName(dpy, win, "Settings");
    XClassHint ch = { "optic-settings", "optic-settings" };
    XSetClassHint(dpy, win, &ch);
    XSetWMProtocols(dpy, win, &A_DELETE, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask);
    gc = XCreateGC(dpy, win, 0, NULL);
    build();
    XMapWindow(dpy, win);
    int dragY = 0, dragging = 0;
    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        switch (e.type) {
        case Expose: if (!e.xexpose.count) draw(); break;
        case ConfigureNotify: W = e.xconfigure.width; H = e.xconfigure.height; break;
        case ClientMessage: if ((Atom)e.xclient.data.l[0] == A_DELETE) return 0; break;
        case KeyPress: if (XLookupKeysym(&e.xkey, 0) == XK_Escape || XLookupKeysym(&e.xkey, 0) == XK_BackSpace) back(); break;
        case ButtonPress: {
            int x = e.xbutton.x, y = e.xbutton.y;
            if (e.xbutton.button == 4) { scroll -= ROW; if (scroll < 0) scroll = 0; draw(); break; }
            if (e.xbutton.button == 5) { int m = nrows * ROW - (H - listTop()); scroll += ROW; if (scroll > m) scroll = m > 0 ? m : 0; draw(); break; }
            dragY = y; dragging = 0;
            if (x < PAD + PILL + 4 * S && y < PAD + PILL + 4 * S) { backPressed = 1; draw(); break; }
            int i = (y - listTop() + scroll) / ROW;
            pressedRow = (y >= listTop() && i >= 0 && i < nrows) ? i : -1;
            draw();
            break;
        }
        case MotionNotify:
            if ((e.xmotion.state & Button1Mask) && (dragging || abs(e.xmotion.y - dragY) > 6 * S)) {
                dragging = 1; pressedRow = -1;
                scroll -= e.xmotion.y - dragY; dragY = e.xmotion.y;
                int m = nrows * ROW - (H - listTop());
                if (scroll > m) scroll = m; if (scroll < 0) scroll = 0;
                draw();
            }
            break;
        case ButtonRelease:
            if (backPressed) { backPressed = 0; back(); break; }
            if (pressedRow >= 0 && !dragging) { int i = pressedRow; pressedRow = -1; openRow(i); }
            pressedRow = -1;
            draw();
            break;
        }
    }
}
