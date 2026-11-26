/* anywin runtime - service control manager and process tokens (advapi32).
 *
 * Services are systemd units: the unit list comes from `systemctl list-unit-files` and
 * `systemctl list-units` (cached for a few seconds). A Windows service name is the unit name
 * without ".service". Configuration is read-only: ChangeServiceConfigW reports access denied, so
 * no system state is modified. Without systemctl the list is empty and calls still succeed.
 * A service handle is an H_FILE object with no fd whose path slot holds the unit name. */
#include "aw.h"
#include "k32.h"
#include <ctype.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ERR_ACCESS_DENIED 5
#define ERR_INVALID_HANDLE 6
#define ERR_INSUFFICIENT_BUFFER 122
#define ERR_INVALID_LEVEL 124
#define ERR_INVALID_PARAMETER 87
#define ERR_MORE_DATA 234
#define ERR_SERVICE_DOES_NOT_EXIST 1060

#define SERVICE_WIN32_OWN_PROCESS 0x10
#define SERVICE_STOPPED 1
#define SERVICE_RUNNING 4
#define SERVICE_AUTO_START 2
#define SERVICE_DEMAND_START 3
#define SERVICE_DISABLED 4
#define SERVICE_STATE_ACTIVE 1
#define SERVICE_STATE_INACTIVE 2
#define SC_ENUM_PROCESS_INFO 0
#define SERVICE_CONFIG_DESCRIPTION 1
#define SERVICE_CONFIG_FAILURE_ACTIONS 2
#define SERVICE_CONFIG_DELAYED_AUTO_START_INFO 3
#define TOKEN_ELEVATION_TYPE_CLASS 18
#define TOKEN_ELEVATION_CLASS 20
#define TOKEN_INTEGRITY_LEVEL_CLASS 25
#define SECURITY_MANDATORY_HIGH_RID 0x3000
#define SECURITY_MANDATORY_MEDIUM_RID 0x2000
#define SE_GROUP_INTEGRITY 0x20

BOOL WINAPI __aw_kernel32_CloseHandle(HANDLE h);

struct svc {
    char *unit;   /* "name.service" */
    char *name;   /* Windows name: unit without the suffix */
    char *desc;
    char *cmd;    /* ExecStart as a command line, "" when unknown */
    int state;    /* SERVICE_RUNNING or SERVICE_STOPPED */
    int start;    /* SERVICE_AUTO_START, DEMAND or DISABLED */
};

static char *command_of_execstart(const char *v);

static struct svc *cache;
static size_t cache_n;
static time_t cache_at;

/* whole output of a fixed command line (no user data is ever placed in it) */
static char *capture(const char *cmd)
{
    FILE *p = popen(cmd, "r");
    if (!p)
        return strdup("");
    size_t cap = 4096, n = 0;
    char *out = malloc(cap);
    for (;;) {
        if (n + 1024 + 1 > cap)
            out = realloc(out, cap *= 2);
        size_t r = fread(out + n, 1, 1024, p);
        n += r;
        if (r < 1024)
            break;
    }
    out[n] = 0;
    pclose(p);
    return out;
}

static struct svc *svc_find(const char *unit_or_name)
{
    for (size_t i = 0; i < cache_n; i++)
        if (!strcasecmp(cache[i].name, unit_or_name) || !strcasecmp(cache[i].unit, unit_or_name))
            return &cache[i];
    return NULL;
}

static struct svc *svc_add(const char *unit)
{
    cache = realloc(cache, (cache_n + 1) * sizeof *cache);
    struct svc *s = &cache[cache_n++];
    memset(s, 0, sizeof *s);
    s->unit = strdup(unit);
    size_t l = strlen(unit);
    s->name = strndup(unit, l > 8 && !strcmp(unit + l - 8, ".service") ? l - 8 : l);
    s->desc = strdup("");
    s->cmd = strdup("");
    s->state = SERVICE_STOPPED;
    s->start = SERVICE_DEMAND_START;
    return s;
}

