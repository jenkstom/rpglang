# WORKSTN Support — Implementation Plan

**Status: planning only. No implementation has started.** This document is a
design and phasing plan for adding real WORKSTN (interactive display-terminal)
device support to the RPG II → LLVM compiler, replacing the current hard
compile error (`fspec.cpp`'s E8) with actual codegen and a runtime backend.

This is a materially bigger lift than any item previously closed in the
compiler's finish-out work (see `CALL_LINKAGE_PLAN.md` and
`MISC_OPCODES_PLAN.md` for the other large remaining items, and git history
for the closed groups): most other work was "the manual defines X, the
compiler either mishandles or omits it, fix the existing code path." WORKSTN
has **no existing code path to extend** — it needs a new display-format definition
mechanism (the manual's SDA/S-D-spec equivalent, which today is produced by a
tool entirely outside this project), a new terminal I/O runtime backend (this
compiler currently only knows how to read/write flat files), and five new
operation codes with a device model (multiple attached terminals, device IDs,
function keys) that doesn't resemble anything the codegen currently does.

Manual citations below are to `docs/ref/manual_text.txt`. Chapter 7 ("Using a
WORKSTN File") is the primary reference, body at lines 30841–39719; Chapter 27
("Operation Codes") has the ACQ/REL/NEXT/POST/SHTDN entries; Chapter 19
("File Description Specifications") has the F-spec continuation-line options
(78538–78715); Chapter 25 ("Output Specifications") has the format-name O-spec
entry (89007–89017).

---

## 0. Scope-defining finding: not everything in the old C9 grouping is actually WORKSTN

Before phasing this out, one correction to the compiler finish-out work's
former C9 item (now retired along with `docs/TODO.md` — see
`CALL_LINKAGE_PLAN.md` §0 and `MISC_OPCODES_PLAN.md`), which grouped
`ACQ`/`REL`/`NEXT`/`KEY`/`POST`/`FORCE`/`FREE`/`SHTDN` together as
"WORKSTN-adjacent." Reading each operation's actual manual entry shows three
of the eight don't belong in a WORKSTN effort at all:

- **`KEY`** (manual 113040-113092) is explicitly **KEYBORD-device-only** — its
  own spec requires the F-spec device to be `KEYBORD`, a different (simpler,
  single-field) device type this plan does not cover. Same family as the `SET`
  operation. Both now have a real plan: `KEYBORD_PLAN.md`.
- **`FORCE`** (111276-111414) **cannot target a WORKSTN or KEYBORD device at
  all** — the manual explicitly excludes them (111310-111311); it only
  reorders which DISK primary/secondary/update file gets read next cycle.
  It's a small, fully independent DISK-cycle feature that could be implemented
  with no WORKSTN work whatsoever. Now has a real plan: `MISC_OPCODES_PLAN.md`
  §2.
- **`FREE`** (111422-111466) deactivates a *called program* (forces
  re-initialization on the next `CALL`) — its factor 2 is a program name, not
  a device. It is entirely dependent on the `CALL`/`PARM`/`PLIST`/`RETRN`/
  `EXIT` linkage family, itself unimplemented and explicitly noted as needing
  its own design pass before any coding starts. It has nothing to do with
  WORKSTN devices beyond being listed in the same manual section. Now has a
  real plan: `CALL_LINKAGE_PLAN.md` §0 and phase L5.

**This plan's actual opcode scope is: `ACQ`, `REL`, `NEXT`, `POST`, `SHTDN`,**
**plus making `READ`/`EXCPT`/O-spec output WORKSTN-aware.** `FORCE`, `FREE`,
and `KEY` are out of scope here entirely — each now has its own plan
document as noted above, none of which this plan depends on or blocks.

---

## 1. Explicit non-goals

To keep this achievable, the following are **not** in scope for "WORKSTN
support" as planned here, even though they appear in the same manual
chapters:

- **SPECIAL, CONSOLE, BSCA, ICF/CFILE (telecommunications) devices.** Only
  `WORKSTN` proper. The existing E8 hard error stays in place for the other
  three.
- **MRT (Multiple Requester Terminal) programs** — `SUBR20`/`SUBR21`,
  `MRTMAX`, and true multi-terminal sharing of one program copy (35562-35930).
  Initial implementation targets **SRT (Single Requester Terminal)** only:
  one program instance, the `NUM` continuation option capped at reasonable
  small values for multiple *acquired* devices (ACQ-acquired secondary
  devices are legal even in an SRT program — that part stays in scope), but
  no shared-copy/SUBR20/SUBR21 semantics. Flagged as an explicit follow-on.
- **DBCS** (double-byte character sets) fields/attributes on display formats.
- **Real 5250 protocol / actual attached terminal hardware.** The backend is a
  Linux terminal (see §3) standing in for the display station; this is a
  deliberate porting decision, same category as the existing "record format"
  porting note in `docs/ARCHITECTURE.md` §4 for DISK files.
- **Full SDA-equivalent tooling.** No visual screen designer; formats are
  authored as text in the new spec format from §2.
- **`KEY`, `SET`, `FORCE`, `FREE`** — see §0.

---

## 2. New surface area: display-format definitions

On a real System/36, a WORKSTN file's formats are compiled *outside* RPG II,
by SDA or hand-written S-spec/D-spec, into a "display file" the F-spec's
`FMTS` continuation option names (default: program-name + `FM`). This project
has no such compiler and no DDS equivalent — it must be built.

**Decision needed before implementation (see §8, open question 1):** what to
call this new file type and its extension. This plan assumes a new pair of
spec-line types, parsed by new `compiler/src/sspec.h`/`sspec.cpp` +
`compiler/src/dspec.h`/`dspec.cpp` (mirroring the manual's own "S-spec"/
"D-spec" terminology for format header vs. field lines — see manual references
to "S-spec col 17" for variable start line and "S/D-spec cols 33-34" for the
override indicator), living in a **separate source file** referenced by the
F-spec's `FMTS` value, structurally the same "separate file, spliced in by
name" shape `/COPY` already uses (`source.cpp`'s `expand_copy_statements`,
Group D3) — reuse that lookup-by-name-next-to-main-source convention rather
than inventing a new one.

