/* user32 core - message queues, send/post, GetMessage loop, dispatch, timers, DefWindowProc (S5).
 *
 * Each thread has a queue of posted/input messages (FIFO).  GetMessage serves, in order:
 *   1. queued messages, 2. X events (polled without blocking), 3. WM_PAINT (parents first),
 *   4. WM_TIMER when due; before blocking it flushes dirty back buffers to X and waits for X events,
 *   a timer, or a wakeup from another thread.  String parameters are converted to the charset of the
 *   target window when a message is posted or sent (A <-> W), so queued messages are always in the
 *   target's charset and DispatchMessage just calls the procedure. */
#include "user32_int.h"
#include <time.h>

#define ERR_INVALID_WINDOW 1400
#define ERR_INVALID_PARAMETER 87
#define AW_USER_TIMER_MIN 10
#define NC_HTCLIENT_CODE 1

static aw_queue *g_queues;
static __thread aw_queue *tls_queue;
static pthread_cond_t g_wake_cond = PTHREAD_COND_INITIALIZER;
static int g_poller_busy; /* a thread is inside plat_poll; guarded by aw_wnd_lock */

static aw_win *from(HWND h) { return aw_win_from_hwnd(h); }

void aw_wake(void)
{
    if (aw_display)
        plat_wakeup();
    pthread_cond_broadcast(&g_wake_cond);
    pthread_cond_broadcast(&aw_send_cond);
}

static aw_queue *queue_find_locked(DWORD tid)
{
    for (aw_queue *q = g_queues; q; q = q->next)
        if (q->tid == tid)
            return q;
    return NULL;
}

static aw_queue *queue_new(DWORD tid)
{
    aw_queue *q = calloc(1, sizeof *q);
    if (!q)
        return NULL;
    q->tid = tid;
    q->next_timer_id = 1;
    pthread_mutex_lock(&aw_wnd_lock);
    q->next = g_queues;
    g_queues = q;
    pthread_mutex_unlock(&aw_wnd_lock);
    return q;
}

aw_queue *aw_queue_for_tid(DWORD tid)
{
    pthread_mutex_lock(&aw_wnd_lock);
    aw_queue *q = queue_find_locked(tid);
    pthread_mutex_unlock(&aw_wnd_lock);
    if (q)
        return q;
    if (tid == aw_current_tid())
        return aw_queue_current();
    return NULL;
}

aw_queue *aw_queue_current(void)
{
    if (!tls_queue) {
        DWORD tid = aw_current_tid();
        pthread_mutex_lock(&aw_wnd_lock);
        tls_queue = queue_find_locked(tid); /* a post may have created it already */
        pthread_mutex_unlock(&aw_wnd_lock);
        if (!tls_queue)
            tls_queue = queue_new(tid);
    }
    return tls_queue;
}

/* queue for a thread that may not have pumped yet (PostThreadMessage / PostMessage from elsewhere) */
static aw_queue *queue_for_post(DWORD tid)
{
    aw_queue *q = aw_queue_for_tid(tid);
    if (!q) {
        pthread_mutex_lock(&aw_wnd_lock);
        q = queue_find_locked(tid);
        pthread_mutex_unlock(&aw_wnd_lock);
        if (!q)
            q = queue_new(tid);
    }
    return q;
}

/* ------------------------------------------------------------------ charset conversion of params */
/* 1: lParam is a string in the caller's charset; 2: lParam is an output buffer of wParam chars. */
static int string_kind(UINT msg)
{
    switch (msg) {
    case WM_SETTEXT: case WM_SETTINGCHANGE: case EM_REPLACESEL:
    case 0x0143: /* CB_ADDSTRING */ case 0x014A: /* CB_INSERTSTRING */ case 0x014C: /* CB_FINDSTRING */
    case 0x014D: /* CB_SELECTSTRING */ case 0x0145: /* CB_DIR */ case 0x0158: /* CB_FINDSTRINGEXACT */
    case 0x0180: /* LB_ADDSTRING */ case 0x0181: /* LB_INSERTSTRING */ case 0x018C: /* LB_SELECTSTRING */
    case 0x018D: /* LB_DIR */ case 0x018F: /* LB_FINDSTRING */ case 0x01A2: /* LB_FINDSTRINGEXACT */
        return 1;
    case WM_GETTEXT:
        return 2;
    case 0x0148: /* CB_GETLBTEXT */ case 0x0189: /* LB_GETTEXT: the index is wParam, the buffer size comes from the LEN message */
        return 3;
    default:
        return 0;
    }
}

/* Calls fn(h, msg, w, l) with the params converted from src_uni charset to dst_uni (1 = UTF-16).
 * Used for window procedures and for user dialog procedures (the same message shapes). */