/* reloads the unit list unless the cached one is fresh */
static void svc_load(void)
{
    time_t now = time(NULL);
    if (cache && now - cache_at < 5)
        return;
    for (size_t i = 0; i < cache_n; i++) {
        free(cache[i].unit);
        free(cache[i].name);
        free(cache[i].desc);
        free(cache[i].cmd);
    }
    free(cache);
    cache = NULL;
    cache_n = 0;
    cache_at = now;

    char *files = capture("systemctl list-unit-files --type=service --no-legend --no-pager --plain 2>/dev/null");
    for (char *line = strtok(files, "\n"); line; line = strtok(NULL, "\n")) {
        char unit[256], state[64];
        if (sscanf(line, "%255s %63s", unit, state) != 2 || !strstr(unit, ".service"))
            continue;
        struct svc *s = svc_add(unit);
        if (!strcmp(state, "enabled") || !strcmp(state, "enabled-runtime"))
            s->start = SERVICE_AUTO_START;
        else if (!strcmp(state, "disabled") || !strcmp(state, "masked") || !strcmp(state, "masked-runtime"))
            s->start = SERVICE_DISABLED;
        else
            s->start = SERVICE_DEMAND_START;
    }
    free(files);

    char *units = capture("systemctl list-units --type=service --all --no-legend --no-pager --plain 2>/dev/null");
    for (char *line = strtok(units, "\n"); line; line = strtok(NULL, "\n")) {
        char unit[256], load[32], active[32], sub[32];
        char *p = line;
        while (*p && !isalnum((unsigned char)*p))
            p++; /* failed units carry a bullet in front */
        if (sscanf(p, "%255s %31s %31s %31s", unit, load, active, sub) != 4 || !strstr(unit, ".service"))
            continue;
        struct svc *s = svc_find(unit);
        if (!s)
            s = svc_add(unit);
        s->state = !strcmp(active, "active") && !strcmp(sub, "running") ? SERVICE_RUNNING : SERVICE_STOPPED;
        /* description: the text after the fourth column */
        const char *d = p;
        for (int k = 0; k < 4 && *d; k++) {
            while (*d == ' ')
                d++;
            while (*d && *d != ' ')
                d++;
        }
        while (*d == ' ')
            d++;
        free(s->desc);
        s->desc = strdup(d);
        size_t dl = strlen(s->desc);
        while (dl && (s->desc[dl - 1] == '\r' || s->desc[dl - 1] == ' '))
            s->desc[--dl] = 0;
    }
    free(units);

    /* ExecStart of every unit in one call: blocks of "Id=" then "ExecStart=" lines */
    char *show = capture("systemctl show --type=service --all --property=Id,ExecStart --no-pager 2>/dev/null");
    struct svc *cur = NULL;
    for (char *line = strtok(show, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "Id=", 3))
            cur = svc_find(line + 3);
        else if (!strncmp(line, "ExecStart=", 10) && cur) {
            free(cur->cmd);
            cur->cmd = command_of_execstart(line + 10);
        }
    }
    free(show);
}

static void put_wstr(uint8_t *base, size_t *off, const char *s)
{
    WCHAR *w = aw_u2w(s, -1);
    size_t n = (aw_wcslen(w) + 1) * sizeof(WCHAR);
    memcpy(base + *off, w, n);
    *off += n;
    free(w);
}

static size_t wstr_bytes(const char *s)
{
    WCHAR *w = aw_u2w(s, -1);
    size_t n = (aw_wcslen(w) + 1) * sizeof(WCHAR);
    free(w);
    return n;
}

/* ---- service control manager ---------------------------------------------- */
API(advapi32, HANDLE, OpenSCManagerW, (const WCHAR *machine, const WCHAR *db, DWORD access))
{
    (void)machine;
    (void)db;
    (void)access;
    aw_object *o = aw_object_new(H_FILE);
    HANDLE h = aw_handle_alloc(H_FILE, o);
    if (!h) {
        free(o);
        aw_set_last_error(8);
        return NULL;
    }
    aw_set_last_error(0);
    return h;
}

