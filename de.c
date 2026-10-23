/*
 * Optic DE — the whole desktop in one small X11 process:
 *  - window manager: apps open as windows in MinUI frames (a dark title pill; minimise,
 *    maximise and close as round pills that light up white when pressed). Drag the title bar to
 *    move, double-click it to maximise, drag the bottom-right corner to resize. Apps that draw
 *    their own title bar (Firefox, GNOME Files) get no frame and open maximised. Dialogs float,
 *    centred. Click-to-focus; EWMH basics so apps and the taskbar agree on what's open.
 *  - wallpaper: the stone with the Optic eye, set as the root background;
 *  - taskbar: a MinUI pill, only as long as what's in it, centred at the bottom. It comes up when
 *    the pointer moves into the bottom edge (or on a click there) and fades out after a moment
 *    without use: super (the Optic logo) · Terminal · Firefox · Files · Settings · other open apps
 *    · the clock (month/day + two-letter weekday over the time with seconds, as in Trash and Bus);
 *  - super menu: every installed app, as a MinUI list.
 * Right-click a taskbar app to close it.
 */
#include "ui.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/Xcursor/Xcursor.h>
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHARE "/opt/optic-de/share"
/* Sizes at scale 1; S (OPTIC_SCALE) multiplies them on a native-resolution phone desktop. */
static int S = 1;
#define PH (44 * S)        /* taskbar height */
#define MARGIN (8 * S)     /* gap below it */
#define ICON (36 * S)      /* item slot */
#define EDGE (6 * S)       /* the strip at the bottom edge that brings the taskbar back */
#define TB (34 * S)        /* window title bar */
#define GRIP (22 * S)      /* bottom-right corner that resizes */
#define ROWH (34 * S)
#define MENU_TOP (44 * S)
#define HINTW (80 * S)

enum { IC_LOGO, IC_TERM, IC_WEB, IC_FILES, IC_GEAR, IC_LETTER };

typedef struct { const char *label, *cmd, *cls; int icon; } Pin;
static Pin pins[] = {
    { "Terminal", "optic-term", "optic-term", IC_TERM },
    { "Firefox", "optic-firefox", "firefox", IC_WEB },
    { "Files", "optic-files", "optic-files", IC_FILES },
    { "Settings", "optic-settings", "optic-settings", IC_GEAR },
};
#define NPINS (int)(sizeof pins / sizeof pins[0])

typedef struct {
    Window w, frame;          /* the app's window; our frame around it (0 = none) */
    char cls[64], name[128];
    int x, y, cw, ch;         /* the app area (inside the frame), root coordinates */
    int nx, ny, ncw, nch;     /* the size before maximising */
    int dialog, max, iconic, ignoreUnmap, snapped;
    double quietUntil;        /* the preloaded Firefox: its own "show me" requests are ignored until then */
} Client;
#define MAXC 128
static Client cl[MAXC];
static int ncl;
static Window focused;

static Display *dpy;
static Window root;
static UI ui;
static int SW, SH;
static Canvas *wall;
static uint8_t *logo; static int logoW, logoH;
static GC gc;

static Atom A_PROTOCOLS, A_DELETE, A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE, A_NET_CLOSE, A_NET_STATE,
    A_NET_FULL, A_NET_TYPE, A_TYPE_DIALOG, A_TYPE_UTILITY, A_TYPE_SPLASH, A_TYPE_NOTIF, A_NET_NAME, A_UTF8,
    A_RELOAD, A_ROOTPMAP, A_CHECK, A_GTK_EXTENTS, A_MOTIF, A_WM_STATE, A_FRAME_EXTENTS,
    A_MOVERESIZE, A_MAX_V, A_MAX_H, A_CHANGE_STATE;

static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }
static int xerror(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; } /* windows vanish: fine */
static Atom atom(const char *n) { return XInternAtom(dpy, n, False); }

