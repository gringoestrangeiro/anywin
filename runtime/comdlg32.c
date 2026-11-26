/* comdlg32: file open/save and color/font choosers. The runtime has no widget toolkit of its own,
 * so each dialog runs the desktop's chooser program (zenity, else kdialog) and reads the answer
 * from its standard output. When no chooser is installed the dialog reports cancel
 * (FALSE with CommDlgExtendedError 0), which is what a user who dismisses it would see. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERR_INVALID_PARAMETER 87
#define OFN_OVERWRITEPROMPT 0x00000002u
#define CC_RGBINIT 0x00000001u
#define FNERR_BUFFERTOOSMALL 0x00003003u

/* OPENFILENAME, x64: 152 bytes; A and W share the layout with char or WCHAR strings */
typedef struct {
    DWORD lStructSize;
    void *hwndOwner, *hInstance;
    const void *lpstrFilter;
    void *lpstrCustomFilter;
    DWORD nMaxCustFilter, nFilterIndex;
    void *lpstrFile;
    DWORD nMaxFile;
    void *lpstrFileTitle;
    DWORD nMaxFileTitle;
    const void *lpstrInitialDir, *lpstrTitle;
    DWORD Flags;
    WORD nFileOffset, nFileExtension;
    const void *lpstrDefExt;
    LPARAM lCustData;
    void *lpfnHook;
    const void *lpTemplateName;
    void *pvReserved;
    DWORD dwReserved, FlagsEx;
} OPENFILENAME_L;

/* CHOOSECOLOR, x64 */
typedef struct {
    DWORD lStructSize;
    void *hwndOwner, *hInstance;
    DWORD rgbResult;
    DWORD *lpCustColors;
    DWORD Flags;
    LPARAM lCustData;
    void *lpfnHook;
    const void *lpTemplateName;
} CHOOSECOLOR_L;

/* LOGFONT, x64 */
typedef struct {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    char lfFaceName[32];
} LOGFONT_L;

/* CHOOSEFONT, x64 */
typedef struct {
    DWORD lStructSize;
    void *hwndOwner, *hDC;
    LOGFONT_L *lpLogFont;
    INT iPointSize;
    DWORD Flags;
    DWORD rgbColors;
    LPARAM lCustData;
    void *lpfnHook;
    const void *lpTemplateName;
    void *hInstance;
    char *lpszStyle;
    WORD nFontType;
    INT nSizeMin, nSizeMax;
} CHOOSEFONT_L;

static DWORD cderr; /* CommDlgExtendedError */

API(comdlg32, DWORD, CommDlgExtendedError, (void)) { return cderr; }

/* ---- the chooser program ----------------------------------------------------------- */
/* Runs argv with stdout captured. Returns 0 when it exited 0, 1 when it exited otherwise
 * (cancel), -1 when the program could not be started. */
