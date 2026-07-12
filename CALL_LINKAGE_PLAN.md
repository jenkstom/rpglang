# Program-Linkage Support — Implementation Plan

**Status: planning only. No implementation has started.** This document is a
design and phasing plan for `CALL`, `PARM`, `PLIST`, `RETRN`, `EXIT`, `RLABL`,
and `FREE` — the RPG II "program linkage" family that lets one compiled
program call another, pass parameters, and call an external (non-RPG)
subroutine. Without this family, no RPG II source that calls an external
subprogram can be compiled at all; it is the largest functional gap left
after `docs/TODO.md` Groups A–F closed.

---

## 0. Why this is one plan, not seven small opcodes

`docs/TODO.md`'s C8 called this "the largest item in [the missing-opcode]
group: it needs a real design decision for how a 'call' maps onto the
LLVM/ELF model ... before implementation starts — treat as its own design
spike, not a quick fix." That's still true, and reading the manual entries in
full sharpens *why*: these seven opcodes are not independent features that
happen to share a chapter — they're one mechanism with two entry points
(`CALL`/`PARM`/`PLIST`/`RETRN` for calling another *RPG or non-RPG program*,
`EXIT`/`RLABL` for calling an external, non-RPG *subroutine* from inside a
program) that share the same parameter-passing-by-address model. Picking a
design for one half fixes the other's design too, so splitting this into
per-opcode tickets (the way Group C1-C6 worked) would produce inconsistent,
probably incompatible parameter-passing code paths.

`FREE` (111422-111466, listed under `docs/TODO.md`'s C9 as
"WORKSTN-adjacent") is not actually WORKSTN-related — its factor 2 is a
*called program name*, not a device (`WRKSTN_PLAN.md` §0 already made this
correction when scoping WORKSTN). It "removes a program from the list of
activated programs and ensures program initialization... the next time the
program is called" — meaningless without `CALL` existing first. It belongs
here, added as phase L5 below.

`RLABL` (`docs/TODO.md`'s C7) is not independently useful either — the
manual is explicit: "RLABL operations must be specified immediately after
the EXIT operation that refers to the subroutine." It's `EXIT`'s parameter
list, not a standalone opcode. It belongs here too, as part of phase L3.

`docs/TODO.md`'s C7 also listed `DEBUG` and `SET` alongside `RLABL` as
"lower priority... edge-case linkage/console operations" — that grouping was
imprecise. `DEBUG` and `SET`/`KEY` have no linkage dependency at all; they're
tracked separately in `MISC_OPCODES_PLAN.md` and `KEYBORD_PLAN.md`.

---

## 1. What "call" means in the compiler's execution model

This is open question 1 (§6) and the one decision everything else depends
on. `compiler/src` compiles one RPG II source file to one LLVM module,
linked against `runtime/rpg_runtime.c` into a single native executable
(`docs/ARCHITECTURE.md`). There is no existing notion of "another compiled
RPG program" as an object the compiler can reference — every prior feature
in the project has stayed inside a single translation unit.

Two shapes are available, evaluated against the manual's actual
requirements (multiple CALLs to the same program skip re-initialization
unless `FREE`'d; a called program can itself be "a System/36 Environment
program, a System/38 Environment program, or an AS/400 system program" —
i.e., on real hardware the callee need not even be RPG II):

- **(a) Static link, separately-compiled RPG objects.** Compile each `.rpg`
  file to a `.o` with an exported entry symbol (derived from the H-spec
  program-id, `hspec.cpp:75`) and a fixed parameter-passing ABI; `CALL`
  becomes a direct LLVM `call` to an externally-declared function; the build
  step links all referenced objects together. This mirrors real System/36
  behavior most closely and lets `RETRN`/`LR` map onto an ordinary function
  return, but requires the compiler driver to discover and compile
  dependency `.rpg` files (a `CALL` target found via a field/literal at
  runtime, not always statically known — see §2) and needs a resolution
  story for a program calling one not yet compiled.
- **(b) Dynamic dispatch through a runtime program registry.** Every
  compiled RPG program registers itself (name → entry-point function
  pointer) in a `rpg_rt_program_registry` at process startup; `CALL` becomes
  a runtime lookup-and-invoke (`rpg_rt_call(name, plist_ptr)`) instead of a
  direct LLVM call. This defers name resolution to runtime (matching the
  manual's own two-tier lookup: literal program names are resolved once via
  the "library list" and cached, field-name program names are re-resolved
  every call) and sidesteps static link-graph discovery, at the cost of an
  indirect call on every invocation and needing every potentially-called
  `.rpg` file compiled into the *same* executable (no separate .o linking).

**Recommendation: start with (b).** It's a strictly smaller first slice —
no changes to the build/link driver, no dependency-discovery pass, and it
naturally supports the manual's "first CALL initializes, subsequent CALLs
skip initialization unless FREE'd" rule as a per-entry `bool initialized`
flag in the registry. (a) is a legitimate follow-on once real multi-file
RPG "libraries" of programs are a concrete use case, and the (b) runtime
registry's `name → function pointer` shape is a subset of what (a)'s object
export/import table would need anyway, so (b) isn't wasted work if (a) is
built later. This recommendation is not yet a decision — flag to the user
before starting §3's implementation.

**Non-RPG callees (System/36/38/AS/400 environment programs) are out of
scope.** The compiler has no notion of "environment programs" and no
non-RPG language front end; a `CALL` naming a program the compiler didn't
itself compile into the executable is a hard compile-time or link-time
error, not a silent no-op.

---

## 2. Parameter passing model

The manual's rules (`PARM`, 123455-123561) are precise and must be followed
exactly, since they define observable calling-convention behavior:

- A `PLIST` groups `PARM` lines; `*ENTRY PLIST` is the special "these are my
  formal parameters" list a called program declares for itself (only one
  per program).
