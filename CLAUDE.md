# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

hweb is a vim-like browser: one WebKitGTK web view per process in a plain
GTK3 window (the WM tiles windows; there are no tabs — hwm's columns and
hws's overview are the tabs), a one-line status bar, and a `:`/`/` entry.
Two source files:

- `hweb.c` — the UI process: modes, keymap, command language, status
  bar, events on stdout, commands from stdin.
- `history.c` — the visit log: `$XDG_DATA_HOME/hweb/history`, a flat
  append-only file of `url<TAB>title` lines (one per finished load, written
  once the title arrives), and `histmatch()`, which runs `histfilter` from
  `config.h` (`tac FILE | fzf --filter QUERY`) over it and drops repeated
  urls, newest first.
- `hweb-ext.c` — a WebKit web-process extension (`hweb-ext.so`) that
  rewrites request headers. WebKit only lets headers be changed from
  inside the web process, so this is the only way to make every request
  carry the Chrome/macOS `Sec-CH-UA*` / `Accept-Language` headers from
  `config.h`. hweb finds the `.so` next to its own binary (`/proc/self/exe`
  resolved through the `~/.local/bin` symlink), and hands it the header
  table as initialization user data, so only `hweb.c` includes `config.h`.

Suckless-style: everything is configured in `config.h` and compiled in.

## Build

```sh
make            # ./hweb + ./hweb-ext.so (needs libwebkit2gtk41-devel; fzf at runtime)
make install    # symlinks hweb into ~/.local/bin (the .so stays here), installs
                # hweb.desktop and makes hweb the xdg default browser
make clean
```

No tests; `-std=c11 -pedantic -Wall -Wextra` on the whole build is the
lint — keep it warning-free (WebKit/GTK headers are included with
`-isystem` so their own noise does not count). `-Wno-deprecated-declarations`
is on because GTK3 deprecates half of itself.

Verify under Xephyr, never on the live display unless you mean to open a
window: `Xephyr :77 -screen 1000x700 &`, then
`DISPLAY=:77 ./hweb https://httpbin.org/headers < fifo` and write commands
to the fifo (e.g. `js document.body.innerText`) to see the effective headers
in the `js` event on stdout.

## Concepts

- **Modes** — `normal` (keys run commands via `keys[]`), `insert` (keys go
  to the page; entered by `i`, or automatically when an editable element
  gains focus, left with Escape), `hint` (after `f`/`F`/`gf`; typed hint
  letters filter the labels), `prompt` (the entry: `:cmd`, `/find`).
- **Commands** — one text language used by the keymap, the `:` prompt and
  stdin. `cmd()` in `hweb.c` is the whole list: `open`, `tab`, `back`,
  `forward`, `reload`, `reload!`, `stop`, `quit`, `scroll DX DY`,
  `scrollpage F`, `scrollto N`, `zoom +|-|N`, `find`, `findnext`,
  `findprev`, `insert`, `normal`, `hint open|new|yank`, `js CODE`,
  `inject FILE`, `inspect`, `yank [URL]`, `prompt TEXT`, `echo`, `title`.
  Keymap commands expand `%u` (url), `%t` (title), `%c` (clipboard).
- **Completion** — Tab in the `:` prompt: on a (partial) verb it lists the
  commands, after `:open `/`:tab ` it lists fuzzy history matches for the
  typed text (`compmax` rows at a time, above the entry). Tab/Shift-Tab
  cycle, any other key drops the list, Enter runs what is in the entry.
- **Events** — one line per event on stdout: `mode`, `load
  started|committed|finished|failed`, `title`, `uri`, `hover`, `js RESULT`,
  `msg TEXT` (from page or injected JS calling
  `webkit.messageHandlers.hweb.postMessage(...)`), `new`, `yank`, `inject`,
  `download started|finished`. When stdin is not a tty each line read from
  it is run as a command, so `hweb URL < cmds > events` scripts the browser.
- **Injection** — `spoofjs` (page world, document start) makes
  `navigator` agree with the user agent; `corejs` (isolated world `hweb`,
  document start) is the insert-mode detector and hint machinery, reached
  from C via `js("hweb", ...)`. Every `*.js` in `scriptdir` is injected into
  every page at document end, page world; `inject FILE` adds one at
  runtime (and runs it now). `js CODE` evaluates in the page world.
- **Debugging** — `gd` / `:inspect` toggles the WebKit inspector
  (developer extras are on). `consolelog = 1` mirrors page console output
  to stdout. `WEBKIT_INSPECTOR_SERVER=127.0.0.1:9222` in the environment
  exposes the remote inspector protocol.
- **Window title** is `titlefmt` (`title — url`), so pages are found by
  either in `hmenu`'s window list and `hws`. WM_CLASS is `hweb`.
- **New windows** — `t`/`T`, `F` hints, `target=_blank`, `window.open`,
  middle-click and ctrl+click all `spawn()` another `hweb` process (from the same
  directory as the running binary).
- **State** lives in `$XDG_DATA_HOME/hweb` (cookies.sqlite, storage) and
  `$XDG_CACHE_HOME/hweb`; third-party cookies are refused. Downloads go to
  `downloaddir` when a response's mime type cannot be shown.

## Style

Suckless/OpenBSD C conventions like the siblings — C11, fixed-size
buffers with `snprintf`, GLib allocation only where WebKit hands it out —
formatted by clang-format via the repo's `.clang-format` (shared across
the siblings). Run `clang-format -i` on files you touch. Prefer adding a
command (and a key in `config.h`) over adding a flag.
