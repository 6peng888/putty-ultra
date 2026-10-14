/*
 * storage.c: Windows-specific implementation of the interface
 * defined in storage.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include "putty.h"
#include "storage.h"

#include <shlobj.h>
#ifndef CSIDL_APPDATA
#define CSIDL_APPDATA 0x001a
#endif
#ifndef CSIDL_LOCAL_APPDATA
#define CSIDL_LOCAL_APPDATA 0x001c
#endif

static const char *const reg_jumplist_key = PUTTY_REG_POS "\\Jumplist";
static const char *const reg_jumplist_value = "Recent sessions";
static const char *const reg_sessions_key = PUTTY_REG_POS "\\Sessions";
static const char *const host_ca_key = PUTTY_REG_POS "\\SshHostCAs";

static bool tried_shgetfolderpath = false;
static HMODULE shell32_module = NULL;
DECL_WINDOWS_FUNCTION(static, HRESULT, SHGetFolderPathA,
                      (HWND, int, HANDLE, DWORD, LPSTR));

/* ======================================================================
 * Saved sessions.
 *
 * Saved sessions are stored as files rather than as Registry keys, so
 * that they can be organised into folders. The arrangement is
 *
 *   %APPDATA%\PuTTY\sessions\            one file per saved session
 *       spark-4-211                      the session "spark-4-211"
 *       prod\web-01                      the session "prod/web-01"
 *
 * so a '/' in a session name denotes a folder, which is represented on
 * disk by a subdirectory of that name.
 *
 * Each session file is a plain text file of `keyword=value' lines; see
 * put_setting_line() for the details of the encoding. File names are
 * escaped, because session names were historically Registry key names
 * and so may contain characters that Windows file names can't (and
 * because Windows silently discards a trailing '.' or space).
 * ====================================================================== */

#define SESSIONS_SUBDIR   "PuTTY\\sessions"
#define SESSIONS_MARKER   "PuTTY\\.sessions-migrated"
#define SESSIONS_TEMPFILE "PuTTY\\.sessions-saving"

static void migrate_registry_sessions_if_needed(void);
static void migrate_registry_session(HKEY parent, const char *subname,
                                     const char *sessionname);

/*
 * Find the user's roaming Application Data directory, where we keep
 * saved sessions.
 */
static const char *appdata_dir(void)
{
    static bool tried = false;
    static char *cached = NULL;

    if (!tried) {
        char path[MAX_PATH + 1];
        DWORD len;

        tried = true;

        /*
         * %APPDATA% normally names the same directory as
         * CSIDL_APPDATA, so prefer the environment variable, and fall
         * back to the shell's idea of the user's profile if it isn't
         * set for some reason.
         */
        len = GetEnvironmentVariable("APPDATA", path, lenof(path));
        if (len > 0 && len < lenof(path))
            return cached = dupstr(path);

        if (!tried_shgetfolderpath) {
            shell32_module = load_system32_dll("shell32.dll");
            GET_WINDOWS_FUNCTION(shell32_module, SHGetFolderPathA);
            tried_shgetfolderpath = true;
        }
        if (p_SHGetFolderPathA &&
            SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL,
                                         SHGFP_TYPE_CURRENT, path)))
            cached = dupstr(path);
    }

    return cached;
}

/* Return a path of the form %APPDATA%\<subpath>, or NULL. */
static char *appdata_subpath(const char *subpath)
{
    const char *dir = appdata_dir();
    if (!dir)
        return NULL;
    return dupcat(dir, "\\", subpath);
}

