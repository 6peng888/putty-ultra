/*
 * sftpgui.c - two-pane SFTP browser and file transfer window for the
 * Windows GUI.
 *
 * The window does not dial a second connection. It asks the SSH
 * backend of an existing session for an extra 'session' channel (see
 * ssh_open_session_channel) and starts the "sftp" subsystem on it, so
 * the user is never asked for a password again.
 *
 * Everything is asynchronous: SFTP bytes arrive through the Channel
 * implementation below, are reassembled into complete SFTP packets and
 * handed to whichever request asked for them. There is no blocking
 * receive anywhere, so the window stays responsive and the terminal
 * keeps working while a transfer runs.
 *
 * ssh/sftp.c keeps its pending-request table in file-scope globals, so
 * only one of these windows can exist per process.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <wchar.h>

#include "putty.h"
#include "i18n.h"
#include "ssh.h"
#include "ssh/channel.h"
#include "ssh/sftp.h"
#include "sftpgui.h"

#include <commctrl.h>

/* ----------------------------------------------------------------------
 * UI identifiers.
 */

#define SG_ID_L_LABEL    1100
#define SG_ID_L_PATH     1101
#define SG_ID_L_UP       1102
#define SG_ID_L_LIST     1103
#define SG_ID_R_LABEL    1110
#define SG_ID_R_PATH     1111
#define SG_ID_R_UP       1112
#define SG_ID_R_LIST     1113
#define SG_ID_UPLOAD     1120
#define SG_ID_DOWNLOAD   1121
#define SG_ID_CANCEL     1123
#define SG_ID_PROGRESS   1130
#define SG_ID_STATUS     1131

#define SG_CLASSNAME L"PuTTYSftpWindow"

/*
 * Messages the channel implementation posts to the window. The
 * wParam is one of the SG_CH_* codes below.
 */
#define WM_SG_NOTIFY (WM_APP + 21)

#define SG_CH_LOST        0     /* channel (and so the connection) is gone */
#define SG_CH_OPENFAILED  1     /* lParam is a malloc'd error message */
#define SG_CH_SUBSYS_OK   2     /* the sftp subsystem is running */
#define SG_CH_SUBSYS_BAD  3     /* the server refused the subsystem */

/*
 * State of the single SFTP request the window has outstanding outside
 * of a bulk transfer. Bulk transfers use their own per-request
 * contexts (see struct SgReqCtx) instead.
 */
enum {
    SG_P_NONE = 0,
    SG_P_VERSION,          /* waiting for SSH_FXP_VERSION */
    SG_P_REALPATH,         /* changing the remote directory */
    SG_P_OPENDIR,
    SG_P_READDIR,
    SG_P_CLOSE_DIR,
    SG_P_TASKMKDIR,        /* batch: create a remote directory */
    SG_P_TASKOPENDIR,      /* batch: start listing a remote directory */
    SG_P_TASKREADDIR,      /* batch: keep listing a remote directory */
    SG_P_TASKCLOSEDIR,     /* batch: finish listing a remote directory */
    SG_P_OPEN_DOWNLOAD,
    SG_P_OPEN_UPLOAD,
    SG_P_DOWNLOAD,         /* bulk transfer, requests carry a context */
    SG_P_UPLOAD,
    SG_P_CLOSE_FILE,
};

/* Tag telling the reply handler what a bulk-transfer request was. */
#define SG_TAG_READ  1
#define SG_TAG_WRITE 2

/* How much we ask for in one go, and how many requests we keep in flight. */
#define SG_CHUNK      32768
#define SG_INFLIGHT   4

typedef struct SftpGui SftpGui;

struct SgEntry {
    char *name;                 /* UTF-8, for both panes */
    wchar_t *wname;             /* local entries: the name as NTFS gave it,
                                 * so it is displayed without any conversion */
    uint64_t size;
    time_t mtime;
    bool is_dir;
    bool reparse;               /* junction or symlink: don't recurse into */
};

struct SgPane {
    HWND label, path, up, list;
    char *cwd;                  /* UTF-8 */
    struct SgEntry *ents;
    int nents, entsize;
};

/* What to do next in a transfer batch. */
#define SG_T_FILE 0             /* copy one file */
#define SG_T_LDIR 1             /* upload: make 'remote', then walk 'local' */
#define SG_T_RDIR 2             /* download: make 'local', then walk 'remote' */

struct SgTask {
    int kind;
    char *local;
    char *remote;
    uint64_t size;              /* for the progress bar */
};

/* Per-request context attached to read/write requests. */
struct SgReqCtx {
    int tag;
    SftpGui *g;
    uint64_t offset;
    int len;
    char *buf;                  /* read requests only */
};

struct SgChan {
    Channel chan;
    SftpGui *g;                 /* NULL once the window has gone away */
    SshChannel *sc;
};

struct SftpGui {
    HWND hwnd;

    SshChannel *sc;
    struct SgChan *schan;
    bool chan_open, subsys_ok;

    struct SgPane lcl, rmt;     /* local and remote panes */
    HWND btn_upload, btn_download, btn_cancel;
    HWND prog, status;

    /* Reassembly buffer for incoming SFTP data. */
    char *inbuf;
    size_t inlen, incap;

    int pend;
    struct fxp_handle *dirhandle;   /* directory being listed */
    struct fxp_handle *handle;      /* file being transferred */
    char *target_path;          /* what the pending realpath was for */

    /* Remote directory listing under construction. */
    struct SgEntry *rents;
    int nrents, rents_size;

    /* Bulk transfer state. */
    bool busy, uploading, cancel;
    struct SgTask *tasks;
    int ntasks, tasks_size, task_head;
    char *cur_local, *cur_remote;   /* file being transferred */
    char *walk_local, *walk_remote; /* directory being enumerated */
    struct fxp_handle *walk_handle;
    uint64_t total_bytes, done_bytes;
    uint64_t next_offset;
    int inflight;
    bool at_eof;
    HANDLE lfh;
    uint64_t cur_size;
    bool any_error;
    bool fatal_shown;           /* don't let a later error overwrite one */

    /*
     * Short read in the middle of a download: if the server ever
     * returns less than we asked for before the end of the file, we
     * know where the data really stops and can catch the case where a
     * later (already queued) request comes back with data past it.
     */
    bool dl_end_known;
    uint64_t dl_end;
};

static SftpGui *sg_instance;
static const char *sg_startup_error;

/* ----------------------------------------------------------------------
 * Forward declarations for the mutually recursive parts.
 */
static void sg_local_refresh(SftpGui *g);
static void sg_remote_refresh(SftpGui *g);
static void sg_remote_goto(SftpGui *g, const char *path);
static void sg_process_inbuf(SftpGui *g);
static void sg_xfer_fail(SftpGui *g, const char *why);
static void sg_xfer_next(SftpGui *g);
static void sg_set_busy(SftpGui *g, bool busy);
static void sg_lv_fill(HWND lv, struct SgEntry *ents, int n);
static void sg_entries_free(struct SgEntry *ents, int n);
static LRESULT CALLBACK SgWndProc(HWND, UINT, WPARAM, LPARAM);

/* ----------------------------------------------------------------------
 * Small helpers.
 */

/*
 * UTF-8 <-> UTF-16 conversion done by Windows itself.
 *
 * Doing it this way rather than through the generic PuTTY charset
 * helpers keeps filenames away from any code-page table lookup, which
 * is what used to turn non-ASCII local filenames into question marks.
 */
static wchar_t *sg_w(const char *utf8)
{
    int n;
    wchar_t *ret;
    UINT cp = CP_UTF8;

    if (!utf8)
        utf8 = "";
    n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) {
        /*
         * Not valid UTF-8. Remote filenames are whatever the server
         * sent, so fall back to the system code page rather than
         * showing nothing at all.
         */
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, utf8, -1, NULL, 0);
    }
    if (n <= 0) {
        ret = snewn(1, wchar_t);
        ret[0] = L'\0';
        return ret;
    }
    ret = snewn(n, wchar_t);
    MultiByteToWideChar(cp, 0, utf8, -1, ret, n);
    return ret;
}

static wchar_t *sg_dupw(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *ret = snewn(n, wchar_t);

    memcpy(ret, s, n * sizeof(wchar_t));
    return ret;
}

