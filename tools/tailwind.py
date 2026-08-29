#!/usr/bin/env python3
"""tools/tailwind.py — the static Tailwind pipeline.

Reads assets/ui/tokens.tw and an assets/ui/*.vxml shell, resolves every
utility class against the tokens, and emits two artifacts:

    build/ui/vxui_gen.h    a node table the kernel lays out and paints
    build/ui/vextro.css    the equivalent stylesheet, for WebKit

WHY A TOOL AND NOT A STYLESHEET
-------------------------------

Nothing on this machine can parse CSS. src/browser.h turns HTML into a
flat list of word-wrapped lines with eight styles and discards <style>
elements entirely; WebKit, which would render the real thing, does not
build yet. So a stylesheet shipped on its own would be a file nothing
reads — which is exactly the stub this was asked not to be.

What Tailwind actually *is*, though, is a resolver: utility classes in,
a small set of declarations out, and only the ones used. That part is
portable to a machine with no CSS engine, because the output need not be
CSS. Here it is a table of resolved styles the compositor draws directly,
and the class strings in the shell are therefore real — change
`bg-vextro-gold` to `bg-vextro-charcoal` and the pixels change.

The stylesheet is emitted too, from the same resolution, so that the work
is not repeated the day WebKit compiles. It is the secondary artifact and
it is labelled as one.

WHAT IS RESOLVED WHEN
---------------------

Styles here, geometry at run time. A colour is a colour and `px-4` is
sixteen pixels whatever the window is doing, so those become constants.
The flex solve is not: brw_draw is handed a width and a height on every
frame and the window is resizable, so absolute coordinates baked into a
header would be correct at exactly one size. src/vxui.h does that half.

THE UTILITY TABLE IS CLOSED
---------------------------

Only the utilities below are understood, and an unknown class is a build
error naming the file and the class. That is the line between a build
tool and a browser engine: Tailwind proper generates from an open-ended
grammar, and reproducing that would mean reproducing CSS. What is here
is the set the shell uses plus its obvious neighbours, and adding one is
adding a line to this table.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class BuildError(Exception):
    pass


# ---------------------------------------------------------------- tokens

class Tokens:
    """The palette and the scales, from assets/ui/tokens.tw."""

    def __init__(self):
        self.color = {}
        self.space = {}
        self.text = {}
        self.radius = {}
        self.track = {}

    @staticmethod
    def load(path):
        t = Tokens()
        table = {
            "color": t.color, "space": t.space, "text": t.text,
            "radius": t.radius, "track": t.track,
        }
        with open(path) as f:
            for lineno, raw in enumerate(f, 1):
                # A '#' starts a comment *unless* it starts a colour.
                # Splitting on the first '#' unconditionally is the
                # obvious thing and eats every value in the file.
                line = re.sub(r"#(?![0-9A-Fa-f]{6}\b).*$", "", raw).strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) != 3:
                    raise BuildError(
                        f"{path}:{lineno}: expected 'kind name value', got {line!r}")
                kind, name, value = parts
                if kind not in table:
                    raise BuildError(f"{path}:{lineno}: unknown token kind {kind!r}")
                if kind == "color":
                    if not re.fullmatch(r"#[0-9A-Fa-f]{6}", value):
                        raise BuildError(
                            f"{path}:{lineno}: {value!r} is not a #RRGGBB colour")
                    table[kind][name] = int(value[1:], 16)
                else:
                    table[kind][name] = int(value)
        if not t.color:
            raise BuildError(f"{path}: no colours defined")
        return t


# ------------------------------------------------------------- the style

# Border sides, as a bitmask. The generated header repeats these.
SIDE_T, SIDE_R, SIDE_B, SIDE_L = 1, 2, 4, 8
SIDE_ALL = SIDE_T | SIDE_R | SIDE_B | SIDE_L

# flags
F_FLEX_ROW   = 1 << 0
F_FLEX_COL   = 1 << 1
F_ITEMS_CTR  = 1 << 2
F_JUST_BETW  = 1 << 3
F_JUST_CTR   = 1 << 4
F_JUST_END   = 1 << 5
F_FONT_MONO  = 1 << 6
F_FONT_BOLD  = 1 << 7
F_WIDTH_FULL = 1 << 8
F_GRAD_TEXT  = 1 << 9
F_SHADOW_IN  = 1 << 10
F_TEXT_TRANS = 1 << 11


class Style:
    def __init__(self):
        self.flags = 0
        self.flex = 0
        self.w = -1
        self.h = -1
        self.pad = [0, 0, 0, 0]      # t r b l
        self.mar = [0, 0, 0, 0]
        self.gap = 0
        self.bg = None
        self.bg_alpha = 255
        self.fg = None
        self.border = None
        self.border_sides = 0
        self.radius = 0
        self.font_size = 0
        self.track = 0
        self.grad = [None, None, None]   # from, via, to
        # Every declaration this resolved to, in CSS terms, for the
        # stylesheet emit. Kept as a list so the order matches the class
        # order — which is what makes the generated CSS diffable against
        # the shell it came from.
        self.css = []


def resolve(classes, tok, where):
    """One class string to a Style. Raises on anything not in the table."""
    st = Style()

    def color(name, lineno_what):
        if name not in tok.color:
            raise BuildError(f"{where}: {lineno_what}: no colour token "
                             f"{name!r} in tokens.tw")
        return tok.color[name]

    def space(n, what):
        if n not in tok.space:
            raise BuildError(f"{where}: {what}: no spacing step {n!r} in "
                             f"tokens.tw")
        return tok.space[n]

    for cls in classes:
        # bg-<color>, optionally /<alpha percent>
        m = re.fullmatch(r"bg-([a-z0-9-]+?)(?:/(\d+))?", cls)
        if m and m.group(1) in tok.color:
            st.bg = color(m.group(1), cls)
            if m.group(2):
                pct = int(m.group(2))
                if not 0 <= pct <= 100:
                    raise BuildError(f"{where}: {cls}: opacity out of range")
                st.bg_alpha = (pct * 255) // 100
                st.css.append(f"background-color: rgb({st.bg >> 16 & 255} "
                              f"{st.bg >> 8 & 255} {st.bg & 255} / {pct}%)")
            else:
                st.css.append(f"background-color: #{st.bg:06X}")
            continue

        m = re.fullmatch(r"text-([a-z0-9-]+)", cls)
        if m and m.group(1) in tok.color:
            st.fg = color(m.group(1), cls)
            st.css.append(f"color: #{st.fg:06X}")
            continue

        m = re.fullmatch(r"text-([a-z0-9]+)", cls)
        if m and m.group(1) in tok.text:
            st.font_size = tok.text[m.group(1)]
            st.css.append(f"font-size: {st.font_size}px")
            continue

        m = re.fullmatch(r"border-([a-z0-9-]+)", cls)
        if m and m.group(1) in tok.color:
            st.border = color(m.group(1), cls)
            if st.border_sides == 0:
                st.border_sides = SIDE_ALL
            st.css.append(f"border-color: #{st.border:06X}")
            continue

        if cls == "border":
            st.border_sides |= SIDE_ALL
            st.css.append("border-width: 1px")
            continue
        if cls in ("border-t", "border-r", "border-b", "border-l"):
            bit = {"border-t": SIDE_T, "border-r": SIDE_R,
                   "border-b": SIDE_B, "border-l": SIDE_L}[cls]
            st.border_sides |= bit
            side = {"border-t": "top", "border-r": "right",
                    "border-b": "bottom", "border-l": "left"}[cls]
            st.css.append(f"border-{side}-width: 1px")
            continue

        m = re.fullmatch(r"rounded(?:-([a-z]+))?", cls)
        if m:
            name = m.group(1) or "md"
            if name not in tok.radius:
                raise BuildError(f"{where}: {cls}: no radius token {name!r}")
            st.radius = tok.radius[name]
            st.css.append(f"border-radius: {st.radius}px")
            continue

        m = re.fullmatch(r"([hw])-(\d+)", cls)
        if m:
            v = space(m.group(2), cls)
            if m.group(1) == "h":
                st.h = v
                st.css.append(f"height: {v}px")
            else:
                st.w = v
                st.css.append(f"width: {v}px")
            continue

        if cls == "w-full":
            st.flags |= F_WIDTH_FULL
            st.css.append("width: 100%")
            continue

        m = re.fullmatch(r"(p|m)([xytrbl]?)-(\d+)", cls)
        if m:
            kind, which, n = m.group(1), m.group(2), m.group(3)
            v = space(n, cls)
            target = st.pad if kind == "p" else st.mar
            prop = "padding" if kind == "p" else "margin"
            if which == "" :
                target[0] = target[1] = target[2] = target[3] = v
                st.css.append(f"{prop}: {v}px")
            elif which == "x":
                target[1] = target[3] = v
                st.css.append(f"{prop}-inline: {v}px")
            elif which == "y":
                target[0] = target[2] = v
                st.css.append(f"{prop}-block: {v}px")
            else:
                idx = {"t": 0, "r": 1, "b": 2, "l": 3}[which]
                target[idx] = v
                side = {"t": "top", "r": "right", "b": "bottom", "l": "left"}[which]
                st.css.append(f"{prop}-{side}: {v}px")
            continue

        m = re.fullmatch(r"gap-(\d+)", cls)
        if m:
            st.gap = space(m.group(1), cls)
            st.css.append(f"gap: {st.gap}px")
            continue

        if cls == "flex":
            st.flags |= F_FLEX_ROW
            st.css.append("display: flex")
            continue
        if cls == "flex-col":
            st.flags |= F_FLEX_COL
            st.css.append("display: flex; flex-direction: column")
            continue
        m = re.fullmatch(r"flex-(\d+)", cls)
        if m:
            st.flex = int(m.group(1))
            st.css.append(f"flex: {st.flex} 1 0%")
            continue

        if cls == "items-center":
            st.flags |= F_ITEMS_CTR
            st.css.append("align-items: center")
            continue
        if cls == "items-start":
            st.css.append("align-items: flex-start")
            continue
        if cls == "justify-between":
            st.flags |= F_JUST_BETW
            st.css.append("justify-content: space-between")
            continue
        if cls == "justify-center":
            st.flags |= F_JUST_CTR
            st.css.append("justify-content: center")
            continue
        if cls == "justify-end":
            st.flags |= F_JUST_END
            st.css.append("justify-content: flex-end")
            continue
        if cls == "justify-start":
            st.css.append("justify-content: flex-start")
            continue

        if cls == "font-mono":
            st.flags |= F_FONT_MONO
            st.css.append("font-family: ui-monospace, monospace")
            continue
        if cls == "font-sans":
            st.css.append("font-family: ui-sans-serif, system-ui")
            continue
        if cls == "font-bold":
            st.flags |= F_FONT_BOLD
            st.css.append("font-weight: 700")
            continue

        m = re.fullmatch(r"tracking-([a-z]+)", cls)
        if m:
            name = m.group(1)
            if name not in tok.track:
                raise BuildError(f"{where}: {cls}: no tracking token {name!r}")
            st.track = tok.track[name]
            st.css.append(f"letter-spacing: {st.track}px")
            continue

        if cls == "shadow-inner":
            st.flags |= F_SHADOW_IN
            st.css.append("box-shadow: inset 0 2px 4px rgb(0 0 0 / 0.35)")
            continue

        if cls == "text-transparent":
            st.flags |= F_TEXT_TRANS
            st.css.append("color: transparent")
            continue
        if cls == "bg-clip-text":
            st.flags |= F_GRAD_TEXT
            st.css.append("background-clip: text")
            continue
        if cls == "bg-gradient-to-r":
            st.flags |= F_GRAD_TEXT
            st.css.append("background-image: linear-gradient(to right, "
                          "var(--tw-gradient-stops))")
            continue

        m = re.fullmatch(r"(from|via|to)-([a-z0-9-]+)", cls)
        if m and m.group(2) in tok.color:
            slot = {"from": 0, "via": 1, "to": 2}[m.group(1)]
            st.grad[slot] = color(m.group(2), cls)
            st.css.append(f"--tw-gradient-{m.group(1)}: #{st.grad[slot]:06X}")
            continue

        raise BuildError(
            f"{where}: unknown utility class {cls!r}.\n"
            f"  The utility table in tools/tailwind.py is closed on purpose "
            f"— see the note at the top of that file. Add it there, or fix "
            f"the spelling.")

    return st


# --------------------------------------------------------------- parsing

class Node:
    def __init__(self, ident, classes, text):
        self.next_sibling = -1
        self.id = ident
        self.classes = classes
        self.text = text
        self.children = []
        self.style = None
        self.index = -1


TAG = re.compile(r"<node\b(?P<attrs>[^>]*?)(?P<selfclose>/?)>|</node\s*>")
ATTR = re.compile(r'(\w+)\s*=\s*"([^"]*)"')


def parse_shell(path):
    """The shell, into a forest of Nodes.

    A deliberately small parser: <node> elements with attributes, nested,
    and nothing else. Comments and the <ui> wrapper are stripped. It is
    not an XML parser and does not pretend to be — what it accepts is
    exactly what assets/ui/*.vxml is allowed to contain, and anything
    else is a build error rather than a guess.
    """
    src = open(path).read()
    src = re.sub(r"<!--.*?-->", "", src, flags=re.S)
    src = re.sub(r"</?ui\s*>", "", src)

    roots, stack = [], []
    pos = 0
    for m in TAG.finditer(src):
        between = src[pos:m.start()].strip()
        if between:
            raise BuildError(f"{path}: stray text {between[:40]!r} outside a node")
        pos = m.end()

        if m.group(0).startswith("</"):
            if not stack:
                raise BuildError(f"{path}: closing tag with nothing open")
            stack.pop()
            continue

        attrs = dict(ATTR.findall(m.group("attrs") or ""))
        unknown = set(attrs) - {"id", "class", "text"}
        if unknown:
            raise BuildError(f"{path}: unknown attribute(s) "
                             f"{', '.join(sorted(unknown))}")

        node = Node(attrs.get("id"), (attrs.get("class") or "").split(),
                    attrs.get("text"))
        if stack:
            stack[-1].children.append(node)
        else:
            roots.append(node)
        if not m.group("selfclose"):
            stack.append(node)

    if stack:
        raise BuildError(f"{path}: {len(stack)} node(s) left open")
    tail = src[pos:].strip()
    if tail:
        raise BuildError(f"{path}: stray text {tail[:40]!r} after the last node")
    return roots


def flatten(roots):
    """Depth-first, assigning indices.

    The table is flat and children are reached by index, because a tree
    of pointers in a generated header would need relocation and this does
    not.

    Each node records its *next sibling* explicitly rather than the
    children being assumed contiguous. They are not: depth-first order
    puts a child's whole subtree between it and the next child, so
    walking `first_child, first_child+1, ...` would descend into a
    grandchild and lay it out as a sibling.
    """
    order = []

    def walk(n):
        n.index = len(order)
        order.append(n)
        for c in n.children:
            walk(c)
        for i, c in enumerate(n.children):
            c.next_sibling = n.children[i + 1].index \
                if i + 1 < len(n.children) else -1

    for r in roots:
        r.next_sibling = -1
        walk(r)
    return order


# --------------------------------------------------------------- emitting

def c_string(s):
    if s is None:
        return "0"
    out = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{out}"'


def emit_header(roots, order, tok, out_path, sources):
    L = []
    a = L.append
    a("/* Generated by tools/tailwind.py — do not edit.")
    a(" *")
    a(" * Source:")
    for s in sources:
        a(f" *   {s}")
    a(" *")
    a(" * The utility classes in the shell, resolved against the design")
    a(" * tokens. Geometry is *not* here: the flex solve happens at run")
    a(" * time in src/vxui.h, because the browser window is resizable and")
    a(" * a coordinate baked in here would be right at one size only.")
    a(" */")
    a("#ifndef VXUI_GEN_H")
    a("#define VXUI_GEN_H")
    a("")
    a("/* ---- the palette, as the tokens define it ---- */")
    for name, value in tok.color.items():
        a(f"#define VXUI_{name.upper().replace('-', '_')}  0x{value:06X}u")
    a("")
    a("/* ---- node ids, for the C that has to find one ---- */")
    ids = [n for n in order if n.id]
    for n in ids:
        a(f"#define VXUI_ID_{n.id.upper().replace('-', '_')}  {n.index}")
    a(f"#define VXUI_NODE_COUNT  {len(order)}")
    a("")
    a("/* ---- roots ---- */")
    for r in roots:
        if r.id:
            a(f"#define VXUI_ROOT_{r.id.upper().replace('-', '_')}  {r.index}")
    a("")
    a("static const vxui_node_t vxui_nodes[VXUI_NODE_COUNT] = {")
    for n in order:
        st = n.style
        kids = n.children
        first = kids[0].index if kids else -1
        # siblings are contiguous in depth-first order only for the first;
        # each child records the next explicitly.
        a(f"    /* {n.index:2d} {n.id or '-'} */ {{")
        a(f"        .first_child = {first}, .child_count = {len(kids)},")
        a(f"        .next_sibling = {getattr(n, 'next_sibling', -1)},")
        a(f"        .flags = {st.flags}u, .flex = {st.flex},")
        a(f"        .w = {st.w}, .h = {st.h},")
        a(f"        .pad = {{ {st.pad[0]}, {st.pad[1]}, {st.pad[2]}, {st.pad[3]} }},")
        a(f"        .mar = {{ {st.mar[0]}, {st.mar[1]}, {st.mar[2]}, {st.mar[3]} }},")
        a(f"        .gap = {st.gap},")
        a(f"        .has_bg = {1 if st.bg is not None else 0}, "
          f".bg = 0x{(st.bg or 0):06X}u, .bg_alpha = {st.bg_alpha},")
        a(f"        .has_fg = {1 if st.fg is not None else 0}, "
          f".fg = 0x{(st.fg or 0):06X}u,")
        a(f"        .border_sides = {st.border_sides}u, "
          f".border = 0x{(st.border or 0):06X}u,")
        a(f"        .radius = {st.radius}, .font_size = {st.font_size},")
        a(f"        .track = {st.track},")
        a(f"        .grad = {{ 0x{(st.grad[0] or 0):06X}u, "
          f"0x{(st.grad[1] or 0):06X}u, 0x{(st.grad[2] or 0):06X}u }},")
        a(f"        .text = {c_string(n.text)},")
        a("    },")
    a("};")
    a("")
    a("#endif /* VXUI_GEN_H */")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(L) + "\n")
    return len(order)