static void spawn(const char *cmd) {
    if (fork() == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        /* The desktop ignores its children's exits; the apps it starts must not inherit that
           (it breaks programs that wait for their own helpers, and leaves zombies piling up). */
        signal(SIGCHLD, SIG_DFL);
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* ---- taskbar / menu state (used by the window code to stay on top) -------------------------- */

typedef struct { int kind; int pin; int client; int x, w; } Item; /* 0 super, 1 pin, 2 other app, 3 clock */
static Item items[64];
static int nitems;
static Window panel, edgeWin;
static int panelW, shown, autohide = 1, seconds = 1;
static float fade;               /* 0 invisible .. 1 fully shown */
static int fadeDir;              /* +1 fading in, -1 fading out */
static double lastUse;           /* the last time the taskbar was touched */
static int pressed = -1;
static Canvas *backdrop, *panelFull;
static Window menu;

static void drawPanel(void);
static void layoutPanel(void);

static void keepOnTop(void) {
    if (edgeWin && !shown) XRaiseWindow(dpy, edgeWin);
    if (panel && shown) XRaiseWindow(dpy, panel);
    if (menu) XRaiseWindow(dpy, menu);
}

/* ---- clients ------------------------------------------------------------------------------- */

static int findc(Window w) {
    for (int i = 0; i < ncl; i++) if (cl[i].w == w || (cl[i].frame && cl[i].frame == w)) return i;
    return -1;
}
static Window top(Client *c) { return c->frame ? c->frame : c->w; }

static void publish(void) {
    Window ws[MAXC];
    for (int i = 0; i < ncl; i++) ws[i] = cl[i].w;
    XChangeProperty(dpy, root, A_NET_CLIENT_LIST, XA_WINDOW, 32, PropModeReplace, (unsigned char *)ws, ncl);
    XChangeProperty(dpy, root, A_NET_ACTIVE, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&focused, 1);
}

static void drawFrame(Client *c);

static void focus(Window w) {
    Window was = focused;
    focused = w;
    int i = findc(w);
    if (i >= 0) {
        XRaiseWindow(dpy, top(&cl[i]));
        XSetInputFocus(dpy, cl[i].w, RevertToPointerRoot, CurrentTime);
        drawFrame(&cl[i]);
    } else XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    int j = findc(was);
    if (j >= 0 && j != i) drawFrame(&cl[j]);
    keepOnTop();
    publish();
    drawPanel();
}

static void workArea(int *x, int *y, int *w, int *h) {
    *x = 0; *y = 0; *w = SW; *h = autohide ? SH : SH - PH - MARGIN * 2;
}

/* Tells the app where it really is (it can't see the frame), as the ICCCM asks. */
static void tellGeometry(Client *c) {
    XConfigureEvent ce = { 0 };
    ce.type = ConfigureNotify; ce.event = c->w; ce.window = c->w;
    ce.x = c->x; ce.y = c->y; ce.width = c->cw; ce.height = c->ch; ce.above = None;
    XSendEvent(dpy, c->w, False, StructureNotifyMask, (XEvent *)&ce);
}

static void shapeFrame(Client *c);
static void roundWindow(Window win, int w, int h, int square);

/* Puts the frame and the app where the client's numbers say. */
static void apply(Client *c) {
    if (c->frame) {
        XMoveResizeWindow(dpy, c->frame, c->x, c->y - TB, (unsigned)c->cw, (unsigned)(c->ch + TB));
        XMoveResizeWindow(dpy, c->w, 0, TB, (unsigned)c->cw, (unsigned)c->ch);
        shapeFrame(c);
        drawFrame(c);
    } else {
        XMoveResizeWindow(dpy, c->w, c->x, c->y, (unsigned)c->cw, (unsigned)c->ch);
        if (!c->dialog) roundWindow(c->w, c->cw, c->ch, c->max || c->snapped);
    }
    tellGeometry(c);
}

static void maximise(Client *c, int on) {
    int x, y, w, h;
    if (on && c->snapped) { c->snapped = 0; } /* keep the pre-snap size as the restore size */
    else if (on && !c->max) { c->nx = c->x; c->ny = c->y; c->ncw = c->cw; c->nch = c->ch; }
    workArea(&x, &y, &w, &h);
    c->max = on;
    if (on) { c->x = x; c->y = y + (c->frame ? TB : 0); c->cw = w; c->ch = h - (c->frame ? TB : 0); }
    else if (c->ncw > 0) { c->x = c->nx; c->y = c->ny; c->cw = c->ncw; c->ch = c->nch; }
    else { c->cw = w * 3 / 4; c->ch = h * 3 / 4; c->x = x + (w - c->cw) / 2; c->y = y + (h - c->ch) / 2; }
    /* Tell the app (its own maximise / restore icon follows this). */
    Atom st[2] = { A_MAX_V, A_MAX_H };
    XChangeProperty(dpy, c->w, A_NET_STATE, XA_ATOM, 32, PropModeReplace, (unsigned char *)st, on ? 2 : 0);
    apply(c);
}

/* Keeps a window's frame on screen and no bigger than the work area. */
static void fit(Client *c) {
    int x, y, w, h;
    workArea(&x, &y, &w, &h);
    int tb = c->frame ? TB : 0;
    if (c->cw > w) c->cw = w;
    if (c->ch + tb > h) c->ch = h - tb;
    if (c->x < x) c->x = x;
    if (c->y - tb < y) c->y = y + tb;
    if (c->x + c->cw > x + w) c->x = x + w - c->cw;
    if (c->y + c->ch > y + h) c->y = y + h - c->ch;
}

static int hasAtom(Window w, Atom prop, Atom want) {
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    int hit = 0;
    if (XGetWindowProperty(dpy, w, prop, 0, 32, False, XA_ATOM, &type, &fmt, &n, &after, &data) == Success && data) {
        for (unsigned long i = 0; i < n; i++) if (((Atom *)data)[i] == want) hit = 1;
        XFree(data);
    }
    return hit;
}

static int hasProp(Window w, Atom prop) {
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    int ok = XGetWindowProperty(dpy, w, prop, 0, 8, False, AnyPropertyType, &type, &fmt, &n, &after, &data) == Success && type != None;
    if (data) XFree(data);
    return ok;
}

/* Motif hints with decorations switched off: the app draws its own (GTK client-side frames). */
/* Optic's own apps that draw their own title bar but open as normal windows (Optic Terminal). */
static int ownBar(Client *c) { return !strcmp(c->cls, "optic-term"); }

static int noDecor(Window w) {
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    int off = 0;
    if (XGetWindowProperty(dpy, w, A_MOTIF, 0, 5, False, AnyPropertyType, &type, &fmt, &n, &after, &data) == Success && data) {
        long *h = (long *)data;
        if (n >= 3 && (h[0] & 2) && h[2] == 0) off = 1;
        XFree(data);
    }
    return off;
}

static void readName(Client *c) {
    XTextProperty tp;
    c->name[0] = 0;
    if (XGetTextProperty(dpy, c->w, &tp, A_NET_NAME) && tp.value) { snprintf(c->name, sizeof c->name, "%s", (char *)tp.value); XFree(tp.value); }
    else if (XGetWMName(dpy, c->w, &tp) && tp.value) { snprintf(c->name, sizeof c->name, "%s", (char *)tp.value); XFree(tp.value); }
}

static int cascade;
static int preloadPending;   /* a Firefox started in the background, not yet shown */
static double preloadAt;

static void restore(int i);

static void manage(Window w, int wasMapped) {
    int known = findc(w);
    if (known >= 0) {
        /* An app mapping its window again: a preloaded Firefox stays tucked away while it starts;
           a minimised window comes back properly. */
        if (now() < cl[known].quietUntil) return;
        if (cl[known].iconic) restore(known); else XMapWindow(dpy, w);
        return;
    }
    if (ncl >= MAXC) { XMapWindow(dpy, w); return; }
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect) { XMapWindow(dpy, w); return; }
    Client *c = &cl[ncl++];
    memset(c, 0, sizeof *c);
    c->w = w;
    XClassHint ch;
    if (XGetClassHint(dpy, w, &ch)) {
        snprintf(c->cls, sizeof c->cls, "%s", ch.res_class ? ch.res_class : "");
        for (char *p = c->cls; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    readName(c);
    Window tf;
    XSizeHints sh; long sup;
    int fixed = XGetWMNormalHints(dpy, w, &sh, &sup) && (sh.flags & PMinSize) && (sh.flags & PMaxSize) &&
                sh.min_width == sh.max_width && sh.min_height == sh.max_height;
    c->dialog = XGetTransientForHint(dpy, w, &tf) || fixed ||
        hasAtom(w, A_NET_TYPE, A_TYPE_DIALOG) || hasAtom(w, A_NET_TYPE, A_TYPE_UTILITY) ||
        hasAtom(w, A_NET_TYPE, A_TYPE_SPLASH) || hasAtom(w, A_NET_TYPE, A_TYPE_NOTIF);
    int csd = hasProp(w, A_GTK_EXTENTS) || noDecor(w);
    int splash = hasAtom(w, A_NET_TYPE, A_TYPE_SPLASH) || hasAtom(w, A_NET_TYPE, A_TYPE_NOTIF);
    XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
    /* Keep its picture when covered: moving windows over it shows it at once, no repaint. */
    XSetWindowAttributes bs = { .backing_store = Always };
    XChangeWindowAttributes(dpy, w, CWBackingStore, &bs);

    /* Where it opens: its own size, centred (a little cascade so new windows don't stack exactly). */
    c->cw = wa.width; c->ch = wa.height;
    int x, y, ww, hh;
    workArea(&x, &y, &ww, &hh);
    if (!csd && !splash) {
        XSetWindowAttributes fa = { .backing_store = Always, .bit_gravity = NorthWestGravity };
        c->frame = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, ui.depth, InputOutput, ui.vis, CWBackingStore | CWBitGravity, &fa);
        XSelectInput(dpy, c->frame, ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
                     SubstructureRedirectMask | SubstructureNotifyMask);
        XAddToSaveSet(dpy, w);
        XSetWindowBorderWidth(dpy, w, 0);
        if (wasMapped) c->ignoreUnmap++;
        XReparentWindow(dpy, w, c->frame, 0, TB);
        long ext[4] = { 0, 0, TB, 0 };
        XChangeProperty(dpy, w, A_FRAME_EXTENTS, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)ext, 4);
    }
    int tb = c->frame ? TB : 0;
    c->x = x + (ww - c->cw) / 2 + (c->dialog ? 0 : cascade);
    c->y = y + (hh - c->ch - tb) / 2 + tb + (c->dialog ? 0 : cascade);
    if (!c->dialog) cascade = (cascade + 28 * S) % (140 * S);
    fit(c);
    /* Apps with their own title bar (Firefox, Files) fill the screen; big windows too. */
    if (!c->dialog && ((csd && !ownBar(c)) || c->cw >= ww * 9 / 10)) maximise(c, 1);
    else apply(c);
    /* Click-to-focus: catch the first click, then let it through to the app. */
    XGrabButton(dpy, AnyButton, AnyModifier, w, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
    long state[2] = { NormalState, None };
    XChangeProperty(dpy, w, A_WM_STATE, A_WM_STATE, 32, PropModeReplace, (unsigned char *)state, 2);
    if (preloadPending && strstr(c->cls, "firefox")) {
        /* The preloaded Firefox: ready but tucked away until its taskbar button is tapped. */
        preloadPending = 0;
        c->iconic = 1;
        c->quietUntil = now() + 25;  /* GTK asks to be shown as it starts: not now */
        long ic[2] = { IconicState, None };
        XChangeProperty(dpy, w, A_WM_STATE, A_WM_STATE, 32, PropModeReplace, (unsigned char *)ic, 2);
        if (c->frame) XMapWindow(dpy, w);
        layoutPanel();
        return;
    }
    XMapWindow(dpy, w);
    if (c->frame) XMapWindow(dpy, c->frame);
    focus(w);
    layoutPanel();
}

static void unmanage(int i, int gone) {
    Client c = cl[i];
    memmove(&cl[i], &cl[i + 1], sizeof(Client) * (size_t)(ncl - i - 1));
    ncl--;
    if (c.frame) {
        if (!gone) { XReparentWindow(dpy, c.w, root, c.x, c.y); XRemoveFromSaveSet(dpy, c.w); }
        XDestroyWindow(dpy, c.frame);
    }
    if (focused == c.w) {
        Window next = 0;
        for (int k = ncl - 1; k >= 0; k--) if (!cl[k].iconic) { next = cl[k].w; break; }
        focus(next);
    }
    layoutPanel();
    publish();
}

static void closeWindow(Window w) {
    Atom *prot; int n; int polite = 0;
    if (XGetWMProtocols(dpy, w, &prot, &n)) {
        for (int i = 0; i < n; i++) if (prot[i] == A_DELETE) polite = 1;
        XFree(prot);
    }
    if (polite) {
        XEvent e = { 0 };
        e.xclient.type = ClientMessage; e.xclient.window = w; e.xclient.message_type = A_PROTOCOLS;
        e.xclient.format = 32; e.xclient.data.l[0] = (long)A_DELETE; e.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &e);
    } else XKillClient(dpy, w);
}

static void iconify(int i) {
    Client *c = &cl[i];
    c->iconic = 1;
    if (c->frame) XUnmapWindow(dpy, c->frame);
    else { c->ignoreUnmap++; XUnmapWindow(dpy, c->w); }
    Window next = 0;
    for (int k = ncl - 1; k >= 0; k--) if (!cl[k].iconic) { next = cl[k].w; break; }
    focus(next);
}

static void restore(int i) {
    Client *c = &cl[i];
    if (c->iconic) {
        c->iconic = 0;
        long st[2] = { NormalState, None };
        XChangeProperty(dpy, c->w, A_WM_STATE, A_WM_STATE, 32, PropModeReplace, (unsigned char *)st, 2);
        if (c->frame) XMapWindow(dpy, c->frame); else XMapWindow(dpy, c->w);
    }
    focus(c->w);
}

/* ---- frames: the MinUI title bar --------------------------------------------------------------- */

enum { B_NONE = -1, B_MIN, B_MAX, B_CLOSE };
static int framePressed = B_NONE;
static Window framePressedWin;

/* The three round buttons, right to left: close, maximise, minimise. */
static float btnX(Client *c, int b) { float d = TB - 10 * S; return c->cw - 8 * S - d / 2 - (2 - b) * (d + 6 * S); }

static int buttonAt(Client *c, int x, int y) {
    float d = TB - 10 * S;
    /* generous: a finger-tap a little off the circle still hits it (checked before any resize) */
    for (int b = 0; b < 3; b++) if (hypotf(x - btnX(c, b), y - TB / 2.f) <= d / 2 + 7 * S) return b;
    return B_NONE;
}

/* Rounded corners, all four (square when maximised): frames, and apps that draw their own title
   bar (Firefox), which have no frame to round. */
static void roundWindow(Window win, int w, int h, int square) {
    float r = 14.f * S;
    if (square || w < 2 * r || h < 2 * r) { XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, None, ShapeSet); return; }
    Pixmap mask = XCreatePixmap(dpy, win, (unsigned)w, (unsigned)h, 1);
    GC mg = XCreateGC(dpy, mask, 0, NULL);
    XSetForeground(dpy, mg, 1);
    XFillRectangle(dpy, mask, mg, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, mg, 0);
    for (int y = 0; y < (int)r; y++) {
        float dy = r - (y + .5f);
        int cut = (int)floorf(r - sqrtf(fmaxf(0, r * r - dy * dy)));
        if (cut > 0) {
            XFillRectangle(dpy, mask, mg, 0, y, (unsigned)cut, 1); XFillRectangle(dpy, mask, mg, w - cut, y, (unsigned)cut, 1);
            XFillRectangle(dpy, mask, mg, 0, h - 1 - y, (unsigned)cut, 1); XFillRectangle(dpy, mask, mg, w - cut, h - 1 - y, (unsigned)cut, 1);
        }
    }
    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(dpy, mg);
    XFreePixmap(dpy, mask);
}