LRESULT aw_call_conv(WNDPROC fn, HWND h, UINT msg, WPARAM wp, LPARAM lp, int src_uni, int dst_uni)
{
    if (!fn)
        return 0;
    int kind = string_kind(msg);
    if (!kind || src_uni == dst_uni)
        return fn(h, msg, wp, lp);
    if (kind == 3 && lp && !src_uni && dst_uni) {
        /* A caller of LB_GETTEXT / CB_GETLBTEXT: the W text is read into a buffer of the length the
         * LEN message reports, then written as bytes (the caller sized its buffer as len + 1) */
        UINT lenmsg = msg == 0x0189 ? 0x018A /* LB_GETTEXTLEN */ : 0x0149 /* CB_GETLBTEXTLEN */;
        LRESULT units = fn(h, lenmsg, wp, 0);
        if (units < 0)
            return units;
        WCHAR *tmp = calloc((size_t)units + 1, sizeof(WCHAR));
        if (!tmp)
            return -1;
        LRESULT r = fn(h, msg, wp, (LPARAM)tmp);
        if (r >= 0)
            r = aw_w2u_term(tmp, (int)units, (char *)lp, (int)units + 1);
        free(tmp);
        return r;
    }
    if (kind == 1 && lp) {
        if (src_uni) { /* W caller -> A proc */
            char *u = aw_w2u((const WCHAR *)lp, -1);
            LRESULT r = fn(h, msg, wp, (LPARAM)(u ? u : ""));
            free(u);
            return r;
        }
        WCHAR *x = aw_u2w((const char *)lp, -1);
        LRESULT r = fn(h, msg, wp, (LPARAM)(x ? x : aw_wempty));
        free(x);
        return r;
    }
    if (kind == 2 && lp && wp > 0) {
        size_t n = (size_t)wp;
        if (src_uni) { /* W caller wants WCHARs, A proc fills bytes */
            char *tmp = calloc(n + 1, 1);
            if (!tmp) return 0;
            LRESULT r = fn(h, msg, (WPARAM)n, (LPARAM)tmp);
            int out = aw_u2w_buf(tmp, -1, (WCHAR *)lp, (int)n);
            free(tmp);
            return out > 0 ? out : r;
        }
        /* A caller wants bytes, W proc fills WCHARs */
        WCHAR *tmp = calloc(n + 1, sizeof(WCHAR));
        if (!tmp) return 0;
        LRESULT r = fn(h, msg, (WPARAM)n, (LPARAM)tmp);
        int out = aw_w2u_term(tmp, (int)r, (char *)lp, (int)n);
        free(tmp);
        return out;
    }
    return fn(h, msg, wp, lp);
}

/* Runs proc(h, msg, w, l) where the params are in src_unicode charset and the proc wants its own. */
LRESULT aw_call_proc(aw_win *w, UINT msg, WPARAM wp, LPARAM lp, int src_unicode)
{
    if (!w || !w->proc)
        return 0;
    if (src_unicode == w->unicode)
        return w->proc(w->hwnd, msg, wp, lp);
    return aw_call_conv(w->proc, w->hwnd, msg, wp, lp, src_unicode, w->unicode);
}

/* Converted copy of the string param of a message for posting to a window of another charset. */
static void *convert_for_post(UINT msg, LPARAM lp, int src_unicode, int dst_unicode)
{
    if (!lp || string_kind(msg) != 1 || src_unicode == dst_unicode)
        return NULL;
    if (src_unicode)
        return aw_w2u((const WCHAR *)lp, -1);
    return aw_u2w((const char *)lp, -1);
}

/* ------------------------------------------------------------------ posting */
static void queue_append_locked(aw_queue *q, aw_msg *m)
{
    m->next = NULL;
    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
}

BOOL aw_post_input_text(aw_win *w, UINT msg, WPARAM wp, LPARAM lp, DWORD time, int x, int y, const char *text)
{
    aw_queue *q = queue_for_post(w->tid);
    aw_msg *m = calloc(1, sizeof *m);
    if (!q || !m) {
        free(m);
        return FALSE;
    }
    m->hwnd = w->hwnd;
    m->msg = msg;
    m->w = wp;
    m->l = lp;
    m->time = time;
    m->pt.x = x;
    m->pt.y = y;
    if (text)
        strncpy(m->key_text, text, sizeof m->key_text - 1);
    if ((msg >= 0x0100 /* WM_KEYFIRST */ && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSEHWHEEL)) {
        memcpy(m->keys, aw_key_live, sizeof m->keys); /* the key state at the event, see aw_key_state */
        m->has_keys = 1;
    }
    pthread_mutex_lock(&aw_wnd_lock);
    queue_append_locked(q, m);
    pthread_mutex_unlock(&aw_wnd_lock);
    aw_wake();
    return TRUE;
}

static BOOL post_to_window(HWND h, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
    aw_win *w = from(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return FALSE;
    }
    aw_queue *q = queue_for_post(w->tid);
    aw_msg *m = calloc(1, sizeof *m);
    if (!q || !m) {
        free(m);
        return FALSE;
    }
    m->owned = convert_for_post(msg, lp, unicode, w->unicode);
    if (m->owned)
        lp = (LPARAM)m->owned;
    m->hwnd = h;
    m->msg = msg;
    m->w = wp;
    m->l = lp;
    m->time = aw_tick_ms();
    pthread_mutex_lock(&aw_wnd_lock);
    queue_append_locked(q, m);
    pthread_mutex_unlock(&aw_wnd_lock);
    aw_wake();
    return TRUE;
}