API(advapi32, BOOL, CloseServiceHandle, (HANDLE h)) { return __aw_kernel32_CloseHandle(h); }

API(advapi32, HANDLE, OpenServiceW, (HANDLE scm, const WCHAR *name, DWORD access))
{
    (void)access;
    if (!aw_handle_object(scm)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return NULL;
    }
    char *n = aw_w2u(name, -1);
    svc_load();
    struct svc *s = n ? svc_find(n) : NULL;
    free(n);
    if (!s) {
        aw_set_last_error(ERR_SERVICE_DOES_NOT_EXIST);
        return NULL;
    }
    aw_object *o = aw_object_new(H_FILE);
    *aw_obj_path(o) = strdup(s->unit); /* the unit name travels with the handle */
    HANDLE h = aw_handle_alloc(H_FILE, o);
    if (!h) {
        free(*aw_obj_path(o));
        free(o);
        aw_set_last_error(8);
        return NULL;
    }
    aw_set_last_error(0);
    return h;
}

/* the unit a service handle refers to, or NULL */
static struct svc *svc_of(HANDLE h)
{
    aw_object *o = aw_handle_object(h);
    if (!o || !*aw_obj_path(o)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return NULL;
    }
    svc_load();
    struct svc *s = svc_find(*aw_obj_path(o));
    if (!s)
        aw_set_last_error(ERR_SERVICE_DOES_NOT_EXIST);
    return s;
}

/* "{ path=/x ; argv[]=/x args ; ... }" -> "/x args" */
static char *command_of_execstart(const char *v)
{
    const char *path = strstr(v, "path=");
    const char *argv = strstr(v, "argv[]=");
    if (!path)
        return strdup("");
    path += 5;
    char *exe = strndup(path, strcspn(path, " ;\n"));
    char *cmd = NULL;
    if (argv) {
        argv += 7;
        char *args = strndup(argv, strcspn(argv, ";\n"));
        char *sp = args;
        while (*sp && *sp != ' ')
            sp++; /* argv[0] repeats the path: keep what follows it */
        while (*sp == ' ')
            sp++;
        if (asprintf(&cmd, "%s %s", exe, sp) < 0)
            cmd = NULL;
        free(args);
    }
    if (!cmd)
        cmd = strdup(exe);
    free(exe);
    return cmd;
}