static bool path_is_dir(const char *path)
{
    DWORD attrs = GetFileAttributes(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool path_exists(const char *path)
{
    return GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES;
}

/* Create one directory, tolerating the case where it already exists. */
static bool ensure_dir(const char *path)
{
    if (CreateDirectory(path, NULL))
        return true;
    return path_is_dir(path);
}

/* Create a directory and all of its parents. */
static bool create_dir_tree(const char *path)
{
    char *copy = dupstr(path);
    char *start = copy;
    char *p;
    bool ok = true;

    /* Leave a leading drive specification such as "C:" alone. */
    if (start[0] && start[1] == ':')
        start += 2;

    for (p = start; *p; p++) {
        if (*p != '\\' || p == start || p[-1] == '\\')
            continue;
        *p = '\0';
        if (!ensure_dir(copy))
            ok = false;
        *p = '\\';
    }

    if (!ensure_dir(copy))
        ok = false;

    sfree(copy);
    return ok;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static bool ascii_caseeq(const char *a, const char *b, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z')
            ca -= 'a' - 'A';
        if (cb >= 'a' && cb <= 'z')
            cb -= 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return true;
}

/*
 * True if this name component can't be used as a file name as it
 * stands: "." and "..", and the reserved DOS device names (which are
 * special even with an extension, e.g. "con.txt").
 */
static bool is_reserved_dos_name(const char *in, size_t len)
{
    static const char *const names[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
        "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
        "LPT8", "LPT9",
    };
    size_t base = 0, i;

    if ((len == 1 && in[0] == '.') || (len == 2 && in[0] == '.' &&
                                       in[1] == '.'))
        return true;

    while (base < len && in[base] != '.')
        base++;

    for (i = 0; i < lenof(names); i++)
        if (strlen(names[i]) == base && ascii_caseeq(in, names[i], base))
            return true;

    return false;
}

static void put_escaped_byte(strbuf *out, unsigned char c)
{
    static const char hex[] = "0123456789ABCDEF";
    put_byte(out, '%');
    put_byte(out, hex[c >> 4]);
    put_byte(out, hex[c & 15]);
}

/* Escape one component of a session name, so it can be a file name. */
static void escape_name_component(const char *in, size_t len, strbuf *out)
{
    size_t i;

    /*
     * Escape the first character of anything that a file name can't be
     * given verbatim, so that no component can collide with "." or a
     * reserved device name.
     */
    if (is_reserved_dos_name(in, len)) {
        put_escaped_byte(out, (unsigned char)in[0]);
        put_data(out, in + 1, len - 1);
        return;
    }

    for (i = 0; i < len; i++) {
        unsigned char c = in[i];
        if (c == '%' || c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            c < 0x20 || (i + 1 == len && (c == '.' || c == ' '))) {
            put_escaped_byte(out, c);
        } else {
            put_byte(out, c);
        }
    }
}

static void unescape_name_component(const char *in, size_t len, strbuf *out)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (in[i] == '%' && i + 2 < len) {
            int hi = hexval(in[i+1]), lo = hexval(in[i+2]);
            if (hi >= 0 && lo >= 0) {
                put_byte(out, (hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        put_byte(out, in[i]);
    }
}

/*
 * The folder in which saved sessions live. Creating it (and importing
 * any old Registry-based sessions into it) is done on the first call.
 */
static const char *sessions_root(void)
{
    static bool tried = false;
    static char *cached = NULL;

    if (!tried) {
        tried = true;
        cached = appdata_subpath(SESSIONS_SUBDIR);
        if (cached) {
            create_dir_tree(cached);
            migrate_registry_sessions_if_needed();
        }
    }

    return cached;
}

/*
 * Translate a session name such as "prod/web-01" into the path of the
 * file which stores it. Returns NULL, and fills in *errmsg if it isn't
 * NULL, if the name can't be represented as a set of file names.
 */
static char *session_name_to_path(const char *name, char **errmsg)
{
    const char *root = sessions_root();
    strbuf *sb;
    const char *p;

    if (!root) {
        if (errmsg)
            *errmsg = dupstr("Unable to find the folder in which to save "
                             "sessions:\nno %APPDATA% directory is available");
        return NULL;
    }

    sb = strbuf_new();
    put_data(sb, root, strlen(root));

    for (p = name; ; ) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0) {
            if (errmsg)
                *errmsg = dupprintf("Invalid session name \"%s\":\n"
                                    "folder names must not be empty", name);
            strbuf_free(sb);
            return NULL;
        }

        put_byte(sb, '\\');
        escape_name_component(p, len, sb);

        if (!slash)
            break;
        p = slash + 1;
    }

    return strbuf_to_str(sb);
}

/* Create the folder which will contain a session file. */
static bool create_parent_dirs(const char *path)
{
    char *copy = dupstr(path);
    char *slash = strrchr(copy, '\\');
    bool ok = true;

    if (slash && slash > copy && slash[-1] != ':') {
        *slash = '\0';
        ok = create_dir_tree(copy);
    }

    sfree(copy);
    return ok;
}

/*
 * The body of a session file is a sequence of `keyword=value' lines,
 * with the value stored exactly as it is - the same format used by
 * PuTTY on other platforms, so that session files can be copied
 * between them.
 */
static void put_setting_line(strbuf *sb, const char *key, const char *value)
{
    put_data(sb, key, strlen(key));
    put_byte(sb, '=');
    put_data(sb, value, strlen(value));
    put_byte(sb, '\n');
}

static char *lookup_setting(const char *data, const char *key)
{
    size_t keylen = strlen(key);
    const char *p = data;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);

        /* Tolerate files which have been through a CRLF conversion. */
        while (linelen > 0 && p[linelen-1] == '\r')
            linelen--;

        if (linelen > keylen && p[keylen] == '=' &&
            !memcmp(p, key, keylen)) {
            strbuf *sb = strbuf_new();
            put_data(sb, p + keylen + 1, linelen - keylen - 1);
            return strbuf_to_str(sb);
        }

        if (!eol)
            break;
        p = eol + 1;
    }

    return NULL;
}

struct settings_w {
    char *path;
    strbuf *body;
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    char *path;
    settings_w *handle;

    *errmsg = NULL;

    if (!sessionname || !*sessionname)
        sessionname = "Default Settings";

    path = session_name_to_path(sessionname, errmsg);
    if (!path)
        return NULL;

    if (path_is_dir(path)) {
        *errmsg = dupprintf("Unable to save session \"%s\":\n"
                            "a folder of that name already exists",
                            sessionname);
        sfree(path);
        return NULL;
    }

    if (!create_parent_dirs(path)) {
        char *dir = dupstr(path);
        *strrchr(dir, '\\') = '\0';
        *errmsg = dupprintf("Unable to create the folder for session \"%s\":\n"
                            "%s\n%s", sessionname, dir,
                            win_strerror(GetLastError()));
        sfree(dir);
        sfree(path);
        return NULL;
    }

    handle = snew(settings_w);
    handle->path = path;
    handle->body = strbuf_new();
    return handle;
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
    if (handle)
        put_setting_line(handle->body, key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    if (handle) {
        char *str = dupprintf("%d", value);
        put_setting_line(handle->body, key, str);
        sfree(str);
    }
}

void close_settings_w(settings_w *handle)
{
    /*
     * Write the new session to a temporary file and then rename it into
     * place, so that an interrupted write can't leave a half-written
     * session behind.
     */
    char *tmp = appdata_subpath(SESSIONS_TEMPFILE);
    bool ok = false;

    if (tmp) {
        HANDLE f = CreateFile(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ok = WriteFile(f, handle->body->s, handle->body->len,
                           &written, NULL) &&
                written == handle->body->len;
            CloseHandle(f);
            if (ok) {
                ok = MoveFileEx(tmp, handle->path, MOVEFILE_REPLACE_EXISTING);
            }
            if (!ok)
                DeleteFile(tmp);
        }
        sfree(tmp);
    }

    if (!ok)
        nonfatal("Unable to save the session to\n%s\n%s",
                 handle->path, win_strerror(GetLastError()));

    strbuf_free(handle->body);
    sfree(handle->path);
    sfree(handle);
}

struct settings_r {
    char *body;                        /* contents of the session file */
};

settings_r *open_settings_r(const char *sessionname)
{
    char *path;
    HANDLE f;
    strbuf *sb;
    settings_r *handle;
    char buf[4096];
    DWORD len;

    if (!sessionname || !*sessionname)
        sessionname = "Default Settings";

    path = session_name_to_path(sessionname, NULL);
    if (!path)
        return NULL;

    f = CreateFile(path, GENERIC_READ,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    sfree(path);

    if (f == INVALID_HANDLE_VALUE)
        return NULL;                   /* no such session */

    sb = strbuf_new();
    while (ReadFile(f, buf, sizeof(buf), &len, NULL) && len > 0)
        put_data(sb, buf, len);
    CloseHandle(f);

    handle = snew(settings_r);
    handle->body = strbuf_to_str(sb);
    return handle;
}

char *read_setting_s(settings_r *handle, const char *key)
{
    if (!handle)
        return NULL;
    return lookup_setting(handle->body, key);
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    char *val;
    int ret;

    if (!handle)
        return defvalue;

    val = lookup_setting(handle->body, key);
    if (!val)
        return defvalue;

    ret = (int)strtol(val, NULL, 10);
    sfree(val);
    return ret;
}

FontSpec *read_setting_fontspec(settings_r *handle, const char *name)
{
    char *settingname;
    char *fontname;
    FontSpec *ret;
    int isbold, height, charset;

    fontname = read_setting_s(handle, name);
    if (!fontname)
        return NULL;

    settingname = dupcat(name, "IsBold");
    isbold = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (isbold == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "CharSet");
    charset = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (charset == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "Height");
    height = read_setting_i(handle, settingname, INT_MIN);
    sfree(settingname);
    if (height == INT_MIN) {
        sfree(fontname);
        return NULL;
    }

    ret = fontspec_new(fontname, isbold, height, charset);
    sfree(fontname);
    return ret;
}

void write_setting_fontspec(settings_w *handle,
                            const char *name, FontSpec *font)
{
    char *settingname;

    write_setting_s(handle, name, font->name);
    settingname = dupcat(name, "IsBold");
    write_setting_i(handle, settingname, font->isbold);
    sfree(settingname);
    settingname = dupcat(name, "CharSet");
    write_setting_i(handle, settingname, font->charset);
    sfree(settingname);
    settingname = dupcat(name, "Height");
    write_setting_i(handle, settingname, font->height);
    sfree(settingname);
}

Filename *read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
        sfree(tmp);
        return ret;
    } else
        return NULL;
}

void write_setting_filename(settings_w *handle,
                            const char *name, Filename *result)
{
    /*
     * When saving a session involving a Filename, we use the 'cpath'
     * member of the Filename structure, because otherwise we break
     * backwards compatibility with existing saved sessions.
     *
     * This means that 'exotic' filenames - those including Unicode
     * characters outside the host system's CP_ACP default code page -
     * cannot be represented faithfully, and saving and reloading a
     * Conf including one will break it.
     *
     * This can't be fixed without breaking backwards compatibility,
     * and if we're going to break compatibility then we should break
     * it good and hard (the Nanny Ogg principle), and devise a
     * completely fresh storage representation that fixes as many
     * other legacy problems as possible at the same time.
     */
    write_setting_s(handle, name, result->cpath); /* FIXME */
}

void close_settings_r(settings_r *handle)
{
    if (handle) {
        sfree(handle->body);
        sfree(handle);
    }
}

/*
 * True if a directory (which we're thinking of deleting) has nothing
 * in it at all.
 */
static bool dir_is_empty(const char *dir)
{
    char *pattern = dupcat(dir, "\\*");
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    bool empty = true;

    sfree(pattern);

    if (h == INVALID_HANDLE_VALUE)
        return true;

    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
            empty = false;
            break;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
    return empty;
}

/*
 * Delete any folders that a deletion has left empty, up to but not
 * including the folder that holds all the sessions.
 */
static void prune_empty_dirs(const char *filepath)
{
    const char *root = sessions_root();
    char *dir, *slash;

    if (!root)
        return;

    dir = dupstr(filepath);
    slash = strrchr(dir, '\\');
    if (!slash) {
        sfree(dir);
        return;
    }
    *slash = '\0';

    while (strlen(dir) > strlen(root) && dir_is_empty(dir)) {
        if (!RemoveDirectory(dir))
            break;
        slash = strrchr(dir, '\\');
        if (!slash)
            break;
        *slash = '\0';
    }

    sfree(dir);
}

void del_settings(const char *sessionname)
{
    char *path = session_name_to_path(sessionname, NULL);

    if (!path)
        return;

    DeleteFile(path);
    prune_empty_dirs(path);
    sfree(path);

    remove_session_from_jumplist(sessionname);
}

struct settings_e {
    char **names;                      /* names of all the sessions */
    int nnames;
    int i;
};

static void add_session_name(char ***names, int *nnames, size_t *size,
                             const char *name)
{
    sgrowarray(*names, *size, *nnames);
    (*names)[(*nnames)++] = dupstr(name);
}

/*
 * Find all the session files at or below a directory. `prefix' holds
 * the session-name prefix of that directory, with '/' separators.
 */
static void enum_session_files(const char *dir, strbuf *prefix,
                               char ***names, int *nnames, size_t *size)
{
    char *pattern = dupcat(dir, "\\*");
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);

    sfree(pattern);

    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        size_t prefixlen = prefix->len;
        char *subdir;

        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;

        if (prefixlen)
            put_byte(prefix, '/');
        unescape_name_component(fd.cFileName, strlen(fd.cFileName), prefix);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subdir = dupcat(dir, "\\", fd.cFileName);
            enum_session_files(subdir, prefix, names, nnames, size);
            sfree(subdir);
        } else {
            add_session_name(names, nnames, size, prefix->s);
        }

        strbuf_shrink_to(prefix, prefixlen);
    } while (FindNextFile(h, &fd));

    FindClose(h);
}

