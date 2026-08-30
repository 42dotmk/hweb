/* hweb-ext: WebKit web-process extension. Request headers can only be
 * changed from inside the web process, so this is where hweb's
 * `headers[]` (passed in as initialization user data, a(ss)) get applied
 * to every outgoing HTTP request. */
#include <webkit2/webkit-web-extension.h>

static GVariant *hdrs;

static gboolean sendrequest(WebKitWebPage *page, WebKitURIRequest *req,
                            WebKitURIResponse *redirect, gpointer data) {
    SoupMessageHeaders *h = webkit_uri_request_get_http_headers(req);
    GVariantIter it;
    const char *name, *value;
    (void)page, (void)redirect, (void)data;

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
    hdrs = g_variant_ref((GVariant *)data);
    g_signal_connect(ext, "page-created", G_CALLBACK(pagecreated), NULL);
}
