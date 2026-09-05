# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

hweb is a vim-like browser: one WebKitGTK web view per process in a plain
GTK3 window (the WM tiles windows; there are no tabs — hwm's columns and
hws's overview are the tabs), a one-line status bar, and a `:`/`/` entry.
Two source files:

- `hweb.c` — the UI process: modes, keymap, command language, status
  bar, events on stdout, commands from stdin and the control socket.
- `args.h` — the command tokenizer (shell-like quoting, `--flag` grammar)
  used by every verb.
- `auto.js` — the page-side automation library (isolated world `hweb`;
  the makefile embeds it as `auto.h`), see *Automation* below.
- `hwebc.c` — the control client: `hwebc VERB ARGS...` sends one command
  line to a running window's socket and prints the JSON reply.
- `history.c` — the visit log: `$XDG_DATA_HOME/hweb/history`, a flat
  append-only file of `url<TAB>title` lines (one per finished load, written
  once the title arrives), and `histmatch()`, which runs `histfilter` from
  `config.h` (`tac FILE | fzf --filter QUERY`) over it and drops repeated
  urls, newest first.
- `hweb-ext.c` — a WebKit web-process extension (`hweb-ext.so`) hooked
  on every page's `send-request`. It does the two things only the web
  process can: rewrite request headers (the `headers[]` from `config.h`,
  e.g. `Accept-Language`; the browser otherwise presents its real WebKit
  identity — no Chrome spoofing), and cancel requests to blocked hosts.
  Blocking is host-based, hosts-file style: `blocklist` in `config.h`
  names a file of hosts (plain or `0.0.0.0 host` lines, so a StevenBlack
  hosts file works unmodified); a request whose host or any parent
  domain is listed is cancelled (`send-request` returns TRUE) and
  reported to the UI process as a `blocked` user message — except the
  main-frame navigation itself, so blocked sites can still be opened
  deliberately. `isblocked()` is where a richer (EasyList-style) matcher
  would go. hweb finds the `.so` next to its own binary (`/proc/self/exe`
  resolved through the `~/.local/bin` symlink), and hands it the header
  table and the blocklist path as initialization user data (`(a(ss)s)`),
  so only `hweb.c` includes `config.h`.

Suckless-style: everything is configured in `config.h` and compiled in.

## Build

```sh
make            # ./hweb + ./hweb-ext.so + ./hwebc (needs libwebkit2gtk41-devel;
                # fzf at runtime); auto.js -> auto.h on the way
make install    # symlinks hweb and hwebc into ~/.local/bin (the .so stays
                # here), installs hweb.desktop and makes hweb the xdg default
make clean
```

No tests; `-std=c11 -pedantic -Wall -Wextra` on the whole build is the
lint — keep it warning-free (WebKit/GTK headers are included with
`-isystem` so their own noise does not count). `-Wno-deprecated-declarations`
is on because GTK3 deprecates half of itself.

Verify under Xephyr, never on the live display unless you mean to open a
window: `Xephyr :77 -screen 1000x700 &`, then
`DISPLAY=:77 ./hweb test/auto.html &` and drive it with `./hwebc` (point
`XDG_RUNTIME_DIR` at a short scratch path for both so the test window's
socket is kept apart from the live ones; socket paths are limited to 108
bytes). `test/auto.html` is a fixture page with two same-text buttons, a
React-style controlled input, a `<select>`, a contenteditable, an inner
scroller, a form and a paragraph; it logs what the page saw into
`window.LOG` (`hwebc js 'return LOG'`). The older way still works:
`DISPLAY=:77 ./hweb URL < fifo` with commands written to the fifo, results
as `result` events on stdout.

## Concepts

- **Modes** — `normal` (keys run commands via `keys[]`), `insert` (keys go
  to the page; entered by `i`, or automatically when an editable element
  gains focus, left with Escape), `hint` (after `f`/`F`/`gf`; typed hint
  letters filter the labels), `prompt` (the entry: `:cmd`, `/find`).
- **Commands** — one text language used by the keymap, the `:` prompt,
  stdin and the control socket. `commands[]` in `hweb.c` is the whole
  list (one handler per verb, taking the request and the parsed `Args`);
  `:` completion reads it. A line is tokenized shell-style by `args.h`
  (`"..."` keeps spaces, `--flag value`, `--flag=value`, boolean flags
  declared per verb in the table), except for the *raw* verbs whose
  argument is the rest of the line verbatim: `open`, `tab`, `private`,
  `js`, `inject`, `yank`, `download`, `prompt`, `echo`, `find`.
  Browser verbs: `open [URL|query]` (this window), `nav URL [--wait]`
  (same, tokenized), `tab`/`private [URL]` (a new window, replies its
  pid), `back`/`forward [--steps N] [--wait]`, `reload`, `reload!`,
  `stop`, `quit`/`close`, `scroll DX DY` (keymap steps) or chrome-dumper's
  `scroll [up|down] [--pages F] [--pixels N] [--to top|bottom|css]
  [--no-smooth]`, `scrollpage F`, `scrollto N`, `zoom [PCT] | + | - |
  --in [PCT] | --out [PCT] | --reset` (numbers are percent: `zoom 100`),
  `find`, `findnext`, `findprev`, `insert`, `normal`, `hint
  open|new|yank|download`, `js CODE` (an expression, or a function body
  when it contains `return`; a returned promise is awaited), `inject
  FILE`, `inspect`, `yank [URL]`, `download [URL]` (to `downloaddir`; the
  page itself without URL), `prompt TEXT`, `echo`, `title`, `info`,
  `resize WxH`, `blockupdate` (runs the `blockupdate` shell snippet from
  `config.h`, which refetches the blocklist, and shows its output). The
  automation verbs are listed under *Automation*.
  Keymap commands expand `%u` (url), `%t` (title), `%c` (clipboard).
- **Requests and replies** — every command answers exactly once with one
  JSON object, `{"type":"clicked",...}` or `{"type":"error","error":...}`
  (`Req` in `hweb.c`: a small pool; async work — js, snapshots, a
  navigation wait — completes it later). Where the reply goes depends on
  who asked: a socket client gets it as its one reply line; a command
  read from stdin gets it as a `result JSON` event; keymap and `:` prompt
  commands get nothing (errors show in the status bar). `--wait` on
  nav/back/forward/reload/click/type/key/mouse-click holds the reply for
  the navigation the action starts: a load must begin within 300 ms
  (else `"loaded":false`) and finish within 30 s; the reply then carries
  `loaded`, `url` and, on failure, `failed`.
- **Completion** — Tab in the `:` prompt: on a (partial) verb it lists the
  commands, after `:open `/`:tab ` it lists fuzzy history matches for the
  typed text (`compmax` rows at a time, above the entry). Tab/Shift-Tab
  cycle, any other key drops the list, Enter runs what is in the entry.
- **Events** — one line per event on stdout: `mode`, `load
  started|committed|finished|failed`, `title`, `uri`, `hover`, `js RESULT`,
  `msg TEXT` (from page or injected JS calling
  `webkit.messageHandlers.hweb.postMessage(...)`), `new`, `popup`, `yank`, `inject`,
  `download started|finished`, `permission media [audio] [video] [display]`
  (a granted getUserMedia request), `blocked URL` (a request cancelled by the
  blocklist), `blockupdate OUTPUT`, `result JSON` (the reply to a command
  read from stdin). Newlines and backslashes in a payload are escaped
  (`\n`, `\\`) so an event is always one line; unescape after splitting.
  When stdin is not a tty each line read from it is run as a command, so
  `hweb URL < cmds > events` scripts the browser.
- **Control socket** — every window listens on
  `$XDG_RUNTIME_DIR/hweb/<pid>.sock` (fallback `/tmp/hweb-<uid>/`): one
  request line per connection, one JSON reply line back, then close.
  Sockets of dead pids are swept by whoever looks at the directory. `hwebc`
  is the client: `hwebc [--win PID] [--timeout S] VERB ARGS...` (the shell
  words are re-quoted for `args.h`, raw verbs are joined verbatim; a
  relative `--out` for screenshot/dump is made absolute), `hwebc windows`
  (every window's `info`), `hwebc < script` (one command per line). Without
  `--win`/`$HWEB_WIN` it targets the focused window, else the most recently
  focused (`info` reports `focused` and `active`), else the only one.
  `hwebc open URL` with no window running starts hweb. Exit status 0 ok,
  1 error reply, 2 usage/no window, 3 unreachable.
- **Automation** — chrome-dumper's command surface, the same grammar and
  reply shapes, so its scripts and agent workflow port with `uv run
  dumper` → `hwebc` (no tabs: a window is a process, `tabId` is gone,
  `open` keeps hweb's meaning and `tab URL` is the new-window verb; `tab`
  as a key is `key Tab`). Two halves:
  - `auto.js` (isolated world `hweb`, `__hweb.auto.run(verb, opts)`,
    called through `webkit_web_view_call_async_javascript_function` with
    the parsed command line as JSON: positionals in `_`, flags by name)
    does the DOM work: `click [--selector css | --text s] [--nth N]
    [--wait]` (text matches skip invisible elements; `el.click()` after
    focusing), `focus`, `type VALUE [--selector | --placeholder | --label]
    [--nth N] [--no-clear] [--submit] [--wait]` (`input` is an alias;
    `<select>` picks an option by value or text; contenteditable via
    `execCommand insertText`; `--submit` submits the form or, without one,
    presses a real Return), `select --selector | --text | --from --to |
    --rect x1,y1,x2,y2 [--no-mouse --no-scroll --no-focus]`,
    `select-clear`, `scroll` (see above; finds the real scroll container
    and waits for it to settle), `highlight --selector | --text | --rect
    x,y,w,h [--all] [--nth N] [--color #hex] [--label s] [--duration MS]
    [--no-scroll]` (boxes follow their element through scrolling),
    `clear-highlights`, `dump [--out PATH] [--stdout]` (live DOM without
    our overlays to `dumpdir`), `metrics` (viewport, scroll, dpr),
    `active` (the focused element).
  - Real input: `mouse-move X Y`, `mouse-up|down|left|right [N] [--less |
    --more]` (nudges), `mouse-click X Y [--button left|right|middle]
    [--count N | --double] [--wait]`, `mouse-press`/`mouse-release [X Y]`,
    `mouse-drag X1 Y1 X2 Y2 [--steps N]`, `mouse-scroll [up|down|left|right]
    [N] [--less | --more] [--at X Y]`, `mouse-hide`, `key NAME [--selector
    css] [--wait]`, `enter`, `space`, all with `--shift --ctrl --alt
    --meta`, and `mouse-move`/`mouse-click` with `--instant` or `--duration
    MS`. These build GdkEvents and hand them to the web view with
    `gtk_widget_event()`, the path real input takes, so the page sees
    trusted events, `:hover`, native scrolling, Tab focus traversal and
    Return submitting forms. Coordinates are CSS px (widget px = CSS px ×
    zoom); the pointer is virtual — the X pointer never moves — so
    `auto.js` draws a cursor for it, and the moves glide along an eased
    curve like a hand would. Mouse replies carry `target`, the element
    under the point, so a caller can check what it hit. A synthetic right
    click reaches the page but WebKit's own context menu is suppressed.
    WebKit scrolls 11% of the view height (at least 40 px) per smooth
    wheel unit; `mouse-scroll N` converts px accordingly and reports the
    resulting `scrollX`/`scrollY`.
  - `screenshot [--format png|jpeg] [--quality N] [--rect x,y,w,h |
    --selector css | --text s] [--full] [--out PATH]` snapshots the
    viewport, or the whole document with `--full` (which Chrome cannot),
    crops in device px, and writes `dumpdir/<pid>_<title>.png` (or `--out`);
    the reply has the path, never image data.
  The agent loop chrome-dumper documents works unchanged: `screenshot`,
  read the image, estimate the target as fractions of the picture, scale
  by `metrics`' `innerWidth`/`innerHeight`, `mouse-click X Y`, check
  `target` in the reply, `dump` to read the result.
- **Injection** — `corejs` (isolated world `hweb`, document start) is the
  insert-mode detector and hint machinery, reached from C via
  `js("hweb", ...)`; `auto.js` follows it into the same world and reuses
  its `H.vis`/`H.ed`. The isolated world shares the page's DOM but not its
  JS globals, which is why a plain `el.value =` there already bypasses a
  React value tracker. Every `*.js` in `scriptdir` is injected into
  every page at document end, page world; `inject FILE` adds one at
  runtime (and runs it now). `js CODE` evaluates in the page world.
- **Debugging** — `gd` / `:inspect` toggles the WebKit inspector
  (developer extras are on). `consolelog = 1` mirrors page console output
  to stdout. `WEBKIT_INSPECTOR_SERVER=127.0.0.1:9222` in the environment
  exposes the remote inspector protocol.
- **Window title** is `titlefmt` (`title — url`), so pages are found by
  either in `hmenu`'s window list and `hws`. WM_CLASS is `hweb`.
- **New windows** — `t`/`T`, `F` hints, and clicks that ask for a new
  window (`target=_blank`, middle-click, ctrl+click) `spawn()` another
  `hweb` process (from the same directory as the running binary). A
  *scripted* `window.open` instead gets an opener-linked popup in the
  same process (`create()` in `hweb.c`: a bare webview, no status bar or
  keymap, closes on `window.close()`), so OAuth popups can reach
  `window.opener`/`postMessage` and hand their result back.
- **State** lives in `$XDG_DATA_HOME/hweb` (cookies.sqlite, storage) and
  `$XDG_CACHE_HOME/hweb`; third-party cookies are refused. Downloads go to
  `downloaddir` when a response's mime type cannot be shown.
- **Private browsing** — `hweb -p [URL]`, `HWEB_PRIVATE=1 hweb`, the
  `private [URL]` command, or super+shift+b (`<D-B>` in `keys[]`; `<D-x>`
  is the super/cmd chord token, case kept so shift matters):
  ephemeral website data manager (cookies, storage, cache in memory
  only), no history writes (completion still reads the existing
  history), `[private]` in the status bar. `-p` also setenvs
  `HWEB_PRIVATE`, and the env is what windows spawned from a private
  window (`t`/`T`, `F` hints) inherit — so they are private too.

## Style

Suckless/OpenBSD C conventions like the siblings — C11, fixed-size
buffers with `snprintf`, GLib allocation only where WebKit hands it out —
formatted by clang-format via the repo's `.clang-format` (shared across
the siblings). Run `clang-format -i` on files you touch. Prefer adding a
command (and a key in `config.h`) over adding a flag.