settings_e *enum_settings_start(void)
{
    const char *root = sessions_root();
    settings_e *e;
    strbuf *prefix;
    size_t size = 0;

    if (!root)
        return NULL;

    e = snew(settings_e);
    e->names = NULL;
    e->nnames = e->i = 0;

    prefix = strbuf_new();
    enum_session_files(root, prefix, &e->names, &e->nnames, &size);
    strbuf_free(prefix);

    return e;
}

bool enum_settings_next(settings_e *e, strbuf *sb)
{
    const char *name;

    if (!e || e->i >= e->nnames)
        return false;

    name = e->names[e->i++];
    put_data(sb, name, strlen(name));
    return true;
}

void enum_settings_finish(settings_e *e)
{
    int i;

    if (!e)
        return;

    for (i = 0; i < e->nnames; i++)
        sfree(e->names[i]);
    sfree(e->names);
    sfree(e);
}

/*
 * Copy a session out of the Registry and into a file, and then record
 * that we've done that, so we never do it again unless the marker file
 * is deliberately deleted.
 */
static void migrate_registry_session(HKEY parent, const char *subname,
                                     const char *sessionname)
{
    HKEY key = open_regkey_ro(parent, subname);
    char *errmsg = NULL;
    settings_w *handle;
    int i;

    if (!key)
        return;

    handle = open_settings_w(sessionname, &errmsg);
    sfree(errmsg);

    if (!handle) {
        close_regkey(key);
        return;
    }

    for (i = 0; ; i++) {
        strbuf *vname = strbuf_new(), *vdata = strbuf_new();
        DWORD type;

        if (!enum_regvalue(key, i, vname, &type, vdata)) {
            strbuf_free(vname);
            strbuf_free(vdata);
            break;
        }

        if (type == REG_SZ) {
            write_setting_s(handle, vname->s, vdata->s);
        } else if (type == REG_DWORD && vdata->len >= 4) {
            DWORD value;
            memcpy(&value, vdata->s, 4);
            write_setting_i(handle, vname->s, (int)value);
        }

        strbuf_free(vname);
        strbuf_free(vdata);
    }

    close_settings_w(handle);
    close_regkey(key);
}