Minimum viable field set per format (a reduced, explicitly-scoped subset of
real DDS — full 5250 attribute support is a non-goal per §1):

- Format name (manual: S-spec cols 7-14) and record length/size.
- Per field: name, start row/column, length, decimal positions (numeric) or
  blank (alphameric), usage (**I**nput / **O**utput / **B**oth — matches the
  I-spec-style "usage" concept already familiar from ordinary field decode),
  and a small attribute set: **protect** (display-only, no cursor stop),
  **color** (mapped to ANSI SGR in the terminal backend — see §3), **reverse
  image**, **blink**. Constant/literal text fields (labels) with just
  row/column/text, no field name.
- Function-key enablement per format (which of KA-KY are valid) and
  command-key enablement (Print/RollUp/RollDown/Clear/Help/Home) — manual
  33667-33741 for the function-key side, 33787-33820 for the command-key/
  exception terminology.

Explicitly deferred from v1 (documented gap, not silent): auto-record-advance
fields (D-spec col 36, manual "Data returns... on Field Exit/Field+/Field-"),
Dup-key handling (35938-35976), variable start line (`V`, needs the F-spec
`SLN` continuation value), and redisplay field-override indicators
(36006-36080).

---

## 3. New surface area: runtime terminal backend

`runtime/rpg_runtime.h` today has no concept of an interactive device, device
IDs, or "multiple things attached to one file." New primitives needed (naming
illustrative, not final):

```
int  rpg_rt_ws_open(const char *fmts_file, const char *program_id);
int  rpg_rt_ws_acquire(int ws_id, const char *device_id /* nullable */);
int  rpg_rt_ws_release(int ws_id, const char *device_id);
int  rpg_rt_ws_read(int ws_id, char *buf, int buflen,
                     char *out_device_id, char *out_format_name,
                     int *out_status /* *STATUS-style */);
int  rpg_rt_ws_write(int ws_id, const char *format_name,
                      const char *device_id, const char *buf, int buflen);
int  rpg_rt_ws_next(int ws_id, const char *device_id);
int  rpg_rt_ws_post(int ws_id, const char *device_id, int *size,
                     int *mode, int *inp, int *out);
```

These sit alongside, not replacing, the existing `rpg_rt_open_input`/
`rpg_rt_read_next`/`rpg_rt_open_output`/`rpg_rt_emit_line` family used by
DISK/PRINTER.

**Two backends behind one interface — this is the key design decision that
makes the feature testable at all:**

1. **Real terminal backend** (`termios` raw mode + ANSI/VT100 escape
   sequences for cursor positioning `\x1b[{row};{col}H` and SGR attributes
   for color/reverse/blink). Reads keystrokes, distinguishes Enter from
   function/command keys via escape-sequence parsing. **Needs a documented
   keyboard-mapping convention** since a Linux terminal doesn't have 24
   function keys or dedicated Roll/Clear/Help/Home keys — proposed mapping
   (open question 2, §8): F1-F12 direct, Shift+F1-F12 → F13-F24 (a common
   5250-emulator convention), PageUp/PageDown → RollUp/RollDown, and a small
   documented table for Print/Clear/Help/Home.
