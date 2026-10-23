/*
 * Optic Terminal — xterm with tabs, in the MinUI look. The title bar is a tab strip, as in
 * Firefox: one pill per tab (its title and an ×), a round + for a new tab, then the window's
 * own round minimise / maximise / close, drawn exactly like Optic DE's frames. Each tab is an
 * xterm embedded in the window (xterm -into); the active one is raised over the others.
 * Drag the strip to move the window (the desktop snaps it as usual); double-tap it to maximise.
 * Keys: Ctrl+Shift+T new tab, Ctrl+Shift+W close tab, Ctrl+PageUp / PageDown switch.
 */
#include "ui.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define SHARE "/opt/optic-de/share"
static int S = 1;
#define TB (34 * S)          /* same height as Optic DE's title bars */
#define MAXT 24

enum { B_MIN, B_MAX, B_CLOSE, B_NONE = -1 };

typedef struct { Window w; char title[128]; } Tab;

static Display *dpy;
static Window win, body;
static UI ui;
static GC gc;
static int W, H, maxed, active;
static Tab tabs[MAXT];
static int ntabs, cur;
static Atom A_DELETE, A_PROTOCOLS, A_NET_NAME, A_UTF8, A_STATE, A_MAX_V, A_MAX_H, A_MOVERESIZE, A_CHANGE_STATE;

/* What's pressed in the strip: a window button, a tab, a tab's ×, the + (for the white press). */
static int pressBtn = B_NONE, pressTab = -1, pressClose = -1, pressPlus;
static int pressX, pressY, pressRX, pressRY, pressing;
static double lastTitleTap;

static double now(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec + t.tv_usec / 1e6; }

/* ---- geometry of the strip ------------------------------------------------------------------- */

static float dia(void) { return TB - 10 * S; }
static float btnX(int b) { float d = dia(); return W - 8 * S - d / 2 - (2 - b) * (d + 6 * S); }
static float gap(void) { return 6 * S; }

/* Tab pills share the room left of the + and the window buttons, up to a comfortable width. */
static float tabW(void) {
    float d = dia();
    float room = btnX(B_MIN) - d / 2 - 14 * S - (d + gap()) - 6 * S;
    float w = ntabs ? (room - (ntabs - 1) * gap()) / ntabs : room;
    if (w > 220 * S) w = 220 * S;
    if (w < 56 * S) w = 56 * S;
    return w;
}
static float tabX(int i) { return 6 * S + i * (tabW() + gap()); }
static float plusX(void) { return tabX(ntabs) + dia() / 2; }
static float closeX(int i) { return tabX(i) + tabW() - dia() / 2; }

static int buttonAt(int x, int y) {
    float d = dia();
    for (int b = 0; b < 3; b++) if (hypotf(x - btnX(b), y - TB / 2.f) <= d / 2 + 3 * S) return b;
    return B_NONE;
}
static int tabAt(int x, int y) {
    if (y < 3 * S || y > TB - 3 * S) return -1;
    for (int i = 0; i < ntabs; i++) if (x >= tabX(i) && x < tabX(i) + tabW()) return i;
    return -1;
}
static int onClose(int i, int x, int y) { return i >= 0 && hypotf(x - closeX(i), y - TB / 2.f) <= dia() / 2; }
static int onPlus(int x, int y) { return hypotf(x - plusX(), y - TB / 2.f) <= dia() / 2 + 3 * S; }

/* ---- drawing ----------------------------------------------------------------------------------- */

