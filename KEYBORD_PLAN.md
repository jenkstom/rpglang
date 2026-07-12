# KEYBORD/CONSOLE/CRT Device Support — Implementation Plan

**Status: planning only. No implementation has started.** This document is a
design and phasing plan for the `KEY` and `SET` operation codes and their
three supporting devices — `KEYBORD`, `CONSOLE`, and `CRT` — which
`WRKSTN_PLAN.md` explicitly scoped out of its own WORKSTN effort (§0: "`KEY`
... is explicitly KEYBORD-device-only ... a different (simpler,
single-field) device type this plan does not cover"; §1: "`KEY`, `SET` ...
see §0").

Manual citations below are to `docs/ref/manual_text.txt`. Chapter 10
("Using a CONSOLE, KEYBORD, or CRT File") is the primary reference, body at
lines 42586-47204+; Chapter 27 ("Operation Codes") has the `KEY`
(113035-113092) and `SET` (124240-124291) opcode entries.

---

## 0. Immediate finding: KEYBORD and CRT devices are an undiagnosed silent
   miscompile today, worse than the WORKSTN/SPECIAL/CONSOLE case

`fspec.cpp:101-108` recognizes exactly five device tokens: `DISK`,
`PRINTER`, `WORKSTN`, `SPECIAL`, `CONSOLE`. `CONSOLE` gets a hard error
(`fspec.cpp:119-125`). **`KEYBORD` and `CRT` do not appear
in the token list at all** — they fall through to `Device::Other` with no
diagnostic, and the compiler then treats the F-spec filename as an
ordinary flat file on Linux, exactly the silent-miscompile failure mode E8
was written to close for the other three devices. This is strictly worse
than the WORKSTN case: WORKSTN at least errors loudly today.

This should be fixed as a standalone, near-zero-risk change **independent
of the rest of this plan** — add `Device::Keybord`/`Device::Crt` to the
enum (`fspec.h:20`) and extend the `E8` hard-error condition
(`fspec.cpp:119-120`) to include them, with a `neg_keybord.rpg`/
`neg_crt.rpg` regression test pair (same shape as the existing
`tests/neg_workstn.rpg`). This closes the silent-miscompile gap immediately
and gives phase K1 below a clean, already-diagnosed starting point (narrow
the hard error, the same pattern `WRKSTN_PLAN.md` W1 uses for
`Device::Workstn`) rather than a "nothing happens" starting point.

---

## 1. Why this is one small plan, not folded into `WRKSTN_PLAN.md`

The manual is explicit that these are the *legacy*, single-field-at-a-time
predecessors WORKSTN replaced ("Whenever possible, use a WORKSTN file
instead of a CONSOLE, KEYBORD, or CRT file... so that programs that used
those files on earlier IBM systems can run on the AS/400 system without
being rewritten," 42588-42593) — a genuinely different, much smaller
interaction model than WORKSTN's named-format full-screen I/O:

- **`CONSOLE`** — input-only. Provides data either as an ad hoc input file
  with auto-generated prompts, or as a record-address file supplying key
  values. A program may declare at most one. No `KEY`/`SET` involvement for
  reading it (SET's `ERASE` form is the one CONSOLE-adjacent use of `SET`,
  see §3).
- **`KEYBORD`** — input *and* output, but only via the `KEY`/`SET`
  operation codes below, never via ordinary I-specs ("Although a KEYBORD
  file is an input file, you do not code input specifications for a
  KEYBORD file. Instead, you define the input data on the calculation
  specifications for a KEY operation," 45429-45432). One field prompted
  and entered at a time, not a laid-out screen format.
  - **`KEY`**: pauses calculation; factor 1 (constant/literal/field/table
    or array element) is displayed as the prompt, the result field receives
    what the user types. Screen layout is fixed and derived purely from the
    KEYBORD file's record length, not an authored format (unlike WORKSTN):
    record length ≤ 40 → six lines of 40 characters, centered; record
    length > 40 → 24 lines of 79 characters. Entry-function-key handling
    (Field Exit/Field-/Field+/Enter) and the Dup-key "leave unchanged"
    convention are the only "function key" concepts KEY has, distinct from
    WORKSTN's KA-KY set.
  - **`SET`**: any combination of (a) triggering specific function keys
    listed in cols 54-59, (b) displaying factor 1's field/literal/array or
    table element, (c) displaying a numbered message (0001-0099) from the
    `USER1` message member via a `SETnn`/`KEYnn` suffix convention
    (124278-124281 — needs the exact column/suffix mechanics confirmed
    against the Chapter 10 calc-spec layout before implementation, see §5
    open question 2), (d) for a `CONSOLE` file specifically, blanking its
    buffer when the result field contains the literal `ERASE`.
- **`CRT`** — output-only, cannot accept input at all. A program may
  declare at most one, and per the manual's mutual-exclusion rule, a
  program with a `WORKSTN` file cannot also declare `KEYBORD`, `CRT`, or
  `CONSOLE`.

None of this needs WORKSTN's new surface area (a second display-format
spec-file type, row/column screen layout, multi-device ACQ/REL, INFDS).
It's a much smaller runtime surface — one prompt/response pair or one
message per operation — closer in size to a single new opcode pair than to
the WORKSTN effort. Folding it into `WRKSTN_PLAN.md` would have made that
plan's already-largest-item status worse for no shared implementation
benefit; the two efforts touch different F-spec device values and don't
share codegen.

---

## 2. Relationship to the WORKSTN terminal backend

`WRKSTN_PLAN.md` §3 already designs a two-backend split (real `termios`
terminal vs. headless scripted backend for the test suite) for WORKSTN's
full-screen I/O. `KEY`/`SET`'s single-field prompt/response model is simple
enough that it likely does **not** need its own termios/ANSI escape-code
backend at all — a prompt-then-read-a-line interaction is expressible with
plain blocking stdin/stdout, no raw mode or cursor positioning required
(the "six lines of 40 characters, centered" layout is a *rendering*
detail, printable as plain text without terminal control codes). This is a
meaningfully smaller runtime lift than WORKSTN's backend.

**If WORKSTN support (`WRKSTN_PLAN.md`) lands first**, reuse its headless
scripted-backend convention (§3.2's plain-text script format, once that
open question is settled) for `KEY`/`SET`'s test fixtures too, rather than
inventing a second script format — same reasoning as this plan's device
list reusing WORKSTN's `Device` enum precedent. If this plan is implemented
**before** WORKSTN, its headless-mode design (§4 K3 below) should be built
generically enough that `WRKSTN_PLAN.md` can reuse it rather than the
other way around, since KEY/SET's is the simpler of the two models.

---

## 3. New surface area

Two runtime primitives cover both opcodes — no new spec-file type, unlike
WORKSTN's S/D-spec (`WRKSTN_PLAN.md` §2):

```
int  rpg_rt_key(const char *prompt, int prompt_len,
                 char *result_buf, int result_len, int is_numeric);
int  rpg_rt_set(const char *display_text, int display_len,
                 const int *func_keys, int func_key_count,
                 int msg_number /* -1 if none */,
                 int erase_console /* CONSOLE+ERASE form only */);
```

`rpg_rt_key` implements the numeric-field right-justify-zero-pad /
alphameric left-justify-blank-pad rule (113063-113066) and the Dup-key
"leave result field unchanged" case; `rpg_rt_set`'s message-number path
needs the `USER1` message member format nailed down first (§5 open
question 2) since nothing in the project currently has a "message member"
concept to draw on.

Backend selection follows `WRKSTN_PLAN.md` §3's precedent: an environment
variable read once at file-open time selects a real-stdin/stdout backend
vs. a headless scripted backend for the regression suite.

---

## 4. Phased breakdown

### K1 — F-spec device recognition (supersedes §0's standalone fix if not
     already applied separately)

- `fspec.h`: add `Device::Keybord`, `Device::Crt`.
- `fspec.cpp:101-108`: recognize the `KEYBORD` and `CRT` tokens.
- `fspec.cpp:119-125`'s E8 hard error: include the two new device values
  (still hard errors until K2+ land), narrowed away once the real codegen
  exists — same "hard error now, narrow later" precedent
  `WRKSTN_PLAN.md` W1 uses for `Device::Workstn`.
- Enforce the mutual-exclusion rule (a program declaring `WORKSTN` cannot
  also declare `KEYBORD`/`CRT`/`CONSOLE`, and at most one of each) as a
  parse-time check across all F-specs in the program, not per-line.
- Regression tests: `neg_keybord.rpg`, `neg_crt.rpg` (device still
  unsupported at this phase), `neg_multi_device.rpg` (mutual-exclusion
  violation).

### K2 — `KEY` operation

- `cspec.h`/`cspec.cpp`: parse `KEY` (factor 1 optional — constant,
  literal, field, table/array element; factor 2 blank; result field
  required). Validate at parse time that the file named by context (the
  program's one `KEYBORD` file — `KEY` has no explicit factor naming the
  file, per the manual's column layout, since only one `KEYBORD` file can
  exist) is actually declared `KEYBORD`, else hard error.
- `emit_key` (`codegen.cpp`): resolves factor 1 to a display string (reuse
  the existing literal/field-to-string resolution already used by O-spec
  constant output), calls `rpg_rt_key`, stores the result per the
  numeric/alphameric padding rule.
- Regression test: `tests/key_basic.rpg` against the headless backend (K3).

### K3 — Headless backend + real-terminal backend

- Headless: a plain-text script (prompt text in, response text out) read
  from a file, matching `tests/run_tests.sh`'s existing fixture
  conventions — reuse `WRKSTN_PLAN.md`'s eventual script format if that
  plan lands first (§2 above).
  This phase is what actually makes K2/K4 verifiable by the existing
  non-interactive test harness; sequence it right after K2 rather than
  batching it with K4, since K2 alone is otherwise untestable.
- Real terminal: blocking stdin read with the prompt printed to stdout;
  needs the Field Exit/Field-/Field+/Enter and Dup-key conventions mapped
  to real keyboard input (simpler than WORKSTN's function-key mapping
  problem — these are ordinary line-editing keys, not F-keys).

### K4 — `SET` operation

- `cspec.h`/`cspec.cpp`: parse `SET` (factor 1 optional display
  value; factor 2 required only for the CONSOLE+`ERASE` form, else blank;
  result field optional, `ERASE` literal recognized; cols 54-59 optional
  function-key resulting-indicator-style entries; message number parsed
  per §5 open question 2's resolved column mechanics).
- `emit_set` (`codegen.cpp`): dispatches to `rpg_rt_set`'s
  display/function-key/message/erase paths per which optional pieces are
  present (the manual allows "any one or any combination").
- Regression tests: `tests/set_display.rpg`, `tests/set_console_erase.rpg`,
  `tests/set_message.rpg` (once §5 Q2 resolves the message-number
  mechanics — otherwise defer this one sub-case with a documented gap,
  same precedent as H-spec's other parsed-but-inert columns
  (`hspec.cpp`/`hspec.h`).

### K5 — `CONSOLE`-as-record-address-file and CRT output

- `CONSOLE` used as a record-address file (supplying key values for
  keyed processing, distinct from its plain-input-file use) needs the same
  kind of care the compiler already gives record-address `DESIGNATION R`
  files (`fspec.cpp`'s hard-error precedent for that designation) —
  confirm whether the compiler's existing keyed-access codegen
  (SETLL/CHAIN/READE) can be adapted, or whether this sub-case should stay
  a documented hard error separate from K1's general CONSOLE-as-input-file
  support.
- `CRT` output: a program's O-spec lines targeting a `CRT` file reuse the
  ordinary O-spec-to-buffer machinery already in `codegen.cpp`, writing to
  `rpg_rt_set`'s display path (or a dedicated `rpg_rt_crt_write`) instead
  of a flat file — the smallest phase in this plan, since `CRT` is
  output-only with no new opcode.

---

## 5. Open questions to resolve before starting implementation

1. **Message member format** (`USER1`, referenced by `SET`'s message-number
   path, 124278-124281) — the project has no existing "message file"
   concept anywhere. Needs its own small design: a lookup file format
   (message number → text) and where it's expected relative to the source
   file, following the same "sibling file, looked up by convention" shape
   `/COPY` (`source.cpp`'s `expand_copy_statements`) and WORKSTN's `FMTS`
   (`WRKSTN_PLAN.md` §2) both already use.
2. **`SETnn`/`KEYnn` column mechanics** — the manual references a message
   ID "in columns 31 and 32 of the SET operation" (124278) but also
   describes a `SETnn`/`KEYnn` opcode-suffix convention (124278-124281);
   these two descriptions need reconciling against the actual Chapter 10
   calculation-specification column diagram (45908-46050 in
   `docs/ref/manual_text.txt`, an OCR'd column table that did not extract
   cleanly as text — re-read directly from the source PDF/manual if
   available, not from the extracted `manual_text.txt`, before
   implementing K4's message path) before K4 can parse it correctly.
3. **Screen rendering fidelity for the real-terminal backend** — the
   fixed six-line/24-line centered layouts (113078-113083) are cosmetic;
   confirm whether v1 needs to reproduce them pixel/column-exact or
   whether a simpler "print the prompt, read a line" real-terminal backend
   is acceptable for v1, deferring exact layout replication the same way
   `WRKSTN_PLAN.md` §1 defers full DDS attribute fidelity.

Question 2 blocks K4's message-display sub-feature specifically, not K1-K3
or K4's non-message paths (display, function-key, ERASE) — those can
proceed without it.
