/*
 * Stub used by programs built without the SSH backend (PuTTYtel,
 * pterm). The GUI calls backend_get_dedicated_share_pipe when running
 * "Duplicate channel", which can only ever succeed for SSH, so without
 * SSH the answer is always NULL (and the menu item is greyed out
 * anyway, since the active session's backend protocol isn't SSH).
 */

#include "putty.h"

char *backend_get_dedicated_share_pipe(Backend *be)
{
    (void)be;
    return NULL;
}
