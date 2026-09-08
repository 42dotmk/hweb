/* hweb - vim-like WebKitGTK browser. See config.h for keys and settings.
 *
 * One window per process (like surf; the WM tiles them). Modes: normal
 * (keys map to commands), insert (keys go to the page), hint (link
 * labels), prompt (the : / entry). Commands are one text language used
 * by the keymap, the : prompt, and stdin (one command per line when
 * stdin is not a terminal). Events are printed to stdout, one per line. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <webkit2/webkit2.h>

#include "args.h"
#include "config.h"
#include "history.h"

#define LENGTH(x) (sizeof(x) / sizeof(*(x)))

enum mode { NORMAL, INSERT, HINT, PROMPT };
static const char *modename[] = {"normal", "insert", "hint", "prompt"};

static GtkWidget *win, *view, *status, *modelbl, *urllbl, *entry, *complbl;
static WebKitUserContentManager *ucm;
static enum mode mode = NORMAL;
static int private; /* HWEB_PRIVATE: ephemeral storage, no history writes */
/* HWEB_PROFILE: the data directory (cookies, storage, history; cache under
 * it) — -P DIR; empty means the default $XDG_DATA_HOME/hweb */
static char profile[4096];
static char pending[64];
static int inspecting;
static char *hoveruri;
/* prompt completion: texts[] replace the entry, disps[] are shown */
static GPtrArray *comptexts, *compdisps;
static guint compidx;
static int compquiet;     /* entry changes made by completion itself */
static char *histpending; /* loaded url still waiting for its title */
static char exedir[4096];
static int lsock = -1; /* control socket, one line in -> one JSON out */
static char sockpath[256];
static gint64 lastactive; /* when the window last had focus (us) */

/* a command in flight and where its reply goes: a socket client, stdout
 * (as a `result` event, for commands read from stdin) or nowhere (keymap,
 * prompt: errors show in the status bar). Async work (js, snapshots, a
 * --wait for navigation) finishes the request later. */
enum { SINK_NONE = -2, SINK_STDOUT = -1 };
typedef struct {
    int used, fd;
    int wait;    /* --wait: hold the reply for a navigation */
    int loading; /* a load started after the action */
    guint timer;
    char *json;   /* reply held back by --wait */
    char *target; /* element under a mouse command, JSON */
    double x, y, w, h;
    int full, quality;
    int button, count; /* mouse */
    guint state;       /* modifier mask */
    guint keyval;
    char kind[24]; /* reply type of a multi-step command */
    char key[64];
    char fmt[8];
    char path[4096];
} Req;
static Req reqs[16];

typedef void cmdfn(Req *r, Args *a);
struct command {
    const char *name;
    cmdfn *fn;
    const char *bools; /* flags that take no value */
};

/* internal scripts, isolated world "hweb": insert-mode tracking and the
 * hint machinery. Messages come back through webkit.messageHandlers.hweb. */
static const char *corejs =
    "(function(){"
    "var P=s=>window.webkit.messageHandlers.hweb.postMessage(s);"
    "function ed(e){return e&&(e.isContentEditable||"
    "(/^(INPUT|TEXTAREA|SELECT)$/.test(e.tagName)&&"
    "!/^(button|checkbox|radio|submit|reset|file|image|range|color)$/i"
    ".test(e.type)));}"
    "document.addEventListener('focusin',e=>{if(ed(e.target))P('mode "
    "insert');},true);"
    "document.addEventListener('focusout',e=>{if(ed(e.target))P('mode "
    "normal');},true);"
    "var H=window.__hweb={labels:[],typed:'',action:''};"
    "H.blur=()=>{document.activeElement&&document.activeElement.blur();};"
    "function vis(el){var r=el.getBoundingClientRect();"
    "if(r.width<=0||r.height<=0||r.bottom<0||r.right<0||r.top>innerHeight"
    "||r.left>innerWidth)return null;var s=getComputedStyle(el);"
    "if(s.visibility=='hidden'||s.display=='none'||s.opacity=='0')"
    "return null;return r;}"
    "H.start=function(chars,action){H.cancel();H.action=action;"
    "var els=[];document.querySelectorAll('a[href],button,input,select,"
    "textarea,summary,label,[onclick],[role=button],[role=link],"
    "[role=menuitem],[role=tab],[contenteditable],[tabindex]:not([tabindex"
    "=\"-1\"])').forEach(e=>{var r=vis(e);if(r)els.push([e,r]);});"
    "if(!els.length){P('hint none');return;}"
    "var n=chars.length,len=Math.max(1,Math.ceil(Math.log(els.length)/"
    "Math.log(n)));els.forEach((er,i)=>{var s='',k=i;"
    "for(var j=0;j<len;j++){s=chars[k%n]+s;k=Math.floor(k/n);}"
    "var d=document.createElement('div');d.textContent=s.toUpperCase();"
    "d.style.cssText='position:fixed;left:'+Math.max(0,er[1].left-2)+"
    "'px;top:'+Math.max(0,er[1].top-2)+'px;z-index:2147483647;"
    "background:#7aa2f7;color:#1a1b26;font:bold 12px monospace;"
    "padding:1px 3px;border-radius:2px;line-height:1;';"
    "document.documentElement.appendChild(d);H.labels.push([s,er[0],d]);});"
    "};"
    "H.key=function(ch){if(ch=='\\b')H.typed=H.typed.slice(0,-1);"
    "else H.typed+=ch;var t=H.typed,m=H.labels.filter(l=>l[0]"
    ".startsWith(t));H.labels.forEach(l=>l[2].style.display="
    "l[0].startsWith(t)?'':'none');if(m.length==1&&m[0][0]==t)"
    "{var el=m[0][1];H.cancel();H.act(el);}"
    "else if(!m.length){H.cancel();P('hint none');}};"
    "H.act=function(el){var a=H.action;"
    "if(a!='open'){P('hint '+a+' '+(el.href||el.src||''));return;}"
    "P('hint done');if(ed(el)){el.focus();return;}el.focus();el.click();};"
    "H.cancel=function(){H.labels.forEach(l=>l[2].remove());"
    "H.labels=[];H.typed='';};"
    "H.vis=vis;H.ed=ed;"
    "})();";
/* the automation library (auto.js), embedded by the makefile */
#include "auto.h"

static void cmd(const char *line);
static void cmdreq(Req *r, const char *line);
static void setstatus(const char *msg);

/* one event per line on stdout. Newlines and backslashes in the payload
 * are escaped (\n, \\) so a multi-line js result stays one line;
 * readers unescape after splitting lines. */
static void ev(const char *fmt, ...) {
    static char buf[65536];
    const char *s;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (s = buf; *s; s++) {
        if (*s == '\n')
            fputs("\\n", stdout);
        else if (*s == '\\')
            fputs("\\\\", stdout);
        else
            putchar(*s);
    }
    putchar('\n');
    fflush(stdout);
}

static void die(const char *msg) {
    fprintf(stderr, "hweb: %s\n", msg);
    if (sockpath[0])
        unlink(sockpath);
    exit(1);
}

/* JSON string literal of s written into dst */
static char *jq(char *dst, size_t n, const char *s) {
    size_t i = 0;
    if (n < 3) {
        if (n)
            *dst = 0;
        return dst;
    }
    dst[i++] = '"';
    for (; s && *s && i + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            dst[i++] = '\\';
            dst[i++] = (char)c;
        } else if (c == '\n') {
            dst[i++] = '\\';
            dst[i++] = 'n';
        } else if (c == '\r') {
            dst[i++] = '\\';
            dst[i++] = 'r';
        } else if (c == '\t') {
            dst[i++] = '\\';
            dst[i++] = 't';
        } else if (c < 0x20) {
            i += (size_t)snprintf(dst + i, n - i, "\\u%04x", c);
        } else {
            dst[i++] = (char)c;
        }
    }
    dst[i++] = '"';
    dst[i] = 0;
    return dst;
}

/* requests */

static void reqfree(Req *r) {
    if (r->timer)
        g_source_remove(r->timer);
    g_free(r->json);
    g_free(r->target);
    memset(r, 0, sizeof *r);
}

static void replyfd(int fd, const char *json) {
    if (write(fd, json, strlen(json)) >= 0)
        (void)!write(fd, "\n", 1); /* else the client is gone */
    close(fd);
}

static Req *reqnew(int fd) {
    size_t i;
    for (i = 0; i < LENGTH(reqs); i++)
        if (!reqs[i].used) {
            memset(&reqs[i], 0, sizeof reqs[i]);
            reqs[i].used = 1;
            reqs[i].fd = fd;
            return &reqs[i];
        }
    if (fd >= 0)
        replyfd(fd, "{\"type\":\"error\",\"error\":\"busy\"}");
    return NULL;
}

/* send the reply and release the request */
static void reply(Req *r, const char *json) {
    if (r->fd >= 0)
        replyfd(r->fd, json);
    else if (r->fd == SINK_STDOUT)
        ev("result %s", json);
    reqfree(r);
}

static void replyf(Req *r, const char *fmt, ...) {
    static char buf[65536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    reply(r, buf);
}

static void replyerr(Req *r, const char *fmt, ...) {
    char msg[1024], q[2100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (r->fd == SINK_NONE)
        setstatus(msg);
    replyf(r, "{\"type\":\"error\",\"error\":%s}", jq(q, sizeof q, msg));
}

static void ok(Req *r) { reply(r, "{\"type\":\"ok\"}"); }

/* the held reply gets the navigation outcome spliced in after its '{' */
static void replywait(Req *r, int loaded, const char *extra) {
    GString *g = g_string_new(r->json);
    char u[4096], ins[4400];
    snprintf(
        ins, sizeof ins, "\"loaded\":%s,%s%s%s%s", loaded ? "true" : "false",
        extra, loaded ? "\"url\":" : "",
        loaded ? jq(u, sizeof u, webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view)))
               : "",
        loaded ? "," : "");
    if (g->len >= 2 && g->str[1] == '}')
        ins[strlen(ins) - 1] = 0; /* no trailing comma into an empty object */
    g_string_insert(g, 1, ins);
    reply(r, g->str);
    g_string_free(g, TRUE);
}

static gboolean waitnone(gpointer p) {
    Req *r = p;
    r->timer = 0;
    replywait(r, 0, "");
    return FALSE;
}

static gboolean waitcap(gpointer p) {
    Req *r = p;
    r->timer = 0;
    replywait(r, 0, "\"timeout\":true,");
    return FALSE;
}

/* reply now, or after the navigation this command may have started:
 * a load must begin within 300 ms and finish within 30 s */
static void finish(Req *r, const char *json) {
    if (!r->wait) {
        reply(r, json);
        return;
    }
    r->json = g_strdup(json);
    r->timer = g_timeout_add(300, waitnone, r);
}

static void waitload(WebKitLoadEvent e, const char *extra) {
    size_t i;
    for (i = 0; i < LENGTH(reqs); i++) {
        Req *r = &reqs[i];
        if (!r->used || !r->json)
            continue;
        if (e == WEBKIT_LOAD_STARTED && !r->loading) {
            r->loading = 1;
            if (r->timer)
                g_source_remove(r->timer);
            r->timer = g_timeout_add(30000, waitcap, r);
        } else if (e == WEBKIT_LOAD_FINISHED && r->loading) {
            replywait(r, !*extra, extra);
        }
    }
}

static char *expandhome(const char *p) {
    if (p[0] == '~' && (p[1] == '/' || !p[1]))
        return g_build_filename(g_get_home_dir(), p + 1, NULL);
    return g_strdup(p);
}

static void setstatus(const char *msg) {
    gtk_label_set_text(GTK_LABEL(urllbl),
                       msg ? msg
                       : hoveruri
                           ? hoveruri
                           : webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view)));
}