static void drawStrip(void) {
    if (W <= 0) return;
    float d = dia(), tw = tabW();
    Canvas *cv = cv_new(W, TB);
    cv_clear(cv, C_BLACK);
    float t = 1.8f * S;
    for (int i = 0; i < ntabs; i++) {
        float x = tabX(i);
        int on = i == cur;
        cv_pill(cv, x, 5 * S, tw, d, on ? C_DARK : 0x141414, 1);
        /* its × in a circle that lights up white while pressed */
        float cx = closeX(i), cy = TB / 2.f, a = d * .16f;
        int down = pressClose == i;
        if (down) cv_circle(cv, cx, cy, d / 2 - 3 * S, C_WHITE, 1);
        uint32_t g = down ? C_BLACK : (on && active ? C_WHITE : C_GRAY);
        cv_line(cv, cx - a, cy - a, cx + a, cy + a, t, g, 1);
        cv_line(cv, cx + a, cy - a, cx - a, cy + a, t, g, 1);
    }
    /* + : a new tab */
    {
        float cx = plusX(), cy = TB / 2.f, a = d * .2f;
        cv_circle(cv, cx, cy, d / 2, pressPlus ? C_WHITE : C_DARK, 1);
        uint32_t g = pressPlus ? C_BLACK : (active ? C_WHITE : C_GRAY);
        cv_line(cv, cx - a, cy, cx + a, cy, t, g, 1);
        cv_line(cv, cx, cy - a, cx, cy + a, t, g, 1);
    }
    /* the window's buttons, as on Optic DE's frames */
    for (int b = 0; b < 3; b++) {
        float cx = btnX(b), cy = TB / 2.f;
        int on = pressBtn == b;
        cv_circle(cv, cx, cy, d / 2, on ? C_WHITE : C_DARK, 1);
        uint32_t g = on ? C_BLACK : (active ? C_WHITE : C_GRAY);
        float a = d * .2f;
        if (b == B_MIN) cv_line(cv, cx - a, cy, cx + a, cy, t, g, 1);
        if (b == B_MAX) {
            if (maxed) { cv_rrect_outline(cv, cx - a * .7f, cy - a * .45f, a * 1.5f, a * 1.5f, 2 * S, t, g, 1); cv_line(cv, cx - a * .35f, cy - a * .95f, cx + a * .95f, cy - a * .95f, t, g, 1); cv_line(cv, cx + a * .95f, cy - a * .95f, cx + a * .95f, cy + a * .35f, t, g, 1); }
            else cv_rrect_outline(cv, cx - a, cy - a, a * 2, a * 2, 2.5f * S, t, g, 1);
        }
        if (b == B_CLOSE) { cv_line(cv, cx - a, cy - a, cx + a, cy + a, t, g, 1); cv_line(cv, cx + a, cy - a, cx - a, cy + a, t, g, 1); }
    }
    Pixmap pm = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)TB, (unsigned)ui.depth);
    cv_put(&ui, cv, pm, gc, 0, 0);
    XftDraw *xd = XftDrawCreate(dpy, pm, ui.vis, ui.cmap);
    for (int i = 0; i < ntabs; i++) {
        float x = tabX(i);
        int textW = (int)(tw - 14 * S - d);
        if (textW < 8 * S) continue;
        XRectangle clip = { (short)(x + 14 * S), 0, (unsigned short)textW, (unsigned short)TB };
        XftDrawSetClipRectangles(xd, 0, 0, &clip, 1);
        ui_text(&ui, xd, ui.small, (int)(x + 14 * S), ui_baseline(ui.small, 5 * S, (int)d),
                i == cur && active ? C_WHITE : C_GRAY, tabs[i].title[0] ? tabs[i].title : "Terminal");
    }
    XftDrawDestroy(xd);
    XCopyArea(dpy, pm, win, gc, 0, 0, (unsigned)W, (unsigned)TB, 0, 0);
    XFreePixmap(dpy, pm);
    cv_free(cv);
    XFlush(dpy);
}

/* ---- tabs -------------------------------------------------------------------------------------- */

static int findTab(Window w) { for (int i = 0; i < ntabs; i++) if (tabs[i].w == w) return i; return -1; }

static void readTitle(Tab *t) {
    XTextProperty tp;
    t->title[0] = 0;
    if (XGetTextProperty(dpy, t->w, &tp, A_NET_NAME) && tp.value) { snprintf(t->title, sizeof t->title, "%s", (char *)tp.value); XFree(tp.value); }
    else if (XGetWMName(dpy, t->w, &tp) && tp.value) { snprintf(t->title, sizeof t->title, "%s", (char *)tp.value); XFree(tp.value); }
}

/* The window's name follows the open tab (the taskbar shows it). */
static void nameWindow(void) {
    const char *n = ntabs && tabs[cur].title[0] ? tabs[cur].title : "Terminal";
    XStoreName(dpy, win, n);
    XChangeProperty(dpy, win, A_NET_NAME, A_UTF8, 8, PropModeReplace, (const unsigned char *)n, (int)strlen(n));
}

