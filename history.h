/* visited-page history: a flat append-only file of "url\ttitle" lines,
 * searched through an external fuzzy filter (fzf). */
#include <glib.h>

/* file: history path; filter: shell snippet run as `sh -c filter sh FILE
 * QUERY`, must print matching lines of FILE, best first */
void histinit(const char *file, const char *filter);
void histadd(const char *uri, const char *title);
/* up to max matching "url\ttitle" lines, most recent first, one per url;
 * free with g_ptr_array_free(a, TRUE) */
GPtrArray *histmatch(const char *query, unsigned max);