static char *sg_to_utf8(const wchar_t *w)
{
    int n;
    char *ret;

    if (!w)
        w = L"";
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return dupstr("");
    ret = snewn(n, char);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, ret, n, NULL, NULL);
    return ret;
}

static void sg_set_text(HWND h, const char *utf8)
{
    wchar_t *w = sg_w(utf8);
    SetWindowTextW(h, w);
    sfree(w);
}

/* Set a control's text from an English string that may be translated. */
static void sg_set_tr(HWND h, const char *english)
{
    sg_set_text(h, i18n_or(english));
}

static void sg_msgbox(HWND hwnd, const char *english, UINT type)
{
    wchar_t *w = sg_w(i18n_or(english));
    wchar_t *cap = sg_w("SFTP");
    MessageBoxW(hwnd, w, cap, type);
    sfree(w);
    sfree(cap);
}

static void sg_status(SftpGui *g, const char *msg)
{
    sg_set_text(g->status, msg);
}

static void sg_statusf(SftpGui *g, const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    sg_set_text(g->status, msg);
    sfree(msg);
}

static void sg_fmt_size(char *buf, size_t n, uint64_t size)
{
    if (size < 1024)
        snprintf(buf, n, "%llu B", (unsigned long long)size);
    else if (size < 1024 * 1024)
        snprintf(buf, n, "%.1f KB", size / 1024.0);
    else if (size < 1024ULL * 1024 * 1024)
        snprintf(buf, n, "%.1f MB", size / (1024.0 * 1024));
    else
        snprintf(buf, n, "%.1f GB", size / (1024.0 * 1024 * 1024));
}

static void sg_fmt_time(char *buf, size_t n, time_t t)
{
    struct tm *tm;

    if (t <= 0) {
        snprintf(buf, n, "-");
        return;
    }
    tm = localtime(&t);
    if (!tm || !strftime(buf, n, "%Y-%m-%d %H:%M", tm))
        snprintf(buf, n, "-");
}

/* Join two path elements with a separator, avoiding a doubled one. */
static char *sg_join(const char *dir, const char *name, char sep)
{
    size_t dl = strlen(dir), nl = strlen(name);
    bool addsep = (dl > 0 && dir[dl-1] != sep);
    char *ret = snewn(dl + (addsep ? 1 : 0) + nl + 1, char);
    size_t p = dl;

    memcpy(ret, dir, dl);
    if (addsep)
        ret[p++] = sep;
    memcpy(ret + p, name, nl);
    ret[p + nl] = '\0';
    return ret;
}

static char *sg_parent_remote(const char *path)
{
    size_t n = strlen(path);
    char *ret;

    while (n > 1 && path[n-1] == '/')
        n--;
    if (n <= 1)
        return dupstr("/");
    size_t i = n;
    while (i > 0 && path[i-1] != '/')
        i--;
    if (i <= 1)
        return dupstr("/");
    ret = snewn(i, char);              /* i-1 characters plus NUL */
    memcpy(ret, path, i - 1);
    ret[i-1] = '\0';
    return ret;
}

static char *sg_parent_local(const char *path)
{
    size_t n = strlen(path), i, keep;
    char *ret;

    if (n == 0)
        return dupstr("C:\\");
    /* A drive root, "C:" or "C:\", is its own parent. */
    if (n <= 3 && n >= 2 && path[1] == ':')
        return dupstr(n == 2 ? "C:\\" : path);
    if (n > 3 && path[n-1] == '\\')
        n--;
    i = n;
    while (i > 0 && path[i-1] != '\\')
        i--;
    if (i <= 1)
        return dupstr("C:\\");
    keep = i - 1;
    if (keep == 2 && path[1] == ':')
        keep = 3;                      /* keep the drive root's slash */
    ret = snewn(keep + 1, char);
    memcpy(ret, path, keep);
    ret[keep] = '\0';
    return ret;
}

/* Turn whatever the user typed into an absolute local path. */
static char *sg_local_absolute(const char *path)
{
    wchar_t *win = sg_w(path);
    wchar_t *wfull = NULL;
    char *ret = NULL;
    DWORD n = GetFullPathNameW(win, 0, NULL, NULL);

    if (n > 0) {
        wfull = snewn(n + 1, wchar_t);
        if (GetFullPathNameW(win, n + 1, wfull, NULL) == 0) {
            sfree(wfull);
            wfull = NULL;
        }
    }
    sfree(win);
    if (!wfull)
        return dupstr(path);

    /* Drop a trailing backslash unless it is a drive root. */
    size_t wl = wcslen(wfull);
    if (wl > 3 && wfull[wl-1] == L'\\')
        wfull[wl-1] = L'\0';

    ret = sg_to_utf8(wfull);
    sfree(wfull);
    return ret;
}

/* ----------------------------------------------------------------------
 * Entry lists.
 */

static void sg_entries_free(struct SgEntry *ents, int n)
{
    if (!ents)
        return;
    for (int i = 0; i < n; i++) {
        sfree(ents[i].name);
        sfree(ents[i].wname);
    }
    sfree(ents);
}

static void sg_entry_push(struct SgEntry **arr, int *n, int *size,
                          const char *name, const wchar_t *wname,
                          uint64_t sz, time_t mt, bool dir)
{
    struct SgEntry *e;

    if (*n >= *size) {
        *size = *size ? *size * 2 : 64;
        *arr = sresize(*arr, *size, struct SgEntry);
    }
    e = &(*arr)[(*n)++];
    e->name = dupstr(name);
    e->wname = wname ? sg_dupw(wname) : NULL;
    e->size = sz;
    e->mtime = mt;
    e->is_dir = dir;
    e->reparse = false;
}

static int sg_entry_cmp(const void *av, const void *bv)
{
    const struct SgEntry *a = (const struct SgEntry *)av;
    const struct SgEntry *b = (const struct SgEntry *)bv;
    bool adot = !strcmp(a->name, ".."), bdot = !strcmp(b->name, "..");

    /* ".." always first, then directories, then files, each by name. */
    if (adot != bdot)
        return adot ? -1 : +1;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : +1;
    return stricmp(a->name, b->name);
}

/* Read a local directory. Returns NULL, or an allocated error message. */
static char *sg_local_list(const char *cwd,
                           struct SgEntry **out, int *nout)
{
    struct SgEntry *ents = NULL;
    int n = 0, size = 0;
    WIN32_FIND_DATAW fd;
    wchar_t *wdir = sg_w(cwd);
    size_t dl = wcslen(wdir);
    bool addsep = (dl > 0 && wdir[dl-1] != L'\\');
    wchar_t *pattern = snewn(dl + (addsep ? 1 : 0) + 2, wchar_t);
    char *err = NULL;
    size_t p = dl;
    HANDLE h;

    memcpy(pattern, wdir, dl * sizeof(wchar_t));
    if (addsep)
        pattern[p++] = L'\\';
    pattern[p++] = L'*';
    pattern[p] = L'\0';
    sfree(wdir);

