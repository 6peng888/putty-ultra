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
#include <ctype.h>
#include <time.h>
#include <wchar.h>

#include "putty.h"
#include "i18n.h"
#include "ssh.h"
#include "ssh/channel.h"
#include "ssh/sftp.h"
#include "sftpgui.h"
#include "putty-rc.h"          /* IDI_MAINICON: the PuTTY window icon */

#include <commctrl.h>
#include <shellapi.h>          /* SHGetFileInfoW: the system file icons */
#include <shlobj.h>            /* SHGetFolderPathW: the shell's folders */

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
#define SG_ID_PROGTEXT   1132

/* Items of the list views' context menu. */
#define SG_ID_CTX_XFER   1140
#define SG_ID_CTX_DELETE 1141
#define SG_ID_CTX_RENAME 1142

#define SG_CLASSNAME L"PuTTYSftpWindow"

/* Timer that keeps the transfer readout ticking while a batch runs. */
#define SG_TIMER_PROGRESS 1

/*
 * Messages the channel implementation posts to the window. The
 * wParam is one of the SG_CH_* codes below.
 */
#define WM_SG_NOTIFY (WM_APP + 21)

#define SG_CH_LOST        0     /* channel (and so the connection) is gone */
#define SG_CH_OPENFAILED  1     /* lParam is a malloc'd error message */
#define SG_CH_SUBSYS_OK   2     /* the sftp subsystem is running */
#define SG_CH_SUBSYS_BAD  3     /* the server refused the subsystem */
#define SG_CH_EXEC_DONE   4     /* an exec channel has finished */
#define SG_CH_PUMP        5     /* carry on feeding an SCP upload */
#define SG_CH_PICK_LOCAL  6     /* a local drop-down item was chosen */
#define SG_CH_PICK_REMOTE 7     /* a remote drop-down item was chosen */
#define SG_CH_RENAME      8     /* a name has been typed into a label edit */
#define SG_CH_RENAME_EDIT 9     /* start editing a row's name in place */

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
    /* Context menu: deleting remote entries, one directory at a time. */
    SG_P_DEL_OPENDIR,
    SG_P_DEL_READDIR,
    SG_P_DEL_CLOSEDIR,
    SG_P_DEL_REMOVE,       /* deleting one remote file */
    SG_P_DEL_RMDIR,        /* deleting one remote directory */
    SG_P_RENAME,           /* renaming a remote entry */
    /* SCP: a one-shot command (directory listing or mkdir) is running */
    SG_P_SCP_EXEC,
    /* SCP: an interactive scp session is in progress */
    SG_P_SCP_UPLOAD,
    SG_P_SCP_DOWNLOAD,
};

/*
 * What an exec channel was started for. Anything that is not an
 * interactive scp session simply accumulates its output and is dealt
 * with when the channel closes.
 */
#define SG_EXEC_LS       1      /* cd + pwd + ls -lA */
#define SG_EXEC_MKDIR    2      /* mkdir -p */
#define SG_EXEC_SCP_UL   3      /* scp -t: we are the source */
#define SG_EXEC_SCP_DL   4      /* scp -f: we are the sink */
#define SG_EXEC_RM       5      /* rm -rf: delete remote entries */
#define SG_EXEC_MV       6      /* mv: rename a remote entry */

/* Which kind of channel a struct SgChan is. */
#define SG_CHAN_SFTP     1
#define SG_CHAN_SENTINEL 2      /* holds a session channel open so that we
                                 * notice the connection dying */
#define SG_CHAN_EXEC     3

/* Sub-states of an interactive scp session started as the source. */
enum {
    SG_SU_BANNER = 0,       /* waiting for the sink's first NUL */
    SG_SU_HDRACK,           /* waiting for the ack of the C record */
    SG_SU_DATA,             /* streaming the file body */
    SG_SU_DATAACK,          /* waiting for the ack after the body */
    SG_SU_FINAL,            /* waiting for the ack of the final NUL */
    /* and as the sink: */
    SG_SD_RECORD,           /* waiting for a control record */
    SG_SD_DATA,             /* receiving file data */
    SG_SD_DATAEND,          /* waiting for the trailing NUL */
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

    /* The path box is an editable combo box with a drop-down history of
     * places worth jumping to (see sg_local_combo_build and
     * sg_remote_combo_build). 'edit' is the combo's real edit control,
     * which is what the keyboard subclass is installed on. */
    HWND edit;
    char **cbpath;              /* the full path behind each drop item */
    int ncbpath, cbcap;
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

/* One remote entry queued for deletion from the context menu. */
struct SgDelItem {
    char *path;
    bool is_dir;
    bool scanned;               /* its children are already in the list */
};

struct SgChan {
    Channel chan;
    SftpGui *g;                 /* NULL once the window has gone away */
    SshChannel *sc;
    int kind;                   /* SG_CHAN_* */
    int exec_kind;              /* SG_EXEC_* when kind == SG_CHAN_EXEC */
    bool interactive;           /* the SCP state machine drives this one */
    bool failed;                /* open or exec request was refused */
    bool got_exit;
    int exit_status;
    char *command;              /* exec command, or subsystem name */
    char *out;                  /* accumulated stdout */
    size_t outlen, outcap;
    char *err;                  /* accumulated stderr */
    size_t errlen, errcap;
};

struct SftpGui {
    HWND hwnd;

    bool scp;                   /* SCP mode instead of SFTP */
    Backend *be;                /* cleared when the session goes away */

    SshChannel *sc;             /* the SFTP subsystem channel */
    struct SgChan *schan;       /* its Channel wrapper */
    bool chan_open, subsys_ok;

    /* SCP: the one exec channel that may be running. */
    struct SgChan *exec_chan;
    int exec_kind;
    bool exec_walk;             /* the listing is part of a transfer, not
                                 * the browser pane */
    bool exec_ok;
    char *exec_out, *exec_err;
    size_t exec_outlen, exec_errlen;

    /*
     * A path picked from a drop-down, waiting to be navigated to once
     * the combo box has finished updating its own edit field.
     */
    char *pick;

    /*
     * A name typed into a list view's label edit. The rename runs from
     * a posted message rather than from the notification itself, so the
     * edit control is gone before the pane is refilled.
     */
    bool ren_local;
    int ren_idx;
    char *ren_name;

    /* SCP: state of the interactive scp session in progress. */
    int scp_state;
    uint64_t scp_sent;
    uint64_t scp_remaining;

    /*
     * SCP: the run of files being moved by the current scp process.
     * One process per directory's worth of files rather than one per
     * file, because starting a remote shell is by far the most
     * expensive part of a transfer.
     */
    struct SgBatchItem { char *local, *remote; uint64_t size; } *batch;
    int nbatch, batch_pos;

    struct SgPane lcl, rmt;     /* local and remote panes */
    HWND btn_upload, btn_download, btn_cancel;
    HWND prog, status;
    HWND progtext;              /* "12.3 MB / 45.6 MB   1.2 MB/s" */

    /* Realtime transfer rate, smoothed over successive samples. */
    double speed;               /* bytes per second */
    uint64_t speed_mark;        /* done_bytes at the last sample */
    DWORD speed_tick;           /* GetTickCount() at the last sample */

    /* Reassembly buffer for incoming SFTP data. */
    char *inbuf;
    size_t inlen, incap;