static void updatemode(void) {
    char buf[256];
    const char *name = strrchr(profile, '/');
    name = name ? name + 1 : profile;
    snprintf(buf, sizeof buf, "%s%.64s%s%s%s%s%s", *name ? "[" : "", name,
             *name ? "] " : "", private ? "[private] " : "",
             mode == NORMAL ? "" : "-- ",
             mode == NORMAL ? pending : modename[mode],
             mode == NORMAL ? "" : " --");
    gtk_label_set_text(GTK_LABEL(modelbl), buf);
}

static void setmode(enum mode m) {
    if (m == mode)
        return;
    mode = m;
    pending[0] = 0;
    updatemode();
    ev("mode %s", modename[m]);
}

static void updatetitle(void) {
    const char *t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(view));
    const char *u = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view));
    char *s = g_strdup_printf(titlefmt, t && *t ? t : "hweb", u ? u : "");
    gtk_window_set_title(GTK_WINDOW(win), s);
    g_free(s);
}

/* run js in the given world ("hweb" for our own helpers, NULL for the
 * page's world). With a request the result answers it: as JSON to a
 * socket client, as the `js RESULT` event otherwise. */
static void jsreport(Req *r, JSCValue *v, GError *err);

static void jsdone(GObject *o, GAsyncResult *res, gpointer p) {
    GError *err = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o),
                                                             res, &err);
    jsreport(p, v, err);
}

static void jsreport(Req *r, JSCValue *v, GError *err) {
    if (err) {
        if (r && r->fd >= 0)
            replyerr(r, "%s", err->message);
        else if (r) {
            ev("js error %s", err->message);
            reqfree(r);
        }
        g_error_free(err);
        return;
    }
    if (r && r->fd >= 0) {
        char *j = jsc_value_is_undefined(v) ? NULL : jsc_value_to_json(v, 0);
        if (j) {
            replyf(r, "{\"type\":\"js\",\"value\":%s}", j);
            g_free(j);
        } else {
            static char q[65536];
            char *str = jsc_value_to_string(v);
            replyf(r, "{\"type\":\"js\",\"value\":%s}", jq(q, sizeof q, str));
            g_free(str);
        }
    } else if (r) {
        char *str = jsc_value_is_undefined(v) ? g_strdup("undefined")
                                              : jsc_value_to_string(v);
        ev("js %s", str);
        g_free(str);
        reqfree(r);
    }
    g_object_unref(v);
}

static void jsfndone(GObject *o, GAsyncResult *res, gpointer p) {
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(o), res, &err);
    jsreport(p, v, err);
}

static void js(const char *world, Req *r, const char *fmt, ...) {
    static char buf[65536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(view), buf, -1, world,
                                        NULL, NULL, jsdone, r);
}

/* JS string literal from arbitrary text */
static char *jsstr(const char *s) {
    GString *g = g_string_new("'");
    for (; *s; s++) {
        if (*s == '\'' || *s == '\\')
            g_string_append_c(g, '\\');
        if (*s == '\n')
            g_string_append(g, "\\n");
        else
            g_string_append_c(g, *s);
    }
    g_string_append_c(g, '\'');
    return g_string_free(g, FALSE);
}

static void reap(GPid pid, gint st, gpointer d) {
    (void)st, (void)d;
    g_spawn_close_pid(pid);
}

/* a new hweb window: its own process, from our directory; returns its pid */
static int spawn(const char *uri, int priv) {
    char *argv[] = {NULL, NULL, NULL, NULL};
    char path[4200];
    GPid pid = 0;
    int i = 0;
    snprintf(path, sizeof path, "%s/hweb", exedir);
    argv[i++] = path;
    if (priv)
        argv[i++] = "-p";
    argv[i] = (char *)uri;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                      &pid, NULL))
        g_child_watch_add(pid, reap, NULL);
    ev("new %s", uri);
    return (int)pid;
}

static char *tourl(const char *s) {
    char *q;
    if (!*s)
        return g_strdup(homepage);
    if (strstr(s, "://") || !strncmp(s, "about:", 6) ||
        !strncmp(s, "data:", 5) || !strncmp(s, "javascript:", 11))
        return g_strdup(s);
    if (*s == '/' || *s == '~' || !strncmp(s, "./", 2)) {
        char *p = expandhome(s), *abs = g_canonicalize_filename(p, NULL);
        char *u = g_strconcat("file://", abs, NULL);
        g_free(p), g_free(abs);
        return u;
    }
    if (!strchr(s, ' ') && (strchr(s, '.') || !strncmp(s, "localhost", 9)))
        return g_strconcat("https://", s, NULL);
    q = g_uri_escape_string(s, NULL, FALSE);
    s = g_strdup_printf(searchurl, q);
    g_free(q);
    return (char *)s;
}

static void inject(const char *file, int now) {
    char *p = expandhome(file), *src = NULL;
    if (!g_file_get_contents(p, &src, NULL, NULL)) {
        setstatus("cannot read script");
        g_free(p);
        return;
    }
    WebKitUserScript *us = webkit_user_script_new(
        src, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);
    if (now)
        js(NULL, NULL, "%s", src);
    ev("inject %s", p);
    g_free(src), g_free(p);
}

static void compclear(void) {
    if (comptexts)
        g_ptr_array_free(comptexts, TRUE);
    if (compdisps)
        g_ptr_array_free(compdisps, TRUE);
    comptexts = compdisps = NULL;
    gtk_widget_hide(complbl);
}

static void compshow(void) {
    GString *m = g_string_new(NULL);
    guint i, from = compidx - compidx % compmax;
    for (i = from; i < compdisps->len && i < from + compmax; i++) {
        char *e = g_markup_escape_text(g_ptr_array_index(compdisps, i), -1);
        g_string_append_printf(m, "%s%s%s%s", i > from ? "\n" : "",
                               i == compidx ? "<b>&gt; " : "  ", e,
                               i == compidx ? "</b>" : "");
        g_free(e);
    }
    gtk_label_set_markup(GTK_LABEL(complbl), m->str);
    gtk_widget_show(complbl);
    g_string_free(m, TRUE);
    compquiet = 1;
    gtk_entry_set_text(GTK_ENTRY(entry), g_ptr_array_index(comptexts, compidx));
    gtk_editable_set_position(GTK_EDITABLE(entry), -1);
    compquiet = 0;
}

static void compadd(const char *text, const char *disp) {
    g_ptr_array_add(comptexts, g_strdup(text));
    g_ptr_array_add(compdisps, g_strdup(disp));
}

static void prompt(const char *text) {
    gtk_widget_hide(status);
    gtk_widget_show(entry);
    gtk_entry_set_text(GTK_ENTRY(entry), text);
    gtk_widget_grab_focus(entry);
    gtk_editable_set_position(GTK_EDITABLE(entry), -1);
    setmode(PROMPT);
}

static void unprompt(void) {
    compclear();
    gtk_widget_hide(entry);
    gtk_widget_show(status);
    gtk_widget_grab_focus(view);
    setmode(NORMAL);
}

static void find(const char *text, guint32 extra) {
    WebKitFindController *fc =
        webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(view));
    if (!*text) {
        webkit_find_controller_search_finish(fc);
        return;
    }
    webkit_find_controller_search(fc, text,
                                  WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE |
                                      WEBKIT_FIND_OPTIONS_WRAP_AROUND | extra,
                                  G_MAXUINT);
}

#define V WEBKIT_WEB_VIEW(view)

/* the parsed command line as JSON for auto.js: positionals in "_", flags
 * by name, boolean flags true */
static void argsjson(Args *a, char *out, size_t n) {
    GString *g = g_string_new("{\"_\":[");
    char q[8300];
    int i, first = 1;
    for (i = 1; i < a->argc; i++) {
        if (a->argv[i][0] == '-' && a->argv[i][1] == '-') {
            if (!strchr(a->argv[i], '=') && !isbool(a, a->argv[i] + 2))
                i++; /* its value */
            continue;
        }
        g_string_append_printf(g, "%s%s", first ? "" : ",",
                               jq(q, sizeof q, a->argv[i]));
        first = 0;
    }
    g_string_append_c(g, ']');
    for (i = 1; i < a->argc; i++) {
        char *eq, name[256];
        if (a->argv[i][0] != '-' || a->argv[i][1] != '-')
            continue;
        snprintf(name, sizeof name, "%s", a->argv[i] + 2);
        if ((eq = strchr(name, '='))) {
            *eq = 0;
            g_string_append_printf(g, ",%s:%s", jq(q, sizeof q, name),
                                   jq(q, sizeof q, eq + 1));
        } else if (isbool(a, name)) {
            g_string_append_printf(g, ",%s:true", jq(q, sizeof q, name));
        } else if (i + 1 < a->argc) {
            g_string_append_printf(g, ",%s:", jq(q, sizeof q, name));
            g_string_append(g, jq(q, sizeof q, a->argv[++i]));
        }
    }
    g_string_append_c(g, '}');
    snprintf(out, n, "%s", g->str);
    g_string_free(g, TRUE);
}

/* auto.js answered: its object is the reply (honouring --wait) */
static void jsrundone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    char *j;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(o), res, &err);
    if (err) {
        replyerr(r, "%s", err->message);
        g_error_free(err);
        return;
    }
    j = jsc_value_to_json(v, 0);
    if (!j)
        replyerr(r, "unserializable result");
    else if (!strncmp(j, "{\"type\":\"error\"", 15) ||
             strstr(j, "\"type\":\"error\""))
        reply(r, j);
    else
        finish(r, j);
    g_free(j);
    g_object_unref(v);
}

/* run verb in auto.js with opts (JSON); cb gets the JSCValue */
static void jscall(Req *r, const char *verb, const char *opts,
                   GAsyncReadyCallback cb) {
    webkit_web_view_call_async_javascript_function(
        V, "return __hweb.auto.run(v, JSON.parse(a))", -1,
        g_variant_new_parsed("{'v': <%s>, 'a': <%s>}", verb, opts), "hweb",
        NULL, NULL, cb, r);
}

/* ... with the command's own arguments */
static void jsrun(Req *r, const char *verb, Args *a, GAsyncReadyCallback cb) {
    static char json[65536];
    argsjson(a, json, sizeof json);
    jscall(r, verb, json, cb);
}

/* the verbs auto.js implements: click, focus, type, select, highlight... */
static void c_auto(Req *r, Args *a) {
    r->wait = optflag(a, "wait");
    jsrun(r, a->argv[0], a, jsrundone);
}

/* synthesized input: real GDK events handed to the web view, which WebKit
 * treats as user input (trusted DOM events, hover, native scrolling,
 * focus traversal). Coordinates are CSS px; the pointer they move is
 * virtual, the X pointer stays put, so auto.js draws a cursor for it. */
static double px, py;             /* the virtual pointer, CSS px */
static guint32 evtime = 1000;     /* event timestamps, ms, increasing */
static gint64 lastsynthright = 0; /* to suppress WebKit's context menu */
static struct {
    Req *r;
    double x0, y0, x1, y1, cx, cy; /* from, to, bezier control point */
    int step, steps, linear;
    guint state;
    void (*done)(Req *);
    guint timer;
} glide;

static double zoom(void) { return webkit_web_view_get_zoom_level(V); }

