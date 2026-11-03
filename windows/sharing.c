/*
 * Windows implementation of SSH connection-sharing IPC setup.
 */

#include <stdio.h>
#include <assert.h>

#include "tree234.h"
#include "putty.h"
#include "network.h"
#include "proxy/proxy.h"
#include "ssh.h"

#include "cryptoapi.h"
#include "security-api.h"

#define CONNSHARE_PIPE_PREFIX "\\\\.\\pipe\\putty-connshare"
#define CONNSHARE_MUTEX_PREFIX "Local\\putty-connshare-mutex"

static char *make_name(const char *prefix, const char *name)
{
    char *username, *retname;

    username = get_username();
    retname = dupprintf("%s.%s.%s", prefix, username, name);
    sfree(username);

    return retname;
}

char *platform_ssh_share_new_listener(Plug *upplug, Socket **sock_out,
                                      char **err)
{
    static unsigned long counter;
    char *suffix, *obf, *pipename;
    Socket *ret;

    *err = NULL;

    /*
     * The name only needs to be unique among this process's listening
     * sockets: the name is handed directly (in-process) to the
     * downstream that targets it, and the counter is never reused.
     */
    suffix = dupprintf("dedicated.%lu.%lu", GetCurrentProcessId(),
                       ++counter);
    obf = capi_obfuscate_string(suffix);
    sfree(suffix);
    if (!obf) {
        *err = dupprintf("Unable to call CryptProtectMemory: %s",
                         win_strerror(GetLastError()));
        return NULL;
    }
    pipename = make_name(CONNSHARE_PIPE_PREFIX, obf);
    sfree(obf);

    ret = new_named_pipe_listener(pipename, upplug);
    if (sk_socket_error(ret)) {
        *err = dupstr(sk_socket_error(ret));
        sk_close(ret);
        sfree(pipename);
        return NULL;
    }
    *sock_out = ret;
    return pipename;
}

int platform_ssh_share(const char *pi_name, Conf *conf,
                       Plug *downplug, Plug *upplug, Socket **sock,
                       char **logtext, char **ds_err, char **us_err,
                       bool can_upstream, bool can_downstream)
{
    char *name, *mutexname, *pipename;
    HANDLE mutex;
    Socket *retsock;

    /*
     * Directed attach: the caller (in-process "Duplicate channel")
     * knows the exact listening socket of the upstream it wants, so
     * try only that, as downstream only, and skip the mutex dance
     * since we will never want to become an upstream ourselves.
     */
    const char *dedpipe = conf_get_str(conf, CONF_ssh_share_pipe);
    if (dedpipe && *dedpipe) {
        *logtext = NULL;
        *ds_err = NULL;
        *us_err = NULL;
        if (can_downstream) {
            retsock = new_named_pipe_client(dedpipe, downplug);
            if (sk_socket_error(retsock) == NULL) {
                *logtext = dupstr(dedpipe);
                *sock = retsock;
                return SHARE_DOWNSTREAM;
            }
            *ds_err = dupprintf("%s: %s", dedpipe, sk_socket_error(retsock));
            sk_close(retsock);
        }
        return SHARE_NONE;
    }

    /*
     * Transform the platform-independent version of the connection
     * identifier into the obfuscated version we'll use for our
     * Windows named pipe and mutex. A side effect of doing this is
     * that it also eliminates any characters illegal in Windows pipe
     * names.
     */
    name = capi_obfuscate_string(pi_name);
    if (!name) {
        *logtext = dupprintf("Unable to call CryptProtectMemory: %s",
                             win_strerror(GetLastError()));
        return SHARE_NONE;
    }

    /*
     * Make a mutex name out of the connection identifier, and lock it
     * while we decide whether to be upstream or downstream.
     */
    mutexname = make_name(CONNSHARE_MUTEX_PREFIX, name);
    mutex = lock_interprocess_mutex(mutexname, logtext);
    if (!mutex) {
        sfree(mutexname);
        sfree(name);
        return SHARE_NONE;
    }

    pipename = make_name(CONNSHARE_PIPE_PREFIX, name);

    *logtext = NULL;

    if (can_downstream) {
        retsock = new_named_pipe_client(pipename, downplug);
        if (sk_socket_error(retsock) == NULL) {
            sfree(*logtext);
            *logtext = pipename;
            *sock = retsock;
            sfree(name);
            unlock_interprocess_mutex(mutex);
            return SHARE_DOWNSTREAM;
        }
        sfree(*ds_err);
        *ds_err = dupprintf("%s: %s", pipename, sk_socket_error(retsock));
        sk_close(retsock);
    }

    if (can_upstream) {
        retsock = new_named_pipe_listener(pipename, upplug);
        if (sk_socket_error(retsock) == NULL) {
            sfree(*logtext);
            *logtext = pipename;
            *sock = retsock;
            sfree(name);
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            return SHARE_UPSTREAM;
        }
        sfree(*us_err);
        *us_err = dupprintf("%s: %s", pipename, sk_socket_error(retsock));
        sk_close(retsock);
    }

    /* One of the above clauses ought to have happened. */
    assert(*logtext || *ds_err || *us_err);

    sfree(pipename);
    sfree(name);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return SHARE_NONE;
}

void platform_ssh_share_cleanup(const char *name)
{
}
