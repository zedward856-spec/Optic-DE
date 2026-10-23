/*
 * Optic Files — the desktop's file manager, in the MinUI look (as Optic Settings).
 * Top: round back and up buttons, the path in a pill (tap it to type a path), a Hidden toggle and
 * a round + for a new folder. Left: Places. Right: the folder, one pill-high row per item — a line
 * icon, the name, the date and the size in its own pill; the selected row is white.
 * Tap selects, double-tap opens (folders here; pictures, PDFs, video and pages in Firefox, text in
 * nano, .exe in Wine). Right-click (two-finger tap) for Open / Copy / Cut / Paste / Rename / Move to
 * Trash and more. Copies and moves run in the background; the list follows changes on its own.
 * Keys: arrows, Enter, Backspace (up), Delete (to Trash), F2 (rename), Ctrl+C / X / V,
 * Ctrl+H (hidden files), Ctrl+L (type a path), Ctrl+Shift+N (new folder).
 */
#define _GNU_SOURCE
#include "ui.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHARE "/opt/optic-de/share"
static int S = 1;
#define PAD (14 * S)
#define PILL (36 * S)
#define ROW (44 * S)
#define SIDE (190 * S)
#define BAR (PAD + PILL + PAD)

static Display *dpy;
static Window win, menu;
static UI ui;
static GC gc;
static int W = 900, H = 540;
static Atom A_DELETE;

/* ---- the folder ---------------------------------------------------------------------------- */

typedef struct { char name[256]; int dir; off_t size; time_t mtime; } Entry;
static Entry *ents;
static int nents, cap;
static char cwd[PATH_MAX], home[PATH_MAX], trashDir[PATH_MAX];
static char hist[32][PATH_MAX];
static int nhist;
static int sel = -1, pressed = -1, scroll, showHidden;
static time_t dirStamp;

typedef struct { char label[32]; char path[PATH_MAX]; } Place;
static Place places[12];
static int nplaces, pressedPlace = -1;

/* top bar buttons: 0 back, 1 up, 2 path, 3 hidden, 4 new folder */
static int pressedBtn = -1;

static char clip[PATH_MAX];
static int clipCut;

/* typing: 1 rename, 2 new folder, 3 go to a path */
static int inputMode;
static char input[PATH_MAX];
static int inputLen;

static char status[200];
static double statusUntil;
static pid_t job;
static char jobDone[96];

static double now(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec + t.tv_usec / 1e6; }

static void say(const char *s) { snprintf(status, sizeof status, "%s", s); statusUntil = now() + 2.6; }

static int inTrash(void) { return !strcmp(cwd, trashDir); }

static int cmpEnt(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir; /* folders first */
    return strcasecmp(x->name, y->name);
}

static time_t stampOf(const char *p) { struct stat st; return stat(p, &st) == 0 ? st.st_mtime ^ (st.st_ctime << 1) : 0; }

static void load(void) {
    nents = 0;
    DIR *d = opendir(cwd);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!showHidden && de->d_name[0] == '.') continue;
        if (nents == cap) { cap = cap ? cap * 2 : 256; ents = realloc(ents, (size_t)cap * sizeof *ents); }
        Entry *e = &ents[nents++];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", strcmp(cwd, "/") ? cwd : "", de->d_name);
        struct stat st;
        if (stat(p, &st) != 0 && lstat(p, &st) != 0) memset(&st, 0, sizeof st);
        e->dir = S_ISDIR(st.st_mode);
        e->size = st.st_size;
        e->mtime = st.st_mtime;
    }
    closedir(d);
    qsort(ents, (size_t)nents, sizeof *ents, cmpEnt);
    dirStamp = stampOf(cwd);
    if (sel >= nents) sel = nents - 1;
}

static void pathOf(int i, char *out, size_t n) { snprintf(out, n, "%s/%s", strcmp(cwd, "/") ? cwd : "", ents[i].name); }

static void go(const char *path, int remember) {
    char real[PATH_MAX];
    if (!realpath(path, real)) { say(strerror(errno)); return; }
    DIR *d = opendir(real);
    if (!d) { char m[200]; snprintf(m, sizeof m, "Can't open: %s", strerror(errno)); say(m); return; }
    closedir(d);
    if (remember && strcmp(real, cwd)) {
        if (nhist == 32) { memmove(hist[0], hist[1], sizeof hist[0] * 31); nhist--; }
        snprintf(hist[nhist++], PATH_MAX, "%s", cwd);
    }
    snprintf(cwd, sizeof cwd, "%s", real);
    sel = -1; scroll = 0;
    load();
}

static void goUp(void) {
    if (!strcmp(cwd, "/")) return;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", cwd);
    char *s = strrchr(p, '/');
    if (s == p) p[1] = 0; else *s = 0;
    char was[256];
    snprintf(was, sizeof was, "%s", strrchr(cwd, '/') + 1);
    go(p, 1);
    for (int i = 0; i < nents; i++) if (!strcmp(ents[i].name, was)) sel = i; /* where we came from */
}

static void goBack(void) {
    if (nhist) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s", hist[--nhist]); go(p, 0); }
    else goUp();
}

static void addPlace(const char *label, const char *path, int must) {
    struct stat st;
    if (nplaces >= 12 || (must == 0 && (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)))) return;
    snprintf(places[nplaces].label, sizeof places[0].label, "%s", label);
    snprintf(places[nplaces].path, sizeof places[0].path, "%s", path);
    nplaces++;
}

