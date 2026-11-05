/* pty.h — the process side: forkpty, the child exec, SIGWINCH-ish
 * resize (TIOCSWINSZ), and the drain/read helpers. Pure POSIX;
 * login-shell and working-directory options per the profile. */
#ifndef FDKTERM_PTY_H
#define FDKTERM_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct pty {
    int master;     /* -1 when not running                     */
    pid_t pid;
    char *cwd;      /* the last OSC 7-reported directory        */
} pty;

/* Spawns `shell` (argv[0]-style basename) with the profile options.
 * shell==NULL probes $SHELL then /bin/sh. argv/env are built here:
 * TERM=xterm-256color, COLORTERM=truecolor, the window size set.
 * login=true prepends '-' to argv[0] and cds to $HOME first when
 * cwd is NULL. Returns false (errno set) on any failure. */
bool pty_spawn(pty *p, const char *shell, const char *cwd, bool login,
               int cols, int rows);

/* The master fd (-1 when dead). */
int pty_fd(const pty *p);

/* Reads up to cap bytes (non-blocking); returns -1 on EOF/error
 * (child gone), else the count (0 = nothing right now). */
int pty_read(pty *p, char *buf, size_t cap);

/* Writes n bytes (blocking-ish loop over partial writes). */
bool pty_write(pty *p, const char *buf, size_t n);

/* Window size change. */
void pty_resize(const pty *p, int cols, int rows);

/* True when the child has exited (waitpid WNOHANG). */
bool pty_child_gone(pty *p);

/* Reaps and closes; safe to call twice. */
void pty_shutdown(pty *p);

#endif /* FDKTERM_PTY_H */
