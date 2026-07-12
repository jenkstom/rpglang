#!/usr/bin/env python3
"""Assemble the rpglang documentation site.

Builds a self-contained static site into BUILD_DIR (default: public) that
links together every piece of project documentation:

  * The interactive HTML tutorial (docs/tutorial/)
  * Man pages rendered to HTML (docs/man/*.1)
  * Markdown reference and design docs, converted to themed HTML
  * A landing index.html that is the single entry point to all of the above

The generated pages share the same dark "Tokyo Night" theme as the tutorial so
the whole site feels cohesive, and every page carries a top navigation bar back
to the landing page and its sibling documents.

Usage:
    python3 tools/build-docs.py [BUILD_DIR]

Dependencies (only needed for the non-tutorial conversions):
    * groff           - man page -> HTML   (apt-get install groff)
    * markdown        - Markdown -> HTML   (pip install markdown)
"""

from __future__ import annotations

import html
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else (sys.argv[1] if len(sys.argv) > 1 else os.environ.get("BUILD_DIR", "public")))

# Shared stylesheet, matching the tutorial's Tokyo Night palette so the entire
# site is visually unified.
CSS = """
* { box-sizing: border-box; }
body {
  font-family: "Segoe UI", system-ui, -apple-system, sans-serif;
  max-width: 920px; margin: 2rem auto; padding: 0 1.5rem;
  line-height: 1.6; color: #c0caf5; background: #1a1b26;
}
header.sitenav {
  position: sticky; top: 0; z-index: 10;
  display: flex; flex-wrap: wrap; gap: .4rem .9rem; align-items: center;
  background: rgba(26,27,38,.92); backdrop-filter: blur(6px);
  padding: .55rem 0; margin: -1rem 0 1.5rem;
  border-bottom: 1px solid #333653;
}
header.sitenav .brand { font-weight: 700; color: #7aa2f7; letter-spacing: .02em; }
header.sitenav a { color: #9aa5ce; font-size: .9rem; text-decoration: none; }
header.sitenav a:hover { color: #7dcfff; text-decoration: underline; }
header.sitenav .sep { color: #414868; }
h1 { font-size: 2rem; border-bottom: 3px solid #7aa2f7; color: #7aa2f7; padding-bottom: .3rem; margin-top: 1.2rem; }
h2 { font-size: 1.4rem; margin-top: 2.5rem; border-bottom: 1px solid #333653; color: #7dcfff; padding-bottom: .2rem; }
h3 { font-size: 1.15rem; margin-top: 1.8rem; color: #bb9af7; }
h4 { color: #bb9af7; }
h5, h6 { color: #9aa5ce; }
p { margin: 1rem 0; }
code { font-family: "Consolas","Courier New",monospace; background: #2a2b3d; color: #e0af68; padding: .1em .35em; border-radius: 3px; font-size: .9em; }
pre { background: #13131f; color: #c0caf5; padding: 1rem; border-radius: 8px; overflow-x: auto; border: 1px solid #333653; }
pre code { background: none; color: inherit; padding: 0; font-size: .88em; }
blockquote { border-left: 4px solid #7aa2f7; background: #24283b; margin: 1rem 0; padding: .8rem 1.2rem; border-radius: 0 6px 6px 0; }
table { border-collapse: collapse; width: 100%; margin: 1rem 0; }
th, td { border: 1px solid #333653; padding: .5rem .8rem; text-align: left; vertical-align: top; }
th { background: #7aa2f7; color: #1a1b26; }
tr:nth-child(even) { background: #1f2335; }
a { color: #7dcfff; text-decoration: none; }
a:hover { text-decoration: underline; }
hr { border: none; border-top: 1px solid #333653; margin: 2rem 0; }
ul, ol { padding-left: 1.4rem; }
li { margin-bottom: .3rem; }
.docbody { font-size: 1rem; }
.lead { font-size: 1.1rem; color: #9aa5ce; }
footer { margin-top: 3rem; padding-top: 1rem; border-top: 1px solid #333653; color: #565f89; font-size: .85rem; }
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 1rem; margin: 1.5rem 0; }
.card { background: #1f2335; border: 1px solid #333653; border-radius: 10px; padding: 1.1rem 1.3rem; transition: border-color .15s, transform .15s; }
.card:hover { border-color: #7aa2f7; transform: translateY(-2px); }
.card h3 { margin-top: 0; color: #7aa2f7; }
.card p { margin: .4rem 0 0; color: #9aa5ce; font-size: .92rem; }
.card a { color: inherit; text-decoration: none; }
.card h3 a { color: #7aa2f7; }
.section-title { font-size: 1.25rem; font-weight: 700; color: #7dcfff; margin: 2.4rem 0 .2rem; padding-bottom: .3rem; border-bottom: 2px solid #333653; }
.hero { background: #1f2335; border: 1px solid #7aa2f7; border-radius: 14px; padding: 1.8rem 2rem; margin: 1.5rem 0 1rem; }
.hero h1 { border: none; margin-top: 0; }
.tags { display: flex; gap: .4rem; flex-wrap: wrap; margin-top: .6rem; }
.tag { font-size: .75rem; background: #2a2b3d; border: 1px solid #333653; color: #9aa5ce; padding: .15rem .55rem; border-radius: 999px; }
"""

