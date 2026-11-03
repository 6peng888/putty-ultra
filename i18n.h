/*
 * i18n.h - run-time translation of user-interface strings.
 *
 * The English text in the UI code doubles as the lookup key: the
 * translation tables map the exact English string to its localised
 * equivalent. Untranslated strings fall through unchanged, so a
 * partial translation is always usable and English remains the safe
 * default.
 *
 * The table itself is platform-neutral (utils/i18n.c); each platform
 * provides a loader (windows/i18n.c reads a text file next to the
 * executable).
 */

#ifndef PUTTY_I18N_H
#define PUTTY_I18N_H

/*
 * Load the translation table for the user's language. Called once at
 * startup; harmless (and silent) if no translation data is available.
 */
void i18n_init(void);

/*
 * Look up the translation of `english'. Returns NULL if there is no
 * translation, or the translated text as a UTF-8 string owned by the
 * table (never freed by the caller).
 */
const char *i18n_lookup(const char *english);

/*
 * Convenience wrapper: the translation if there is one, else the
 * original string. Suitable for printf format strings and anywhere a
 * non-NULL result is required.
 */
const char *i18n_or(const char *english);

/*
 * Internal API, used by the platform loaders to build the table.
 */
void i18n_add_entry(const char *key, const char *value);
void i18n_entries_done(void);

#ifdef _WIN32
/* Platform loader, implemented in windows/i18n.c. */
void platform_i18n_load(void);

/*
 * As i18n_lookup, but converted to a wide string for handing to the
 * Win32 API (which cannot display UTF-8). Returns NULL if the string
 * is untranslated; the result must be freed with sfree().
 */
wchar_t *i18n_lookup_w(const char *english);
#endif

#endif /* PUTTY_I18N_H */
