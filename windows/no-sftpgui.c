/*
 * Stub for builds without the SSH backend (PuTTYtel): there is no
 * connection to browse, so the SFTP window can never be opened.
 */

#include "putty.h"
#include "sftpgui.h"

void sftp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label)
{
    (void)parent;
    (void)be;
    (void)tab_label;
}

void scp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label)
{
    (void)parent;
    (void)be;
    (void)tab_label;
}

void sftpgui_tab_label_changed(Backend *be, const wchar_t *tab_label)
{
    (void)be;
    (void)tab_label;
}
