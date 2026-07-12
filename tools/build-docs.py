#!/usr/bin/env python3
"""Assemble the rpglang documentation site.

Builds a self-contained static site into BUILD_DIR (default: public) that
links together every piece of project documentation:

  * The interactive HTML tutorial (docs/tutorial/)
  * Man pages rendered to HTML (docs/man/*.1)
  * Markdown reference and design docs, converted to themed HTML
  * A landing index.html that is the single entry point to all of the above

The generated pages share a GitHub-dark theme (matching github.com's dark
mode) with a sticky global header that links back to the repository, a repo
tab bar, and a landing page laid out like a GitHub repo overview. Every
page carries navigation back to the landing page and its sibling documents.

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

REPO_URL = "https://github.com/jenkstom/rpglang"
REPO_OWNER = "jenkstom"
REPO_NAME = "rpglang"


def fetch_repo_meta() -> dict:
    """Fetch public repo metadata (stars, forks, description) at build time.

    Falls back gracefully to sensible defaults when the network or API is
    unavailable so the build never fails for a missing star count.
    """
    import json as _json
    import urllib.error
    import urllib.request
    defaults = {"stargazers_count": None, "forks_count": None,
                "description": "RPG II compiler, runtime, and static-analysis toolkit",
                "language": "C++", "topics": []}
    try:
        req = urllib.request.Request(
            f"https://api.github.com/repos/{REPO_OWNER}/{REPO_NAME}",
            headers={"Accept": "application/vnd.github+json", "User-Agent": "rpglang-docs-build"})
        with urllib.request.urlopen(req, timeout=8) as resp:
            data = _json.loads(resp.read().decode("utf-8"))
        defaults.update({k: data.get(k) for k in
                         ("stargazers_count", "forks_count", "description", "language", "topics")})
    except (urllib.error.URLError, OSError, ValueError):
        pass
    return defaults


REPO_META = fetch_repo_meta()

# Official GitHub mark (octocat silhouette).
GITHUB_MARK_SVG = (
    '<svg height="20" viewBox="0 0 16 16" width="20" fill="currentColor" '
    'aria-hidden="true"><path d="M8 0c4.42 0 8 3.58 8 8a8.013 8.013 0 0 1-5.45 7.59c-.4.08-.55-.17-.55-.38 '
    '0-.27.01-1.13.01-2.2 0-.75-.25-1.23-.54-1.48 1.78-.2 3.65-.88 3.65-3.95 0-.88-.31-1.59-.82-2.15.08-.2.36-1.02-.08-2.12 '
    '0 0-.67-.22-2.2.82-.64-.18-1.32-.27-2-.27-.68 0-1.36.09-2 .27-1.53-1.03-2.2-.82-2.2-.82-.44 1.1-.16 1.92-.08 2.12-.51.56-.82 '
    '1.28-.82 2.15 0 3.06 1.86 3.75 3.64 3.95-.23.2-.44.55-.51 1.07-.46.21-1.61.55-2.33-.66-.15-.24-.6-.83-1.23-.82-.67.01-.27.38.01.53.34.19.73.9.82 '
    '1.13.16.45.68 1.31 2.69.94 0 .67.01 1.3.01 1.49 0 .21-.15.45-.55.38A7.995 7.995 0 0 1 0 8c0-4.42 3.58-8 8-8Z"></path></svg>'
)
STAR_SVG = (
    '<svg height="14" viewBox="0 0 16 16" width="14" fill="currentColor" aria-hidden="true">'
    '<path d="M8 .25a.75.75 0 0 1 .673.418l1.882 3.815 4.21.612a.75.75 0 0 1 .416 1.279l-3.046 2.97.719 4.192a.751.751 '
    '0 0 1-1.088.791L8 12.347l-3.766 1.98a.75.75 0 0 1-1.088-.79l.72-4.194L.818 6.374a.75.75 0 0 1 .416-1.28l4.21-.611L7.327.668A.75.75 '
    '0 0 1 8 .25Z"></path></svg>'
)
FORK_SVG = (
    '<svg height="14" viewBox="0 0 16 16" width="14" fill="currentColor" aria-hidden="true">'
    '<path d="M5 5.372v.878c0 .414.336.75.75.75h4.5a.75.75 0 0 0 .75-.75v-.878a2.25 2.25 0 1 1 1.5 0v.878a2.25 2.25 0 0 '
    '1-2.25 2.25h-1.5v2.128a2.251 2.251 0 1 1-1.5 0V8.5h-1.5A2.25 2.25 0 0 1 3.5 6.25v-.878a2.25 2.25 0 1 1 1.5 0ZM5 '
    '3.25a.75.75 0 1 0-1.5 0 .75.75 0 0 0 1.5 0Zm6.75.75a.75.75 0 1 0 0-1.5.75.75 0 0 0 0 1.5Zm-3 8.75a.75.75 0 1 0-1.5 0 .75.75 0 0 0 1.5 0Z"></path></svg>'
)
BOOK_SVG = (
    '<svg height="14" viewBox="0 0 16 16" width="14" fill="currentColor" aria-hidden="true">'
    '<path d="M0 1.75A.75.75 0 0 1 .75 1h4.253c1.227 0 2.317.59 3 1.501A3.743 3.743 0 0 1 11.006 1h4.245a.75.75 0 0 1 .75.75v10.5a.75.75 '
    '0 0 1-.75.75h-4.507a2.25 2.25 0 0 0-1.591.659l-.622.621a.75.75 0 0 1-1.06 0l-.622-.621A2.25 2.25 0 0 0 5.258 13H.75a.75.75 '
    '0 0 1-.75-.75Z"></path></svg>'
)
TAG_SVG = (
    '<svg height="14" viewBox="0 0 16 16" width="14" fill="currentColor" aria-hidden="true">'
    '<path d="M1 7.775V2.75C1 1.784 1.784 1 2.75 1h5.025c.464 0 .91.184 1.238.513l6.25 6.25a1.75 1.75 0 0 1 0 2.474l-5.026 5.026a1.75 '
    '1.75 0 0 1-2.474 0l-6.25-6.25A1.752 1.752 0 0 1 1 7.775Z"></path></svg>'
)

# Shared stylesheet, matching the tutorial's Tokyo Night palette so the entire
# site is visually unified.
CSS = r"""
:root {
  --gh-canvas:      #0d1117;
  --gh-inset:       #010409;
  --gh-subtle:      #161b22;
  --gh-overlay:     #1c2128;
  --gh-border:      #30363d;
  --gh-border-muted:#21262d;
  --gh-fg:          #e6edf3;
  --gh-fg-muted:    #8b949e;
  --gh-fg-subtle:   #6e7681;
  --gh-accent:      #58a6ff;
  --gh-accent-emph: #1f6feb;
  --gh-success:     #3fb950;
  --gh-success-emph:#238636;
  --gh-attention:   #d29922;
  --gh-danger:      #f85149;
  --gh-done:        #a371f7;
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Noto Sans", Helvetica, Arial, sans-serif;
  margin: 0; line-height: 1.5; color: var(--gh-fg); background: var(--gh-canvas);
  -webkit-font-smoothing: antialiased;
}

