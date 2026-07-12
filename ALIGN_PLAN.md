# Tutorial Column-Alignment Plan

## Problem

All 108 RPG II code examples across the 19 tutorial chapters are rendered as
plain `<pre><code>` text blocks. Each block begins with a ruler line
(`....+....1....+....2...`), and the code lines follow underneath. Alignment
between the ruler and the code depends entirely on the browser's monospace
font rendering — which varies across fonts, zoom levels, and rendering
engines. Characters drift progressively the further right you look.

The appendix cheat sheet (`appendix-cheat-sheet.html`) solved this by
rendering code as SVG with **per-character x positioning**: every glyph is
pinned to an exact pixel coordinate, so characters always line up with
column tick marks and colored field bands — regardless of browser, font, or
zoom.

## Goal

Bring that same per-character SVG alignment to every RPG II code block in
the tutorial, while leaving non-RPG blocks (Python analogies, sample data,
sample output) as plain `<pre><code>`.

## Scope

| Category | Count | Action |
|----------|------:|--------|
| RPG code blocks (start with ruler, contain F/I/C/O/E/\*/H lines) | 108 | Convert to SVG |
| Non-RPG blocks (Python, pseudo-code, sample I/O, column diagrams) | 15 | Leave as `<pre><code>` |
| **Total `<pre><code>` blocks** | **123** | |

All 108 RPG blocks already start with the ruler line, so detection is
reliable: a `<pre><code>` block whose first line matches
`^\.\.\.\.\+` is an RPG block.

### Files and block counts

| File | RPG blocks | Non-RPG blocks |
|------|----------:|---------------:|
| `01-introduction.html` | 0 | 0 |
| `02-source-layout.html` | 6 | 1 |
| `03-program-cycle.html` | 2 | 1 |
| `04-indicators.html` | 6 | 0 |
| `05-data-fields.html` | 6 | 0 |
| `06-arithmetic.html` | 8 | 0 |
| `07-file-specs.html` | 5 | 1 |
| `08-input-specs.html` | 5 | 1 |
| `09-calc-specs.html` | 7 | 1 |
| `10-conditional-logic.html` | 7 | 1 |
| `11-loops-branching.html` | 6 | 0 |
| `12-subroutines.html` | 5 | 1 |
| `13-field-operations.html` | 8 | 0 |
| `14-control-levels.html` | 7 | 3 |
| `15-arrays-tables.html` | 7 | 1 |
| `16-output-specs.html` | 8 | 2 |
| `17-output-formatting.html` | 3 | 2 |
| `18-file-access.html` | 7 | 0 |
| `19-multifile.html` | 5 | 0 |
| **Total** | **108** | **15** |

## Technical approach

### One shared Python script: `tools/align_codeblocks.py`

The script reads each `docs/tutorial/NN-*.html` file, finds every
`<pre><code>` block whose first line starts with `....+`, and replaces it
with an inline SVG. Non-RPG blocks are left untouched.

### SVG structure per code block

Each converted block becomes a self-contained `<svg>` wrapped in a
`.svg-container` (already styled in the chapter CSS). The SVG contains:

```
┌──────────────────────────────────────────────────────┐
│  tick marks (every column, longer at every 5th)        │  ← ruler row
│  ....+....1....+....2....+....3....+....4...           │
│     C                     ADD  AMT       TOTAL         │  ← code line 1
│     C*  comment line                                   │  ← code line 2
│                                                        │
│  (optional field bands behind specific columns)        │
└──────────────────────────────────────────────────────┘
```

#### Coordinate system (matches the appendix exactly)

| Constant | Value | Purpose |
|----------|-------|---------|
| `CW` | 10 px | width per column |
| `LM` | 30 px | left margin (cols 1–5 are sequence, visually de-emphasized) |
| `ROW_H` | 18 px | height per code line |
| `RULER_Y` | 16 px | y of ruler text baseline |
| `TICK_Y` | 2 px | y of tick marks (top edge) |
| `CODE_Y0` | 34 px | y baseline of first code line |

Total SVG width: `LM + 80 * CW = 830 px` (+ right padding).
Total SVG height: `CODE_Y0 + (num_lines * ROW_H) + 6 px`.