    h = FindFirstFileW(pattern, &fd);
    sfree(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return dupprintf("%s: %s", cwd, win_strerror(GetLastError()));

    do {
        char *name;
        uint64_t sz;
        time_t mt;
        bool dir, dot;

        dot = !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..");
        if (dot)
            continue;

        dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        mt = (time_t)(((((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                        fd.ftLastWriteTime.dwLowDateTime) / 10000000ULL)
                      - 11644473600ULL);
        name = sg_to_utf8(fd.cFileName);
        sg_entry_push(&ents, &n, &size, name, fd.cFileName, sz, mt, dir);
        /* Junctions and symlinks are not followed: "Application Data"
         * and friends point back up the tree and would loop for ever. */
        ents[n-1].reparse =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        sfree(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    /* A parent entry, so the pane can be navigated with the mouse. */
    sg_entry_push(&ents, &n, &size, "..", L"..", 0, 0, true);

    qsort(ents, n, sizeof(struct SgEntry), sg_entry_cmp);
    *out = ents;
    *nout = n;
    return err;
}

/* ----------------------------------------------------------------------
 * List view plumbing.
 */

static void sg_place(HWND h, int x, int y, int w, int hgt)
{
    if (w < 0) w = 0;
    if (hgt < 0) hgt = 0;
    MoveWindow(h, x, y, w, hgt, TRUE);
}

static void sg_lv_fill(HWND lv, struct SgEntry *ents, int n)
{
    LVITEMW item;
    char sizebuf[32], timebuf[32];

    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);

    for (int i = 0; i < n; i++) {
        wchar_t *wname, *wsize, *wtime;
        int idx;

        if (ents[i].is_dir)
            sizebuf[0] = '\0';
        else
            sg_fmt_size(sizebuf, sizeof(sizebuf), ents[i].size);
        sg_fmt_time(timebuf, sizeof(timebuf), ents[i].mtime);

        wname = ents[i].wname ? sg_dupw(ents[i].wname) : sg_w(ents[i].name);
        wsize = sg_w(sizebuf);
        wtime = sg_w(timebuf);

        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = wname;
        item.lParam = (LPARAM)i;
        idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&item);

        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.iSubItem = 1;
        item.pszText = wsize;
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, (LPARAM)&item);
        item.iSubItem = 2;
        item.pszText = wtime;
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

        sfree(wname);
        sfree(wsize);
        sfree(wtime);
    }

    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
}

/* Index of the one selected row, -1 for none, -2 for several. */
static int sg_lv_selected(HWND lv)
{
    int idx = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    int nxt;

    if (idx < 0)
        return -1;
    nxt = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)idx, LVNI_SELECTED);
    return nxt >= 0 ? -2 : idx;
}

/* Collect all selected rows. Caller frees the returned array. */
static int sg_lv_collect(HWND lv, int **out)
{
    int cap = 8, n = 0, i = -1;
    int *idxs = snewn(cap, int);

    while ((i = (int)SendMessageW(lv, LVM_GETNEXTITEM,
                                  (WPARAM)i, LVNI_SELECTED)) >= 0) {
        if (n >= cap) {
            cap *= 2;
            idxs = sresize(idxs, cap, int);
        }
        idxs[n++] = i;
    }
    *out = idxs;
    return n;
}

/* ----------------------------------------------------------------------
 * The SSH channel the SFTP protocol runs over.
 */

static void sg_transport_lost(SftpGui *g)
{
    g->chan_open = false;
    g->sc = NULL;
    PostMessage(g->hwnd, WM_SG_NOTIFY, SG_CH_LOST, 0);
}

static void sg_chan_free(Channel *chan)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);
    SftpGui *g = c->g;

    if (g) {
        g->schan = NULL;
        sg_transport_lost(g);
    }
    sfree(c);
}

static void sg_chan_open_confirmation(Channel *chan)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);

    if (!c->g)
        return;
    c->g->chan_open = true;
    /* Now that the channel exists, ask the server for the subsystem. */
    sshfwd_start_subsystem(c->sc, true, "sftp");
}

static void sg_chan_open_failed(Channel *chan, const char *errtext)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);
    char *msg;

    if (!c->g)
        return;
    msg = dupprintf("SFTP: %s", errtext);
    PostMessage(c->g->hwnd, WM_SG_NOTIFY, SG_CH_OPENFAILED, (LPARAM)msg);
}

static size_t sg_chan_send(Channel *chan, bool is_stderr,
                           const void *data, size_t len)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);
    SftpGui *g = c->g;

    if (!g || is_stderr)
        return len;                     /* nothing useful in stderr */
    if (g->inlen + len > g->incap) {
        size_t want = g->incap ? g->incap : 16384;
        while (want < g->inlen + len)
            want *= 2;
        g->inbuf = sresize(g->inbuf, want, char);
        g->incap = want;
    }
    memcpy(g->inbuf + g->inlen, data, len);
    g->inlen += len;
    sg_process_inbuf(g);
    return len;
}

static void sg_chan_send_eof(Channel *chan)
{
    (void)chan;
}

static void sg_chan_set_input_wanted(Channel *chan, bool wanted)
{
    (void)chan;
    (void)wanted;
}

static char *sg_chan_log_close_msg(Channel *chan)
{
    (void)chan;
    return dupstr("SFTP channel closed");
}

static bool sg_chan_rcvd_exit_status(Channel *chan, int status)
{
    (void)chan;
    (void)status;
    return true;
}

static void sg_chan_request_response(Channel *chan, bool success)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);

    if (!c->g)
        return;
    PostMessage(c->g->hwnd, WM_SG_NOTIFY,
                success ? SG_CH_SUBSYS_OK : SG_CH_SUBSYS_BAD, 0);
}

static const ChannelVtable SgChan_vt = {
    .free = sg_chan_free,
    .open_confirmation = sg_chan_open_confirmation,
    .open_failed = sg_chan_open_failed,
    .send = sg_chan_send,
    .send_eof = sg_chan_send_eof,
    .set_input_wanted = sg_chan_set_input_wanted,
    .log_close_msg = sg_chan_log_close_msg,
    .want_close = chan_default_want_close,
    .rcvd_exit_status = sg_chan_rcvd_exit_status,
    .rcvd_exit_signal = chan_no_exit_signal,
    .rcvd_exit_signal_numeric = chan_no_exit_signal_numeric,
    .run_shell = chan_no_run_shell,
    .run_command = chan_no_run_command,
    .run_subsystem = chan_no_run_subsystem,
    .enable_x11_forwarding = chan_no_enable_x11_forwarding,
    .enable_agent_forwarding = chan_no_enable_agent_forwarding,
    .allocate_pty = chan_no_allocate_pty,
    .set_env = chan_no_set_env,
    .send_break = chan_no_send_break,
    .send_signal = chan_no_send_signal,
    .change_window_size = chan_no_change_window_size,
    .request_response = sg_chan_request_response,
};

/*
 * Transport hooks that ssh/sftp.c expects the front end to provide.
 * They all talk to the one SFTP channel this module can have.
 */
bool sftp_senddata(const char *data, size_t len)
{
    SftpGui *g = sg_instance;

    if (!g || !g->sc || !g->chan_open)
        return false;
    sshfwd_write(g->sc, data, len);
    return true;
}

size_t sftp_sendbuffer(void)
{
    /*
     * We pace our own uploads by counting requests in flight, so the
     * SSH socket backlog never grows large on our account.
     */
    return 0;
}

bool sftp_recvdata(char *data, size_t len)
{
    (void)data;
    (void)len;
    /*
     * Never called: this front end reassembles and dispatches packets
     * as they arrive instead of blocking for them.
     */
    return false;
}

/* ----------------------------------------------------------------------
 * Pane navigation.
 */

static void sg_local_refresh(SftpGui *g)
{
    struct SgEntry *ents = NULL;
    int n = 0;
    char *err = sg_local_list(g->lcl.cwd, &ents, &n);

    if (err) {
        sg_status(g, err);
        sfree(err);
        return;
    }
    sg_entries_free(g->lcl.ents, g->lcl.nents);
    g->lcl.ents = ents;
    g->lcl.nents = n;
    sg_set_text(g->lcl.path, g->lcl.cwd);
    sg_lv_fill(g->lcl.list, ents, n);
}

/* Move the local pane, keeping the old directory if it isn't readable. */
static void sg_local_goto(SftpGui *g, const char *path)
{
    char *abs = sg_local_absolute(path);
    struct SgEntry *ents = NULL;
    int n = 0;
    char *err = sg_local_list(abs, &ents, &n);

    if (err) {
        sg_status(g, err);
        sfree(err);
        sfree(abs);
        sg_set_text(g->lcl.path, g->lcl.cwd);
        return;
    }
    sg_entries_free(g->lcl.ents, g->lcl.nents);
    sfree(g->lcl.cwd);
    g->lcl.cwd = abs;
    g->lcl.ents = ents;
    g->lcl.nents = n;
    sg_set_text(g->lcl.path, abs);
    sg_lv_fill(g->lcl.list, ents, n);
}

/* ----------------------------------------------------------------------
 * Remote directory browsing.
 */

static void sg_remote_send_readdir(SftpGui *g)
{
    struct sftp_request *req;

    g->pend = SG_P_READDIR;
    req = fxp_readdir_send(g->dirhandle);
    sftp_register(req);
}