static void migrate_registry_sessions_if_needed(void)
{
    char *marker = appdata_subpath(SESSIONS_MARKER);
    HKEY key;
    int i;

    if (!marker)
        return;

    if (path_exists(marker)) {
        sfree(marker);
        return;
    }

    /*
     * Nothing is deleted from the Registry, so the old Registry-based
     * sessions survive as a backup.
     */
    key = open_regkey_ro(HKEY_CURRENT_USER, reg_sessions_key);
    if (key) {
        for (i = 0; ; i++) {
            char *subname = enum_regkey(key, i);
            strbuf *name;

            if (!subname)
                break;

            name = strbuf_new();
            unescape_registry_key(subname, name);
            migrate_registry_session(key, subname, name->s);
            strbuf_free(name);
            sfree(subname);
        }
        close_regkey(key);
    }

    {
        HANDLE f = CreateFile(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE)
            CloseHandle(f);
    }

    sfree(marker);
}

/* Delete a directory and everything in it. */
static void remove_dir_recursive(const char *dir)
{
    char *pattern = dupcat(dir, "\\*");
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);

    sfree(pattern);

    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *sub;
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            sub = dupcat(dir, "\\", fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_dir_recursive(sub);
            else
                DeleteFile(sub);
            sfree(sub);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }

    RemoveDirectory(dir);
}

