/* pty.c — see pty.h. forkpty + exec, the works. */
#define _GNU_SOURCE 1
#include "term_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

bool pty_spawn(pty *p, const char *shell, const char *cwd, bool login,
               int cols, int rows) {
    memset(p, 0, sizeof *p);
    p->master = -1;

    if (shell == NULL || shell[0] == '\0') {
        shell = getenv("SHELL");
        if (shell == NULL || shell[0] == '\0') {
            shell = "/bin/sh";
        }
    }

    /* the basename for argv[0] (login shells get the '-' prefix) */
    const char *slash = strrchr(shell, '/');
    const char *base = slash != NULL ? slash + 1 : shell;
    char argv0[128];
    if (login) {
        snprintf(argv0, sizeof argv0, "-%s", base);
    } else {
        snprintf(argv0, sizeof argv0, "%s", base);
    }

    struct winsize ws = {
        .ws_col = (unsigned short)(cols > 0 ? cols : 80),
        .ws_row = (unsigned short)(rows > 0 ? rows : 24),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        /* child */
        const char *dir = cwd != NULL && cwd[0] != '\0' ? cwd
                         : (login ? getenv("HOME") : NULL);
        if (dir != NULL) {
            chdir(dir);
        }
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("LINES");
        unsetenv("COLUMNS");
        char *const argv[] = {argv0, NULL};
        char *const envp[] = {NULL};
        extern char **environ;
        (void)envp;
        execve(shell, argv, environ);
        _exit(127);
    }
    /* parent: non-blocking master */
    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }
    p->master = master;
    p->pid = pid;
    return true;
}

int pty_fd(const pty *p) {
    return p->master;
}

int pty_read(pty *p, char *buf, size_t cap) {
    if (p->master < 0) {
        return -1;
    }
    ssize_t n = read(p->master, buf, cap);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return 0;
        }
        return -1; /* EIO: child closed its side */
    }
    return (int)n;
}

bool pty_write(pty *p, const char *buf, size_t n) {
    if (p->master < 0) {
        return false;
    }
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(p->master, buf + off, n - off);
        if (w < 0) {
            if (errno == EAGAIN) {
                /* the master is full: a brief yield-and-retry loop;
                 * a terminal that stays full >1s is a stuck child */
                struct timespec ts = {0, 2000000}; /* 2ms */
                nanosleep(&ts, NULL);
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

void pty_resize(const pty *p, int cols, int rows) {
    if (p->master < 0 || cols <= 0 || rows <= 0) {
        return;
    }
    struct winsize ws = {
        .ws_col = (unsigned short)cols,
        .ws_row = (unsigned short)rows,
    };
    (void)ioctl(p->master, TIOCSWINSZ, &ws);
}

bool pty_child_gone(pty *p) {
    if (p->pid <= 0) {
        return true;
    }
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    return r == p->pid;
}

void pty_shutdown(pty *p) {
    if (p->master >= 0) {
        close(p->master);
        p->master = -1;
    }
    if (p->pid > 0) {
        int status = 0;
        (void)waitpid(p->pid, &status, 0);
        p->pid = -1;
    }
    free(p->cwd);
    p->cwd = NULL;
}