API(advapi32, BOOL, EnumServicesStatusExW, (HANDLE scm, DWORD level, DWORD type, DWORD state, BYTE *buf, DWORD cb,
                                           DWORD *needed, DWORD *returned, DWORD *resume, const WCHAR *group))
{
    (void)group;
    (void)resume;
    if (!aw_handle_object(scm)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return FALSE;
    }
    if (level != SC_ENUM_PROCESS_INFO) {
        aw_set_last_error(ERR_INVALID_LEVEL);
        return FALSE;
    }
    svc_load();
    size_t n = 0, bytes = 0;
    for (size_t i = 0; i < cache_n; i++) {
        struct svc *s = &cache[i];
        if (!(type & SERVICE_WIN32_OWN_PROCESS))
            continue;
        if ((state == SERVICE_STATE_ACTIVE && s->state != SERVICE_RUNNING) ||
            (state == SERVICE_STATE_INACTIVE && s->state != SERVICE_STOPPED))
            continue;
        n++;
        bytes += 56 + wstr_bytes(s->name) + wstr_bytes(s->name);
    }
    if (needed)
        *needed = (DWORD)bytes;
    if (!buf || cb < bytes) {
        if (returned)
            *returned = 0;
        aw_set_last_error(ERR_MORE_DATA);
        return FALSE;
    }
    /* ENUM_SERVICE_STATUS_PROCESSW: name, display name, SERVICE_STATUS_PROCESS (36 bytes), 8-aligned */
    size_t strings = n * 56, k = 0;
    for (size_t i = 0; i < cache_n; i++) {
        struct svc *s = &cache[i];
        if (!(type & SERVICE_WIN32_OWN_PROCESS))
            continue;
        if ((state == SERVICE_STATE_ACTIVE && s->state != SERVICE_RUNNING) ||
            (state == SERVICE_STATE_INACTIVE && s->state != SERVICE_STOPPED))
            continue;
        uint8_t *rec = buf + k * 56;
        size_t off = strings;
        put_wstr(buf, &off, s->name);
        size_t off2 = off;
        put_wstr(buf, &off2, s->name);
        uint64_t p1 = (uint64_t)(uintptr_t)(buf + strings), p2 = (uint64_t)(uintptr_t)(buf + off);
        memcpy(rec, &p1, 8);
        memcpy(rec + 8, &p2, 8);
        uint32_t st[9] = {SERVICE_WIN32_OWN_PROCESS, (uint32_t)s->state, s->state == SERVICE_RUNNING ? 1u : 0u,
                          0, 0, 0, 0, 0, 0};
        memcpy(rec + 16, st, sizeof st);
        memset(rec + 52, 0, 4);
        strings = off2;
        k++;
    }
    if (returned)
        *returned = (DWORD)n;
    aw_set_last_error(0);
    return TRUE;
}