    /*
     * Context menu: the remote entries being deleted. Parents come
     * before their children in the list, so deleting backwards removes
     * a tree from the bottom up.
     */
    struct SgDelItem *del;
    int ndel, delsize, del_walk, del_done;
    bool del_error;

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
static bool sg_pending_scp;     /* mode for the window being created */

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
static void sg_remote_fill(SftpGui *g);
static void sg_scp_ul_begin(SftpGui *g);
static void sg_scp_dl_begin(SftpGui *g);
static void sg_scp_rx(SftpGui *g, struct SgChan *c);
static void sg_exec_done(SftpGui *g);
static void sg_scp_ul_pump(SftpGui *g);
static void sg_abort_exec(SftpGui *g);
static void sg_batch_free(SftpGui *g);
static void sg_del_run(SftpGui *g);
static void sg_del_free(SftpGui *g);
static void sg_del_push(SftpGui *g, const char *path, bool is_dir);
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

/* The last component of a remote path. */
static char *sg_basename_remote(const char *path)
{
    const char *slash = strrchr(path, '/');

    return dupstr(slash ? slash + 1 : path);
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

/*
 * The system's small-icon list, so the panes show the same folder and
 * file icons as Explorer rather than a plain list of names. Asking
 * shell32 for it once is enough; it is shared by every window in the
 * process and must not be destroyed.
 */
static HIMAGELIST sg_sys_small_icons(void)
{
    static HIMAGELIST list;
    static bool tried;

    if (!tried) {
        SHFILEINFOW sfi;
        tried = true;
        list = (HIMAGELIST)SHGetFileInfoW(
            L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    }
    return list;
}

/*
 * Which icon to use for a name. SHGFI_USEFILEATTRIBUTES makes shell32
 * work from the extension alone, without touching the file system,
 * which is what lets it answer for remote names too.
 */
static int sg_icon_index(const char *name, bool is_dir)
{
    SHFILEINFOW sfi;
    wchar_t *wname = sg_w(name);
    DWORD attr = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    int ret = -1;

    if (SHGetFileInfoW(wname, attr, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON |
                       SHGFI_USEFILEATTRIBUTES))
        ret = sfi.iIcon;
    sfree(wname);
    return ret;
}

static const struct {
    const char *ext;
    const char *desc;
} sg_types[] = {
    { "zip", "压缩文件" },   { "7z", "压缩文件" },
    { "rar", "压缩文件" },   { "tar", "压缩文件" },
    { "gz", "压缩文件" },    { "xz", "压缩文件" },
    { "bz2", "压缩文件" },
    { "exe", "应用程序" },   { "msi", "安装程序" },  { "com", "应用程序" },
    { "bat", "批处理文件" }, { "cmd", "批处理文件" },
    { "dll", "应用程序扩展" }, { "sys", "系统文件" },
    { "txt", "文本文档" },   { "log", "日志文件" },  { "md", "Markdown 文档" },
    { "pdf", "PDF 文档" },
    { "doc", "Word 文档" },  { "docx", "Word 文档" },
    { "xls", "Excel 表格" }, { "xlsx", "Excel 表格" },
    { "ppt", "演示文稿" },   { "pptx", "演示文稿" },
    { "png", "PNG 图片" },   { "jpg", "JPEG 图片" },
    { "jpeg", "JPEG 图片" }, { "gif", "GIF 图片" },
    { "bmp", "BMP 图片" },   { "ico", "图标" },      { "svg", "SVG 图片" },
    { "mp3", "音频文件" },   { "wav", "音频文件" },  { "flac", "音频文件" },
    { "mp4", "视频文件" },   { "avi", "视频文件" },  { "mkv", "视频文件" },
    { "mov", "视频文件" },
    { "c", "C 源文件" },     { "h", "C 头文件" },    { "cpp", "C++ 源文件" },
    { "py", "Python 源文件" }, { "sh", "Shell 脚本" }, { "pl", "Perl 脚本" },
    { "js", "JavaScript 文件" }, { "java", "Java 源文件" }, { "go", "Go 源文件" },
    { "rs", "Rust 源文件" },
    { "html", "HTML 文档" }, { "htm", "HTML 文档" },
    { "xml", "XML 文档" },   { "json", "JSON 文件" },
    { "ini", "配置文件" },   { "conf", "配置文件" }, { "cfg", "配置文件" },
};

/* The text for the "Type" column. Caller frees. */
static char *sg_type_name(const char *name, bool is_dir)
{
    const char *dot, *ext;
    char upper[32];
    size_t i;

    if (!strcmp(name, ".."))
        return dupstr("上级目录");
    if (is_dir)
        return dupstr("文件夹");

    dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1])
        return dupstr("文件");
    ext = dot + 1;

    for (i = 0; i < lenof(sg_types); i++)
        if (!stricmp(ext, sg_types[i].ext))
            return dupstr(sg_types[i].desc);

    /* Unknown extension: show it in the way Explorer does. */
    for (i = 0; i < lenof(upper) - 1 && ext[i]; i++)
        upper[i] = (char)toupper((unsigned char)ext[i]);
    upper[i] = '\0';
    return dupprintf("%s 文件", upper);
}

/* ----------------------------------------------------------------------
 * The path boxes.
 *
 * Each pane's path box is an editable combo box: the text stays a
 * normal path box that can be typed into, but the drop-down lists the
 * places worth jumping to - the shell's folders and the drives on the
 * local side, the components of the current path on the remote side -
 * so a far-away directory is two clicks away instead of a typed path.
 */

static void sg_combo_reset(struct SgPane *p)
{
    for (int i = 0; i < p->ncbpath; i++)
        sfree(p->cbpath[i]);
    p->ncbpath = 0;
    SendMessageW(p->path, CB_RESETCONTENT, 0, 0);
}

static void sg_combo_add(struct SgPane *p, const char *display,
                         const char *path, int icon)
{
    wchar_t *w = sg_w(display);
    int idx = (int)SendMessageW(p->path, CB_ADDSTRING, 0, (LPARAM)w);

    sfree(w);
    if (idx < 0 || idx == CB_ERRSPACE)
        return;
    SendMessageW(p->path, CB_SETITEMDATA, idx, (LPARAM)icon);

    if (p->ncbpath >= p->cbcap) {
        p->cbcap = p->cbcap ? p->cbcap * 2 : 16;
        p->cbpath = sresize(p->cbpath, p->cbcap, char *);
    }
    p->cbpath[p->ncbpath++] = dupstr(path);
}

/* The icon for a path that really exists (a drive or a folder). */
static int sg_icon_for_path(const wchar_t *wpath)
{
    SHFILEINFOW sfi;

    if (SHGetFileInfoW(wpath, 0, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
        return sfi.iIcon;
    return -1;
}

static void sg_local_combo_build(SftpGui *g)
{
    static const struct { int csidl; const char *label; } shell[] = {
        { CSIDL_PERSONAL,         "我的文档" },
        { CSIDL_DESKTOPDIRECTORY, "桌面" },
        { CSIDL_PROFILE,          "用户目录" },
    };
    wchar_t buf[MAX_PATH + 1];
    wchar_t drives[512];

    sg_combo_reset(&g->lcl);

    /* A few of the shell's own folders first, like Explorer's address
     * bar does, since those are what people reach for most often. */
    for (size_t i = 0; i < lenof(shell); i++) {
        if (SHGetFolderPathW(NULL, shell[i].csidl, NULL, 0, buf) == S_OK) {
            char *u = sg_to_utf8(buf);

            sg_combo_add(&g->lcl, shell[i].label, u, sg_icon_for_path(buf));
            sfree(u);
        }
    }

    if (GetLogicalDriveStringsW(lenof(drives) - 1, drives)) {
        for (wchar_t *p = drives; *p; p += wcslen(p) + 1) {
            wchar_t label[MAX_PATH + 1] = L"";
            const char *kind;
            char *u, *disp;
            int icon;

            switch (GetDriveTypeW(p)) {
              case DRIVE_REMOVABLE: kind = "可移动磁盘"; break;
              case DRIVE_FIXED:     kind = "本地磁盘"; break;
              case DRIVE_REMOTE:    kind = "网络驱动器"; break;
              case DRIVE_CDROM:     kind = "光盘驱动器"; break;
              case DRIVE_RAMDISK:   kind = "内存盘"; break;
              default:              kind = "驱动器"; break;
            }

            if (GetVolumeInformationW(p, label, lenof(label) - 1,
                                      NULL, NULL, NULL, NULL, 0) &&
                label[0]) {
                char *ul = sg_to_utf8(label);
                disp = dupprintf("%c: %s (%s)", (char)p[0], kind, ul);
                sfree(ul);
            } else {
                disp = dupprintf("%c: %s", (char)p[0], kind);
            }

            u = sg_to_utf8(p);
            icon = sg_icon_for_path(p);
            sg_combo_add(&g->lcl, disp, u, icon);
            sfree(disp);
            sfree(u);
        }
    }

    /* CB_RESETCONTENT took the text out of the edit field with it. */
    sg_set_text(g->lcl.path, g->lcl.cwd);
}

static void sg_remote_combo_build(SftpGui *g)
{
    const char *cwd = g->rmt.cwd;
    const char *p;
    int icon = sg_icon_index("folder", true);

    sg_combo_reset(&g->rmt);

    /* The root, then one entry per path component, each of which is a
     * complete path in its own right. */
    sg_combo_add(&g->rmt, "/", "/", icon);

    for (p = cwd; *p; ) {
        char *prefix;
        size_t n;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        while (*p && *p != '/')
            p++;

        n = (size_t)(p - cwd);
        prefix = snewn(n + 1, char);
        memcpy(prefix, cwd, n);
        prefix[n] = '\0';
        sg_combo_add(&g->rmt, prefix, prefix, icon);
        sfree(prefix);
    }

    /* Highlight the entry we are currently in, and put the full path
     * back in the edit field afterwards (CB_RESETCONTENT empties it). */
    for (int i = 0; i < g->rmt.ncbpath; i++) {
        if (!strcmp(g->rmt.cbpath[i], cwd)) {
            SendMessageW(g->rmt.path, CB_SETCURSEL, i, 0);
            break;
        }
    }
    sg_set_text(g->rmt.path, g->rmt.cwd);
}

/* Draw one drop-down entry: its icon, then its text. */
static void sg_combo_draw(const DRAWITEMSTRUCT *dis)
{
    wchar_t text[512];
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    HIMAGELIST icons = sg_sys_small_icons();
    COLORREF bk, fg;
    int icon = -1, x;

    /* The selection field of an editable combo keeps its own edit
     * control, which paints (and scrolls) the text itself. */
    if (dis->itemID == (UINT)-1)
        return;

    text[0] = L'\0';
    SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)text);
    icon = (int)SendMessageW(dis->hwndItem, CB_GETITEMDATA, dis->itemID, 0);

    if (dis->itemState & ODS_SELECTED) {
        bk = GetSysColor(COLOR_HIGHLIGHT);
        fg = GetSysColor(COLOR_HIGHLIGHTTEXT);
    } else {
        bk = GetSysColor(COLOR_WINDOW);
        fg = GetSysColor(COLOR_WINDOWTEXT);
    }
    SetBkColor(dc, bk);
    SetTextColor(dc, fg);
    ExtTextOutW(dc, rc.left, rc.top, ETO_OPAQUE, &rc, NULL, 0, NULL);

    x = rc.left + 2;
    if (icons && icon >= 0) {
        ImageList_Draw(icons, icon, dc, x, rc.top + 2, ILD_TRANSPARENT);
        x += 18;
    }
    rc.left = x;
    DrawTextW(dc, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
              DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void sg_lv_fill(HWND lv, struct SgEntry *ents, int n)
{
    LVITEMW item;
    char sizebuf[32], timebuf[32];

    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);

    for (int i = 0; i < n; i++) {
        wchar_t *wname, *wsize, *wtype, *wtime;
        char *type;
        int idx, icon;

        if (ents[i].is_dir)
            sizebuf[0] = '\0';
        else
            sg_fmt_size(sizebuf, sizeof(sizebuf), ents[i].size);
        sg_fmt_time(timebuf, sizeof(timebuf), ents[i].mtime);
        type = sg_type_name(ents[i].name, ents[i].is_dir);

        wname = ents[i].wname ? sg_dupw(ents[i].wname) : sg_w(ents[i].name);
        wsize = sg_w(sizebuf);
        wtype = sg_w(type);
        wtime = sg_w(timebuf);
        icon = sg_icon_index(ents[i].name, ents[i].is_dir);
        sfree(type);

        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = wname;
        item.lParam = (LPARAM)i;
        if (icon >= 0) {
            item.mask |= LVIF_IMAGE;
            item.iImage = icon;
        }
        idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&item);

        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.iSubItem = 1;
        item.pszText = wsize;
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, (LPARAM)&item);
        item.iSubItem = 2;
        item.pszText = wtype;
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, (LPARAM)&item);
        item.iSubItem = 3;
        item.pszText = wtime;
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

        sfree(wname);
        sfree(wsize);
        sfree(wtype);
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
    g->be = NULL;               /* the backend is being torn down */
    PostMessage(g->hwnd, WM_SG_NOTIFY, SG_CH_LOST, 0);
}

static void sg_chan_free(Channel *chan)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);
    SftpGui *g = c->g;

    if (c->kind == SG_CHAN_EXEC) {
        /*
         * The command has finished. Only the one we are still waiting
         * for matters; anything else is a straggler we have already
         * moved on from.
         */
        if (g && g->exec_chan == c) {
            bool ok = !c->failed &&
                (!c->got_exit || c->exit_status == 0);

            g->exec_chan = NULL;
            sfree(g->exec_out);
            sfree(g->exec_err);
            g->exec_out = c->out;
            g->exec_outlen = c->outlen;
            g->exec_err = c->err;
            g->exec_errlen = c->errlen;
            g->exec_ok = ok;
            c->out = c->err = NULL;
            PostMessage(g->hwnd, WM_SG_NOTIFY, SG_CH_EXEC_DONE, 0);
        }
        sfree(c->out);
        sfree(c->err);
        sfree(c->command);
        sfree(c);
        return;
    }

    if (g) {
        g->schan = NULL;
        sg_transport_lost(g);
    }
    sfree(c->command);
    sfree(c);
}

static void sg_chan_open_confirmation(Channel *chan)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);
    SftpGui *g = c->g;

    if (!g)
        return;
    g->chan_open = true;

    switch (c->kind) {
      case SG_CHAN_SFTP:
        /* Now that the channel exists, ask for the subsystem. */
        sshfwd_start_subsystem(c->sc, true, "sftp");
        break;

      case SG_CHAN_SENTINEL:
        /* SCP needs no persistent channel; this one just keeps us
         * informed if the session dies. */
        PostMessage(g->hwnd, WM_SG_NOTIFY, SG_CH_SUBSYS_OK, 0);
        break;

      case SG_CHAN_EXEC:
        sshfwd_start_command(c->sc, true, c->command);
        break;
    }
}

static void sg_chan_open_failed(Channel *chan, const char *errtext)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);
    char *msg;

    if (!c->g)
        return;
    c->failed = true;
    msg = dupprintf("%s: %s", c->kind == SG_CHAN_EXEC ? "SCP" : "SFTP",
                    errtext);
    PostMessage(c->g->hwnd, WM_SG_NOTIFY, SG_CH_OPENFAILED, (LPARAM)msg);
}

static size_t sg_chan_send(Channel *chan, bool is_stderr,
                           const void *data, size_t len)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);
    SftpGui *g = c->g;

    if (!g)
        return len;

    if (c->kind != SG_CHAN_SFTP) {
        char **buf = is_stderr ? &c->err : &c->out;
        size_t *blen = is_stderr ? &c->errlen : &c->outlen;
        size_t *bcap = is_stderr ? &c->errcap : &c->outcap;

        if (*blen + len + 1 > *bcap) {
            size_t want = *bcap ? *bcap : 4096;
            while (want < *blen + len + 1)
                want *= 2;
            *buf = sresize(*buf, want, char);
            *bcap = want;
        }
        memcpy(*buf + *blen, data, len);
        *blen += len;
        (*buf)[*blen] = '\0';

        if (c->interactive && g->exec_chan == c)
            sg_scp_rx(g, c);
        return len;
    }

    if (is_stderr)
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
    struct SgChan *c = container_of(chan, struct SgChan, chan);

    if (c->kind == SG_CHAN_EXEC)
        return dupstr("Transfer command finished");
    return dupstr("SFTP channel closed");
}

