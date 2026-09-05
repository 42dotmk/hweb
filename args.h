/* args.h - command line tokenizer and --flag helpers for hweb's command
 * language. Included by hweb.c only.
 *
 * A line is split shell-style: whitespace separates tokens, "..." keeps
 * spaces and understands \" \\ \n \t, '...' is literal, \x outside quotes
 * escapes x. argv[0] is the verb; `rest` is the text after the verb
 * verbatim, for verbs whose argument is free text (open, js, echo...).
 * Flags are --name value or --name=value; a verb's boolean flags (which
 * take no value) are listed in `bools` so positionals can be told apart. */
#ifndef ARGS_H
#define ARGS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char buf[65536]; /* the tokens, NUL separated */
    char raw[65536]; /* copy of the line, for rest */
    char *argv[128];
    int argc;          /* -1: unterminated quote */
    const char *rest;  /* after the verb and its spaces */
    const char *bools; /* "wait,submit,..." for the dispatched verb */
} Args;

/* split line into a->argv; returns argc, -1 on an unterminated quote */
static int argsparse(Args *a, const char *line) {
    const char *s = line;
    char *o = a->buf, *end = a->buf + sizeof a->buf - 1;
    int q;

    a->argc = 0;
    a->bools = "";
    snprintf(a->raw, sizeof a->raw, "%s", line);
    while (*s == ' ' || *s == '\t')
        s++;
    while (*s && a->argc < (int)(sizeof a->argv / sizeof *a->argv) - 1) {
        a->argv[a->argc++] = o;
        q = 0;
        while (*s && (q || (*s != ' ' && *s != '\t'))) {
            if (q && *s == q) {
                q = 0;
            } else if (!q && (*s == '"' || *s == '\'')) {
                q = *s;
            } else if (*s == '\\' && q != '\'' && s[1]) {
                s++;
                if (o < end)
                    *o++ = q == '"' && *s == 'n'   ? '\n'
                           : q == '"' && *s == 't' ? '\t'
                                                   : *s;
            } else if (o < end) {
                *o++ = *s;
            }
            s++;
        }
        if (q)
            return a->argc = -1;
        *o++ = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (a->argc == 1) {
            const char *r = line;
            while (*r == ' ' || *r == '\t')
                r++;
            r += strcspn(r, " \t");
            while (*r == ' ' || *r == '\t')
                r++;
            a->rest = a->raw + (r - line);
        }
    }
    a->argv[a->argc] = NULL;
    if (!a->argc)
        a->rest = a->raw;
    return a->argc;
}

static int isbool(const Args *a, const char *flag) {
    const char *b = a->bools;
    size_t n = strlen(flag);
    while (b && *b) {
        if (!strncmp(b, flag, n) && (b[n] == ',' || !b[n]))
            return 1;
        b = strchr(b, ',');
        if (b)
            b++;
    }
    return 0;
}

/* index of --flag in argv, or -1 */
static int optidx(const Args *a, const char *flag) {
    int i;
    size_t n = strlen(flag);
    for (i = 1; i < a->argc; i++)
        if (a->argv[i][0] == '-' && a->argv[i][1] == '-' &&
            !strncmp(a->argv[i] + 2, flag, n) &&
            (!a->argv[i][2 + n] || a->argv[i][2 + n] == '='))
            return i;
    return -1;
}

static int optflag(const Args *a, const char *flag) {
    return optidx(a, flag) >= 0;
}

/* value of --flag (or --flag=value), NULL when absent or valueless */
static const char *optstr(const Args *a, const char *flag) {
    int i = optidx(a, flag);
    const char *eq;
    if (i < 0)
        return NULL;
    if ((eq = strchr(a->argv[i], '=')))
        return eq + 1;
    return i + 1 < a->argc ? a->argv[i + 1] : NULL;
}

static double optnum(const Args *a, const char *flag, double dflt) {
    const char *v = optstr(a, flag);
    return v ? atof(v) : dflt;
}

/* i-th positional argument after the verb (0 = first), skipping flags
 * and their values; NULL when there is none */
static const char *pos(const Args *a, int want) {
    int i, k = 0;
    for (i = 1; i < a->argc; i++) {
        if (a->argv[i][0] == '-' && a->argv[i][1] == '-') {
            if (!strchr(a->argv[i], '=') && !isbool(a, a->argv[i] + 2))
                i++; /* its value */
            continue;
        }
        if (k++ == want)
            return a->argv[i];
    }
    return NULL;
}

static int npos(const Args *a) {
    int n = 0;
    while (pos(a, n))
        n++;
    return n;
}

#endif
