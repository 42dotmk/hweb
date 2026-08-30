/* hweb configuration. Compiled in: edit and `make`. */

/* what servers see: user agent + extra request headers. The headers are
 * applied to every HTTP request by the web-process extension
 * (hweb-ext.c); an empty value removes the header instead. Keep the
 * Chrome version in sync across useragent, headers and spoofjs. */
static const char *useragent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36";
static const struct header {
    const char *name, *value;
} headers[] = {
    {"Sec-CH-UA", "\"Google Chrome\";v=\"151\", \"Chromium\";v=\"151\", "
                  "\"Not_A Brand\";v=\"24\""},
    {"Sec-CH-UA-Mobile", "?0"},
    {"Sec-CH-UA-Platform", "\"macOS\""},
    {"Accept-Language", "en-US,en;q=0.9"},
};
static const char *languages[] = {"en-US", "en",
                                  NULL}; /* navigator.languages */

/* runs in every page before its own scripts (main world): make the JS side
 * agree with the user agent. */
static const char *spoofjs =
    "(function(){var d=(o,k,v)=>Object.defineProperty(o,k,{get:()=>v,"
    "configurable:true});"
    "d(Navigator.prototype,'platform','MacIntel');"
    "d(Navigator.prototype,'vendor','Google Inc.');"
    "var b=[{brand:'Google Chrome',version:'151'},{brand:'Chromium',"
    "version:'151'},{brand:'Not_A Brand',version:'24'}];"
    "d(Navigator.prototype,'userAgentData',{brands:b,mobile:false,"
    "platform:'macOS',getHighEntropyValues:()=>Promise.resolve({brands:b,"
    "mobile:false,platform:'macOS',platformVersion:'15.0.0',architecture:"
    "'x86',bitness:'64',model:'',uaFullVersion:'151.0.0.0'}),"
    "toJSON(){return{brands:b,mobile:false,platform:'macOS'}}});"
    "window.chrome={runtime:{},app:{},loadTimes:function(){},csi:function(){}};"
    "})();";

/* window title, so windows are searchable by page title and url in
 * hmenu/hws: %s = page title, second %s = url */
static const char *titlefmt = "%s \xe2\x80\x94 %s";

static const char *homepage = "https://duckduckgo.com";
/* `open` argument that is not a url/file is searched here (%s = query) */
static const char *searchurl = "https://duckduckgo.com/?q=%s";
/* every *.js in this directory is injected into every page (document end,
 * main world); "inject FILE" adds more at runtime */
static const char *scriptdir = "~/.config/hweb/scripts";
static const char *downloaddir = "~/Downloads";
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
 * typed character, or <C-x>/<M-x> for ctrl/alt chords, or <Name> for
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
    {"f", "hint open"},
    {"F", "hint new"},
    {"gf", "hint yank"},
    {"gi", "js (document.querySelector('input:not([type=hidden]),"
           "textarea')||{focus(){}}).focus()"},
    {"/", "prompt /"},
    {"n", "findnext"},
    {"N", "findprev"},
    {"i", "insert"},
    {":", "prompt :"},
    {"yy", "yank"},
    {"p", "open %c"},
    {"P", "tab %c"},
    {"+", "zoom +"},
    {"-", "zoom -"},
    {"=", "zoom 1"},
    {"gd", "inspect"},
    {"ZZ", "quit"},
    {"q", "quit"},
};
