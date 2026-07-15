# Auto Report Preprocessor — Design

Status: **implemented** (compiler/src/autoreport.cpp, uspec.cpp). This document
is derived from the IBM *System/36-Compatible RPG II User's Guide and
Reference*, Chapter 26 ("The Auto Report Feature"), with all column numbers and
generation rules cross-checked against the manual text in
`docs/ref/manual_text.txt` (lines cited below). It was originally written as an
implementation plan; the design it describes is now realized in the compiler.

## 1. What Auto Report is, and why it's a preprocessor

Auto Report (manual Ch. 26, `manual_text.txt:89552+`) is a **source-to-source
preprocessor** that runs *before* the RPG II compiler. It takes an "Auto Report
source program" — ordinary RPG specs plus one of three terse Auto Report
constructs — and expands it into a complete, ordinary RPG program (H/F/E/L/I/C/O
specs in compiler order), which is then compiled normally.

There are **three independent sub-features**, with very different costs:

| Sub-feature | Manual ref | Status | Implementation |
|---|---|---|---|
| **Copy** (`/COPY`) — splice a library member | `89980+` | **Done** | `compiler/src/source.cpp` `expand_copy_statements` |
| **Page-heading** (`H-*AUTO`) — generate heading O-specs | `90700+` | **Done** | `compiler/src/autoreport.cpp` (Phase B) |
| **Output** (`D/T-*AUTO`) — generate C-specs + O-specs from a field list | `90907+` | **Done** | `compiler/src/autoreport.cpp` (Phase C) |

The `U` form-type line itself (the "option spec") is just the entry point and a
handful of option flags — it carries none of the expansion logic. The expansion
lives in the `*AUTO` constructs it activates.

This plan implements all three. The copy function already exists, so the work is
the heading and output sub-features plus the shared infrastructure.

## 2. Integration point (where it hooks in)

The preprocessor is a **transform on `std::vector<SourceLine>`**, run right after
`expand_copy_statements` and **in place of** `reject_uspecs`, before any
`parse_*` call. Two call sites today (both must change):

- `compiler/src/main.cpp:315` — `rpgc::reject_uspecs(src);`
- `analyze/src/ir.cpp:508`   — `reject_uspecs(ir.raw_lines);`

Both become:

```cpp
rpgc::AutoReportReport ar;
if (!rpgc::expand_autoreport(src, base_dir, ar)) {
    return 1;   // hard error already reported via diagnostics
}
// src now contains ordinary specs; parsing proceeds as today.
// `ar.notes` (what was generated) can be surfaced like CleanReport notes.
```

Rationale: the preprocessor must run before `parse_cspecs`/`parse_ospecs` because
it **rewrites the source lines themselves** (emitting real C/O-spec text that the
existing column parsers then consume). This keeps the rest of the compiler —
every `parse_*` function, codegen, the entire analyzer — unchanged. The
generated specs are plain RPG text; nothing downstream needs to know Auto Report
was involved.

Why a source rewrite (not direct emission into `Program::calcs`/`outputs`):
- The existing `CSpec`/`ORecord` parsers already validate column layout,
  indicators, field references, etc. Emitting text and re-parsing gets all that
  validation for free and guarantees the generated program is well-formed.
- The manual requires a specific column layout for generated specs (cols 1-4
  sequence, col 5 source-code letter, cols 6-80 spec — Table 19, `89858-89880`),
  which is naturally expressed as text.
- The analyzer benefits identically with no second code path.

**Cost of this choice:** the preprocessor must format text into fixed columns
correctly. We already have `col()`/`col_trim()` helpers (`source.h`) and the
compiler's own O/C spec emitters, so this is mechanical, not novel.

## 3. New files

```
compiler/src/autoreport.h      // public API: expand_autoreport + AutoReportReport
compiler/src/autoreport.cpp    // the preprocessor (all sub-features)
compiler/src/uspec.{h,cpp}     // RETAINED but downgraded: parse the U line options
                               //   into a struct; no longer hard-rejects.
tests/autoreport/              // test corpus (one .rpg per scenario + expected .out)
```