static bool sg_chan_rcvd_exit_status(Channel *chan, int status)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);

    c->got_exit = true;
    c->exit_status = status;
    return true;
}

static void sg_chan_request_response(Channel *chan, bool success)
{
    struct SgChan *c = container_of(chan, struct SgChan, chan);

    if (!c->g)
        return;

    if (c->kind == SG_CHAN_EXEC) {
        if (!success) {
            /* The server would not run the command. */
            c->failed = true;
            if (c->g->exec_chan == c)
                sshfwd_initiate_close(c->sc, NULL);
        } else if (c->interactive && c->exec_kind == SG_EXEC_SCP_DL) {
            /*
             * In SCP the sink always speaks first: one NUL byte tells
             * the source that it may start sending. We are the sink
             * here, so this has to go out before we can expect a
             * control record.
             */
            sshfwd_write(c->sc, "", 1);
        }
        return;
    }

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
 * SCP transport.
 *
 * SCP has no directory service at all, so the remote side is driven by
 * running ordinary shell commands on the same SSH connection:
 *
 *   - "cd DIR && pwd && ls -lA" lists a directory (and canonicalises
 *     it, so a failed cd is detectable);
 *   - "mkdir -p DIR" creates one;
 *   - "scp -t -d DIR" and "scp -f FILE" are the SCP protocol itself.
 *
 * Every one of those is a separate exec channel; they are used one at a
 * time, which keeps the state machine single-threaded.
 */

/* Quote a string for the remote shell. */
static char *sg_shquote(const char *s)
{
    size_t n = 3;                       /* two quotes and the NUL */
    char *ret, *q;

    for (const char *p = s; *p; p++)
        n += (*p == '\'') ? 4 : 1;
    ret = snewn(n, char);
    q = ret;
    *q++ = '\'';
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            memcpy(q, "'\\''", 4);
            q += 4;
        } else {
            *q++ = *p;
        }
    }
    *q++ = '\'';
    *q = '\0';
    return ret;
}

static void sg_exec_write(SftpGui *g, const void *data, size_t len)
{
    if (g->exec_chan && g->exec_chan->sc)
        sshfwd_write(g->exec_chan->sc, data, len);
}

/* Drop the first n bytes of an exec channel's captured stdout. */
static void sg_exec_consume(struct SgChan *c, size_t n)
{
    if (n >= c->outlen) {
        c->outlen = 0;
    } else {
        memmove(c->out, c->out + n, c->outlen - n);
        c->outlen -= n;
    }
    if (c->out)
        c->out[c->outlen] = '\0';
}

static bool sg_exec_start(SftpGui *g, int kind, bool interactive,
                          const char *cmd)
{
    struct SgChan *c;

    if (!g->be || g->exec_chan)
        return false;

    c = snew(struct SgChan);
    memset(c, 0, sizeof(*c));
    c->chan.vt = &SgChan_vt;
    c->chan.initial_fixed_window_size = 0;
    c->g = g;
    c->kind = SG_CHAN_EXEC;
    c->exec_kind = kind;
    c->interactive = interactive;
    c->command = dupstr(cmd);

    c->sc = ssh_open_session_channel(g->be, &c->chan);
    if (!c->sc) {
        sfree(c->command);
        sfree(c);
        return false;
    }
    g->exec_chan = c;
    g->exec_kind = kind;
    return true;
}

static void sg_abort_exec(SftpGui *g)
{
    struct SgChan *c = g->exec_chan;

    g->exec_chan = NULL;
    if (c && c->sc)
        sshfwd_initiate_close(c->sc, NULL);
}

/* ----------------------------------------------------------------------
 * Parsing "pwd" + "ls -lA" output.
 */

static int sg_month_index(const char *s)
{
    static const char *const names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    for (int i = 0; i < 12; i++)
        if (!strncmp(s, names[i], 3))
            return i;
    return -1;
}

/*
 * Parse one "ls -l" line into an entry. Returns false if the line
 * carries no file (the "total N" header, blank lines, ...).
 */
static bool sg_parse_ls_line(char *line, struct SgEntry *e)
{
    char *tok[10];
    int ntok = 0;
    char *p = line, *name, *arrow;
    char type;

    memset(e, 0, sizeof(*e));

    /* Split off enough leading fields; the name is whatever is left. */
    while (ntok < 10) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        tok[ntok++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (!*p)
            break;
        *p++ = '\0';
        /* A device has "major, minor" in the size column. */
        if (ntok == 5 && tok[4][strlen(tok[4]) - 1] == ',')
            continue;
        if (ntok >= 8)
            break;
    }
    if (ntok < 8)
        return false;

    type = tok[0][0];
    if (!strchr("d-lbcps", type) || strlen(tok[0]) < 10)
        return false;

    /*
     * After the eighth field we consumed exactly one space; the rest of
     * the line is the name, spaces and all.
     */
    name = p;
    if (!*name)
        return false;
    if (type == 'l' && (arrow = strstr(name, " -> ")) != NULL)
        *arrow = '\0';

    e->is_dir = (type == 'd');
    e->name = dupstr(name);
    e->wname = NULL;
    if (type == 'd' || type == '-' || type == 'l')
        e->size = strtoull(tok[4], NULL, 10);

    /* Date: either "Mon DD HH:MM" (this year) or "Mon DD YYYY". */
    {
        int mon = sg_month_index(tok[5]);
        int day = atoi(tok[6]);
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (mon >= 0 && day > 0) {
            tm.tm_mon = mon;
            tm.tm_mday = day;
            if (strchr(tok[7], ':')) {
                time_t now = time(NULL);
                struct tm *nowtm = localtime(&now);
                int hh = 0, mm = 0;
                if (sscanf(tok[7], "%d:%d", &hh, &mm) == 2) {
                    tm.tm_hour = hh;
                    tm.tm_min = mm;
                }
                if (nowtm)
                    tm.tm_year = nowtm->tm_year;
            } else {
                tm.tm_year = atoi(tok[7]) - 1900;
            }
            tm.tm_isdst = -1;
            e->mtime = mktime(&tm);
        }
    }
    return true;
}

/*
 * Turn the output of "cd DIR && pwd && ls -lA" into a canonical path
 * plus a list of entries. Either output pointer may be NULL. 'out' is
 * modified in place.
 */
static void sg_parse_ls(char *out, char **canon_p,
                        struct SgEntry **ents_p, int *nents_p)
{
    struct SgEntry *ents = NULL;
    int n = 0, size = 0;
    char *p = out, *canon = NULL;
    bool first = true;

    while (p && *p) {
        char *line = p;
        char *nl = strchr(p, '\n');

        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = line + strlen(line);
        }
        if (*line && line[strlen(line) - 1] == '\r')
            line[strlen(line) - 1] = '\0';

        if (first) {
            /* The first line is pwd's output. */
            first = false;
            if (*line)
                canon = dupstr(line);
            continue;
        }
        if (!*line)
            continue;

        struct SgEntry e;
        if (sg_parse_ls_line(line, &e)) {
            if (n >= size) {
                size = size ? size * 2 : 64;
                ents = sresize(ents, size, struct SgEntry);
            }
            ents[n++] = e;
        }
    }

    if (canon_p)
        *canon_p = canon;
    else
        sfree(canon);
    if (ents_p) {
        *ents_p = ents;
        *nents_p = n;
    } else {
        sg_entries_free(ents, n);
    }
}

/*
 * Start listing a remote directory. When 'walk' is set the result feeds
 * a transfer instead of the browser pane, and the directory in question
 * is the remote side of g->walk_*.
 */
static void sg_scp_ls_start(SftpGui *g, const char *dir, bool walk)
{
    char *q, *cmd;

    if (dir && *dir) {
        q = sg_shquote(dir);
        cmd = dupprintf("cd %s && pwd && LC_ALL=C ls -lA", q);
        sfree(q);
    } else {
        /* No directory given: start where the login puts us. */
        cmd = dupstr("pwd && LC_ALL=C ls -lA");
    }

    g->exec_walk = walk;
    g->pend = SG_P_SCP_EXEC;
    if (!sg_exec_start(g, SG_EXEC_LS, false, cmd)) {
        sfree(cmd);
        sg_status(g, "无法在会话上打开通道");
        g->pend = SG_P_NONE;
        if (walk)
            sg_xfer_next(g);
        return;
    }
    sfree(cmd);
}

/* Move the remote pane to a directory (NULL means the login directory). */
static void sg_scp_goto(SftpGui *g, const char *dir)
{
    sg_scp_ls_start(g, dir, false);
}

/* Create a remote directory for a tree upload. */
static void sg_scp_mkdir_start(SftpGui *g, const char *local,
                               const char *remote)
{
    char *q = sg_shquote(remote);
    char *cmd = dupprintf("mkdir -p %s", q);

    sfree(q);
    sfree(g->walk_local);
    sfree(g->walk_remote);
    g->walk_local = dupstr(local);
    g->walk_remote = dupstr(remote);

    g->exec_walk = true;
    g->pend = SG_P_SCP_EXEC;
    if (!sg_exec_start(g, SG_EXEC_MKDIR, false, cmd)) {
        sfree(cmd);
        sg_status(g, "无法在会话上打开通道");
        g->pend = SG_P_NONE;
        sg_xfer_next(g);
        return;
    }
    sfree(cmd);
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

    if (g->scp) {
        if (g->subsys_ok)
            sg_scp_goto(g, g->rmt.cwd);
        return;
    }

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

    if (g->scp) {
        sfree(g->target_path);
        g->target_path = dupstr(path);
        sg_scp_goto(g, path);
        return;
    }

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

    /* The progress readout is driven by this timer while a transfer
     * runs, so that it keeps ticking whatever the data flow does. */
    if (busy)
        SetTimer(g->hwnd, SG_TIMER_PROGRESS, 250, NULL);
    else
        KillTimer(g->hwnd, SG_TIMER_PROGRESS);
}

/* Advance the progress bar. Cheap enough to call per chunk of data. */
static void sg_progress(SftpGui *g)
{
    int permille = 0;

    if (g->total_bytes > 0)
        permille = (int)((g->done_bytes * 1000) / g->total_bytes);
    if (permille > 1000)
        permille = 1000;
    SendMessageW(g->prog, PBM_SETPOS, (WPARAM)permille, 0);
}

/*
 * Refresh the "transferred / total   rate" text.
 *
 * This runs off a timer rather than off incoming data, because SCP
 * hands a whole burst to the SSH layer in one go: keyed to the data, the
 * figure would never update while that burst is draining. On a timer it
 * stays live for both protocols, and the smoothing makes the rate read
 * as a rate instead of jittering with every sample.
 */
static void sg_progress_refresh(SftpGui *g)
{
    DWORD now = GetTickCount();
    DWORD dt = now - g->speed_tick;
    char done[32], total[32], spd[32], text[160];

    if (dt >= 100) {
        double instant = (double)(g->done_bytes - g->speed_mark) * 1000.0 / dt;

        if (g->speed <= 0.0)
            g->speed = instant;
        else
            g->speed = g->speed * 0.35 + instant * 0.65;
        g->speed_mark = g->done_bytes;
        g->speed_tick = now;
    }

    sg_fmt_size(done, sizeof(done), g->done_bytes);
    sg_fmt_size(total, sizeof(total), g->total_bytes);
    if (g->speed >= 1024.0) {
        sg_fmt_size(spd, sizeof(spd), (uint64_t)g->speed);
        snprintf(text, sizeof(text), "%s / %s   %s/s", done, total, spd);
    } else {
        snprintf(text, sizeof(text), "%s / %s", done, total);
    }
    sg_set_text(g->progtext, text);
    sg_progress(g);
}