#### Per-character text rendering

Every character (ruler + code lines) uses SVG `<text>` with an explicit `x`
attribute listing 80 space-separated coordinates — one per character. This
is the technique proven in the appendix:

```xml
<text x="35 45 55 65 ... 825" y="34" font-family="monospace"
      font-size="12" text-anchor="middle">     C   ...</text>
```

This guarantees pixel-perfect alignment with the tick marks and any field
bands, regardless of font metrics.

#### Visual styling

| Element | Style |
|---------|-------|
| Background | `#13131f` (matches existing `pre` background) |
| Text color | `#c0caf5` (matches existing `pre code` color) |
| Ruler text | `#565f89` (dimmer — a muted blue-gray) |
| Tick marks | `#333653` (every column), `#565f89` (every 5th) |
| Comment lines (`*` in col 7) | `#565f89` (dimmed, italic) |
| Field bands (optional) | Translucent colored rects behind specific column ranges |

The SVG uses `viewBox` and `width="100%"` with `max-width:830px` so it
scales on narrow viewports. A `overflow-x: auto` wrapper handles very
narrow screens.

#### Font size

The appendix uses font-size 9px for code. The chapters' `<pre>` blocks use
`0.9em` of body text (~14.4px). The SVG code blocks should use **12px** —
large enough to read comfortably, small enough that 80 columns fit in 830px
at `CW=10`. (12px monospace glyphs are ~7.2px wide, well within the 10px
column allotment.)

### Field-band highlighting (optional, phase 2)

After the basic conversion is done, a second pass can add translucent
colored bands behind specific column ranges, reusing the same color palette
as the appendix cheat sheet. The spec type is auto-detected from column 6
of each line:

| Spec letter | Example bands |
|-------------|---------------|
| `F` | blue (filename), green (type), orange (designation), teal (rec len), purple (device) |
| `I` | blue (filename), red (from), amber (to), cyan (field name) |
| `C` | green (cond inds), orange (factor 1), purple (opcode), teal (factor 2), red (result) |
| `O` | blue (filename), green (type), teal (cond inds), red (field name), amber (edit) |

Bands are drawn as `<rect>` elements behind the text, with
`fill-opacity="0.12"` so they tint without obscuring text. A single band
spans the full height of all code lines that share the same spec type.

### What stays as `<pre><code>`

These 15 blocks are not RPG source and don't use column positions:

- Python/pseudo-code analogies (chapters 03, 07, 08, 10, 12, 14, 16)
- Sample input data lines (chapter 14: `110`, `120`, ...)
- Sample report output previews (chapters 14, 16, 17)
- Column-diagram ASCII art (chapter 09 line 160: the conditioning-indicator
  group format)
- The ruler-definition reference (chapter 02 line 91 — this one shows the
  ruler itself as a teaching artifact; converting it to SVG would be
  recursive)

## Handling special cases

### Multi-line programs (up to 19 lines)

The largest code block is the complete control-break program in chapter 14
(19 lines). At `ROW_H=18px`, that's a 376px-tall SVG — taller than the
current `<pre>` block but still reasonable on the page. No special handling
needed; the SVG just has more rows.

### Lines with HTML entities

Some code lines contain `&larr;` (←), `&rarr;` (→), `&amp;`, `&ndash;`.
The script must:
1. Decode entities in the source text before measuring character positions
2. Re-escape `<`, `>`, `&` when emitting SVG `<text>` content
3. Arrow entities (`&larr;`, `&rarr;`) render as single characters and occupy
   one column position — the script treats them as one character width

### Lines shorter than 80 characters

Most RPG source lines in the tutorial are shorter than 80 columns. The
script pads each line to 80 characters with spaces before generating x
coordinates. This ensures the tick-mark ruler above always spans the full
width.

### Lines longer than 80 characters

A few blocks contain annotation arrows extending past column 80 (e.g.,
`← SUB = SUB + AMT (group subtotal)`). The script clips or wraps these:
text beyond column 80 is emitted as a separate `<text>` element in a muted
color, positioned after column 80 without tick marks. Alternatively, these
annotation suffixes can be stripped and moved to a caption below the SVG.