static GdkEvent *mkev(GdkEventType t, double cx, double cy, guint state) {
    GdkEvent *e = gdk_event_new(t);
    GdkWindow *w = gtk_widget_get_window(view);
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    GdkDevice *ptr = gdk_seat_get_pointer(seat);
    double x = cx * zoom(), y = cy * zoom();
    int ox, oy;
    gdk_window_get_origin(w, &ox, &oy);
    e->any.window = g_object_ref(w);
    e->any.send_event = FALSE;
    evtime += 8;
    switch (t) {
    case GDK_MOTION_NOTIFY:
        e->motion.time = evtime;
        e->motion.x = x;
        e->motion.y = y;
        e->motion.state = state;
        e->motion.x_root = ox + x;
        e->motion.y_root = oy + y;
        break;
    case GDK_BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
        e->button.time = evtime;
        e->button.x = x;
        e->button.y = y;
        e->button.state = state;
        e->button.x_root = ox + x;
        e->button.y_root = oy + y;
        break;
    case GDK_SCROLL:
        e->scroll.time = evtime;
        e->scroll.x = x;
        e->scroll.y = y;
        e->scroll.state = state;
        e->scroll.direction = GDK_SCROLL_SMOOTH;
        e->scroll.x_root = ox + x;
        e->scroll.y_root = oy + y;
        break;
    default:
        break;
    }
    gdk_event_set_device(e, ptr);
    gdk_event_set_source_device(e, ptr);
    return e;
}

static void sendev(GdkEvent *e) {
    gtk_widget_event(view, e);
    gdk_event_free(e);
}

static void motion(double x, double y, guint state) {
    sendev(mkev(GDK_MOTION_NOTIFY, x, y, state));
    px = x;
    py = y;
    js("hweb", NULL, "__hweb.auto.run('cursor',{_:[%g,%g]})", x, y);
}

static guint buttonmask(int b) {
    return b == 1   ? GDK_BUTTON1_MASK
           : b == 2 ? GDK_BUTTON2_MASK
                    : GDK_BUTTON3_MASK;
}

static void press(double x, double y, int b, guint state) {
    GdkEvent *e = mkev(GDK_BUTTON_PRESS, x, y, state);
    e->button.button = (guint)b;
    if (b == 3)
        lastsynthright = g_get_monotonic_time();
    sendev(e);
}

static void release(double x, double y, int b, guint state) {
    GdkEvent *e = mkev(GDK_BUTTON_RELEASE, x, y, state | buttonmask(b));
    e->button.button = (guint)b;
    sendev(e);
}

static gboolean ctxmenu(WebKitWebView *v, WebKitContextMenu *m, GdkEvent *e,
                        WebKitHitTestResult *h, gpointer d) {
    (void)v, (void)m, (void)e, (void)h, (void)d;
    /* the page saw its contextmenu event; no GTK menu for a synthetic click */
    return g_get_monotonic_time() - lastsynthright < 500000;
}

static gboolean glidestep(gpointer d) {
    double t = (double)++glide.step / glide.steps, e, u;
    Req *r;
    (void)d;
    e = glide.linear ? t
        : t < 0.5    ? 4 * t * t * t
                     : 1 - pow(-2 * t + 2, 3) / 2; /* ease in-out cubic */
    u = 1 - e;
    motion(u * u * glide.x0 + 2 * u * e * glide.cx + e * e * glide.x1,
           u * u * glide.y0 + 2 * u * e * glide.cy + e * e * glide.y1,
           glide.state);
    if (glide.step < glide.steps)
        return TRUE;
    glide.timer = 0;
    r = glide.r;
    glide.r = NULL;
    glide.done(r);
    return FALSE;
}

/* move the virtual pointer to x,y along a human-ish path (a bowed,
 * eased curve; straight and linear for drags), then call done */
static void glideto(Req *r, double x, double y, int instant, double durms,
                    guint state, int steps, void (*done)(Req *)) {
    double dist = hypot(x - px, y - py), arc, nx, ny;
    if (glide.r) {
        replyerr(r, "busy: pointer in motion");
        return;
    }
    if (instant || dist < 2) {
        motion(x, y, state);
        done(r);
        return;
    }
    glide.r = r;
    glide.x0 = px;
    glide.y0 = py;
    glide.x1 = x;
    glide.y1 = y;
    glide.linear = steps > 0;
    glide.steps = steps > 0 ? steps : (int)CLAMP(lround(dist / 11), 10, 45);
    glide.step = 0;
    glide.state = state;
    glide.done = done;
    if (durms <= 0)
        durms = CLAMP(dist * 0.77, 94, 402);
    arc = glide.linear ? 0 : MIN(36, dist * 0.10);
    nx = -(y - py) / dist;
    ny = (x - px) / dist;
    glide.cx = (px + x) / 2 + nx * arc;
    glide.cy = (py + y) / 2 + ny * arc;
    glide.timer =
        g_timeout_add((guint)MAX(1, durms / glide.steps), glidestep, NULL);
}

static guint modstate(Args *a) {
    return (optflag(a, "shift") ? GDK_SHIFT_MASK : 0) |
           (optflag(a, "ctrl") ? GDK_CONTROL_MASK : 0) |
           (optflag(a, "alt") ? GDK_MOD1_MASK : 0) |
           (optflag(a, "meta") ? GDK_META_MASK : 0);
}

static int buttonof(Args *a) {
    const char *b = optstr(a, "button");
    return !b || !strcmp(b, "left") ? 1 : !strcmp(b, "middle") ? 2 : 3;
}

/* mouse-move/click landed: what is under the pointer, then act */
static void atdone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
                 WEBKIT_WEB_VIEW(o), res, &err),
             *t;
    int i;
    if (err) {
        r->target = g_strdup("null");
        g_error_free(err);
    } else {
        t = jsc_value_object_get_property(v, "target");
        r->target = jsc_value_to_json(t, 0);
        g_object_unref(t);
        g_object_unref(v);
    }
    if (!strcmp(r->kind, "mouse_moved")) {
        replyf(r, "{\"type\":\"mouse_moved\",\"x\":%g,\"y\":%g,\"target\":%s}",
               r->x, r->y, r->target);
        return;
    }
    for (i = 0; i < r->count; i++) {
        press(r->x, r->y, r->button, r->state);
        release(r->x, r->y, r->button, r->state);
    }
    js("hweb", NULL, "__hweb.auto.run('cursor',{_:[%g,%g],click:true})", r->x,
       r->y);
    {
        static char buf[65536];
        snprintf(
            buf, sizeof buf,
            "{\"type\":\"mouse_clicked\",\"x\":%g,\"y\":%g,\"button\":\"%s\","
            "\"count\":%d,\"target\":%s}",
            r->x, r->y,
            r->button == 1   ? "left"
            : r->button == 2 ? "middle"
                             : "right",
            r->count, r->target);
        finish(r, buf);
    }
}

static gboolean atquery(gpointer p) {
    Req *r = p;
    char opts[128];
    r->timer = 0;
    snprintf(opts, sizeof opts, "{\"_\":[%g,%g]}", r->x, r->y);
    jscall(r, "at", opts, atdone);
    return FALSE;
}

/* WebKit queues pointer events to the web process: ask what is under the
 * pointer a moment after moving it */
static void landed(Req *r) { r->timer = g_timeout_add(60, atquery, r); }

/* mouse-move X Y | mouse-up/down/left/right [N] [--less|--more]
 * [--instant] [--duration MS] [--shift --ctrl --alt --meta] */
static void c_mousemove(Req *r, Args *a) {
    const char *verb = a->argv[0] + 6, *n = pos(a, 0);
    double x = px, y = py, step;
    if (!strcmp(verb, "move")) {
        if (npos(a) < 2) {
            replyerr(r, "mouse-move needs X Y");
            return;
        }
        x = atof(pos(a, 0));
        y = atof(pos(a, 1));
    } else {
        step = n                    ? atof(n)
               : optflag(a, "less") ? 1
               : optflag(a, "more") ? 100
                                    : 10;
        x += !strcmp(verb, "right") ? step : !strcmp(verb, "left") ? -step : 0;
        y += !strcmp(verb, "down") ? step : !strcmp(verb, "up") ? -step : 0;
    }
    r->x = x;
    r->y = y;
    snprintf(r->kind, sizeof r->kind, "mouse_moved");
    glideto(r, x, y, optflag(a, "instant"), optnum(a, "duration", 0),
            modstate(a), 0, landed);
}

/* mouse-click X Y [--button left|right|middle] [--count N|--double]
 * [--instant] [--duration MS] [--wait] [modifiers] */
static void c_mouseclick(Req *r, Args *a) {
    if (npos(a) < 2) {
        replyerr(r, "mouse-click needs X Y");
        return;
    }
    r->x = atof(pos(a, 0));
    r->y = atof(pos(a, 1));
    r->button = buttonof(a);
    r->count = optflag(a, "double") ? 2 : (int)optnum(a, "count", 1);
    r->state = modstate(a);
    r->wait = optflag(a, "wait");
    snprintf(r->kind, sizeof r->kind, "mouse_clicked");
    glideto(r, r->x, r->y, optflag(a, "instant"), optnum(a, "duration", 0),
            r->state, 0, landed);
}

/* mouse-press / mouse-release [X Y] [--button B] */
static void c_mousepress(Req *r, Args *a) {
    int down = a->argv[0][6] == 'p', b = buttonof(a);
    if (npos(a) >= 2) {
        motion(atof(pos(a, 0)), atof(pos(a, 1)), modstate(a));
    }
    if (down)
        press(px, py, b, modstate(a));
    else
        release(px, py, b, modstate(a));
    replyf(r, "{\"type\":\"%s\",\"x\":%g,\"y\":%g,\"button\":\"%s\"}",
           down ? "mouse_pressed" : "mouse_released", px, py,
           b == 1   ? "left"
           : b == 2 ? "middle"
                    : "right");
}

static void dragdone(Req *r) {
    release(r->x, r->y, r->button, r->state);
    replyf(r,
           "{\"type\":\"mouse_dragged\",\"from\":{\"x\":%g,\"y\":%g},"
           "\"to\":{\"x\":%g,\"y\":%g},\"button\":\"%s\",\"steps\":%d}",
           r->w, r->h, r->x, r->y,
           r->button == 1   ? "left"
           : r->button == 2 ? "middle"
                            : "right",
           r->count);
}

/* mouse-drag X1 Y1 X2 Y2 [--steps N] [--button B] */
static void c_mousedrag(Req *r, Args *a) {
    if (npos(a) < 4) {
        replyerr(r, "mouse-drag needs X1 Y1 X2 Y2");
        return;
    }
    r->w = atof(pos(a, 0)); /* from */
    r->h = atof(pos(a, 1));
    r->x = atof(pos(a, 2)); /* to */
    r->y = atof(pos(a, 3));
    r->button = buttonof(a);
    r->count = (int)optnum(a, "steps", 10);
    r->state = modstate(a);
    motion(r->w, r->h, r->state);
    press(r->w, r->h, r->button, r->state);
    glideto(r, r->x, r->y, 0, r->count * 16.0, r->state | buttonmask(r->button),
            r->count, dragdone);
}

static void scrolldone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
                 WEBKIT_WEB_VIEW(o), res, &err),
             *sx, *sy;
    double x = 0, y = 0;
    if (!err) {
        sx = jsc_value_object_get_property(v, "scrollX");
        sy = jsc_value_object_get_property(v, "scrollY");
        x = jsc_value_to_double(sx);
        y = jsc_value_to_double(sy);
        g_object_unref(sx), g_object_unref(sy), g_object_unref(v);
    } else
        g_error_free(err);
    replyf(r,
           "{\"type\":\"mouse_scrolled\",\"x\":%g,\"y\":%g,\"deltaX\":%g,"
           "\"deltaY\":%g,\"scrollX\":%g,\"scrollY\":%g}",
           r->x, r->y, r->w, r->h, x, y);
}

static gboolean scrollquery(gpointer p) {
    Req *r = p;
    r->timer = 0;
    jscall(r, "metrics", "{}", scrolldone);
    return FALSE;
}

