/* hweb configuration. Compiled in: edit and `make`. */

/* what servers see: user agent + extra request headers. The headers are
 * applied to every HTTP request by the web-process extension
 * (hweb-ext.c); an empty value removes the header instead. */
static const struct header {
    const char *name, *value;
} headers[] = {
    {"Accept-Language", "en-US,en;q=0.9"},
};
static const char *languages[] = {"en-US", "en",
                                  NULL}; /* navigator.languages */

/* ad/tracker blocking: every request to a host listed in this file, or to
 * a subdomain of one, is cancelled by the extension before it is sent
 * (reported as a `blocked URL` event). One host per line; the hosts-file
 * format (`0.0.0.0 host`) is accepted too, so a list like
 *   curl -o ~/.local/share/hweb/blocklist \
 *        https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts
 * works as is. Hosts without a dot (localhost...) are ignored. */
static const char *blocklist = "~/.local/share/hweb/blocklist";
/* `blockupdate` runs this shell snippet ($1 = blocklist file) and shows
 * its output; the new list is picked up by windows opened afterwards */
static const char *blockupdate =
    "curl -sSf -o \"$1\" "
    "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts "
    "&& echo \"$(grep -c . \"$1\") lines\"";

/* window title, so windows are searchable by page title and url in
 * hmenu/hws: %s = page title, second %s = url */
static const char *titlefmt = "%s \xe2\x80\x94 %s";

static const char *homepage = "https://duckduckgo.com";
/* `open` argument that is not a url/file is searched here (%s = query) */
static const char *searchurl = "https://duckduckgo.com/?q=%s";
/* every *.js in this directory is injected into every page (document end,
 * main world); "inject FILE" adds more at runtime */
static const char *scriptdir = "~/.config/hweb/scripts";
static const char *downloaddir = "~/downloads";
/* visited pages are appended to $XDG_DATA_HOME/hweb/history as
 * "url<TAB>title"; Tab after `:open `/`:tab ` completes from it through this
 * shell snippet ($1 = history file, $2 = typed query), which must print
 * matching lines best first; hweb drops repeated urls */
static const char *histfilter = "tac \"$1\" | fzf --filter \"$2\"";
static unsigned compmax = 10; /* completion rows shown at once */

static const char *hintchars = "asdfghjkl"; /* labels for f/F hints */
static double zoomstep = 0.1;
static int scrollstep = 60; /* px per j/k/h/l */
static int consolelog = 0;  /* 1: page console output to stdout */

/* status bar look (GTK CSS) */
static const char *statuscss =
    "#status, #cmd { background: #1a1b26; color: #c0caf5; "
    "font-family: 'Iosevka NFM'; font-size: 12px; padding: 2px 6px; "
    "border: none; box-shadow: none; caret-color: #7aa2f7; }"
    "#mode { color: #7aa2f7; font-weight: bold; }"
    "#comp { background: #16161e; color: #a9b1d6; font-family: 'Iosevka NFM'; "
    "font-size: 12px; padding: 2px 6px; }";

/* normal-mode keys: a sequence of key tokens -> command. Tokens are the
 * typed character, or <C-x>/<M-x>/<D-x> for ctrl/alt/super chords (super
 * keeps the keyval's case, so <D-B> means super+shift+b), or <Name> for
 * other keys using GDK key names (<Escape>, <Down>, <Page_Down>...).
 * In commands, %u = current url, %t = title, %c = clipboard text.
 * See cmd() in hweb.c for the command list. */
static const struct key {
    const char *keys, *cmd;
} keys[] = {
    {"j", "scroll 0 1"},
    {"k", "scroll 0 -1"},
    {"h", "scroll -1 0"},
    {"l", "scroll 1 0"},
    {"<Down>", "scroll 0 1"},
    {"<Up>", "scroll 0 -1"},
    {"<C-d>", "scrollpage 0.5"},
    {"<C-u>", "scrollpage -0.5"},
    {"<C-f>", "scrollpage 1"},
    {"<C-b>", "scrollpage -1"},
    {" ", "scrollpage 1"},
    {"<Page_Down>", "scrollpage 1"},
    {"<Page_Up>", "scrollpage -1"},
    {"gg", "scrollto 0"},
    {"G", "scrollto -1"},
    {"H", "back"},
    {"L", "forward"},
    {"r", "reload"},
    {"R", "reload!"},
    {"<Escape>", "stop"},
    {"o", "prompt :open "},
    {"O", "prompt :open %u"},
    {"t", "prompt :tab "},
    {"T", "tab %u"},
    {"<D-B>", "private"},
    {"f", "hint open"},
    {"F", "hint new"},
    {"gf", "hint yank"},
    {"gD", "hint download"},
    {"gi", "js (document.querySelector('input:not([type=hidden]),"
           "textarea')||{focus(){}}).focus()"},
    {"/", "prompt /"},
    {"n", "findnext"},
    {"N", "findprev"},
    {"i", "insert"},
    {":", "prompt :"},
    {"yy", "yank"},
    {"<C-s>", "download %u"},
    {"p", "open %c"},
    {"P", "tab %c"},
    {"+", "zoom +"},
    {"-", "zoom -"},
    {"=", "zoom 1"},
    {"gd", "inspect"},
    {"ZZ", "quit"},
    {"q", "quit"},
};