static void shapeFrame(Client *c) { roundWindow(c->frame, c->cw, c->ch + TB, c->max || c->snapped); }

static void drawFrame(Client *c) {
    if (!c->frame || c->cw <= 0) return;
    int w = c->cw;
    int act = c->w == focused;
    Canvas *cv = cv_new(w, TB);
    cv_clear(cv, C_BLACK);
    float d = TB - 10 * S;
    /* the title in its pill */
    int tw = ui_text_w(&ui, ui.small, c->name[0] ? c->name : c->cls);
    float maxTw = btnX(c, B_MIN) - d / 2 - 26 * S;
    float pw = fminf(tw + 28 * S, maxTw);
    if (pw > 0) cv_pill(cv, 6 * S, 5 * S, pw, d, C_DARK, 1);
    for (int b = 0; b < 3; b++) {
        float cx = btnX(c, b), cy = TB / 2.f;
        int on = framePressed == b && framePressedWin == c->frame;
        cv_circle(cv, cx, cy, d / 2, on ? C_WHITE : C_DARK, 1);
        uint32_t g = on ? C_BLACK : (act ? C_WHITE : C_GRAY);
        float a = d * .2f, t = 1.8f * S;
        if (b == B_MIN) cv_line(cv, cx - a, cy, cx + a, cy, t, g, 1);
        if (b == B_MAX) {
            if (c->max) { cv_rrect_outline(cv, cx - a * .7f, cy - a * .45f, a * 1.5f, a * 1.5f, 2 * S, t, g, 1); cv_line(cv, cx - a * .35f, cy - a * .95f, cx + a * .95f, cy - a * .95f, t, g, 1); cv_line(cv, cx + a * .95f, cy - a * .95f, cx + a * .95f, cy + a * .35f, t, g, 1); }
            else cv_rrect_outline(cv, cx - a, cy - a, a * 2, a * 2, 2.5f * S, t, g, 1);
        }
        if (b == B_CLOSE) { cv_line(cv, cx - a, cy - a, cx + a, cy + a, t, g, 1); cv_line(cv, cx + a, cy - a, cx - a, cy + a, t, g, 1); }
    }
    Pixmap pm = XCreatePixmap(dpy, c->frame, (unsigned)w, (unsigned)TB, (unsigned)ui.depth);
    cv_put(&ui, cv, pm, gc, 0, 0);
    if (pw > 28 * S) {
        XftDraw *xd = XftDrawCreate(dpy, pm, ui.vis, ui.cmap);
        XRectangle clip = { (short)(6 * S), 0, (unsigned short)(pw - 14 * S), (unsigned short)TB };
        XftDrawSetClipRectangles(xd, 0, 0, &clip, 1);
        ui_text(&ui, xd, ui.small, 6 * S + 14 * S, ui_baseline(ui.small, 5 * S, (int)d), act ? C_WHITE : C_GRAY, c->name[0] ? c->name : c->cls);
        XftDrawDestroy(xd);
    }
    XCopyArea(dpy, pm, c->frame, gc, 0, 0, (unsigned)w, (unsigned)TB, 0, 0);
    XFreePixmap(dpy, pm);
    cv_free(cv);
}

/* ---- snapping (as on Windows, in the MinUI look) ------------------------------------------------
 * Pull a window to the top: the snap card drops in at the top centre with layouts (halves, thirds,
 * quarters, full); move over a zone and it lights up white, let go to drop the window there. Pull
 * it to the left / right edge for that half, a corner for that quarter; the very top (off the card)
 * maximises. A white rounded outline shows where it will land. Pulling a snapped window away
 * gives it back its old size under the finger. */

typedef struct { float x, y, w, h; } Zone;
typedef struct { int n; Zone z[4]; } Layout;
static const Layout layouts[] = {
    { 2, { { 0, 0, .5f, 1 }, { .5f, 0, .5f, 1 } } },
    { 2, { { 0, 0, 1, .5f }, { 0, .5f, 1, .5f } } },
    { 2, { { 0, 0, .667f, 1 }, { .667f, 0, .333f, 1 } } },
    { 3, { { 0, 0, .333f, 1 }, { .333f, 0, .334f, 1 }, { .667f, 0, .333f, 1 } } },
    { 4, { { 0, 0, .5f, .5f }, { .5f, 0, .5f, .5f }, { 0, .5f, .5f, .5f }, { .5f, .5f, .5f, .5f } } },
    { 1, { { 0, 0, 1, 1 } } },
};
#define NLAYOUTS (int)(sizeof layouts / sizeof layouts[0])
#define GW (96 * S)        /* one layout's mini screen */
#define GH (56 * S)
#define GPAD (12 * S)

static Window fly, preview;
static int flyW, flyH, flyX, flyY;
static int hotL = -1, hotZ = -1;        /* the zone under the pointer on the card */
static int snapOn;                      /* a target is showing */
static Zone snapZ;                      /* where the window will go (fractions of the work area) */
static Canvas *flyBack;

static void groupRect(int l, float *gx, float *gy) { *gx = GPAD + l * (GW + GPAD); *gy = GPAD; }

/* A zone's box inside its mini screen (card coordinates). */
static void zoneBox(int l, int k, float *x, float *y, float *w, float *h) {
    float gx, gy; groupRect(l, &gx, &gy);
    float in = 5 * S, gap = 2.5f * S;
    const Zone *z = &layouts[l].z[k];
    *x = gx + in + z->x * (GW - 2 * in) + (z->x > 0 ? gap : 0);
    *y = gy + in + z->y * (GH - 2 * in) + (z->y > 0 ? gap : 0);
    *w = z->w * (GW - 2 * in) - (z->x > 0 ? gap : 0) - (z->x + z->w < .999f ? gap : 0);
    *h = z->h * (GH - 2 * in) - (z->y > 0 ? gap : 0) - (z->y + z->h < .999f ? gap : 0);
}

static void drawFly(void) {
    if (!fly) return;
    Canvas *c = cv_new(flyW, flyH);
    cv_blit(c, flyBack, 0, 0);
    cv_rrect(c, 0, 0, flyW, flyH, 22 * S, C_BLACK, 0.95f);
    cv_rrect_outline(c, 0.75f * S, 0.75f * S, flyW - 1.5f * S, flyH - 1.5f * S, 21 * S, 1.2f * S, C_DARK, 1);
    for (int l = 0; l < NLAYOUTS; l++) {
        float gx, gy; groupRect(l, &gx, &gy);
        cv_rrect(c, gx, gy, GW, GH, 12 * S, 0x141414, 1);
        for (int k = 0; k < layouts[l].n; k++) {
            float x, y, w, h; zoneBox(l, k, &x, &y, &w, &h);
            cv_rrect(c, x, y, w, h, 7 * S, (l == hotL && k == hotZ) ? C_WHITE : C_DARK, 1);
        }
    }
    cv_put(&ui, c, fly, gc, 0, 0);
    cv_free(c);
}