- Parameters are passed **by address**, not by value: "There is only one
  storage location for each parameter field. It is in the calling program
  ... What is passed to the called program ... is the address of the
  storage location of the result field." A callee mutating a parameter
  mutates the caller's field directly.
- `PARM`'s factor 1/factor 2 (optional) implement a **copy-in/copy-out
  shim** around that shared address, run automatically at four points (call
  time in caller, entry in callee, return time in callee, return time in
  caller) — see the four numbered steps at manual 123591-123613. If neither
  factor is specified, the result field alone is the by-address parameter
  with no shim.
- If the caller has more parameters than the callee's `*ENTRY PLIST`, the
  callee fails; if the callee expects more than the caller supplies, using
  the unresolved extra parameter fails. Both are runtime errors in this
  model (registry-based dispatch can't statically check this across
  independently-compiled translation units — matches real behavior, which
  is also a runtime failure).

**Design implication:** every RPG value in the compiler (scalar numeric
field, character field, array, DS) already has a stable global storage
location (`SymbolTable`, `docs/ARCHITECTURE.md`) — good, "pass by address"
is just "pass the existing global's pointer," no new storage model needed.
The copy-in/copy-out shim is the only genuinely new codegen: `PARM`'s
factor-1/factor-2 assignment logic reuses the existing MOVE/Z-ADD-style
scalar-copy codegen already in `codegen.cpp`, just triggered at four call
sites instead of one straight-line spot in the C-spec stream.

`PLIST`/`PARM`'s attribute-checking rule ("the attributes of the
corresponding parameter fields in the calling and called programs must be
the same... If they are not, undesirable results may occur") is explicitly
the manual's own words for "undefined behavior on mismatch" — the compiler
should do better where cheap: since registry dispatch (§1 option (b)) links
all called programs into the same executable, a compile-time attribute
check across the caller's PARM types and the callee's `*ENTRY PLIST` types
is possible and should be a hard error rather than silent corruption,
matching the project's consistent precedent of converting silent-wrong-
output gaps into hard errors on first implementation pass (e.g. `fspec.cpp`'s
device and record-address-designation checks).

---

## 3. Phased breakdown

### L1 — `PLIST`/`PARM`/`*ENTRY PLIST` parsing (no codegen yet)

- `cspec.h`/`cspec.cpp`: parse `PLIST` (factor 1 = list name or `*ENTRY`)
  and `PARM` (optional factor 1/factor 2, required result field with
  optional length cols 49-51/decimals col 52 the same way other C-spec
  result fields already parse them). A `PLIST` is followed immediately by
  one or more `PARM` lines with no intervening non-`PARM` operation — parse
  this as a grouping pass over the C-spec list (same shape as the existing
  `CASxx`...`END` group parsing, `codegen.cpp:2863-2884`).
  Enforce: only one `*ENTRY PLIST` per program (hard error on a second); a
  `PLIST` with zero following `PARM` lines is a hard error ("invalid" per
  the manual).
- New `Program` field: `vector<ParamList>` (name, `is_entry`, ordered
  `vector<ParmDecl>`).
- Regression test: parse-only fixture checking the `Program::param_lists`
  structure — no executable behavior yet, matching `WRKSTN_PLAN.md` W2's
  precedent of a parse-only first phase.

### L2 — Runtime program registry + `CALL`

