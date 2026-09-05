/* hwebc - drive a running hweb window from the shell.
 *
 * Sends one command line over the window's control socket
 * ($XDG_RUNTIME_DIR/hweb/<pid>.sock) and prints the JSON reply. The
 * command language is hweb's own (the same as the : prompt and stdin),
 * with chrome-dumper's grammar for the automation verbs:
 *
 *   hwebc [--win PID] [--timeout S] VERB [ARGS...]
 *   hwebc windows                 list windows (their `info` replies)
 *   hwebc [--win PID] < script    one command per line, one reply each
 *
 * Without --win (or $HWEB_WIN) the focused window is targeted, else the
 * most recently focused one. `hwebc open URL` with no window running
 * starts hweb. Exit status: 0 ok, 1 hweb replied an error, 2 usage or no
 * window, 3 cannot reach the window.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* verbs whose argument is the rest of the line, unquoted (kept in step
 * with the "raw" verbs in hweb.c) */
static const char *rawverbs[] = {"open",   "tab",  "private",  "js",
                                 "inject", "yank", "download", "prompt",
                                 "echo",   "find", NULL};
static double timeout = 60;

static void msleep(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static int isnum(const char *s) {
    char *e;
    if (!*s)
        return 0;
    strtol(s, &e, 10);
    return !*e;
}

/* the socket directory, sweeping sockets of dead windows */
static void sockdir(char *dst, size_t n) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    DIR *d;
    struct dirent *e;
    if (rt && *rt)
        snprintf(dst, n, "%s/hweb", rt);
    else
        snprintf(dst, n, "/tmp/hweb-%d", (int)getuid());
    if (!(d = opendir(dst)))
        return;
    while ((e = readdir(d))) {
        int pid;
        char p[600];
        if (sscanf(e->d_name, "%d.sock", &pid) == 1 && kill(pid, 0) < 0 &&
            errno == ESRCH) {
            snprintf(p, sizeof p, "%s/%s", dst, e->d_name);
            unlink(p);
        }
    }
    closedir(d);
}

/* pids of the windows that have a socket, most recent last */
static int windows(int *pids, int max) {
    char dir[300];
    DIR *d;
    struct dirent *e;
    int n = 0, pid;
    sockdir(dir, sizeof dir);
    if (!(d = opendir(dir)))
        return 0;
    while ((e = readdir(d)) && n < max)
        if (sscanf(e->d_name, "%d.sock", &pid) == 1)
            pids[n++] = pid;
    closedir(d);
    return n;
}

/* one round trip; the reply line lands in buf. -1: cannot connect,
 * -2: no reply in time */
static int query(int pid, const char *line, char *buf, size_t n, double secs) {
    struct sockaddr_un sa;
    struct timeval tv;
    char dir[300], path[400];
    ssize_t k, tot = 0;
    int fd;

    sockdir(dir, sizeof dir);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(path, sizeof path, "%s/%d.sock", dir, pid);
    if (strlen(path) >= sizeof sa.sun_path)
        return -1;
    memcpy(sa.sun_path, path, strlen(path) + 1);
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
        connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    tv.tv_sec = (time_t)secs;
    tv.tv_usec = (long)((secs - (double)tv.tv_sec) * 1e6);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (write(fd, line, strlen(line)) < 0 || write(fd, "\n", 1) < 0) {
        close(fd);
        return -1;
    }
    while (tot < (ssize_t)n - 1 && (k = read(fd, buf + tot, n - 1 - tot)) > 0) {
        tot += k;
        if (memchr(buf + tot - k, '\n', (size_t)k))
            break;
    }
    close(fd);
    buf[tot] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return tot ? 0 : -2;
}

/* a numeric or boolean field of a flat JSON object, 0 when absent */
static long long field(const char *json, const char *name) {
    char key[64];
    const char *p;
    snprintf(key, sizeof key, "\"%s\":", name);
    if (!(p = strstr(json, key)))
        return 0;
    p += strlen(key);
    if (!strncmp(p, "true", 4))
        return 1;
    return atoll(p);
}

/* the window to talk to: --win/$HWEB_WIN, else the focused window, else
 * the most recently focused, else the only one */
static int pickwin(const char *want) {
    int pids[256], n, i, best = 0;
    long long bestactive = -1;
    char buf[4096];
    if (want)
        return atoi(want);
    n = windows(pids, 256);
    if (n == 1)
        return pids[0];
    for (i = 0; i < n; i++) {
        if (query(pids[i], "info", buf, sizeof buf, 0.3) < 0)
            continue;
        if (field(buf, "focused"))
            return pids[i];
        if (field(buf, "active") > bestactive) {
            bestactive = field(buf, "active");
            best = pids[i];
        }
    }
    return best;
}

/* wait for a freshly spawned window's socket to answer */
static int waitwin(int pid) {
    char buf[4096];
    int i;
    for (i = 0; i < 100; i++) {
        if (query(pid, "info", buf, sizeof buf, 1) == 0)
            return 0;
        msleep(50);
    }
    return -1;
}