static void buildPlaces(void) {
    char p[PATH_MAX];
    nplaces = 0;
    addPlace("Home", home, 1);
    const char *sub[] = { "Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos" };
    for (int i = 0; i < 6; i++) { snprintf(p, sizeof p, "%s/%s", home, sub[i]); addPlace(sub[i], p, 0); }
    snprintf(p, sizeof p, "%s/.wine/drive_c", home); addPlace("Windows C:", p, 0);
    addPlace("Computer", "/", 1);
    addPlace("Trash", trashDir, 1);
}

/* ---- running things -------------------------------------------------------------------------- */

/* Starts a program on its own (not waited for), from this folder. */
static void launch(char *const argv[]) {
    pid_t p = fork();
    if (p == 0) {
        close(ConnectionNumber(dpy));
        setsid();
        if (fork() == 0) { if (chdir(cwd)) {} execvp(argv[0], argv); _exit(127); }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

/* A file operation in the background; the list reloads when it's done. */
static void runJob(const char *what, const char *done, char *const argv[]) {
    if (job) { say("Still busy with the last one"); return; }
    pid_t p = fork();
    if (p == 0) { close(ConnectionNumber(dpy)); execvp(argv[0], argv); _exit(127); }
    if (p < 0) { say("Couldn't start"); return; }
    job = p;
    snprintf(jobDone, sizeof jobDone, "%s", done);
    snprintf(status, sizeof status, "%s", what);
    statusUntil = 1e18;
}

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

/* "name", "name (copy)", "name (copy 2)"… : the first free one in [dir]. */
static void freeName(const char *dir, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dir, name);
    if (!exists(out)) return;
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int k = 1; k < 1000; k++) {
        char tag[24];
        if (k == 1) snprintf(tag, sizeof tag, " (copy)"); else snprintf(tag, sizeof tag, " (copy %d)", k);
        snprintf(out, n, "%s/%.*s%s%s", dir, stem, name, tag, name + stem);
        if (!exists(out)) return;
    }
}

static int hasExt(const char *n, const char *list) {
    const char *dot = strrchr(n, '.');
    if (!dot) return 0;
    char e[16];
    snprintf(e, sizeof e, "%s", dot + 1);
    for (char *c = e; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    char pat[32];
    snprintf(pat, sizeof pat, " %s ", e);
    return strstr(list, pat) != NULL;
}

static void openItem(int i) {
    if (i < 0 || i >= nents) return;
    char p[PATH_MAX];
    pathOf(i, p, sizeof p);
    if (ents[i].dir) { go(p, 1); return; }
    const char *n = ents[i].name;
    if (hasExt(n, " exe msi bat ") && exists("/opt/wine/bin/wine64")) {
        char *a[] = { "box64", "/opt/wine/bin/wine64", p, NULL };
        launch(a); say("Opening in Windows…"); return;
    }
    if (hasExt(n, " txt md c h cpp hpp py sh kt java js ts json xml yml yaml toml ini conf cfg log csv css rs go lua rb pl ") ||
        (!strchr(n, '.') && ents[i].size < 1 << 20)) {
        if (exists("/usr/bin/nano")) { char *a[] = { "xterm", "-e", "nano", p, NULL }; launch(a); return; }
    }
    char url[PATH_MAX + 8];
    snprintf(url, sizeof url, "file://%s", p);
    if (hasExt(n, " png jpg jpeg gif webp bmp svg ico avif pdf html htm mp4 webm ogg ogv mp3 wav flac m4a txt json xml md ")) {
        char *a[] = { "optic-firefox", url, NULL };
        launch(a); say("Opening in Firefox…"); return;
    }
    if (exists("/usr/bin/xdg-open")) { char *a[] = { "xdg-open", p, NULL }; launch(a); return; }
    char *a[] = { "optic-firefox", url, NULL };
    launch(a);
}

static void trashItem(int i) {
    char p[PATH_MAX], dst[PATH_MAX], info[PATH_MAX + 32], files[PATH_MAX], infos[PATH_MAX];
    pathOf(i, p, sizeof p);
    snprintf(files, sizeof files, "%s", trashDir);
    snprintf(infos, sizeof infos, "%s/../info", trashDir);
    mkdir(infos, 0700);
    freeName(files, ents[i].name, dst, sizeof dst);
    snprintf(info, sizeof info, "%s/%s.trashinfo", infos, strrchr(dst, '/') + 1);
    FILE *f = fopen(info, "w");
    if (f) {
        char when[32];
        time_t t = time(NULL);
        strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&t));
        fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", p, when);
        fclose(f);
    }
    if (rename(p, dst) == 0) { say("Moved to Trash"); load(); return; }
    if (errno == EXDEV) { char *a[] = { "mv", "--", p, dst, NULL }; runJob("Moving to Trash…", "Moved to Trash", a); return; }
    unlink(info);
    char m[200]; snprintf(m, sizeof m, "Couldn't: %s", strerror(errno)); say(m);
}

static void restoreItem(int i) {
    char p[PATH_MAX], info[PATH_MAX + 32], line[PATH_MAX + 16], to[PATH_MAX] = "";
    pathOf(i, p, sizeof p);
    snprintf(info, sizeof info, "%s/../info/%s.trashinfo", trashDir, ents[i].name);
    FILE *f = fopen(info, "r");
    if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "Path=", 5)) { snprintf(to, sizeof to, "%s", line + 5); to[strcspn(to, "\n")] = 0; } fclose(f); }
    if (!to[0]) snprintf(to, sizeof to, "%s/%s", home, ents[i].name);
    if (exists(to)) { char d[PATH_MAX]; snprintf(d, sizeof d, "%s", to); *strrchr(d, '/') = 0; freeName(d, strrchr(to, '/') + 1, to, sizeof to); }
    if (rename(p, to) == 0) { unlink(info); say("Restored"); load(); return; }
    char *a[] = { "mv", "--", p, to, NULL };
    unlink(info);
    runJob("Restoring…", "Restored", a);
}