/* The final figures, with no rate: shown once a batch has ended. */
static void sg_progress_final(SftpGui *g)
{
    char done[32], total[32], text[96];

    g->speed = 0.0;
    sg_fmt_size(done, sizeof(done), g->done_bytes);
    sg_fmt_size(total, sizeof(total), g->total_bytes);
    snprintf(text, sizeof(text), "%s / %s", done, total);
    sg_set_text(g->progtext, text);
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
    sg_abort_exec(g);
    sg_batch_free(g);
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    g->pend = SG_P_NONE;
    g->inflight = 0;
    g->at_eof = false;
    sg_tasks_free(g);
    sg_set_busy(g, false);
    sg_progress_final(g);
    sg_status(g, msg);
    sg_remote_refresh(g);
    sg_local_refresh(g);
}

/* Abandon the running transfer and say why. */
static void sg_xfer_fail(SftpGui *g, const char *why)
{
    g->any_error = true;
    sg_abort_exec(g);
    sg_batch_free(g);
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
    sg_progress_final(g);
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

        /*
         * In SCP mode the file handlers take over a whole run of
         * consecutive files, so they consume the list themselves.
         */
        if (g->scp && t->kind == SG_T_FILE) {
            if (g->uploading)
                sg_scp_ul_begin(g);
            else
                sg_scp_dl_begin(g);
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
            if (g->scp)
                sg_scp_mkdir_start(g, t->local, t->remote);
            else
                sg_ul_dir_begin(g, t->local, t->remote);
            return;

          case SG_T_RDIR:
            if (g->scp) {
                wchar_t *w = sg_w(t->local);
                CreateDirectoryW(w, NULL);
                sfree(w);
                sfree(g->walk_local);
                sfree(g->walk_remote);
                g->walk_local = dupstr(t->local);
                g->walk_remote = dupstr(t->remote);
                sg_scp_ls_start(g, t->remote, true);
            } else {
                sg_dl_dir_begin(g, t->local, t->remote);
            }
            return;
        }
    }
    sg_xfer_finish(g, i18n_or("Transfer finished."));
}

static void sg_start_batch(SftpGui *g, bool uploading)
{
    g->uploading = uploading;
    g->cancel = false;
    /* total_bytes was worked out while queueing the selection, so it
     * must not be cleared here. */
    g->done_bytes = 0;
    g->any_error = false;
    g->speed = 0.0;
    g->speed_mark = 0;
    g->speed_tick = GetTickCount();
    SendMessageW(g->prog, PBM_SETPOS, 0, 0);
    sg_set_text(g->progtext, "");
    sg_set_busy(g, true);
    sg_xfer_next(g);
}

/* ----------------------------------------------------------------------
 * SCP: one scp process per directory's worth of files
 *
 * Starting a remote shell is by far the most expensive part of an SCP
 * transfer, so a run of files that all live in the same remote
 * directory is moved by a single "scp -t" / "scp -f" process: several
 * C records in one session, exactly as the protocol intends. That
 * keeps the driver at one level - no D/E record nesting - while making
 * a directory of many small files roughly as fast as one big file.
 *
 * Trees are walked above this, with mkdir -p and ls -lA doing the
 * non-file work.
 */

static void sg_batch_free(SftpGui *g)
{
    for (int i = 0; i < g->nbatch; i++) {
        sfree(g->batch[i].local);
        sfree(g->batch[i].remote);
    }
    sfree(g->batch);
    g->batch = NULL;
    g->nbatch = g->batch_pos = 0;
}

/*
 * Take the run of file tasks at the head of the work list that all go
 * into the same remote directory. Consumes them from the list.
 */
static void sg_batch_collect(SftpGui *g)
{
    char *dir;
    int size = 0;

    sg_batch_free(g);
    if (g->task_head >= g->ntasks ||
        g->tasks[g->task_head].kind != SG_T_FILE)
        return;

    dir = sg_parent_remote(g->tasks[g->task_head].remote);

    while (g->task_head < g->ntasks && g->nbatch < 64) {
        struct SgTask *t = &g->tasks[g->task_head];
        char *d;
        bool same;

        if (t->kind != SG_T_FILE)
            break;
        d = sg_parent_remote(t->remote);
        same = !strcmp(d, dir);
        sfree(d);
        if (!same)
            break;

        if (g->nbatch >= size) {
            size = size ? size * 2 : 16;
            g->batch = sresize(g->batch, size, struct SgBatchItem);
        }
        g->batch[g->nbatch].local = dupstr(t->local);
        g->batch[g->nbatch].remote = dupstr(t->remote);
        g->batch[g->nbatch].size = t->size;
        g->nbatch++;
        g->task_head++;
    }
    g->batch_pos = 0;
    sfree(dir);
}

/* End the current scp session and move on to the next task. */
static void sg_scp_done(SftpGui *g)
{
    struct SgChan *c = g->exec_chan;

    g->exec_chan = NULL;               /* stop routing data here */
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    if (c && c->sc)
        sshfwd_initiate_close(c->sc, NULL);
    sg_batch_free(g);
    g->pend = SG_P_NONE;
    sg_xfer_next(g);
}

/* The remote end said something other than "ok". */
static void sg_scp_proto_error(SftpGui *g, struct SgChan *c)
{
    char *msg = NULL;
    size_t n = 1;

    while (n < c->outlen && c->out[n] != '\n')
        n++;
    if (n > 1) {
        msg = snewn(n, char);
        memcpy(msg, c->out + 1, n - 1);
        msg[n-1] = '\0';
    }
    if (msg && *msg)
        sg_xfer_fail(g, msg);
    else
        sg_xfer_fail(g, "远端 scp 报告了错误");
    sfree(msg);
}

/*
 * Feed the next block of the local file into the scp session. Called
 * whenever the sink acknowledges the previous record, and again from the
 * event loop while the SSH layer's write backlog drains.
 */
static void sg_scp_ul_pump(SftpGui *g)
{
    char buf[16384];

    if (!g->exec_chan)
        return;                        /* the session has gone */

    while (g->scp_sent < g->cur_size) {
        uint64_t left = g->cur_size - g->scp_sent;
        DWORD want = (DWORD)(left > sizeof(buf) ? sizeof(buf) : left);
        DWORD got = 0;

        if (g->cancel) {
            sg_scp_done(g);
            return;
        }
        /*
         * Let the socket catch up. The limit is deliberately generous:
         * it is what caps the amount of data in flight, and hence the
         * achievable throughput on a link with any real round trip.
         */
        if (g->be && g->exec_chan &&
            ssh_channel_backlog(g->be, g->exec_chan->sc) > 512 * 1024) {
            PostMessage(g->hwnd, WM_SG_NOTIFY, SG_CH_PUMP, 0);
            return;
        }
        if (!ReadFile(g->lfh, buf, want, &got, NULL) || got == 0) {
            sg_xfer_fail(g, "读取本地文件失败");
            return;
        }
        sg_exec_write(g, buf, got);
        g->scp_sent += got;
        g->done_bytes += got;
        sg_progress_refresh(g);
    }

    /* End of the file body. */
    sg_exec_write(g, "", 1);
    g->scp_state = SG_SU_DATAACK;
}

/* Open the batch entry's local file and send its C record. */
static void sg_scp_ul_next_file(SftpGui *g)
{
    struct SgBatchItem *b = &g->batch[g->batch_pos];
    wchar_t *w = sg_w(b->local);
    char *base, *hdr;
    LARGE_INTEGER sz;

    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    g->lfh = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    sfree(w);
    if (g->lfh == INVALID_HANDLE_VALUE) {
        sg_statusf(g, "%s: 无法打开本地文件", b->local);
        sg_scp_done(g);
        return;
    }
    if (!GetFileSizeEx(g->lfh, &sz)) {
        sg_statusf(g, "%s: 无法读取本地文件大小", b->local);
        sg_scp_done(g);
        return;
    }
    b->size = (uint64_t)sz.QuadPart;
    g->cur_size = b->size;
    g->scp_sent = 0;

    sfree(g->cur_local);
    sfree(g->cur_remote);
    g->cur_local = dupstr(b->local);
    g->cur_remote = dupstr(b->remote);
    sg_statusf(g, "正在上传 %s", b->remote);

    base = sg_basename_remote(b->remote);
    hdr = dupprintf("C%04o %llu %s\n", 0644,
                    (unsigned long long)b->size, base);
    sg_exec_write(g, hdr, strlen(hdr));
    sfree(hdr);
    sfree(base);

    g->scp_state = SG_SU_HDRACK;
}

static void sg_scp_ul_begin(SftpGui *g)
{
    struct SgBatchItem *b;
    char *dir, *q, *cmd;

    sg_batch_collect(g);
    if (g->nbatch == 0) {
        sg_xfer_next(g);
        return;
    }
    b = &g->batch[0];
    sfree(g->cur_local);
    sfree(g->cur_remote);
    g->cur_local = dupstr(b->local);
    g->cur_remote = dupstr(b->remote);
    g->cur_size = b->size;
    g->scp_sent = 0;
    g->scp_state = SG_SU_BANNER;

    dir = sg_parent_remote(b->remote);
    q = sg_shquote(dir);
    cmd = dupprintf("scp -t -d %s", q);
    sfree(q);
    sfree(dir);

    g->pend = SG_P_SCP_UPLOAD;
    if (!sg_exec_start(g, SG_EXEC_SCP_UL, true, cmd)) {
        sfree(cmd);
        sg_xfer_fail(g, "无法在会话上启动 scp 命令");
        return;
    }
    sfree(cmd);
}

static void sg_scp_dl_begin(SftpGui *g)
{
    char *cmd, *q;
    size_t len;

    sg_batch_collect(g);
    if (g->nbatch == 0) {
        sg_xfer_next(g);
        return;
    }
    sfree(g->cur_local);
    sfree(g->cur_remote);
    g->cur_local = dupstr(g->batch[0].local);
    g->cur_remote = dupstr(g->batch[0].remote);
    g->scp_remaining = 0;
    g->scp_state = SG_SD_RECORD;

    /* "scp -f 'a' 'b' ..." sends one C record per file, in order. */
    len = 16;
    for (int i = 0; i < g->nbatch; i++) {
        char *sq = sg_shquote(g->batch[i].remote);
        len += strlen(sq) + 1;
        sfree(sq);
    }
    cmd = snewn(len + 16, char);
    strcpy(cmd, "scp -f");
    for (int i = 0; i < g->nbatch; i++) {
        q = sg_shquote(g->batch[i].remote);
        strcat(cmd, " ");
        strcat(cmd, q);
        sfree(q);
    }

    g->pend = SG_P_SCP_DOWNLOAD;
    if (!sg_exec_start(g, SG_EXEC_SCP_DL, true, cmd)) {
        sfree(cmd);
        sg_xfer_fail(g, "无法在会话上启动 scp 命令");
        return;
    }
    sfree(cmd);
}

