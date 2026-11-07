/* open.c — fdk_desktop_open_uri/_path (see include/fdk/fdk_desktop.h).
 *
 * The XDG opener: probe xdg-open on the PATH, then fork+setsid+
 * exec, never waiting. The validation ladder is deliberately
 * paranoid — the string reaches an exec ARGV, not a shell, but the
 * no-leading-dash and no-control-bytes rules keep even a hostile
 * string from becoming someone else's option parsing problem. */
#include "fdk/fdk_desktop.h"
#include "fdk/fdk_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Is `cmd` executable somewhere on $PATH (X_OK)? */
static bool xdg_open_available(void) {
    const char *path = getenv("PATH");
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    const char *p = path;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t dirlen = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (dirlen > 0) {
            char probe[1024];
            if (dirlen < sizeof probe - sizeof "/xdg-open") {
                memcpy(probe, p, dirlen);
                probe[dirlen] = '\0';
                strcat(probe, "/xdg-open");
                if (access(probe, X_OK) == 0) {
                    return true;
                }
            }
        }
        p = colon != NULL ? colon + 1 : p + dirlen;
    }
    return false;
}

/* The shared validation: scheme-prefixed or absolute-path, printable
 * ASCII-or-UTF-8 bytes only, no leading dash. Returns true when the
 * string is safe to hand to xdg-open. */
static bool uri_validate(const char *uri) {
    if (uri == NULL || uri[0] == '\0') {
        return false;
    }
    if (uri[0] == '-') {
        return false; /* option injection */
    }
    /* no control bytes anywhere */
    for (const unsigned char *u = (const unsigned char *)uri; *u != 0;
         u++) {
        if (*u < 0x20 || *u == 0x7F) {
            return false;
        }
    }
    /* scheme? [A-Za-z][A-Za-z0-9+.-]*: */
    size_t i = 0;
    if (isalpha((unsigned char)uri[0])) {
        i = 1;
        while (uri[i] != '\0' &&
               (isalnum((unsigned char)uri[i]) || uri[i] == '+' ||
                uri[i] == '.' || uri[i] == '-')) {
            i++;
        }
        if (uri[i] == ':') {
            return true;
        }
    }
    /* absolute path */
    if (uri[0] == '/') {
        return true;
    }
    return false;
}

/* percent-encodes path into out (cap bytes) as file:// URI. */
static bool path_to_file_uri(const char *path, char *out, size_t cap) {
    size_t o = 0;
    const char *prefix = "file://";
    for (const char *s = prefix; *s != '\0'; s++) {
        if (o + 1 >= cap) {
            return false;
        }
        out[o++] = *s;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p != 0;
         p++) {
        bool safe = isalnum(*p) || *p == '/' || *p == '-' || *p == '_' ||
                    *p == '.' || *p == '~' || *p == '+';
        if (safe) {
            if (o + 1 >= cap) {
                return false;
            }
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= cap) {
                return false;
            }
            static const char hex[] = "0123456789ABCDEF";
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0xF];
        }
    }
    out[o] = '\0';
    return true;
}

/* The detached launch. The double-fork shape (setsid in the child)
 * reaps the intermediate so no zombie lingers on the UI process. */
static fdk_result uri_launch(const char *uri) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        return FDK_ERR_PLATFORM;
    }
    if (pid == 0) {
        /* child: own session, quiet stdio, exec the opener */
        (void)setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) {
                close(devnull);
            }
        }
        for (int fd = 3; fd < 1024; fd++) {
            (void)close(fd);
        }
        execlp("xdg-open", "xdg-open", uri, (char *)NULL);
        _exit(127);
    }
    /* parent: reap the immediate child (the session leader detached
     * above survives it; the opener's lifetime is the desktop's) */
    int status = 0;
    (void)waitpid(pid, &status, 0);
    return FDK_OK;
}

fdk_result fdk_desktop_open_uri(const char *uri) {
    if (!uri_validate(uri)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!xdg_open_available()) {
        return FDK_ERR_UNSUPPORTED;
    }
    return uri_launch(uri);
}

fdk_result fdk_desktop_open_path(const char *path) {
    if (path == NULL || path[0] != '/') {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!uri_validate(path)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!xdg_open_available()) {
        return FDK_ERR_UNSUPPORTED;
    }
    char uri[2048];
    if (!path_to_file_uri(path, uri, sizeof uri)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return uri_launch(uri);
}