def emit_css(order, tok, out_path, sources):
    """The same resolution, as a stylesheet.

    Secondary, and labelled as such: nothing on this machine reads CSS
    today. src/browser.h discards <style> and WebKit does not build yet
    — third_party/wpe-config/README.md has the ladder. It is emitted
    because it costs one pass over a table that already exists, and
    because the day the engine builds this is the skin it should wear.
    """
    L = []
    a = L.append
    a("/* Generated by tools/tailwind.py — do not edit.")
    a(" *")
    a(" * Source:")
    for s in sources:
        a(f" *   {s}")
    a(" *")
    a(" * NOT USED YET, and that is not an oversight. Nothing on this")
    a(" * machine parses CSS: src/browser.h turns HTML into word-wrapped")
    a(" * lines and discards <style>, and WPE WebKit -- which would render")
    a(" * this -- does not compile here yet. What *is* live is the C")
    a(" * header emitted beside this file from the same resolution, which")
    a(" * the compositor draws directly.")
    a(" *")
    a(" * This exists so the day the engine builds, the skin is already")
    a(" * written and already agrees with what the desktop shows.")
    a(" */")
    a("")
    a(":root {")
    for name, value in tok.color.items():
        a(f"  --{name}: #{value:06X};")
    a("}")
    a("")
    seen = set()
    for n in order:
        if not n.id:
            continue
        if n.id in seen:
            raise BuildError(f"duplicate node id {n.id!r}")
        seen.add(n.id)
        decls = n.style.css
        if not decls:
            continue
        a(f"#{n.id} {{")
        for d in decls:
            a(f"  {d};")
        a("}")
        a("")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(L) + "\n")