2. **Scripted/headless backend** for the test suite and non-interactive runs:
   reads a plain-text script of "screen filled, here's the next input record
   and which key was pressed" from a file (format TBD, but should look like
   the rest of this project's test fixtures — compare `tests/run_tests.sh`'s
   existing input/output-file comparison model), and *dumps* each rendered
   screen to an output file instead of a real terminal. Without this,
   "WORKSTN support" would be entirely unverifiable by the existing
   regression-test harness, which drives programs non-interactively
   (`tests/run_tests.sh`). Backend selection: an environment variable (e.g.
   `RPG_WORKSTN_MODE=headless|terminal`, defaulting to `terminal`) read once
   at `rpg_rt_ws_open` time.

Function-key indicator plumbing has a head start: Group A's A4 fix already
gave `KA`-`KY` (and `U1`-`U8`, `H1`-`H9`) real backing globals
(`codegen.cpp:252-254`'s `key_` array) instead of dropping them from
conditioning. The new WORKSTN read path only needs to *reset all, then set
one* per manual 33740-33741/36264-36267 — the storage and the conditioning
side (C-spec `KA` in cols 9-17) already work.

---

## 4. Phased breakdown

### W1 — F-spec continuation options + device gate

- `fspec.h`: add `Device::Workstn`'s real fields — `num` (max attachable
  devices, ≤251, default 1), `savds`, `ind_count`, `sln`, `fmts` (defaults to
  program-id + `FM` if blank), `id_field`, `infsr`, `infds`, `cfile` (parsed
  but inert — ICF/telecommunications is a non-goal). Manual 78538-78715 for
  column positions/keyword syntax (cols 54-59 keyword, 60-65/60-67 value).
- `fspec.cpp:100-125`: narrow the E8 hard error to exclude `Device::Workstn`
  (SPECIAL/CONSOLE stay hard errors).
- Regression test: extend `tests/neg_workstn.rpg`'s sibling — keep a
  `neg_special.rpg`/`neg_console.rpg`-style negative test proving those two
  *still* hard-error, since W1 must not accidentally widen the E8 exemption.

### W2 — Display-format spec parser (S/D-spec)

- New `compiler/src/sspec.h`/`.cpp`, `compiler/src/dspec.h`/`.cpp` per §2.
- `source.cpp`: extend the `/COPY`-style file-lookup helper (or factor the
  existing one out) to locate the `FMTS`-named file next to the main source.
- New `Program` field (`program.h`): a list of parsed display formats, keyed
  by name, available to both I-spec (record-type-by-format-name, W3) and
  O-spec (format-name output line, W4) resolution.
- This phase has no RPG-source-visible behavior yet (nothing references a
  format until W3/W4 land) — verify purely by parsing sample `.dspf`-style
  fixture files and checking the parsed structure, no codegen involved.

### W3 — I-spec: WORKSTN record types and INFDS