static void hostkey_regname(strbuf *sb, const char *hostname,
                            int port, const char *keytype)
{
    put_fmt(sb, "%s@%d:", keytype, port);
    escape_registry_key(hostname, sb);
}

int check_stored_host_key(const char *hostname, int port,
                          const char *keytype, const char *key)
{
    /*
     * Read a saved key in from the registry and see what it says.
     */
    strbuf *regname = strbuf_new();
    hostkey_regname(regname, hostname, port, keytype);

    HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER,
                               PUTTY_REG_POS "\\SshHostKeys");
    if (!rkey) {
        strbuf_free(regname);
        return 1;                      /* key does not exist in registry */
    }

    char *otherstr = get_reg_sz(rkey, regname->s);
    if (!otherstr && !strcmp(keytype, "rsa")) {
        /*
         * Key didn't exist. If the key type is RSA, we'll try
         * another trick, which is to look up the _old_ key format
         * under just the hostname and translate that.
         */
        char *justhost = regname->s + 1 + strcspn(regname->s, ":");
        char *oldstyle = get_reg_sz(rkey, justhost);

        if (oldstyle) {
            /*
             * The old format is two old-style bignums separated by
             * a slash. An old-style bignum is made of groups of
             * four hex digits: digits are ordered in sensible
             * (most to least significant) order within each group,
             * but groups are ordered in silly (least to most)
             * order within the bignum. The new format is two
             * ordinary C-format hex numbers (0xABCDEFG...XYZ, with
             * A nonzero except in the special case 0x0, which
             * doesn't appear anyway in RSA keys) separated by a
             * comma. All hex digits are lowercase in both formats.
             */
            strbuf *new = strbuf_new();
            const char *q = oldstyle;
            int i, j;

            for (i = 0; i < 2; i++) {
                int ndigits, nwords;
                put_datapl(new, PTRLEN_LITERAL("0x"));
                ndigits = strcspn(q, "/");      /* find / or end of string */
                nwords = ndigits / 4;
                /* now trim ndigits to remove leading zeros */
                while (q[(ndigits - 1) ^ 3] == '0' && ndigits > 1)
                    ndigits--;
                /* now move digits over to new string */
                for (j = ndigits; j-- > 0 ;)
                    put_byte(new, q[j ^ 3]);
                q += nwords * 4;
                if (*q) {
                    q++;                 /* eat the slash */
                    put_byte(new, ',');  /* add a comma */
                }
            }

            /*
             * Now _if_ this key matches, we'll enter it in the new
             * format. If not, we'll assume something odd went
             * wrong, and hyper-cautiously do nothing.
             */
            if (!strcmp(new->s, key)) {
                put_reg_sz(rkey, regname->s, new->s);
                otherstr = strbuf_to_str(new);
            } else {
                strbuf_free(new);
            }
        }

        sfree(oldstyle);
    }

    close_regkey(rkey);

    int compare = otherstr ? strcmp(otherstr, key) : -1;

    sfree(otherstr);
    strbuf_free(regname);

    if (!otherstr)
        return 1;                      /* key does not exist in registry */
    else if (compare)
        return 2;                      /* key is different in registry */
    else
        return 0;                      /* key matched OK in registry */
}

bool have_ssh_host_key(const char *hostname, int port,
                       const char *keytype)
{
    /*
     * If we have a host key, check_stored_host_key will return 0 or 2.
     * If we don't have one, it'll return 1.
     */
    return check_stored_host_key(hostname, port, keytype, "") != 1;
}

void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key)
{
    strbuf *regname = strbuf_new();
    hostkey_regname(regname, hostname, port, keytype);

    HKEY rkey = create_regkey(HKEY_CURRENT_USER,
                              PUTTY_REG_POS "\\SshHostKeys");
    if (rkey) {
        put_reg_sz(rkey, regname->s, key);
        close_regkey(rkey);
    } /* else key does not exist in registry */

    strbuf_free(regname);
}