# ------------------------------------------------------------------ main

def build(shell, tokens_path, header_out, css_out):
    tok = Tokens.load(tokens_path)
    roots = parse_shell(shell)
    order = flatten(roots)

    seen_ids = set()
    for n in order:
        if n.id:
            if n.id in seen_ids:
                raise BuildError(f"{shell}: duplicate id {n.id!r}")
            seen_ids.add(n.id)
        n.style = resolve(n.classes, tok, f"{shell} (node {n.id or n.index})")

    sources = [os.path.relpath(tokens_path, ROOT), os.path.relpath(shell, ROOT)]
    count = emit_header(roots, order, tok, header_out, sources)
    emit_css(order, tok, css_out, sources)
    return count, len(tok.color)


def main(argv):
    shell = os.path.join(ROOT, "assets/ui/browser.vxml")
    tokens_path = os.path.join(ROOT, "assets/ui/tokens.tw")
    header_out = os.path.join(ROOT, "build/ui/vxui_gen.h")
    css_out = os.path.join(ROOT, "build/ui/vextro.css")

    if "--check" in argv:
        # Resolve and report, writing nothing. What `make test` runs.
        tok = Tokens.load(tokens_path)
        roots = parse_shell(shell)
        order = flatten(roots)
        for n in order:
            n.style = resolve(n.classes, tok, f"{shell} (node {n.id or n.index})")
        used = sum(len(n.classes) for n in order)
        print(f"  ok   tailwind: {len(order)} nodes, {used} utility classes, "
              f"{len(tok.color)} colour tokens")
        return 0

    count, colors = build(shell, tokens_path, header_out, css_out)
    print(f"  TAILWIND {os.path.relpath(header_out, ROOT)}: "
          f"{count} nodes, {colors} colour tokens")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BuildError as e:
        print(f"tailwind: {e}", file=sys.stderr)
        sys.exit(1)