static void focusTab(void) {
    if (!ntabs) return;
    XSetInputFocus(dpy, tabs[cur].w, RevertToParent, CurrentTime);
}

static void show(int i) {
    if (i < 0 || i >= ntabs) return;
    cur = i;
    XRaiseWindow(dpy, tabs[cur].w);
    if (active) focusTab();
    nameWindow();
    drawStrip();
}

static void fitTabs(void) {
    int h = H - TB;
    if (h < 1) h = 1;
    XMoveResizeWindow(dpy, body, 0, TB, (unsigned)W, (unsigned)h);
    for (int i = 0; i < ntabs; i++) XMoveResizeWindow(dpy, tabs[i].w, 0, 0, (unsigned)W, (unsigned)h);
}

static void newTab(void) {
    char id[32];
    snprintf(id, sizeof id, "%lu", (unsigned long)body);
    if (fork() == 0) {
        signal(SIGCHLD, SIG_DFL);
        close(ConnectionNumber(dpy));
        execlp("xterm", "xterm", "-into", id, (char *)NULL);
        _exit(127);
    }
}

/* An xterm arrived in the window: it becomes a tab, and the open one. */
static void adopt(Window w) {
    if (findTab(w) >= 0 || ntabs >= MAXT) return;
    Tab *t = &tabs[ntabs++];
    t->w = w;
    XSelectInput(dpy, w, PropertyChangeMask);
    XSetWindowBorderWidth(dpy, w, 0);
    readTitle(t);
    fitTabs();
    show(ntabs - 1);
}

static void dropTab(Window w) {
    int i = findTab(w);
    if (i < 0) return;
    memmove(&tabs[i], &tabs[i + 1], (size_t)(ntabs - i - 1) * sizeof(Tab));
    ntabs--;
    if (!ntabs) { XCloseDisplay(dpy); exit(0); }  /* the last tab closed: so does the window */
    if (cur >= ntabs || cur > i) cur = cur > 0 ? cur - 1 : 0;
    show(cur);
}

static void closeTab(int i) { if (i >= 0 && i < ntabs) XKillClient(dpy, tabs[i].w); }

static void closeAll(void) {
    for (int i = 0; i < ntabs; i++) XKillClient(dpy, tabs[i].w);
    XSync(dpy, False);
    exit(0);
}

/* ---- the window's own buttons, through the desktop ----------------------------------------- */

static void tellWM(Atom type, long a, long b, long c, long d, long e) {
    XEvent ev = { 0 };
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = a; ev.xclient.data.l[1] = b; ev.xclient.data.l[2] = c;
    ev.xclient.data.l[3] = d; ev.xclient.data.l[4] = e;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static void minimise(void) { tellWM(A_CHANGE_STATE, IconicState, 0, 0, 0, 0); }
static void toggleMax(void) { tellWM(A_STATE, 2, (long)A_MAX_V, (long)A_MAX_H, 1, 0); }

/* Hand the drag to the desktop: it moves the window (and snaps it). */
static void startMove(int rx, int ry) {
    XUngrabPointer(dpy, CurrentTime);
    tellWM(A_MOVERESIZE, rx, ry, 8 /* move */, 1, 1);
}

static void readMax(void) {
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    maxed = 0;
    if (XGetWindowProperty(dpy, win, A_STATE, 0, 16, False, XA_ATOM, &type, &fmt, &n, &after, &data) == Success && data) {
        Atom *a = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) if (a[i] == A_MAX_V || a[i] == A_MAX_H) maxed = 1;
        XFree(data);
    }
}

/* Is the keyboard on this window (or one of its tabs)? */
static int ownsFocus(void) {
    Window f; int rev;
    XGetInputFocus(dpy, &f, &rev);
    while (f && f != PointerRoot) {
        if (f == win) return 1;
        Window r, parent, *kids = NULL; unsigned n;
        if (!XQueryTree(dpy, f, &r, &parent, &kids, &n)) return 0;
        if (kids) XFree(kids);
        if (parent == r) return 0;
        f = parent;
    }
    return 0;
}