/* mouse-scroll [up|down|left|right] [N] [--less|--more] [--at X Y]: a
 * wheel event at the pointer. WebKit scrolls 11% of the view height
 * (at least 40 px) per smooth-scroll unit, so N px is N/unit units. */
static void c_mousescroll(Req *r, Args *a) {
    const char *dir = NULL, *n = NULL;
    double pxls, unit, dx = 0, dy = 0, x = px, y = py;
    GdkEvent *e;
    int i, k = 0;
    /* positionals by hand: --at takes two values */
    for (i = 1; i < a->argc; i++) {
        if (!strcmp(a->argv[i], "--at")) {
            i += 2;
            continue;
        }
        if (a->argv[i][0] == '-' && a->argv[i][1] == '-')
            continue;
        if (k == 0)
            dir = a->argv[i];
        else if (k == 1)
            n = a->argv[i];
        k++;
    }
    if (dir && g_ascii_isdigit(*dir)) {
        n = dir;
        dir = "down";
    }
    if (!dir)
        dir = "down";
    pxls = n                    ? atof(n)
           : optflag(a, "less") ? 100
           : optflag(a, "more") ? 700
                                : 300;
    unit =
        MAX(40, lround(gtk_widget_get_allocated_height(view) / zoom() * 0.11));
    if (!strcmp(dir, "up"))
        dy = -pxls;
    else if (!strcmp(dir, "left"))
        dx = -pxls;
    else if (!strcmp(dir, "right"))
        dx = pxls;
    else
        dy = pxls;
    if ((i = optidx(a, "at")) >= 0 && i + 2 < a->argc) {
        x = atof(a->argv[i + 1]);
        y = atof(a->argv[i + 2]);
    }
    e = mkev(GDK_SCROLL, x, y, modstate(a));
    e->scroll.delta_x = dx / unit;
    e->scroll.delta_y = dy / unit;
    e->scroll.is_stop = 0;
    sendev(e);
    r->x = x;
    r->y = y;
    r->w = dx;
    r->h = dy;
    r->timer = g_timeout_add(400, scrollquery, r);
}

static void hidedone(GObject *o, GAsyncResult *res, gpointer p) {
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(o), res, &err);
    if (err)
        g_error_free(err);
    else
        g_object_unref(v);
    reply(p, "{\"type\":\"mouse_hidden\"}");
}

static void c_mousehide(Req *r, Args *a) {
    (void)a;
    jscall(r, "cursor_hide", "{}", hidedone);
}

/* keyboard: DOM key names (Enter, ArrowDown, a...) or GDK names */
static guint keyvalof(const char *name, guint state) {
    static const struct {
        const char *dom, *gdk;
    } map[] = {{"Enter", "Return"},
               {"Esc", "Escape"},
               {"Backspace", "BackSpace"},
               {"ArrowUp", "Up"},
               {"ArrowDown", "Down"},
               {"ArrowLeft", "Left"},
               {"ArrowRight", "Right"},
               {"PageUp", "Page_Up"},
               {"PageDown", "Page_Down"},
               {"Space", "space"},
               {" ", "space"}};
    size_t i;
    guint kv;
    if (g_utf8_strlen(name, -1) == 1) {
        kv = gdk_unicode_to_keyval(g_utf8_get_char(name));
        return state & GDK_SHIFT_MASK ? gdk_keyval_to_upper(kv) : kv;
    }
    for (i = 0; i < LENGTH(map); i++)
        if (!strcmp(map[i].dom, name))
            return gdk_keyval_from_name(map[i].gdk);
    kv = gdk_keyval_from_name(name);
    return kv == GDK_KEY_VoidSymbol ? 0 : kv;
}

static void sendkey(guint keyval, guint state, int down) {
    GdkEvent *e = gdk_event_new(down ? GDK_KEY_PRESS : GDK_KEY_RELEASE);
    GdkKeymap *km = gdk_keymap_get_for_display(gdk_display_get_default());
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    GdkKeymapKey *keys;
    gint n;
    e->key.window = g_object_ref(gtk_widget_get_window(view));
    e->key.keyval = keyval;
    e->key.state = state;
    e->key.time = evtime += 8;
    if (gdk_keymap_get_entries_for_keyval(km, keyval, &keys, &n) && n) {
        e->key.hardware_keycode = keys[0].keycode;
        e->key.group = (guint8)keys[0].group;
        if (keys[0].level == 1)
            e->key.state |= GDK_SHIFT_MASK;
        g_free(keys);
    }
    gdk_event_set_device(e, gdk_seat_get_keyboard(seat));
    sendev(e);
}

static void keydone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
                 WEBKIT_WEB_VIEW(o), res, &err),
             *f;
    char *focused = NULL, q[200];
    static char buf[65536];
    if (!err) {
        f = jsc_value_object_get_property(v, "focused");
        focused = jsc_value_to_json(f, 0);
        g_object_unref(f), g_object_unref(v);
    } else
        g_error_free(err);
    snprintf(buf, sizeof buf,
             "{\"type\":\"key_sent\",\"key\":%s,\"focused\":%s}",
             jq(q, sizeof q, r->key), focused ? focused : "null");
    g_free(focused);
    finish(r, buf);
}

static gboolean keyquery(gpointer p) {
    Req *r = p;
    r->timer = 0;
    jscall(r, "active", "{}", keydone);
    return FALSE;
}

static void keysend(Req *r) {
    sendkey(r->keyval, r->state, 1);
    sendkey(r->keyval, r->state, 0);
    r->timer = g_timeout_add(60, keyquery, r);
}

static void keyfocused(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
                 WEBKIT_WEB_VIEW(o), res, &err),
             *f;
    int ok = 0;
    if (err) {
        replyerr(r, "%s", err->message);
        g_error_free(err);
        return;
    }
    f = jsc_value_object_get_property(v, "focused");
    ok = jsc_value_is_boolean(f) && jsc_value_to_boolean(f);
    g_object_unref(f), g_object_unref(v);
    if (!ok) {
        replyerr(r, "focus_failed");
        return;
    }
    keysend(r);
}

/* key NAME [--shift --ctrl --alt --meta] [--selector css] [--wait];
 * enter and space are shortcuts */
static void c_key(Req *r, Args *a) {
    const char *name = a->argv[0][0] == 'e'   ? "Enter"
                       : a->argv[0][0] == 's' ? " "
                                              : pos(a, 0);
    const char *sel = optstr(a, "selector");
    char opts[8300], q[8200];
    if (!name) {
        replyerr(r, "key needs a key name");
        return;
    }
    if (mode == PROMPT) {
        replyerr(r, "the prompt has the keyboard");
        return;
    }
    r->state = modstate(a);
    r->keyval = keyvalof(name, r->state);
    if (!r->keyval) {
        replyerr(r, "unknown key: %s", name);
        return;
    }
    snprintf(r->key, sizeof r->key, "%s", name);
    r->wait = optflag(a, "wait");
    if (sel) {
        snprintf(opts, sizeof opts, "{\"_\":[],\"selector\":%s}",
                 jq(q, sizeof q, sel));
        jscall(r, "focus", opts, keyfocused);
    } else
        keysend(r);
}

/* type: a --submit with no form around the field presses Return */
static void typedone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    char *j;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(o), res, &err);
    if (err) {
        replyerr(r, "%s", err->message);
        g_error_free(err);
        return;
    }
    j = jsc_value_to_json(v, 0);
    if (j && strstr(j, "\"submit\":\"none\"") && mode != PROMPT) {
        sendkey(GDK_KEY_Return, 0, 1);
        sendkey(GDK_KEY_Return, 0, 0);
    }
    if (!j)
        replyerr(r, "unserializable result");
    else if (strstr(j, "\"type\":\"error\""))
        reply(r, j);
    else
        finish(r, j);
    g_free(j);
    g_object_unref(v);
}

static void c_type(Req *r, Args *a) {
    r->wait = optflag(a, "wait");
    jsrun(r, "type", a, typedone);
}

/* commands. Every handler answers its request exactly once, through
 * reply/replyf/replyerr/finish/ok or by handing it to an async callback.
 * "Raw" verbs (open, tab, private, js, inject, yank, download, prompt,
 * echo, find) take the rest of the line verbatim; the others are
 * tokenized, with chrome-dumper's --flag grammar. */

static void c_open(Req *r, Args *a) {
    char *u = tourl(a->rest);
    webkit_web_view_load_uri(V, u);
    g_free(u);
    finish(r, "{\"type\":\"navigated\"}");
}

static void c_nav(Req *r, Args *a) {
    char *u = tourl(pos(a, 0) ? pos(a, 0) : "");
    webkit_web_view_load_uri(V, u);
    g_free(u);
    r->wait = optflag(a, "wait");
    finish(r, "{\"type\":\"navigated\"}");
}

static void c_tab(Req *r, Args *a) {
    char *u = tourl(a->rest), q[4200];
    int priv = a->argv[0][0] == 'p', pid = spawn(u, priv);
    replyf(r, "{\"type\":\"opened\",\"pid\":%d,\"url\":%s,\"private\":%s}", pid,
           jq(q, sizeof q, u), priv ? "true" : "false");
    g_free(u);
}

static void c_back(Req *r, Args *a) {
    int n = (int)optnum(a, "steps", 1);
    while (n-- > 0)
        webkit_web_view_go_back(V);
    r->wait = optflag(a, "wait");
    finish(r, "{\"type\":\"navigated\"}");
}

static void c_forward(Req *r, Args *a) {
    int n = (int)optnum(a, "steps", 1);
    while (n-- > 0)
        webkit_web_view_go_forward(V);
    r->wait = optflag(a, "wait");
    finish(r, "{\"type\":\"navigated\"}");
}

static void c_reload(Req *r, Args *a) {
    if (a->argv[0][6] == '!')
        webkit_web_view_reload_bypass_cache(V);
    else
        webkit_web_view_reload(V);
    r->wait = optflag(a, "wait");
    finish(r, "{\"type\":\"navigated\"}");
}

static void c_stop(Req *r, Args *a) {
    (void)a;
    webkit_web_view_stop_loading(V);
    find("", 0);
    js("hweb", NULL, "__hweb.cancel()");
    if (mode == HINT)
        setmode(NORMAL);
    setstatus(NULL);
    ok(r);
}

static void c_quit(Req *r, Args *a) {
    (void)a;
    replyf(r, "{\"type\":\"closed\",\"pid\":%d}", (int)getpid());
    gtk_main_quit();
}

/* scroll DX DY (keymap: steps of scrollstep px) or chrome-dumper's
 * scroll [up|down] [--pages F] [--pixels N] [--to top|bottom|css] */
static void c_scroll(Req *r, Args *a) {
    const char *p = pos(a, 0);
    if (p && npos(a) == 2 && a->argc == 3) {
        js("hweb", NULL, "scrollBy(%d,%d)", atoi(p) * scrollstep,
           atoi(pos(a, 1)) * scrollstep);
        ok(r);
        return;
    }
    jsrun(r, "scroll", a, jsrundone);
}

static void c_scrollpage(Req *r, Args *a) {
    const char *p = pos(a, 0);
    js("hweb", NULL, "scrollBy(0,innerHeight*%s)", p ? p : "1");
    ok(r);
}

static void c_scrollto(Req *r, Args *a) {
    const char *p = pos(a, 0);
    if (p && atoi(p) < 0)
        js("hweb", NULL, "scrollTo(0,document.documentElement.scrollHeight)");
    else
        js("hweb", NULL, "scrollTo(0,%d)", p ? atoi(p) : 0);
    ok(r);
}

/* zoom [PCT] | + | - | --in [PCT] | --out [PCT] | --reset; numbers are
 * percent (zoom 100 = 1:1) */