static void deleteForever(int i) {
    char p[PATH_MAX], info[PATH_MAX + 32];
    pathOf(i, p, sizeof p);
    snprintf(info, sizeof info, "%s/../info/%s.trashinfo", trashDir, ents[i].name);
    unlink(info);
    char *a[] = { "rm", "-rf", "--", p, NULL };
    runJob("Deleting…", "Deleted", a);
}

static void emptyTrash(void) {
    char *a[] = { "sh", "-c", "rm -rf -- \"$1\"/* \"$1\"/.[!.]* \"$1\"/../info/*", "sh", trashDir, NULL };
    runJob("Emptying Trash…", "Trash is empty", a);
}

static void paste(void) {
    if (!clip[0]) return;
    if (!exists(clip)) { say("It's not there any more"); clip[0] = 0; return; }
    char dst[PATH_MAX];
    const char *name = strrchr(clip, '/') + 1;
    freeName(cwd, name, dst, sizeof dst);
    if (clipCut) {
        if (rename(clip, dst) == 0) { clip[0] = 0; say("Moved"); load(); return; }
        char *a[] = { "mv", "--", clip, dst, NULL };
        runJob("Moving…", "Moved", a);
        clip[0] = 0;
    } else {
        char *a[] = { "cp", "-a", "--", clip, dst, NULL };
        runJob("Copying…", "Copied", a);
    }
}

static void terminalHere(void) { char *a[] = { "optic-term", NULL }; launch(a); }

static void startInput(int mode) {
    inputMode = mode;
    if (mode == 1 && sel >= 0) snprintf(input, sizeof input, "%s", ents[sel].name);
    else if (mode == 3) snprintf(input, sizeof input, "%s", cwd);
    else input[0] = 0;
    inputLen = (int)strlen(input);
}

static void finishInput(void) {
    int mode = inputMode;
    inputMode = 0;
    if (!inputLen) return;
    if (mode == 3) { go(input, 1); return; }
    if (strchr(input, '/')) { say("A name can't have / in it"); return; }
    char to[PATH_MAX];
    snprintf(to, sizeof to, "%s/%s", strcmp(cwd, "/") ? cwd : "", input);
    if (exists(to)) { say("Something here already has that name"); return; }
    if (mode == 2) {
        if (mkdir(to, 0755) == 0) { load(); for (int i = 0; i < nents; i++) if (!strcmp(ents[i].name, input)) sel = i; }
        else { char m[200]; snprintf(m, sizeof m, "Couldn't: %s", strerror(errno)); say(m); }
    } else if (mode == 1 && sel >= 0) {
        char from[PATH_MAX];
        pathOf(sel, from, sizeof from);
        if (rename(from, to) == 0) { load(); for (int i = 0; i < nents; i++) if (!strcmp(ents[i].name, input)) sel = i; }
        else { char m[200]; snprintf(m, sizeof m, "Couldn't: %s", strerror(errno)); say(m); }
    }
}

/* ---- drawing --------------------------------------------------------------------------------- */

static int listX(void) { return SIDE + PAD; }
static int listW(void) { return W - listX() - PAD; }
static int listH(void) { return H - BAR; }
static int maxScroll(void) { int m = nents * ROW + PAD - listH(); return m > 0 ? m : 0; }
static float btnCX(int b) {
    switch (b) {
    case 0: return PAD + PILL / 2.f;
    case 1: return PAD + PILL + 8 * S + PILL / 2.f;
    case 4: return W - PAD - PILL / 2.f;
    }
    return 0;
}
static int hiddenW(void) { return ui_text_w(&ui, ui.small, "Hidden") + 28 * S; }
static int hiddenX(void) { return W - PAD - PILL - 8 * S - hiddenW(); }
static int pathX(void) { return PAD + 2 * (PILL + 8 * S); }
static int pathW(void) { return hiddenX() - 8 * S - pathX(); }

static void humanSize(off_t b, char *out, size_t n) {
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int k = 0;
    while (v >= 1024 && k < 4) { v /= 1024; k++; }
    if (k == 0) snprintf(out, n, "%ld B", (long)b); else snprintf(out, n, v < 10 ? "%.1f %s" : "%.0f %s", v, u[k]);
}

static void niceDate(time_t t, char *out, size_t n) {
    time_t nw = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a); localtime_r(&nw, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(out, n, "%H:%M", &a);
    else if (a.tm_year == b.tm_year) strftime(out, n, "%b %d", &a);
    else strftime(out, n, "%Y-%m-%d", &a);
}

/* Shortens [s] to fit [w] px with "…" (at the front when [front]). */
static void fit(XftFont *f, const char *s, int w, int front, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    if (ui_text_w(&ui, f, out) <= w) return;
    int len = (int)strlen(s);
    for (int k = 1; k < len; k++) {
        if (front) snprintf(out, n, "…%s", s + k);
        else snprintf(out, n, "%.*s…", len - k, s);
        if (ui_text_w(&ui, f, out) <= w) return;
    }
}