DOCS = [
    # (title, source path relative to REPO, output slug, blurb, category)
    {
        "title": "rpglang — Overview",
        "src": "README.md",
        "slug": "readme",
        "blurb": "Project summary, quick start, feature coverage, and the layout of the repository. Start here for the elevator pitch.",
        "category": "Getting started",
        "icon": "★",
    },
    {
        "title": "Programming in RPG II — A Tutorial",
        "src": "docs/tutorial/index.html",
        "slug": "tutorial",
        "blurb": "A hands-on, 20-chapter walkthrough of RPG II from fixed-column source layout through multifile processing, with diagrams and worked examples.",
        "category": "Getting started",
        "icon": "✦",
        "raw": True,
    },
    {
        "title": "rpgc(1) — RPG II to LLVM compiler",
        "src": "docs/man/rpgc.1",
        "slug": "man/rpgc",
        "blurb": "Man page for the compiler: every option, emit mode, optimization flag, and the test-hook exit-code convention.",
        "category": "Reference",
        "icon": "❯",
        "man": True,
    },
    {
        "title": "rpg-analyze(1) — static analysis toolkit",
        "src": "docs/man/rpg-analyze.1",
        "slug": "man/rpg-analyze",
        "blurb": "Man page for the analyzer: all 20 modules, utility commands (decode, search, diff, callgraph, portfolio…), and JSON/CI output.",
        "category": "Reference",
        "icon": "❯",
        "man": True,
    },
    {
        "title": "RPG II Spec Column Map",
        "src": "docs/SPEC_MAP.md",
        "slug": "spec-map",
        "blurb": "The authoritative column-by-column reference for every spec type (F, I, C, O, E, H). Look here when a column-alignment error stumps you.",
        "category": "Reference",
        "icon": "◧",
    },
    {
        "title": "Building & Packaging",
        "src": "docs/BUILDING.md",
        "slug": "building",
        "blurb": "Build prerequisites, installation, and Debian/.deb packaging with CPack or dpkg-buildpackage.",
        "category": "Reference",
        "icon": "◧",
    },
    {
        "title": "Compiler Architecture",
        "src": "docs/ARCHITECTURE.md",
        "slug": "architecture",
        "blurb": "Internal design for compiler hackers: the column parser, indicator→IR mapping, the program cycle, and the LLVM codegen pipeline.",
        "category": "Internals & design",
        "icon": "⚙",
    },
    {
        "title": "rpg-analyze — Design & Rationale",
        "src": "TOOLS_IDEAS.md",
        "slug": "analyzer-design",
        "blurb": "Full design rationale for every analysis module and utility command in rpg-analyze.",
        "category": "Internals & design",
        "icon": "⚙",
    },
    {
        "title": "Plan: CALL / External Linkage",
        "src": "CALL_LINKAGE_PLAN.md",
        "slug": "plan-call-linkage",
        "blurb": "Design status for CALL to external programs (not yet implemented).",
        "category": "Unimplemented-feature plans",
        "icon": "☐",
    },
    {
        "title": "Plan: WORKSTN / Display Files",
        "src": "WRKSTN_PLAN.md",
        "slug": "plan-workstation",
        "blurb": "Design status for WORKSTN/display-file support (not yet implemented).",
        "category": "Unimplemented-feature plans",
        "icon": "☐",
    },
    {
        "title": "Plan: Keyboard / WORKSTN Keys",
        "src": "KEYBORD_PLAN.md",
        "slug": "plan-keyboard",
        "blurb": "Design status for keyboard/attention-key handling (not yet implemented).",
        "category": "Unimplemented-feature plans",
        "icon": "☐",
    },
    {
        "title": "Plan: Miscellaneous Opcodes",
        "src": "MISC_OPCODES_PLAN.md",
        "slug": "plan-misc-opcodes",
        "blurb": "Design status for the remaining miscellaneous C-spec opcodes (not yet implemented).",
        "category": "Unimplemented-feature plans",
        "icon": "☐",
    },
]