static void c_zoom(Req *r, Args *a) {
    double z = webkit_web_view_get_zoom_level(V);
    const char *p = pos(a, 0);
    int in = optflag(a, "in"), out = optflag(a, "out");
    if (optflag(a, "reset"))
        z = 1.0;
    else if (in || out)
        z += (in ? 1 : -1) * (p ? atof(p) : 10.0) / 100.0;
    else if (p && *p == '+')
        z += zoomstep;
    else if (p && *p == '-')
        z -= zoomstep;
    else if (p)
        z = atof(p) / 100.0;
    z = z < 0.25 ? 0.25 : z > 5.0 ? 5.0 : z;
    if (z != webkit_web_view_get_zoom_level(V))
        webkit_web_view_set_zoom_level(V, z);
    replyf(r, "{\"type\":\"zoomed\",\"percent\":%d,\"factor\":%.3f}",
           (int)(z * 100 + 0.5), z);
}

static void c_find(Req *r, Args *a) {
    find(a->rest, 0);
    ok(r);
}

static void c_findnext(Req *r, Args *a) {
    WebKitFindController *fc = webkit_web_view_get_find_controller(V);
    if (a->argv[0][4] == 'n')
        webkit_find_controller_search_next(fc);
    else
        webkit_find_controller_search_previous(fc);
    ok(r);
}

static void c_insert(Req *r, Args *a) {
    (void)a;
    setmode(INSERT);
    ok(r);
}

static void c_normal(Req *r, Args *a) {
    (void)a;
    js("hweb", NULL, "__hweb.blur()");
    setmode(NORMAL);
    ok(r);
}

static void c_hint(Req *r, Args *a) {
    const char *p = pos(a, 0);
    char *c = jsstr(hintchars), *act = jsstr(p ? p : "open");
    setmode(HINT);
    js("hweb", NULL, "__hweb.start(%s,%s)", c, act);
    g_free(c), g_free(act);
    ok(r);
}

/* js CODE: an expression, or a function body when it contains `return`
 * (chrome-dumper style: `js 'return {w: innerWidth}'`), in the page world.
 * A body may return a promise; it is awaited. */
static void c_js(Req *r, Args *a) {
    const char *p = a->rest;
    int body = 0;
    while ((p = strstr(p, "return"))) {
        if ((p == a->rest || !g_ascii_isalnum(p[-1])) &&
            !g_ascii_isalnum(p[6]) && p[6] != '_') {
            body = 1;
            break;
        }
        p += 6;
    }
    if (body)
        webkit_web_view_call_async_javascript_function(
            V, a->rest, -1, NULL, NULL, NULL, NULL, jsfndone, r);
    else
        js(NULL, r, "%s", a->rest);
}

static void c_inject(Req *r, Args *a) {
    inject(a->rest, 1);
    ok(r);
}

static void c_inspect(Req *r, Args *a) {
    WebKitWebInspector *in = webkit_web_view_get_inspector(V);
    (void)a;
    if (inspecting)
        webkit_web_inspector_close(in);
    else
        webkit_web_inspector_show(in);
    ok(r);
}

static void c_yank(Req *r, Args *a) {
    const char *u = *a->rest ? a->rest : webkit_web_view_get_uri(V);
    char q[4200];
    if (!u) {
        replyerr(r, "nothing to yank");
        return;
    }
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), u, -1);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), u, -1);
    setstatus("yanked");
    ev("yank %s", u);
    replyf(r, "{\"type\":\"yanked\",\"url\":%s}", jq(q, sizeof q, u));
}

static void c_download(Req *r, Args *a) {
    const char *u = *a->rest ? a->rest : webkit_web_view_get_uri(V);
    if (!u) {
        replyerr(r, "nothing to download");
        return;
    }
    webkit_web_view_download_uri(V, u);
    ok(r);
}

static void c_prompt(Req *r, Args *a) {
    prompt(a->rest);
    ok(r);
}

static void c_echo(Req *r, Args *a) {
    setstatus(a->rest);
    ok(r);
}

static void c_info(Req *r, Args *a) {
    const char *t = webkit_web_view_get_title(V),
               *u = webkit_web_view_get_uri(V);
    double z = webkit_web_view_get_zoom_level(V);
    char qt[4096], qu[4096], qp[4096];
    (void)a;
    replyf(r,
           "{\"type\":\"info\",\"pid\":%d,\"url\":%s,\"title\":%s,"
           "\"focused\":%s,\"active\":%lld,\"private\":%s,\"profile\":%s,"
           "\"zoom\":%d,\"mode\":\"%s\",\"width\":%d,\"height\":%d}",
           (int)getpid(), jq(qu, sizeof qu, u ? u : ""),
           jq(qt, sizeof qt, t ? t : ""),
           gtk_window_is_active(GTK_WINDOW(win)) ? "true" : "false",
           (long long)lastactive, private ? "true" : "false",
           jq(qp, sizeof qp, profile), (int)(z * 100 + 0.5), modename[mode],
           (int)(gtk_widget_get_allocated_width(view) / z),
           (int)(gtk_widget_get_allocated_height(view) / z));
}

static void c_title(Req *r, Args *a) {
    const char *t = webkit_web_view_get_title(V);
    ev("title %s", t ? t : "");
    ev("uri %s", webkit_web_view_get_uri(V));
    c_info(r, a);
}

static gboolean resized(gpointer p) {
    Req *r = p;
    int w, h;
    r->timer = 0;
    gtk_window_get_size(GTK_WINDOW(win), &w, &h);
    replyf(r, "{\"type\":\"resized\",\"width\":%d,\"height\":%d}", w, h);
    return FALSE;
}

/* resize WxH: a request to the WM, which may tile us regardless */
static void c_resize(Req *r, Args *a) {
    const char *p = pos(a, 0);
    int w = 0, h = 0;
    if (p &&
        (sscanf(p, "%dx%d", &w, &h) == 2 || sscanf(p, "%d,%d", &w, &h) == 2) &&
        w > 0 && h > 0)
        gtk_window_resize(GTK_WINDOW(win), w, h);
    else if (p) {
        replyerr(r, "resize: expected WxH");
        return;
    }
    r->timer = g_timeout_add(100, resized, r);
}

static void c_blockupdate(Req *r, Args *a) {
    char *path = expandhome(blocklist), *out = NULL, *err = NULL, q[4096];
    const char *argv[] = {"/bin/sh", "-c", blockupdate, "sh", path, NULL};
    const char *msg = "";
    (void)a;
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out,
                 &err, NULL, NULL);
    if (out && *g_strstrip(out))
        msg = out;
    else if (err)
        msg = g_strstrip(err);
    ev("blockupdate %s", msg);
    setstatus(msg);
    replyf(r, "{\"type\":\"blockupdate\",\"output\":%s}", jq(q, sizeof q, msg));
    g_free(out), g_free(err), g_free(path);
}

/* files: <dumpdir>/<pid>_<title>.<ext> unless --out gave a path */
static const char *outpath(Req *r, const char *ext) {
    const char *t = webkit_web_view_get_title(V);
    char *dir, name[96];
    size_t i = 0;
    if (r->path[0])
        return r->path;
    for (; t && *t && i < 80; t++)
        name[i++] = g_ascii_isalnum(*t) || strchr("._-", *t) ? *t : '_';
    name[i] = 0;
    if (!i)
        strcpy(name, "page");
    dir = expandhome(dumpdir);
    g_mkdir_with_parents(dir, 0755);
    snprintf(r->path, sizeof r->path, "%s/%d_%s.%s", dir, (int)getpid(), name,
             ext);
    g_free(dir);
    return r->path;
}

static void snapdone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    cairo_surface_t *s =
        webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(o), res, &err);
    double k = zoom() * gtk_widget_get_scale_factor(view);
    const char *path;
    char qp[4300], qu[4096], qt[4096], rect[128];
    int w, h, ok;
    if (err) {
        replyerr(r, "%s", err->message);
        g_error_free(err);
        return;
    }
    if (r->w > 0 && r->h > 0) { /* crop, CSS px -> device px */
        cairo_surface_t *c = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, (int)(r->w * k), (int)(r->h * k));
        cairo_t *cr = cairo_create(c);
        cairo_set_source_surface(cr, s, -r->x * k, -r->y * k);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(s);
        s = c;
        snprintf(rect, sizeof rect,
                 "{\"x\":%g,\"y\":%g,\"width\":%g,\"height\":%g}", r->x, r->y,
                 r->w, r->h);
    } else
        strcpy(rect, "null");
    w = cairo_image_surface_get_width(s);
    h = cairo_image_surface_get_height(s);
    path = outpath(r, r->fmt[0] == 'j' ? "jpg" : "png");
    if (r->fmt[0] == 'j') {
        GdkPixbuf *pb = gdk_pixbuf_get_from_surface(s, 0, 0, w, h);
        char q[8];
        snprintf(q, sizeof q, "%d", CLAMP(r->quality, 1, 100));
        ok = pb && gdk_pixbuf_save(pb, path, "jpeg", NULL, "quality", q, NULL);
        if (pb)
            g_object_unref(pb);
    } else
        ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    if (!ok) {
        replyerr(r, "cannot write %s", path);
        return;
    }
    replyf(
        r,
        "{\"type\":\"screenshot_result\",\"path\":%s,\"url\":%s,\"title\":%s,"
        "\"format\":\"%s\",\"width\":%d,\"height\":%d,\"rect\":%s}",
        jq(qp, sizeof qp, path), jq(qu, sizeof qu, webkit_web_view_get_uri(V)),
        jq(qt, sizeof qt, webkit_web_view_get_title(V)), r->fmt, w, h, rect);
}

static void snapshot(Req *r) {
    webkit_web_view_get_snapshot(V,
                                 r->full ? WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT
                                         : WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, NULL, snapdone,
                                 r);
}

static double prop(JSCValue *o, const char *name) {
    JSCValue *v = jsc_value_object_get_property(o, name);
    double d = jsc_value_is_number(v) ? jsc_value_to_double(v) : 0;
    g_object_unref(v);
    return d;
}

/* the crop rectangle came back from auto.js */
static void shotdone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
                 WEBKIT_WEB_VIEW(o), res, &err),
             *rect, *e;
    char *msg;
    if (err) {
        replyerr(r, "%s", err->message);
        g_error_free(err);
        return;
    }
    e = jsc_value_object_get_property(v, "error");
    if (jsc_value_is_string(e)) {
        msg = jsc_value_to_string(e);
        replyerr(r, "%s", msg);
        g_free(msg), g_object_unref(e), g_object_unref(v);
        return;
    }
    g_object_unref(e);
    rect = jsc_value_object_get_property(v, "rect");
    if (jsc_value_is_object(rect)) {
        r->x = prop(rect, "x");
        r->y = prop(rect, "y");
        r->w = prop(rect, "width");
        r->h = prop(rect, "height");
    }
    g_object_unref(rect), g_object_unref(v);
    snapshot(r);
}

/* screenshot [--format png|jpeg] [--quality N] [--rect x,y,w,h |
 * --selector css | --text s] [--full] [--out PATH]: the viewport (or the
 * whole document with --full) to a file */
static void c_screenshot(Req *r, Args *a) {
    const char *fmt = optstr(a, "format"), *out = optstr(a, "out");
    snprintf(r->fmt, sizeof r->fmt, "%s",
             fmt && !strcmp(fmt, "jpeg") ? "jpeg" : "png");
    r->quality = (int)optnum(a, "quality", 85);
    r->full = optflag(a, "full");
    if (out)
        snprintf(r->path, sizeof r->path, "%s", out);
    if (optstr(a, "selector") || optstr(a, "text") || optstr(a, "rect"))
        jsrun(r, "shot", a, shotdone);
    else
        snapshot(r);
}

