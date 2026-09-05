/* auto.js - hweb's page-side automation library (chrome-dumper's command
 * surface). Injected into the isolated world "hweb" at document start,
 * after corejs, so it shares the page's DOM but not its JS globals.
 *
 * C calls __hweb.auto.run(verb, opts) and forwards the resolved object as
 * the command's JSON reply. opts is hweb's parsed command line:
 * positionals in opts._ and --flags by name (booleans true), e.g.
 * `type hi --selector #q --submit` -> {_:["hi"], selector:"#q", submit:true}.
 * Every result is {type:...} or {type:"error", error:...}; run() never
 * rejects. Coordinates are CSS px relative to the viewport. */
(function () {
    var H = window.__hweb, A = H.auto = {};

    /* helpers */
    var norm = function (s) { return (s || "").trim().toLowerCase(); };
    var txt = function (e) {
        return (e.innerText || e.value || e.placeholder ||
                e.getAttribute("aria-label") || "").trim();
    };
    var num = function (v, d) { var n = parseFloat(v); return isNaN(n) ? d : n; };
    var err = function (e) { return {type: "error", error: e}; };
    var info = function (el) {
        return {
            tag: el.tagName.toLowerCase(), id: el.id || null,
            name: el.name || null, type: el.type || null,
            href: el.href || null,
            text: (el.innerText || el.value || "").trim().slice(0, 200)
        };
    };
    var pick = function (o, keys) {
        var r = {};
        keys.forEach(function (k) { r[k] = o[k]; });
        return r;
    };
    var rect = function (r) { return {x: r.x, y: r.y, width: r.width, height: r.height}; };
    var center = function (el) { el.scrollIntoView({block: "center", inline: "center"}); };
    var sleep = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
    var SEL = {
        click: "a, button, [role=button], input[type=submit], input[type=button]",
        focus: "a, button, [role=button], input, textarea, select, [contenteditable]",
        block: "a, button, [role=button], h1, h2, h3, h4, span, div, p, li"
    };

    /* element by --selector/--text/--nth; text matches are visible ones
     * only when `visible` (chrome-dumper would click a hidden duplicate) */
    var find = function (o, sel, visible) {
        var els, needle = norm(o.text), nth = num(o.nth, 0);
        if (o.selector) {
            els = Array.from(document.querySelectorAll(o.selector));
        } else if (o.text) {
            els = Array.from(document.querySelectorAll(sel)).filter(function (e) {
                return norm(txt(e)).indexOf(needle) >= 0 && (!visible || H.vis(e));
            });
        } else {
            return null;
        }
        return els[nth] || null;
    };

    var focusable = function (e) {
        var tag;
        if (!e || e === document.body || e === document.documentElement) return false;
        tag = e.tagName.toLowerCase();
        if (["input", "select", "textarea", "button"].indexOf(tag) >= 0) return true;
        if (tag === "a" && e.getAttribute("href")) return true;
        if (e.getAttribute("tabindex") !== null) return true;
        return !!e.isContentEditable;
    };
    /* the element itself, its first focusable ancestor, or the element
     * made focusable with tabindex=0 */
    var focusTarget = function (el) {
        var t = el, p;
        if (focusable(t)) return {el: t, made: false};
        for (p = el.parentElement; p; p = p.parentElement)
            if (focusable(p)) return {el: p, made: false};
        t.setAttribute("tabindex", "0");
        return {el: t, made: true};
    };

    /* what a click on el would do: follow a link, submit a form, or nothing */
    var navKind = function (el) {
        var a = el.closest("a[href]");
        if (a && !/^(#|javascript:)/.test(a.getAttribute("href")) && a.target !== "_blank")
            return "link";
        if (el.form && (el.type === "submit" || el.type === "image" ||
                        (el.tagName === "BUTTON" && el.type !== "button")))
            return "submit";
        return null;
    };

    var findScroller = function () {
        var root = document.scrollingElement || document.documentElement, best = null;
        if (root && root.scrollHeight > root.clientHeight + 1) return root;
        var walk = function (el) {
            var cs, oy, i;
            if (!el || el.nodeType !== 1) return;
            cs = getComputedStyle(el);
            oy = cs.overflowY;
            if ((oy === "auto" || oy === "scroll" || oy === "overlay") &&
                el.scrollHeight > el.clientHeight + 1 && el.clientHeight > 200)
                if (!best || el.clientHeight > best.clientHeight) best = el;
            for (i = 0; i < el.children.length; i++) walk(el.children[i]);
        };
        walk(document.body);
        return best || root;
    };
    var isRoot = function (s) {
        return s === document.scrollingElement || s === document.documentElement ||
               s === document.body;
    };

    /* wait until read() stops changing (a smooth scroll needs a moment to
     * even start), bounded */
    var settle = async function (read, smooth) {
        var start = performance.now(), first = read(), prev = first, stable = 0, cur;
        var minWait = smooth ? 250 : 0, maxWait = smooth ? 2000 : 200;
        while (performance.now() - start < maxWait) {
            await sleep(50);
            cur = read();
            if (cur !== prev) { stable = 0; prev = cur; }
            else if (++stable >= 2 && performance.now() - start >= minWait && cur !== first) break;
        }
    };

    /* overlays we draw; hidden while dumping */
    var HL = "__hweb_hl", CUR = "__hweb_cursor";
    A.hl = []; /* {el|null, doc:{x,y,w,h}|null, box} */
    var relayoutPending = false;
    var relayout = function () {
        if (relayoutPending) return;
        relayoutPending = true;
        requestAnimationFrame(function () {
            relayoutPending = false;
            A.hl.forEach(function (h) {
                var r = h.el ? h.el.getBoundingClientRect()
                             : {x: h.doc.x - scrollX, y: h.doc.y - scrollY, width: h.doc.w, height: h.doc.h};
                h.box.style.left = r.x + "px";
                h.box.style.top = r.y + "px";
                h.box.style.width = r.width + "px";
                h.box.style.height = r.height + "px";
            });
        });
    };
    var hlHost = function () {
        var host = document.getElementById(HL);
        if (!host) {
            host = document.createElement("div");
            host.id = HL;
            host.style.cssText = "position:fixed;left:0;top:0;width:0;height:0;" +
                "pointer-events:none;z-index:2147483647";
            document.documentElement.appendChild(host);
            addEventListener("scroll", relayout, {capture: true, passive: true});
            addEventListener("resize", relayout, {passive: true});
        }
        return host;
    };

    var cmds = {};

    cmds.click = function (o) {
        var el = find(o, SEL.click, true), r;
        if (!el && !o.selector && !o.text && document.activeElement &&
            document.activeElement !== document.body)
            el = document.activeElement;
        if (!el) return err("not_found");
        center(el);
        r = pick(info(el), ["tag", "href", "text"]);
        r.type = "clicked";
        r.nav = navKind(el);
        if (focusable(el)) el.focus(); /* as a real click would */
        el.click();
        return r;
    };

    cmds.focus = function (o) {
        var el = find(o, SEL.focus, true), t, r;
        if (!o.selector && !o.text) return err("no_target");
        if (!el) return err("not_found");
        t = focusTarget(el);
        center(t.el);
        t.el.focus();
        r = pick(info(el), ["tag", "href", "text"]);
        r.type = "focused";
        r.focusedTag = t.el.tagName.toLowerCase();
        r.focusedByAncestor = el !== t.el;
        r.focused = document.activeElement === t.el;
        r.madeFocusable = t.made;
        return r;
    };

    /* type VALUE [--selector|--placeholder|--label] [--nth N] [--no-clear]
     * [--submit]: sets the value (appends with --no-clear) and fires input
     * and change; <select> picks an option by value or text; a
     * contenteditable gets execCommand insertText. From this isolated
     * world a plain el.value= reaches React's own value tracker too. */
    cmds.type = function (o) {
        var value = (o._ && o._[0]) || "", clear = !o["no-clear"], submit = !!o.submit;
        var needle, cands = [], el, opts, opt, want, isCE, ok, r, sel, range, form;
        if (o.selector) {
            cands = Array.from(document.querySelectorAll(o.selector));
        } else if (o.placeholder) {
            needle = norm(o.placeholder);
            cands = Array.from(document.querySelectorAll("input, textarea, [contenteditable]"))
                .filter(function (e) {
                    return norm(e.placeholder || e.getAttribute("aria-placeholder")).indexOf(needle) >= 0;
                });
        } else if (o.label) {
            needle = norm(o.label);
            cands = Array.from(document.querySelectorAll("input, textarea, select, [contenteditable]"))
                .filter(function (e) {
                    var lab, wrap;
                    if (norm(e.getAttribute("aria-label")).indexOf(needle) >= 0) return true;
                    if (norm(e.name).indexOf(needle) >= 0) return true;
                    if (e.id) {
                        lab = document.querySelector('label[for="' + CSS.escape(e.id) + '"]');
                        if (lab && norm(lab.innerText).indexOf(needle) >= 0) return true;
                    }
                    wrap = e.closest("label");
                    return !!(wrap && norm(wrap.innerText).indexOf(needle) >= 0);
                });
        } else if (document.activeElement &&
                   (document.activeElement.matches("input, textarea") ||
                    document.activeElement.isContentEditable)) {
            cands = [document.activeElement];
        }
        el = cands[num(o.nth, 0)] || null;
        if (!el) return err("field_not_found");
        center(el);
        el.focus();
        if (el.tagName === "SELECT") {
            want = norm(value);
            opts = Array.from(el.options);
            opt = opts.find(function (x) { return norm(x.value) === want; }) ||
                  opts.find(function (x) { return norm(x.textContent) === want; }) ||
                  opts.find(function (x) { return norm(x.textContent).indexOf(want) >= 0; }) ||
                  opts.find(function (x) { return norm(x.value).indexOf(want) >= 0; });
            if (!opt) return err("option_not_found");
            el.value = opt.value;
            el.dispatchEvent(new Event("input", {bubbles: true}));
            el.dispatchEvent(new Event("change", {bubbles: true}));
            return {type: "typed", tag: "select", name: el.name || null, id: el.id || null,
                    value: el.value, text: opt.textContent, mode: "value", nav: null};
        }
        isCE = el.isContentEditable;
        if (isCE) {
            sel = getSelection();
            range = document.createRange();
            range.selectNodeContents(el);
            if (!clear) range.collapse(false);
            sel.removeAllRanges();
            sel.addRange(range);
            ok = document.execCommand("insertText", false, value);
            if (!ok) {
                el.textContent = (clear ? "" : el.textContent) + value;
                el.dispatchEvent(new InputEvent("input", {bubbles: true, inputType: "insertText", data: value}));
            }
        } else {
            if (clear) {
                el.value = "";
                el.dispatchEvent(new Event("input", {bubbles: true}));
            }
            el.value = (clear ? "" : el.value) + value;
            el.dispatchEvent(new InputEvent("input", {bubbles: true, inputType: "insertText", data: value}));
        }
        el.dispatchEvent(new Event("change", {bubbles: true}));
        r = {type: "typed", tag: el.tagName.toLowerCase(), name: el.name || null,
             id: el.id || null, value: isCE ? el.textContent : el.value,
             mode: isCE ? (ok ? "execCommand" : "textContent") : "value", nav: null};
        if (submit) {
            form = el.form;
            if (form && typeof form.requestSubmit === "function") { form.requestSubmit(); r.nav = "submit"; }
            else if (form) { form.submit(); r.nav = "submit"; }
            else r.submit = "none"; /* C presses a real Return */
        }
        return r;
    };

    var caretAt = function (x, y) {
        var p, r;
        if (document.caretRangeFromPoint) return document.caretRangeFromPoint(x, y);
        if (document.caretPositionFromPoint) {
            p = document.caretPositionFromPoint(x, y);
            if (!p) return null;
            r = document.createRange();
            r.setStart(p.offsetNode, p.offset);
            r.setEnd(p.offsetNode, p.offset);
            return r;
        }
        return null;
    };

    /* select --selector css | --text s | --from css --to css | --rect x1,y1,x2,y2
     * [--no-mouse] [--no-scroll] [--no-focus] */
    cmds.select = function (o) {
        var sel = getSelection(), range = document.createRange(), el, a, b, walker, node, i;
        var found = null, offset = -1, target = null, t, rr, r, nums, fire, steps, k;
        var dispatchMouse = !o["no-mouse"], scroll = !o["no-scroll"], focus = !o["no-focus"];
        sel.removeAllRanges();
        if (o.selector) {
            el = document.querySelector(o.selector);
            if (!el) return err("selector_not_found");
            range.selectNodeContents(el);
            target = el;
        } else if (o.from && o.to) {
            a = document.querySelector(o.from);
            b = document.querySelector(o.to);
            if (!a || !b) return err("from_or_to_not_found");
            range.setStartBefore(a);
            range.setEndAfter(b);
            target = a;
        } else if (o.text) {
            walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null);
            while ((node = walker.nextNode())) {
                i = node.nodeValue.indexOf(o.text);
                if (i >= 0) { found = node; offset = i; break; }
            }
            if (!found) return err("text_not_found");
            range.setStart(found, offset);
            range.setEnd(found, offset + o.text.length);
            target = found.parentElement;
        } else if (o.rect) {
            nums = String(o.rect).split(",").map(parseFloat);
            a = caretAt(nums[0], nums[1]);
            b = caretAt(nums[2], nums[3]);
            if (!a || !b) return err("no_caret_at_point");
            range.setStart(a.startContainer, a.startOffset);
            range.setEnd(b.startContainer, b.startOffset);
        } else {
            return err("no_target");
        }
        if (scroll) {
            rr = range.getBoundingClientRect();
            if (rr.top < 0 || rr.bottom > innerHeight)
                scrollTo({top: scrollY + rr.top - innerHeight / 3, behavior: "auto"});
        }
        sel.addRange(range);
        if (focus && target) {
            t = focusTarget(target);
            t.el.focus();
        }
        rr = range.getBoundingClientRect();
        r = {type: "selected", text: sel.toString(), rect: rect(rr)};
        /* page handlers see a drag along the selection's midline; the
         * selection itself is already set */
        if (dispatchMouse && rr.width > 0 && rr.height > 0) {
            fire = function (type, x, y, buttons) {
                (document.elementFromPoint(x, y) || document.body).dispatchEvent(
                    new MouseEvent(type, {bubbles: true, cancelable: true, view: window,
                                          clientX: x, clientY: y, button: 0, buttons: buttons}));
            };
            fire("mousedown", rr.x + 1, rr.y + rr.height / 2, 1);
            steps = 6;
            for (k = 1; k <= steps; k++)
                fire("mousemove", rr.x + 1 + (rr.width - 2) * k / steps, rr.y + rr.height / 2, 1);
            fire("mouseup", rr.right - 1, rr.y + rr.height / 2, 0);
        }
        return r;
    };

    cmds.select_clear = function () {
        getSelection().removeAllRanges();
        return {type: "selection_cleared"};
    };

    /* scroll [up|down] [--pages F] [--pixels N] [--to top|bottom|css]
     * [--no-smooth], on the real scroll container */
    cmds.scroll = async function (o) {
        var dir = (o._ && o._[0]) || "down", smooth = !o["no-smooth"];
        var behavior = smooth ? "smooth" : "auto", s = findScroller(), root = isRoot(s);
        var vh = root ? innerHeight : s.clientHeight, el, delta;
        var to = function (top) { (root ? window : s).scrollTo({top: top, behavior: behavior}); };
        var by = function (top) { (root ? window : s).scrollBy({top: top, behavior: behavior}); };
        if (o.to === "top") to(0);
        else if (o.to === "bottom") to(s.scrollHeight);
        else if (o.to) {
            el = document.querySelector(o.to);
            if (!el) return err("selector_not_found");
            el.scrollIntoView({block: "center", inline: "center", behavior: behavior});
        } else {
            delta = o.pixels != null ? num(o.pixels, 0) : num(o.pages, 0.5) * vh;
            by(dir === "up" ? -delta : delta);
        }
        await settle(function () { return root ? scrollY : s.scrollTop; }, smooth);
        return {
            type: "scrolled",
            scrollX: root ? scrollX : s.scrollLeft, scrollY: root ? scrollY : s.scrollTop,
            viewport: {width: innerWidth, height: innerHeight}, docHeight: s.scrollHeight,
            scrollerTag: s.tagName, scrollerId: s.id || null, scrollerClass: s.className || null
        };
    };

    /* highlight --selector css | --text s | --rect x,y,w,h [--all] [--nth N]
     * [--color #hex] [--label s] [--duration MS] [--no-scroll]; boxes
     * follow their element through scrolling and layout changes */
    cmds.highlight = function (o) {
        var color = o.color || "#ff1744", els, needle, chosen, made = [], host, nums;
        var targets = [], duration = num(o.duration, 0), boxes = [];
        if (o.rect) {
            nums = String(o.rect).split(",").map(parseFloat);
            targets.push({el: null, doc: {x: nums[0] + scrollX, y: nums[1] + scrollY, w: nums[2], h: nums[3]}});
        } else if (o.selector || o.text) {
            if (o.selector) els = Array.from(document.querySelectorAll(o.selector));
            else {
                needle = norm(o.text);
                els = Array.from(document.querySelectorAll(SEL.block)).filter(function (e) {
                    return norm(e.innerText).indexOf(needle) >= 0;
                });
            }
            if (!els.length) return err("not_found");
            chosen = o.all ? els : [els[num(o.nth, 0)] || els[0]];
            chosen.forEach(function (e) { targets.push({el: e, doc: null}); });
        } else {
            return err("no_target");
        }
        if (!o["no-scroll"] && targets[0].el) center(targets[0].el);
        host = hlHost();
        targets.forEach(function (t) {
            var r = t.el ? t.el.getBoundingClientRect()
                         : {x: t.doc.x - scrollX, y: t.doc.y - scrollY, width: t.doc.w, height: t.doc.h};
            var box = document.createElement("div"), tag;
            box.style.cssText = "position:fixed;box-sizing:border-box;pointer-events:none;" +
                "border-radius:2px;border:2px solid " + color + ";box-shadow:0 0 0 2px " +
                color + "40,0 0 8px " + color;
            box.style.left = r.x + "px";
            box.style.top = r.y + "px";
            box.style.width = r.width + "px";
            box.style.height = r.height + "px";
            if (o.label) {
                tag = document.createElement("div");
                tag.textContent = o.label;
                tag.style.cssText = "position:absolute;left:0;top:-20px;color:white;" +
                    "font:12px/16px system-ui,sans-serif;padding:1px 6px;border-radius:2px;" +
                    "white-space:nowrap;background:" + color;
                box.appendChild(tag);
            }
            host.appendChild(box);
            t.box = box;
            A.hl.push(t);
            boxes.push(t);
            made.push(rect(r));
        });
        if (duration > 0)
            setTimeout(function () {
                boxes.forEach(function (t) {
                    t.box.remove();
                    A.hl = A.hl.filter(function (h) { return h !== t; });
                });
            }, duration);
        return {type: "highlighted", count: made.length, rects: made};
    };

    cmds.clear_highlights = function () {
        var host = document.getElementById(HL);
        if (host) host.remove();
        A.hl = [];
        removeEventListener("scroll", relayout, {capture: true});
        removeEventListener("resize", relayout);
        return {type: "highlights_cleared"};
    };

    /* the live DOM, without our overlays */
    cmds.dump = function () {
        var ours = [document.getElementById(HL), document.getElementById(CUR)]
            .concat(H.labels.map(function (l) { return l[2]; }))
            .filter(Boolean);
        var parents = ours.map(function (n) { return [n, n.parentNode, n.nextSibling]; }), html;
        ours.forEach(function (n) { n.remove(); });
        html = "<!DOCTYPE " + (document.doctype ? document.doctype.name : "html") + ">\n" +
               document.documentElement.outerHTML;
        parents.forEach(function (p) { p[1].insertBefore(p[0], p[2]); });
        return {type: "dump_result", html: html};
    };

    /* screenshot crop: the element's rect (scrolled into view) and dpr */
    cmds.shot = async function (o) {
        var el = null, r, res = {type: "shot", dpr: devicePixelRatio || 1, rect: null}, nums;
        if (o.rect) {
            nums = String(o.rect).split(",").map(parseFloat);
            res.rect = {x: nums[0], y: nums[1], width: nums[2], height: nums[3]};
            return res;
        }
        if (!o.selector && !o.text) return res;
        el = find(o, SEL.block, false);
        if (!el) return err("target_not_found");
        center(el);
        await settle(function () { return scrollY; }, false);
        r = el.getBoundingClientRect();
        res.rect = rect(r);
        return res;
    };

    /* what is under a point, for mouse replies */
    cmds.at = function (o) {
        var el = document.elementFromPoint(num(o._[0], 0), num(o._[1], 0)), cls;
        if (!el) return {type: "at", target: null};
        cls = typeof el.className === "string" ? el.className : null;
        return {type: "at", target: {
            tag: el.tagName.toLowerCase(), id: el.id || null, cls: cls || null,
            href: el.href || null, text: (el.innerText || el.value || "").trim().slice(0, 120)
        }};
    };

    /* a visible cursor for the synthetic pointer (the real one never moves) */
    cmds.cursor = function (o) {
        var x = num(o._[0], 0), y = num(o._[1], 0), clicked = !!o.click;
        var c = document.getElementById(CUR), d, r;
        if (!c) {
            c = document.createElement("div");
            c.id = CUR;
            c.style.cssText = "position:fixed;top:0;left:0;width:18px;height:18px;" +
                "margin:-9px 0 0 -9px;border:2px solid #00e5ff;border-radius:50%;" +
                "box-shadow:0 0 0 1px rgba(0,0,0,.45),0 0 6px rgba(0,229,255,.85);" +
                "background:rgba(0,229,255,.15);pointer-events:none;z-index:2147483647;" +
                "transition:transform .04s linear;will-change:transform";
            d = document.createElement("div");
            d.style.cssText = "position:absolute;top:50%;left:50%;width:3px;height:3px;" +
                "margin:-1.5px 0 0 -1.5px;background:#00e5ff;border-radius:50%";
            c.appendChild(d);
            document.documentElement.appendChild(c);
        }
        c.style.transform = "translate(" + x + "px," + y + "px)";
        if (clicked) {
            r = document.createElement("div");
            r.style.cssText = "position:fixed;top:0;left:0;width:14px;height:14px;" +
                "margin:-7px 0 0 -7px;border:2px solid #ff1744;border-radius:50%;" +
                "pointer-events:none;z-index:2147483647;opacity:.9;" +
                "transition:transform .35s ease-out,opacity .35s ease-out";
            r.style.transform = "translate(" + x + "px," + y + "px) scale(1)";
            document.documentElement.appendChild(r);
            requestAnimationFrame(function () {
                r.style.transform = "translate(" + x + "px," + y + "px) scale(3)";
                r.style.opacity = "0";
            });
            setTimeout(function () { r.remove(); }, 420);
        }
        return {type: "cursor"};
    };

    cmds.cursor_hide = function () {
        var c = document.getElementById(CUR);
        if (c) c.remove();
        return {type: "cursor_hidden"};
    };

    cmds.metrics = function () {
        var s = findScroller(), root = isRoot(s);
        return {
            type: "metrics", innerWidth: innerWidth, innerHeight: innerHeight,
            scrollX: root ? scrollX : s.scrollLeft, scrollY: root ? scrollY : s.scrollTop,
            docWidth: s.scrollWidth, docHeight: s.scrollHeight, dpr: devicePixelRatio || 1,
            scroller: {tag: s.tagName, id: s.id || null}
        };
    };

    cmds.active = function () {
        var a = document.activeElement;
        return {type: "active", focused: a && a !== document.body ? info(a) : null};
    };

    A.run = async function (verb, o) {
        var f = cmds[String(verb).replace(/-/g, "_")];
        o = o || {};
        if (!o._) o._ = [];
        if (!f) return err("unknown_command");
        try {
            return await f(o);
        } catch (e) {
            return err(String(e && e.message || e));
        }
    };
})();
