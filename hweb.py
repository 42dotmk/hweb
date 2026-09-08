"""hweb.py - drive hweb windows from Python.

A thin client for hweb's control socket ($XDG_RUNTIME_DIR/hweb/<pid>.sock,
what hwebc speaks): one command line in, one JSON reply out. Method names
follow chrome-dumper's DumperClient where the commands coincide.

    import hweb
    w = hweb.Window()                 # the focused window (or the only one)
    w.nav("https://example.com", wait=True)
    w.type("hello", placeholder="Search", submit=True, wait=True)
    print(w.html()[:200])
    w.mouse_click(400, 300)["target"]
    for win in hweb.windows(): print(win.info()["title"])

Every method returns the reply dict; an error reply raises hweb.Error.
Standard library only.
"""

import json
import os
import shutil
import socket
import subprocess
import time

__all__ = ["Window", "windows", "Error", "Unreachable", "quote"]

# verbs whose argument is the rest of the line, unquoted (as in hweb.c)
RAW = {"open", "tab", "private", "js", "inject", "yank", "download", "prompt",
       "echo", "find"}


class Error(Exception):
    """hweb replied {"type": "error", ...}; .error is its message."""

    def __init__(self, reply):
        super().__init__(reply.get("error", "error"))
        self.reply = reply
        self.error = reply.get("error")


class Unreachable(Exception):
    """no such window, or it did not answer in time"""


def sockdir():
    """the socket directory, sweeping sockets of dead windows"""
    rt = os.environ.get("XDG_RUNTIME_DIR")
    d = os.path.join(rt, "hweb") if rt else "/tmp/hweb-%d" % os.getuid()
    try:
        names = os.listdir(d)
    except OSError:
        return d
    for n in names:
        pid = _pidof(n)
        if pid and not _alive(pid):
            try:
                os.unlink(os.path.join(d, n))
            except OSError:
                pass
    return d


def _pidof(name):
    if name.endswith(".sock") and name[:-5].isdigit():
        return int(name[:-5])
    return 0


def _alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _pids():
    d = sockdir()
    try:
        return [p for p in map(_pidof, os.listdir(d)) if p]
    except OSError:
        return []


def quote(s):
    """a token hweb's tokenizer (args.h) turns back into s"""
    s = str(s)
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def build(verb, *args, **flags):
    """verb, positionals and --flags as one command line. A flag that is
    True is boolean, None/False is omitted, anything else is quoted as its
    value; underscores in names become dashes (no_clear -> --no-clear).
    The trailing underscore of Python keywords is dropped (from_)."""
    parts = [verb]
    raw = verb in RAW
    for a in args:
        parts.append(str(a) if raw else quote(a))
    for name, v in flags.items():
        if v is None or v is False:
            continue
        name = "--" + name.rstrip("_").replace("_", "-")
        if v is True:
            parts.append(name)
        elif isinstance(v, (list, tuple)):
            parts.append(name)
            parts.extend(quote(x) for x in v)
        else:
            parts.append(name)
            parts.append(quote(v))
    return " ".join(parts)