/* Drive the scp protocol as far as the buffered data allows. */
static void sg_scp_rx(SftpGui *g, struct SgChan *c)
{
    bool again = true;

    while (again && g->exec_chan == c) {
        again = false;

        switch (g->scp_state) {
          case SG_SU_BANNER:
            if (c->outlen < 1)
                return;
            if (c->out[0] != 0) {
                sg_scp_proto_error(g, c);
                return;
            }
            sg_exec_consume(c, 1);
            sg_scp_ul_next_file(g);
            break;

          case SG_SU_HDRACK:
            if (c->outlen < 1)
                return;
            if (c->out[0] != 0) {
                sg_scp_proto_error(g, c);
                return;
            }
            sg_exec_consume(c, 1);
            g->scp_state = SG_SU_DATA;
            sg_scp_ul_pump(g);
            return;

          case SG_SU_DATAACK:
            if (c->outlen < 1)
                return;
            if (c->out[0] != 0) {
                sg_scp_proto_error(g, c);
                return;
            }
            sg_exec_consume(c, 1);
            g->batch_pos++;
            if (g->batch_pos < g->nbatch) {
                sg_scp_ul_next_file(g);
                again = true;
                break;
            }
            /*
             * That was the last file. There is no separate "end of
             * transfer" byte to send here: the sender simply stops
             * after the last file's data terminator has been
             * acknowledged, and the receiver exits when it sees EOF on
             * the channel. (Verified against pscp's wire trace, and
             * ssh/scpserver.c's protocol notes say the final EOF is
             * sent without waiting for an ack anyway.)
             */
            sg_scp_done(g);
            return;

          case SG_SU_DATA:
            /* Nothing should arrive while we are streaming. */
            return;

          case SG_SD_RECORD: {
            char *nl, *line;
            size_t len;

            if (c->outlen < 1)
                return;
            if (c->out[0] == 0) {
                /* End of the file list. */
                sg_exec_consume(c, 1);
                sg_exec_write(g, "", 1);
                sg_scp_done(g);
                return;
            }
            if (c->out[0] == 1 || c->out[0] == 2) {
                sg_scp_proto_error(g, c);
                return;
            }
            nl = memchr(c->out, '\n', c->outlen);
            if (!nl)
                return;                /* wait for the whole record */
            len = (size_t)(nl - c->out) + 1;
            line = snewn(len + 1, char);
            memcpy(line, c->out, len);
            line[len] = '\0';
            sg_exec_consume(c, len);

            if (line[0] == 'T') {
                sg_exec_write(g, "", 1);
                sfree(line);
                again = true;
                break;
            }
            if (line[0] == 'C') {
                unsigned long mode = 0;
                unsigned long long sz = 0;
                int off = 0;
                struct SgBatchItem *b;
                wchar_t *w;

                if (sscanf(line, "C%lo %llu %n", &mode, &sz, &off) != 2) {
                    sfree(line);
                    sg_xfer_fail(g, "SCP 协议错误：无法解析文件头");
                    return;
                }
                sfree(line);

                if (g->batch_pos >= g->nbatch) {
                    sg_xfer_fail(g, "SCP 协议错误：收到了多余的文件");
                    return;
                }
                /* Records come back in the order we asked for them. */
                b = &g->batch[g->batch_pos];
                if (g->lfh != INVALID_HANDLE_VALUE) {
                    CloseHandle(g->lfh);
                    g->lfh = INVALID_HANDLE_VALUE;
                }
                w = sg_w(b->local);
                g->lfh = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, NULL);
                sfree(w);
                if (g->lfh == INVALID_HANDLE_VALUE) {
                    sg_statusf(g, "%s: 无法创建本地文件", b->local);
                    sg_scp_done(g);
                    return;
                }
                sfree(g->cur_local);
                sfree(g->cur_remote);
                g->cur_local = dupstr(b->local);
                g->cur_remote = dupstr(b->remote);
                sg_statusf(g, "正在下载 %s", b->remote);

                g->scp_remaining = (uint64_t)sz;
                sg_exec_write(g, "", 1);   /* accept the file */
                g->scp_state = SG_SD_DATA;
                again = true;
                break;
            }
            sfree(line);
            sg_xfer_fail(g, "SCP 协议错误：未知的响应记录");
            return;
          }

          case SG_SD_DATA: {
            size_t take;
            DWORD written = 0;

            if (g->scp_remaining == 0) {
                g->scp_state = SG_SD_DATAEND;
                again = true;
                break;
            }
            if (c->outlen == 0)
                return;
            take = c->outlen < g->scp_remaining ? c->outlen
                                                : (size_t)g->scp_remaining;
            if (!WriteFile(g->lfh, c->out, (DWORD)take, &written, NULL) ||
                written != take) {
                sg_xfer_fail(g, "写入本地文件失败");
                return;
            }
            sg_exec_consume(c, take);
            g->scp_remaining -= take;
            g->done_bytes += take;
            sg_progress_refresh(g);
            again = true;
            break;
          }

          case SG_SD_DATAEND:
            if (c->outlen < 1)
                return;
            if (c->out[0] != 0) {
                sg_scp_proto_error(g, c);
                return;
            }
            sg_exec_consume(c, 1);
            sg_exec_write(g, "", 1);   /* acknowledge the file */
            g->batch_pos++;
            /* Back for the next C record, or for the source's EOF
             * marker if that was the last file. */
            g->scp_state = SG_SD_RECORD;
            again = true;
            break;

          default:
            return;
        }
    }
}

/* Copy a command's error output, minus the line ending, for the status
 * line. Caller frees. */
static char *sg_exec_message(const char *text, const char *fallback)
{
    const char *src = (text && *text) ? text : fallback;
    size_t n = strlen(src);

    while (n > 0 && (src[n-1] == '\n' || src[n-1] == '\r' || src[n-1] == ' '))
        n--;

    char *ret = snewn(n + 1, char);
    memcpy(ret, src, n);
    ret[n] = '\0';
    return ret;
}