API(user32, BOOL, PostMessageA, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    if (h == HWND_BROADCAST) {
        for (int i = 0; i < AW_MAX_WINDOWS; i++) {
            aw_win *w = aw_win_slot(i);
            if (w->used && !w->destroyed && !w->parent && !w->is_desktop)
                post_to_window(w->hwnd, msg, wp, lp, 0);
        }
        return TRUE;
    }
    return post_to_window(h, msg, wp, lp, 0);
}
API(user32, BOOL, PostMessageW, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    if (h == HWND_BROADCAST) {
        for (int i = 0; i < AW_MAX_WINDOWS; i++) {
            aw_win *w = aw_win_slot(i);
            if (w->used && !w->destroyed && !w->parent && !w->is_desktop)
                post_to_window(w->hwnd, msg, wp, lp, 1);
        }
        return TRUE;
    }
    return post_to_window(h, msg, wp, lp, 1);
}

static BOOL post_thread(DWORD tid, UINT msg, WPARAM wp, LPARAM lp)
{
    aw_queue *q = queue_for_post(tid);
    aw_msg *m = calloc(1, sizeof *m);
    if (!q || !m) {
        free(m);
        return FALSE;
    }
    m->msg = msg;
    m->w = wp;
    m->l = lp;
    m->time = aw_tick_ms();
    pthread_mutex_lock(&aw_wnd_lock);
    queue_append_locked(q, m);
    pthread_mutex_unlock(&aw_wnd_lock);
    aw_wake();
    return TRUE;
}

API(user32, BOOL, PostThreadMessageA, (DWORD tid, UINT msg, WPARAM wp, LPARAM lp)) { return post_thread(tid, msg, wp, lp); }
API(user32, BOOL, PostThreadMessageW, (DWORD tid, UINT msg, WPARAM wp, LPARAM lp)) { return post_thread(tid, msg, wp, lp); }

API(user32, void, PostQuitMessage, (int code))
{
    post_thread(aw_current_tid(), WM_QUIT, (WPARAM)code, 0);
}

/* ------------------------------------------------------------------ sending */
int aw_post_thread_msg_q(aw_queue *q, UINT msg, WPARAM wp, LPARAM lp)
{
    aw_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->msg = msg;
    m->w = wp;
    m->l = lp;
    pthread_mutex_lock(&aw_wnd_lock);
    queue_append_locked(q, m);
    pthread_mutex_unlock(&aw_wnd_lock);
    aw_wake();
    return 1;
}

/* Serves SendMessage requests other threads queued for this queue's thread. */
static void pump_sent(aw_queue *q)
{
    for (;;) {
        pthread_mutex_lock(&aw_wnd_lock);
        aw_sendreq *r = q->sent;
        if (r)
            q->sent = r->next;
        pthread_mutex_unlock(&aw_wnd_lock);
        if (!r)
            return;
        aw_win *w = from(r->hwnd);
        LRESULT res = w ? aw_call_proc(w, r->msg, r->w, r->l, r->unicode) : 0;
        pthread_mutex_lock(&aw_wnd_lock);
        r->result = res;
        r->done = 1;
        pthread_cond_broadcast(&aw_send_cond);
        pthread_mutex_unlock(&aw_wnd_lock);
    }
}

LRESULT aw_send_msg(HWND h, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
    aw_win *w = from(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return 0;
    }
    if (w->tid == aw_current_tid() || !w->tid)
        return aw_call_proc(w, msg, wp, lp, unicode);
    aw_queue *q = aw_queue_for_tid(w->tid);
    if (!q)
        return aw_call_proc(w, msg, wp, lp, unicode);
    aw_sendreq req;
    memset(&req, 0, sizeof req);
    req.hwnd = h;
    req.msg = msg;
    req.w = wp;
    req.l = lp;
    req.unicode = unicode;
    pthread_mutex_lock(&aw_wnd_lock);
    aw_sendreq **pp = &q->sent;
    while (*pp)
        pp = &(*pp)->next;
    *pp = &req;
    pthread_mutex_unlock(&aw_wnd_lock);
    aw_wake();
    pthread_mutex_lock(&aw_wnd_lock);
    while (!req.done)
        pthread_cond_wait(&aw_send_cond, &aw_wnd_lock);
    pthread_mutex_unlock(&aw_wnd_lock);
    return req.result;
}