# Map of source-relative link targets -> site-relative URLs, applied to every
# converted page so cross-references resolve correctly across the flattened site.
LINK_REWRITES = {
    "README.md": "readme.html",
    "docs/BUILDING.md": "building.html",
    "docs/SPEC_MAP.md": "spec-map.html",
    "docs/ARCHITECTURE.md": "architecture.html",
    "docs/tutorial/": "tutorial/",
    "docs/tutorial/index.html": "tutorial/index.html",
    "docs/man/rpgc.1": "man/rpgc.html",
    "docs/man/rpg-analyze.1": "man/rpg-analyze.html",
    "TOOLS_IDEAS.md": "analyzer-design.html",
    "CALL_LINKAGE_PLAN.md": "plan-call-linkage.html",
    "WRKSTN_PLAN.md": "plan-workstation.html",
    "KEYBORD_PLAN.md": "plan-keyboard.html",
    "MISC_OPCODES_PLAN.md": "plan-misc-opcodes.html",
    # groff emits ".1.html" cross-reference links; normalise them.
    "rpgc.1.html": "rpgc.html",
    "rpg-analyze.1.html": "rpg-analyze.html",
}


def log(msg: str) -> None:
    print(f"  • {msg}", flush=True)


def nav_html(current_slug: str | None = None) -> str:
    """Top navigation bar present on every generated page."""
    items = [("index.html", "Home"), ("readme.html", "Overview"),
             ("tutorial/index.html", "Tutorial"),
             ("man/rpgc.html", "rpgc(1)"), ("man/rpg-analyze.html", "rpg-analyze(1)")]
    links = ['<span class="brand">rpglang docs</span>']
    for href, label in items:
        cls = ' style="color:#7dcfff;font-weight:600"' if href.rstrip("/") == (current_slug or "") else ""
        links.append(f'<a href="{href}"{cls}>{label}</a>')
    sep = '<span class="sep">·</span>'
    return '<header class="sitenav">' + sep.join(links) + "</header>"


