/* history.c - append-only visit log and fuzzy search over it (see history.h) */
#include "history.h"

#include <stdio.h>
#include <string.h>

static const char *histfile, *histfilter;

void histinit(const char *file, const char *filter) {
    histfile = file;
    histfilter = filter;
}

void histadd(const char *uri, const char *title) {
    FILE *f;
    char *t, *p;
    if (!uri || !*uri || !strncmp(uri, "about:", 6))
        return;
    f = fopen(histfile, "a");
    if (!f)
        return;
    t = g_strdup(title ? title : "");
    for (p = t; *p; p++)
        if (*p == '\t' || *p == '\n')
            *p = ' ';
    fprintf(f, "%s\t%s\n", uri, t);
    fclose(f);
    g_free(t);
}

/* first line wins: the filter emits the file newest-first */
static int seen(GPtrArray *a, const char *line, size_t urllen) {
    guint i;
    for (i = 0; i < a->len; i++) {
        const char *s = g_ptr_array_index(a, i);
        if (!strncmp(s, line, urllen) && (s[urllen] == '\t' || !s[urllen]))
            return 1;
    }
    return 0;
}

GPtrArray *histmatch(const char *query, unsigned max) {
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    const char *argv[] = {"/bin/sh", "-c",  histfilter, "sh",
                          histfile,  query, NULL};
    char *out = NULL, **lines, **l;

    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, NULL, NULL) ||
        !out)
        return a;
    lines = g_strsplit(out, "\n", -1);
    for (l = lines; *l && a->len < max; l++) {
        if (!**l || seen(a, *l, strcspn(*l, "\t")))
            continue;
        g_ptr_array_add(a, g_strdup(*l));
    }
    g_strfreev(lines);
    g_free(out);
    return a;
}