/* An exec channel has closed: work out what it meant. */
static void sg_exec_done(SftpGui *g)
{
    int kind = g->exec_kind;
    char *out = g->exec_out, *err = g->exec_err;
    bool ok = g->exec_ok, walk = g->exec_walk;

    g->exec_out = g->exec_err = NULL;
    g->exec_outlen = g->exec_errlen = 0;
    g->exec_kind = 0;
    g->exec_walk = false;

    if (kind == SG_EXEC_SCP_UL || kind == SG_EXEC_SCP_DL) {
        /* The session ended before we had finished with it. */
        if (g->pend == SG_P_SCP_UPLOAD || g->pend == SG_P_SCP_DOWNLOAD) {
            if (g->lfh != INVALID_HANDLE_VALUE) {
                CloseHandle(g->lfh);
                g->lfh = INVALID_HANDLE_VALUE;
            }
            g->pend = SG_P_NONE;
            if (ok)
                sg_xfer_next(g);
            else
                sg_xfer_fail(g, (err && *err) ? err : "SCP 传输中断");
        }
        goto out;
    }

    if (kind == SG_EXEC_MKDIR) {
        /* "mkdir -p" failing just means it was already there. */
        g->pend = SG_P_NONE;
        sg_local_enqueue_tree(g, g->walk_local, g->walk_remote);
        sg_xfer_next(g);
        goto out;
    }

    if (kind == SG_EXEC_RM || kind == SG_EXEC_MV) {
        /* The context menu's remote delete and rename. */
        g->pend = SG_P_NONE;
        if (!ok) {
            char *msg = sg_exec_message(
                err, kind == SG_EXEC_RM ? "删除失败" : "重命名失败");

            sg_status(g, msg);
            sfree(msg);
        } else {
            sg_status(g, kind == SG_EXEC_RM ? i18n_or("Deleted.")
                                            : "已重命名");
        }
        sg_remote_refresh(g);
        goto out;
    }

    if (kind == SG_EXEC_LS) {
        g->pend = SG_P_NONE;

        if (!ok || !out || !*out) {
            sg_statusf(g, "%s: %s", walk ? g->walk_remote : g->rmt.cwd,
                       (err && *err) ? err : "无法列出远端目录");
            if (walk)
                sg_xfer_next(g);
            goto out;
        }

        if (walk) {
            char *canon = NULL;
            struct SgEntry *ents = NULL;
            int n = 0;

            sg_parse_ls(out, &canon, &ents, &n);
            sfree(canon);
            for (int i = 0; i < n; i++) {
                char *l, *r;

                if (!strcmp(ents[i].name, ".."))
                    continue;
                l = sg_join(g->walk_local, ents[i].name, '\\');
                r = sg_join(g->walk_remote, ents[i].name, '/');
                if (ents[i].is_dir) {
                    sg_task_push(g, SG_T_RDIR, l, r, 0);
                } else {
                    sg_task_push(g, SG_T_FILE, l, r, ents[i].size);
                    g->total_bytes += ents[i].size;
                }
                sfree(l);
                sfree(r);
            }
            sg_entries_free(ents, n);
            sg_xfer_next(g);
        } else {
            char *canon = NULL;
            int n = 0;

            sg_entries_free(g->rents, g->nrents);
            g->rents = NULL;
            g->nrents = g->rents_size = 0;
            sg_parse_ls(out, &canon, &g->rents, &n);
            if (canon) {
                sfree(g->rmt.cwd);
                g->rmt.cwd = canon;
            }
            g->nrents = n;
            g->rents_size = n;
            sg_set_text(g->rmt.path, g->rmt.cwd);
            sg_remote_fill(g);
        }
    }

  out:
    sfree(out);
    sfree(err);
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
        sg_progress_refresh(g);
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
    sg_progress_refresh(g);

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

      case SG_P_DEL_OPENDIR: {
        struct fxp_handle *h = fxp_opendir_recv(pktin, req);

        g->pend = SG_P_NONE;
        if (!h) {
            sg_statusf(g, "%s: %s", g->del[g->del_walk].path, fxp_error());
            g->del_error = true;
            g->del_walk++;
            sg_del_run(g);
            break;
        }
        g->dirhandle = h;
        g->pend = SG_P_DEL_READDIR;
        sftp_register(fxp_readdir_send(h));
        break;
      }

      case SG_P_DEL_READDIR: {
        struct fxp_names *names = fxp_readdir_recv(pktin, req);

        if (!names) {
            struct fxp_handle *h = g->dirhandle;

            if (fxp_error_type() != SSH_FX_EOF) {
                sg_statusf(g, "%s: %s", g->del[g->del_walk].path,
                           fxp_error());
                g->del_error = true;
            }
            g->dirhandle = NULL;
            g->pend = SG_P_DEL_CLOSEDIR;
            sftp_register(fxp_close_send(h));
            break;
        }
        for (int i = 0; i < names->nnames; i++) {
            struct fxp_name *fn = &names->names[i];
            bool isdir =
                ((fn->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
                 (fn->attrs.permissions & 0170000) == PERMS_DIRECTORY);
            char *p;

            if (!strcmp(fn->filename, ".") || !strcmp(fn->filename, ".."))
                continue;
            p = sg_join(g->del[g->del_walk].path, fn->filename, '/');
            sg_del_push(g, p, isdir);
            sfree(p);
        }
        fxp_free_names(names);
        sftp_register(fxp_readdir_send(g->dirhandle));
        break;
      }

      case SG_P_DEL_CLOSEDIR:
        fxp_close_recv(pktin, req);
        g->pend = SG_P_NONE;
        g->del[g->del_walk].scanned = true;
        g->del_walk++;
        sg_del_run(g);
        break;

      case SG_P_DEL_REMOVE:
      case SG_P_DEL_RMDIR: {
        bool ok = (g->pend == SG_P_DEL_RMDIR)
            ? fxp_rmdir_recv(pktin, req) : fxp_remove_recv(pktin, req);

        g->pend = SG_P_NONE;
        if (!ok) {
            sg_statusf(g, "%s: %s", g->del[g->ndel-1].path, fxp_error());
            g->del_error = true;
        } else {
            g->del_done++;
        }
        sfree(g->del[g->ndel-1].path);
        g->ndel--;
        sg_del_run(g);
        break;
      }

      case SG_P_RENAME:
        g->pend = SG_P_NONE;
        if (!fxp_rename_recv(pktin, req))
            sg_status(g, fxp_error());
        sg_remote_refresh(g);
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
    sg_del_free(g);
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
        sg_msgbox(g->hwnd, "尚未连接", MB_ICONWARNING | MB_OK);
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
    int *idxs = NULL;
    int n = sg_lv_collect(lv, &idxs);
    int count = 0;

    /* Sizes are accumulated here; the batch starts with none. */
    g->total_bytes = 0;

    for (int i = 0; i < n; i++) {
        struct SgEntry *e;
        char *lpath, *rpath;

        if (idxs[i] < 0 || idxs[i] >= nents)
            continue;
        e = &ents[idxs[i]];
        if (!strcmp(e->name, ".."))
            continue;
        if (from_local && e->is_dir && e->reparse)
            continue;           /* junction or symlink: not followed */

        /*
         * A task always stores the local path and the remote path, in
         * that order, whichever way the data is going.
         */
        lpath = sg_join(g->lcl.cwd, e->name, '\\');
        rpath = sg_join(g->rmt.cwd, e->name, '/');
        if (e->is_dir) {
            sg_task_push(g, from_local ? SG_T_LDIR : SG_T_RDIR,
                         lpath, rpath, 0);
        } else {
            sg_task_push(g, SG_T_FILE, lpath, rpath, e->size);
            g->total_bytes += e->size;
        }
        sfree(lpath);
        sfree(rpath);
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

    g->cancel = true;

    /*
     * Stop straight away rather than letting the current file finish:
     * tear down the scp process (or leave the SFTP requests to be
     * discarded when their replies arrive), drop whatever is left of
     * the batch, and tidy up a partial download.
     */
    if (g->lfh != INVALID_HANDLE_VALUE &&
        (g->pend == SG_P_DOWNLOAD || g->pend == SG_P_SCP_DOWNLOAD) &&
        g->cur_local) {
        wchar_t *w = sg_w(g->cur_local);
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
        DeleteFileW(w);                /* don't leave half a file behind */
        sfree(w);
    }
    sg_xfer_finish(g, i18n_or("Transfer cancelled."));
}

/* ----------------------------------------------------------------------
 * Keyboard input for the window's own controls.
 *
 * PuTTY's message loop deliberately never calls TranslateMessage: the
 * terminal window translates keystrokes itself, and keeping the two
 * apart is what lets it tie a character to the key that produced it.
 * The consequence is that no other window in this process ever receives
 * a WM_CHAR, so the path boxes and the in-place name editor have to
 * translate their own WM_KEYDOWN messages, which is what this does.
 */

static void sg_type_key(HWND hwnd, WPARAM vk, LPARAM lParam)
{
    BYTE state[256];
    wchar_t buf[8];
    UINT scancode = (UINT)((lParam >> 16) & 0xFF);
    int n;

    if (vk == VK_PACKET) {
        /* An injected Unicode character: the character is in lParam. */
        SendMessageW(hwnd, WM_CHAR, (WPARAM)(lParam & 0xFFFF), lParam);
        return;
    }
    if (!GetKeyboardState(state))
        return;

    n = ToUnicode((UINT)vk, scancode, state, buf, lenof(buf), 0);
    for (int i = 0; i < n; i++)
        SendMessageW(hwnd, WM_CHAR, (WPARAM)buf[i], lParam);
}

/* The name editor a list view creates, once we have subclassed it. */
static WNDPROC sg_label_origproc;
static HWND sg_label_edit;

static LRESULT CALLBACK sg_label_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
        sg_type_key(hwnd, wParam, lParam);
    return CallWindowProcW(sg_label_origproc, hwnd, msg, wParam, lParam);
}

static void sg_watch_label_edit(HWND edit)
{
    if (!edit || edit == sg_label_edit)
        return;
    sg_label_edit = edit;
    sg_label_origproc = (WNDPROC)SetWindowLongPtrW(
        edit, GWLP_WNDPROC, (LONG_PTR)sg_label_proc);
}

/* ----------------------------------------------------------------------
 * The list views' context menu: upload/download, delete and rename.
 *
 * The same menu serves both panes. The transfer item is the one
 * difference: in the local pane it pushes the selection out to the
 * server, and in the remote pane it pulls it in.
 */

/* The rows selected in a pane, ignoring the ".." entry. Caller frees. */
static int sg_ctx_selection(struct SgPane *p, int **out)
{
    int *idxs = NULL;
    int n = sg_lv_collect(p->list, &idxs);
    int m = 0;

    for (int i = 0; i < n; i++)
        if (idxs[i] >= 0 && idxs[i] < p->nents &&
            strcmp(p->ents[idxs[i]].name, "..") != 0)
            idxs[m++] = idxs[i];
    *out = idxs;
    return m;
}

/* Select the row under the mouse unless it already is selected. */
static bool sg_ctx_hit_test(HWND lv, POINT screen)
{
    LVHITTESTINFO ht;

    memset(&ht, 0, sizeof(ht));
    ht.pt = screen;
    ScreenToClient(lv, &ht.pt);
    if ((int)SendMessageW(lv, LVM_HITTEST, 0, (LPARAM)&ht) < 0)
        return false;
    if (!(ht.flags & LVIS_SELECTED)) {
        ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(lv, ht.iItem, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    return true;
}

/* Delete a local file, or a directory and everything below it. */
static char *sg_local_delete(const char *path, bool is_dir, bool recurse)
{
    wchar_t *w;
    DWORD e;

    if (is_dir && recurse) {
        struct SgEntry *ents = NULL;
        int n = 0;
        char *err = sg_local_list(path, &ents, &n);

        if (err)
            return err;
        for (int i = 0; i < n; i++) {
            char *child;

            if (!strcmp(ents[i].name, ".."))
                continue;
            child = sg_join(path, ents[i].name, '\\');
            /* A junction or symlink is removed, never followed. */
            err = sg_local_delete(child, ents[i].is_dir, !ents[i].reparse);
            sfree(child);
            if (err)
                break;
        }
        sg_entries_free(ents, n);
        if (err)
            return err;
    }

    w = sg_w(path);
    if (is_dir ? !RemoveDirectoryW(w) : !DeleteFileW(w)) {
        e = GetLastError();
        sfree(w);
        return dupprintf("%s: %s", path, win_strerror(e));
    }
    sfree(w);
    return NULL;
}

/* ----------------------------------------------------------------------
 * Remote deletion: walk the tree into a list, then remove it backwards,
 * so every directory is empty by the time its turn comes.
 */

static void sg_del_free(SftpGui *g)
{
    for (int i = 0; i < g->ndel; i++)
        sfree(g->del[i].path);
    sfree(g->del);
    g->del = NULL;
    g->ndel = g->delsize = g->del_walk = g->del_done = 0;
    g->del_error = false;
}

static void sg_del_push(SftpGui *g, const char *path, bool is_dir)
{
    if (g->ndel >= g->delsize) {
        g->delsize = g->delsize ? g->delsize * 2 : 16;
        g->del = sresize(g->del, g->delsize, struct SgDelItem);
    }
    g->del[g->ndel].path = dupstr(path);
    g->del[g->ndel].is_dir = is_dir;
    g->del[g->ndel].scanned = false;
    g->ndel++;
}

static void sg_del_run(SftpGui *g)
{
    while (g->pend == SG_P_NONE) {
        if (g->del_walk < g->ndel) {
            struct SgDelItem *it = &g->del[g->del_walk];

            if (it->is_dir && !it->scanned) {
                g->pend = SG_P_DEL_OPENDIR;
                sftp_register(fxp_opendir_send(it->path));
                return;
            }
            g->del_walk++;
            continue;
        }

        if (g->ndel == 0) {
            bool bad = g->del_error;
            int done = g->del_done;

            sg_del_free(g);
            if (bad)
                sg_status(g, "部分项目删除失败");
            else
                sg_statusf(g, i18n_or("Deleted %d item(s)."), done);
            sg_remote_refresh(g);
            return;
        }

        struct SgDelItem *it = &g->del[g->ndel-1];

        g->pend = it->is_dir ? SG_P_DEL_RMDIR : SG_P_DEL_REMOVE;
        sftp_register(it->is_dir ? fxp_rmdir_send(it->path)
                                 : fxp_remove_send(it->path));
        return;
    }
}

static void sg_remote_delete(SftpGui *g, int *idxs, int n)
{
    sg_del_free(g);
    for (int i = 0; i < n; i++) {
        struct SgEntry *e = &g->rmt.ents[idxs[i]];
        char *path = sg_join(g->rmt.cwd, e->name, '/');

        sg_del_push(g, path, e->is_dir);
        sfree(path);
    }
    sg_del_run(g);
}

/* SCP has no delete of its own: ask the shell on the far end. */
static void sg_scp_delete(SftpGui *g, int *idxs, int n)
{
    char *cmd = dupstr("rm -rf");

    for (int i = 0; i < n; i++) {
        struct SgEntry *e = &g->rmt.ents[idxs[i]];
        char *path = sg_join(g->rmt.cwd, e->name, '/');
        char *q = sg_shquote(path);
        char *next = dupprintf("%s %s", cmd, q);

        sfree(cmd);
        sfree(path);
        sfree(q);
        cmd = next;
    }

    g->exec_walk = false;
    g->pend = SG_P_SCP_EXEC;
    if (!sg_exec_start(g, SG_EXEC_RM, false, cmd)) {
        sg_status(g, "无法在会话上打开通道");
        g->pend = SG_P_NONE;
    }
    sfree(cmd);
}

/* ----------------------------------------------------------------------
 * Renaming.
 */

static void sg_rename_local(SftpGui *g, const char *from, const char *to)
{
    char *oldp = sg_join(g->lcl.cwd, from, '\\');
    char *newp = sg_join(g->lcl.cwd, to, '\\');
    wchar_t *wo = sg_w(oldp), *wn = sg_w(newp);
    bool ok = MoveFileW(wo, wn) != 0;
    DWORD e = GetLastError();

    sfree(wo);
    sfree(wn);
    if (!ok)
        sg_statusf(g, "%s: %s", oldp, win_strerror(e));
    else
        sg_statusf(g, "已重命名为 %s", to);
    sfree(oldp);
    sfree(newp);
    sg_local_refresh(g);
}

static void sg_rename_remote(SftpGui *g, const char *from, const char *to)
{
    char *oldp = sg_join(g->rmt.cwd, from, '/');
    char *newp = sg_join(g->rmt.cwd, to, '/');

    if (g->scp) {
        char *qo = sg_shquote(oldp), *qn = sg_shquote(newp);
        char *cmd = dupprintf("mv %s %s", qo, qn);

        sfree(qo);
        sfree(qn);
        g->exec_walk = false;
        g->pend = SG_P_SCP_EXEC;
        if (!sg_exec_start(g, SG_EXEC_MV, false, cmd)) {
            sg_status(g, "无法在会话上打开通道");
            g->pend = SG_P_NONE;
        }
        sfree(cmd);
    } else {
        g->pend = SG_P_RENAME;
        sftp_register(fxp_rename_send(oldp, newp));
    }
    sfree(oldp);
    sfree(newp);
}

/*
 * Open the in-place name editor on one row. The list view creates an
 * edit box of its own, but it only hands the keyboard to it when the
 * command came from the keyboard or a slow click, so the focus is set
 * explicitly here as well.
 */
static void sg_start_label_edit(SftpGui *g, bool is_local, int idx)
{
    HWND lv = is_local ? g->lcl.list : g->rmt.list;
    int n = is_local ? g->lcl.nents : g->rmt.nents;
    HWND edit;

    if (idx < 0 || idx >= n)
        return;

    ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(lv, idx, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(lv, LVM_ENSUREVISIBLE, idx, FALSE);
    SetFocus(lv);
    if (!SendMessageW(lv, LVM_EDITLABELW, idx, 0))
        return;

    edit = (HWND)SendMessageW(lv, LVM_GETEDITCONTROL, 0, 0);
    if (edit) {
        sg_watch_label_edit(edit);
        SetFocus(edit);
    }
}

/* Rename one entry of a pane, once the label edit box has finished. */
static void sg_ctx_rename(SftpGui *g, bool is_local, int idx, const char *name)
{
    struct SgPane *p = is_local ? &g->lcl : &g->rmt;

    if (idx < 0 || idx >= p->nents || !*name)
        return;
    if (!strcmp(name, "..") || !strcmp(name, p->ents[idx].name))
        return;
    if (strpbrk(name, is_local ? "\\/:*?\"<>|" : "/"))
        return;                        /* not a name Windows or the
                                        * server would accept */

    if (is_local)
        sg_rename_local(g, p->ents[idx].name, name);
    else
        sg_rename_remote(g, p->ents[idx].name, name);
}

/* ----------------------------------------------------------------------
 * Delete, after asking first.
 */

static void sg_ctx_delete(SftpGui *g, bool is_local, int *idxs, int n)
{
    struct SgPane *p = is_local ? &g->lcl : &g->rmt;
    char *msg;

    if (n == 0)
        return;

    msg = dupprintf(i18n_or("Delete %d selected item(s)?"), n);
    {
        wchar_t *wmsg = sg_w(msg), *wcap = sg_w("SFTP");
        int ret = MessageBoxW(g->hwnd, wmsg, wcap,
                              MB_ICONWARNING | MB_YESNO);

        sfree(wmsg);
        sfree(wcap);
        if (ret != IDYES) {
            sfree(msg);
            return;
        }
    }
    sfree(msg);

    if (is_local) {
        char *err = NULL;
        int done = 0;

        for (int i = 0; i < n && !err; i++) {
            struct SgEntry *e = &p->ents[idxs[i]];
            char *path = sg_join(g->lcl.cwd, e->name, '\\');

            err = sg_local_delete(path, e->is_dir, !e->reparse);
            sfree(path);
            if (!err)
                done++;
        }
        if (err) {
            sg_status(g, err);
            sfree(err);
        } else {
            sg_statusf(g, i18n_or("Deleted %d item(s)."), done);
        }
        sg_local_refresh(g);
    } else if (g->scp) {
        sg_scp_delete(g, idxs, n);
    } else {
        sg_remote_delete(g, idxs, n);
    }
}

static void sg_popup_menu(SftpGui *g, bool is_local, int x, int y)
{
    struct SgPane *p = is_local ? &g->lcl : &g->rmt;
    int *idxs = NULL;
    int n = sg_ctx_selection(p, &idxs);
    bool ready = !g->busy && g->subsys_ok && g->pend == SG_P_NONE;
    bool can_xfer = n > 0 && ready;
    /* The local pane needs no connection, but not mid-transfer either. */
    bool can_edit = is_local ? !g->busy : ready;
    bool can_del = n > 0 && can_edit;
    bool can_ren = n == 1 && can_edit;
    HMENU m = CreatePopupMenu();
    wchar_t *wxfer = sg_w(i18n_or(is_local ? "Upload..." : "Download..."));
    wchar_t *wdel = sg_w(i18n_or("Delete"));
    wchar_t *wren = sg_w(i18n_or("Rename"));
    int cmd;

    AppendMenuW(m, MF_STRING, SG_ID_CTX_XFER, wxfer);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, SG_ID_CTX_DELETE, wdel);
    AppendMenuW(m, MF_STRING, SG_ID_CTX_RENAME, wren);
    EnableMenuItem(m, SG_ID_CTX_XFER, can_xfer ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, SG_ID_CTX_DELETE, can_del ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, SG_ID_CTX_RENAME, can_ren ? MF_ENABLED : MF_GRAYED);

    sfree(wxfer);
    sfree(wdel);
    sfree(wren);

    SetForegroundWindow(g->hwnd);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              x, y, 0, g->hwnd, NULL);
    DestroyMenu(m);

    switch (cmd) {
      case SG_ID_CTX_XFER:
        if (is_local)
            sg_do_upload(g);
        else
            sg_do_download(g);
        break;

      case SG_ID_CTX_DELETE:
        sg_ctx_delete(g, is_local, idxs, n);
        break;

      case SG_ID_CTX_RENAME:
        if (n == 1) {
            /*
             * Start the label edit from a posted message. The menu is
             * still being torn down when TrackPopupMenu returns, and it
             * takes the keyboard focus back with it: an edit box opened
             * here would end up without focus, and typing would go
             * nowhere.
             */
            g->ren_idx = idxs[0];
            g->ren_local = is_local;
            PostMessage(g->hwnd, WM_SG_NOTIFY, SG_CH_RENAME_EDIT, 0);
        }
        break;
    }

    sfree(idxs);
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
    if (hwnd == g->lcl.edit)
        sg_local_goto(g, path);
    else if (hwnd == g->rmt.edit)
        sg_remote_goto(g, path);
    sfree(path);
}

static LRESULT CALLBACK sg_edit_proc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            sg_edit_enter(hwnd);
            return 0;
        }
        sg_type_key(hwnd, wParam, lParam);
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
    static const char *const titles[] = { "Name", "Size", "Type", "Modified" };
    static const int widths[] = { 200, 84, 88, 112 };
    LVCOLUMNW col;
    HIMAGELIST icons = sg_sys_small_icons();

    /*
     * Report view draws the small image list beside the first column,
     * which is what puts the folder/file icon in front of each name.
     */
    if (icons)
        SendMessageW(lv, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)icons);

    for (int i = 0; i < (int)lenof(titles); i++) {
        wchar_t *w = sg_w(i18n_or(titles[i]));
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = widths[i];
        col.pszText = w;
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
        sfree(w);
    }
}

