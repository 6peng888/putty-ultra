/*
 * i18n.c - platform-neutral storage of UI translation tables.
 *
 * The table maps an English source string to its translation and is
 * built once at startup by the platform loader (see i18n.h). Lookup
 * is a binary search over a sorted array; an English string with no
 * entry simply stays as it is, so translation data is always optional
 * and can be incomplete.
 */

#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "i18n.h"

struct i18n_entry {
    char *key;      /* UTF-8 English source text */
    char *value;    /* UTF-8 translation */
};

static struct i18n_entry *entries;
static size_t nentries, entries_size;
static bool initialised;

static int entry_cmp(const void *av, const void *bv)
{
    const struct i18n_entry *a = (const struct i18n_entry *)av;
    const struct i18n_entry *b = (const struct i18n_entry *)bv;
    return strcmp(a->key, b->key);
}

void i18n_add_entry(const char *key, const char *value)
{
    if (nentries >= entries_size) {
        entries_size = entries_size ? entries_size * 2 : 256;
        entries = sresize(entries, entries_size, struct i18n_entry);
    }
    entries[nentries].key = dupstr(key);
    entries[nentries].value = dupstr(value);
    nentries++;
}

void i18n_entries_done(void)
{
    if (nentries > 1)
        qsort(entries, nentries, sizeof(*entries), entry_cmp);
}

void i18n_init(void)
{
    if (initialised)
        return;
    initialised = true;

#ifdef _WIN32
    platform_i18n_load();
#endif
}

const char *i18n_lookup(const char *english)
{
    struct i18n_entry key;
    struct i18n_entry *found;

    if (!english || !nentries)
        return NULL;

    key.key = (char *)english;
    found = bsearch(&key, entries, nentries, sizeof(*entries), entry_cmp);
    return found ? found->value : NULL;
}

const char *i18n_or(const char *english)
{
    const char *translated = i18n_lookup(english);
    return translated ? translated : english;
}