static void grabKey(KeySym k, unsigned mods) {
    KeyCode kc = XKeysymToKeycode(dpy, k);
    unsigned extra[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    for (int i = 0; i < 4; i++) XGrabKey(dpy, kc, mods | extra[i], win, True, GrabModeAsync, GrabModeAsync);
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    signal(SIGCHLD, SIG_IGN);
    ui_open(&ui, dpy, SHARE);
    S = ui.scale;
    A_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    A_PROTOCOLS = XInternAtom(dpy, "WM_PROTOCOLS", False);
    A_NET_NAME = XInternAtom(dpy, "_NET_WM_NAME", False);
    A_UTF8 = XInternAtom(dpy, "UTF8_STRING", False);
    A_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_MAX_V = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    A_MAX_H = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    A_MOVERESIZE = XInternAtom(dpy, "_NET_WM_MOVERESIZE", False);
    A_CHANGE_STATE = XInternAtom(dpy, "WM_CHANGE_STATE", False);

    int scr = DefaultScreen(dpy);
    W = DisplayWidth(dpy, scr) * 11 / 20;
    H = DisplayHeight(dpy, scr) * 3 / 5;
    XSetWindowAttributes wa = { .background_pixel = 0, .backing_store = Always, .colormap = ui.cmap, .border_pixel = 0,
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | StructureNotifyMask |
                      SubstructureNotifyMask | FocusChangeMask | PropertyChangeMask };
    win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)W, (unsigned)H, 0, ui.depth, InputOutput, ui.vis,
                        CWBackPixel | CWBackingStore | CWColormap | CWBorderPixel | CWEventMask, &wa);
    XSetWindowAttributes ba = { .background_pixel = 0, .event_mask = SubstructureNotifyMask };
    body = XCreateWindow(dpy, win, 0, TB, (unsigned)W, (unsigned)(H - TB), 0, ui.depth, InputOutput, ui.vis, CWBackPixel | CWEventMask, &ba);
    gc = XCreateGC(dpy, win, 0, NULL);

    XClassHint ch = { "optic-term", "optic-term" };
    XSetClassHint(dpy, win, &ch);
    XSetWMProtocols(dpy, win, &A_DELETE, 1);
    /* No desktop frame: this window draws its own title bar (the tab strip). */
    long motif[5] = { 2, 0, 0, 0, 0 };
    Atom A_MOTIF = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    XChangeProperty(dpy, win, A_MOTIF, A_MOTIF, 32, PropModeReplace, (unsigned char *)motif, 5);
    long pid = getpid();
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
    XSizeHints sh = { .flags = PMinSize, .min_width = 220 * S, .min_height = 120 * S };
    XSetWMNormalHints(dpy, win, &sh);
    nameWindow();

    grabKey(XK_t, ControlMask | ShiftMask);
    grabKey(XK_w, ControlMask | ShiftMask);
    grabKey(XK_Page_Up, ControlMask);
    grabKey(XK_Page_Down, ControlMask);

    XMapWindow(dpy, body);
    XMapWindow(dpy, win);
    newTab();

    XEvent e;
    for (;;) {
        XNextEvent(dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.window == win && e.xexpose.count == 0) drawStrip();
            break;
        case ConfigureNotify:
            if (e.xconfigure.window == win && (e.xconfigure.width != W || e.xconfigure.height != H)) {
                W = e.xconfigure.width; H = e.xconfigure.height;
                fitTabs();
                drawStrip();
            } else if (e.xconfigure.event == body && findTab(e.xconfigure.window) >= 0 &&
                       (e.xconfigure.x || e.xconfigure.y || e.xconfigure.width != W || e.xconfigure.height != H - TB)) {
                /* a tab resized itself (a font change): it fills the window again */
                XMoveResizeWindow(dpy, e.xconfigure.window, 0, 0, (unsigned)W, (unsigned)(H - TB > 0 ? H - TB : 1));
            }
            break;
        case MapNotify:
            if (e.xmap.event == body && e.xmap.window != body) adopt(e.xmap.window);
            break;
        case ReparentNotify:
            if (e.xreparent.event == body && e.xreparent.parent == body) {
                XWindowAttributes a;
                if (XGetWindowAttributes(dpy, e.xreparent.window, &a) && a.map_state != IsUnmapped) adopt(e.xreparent.window);
            } else if (e.xreparent.event == body) dropTab(e.xreparent.window);
            break;
        case DestroyNotify:
            if (e.xdestroywindow.event == body) dropTab(e.xdestroywindow.window);
            break;
        case PropertyNotify:
            if (e.xproperty.window == win && e.xproperty.atom == A_STATE) { readMax(); drawStrip(); }
            else {
                int i = findTab(e.xproperty.window);
                if (i >= 0 && (e.xproperty.atom == XA_WM_NAME || e.xproperty.atom == A_NET_NAME)) {
                    readTitle(&tabs[i]);
                    if (i == cur) nameWindow();
                    drawStrip();
                }
            }
            break;
        case FocusIn:
            /* The desktop focuses this window: the keyboard goes to the open tab. */
            if (e.xfocus.window == win && e.xfocus.detail != NotifyInferior && e.xfocus.detail != NotifyPointer) focusTab();
            /* fall through */
        case FocusOut: {
            int a = ownsFocus();
            if (a != active) { active = a; drawStrip(); }
            break;
        }
        case KeyPress: {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if (k == XK_t || k == XK_T) newTab();
            else if (k == XK_w || k == XK_W) closeTab(cur);
            else if (k == XK_Page_Up && ntabs) show((cur + ntabs - 1) % ntabs);
            else if (k == XK_Page_Down && ntabs) show((cur + 1) % ntabs);
            break;
        }
        case ButtonPress: {
            if (e.xbutton.window != win || e.xbutton.y >= TB) break;
            int x = e.xbutton.x, y = e.xbutton.y;
            if (e.xbutton.button == 2) { closeTab(tabAt(x, y)); break; }   /* middle click closes a tab */
            if (e.xbutton.button != 1) break;
            pressBtn = buttonAt(x, y);
            pressTab = pressBtn == B_NONE ? tabAt(x, y) : -1;
            pressClose = onClose(pressTab, x, y) ? pressTab : -1;
            pressPlus = pressBtn == B_NONE && pressTab < 0 && onPlus(x, y);
            pressX = x; pressY = y; pressRX = e.xbutton.x_root; pressRY = e.xbutton.y_root;
            pressing = 1;
            if (pressTab >= 0 && pressClose < 0) show(pressTab);
            if (pressBtn == B_NONE && pressTab < 0 && !pressPlus) {
                /* the empty strip: double-tap maximises */
                double t = now();
                if (t - lastTitleTap < .35) { lastTitleTap = 0; pressing = 0; toggleMax(); break; }
                lastTitleTap = t;
            }
            drawStrip();
            break;
        }
        case MotionNotify: {
            if (!pressing) break;
            while (XCheckTypedWindowEvent(dpy, win, MotionNotify, &e)) {}
            /* Dragging the strip (or a tab) moves the window. */
            if (pressBtn == B_NONE && pressClose < 0 && !pressPlus &&
                (abs(e.xmotion.x - pressX) > 6 * S || abs(e.xmotion.y - pressY) > 6 * S)) {
                pressing = 0; pressTab = -1;
                startMove(pressRX, pressRY);
            }
            break;
        }
        case ButtonRelease: {
            if (!pressing || e.xbutton.button != 1) break;
            pressing = 0;
            int x = e.xbutton.x, y = e.xbutton.y;
            int b = pressBtn, c = pressClose, p = pressPlus;
            pressBtn = B_NONE; pressClose = -1; pressPlus = 0; pressTab = -1;
            if (b != B_NONE && buttonAt(x, y) == b) {
                if (b == B_MIN) minimise(); else if (b == B_MAX) toggleMax(); else closeAll();
            } else if (c >= 0 && onClose(c, x, y)) closeTab(c);
            else if (p && onPlus(x, y)) newTab();
            drawStrip();
            break;
        }
        case ClientMessage:
            if (e.xclient.message_type == A_PROTOCOLS && (Atom)e.xclient.data.l[0] == A_DELETE) closeAll();
            break;
        }
    }
}