static void sg_remote_fill(SftpGui *g)
{
    if (strcmp(g->rmt.cwd, "/") != 0)
        sg_entry_push(&g->rents, &g->nrents, &g->rents_size,
                      "..", NULL, 0, 0, true);
    qsort(g->rents, g->nrents, sizeof(struct SgEntry), sg_entry_cmp);
    sg_entries_free(g->rmt.ents, g->rmt.nents);
    g->rmt.ents = g->rents;
    g->rmt.nents = g->nrents;
    g->rents = NULL;
    g->nrents = g->rents_size = 0;
    sg_lv_fill(g->rmt.list, g->rmt.ents, g->rmt.nents);
}

static void sg_remote_refresh(SftpGui *g)
{
    struct sftp_request *req;

    if (g->pend != SG_P_NONE)
        return;                        /* busy; a later event will retry */

    sg_entries_free(g->rents, g->nrents);
    g->rents = NULL;
    g->nrents = g->rents_size = 0;

    sg_set_text(g->rmt.path, g->rmt.cwd);
    g->pend = SG_P_OPENDIR;
    req = fxp_opendir_send(g->rmt.cwd);
    sftp_register(req);
}

/* Ask the server to canonicalise 'path' and make it our directory. */
static void sg_remote_goto(SftpGui *g, const char *path)
{
    struct sftp_request *req;

    if (g->pend != SG_P_NONE || !g->subsys_ok)
        return;

    sfree(g->target_path);
    g->target_path = dupstr(path);
    g->pend = SG_P_REALPATH;
    req = fxp_realpath_send(path);
    sftp_register(req);
}

/* ----------------------------------------------------------------------
 * Transfers.
 */

static void sg_set_busy(SftpGui *g, bool busy)
{
    g->busy = busy;
    EnableWindow(g->btn_upload, !busy);
    EnableWindow(g->btn_download, !busy);
    EnableWindow(g->btn_cancel, busy);
    EnableWindow(g->rmt.up, !busy);
    EnableWindow(g->lcl.up, !busy);
}

static void sg_progress(SftpGui *g)
{
    int permille = 0;

    if (g->total_bytes > 0)
        permille = (int)((g->done_bytes * 1000) / g->total_bytes);
    if (permille > 1000)
        permille = 1000;
    SendMessageW(g->prog, PBM_SETPOS, (WPARAM)permille, 0);
}

/* ----------------------------------------------------------------------
 * The transfer work list. Tasks are consumed from the front, and a
 * directory task appends its children, so a whole tree is walked
 * breadth-first without needing any recursion.
 */

static void sg_task_push(SftpGui *g, int kind, const char *local,
                         const char *remote, uint64_t size)
{
    struct SgTask *t;

    if (g->ntasks >= g->tasks_size) {
        g->tasks_size = g->tasks_size ? g->tasks_size * 2 : 16;
        g->tasks = sresize(g->tasks, g->tasks_size, struct SgTask);
    }
    t = &g->tasks[g->ntasks++];
    t->kind = kind;
    t->local = dupstr(local);
    t->remote = dupstr(remote);
    t->size = size;
}

static void sg_tasks_free(SftpGui *g)
{
    for (int i = 0; i < g->ntasks; i++) {
        sfree(g->tasks[i].local);
        sfree(g->tasks[i].remote);
    }
    sfree(g->tasks);
    g->tasks = NULL;
    g->ntasks = g->tasks_size = g->task_head = 0;
    sfree(g->cur_local);
    sfree(g->cur_remote);
    g->cur_local = g->cur_remote = NULL;
    sfree(g->walk_local);
    sfree(g->walk_remote);
    g->walk_local = g->walk_remote = NULL;
    g->walk_handle = NULL;
}

static void sg_xfer_finish(SftpGui *g, const char *msg)
{
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    g->pend = SG_P_NONE;
    g->inflight = 0;
    g->at_eof = false;
    sg_tasks_free(g);
    sg_set_busy(g, false);
    sg_status(g, msg);
    sg_remote_refresh(g);
    sg_local_refresh(g);
}

/* Abandon the running transfer and say why. */
static void sg_xfer_fail(SftpGui *g, const char *why)
{
    g->any_error = true;
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    g->inflight = 0;
    g->at_eof = false;
    g->pend = SG_P_NONE;
    /* The server drops any file handle we don't close when the channel
     * eventually goes away, so don't queue a close we'd have to track. */
    g->handle = NULL;
    sg_tasks_free(g);
    sg_set_busy(g, false);
    sg_status(g, why);
    sg_remote_refresh(g);
    sg_local_refresh(g);
}

/* ----------------------------------------------------------------------
 * Download: remote file -> local file.
 */

static bool sg_write_at(HANDLE h, const char *buf, DWORD len, uint64_t offset)
{
    LARGE_INTEGER li;
    DWORD written = 0;

    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return false;
    if (!WriteFile(h, buf, len, &written, NULL))
        return false;
    return written == len;
}

static void sg_dl_check_done(SftpGui *g)
{
    struct sftp_request *req;

    if (!g->at_eof || g->inflight > 0)
        return;
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    g->pend = SG_P_CLOSE_FILE;
    req = fxp_close_send(g->handle);
    g->handle = NULL;
    sftp_register(req);
}

static void sg_dl_pump(SftpGui *g)
{
    while (!g->at_eof && g->inflight < SG_INFLIGHT) {
        struct SgReqCtx *ctx = snew(struct SgReqCtx);
        struct sftp_request *req;

        ctx->tag = SG_TAG_READ;
        ctx->g = g;
        ctx->offset = g->next_offset;
        ctx->len = SG_CHUNK;
        ctx->buf = snewn(SG_CHUNK, char);

        req = fxp_read_send(g->handle, ctx->offset, ctx->len);
        fxp_set_userdata(req, ctx);
        sftp_register(req);

        g->next_offset += SG_CHUNK;
        g->inflight++;
    }
    sg_dl_check_done(g);
}

static void sg_dl_begin(SftpGui *g, const char *remote)
{
    struct sftp_request *req;

    g->lfh = INVALID_HANDLE_VALUE;
    g->inflight = 0;
    g->at_eof = false;
    g->next_offset = 0;
    g->dl_end_known = false;
    g->dl_end = 0;

    g->pend = SG_P_OPEN_DOWNLOAD;
    req = fxp_open_send(remote, SSH_FXF_READ, NULL);
    sftp_register(req);
}

/* ----------------------------------------------------------------------
 * Upload: local file -> remote file.
 */

static void sg_ul_check_done(SftpGui *g)
{
    struct sftp_request *req;

    if (!g->at_eof || g->inflight > 0)
        return;
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    g->pend = SG_P_CLOSE_FILE;
    req = fxp_close_send(g->handle);
    g->handle = NULL;
    sftp_register(req);
}

static void sg_ul_pump(SftpGui *g)
{
    while (!g->at_eof && g->inflight < SG_INFLIGHT) {
        char *buf = snewn(SG_CHUNK, char);
        DWORD got = 0;
        struct SgReqCtx *ctx;
        struct sftp_request *req;

        if (!ReadFile(g->lfh, buf, SG_CHUNK, &got, NULL)) {
            sfree(buf);
            sg_xfer_fail(g, "读取本地文件失败");
            return;
        }
        if (got == 0) {
            sfree(buf);
            g->at_eof = true;
            break;
        }

        ctx = snew(struct SgReqCtx);
        ctx->tag = SG_TAG_WRITE;
        ctx->g = g;
        ctx->offset = g->next_offset;
        ctx->len = (int)got;
        ctx->buf = NULL;

        req = fxp_write_send(g->handle, buf, ctx->offset, ctx->len);
        fxp_set_userdata(req, ctx);
        sftp_register(req);
        sfree(buf);                    /* fxp_write_send copied it */

        g->next_offset += got;
        g->inflight++;
    }
    sg_ul_check_done(g);
}

static void sg_ul_begin(SftpGui *g, const char *local, const char *remote)
{
    struct sftp_request *req;
    wchar_t *wpath = sg_w(local);
    LARGE_INTEGER sz;

    g->inflight = 0;
    g->at_eof = false;
    g->next_offset = 0;

    g->lfh = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    sfree(wpath);
    if (g->lfh == INVALID_HANDLE_VALUE) {
        sg_xfer_fail(g, "无法打开本地文件");
        return;
    }
    if (!GetFileSizeEx(g->lfh, &sz)) {
        sg_xfer_fail(g, "无法获取本地文件大小");
        return;
    }
    /* Sizes were already counted when the task was queued. */
    g->cur_size = (uint64_t)sz.QuadPart;

    g->pend = SG_P_OPEN_UPLOAD;
    req = fxp_open_send(remote,
                        SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC, NULL);
    sftp_register(req);
}