/*
 * A path box is an editable combo box. Its real edit control is a child
 * window, and that is where the Return key has to be intercepted, so
 * subclass it rather than the combo.
 */
static void sg_subclass_combo(struct SgPane *p)
{
    COMBOBOXINFO ci;

    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (GetComboBoxInfo(p->path, &ci) && ci.hwndItem) {
        p->edit = ci.hwndItem;
        sg_edit_origproc = (WNDPROC)SetWindowLongPtrW(
            p->edit, GWLP_WNDPROC, (LONG_PTR)sg_edit_proc);
    }
}

static void sg_create_controls(SftpGui *g)
{
    const DWORD combo_style = CBS_DROPDOWN | CBS_AUTOHSCROLL |
        CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP;

    g->lcl.label = sg_child(g, L"STATIC", "Local", SS_LEFT, 0, SG_ID_L_LABEL);
    g->lcl.path = sg_child(g, L"COMBOBOX", "", combo_style, 0, SG_ID_L_PATH);
    sg_subclass_combo(&g->lcl);
    g->lcl.up = sg_child(g, L"BUTTON", "..", BS_PUSHBUTTON, 0, SG_ID_L_UP);
    g->lcl.list = sg_child(g, WC_LISTVIEWW, "",
                           LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                           WS_EX_CLIENTEDGE, SG_ID_L_LIST);

    g->rmt.label = sg_child(g, L"STATIC", "Remote", SS_LEFT, 0, SG_ID_R_LABEL);
    g->rmt.path = sg_child(g, L"COMBOBOX", "", combo_style, 0, SG_ID_R_PATH);
    sg_subclass_combo(&g->rmt);
    g->rmt.up = sg_child(g, L"BUTTON", "..", BS_PUSHBUTTON, 0, SG_ID_R_UP);
    g->rmt.list = sg_child(g, WC_LISTVIEWW, "",
                           LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                           WS_EX_CLIENTEDGE, SG_ID_R_LIST);

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

    g->progtext = sg_child(g, L"STATIC", "", SS_RIGHT | SS_CENTERIMAGE, 0,
                           SG_ID_PROGTEXT);

    g->status = sg_child(g, L"STATIC", "", SS_LEFT | SS_ENDELLIPSIS, 0,
                         SG_ID_STATUS);

    EnableWindow(g->btn_cancel, FALSE);
}

static void sg_layout(SftpGui *g)
{
    RECT rc;
    int W, H, m = 8, gap = 6, row = 24, midw = 96, prog_h = 16;
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

    /*
     * The path boxes are combo boxes: the height given here is the
     * height of the drop-down list, not of the closed box (Windows
     * sizes that from the font).
     */
    sg_place(g->lcl.label, left_x, m, 46, row);
    sg_place(g->lcl.path, left_x + 48, m, pane_w - 48 - 30, row + 220);
    sg_place(g->lcl.up, left_x + pane_w - 28, m, 28, row);
    sg_place(g->lcl.list, left_x, list_y, pane_w, list_h);

    sg_place(g->rmt.label, right_x, m, 46, row);
    sg_place(g->rmt.path, right_x + 48, m, pane_w - 48 - 30, row + 220);
    sg_place(g->rmt.up, right_x + pane_w - 28, m, 28, row);
    sg_place(g->rmt.list, right_x, list_y, pane_w, list_h);

    bx = m + pane_w + gap;
    by = list_y + 8;
    sg_place(g->btn_upload, bx, by, midw, row);
    by += row + gap;
    sg_place(g->btn_download, bx, by, midw, row);
    by += row + gap * 2;
    sg_place(g->btn_cancel, bx, by, midw, row);

    /* The bar, with the transferred/total bytes and the rate beside it. */
    sg_place(g->prog, left_x, prog_y, total_w - 300, prog_h);
    sg_place(g->progtext, left_x + total_w - 296, prog_y - 2, 296,
             prog_h + 4);
    sg_place(g->status, left_x, status_y, total_w, row);

    /*
     * Size, Type and Modified are fixed; Name takes whatever is left.
     * Size the name column from the list view's real width minus room
     * for the vertical scrollbar, so a horizontal scrollbar does not
     * appear the moment the list is long enough to scroll.
     */
    for (i = 0; i < 2; i++) {
        HWND lv = i ? g->rmt.list : g->lcl.list;

        GetClientRect(lv, &rc);
        namew = (rc.right - rc.left) - 22 - 84 - 88 - 112;
        if (namew < 90)
            namew = 90;
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(namew, 0));
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, 1, MAKELPARAM(84, 0));
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, 2, MAKELPARAM(88, 0));
        SendMessageW(lv, LVM_SETCOLUMNWIDTH, 3, MAKELPARAM(112, 0));
    }
}

/* Free everything a SftpGui owns; used on a failed start and at destroy. */
static void sg_free(SftpGui *g, bool channel_still_ours)
{
    if (channel_still_ours) {
        struct SgChan *ec = g->exec_chan;
        g->exec_chan = NULL;
        if (ec) {
            ec->g = NULL;
            if (ec->sc)
                sshfwd_initiate_close(ec->sc, NULL);
        }
        if (g->schan) {
            SshChannel *sc = g->sc;
            g->schan->g = NULL;
            g->schan = NULL;
            g->sc = NULL;
            if (sc)
                sshfwd_initiate_close(sc, NULL);
        }
    }
    if (g->lfh != INVALID_HANDLE_VALUE) {
        CloseHandle(g->lfh);
        g->lfh = INVALID_HANDLE_VALUE;
    }
    sg_tasks_free(g);
    sg_batch_free(g);
    sg_del_free(g);
    sftp_cleanup_request();
    sg_entries_free(g->lcl.ents, g->lcl.nents);
    sg_entries_free(g->rmt.ents, g->rmt.nents);
    sg_entries_free(g->rents, g->nrents);
    for (int i = 0; i < g->lcl.ncbpath; i++)
        sfree(g->lcl.cbpath[i]);
    sfree(g->lcl.cbpath);
    for (int i = 0; i < g->rmt.ncbpath; i++)
        sfree(g->rmt.cbpath[i]);
    sfree(g->rmt.cbpath);
    sfree(g->lcl.cwd);
    sfree(g->rmt.cwd);
    sfree(g->target_path);
    sfree(g->pick);
    sfree(g->ren_name);
    sfree(g->inbuf);
    sfree(g->exec_out);
    sfree(g->exec_err);
    sfree(g);
}