API(user32, LRESULT, SendMessageA, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    if (h == HWND_BROADCAST) {
        for (int i = 0; i < AW_MAX_WINDOWS; i++) {
            aw_win *w = aw_win_slot(i);
            if (w->used && !w->destroyed && !w->parent && !w->is_desktop)
                aw_send_msg(w->hwnd, msg, wp, lp, 0);
        }
        return 0;
    }
    return aw_send_msg(h, msg, wp, lp, 0);
}
API(user32, LRESULT, SendMessageW, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    if (h == HWND_BROADCAST) {
        for (int i = 0; i < AW_MAX_WINDOWS; i++) {
            aw_win *w = aw_win_slot(i);
            if (w->used && !w->destroyed && !w->parent && !w->is_desktop)
                aw_send_msg(w->hwnd, msg, wp, lp, 1);
        }
        return 0;
    }
    return aw_send_msg(h, msg, wp, lp, 1);
}
API(user32, BOOL, SendMessageTimeoutA, (HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT flags, UINT timeout, PDWORD_PTR res))
{
    (void)flags; (void)timeout;
    LRESULT r = aw_send_msg(h, msg, wp, lp, 0);
    if (res) *res = (DWORD_PTR)r;
    return TRUE;
}
API(user32, BOOL, SendMessageTimeoutW, (HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT flags, UINT timeout, PDWORD_PTR res))
{
    (void)flags; (void)timeout;
    LRESULT r = aw_send_msg(h, msg, wp, lp, 1);
    if (res) *res = (DWORD_PTR)r;
    return TRUE;
}
API(user32, BOOL, SendNotifyMessageA, (HWND h, UINT msg, WPARAM wp, LPARAM lp)) { return post_to_window(h, msg, wp, lp, 0); }
API(user32, BOOL, SendNotifyMessageW, (HWND h, UINT msg, WPARAM wp, LPARAM lp)) { return post_to_window(h, msg, wp, lp, 1); }