/*
 * Queue everything inside a local directory, for an upload. Local
 * directories can be read synchronously, so the whole subtree goes on
 * the work list at once.
 */
static void sg_local_enqueue_tree(SftpGui *g, const char *local,
                                  const char *remote)
{
    struct SgEntry *ents = NULL;
    int n = 0;
    char *err = sg_local_list(local, &ents, &n);

    if (err) {
        sg_status(g, err);
        sfree(err);
        return;
    }
    for (int i = 0; i < n; i++) {
        char *l, *r;

        if (!strcmp(ents[i].name, ".."))
            continue;
        l = sg_join(local, ents[i].name, '\\');
        r = sg_join(remote, ents[i].name, '/');
        if (ents[i].is_dir) {
            if (!ents[i].reparse)
                sg_task_push(g, SG_T_LDIR, l, r, 0);
        } else {
            sg_task_push(g, SG_T_FILE, l, r, ents[i].size);
            g->total_bytes += ents[i].size;
        }
        sfree(l);
        sfree(r);
    }
    sg_entries_free(ents, n);
}

/* Upload side of a directory task: create the remote directory, and
 * queue its contents when the server confirms it exists. */
static void sg_ul_dir_begin(SftpGui *g, const char *local, const char *remote)
{
    struct sftp_request *req;

    sfree(g->walk_local);
    sfree(g->walk_remote);
    g->walk_local = dupstr(local);
    g->walk_remote = dupstr(remote);

    g->pend = SG_P_TASKMKDIR;
    req = fxp_mkdir_send(remote, NULL);
    sftp_register(req);
}

/* Download side of a directory task: make the local directory (which is
 * allowed to fail if it already exists) and start listing the remote
 * one. */
static void sg_dl_dir_begin(SftpGui *g, const char *local, const char *remote)
{
    struct sftp_request *req;
    wchar_t *w = sg_w(local);

    CreateDirectoryW(w, NULL);
    sfree(w);

    sfree(g->walk_local);
    sfree(g->walk_remote);
    g->walk_local = dupstr(local);
    g->walk_remote = dupstr(remote);

    g->pend = SG_P_TASKOPENDIR;
    req = fxp_opendir_send(remote);
    sftp_register(req);
}

/* Start the next task, or wind the batch up. */
static void sg_xfer_next(SftpGui *g)
{
    while (g->task_head < g->ntasks) {
        struct SgTask *t = &g->tasks[g->task_head];

        if (g->cancel) {
            sg_xfer_finish(g, i18n_or("Transfer cancelled."));
            return;
        }
        g->task_head++;

        switch (t->kind) {
          case SG_T_FILE:
            sfree(g->cur_local);
            sfree(g->cur_remote);
            g->cur_local = dupstr(t->local);
            g->cur_remote = dupstr(t->remote);
            g->cur_size = t->size;
            sg_statusf(g, g->uploading ? "正在上传 %s" : "正在下载 %s",
                       t->remote);
            if (g->uploading)
                sg_ul_begin(g, t->local, t->remote);
            else
                sg_dl_begin(g, t->remote);
            return;

          case SG_T_LDIR:
            sg_ul_dir_begin(g, t->local, t->remote);
            return;

          case SG_T_RDIR:
            sg_dl_dir_begin(g, t->local, t->remote);
            return;
        }
    }
    sg_xfer_finish(g, i18n_or("Transfer finished."));
}

static void sg_start_batch(SftpGui *g, bool uploading)
{
    g->uploading = uploading;
    g->cancel = false;
    g->done_bytes = 0;
    g->total_bytes = 0;
    g->any_error = false;
    SendMessageW(g->prog, PBM_SETPOS, 0, 0);
    sg_set_busy(g, true);
    sg_xfer_next(g);
}

/* ----------------------------------------------------------------------
 * SFTP packet dispatch.
 */

static void sg_send_init(SftpGui *g)
{
    struct sftp_packet *pkt = sftp_pkt_init(SSH_FXP_INIT);

    put_uint32(pkt, SFTP_PROTO_VERSION);
    sftp_send_prepare(pkt);
    sftp_senddata(pkt->data, pkt->length);
    sftp_pkt_free(pkt);
    g->pend = SG_P_VERSION;
}

static void sg_got_version(SftpGui *g, struct sftp_packet *pktin)
{
    unsigned long ver = get_uint32(pktin);
    bool bad = get_err(pktin);

    sftp_pkt_free(pktin);
    if (bad || ver > SFTP_PROTO_VERSION) {
        sg_status(g, "服务器不支持所需的 SFTP 协议版本");
        return;
    }
    sg_status(g, i18n_or("Connected."));
    g->pend = SG_P_NONE;
    g->subsys_ok = true;
    /* Start where the user's login puts them. */
    sg_remote_goto(g, ".");
}

static void sg_handle_read_reply(SftpGui *g, struct sftp_request *req,
                                 struct sftp_packet *pktin)
{
    struct SgReqCtx *ctx = (struct SgReqCtx *)fxp_get_userdata(req);
    int n = fxp_read_recv(pktin, req, ctx->buf, ctx->len);

    g->inflight--;
    if (g->pend != SG_P_DOWNLOAD) {
        /* The transfer was abandoned while this request was airborne. */
        sfree(ctx->buf);
        sfree(ctx);
        return;
    }
    if (n >= 0) {
        /*
         * A short read marks the real end of the file. Any data at or
         * beyond that point would leave a hole in the copy, because the
         * requests after it were queued with fixed offsets.
         */
        if (n > 0 && g->dl_end_known &&
            ctx->offset + (uint64_t)n > g->dl_end) {
            sfree(ctx->buf);
            sfree(ctx);
            sg_xfer_fail(g, "远端文件读取不完整（中途短读）");
            return;
        }
        if (n < ctx->len) {
            uint64_t end = ctx->offset + (uint64_t)n;
            if (!g->dl_end_known || end < g->dl_end) {
                g->dl_end = end;
                g->dl_end_known = true;
            }
        }
    }

    if (n < 0) {
        if (fxp_error_type() != SSH_FX_EOF) {
            sfree(ctx->buf);
            sfree(ctx);
            sg_xfer_fail(g, "从远端读取文件失败");
            return;
        }
        g->at_eof = true;
    } else if (n == 0) {
        g->at_eof = true;
    } else if (g->lfh != INVALID_HANDLE_VALUE) {
        if (!sg_write_at(g->lfh, ctx->buf, (DWORD)n, ctx->offset)) {
            sfree(ctx->buf);
            sfree(ctx);
            sg_xfer_fail(g, "写入本地文件失败");
            return;
        }
        g->done_bytes += n;
        sg_progress(g);
    }
    sfree(ctx->buf);
    sfree(ctx);

    if (g->cancel) {
        g->at_eof = true;
        sg_dl_check_done(g);
        return;
    }
    sg_dl_pump(g);
}

static void sg_handle_write_reply(SftpGui *g, struct sftp_request *req,
                                  struct sftp_packet *pktin)
{
    struct SgReqCtx *ctx = (struct SgReqCtx *)fxp_get_userdata(req);
    bool ok = fxp_write_recv(pktin, req);

    g->inflight--;
    if (g->pend != SG_P_UPLOAD) {
        /* The transfer was abandoned while this request was airborne. */
        sfree(ctx);
        return;
    }
    if (!ok) {
        sfree(ctx);
        sg_xfer_fail(g, "写入远端文件失败");
        return;
    }
    g->done_bytes += ctx->len;
    sfree(ctx);
    sg_progress(g);

    if (g->cancel) {
        g->at_eof = true;
        sg_ul_check_done(g);
        return;
    }
    sg_ul_pump(g);
}

static void sg_close_dir(SftpGui *g)
{
    struct fxp_handle *h = g->dirhandle;
    struct sftp_request *req;

    g->dirhandle = NULL;
    g->pend = SG_P_CLOSE_DIR;
    req = fxp_close_send(h);
    sftp_register(req);
}