static void jqg(GString *g, const char *s) {
    g_string_append_c(g, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            g_string_append_printf(g, "\\%c", c);
        else if (c == '\n')
            g_string_append(g, "\\n");
        else if (c == '\r')
            g_string_append(g, "\\r");
        else if (c == '\t')
            g_string_append(g, "\\t");
        else if (c < 0x20)
            g_string_append_printf(g, "\\u%04x", c);
        else
            g_string_append_c(g, (char)c);
    }
    g_string_append_c(g, '"');
}

static void dumpdone(GObject *o, GAsyncResult *res, gpointer p) {
    Req *r = p;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(
                 WEBKIT_WEB_VIEW(o), res, &err),
             *h;
    char *html;
    const char *path;
    GString *g;
    char qp[4300], qu[4096], qt[4096];
    if (err) {
        replyerr(r, "%s", err->message);
        g_error_free(err);
        return;
    }
    h = jsc_value_object_get_property(v, "html");
    html = jsc_value_to_string(h);
    g_object_unref(h), g_object_unref(v);
    path = outpath(r, "html");
    if (!g_file_set_contents(path, html, -1, NULL)) {
        replyerr(r, "cannot write %s", path);
        g_free(html);
        return;
    }
    g = g_string_new(NULL);
    g_string_append_printf(g,
                           "{\"type\":\"dump_result\",\"path\":%s,\"bytes\":%"
                           "zu,\"url\":%s,\"title\":%s",
                           jq(qp, sizeof qp, path), strlen(html),
                           jq(qu, sizeof qu, webkit_web_view_get_uri(V)),
                           jq(qt, sizeof qt, webkit_web_view_get_title(V)));
    if (r->full) {
        g_string_append(g, ",\"html\":");
        jqg(g, html);
    }
    g_string_append_c(g, '}');
    reply(r, g->str);
    g_string_free(g, TRUE);
    g_free(html);
}

/* dump [--out PATH] [--stdout]: the live DOM to a file */
static void c_dump(Req *r, Args *a) {
    const char *out = optstr(a, "out");
    if (out)
        snprintf(r->path, sizeof r->path, "%s", out);
    r->full = optflag(a, "stdout");
    jscall(r, "dump", "{}", dumpdone);
}

static const struct command commands[] = {
    /* name          handler        boolean flags */
    {"open", c_open, ""},
    {"nav", c_nav, "wait"},
    {"tab", c_tab, ""},
    {"private", c_tab, ""},
    {"back", c_back, "wait"},
    {"forward", c_forward, "wait"},
    {"reload", c_reload, "wait"},
    {"reload!", c_reload, "wait"},
    {"stop", c_stop, ""},
    {"quit", c_quit, ""},
    {"close", c_quit, ""},
    {"scroll", c_scroll, "no-smooth"},
    {"scrollpage", c_scrollpage, ""},
    {"scrollto", c_scrollto, ""},
    {"zoom", c_zoom, "in,out,reset"},
    {"find", c_find, ""},
    {"findnext", c_findnext, ""},
    {"findprev", c_findnext, ""},
    {"insert", c_insert, ""},
    {"normal", c_normal, ""},
    {"hint", c_hint, ""},
    {"js", c_js, ""},
    {"inject", c_inject, ""},
    {"inspect", c_inspect, ""},
    {"yank", c_yank, ""},
    {"download", c_download, ""},
    {"prompt", c_prompt, ""},
    {"echo", c_echo, ""},
    {"title", c_title, ""},
    {"info", c_info, ""},
    {"windows", c_info, ""},
    {"resize", c_resize, ""},
    {"blockupdate", c_blockupdate, ""},
    /* auto.js */
    {"click", c_auto, "wait"},
    {"focus", c_auto, ""},
    {"type", c_type, "no-clear,submit,wait"},
    {"input", c_type, "no-clear,submit,wait"},
    {"select", c_auto, "no-mouse,no-scroll,no-focus"},
    {"select-clear", c_auto, ""},
    {"highlight", c_auto, "all,no-scroll"},
    {"clear-highlights", c_auto, ""},
    {"metrics", c_auto, ""},
    {"active", c_auto, ""},
    {"screenshot", c_screenshot, "full"},
    {"dump", c_dump, "stdout"},
    /* synthesized input */
    {"mouse-move", c_mousemove, "instant,shift,ctrl,alt,meta"},
    {"mouse-up", c_mousemove, "less,more,instant,shift,ctrl,alt,meta"},
    {"mouse-down", c_mousemove, "less,more,instant,shift,ctrl,alt,meta"},
    {"mouse-left", c_mousemove, "less,more,instant,shift,ctrl,alt,meta"},
    {"mouse-right", c_mousemove, "less,more,instant,shift,ctrl,alt,meta"},
    {"mouse-click", c_mouseclick, "double,instant,wait,shift,ctrl,alt,meta"},
    {"mouse-press", c_mousepress, "shift,ctrl,alt,meta"},
    {"mouse-release", c_mousepress, "shift,ctrl,alt,meta"},
    {"mouse-drag", c_mousedrag, "shift,ctrl,alt,meta"},
    {"mouse-scroll", c_mousescroll, "less,more,shift,ctrl,alt,meta"},
    {"mouse-hide", c_mousehide, ""},
    {"key", c_key, "shift,ctrl,alt,meta,wait"},
    {"enter", c_key, "shift,ctrl,alt,meta,wait"},
    {"space", c_key, "shift,ctrl,alt,meta,wait"},
};

static void cmdreq(Req *r, const char *line) {
    Args a;
    size_t i;
    if (argsparse(&a, line) < 1) {
        if (a.argc < 0)
            replyerr(r, "unterminated quote");
        else
            reqfree(r);
        return;
    }
    for (i = 0; i < LENGTH(commands); i++)
        if (!strcmp(commands[i].name, a.argv[0])) {
            a.bools = commands[i].bools;
            commands[i].fn(r, &a);
            return;
        }
    replyerr(r, "unknown command: %s", a.argv[0]);
}

/* keymap, prompt and hint actions: no reply channel */
static void cmd(const char *line) {
    Req *r = reqnew(SINK_NONE);
    if (r)
        cmdreq(r, line);
}

/* Tab in the : prompt: a verb prefix lists commands, an open/tab argument
 * lists fuzzy history matches; further Tabs cycle (dir = -1 for Shift-Tab) */
static void complete(int dir) {
    if (!comptexts) {
        const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
        char verb[32], buf[4096];
        size_t n, i;
        if (t[0] != ':')
            return;
        t++;
        n = strcspn(t, " ");
        comptexts = g_ptr_array_new_with_free_func(g_free);
        compdisps = g_ptr_array_new_with_free_func(g_free);
        if (!t[n]) {
            for (i = 0; i < LENGTH(commands); i++)
                if (!strncmp(commands[i].name, t, n)) {
                    snprintf(buf, sizeof buf, ":%s ", commands[i].name);
                    compadd(buf, commands[i].name);
                }
        } else if (n < sizeof verb) {
            GPtrArray *h;
            memcpy(verb, t, n);
            verb[n] = 0;
            t += n;
            while (*t == ' ')
                t++;
            if (strcmp(verb, "open") && strcmp(verb, "tab") &&
                strcmp(verb, "private") && strcmp(verb, "nav")) {
                compclear();
                return;
            }
            h = histmatch(t, compmax * 5);
            for (i = 0; i < h->len; i++) {
                char *line = g_ptr_array_index(h, i),
                     *title = strchr(line, '\t');
                if (title)
                    *title++ = 0;
                char disp[4096];
                snprintf(buf, sizeof buf, ":%s %s", verb, line);
                snprintf(disp, sizeof disp, "%s%s%s", line,
                         title && *title ? "  " : "", title ? title : "");
                compadd(buf, disp);
            }
            g_ptr_array_free(h, TRUE);
        }
        if (!comptexts->len) {
            compclear();
            return;
        }
        compidx = dir < 0 ? comptexts->len - 1 : 0;
    } else
        compidx = (compidx + comptexts->len + dir) % comptexts->len;
    compshow();
}

/* %u url, %t title, %c clipboard */
static void expand(const char *in, char *out, size_t n) {
    GString *g = g_string_new(NULL);
    for (; *in; in++) {
        if (*in == '%' && in[1]) {
            const char *s = NULL;
            char *clip = NULL;
            in++;
            if (*in == 'u')
                s = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view));
            else if (*in == 't')
                s = webkit_web_view_get_title(WEBKIT_WEB_VIEW(view));
            else if (*in == 'c')
                s = clip = gtk_clipboard_wait_for_text(
                    gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
            else
                g_string_append_c(g, '%');
            if (s)
                g_string_append(g, g_strstrip(s == clip ? clip : (char *)s));
            g_free(clip);
        } else
            g_string_append_c(g, *in);
    }
    snprintf(out, n, "%s", g->str);
    g_string_free(g, TRUE);
}

static void keytoken(GdkEventKey *e, char *buf, size_t n) {
    /* the raw state has Mod4, not the virtual SUPER bit; translate first */
    GdkModifierType state = e->state;
    guint mods;
    gdk_keymap_add_virtual_modifiers(
        gdk_keymap_get_for_display(gdk_display_get_default()), &state);
    mods = state & gtk_accelerator_get_default_mod_mask() & ~GDK_SHIFT_MASK;
    gunichar u = gdk_keyval_to_unicode(e->keyval);
    if (mods & GDK_CONTROL_MASK)
        snprintf(buf, n, "<C-%s>",
                 gdk_keyval_name(gdk_keyval_to_lower(e->keyval)));
    else if (mods & GDK_MOD1_MASK)
        snprintf(buf, n, "<M-%s>",
                 gdk_keyval_name(gdk_keyval_to_lower(e->keyval)));
    else if (mods & GDK_SUPER_MASK)
        /* keyval keeps its case, so <D-b> and <D-B> (shift) differ */
        snprintf(buf, n, "<D-%s>", gdk_keyval_name(e->keyval));
    else if (u && g_unichar_isprint(u))
        buf[g_unichar_to_utf8(u, buf)] = 0;
    else
        snprintf(buf, n, "<%s>", gdk_keyval_name(e->keyval));
}

static gboolean keypress(GtkWidget *w, GdkEventKey *e, gpointer data) {
    char tok[48], out[4096];
    int prefix = 0;
    size_t i;
    (void)w, (void)data;

    if (e->is_modifier)
        return FALSE;
    keytoken(e, tok, sizeof tok);

    switch (mode) {
    case PROMPT:
        if (e->keyval == GDK_KEY_Escape) {
            unprompt();
            return TRUE;
        }
        if (e->keyval == GDK_KEY_Tab || e->keyval == GDK_KEY_ISO_Left_Tab) {
            complete(e->keyval == GDK_KEY_Tab && !(e->state & GDK_SHIFT_MASK)
                         ? 1
                         : -1);
            return TRUE;
        }
        return FALSE;
    case INSERT:
        if (e->keyval == GDK_KEY_Escape) {
            cmd("normal");
            return TRUE;
        }
        return FALSE;
    case HINT:
        if (e->keyval == GDK_KEY_Escape) {
            cmd("stop");
        } else if (e->keyval == GDK_KEY_BackSpace) {
            js("hweb", NULL, "__hweb.key('\\b')");
        } else if (tok[0] != '<') {
            char *s = jsstr(tok);
            js("hweb", NULL, "__hweb.key(%s)", s);
            g_free(s);
        }
        return TRUE;
    case NORMAL:
        break;
    }

    if (strlen(pending) + strlen(tok) >= sizeof pending)
        pending[0] = 0;
    strcat(pending, tok);
    for (i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (!strcmp(keys[i].keys, pending)) {
            pending[0] = 0;
            updatemode();
            expand(keys[i].cmd, out, sizeof out);
            cmd(out);
            return TRUE;
        }
        if (!strncmp(keys[i].keys, pending, strlen(pending)))
            prefix = 1;
    }
    if (!prefix)
        pending[0] = 0;
    updatemode();
    return TRUE;
}