API(advapi32, BOOL, QueryServiceConfigW, (HANDLE h, BYTE *buf, DWORD cb, DWORD *needed))
{
    struct svc *s = svc_of(h);
    if (!s)
        return FALSE;
    char *cmd = strdup(s->cmd);
    /* QUERY_SERVICE_CONFIGW: type, start, error control, binary path, group, tag, deps, start name, display */
    size_t total = 64 + wstr_bytes(cmd) + 2 + 2 + wstr_bytes("LocalSystem") + wstr_bytes(s->name) + 2;
    if (needed)
        *needed = (DWORD)total;
    if (!buf || cb < total) {
        free(cmd);
        aw_set_last_error(ERR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buf, 0, 64);
    uint32_t *hdr = (uint32_t *)buf;
    hdr[0] = SERVICE_WIN32_OWN_PROCESS;
    hdr[1] = (uint32_t)s->start;
    hdr[2] = 1; /* SERVICE_ERROR_NORMAL */
    size_t off = 64;
    uint64_t p;
    p = (uint64_t)(uintptr_t)(buf + off); memcpy(buf + 16, &p, 8);
    put_wstr(buf, &off, cmd);
    p = (uint64_t)(uintptr_t)(buf + off); memcpy(buf + 24, &p, 8);
    put_wstr(buf, &off, "");                     /* load order group */
    p = (uint64_t)(uintptr_t)(buf + off); memcpy(buf + 40, &p, 8);
    put_wstr(buf, &off, "");                     /* dependencies: empty list */
    buf[off++] = 0;
    buf[off++] = 0;
    p = (uint64_t)(uintptr_t)(buf + off); memcpy(buf + 48, &p, 8);
    put_wstr(buf, &off, "LocalSystem");
    p = (uint64_t)(uintptr_t)(buf + off); memcpy(buf + 56, &p, 8);
    put_wstr(buf, &off, s->name);
    free(cmd);
    aw_set_last_error(0);
    return TRUE;
}

API(advapi32, BOOL, QueryServiceConfig2W, (HANDLE h, DWORD level, BYTE *buf, DWORD cb, DWORD *needed))
{
    struct svc *s = svc_of(h);
    if (!s)
        return FALSE;
    size_t total;
    if (level == SERVICE_CONFIG_DESCRIPTION)
        total = 8 + wstr_bytes(s->desc);
    else if (level == SERVICE_CONFIG_FAILURE_ACTIONS)
        total = 40;
    else if (level == SERVICE_CONFIG_DELAYED_AUTO_START_INFO)
        total = 4;
    else {
        aw_set_last_error(ERR_INVALID_LEVEL);
        return FALSE;
    }
    if (needed)
        *needed = (DWORD)total;
    if (!buf || cb < total) {
        aw_set_last_error(ERR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buf, 0, total);
    if (level == SERVICE_CONFIG_DESCRIPTION) {
        size_t off = 8;
        uint64_t p = (uint64_t)(uintptr_t)(buf + off);
        memcpy(buf, &p, 8);
        put_wstr(buf, &off, s->desc);
    }
    aw_set_last_error(0);
    return TRUE;
}

/* Configuration is not changed: the call is refused like an unprivileged caller would see. */
API(advapi32, BOOL, ChangeServiceConfigW, (HANDLE h, DWORD type, DWORD start, DWORD err, const WCHAR *bin,
                                          const WCHAR *group, DWORD *tag, const WCHAR *deps, const WCHAR *account,
                                          const WCHAR *password, const WCHAR *display))
{
    (void)h; (void)type; (void)start; (void)err; (void)bin; (void)group; (void)tag; (void)deps;
    (void)account; (void)password; (void)display;
    aw_set_last_error(ERR_ACCESS_DENIED);
    return FALSE;
}

/* ---- process tokens ----------------------------------------------------------- */
/* A token is an H_FILE object with no fd; its class answers come from the effective uid. */
API(advapi32, BOOL, OpenProcessToken, (HANDLE proc, DWORD access, HANDLE *token))
{
    (void)proc;
    (void)access;
    if (!token) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return FALSE;
    }
    aw_object *o = aw_object_new(H_FILE);
    HANDLE h = aw_handle_alloc(H_FILE, o);
    if (!h) {
        free(o);
        aw_set_last_error(8);
        return FALSE;
    }
    *token = h;
    aw_set_last_error(0);
    return TRUE;
}

API(advapi32, BOOL, GetTokenInformation, (HANDLE token, DWORD cls, void *buf, DWORD cb, DWORD *needed))
{
    if (!aw_handle_object(token)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return FALSE;
    }
    int root = geteuid() == 0;
    uint8_t tmp[64];
    DWORD size;
    if (cls == TOKEN_ELEVATION_TYPE_CLASS || cls == TOKEN_ELEVATION_CLASS) {
        uint32_t v = cls == TOKEN_ELEVATION_CLASS ? (root ? 1u : 0u) : (root ? 2u : 1u);
        memcpy(tmp, &v, 4);
        size = 4;
    } else if (cls == TOKEN_INTEGRITY_LEVEL_CLASS) {
        size = 28; /* TOKEN_MANDATORY_LABEL followed by its SID, sized up front */
    } else {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return FALSE;
    }
    if (needed)
        *needed = size;
    if (!buf || cb < size) {
        aw_set_last_error(ERR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (cls == TOKEN_INTEGRITY_LEVEL_CLASS) {
        /* the label's SID pointer must refer to the caller's buffer, so the record is built in place */
        uint8_t *out = buf;
        uint8_t *sid = out + 16;
        uint64_t sp = (uint64_t)(uintptr_t)sid;
        memset(out, 0, 28);
        memcpy(out, &sp, 8);
        uint32_t attr = SE_GROUP_INTEGRITY;
        memcpy(out + 8, &attr, 4);
        sid[0] = 1;  /* revision */
        sid[1] = 1;  /* one sub-authority */
        sid[7] = 16; /* mandatory label authority 0x10 */
        uint32_t rid = root ? SECURITY_MANDATORY_HIGH_RID : SECURITY_MANDATORY_MEDIUM_RID;
        memcpy(sid + 8, &rid, 4);
    } else {
        memcpy(buf, tmp, size);
    }
    aw_set_last_error(0);
    return TRUE;
}
