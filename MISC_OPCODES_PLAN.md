# DEBUG and FORCE — Implementation Plan

**Status: planning only. No implementation has started.** This document
covers the two remaining `docs/TODO.md` opcode items that don't belong in
`KEYBORD_PLAN.md` or program linkage: `DEBUG` (from the former C7 group) and
`FORCE` (from the former C9 group). Unlike the KEYBORD/CONSOLE/CRT device
family, these two share no mechanism with each other or with that plan —
they're grouped here only because each is small enough that a standalone
document would be mostly boilerplate. Treat the two sections below as fully
independent; implement in either order, or in parallel.

---

## 1. `DEBUG` — correction to `docs/TODO.md`'s C7 note, then a real plan

`docs/TODO.md`'s C7 entry said DEBUG was blocked because "the compiler has
no debug/symbol-table generation for a parsed DEBUG entry to plug into
either way." Re-reading the manual's H-spec column-15 description in full
(69625-69634, "Column 15 (Debug)") shows that claim conflates two separate
things the manual keeps distinct:

1. **Whether a `DEBUG` C-spec line is active or is treated as a comment** —
   controlled purely by H-spec column 15 (`1` = active, blank = comment).
   This is a simple boolean gate, nothing more.
2. **Whether compiler-generated symbols (names starting with `.`) are
   added to a symbol table "printed with the program dump"** — this is a
   genuinely separate AS/400 feature (a debugger-facing symbol table tied
   to a program-dump facility) that the compiler indeed has no equivalent
   of, and legitimately stays out of scope.

The `DEBUG` *operation code itself* (106221-106340) only needs (1), not
(2): at runtime it writes one or two fully-specified fixed-format records
to an output file — a statement-number/factor-1 label plus a list of
currently-on indicator names (first record), and optionally the result
field's contents (second record, only "when an entry is made in the result
field"). Both record layouts are given in full in the manual (see §1.2
below) and need no symbol-table infrastructure to produce — they're built
from data the compiler already tracks (indicator state, field values).

**Revised finding: `DEBUG` is implementable now.** It was miscategorized as
blocked; treat it as a contained, Group-C-style opcode addition.

### 1.1 H-spec column 15

- `hspec.h`/`hspec.cpp`: add `HSpec::debug_enabled`, parsed from column 15
  (`"1"` → true, blank → false) alongside the other already-parsed columns
  (`hspec.cpp:62-75`).
- `cspec.cpp`/`codegen.cpp`: when `!program.hspec.debug_enabled`, a `DEBUG`
  C-spec line (and its conditioning indicators, per the manual: "the DEBUG
  operation code and its conditioning indicators are treated as a
  comment") compiles to nothing — no diagnostic, matching every other
  spec's ordinary comment handling, since this is documented, intentional
  behavior, not a gap. When enabled, parse and emit normally (§1.2).

### 1.2 The `DEBUG` operation

- `cspec.h`/`cspec.cpp`: parse `DEBUG` (factor 1 optional — literal or
  field, 1-8 characters, used as a label; factor 2 required — the name of
  an output file, "any valid output file, except a CRT file," record
  length ≥ 80; result field optional — a field or array name to dump;
  columns 49-59 must be blank). Enforce "the same output file name must
  appear in factor 2 for all DEBUG statements in a program" as a
  parse-time consistency check across every `DEBUG` line. Enforce
  "externally described files are not allowed with the DEBUG operation" —
  the compiler has no externally-described-file concept at all
  (`docs/ARCHITECTURE.md`), so this constraint is vacuously satisfied,
  worth a one-line note rather than code.
- `emit_debug` (`codegen.cpp`): builds and writes the two fixed-format
  records to the factor-2 file's existing output-buffer machinery (reuse
  the same O-spec byte-placement codegen every other output field already
  uses, not a new writer):
  - **Record 1** (always written): cols 1-8 literal `DEBUG =`, cols 9-18
    the compiler-assigned statement number of this `DEBUG` line (this
    compiler already assigns a source line number to every C-spec; use
    that), cols 19-26 factor 1's literal/field contents if present else
    blank, cols 27-28 blank, cols 29-44 literal `INDICATORS ON =`, cols 45+
    a space-separated list of every currently-on indicator's name. "More
    than one record may be needed" for the indicator list on a narrow
    output file — wrap to additional records rather than truncating
    silently.
  - **Record 2** (only when the result field is present): cols 1-14
    literal `FIELD VALUE =`, cols 15+ the result field's contents (up to
    256 characters; if the result field is an array, "more than one output
    record may be needed" — same wrap rule as record 1).
