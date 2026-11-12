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
 * window that owns the new one.
 *
 * Only one such window can exist per process, because ssh/sftp.c keeps
 * its request table in globals; a second call just re-activates the
 * window that is already open.
 */
void sftp_gui_open(HWND parent, Backend *be);
void scp_gui_open(HWND parent, Backend *be);

#endif /* PUTTY_WIN_SFTPGUI_H */
