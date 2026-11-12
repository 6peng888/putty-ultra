/*
 * Stub for builds without the SSH backend (PuTTYtel): there is no
 * connection to browse, so the SFTP window can never be opened.
 */

#include "putty.h"
#include "sftpgui.h"

void sftp_gui_open(HWND parent, Backend *be)
{
    (void)parent;
    (void)be;
}