static void showFly(void) {
    if (fly) return;
    flyW = GPAD + NLAYOUTS * (GW + GPAD);
    flyH = GH + 2 * GPAD;
    flyX = (SW - flyW) / 2; flyY = 10 * S;
    cv_free(flyBack);
    flyBack = cv_new(flyW, flyH);
    XImage *im = XGetImage(dpy, root, flyX, flyY, (unsigned)flyW, (unsigned)flyH, AllPlanes, ZPixmap);
    if (im) {
        for (int j = 0; j < flyH; j++) for (int i = 0; i < flyW; i++) flyBack->px[j * flyW + i] = (uint32_t)XGetPixel(im, i, j) & 0xFFFFFF;
        XDestroyImage(im);
    }
    XSetWindowAttributes sa = { .override_redirect = True, .save_under = True };
    fly = XCreateWindow(dpy, root, flyX, flyY, (unsigned)flyW, (unsigned)flyH, 0, ui.depth, InputOutput, ui.vis, CWOverrideRedirect | CWSaveUnder, &sa);
    /* round the card's corners */
    Pixmap mask = XCreatePixmap(dpy, fly, (unsigned)flyW, (unsigned)flyH, 1);
    GC mg = XCreateGC(dpy, mask, 0, NULL);
    XSetForeground(dpy, mg, 0); XFillRectangle(dpy, mask, mg, 0, 0, (unsigned)flyW, (unsigned)flyH);
    XSetForeground(dpy, mg, 1);
    float r = 22.f * S;
    for (int yy = 0; yy < flyH; yy++) {
        float dy = yy + .5f < r ? r - (yy + .5f) : (yy + .5f > flyH - r ? yy + .5f - (flyH - r) : 0);
        float dx = r - sqrtf(fmaxf(0, (r + .5f) * (r + .5f) - dy * dy));
        int x0 = dy > 0 ? (int)floorf(dx) : 0;
        XFillRectangle(dpy, mask, mg, x0, yy, (unsigned)(flyW - 2 * x0), 1);
    }
    XShapeCombineMask(dpy, fly, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(dpy, mg); XFreePixmap(dpy, mask);
    XMapRaised(dpy, fly);
    drawFly();
}

static void hideFly(void) { if (fly) { XDestroyWindow(dpy, fly); fly = 0; hotL = hotZ = -1; } }

/* The landing spot of a zone on screen (with the same gaps as the frames have from the edges). */
static void zoneRect(const Zone *z, int *x, int *y, int *w, int *h) {
    int ax, ay, aw, ah;
    workArea(&ax, &ay, &aw, &ah);
    /* Flush: zones meet each other and the screen's edges, no gaps. */
    int x0 = ax + (int)(z->x * aw + .5f), x1 = ax + (int)((z->x + z->w) * aw + .5f);
    int y0 = ay + (int)(z->y * ah + .5f), y1 = ay + (int)((z->y + z->h) * ah + .5f);
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

/* Fills a rounded box in a 1-bit mask with [val]. */
static void fillRound(Pixmap m, GC g, int x, int y, int w, int h, float r, int val) {
    XSetForeground(dpy, g, (unsigned long)val);
    for (int yy = 0; yy < h; yy++) {
        float dy = yy + .5f < r ? r - (yy + .5f) : (yy + .5f > h - r ? yy + .5f - (h - r) : 0);
        int cut = dy > 0 ? (int)floorf(r - sqrtf(fmaxf(0, r * r - dy * dy))) : 0;
        if (w - 2 * cut > 0) XFillRectangle(dpy, m, g, x + cut, y + yy, (unsigned)(w - 2 * cut), 1);
    }
}

/* The white rounded outline where the window will land (a shaped ring: no compositor needed). */
static void showPreview(const Zone *z) {
    int x, y, w, h;
    zoneRect(z, &x, &y, &w, &h);
    int t = 3 * S;
    if (!preview) {
        XSetWindowAttributes sa = { .override_redirect = True, .background_pixel = 0xFFFFFF };
        preview = XCreateWindow(dpy, root, x, y, (unsigned)w, (unsigned)h, 0, ui.depth, InputOutput, ui.vis, CWOverrideRedirect | CWBackPixel, &sa);
    }
    XMoveResizeWindow(dpy, preview, x, y, (unsigned)w, (unsigned)h);
    Pixmap mask = XCreatePixmap(dpy, preview, (unsigned)w, (unsigned)h, 1);
    GC mg = XCreateGC(dpy, mask, 0, NULL);
    XSetForeground(dpy, mg, 0); XFillRectangle(dpy, mask, mg, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, mg, 1);
    /* a rounded band: the outer rounded box, minus the inner one */
    fillRound(mask, mg, 0, 0, w, h, 14.f * S, 1);
    fillRound(mask, mg, t, t, w - 2 * t, h - 2 * t, 14.f * S - t, 0);
    XShapeCombineMask(dpy, preview, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(dpy, mg); XFreePixmap(dpy, mask);
    XMapRaised(dpy, preview);
    if (fly) XRaiseWindow(dpy, fly);
}

static void hidePreview(void) { if (preview) { XDestroyWindow(dpy, preview); preview = 0; } }

/* While a window is being moved: which snap target is the pointer asking for? */
static void snapTrack(int rx, int ry, int wx, int ww) {
    int edge = 8 * S;
    snapOn = 0;
    int oldL = hotL, oldZ = hotZ;
    hotL = hotZ = -1;
    if (ry < edge) showFly();
    if (fly) {
        int fx = rx - flyX, fy = ry - flyY;
        for (int l = 0; l < NLAYOUTS && hotL < 0; l++)
            for (int k = 0; k < layouts[l].n; k++) {
                float x, y, w, h; zoneBox(l, k, &x, &y, &w, &h);
                if (fx >= x - 3 * S && fx < x + w + 3 * S && fy >= y - 3 * S && fy < y + h + 3 * S) { hotL = l; hotZ = k; break; }
            }
        if (hotL >= 0) { snapOn = 1; snapZ = layouts[hotL].z[hotZ]; }
        else if (ry < edge) { snapOn = 1; snapZ = (Zone){ 0, 0, 1, 1 }; }
        else if (ry > flyY + flyH + 60 * S) hideFly();
        if (fly && (hotL != oldL || hotZ != oldZ)) drawFly();
    }
    if (!snapOn) {
        /* the pointer at a side, or the window pushed a quarter off it (a phone's trackpad
           may run out of finger before the pointer reaches the edge) */
        int left = rx < edge || wx < -ww / 4, right = rx > SW - 1 - edge || wx + ww > SW + ww / 4;
        if (left || right) {
            float x = left ? 0 : .5f;
            if (ry < SH / 4) snapZ = (Zone){ x, 0, .5f, .5f };
            else if (ry > SH * 3 / 4) snapZ = (Zone){ x, .5f, .5f, .5f };
            else snapZ = (Zone){ x, 0, .5f, 1 };
            snapOn = 1;
        }
    }
    if (snapOn) showPreview(&snapZ); else hidePreview();
}

/* Let go: put the window in its zone. */
static void snapApply(Client *c) {
    if (!snapOn) return;
    snapOn = 0;
    hideFly(); hidePreview();
    if (snapZ.w > .999f && snapZ.h > .999f) { maximise(c, 1); return; }
    if (!c->snapped && !c->max) { c->nx = c->x; c->ny = c->y; c->ncw = c->cw; c->nch = c->ch; }
    c->max = 0; c->snapped = 1;
    int x, y, w, h;
    zoneRect(&snapZ, &x, &y, &w, &h);
    int tb = c->frame ? TB : 0;
    c->x = x; c->y = y + tb; c->cw = w; c->ch = h - tb;
    Atom none = 0; (void)none;
    XChangeProperty(dpy, c->w, A_NET_STATE, XA_ATOM, 32, PropModeReplace, NULL, 0);
    apply(c);
}

/* Starting to move a snapped or maximised window: back to its old size, under the finger. */
static void unsnapForMove(Client *c, int rx, int ry) {
    if (!c->snapped && !c->max) return;
    int w = c->ncw > 0 ? c->ncw : c->cw * 2 / 3, h = c->nch > 0 ? c->nch : c->ch * 2 / 3;
    float fx = c->cw > 0 ? (float)(rx - c->x) / c->cw : .5f;
    c->max = 0; c->snapped = 0;
    c->cw = w; c->ch = h;
    c->x = rx - (int)(fx * w);
    c->y = ry + (c->frame ? TB / 2 : 0);
    XChangeProperty(dpy, c->w, A_NET_STATE, XA_ATOM, 32, PropModeReplace, NULL, 0);
    apply(c);
}

/* Dragging a frame (move) or an app's corner (resize). */
static int dragMode;            /* 0 none, 1 move, 2 resize */
static Client *dragC;
static int dragRX, dragRY, dragX0, dragY0, dragW0, dragH0;
static int dragEdges;           /* which edges a resize moves: 1 left, 2 right, 4 top, 8 bottom */
static int dragPending;         /* a snapped / maximised window: becomes free once it really moves */

/* Edges of a framed window near a root point (within GRIP). */
static int edgesAt(Client *c, int rx, int ry) {
    /* The title bar is never an edge: its buttons (an app's own, for Optic Terminal) live there.
       Sides and bottom: a thin band, so taps meant for the app (or its ×) don't start a resize.
       Bottom corners: a finger-sized square. */
    if (ry < c->y + (c->frame ? 0 : TB)) return 0;
    int l = c->x, r = c->x + c->cw, b = c->y + c->ch, band = 8 * S, e = 0;
    int low = ry > b - GRIP;
    if (rx < l + band || (low && rx < l + GRIP)) e |= 1;
    else if (rx > r - band || (low && rx > r - GRIP)) e |= 2;
    if (ry > b - band || (low && (e & 3))) e |= 8;
    return e;
}

static void startResize(Client *c, int edges, int rx, int ry) {
    dragMode = 2; dragC = c; dragEdges = edges;
    dragRX = rx; dragRY = ry; dragX0 = c->x; dragY0 = c->y; dragW0 = c->cw; dragH0 = c->ch;
    XGrabPointer(dpy, root, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}
static Time lastTitleClick;
static Window lastTitleWin;

static void frameButton(XButtonEvent *e, int i) {
    Client *c = &cl[i];
    if (c->w != focused) focus(c->w);
    if (e->y >= TB) return;
    int b = buttonAt(c, e->x, e->y);
    if (b != B_NONE) { framePressed = b; framePressedWin = c->frame; drawFrame(c); return; }
    /* The frame's outer edges (left, right, top) resize. */
    if (!c->max) {
        /* thin bands only, so the title pill and the buttons stay theirs */
        int edges = (e->x < 6 * S ? 1 : e->x > c->cw - 5 * S ? 2 : 0) | (e->y < 4 * S ? 4 : 0);
        if (edges) { startResize(c, edges, e->x_root, e->y_root); return; }
    }
    /* The title: double-click maximises, drag moves. */
    if (lastTitleWin == c->frame && e->time - lastTitleClick < 350) { lastTitleClick = 0; maximise(c, !c->max); return; }
    lastTitleClick = e->time; lastTitleWin = c->frame;
    dragMode = 1; dragC = c; dragRX = e->x_root; dragRY = e->y_root; dragX0 = c->x; dragY0 = c->y;
    dragPending = c->max || c->snapped; /* unsnap on the first real move, not on a click */
}

static void frameRelease(XButtonEvent *e, int i) {
    Client *c = &cl[i];
    if (framePressed != B_NONE && framePressedWin == c->frame) {
        int b = framePressed;
        framePressed = B_NONE;
        drawFrame(c);
        if (buttonAt(c, e->x, e->y) == b) {
            if (b == B_MIN) iconify(i);
            else if (b == B_MAX) maximise(c, !c->max);
            else closeWindow(c->w);
        }
    }
}

static void dragMotion(int rx, int ry) {
    Client *c = dragC;
    if (!c) return;
    if (dragMode == 1) {
        if (dragPending) {
            if (abs(rx - dragRX) < 12 * S && abs(ry - dragRY) < 12 * S) return;
            dragPending = 0;
            unsnapForMove(c, rx, ry);
            dragRX = rx; dragRY = ry; dragX0 = c->x; dragY0 = c->y;
        }
        snapTrack(rx, ry, dragX0 + rx - dragRX, c->cw);
        c->x = dragX0 + rx - dragRX;
        c->y = dragY0 + ry - dragRY;
        int tb = c->frame ? TB : 0;
        if (c->y - tb < 0) c->y = tb;                 /* the title bar stays reachable */
        if (c->y > SH - 8 * S) c->y = SH - 8 * S;
        XMoveWindow(dpy, top(c), c->x, c->y - tb);
    } else {
        int dx = rx - dragRX, dy = ry - dragRY, minW = 160 * S, minH = 90 * S;
        int x = dragX0, y = dragY0, w = dragW0, h = dragH0;
        if (dragEdges & 2) w = dragW0 + dx;
        if (dragEdges & 8) h = dragH0 + dy;
        if (dragEdges & 1) { w = dragW0 - dx; if (w < minW) w = minW; x = dragX0 + dragW0 - w; }
        if (dragEdges & 4) { h = dragH0 - dy; if (h < minH) h = minH; y = dragY0 + dragH0 - h; }
        c->x = x; c->y = y;
        c->cw = w < minW ? minW : w;
        c->ch = h < minH ? minH : h;
        apply(c);
    }
}

static void dragEnd(void) {
    if (dragC && dragMode == 1) snapApply(dragC);
    if (dragC) tellGeometry(dragC);
    hideFly(); hidePreview(); snapOn = 0; dragPending = 0;
    dragMode = 0; dragC = NULL;
}

/* ---- wallpaper ----------------------------------------------------------------------------- */

static void setWallpaper(void) {
    conf_load();
    cv_free(wall);
    const char *mode = conf_get("wallpaper", "stone");
    Canvas *src = strcmp(mode, "black") ? png_load(SHARE "/wallpaper.png") : NULL;
    if (src && src->w == SW && src->h == SH) wall = src;   /* made for this screen: no scaling */
    else if (src) { wall = cv_cover(src, SW, SH); cv_free(src); }
    else { wall = cv_new(SW, SH); cv_clear(wall, C_BLACK); }
    Pixmap pm = XCreatePixmap(dpy, root, (unsigned)SW, (unsigned)SH, (unsigned)ui.depth);
    cv_put(&ui, wall, pm, gc, 0, 0);
    XSetWindowBackgroundPixmap(dpy, root, pm);
    XChangeProperty(dpy, root, A_ROOTPMAP, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pm, 1);
    XClearWindow(dpy, root);
    XFreePixmap(dpy, pm);
}

/* ---- icons --------------------------------------------------------------------------------- */

static void icon(Canvas *c, int kind, float cx, float cy, float s, uint32_t col) {
    float t = s * 0.11f;
    switch (kind) {
    case IC_LOGO: cv_mask(c, logo, logoW, logoH, cx - s * .62f, cy - s * .5f, s * 1.24f, s, col); break;
    case IC_TERM:
        cv_rrect_outline(c, cx - s * .5f, cy - s * .4f, s, s * .8f, s * .16f, t, col, 1);
        cv_line(c, cx - s * .28f, cy - s * .14f, cx - s * .1f, cy + .02f * s, t, col, 1);
        cv_line(c, cx - s * .1f, cy + .02f * s, cx - s * .28f, cy + s * .18f, t, col, 1);
        cv_line(c, cx + s * .02f, cy + s * .2f, cx + s * .26f, cy + s * .2f, t, col, 1);
        break;
    case IC_WEB:
        cv_ring(c, cx, cy, s * .44f, t, col, 1);
        cv_line(c, cx - s * .44f, cy, cx + s * .44f, cy, t * .8f, col, 1);
        for (int a = 0; a < 360; a += 10) {
            float a0 = a * (float)M_PI / 180, a1 = (a + 10) * (float)M_PI / 180;
            cv_line(c, cx + s * .18f * cosf(a0), cy + s * .44f * sinf(a0), cx + s * .18f * cosf(a1), cy + s * .44f * sinf(a1), t * .8f, col, 1);
        }
        break;
    case IC_FILES:
        cv_rrect_outline(c, cx - s * .48f, cy - s * .28f, s * .96f, s * .66f, s * .1f, t, col, 1);
        cv_line(c, cx - s * .44f, cy - s * .28f, cx - s * .38f, cy - s * .4f, t, col, 1);
        cv_line(c, cx - s * .38f, cy - s * .4f, cx - s * .08f, cy - s * .4f, t, col, 1);
        cv_line(c, cx - s * .08f, cy - s * .4f, cx + s * .0f, cy - s * .28f, t, col, 1);
        break;
    case IC_GEAR:
        cv_ring(c, cx, cy, s * .27f, t * 1.1f, col, 1);
        cv_ring(c, cx, cy, s * .09f, t, col, 1);
        for (int k = 0; k < 8; k++) {
            float a = k * (float)M_PI / 4;
            cv_line(c, cx + cosf(a) * s * .32f, cy + sinf(a) * s * .32f, cx + cosf(a) * s * .45f, cy + sinf(a) * s * .45f, t * 1.3f, col, 1);
        }
        break;
    case IC_LETTER: cv_ring(c, cx, cy, s * .42f, t, col, 1); break;
    }
}

/* ---- taskbar ------------------------------------------------------------------------------- */

static int pinRunning(int p, int *which) {
    for (int i = ncl - 1; i >= 0; i--)
        if (strstr(cl[i].cls, pins[p].cls)) { if (which) *which = i; return 1; }
    return 0;
}

static int clockW(void) {
    int a = ui_text_w(&ui, ui.small, "00/00 WE"), b = ui_text_w(&ui, ui.font, seconds ? "00:00:00" : "00:00");
    return (a > b ? a : b) + 4 * S;
}

static int panelX(void) { return (SW - panelW) / 2; }
static int panelYPos(void) { return SH - PH - MARGIN; }

static void layoutPanel(void) {
    nitems = 0;
    int x = 6 * S;
    items[nitems++] = (Item){ 0, -1, -1, x, ICON + 8 * S }; x += ICON + 8 * S;
    x += 4 * S;
    for (int p = 0; p < NPINS; p++) { items[nitems++] = (Item){ 1, p, -1, x, ICON }; x += ICON + 2 * S; }
    for (int i = 0; i < ncl && nitems < 60; i++) {
        int pinned = 0;
        for (int p = 0; p < NPINS; p++) if (strstr(cl[i].cls, pins[p].cls)) pinned = 1;
        if (pinned || cl[i].dialog) continue;
        int dup = 0;
        for (int k = 0; k < nitems; k++) if (items[k].kind == 2 && !strcmp(cl[items[k].client].cls, cl[i].cls)) dup = 1;
        if (dup) continue;
        items[nitems++] = (Item){ 2, -1, i, x, ICON }; x += ICON + 2 * S;
    }
    x += 10 * S;
    int cw = clockW();
    items[nitems++] = (Item){ 3, -1, -1, x, cw }; x += cw + 16 * S;
    if (x != panelW) {
        panelW = x;
        cv_free(backdrop); backdrop = NULL;
        if (panel) XMoveResizeWindow(dpy, panel, panelX(), panelYPos(), (unsigned)panelW, PH);
    }
    drawPanel();
}

static void grabBackdrop(void) {
    cv_free(backdrop);
    backdrop = cv_new(panelW, PH);
    int x = panelX(), y = panelYPos();
    /* What's on screen there, without the taskbar itself. */
    int was = shown;
    if (was) XUnmapWindow(dpy, panel);
    XSync(dpy, False);
    XImage *im = XGetImage(dpy, root, x, y, (unsigned)panelW, PH, AllPlanes, ZPixmap);
    if (im) {
        for (int j = 0; j < PH; j++) for (int i = 0; i < panelW; i++) backdrop->px[j * panelW + i] = (uint32_t)XGetPixel(im, i, j) & 0xFFFFFF;
        XDestroyImage(im);
    } else cv_blit(backdrop, wall, x, y);
    if (was) XMapRaised(dpy, panel);
}

static void shapePanel(void) {
    Pixmap mask = XCreatePixmap(dpy, panel, (unsigned)panelW, PH, 1);
    GC mg = XCreateGC(dpy, mask, 0, NULL);
    XSetForeground(dpy, mg, 0);
    XFillRectangle(dpy, mask, mg, 0, 0, (unsigned)panelW, PH);
    XSetForeground(dpy, mg, 1);
    float r = PH / 2.f;
    for (int y = 0; y < PH; y++) {
        float dy = fabsf(y + .5f - r);
        float dx = dy < r + .5f ? sqrtf(fmaxf(0, (r + .5f) * (r + .5f) - dy * dy)) : 0;
        int x0 = (int)floorf(r - dx), x1 = (int)ceilf(panelW - r + dx);
        if (x1 > x0) XFillRectangle(dpy, mask, mg, x0, y, (unsigned)(x1 - x0), 1);
    }
    XShapeCombineMask(dpy, panel, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(dpy, mg);
    XFreePixmap(dpy, mask);
}

/* Renders the taskbar fully (shapes + text) into panelFull; drawPanel shows it at [fade]. */
static void renderPanel(void) {
    if (!backdrop) grabBackdrop();
    Canvas *c = cv_new(panelW, PH);
    cv_blit(c, backdrop, 0, 0);
    cv_pill(c, 0, 0, panelW, PH, C_BLACK, 0.92f);
    cv_rrect_outline(c, 0.75f * S, 0.75f * S, panelW - 1.5f * S, PH - 1.5f * S, (PH - 1.5f * S) / 2, 1.2f * S, C_DARK, 1);
    float cy = PH / 2.f;
    for (int k = 0; k < nitems; k++) {
        Item *it = &items[k];
        if (it->kind == 3) continue;
        float cx = it->x + it->w / 2.f;
        int on = k == pressed;
        int running = 0, active = 0, ci = -1;
        if (it->kind == 1) running = pinRunning(it->pin, &ci);
        if (it->kind == 2) { running = 1; ci = it->client; }
        if (ci >= 0 && cl[ci].w == focused && !cl[ci].iconic) active = 1;
        int lit = on || (it->kind == 0 && menu);
        if (lit) cv_pill(c, it->x, 4 * S, it->w, PH - 8 * S, C_WHITE, 1);
        else if (active) cv_pill(c, it->x, 4 * S, it->w, PH - 8 * S, C_DARK, 1);
        int kind = it->kind == 0 ? IC_LOGO : it->kind == 1 ? pins[it->pin].icon : IC_LETTER;
        icon(c, kind, cx, cy - (running ? 2 * S : 0), (it->kind == 0 ? 22 : 20) * S, lit ? C_BLACK : C_WHITE);
        if (running) cv_circle(c, cx, PH - 7 * S, 1.8f * S, lit ? C_BLACK : C_WHITE, 1);
    }
    Pixmap pm = XCreatePixmap(dpy, panel, (unsigned)panelW, PH, (unsigned)ui.depth);
    cv_put(&ui, c, pm, gc, 0, 0);
    XftDraw *xd = XftDrawCreate(dpy, pm, ui.vis, ui.cmap);
    for (int k = 0; k < nitems; k++) {
        Item *it = &items[k];
        if (it->kind == 2) {
            char ch = cl[it->client].cls[0];
            char l[2] = { (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch ? ch : '?'), 0 };
            int w = ui_text_w(&ui, ui.small, l);
            ui_text(&ui, xd, ui.small, it->x + (it->w - w) / 2, ui_baseline(ui.small, 0, PH - 4 * S), k == pressed ? C_BLACK : C_WHITE, l);
        }
        if (it->kind == 3) {
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            static const char *wd[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
            char topS[32], bot[32];
            snprintf(topS, sizeof topS, "%d/%d %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
            if (seconds) snprintf(bot, sizeof bot, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            else snprintf(bot, sizeof bot, "%02d:%02d", tm.tm_hour, tm.tm_min);
            int right = it->x + it->w;
            ui_text(&ui, xd, ui.small, right - ui_text_w(&ui, ui.small, topS), 18 * S, C_GRAY, topS);
            ui_text(&ui, xd, ui.font, right - ui_text_w(&ui, ui.font, bot), 35 * S, C_WHITE, bot);
        }
    }
    XftDrawDestroy(xd);
    /* Keep the finished picture as pixels, so fading is a blend with what's behind. */
    XImage *im = XGetImage(dpy, pm, 0, 0, (unsigned)panelW, PH, AllPlanes, ZPixmap);
    cv_free(panelFull);
    panelFull = cv_new(panelW, PH);
    if (im) {
        for (int j = 0; j < PH; j++) for (int i = 0; i < panelW; i++) panelFull->px[j * panelW + i] = (uint32_t)XGetPixel(im, i, j) & 0xFFFFFF;
        XDestroyImage(im);
    }
    XFreePixmap(dpy, pm);
    cv_free(c);
}

/* Shows the rendered taskbar at the current fade (no re-render: fading only blends). */
static void showFaded(void) {
    if (!panel || !shown || !panelFull || !backdrop) return;
    Canvas *c = cv_new(panelW, PH);
    if (fade >= 1) memcpy(c->px, panelFull->px, (size_t)panelW * PH * 4);
    else {
        float a = fade * fade * (3 - 2 * fade);
        for (int i = 0; i < panelW * PH; i++) {
            uint32_t p = panelFull->px[i], b = backdrop->px[i], o = 0;
            for (int sh = 0; sh <= 16; sh += 8) o |= (uint32_t)(((b >> sh) & 255) + (((int)((p >> sh) & 255) - (int)((b >> sh) & 255)) * a)) << sh;
            c->px[i] = o;
        }
    }
    cv_put(&ui, c, panel, gc, 0, 0);
    cv_free(c);
}

static void drawPanel(void) {
    if (!panel || !shown) return;
    renderPanel();
    showFaded();
}

static void showPanel(void) {
    lastUse = now();
    if (shown && fadeDir >= 0) return;
    if (!shown) {
        layoutPanel();
        XMoveResizeWindow(dpy, panel, panelX(), panelYPos(), (unsigned)panelW, PH);
        cv_free(backdrop); backdrop = NULL;
        grabBackdrop();
        shapePanel();
        shown = 1;
        fade = 0;
        XUnmapWindow(dpy, edgeWin);
        XMapRaised(dpy, panel);
        renderPanel();
    }
    fadeDir = 1;
}

static void hidePanel(void) {
    if (!autohide || menu || pressed >= 0 || !shown) return;
    fadeDir = -1;
}

static void stepFade(void) {
    if (!fadeDir) return;
    fade += fadeDir * 0.1f;
    if (fade >= 1) { fade = 1; fadeDir = 0; }
    if (fade <= 0) {
        fade = 0; fadeDir = 0; shown = 0;
        XUnmapWindow(dpy, panel);
        /* Back to waiting at the edge: only a move into it (not a window appearing under a
           resting pointer) brings the taskbar back, so it can't bounce. */
        XMapRaised(dpy, edgeWin);
        return;
    }
    if (!panelFull) renderPanel();
    showFaded();
}

static int itemAt(int x) {
    for (int k = 0; k < nitems; k++) if (x >= items[k].x && x < items[k].x + items[k].w) return k;
    return -1;
}

static void openMenu(void);
static void closeMenu(void);

static void activate(int k, int button) {
    Item *it = &items[k];
    if (it->kind == 0) { if (menu) closeMenu(); else openMenu(); return; }
    int ci = -1;
    if (it->kind == 1) pinRunning(it->pin, &ci);
    if (it->kind == 2) ci = it->client;
    if (button == 3) { if (ci >= 0) closeWindow(cl[ci].w); return; }
    if (ci < 0) {
        if (it->kind == 1) {
            /* Tapped while the preload is still starting: show it as soon as it arrives. */
            if (preloadPending && strstr(pins[it->pin].cls, "firefox")) { preloadPending = 0; return; }
            spawn(pins[it->pin].cmd);
        }
        return;
    }
    cl[ci].quietUntil = 0; /* tapped: show it, preloaded or not */
    if (cl[ci].w == focused && !cl[ci].iconic) iconify(ci);
    else restore(ci);
}

/* ---- super menu ---------------------------------------------------------------------------- */

typedef struct { char name[96]; char cmd[256]; } App;
static App apps[512];
static int napps;
static int menuW, menuH, menuScroll, menuPressed = -1, menuDragY, menuDragging;
static Canvas *menuBack;

static int appCmp(const void *a, const void *b) { return strcasecmp(((const App *)a)->name, ((const App *)b)->name); }

static void loadApps(void) {
    napps = 0;
    const char *dirs[] = { "/usr/share/applications", "/usr/local/share/applications" };
    for (int d = 0; d < 2; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) && napps < 512) {
            if (!strstr(e->d_name, ".desktop")) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dirs[d], e->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            char line[512], name[96] = "", exec[256] = "";
            int hidden = 0, term = 0, inEntry = 0;
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0] == '[') { inEntry = !strcmp(line, "[Desktop Entry]"); continue; }
                if (!inEntry) continue;
                if (!strncmp(line, "Name=", 5) && !name[0]) snprintf(name, sizeof name, "%.95s", line + 5);
                else if (!strncmp(line, "Exec=", 5) && !exec[0]) snprintf(exec, sizeof exec, "%.255s", line + 5);
                else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true")) hidden = 1;
                else if (!strcmp(line, "Terminal=true")) term = 1;
                else if (!strncmp(line, "OnlyShowIn=", 11)) hidden = 1;
            }
            fclose(f);
            if (hidden || !name[0] || !exec[0]) continue;
            char *p;
            while ((p = strchr(exec, '%'))) { if (p[1]) memmove(p, p + 2, strlen(p + 2) + 1); else *p = 0; }
            App *a = &apps[napps++];
            snprintf(a->name, sizeof a->name, "%s", name);
            if (term) snprintf(a->cmd, sizeof a->cmd, "xterm -e %.240s", exec); else snprintf(a->cmd, sizeof a->cmd, "%s", exec);
        }
        closedir(dir);
    }
    qsort(apps, (size_t)napps, sizeof(App), appCmp);
}

static int menuX(void) { return panelX(); }
static int menuY(void) { return SH - PH - MARGIN * 2 - menuH; }

static void drawMenu(void) {
    if (!menu) return;
    Canvas *c = cv_new(menuW, menuH);
    cv_blit(c, menuBack, 0, 0);
    cv_rrect(c, 0, 0, menuW, menuH, 22 * S, C_BLACK, 0.95f);
    int topY = MENU_TOP;
    for (int i = 0; i < napps; i++) {
        int y = topY + i * ROWH - menuScroll;
        if (y + ROWH < topY || y > menuH - 10 * S) continue;
        if (i == menuPressed) cv_pill(c, 8 * S, y, menuW - 16 * S, ROWH, C_WHITE, 1);
    }
    Pixmap pm = XCreatePixmap(dpy, menu, (unsigned)menuW, (unsigned)menuH, (unsigned)ui.depth);
    cv_put(&ui, c, pm, gc, 0, 0);
    XftDraw *xd = XftDrawCreate(dpy, pm, ui.vis, ui.cmap);
    ui_text(&ui, xd, ui.small, 22 * S, ui_baseline(ui.small, 12 * S, 24 * S), C_DIM, "ALL APPS");
    XRectangle clip = { 0, (short)topY, (unsigned short)menuW, (unsigned short)(menuH - topY - 10 * S) };
    XftDrawSetClipRectangles(xd, 0, 0, &clip, 1);
    for (int i = 0; i < napps; i++) {
        int y = topY + i * ROWH - menuScroll;
        if (y + ROWH < topY || y > menuH - 10 * S) continue;
        ui_text(&ui, xd, ui.font, 22 * S, ui_baseline(ui.font, y, ROWH), i == menuPressed ? C_BLACK : C_WHITE, apps[i].name);
    }
    XftDrawDestroy(xd);
    XCopyArea(dpy, pm, menu, gc, 0, 0, (unsigned)menuW, (unsigned)menuH, 0, 0);
    XFreePixmap(dpy, pm);
    cv_free(c);
}

static void openMenu(void) {
    loadApps();
    menuW = 300 * S;
    menuH = MENU_TOP + napps * ROWH + 12 * S;
    int maxH = SH - PH - MARGIN * 3 - 20 * S;
    if (menuH > maxH) menuH = maxH;
    menuScroll = 0;
    menuPressed = -1;
    cv_free(menuBack);
    menuBack = cv_new(menuW, menuH);
    XImage *im = XGetImage(dpy, root, menuX(), menuY(), (unsigned)menuW, (unsigned)menuH, AllPlanes, ZPixmap);
    if (im) {
        for (int j = 0; j < menuH; j++) for (int i = 0; i < menuW; i++) menuBack->px[j * menuW + i] = (uint32_t)XGetPixel(im, i, j) & 0xFFFFFF;
        XDestroyImage(im);
    }
    XSetWindowAttributes sa = { .override_redirect = True, .save_under = True,
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask };
    menu = XCreateWindow(dpy, root, menuX(), menuY(), (unsigned)menuW, (unsigned)menuH, 0, ui.depth, InputOutput, ui.vis,
                         CWOverrideRedirect | CWEventMask | CWSaveUnder, &sa);
    Pixmap mask = XCreatePixmap(dpy, menu, (unsigned)menuW, (unsigned)menuH, 1);
    GC mg = XCreateGC(dpy, mask, 0, NULL);
    XSetForeground(dpy, mg, 0); XFillRectangle(dpy, mask, mg, 0, 0, (unsigned)menuW, (unsigned)menuH);
    XSetForeground(dpy, mg, 1);
    float r = 22.f * S;
    for (int yy = 0; yy < menuH; yy++) {
        float dy = yy + .5f < r ? r - (yy + .5f) : (yy + .5f > menuH - r ? yy + .5f - (menuH - r) : 0);
        float dx = r - sqrtf(fmaxf(0, (r + .5f) * (r + .5f) - dy * dy));
        int x0 = dy > 0 ? (int)floorf(dx) : 0;
        XFillRectangle(dpy, mask, mg, x0, yy, (unsigned)(menuW - 2 * x0), 1);
    }
    XShapeCombineMask(dpy, menu, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(dpy, mg); XFreePixmap(dpy, mask);
    XMapRaised(dpy, menu);
    XGrabPointer(dpy, menu, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    drawMenu();
    drawPanel();
}

static void closeMenu(void) {
    if (!menu) return;
    XUngrabPointer(dpy, CurrentTime);
    XDestroyWindow(dpy, menu);
    menu = 0;
    lastUse = now();
    drawPanel();
}

static void clampMenuScroll(void) {
    int maxS = MENU_TOP + napps * ROWH + 12 * S - menuH;
    if (maxS < 0) maxS = 0;
    if (menuScroll < 0) menuScroll = 0;
    if (menuScroll > maxS) menuScroll = maxS;
}

static void menuEvent(XEvent *e) {
    int mx = menuX(), my = menuY();
    lastUse = now();
    if (e->type == ButtonPress) {
        int x = e->xbutton.x_root - mx, y = e->xbutton.y_root - my;
        if (x < 0 || y < 0 || x >= menuW || y >= menuH) { closeMenu(); return; }
        if (e->xbutton.button == 4) menuScroll -= ROWH;
        else if (e->xbutton.button == 5) menuScroll += ROWH;
        else { menuDragY = y; menuDragging = 0; menuPressed = (y - MENU_TOP + menuScroll) / ROWH; if (y < MENU_TOP || menuPressed >= napps) menuPressed = -1; }
        clampMenuScroll();
        drawMenu();
    } else if (e->type == MotionNotify && (e->xmotion.state & Button1Mask)) {
        int y = e->xmotion.y_root - my;
        if (abs(y - menuDragY) > 6 * S || menuDragging) {
            menuDragging = 1; menuPressed = -1;
            menuScroll -= y - menuDragY; menuDragY = y;
            clampMenuScroll();
            drawMenu();
        }
    } else if (e->type == ButtonRelease && e->xbutton.button == 1) {
        int i = menuPressed;
        menuPressed = -1;
        if (!menuDragging && i >= 0 && i < napps) { char cmd[256]; snprintf(cmd, sizeof cmd, "%s", apps[i].cmd); closeMenu(); spawn(cmd); return; }
        drawMenu();
    }
}

/* ---- setup --------------------------------------------------------------------------------- */

static void readConf(void) {
    conf_load();
    autohide = strcmp(conf_get("taskbar", "autohide"), "always") != 0;
    seconds = strcmp(conf_get("seconds", "on"), "off") != 0;
}

static void relayout(void) {
    XWindowAttributes ra;
    XGetWindowAttributes(dpy, root, &ra);
    SW = ra.width; SH = ra.height;
    setWallpaper();
    for (int i = 0; i < ncl; i++) { if (cl[i].max) maximise(&cl[i], 1); else { fit(&cl[i]); apply(&cl[i]); } }
    XMoveResizeWindow(dpy, edgeWin, 0, SH - EDGE, (unsigned)SW, EDGE);
    cv_free(backdrop); backdrop = NULL;
    layoutPanel();
    if (!autohide) showPanel();
    if (shown) { XMoveResizeWindow(dpy, panel, panelX(), panelYPos(), (unsigned)panelW, PH); shapePanel(); drawPanel(); }
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "optic-de: no display\n"); return 1; }
    XSetErrorHandler(xerror);
    root = DefaultRootWindow(dpy);
    ui_open(&ui, dpy, SHARE);
    S = ui.scale;
    gc = XCreateGC(dpy, root, 0, NULL);
    logo = a8_load(SHARE "/logo.a8", &logoW, &logoH);
    readConf();

    A_PROTOCOLS = atom("WM_PROTOCOLS"); A_DELETE = atom("WM_DELETE_WINDOW");
    A_NET_SUPPORTED = atom("_NET_SUPPORTED"); A_NET_CLIENT_LIST = atom("_NET_CLIENT_LIST");
    A_NET_ACTIVE = atom("_NET_ACTIVE_WINDOW"); A_NET_CLOSE = atom("_NET_CLOSE_WINDOW");
    A_NET_STATE = atom("_NET_WM_STATE"); A_NET_FULL = atom("_NET_WM_STATE_FULLSCREEN");
    A_NET_TYPE = atom("_NET_WM_WINDOW_TYPE"); A_TYPE_DIALOG = atom("_NET_WM_WINDOW_TYPE_DIALOG");
    A_TYPE_UTILITY = atom("_NET_WM_WINDOW_TYPE_UTILITY"); A_TYPE_SPLASH = atom("_NET_WM_WINDOW_TYPE_SPLASH");
    A_TYPE_NOTIF = atom("_NET_WM_WINDOW_TYPE_NOTIFICATION"); A_NET_NAME = atom("_NET_WM_NAME");
    A_UTF8 = atom("UTF8_STRING"); A_RELOAD = atom("_OPTIC_DE_RELOAD"); A_ROOTPMAP = atom("_XROOTPMAP_ID");
    A_CHECK = atom("_NET_SUPPORTING_WM_CHECK"); A_GTK_EXTENTS = atom("_GTK_FRAME_EXTENTS");
    A_MOTIF = atom("_MOTIF_WM_HINTS"); A_WM_STATE = atom("WM_STATE"); A_FRAME_EXTENTS = atom("_NET_FRAME_EXTENTS");
    A_MOVERESIZE = atom("_NET_WM_MOVERESIZE"); A_MAX_V = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    A_MAX_H = atom("_NET_WM_STATE_MAXIMIZED_HORZ"); A_CHANGE_STATE = atom("WM_CHANGE_STATE");

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);
    XSync(dpy, False);
    Atom sup[] = { A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE, A_NET_CLOSE, A_NET_STATE, A_NET_FULL, A_NET_TYPE, A_NET_NAME, A_CHECK, A_FRAME_EXTENTS,
                   A_MOVERESIZE, A_MAX_V, A_MAX_H };
    XChangeProperty(dpy, root, A_NET_SUPPORTED, XA_ATOM, 32, PropModeReplace, (unsigned char *)sup, (int)(sizeof sup / sizeof sup[0]));
    Window check = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, root, A_CHECK, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
    XChangeProperty(dpy, check, A_CHECK, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
    XChangeProperty(dpy, check, A_NET_NAME, A_UTF8, 8, PropModeReplace, (unsigned char *)"Optic", 5);
    Cursor arrow = XcursorLibraryLoadCursor(dpy, "left_ptr");
    XDefineCursor(dpy, root, arrow ? arrow : XCreateFontCursor(dpy, 68 /* XC_left_ptr */));

    XWindowAttributes ra;
    XGetWindowAttributes(dpy, root, &ra);
    SW = ra.width; SH = ra.height;
    setWallpaper();

    XSetWindowAttributes sa = { .override_redirect = True,
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | EnterWindowMask | LeaveWindowMask | PointerMotionMask };
    sa.save_under = True;
    panel = XCreateWindow(dpy, root, 0, SH, 100, PH, 0, ui.depth, InputOutput, ui.vis, CWOverrideRedirect | CWEventMask | CWSaveUnder, &sa);
    sa.event_mask = PointerMotionMask | ButtonPressMask;
    edgeWin = XCreateWindow(dpy, root, 0, SH - EDGE, (unsigned)SW, EDGE, 0, 0, InputOnly, ui.vis, CWOverrideRedirect | CWEventMask, &sa);
    XMapRaised(dpy, edgeWin);

    Window r0, p0, *kids; unsigned nk;
    if (XQueryTree(dpy, root, &r0, &p0, &kids, &nk)) {
        for (unsigned i = 0; i < nk; i++) {
            XWindowAttributes wa;
            if (kids[i] != panel && kids[i] != edgeWin && kids[i] != check &&
                XGetWindowAttributes(dpy, kids[i], &wa) && !wa.override_redirect && wa.map_state == IsViewable) manage(kids[i], 1);
        }
        if (kids) XFree(kids);
    }
    layoutPanel();
    showPanel(); /* say hello, then fade away */
    /* Firefox takes ~6 s to start under proot: start it in the background once the desktop is up
       (Settings → Preload Firefox), so its button opens it at once. */
    if (strcmp(conf_get("preload", "on"), "off")) preloadAt = now() + 8;
    /* Files' background service (inside this session's D-Bus), so Files opens in a blink. */

    int fd = ConnectionNumber(dpy);
    time_t lastSec = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (menu && (e.type == ButtonPress || e.type == ButtonRelease || e.type == MotionNotify)) { menuEvent(&e); continue; }
            switch (e.type) {
            case MapRequest: manage(e.xmaprequest.window, 0); break;
            case ConfigureRequest: {
                XConfigureRequestEvent *r = &e.xconfigurerequest;
                int i = findc(r->window);
                if (i >= 0 && cl[i].w == r->window) {
                    Client *c = &cl[i];
                    if (!c->max) {
                        if (r->value_mask & CWWidth) c->cw = r->width;
                        if (r->value_mask & CWHeight) c->ch = r->height;
                        if (r->value_mask & CWX) c->x = r->x;
                        if (r->value_mask & CWY) c->y = r->y + (c->frame ? TB : 0);
                        fit(c);
                    }
                    apply(c);
                    if (r->value_mask & CWStackMode) { XRaiseWindow(dpy, top(c)); keepOnTop(); }
                    break;
                }
                XWindowChanges wc = { r->x, r->y, r->width, r->height, r->border_width, r->above, r->detail };
                XConfigureWindow(dpy, r->window, (unsigned)r->value_mask, &wc);
                break;
            }
            case UnmapNotify: {
                int i = findc(e.xunmap.window);
                if (i >= 0 && cl[i].w == e.xunmap.window) { if (cl[i].ignoreUnmap) cl[i].ignoreUnmap--; else unmanage(i, 0); }
                break;
            }
            case DestroyNotify: {
                int i = findc(e.xdestroywindow.window);
                if (i >= 0 && cl[i].w == e.xdestroywindow.window) unmanage(i, 1);
                break;
            }
            case ConfigureNotify: if (e.xconfigure.window == root && (e.xconfigure.width != SW || e.xconfigure.height != SH)) relayout(); break;
            case PropertyNotify:
                if (e.xproperty.window == root && e.xproperty.atom == A_RELOAD) { readConf(); relayout(); break; }
                {
                    int i = findc(e.xproperty.window);
                    if (i >= 0 && (e.xproperty.atom == XA_WM_NAME || e.xproperty.atom == A_NET_NAME)) { readName(&cl[i]); drawFrame(&cl[i]); }
                }
                break;
            case ClientMessage: {
                int i = findc(e.xclient.window);
                if (e.xclient.message_type == A_NET_ACTIVE && i >= 0) { if (now() >= cl[i].quietUntil) restore(i); }
                else if (e.xclient.message_type == A_NET_CLOSE && i >= 0) closeWindow(cl[i].w);
                else if (e.xclient.message_type == A_CHANGE_STATE && i >= 0 && e.xclient.data.l[0] == IconicState) iconify(i);
                else if (e.xclient.message_type == A_NET_STATE && i >= 0) {
                    /* maximise / restore from the app's own button (0 remove, 1 add, 2 toggle) */
                    Atom p1 = (Atom)e.xclient.data.l[1], p2 = (Atom)e.xclient.data.l[2];
                    if (p1 == A_MAX_V || p1 == A_MAX_H || p2 == A_MAX_V || p2 == A_MAX_H) {
                        long how = e.xclient.data.l[0];
                        maximise(&cl[i], how == 2 ? !cl[i].max : how == 1);
                    }
                } else if (e.xclient.message_type == A_MOVERESIZE && i >= 0) {
                    /* dragging the app's own title bar (8) or an edge (0-7); 11 cancels */
                    Client *c = &cl[i];
                    long dir = e.xclient.data.l[2];
                    int rx = (int)e.xclient.data.l[0], ry = (int)e.xclient.data.l[1];
                    if (dir == 11) { if (dragMode == 2) XUngrabPointer(dpy, CurrentTime); dragEnd(); break; }
                    if ((c->max || c->snapped) && dir != 8) break;
                    static const int ed[8] = { 1 | 4, 4, 2 | 4, 2, 2 | 8, 8, 1 | 8, 1 };
                    if (dir == 8) {
                        dragMode = 1; dragC = c; dragRX = rx; dragRY = ry; dragX0 = c->x; dragY0 = c->y;
                        dragPending = c->max || c->snapped;
                        XGrabPointer(dpy, root, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                    } else if (dir >= 0 && dir < 8) startResize(c, ed[dir], rx, ry);
                }
                break;
            }
            case MotionNotify:
                if (e.xmotion.window == edgeWin) { showPanel(); break; }
                if (e.xmotion.window == panel) { lastUse = now(); break; }
                if (dragMode) {
                    /* Only the newest position matters. */
                    while (XCheckTypedEvent(dpy, MotionNotify, &e)) {}
                    dragMotion(e.xmotion.x_root, e.xmotion.y_root);
                }
                break;
            case ButtonPress: {
                if (e.xbutton.window == edgeWin) { showPanel(); break; }
                if (e.xbutton.window == panel) {
                    pressed = itemAt(e.xbutton.x);
                    if (pressed >= 0 && items[pressed].kind == 3) pressed = -1;
                    lastUse = now();
                    drawPanel();
                    break;
                }
                int i = findc(e.xbutton.window);
                if (i >= 0 && cl[i].frame == e.xbutton.window) { frameButton(&e.xbutton, i); break; }
                if (i >= 0) {
                    Client *c = &cl[i];
                    if (c->w != focused) focus(c->w);
                    /* The edges of a framed, unmaximised window (left, right, bottom, corners) resize it. */
                    if ((c->frame || ownBar(c)) && !c->max && e.xbutton.button == 1) {
                        int edges = edgesAt(c, e.xbutton.x_root, e.xbutton.y_root) & 11;
                        if (edges) {
                            XAllowEvents(dpy, AsyncPointer, CurrentTime);
                            startResize(c, edges, e.xbutton.x_root, e.xbutton.y_root);
                            break;
                        }
                    }
                }
                XAllowEvents(dpy, ReplayPointer, CurrentTime);
                break;
            }
            case ButtonRelease:
                if (dragMode) { XUngrabPointer(dpy, CurrentTime); dragEnd(); break; }
                if (e.xbutton.window == panel) {
                    int k = itemAt(e.xbutton.x);
                    int was = pressed;
                    pressed = -1;
                    if (k >= 0 && k == was) activate(k, (int)e.xbutton.button);
                    lastUse = now();
                    drawPanel();
                    break;
                }
                {
                    int i = findc(e.xbutton.window);
                    if (i >= 0 && cl[i].frame == e.xbutton.window) frameRelease(&e.xbutton, i);
                }
                break;
            case Expose:
                if (e.xexpose.count) break;
                if (e.xexpose.window == panel) drawPanel();
                else if (e.xexpose.window == menu) drawMenu();
                else { int i = findc(e.xexpose.window); if (i >= 0 && cl[i].frame == e.xexpose.window) drawFrame(&cl[i]); }
                break;
            }
        }
        /* The clock ticks; the taskbar fades out after a while without use. */
        time_t t = time(NULL);
        if (t != lastSec) { lastSec = t; if (shown && !fadeDir) drawPanel(); }
        if (shown && autohide && !fadeDir && !menu && pressed < 0 && now() - lastUse > 2.5) hidePanel();
        if (preloadAt && now() >= preloadAt) {
            preloadAt = 0;
            int running = 0;
            for (int i = 0; i < ncl; i++) if (strstr(cl[i].cls, "firefox")) running = 1;
            if (!running) { preloadPending = 1; spawn("optic-firefox"); }
        }
        stepFade();
        XFlush(dpy);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { 0, fadeDir ? 16000 : 200000 };
        select(fd + 1, &fds, NULL, NULL, &tv);
    }
}
