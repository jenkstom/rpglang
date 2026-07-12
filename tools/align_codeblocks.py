#!/usr/bin/env python3
"""
Convert RPG II code blocks in tutorial HTML from <pre><code> to per-character SVG.

Each RPG code block — detected by a first line starting with ``....+`` — is
replaced with an inline SVG in which every character is pinned to an exact
pixel coordinate.  This guarantees column alignment regardless of browser,
font, or zoom level, matching the technique proven in the appendix cheat sheet.

Non-RPG blocks (Python analogies, sample data, column diagrams) are left as
plain ``<pre><code>``.

The original source text is preserved in an HTML comment immediately before
each SVG, making the transform reversible and idempotent.

Usage::

    python3 tools/align_codeblocks.py docs/tutorial/09-calc-specs.html   # print to stdout
    python3 tools/align_codeblocks.py docs/tutorial/09-calc-specs.html --in-place
    python3 tools/align_codeblocks.py docs/tutorial/*.html --in-place
    python3 tools/align_codeblocks.py docs/tutorial/*.html --in-place --revert
"""

import argparse
import html as html_module
import re
import sys
from pathlib import Path

# ── Constants (match the appendix cheat sheet) ────────────────────────────
CW = 10            # px per column
LM = 30            # left margin (cols 1–5 are sequence, de-emphasised)
ROW_H = 18         # px per code line
RULER_Y = 16       # ruler text baseline y
TICK_Y = 2         # tick mark top y
CODE_Y0 = 34       # first code line baseline y
FONT_SIZE = 12
NUM_COLS = 80
RIGHT_PAD = 10

# ── Colours ───────────────────────────────────────────────────────────────
BG = "#13131f"
TEXT_CLR = "#c0caf5"
RULER_CLR = "#565f89"
TICK_CLR = "#333653"
TICK_CLR_5 = "#565f89"
COMMENT_CLR = "#565f89"
ANNOT_CLR = "#565f89"