def page(title: str, body: str, current_slug: str | None = None, extra_head: str = "") -> str:
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{html.escape(title)}</title>
<style>
{CSS}
</style>
{extra_head}
</head>
<body>
{nav_html(current_slug)}
{body}
<footer>Generated from the <code>rpglang</code> repository source ·
<a href="readme.html">Overview</a> · <a href="tutorial/index.html">Tutorial</a> ·
<a href="man/rpgc.html">rpgc(1)</a> · <a href="man/rpg-analyze.html">rpg-analyze(1)</a></footer>
</body>
</html>
"""


def rewrite_links(text: str) -> str:
    """Rewrite repository-relative href/src targets to site-relative URLs."""

    def repl(match: re.Match) -> str:
        quote = match.group(1)
        target = match.group(2)
        # Only rewrite the path portion of a URL (before any fragment/query).
        path, sep, rest = target.partition("#") if "#" in target else target.partition("?")
        path = path.strip()
        if path in LINK_REWRITES:
            new = LINK_REWRITES[path]
            return f'href={quote}{new}{sep}{rest}{quote}'
        return match.group(0)

    return re.sub(r'href=(["\'])([^"\']+)\1', repl, text)


def convert_markdown(src: Path, title: str) -> str:
    try:
        import markdown  # type: ignore
    except ImportError:
        log("WARNING: 'markdown' package not installed; embedding raw Markdown.")
        raw = html.escape(src.read_text(encoding="utf-8"))
        return f'<pre style="white-space:pre-wrap">{raw}</pre>'

    text = src.read_text(encoding="utf-8")
    md = markdown.Markdown(
        extensions=["fenced_code", "tables", "toc", "sane_lists", "attr_list"],
        output_format="html5",
    )
    body = md.convert(text)
    return rewrite_links(body)


def extract_groff_body(groff_html: str) -> str:
    """Pull the inner <body> out of groff's full HTML document."""
    m = re.search(r"<body[^>]*>(.*)</body>", groff_html, re.S | re.I)
    inner = m.group(1) if m else groff_html
    # groff wraps page content in <p>...</p> with style attributes; strip those
    # wrappers so our stylesheet governs layout.
    inner = re.sub(r'<p style="[^"]*">', "<p>", inner)
    return inner


def convert_man(src: Path) -> str:
    if shutil.which("groff"):
        # Preferred: rich HTML from the groff HTML device (full groff package).
        try:
            proc = subprocess.run(
                ["groff", "-mandoc", "-Thtml", "-r", "ll=100n", str(src)],
                capture_output=True, text=True, check=True,
            )
            body = extract_groff_body(proc.stdout)
            body = re.sub(r"<title>.*?</title>", "", body, flags=re.S | re.I)
            return rewrite_links(body)
        except subprocess.CalledProcessError:
            pass  # fall through to the ASCII device (groff-base only)
        # Fallback: the ASCII device ships with every groff install. Strip the
        # ANSI attribute escapes and present a clean monospaced rendering.
        try:
            proc = subprocess.run(
                ["groff", "-mandoc", "-Tascii", str(src)],
                capture_output=True, text=True, check=True,
            )
            clean = re.sub(r"\x1b\[[0-9;]*m", "", proc.stdout)
            return rewrite_links(clean)
        except subprocess.CalledProcessError as exc:
            log(f"WARNING: groff failed on {src.name}: {exc.stderr[:200]}")
    log(f"WARNING: groff unavailable; embedding raw source for {src.name}.")
    raw = html.escape(src.read_text(encoding="utf-8"))
    return f'<pre style="white-space:pre-wrap">{raw}</pre>'


def by_slug(slug: str) -> dict | None:
    for d in DOCS:
        if d["slug"] == slug:
            return d
    return None


