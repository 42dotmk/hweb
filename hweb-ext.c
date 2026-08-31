/* hweb-ext: WebKit web-process extension. Two things can only be done from
 * inside the web process, so they live here: rewriting request headers
 * (hweb's `headers[]`) and cancelling requests to blocked hosts (hweb's
 * `blocklist` file). Both arrive as initialization user data, (a(ss)s). */
#include <string.h>
#include <webkit2/webkit-web-extension.h>

static GVariant *hdrs;
static GHashTable *blocked; /* set of lowercase hosts */

/* read the blocklist: one host per line, or hosts-file lines whose
 * address is 0.0.0.0/127.0.0.1; `#` starts a comment. Dot-less names
 * (localhost, broadcasthost...) are skipped. The buffer is kept: the
 * table keys point into it. */
static void loadblocklist(const char *path) {
    char *buf, *line, *nl, *addr, *host, *sp;

    blocked = g_hash_table_new(g_str_hash, g_str_equal);
    if (!g_file_get_contents(path, &buf, NULL, NULL))
        return;
    for (line = buf; line; line = nl) {
        if ((nl = strchr(line, '\n')))
            *nl++ = 0;
        if ((sp = strchr(line, '#')))
            *sp = 0;
        if (!(addr = strtok_r(line, " \t\r", &sp)))
            continue;
        if ((host = strtok_r(NULL, " \t\r", &sp))) {
            if (strcmp(addr, "0.0.0.0") && strcmp(addr, "127.0.0.1"))
                continue;
        } else
            host = addr;
        if (strchr(host, '.'))
            g_hash_table_add(blocked, g_ascii_strdown(host, -1));
    }
}

/* the host itself or any parent domain listed? (like `||host^`) */
static gboolean isblocked(const char *uri) {
    GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    char *host, *h;
    gboolean hit = FALSE;

    if (!u)
        return FALSE;
    host = g_uri_get_host(u) ? g_ascii_strdown(g_uri_get_host(u), -1) : NULL;
    for (h = host; h && !hit; h = strchr(h, '.') ? strchr(h, '.') + 1 : NULL)
        hit = g_hash_table_contains(blocked, h);
    g_free(host);
    g_uri_unref(u);
    return hit;
}

static gboolean sendrequest(WebKitWebPage *page, WebKitURIRequest *req,
                            WebKitURIResponse *redirect, gpointer data) {
    SoupMessageHeaders *h = webkit_uri_request_get_http_headers(req);
    const char *uri = webkit_uri_request_get_uri(req);
    GVariantIter it;
    const char *name, *value;
    (void)redirect, (void)data;

    /* only subresources: the page's own uri is the navigation in progress */
    if (g_strcmp0(uri, webkit_web_page_get_uri(page)) && isblocked(uri)) {
        webkit_web_page_send_message_to_view(
            page, webkit_user_message_new("blocked", g_variant_new_string(uri)),
            NULL, NULL, NULL);
        return TRUE; /* cancels the request */
    }
    if (!h)
        return FALSE;
    g_variant_iter_init(&it, hdrs);
    while (g_variant_iter_next(&it, "(&s&s)", &name, &value)) {
        if (*value)
            soup_message_headers_replace(h, name, value);
        else
            soup_message_headers_remove(h, name);
    }
    return FALSE;
}

static void pagecreated(WebKitWebExtension *ext, WebKitWebPage *page,
                        gpointer data) {
    (void)ext, (void)data;
    g_signal_connect(page, "send-request", G_CALLBACK(sendrequest), NULL);
}

G_MODULE_EXPORT void
webkit_web_extension_initialize_with_user_data(WebKitWebExtension *ext,
                                               GVariant *data) {
    const char *path;

    g_variant_get(data, "(@a(ss)&s)", &hdrs, &path);
    loadblocklist(path);
    g_signal_connect(ext, "page-created", G_CALLBACK(pagecreated), NULL);
}