- New `runtime/rpg_runtime.h` primitives (naming illustrative, not final —
  same caveat as `WRKSTN_PLAN.md` §3):
  ```
  void rpg_rt_register_program(const char *name, rpg_entry_fn fn);
  int  rpg_rt_call(const char *name, void **parm_ptrs, int parm_count,
                    int *out_error_ind, int *out_lr_ind);
  ```
  Every compiled program emits a constructor-style global initializer that
  calls `rpg_rt_register_program` with its H-spec program-id
  (`hspec.cpp:75`/`codegen.cpp:342-343`, already parsed and used to set the
  LLVM module identifier) at process startup, mirroring how LLVM/C++
  already supports global constructors — no new linkage mechanism needed.
- `cspec.cpp`: parse `CALL` (factor 2 required — field/literal/array
  element containing the program name, optional `LIB/PGM` slash form parsed
  but the library segment ignored, same precedent as `WRKSTN_PLAN.md` §2's
  `LIBRARY,MEMBER` FMTS handling; result field optionally names a `PLIST`).
- `emit_call` (`codegen.cpp`): resolves the named `PLIST`'s `PARM` pointers,
  calls `rpg_rt_call`, stores cols 56-57 (error) / 58-59 (callee LR) into
  the named resulting indicators if present.
- The "first CALL initializes, subsequent CALLs skip init unless FREE'd"
  rule lives in the registry entry (`bool initialized` flag, set on first
  invocation, checked by the callee's own generated entry-point prologue —
  see L4).