- I-spec record-identification for a WORKSTN file: **the "record type" is
  which named format was read back**, not a byte-pattern match (manual
  31010-31084 — the worked example's blank/A/B record types map straight to
  its two format names). This is a different matching key than the general
  DISK record-ID mechanism (`ispec.cpp`'s existing cols 21-41 handling,
  Section E) — **read Chapter 23's WORKSTN-specific I-spec column
  description in full before implementing this**; the exact column range
  used to name the format per record-identification line needs to be
  confirmed against the manual directly (the worked example shows the
  mechanic but this plan hasn't pinned the precise columns).
- INFDS: extend the existing `ISpecDS`/`ISpecSubfield` machinery (Group D2)
  with the keyword-subfield addressing INFDS needs (`*STATUS`, `*OPCODE`,
  `*RECORD`, `*SIZE`, `*MODE`, `*INP`, `*OUT` in cols 44-51, manual
  33921-34869) — this is a different subfield-addressing mode than D2's
  byte-range subfields (keyword-selected, not position-selected), so
  `ISpecSubfield` likely needs a variant/flag rather than reuse as-is.
- Local Data Area (`is_lda`, I-spec col 18 `U`): currently a hard error at
  `codegen.cpp:703-712` (mislabeled in-code as "H-spec col 18" — it's I-spec;
  fix that comment while touching this code). Give it real per-device backing
  storage instead of erroring; full SRT semantics only (no SUBR20/SUBR21
  per §1).
- Regression tests: format-name record-type dispatch, INFDS `*STATUS`/
  `*OPCODE` readback after a READ, LDA read/write.

### W4 — O-spec: format-name output lines

- `ospec.h`/`ospec.cpp`: parse cols 40-43 as `Kn` (format-name length) instead
  of an end position when a WORKSTN output record line names a format in
  cols 45-54 (manual 89007-89017). Enforce the manual's rule that this line
  cannot carry conditioning indicators of its own (conditioning lives on the
  record line).
- Field lines under a format-name O-spec group place into the **format's
  buffer** (from W2's parsed layout), not a plain print line — reuses the
  existing "fields placed by byte position into a buffer" mechanism from
  ordinary O-spec/DISK output, per the manual's own confirmation
  (32117-32566) that WORKSTN field placement is byte-offset-based, not
  row/column (row/column live in the *display format*, from W2).
- Fix the currently-dead `ORecord::release_device` field (`ospec.cpp:135-150`
  sets an error but never sets the flag) as part of wiring the real `R`
  release path — see W5's REL/ACQ pairing.

### W5 — New operation codes: ACQ, REL, NEXT, POST, SHTDN

Add five entries to `cspec.h`'s `Op` enum (alongside the existing
Group-C-style additions like `BITON`/`SORTA`), parse them in `cspec.cpp`, and
add `emit_acq`/`emit_rel`/`emit_next`/`emit_post`/`emit_shtdn` cases to the
opcode switch in `codegen.cpp:3057` (same pattern as `case Op::CHAIN:
emit_chain(c);` at line 3080), each calling the matching new runtime entry
point from §3:

| Op | Manual | Factor1 | Factor2 | Result | Notes |
|---|---|---|---|---|---|
| `ACQ` | 105067-105107 | opt: device | req: WORKSTN filename | — | opt ER indicator |
| `REL` | 123857-123920 | req: device | req: WORKSTN filename | — | releasing all devices on a primary file → EOF+LR (reuse existing LR-setting path); on demand → EOF indicator (cols 58-59, reuse existing READ-EOF plumbing) |
| `NEXT` | 123403-123448 | req: device ID | req: WORKSTN filename | — | forces next input's source device |
| `POST` | 123621-123669 | req: device ID | — | req: INFDS DS name | fills `*SIZE`/`*MODE`/`*INP`/`*OUT` only, cols 33-42/49-55/58-59 must be blank (parse-time check) |
| `SHTDN` | 124436-124473 | blank | blank | — | req resulting indicator; system-shutdown-requested flag from the runtime backend |

Also: `READ` against a WORKSTN file (123677-123760) and `EXCPT` targeting a
WORKSTN file (uses the INFDS `ID` field to direct output, manual 36272-36273)
need WORKSTN-aware branches in their existing emitters, not new opcodes —
`READ`'s existing emitter (`Op::READ`) gains a device-type check that routes
to `rpg_rt_ws_read` instead of `rpg_rt_read_next` when the target file is
`Device::Workstn`.

### W6 — Cycle integration

- `codegen.cpp:386-395`'s three-way dispatch (`generate_multifile_cycle` /
  `generate_cycle` / `generate_linear`) gains a fourth branch,
  `generate_workstn_cycle`, keyed on the primary file's device being
  `Device::Workstn` — checked ahead of the plain `generate_cycle` call.
- Per manual 36118-36360 (Figure 69/70), only cycle steps 1, 3, 11, 12, 13, 15
  differ from the ordinary DISK cycle. Concretely: step 1 opens/acquires the
  device instead of a flat-file open; step 3/11-13 is the
  reset-function-key-indicators → read → dispatch-on-function-key sequence
  from §3; step 15 (or wherever the existing per-record-type field extract
  sits) dispatches on *format name* (W3) instead of byte-pattern record ID.
  Steps 2, 4-10, 14, 16+ (control levels, matching, total/detail calc
  ordering, LR handling) are unchanged and should be reused verbatim from
  `generate_cycle`, not reimplemented — factor shared logic out if the
  duplication gets large, but don't design a parallel cycle from scratch.
- `open_input_files` (`codegen.cpp:1089`, currently `if (f.device !=
  Device::Disk) continue;`) needs a WORKSTN branch that calls
  `rpg_rt_ws_open`/`rpg_rt_ws_acquire` instead of being skipped.

### W7 — Tests and documentation

- Positive tests (headless backend): acquire/release, function-key-driven
  branching, multi-format record-type dispatch, INFDS status readback, NEXT
  redirection, POST, SHTDN.