static void entryactivate(GtkEntry *en, gpointer data) {
    char text[4096];
    (void)data;
    snprintf(text, sizeof text, "%s", gtk_entry_get_text(en));
    unprompt();
    if (text[0] == ':')
        cmd(text + 1);
    else if (text[0] == '/')
        find(text + 1, 0);
    else if (text[0])
        cmd(text);
}

static void entrychanged(GtkEditable *en, gpointer data) {
    const char *t = gtk_entry_get_text(GTK_ENTRY(en));
    (void)data;
    if (!compquiet)
        compclear();
    if (t[0] == '/')
        find(t + 1, 0);
}

static void message(WebKitUserContentManager *m, WebKitJavascriptResult *r,
                    gpointer data) {
    JSCValue *v = webkit_javascript_result_get_js_value(r);
    char *s = jsc_value_to_string(v);
    (void)m, (void)data;
    if (!strncmp(s, "mode insert", 11)) {
        if (mode == NORMAL || mode == INSERT)
            setmode(INSERT);
    } else if (!strncmp(s, "mode normal", 11)) {
        if (mode == INSERT)
            setmode(NORMAL);
    } else if (!strncmp(s, "hint ", 5)) {
        const char *rest = s + 5;
        if (mode == HINT)
            setmode(NORMAL);
        if (!strncmp(rest, "new ", 4) && rest[4])
            spawn(rest + 4, 0);
        else if ((!strncmp(rest, "yank ", 5) && rest[5]) ||
                 (!strncmp(rest, "download ", 9) && rest[9]))
            cmd(rest); /* the hint action is the command verb */
        else if (!strcmp(rest, "none"))
            setstatus("no hints");
    } else
        ev("msg %s", s);
    g_free(s);
}

/* the title usually arrives after load finished: record the visit once it
 * does, or without one when the next load starts first */
static void histflush(const char *title, int force) {
    if (!histpending || (!force && (!title || !*title)))
        return;
    if (!private)
        histadd(histpending, title);
    g_free(histpending);
    histpending = NULL;
}

static void loadchanged(WebKitWebView *v, WebKitLoadEvent e, gpointer data) {
    (void)data;
    switch (e) {
    case WEBKIT_LOAD_STARTED:
        ev("load started %s", webkit_web_view_get_uri(v));
        histflush(webkit_web_view_get_title(v), 1);
        waitload(e, "");
        break;
    case WEBKIT_LOAD_COMMITTED:
        ev("load committed %s", webkit_web_view_get_uri(v));
        setstatus(NULL);
        break;
    case WEBKIT_LOAD_FINISHED:
        ev("load finished %s", webkit_web_view_get_uri(v));
        setstatus(NULL);
        g_free(histpending);
        histpending = g_strdup(webkit_web_view_get_uri(v));
        histflush(webkit_web_view_get_title(v), 0);
        waitload(e, "");
        break;
    default:
        break;
    }
}

static gboolean loadfailed(WebKitWebView *v, WebKitLoadEvent e, const char *uri,
                           GError *err, gpointer data) {
    char buf[1200], q[1024];
    (void)v, (void)e, (void)data;
    if (g_error_matches(err, WEBKIT_NETWORK_ERROR,
                        WEBKIT_NETWORK_ERROR_CANCELLED))
        return FALSE;
    snprintf(buf, sizeof buf, "load failed: %s", err->message);
    setstatus(buf);
    ev("load failed %s %s", uri, err->message);
    snprintf(buf, sizeof buf, "\"failed\":%s,", jq(q, sizeof q, err->message));
    waitload(WEBKIT_LOAD_FINISHED, buf);
    return FALSE;
}

static void progress(GObject *o, GParamSpec *p, gpointer data) {
    double d = webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(o));
    char buf[4096];
    (void)p, (void)data;
    if (d < 1.0) {
        snprintf(buf, sizeof buf, "[%d%%] %s", (int)(d * 100),
                 webkit_web_view_get_uri(WEBKIT_WEB_VIEW(o)));
        setstatus(buf);
    } else
        setstatus(NULL);
}

static void titlechanged(GObject *o, GParamSpec *p, gpointer data) {
    (void)o, (void)p, (void)data;
    const char *t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(view));
    updatetitle();
    ev("title %s", t ? t : "");
    if (histpending &&
        !strcmp(histpending, webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view))))
        histflush(t, 0);
}

static void urichanged(GObject *o, GParamSpec *p, gpointer data) {
    (void)o, (void)p, (void)data;
    updatetitle();
    setstatus(NULL);
    ev("uri %s", webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view)));
}

static void hover(WebKitWebView *v, WebKitHitTestResult *h, guint mods,
                  gpointer data) {
    (void)v, (void)mods, (void)data;
    g_free(hoveruri);
    hoveruri = webkit_hit_test_result_context_is_link(h)
                   ? g_strdup(webkit_hit_test_result_get_link_uri(h))
                   : NULL;
    setstatus(NULL);
    ev("hover %s", hoveruri ? hoveruri : "");
}

/* scripted popups (window.open): same-process windows related to their
 * opener, so window.opener/postMessage work and OAuth flows can hand
 * their result back. A bare webview in a plain window: no status bar,
 * no keymap, closes on window.close(). */
static void popupclose(WebKitWebView *pv, gpointer w) {
    (void)pv;
    gtk_widget_destroy(GTK_WIDGET(w));
}

static void popupready(WebKitWebView *pv, gpointer w) {
    (void)pv;
    gtk_widget_show_all(GTK_WIDGET(w));
}

static void popuptitle(GObject *o, GParamSpec *p, gpointer w) {
    const char *t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(o));
    (void)p;
    gtk_window_set_title(GTK_WINDOW(w), t && *t ? t : "hweb");
}

/* new-window requests: a real click gets its own hweb process, a
 * scripted window.open gets an opener-linked popup in this one */
static GtkWidget *create(WebKitWebView *v, WebKitNavigationAction *a,
                         gpointer data) {
    GtkWidget *w, *pv;
    (void)data;

    if (webkit_navigation_action_get_mouse_button(a)) {
        spawn(
            webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)),
            0);
        return NULL;
    }
    pv = webkit_web_view_new_with_related_view(v);
    webkit_web_view_set_settings(WEBKIT_WEB_VIEW(pv),
                                 webkit_web_view_get_settings(v));
    w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(w), 600, 700);
    gtk_window_set_role(GTK_WINDOW(w), "popup");
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(win));
    gtk_container_add(GTK_CONTAINER(w), pv);
    g_signal_connect(pv, "ready-to-show", G_CALLBACK(popupready), w);
    g_signal_connect(pv, "close", G_CALLBACK(popupclose), w);
    g_signal_connect(pv, "notify::title", G_CALLBACK(popuptitle), w);
    ev("popup %s",
       webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)));
    return pv;
}

/* mic/camera/screen requests are granted (that is what the device is
 * for); everything else keeps WebKit's default refusal */
static gboolean permission(WebKitWebView *v, WebKitPermissionRequest *r,
                           gpointer data) {
    (void)v, (void)data;
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(r)) {
        WebKitUserMediaPermissionRequest *m =
            WEBKIT_USER_MEDIA_PERMISSION_REQUEST(r);
        ev("permission media%s%s%s",
           webkit_user_media_permission_is_for_audio_device(m) ? " audio" : "",
           webkit_user_media_permission_is_for_video_device(m) ? " video" : "",
           webkit_user_media_permission_is_for_display_device(m) ? " display"
                                                                 : "");
        webkit_permission_request_allow(r);
        return TRUE;
    }
    if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(r)) {
        webkit_permission_request_allow(r); /* device labels */
        return TRUE;
    }
    return FALSE;
}

static gboolean policy(WebKitWebView *v, WebKitPolicyDecision *d,
                       WebKitPolicyDecisionType t, gpointer data) {
    (void)v, (void)data;
    if (t == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction *a =
            webkit_navigation_policy_decision_get_navigation_action(
                WEBKIT_NAVIGATION_POLICY_DECISION(d));
        /* ctrl+click or middle-click a link: new hweb window */
        if (webkit_navigation_action_get_navigation_type(a) ==
                WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
            (webkit_navigation_action_get_mouse_button(a) == 2 ||
             (webkit_navigation_action_get_modifiers(a) & GDK_CONTROL_MASK))) {
            spawn(webkit_uri_request_get_uri(
                      webkit_navigation_action_get_request(a)),
                  0);
            webkit_policy_decision_ignore(d);
            return TRUE;
        }
    }
    if (t == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationAction *a =
            webkit_navigation_policy_decision_get_navigation_action(
                WEBKIT_NAVIGATION_POLICY_DECISION(d));
        /* clicks get their own process; scripted requests fall through
         * to the default policy so ::create makes an opener popup */
        if (webkit_navigation_action_get_mouse_button(a)) {
            spawn(webkit_uri_request_get_uri(
                      webkit_navigation_action_get_request(a)),
                  0);
            webkit_policy_decision_ignore(d);
            return TRUE;
        }
        return FALSE;
    }
    if (t == WEBKIT_POLICY_DECISION_TYPE_RESPONSE &&
        !webkit_response_policy_decision_is_mime_type_supported(
            WEBKIT_RESPONSE_POLICY_DECISION(d))) {
        webkit_policy_decision_download(d);
        return TRUE;
    }
    return FALSE;
}

static gboolean dldest(WebKitDownload *dl, const char *name, gpointer data) {
    char *dir = expandhome(downloaddir), *path, *uri;
    (void)data;
    g_mkdir_with_parents(dir, 0755);
    path = g_build_filename(dir, *name ? name : "download", NULL);
    uri = g_filename_to_uri(path, NULL, NULL);
    webkit_download_set_destination(dl, uri);
    ev("download started %s", path);
    g_free(dir), g_free(path), g_free(uri);
    return TRUE;
}

static void dldone(WebKitDownload *dl, gpointer data) {
    (void)data;
    ev("download finished %s", webkit_download_get_destination(dl));
    setstatus("download finished");
}

static void dlstarted(WebKitWebContext *c, WebKitDownload *dl, gpointer data) {
    (void)c, (void)data;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(dldest), NULL);
    g_signal_connect(dl, "finished", G_CALLBACK(dldone), NULL);
}

static gboolean inspectopen(WebKitWebInspector *i, gpointer d) {
    (void)i, (void)d;
    inspecting = 1;
    return FALSE;
}

static void inspectclosed(WebKitWebInspector *i, gpointer d) {
    (void)i, (void)d;
    inspecting = 0;
}

static gboolean stdinline(GIOChannel *ch, GIOCondition c, gpointer data) {
    char *line = NULL;
    gsize n;
    (void)data;
    if (c & G_IO_IN && g_io_channel_read_line(ch, &line, &n, NULL, NULL) ==
                           G_IO_STATUS_NORMAL) {
        Req *r;
        g_strchomp(line);
        if ((r = reqnew(SINK_STDOUT)))
            cmdreq(r, line);
        g_free(line);
        return TRUE;
    }
    return FALSE;
}

/* control socket: $XDG_RUNTIME_DIR/hweb/<pid>.sock, one request line per
 * connection, one JSON line back (hwebc is the client). Sockets of dead
 * windows are swept whenever the directory is looked at. */