/* ── GitHub-style global header ───────────────────────────── */
.gh-header {
  position: sticky; top: 0; z-index: 50;
  background: var(--gh-inset);
  border-bottom: 1px solid var(--gh-border-muted);
}
.gh-header-inner {
  display: flex; align-items: center; gap: .85rem;
  max-width: 1180px; margin: 0 auto; padding: .7rem 1.5rem;
}
.gh-mark-link { display: inline-flex; color: #f0f6fc; flex-shrink: 0; }
.gh-mark-link:hover { color: var(--gh-fg-muted); }
.gh-breadcrumb { display: flex; align-items: center; gap: .25rem; font-size: 1.1rem; min-width: 0; }
.gh-breadcrumb a { color: var(--gh-accent); text-decoration: none; font-weight: 500; }
.gh-breadcrumb a:hover { text-decoration: underline; }
.gh-breadcrumb .gh-owner { color: var(--gh-fg-muted); font-weight: 400; }
.gh-breadcrumb .gh-sep { color: var(--gh-fg-subtle); padding: 0 .1rem; }
.gh-pub-badge {
  font-size: .75rem; font-weight: 500; color: var(--gh-fg-muted);
  border: 1px solid var(--gh-border); border-radius: 999px;
  padding: .05rem .55rem; background: transparent;
}
.gh-spacer { flex: 1; }
.gh-btn {
  display: inline-flex; align-items: center; gap: .4rem;
  font-size: .85rem; font-weight: 500; color: var(--gh-fg);
  background: var(--gh-subtle); border: 1px solid var(--gh-border);
  border-radius: 6px; padding: .35rem .8rem; text-decoration: none; cursor: pointer;
  transition: background .12s, border-color .12s; line-height: 1.2;
}
.gh-btn:hover { background: var(--gh-border-muted); border-color: var(--gh-fg-subtle); text-decoration: none; }
.gh-btn .gh-count {
  display: inline-flex; align-items: center; gap: .25rem;
  padding-left: .6rem; margin-left: .15rem;
  border-left: 1px solid var(--gh-border); color: var(--gh-fg-muted);
}
.gh-btn .gh-count b { color: var(--gh-fg); font-weight: 600; }

/* ── Repo tab nav ─────────────────────────────────────────── */
.gh-tabs {
  position: sticky; top: 48px; z-index: 40;
  background: var(--gh-canvas);
  border-bottom: 1px solid var(--gh-border-muted);
  overflow-x: auto;
}
.gh-tabs-inner {
  display: flex; gap: .5rem; max-width: 1180px; margin: 0 auto;
  padding: 0 1.5rem;
}
.gh-tab {
  display: inline-flex; align-items: center; gap: .4rem;
  padding: .75rem .85rem; font-size: .9rem; color: var(--gh-fg-muted);
  text-decoration: none; white-space: nowrap; border-bottom: 2px solid transparent;
  border-radius: 0; margin-bottom: -1px;
}
.gh-tab:hover { color: var(--gh-fg); text-decoration: none; border-bottom-color: var(--gh-border-muted); }
.gh-tab.active { color: var(--gh-fg); font-weight: 600; border-bottom-color: #fd8c73; }

/* ── Page container ───────────────────────────────────────── */
.gh-container {
  max-width: 1180px; margin: 0 auto; padding: 1.8rem 1.5rem 4rem;
}

/* ── Content typography (GitHub markdown style) ───────────── */
.docbody, .gh-main { font-size: 1rem; }
.gh-main { min-width: 0; }
h1 { font-size: 2rem; font-weight: 600; color: var(--gh-fg); padding-bottom: .4rem; margin: 0 0 1rem; border-bottom: 1px solid var(--gh-border-muted); }
h2 { font-size: 1.4rem; font-weight: 600; color: var(--gh-fg); margin: 2.4rem 0 1rem; padding-bottom: .35rem; border-bottom: 1px solid var(--gh-border-muted); }
h3 { font-size: 1.2rem; font-weight: 600; color: var(--gh-fg); margin: 1.8rem 0 .8rem; }
h4 { font-size: 1.05rem; font-weight: 600; color: var(--gh-fg); margin: 1.4rem 0 .6rem; }
h5, h6 { color: var(--gh-fg-muted); margin: 1.2rem 0 .5rem; }
p { margin: 0 0 1rem; }
a { color: var(--gh-accent); text-decoration: none; }
a:hover { text-decoration: underline; }
strong { color: var(--gh-fg); font-weight: 600; }
code { font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, "Liberation Mono", monospace; background: var(--gh-subtle); color: #e6edf3; padding: .15em .4em; border-radius: 6px; font-size: .85em; }
pre { background: var(--gh-canvas); color: #e6edf3; padding: 1rem; border-radius: 8px; overflow-x: auto; border: 1px solid var(--gh-border-muted); margin: 0 0 1rem; }
pre code { background: none; color: inherit; padding: 0; font-size: .85em; }
blockquote { border-left: 3px solid var(--gh-border); color: var(--gh-fg-muted); margin: 0 0 1rem; padding: 0 1rem; }
table { border-collapse: collapse; width: 100%; margin: 0 0 1rem; display: block; overflow-x: auto; }
th, td { border: 1px solid var(--gh-border-muted); padding: .45rem .7rem; text-align: left; vertical-align: top; }
th { background: var(--gh-subtle); color: var(--gh-fg); font-weight: 600; }
tr:nth-child(2n) { background: rgba(110,118,129,.07); }
hr { border: none; border-top: 1px solid var(--gh-border-muted); margin: 2rem 0; }
ul, ol { padding-left: 1.6rem; margin: 0 0 1rem; }
li { margin-bottom: .3rem; }
img { max-width: 100%; }
.lead { font-size: 1.12rem; color: var(--gh-fg-muted); margin-bottom: 1.4rem; }

/* ── Landing: hero ────────────────────────────────────────── */
.gh-hero {
  background: linear-gradient(180deg, var(--gh-subtle) 0%, var(--gh-canvas) 100%);
  border: 1px solid var(--gh-border-muted); border-radius: 10px;
  padding: 1.8rem 2rem; margin-bottom: 1.8rem;
}
.gh-hero h1 { border: none; margin-top: 0; }
.gh-hero .lead { margin-bottom: 0; }
.gh-tags { display: flex; gap: .5rem; flex-wrap: wrap; margin-top: 1.1rem; }
.gh-tag {
  display: inline-flex; align-items: center; gap: .35rem;
  font-size: .8rem; color: var(--gh-accent);
  background: rgba(56,139,253,.12); border: 1px solid rgba(56,139,253,.4);
  border-radius: 999px; padding: .2rem .75rem; text-decoration: none;
}
.gh-tag:hover { background: rgba(56,139,253,.2); text-decoration: none; }

/* ── Landing: two-column layout ───────────────────────────── */
.gh-layout { display: grid; grid-template-columns: minmax(0, 1fr) 296px; gap: 1.8rem; align-items: start; }
@media (max-width: 900px) { .gh-layout { grid-template-columns: 1fr; } }

.gh-sidebar { position: sticky; top: 108px; }
.gh-about {
  background: var(--gh-canvas); border: 1px solid var(--gh-border-muted);
  border-radius: 10px; padding: 1rem 1.1rem;
}
.gh-about h2 { font-size: 1rem; margin: 0 0 .6rem; border: none; padding: 0; color: var(--gh-fg); }
.gh-about p { font-size: .92rem; color: var(--gh-fg-muted); margin: 0 0 1rem; }
.gh-about-meta { list-style: none; padding: 0; margin: 0 0 1rem; border-top: 1px solid var(--gh-border-muted); padding-top: .8rem; }
.gh-about-meta li { display: flex; align-items: center; gap: .5rem; font-size: .88rem; color: var(--gh-fg-muted); margin-bottom: .5rem; }
.gh-about-meta li svg { color: var(--gh-fg-muted); flex-shrink: 0; }
.gh-about-meta b { color: var(--gh-fg); font-weight: 600; }
.gh-about-actions { display: flex; gap: .5rem; flex-wrap: wrap; }

/* ── Landing: doc cards ───────────────────────────────────── */
.section-title {
  display: flex; align-items: center; gap: .5rem;
  font-size: 1.15rem; font-weight: 600; color: var(--gh-fg);
  margin: 2rem 0 .8rem; padding-bottom: .5rem; border-bottom: 1px solid var(--gh-border-muted);
}
.section-title .section-count {
  font-size: .8rem; font-weight: 500; color: var(--gh-fg-muted);
  background: var(--gh-subtle); border: 1px solid var(--gh-border-muted);
  border-radius: 999px; padding: .05rem .55rem;
}
.cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 1rem; margin-bottom: 1.2rem; }
.card {
  background: var(--gh-subtle); border: 1px solid var(--gh-border-muted); border-radius: 10px;
  padding: 1rem 1.2rem; transition: border-color .14s, transform .14s, box-shadow .14s;
  display: flex; flex-direction: column;
}
.card:hover { border-color: var(--gh-accent); transform: translateY(-2px); box-shadow: 0 6px 18px rgba(1,4,9,.4); }
.card h3 { margin: 0 0 .4rem; font-size: 1.02rem; font-weight: 600; }
.card h3 a { color: var(--gh-accent); text-decoration: none; }
.card h3 a:hover { text-decoration: underline; }
.card p { margin: 0; color: var(--gh-fg-muted); font-size: .88rem; }
.card .card-icon { font-size: 1.1rem; line-height: 1; margin-right: .3rem; }

.gh-manuals { list-style: none; padding: 0; margin: 1rem 0 0; display: grid; gap: .6rem; }
.gh-manuals li { margin: 0; }
.gh-manuals a {
  display: flex; align-items: center; gap: .5rem; font-size: .9rem;
  padding: .6rem .9rem; background: var(--gh-subtle); border: 1px solid var(--gh-border-muted);
  border-radius: 8px;
}
.gh-manuals a:hover { border-color: var(--gh-accent); }
.gh-manuals .gh-manual-size { margin-left: auto; color: var(--gh-fg-subtle); font-size: .8rem; }

footer.gh-footer {
  border-top: 1px solid var(--gh-border-muted); color: var(--gh-fg-muted); font-size: .82rem;
  padding: 1.2rem 1.5rem; max-width: 1180px; margin: 0 auto;
}
footer.gh-footer a { color: var(--gh-accent); }
footer.gh-footer .gh-footer-links { display: flex; flex-wrap: wrap; gap: .4rem 1rem; align-items: center; }
footer.gh-footer .gh-footer-copy { margin-top: .5rem; color: var(--gh-fg-subtle); }
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
    """GitHub-style header: black global bar + repo tab nav."""
    star = REPO_META.get("stargazers_count")
    fork = REPO_META.get("forks_count")
    star_count = f"<b>{star}</b>" if star is not None else ""
    fork_count = f"<b>{fork}</b>" if fork is not None else ""

    header = f"""<div class="gh-header">
  <div class="gh-header-inner">
    <a class="gh-mark-link" href="{REPO_URL}" title="View on GitHub" target="_blank" rel="noopener">{GITHUB_MARK_SVG}</a>
    <span class="gh-breadcrumb">
      <a class="gh-owner" href="https://github.com/{REPO_OWNER}" target="_blank" rel="noopener">{REPO_OWNER}</a>
      <span class="gh-sep">/</span>
      <a href="{REPO_URL}" target="_blank" rel="noopener">{REPO_NAME}</a>
    </span>
    <span class="gh-pub-badge">Public</span>
    <span class="gh-spacer"></span>
    <a class="gh-btn" href="{REPO_URL}/fork" target="_blank" rel="noopener">{FORK_SVG} Fork{(' <span class="gh-count">' + fork_count + '</span>') if fork is not None else ''}</a>
    <a class="gh-btn" href="{REPO_URL}/stargazers" target="_blank" rel="noopener">{STAR_SVG} Star{(' <span class="gh-count">' + star_count + '</span>') if star is not None else ''}</a>
  </div>
</div>"""

    tabs = [
        ("index.html", "Overview", BOOK_SVG),
        ("tutorial/index.html", "Tutorial", BOOK_SVG),
        ("man/rpgc.html", "rpgc(1)", None),
        ("man/rpg-analyze.html", "rpg-analyze(1)", None),
        ("spec-map.html", "Reference", None),
        ("architecture.html", "Internals", None),
    ]
    tab_items = []
    for href, label, icon in tabs:
        active = " active" if href.rstrip("/") == (current_slug or "") else ""
        icon_html = f"{icon} " if icon else ""
        tab_items.append(f'<a class="gh-tab{active}" href="{href}">{icon_html}{label}</a>')

    tabbar = f"""<nav class="gh-tabs">
  <div class="gh-tabs-inner">
{'    ' + chr(10).join(tab_items)}
  </div>
</nav>"""
    return header + "\n" + tabbar


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
<div class="gh-container">
{body}
</div>
<footer class="gh-footer">
  <div class="gh-footer-links">
    {GITHUB_MARK_SVG}
    <a href="{REPO_URL}" target="_blank" rel="noopener">View repository on GitHub</a>
    <span class="gh-sep">·</span>
    <a href="index.html">Docs home</a>
    <span class="gh-sep">·</span>
    <a href="tutorial/index.html">Tutorial</a>
    <span class="gh-sep">·</span>
    <a href="man/rpgc.html">rpgc(1)</a>
    <span class="gh-sep">·</span>
    <a href="man/rpg-analyze.html">rpg-analyze(1)</a>
  </div>
  <div class="gh-footer-copy">Generated from the <code>{REPO_NAME}</code> repository · © {REPO_OWNER}</div>
</footer>
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
                f'<article class="card"><h3><a href="{href}"><span class="card-icon">{icon}</span>{html.escape(d["title"].split(" — ")[0])}</a></h3>'
                f'<p>{html.escape(d["blurb"])}</p></article>'
            )
        sections.append(
            f'<div class="section-title">{html.escape(cat)} <span class="section-count">{len(items)}</span></div>\n'
            f'<div class="cards">\n' + "\n".join(cards) + "\n</div>"
        )

    # About sidebar — mirrors GitHub's repo "About" box.
    stars = REPO_META.get("stargazers_count")
    forks = REPO_META.get("forks_count")
    lang = REPO_META.get("language") or "C++"
    desc = REPO_META.get("description") or "RPG II compiler, runtime, and static-analysis toolkit"
    topics = REPO_META.get("topics") or []
    stars_str = str(stars) if stars is not None else "—"
    forks_str = str(forks) if forks is not None else "—"
    topics_html = "".join(
        f'<a class="gh-tag" href="{REPO_URL}/topics/{t}" target="_blank" rel="noopener">{html.escape(t)}</a>'
        for t in topics
    )
    topic_section = f'<div class="gh-tags">{topics_html}</div>' if topics_html else ""

    sidebar = f"""<aside class="gh-sidebar">
  <div class="gh-about">
    <h2>About</h2>
    <p>{html.escape(desc)}</p>
    {topic_section}
    <ul class="gh-about-meta">
      <li>{BOOK_SVG} <span>Docs site for <b>{REPO_OWNER}/{REPO_NAME}</b></span></li>
      <li>{STAR_SVG} <b>{stars_str}</b> stars</li>
      <li>{FORK_SVG} <b>{forks_str}</b> forks</li>
      <li>{TAG_SVG} Language: <b>{html.escape(lang)}</b></li>
    </ul>
    <div class="gh-about-actions">
      <a class="gh-btn" href="{REPO_URL}" target="_blank" rel="noopener">{GITHUB_MARK_SVG} View on GitHub</a>
    </div>
  </div>
</aside>"""

    manuals = f"""<div class="section-title">IBM RPG II reference manuals (raw) <span class="section-count">2</span></div>
<ul class="gh-manuals">
  <li><a href="ref/manual_text.html">{BOOK_SVG} <strong>manual_text.txt</strong> — full IBM RPG II reference text <span class="gh-manual-size">~1.9&nbsp;MB</span></a></li>
  <li><a href="ref/manual_layout.txt">{BOOK_SVG} <strong>manual_layout.txt</strong> — column layout dump (raw) <span class="gh-manual-size">~5&nbsp;MB</span></a></li>
</ul>"""

    body = f"""
<div class="gh-layout">
<main class="gh-main">
  <div class="gh-hero">
    <h1>rpglang documentation</h1>
    <p class="lead">An <strong>RPG&nbsp;II</strong> compiler and static-analysis toolkit for Linux,
    compiling the column-oriented fixed-format language of IBM System/34, System/36,
    and AS/400 midrange systems into native ELF executables via <strong>LLVM&nbsp;19</strong>.</p>
    <div class="gh-tags">
      <a class="gh-tag" href="{REPO_URL}" target="_blank" rel="noopener">RPG II compiler</a>
      <a class="gh-tag" href="architecture.html">LLVM 19 codegen</a>
      <a class="gh-tag" href="analyzer-design.html">static analysis</a>
      <a class="gh-tag" href="building.html">C runtime</a>
      <a class="gh-tag" href="{REPO_URL}" target="_blank" rel="noopener">Linux native</a>
    </div>
  </div>

  <p>Everything is reachable from this page: the interactive tutorial, the
  <code>rpgc</code> and <code>rpg-analyze</code> man pages, the column-reference
  and build guides, and the internal architecture &amp; design documents.
  Pick a card below, or use the tab bar at the top of any page.</p>

{chr(10).join(sections)}

{manuals}
</main>
{sidebar}
</div>
"""
    return page("rpglang — Documentation", body, current_slug="index.html")


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