static void folderIcon(Canvas *c, float x, float y, float s, uint32_t ink) {
    float t = 1.8f * S;
    cv_rrect_outline(c, x, y + s * .22f, s * 1.15f, s * .78f, 3 * S, t, ink, 1);
    cv_line(c, x + 2 * S, y + s * .22f, x + s * .12f, y + s * .05f, t, ink, 1);
    cv_line(c, x + s * .12f, y + s * .05f, x + s * .45f, y + s * .05f, t, ink, 1);
    cv_line(c, x + s * .45f, y + s * .05f, x + s * .55f, y + s * .22f, t, ink, 1);
}

static void fileIcon(Canvas *c, float x, float y, float s, uint32_t ink) {
    float t = 1.8f * S, w = s * .82f, f = s * .28f;
    cv_line(c, x, y, x + w - f, y, t, ink, 1);
    cv_line(c, x + w - f, y, x + w, y + f, t, ink, 1);
    cv_line(c, x + w, y + f, x + w, y + s, t, ink, 1);
    cv_line(c, x + w, y + s, x, y + s, t, ink, 1);
    cv_line(c, x, y + s, x, y, t, ink, 1);
    cv_line(c, x + w - f, y, x + w - f, y + f, t, ink, 1);
    cv_line(c, x + w - f, y + f, x + w, y + f, t, ink, 1);
}