# Pre-computed x positions for 80 characters (centred in each column)
X_POSITIONS = " ".join(str(LM + i * CW + CW // 2) for i in range(NUM_COLS))


# ── Helpers ───────────────────────────────────────────────────────────────

def decode_entities(text: str) -> str:
    """Decode HTML entities (``&larr;`` → ``←`` etc.)."""
    return html_module.unescape(text)


def escape_svg(text: str) -> str:
    """Escape characters that are special in XML/SVG text content."""
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def is_rpg_block(body: str) -> bool:
    """True when the first line of a ``<pre><code>`` body starts with ``....+``."""
    return body.lstrip().split("\n")[0].startswith("....+")


def split_annotation(line: str) -> tuple[str, str]:
    """Split a *decoded* line into ``(code, annotation)``.

    The annotation is everything from the first ``←`` arrow onward, or — when
    no arrow is present — any text past column 80.
    """
    pos = line.find("\u2190")  # ←
    if pos >= 0:
        return line[:pos], line[pos:]
    if len(line) > NUM_COLS:
        return line[:NUM_COLS], line[NUM_COLS:]
    return line, ""


# ── SVG element generators ────────────────────────────────────────────────

def generate_ticks() -> str:
    """Tick marks: short line per column, longer at every 5th."""
    parts = []
    for col in range(1, NUM_COLS + 1):
        x = LM + (col - 1) * CW + CW // 2
        if col % 5 == 0:
            parts.append(
                f'<line x1="{x}" y1="{TICK_Y}" x2="{x}" y2="{TICK_Y + 5}" '
                f'stroke="{TICK_CLR_5}" stroke-width="0.5"/>'
            )
        else:
            parts.append(
                f'<line x1="{x}" y1="{TICK_Y}" x2="{x}" y2="{TICK_Y + 2}" '
                f'stroke="{TICK_CLR}" stroke-width="0.5"/>'
            )
    return "<g>" + "".join(parts) + "</g>"


def generate_text_line(decoded: str, y: int) -> str:
    """SVG element(s) for a single code line at baseline *y*."""
    code, ann = split_annotation(decoded)
    padded = code.ljust(NUM_COLS)[:NUM_COLS]

    is_comment = len(decoded) > 6 and decoded[6] == "*"
    colour = COMMENT_CLR if is_comment else TEXT_CLR
    style = ' font-style="italic"' if is_comment else ""

    parts = [
        f'<text x="{X_POSITIONS}" y="{y}" font-family="monospace" '
        f'font-size="{FONT_SIZE}" text-anchor="middle" fill="{colour}"{style} '
        f'xml:space="preserve">{escape_svg(padded)}</text>'
    ]

    if ann:
        ann_x = LM + len(code) * CW + CW // 2
        parts.append(
            f'<text x="{ann_x}" y="{y}" font-family="monospace" '
            f'font-size="{FONT_SIZE}" text-anchor="start" fill="{ANNOT_CLR}" '
            f'font-style="italic" xml:space="preserve">{escape_svg(ann)}</text>'
        )

    return "".join(parts)


def compute_width(code_lines: list[str]) -> int:
    """SVG pixel width — wide enough for the 80-column grid plus any annotations."""
    w = LM + NUM_COLS * CW + RIGHT_PAD  # 840
    for line in code_lines:
        code, ann = split_annotation(line)
        if ann:
            ann_x = LM + len(code) * CW + CW // 2
            ann_end = ann_x + len(ann) * FONT_SIZE * 0.65
            w = max(w, int(ann_end + RIGHT_PAD))
    return w


def block_to_svg(body: str) -> str:
    """Convert a ``<pre><code>`` body string to a self-contained SVG string."""
    raw_lines = body.split("\n")
    ruler = decode_entities(raw_lines[0])
    code_lines = [decode_entities(l) for l in raw_lines[1:]]

    width = compute_width(code_lines)
    height = CODE_Y0 + len(code_lines) * ROW_H + 6

    elements = [
        f'<div class="svg-container" style="overflow-x:auto">',
        f'<svg viewBox="0 0 {width} {height}" width="100%" '
        f'style="max-width:{width}px" xmlns="http://www.w3.org/2000/svg">',
        f'<rect x="0" y="0" width="{width}" height="{height}" fill="{BG}"/>',
        generate_ticks(),
        f'<text x="{X_POSITIONS}" y="{RULER_Y}" font-family="monospace" '
        f'font-size="{FONT_SIZE}" text-anchor="middle" fill="{RULER_CLR}" '
        f'xml:space="preserve">'
        f"{escape_svg(ruler.ljust(NUM_COLS)[:NUM_COLS])}</text>",
    ]

    for i, line in enumerate(code_lines):
        elements.append(generate_text_line(line, CODE_Y0 + i * ROW_H))

    elements.append("</svg>")
    elements.append("</div>")

    return "\n".join(elements)


# ── HTML processing ───────────────────────────────────────────────────────

# Matches a source-comment + SVG produced by this script (for regeneration).
_SVG_BLOCK = re.compile(
    r"( *)<!-- codeblock-source:\n(.*?)\n *-->"
    r"\s*\n *<div class=\"svg-container\"[^>]*>.*?</svg>\s*</div>",
    re.DOTALL,
)

# Matches a raw <pre><code>…</code></pre> block.
_PRE_BLOCK = re.compile(r"( *)<pre><code>(.*?)</code></pre>", re.DOTALL)


def _restore_svg_blocks(text: str) -> str:
    """Replace every source-comment + SVG pair back to ``<pre><code>``."""

    def _restore(m: re.Match) -> str:
        indent = m.group(1)
        body = m.group(2)
        return f"{indent}<pre><code>{body}</code></pre>"

    return _SVG_BLOCK.sub(_restore, text)


def _convert_pre_blocks(text: str) -> str:
    """Convert every RPG ``<pre><code>`` block to a source-comment + SVG."""

    def _convert(m: re.Match) -> str:
        indent = m.group(1)
        body = m.group(2)

        if not is_rpg_block(body):
            return m.group(0)          # leave non-RPG blocks untouched
        if len(body.split("\n")) < 2:
            return m.group(0)          # skip ruler-only definition blocks

        svg = block_to_svg(body)
        svg_indented = "\n".join(indent + line for line in svg.split("\n"))
        return f"{indent}<!-- codeblock-source:\n{body}\n{indent}-->\n{svg_indented}"

    return _PRE_BLOCK.sub(_convert, text)


def process_html(text: str) -> str:
    """Restore existing SVGs then re-convert (idempotent)."""
    text = _restore_svg_blocks(text)
    text = _convert_pre_blocks(text)
    return text


def revert_html(text: str) -> str:
    """Restore all SVG blocks back to ``<pre><code>``."""
    return _restore_svg_blocks(text)


# ── CLI ───────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Convert RPG II code blocks to per-character SVG."
    )
    ap.add_argument("files", nargs="+", help="HTML file(s) to process")
    ap.add_argument("--in-place", action="store_true", help="write changes back to file(s)")
    ap.add_argument("--revert", action="store_true", help="restore <pre><code> from SVGs")
    args = ap.parse_args()

    rpg_count = 0
    non_rpg_count = 0

    for fpath in args.files:
        path = Path(fpath)
        original = path.read_text(encoding="utf-8")

        if args.revert:
            result = revert_html(original)
        else:
            # Count before/after for reporting
            result = process_html(original)

        if args.in_place and result != original:
            path.write_text(result, encoding="utf-8")

        # Quick stats
        for m in _PRE_BLOCK.finditer(result):
            if is_rpg_block(m.group(2)):
                rpg_count += 1
            else:
                non_rpg_count += 1

    action = "reverted" if args.revert else "processed"
    dest = "in-place" if args.in_place else "to stdout"
    print(f"{action} {len(args.files)} file(s) ({dest})", file=sys.stderr)

    if not args.revert:
        svg_count = len(re.findall(r"class=.svg-container", result if not args.in_place else ""))
        print(f"  remaining <pre><code>: {rpg_count} RPG, {non_rpg_count} non-RPG", file=sys.stderr)


if __name__ == "__main__":
    main()
