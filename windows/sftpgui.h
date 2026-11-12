/*
 * sftpgui.h - the Windows GUI's SFTP file transfer window.
 */

#ifndef PUTTY_WIN_SFTPGUI_H
#define PUTTY_WIN_SFTPGUI_H

/*
 * Open the two-pane SFTP browser for the SSH connection that 'be'
 * belongs to. The window reuses that connection (it opens an extra
 * channel on it and starts the "sftp" subsystem), so no new login is
 * needed. 'parent' is the window that owns the new one.
 *
 * Only one such window can exist per process, because ssh/sftp.c keeps
 * its request table in globals; a second call just re-activates the
 * window that is already open.
 */
void sftp_gui_open(HWND parent, Backend *be);

#endif /* PUTTY_WIN_SFTPGUI_H */