static void draw(void) {
    if (W <= 0 || H <= 0) return;
    Canvas *c = cv_new(W, H);
    cv_clear(c, C_BLACK);
    /* places */
    int cur = -1;
    for (int i = 0; i < nplaces; i++) if (!strcmp(places[i].path, cwd)) cur = i;
    for (int i = 0; i < nplaces; i++) {
        int y = BAR + 26 * S + i * ROW;
        if (i == cur || i == pressedPlace) cv_pill(c, PAD, y + 4 * S, SIDE - PAD, PILL, C_WHITE, 1);
    }
    /* rows */
    int top = BAR;
    float is = 20 * S;
    for (int i = 0; i < nents; i++) {
        int y = top + i * ROW - scroll;
        if (y + ROW < top || y > H) continue;
        int on = i == sel || i == pressed;
        if (on) cv_pill(c, listX(), y + 4 * S, listW(), PILL, C_WHITE, 1);
        uint32_t ink = on ? C_BLACK : C_WHITE;
        float ix = listX() + 16 * S, iy = y + 4 * S + (PILL - is) / 2;
        if (ents[i].dir) folderIcon(c, ix, iy, is, ink); else fileIcon(c, ix + 3 * S, iy, is, ink);
        if (!ents[i].dir) {
            char sz[32]; humanSize(ents[i].size, sz, sizeof sz);
            int vw = ui_text_w(&ui, ui.small, sz) + 28 * S;
            cv_pill(c, listX() + listW() - 6 * S - vw, y + 10 * S, vw, PILL - 12 * S, on ? 0xDDDDDD : C_DARK, 1);
        }
    }
    /* the top bar over a cleared band, so rows scrolled up under it don't show through */
    cv_rrect(c, 0, 0, W, BAR, 0, C_BLACK, 1);
    float t = 2.6f * S;
    /* back and up */
    for (int b = 0; b < 2; b++) {
        float cx = btnCX(b), cy = PAD + PILL / 2.f;
        int on = pressedBtn == b;
        cv_circle(c, cx, cy, PILL / 2.f, on ? C_WHITE : C_DARK, 1);
        uint32_t g = on ? C_BLACK : C_WHITE;
        if (b == 0) { cv_line(c, cx + 3 * S, cy - 7 * S, cx - 4 * S, cy, t, g, 1); cv_line(c, cx - 4 * S, cy, cx + 3 * S, cy + 7 * S, t, g, 1); }
        else { cv_line(c, cx - 7 * S, cy + 3 * S, cx, cy - 4 * S, t, g, 1); cv_line(c, cx, cy - 4 * S, cx + 7 * S, cy + 3 * S, t, g, 1); }
    }
    /* the path (white while typing) */
    cv_pill(c, pathX(), PAD, pathW(), PILL, inputMode || pressedBtn == 2 ? C_WHITE : C_DARK, 1);
    /* Hidden toggle and + */
    cv_pill(c, hiddenX(), PAD + 3 * S, hiddenW(), PILL - 6 * S, showHidden || pressedBtn == 3 ? C_WHITE : C_DARK, 1);
    {
        float cx = btnCX(4), cy = PAD + PILL / 2.f;
        int on = pressedBtn == 4;
        cv_circle(c, cx, cy, PILL / 2.f, on ? C_WHITE : C_DARK, 1);
        uint32_t g = on ? C_BLACK : C_WHITE;
        cv_line(c, cx - 7 * S, cy, cx + 7 * S, cy, t, g, 1);
        cv_line(c, cx, cy - 7 * S, cx, cy + 7 * S, t, g, 1);
    }
    /* status toast */
    int toast = status[0] && now() < statusUntil;
    int tw = toast ? ui_text_w(&ui, ui.small, status) + 36 * S : 0;
    if (toast) cv_pill(c, listX() + (listW() - tw) / 2, H - PAD - PILL, tw, PILL, C_DARK, 1);

    Pixmap pm = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H, (unsigned)ui.depth);
    cv_put(&ui, c, pm, gc, 0, 0);
    XftDraw *xd = XftDrawCreate(dpy, pm, ui.vis, ui.cmap);
    char buf[PATH_MAX + 8];
    /* path text */
    if (inputMode) {
        const char *label = inputMode == 1 ? "Rename" : inputMode == 2 ? "New folder" : "Go to";
        int lw = ui_text_w(&ui, ui.small, label);
        ui_text(&ui, xd, ui.small, pathX() + 18 * S, ui_baseline(ui.small, PAD, PILL), C_DIM, label);
        int room = pathW() - lw - 50 * S;
        char shown[PATH_MAX + 8];
        snprintf(buf, sizeof buf, "%s|", input);
        fit(ui.font, buf, room, 1, shown, sizeof shown);
        ui_text(&ui, xd, ui.font, pathX() + 30 * S + lw, ui_baseline(ui.font, PAD, PILL), C_BLACK, shown);
    } else {
        const char *p = cwd;
        char view[PATH_MAX + 8];
        size_t hl = strlen(home);
        if (!strncmp(cwd, home, hl) && (cwd[hl] == 0 || cwd[hl] == '/')) snprintf(view, sizeof view, "Home%s", cwd + hl);
        else if (inTrash()) snprintf(view, sizeof view, "Trash");
        else snprintf(view, sizeof view, "%s", p);
        char shown[PATH_MAX + 8];
        fit(ui.font, view, pathW() - 40 * S, 1, shown, sizeof shown);
        ui_text(&ui, xd, ui.font, pathX() + 20 * S, ui_baseline(ui.font, PAD, PILL), pressedBtn == 2 ? C_BLACK : C_WHITE, shown);
    }
    ui_text(&ui, xd, ui.small, hiddenX() + 14 * S, ui_baseline(ui.small, PAD + 3 * S, PILL - 6 * S), showHidden || pressedBtn == 3 ? C_BLACK : C_WHITE, "Hidden");
    /* places */
    ui_text(&ui, xd, ui.small, PAD + 16 * S, ui_baseline(ui.small, BAR, 22 * S), C_DIM, "PLACES");
    for (int i = 0; i < nplaces; i++) {
        int y = BAR + 26 * S + i * ROW;
        ui_text(&ui, xd, ui.font, PAD + 16 * S, ui_baseline(ui.font, y + 4 * S, PILL), i == cur || i == pressedPlace ? C_BLACK : C_WHITE, places[i].label);
    }
    /* rows */
    XRectangle clipR = { (short)listX(), (short)top, (unsigned short)listW(), (unsigned short)(H - top) };
    XftDrawSetClipRectangles(xd, 0, 0, &clipR, 1);
    for (int i = 0; i < nents; i++) {
        int y = top + i * ROW - scroll;
        if (y + ROW < top || y > H) continue;
        int on = i == sel || i == pressed;
        int right = listX() + listW() - 6 * S;
        if (!ents[i].dir) {
            char sz[32]; humanSize(ents[i].size, sz, sizeof sz);
            int vw = ui_text_w(&ui, ui.small, sz);
            ui_text(&ui, xd, ui.small, right - 14 * S - vw, ui_baseline(ui.small, y + 10 * S, PILL - 12 * S), on ? C_BLACK : C_WHITE, sz);
            right -= vw + 28 * S;
        }
        char dt[32]; niceDate(ents[i].mtime, dt, sizeof dt);
        int dw = ui_text_w(&ui, ui.small, dt);
        int showDate = listW() > 420 * S;
        if (showDate) { ui_text(&ui, xd, ui.small, right - 14 * S - dw, ui_baseline(ui.small, y + 4 * S, PILL), on ? 0x444444 : C_GRAY, dt); right -= dw + 14 * S; }
        int nx = listX() + 52 * S;
        char shown[300];
        fit(ui.font, ents[i].name, right - 16 * S - nx, 0, shown, sizeof shown);
        ui_text(&ui, xd, ui.font, nx, ui_baseline(ui.font, y + 4 * S, PILL), on ? C_BLACK : (ents[i].name[0] == '.' ? C_GRAY : C_WHITE), shown);
    }
    if (!nents) {
        const char *m = inTrash() ? "Trash is empty" : "This folder is empty";
        int mw = ui_text_w(&ui, ui.font, m);
        ui_text(&ui, xd, ui.font, listX() + (listW() - mw) / 2, top + listH() / 2, C_GRAY, m);
    }
    XftDrawSetClip(xd, None);
    if (toast) ui_text(&ui, xd, ui.small, listX() + (listW() - tw) / 2 + 18 * S, ui_baseline(ui.small, H - PAD - PILL, PILL), C_WHITE, status);
    XftDrawDestroy(xd);
    XCopyArea(dpy, pm, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
    XFreePixmap(dpy, pm);
    cv_free(c);
}

/* ---- the right-click menu -------------------------------------------------------------------- */

enum { M_OPEN, M_COPY, M_CUT, M_PASTE, M_RENAME, M_TRASH, M_RESTORE, M_DELETE, M_NEWFOLDER, M_HIDDEN, M_TERM, M_EMPTY, M_COPYPATH };
static int mItems[16], nm, mPressed = -1, mW, mH;
static const char *mLabel(int k) {
    switch (k) {
    case M_OPEN: return "Open"; case M_COPY: return "Copy"; case M_CUT: return "Cut"; case M_PASTE: return "Paste here";
    case M_RENAME: return "Rename"; case M_TRASH: return "Move to Trash"; case M_RESTORE: return "Restore";
    case M_DELETE: return "Delete forever"; case M_NEWFOLDER: return "New folder";
    case M_HIDDEN: return showHidden ? "Hide hidden files" : "Show hidden files";
    case M_TERM: return "Terminal here"; case M_EMPTY: return "Empty Trash"; case M_COPYPATH: return "Copy path";
    }
    return "";
}