static void sg_handle_packet(SftpGui *g, struct sftp_packet *pktin)
{
    struct sftp_request *req;

    if (pktin->type == SSH_FXP_VERSION) {
        sg_got_version(g, pktin);
        return;
    }

    req = sftp_find_request(pktin);
    if (!req) {
        sftp_pkt_free(pktin);
        sg_xfer_fail(g, "收到无法识别的 SFTP 响应");
        return;
    }

    if (fxp_get_userdata(req)) {
        int tag = ((struct SgReqCtx *)fxp_get_userdata(req))->tag;
        if (tag == SG_TAG_READ)
            sg_handle_read_reply(g, req, pktin);
        else
            sg_handle_write_reply(g, req, pktin);
        return;
    }

    switch (g->pend) {
      case SG_P_REALPATH: {
        char *path = fxp_realpath_recv(pktin, req);
        g->pend = SG_P_NONE;
        if (!path) {
            sg_statusf(g, "%s: %s", g->target_path, fxp_error());
            sg_remote_refresh(g);
            break;
        }
        sfree(g->rmt.cwd);
        g->rmt.cwd = path;
        sg_remote_refresh(g);
        break;
      }

      case SG_P_OPENDIR: {
        struct fxp_handle *h = fxp_opendir_recv(pktin, req);
        if (!h) {
            sg_statusf(g, "%s: %s", g->rmt.cwd, fxp_error());
            g->pend = SG_P_NONE;
            break;
        }
        g->dirhandle = h;
        sg_remote_send_readdir(g);
        break;
      }

      case SG_P_READDIR: {
        struct fxp_names *names = fxp_readdir_recv(pktin, req);
        if (!names) {
            if (fxp_error_type() != SSH_FX_EOF)
                sg_statusf(g, "%s: %s", g->rmt.cwd, fxp_error());
            sg_close_dir(g);
            break;
        }
        for (int i = 0; i < names->nnames; i++) {
            struct fxp_name *fn = &names->names[i];
            bool isdir =
                ((fn->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
                 (fn->attrs.permissions & 0170000) == PERMS_DIRECTORY);

            if (!strcmp(fn->filename, ".") || !strcmp(fn->filename, ".."))
                continue;
            sg_entry_push(&g->rents, &g->nrents, &g->rents_size,
                          fn->filename, NULL,
                          (fn->attrs.flags & SSH_FILEXFER_ATTR_SIZE) ?
                          fn->attrs.size : 0,
                          (fn->attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) ?
                          (time_t)fn->attrs.mtime : 0,
                          isdir);
        }
        fxp_free_names(names);
        sg_remote_send_readdir(g);
        break;
      }

      case SG_P_CLOSE_DIR:
        fxp_close_recv(pktin, req);
        g->pend = SG_P_NONE;
        sg_remote_fill(g);
        break;

      case SG_P_OPEN_DOWNLOAD: {
        struct fxp_handle *h = fxp_open_recv(pktin, req);
        wchar_t *wpath;

        g->pend = SG_P_NONE;
        if (!h) {
            sg_statusf(g, "%s: %s", g->cur_remote, fxp_error());
            sg_xfer_next(g);
            break;
        }
        g->handle = h;
        wpath = sg_w(g->cur_local);
        g->lfh = CreateFileW(wpath, GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        sfree(wpath);
        if (g->lfh == INVALID_HANDLE_VALUE) {
            struct sftp_request *creq;
            sg_status(g, "无法创建本地文件");
            creq = fxp_close_send(h);
            g->handle = NULL;
            g->pend = SG_P_CLOSE_FILE;
            sftp_register(creq);
            break;
        }
        g->pend = SG_P_DOWNLOAD;
        g->inflight = 0;
        g->at_eof = false;
        g->next_offset = 0;
        sg_dl_pump(g);
        break;
      }

      case SG_P_OPEN_UPLOAD: {
        struct fxp_handle *h = fxp_open_recv(pktin, req);
        g->pend = SG_P_NONE;
        if (!h) {
            sg_xfer_fail(g, "无法在远端创建文件");
            break;
        }
        g->handle = h;
        g->pend = SG_P_UPLOAD;
        g->inflight = 0;
        g->at_eof = false;
        g->next_offset = 0;
        sg_ul_pump(g);
        break;
      }

      case SG_P_CLOSE_FILE:
        fxp_close_recv(pktin, req);
        g->pend = SG_P_NONE;
        sg_xfer_next(g);
        break;

      case SG_P_TASKMKDIR: {
        /* A directory that already exists is not an error here: we just
         * merge into it, the way every other file manager does. */
        fxp_mkdir_recv(pktin, req);
        g->pend = SG_P_NONE;
        sg_local_enqueue_tree(g, g->walk_local, g->walk_remote);
        sg_xfer_next(g);
        break;
      }

      case SG_P_TASKOPENDIR: {
        struct fxp_handle *h = fxp_opendir_recv(pktin, req);
        if (!h) {
            sg_statusf(g, "%s: %s", g->walk_remote, fxp_error());
            g->pend = SG_P_NONE;
            sg_xfer_next(g);
            break;
        }
        g->walk_handle = h;
        g->pend = SG_P_TASKREADDIR;
        sftp_register(fxp_readdir_send(h));
        break;
      }

      case SG_P_TASKREADDIR: {
        struct fxp_names *names = fxp_readdir_recv(pktin, req);
        if (!names) {
            bool eof = (fxp_error_type() == SSH_FX_EOF);
            struct fxp_handle *h = g->walk_handle;
            if (!eof)
                sg_statusf(g, "%s: %s", g->walk_remote, fxp_error());
            g->walk_handle = NULL;
            g->pend = SG_P_TASKCLOSEDIR;
            sftp_register(fxp_close_send(h));
            break;
        }
        for (int i = 0; i < names->nnames; i++) {
            struct fxp_name *fn = &names->names[i];
            bool isdir =
                ((fn->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
                 (fn->attrs.permissions & 0170000) == PERMS_DIRECTORY);
            char *l, *r;

            if (!strcmp(fn->filename, ".") || !strcmp(fn->filename, ".."))
                continue;
            l = sg_join(g->walk_local, fn->filename, '\\');
            r = sg_join(g->walk_remote, fn->filename, '/');
            if (isdir) {
                sg_task_push(g, SG_T_RDIR, l, r, 0);
            } else {
                uint64_t sz = (fn->attrs.flags & SSH_FILEXFER_ATTR_SIZE) ?
                    fn->attrs.size : 0;
                sg_task_push(g, SG_T_FILE, l, r, sz);
                g->total_bytes += sz;
            }
            sfree(l);
            sfree(r);
        }
        fxp_free_names(names);
        sftp_register(fxp_readdir_send(g->walk_handle));
        break;
      }

      case SG_P_TASKCLOSEDIR:
        fxp_close_recv(pktin, req);
        g->pend = SG_P_NONE;
        sg_xfer_next(g);
        break;

      default:
        sfree(req);
        sftp_pkt_free(pktin);
        break;
    }
}

static void sg_process_inbuf(SftpGui *g)
{
    while (g->inlen >= 4) {
        unsigned long pktlen = GET_32BIT_MSB_FIRST(g->inbuf);
        struct sftp_packet *pkt;

        if (pktlen == 0 || pktlen > (1 << 20)) {
            sg_xfer_fail(g, "SFTP 数据包长度非法");
            return;
        }
        if (g->inlen < 4 + pktlen)
            break;                     /* wait for the rest of it */

        pkt = sftp_recv_prepare(pktlen);
        memcpy(pkt->data, g->inbuf + 4, pktlen);
        if (!sftp_recv_finish(pkt)) {
            sftp_pkt_free(pkt);
            sg_xfer_fail(g, "SFTP 数据包格式错误");
            return;
        }

        memmove(g->inbuf, g->inbuf + 4 + pktlen, g->inlen - 4 - pktlen);
        g->inlen -= 4 + pktlen;

        sg_handle_packet(g, pkt);
    }
}

/* ----------------------------------------------------------------------
 * Commands from the buttons.
 */

static void sg_fatal(SftpGui *g, const char *msg)
{
    if (g->fatal_shown)
        return;                         /* keep the first, more useful one */
    g->fatal_shown = true;
    g->subsys_ok = false;
    g->chan_open = false;
    g->pend = SG_P_NONE;
    g->inflight = 0;
    g->at_eof = false;
    g->handle = NULL;
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    sg_tasks_free(g);
    g->busy = false;
    EnableWindow(g->btn_upload, FALSE);
    EnableWindow(g->btn_download, FALSE);
    EnableWindow(g->btn_cancel, FALSE);
    EnableWindow(g->rmt.up, FALSE);
    EnableWindow(g->lcl.up, FALSE);
    sg_status(g, msg);
}

static void sg_list_activate(SftpGui *g, bool is_local)
{
    HWND lv = is_local ? g->lcl.list : g->rmt.list;
    struct SgEntry *ents = is_local ? g->lcl.ents : g->rmt.ents;
    int n = is_local ? g->lcl.nents : g->rmt.nents;
    const char *cwd = is_local ? g->lcl.cwd : g->rmt.cwd;
    char sep = is_local ? '\\' : '/';
    int idx = sg_lv_selected(lv);
    char *np;

    if (idx < 0 || idx >= n || !ents[idx].is_dir)
        return;

    if (!strcmp(ents[idx].name, ".."))
        np = is_local ? sg_parent_local(cwd) : sg_parent_remote(cwd);
    else
        np = sg_join(cwd, ents[idx].name, sep);

    if (is_local)
        sg_local_goto(g, np);
    else
        sg_remote_goto(g, np);
    sfree(np);
}

/*
 * A transfer can only start when nothing else is outstanding: a
 * half-finished directory listing would otherwise be mistaken for a
 * reply to one of the transfer's own requests.
 */
static bool sg_can_start(SftpGui *g)
{
    if (!g->subsys_ok) {
        sg_msgbox(g->hwnd, "SFTP 尚未连接", MB_ICONWARNING | MB_OK);
        return false;
    }
    if (g->busy || g->pend != SG_P_NONE) {
        sg_status(g, i18n_or("Please wait for the current operation "
                             "to finish."));
        return false;
    }
    return true;
}

/*
 * Queue the current selection of one pane. Directories become tree
 * tasks, so a whole folder goes across with its contents.
 */
static void sg_enqueue_selection(SftpGui *g, bool from_local)
{
    HWND lv = from_local ? g->lcl.list : g->rmt.list;
    struct SgEntry *ents = from_local ? g->lcl.ents : g->rmt.ents;
    int nents = from_local ? g->lcl.nents : g->rmt.nents;
    const char *src = from_local ? g->lcl.cwd : g->rmt.cwd;
    const char *dst = from_local ? g->rmt.cwd : g->lcl.cwd;
    char srcsep = from_local ? '\\' : '/';
    char dstsep = from_local ? '/' : '\\';
    int *idxs = NULL;
    int n = sg_lv_collect(lv, &idxs);
    int count = 0;

    for (int i = 0; i < n; i++) {
        struct SgEntry *e;
        char *s, *d;

        if (idxs[i] < 0 || idxs[i] >= nents)
            continue;
        e = &ents[idxs[i]];
        if (!strcmp(e->name, ".."))
            continue;
        if (from_local && e->is_dir && e->reparse)
            continue;           /* junction or symlink: not followed */

        s = sg_join(src, e->name, srcsep);
        d = sg_join(dst, e->name, dstsep);
        if (e->is_dir) {
            sg_task_push(g, from_local ? SG_T_LDIR : SG_T_RDIR, s, d, 0);
        } else {
            sg_task_push(g, SG_T_FILE, s, d, e->size);
            g->total_bytes += e->size;
        }
        sfree(s);
        sfree(d);
        count++;
    }
    sfree(idxs);

    if (count == 0) {
        sg_status(g, i18n_or("Select the files to transfer first."));
        return;
    }
    sg_start_batch(g, from_local);
}

static void sg_do_upload(SftpGui *g)
{
    if (!sg_can_start(g))
        return;
    sg_tasks_free(g);
    sg_enqueue_selection(g, true);
}

static void sg_do_download(SftpGui *g)
{
    if (!sg_can_start(g))
        return;
    sg_tasks_free(g);
    sg_enqueue_selection(g, false);
}

static void sg_do_cancel(SftpGui *g)
{
    if (!g->busy)
        return;
    /*
     * We cannot retract requests already on the wire, so stop feeding
     * new ones and let the replies drain; the batch winds itself up as
     * soon as nothing is in flight.
     */
    g->cancel = true;
    sg_status(g, i18n_or("Cancelling..."));
}

/* ----------------------------------------------------------------------
 * Window plumbing.
 */

static WNDPROC sg_edit_origproc;

static void sg_edit_enter(HWND hwnd)
{
    SftpGui *g = sg_instance;
    wchar_t buf[1024];
    char *path;

    if (!g)
        return;
    GetWindowTextW(hwnd, buf, lenof(buf));
    path = sg_to_utf8(buf);
    if (hwnd == g->lcl.path)
        sg_local_goto(g, path);
    else if (hwnd == g->rmt.path)
        sg_remote_goto(g, path);
    sfree(path);
}

static LRESULT CALLBACK sg_edit_proc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        sg_edit_enter(hwnd);
        return 0;
    }
    if (msg == WM_CHAR && (wParam == '\r' || wParam == '\n'))
        return 0;
    return CallWindowProcW(sg_edit_origproc, hwnd, msg, wParam, lParam);
}

static HWND sg_child(SftpGui *g, const wchar_t *cls, const char *text,
                     DWORD style, DWORD exstyle, int id)
{
    wchar_t *wtext = sg_w(text ? i18n_or(text) : "");
    HWND h = CreateWindowExW(exstyle, cls, wtext,
                             WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, g->hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);

    SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    sfree(wtext);
    return h;
}

static void sg_lv_columns(HWND lv)
{
    static const char *const titles[3] = { "Name", "Size", "Modified" };
    LVCOLUMNW col;

    for (int i = 0; i < 3; i++) {
        wchar_t *w = sg_w(i18n_or(titles[i]));
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = (i == 0 ? 200 : i == 1 ? 76 : 128);
        col.pszText = w;
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
        sfree(w);
    }
}

static void sg_create_controls(SftpGui *g)
{
    g->lcl.label = sg_child(g, L"STATIC", "Local", SS_LEFT, 0, SG_ID_L_LABEL);
    g->lcl.path = sg_child(g, L"EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 0,
                           SG_ID_L_PATH);
    sg_edit_origproc = (WNDPROC)SetWindowLongPtrW(
        g->lcl.path, GWLP_WNDPROC, (LONG_PTR)sg_edit_proc);
    g->lcl.up = sg_child(g, L"BUTTON", "..", BS_PUSHBUTTON, 0, SG_ID_L_UP);
    g->lcl.list = sg_child(g, WC_LISTVIEWW, "",
                           LVS_REPORT | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE,
                           SG_ID_L_LIST);

    g->rmt.label = sg_child(g, L"STATIC", "Remote", SS_LEFT, 0, SG_ID_R_LABEL);
    g->rmt.path = sg_child(g, L"EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 0,
                           SG_ID_R_PATH);
    sg_edit_origproc = (WNDPROC)SetWindowLongPtrW(
        g->rmt.path, GWLP_WNDPROC, (LONG_PTR)sg_edit_proc);
    g->rmt.up = sg_child(g, L"BUTTON", "..", BS_PUSHBUTTON, 0, SG_ID_R_UP);
    g->rmt.list = sg_child(g, WC_LISTVIEWW, "",
                           LVS_REPORT | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE,
                           SG_ID_R_LIST);

    ListView_SetExtendedListViewStyle(
        g->lcl.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetExtendedListViewStyle(
        g->rmt.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    sg_lv_columns(g->lcl.list);
    sg_lv_columns(g->rmt.list);

    g->btn_upload = sg_child(g, L"BUTTON", "Upload ->", BS_PUSHBUTTON, 0,
                             SG_ID_UPLOAD);
    g->btn_download = sg_child(g, L"BUTTON", "<- Download", BS_PUSHBUTTON, 0,
                               SG_ID_DOWNLOAD);
    g->btn_cancel = sg_child(g, L"BUTTON", "Cancel Transfer", BS_PUSHBUTTON, 0,
                             SG_ID_CANCEL);

    g->prog = sg_child(g, PROGRESS_CLASSW, "", 0, 0, SG_ID_PROGRESS);
    SendMessageW(g->prog, PBM_SETRANGE32, 0, 1000);

    g->status = sg_child(g, L"STATIC", "", SS_LEFT | SS_ENDELLIPSIS, 0,
                         SG_ID_STATUS);

    EnableWindow(g->btn_cancel, FALSE);
}

static void sg_layout(SftpGui *g)
{
    RECT rc;
    int W, H, m = 8, gap = 6, row = 24, midw = 116, prog_h = 16;
    int pane_w, left_x, right_x, list_y, list_h, prog_y, status_y, bx, by;
    int namew, total_w;
    int i;

    GetClientRect(g->hwnd, &rc);
    W = rc.right;
    H = rc.bottom;

    pane_w = (W - 2 * m - midw - 2 * gap) / 2;
    if (pane_w < 120)
        pane_w = 120;
    left_x = m;
    right_x = m + pane_w + gap + midw + gap;
    total_w = right_x + pane_w - left_x;

    status_y = H - m - row;
    prog_y = status_y - gap - prog_h;
    list_y = m + row + gap;
    list_h = prog_y - gap - list_y;
    if (list_h < 60)
        list_h = 60;

    sg_place(g->lcl.label, left_x, m, 46, row);
    sg_place(g->lcl.path, left_x + 48, m, pane_w - 48 - 30, row);
    sg_place(g->lcl.up, left_x + pane_w - 28, m, 28, row);
    sg_place(g->lcl.list, left_x, list_y, pane_w, list_h);

    sg_place(g->rmt.label, right_x, m, 46, row);
    sg_place(g->rmt.path, right_x + 48, m, pane_w - 48 - 30, row);
    sg_place(g->rmt.up, right_x + pane_w - 28, m, 28, row);
    sg_place(g->rmt.list, right_x, list_y, pane_w, list_h);

    bx = m + pane_w + gap;
    by = list_y + 8;
    sg_place(g->btn_upload, bx, by, midw, row);
    by += row + gap;
    sg_place(g->btn_download, bx, by, midw, row);
    by += row + gap * 2;
    sg_place(g->btn_cancel, bx, by, midw, row);

    sg_place(g->prog, left_x, prog_y, total_w, prog_h);
    sg_place(g->status, left_x, status_y, total_w, row);

    namew = pane_w - 76 - 128;
    if (namew < 60)
        namew = 60;
    for (i = 0; i < 2; i++) {
        HWND lv = i ? g->rmt.list : g->lcl.list;
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(namew, 0));
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, 1, MAKELPARAM(76, 0));
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, 2, MAKELPARAM(128, 0));
    }
}