static void quote(char *o, size_t n, const char *s) {
    size_t i = 0;
    if (n < 3)
        return;
    o[i++] = '"';
    for (; *s && i + 3 < n; s++) {
        if (*s == '"' || *s == '\\')
            o[i++] = '\\';
        if (*s == '\n') {
            o[i++] = '\\';
            o[i++] = 'n';
        } else
            o[i++] = *s;
    }
    o[i++] = '"';
    o[i] = 0;
}

/* argv -> one command line hweb's tokenizer turns back into these args;
 * a raw verb gets its arguments joined verbatim */
static void buildline(char *line, size_t n, int argc, char **argv) {
    char tok[8192], cwd[4096], abs[8192];
    size_t len;
    int i, raw = 0;
    for (i = 0; rawverbs[i]; i++)
        if (!strcmp(argv[0], rawverbs[i]))
            raw = 1;
    snprintf(line, n, "%s", argv[0]);
    for (i = 1; i < argc; i++) {
        if (raw)
            snprintf(tok, sizeof tok, "%s", argv[i]);
        else if (i > 1 && !strcmp(argv[i - 1], "--out") && argv[i][0] != '/' &&
                 (!strcmp(argv[0], "screenshot") || !strcmp(argv[0], "dump")) &&
                 getcwd(cwd, sizeof cwd)) {
            /* hweb's cwd is not ours: output paths become absolute */
            snprintf(abs, sizeof abs, "%s/%s", cwd, argv[i]);
            quote(tok, sizeof tok, abs);
        } else
            quote(tok, sizeof tok, argv[i]);
        len = strlen(line);
        snprintf(line + len, n - len, " %s", tok);
    }
}

static char replybuf[1 << 20]; /* the last reply, for callers */

static int iserror(const char *reply) {
    return !strncmp(reply, "{\"type\":\"error\"", 15);
}

/* send one line to pid, print the reply; returns the exit status */
static int run(int pid, const char *line) {
    char *buf = replybuf;
    int k = query(pid, line, buf, sizeof replybuf, timeout);
    if (k == -1) {
        fprintf(stderr, "hwebc: cannot reach window %d\n", pid);
        return 3;
    }
    if (k == -2) {
        fprintf(stderr, "hwebc: window %d did not reply\n", pid);
        return 3;
    }
    puts(buf);
    if (iserror(buf))
        return 1;
    return 0;
}

static int spawnhweb(int argc, char **argv) {
    char **av = calloc((size_t)argc + 2, sizeof *av);
    int pids[256], before[256], n, nb, i, j, pid = 0, tries;
    av[0] = "hweb";
    for (i = 1; i < argc; i++)
        av[i] = argv[i];
    nb = windows(before, 256);
    switch (fork()) {
    case -1:
        return 3;
    case 0:
        setsid();
        execvp(av[0], av);
        _exit(127);
    }
    /* the new window's socket appears within a moment */
    for (tries = 0; tries < 100 && !pid; tries++) {
        msleep(50);
        n = windows(pids, 256);
        for (i = 0; i < n && !pid; i++) {
            for (j = 0; j < nb && before[j] != pids[i]; j++)
                ;
            if (j == nb)
                pid = pids[i];
        }
    }
    free(av);
    if (!pid) {
        fprintf(stderr, "hwebc: hweb did not start\n");
        return 3;
    }
    waitwin(pid);
    printf("{\"type\":\"opened\",\"pid\":%d}\n", pid);
    return 0;
}

static void usage(void) {
    fputs("usage: hwebc [--win PID] [--timeout S] VERB [ARGS...]\n"
          "       hwebc windows\n"
          "       hwebc [--win PID] < script\n",
          stderr);
    exit(2);
}

int main(int argc, char **argv) {
    const char *want = getenv("HWEB_WIN");
    char line[65536], buf[4096];
    int pids[256], n, i, pid, rc = 0;

    while (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--win") && argc > 2) {
            want = argv[2];
            argv += 2, argc -= 2;
        } else if (!strcmp(argv[1], "--timeout") && argc > 2) {
            timeout = atof(argv[2]);
            argv += 2, argc -= 2;
        } else if (!strcmp(argv[1], "--help")) {
            usage();
        } else
            break;
    }
    if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "-h"))) {
        if (argv[1][1] == 'v') {
            puts("hwebc " HWEB_VERSION);
            return 0;
        }
        usage();
    }
    if (want && (!isnum(want) || !*want))
        usage();

    if (argc > 1 && !strcmp(argv[1], "windows")) {
        n = windows(pids, 256);
        for (i = 0; i < n; i++)
            if (query(pids[i], "info", buf, sizeof buf, 1) == 0)
                puts(buf);
        return 0;
    }
    if (argc == 1 && isatty(0))
        usage();

    pid = pickwin(want);
    if (!pid) {
        if (argc > 1 && !strcmp(argv[1], "open"))
            return spawnhweb(argc, argv);
        fprintf(stderr, "hwebc: no hweb window\n");
        return 2;
    }
    if (argc == 1) { /* script mode */
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line && run(pid, line))
                rc = 1;
        }
        return rc;
    }
    buildline(line, sizeof line, argc - 1, argv + 1);
    rc = run(pid, line);
    /* a new window: wait until it can be addressed */
    if (!rc && (!strcmp(argv[1], "tab") || !strcmp(argv[1], "private")))
        waitwin((int)field(replybuf, "pid"));
    return rc;
}