- Regression tests: `tests/debug_basic.rpg` (H-spec col 15 = `1`, one
  `DEBUG` with factor 1 label and a result field, checked against expected
  output-record bytes), `tests/debug_disabled.rpg` (H-spec col 15 blank —
  confirm the `DEBUG` line is silently inert, same output as if the line
  didn't exist), `tests/neg_debug_multifile.rpg` (two `DEBUG` lines naming
  different factor-2 files — hard parse error).

---

## 2. `FORCE`

Already correctly scoped as independent by `WRKSTN_PLAN.md` §0 and flagged
in its §7 as a side-quest "with zero WORKSTN dependency ... a small,
self-contained DISK-cycle feature ... not sequenced there." This section
gives it the actual implementation plan `WRKSTN_PLAN.md` deliberately left
unwritten.

**What it does** (111276-111414): overrides which primary/secondary/update
DISK file the *next* program cycle reads from, superseding the normal
multifile record-selection algorithm for exactly one cycle. "The FORCE
operation can be used for primary or secondary input and update files;
however, it cannot be used to read from files assigned to a KEYBORD or
WORKSTN device" — a static, checkable restriction (cross-reference against
each named file's `Device`, including the new `Device::Keybord` this
project's `KEYBORD_PLAN.md` K1 adds). "If more than one FORCE operation is
processed during the same program cycle, all but the last are ignored" —
last-write-wins within a cycle. "FORCE should not be specified at total
time" is phrased as a recommendation, not a hard rule, in the manual text
itself — parse-time warning, not an error, matching the project's
`DiagKind` severity distinctions elsewhere. "The first record processed is
always selected by the normal method" — `FORCE` never affects the *current*
cycle's already-in-flight record, only the next one.

### 2.1 Implementation

- `cspec.h`/`cspec.cpp`: parse `FORCE` (factor 1 blank, factor 2 required —
  the target file's name, result field blank). Hard error if factor 2
  names a `KEYBORD` or `WORKSTN` file (per the manual restriction above,
  checkable entirely at parse time since file devices are already known
  from the F-specs).
- `codegen.cpp`: add a per-program "forced next file" global (a small
  integer/pointer selecting among the declared input/update files, `-1` /
  null = no override). `emit_force` overwrites this global unconditionally
  (implementing last-wins for free — each new `FORCE` in the same cycle
  just overwrites the previous value before the cycle's own read step
  consults it). The multifile cycle's existing file-selection step (the
  sequence-flag-aware selection loop in `codegen.cpp` that already reads
  each file's F-spec `sequence`, `fspec.cpp:83-88`) checks this global first: if set,
  read from the named file and clear the global; if unset, fall back to
  the existing normal-selection algorithm unchanged. This is additive to
  the existing selection loop, not a replacement — matches the manual's
  "FORCE operations override the multifile processing method... the first
  record ... is always selected by the normal method."
- Regression test: `tests/force_basic.rpg`, adapted directly from the
  manual's own Figure 298 worked example (111276-111414's inline source:
  two input files `INPUT1`/`INPUT2`, a `NBR` counter field controlling how
  many secondary records follow each primary record via alternating
  `FORCE` calls) — the manual provides a complete, ready-to-port fixture
  here, unusually convenient among the project's opcode additions.
  `tests/neg_force_workstn.rpg` for the device restriction.

### 2.2 Dependency note

`FORCE`'s device-restriction check benefits from (but does not strictly
require) `KEYBORD_PLAN.md` K1 landing first, since today a `KEYBORD` file
would be silently misdetected as `Device::Other` rather than a real,
checkable device value (`KEYBORD_PLAN.md` §0's finding). If `FORCE` is
implemented before `KEYBORD_PLAN.md` K1, only the `WORKSTN` half of the
restriction is checkable in the interim; add the `KEYBORD` half once K1
lands rather than blocking on it.

---

## 3. Sequencing

Both sections are ready to implement now, in either order — neither has an
open design question blocking it, unlike `KEYBORD_PLAN.md` (needs the
message-member format and column mechanics settled). `FORCE` is the smaller
of the two (one opcode, one new global, reuses the existing multifile
selection loop); `DEBUG` is slightly larger (one new H-spec column plus a
fixed-format dual-record writer) but still fully self-contained.