static void drawMenu(void) {
    if (!menu) return;
    Canvas *c = cv_new(mW, mH);
    cv_clear(c, C_BLACK);
    cv_rrect(c, 0, 0, mW, mH, 20 * S, 0x1A1A1A, 1);
    for (int i = 0; i < nm; i++) if (i == mPressed) cv_pill(c, 8 * S, 8 * S + i * ROW + 4 * S, mW - 16 * S, PILL, C_WHITE, 1);
    Pixmap pm = XCreatePixmap(dpy, menu, (unsigned)mW, (unsigned)mH, (unsigned)ui.depth);
    cv_put(&ui, c, pm, gc, 0, 0);
    XftDraw *xd = XftDrawCreate(dpy, pm, ui.vis, ui.cmap);
    for (int i = 0; i < nm; i++)
        ui_text(&ui, xd, ui.font, 24 * S, ui_baseline(ui.font, 8 * S + i * ROW + 4 * S, PILL), i == mPressed ? C_BLACK : C_WHITE, mLabel(mItems[i]));
    XftDrawDestroy(xd);
    XCopyArea(dpy, pm, menu, gc, 0, 0, (unsigned)mW, (unsigned)mH, 0, 0);
    XFreePixmap(dpy, pm);
    cv_free(c);
}

/* The menu's window, cut to its rounded shape (no square corners around it). */
static void roundMenu(int w, int h, float r) {
    Pixmap m = XCreatePixmap(dpy, menu, (unsigned)w, (unsigned)h, 1);
    GC g = XCreateGC(dpy, m, 0, NULL);
    XSetForeground(dpy, g, 1);
    XFillRectangle(dpy, m, g, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, g, 0);
    for (int y = 0; y < (int)r; y++) {
        float dy = r - (y + .5f);
        int cut = (int)floorf(r - sqrtf(fmaxf(0, r * r - dy * dy)));
        if (cut <= 0) continue;
        XFillRectangle(dpy, m, g, 0, y, (unsigned)cut, 1); XFillRectangle(dpy, m, g, w - cut, y, (unsigned)cut, 1);
        XFillRectangle(dpy, m, g, 0, h - 1 - y, (unsigned)cut, 1); XFillRectangle(dpy, m, g, w - cut, h - 1 - y, (unsigned)cut, 1);
    }
    XShapeCombineMask(dpy, menu, ShapeBounding, 0, 0, m, ShapeSet);
    XFreeGC(dpy, g);
    XFreePixmap(dpy, m);
}

static void closeMenu(void) {
    if (!menu) return;
    XUngrabPointer(dpy, CurrentTime);
    XDestroyWindow(dpy, menu);
    menu = 0; mPressed = -1;
}