API(user32, LRESULT, CallWindowProcA, (WNDPROC proc, HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    aw_win *w = from(h);
    if (!proc) return 0;
    if (w && w->proc == proc) return aw_call_proc(w, msg, wp, lp, 0);
    return proc(h, msg, wp, lp);
}
API(user32, LRESULT, CallWindowProcW, (WNDPROC proc, HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    aw_win *w = from(h);
    if (!proc) return 0;
    if (w && w->proc == proc) return aw_call_proc(w, msg, wp, lp, 1);
    return proc(h, msg, wp, lp);
}

API(user32, BOOL, InSendMessage, (void)) { return FALSE; }
API(user32, DWORD, InSendMessageEx, (void *p)) { (void)p; return 0; }
API(user32, BOOL, ReplyMessage, (LRESULT r)) { (void)r; return TRUE; }

/* ------------------------------------------------------------------ timers */
static void timer_remove(aw_queue *q, HWND h, UINT_PTR id)
{
    aw_timer **pp = &q->timers;
    while (*pp) {
        if ((*pp)->hwnd == h && (*pp)->id == id) {
            aw_timer *t = *pp;
            *pp = t->next;
            free(t);
            return;
        }
        pp = &(*pp)->next;
    }
}

void aw_timers_kill_for(HWND h)
{
    pthread_mutex_lock(&aw_wnd_lock);
    for (aw_queue *q = g_queues; q; q = q->next) {
        aw_timer **pp = &q->timers;
        while (*pp) {
            if ((*pp)->hwnd == h) {
                aw_timer *t = *pp;
                *pp = t->next;
                free(t);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    pthread_mutex_unlock(&aw_wnd_lock);
}

static UINT_PTR set_timer(HWND h, UINT_PTR id, UINT elapse, TIMERPROC proc)
{
    aw_queue *q = aw_queue_current();
    if (h) {
        aw_win *w = from(h);
        if (!w) {
            aw_set_last_error(ERR_INVALID_WINDOW);
            return 0;
        }
        q = aw_queue_for_tid(w->tid);
        if (!q) return 0;
    }
    if (elapse < AW_USER_TIMER_MIN)
        elapse = AW_USER_TIMER_MIN;
    if (!id) {
        id = q->next_timer_id++;
        if (!id) id = q->next_timer_id++;
    }
    aw_timer *t = NULL;
    for (aw_timer *c = q->timers; c; c = c->next)
        if (c->hwnd == h && c->id == id) { t = c; break; }
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return 0;
        t->hwnd = h;
        t->id = id;
        t->next = q->timers;
        q->timers = t;
    }
    t->elapse = elapse;
    t->proc = proc;
    t->due = aw_tick_ms() + elapse;
    return id;
}

API(user32, UINT_PTR, SetTimer, (HWND h, UINT_PTR id, UINT elapse, TIMERPROC proc)) { return set_timer(h, id, elapse, proc); }
API(user32, UINT_PTR, SetCoalescableTimer, (HWND h, UINT_PTR id, UINT elapse, TIMERPROC proc, ULONG tol))
{
    (void)tol;
    return set_timer(h, id, elapse, proc);
}
API(user32, BOOL, KillTimer, (HWND h, UINT_PTR id))
{
    aw_queue *q = h ? (from(h) ? aw_queue_for_tid(from(h)->tid) : NULL) : aw_queue_current();
    if (!q) return FALSE;
    int found = 0;
    for (aw_timer *c = q->timers; c; c = c->next)
        if (c->hwnd == h && c->id == id) { found = 1; break; }
    if (found) timer_remove(q, h, id);
    return found;
}

/* ------------------------------------------------------------------ the message loop */
static int match(HWND want, UINT min, UINT max, HWND h, UINT msg)
{
    if (want && want != h)
        return 0;
    if (min || max)
        return msg >= min && msg <= max;
    return 1;
}

static void fill_msg(MSG *m, HWND h, UINT msg, WPARAM wp, LPARAM lp, DWORD time, int x, int y)
{
    memset(m, 0, sizeof *m);
    m->hwnd = h;
    m->message = msg;
    m->wParam = wp;
    m->lParam = lp;
    m->time = time;
    m->pt.x = x;
    m->pt.y = y;
}

/* Sleeps until X input, a wake-up from another thread (aw_wake), a socket event, the next timer or
 * hover deadline, or max_ms (-1 = no limit).  The caller re-checks its queue afterwards.
 * Only one thread polls the X connection: a wake-up byte is consumed by whichever thread reads it,
 * so a second poller could sleep through its own wake-up.  The others wait on g_wake_cond. */
static void idle_wait(aw_queue *q, int max_ms)
{
    uint32_t now = aw_tick_ms();
    int timeout = max_ms;
    for (aw_timer *t = q->timers; t; t = t->next) {
        int32_t left = (int32_t)(t->due - now);
        if (left < 0) left = 0;
        if (timeout < 0 || left < timeout)
            timeout = left;
    }
    aw_win *hv = from(q->hover);
    if (hv && hv->tracking_hover) { /* WM_MOUSEHOVER deadline (TME_HOVER, 400 ms) */
        int32_t left = (int32_t)(q->hover_tick + 400 - now);
        if (left < 0) left = 0;
        if (timeout < 0 || left < timeout)
            timeout = left;
    }
    int poller = 0;
    pthread_mutex_lock(&aw_wnd_lock);
    if (q->head || q->sent) { /* a post raced with the caller's check: do not sleep */
        pthread_mutex_unlock(&aw_wnd_lock);
        return;
    }
    if (aw_display && !g_poller_busy) {
        g_poller_busy = 1;
        poller = 1;
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    if (poller) {
        aw_plat_event ev;
        int got = plat_poll(&ev, timeout);
        /* the flag is released before the event is dispatched: a handler may pump (nested loops) */
        pthread_mutex_lock(&aw_wnd_lock);
        g_poller_busy = 0;
        pthread_cond_broadcast(&g_wake_cond); /* let a waiting thread take over the poll */
        pthread_mutex_unlock(&aw_wnd_lock);
        if (got)
            aw_input_dispatch(&ev);
        return;
    }
    pthread_mutex_lock(&aw_wnd_lock);
    if (!q->head && !q->sent) {
        if (timeout < 0) {
            pthread_cond_wait(&g_wake_cond, &aw_wnd_lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout / 1000;
            ts.tv_nsec += (long)(timeout % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_wake_cond, &aw_wnd_lock, &ts);
        }
    }
    pthread_mutex_unlock(&aw_wnd_lock);
}

/* Returns 1 with *m filled (message removed when `remove`), or 0 when nothing is ready and !block. */
int aw_get_message(aw_queue *q, MSG *m, HWND filter, UINT min, UINT max, int remove, int block)
{
    aw_user_init();
    for (;;) {
        pump_sent(q);
        if (remove && q->owned_last) {
            free(q->owned_last);
            q->owned_last = NULL;
        }
        /* 1. posted and input messages, FIFO */
        pthread_mutex_lock(&aw_wnd_lock);
        aw_msg *prev = NULL, *a = q->head;
        while (a && !match(filter, min, max, a->hwnd, a->msg)) {
            prev = a;
            a = a->next;
        }
        if (a) {
            if (remove) {
                if (prev) prev->next = a->next; else q->head = a->next;
                if (q->tail == a) q->tail = prev;
            }
            pthread_mutex_unlock(&aw_wnd_lock);
            fill_msg(m, a->hwnd, a->msg, a->w, a->l, a->time, a->pt.x, a->pt.y);
            q->last = *m;
            q->last_valid = 1;
            q->last_time = a->time;
            q->last_pt = a->pt;
            memcpy(q->key_text, a->key_text, sizeof q->key_text);
            if (remove) {
                if (a->has_keys)
                    memcpy(aw_key_state, a->keys, sizeof aw_key_state);
                if (a->owned) {
                    free(q->owned_last);
                    q->owned_last = a->owned;
                }
                free(a);
            }
            return 1;
        }
        pthread_mutex_unlock(&aw_wnd_lock);
        /* 2. X events that are already waiting */
        if (aw_display) {
            aw_plat_event ev;
            int got = 0;
            while (plat_poll(&ev, 0)) {
                aw_input_dispatch(&ev);
                got = 1;
            }
            if (got)
                continue;
        }
        /* 3. WM_PAINT */
        HWND ph = NULL;
        if (aw_gen_paint(&ph) && match(filter, min, max, ph, WM_PAINT)) {
            aw_win *pw = from(ph);
            if (pw && pw->nc_invalid) {
                pw->nc_invalid = 0;
                aw_nc_paint(pw);
            }
            fill_msg(m, ph, WM_PAINT, 0, 0, aw_tick_ms(), 0, 0);
            q->last = *m;
            q->last_valid = 1;
            memset(q->key_text, 0, sizeof q->key_text);
            return 1;
        }
        /* 4. WM_TIMER */
        uint32_t now = aw_tick_ms();
        aw_timer *due = NULL;
        for (aw_timer *t = q->timers; t; t = t->next)
            if ((int32_t)(now - t->due) >= 0 && match(filter, min, max, t->hwnd, WM_TIMER)) {
                if (!due || (int32_t)(t->due - due->due) < 0)
                    due = t;
            }
        if (due) {
            fill_msg(m, due->hwnd, WM_TIMER, due->id, (LPARAM)due->proc, now, 0, 0);
            q->last = *m;
            q->last_valid = 1;
            memset(q->key_text, 0, sizeof q->key_text);
            if (remove) {
                due->due = now + due->elapse;
            }
            return 1;
        }
        if (!block) {
            aw_flush_all();
            return 0;
        }
        /* 5. idle: show what was drawn, then wait for input, a wake-up, a timer or a socket */
        aw_flush_all();
        aw_hover_idle();
        idle_wait(q, -1);
    }
}

/* MsgWaitForMultipleObjects' sleep: without an X connection nothing wakes the thread but the timeout,
 * so socket events are polled every 10 ms in that case. */
void aw_msg_idle_wait(int max_ms)
{
    if (!aw_display && (max_ms < 0 || max_ms > 10))
        max_ms = 10;
    idle_wait(aw_queue_current(), max_ms);
}

static BOOL get_message(MSG *m, HWND h, UINT min, UINT max)
{
    if (!m) return -1;
    aw_get_message(aw_queue_current(), m, h, min, max, 1, 1);
    return m->message != WM_QUIT;
}

API(user32, BOOL, GetMessageA, (MSG *m, HWND h, UINT min, UINT max)) { return get_message(m, h, min, max); }
API(user32, BOOL, GetMessageW, (MSG *m, HWND h, UINT min, UINT max)) { return get_message(m, h, min, max); }

static BOOL peek_message(MSG *m, HWND h, UINT min, UINT max, UINT remove)
{
    if (!m) return FALSE;
    return aw_get_message(aw_queue_current(), m, h, min, max, (remove & 1) ? 1 : 0, 0) ? TRUE : FALSE;
}

API(user32, BOOL, PeekMessageA, (MSG *m, HWND h, UINT min, UINT max, UINT remove)) { return peek_message(m, h, min, max, remove); }
API(user32, BOOL, PeekMessageW, (MSG *m, HWND h, UINT min, UINT max, UINT remove)) { return peek_message(m, h, min, max, remove); }

API(user32, BOOL, WaitMessage, (void))
{
    MSG m;
    aw_get_message(aw_queue_current(), &m, NULL, 0, 0, 0, 1);
    return TRUE;
}

API(user32, DWORD, GetQueueStatus, (UINT flags))
{
    aw_queue *q = aw_queue_current();
    pthread_mutex_lock(&aw_wnd_lock);
    int any = q->head != NULL;
    pthread_mutex_unlock(&aw_wnd_lock);
    return any ? ((flags << 16) | (flags & 0xFFFF)) : 0;
}

API(user32, DWORD, GetMessageTime, (void)) { return aw_queue_current()->last_time; }
API(user32, LONG, GetMessagePos, (void))
{
    aw_queue *q = aw_queue_current();
    return MAKELONG(q->last_pt.x, q->last_pt.y);
}
API(user32, LPARAM, GetMessageExtraInfo, (void)) { return 0; }

/* TranslateMessage: a key-down with text posts WM_CHAR / WM_SYSCHAR (one per byte for A windows). */
static void post_char(aw_win *w, UINT msg, WPARAM wp, LPARAM lp)
{
    aw_post_input_text(w, msg, wp, lp, aw_tick_ms(), 0, 0, NULL);
}

API(user32, BOOL, TranslateMessage, (const MSG *m))
{
    if (!m || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN))
        return FALSE;
    aw_queue *q = aw_queue_current();
    if (!q->key_text[0])
        return FALSE;
    char text[8];
    memcpy(text, q->key_text, sizeof text);
    memset(q->key_text, 0, sizeof q->key_text);
    aw_win *w = from(m->hwnd);
    if (!w)
        return FALSE;
    UINT cm = m->message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR;
    if (w->unicode) {
        WCHAR units[4];
        int n = aw_u2w_buf(text, -1, units, 4);
        for (int i = 0; i < n; i++)
            post_char(w, cm, units[i], m->lParam);
    } else {
        for (int i = 0; text[i]; i++)
            post_char(w, cm, (unsigned char)text[i], m->lParam);
    }
    return TRUE;
}

API(user32, LRESULT, DispatchMessageA, (const MSG *m))
{
    if (!m) return 0;
    if (m->message == WM_TIMER && m->lParam) {
        ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, (UINT_PTR)m->wParam, m->time);
        return 0;
    }
    aw_win *w = from(m->hwnd);
    if (!w) return 0;
    aw_region painted;
    rgn_init(&painted);
    if (m->message == WM_PAINT)
        for (int i = 0; i < w->update.n; i++)
            rgn_union_rect(&painted, &w->update.r[i]);
    LRESULT r = aw_call_proc(w, m->message, m->wParam, m->lParam, w->unicode);
    if (m->message == WM_PAINT) {
        /* a handler that did not BeginPaint must not be asked again for the same area */
        aw_win *ww = from(m->hwnd);
        if (ww) {
            rgn_clear(&ww->update);
            ww->erase_pending = 0;
            aw_invalidate_children_in(ww, &painted);
        }
    }
    rgn_free(&painted);
    return r;
}
API(user32, LRESULT, DispatchMessageW, (const MSG *m)) { return __aw_user32_DispatchMessageA(m); }

/* Modal loop for menus / message boxes: runs until *done is set or WM_QUIT is seen. */
void aw_modal_loop(volatile int *done)
{
    aw_queue *q = aw_queue_current();
    while (!*done) {
        MSG m;
        aw_get_message(q, &m, NULL, 0, 0, 1, 1);
        if (m.message == WM_QUIT) {
            aw_post_thread_msg_q(q, WM_QUIT, m.wParam, 0);
            break;
        }
        __aw_user32_TranslateMessage(&m);
        __aw_user32_DispatchMessageA(&m);
    }
}

/* ------------------------------------------------------------------ DefWindowProc */
static void set_bkcolors(HDC hdc, int bk_idx, int text_idx)
{
    __aw_gdi32_SetBkColor(hdc, aw_syscolor(bk_idx));
    __aw_gdi32_SetTextColor(hdc, aw_syscolor(text_idx));
}

static LRESULT ctlcolor(aw_win *w, UINT msg, WPARAM wp)
{
    (void)w;
    int bk = 5 /* COLOR_WINDOW */, tx = 8 /* COLOR_WINDOWTEXT */;
    switch (msg) {
    case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR:
        bk = 15 /* COLOR_BTNFACE */;
        tx = 18 /* COLOR_BTNTEXT */;
        break;
    default:
        break;
    }
    set_bkcolors((HDC)wp, bk, tx);
    return (LRESULT)(uintptr_t)__aw_user32_GetSysColorBrush(bk);
}

LRESULT aw_defwndproc(aw_win *w, UINT msg, WPARAM wp, LPARAM lp, int unicode)
{
    (void)unicode;
    HWND h = w->hwnd;
    switch (msg) {
    case WM_NCCREATE:
        return TRUE;
    case WM_NCCALCSIZE:
        if (wp) {
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            aw_nc_calc(w);
            p->rgrc[0].left = w->rect.left + w->client.left;
            p->rgrc[0].top = w->rect.top + w->client.top;
            p->rgrc[0].right = w->rect.left + w->client.right;
            p->rgrc[0].bottom = w->rect.top + w->client.bottom;
        }
        return 0;
    case WM_NCPAINT:
        aw_nc_paint(w);
        return 0;
    case WM_NCHITTEST: {
        int sx = (short)LOWORD(lp), sy = (short)HIWORD(lp);
        int ox, oy;
        aw_screen_pos(w, &ox, &oy);
        return aw_nc_hit(w, sx - ox, sy - oy);
    }
    case WM_NCLBUTTONDOWN:
        if (wp == HTHSCROLL || wp == HTVSCROLL)
            aw_scroll_track(w, (int)wp, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_ERASEBKGND: {
        HBRUSH br = w->cls ? w->cls->brush : NULL;
        if (!br)
            return 0;
        aw_win *top = aw_top_ancestor(w);
        aw_region vis;
        rgn_init(&vis);
        if (w->clip_active) {
            rgn_copy(&vis, &w->clip);
        } else {
            aw_visible_region(w, &vis);
        }
        aw_erase_bkgnd(w, top, &vis, br);
        rgn_free(&vis);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        __aw_user32_BeginPaint(h, &ps);
        __aw_user32_EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        aw_destroy_window_internal(w);
        return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_CLOSE:
            aw_call_proc(w, WM_CLOSE, 0, 0, w->unicode);
            break;
        case SC_MINIMIZE:
            aw_show_window(w, 6);
            break;
        case SC_MAXIMIZE:
            aw_show_window(w, 3);
            break;
        case SC_RESTORE:
            aw_show_window(w, 9);
            break;
        case SC_KEYMENU:
            if (!w->parent && w->menu)
                aw_menubar_track(h, w->menu, -1, -1, (int)lp);
            break;
        default:
            break;
        }
        return 0;
    case WM_SYSKEYDOWN: {
        aw_queue *q = aw_queue_current();
        (void)q;
        if (wp == 0x73 /* VK_F4 */ && aw_is_alt_down()) {
            aw_call_proc(w, WM_SYSCOMMAND, SC_CLOSE, 0, w->unicode);
        } else if (wp == 0x79 /* VK_F10 */ && !w->parent && w->menu) {
            aw_menubar_track(h, w->menu, -1, -1, 0x79);
        }
        return 0;
    }
    case WM_GETTEXT: {
        if (!lp || wp == 0) return 0;
        if (w->unicode) {
            int n = w->text_len;
            if (n > (int)wp - 1) n = (int)wp - 1;
            if (n > 0) memcpy((WCHAR *)lp, w->text, (size_t)n * sizeof(WCHAR));
            ((WCHAR *)lp)[n > 0 ? n : 0] = 0;
            return n > 0 ? n : 0;
        }
        return aw_w2u_term(w->text ? w->text : aw_wempty, -1, (char *)lp, (int)wp);
    }
    case WM_GETTEXTLENGTH: {
        if (w->unicode) return w->text_len;
        char *u = aw_w2u(w->text ? w->text : aw_wempty, -1);
        LRESULT n = u ? (LRESULT)strlen(u) : 0;
        free(u);
        return n;
    }
    case WM_SETTEXT: {
        WCHAR *tmp = NULL;
        const WCHAR *s;
        if (!lp) {
            s = aw_wempty;
        } else if (w->unicode) {
            s = (const WCHAR *)lp;
        } else {
            tmp = aw_u2w((const char *)lp, -1);
            s = tmp ? tmp : aw_wempty;
        }
        aw_win_set_text(w, s, -1);
        free(tmp);
        aw_update_x_title(w);
        return TRUE;
    }
    case WM_SETFONT:
        w->font = (HFONT)wp;
        if (LOWORD(lp))
            aw_invalidate(w, NULL, 1);
        return 0;
    case WM_GETFONT:
        return (LRESULT)(uintptr_t)w->font;
    case WM_SETICON: {
        HICON old = (wp == 0) ? w->icon_small : w->icon_big;
        if (wp == 0) w->icon_small = (HICON)lp; else w->icon_big = (HICON)lp;
        if (!w->parent && w->xwin) {
            HICON ic = w->icon_big ? w->icon_big : w->icon_small;
            aw_icon *ai = aw_icon_from_handle(ic);
            if (ai)
                plat_window_set_icon(w->xwin, ai->argb, ai->w, ai->h);
        }
        return (LRESULT)(uintptr_t)old;
    }
    case WM_GETICON: {
        HICON ic = (wp == 0) ? w->icon_small : w->icon_big;
        if (!ic && w->cls) ic = (wp == 0) ? w->cls->icon_sm : w->cls->icon;
        return (LRESULT)(uintptr_t)ic;
    }
    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS *wpos = (WINDOWPOS *)lp;
        UINT f = wpos ? wpos->flags : 0;
        int cw = w->client.right - w->client.left, ch = w->client.bottom - w->client.top;
        if (!(f & SWP_NOMOVE))
            aw_call_proc(w, WM_MOVE, 0, MAKELONG(w->rect.left, w->rect.top), w->unicode);
        if (!(f & SWP_NOSIZE)) {
            WPARAM st = w->minimized ? 1 : (w->maximized ? 2 : 0);
            aw_call_proc(w, WM_SIZE, st, MAKELONG(cw, ch), w->unicode);
        }
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return 1; /* MA_ACTIVATE */
    case WM_SETCURSOR:
        if (LOWORD(lp) == NC_HTCLIENT_CODE)
            aw_apply_cursor(w);
        return TRUE;
    case WM_CONTEXTMENU:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (w->parent) {
            aw_win *p = from(w->parent);
            if (p) aw_send_msg(p->hwnd, msg, wp, lp, w->unicode);
        }
        return 0;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        return ctlcolor(w, msg, wp);
    case WM_NOTIFYFORMAT:
        if (lp == 3 /* NF_QUERY */ || lp == 4 /* NF_REQUERY */)
            return w->wcs ? 2 : 1;
        return 0;
    case WM_QUERYOPEN:
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_CANCELMODE:
        if (aw_queue_current()->capture) aw_capture_set(NULL);
        return 0;
    default:
        return 0;
    }
}

/* DefWindowProc works in the window's proc charset; the caller's A / W arguments are converted */
static LRESULT AWCALL def_in_proc_charset(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    aw_win *w = from(h);
    return w ? aw_defwndproc(w, msg, wp, lp, w->unicode) : 0;
}

API(user32, LRESULT, DefWindowProcA, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    aw_win *w = from(h);
    if (!w) return 0;
    return aw_call_conv((WNDPROC)(void *)def_in_proc_charset, h, msg, wp, lp, 0, w->unicode);
}
API(user32, LRESULT, DefWindowProcW, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    aw_win *w = from(h);
    if (!w) return 0;
    return aw_call_conv((WNDPROC)(void *)def_in_proc_charset, h, msg, wp, lp, 1, w->unicode);
}
API(user32, LRESULT, DefMDIChildProcA, (HWND h, UINT msg, WPARAM wp, LPARAM lp)) { return __aw_user32_DefWindowProcA(h, msg, wp, lp); }
API(user32, LRESULT, DefMDIChildProcW, (HWND h, UINT msg, WPARAM wp, LPARAM lp)) { return __aw_user32_DefWindowProcW(h, msg, wp, lp); }