`uspec.cpp` keeps responsibility for parsing the `U` option line (it already
knows the form type), but switches from "report a hard error" to "fill an
`AutoReportOptions` struct." The hard-rejection behavior moves into the cases
that genuinely can't be handled yet (gated by phase, see §7).

## 4. Public API (`autoreport.h`)

```cpp
namespace rpgc {

// Options parsed from the U-spec line (cols 6-30). Defaults = all-blank U line.
struct AutoReportOptions {
    bool present          = false;  // a U line exists
    bool catalog_source   = false;  // col 7 == 'C'
    std::string catalog_lib;        // cols 8-15 (library, before the comma)
    std::string catalog_member;     // cols 16-24 (member, after the comma)
    bool suppress_date_page = false; // col 27 == 'N'
    bool suppress_asterisks = false; // col 28 == 'N'
    enum class ListOpt { Full, NoListing, Partial } list_opt = ListOpt::Full; // col 30
};

// What expand_autoreport found and did (mirrors CleanReport).
struct AutoReportReport {
    std::vector<std::string> notes;   // one per construct expanded
    bool changed = false;             // any specs were generated/rewritten
};

// The entry point. Rewrites `src` in place: replaces *AUTO constructs with
// ordinary specs, sorts the program into compiler order, resequences cols 1-5.
// Returns false (after reporting) on a hard Auto Report error. No-op (returns
// true, changed=false) if the program contains no U/*AUTO constructs — so it is
// safe to call unconditionally on every program.
bool expand_autoreport(std::vector<SourceLine> &src,
                       const std::string &base_dir,
                       AutoReportReport &rep);

} // namespace rpgc
```