- Self-call and ancestor-call prevention ("An RPG program cannot call
  itself or a program higher in the program stack") needs an explicit call
  stack the registry tracks (push on `rpg_rt_call` entry, pop on return) —
  a hard runtime error on violation, not silently ignored, since the manual
  frames it as a documented restriction, not merely `UB`.
- Regression test: two-program fixture (`tests/call_basic.rpg` +
  `tests/call_basic_callee.rpg`, compiled together into one executable per
  §1's registry-dispatch model), checking a parameterless CALL runs the
  callee and returns.

### L3 — `EXIT`/`RLABL`

- `cspec.cpp`: parse `EXIT` (factor 2 required, `SUBRnnnnn`-shaped name per
  the manual's naming rule — "must consist of five or six characters, the
  first four of which are SUBR") and `RLABL` (result field = field/DS/
  array/table/indicator name, optional length/decimals cols 49-51/52,
  columns 9-17/18-27/33-42/53/54-59 all enforced blank per the manual's
  explicit column table). Group consecutive `RLABL` lines under their
  preceding `EXIT`, same grouping-pass shape as L1's `PLIST`/`PARM`.
- This is conceptually `CALL`+`PARM` with two differences the manual
  spells out precisely, both of which are new codegen, not reuse:
  1. The callee is **not** RPG — the compiler has no way to *generate* an
     "external subroutine," only to *call* one. In practice this means
     `EXIT`'s target must be an externally-linked C-ABI-compatible symbol
     (same idea as `runtime/rpg_runtime.c`'s existing extern "C" functions)
     that the build links in separately; the compiler cannot exercise
     `EXIT` end-to-end against a hand-written stub without a documented C
     ABI for the parameter block described next.
  2. `RLABL` passes an **extra trailing parameter**: an array (one element
     per `RLABL`) whose element structure is fully specified by the manual
     (123972-124056) — 1-byte type char (`'C'`/`'Z'`), 4-byte zoned length,
     2-byte zoned decimal count, 4-byte zoned array-element-count. `emit_exit`
     must synthesize this attribute array from the `RLABL` list's resolved
     `FieldInfo`s (the compiler already carries length/decimals per field;
     only the zoned-encoding of these four sub-values into the array
     buffer is new).
- Regression test: `tests/exit_rlabl.rpg` against a small hand-written C
  stub in `tests/` (or `runtime/`) that reads the attribute array and one
  scalar `RLABL`'d field, matching manual Figure 307's own worked example
  (`EXIT SUBRA` / `RLABL HERE`).

### L4 — `RETRN` and callee-side `*ENTRY PLIST` wiring

- `cspec.cpp`/`codegen.cpp`: parse and emit `RETRN` — check halt indicators
  first (abnormal-end path, close files, set an error return code the
  caller's `CALL` reads into its 56-57 resulting indicator), else check LR
  (normal end), else fall through as an early return to the caller. This is
  the callee-side mirror of L2's `rpg_rt_call` — `RETRN` needs to actually
  unwind to the point `rpg_rt_call` invoked from, which in the registry
  model (§1(b)) is a plain C-style early `return` from the callee's
  generated entry function, so this is small once L2's call/return
  boundary exists.
- Wire `*ENTRY PLIST`'s `PARM` list as the callee's real parameter-receiving
  prologue: the callee's generated entry function takes `void **parm_ptrs`
  (from L2) and its first block copies each pointer into the matching
  `*ENTRY` `PARM`'s factor-1 receiver per the manual's step 2
  (123591-123594), symmetric with L2's caller-side step-1 copy.
  "Fields specified as parameters in an *ENTRY PLIST can be used at
  first-page (1P) time" — verify the compiler's existing 1P/first-cycle
  codegen path can see `*ENTRY` parameters that early; if 1P codegen runs
  before the parameter-copy prologue currently would, the prologue needs to
  move earlier, not the rule ignored.
- Regression test: extend L2's two-program fixture with real `PARM` values
  flowing in both directions (caller sets a value, callee mutates it,
  caller's `RLABL`/`PARM` factor 2 confirms the mutation is visible after
  return) plus a `RETRN`-without-LR early-return case.

### L5 — `FREE`

- `cspec.cpp`: parse `FREE` (factor 2 required, same
  field/literal/array-element-containing-a-program-name shape as `CALL`'s
  factor 2; optional 56-57 resulting indicator for "not successful").
- `emit_free` / `rpg_rt_free(name)`: clears the named registry entry's
  `initialized` flag (does **not** close files — the manual is explicit:
  "It does not close files"). Small once L2's registry exists; this is why
  `FREE` is sequenced last rather than folded into L2 — it's inert until
  there's an `initialized` flag to clear.
- Regression test: `tests/free_reinit.rpg` — two `CALL`s to the same
  program with a `FREE` between them; some observable first-cycle-only
  side effect (e.g. a 1P-conditioned calc) should fire again on the second
  `CALL`, proving re-initialization actually re-ran.

---

## 4. Dependency graph

```
L1 (PLIST/PARM parsing) ──┬──> L2 (registry + CALL) ──┬──> L4 (RETRN + *ENTRY wiring) ──> L5 (FREE)
                           │                            │
                           └──> L3 (EXIT/RLABL) ────────┘
```

L1 is the shared prerequisite (both the `CALL` and `EXIT` halves consume
parsed `PLIST`/`PARM`/`RLABL` declarations). L2 and L3 can proceed in
parallel once L1 lands — they're the two independent "entry points" noted
in §0. L4 needs L2's call/return boundary defined. L5 needs L2's registry
to exist at all.

---

## 5. Risk / effort notes

- **§1's registry-vs-static-link decision is the load-bearing call for this
  entire plan** — every other phase assumes registry dispatch (option (b)).
  If a future need for genuinely separate compilation units emerges (e.g. a
  shared library of common subprograms compiled once, reused across many
  RPG programs without recompiling them together), (a) would need
  revisiting from scratch; that risk is accepted deliberately here to keep
  the first slice small, per the recommendation in §1.
- **L3 (EXIT/RLABL) is the one phase the project cannot fully verify
  end-to-end without a hand-written non-RPG stub**, since the compiler has
  no second language front end to generate the "external subroutine" side
  itself. Budget for writing that C stub as test fixture work, not just RPG
  fixtures.
- **Compile-time attribute checking (§2's closing paragraph) is a
  deliberate deviation from "undesirable results" (i.e. undefined
  behavior) toward the project's established pattern of loud errors over
  silent corruption** — consistent with `docs/TODO.md`'s A-group fixes, but
  worth flagging since it's stricter than the real System/36 ever was.

---

## 6. Open questions to resolve before starting implementation

1. **Registry dispatch vs. static link** (§1) — this plan recommends
   registry dispatch (all called programs compiled into one executable) as
   the smaller first slice. Confirm before starting L2; changing this later
   means redoing L2, L4, and L5's runtime-call-boundary code.
2. **Multi-program build invocation** — if a source file's `CALL` targets
   are compiled together, does the compiler driver take multiple `.rpg`
   files on one command line, or does it need a manifest/project file
   listing "this program plus everything it calls"? Not addressed above;
   affects `main.cpp`'s CLI surface, not the codegen phases themselves.
3. **`EXIT`/`RLABL`'s C ABI** — what calling convention and header does this
   project publish for hand-written "external subroutines" to target? Needs
   a real decision (a `rpg_ext_subr.h`-style header declaring the parameter
   attribute array layout from §L3) before L3's test fixture can be
   written, not just before "production use."

This plan should not move to implementation until question 1 is settled —
it is the one decision that reshapes every phase below it, the same
lesson `WRKSTN_PLAN.md` §8 draws for its own open questions.