def build_landing() -> str:
    cats: dict[str, list[dict]] = {}
    for d in DOCS:
        cats.setdefault(d["category"], []).append(d)

    sections = []
    for cat, items in cats.items():
        cards = []
        for d in items:
            href = ("tutorial/index.html" if d["slug"] == "tutorial"
                    else (d["slug"] + ".html"))
            icon = d.get("icon", "▪")
            cards.append(
                f'<article class="card"><h3><a href="{href}">{icon} {html.escape(d["title"].split(" — ")[0])}</a></h3>'
                f'<p>{html.escape(d["blurb"])}</p></article>'
            )
        sections.append(
            f'<div class="section-title">{html.escape(cat)}</div>\n'
            f'<div class="cards">\n' + "\n".join(cards) + "\n</div>"
        )

    body = f"""
<div class="hero">
  <h1>rpglang documentation</h1>
  <p class="lead">An <strong>RPG&nbsp;II</strong> compiler and static-analysis toolkit for Linux,
  compiling the column-oriented fixed-format language of IBM System/34, System/36,
  and AS/400 midrange systems into native ELF executables via <strong>LLVM&nbsp;19</strong>.</p>
  <div class="tags">
    <span class="tag">RPG II compiler</span>
    <span class="tag">LLVM 19 codegen</span>
    <span class="tag">static analysis</span>
    <span class="tag">C runtime</span>
    <span class="tag">Linux native</span>
  </div>
</div>

<p>Everything is reachable from this page: the interactive tutorial, the
<code>rpgc</code> and <code>rpg-analyze</code> man pages, the column-reference
and build guides, and the internal architecture &amp; design documents.
Pick a card below, or use the navigation bar at the top of any page.</p>

{chr(10).join(sections)}

<div class="section-title">IBM RPG II reference manuals (raw)</div>
<p>Two large plain-text scans of the original IBM System/36 RPG II reference
manual are bundled in the repository under <code>docs/ref/</code> and linked
here for deep reference:</p>
<ul>
  <li><a href="ref/manual_text.html"><strong>manual_text.txt</strong></a> — full text of the IBM RPG II reference (text view, ~1.9&nbsp;MB)</li>
  <li><a href="ref/manual_layout.txt"><strong>manual_layout.txt</strong></a> — the original column layout dump (raw text, ~5&nbsp;MB)</li>
</ul>
"""
    return page("rpglang — Documentation", body)


def render_text_file(path: Path, title: str) -> str:
    raw = html.escape(path.read_text(encoding="utf-8", errors="replace"))
    return page(title, f'<pre style="white-space:pre-wrap; font-size:.82em">{raw}</pre>')


def main() -> int:
    print(f"Building documentation site into: {OUT}")
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "man").mkdir(parents=True)
    (OUT / "tutorial").mkdir(parents=True)

    # 1. Copy the tutorial verbatim (already-authored HTML).
    tsrc = REPO / "docs" / "tutorial"
    for f in tsrc.glob("*.html"):
        shutil.copy2(f, OUT / "tutorial" / f.name)
        log(f"tutorial/{f.name}")

    # 2. Make IBM reference text browsable.
    refdir = OUT / "ref"
    refdir.mkdir(exist_ok=True)
    mtext = REPO / "docs" / "ref" / "manual_text.txt"
    if mtext.exists():
        (refdir / "manual_text.html").write_text(render_text_file(mtext, "IBM RPG II Reference (text)"), encoding="utf-8")
        log("ref/manual_text.html")
    mlayout = REPO / "docs" / "ref" / "manual_layout.txt"
    if mlayout.exists():
        shutil.copy2(mlayout, refdir / "manual_layout.txt")
        log("ref/manual_layout.txt (raw)")

    # 3. Convert each doc source into a themed, navigable page.
    for d in DOCS:
        src = REPO / d["src"]
        if d.get("raw"):
            continue  # tutorial already copied
        if not src.exists():
            log(f"WARNING: missing {src}, skipping")
            continue
        if d.get("man"):
            body = convert_man(src)
            full = page(d["title"], f'<h1>{html.escape(d["title"])}</h1>\n<div class="docbody">{body}</div>', d["slug"])
        else:
            body = convert_markdown(src, d["title"])
            full = page(d["title"], f'<h1>{html.escape(d["title"])}</h1>\n<div class="docbody">{body}</div>', d["slug"])
        out = OUT / (d["slug"] + ".html")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(full, encoding="utf-8")
        log(f"{out.relative_to(OUT)}")

    # 4. Landing page last so it wins the index.html slot.
    (OUT / "index.html").write_text(build_landing(), encoding="utf-8")
    log("index.html (landing page)")

    n = sum(1 for _ in OUT.rglob("*") if _.is_file())
    print(f"Done: {n} files written to {OUT}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