The `base_dir` is passed only so the copy function can keep resolving `/COPY`
members the same way it does today (it already runs separately before us, but
Auto Report's own `/COPY` semantics differ slightly in sort order — see §6.4).

## 5. Phase A — Infrastructure & U-spec options (small)

**Goal:** stand up the module, parse the U line, and route both call sites
through `expand_autoreport` as a no-op for non-Auto-Report programs. After this
phase, `reject_uspecs` no longer hard-errors; programs *with* U lines still fail
(because the `*AUTO` constructs aren't expanded yet), but the plumbing is real.

### 5.1 `autoreport.cpp` skeleton

```
detect whether the program is an Auto Report program:
    any line with form_type == 'U', OR any O-spec record-description
    whose cols 32-36 == "*AUTO"
if none -> return true, changed=false (fast path; today's behavior)
if a U line is present but is not the first spec -> hard error (manual 90211)
parse the U line into AutoReportOptions (delegate to uspec.cpp)
```

### 5.2 `uspec.cpp` rewrite

Replace the reject-everything loop with a parser filling
`AutoReportOptions`. Column map (manual `90221-90363`; verified):

| Cols | Field | Values |
|---|---|---|
| 6 | form type | `U` |
| 7 | source | blank = don't catalog; `C` = catalog created source |
| 8-24 | source member ref | `library,member` (only when col 7 = `C`); lib defaults to `#LIBRARY` if `F1`/blank |
| 25-26 | — | unused |
| 27 | date suppress | blank = print date+page; `N` = suppress |
| 28 | `*` suppress | blank = print asterisks on totals; `N` = suppress |
| 29 | — | unused |
| 30 | list options | blank = full listing; `B` = no listing; `P` = partial |
| 31-74 | — | unused |

Validation: if col 7 = `C`, require a well-formed `lib,member` (both names start
alphabetic, ≤8 chars); else error. This is the only parsing in this phase.

### 5.3 Tests (Phase A)

- `tests/autoreport/no_u_spec.rpg` — ordinary program, must pass through
  unchanged (`changed == false`).
- `tests/autoreport/u_spec_options.rpg` — a U line with each option set;
  assert the parsed `AutoReportOptions` (via a `--dump-autoreport` debug flag or
  a unit test linking `rpgc_parse`).
- `tests/autoreport/u_spec_not_first.rpg` — U line after an H spec → hard error.
- Update `tests/neg_uspec.rpg` — it currently asserts rejection; repurpose it to
  assert the *option parsing* succeeds (the construct expansion failure comes
  later, in §7).

**Exit criteria:** `rpgc` and `rpg-analyze` accept a program with a bare U line
(no `*AUTO` constructs) and compile/analyze it identically to the same program
without the U line. No regressions in `tests/run_tests.sh`.

## 6. Phase B — Page-heading sub-feature (`H-*AUTO`) (medium)

**Goal:** a program whose only Auto Report construct is `H-*AUTO` page headings
compiles and produces the correct heading lines.

### 6.1 The construct

An `H-*AUTO` block is: one **record-description** O-spec line (form type `O`,
filename in cols 7-14, `H` in col 15, `*AUTO` in cols 32-36) followed by zero or
more **field-description** lines naming fields/constants for that heading line.
Up to **five** `H-*AUTO` records per file (manual `90761`).

Record-description columns (manual `90750-90803`, verified):

| Cols | Field |
|---|---|
| 7-14 | filename (PRINTER file) |
| 15 | `H` |
| 16 | unused |
| 17-22 | spacing/skipping (blank ⇒ skip-to-06 before first, space-2 after last, space-1 after others) |
| 23-31 | output indicators (blank ⇒ `1P OR <overflow>` auto-generated) |
| 32-36 | `*AUTO` |
| 37-70 | unused |

Field-description columns (manual `90805-90898`, verified):

| Cols | Field |
|---|---|
| 7-31 | blank (indicators not allowed on heading fields) |
| 32-37 | field/table/array name, **or blank** (then cols 45-70 must hold a constant) |
| 38 | edit code (numeric fields only) |
| 39 | `B` blank-after |
| 40-44 | unused (no end positions — auto report computes & centers) |
| 45-70 | constant (title) **or** edit word |

### 6.2 Generation rules (manual `90709-90722`, `92117-92125`)

For the **first** `H-*AUTO` line, auto report generates, left to right:
1. **Date** in cols 1-8, format `mm/dd/yy`, source field `UDATE` — *unless* the
   U-spec col 27 = `N`.
2. **Title** — the constant(s)/field(s) from the field lines, centered on the
   report width (one blank before/after fields; none between constants).
3. **Page number** — the literal `PAGE` followed by a 4-digit zero-suppressed
   page field (one of the reserved `PAGE`/`PAGE1`-`PAGE7`; if all used, no page
   number), right-justified to the report's max end position — *unless* col 27 = `N`.

For **subsequent** `H-*AUTO` lines (2-5): the field/constants only, centered on
the first heading line; no date/page.

Conditioning:
- Record-description line with blank cols 23-31 ⇒ generated as conditioned by
  `1P OR <overflow>` (auto report allocates an unused overflow indicator if the
  PRINTER file's F-spec declares none).
- Field lines under a blank-conditioned record ⇒ conditioned by `N1P` (so fields
  don't print on the first page before the first record is read). **Exception:**
  the reserved words `PAGE`, `PAGE1`-`PAGE7`, `UDATE`, `UDAY`, `UMONTH`, `UYEAR`
  do **not** get `N1P` (manual `90840-90846`).

No overflow line is ever created for page-heading specs (manual `92175`).

### 6.3 End-position computation

Auto report computes end positions and centers lines. The "report width" is the
max end position across **all** lines in the report (heading, detail, total). In
Phase B (headings only, no D/T-*AUTO yet) the width is just the heading width;
Phase C will widen it. For now: lay fields/constants left-to-right with the
spacing rules above, then center the whole line within the printer line width
(132 default, or from the F-spec / line-counter spec).

### 6.4 Generated output

Each `H-*AUTO` record-description ⇒ **one** generated type-`H` `ORecord`
(`ospec.h`) with:
- `file` = the PRINTER filename
- `type = OType::Heading`
- `space_before`/`space_after`/`skip_before` from cols 17-22 (or defaults)
- `conditions` = the `1P OR <overflow>` group (or the user's indicators)
- `fields` = the date/title/page `OField`s, each with computed `end_pos` and
  field lines carrying `N1P` (except reserved words)

Emit as O-spec **text** into `src` (per §2), replacing the `H-*AUTO` block.

### 6.5 Tests (Phase B)

- `tests/autoreport/heading_basic.rpg` — one `H-*AUTO` with a title constant;
  assert the generated program prints `mm/dd/yy  <TITLE>  PAGE 0001` on page 1.
  Compare against a hand-written equivalent program
  `tests/autoreport/heading_basic_equiv.rpg` that produces the same output
  without Auto Report (golden-output diff via the existing run_tests.sh pattern).
- `tests/autoreport/heading_date_suppressed.rpg` — U-spec col 27 = `N`; assert no
  date/page.
- `tests/autoreport/heading_multi_line.rpg` — three `H-*AUTO` records; assert
  spacing/skip defaults (skip-to-06 before first, space-1 after middle, space-2
  after last).
- `tests/autoreport/heading_field_n1p.rpg` — a real field on a heading line;
  assert it carries `N1P` and doesn't print on page 1.
- `tests/autoreport/heading_reserved_no_n1p.rpg` — `UDATE`/`PAGE` on a heading;
  assert no `N1P`.

**Exit criteria:** a heading-only Auto Report program compiles and its runtime
output matches the hand-written equivalent. No regressions.

## 7. Phase C — Output sub-feature (`D/T-*AUTO`) (large)

**Goal:** a program with a `D-*AUTO` or `T-*AUTO` field-list block compiles and
produces correct detail + total lines, with the synthesized accumulator
subroutine. This is the bulk of the work.

This phase is itself broken into sub-phases (C1-C4) so the hard parts can be
landed and tested independently.

### 7.1 The construct

A `D/T-*AUTO` block is: one **record-description** O-spec line (`D` or `T` in
col 15, `*AUTO` in cols 32-36) followed by **field-description** lines, each
typed by **column 39**:

| Col 39 | Type | Meaning (manual `90922-90941`) |
|---|---|---|
| blank / `B` | detail field | field/constant on the detail line (`B` = blank-after) |
| `A` | auto-accumulate | numeric field, printed on detail **and** totaled at every control level + LR |
| `C` | heading continuation | 2nd/3rd line of a column heading (cols 45-70 literal) |
| `1`-`9`, `R` | total-line field | field/constant on the L*n* / LR total line |

Record-description columns (manual `90949-91039`, verified):

| Cols | Field |
|---|---|
| 7-14 | filename |
| 15 | `D` (detail report) or `T` (group/total-only report) |
| 16 | `F` fetch overflow (D: detail line; T: lowest total line) |
| 17-22 | spacing/skip (D: detail line; T: lowest total line; blank ⇒ single-space after) |
| 23-31 | indicators (blank ⇒ D gets `N1P`; T gets the lowest defined control level) |
| 32-36 | `*AUTO` |
| 37-70 | unused |

Field-description columns (manual `93856-94003` Figure 252, verified):

| Cols | blank/B/A | C | 1-9/R |
|---|---|---|---|
| 23-31 | indicators | blank | blank |
| 32-37 | field name | blank | field name or blank |
| 38 | edit code (numeric only; blank⇒`K` auto) | blank | edit code |
| 39 | blank/B/A | `C` | `1`-`9`/`R` |
| 40-43 | end pos (optional) | blank | blank (must not enter) |
| 44 | blank | blank | blank |
| 45-70 | column-heading literal **or** edit word | heading literal (≤24ch) | constant **or** edit word/currency/asterisk-fill |

Restriction (manual `91033`): **only one** `D/T-*AUTO` per program.

### 7.2 Sub-phase C1 — Parse & generate detail O-specs only (no accumulation)

**Scope:** `D-*AUTO` with only blank/`B`/`C` field types (no `A`, no totals).
Generate the column-heading lines and the detail line. This exercises the
column-heading, end-position, and edit-code-default logic without the
accumulator machinery.

Generation (manual `92104-92169`):
- **Column headings:** one type-`H` `ORecord` per heading line, conditioned
  `1P OR <overflow>`. The `C`-type field lines extend a field's heading to a 2nd
  /3rd line. Numeric fields right-align under their heading; alphameric
  left-align; the longer of field/heading wins the column width.
- **Detail line:** one type-`D` `ORecord`, conditioned by the record-desc's
  indicators (blank ⇒ `N1P`). Each blank/`B` field becomes an `OField` with
  computed `end_pos`, edit code `K` if numeric & col 38 blank (manual `91100`),
  `blank_after` if col 39 = `B`.
- **Spacing:** detail line single-spaces after unless cols 17-22 say otherwise.
- **End positions:** lay fields left-to-right; ≥2 blanks before each field on a
  body line; no space before constants (manual `92132-92133`).

Tests: `c1_detail_only.rpg`, `c1_column_headings_multiline.rpg`,
`c1_edit_code_default_k.rpg` — each diffed against a hand-written equivalent.

### 7.3 Sub-phase C2 — The accumulator (`A` fields) — generated C-specs

**Scope:** add `A`-field handling. This generates the `A$$SUM` subroutine and the
control-level roll chain.

#### 7.3.1 Control levels come from input specs

Control levels are **not** declared in the `*AUTO` spec. They come from **input
specs, cols 59-60** (manual `85286-85298`): an input field tagged `L1`-`L9` is a
control field. The preprocessor reads `ir.prog.in_fields` (already parsed before
we'd ideally run — see §8 caveat) to discover which of L1-L9 are defined.

→ **Important ordering issue:** today `expand_copy_statements` +
`reject_uspecs` run *before* `parse_ispecs` (`main.cpp:315` vs `:321`). To read
control levels, the preprocessor needs the parsed input specs. Options:
**(a)** parse input specs *inside* the preprocessor (call `parse_ispecs` on the
current `src`), or **(b)** move the preprocessor to run *after* input parsing
and have it emit text that survives a second parse. **Option (a) is cleaner**:
the preprocessor calls `parse_ispecs` locally to discover control fields, then
discards the result (the real parse happens later as today). Document this.

#### 7.3.2 Synthesized field naming (manual `91198-91216`, verified)

For an `A` field named `F`, create one total field per **defined** control level
(L1-L9 present in input cols 59-60) **plus LR**:

- `len(F) < 6`: append the level suffix → `F` + `{1-9|R}`. E.g. `QTY` → `QTY1`,
  `QTY3`, `QTYR` (if L1, L3 defined).
- `len(F) == 6`: **replace** the last char → `F[:5]` + `{1-9|R}`. E.g. `SOLDVA`
  → `SOLDV1`, `SOLDV2`, `SOLDVR`.
- `len(F) > 6`: not allowed for `A` (error).
- If **no** control levels are defined, only the `R` (LR) field is created
  (manual `94754`, the record-count edge case).

Each total field is **+2 digits** vs the source, same decimals (manual
`91214`). It is implicitly declared by being the result field of a generated ADD.

Collision rules to enforce as errors (manual `91223-91249`): an `A` field used
twice; two `A` fields whose first 5 chars match; a hand-coded field whose name
collides with a synthesized one.

#### 7.3.3 The `A$$SUM` subroutine + roll chain (manual `92917-93098`)

The subroutine name is always `A$$SUM`. Generate (as C-spec **text**):

```
// 1. Detail-time call (conditioned like the D-*AUTO record-desc; unconditioned for T-*AUTO)
C ... EXSR A$$SUM

// 2. Total-time roll, one ADD per A-field per defined level (ascending), each
//    conditioned by the lower level's indicator:
C L1  <F2> ADD <F1> <F2> <len+2>     // roll level 1 -> level 2
C L1  ...
C L2  <FR> ADD <F2> <FR> <len+2>     // roll level 2 -> LR
C L2  ...

// 3. (For T-*AUTO only) an L0 Z-ADD resets each A-field to zero each cycle,
//    as the FIRST total calc (manual 93095).

// 4. The subroutine, always last:
C     A$$SUM BEGSR
C     <F1> ADD <F>  <F1> <len+2>     // accumulate source -> lowest level
C     ...
C           ENDSR
```

Where `<F>` is the original `A` field, `<F1>` its lowest-level total field,
`<F2>`/`<F3>`/…/`<FR>` the higher levels. The lowest level's ADD source is the
original field (inside `A$$SUM`); higher rolls add level *n* into level *n+1*.

Each `ADD` inside `A$$SUM` is conditioned by that field's field-description
indicators (cols 23-31), if any (manual `93080`).

Worked example to match exactly (Figures 232→233, 248→249,
`manual_text.txt:89556-89640`, `92772-92908`): input `SOLDVA A` + `VALUE A` with
L1 (BRANCH), L2 (REGION) generates:
```
EXSR A$$SUM
L1 SOLDV2 ADD SOLDV1 SOLDV2 92
L1 VALUE2 ADD VALUE1 VALUE2 92
L2 SOLDVR ADD SOLDV2 SOLDVR 92
L2 VALUER ADD VALUE2 VALUER 92
A$$SUM BEGSR
SOLDV1 ADD SOLDVA SOLDV1 92
VALUE1 ADD VALUE   VALUE1 92
ENDSR
```

Tests: `c2_accumulator_two_levels.rpg` (the above), `c2_no_control_levels.rpg`
(only `R` field — the record-count case), `c2_six_char_field.rpg` (replace vs
append), `c2_name_collision.rpg` (error), `c2_group_printing_t_auto.rpg`
(T-*AUTO + L0 Z-ADD).

### 7.4 Sub-phase C3 — Total O-specs

**Scope:** generate the total output lines (one per defined level, ascending, LR
last) for the `A` fields, plus the `1-9`/`R` total-line fields/constants.

Generation (manual `92879-92907`, `91919-91927`):
- One type-`T` `ORecord` per defined control level (L1…LR), lowest first.
- Each carries the synthesized total field with edit code `K` and `blank_after`
  (`B`), at the same `end_pos` as the detail field.
- **Asterisks:** to the right of the max end position, `*` count = level depth
  (L1=`*`, next=`**`, …, LR = N stars for N defined levels; ≤10). Suppressed if
  U-spec col 28 = `N`.
- `1-9`/`R` field/constants print to the **left** of the first total field,
  preceded/followed by one space (manual `92164-92169`). E.g. the `R`-type
  `'FINAL TOTALS'` constant.
- Spacing: 2 lines after every total line; 1 space before the lowest-level and
  the final total line.

Tests: `c3_total_lines.rpg`, `c3_asterisk_depth.rpg`, `c3_final_totals_const.rpg`,
`c3_suppress_asterisks.rpg`.

### 7.5 Sub-phase C4 — Whole-program sort

**Scope:** reorder the generated specs into compiler order and apply the two
O-spec sort modes. This is what makes a mixed program (hand-coded + `*AUTO`)
come out right.

#### 7.5.1 Standard sort (manual `89904-89915`)

After all generation, sort **all** specs into: H → F → E → L → T → I (sorted
input records, then data structures) → C (detail, L0, L1-L9, LR, subroutines) →
O → compile-time arrays/tables. Resequence cols 1-4 (0010, +0010, wrap to 0000
past 999) and set col 5 = `E` for generated specs (manual Table 19, `89860`).

#### 7.5.2 O-spec sort modes (manual `89933-89955`)

**Default** (no qualifying hand-coded total spec): heading specs, then detail,
then totals (lowest level first, LR last); the whole group sits where the
original `D/T-*AUTO` was; other O-specs keep relative order.

**Alternate** (triggered if the user coded a normal RPG total O-spec conditioned
by a **positive** control-level indicator — no `N` in col 23 — in cols 24-25 for
the Auto-Report file): heading/detail/exception specs and non-qualifying totals
stay in coded order; qualifying totals are sorted ascending by that indicator,
LR last.

The preprocessor must scan hand-coded O-specs to detect this trigger and pick
the mode. This is the fiddliest rule; test it hard.

Tests: `c4_sort_default.rpg`, `c4_sort_alternate_mode.rpg`,
`c4_mixed_handcoded_and_auto.rpg`.

### 7.6 Phase C exit criteria

The full worked example (Figures 232→233 / 248→251) compiles and its runtime
report output (column headings, detail lines, totals with asterisks, final
totals, page headings) matches a hand-written equivalent program line-for-line.
All of `tests/run_tests.sh` still passes.

## 8. Risks, caveats, and explicit non-goals

- **Column accuracy is load-bearing.** The manual's spec-sheet figures are
  mangled in `manual_text.txt` (PDF extraction scattered cells); the **prose**
  column numbers are authoritative and were used for this plan. Validate every
  column rule against a hand-written equivalent program, not just the figures.
- **Catalog output (U-spec col 7 = `C`)** — writing the generated source back to
  a library member — is a **non-goal** for now. We surface `AutoReportReport` so
  a caller *could* write it out, but we don't implement the AS/400 library I/O.
  Generated source is available via a `--emit-autoreport-source` debug flag.
- **`AUTOC` / `CRTS36RPT` CL-command integration** (manual `88793+`, `95707+`)
  is a **non-goal** — those are the IBM compile commands that invoke the
  preprocessor. Our equivalent is just running `expand_autoreport` automatically
  inside `rpgc`/`rpg-analyze`.
- **The "alternate O-spec sort" trigger detection** (§7.5.2) is the most
  error-prone rule. Budget extra tests here.
- **Re-parsing emitted text** (§2 design choice) means generated specs go through
  the normal column validators — this is a feature (free validation) but means
  the emitter must be column-perfect or the validator will reject our own output.
  Mitigation: a `--dump-autoreport-source` flag and golden-text tests on the
  emitted source itself.
- **Phase C2 ordering** (§7.3.1): the preprocessor must call `parse_ispecs`
  locally to discover control levels, because it runs before the main input
  parse. This double-parse is cheap (sources are small) but must be documented.

## 9. Sequencing & effort summary

| Phase | Deliverable | Effort | Dependencies |
|---|---|---|---|
| A | Module skeleton + U-spec option parsing + call-site wiring (no-op for non-AR programs) | Small | none |
| B | `H-*AUTO` page-heading generation | Medium | A |
| C1 | `D-*AUTO` detail + column headings (no accumulation) | Medium | A |
| C2 | `A`-field accumulator: `A$$SUM` + roll chain C-specs | **Large** | C1 |
| C3 | Total O-specs (per level, asterisks, final totals) | Medium | C2 |
| C4 | Whole-program sort + two O-spec sort modes | Medium-Large | C3 |

Phases B and C1 are independent and could proceed in parallel. C2 is the
load-bearing hard part. After C4, `reject_uspecs`'s old hard-rejection behavior
is fully replaced and Auto Report source programs compile end-to-end.

## 10. Validation strategy

1. **Golden-output tests** — every Auto Report test program has a hand-written
   non-Auto-Report equivalent; both are compiled and run (via the existing
   `run_tests.sh` harness) and their printed output is diffed. This is the
   strongest check: it proves the *generated program* behaves identically to the
   reference, not just that the *generated source* parses.
2. **Golden-source tests** — for each construct, assert the emitted source text
   (via `--dump-autoreport-source`) matches a checked-in expected file. Catches
   column-layout regressions directly.
3. **Manual-figure regression** — add Figures 232/248 (input) and 233/249/250/
   251 (expected generated source) from the manual as explicit test cases, since
   they're the canonical examples.
4. **Existing suite** — `tests/run_tests.sh` must stay green throughout; every
   phase lands only if it does.

---

## References (all in `docs/ref/manual_text.txt`)

| Topic | Lines |
|---|---|
| Ch. 26 intro + Figures 232/233 (worked example) | 89552-89880 |
| Order of created specs (overall + calc + output sort modes) | 89880-89980 |
| U-spec option columns (6-30) | 90221-90363 |
| Page-heading specs (`H-*AUTO`) | 90700-90901 |
| Output specs (`D/T-*AUTO`) section | 90907-91503 |
| Created total-field naming rule | 91194-91249 |
| Group printing (`T-*AUTO`) | 91504-91523, 93095-93098 |
| Report format (spacing, placement, overflow) | 91889-92310 |
| Created specs overview (Figs 248/249) | 92554-92915 |
| Created CALC specs + `A$$SUM` rule (Fig 250) | 92917-93098 |
| Created OUTPUT specs (Fig 251) | 93099-93819 |
| Figure 252 column chart (col-39 → valid entries) | 93821-94003 |
| Record-count edge case (`COUNTR` when no levels) | 94725-94756 |