/* Free everything a SftpGui owns; used on a failed start and at destroy. */
static void sg_free(SftpGui *g, bool channel_still_ours)
{
    if (channel_still_ours && g->schan) {
        SshChannel *sc = g->sc;
        g->schan->g = NULL;
        g->schan = NULL;
        g->sc = NULL;
        if (sc)
            sshfwd_initiate_close(sc, NULL);
    }
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    sg_tasks_free(g);
    sftp_cleanup_request();
    sg_entries_free(g->lcl.ents, g->lcl.nents);
    sg_entries_free(g->rmt.ents, g->rmt.nents);
    sg_entries_free(g->rents, g->nrents);
    sfree(g->lcl.cwd);
    sfree(g->rmt.cwd);
    sfree(g->target_path);
    sfree(g->inbuf);
    sfree(g);
}

static SftpGui *sg_create(HWND hwnd, Backend *be)
{
    SftpGui *g = snew(SftpGui);
    wchar_t wcwd[MAX_PATH];
    struct SgChan *c;

    memset(g, 0, sizeof(*g));
    g->hwnd = hwnd;
    g->lfh = INVALID_HANDLE_VALUE;
    g->pend = SG_P_NONE;
    g->rmt.cwd = dupstr("/");

    sg_create_controls(g);

    wcwd[0] = L'\0';
    if (GetCurrentDirectoryW(lenof(wcwd), wcwd))
        g->lcl.cwd = sg_to_utf8(wcwd);
    if (!g->lcl.cwd)
        g->lcl.cwd = dupstr("C:\\");
    size_t n = strlen(g->lcl.cwd);
    if (n > 3 && g->lcl.cwd[n-1] == '\\')
        g->lcl.cwd[n-1] = '\0';

    sg_local_refresh(g);

    c = snew(struct SgChan);
    c->chan.vt = &SgChan_vt;
    c->chan.initial_fixed_window_size = 0;
    c->g = g;
    c->sc = NULL;
    g->schan = c;

    c->sc = ssh_open_session_channel(be, &c->chan);
    if (!c->sc) {
        g->schan = NULL;
        sfree(c);
        sg_startup_error =
            "无法在该会话上打开 SFTP 通道。"
            "仅支持已完成登录的 SSH-2 会话。";
        sg_free(g, false);
        return NULL;
    }
    g->sc = c->sc;

    sg_instance = g;
    sg_layout(g);
    sg_status(g, i18n_or("Connecting..."));
    return g;
}