### Comment lines (`*` in column 7)

Lines where column 7 is `*` (full-line comments) are rendered in a dimmed,
italic style (`#565f89`) to visually distinguish them from code, matching
how syntax highlighters treat comments.

## Migration strategy

### Phase 1: Build the tool (1 session)

1. Write `tools/align_codeblocks.py` — a standalone Python script
2. It takes an input HTML file and an output HTML file (or `--in-place`)
3. It parses `<pre><code>` blocks, detects RPG blocks by ruler prefix,
   generates SVG, and writes the result
4. Test on one chapter (`09-calc-specs.html`) and visually verify in a
   browser

### Phase 2: Convert all chapters (1 session)

1. Run the script across all 19 chapter files
2. Visually spot-check: single-line examples, multi-line programs, comment
   lines, blocks with arrow annotations
3. Verify all internal links and anchors still work (SVG replacement
   shouldn't affect surrounding HTML)
4. Commit

### Phase 3: Field-band highlighting (optional, 1 session)

1. Extend the script to auto-detect spec type per line and emit translucent
   field bands
2. Run across all chapters
3. Verify band colors are consistent with the appendix cheat sheet
4. Commit

### Phase 4: Ruler definition update (optional)

Update the "column ruler" explanation in section 2.2 of
`02-source-layout.html` to mention that all code examples now have
built-in tick marks and don't rely on counting ruler dots manually.

## Reversibility

The conversion is a one-way transform on the HTML files. To make it
reversible:

1. The original `<pre><code>` text is preserved as an HTML comment
   immediately before each SVG:
   ```html
   <!-- codeblock-source:
   ....+....1....+....2...
      C                     ADD  AMT       TOTAL
   -->
   <svg> ... </svg>
   ```
2. A `--revert` flag on the script reads these comments and restores the
   `<pre><code>` blocks

This also makes re-running the script safe: if the source comment exists,
it regenerates the SVG from the source rather than trying to parse the SVG
back to text.

## Testing checklist

After conversion, verify each of these across at least 3 browsers
(Firefox, Chrome, Safari):

- [ ] Ruler tick marks align with code characters at every 5th column
- [ ] No horizontal drift at column 80
- [ ] Multi-line blocks have consistent row spacing
- [ ] Comment lines (`C*`) are visually distinct
- [ ] SVGs scale correctly on narrow viewports (overflow-x scroll works)
- [ ] Copy/paste from SVG text works (selectable text)
- [ ] Non-RPG `<pre><code>` blocks are unchanged
- [ ] Dark-mode and light-mode rendering both legible
- [ ] No layout shift or reflow when SVGs replace `<pre>` blocks

## File inventory

```
tools/align_codeblocks.py          ← new: the conversion script
docs/tutorial/01-introduction.html ← 0 blocks (no change)
docs/tutorial/02-source-layout.html
docs/tutorial/03-program-cycle.html
docs/tutorial/04-indicators.html
docs/tutorial/05-data-fields.html
docs/tutorial/06-arithmetic.html
docs/tutorial/07-file-specs.html
docs/tutorial/08-input-specs.html
docs/tutorial/09-calc-specs.html
docs/tutorial/10-conditional-logic.html
docs/tutorial/11-loops-branching.html
docs/tutorial/12-subroutines.html
docs/tutorial/13-field-operations.html
docs/tutorial/14-control-levels.html
docs/tutorial/15-arrays-tables.html
docs/tutorial/16-output-specs.html
docs/tutorial/17-output-formatting.html
docs/tutorial/18-file-access.html
docs/tutorial/19-multifile.html
docs/tutorial/appendix-cheat-sheet.html  ← already done
docs/tutorial/index.html              ← no code blocks
```

## Estimated effort

| Phase | Effort | Blocks touched |
|-------|--------|---------------|
| 1: Build tool | ~2 hours | 7 (ch.09 only) |
| 2: Convert all | ~30 min | 108 (all chapters) |
| 3: Field bands | ~1 hour | 108 (all chapters) |
| 4: Doc update | ~15 min | 1 section |