static void sockdir(char *dst, size_t n) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    DIR *d;
    struct dirent *e;
    if (rt && *rt)
        snprintf(dst, n, "%s/hweb", rt);
    else
        snprintf(dst, n, "/tmp/hweb-%d", (int)getuid());
    g_mkdir_with_parents(dst, 0700);
    if (!(d = opendir(dst)))
        return;
    while ((e = readdir(d))) {
        int pid;
        char p[512];
        if (sscanf(e->d_name, "%d.sock", &pid) == 1 && kill(pid, 0) < 0 &&
            errno == ESRCH) {
            snprintf(p, sizeof p, "%s/%s", dst, e->d_name);
            unlink(p);
        }
    }
    closedir(d);
}

static gboolean sockaccept(GIOChannel *ch, GIOCondition c, gpointer data) {
    static char buf[65536];
    struct timeval tv = {1, 0};
    ssize_t n, tot = 0;
    int fd = accept(lsock, NULL, NULL);
    Req *r;
    (void)ch, (void)c, (void)data;
    if (fd < 0)
        return TRUE;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (tot < (ssize_t)sizeof buf - 1 &&
           (n = read(fd, buf + tot, sizeof buf - 1 - tot)) > 0) {
        tot += n;
        if (memchr(buf + tot - n, '\n', (size_t)n))
            break;
    }
    buf[tot] = 0;
    buf[strcspn(buf, "\n")] = 0;
    if ((r = reqnew(fd)))
        cmdreq(r, buf);
    return TRUE;
}

static void listensock(void) {
    struct sockaddr_un sa;
    char dir[200];
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    sockdir(dir, sizeof dir);
    snprintf(sockpath, sizeof sockpath, "%s/%d.sock", dir, (int)getpid());
    if (strlen(sockpath) >= sizeof sa.sun_path) {
        fprintf(stderr, "hweb: socket path too long: %s\n", sockpath);
        sockpath[0] = 0;
        return;
    }
    memcpy(sa.sun_path, sockpath, strlen(sockpath) + 1);
    unlink(sockpath); /* a dead window with our pid */
    if ((lsock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
        bind(lsock, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        listen(lsock, 8) < 0) {
        fprintf(stderr, "hweb: cannot listen on %s\n", sockpath);
        sockpath[0] = 0;
        return;
    }
    fcntl(lsock, F_SETFD, FD_CLOEXEC);
    g_io_add_watch(g_io_channel_unix_new(lsock), G_IO_IN, sockaccept, NULL);
}

static gboolean quitsig(gpointer d) {
    (void)d;
    gtk_main_quit();
    return FALSE;
}

static void activechanged(GObject *o, GParamSpec *p, gpointer d) {
    (void)p, (void)d;
    if (gtk_window_is_active(GTK_WINDOW(o)))
        lastactive = g_get_real_time();
}

static void loadscripts(void) {
    char *dir = expandhome(scriptdir);
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        if (g_str_has_suffix(name, ".js")) {
            char *p = g_build_filename(dir, name, NULL);
            inject(p, 0);
            g_free(p);
        }
    }
    if (d)
        g_dir_close(d);
    g_free(dir);
}

/* messages from the extension: "blocked" carries the url of a cancelled
 * request */
static gboolean usermessage(WebKitWebView *v, WebKitUserMessage *m,
                            gpointer data) {
    (void)v, (void)data;
    if (!strcmp(webkit_user_message_get_name(m), "blocked"))
        ev("blocked %s",
           g_variant_get_string(webkit_user_message_get_parameters(m), NULL));
    return TRUE;
}

static WebKitWebContext *context(void) {
    char *data = profile[0]
                     ? g_strdup(profile)
                     : g_build_filename(g_get_user_data_dir(), "hweb", NULL);
    char *cache = profile[0]
                      ? g_build_filename(profile, "cache", NULL)
                      : g_build_filename(g_get_user_cache_dir(), "hweb", NULL);
    char *cookies = g_build_filename(data, "cookies.sqlite", NULL);
    WebKitWebsiteDataManager *dm =
        private ? webkit_website_data_manager_new_ephemeral()
                : webkit_website_data_manager_new("base-data-directory", data,
                                                  "base-cache-directory", cache,
                                                  NULL);
    WebKitWebContext *ctx =
        webkit_web_context_new_with_website_data_manager(dm);
    WebKitCookieManager *cm = webkit_web_context_get_cookie_manager(ctx);
    GVariantBuilder b;
    char *bl;
    size_t i;

    g_mkdir_with_parents(data, 0700);
    /* private windows still read history for completion, just never write */
    histinit(g_build_filename(data, "history", NULL), histfilter);
    if (!private)
        webkit_cookie_manager_set_persistent_storage(
            cm, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    webkit_cookie_manager_set_accept_policy(
        cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
    webkit_web_context_set_preferred_languages(ctx, languages);

    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ss)"));
    for (i = 0; i < sizeof headers / sizeof *headers; i++)
        g_variant_builder_add(&b, "(ss)", headers[i].name, headers[i].value);
    webkit_web_context_set_web_extensions_directory(ctx, exedir);
    bl = expandhome(blocklist);
    webkit_web_context_set_web_extensions_initialization_user_data(
        ctx, g_variant_new("(@a(ss)s)", g_variant_builder_end(&b), bl));
    g_signal_connect(ctx, "download-started", G_CALLBACK(dlstarted), NULL);

    g_free(data), g_free(cache), g_free(cookies), g_free(bl);
    return ctx;
}

static void setup(void) {
    GtkWidget *box, *hbox;
    GtkCssProvider *css = gtk_css_provider_new();
    WebKitSettings *st;
    WebKitUserScript *us;

    gtk_css_provider_load_from_data(css, statuscss, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    ucm = webkit_user_content_manager_new();
    g_signal_connect(ucm, "script-message-received::hweb", G_CALLBACK(message),
                     NULL);
    webkit_user_content_manager_register_script_message_handler_in_world(
        ucm, "hweb", "hweb");
    webkit_user_content_manager_register_script_message_handler(ucm, "hweb");
    us = webkit_user_script_new_for_world(
        corejs, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, "hweb", NULL, NULL);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);
    us = webkit_user_script_new_for_world(
        autojs, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, "hweb", NULL, NULL);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);

    view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context(),
                        "user-content-manager", ucm, NULL);
    st = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));
    webkit_settings_set_enable_developer_extras(st, TRUE);
    webkit_settings_set_enable_smooth_scrolling(st, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(st, consolelog);
    webkit_settings_set_javascript_can_access_clipboard(st, TRUE);
    /* sites open OAuth popups outside the click's gesture stack (e.g.
     * Google Identity Services after a cross-origin iframe postMessage);
     * without this WebKit drops those without ever emitting ::create */
    webkit_settings_set_javascript_can_open_windows_automatically(st, TRUE);
    webkit_settings_set_enable_media_stream(st, TRUE);
    webkit_settings_set_enable_webrtc(st, TRUE); /* needs a WebRTC build */

    g_signal_connect(view, "user-message-received", G_CALLBACK(usermessage),
                     NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(loadchanged), NULL);
    g_signal_connect(view, "permission-request", G_CALLBACK(permission), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(loadfailed), NULL);
    g_signal_connect(view, "notify::estimated-load-progress",
                     G_CALLBACK(progress), NULL);
    g_signal_connect(view, "notify::title", G_CALLBACK(titlechanged), NULL);
    g_signal_connect(view, "notify::uri", G_CALLBACK(urichanged), NULL);
    g_signal_connect(view, "mouse-target-changed", G_CALLBACK(hover), NULL);
    g_signal_connect(view, "create", G_CALLBACK(create), NULL);
    g_signal_connect(view, "context-menu", G_CALLBACK(ctxmenu), NULL);
    g_signal_connect(view, "decide-policy", G_CALLBACK(policy), NULL);
    g_signal_connect(webkit_web_view_get_inspector(WEBKIT_WEB_VIEW(view)),
                     "open-window", G_CALLBACK(inspectopen), NULL);
    g_signal_connect(webkit_web_view_get_inspector(WEBKIT_WEB_VIEW(view)),
                     "closed", G_CALLBACK(inspectclosed), NULL);

    modelbl = gtk_label_new("");
    urllbl = gtk_label_new("");
    gtk_widget_set_name(modelbl, "mode");
    gtk_label_set_ellipsize(GTK_LABEL(urllbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(urllbl), 1.0);
    gtk_label_set_xalign(GTK_LABEL(modelbl), 0.0);
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_name(hbox, "status");
    gtk_box_pack_start(GTK_BOX(hbox), modelbl, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), urllbl, TRUE, TRUE, 0);
    status = hbox;

    complbl = gtk_label_new(NULL);
    gtk_widget_set_name(complbl, "comp");
    gtk_label_set_xalign(GTK_LABEL(complbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(complbl), PANGO_ELLIPSIZE_END);

    entry = gtk_entry_new();
    gtk_widget_set_name(entry, "cmd");
    gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
    g_signal_connect(entry, "activate", G_CALLBACK(entryactivate), NULL);
    g_signal_connect(entry, "changed", G_CALLBACK(entrychanged), NULL);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), view, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), complbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 1024, 768);
    gtk_window_set_role(GTK_WINDOW(win), "browser");
    gtk_container_add(GTK_CONTAINER(win), box);
    g_signal_connect(win, "key-press-event", G_CALLBACK(keypress), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(activechanged), NULL);
    lastactive = g_get_real_time();
    gtk_widget_show_all(win);
    gtk_widget_hide(entry);
    gtk_widget_hide(complbl);
    gtk_widget_grab_focus(view);
    updatemode();
    updatetitle();
    loadscripts();
}

int main(int argc, char *argv[]) {
    char *u;
    ssize_t n = readlink("/proc/self/exe", exedir, sizeof exedir - 1);
    if (n < 0)
        die("cannot resolve /proc/self/exe");
    exedir[n] = 0;
    *strrchr(exedir, '/') = 0;

    if (argc > 1 && !strcmp(argv[1], "-v")) {
        puts("hweb " HWEB_VERSION);
        return 0;
    }
    /* the env carries privateness and the profile to t/T/F-spawned
     * windows, so windows opened from a private window are private too and
     * stay in the same profile; -p/-P set them for both this process and,
     * via setenv, its children */
    private = getenv("HWEB_PRIVATE") != NULL;
    if (getenv("HWEB_PROFILE"))
        snprintf(profile, sizeof profile, "%s", getenv("HWEB_PROFILE"));
    while (argc > 1 && argv[1][0] == '-' && argv[1][1] && !argv[1][2]) {
        if (argv[1][1] == 'p') {
            private = 1;
            setenv("HWEB_PRIVATE", "1", 1);
        } else if (argv[1][1] == 'P' && argc > 2) {
            char *p = expandhome(argv[2]),
                 *abs = g_canonicalize_filename(p, NULL);
            snprintf(profile, sizeof profile, "%s", abs);
            setenv("HWEB_PROFILE", profile, 1);
            g_free(p), g_free(abs);
            argv++, argc--;
        } else
            break;
        argv++, argc--;
    }
    g_set_prgname("hweb");
    gtk_init(&argc, &argv);
    /* WM_CLASS: gtk_init capitalises the program name unless --class=NAME
     * was given; keep it lowercase like the binary */
    if (!strcmp(gdk_get_program_class(), "Hweb"))
        gdk_set_program_class("hweb");
    setup();

    u = tourl(argc > 1 ? argv[1] : "");
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), u);
    g_free(u);

    if (!isatty(0)) {
        GIOChannel *ch = g_io_channel_unix_new(0);
        g_io_add_watch(ch, G_IO_IN | G_IO_HUP, stdinline, NULL);
    }
    listensock();
    g_unix_signal_add(SIGTERM, quitsig, NULL);
    g_unix_signal_add(SIGINT, quitsig, NULL);
    gtk_main();
    if (sockpath[0])
        unlink(sockpath);
    return 0;
}