static LRESULT CALLBACK SgWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam)
{
    SftpGui *g = (SftpGui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SftpGui *ng = sg_create(hwnd, (Backend *)cs->lpCreateParams);
        if (!ng)
            return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ng);
        return 0;
      }

      case WM_SIZE:
        if (g)
            sg_layout(g);
        return 0;

      case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 560;
        mmi->ptMinTrackSize.y = 340;
        return 0;
      }

      case WM_SG_NOTIFY:
        if (!g)
            return 0;
        switch (wParam) {
          case SG_CH_SUBSYS_OK:
            sg_send_init(g);
            break;
          case SG_CH_SUBSYS_BAD:
            sg_fatal(g, "服务器拒绝启动 SFTP 子系统");
            break;
          case SG_CH_OPENFAILED: {
            char *text = (char *)lParam;
            sg_fatal(g, text);
            sfree(text);
            break;
          }
          case SG_CH_LOST:
          default:
            sg_fatal(g, "SFTP 通道已关闭");
            break;
        }
        return 0;

      case WM_COMMAND:
        if (!g)
            break;
        switch (LOWORD(wParam)) {
          case SG_ID_UPLOAD:
            sg_do_upload(g);
            return 0;
          case SG_ID_DOWNLOAD:
            sg_do_download(g);
            return 0;
          case SG_ID_CANCEL:
            sg_do_cancel(g);
            return 0;
          case SG_ID_L_UP: {
            char *p = sg_parent_local(g->lcl.cwd);
            sg_local_goto(g, p);
            sfree(p);
            return 0;
          }
          case SG_ID_R_UP: {
            char *p = sg_parent_remote(g->rmt.cwd);
            sg_remote_goto(g, p);
            sfree(p);
            return 0;
          }
        }
        break;

      case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        if (!g)
            break;
        if (nm->code == NM_DBLCLK) {
            if (nm->hwndFrom == g->lcl.list)
                sg_list_activate(g, true);
            else if (nm->hwndFrom == g->rmt.list)
                sg_list_activate(g, false);
            return 0;
        }
        break;
      }

      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

      case WM_DESTROY:
        if (g) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (sg_instance == g)
                sg_instance = NULL;
            sg_free(g, true);
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void sftp_gui_open(HWND parent, Backend *be)
{
    static bool registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    HWND hwnd;
    wchar_t *title;

    if (sg_instance) {
        ShowWindow(sg_instance->hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(sg_instance->hwnd);
        return;
    }

    if (!registered) {
        WNDCLASSW wc;

        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = SgWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wc.lpszClassName = SG_CLASSNAME;
        if (!RegisterClassW(&wc))
            return;
        registered = true;
    }

    sg_startup_error = NULL;
    title = sg_w(i18n_or("SFTP File Transfer"));
    hwnd = CreateWindowExW(0, SG_CLASSNAME, title,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 880, 540,
                           parent, NULL, inst, be);
    sfree(title);

    if (!hwnd) {
        sg_msgbox(parent,
                  sg_startup_error ? sg_startup_error :
                  "无法创建 SFTP 窗口", MB_ICONERROR | MB_OK);
        return;
    }
    sg_instance = (SftpGui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
}
