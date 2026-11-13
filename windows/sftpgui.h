/*
 * sftpgui.h - the Windows GUI's file transfer window.
 *
 * The same window serves both transfer protocols: SFTP, which is the
 * richer and preferred one, and SCP, which is used when the server has
 * no SFTP subsystem. In both cases the window reuses an existing
 * session's SSH connection instead of dialling a new one.
 */

#ifndef PUTTY_WIN_SFTPGUI_H
#define PUTTY_WIN_SFTPGUI_H

/*
 * Open the two-pane transfer window for the SSH connection that 'be'
 * belongs to, using the SFTP subsystem (open_sftp) or plain SCP
 * (open_scp). No new login is needed either way. 'parent' is the
 * window that reports errors; 'tab_label' is the session tab's caption
 * (as drawn by the tab strip), which is put in the window's title so
 * the two windows can be told apart in the taskbar. It may be NULL.
 *
 * There is one window per session: asking again for a session brings its
 * window to the front, and a different session gets a window of its own.
 * Each window keeps its own SFTP request table (see sftp_client_new), so
 * transfers in different windows do not disturb each other.
 */
void sftp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label);
void scp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label);

/*
 * Called when a session's tab label changes - reordering the tabs
 * renumbers them, and the number is part of the label. If the transfer
 * window belongs to 'be', it takes the new label as its caption.
 */
void sftpgui_tab_label_changed(Backend *be, const wchar_t *tab_label);

#endif /* PUTTY_WIN_SFTPGUI_H */