struct host_ca_enum {
    HKEY key;
    int i;
};

host_ca_enum *enum_host_ca_start(void)
{
    host_ca_enum *e;
    HKEY key;

    if (!(key = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key)))
        return NULL;

    e = snew(host_ca_enum);
    e->key = key;
    e->i = 0;

    return e;
}

bool enum_host_ca_next(host_ca_enum *e, strbuf *sb)
{
    char *regbuf = enum_regkey(e->key, e->i);
    if (!regbuf)
        return false;

    unescape_registry_key(regbuf, sb);
    sfree(regbuf);
    e->i++;
    return true;
}

void enum_host_ca_finish(host_ca_enum *e)
{
    close_regkey(e->key);
    sfree(e);
}

host_ca *host_ca_load(const char *name)
{
    strbuf *sb;
    const char *s;

    sb = strbuf_new();
    escape_registry_key(name, sb);
    HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key, sb->s);
    strbuf_free(sb);

    if (!rkey)
        return NULL;

    host_ca *hca = host_ca_new();
    hca->name = dupstr(name);

    DWORD val;

    if ((s = get_reg_sz(rkey, "PublicKey")) != NULL)
        hca->ca_public_key = base64_decode_sb(ptrlen_from_asciz(s));

    if ((s = get_reg_sz(rkey, "Validity")) != NULL) {
        hca->validity_expression = strbuf_to_str(
            percent_decode_sb(ptrlen_from_asciz(s)));
    } else if ((sb = get_reg_multi_sz(rkey, "MatchHosts")) != NULL) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sb));
        CertExprBuilder *eb = cert_expr_builder_new();

        const char *wc;
        while (wc = get_asciz(src), !get_err(src))
            cert_expr_builder_add(eb, wc);

        hca->validity_expression = cert_expr_expression(eb);
        cert_expr_builder_free(eb);
    }

    if (get_reg_dword(rkey, "PermitRSASHA1", &val))
        hca->opts.permit_rsa_sha1 = val;
    if (get_reg_dword(rkey, "PermitRSASHA256", &val))
        hca->opts.permit_rsa_sha256 = val;
    if (get_reg_dword(rkey, "PermitRSASHA512", &val))
        hca->opts.permit_rsa_sha512 = val;

    close_regkey(rkey);
    return hca;
}

char *host_ca_save(host_ca *hca)
{
    if (!*hca->name)
        return dupstr("CA record must have a name");

    strbuf *sb = strbuf_new();
    escape_registry_key(hca->name, sb);
    HKEY rkey = create_regkey(HKEY_CURRENT_USER, host_ca_key, sb->s);
    if (!rkey) {
        char *err = dupprintf("Unable to create registry key\n"
                              "HKEY_CURRENT_USER\\%s\\%s", host_ca_key, sb->s);
        strbuf_free(sb);
        return err;
    }
    strbuf_free(sb);

    strbuf *base64_pubkey = base64_encode_sb(
        ptrlen_from_strbuf(hca->ca_public_key), 0);
    put_reg_sz(rkey, "PublicKey", base64_pubkey->s);
    strbuf_free(base64_pubkey);

    strbuf *validity = percent_encode_sb(
        ptrlen_from_asciz(hca->validity_expression), NULL);
    put_reg_sz(rkey, "Validity", validity->s);
    strbuf_free(validity);

    put_reg_dword(rkey, "PermitRSASHA1", hca->opts.permit_rsa_sha1);
    put_reg_dword(rkey, "PermitRSASHA256", hca->opts.permit_rsa_sha256);
    put_reg_dword(rkey, "PermitRSASHA512", hca->opts.permit_rsa_sha512);

    close_regkey(rkey);
    return NULL;
}

char *host_ca_delete(const char *name)
{
    HKEY rkey = open_regkey_rw(HKEY_CURRENT_USER, host_ca_key);
    if (!rkey)
        return NULL;

    strbuf *sb = strbuf_new();
    escape_registry_key(name, sb);
    del_regkey(rkey, sb->s);
    strbuf_free(sb);

    return NULL;
}

/*
 * Open (or delete) the random seed file.
 */