- Flip `tests/neg_workstn.rpg` to a positive test (or split: keep a
  `neg_special.rpg`/`neg_console.rpg` proving those two still hard-error, per
  W1) and `tests/neg_orelease.rpg` similarly once REL/release-device lands.
- `docs/SPEC_MAP.md`: new WORKSTN section (F-spec continuation options,
  I-spec record-type-by-format mechanism, O-spec format-name line, the five
  new opcodes) — same density as the existing Section A-G additions.
- `docs/ARCHITECTURE.md`: new section documenting the terminal-backend
  porting decision (§3) alongside the existing "Record format (porting
  decision)" note in §4, and the headless-mode testing rationale.
- Cross-check `CALL_LINKAGE_PLAN.md` and `KEYBORD_PLAN.md` once this plan
  lands: mark their respective mentions of `FREE`/`KEY` as no longer
  blocked on "a future WORKSTN effort" phrasing that predates this plan's
  existence.

---

## 5. Dependency graph

```
W1 (F-spec gate) ──────────────┐
W2 (S/D-spec parser) ──────────┼──> W3 (I-spec record-types+INFDS+LDA) ──┐
                                │                                         │
                                └──> W4 (O-spec format-name output) ──────┼──> W6 (cycle integration) ──> W7 (tests+docs)
                                                                          │
W5 (ACQ/REL/NEXT/POST/SHTDN opcodes, needs W1+W2+W3's INFDS) ────────────┘
```

W1 and W2 can start in parallel (no shared files). W3 and W4 both depend on
W2's parsed format data but not on each other. W5 depends on W3 for the INFDS
plumbing POST needs. W6 is the integration point and necessarily comes last
before W7.

---

## 6. Risk / effort notes

- **W2 (display-format parser) and W3 (record-type-by-format matching) are
  the two items with the least precedent in the existing codebase** — nothing
  today parses a *second* source file into field layouts, and nothing today
  matches a record type by a runtime-returned name rather than a byte
  pattern. Budget the most design/read time here, not in the opcode
  additions (W5), which are mechanically similar to prior Group-C additions.
- **The terminal backend (§3) is the part most likely to need revision after
  first use** — ANSI escape handling and the function-key-mapping convention
  are exactly the kind of thing that looks right on paper and needs
  iteration against a real terminal emulator. Build the headless backend
  first and get W3-W6 correctness-tested against it before investing in the
  real-terminal backend's edge cases.
- **MRT exclusion (§1) is load-bearing for scope, not a minor detail** — an
  SRT-only cut avoids `SUBR20`/`SUBR21`, `MRTMAX` compile-time prompts, and
  shared-copy-of-one-program-instance semantics entirely. Revisit only if a
  concrete multi-terminal use case shows up.

---

## 7. Independent side-quest: FORCE

Per §0, `FORCE` (111276-111414) has zero WORKSTN dependency — it's a small,
self-contained DISK-cycle feature (reorder which primary/secondary/update
file the *next* cycle reads from) that could be picked up on its own, before
or after this plan, with no relation to the phases above. Not sequenced here;
mentioned so it doesn't get silently forgotten when C9 is split per §0.

---

## 8. Open questions to resolve before starting implementation

1. **New spec file naming/extension** (§2): what should the FMTS-referenced
   display-format source file be called by convention (parallel to `/COPY`
   members' `.cpy` convention) — e.g. `.dspf`, `.scr`, reuse `.cpy`? Affects
   `source.cpp`'s file-lookup extension-probing order.
2. **Keyboard-mapping convention for the real terminal backend** (§3): the
   proposed F1-F12 direct / Shift+F1-F12 → F13-F24 / PageUp-PageDown → Roll
   mapping is a reasonable default but should be confirmed rather than
   assumed, since it's the one part of this plan users will interact with
   directly.
3. **Headless-backend script format** (§3): a plain line-oriented protocol
   (e.g. `SCREEN <format>` / `FIELD <name> <value>` / `KEY <name>` blocks) vs.
   something structured (JSON/CSV) — should match the terseness of this
   project's existing `.rpg` test fixtures rather than introducing a new
   heavyweight format.
4. **How far to take the display-format attribute set** (§2): the plan's
   "protect/color/reverse/blink" minimum viable set is a guess at what's
   commonly used in real legacy programs versus what can be deferred; worth a
   second look at Chapter 7's worked examples for what attributes actually
   appear in practice before committing to the v1 column set.

This plan should not move to implementation until at least questions 1-3 are
settled, since they affect file-naming and API shape decisions that are
expensive to change retroactively (same lesson as this project's `/COPY`
member-lookup convention, which question 1 explicitly wants to reuse rather
than duplicate).