static void openMenu(int rx, int ry) {
    nm = 0;
    if (sel >= 0) {
        mItems[nm++] = M_OPEN;
        if (inTrash()) { mItems[nm++] = M_RESTORE; mItems[nm++] = M_DELETE; }
        else {
            mItems[nm++] = M_COPY; mItems[nm++] = M_CUT; mItems[nm++] = M_RENAME;
            mItems[nm++] = M_COPYPATH; mItems[nm++] = M_TRASH;
        }
    } else {
        if (clip[0] && !inTrash()) mItems[nm++] = M_PASTE;
        if (!inTrash()) mItems[nm++] = M_NEWFOLDER;
        mItems[nm++] = M_HIDDEN;
        mItems[nm++] = M_TERM;
        if (inTrash() && nents) mItems[nm++] = M_EMPTY;
    }
    mW = 0;
    for (int i = 0; i < nm; i++) { int w = ui_text_w(&ui, ui.font, mLabel(mItems[i])); if (w > mW) mW = w; }
    mW += 64 * S;
    mH = nm * ROW + 16 * S;
    int sw = DisplayWidth(dpy, DefaultScreen(dpy)), sh = DisplayHeight(dpy, DefaultScreen(dpy));
    if (rx + mW > sw) rx = sw - mW;
    if (ry + mH > sh) ry = sh - mH;
    XSetWindowAttributes a = { .override_redirect = True, .save_under = True,
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask };
    menu = XCreateWindow(dpy, DefaultRootWindow(dpy), rx, ry, (unsigned)mW, (unsigned)mH, 0, ui.depth, InputOutput, ui.vis,
                         CWOverrideRedirect | CWSaveUnder | CWEventMask, &a);
    roundMenu(mW, mH, 20 * S);
    XMapRaised(dpy, menu);
    XGrabPointer(dpy, menu, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    drawMenu();
}

static void copyPathToClipboard(const char *p) {
    /* through xclip / xsel if present (no clipboard owner of our own to keep alive) */
    char *a[] = { "sh", "-c", "printf %s \"$1\" | (xclip -selection clipboard 2>/dev/null || xsel -b 2>/dev/null)", "sh", (char *)p, NULL };
    launch(a);
    say("Path copied");
}

static void menuAct(int k) {
    char p[PATH_MAX] = "";
    if (sel >= 0 && sel < nents) pathOf(sel, p, sizeof p);
    switch (k) {
    case M_OPEN: openItem(sel); break;
    case M_COPY: snprintf(clip, sizeof clip, "%s", p); clipCut = 0; say("Copied — Paste in a folder"); break;
    case M_CUT: snprintf(clip, sizeof clip, "%s", p); clipCut = 1; say("Cut — Paste in a folder"); break;
    case M_PASTE: paste(); break;
    case M_RENAME: startInput(1); break;
    case M_TRASH: trashItem(sel); break;
    case M_RESTORE: restoreItem(sel); break;
    case M_DELETE: deleteForever(sel); break;
    case M_NEWFOLDER: sel = -1; startInput(2); break;
    case M_HIDDEN: showHidden = !showHidden; load(); break;
    case M_TERM: terminalHere(); break;
    case M_EMPTY: emptyTrash(); break;
    case M_COPYPATH: copyPathToClipboard(p); break;
    }
}

/* ---- input ----------------------------------------------------------------------------------- */

static void keepVisible(void) {
    if (sel < 0) return;
    int y = sel * ROW;
    if (y < scroll) scroll = y;
    if (y + ROW > scroll + listH() - PAD) scroll = y + ROW - listH() + PAD;
    if (scroll < 0) scroll = 0;
}

static void key(XKeyEvent *e) {
    char txt[32];
    KeySym k;
    int n = XLookupString(e, txt, sizeof txt - 1, &k, NULL);
    txt[n > 0 ? n : 0] = 0;
    int ctrl = e->state & ControlMask, shift = e->state & ShiftMask;
    if (inputMode) {
        if (k == XK_Return || k == XK_KP_Enter) finishInput();
        else if (k == XK_Escape) inputMode = 0;
        else if (k == XK_BackSpace) { if (inputLen) { do inputLen--; while (inputLen && (input[inputLen] & 0xC0) == 0x80); input[inputLen] = 0; } }
        else if (!ctrl && n > 0 && (unsigned char)txt[0] >= 32 && inputLen + n < (int)sizeof input - 1) { memcpy(input + inputLen, txt, (size_t)n); inputLen += n; input[inputLen] = 0; }
        draw();
        return;
    }
    if (ctrl && (k == XK_c || k == XK_C) && sel >= 0) menuAct(M_COPY);
    else if (ctrl && (k == XK_x || k == XK_X) && sel >= 0) menuAct(M_CUT);
    else if (ctrl && (k == XK_v || k == XK_V)) paste();
    else if (ctrl && (k == XK_h || k == XK_H)) { showHidden = !showHidden; load(); }
    else if (ctrl && (k == XK_l || k == XK_L)) startInput(3);
    else if (ctrl && shift && (k == XK_n || k == XK_N)) { sel = -1; startInput(2); }
    else if (k == XK_Down) { if (sel < nents - 1) sel++; keepVisible(); }
    else if (k == XK_Up) { if (sel > 0) sel--; else if (sel < 0 && nents) sel = 0; keepVisible(); }
    else if (k == XK_Home) { sel = nents ? 0 : -1; keepVisible(); }
    else if (k == XK_End) { sel = nents - 1; keepVisible(); }
    else if (k == XK_Return || k == XK_KP_Enter) openItem(sel);
    else if (k == XK_BackSpace || ((e->state & Mod1Mask) && k == XK_Up)) goUp();
    else if ((e->state & Mod1Mask) && k == XK_Left) goBack();
    else if (k == XK_Delete && sel >= 0) { if (inTrash()) deleteForever(sel); else trashItem(sel); }
    else if (k == XK_F2 && sel >= 0 && !inTrash()) startInput(1);
    else if (k == XK_F5) load();
    else if (k == XK_Escape) sel = -1;
    else return;
    draw();
}

static int rowAt(int x, int y) {
    if (x < listX() || x > listX() + listW() || y < BAR) return -1;
    int i = (y - BAR + scroll) / ROW;
    return i >= 0 && i < nents ? i : -1;
}
static int placeAt(int x, int y) {
    if (x > SIDE || y < BAR + 26 * S) return -1;
    int i = (y - BAR - 26 * S) / ROW;
    return i >= 0 && i < nplaces ? i : -1;
}
static int barAt(int x, int y) {
    if (y < PAD - 4 * S || y > PAD + PILL + 4 * S) return -1;
    for (int b = 0; b < 2; b++) if (fabsf(x - btnCX(b)) <= PILL / 2.f + 3 * S) return b;
    if (fabsf(x - btnCX(4)) <= PILL / 2.f + 3 * S) return 4;
    if (x >= pathX() && x < pathX() + pathW()) return 2;
    if (x >= hiddenX() && x < hiddenX() + hiddenW()) return 3;
    return -1;
}

int main(int argc, char **argv) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    signal(SIGPIPE, SIG_IGN);
    ui_open(&ui, dpy, SHARE);
    S = ui.scale;
    W *= S; H *= S;
    const char *h = getenv("HOME");
    snprintf(home, sizeof home, "%s", h && *h ? h : "/root");
    snprintf(trashDir, sizeof trashDir, "%s/.local/share/Trash/files", home);
    { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/.local", home); mkdir(p, 0755); strcat(p, "/share"); mkdir(p, 0755);
      strcat(p, "/Trash"); mkdir(p, 0700); mkdir(trashDir, 0700); strcat(p, "/info"); mkdir(p, 0700); }
    snprintf(cwd, sizeof cwd, "%s", home);
    buildPlaces();
    go(argc > 1 ? argv[1] : home, 0);
    A_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)W, (unsigned)H, 0, 0, 0);
    XStoreName(dpy, win, "Files");
    XClassHint ch = { "optic-files", "optic-files" };
    XSetClassHint(dpy, win, &ch);
    XSetWMProtocols(dpy, win, &A_DELETE, 1);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask | FocusChangeMask);
    gc = XCreateGC(dpy, win, 0, NULL);
    XMapWindow(dpy, win);

    int dragY = 0, dragging = 0, lastRow = -1;
    double lastTap = 0, nextCheck = 0;
    int fd = ConnectionNumber(dpy);
    for (;;) {
        if (!XPending(dpy)) {
            fd_set fs; FD_ZERO(&fs); FD_SET(fd, &fs);
            struct timeval tv = { 0, 250000 };
            select(fd + 1, &fs, NULL, NULL, &tv);
        }
        double t = now();
        if (t >= nextCheck) {
            nextCheck = t + 0.5;
            int redraw = 0;
            if (job) {
                int st;
                if (waitpid(job, &st, WNOHANG) == job) {
                    job = 0;
                    say(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? jobDone : "That didn't work");
                    load(); redraw = 1;
                }
            }
            if (stampOf(cwd) != dirStamp) { load(); redraw = 1; }  /* the folder changed */
            if (status[0] && t >= statusUntil) { status[0] = 0; redraw = 1; }
            if (redraw) draw();
        }
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (menu && (e.xany.window == menu || e.type == ButtonPress || e.type == ButtonRelease || e.type == MotionNotify)) {
                if (e.type == Expose) { drawMenu(); continue; }
                int x = 0, y = 0;
                if (e.type == ButtonPress || e.type == ButtonRelease) { x = e.xbutton.x_root; y = e.xbutton.y_root; }
                else if (e.type == MotionNotify) { x = e.xmotion.x_root; y = e.xmotion.y_root; }
                else continue;
                Window r; int mx, my; unsigned w2, h2, bw, dp;
                XGetGeometry(dpy, menu, &r, &mx, &my, &w2, &h2, &bw, &dp);
                int lx = x - mx, ly = y - my;
                int i = lx >= 0 && lx < mW && ly >= 8 * S ? (ly - 8 * S) / ROW : -1;
                if (i >= nm) i = -1;
                if (e.type == ButtonPress) { if (i < 0) { closeMenu(); draw(); } else { mPressed = i; drawMenu(); } }
                else if (e.type == MotionNotify) { if (mPressed >= 0 && i != mPressed) { mPressed = i; drawMenu(); } }
                else if (e.type == ButtonRelease && mPressed >= 0) { int k = i >= 0 ? mItems[i] : -1; closeMenu(); if (k >= 0) menuAct(k); draw(); }
                continue;
            }
            switch (e.type) {
            case Expose: if (e.xexpose.window == win && !e.xexpose.count) draw(); break;
            case ConfigureNotify:
                if (e.xconfigure.window == win && (e.xconfigure.width != W || e.xconfigure.height != H)) {
                    W = e.xconfigure.width; H = e.xconfigure.height;
                    if (scroll > maxScroll()) scroll = maxScroll();
                    draw();
                }
                break;
            case ClientMessage: if ((Atom)e.xclient.data.l[0] == A_DELETE) return 0; break;
            case KeyPress: key(&e.xkey); break;
            case ButtonPress: {
                int x = e.xbutton.x, y = e.xbutton.y;
                if (e.xbutton.button == 4 || e.xbutton.button == 5) {
                    scroll += e.xbutton.button == 5 ? ROW : -ROW;
                    if (scroll > maxScroll()) scroll = maxScroll();
                    if (scroll < 0) scroll = 0;
                    draw(); break;
                }
                if (e.xbutton.button == 3) {
                    int i = rowAt(x, y);
                    if (rowAt(x, y) >= 0 || y >= BAR) { sel = i; draw(); openMenu(e.xbutton.x_root, e.xbutton.y_root); }
                    break;
                }
                if (e.xbutton.button != 1) break;
                if (inputMode && barAt(x, y) != 2) inputMode = 0;
                dragY = y; dragging = 0;
                pressedBtn = barAt(x, y);
                pressedPlace = pressedBtn < 0 ? placeAt(x, y) : -1;
                pressed = pressedBtn < 0 && pressedPlace < 0 ? rowAt(x, y) : -1;
                draw();
                break;
            }
            case MotionNotify:
                if ((e.xmotion.state & Button1Mask) && pressedBtn < 0 && pressedPlace < 0 && (dragging || abs(e.xmotion.y - dragY) > 8 * S)) {
                    /* drag the list to scroll it */
                    dragging = 1; pressed = -1;
                    scroll -= e.xmotion.y - dragY; dragY = e.xmotion.y;
                    if (scroll > maxScroll()) scroll = maxScroll();
                    if (scroll < 0) scroll = 0;
                    draw();
                }
                break;
            case ButtonRelease: {
                if (e.xbutton.button != 1) break;
                int x = e.xbutton.x, y = e.xbutton.y;
                int b = pressedBtn, pl = pressedPlace, r = pressed;
                pressedBtn = -1; pressedPlace = -1; pressed = -1;
                if (b >= 0 && barAt(x, y) == b) {
                    if (b == 0) goBack(); else if (b == 1) goUp();
                    else if (b == 2) { if (!inputMode) startInput(3); }
                    else if (b == 3) { showHidden = !showHidden; load(); }
                    else if (b == 4) { sel = -1; startInput(2); }
                } else if (pl >= 0 && placeAt(x, y) == pl) go(places[pl].path, 1);
                else if (!dragging && r >= 0 && rowAt(x, y) == r) {
                    double tt = now();
                    if (r == lastRow && tt - lastTap < 0.45) { lastTap = 0; openItem(r); }
                    else { sel = r; lastRow = r; lastTap = tt; }
                } else if (!dragging && b < 0 && pl < 0 && rowAt(x, y) < 0 && y >= BAR && x >= listX()) sel = -1;
                dragging = 0;
                draw();
                break;
            }
            }
        }
    }
}