enum { DEL, OPEN_R, OPEN_W };
static bool try_random_seed(char const *path, int action, HANDLE *ret)
{
    if (action == DEL) {
        if (!DeleteFile(path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            nonfatal("Unable to delete '%s': %s", path,
                     win_strerror(GetLastError()));
        }
        *ret = INVALID_HANDLE_VALUE;
        return false;                  /* so we'll do the next ones too */
    }

    *ret = CreateFile(path,
                      action == OPEN_W ? GENERIC_WRITE : GENERIC_READ,
                      action == OPEN_W ? 0 : (FILE_SHARE_READ |
                                              FILE_SHARE_WRITE),
                      NULL,
                      action == OPEN_W ? CREATE_ALWAYS : OPEN_EXISTING,
                      action == OPEN_W ? FILE_ATTRIBUTE_NORMAL : 0,
                      NULL);

    return (*ret != INVALID_HANDLE_VALUE);
}

static bool try_random_seed_and_free(char *path, int action, HANDLE *hout)
{
    bool retd = try_random_seed(path, action, hout);
    sfree(path);
    return retd;
}

static HANDLE access_random_seed(int action)
{
    HANDLE rethandle;

    /*
     * Iterate over a selection of possible random seed paths until
     * we find one that works.
     *
     * We do this iteration separately for reading and writing,
     * meaning that we will automatically migrate random seed files
     * if a better location becomes available (by reading from the
     * best location in which we actually find one, and then
     * writing to the best location in which we can _create_ one).
     */

    /*
     * First, try the location specified by the user in the
     * Registry, if any.
     */
    {
        HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER, PUTTY_REG_POS);
        if (rkey) {
            char *regpath = get_reg_sz(rkey, "RandSeedFile");
            close_regkey(rkey);
            if (regpath) {
                bool success = try_random_seed(regpath, action, &rethandle);
                sfree(regpath);
                if (success)
                    return rethandle;
            }
        }
    }

    /*
     * Next, try the user's local Application Data directory,
     * followed by their non-local one. This is found using the
     * SHGetFolderPath function, which won't be present on all
     * versions of Windows.
     */
    if (!tried_shgetfolderpath) {
        /* This is likely only to bear fruit on systems with IE5+
         * installed, or WinMe/2K+. There is some faffing with
         * SHFOLDER.DLL we could do to try to find an equivalent
         * on older versions of Windows if we cared enough.
         * However, the invocation below requires IE5+ anyway,
         * so stuff that. */
        shell32_module = load_system32_dll("shell32.dll");
        GET_WINDOWS_FUNCTION(shell32_module, SHGetFolderPathA);
        tried_shgetfolderpath = true;
    }
    if (p_SHGetFolderPathA) {
        char profile[MAX_PATH + 1];
        if (SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA,
                                         NULL, SHGFP_TYPE_CURRENT, profile)) &&
            try_random_seed_and_free(dupcat(profile, "\\PUTTY.RND"),
                                     action, &rethandle))
            return rethandle;

        if (SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_APPDATA,
                                         NULL, SHGFP_TYPE_CURRENT, profile)) &&
            try_random_seed_and_free(dupcat(profile, "\\PUTTY.RND"),
                                     action, &rethandle))
            return rethandle;
    }

    /*
     * Failing that, try %HOMEDRIVE%%HOMEPATH% as a guess at the
     * user's home directory.
     */
    {
        char drv[MAX_PATH], path[MAX_PATH];

        DWORD drvlen = GetEnvironmentVariable("HOMEDRIVE", drv, sizeof(drv));
        DWORD pathlen = GetEnvironmentVariable("HOMEPATH", path, sizeof(path));

        /* We permit %HOMEDRIVE% to expand to an empty string, but if
         * %HOMEPATH% does that, we abort the attempt. Same if either
         * variable overflows its buffer. */
        if (drvlen == 0)
            drv[0] = '\0';

        if (drvlen < lenof(drv) && pathlen < lenof(path) && pathlen > 0 &&
            try_random_seed_and_free(
                dupcat(drv, path, "\\PUTTY.RND"), action, &rethandle))
            return rethandle;
    }

    /*
     * And finally, fall back to C:\WINDOWS.
     */
    {
        char windir[MAX_PATH];
        DWORD len = GetWindowsDirectory(windir, sizeof(windir));
        if (len < lenof(windir) &&
            try_random_seed_and_free(
                dupcat(windir, "\\PUTTY.RND"), action, &rethandle))
            return rethandle;
    }

    /*
     * If even that failed, give up.
     */
    return INVALID_HANDLE_VALUE;
}

void read_random_seed(noise_consumer_t consumer)
{
    HANDLE seedf = access_random_seed(OPEN_R);

    if (seedf != INVALID_HANDLE_VALUE) {
        while (1) {
            char buf[1024];
            DWORD len;

            if (ReadFile(seedf, buf, sizeof(buf), &len, NULL) && len)
                consumer(buf, len);
            else
                break;
        }
        CloseHandle(seedf);
    }
}

void write_random_seed(void *data, int len)
{
    HANDLE seedf = access_random_seed(OPEN_W);

    if (seedf != INVALID_HANDLE_VALUE) {
        DWORD lenwritten;

        WriteFile(seedf, data, len, &lenwritten, NULL);
        CloseHandle(seedf);
    }
}

/*
 * Internal function supporting the jump list registry code. All the
 * functions to add, remove and read the list have substantially
 * similar content, so this is a generalisation of all of them which
 * transforms the list in the registry by prepending 'add' (if
 * non-null), removing 'rem' from what's left (if non-null), and
 * returning the resulting concatenated list of strings in 'out' (if
 * non-null).
 */