static SftpGui *sg_create(HWND hwnd, Backend *be, bool scp)
{
    SftpGui *g = snew(SftpGui);
    wchar_t wcwd[MAX_PATH];
    struct SgChan *c;

    memset(g, 0, sizeof(*g));
    g->hwnd = hwnd;
    g->scp = scp;
    g->be = be;
    g->lfh = INVALID_HANDLE_VALUE;
    g->pend = SG_P_NONE;
    g->rmt.cwd = dupstr("/");

    sg_create_controls(g);

    /* Start in the user's home directory, not in whatever working
     * directory the process happened to inherit. */
    wcwd[0] = L'\0';
    if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, wcwd) != S_OK ||
        !wcwd[0])
        GetCurrentDirectoryW(lenof(wcwd), wcwd);
    if (wcwd[0])
        g->lcl.cwd = sg_to_utf8(wcwd);
    if (!g->lcl.cwd)
        g->lcl.cwd = dupstr("C:\\");
    size_t n = strlen(g->lcl.cwd);
    if (n > 3 && g->lcl.cwd[n-1] == '\\')
        g->lcl.cwd[n-1] = '\0';

    sg_local_refresh(g);

    c = snew(struct SgChan);
    memset(c, 0, sizeof(*c));
    c->chan.vt = &SgChan_vt;
    c->chan.initial_fixed_window_size = 0;
    c->g = g;
    c->sc = NULL;
    c->kind = scp ? SG_CHAN_SENTINEL : SG_CHAN_SFTP;
    g->schan = c;

    c->sc = ssh_open_session_channel(be, &c->chan);
    if (!c->sc) {
        g->schan = NULL;
        sfree(c);
        sg_startup_error =
            "无法在该会话上打开通道。仅支持已完成登录的 SSH-2 会话。";
        sg_free(g, false);
        return NULL;
    }
    if (!scp)
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
        SftpGui *ng = sg_create(hwnd, (Backend *)cs->lpCreateParams,
                                sg_pending_scp);
        if (!ng)
            return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ng);
        return 0;
      }

      case WM_SIZE:
        if (g)
            sg_layout(g);
        /*
         * Moving the panes uncovers strips of window that the pane that
         * used to be there had painted, so everything is redrawn: the
         * window's own background around the gaps, and the panes
         * themselves, which cannot be trusted to notice that their
         * contents have to be laid out again for the new width. The
         * panes are double-buffered, so this does not flicker.
         */
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;

      case WM_EXITSIZEMOVE:
        /*
         * The drag has finished: do the final repaint now rather than
         * leaving it to the next WM_PAINT, so the window is never left
         * showing a mix of the old and the new layout.
         */
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                     RDW_UPDATENOW);
        return 0;

      case WM_TIMER:
        if (g && wParam == SG_TIMER_PROGRESS) {
            if (g->busy)
                sg_progress_refresh(g);
        }
        return 0;

      case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        /* Wide enough for both panes' four columns plus the buttons. */
        mmi->ptMinTrackSize.x = 1024;
        mmi->ptMinTrackSize.y = 420;
        return 0;
      }

      case WM_SG_NOTIFY:
        if (!g)
            return 0;
        switch (wParam) {
          case SG_CH_SUBSYS_OK:
            if (g->scp) {
                /* The sentinel channel is up, so the connection is
                 * usable; start in the login directory. */
                g->subsys_ok = true;
                sg_scp_goto(g, NULL);
            } else {
                sg_send_init(g);
            }
            break;
          case SG_CH_SUBSYS_BAD:
            sg_fatal(g, "服务器拒绝启动 SFTP 子系统");
            break;
          case SG_CH_EXEC_DONE:
            sg_exec_done(g);
            break;
          case SG_CH_PUMP:
            if (g->pend == SG_P_SCP_UPLOAD && g->scp_state == SG_SU_DATA)
                sg_scp_ul_pump(g);
            break;
          case SG_CH_PICK_LOCAL: {
            char *p = g->pick;
            g->pick = NULL;
            if (p) {
                sg_local_goto(g, p);
                sfree(p);
            }
            break;
          }
          case SG_CH_PICK_REMOTE: {
            char *p = g->pick;
            g->pick = NULL;
            if (p) {
                sg_remote_goto(g, p);
                sfree(p);
            }
            break;
          }
          case SG_CH_RENAME: {
            char *name = g->ren_name;
            g->ren_name = NULL;
            if (name) {
                sg_ctx_rename(g, g->ren_local, g->ren_idx, name);
                sfree(name);
            }
            break;
          }
          case SG_CH_RENAME_EDIT:
            sg_start_label_edit(g, g->ren_local, g->ren_idx);
            break;
          case SG_CH_OPENFAILED: {
            char *text = (char *)lParam;
            sg_fatal(g, text);
            sfree(text);
            break;
          }
          case SG_CH_LOST:
          default:
            sg_fatal(g, g->scp ? "SCP 通道已关闭" : "SFTP 通道已关闭");
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

          case SG_ID_L_PATH:
            /* Refill the list every time it is opened, so a drive that
             * was plugged in a moment ago is already there. */
            if (HIWORD(wParam) == CBN_DROPDOWN) {
                sg_local_combo_build(g);
            } else if (HIWORD(wParam) == CBN_SELCHANGE) {
                int i = (int)SendMessageW(g->lcl.path, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < g->lcl.ncbpath) {
                    sfree(g->pick);
                    g->pick = dupstr(g->lcl.cbpath[i]);
                    /*
                     * Navigate from a posted message rather than here:
                     * the combo box overwrites the edit field with the
                     * item's display text after this notification
                     * returns, which would replace the path we set.
                     */
                    PostMessage(hwnd, WM_SG_NOTIFY, SG_CH_PICK_LOCAL, 0);
                }
            }
            return 0;

          case SG_ID_R_PATH:
            if (HIWORD(wParam) == CBN_DROPDOWN) {
                sg_remote_combo_build(g);
            } else if (HIWORD(wParam) == CBN_SELCHANGE) {
                int i = (int)SendMessageW(g->rmt.path, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < g->rmt.ncbpath) {
                    sfree(g->pick);
                    g->pick = dupstr(g->rmt.cbpath[i]);
                    PostMessage(hwnd, WM_SG_NOTIFY, SG_CH_PICK_REMOTE, 0);
                }
            }
            return 0;
        }
        break;

      case WM_CONTEXTMENU: {
        POINT pt;
        HWND lv = NULL;
        bool keyboard = (lParam == (LPARAM)-1);

        if (!g)
            break;
        if (keyboard) {
            /* Invoked from the keyboard: use the current selection. */
            GetCursorPos(&pt);
            lv = (GetFocus() == g->rmt.list) ? g->rmt.list : g->lcl.list;
        } else {
            pt.x = (int)(short)LOWORD(lParam);
            pt.y = (int)(short)HIWORD(lParam);
            for (int i = 0; i < 2 && !lv; i++) {
                HWND cand = i ? g->rmt.list : g->lcl.list;
                RECT rc;
                GetWindowRect(cand, &rc);
                if (PtInRect(&rc, pt))
                    lv = cand;
            }
        }
        if (!lv)
            break;
        SetFocus(lv);
        if (!keyboard && !sg_ctx_hit_test(lv, pt))
            return 0;                  /* clicked below the last row */
        sg_popup_menu(g, lv == g->lcl.list, pt.x, pt.y);
        return 0;
      }

      case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
        if (mis->CtlType == ODT_COMBOBOX) {
            mis->itemHeight = 20;
            return TRUE;
        }
        break;
      }

      case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        if (dis->CtlType == ODT_COMBOBOX &&
            (dis->CtlID == SG_ID_L_PATH || dis->CtlID == SG_ID_R_PATH)) {
            sg_combo_draw(dis);
            return TRUE;
        }
        break;
      }

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
        if (nm->code == LVN_BEGINLABELEDITW) {
            HWND edit;

            if (nm->hwndFrom != g->lcl.list && nm->hwndFrom != g->rmt.list)
                break;
            /* Make sure the edit box owns the keyboard and turns the
             * keys into characters: this window's message loop does not
             * translate them (see sg_type_key). */
            edit = (HWND)SendMessageW(nm->hwndFrom, LVM_GETEDITCONTROL, 0, 0);
            if (edit) {
                sg_watch_label_edit(edit);
                SetFocus(edit);
            }
            return FALSE;              /* allow the edit */
        }
        if (nm->code == LVN_ENDLABELEDITW) {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)lParam;

            if (nm->hwndFrom != g->lcl.list && nm->hwndFrom != g->rmt.list)
                break;
            sg_label_edit = NULL;      /* the edit box is about to go */
            if (di->item.pszText) {
                sfree(g->ren_name);
                g->ren_name = sg_to_utf8(di->item.pszText);
                g->ren_idx = di->item.iItem;
                g->ren_local = (nm->hwndFrom == g->lcl.list);
                PostMessage(hwnd, WM_SG_NOTIFY, SG_CH_RENAME, 0);
            }
            /* Refuse the edit: the pane is refreshed with the new name
             * once the rename has actually happened. */
            return FALSE;
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

/*
 * The window's caption: the session tab's label first, the way the
 * session's own window is captioned, so that the two can be told apart
 * in the taskbar. Caller frees.
 */
static wchar_t *sg_title(bool scp, const wchar_t *tab_label)
{
    wchar_t *base = sg_w(i18n_or(scp ? "SCP File Transfer"
                                    : "SFTP File Transfer"));
    size_t n = wcslen(base) + (tab_label ? wcslen(tab_label) : 0) + 8;
    wchar_t *title = snewn(n, wchar_t);

    if (tab_label && *tab_label)
        _snwprintf(title, n, L"%s - %s", tab_label, base);
    else
        _snwprintf(title, n, L"%s", base);
    title[n-1] = L'\0';
    sfree(base);
    return title;
}

void sftpgui_tab_label_changed(Backend *be, const wchar_t *tab_label)
{
    SftpGui *g = sg_instance;
    wchar_t *title;

    /*
     * Only the one transfer window can exist, and only the session that
     * opened it owns it: every other tab's re-caption is ignored here.
     * The label carries the tab's number, so reordering the tabs changes
     * it even when the session itself did not.
     */
    if (!g || !be || g->be != be)
        return;
    title = sg_title(g->scp, tab_label);
    SetWindowTextW(g->hwnd, title);
    sfree(title);
}

static void sg_open(HWND parent, Backend *be, bool scp,
                    const wchar_t *tab_label)
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
        wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON));
        wc.lpszClassName = SG_CLASSNAME;
        if (!RegisterClassW(&wc))
            return;
        registered = true;
    }

    sg_startup_error = NULL;
    sg_pending_scp = scp;

    title = sg_title(scp, tab_label);
    /*
     * The window is deliberately unowned. An owned window is kept on top
     * of its owner at all times and gets no taskbar button of its own,
     * so making the session window its owner left this window covering
     * the session and missing from the taskbar.
     */
    hwnd = CreateWindowExW(0, SG_CLASSNAME, title,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1200, 640,
                           NULL, NULL, inst, be);
    sfree(title);

    if (!hwnd) {
        sg_msgbox(parent,
                  sg_startup_error ? sg_startup_error :
                  "无法创建文件传输窗口", MB_ICONERROR | MB_OK);
        return;
    }
    sg_instance = (SftpGui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    /*
     * The title bar and the taskbar show the small icon; the .ico file
     * carries one, so ask for it at the size Windows wants rather than
     * letting the big icon be scaled down.
     */
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                 (LPARAM)LoadImageW(inst, MAKEINTRESOURCEW(IDI_MAINICON),
                                    IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0));
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    /* Now that it is an ordinary top-level window, bring it to the
     * front so it is not hidden behind the session it belongs to. */
    SetForegroundWindow(hwnd);
}

void sftp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label)
{
    sg_open(parent, be, false, tab_label);
}

void scp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label)
{
    sg_open(parent, be, true, tab_label);
}