def query(pid, line, timeout=60.0):
    """one round trip with window pid; the parsed reply"""
    path = os.path.join(sockdir(), "%d.sock" % pid)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path)
        s.sendall(line.encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
    except (OSError, socket.timeout) as e:
        raise Unreachable("window %d: %s" % (pid, e))
    finally:
        s.close()
    if not buf:
        raise Unreachable("window %d did not reply" % pid)
    return json.loads(buf)


def windows(timeout=1.0):
    """every window that answers, as Window objects"""
    out = []
    for pid in _pids():
        try:
            info = query(pid, "info", timeout)
        except (Unreachable, ValueError):
            continue
        w = Window(pid)
        w._info = info
        out.append(w)
    return out


def pick():
    """the focused window, else the most recently focused, else the only
    one; None when there is none (hwebc's rule)"""
    ws = windows(0.3)
    for w in ws:
        if w._info.get("focused"):
            return w
    return max(ws, key=lambda w: w._info.get("active", 0), default=None)


class Window:
    """one hweb window (process). Window() picks the current one,
    Window(pid) a specific one, Window.open(url) starts a new hweb."""

    def __init__(self, pid=None, timeout=60.0):
        if pid is None:
            pid = os.environ.get("HWEB_WIN")
            pid = int(pid) if pid else None
        if pid is None:
            w = pick()
            if not w:
                raise Unreachable("no hweb window")
            pid = w.pid
        self.pid = pid
        self.timeout = timeout
        self._info = None

    def __repr__(self):
        return "Window(%d)" % self.pid

    @classmethod
    def open(cls, url="", private=False, profile=None, timeout=60.0,
             wait=10.0):
        """start a new hweb process showing url and return its Window;
        profile is a data directory (hweb -P DIR), None the default one"""
        before = set(_pids())
        argv = (["hweb"] + (["-p"] if private else []) +
                (["-P", profile] if profile else []) + ([url] if url else []))
        if not shutil.which("hweb"):
            raise Unreachable("hweb not in PATH")
        subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
        return cls._await(before, timeout, wait)

    @classmethod
    def _await(cls, before, timeout, wait):
        deadline = time.time() + wait
        while time.time() < deadline:
            new = set(_pids()) - before
            for pid in new:
                try:
                    query(pid, "info", 1.0)
                    return cls(pid, timeout)
                except Unreachable:
                    pass
            time.sleep(0.05)
        raise Unreachable("new hweb window did not appear")

    # transport

    def send(self, line, timeout=None):
        """run one raw command line; the reply, or Error"""
        r = query(self.pid, line, self.timeout if timeout is None else timeout)
        if r.get("type") == "error":
            raise Error(r)
        return r

    def cmd(self, verb, *args, **flags):
        """run verb with positionals and --flags, see build()"""
        return self.send(build(verb, *args, **flags))

    # window

    def info(self):
        return self.cmd("info")

    def nav(self, url, wait=True):
        return self.cmd("nav", url, wait=wait)

    open_ = nav

    def back(self, steps=1, wait=True):
        return self.cmd("back", steps=steps, wait=wait)

    def forward(self, steps=1, wait=True):
        return self.cmd("forward", steps=steps, wait=wait)

    def reload(self, bypass_cache=False, wait=True):
        return self.cmd("reload!" if bypass_cache else "reload", wait=wait)

    def close(self):
        return self.cmd("close")

    def tab(self, url="", private=False):
        """a new window (process) showing url; returns its Window"""
        before = set(_pids())
        self.send(build("private" if private else "tab", url))
        return Window._await(before, self.timeout, 10.0)

    def resize(self, width, height):
        return self.cmd("resize", "%dx%d" % (width, height))

    def zoom(self, percent=None, in_=None, out=None, reset=False):
        """zoom(150) sets 150%; in_/out step (True = 10 pts, or a number);
        reset -> 100%; no argument reports"""
        args = [] if percent is None else [percent]
        flags = {"reset": reset}
        if in_ is not None:
            flags["in"] = True
            if in_ is not True:
                args = [in_]
        if out is not None:
            flags["out"] = True
            if out is not True:
                args = [out]
        return self.cmd("zoom", *args, **flags)

    def js(self, code):
        """evaluate in the page: an expression, or a body with return
        (a returned promise is awaited); the value"""
        return self.send("js " + code)["value"]

    # page (auto.js)

    def click(self, selector=None, text=None, nth=0, wait=False):
        return self.cmd("click", selector=selector, text=text, nth=nth or None, wait=wait)

    def focus(self, selector=None, text=None, nth=0):
        return self.cmd("focus", selector=selector, text=text, nth=nth or None)

    def type(self, value, selector=None, placeholder=None, label=None, nth=0,
             clear=True, submit=False, wait=False):
        return self.cmd("type", value, selector=selector, placeholder=placeholder,
                        label=label, nth=nth or None, no_clear=not clear,
                        submit=submit, wait=wait)

    def select(self, selector=None, text=None, from_=None, to=None, rect=None,
               mouse=True, scroll=True, focus=True):
        """rect is (x1, y1, x2, y2)"""
        return self.cmd("select", selector=selector, text=text, from_=from_, to=to,
                        rect=_csv(rect), no_mouse=not mouse, no_scroll=not scroll,
                        no_focus=not focus)

    def select_clear(self):
        return self.cmd("select-clear")

    def scroll(self, direction="down", pages=None, pixels=None, to=None, smooth=True):
        return self.cmd("scroll", direction, pages=pages, pixels=pixels, to=to,
                        no_smooth=not smooth)

    def highlight(self, selector=None, text=None, rect=None, all=False, nth=0,
                  color=None, label=None, duration=0, scroll=True):
        """rect is (x, y, width, height); duration in ms, 0 = persistent"""
        return self.cmd("highlight", selector=selector, text=text, rect=_csv(rect),
                        all=all, nth=nth or None, color=color, label=label,
                        duration=duration or None, no_scroll=not scroll)

    def clear_highlights(self):
        return self.cmd("clear-highlights")

    def dump(self, out=None, stdout=False):
        """write the live DOM to a file (dumpdir or out); with stdout the
        reply also carries "html\""""
        return self.cmd("dump", out=_abs(out), stdout=stdout)

    def html(self):
        """the live DOM as a string"""
        return self.dump(stdout=True)["html"]

    def screenshot(self, out=None, format="png", quality=85, rect=None,
                   selector=None, text=None, full=False):
        """the viewport (or the whole document with full) to a file;
        the reply's "path" says where. rect is (x, y, width, height)."""
        return self.cmd("screenshot", out=_abs(out), format=format, quality=quality,
                        rect=_csv(rect), selector=selector, text=text, full=full)

    def metrics(self):
        return self.cmd("metrics")

    def active(self):
        return self.cmd("active")["focused"]

    # real input

    def mouse_move(self, x, y, instant=False, duration=None, **mods):
        return self.cmd("mouse-move", x, y, instant=instant, duration=duration,
                        **_mods(mods))

    def mouse_nudge(self, direction, pixels=None, less=False, more=False, **mods):
        """direction up|down|left|right, relative to the pointer"""
        args = [] if pixels is None else [pixels]
        return self.cmd("mouse-" + direction, *args, less=less, more=more, **_mods(mods))

    def mouse_click(self, x, y, button="left", count=1, instant=False, duration=None,
                    wait=False, **mods):
        return self.cmd("mouse-click", x, y, button=button,
                        count=count if count != 1 else None, instant=instant,
                        duration=duration, wait=wait, **_mods(mods))

    def mouse_press(self, x=None, y=None, button="left", **mods):
        args = [] if x is None else [x, y]
        return self.cmd("mouse-press", *args, button=button, **_mods(mods))

    def mouse_release(self, x=None, y=None, button="left", **mods):
        args = [] if x is None else [x, y]
        return self.cmd("mouse-release", *args, button=button, **_mods(mods))

    def mouse_drag(self, x1, y1, x2, y2, steps=10, button="left", **mods):
        return self.cmd("mouse-drag", x1, y1, x2, y2, steps=steps, button=button,
                        **_mods(mods))

    def mouse_scroll(self, direction="down", pixels=None, at=None, **mods):
        """a real wheel event at the pointer (or at=(x, y))"""
        args = [direction] + ([] if pixels is None else [pixels])
        return self.cmd("mouse-scroll", *args, at=at, **_mods(mods))

    def mouse_hide(self):
        return self.cmd("mouse-hide")

    def key(self, name, selector=None, wait=False, **mods):
        """press a key: DOM names (Enter, Tab, ArrowDown, a...); with
        selector the element is focused first"""
        return self.cmd("key", name, selector=selector, wait=wait, **_mods(mods))

    def enter(self, selector=None, wait=False):
        return self.key("Enter", selector, wait)

    def tab_key(self, shift=False):
        return self.key("Tab", shift=shift)


def _csv(v):
    return None if v is None else ",".join(str(x) for x in v)


def _abs(p):
    return None if p is None else os.path.abspath(p)


def _mods(mods):
    """shift/ctrl/alt/meta keyword arguments, nothing else"""
    bad = set(mods) - {"shift", "ctrl", "alt", "meta"}
    if bad:
        raise TypeError("unknown arguments: %s" % ", ".join(sorted(bad)))
    return mods