static int run_capture(char *const argv[], char *out, size_t cap)
{
    int ep[2], op[2];
    if (pipe2(ep, O_CLOEXEC) != 0)
        return -1;
    if (pipe2(op, O_CLOEXEC) != 0) {
        close(ep[0]);
        close(ep[1]);
        return -1;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(ep[0]);
        close(ep[1]);
        close(op[0]);
        close(op[1]);
        return -1;
    }
    if (pid == 0) {
        close(ep[0]);
        close(op[0]);
        dup2(op[1], 1); /* dup2 clears close-on-exec on the new descriptor */
        execvp(argv[0], argv);
        int e = errno;
        (void)!write(ep[1], &e, sizeof e);
        _exit(127);
    }
    close(ep[1]);
    close(op[1]);
    int err = 0;
    ssize_t r;
    do
        r = read(ep[0], &err, sizeof err);
    while (r < 0 && errno == EINTR);
    close(ep[0]);
    size_t len = 0;
    if (r == (ssize_t)sizeof err) {
        close(op[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }
    for (;;) {
        if (len + 1 >= cap)
            break;
        ssize_t k = read(op[0], out + len, cap - 1 - len);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            break;
        len += (size_t)k;
    }
    out[len] = 0;
    close(op[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

/* Runs the first chooser installed; returns the run_capture result of that chooser */
static int run_chooser(char **zenity_argv, char **kdialog_argv, char *out, size_t cap, int *used_kde)
{
    int r = run_capture(zenity_argv, out, cap);
    *used_kde = 0;
    if (r == -1 && kdialog_argv) {
        *used_kde = 1;
        r = run_capture(kdialog_argv, out, cap);
    }
    return r;
}

static void strip_eol(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

/* ---- file dialogs ------------------------------------------------------------------- */
/* Filters are name/pattern pairs in UTF-8, with a NULL name ending the list */
typedef struct {
    char **items; /* name, pattern, name, pattern, ..., NULL */
    int count;    /* pairs */
} filters_t;

static void filters_free(filters_t *f)
{
    for (int i = 0; i < f->count * 2; i++)
        free(f->items[i]);
    free(f->items);
}

/* ANSI or wide double-NUL list: strings until an empty one */
static void filters_parse(filters_t *f, const void *list, int wide)
{
    f->items = calloc(2 * 64 + 1, sizeof *f->items);
    f->count = 0;
    if (!list)
        return;
    int n = 0;
    if (wide) {
        const WCHAR *w = list;
        while (*w && n < 128) {
            f->items[n++] = aw_w2u(w, -1);
            w += aw_wcslen(w) + 1;
        }
    } else {
        const char *c = list;
        while (*c && n < 128) {
            f->items[n++] = strdup(c);
            c += strlen(c) + 1;
        }
    }
    f->count = n / 2;
}

/* Runs the file chooser. On success *result is a malloc'd Windows path. Returns 1 or 0. */
static int file_dialog(int save, int overwrite, const char *title, const char *dir_win, const char *file_win,
                       filters_t *f, DWORD index, char **result)
{
    char initial[4096] = "";
    if (file_win && *file_win) {
        char *fu = aw_unix_path(file_win);
        snprintf(initial, sizeof initial, "%s", fu ? fu : "");
        free(fu);
    } else if (dir_win && *dir_win) {
        char *du = aw_unix_path(dir_win);
        snprintf(initial, sizeof initial, "%s/", du ? du : "");
        free(du);
    }
    /* the selected filter is offered first */
    int sel = index >= 1 && (int)index <= f->count ? (int)index - 1 : 0;
    char *filt[32][2];
    int nf = 0;
    if (f->count) {
        filt[nf][0] = f->items[sel * 2];
        filt[nf][1] = f->items[sel * 2 + 1];
        nf++;
        for (int i = 0; i < f->count && nf < 32; i++)
            if (i != sel) {
                filt[nf][0] = f->items[i * 2];
                filt[nf][1] = f->items[i * 2 + 1];
                nf++;
            }
    }
    char *z[96], *k[16];
    char bufs[40][512];
    int zi = 0, ki = 0, bi = 0;
    z[zi++] = "zenity";
    z[zi++] = "--file-selection";
    z[zi++] = "--title";
    z[zi++] = (char *)(title && *title ? title : save ? "Save As" : "Open");
    if (save)
        z[zi++] = "--save";
    if (save && overwrite)
        z[zi++] = "--confirm-overwrite";
    if (initial[0]) {
        snprintf(bufs[bi], sizeof bufs[bi], "--filename=%s", initial);
        z[zi++] = bufs[bi++];
    }
    for (int i = 0; i < nf; i++) {
        snprintf(bufs[bi], sizeof bufs[bi], "--file-filter=%s | %s", filt[i][0], filt[i][1]);
        z[zi++] = bufs[bi++];
    }
    z[zi] = NULL;
    /* kdialog takes a start folder and a filter line of "patterns|name" */
    k[ki++] = "kdialog";
    k[ki++] = save ? "--getsavefilename" : "--getopenfilename";
    k[ki++] = initial[0] ? initial : (char *)".";
    if (nf) {
        snprintf(bufs[bi], sizeof bufs[bi], "%s|%s", filt[0][1], filt[0][0]);
        k[ki++] = bufs[bi++];
    }
    k[ki] = NULL;
    char out[8192];
    int used_kde = 0;
    int r = run_chooser(z, k, out, sizeof out, &used_kde);
    (void)used_kde;
    strip_eol(out);
    if (r != 0 || !out[0]) {
        cderr = 0;
        return 0;
    }
    *result = aw_win_path(out);
    if (!*result) {
        cderr = ERR_INVALID_PARAMETER;
        return 0;
    }
    return 1;
}

/* writes a Windows path into the caller's buffer (wide or ANSI) and fills the offsets */
static int store_file(OPENFILENAME_L *o, int wide, const char *win)
{
    DWORD cap = o->nMaxFile;
    size_t n = strlen(win);
    if (wide) {
        WCHAR *w = aw_u2w(win, -1);
        size_t wl = aw_wcslen(w);
        if (wl + 1 > cap) {
            free(w);
            cderr = FNERR_BUFFERTOOSMALL;
            return 0;
        }
        memcpy(o->lpstrFile, w, (wl + 1) * sizeof(WCHAR));
        free(w);
    } else {
        if (n + 1 > cap) {
            cderr = FNERR_BUFFERTOOSMALL;
            return 0;
        }
        memcpy(o->lpstrFile, win, n + 1);
    }
    const char *base = strrchr(win, '\\');
    const char *slash = strrchr(win, '/');
    if (!base || (slash && slash > base))
        base = slash;
    base = base ? base + 1 : win;
    o->nFileOffset = (WORD)(base - win);
    const char *dot = strrchr(base, '.');
    o->nFileExtension = (WORD)(dot ? (dot + 1) - win : 0);
    return 1;
}

static int common_file(OPENFILENAME_L *o, int wide, int save)
{
    if (!o) {
        cderr = ERR_INVALID_PARAMETER;
        return 0;
    }
    cderr = 0;
    char *title = NULL, *dir = NULL, *file = NULL;
    if (o->lpstrTitle)
        title = wide ? aw_w2u(o->lpstrTitle, -1) : strdup(o->lpstrTitle);
    if (o->lpstrInitialDir)
        dir = wide ? aw_w2u(o->lpstrInitialDir, -1) : strdup(o->lpstrInitialDir);
    if (o->lpstrFile && o->nMaxFile) {
        file = wide ? aw_w2u(o->lpstrFile, -1) : strdup(o->lpstrFile);
    }
    filters_t f;
    filters_parse(&f, o->lpstrFilter, wide);
    char *result = NULL;
    int ok = file_dialog(save, (o->Flags & OFN_OVERWRITEPROMPT) != 0, title, dir, file, &f, o->nFilterIndex,
                         &result);
    if (ok && result) {
        ok = store_file(o, wide, result);
    }
    free(result);
    filters_free(&f);
    free(title);
    free(dir);
    free(file);
    return ok;
}

API(comdlg32, BOOL, GetOpenFileNameA, (OPENFILENAME_L * o)) { return common_file(o, 0, 0); }
API(comdlg32, BOOL, GetOpenFileNameW, (OPENFILENAME_L * o)) { return common_file(o, 1, 0); }
API(comdlg32, BOOL, GetSaveFileNameA, (OPENFILENAME_L * o)) { return common_file(o, 0, 1); }
API(comdlg32, BOOL, GetSaveFileNameW, (OPENFILENAME_L * o)) { return common_file(o, 1, 1); }

/* ---- color chooser -------------------------------------------------------------------- */
API(comdlg32, BOOL, ChooseColorA, (CHOOSECOLOR_L * cc))
{
    if (!cc) {
        cderr = ERR_INVALID_PARAMETER;
        return FALSE;
    }
    cderr = 0;
    char start[32];
    DWORD c = cc->rgbResult;
    snprintf(start, sizeof start, "#%02x%02x%02x", c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
    char a1[] = "zenity", a2[] = "--color-selection", a3[64];
    snprintf(a3, sizeof a3, "--color=%s", (cc->Flags & CC_RGBINIT) ? start : "#ffffff");
    char *z[] = {a1, a2, a3, NULL};
    char *k[] = {"kdialog", "--getcolor", NULL};
    char out[128];
    int used_kde = 0;
    int r = run_chooser(z, k, out, sizeof out, &used_kde);
    strip_eol(out);
    unsigned rr = 0, gg = 0, bb = 0;
    int parsed = 0;
    if (r == 0 && out[0] == '#')
        parsed = sscanf(out, "#%2x%2x%2x", &rr, &gg, &bb) == 3;
    else if (r == 0)
        parsed = sscanf(out, "rgb(%u,%u,%u)", &rr, &gg, &bb) == 3;
    if (!parsed)
        return FALSE;
    cc->rgbResult = rr | (gg << 8) | (bb << 16);
    return TRUE;
}

/* ---- font chooser ---------------------------------------------------------------------- */
/* Reads "Family [Style words] Size" as printed by the chooser; the size is the last word */
API(comdlg32, BOOL, ChooseFontA, (CHOOSEFONT_L * cf))
{
    if (!cf || !cf->lpLogFont) {
        cderr = ERR_INVALID_PARAMETER;
        return FALSE;
    }
    cderr = 0;
    char init[128];
    int pt = cf->iPointSize > 0 ? cf->iPointSize / 10 : 10;
    snprintf(init, sizeof init, "%s %d", cf->lpLogFont->lfFaceName[0] ? cf->lpLogFont->lfFaceName : "Sans", pt);
    char a3[160];
    snprintf(a3, sizeof a3, "--font=%s", init);
    char *z[] = {"zenity", "--font-selection", a3, NULL};
    char out[256];
    int used_kde = 0;
    int r = run_chooser(z, NULL, out, sizeof out, &used_kde);
    strip_eol(out);
    if (r != 0 || !out[0])
        return FALSE;
    char *last = strrchr(out, ' ');
    if (!last)
        return FALSE;
    int size = atoi(last + 1);
    *last = 0;
    LOGFONT_L lf = *cf->lpLogFont;
    lf.lfWeight = 400;
    lf.lfItalic = 0;
    char *word = out;
    char face[32] = "";
    size_t fl = 0;
    for (char *tok = strtok(word, " "); tok; tok = strtok(NULL, " ")) {
        if (!strcasecmp(tok, "Bold")) {
            lf.lfWeight = 700;
            continue;
        }
        if (!strcasecmp(tok, "Italic")) {
            lf.lfItalic = 1;
            continue;
        }
        if (fl)
            face[fl++] = ' ';
        size_t tl = strlen(tok);
        if (fl + tl < sizeof face - 1) {
            memcpy(face + fl, tok, tl);
            fl += tl;
        }
    }
    face[fl] = 0;
    snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
    lf.lfHeight = -(LONG)((size > 0 ? size : pt) * 96 / 72);
    *cf->lpLogFont = lf;
    cf->iPointSize = (INT)((size > 0 ? size : pt) * 10);
    return TRUE;
}