static int transform_jumplist_registry(
    const char *add, const char *rem, char **out)
{
    HKEY rkey = create_regkey(HKEY_CURRENT_USER, reg_jumplist_key);
    if (!rkey)
        return JUMPLISTREG_ERROR_KEYOPENCREATE_FAILURE;

    /* Get current list of saved sessions in the registry. */
    strbuf *oldlist = get_reg_multi_sz(rkey, reg_jumplist_value);
    if (!oldlist) {
        /* Start again with the empty list. */
        oldlist = strbuf_new();
        put_data(oldlist, "\0\0", 2);
    }

    /*
     * Modify the list, if we're modifying.
     */
    bool write_failure = false;
    if (add || rem) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(oldlist));
        strbuf *newlist = strbuf_new();

        /* First add the new item to the beginning of the list. */
        if (add)
            put_asciz(newlist, add);

        /* Now add the existing list, taking care to leave out the removed
         * item, if it was already in the existing list. */
        while (true) {
            const char *olditem = get_asciz(src);
            if (get_err(src))
                break;

            if (!rem || strcmp(olditem, rem) != 0) {
                /* Check if this is a valid session, otherwise don't add. */
                settings_r *psettings_tmp = open_settings_r(olditem);
                if (psettings_tmp != NULL) {
                    close_settings_r(psettings_tmp);
                    put_asciz(newlist, olditem);
                }
            }
        }

        /* Save the new list to the registry. */
        write_failure = !put_reg_multi_sz(rkey, reg_jumplist_value, newlist);

        strbuf_free(oldlist);
        oldlist = newlist;
    }

    close_regkey(rkey);

    if (out && !write_failure)
        *out = strbuf_to_str(oldlist);
    else
        strbuf_free(oldlist);

    if (write_failure)
        return JUMPLISTREG_ERROR_VALUEWRITE_FAILURE;
    else
        return JUMPLISTREG_OK;
}

/* Adds a new entry to the jumplist entries in the registry. */
int add_to_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(item, item, NULL);
}

/* Removes an item from the jumplist entries in the registry. */
int remove_from_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(NULL, item, NULL);
}

/* Returns the jumplist entries from the registry. Caller must free
 * the returned pointer. */
char *get_jumplist_registry_entries (void)
{
    char *list_value;

    if (transform_jumplist_registry(NULL,NULL,&list_value) != JUMPLISTREG_OK) {
        list_value = snewn(2, char);
        *list_value = '\0';
        *(list_value + 1) = '\0';
    }
    return list_value;
}

/*
 * Recursively delete a registry key and everything under it.
 */
static void registry_recursive_remove(HKEY key)
{
    char *name;

    DWORD i = 0;
    while ((name = enum_regkey(key, i)) != NULL) {
        HKEY subkey = open_regkey_rw(key, name);
        if (subkey) {
            registry_recursive_remove(subkey);
            close_regkey(subkey);
        }
        del_regkey(key, name);
        sfree(name);
    }
}

void cleanup_all(void)
{
    /* ------------------------------------------------------------
     * Wipe out the random seed file, in all of its possible
     * locations.
     */
    access_random_seed(DEL);

    /* ------------------------------------------------------------
     * Ask Windows to delete any jump list information associated
     * with this installation of PuTTY.
     */
    clear_jumplist();

    /* ------------------------------------------------------------
     * Delete the files holding the saved sessions, and the marker
     * recording that we've already imported the old Registry-based
     * sessions.
     */
    {
        char *path = appdata_subpath(SESSIONS_SUBDIR);
        if (path) {
            remove_dir_recursive(path);
            sfree(path);
        }
        path = appdata_subpath(SESSIONS_TEMPFILE);
        if (path) {
            DeleteFile(path);
            sfree(path);
        }
        path = appdata_subpath(SESSIONS_MARKER);
        if (path) {
            DeleteFile(path);
            sfree(path);
        }
    }

    /* ------------------------------------------------------------
     * Destroy all registry information associated with PuTTY.
     */

    /*
     * Open the main PuTTY registry key and remove everything in it.
     */
    HKEY key = open_regkey_rw(HKEY_CURRENT_USER, PUTTY_REG_POS);
    if (key) {
        registry_recursive_remove(key);
        close_regkey(key);
    }
    /*
     * Now open the parent key and remove the PuTTY main key. Once
     * we've done that, see if the parent key has any other
     * children.
     */
    if ((key = open_regkey_rw(HKEY_CURRENT_USER, PUTTY_REG_PARENT)) != NULL) {
        del_regkey(key, PUTTY_REG_PARENT_CHILD);
        char *name = enum_regkey(key, 0);
        close_regkey(key);

        /*
         * If the parent key had no other children, we must delete
         * it in its turn. That means opening the _grandparent_
         * key.
         */
        if (name) {
            sfree(name);
        } else {
            if ((key = open_regkey_rw(HKEY_CURRENT_USER,
                                      PUTTY_REG_GPARENT)) != NULL) {
                del_regkey(key, PUTTY_REG_GPARENT_CHILD);
                close_regkey(key);
            }
        }
    }
    /*
     * Now we're done.
     */
}
