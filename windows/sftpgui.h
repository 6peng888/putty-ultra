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
 * Open the transfer window for the SSH connection that 'be' belongs to,
 * using the SFTP subsystem (open_sftp) or plain SCP (open_scp). No new
 * login is needed either way. 'parent' is the window that reports
 * errors; 'tab_label' is the session tab's caption, which becomes the
 * caption of the tab this session gets in the transfer window.
 *
 * There is one transfer window, and it has a tab per session, each tab
 * holding that session's own two-pane view. Asking again for a session
 * brings its tab to the front. Each tab keeps its own SFTP request table
 * (see sftp_client_new), so transfers in different tabs do not disturb
 * each other.
 */
void sftp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label);
void scp_gui_open(HWND parent, Backend *be, const wchar_t *tab_label);

/*
 * Called when a session's tab label changes - reordering the tabs
 * renumbers them, and the number is part of the label. If that session
 * has a tab in the transfer window, it takes the new label. The tab
 * itself stays where the user put it.
 */
void sftpgui_tab_label_changed(Backend *be, const wchar_t *tab_label);

#endif /* PUTTY_WIN_SFTPGUI_H */
