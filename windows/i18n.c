/*
 * i18n.c - Windows loader for UI translation tables.
 *
 * The translation data deliberately lives outside the binary, in a
 * UTF-8 text file found next to the executable:
 *
 *     putty-i18n-zh_CN.txt    one file per language
 *     putty-i18n.txt          fallback name, whatever its language
 *
 * The language is taken from the PUTTY_LANG environment variable
 * (e.g. "zh_CN"), or else from the user's default UI language. If no
 * file matches, the UI stays in English - a partial or missing
 * translation never breaks anything, it just leaves text untranslated.
 *
 * Each non-comment line maps one English source string to its
 * translation:
 *
 *     English source text=translated text
 *
 * '#' at the very start of a line introduces a comment; blank lines
 * are ignored. The escapes \n, \r, \t and \\ are understood on both
 * sides. Both strings are stripped of leading and trailing spaces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "i18n.h"

/* Trim leading and trailing ASCII spaces in place; returns new start. */
static char *trim_spaces(char *s)
{
    size_t len;

    while (*s == ' ' || *s == '\t')
        s++;
    len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t'))
        s[--len] = '\0';
    return s;
}

/* Decode \n, \r, \t and \\ in place. */
static void unescape(char *s)
{
    char *p = s, *q = s;

    while (*p) {
        if (*p == '\\') {
            switch (p[1]) {
              case 'n': *q++ = '\n'; p += 2; continue;
              case 'r': *q++ = '\r'; p += 2; continue;
              case 't': *q++ = '\t'; p += 2; continue;
              case '\\': *q++ = '\\'; p += 2; continue;
              default: break;
            }
        }
        *q++ = *p++;
    }
    *q = '\0';
}

static void parse_table(char *body)
{
    char *p = body;

    /* Skip a UTF-8 byte order mark if the editor left one. */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        p += 3;

    while (*p) {
        char *nl = strchr(p, '\n');
        char *line = p;
        size_t len;
        char *eq;

        if (nl)
            *nl = '\0';
        len = strlen(line);
        if (len > 0 && line[len-1] == '\r')
            line[len-1] = '\0';

        if (*line && *line != '#' && (eq = strchr(line, '=')) != NULL) {
            char *key, *value;
            *eq = '\0';
            key = trim_spaces(line);
            value = trim_spaces(eq + 1);
            if (*key && *value) {
                unescape(key);
                unescape(value);
                i18n_add_entry(key, value);
            }
        }

        if (!nl)
            break;
        p = nl + 1;
    }
}

static bool load_file(const wchar_t *path)
{
    HANDLE h;
    strbuf *sb;
    char *body;
    char buf[4096];
    DWORD len;

    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    sb = strbuf_new();
    while (ReadFile(h, buf, sizeof(buf), &len, NULL) && len > 0)
        put_data(sb, buf, len);
    CloseHandle(h);

    body = strbuf_to_str(sb);          /* NUL-terminated */
    parse_table(body);
    sfree(body);

    return true;
}

/* Compose the language tag for this user, e.g. "zh_CN". */
static void get_language_tag(char *out, size_t outsize)
{
    const char *env = getenv("PUTTY_LANG");

    out[0] = '\0';
    if (env && *env && strlen(env) < outsize) {
        /* Normalise RFC-style "zh-CN" to the file-name form "zh_CN". */
        size_t i;
        for (i = 0; env[i] && i < outsize - 1; i++)
            out[i] = (env[i] == '-') ? '_' : env[i];
        out[i] = '\0';
        return;
    }

    {
        LANGID lid = GetUserDefaultUILanguage();
        wchar_t lang[16], ctry[16];

        if (GetLocaleInfoW(lid, LOCALE_SISO639LANGNAME, lang, lenof(lang)) &&
            GetLocaleInfoW(lid, LOCALE_SISO3166CTRYNAME, ctry, lenof(ctry))) {
            /*
             * These names are ASCII by definition, so copy them
             * character by character rather than depending on the C
             * library's wide-format support.
             */
            size_t i, o = 0;
            for (i = 0; lang[i] && o + 2 < outsize; i++)
                if (lang[i] < 128)
                    out[o++] = (char)lang[i];
            if (o + 2 < outsize)
                out[o++] = '_';
            for (i = 0; ctry[i] && o + 1 < outsize; i++)
                if (ctry[i] < 128)
                    out[o++] = (char)ctry[i];
            out[o] = '\0';
        }
    }
}

wchar_t *i18n_lookup_w(const char *english)
{
    const char *translated = i18n_lookup(english);
    if (!translated)
        return NULL;
    return dup_mb_to_wc(CP_UTF8, translated);
}

void platform_i18n_load(void)
{
    wchar_t path[MAX_PATH];
    wchar_t dir[MAX_PATH];
    char lang[64];
    DWORD n;
    bool loaded = false;

    /* Directory containing the executable. */
    n = GetModuleFileNameW(NULL, dir, lenof(dir));
    if (n == 0 || n >= lenof(dir))
        return;
    while (n > 0 && dir[n-1] != L'\\')
        n--;
    dir[n ? n - 1 : 0] = L'\0';

    get_language_tag(lang, sizeof(lang));

    if (lang[0]) {
        size_t i;
        _snwprintf(path, lenof(path), L"%s\\putty-i18n-", dir);
        path[lenof(path)-1] = L'\0';
        for (i = 0; lang[i]; i++) {
            size_t l = wcslen(path);
            if (l + 6 >= lenof(path))
                break;
            path[l] = (wchar_t)(unsigned char)lang[i];
            path[l+1] = L'\0';
        }
        {
            size_t l = wcslen(path);
            if (l + 5 < lenof(path))
                wcscat(path, L".txt");
        }
        loaded = load_file(path);
    }

    if (!loaded) {
        _snwprintf(path, lenof(path), L"%s\\putty-i18n.txt", dir);
        path[lenof(path)-1] = L'\0';
        load_file(path);
    }

    i18n_entries_done();
}
