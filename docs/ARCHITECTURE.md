# RPG II → LLVM Compiler — Architecture

> **Who this is for:** developers who want to understand how `rpgc` works
> internally — extending the compiler, debugging generated IR, or adding new
> opcodes. No prior LLVM knowledge is required to follow the high-level flow;
> the IR-level sections assume familiarity with LLVM's basic concepts (modules,
> basic blocks, SSA values).

This document describes the design of `rpgc`, the RPG II to LLVM compiler. It
is the canonical reference for how source constructs map to LLVM IR, with
particular attention to **indicators** (the most unusual part of RPG semantics)
and the **implicit RPG program cycle**. For the column-position reference, see
`docs/SPEC_MAP.md`; for build instructions, see `docs/BUILDING.md`.

The design is staged across ten phases; sections are marked **[Phase N]**
to indicate when each mechanism comes online.

---

## 1. Big picture

```
   .rpg source
        │
        ▼
 ┌──────────────┐   /COPY splicing +    ┌──────────────┐
 │  Preprocess  │ │  Auto Report *AUTO   │  expanded    │
 │  (source.cpp │─▶  expansion (rewrites │  SourceLines │
 │   autoreport)│    the line vector)    │  (F/I/C/O)   │
 └──────────────┘                       └──────┬───────┘
                                               │ fixed-column
                                               ▼  slicing (no whitespace tokens)
                                       ┌──────────────┐
                                       │  Lexer +     │   AST-ish IR
                                       │  parsers     │   (rpgc::Program)
                                       └──────┬───────┘
                                              │
                                              ▼
                                       ┌──────────────┐
                                       │ CodeGen      │   LLVM C++ API
                                       │ (IRBuilder)  │   (libLLVM)
                                       └──────┬───────┘
                                              │ llvm::Module
                          ┌───────────────────┼────────────────────┐
                          ▼                   ▼                    ▼
                   textual .ll          object (.o)           final ELF
                   (--emit-ir)          (--emit-obj, llc)     (--emit-exe,
                                                              clang + runtime)
```

* **Frontend language:** C++17, chosen for direct access to the canonical,
  best-maintained LLVM bindings (the C++ API itself).
* **Linkage:** the `rpgc` binary links against `libLLVM-19` via the LLVM CMake
  package (`llvm_map_components_to_libnames`). Components used: `core`,
  `support`, `irreader`, `analysis`, `codegen`, `target`, `native`, `passes`.
* **Backend driver:** in `--emit-exe` mode `rpgc` shells out to `llc`
  (IR → object) and `clang` (object + `librpgruntime.a` → ELF). Both paths are
  resolved at configure time and baked in as defaults.

### 1.1 Source preprocessing

Before the column lexer sees a line, the raw source (`std::vector<SourceLine>`)
passes through two source-to-source transforms (in `compiler/src/main.cpp`,
mirrored in `analyze/src/ir.cpp`), each of which rewrites the line vector in
place:

1. **`/COPY` expansion** (`compiler/src/source.cpp`, `expand_copy_statements`)
   splices a named library member's lines into the vector, recursively, before
   any column parsing.

2. **Auto Report expansion** (`compiler/src/autoreport.cpp`,
   `expand_autoreport`) consumes a leading form-type `U` option spec and
   expands its `*AUTO` constructs — `H-*AUTO` page headings, `D/T-*AUTO`
   detail/total output lines, and the accumulator C-spec chain — into ordinary
   F/I/C/O-spec text. The generated lines are plain RPG source, so every
   downstream parser, the codegen, and the analyzer consume them unchanged;
   nothing after this point needs to know Auto Report was involved. A program
   with no `U` line passes through unchanged. `--dump-autoreport` prints the
   expanded source and exits. See `docs/AUTO_REPORT_PLAN.md` for the full
   design.

`rpg-analyze` runs the same two transforms (`analyze/src/ir.cpp`), so its
analysis reflects the post-expansion program a user actually gets.

---

## 2. The column-based parser  [Phase 2+]

RPG II source is **not** tokenized by whitespace; meaning lives in fixed
columns. The lexer therefore works on whole lines:

| Cols | Spec | Meaning                                   |
|-----:|------|-------------------------------------------|
|  6   | all  | spec type: `F` `I` `C` `O`                |
| 7–14 | F    | filename                                  |
| 19   | F    | file format (`F`=fixed)                   |
| 32–37| F    | record length                             |
| 40   | F    | device (`DISK`/`PRINTER`/`WORKSTN`)       |
| 9–17 | C    | **control-level / resulting indicators**  |
| 18–27| C    | factor 1 / factor 2                       |
| 28–32| C    | operation                                 |
| 33–42| C    | result field                              |
| 43–48| C    | length / decimal positions                |
| 54–59| C    | resulting indicators (HI/LO/EQ)           |

The parser slices each line into `std::string_view` substrings at these
offsets and stores them in a `SpecTable`. Comments are lines whose column 7
is `*` (after the spec-type column).

---

## 3. Indicators → LLVM  [Phase 2]

### 3.1 What an indicator is in RPG

RPG indicators are a **flat array of 99 boolean latches** (01–99) plus a set of
special named ones (`LR` last-record, `L1`–`L9` control-level, `OF`/`ON`
file-record, `H1`–`H9` halt). They are:

* **global** — visible from every C-spec,
* **persistent** across cycle iterations until explicitly turned off,
* usable as **conditions** on C-specs (columns 9–17: `N` prefix means negated),
* set as **results** of operations (columns 54–59: HI/LO/EQ on `COMP`, etc.),
* and as **results of I-spec matching** (e.g. field-blank → indicator).

This combination — global, mutable, used as both condition and result — is the
defining quirk of RPG semantics.

### 3.2 Mapping decision (implemented in Phase 2)

We model indicators as **two global LLVM values**:

```
@rpg_in  = internal global [100 x i1] zeroinitializer   ; slots 1..99
@rpg_lr  = internal global i1 false                     ; last-record latch
```

The `[100 x i1]` array holds the 99 general indicators (`01`–`99`); slot 0 is
unused. The special `LR` indicator gets its own `i1` global. Other specials
(`L1`–`L9`, `H1`–`H9`, overflow, function-key) are accepted lexically and will
be promoted to their own globals as later phases need them.

* **Why an array, not one global per indicator?** RPG code addresses them
  *numerically* (`SETON  47`), so a contiguous array lets us index by a runtime
  value if ever needed, and keeps `SETON`/`SETOFF` a single store instruction
  parameterised by index. The actual generated IR confirms this:
  `store i1 true, ptr getelementptr inbounds (@rpg_in, 0, 47)`.
* **Why `i1` and not `i8`?** Indicators are pure booleans; LLVM will keep them
  in registers as `i1` and there is no need to mask. `zext i1 -> i8`/`i32` is
  one instruction when we need to print or return them.
* **Conditioning**: each AND-chain of conditioning indicators (cols 9–17) is
  evaluated to a single `i1` in the spec's block, then a single `br i1`
  gates the op body — exactly as sketched in §3.3.

### 3.3 Generated IR for indicator operations

| RPG construct                | Generated IR (sketch)                          |
|------------------------------|------------------------------------------------|
| `SETON  47`                  | `store i1 true, i1* getelementptr(@rpg_in, 47)`|
| `SETOFF 47`                  | `store i1 false, ...`                          |
| `C 47  ADD  A B C`           | `br i1 %ind47, label %do_add, label %skip`     |
| `C N47 ADD  A B C`           | `br i1 %not47, label %do_add, label %skip`     |
| `COMP A B  10 11`            | compare + three stores into slots 10/11        |
| `LR` (end of cycle)          | `store i1 true, ...special[LR]`                |

So: **indicator reads are `load i1`; indicator conditions are `br i1`; indicator
writes are `store i1`.** No basic-block gymnastics beyond the natural branch
per conditioned C-spec. Each conditioned C-spec may split into two blocks
(`do` / `skip`), but the LLVM optimizer collapses trivial chains.

### 3.5 GOTO / TAG and basic blocks  [Phase 4]

`GOTO`/`TAG` are the one place the codegen can't be purely linear, because a
`GOTO` may branch forward to a `TAG` that hasn't been emitted yet. The emitter
resolves this with a **two-pass** `emit_spec_chain`:

1. **Pass 1** scans the specs in the chain and pre-creates one LLVM basic block
   per `TAG`, stored in a `label → BasicBlock*` map.
2. **Pass 2** emits bodies. A `TAG` wires its predecessor into the pre-created
   block and continues from it; a `GOTO` emits an unconditional `br` to the
   block looked up in the map (resolving the forward reference).

Because every `TAG` block exists before any body is emitted, forward and
backward `GOTO`s both work. The parser enforces that every `GOTO` target has a
matching `TAG` (a missing one is a compile-time error, per the manual). The map
is cleared per chain so detail and total chains don't accidentally cross-
resolve.

### 3.6 Structured operations  [Phase 6]

`IFxx`/`ELSE`/`END` and `DOWxx`/`DOUxx`/`END` are handled inside the same pass-2
loop as GOTO/TAG, using a stack of frames so they nest. Each frame records the
exit (merge) block, and for loops the header/body for back-edges:

* **`IFxx`**: evaluate F1 cmp F2; `condBr` to the then-block or a false-target
  block. `ELSE` repoints the false-target into the else body; `END` branches
  then/else to the merge. If there's no `ELSE`, the false-target just flows to
  merge.
* **`DOWxx`** (test-at-top): a header block tests the condition each iteration;
  false jumps to exit. The body loops back to the header. Body may run 0 times.
* **`DOUxx`** (test-at-bottom): the body is entered unconditionally; at `END` a
  test block checks the condition — true exits, false loops back to the body
  top. Body runs at least once.

The `xx` operators (GT/LT/EQ/NE/GE/LE) compile to `icmp` with signed predicates
for numeric operands.

**MVR across blocks**: `DIV`'s remainder must reach a following `MVR`, but
inside an `IF`/`DO` body the two ops land in different basic blocks and the SSA
remainder value wouldn't dominate the `MVR`'s block (the verifier rejects it).
The fix: `DIV` stashes the remainder in a hidden global `@rpg_divrem`, and
`MVR` loads it back — sidestepping dominance entirely. The parser enforces that
`MVR` immediately follows a `DIV`.

### 3.7 Control levels & subroutines  [Phase 8]

**Control levels (L1–L9).** Each control level has its own `i1` global
(`@rpg_l1` … `@rpg_l9`), alongside `@rpg_lr`. The cycle now:
1. Turns **off** L1–L9 at the top of each iteration (step 6).
2. After extracting fields, compares each control field (declared on the I-spec
   cols 59–60) to a stored "previous value" global. The highest level whose
   field changed sets the break; the code cascade-stores L1..Ln on (e.g. an L3
   break sets L1, L2, L3).
3. On the **first** record, total time is skipped (a first-record flag gate).
4. Total time runs the calc chains in ascending level order — L0, L1, …, L9 —
   each gated on its level indicator being on, then total output. Detail time
   follows.
5. At **EOF/LR**, all of L1–L9 and LR turn on; total time re-runs L0…L9 then LR,
   so the final control group's subtotals print before the grand total.

An `Ln`-conditioned C-spec chain runs when Ln is on — which, due to the cascade,
covers any break at Ln or higher.

**Subroutines.** Each `BEGSR`…`ENDSR` group compiles to a separate
`void @sub_NAME()` function. Subroutines share the program's globals (fields,
indicators), so they take no arguments; `EXSR` compiles to a `call`, `ENDSR` to
`ret void`. Subroutine specs are excluded from the normal detail/total chains.
No recursion is enforced at compile time (RPG forbids it), and GOTO cannot
cross the subroutine boundary.

### 3.4 The cycle-test block  [Phase 3]

At the top of each loop iteration the generated code tests the control-level
and record indicators and branches into the appropriate total/detail calc
blocks. Section 4 shows where these blocks sit.

---

## 4. The implicit RPG cycle  [Phase 3 — implemented]

Every RPG II program with a primary input file runs an implicit outer loop.
`rpgc` emits a `main` whose body is the cycle; the user's C-specs become bodies
of blocks inside it. Phase 3 implements a **simplified cycle**: one primary
input file, no control levels, detail calculations per record, and LR total
calculations at end-of-file. Control-level (L1–L9) detection and total-output
(O-specs) arrive in later phases.

The generated `main` (real output from `tests/cycle.rpg`):

```llvm
define i32 @main() {
entry:
  %file_id = call i32 @rpg_rt_open_input(ptr @fname)
  call void @rpg_rt_set_reclen(i32 %file_id, i32 80)
  br label %cycle.head

cycle.head:                                     ; <- loop back here each record
  %got_rec = call i32 @rpg_rt_read_next(i32 %file_id, ptr @rpg_rec, i64 81)
  %have_rec = icmp ne i32 %got_rec, 0
  br i1 %have_rec, label %extract, label %lr.total

extract:                                        ; map record columns -> fields
  %AMT_in = call i64 @rpg_rt_get_decimal(ptr @rpg_rec, i32 80, i32 1, i32 3)
  %AMT_i32 = trunc i64 %AMT_in to i32
  store i32 %AMT_i32, ptr @rpg_AMT
  br label %detail.calcs

detail.calcs:                                   ; C-specs with blank control level
  ... ADD AMT into RPGRET ...                   ; runs every record
  br label %cycle.head

lr.total:                                       ; EOF reached
  store i1 true, ptr @rpg_lr                    ; turn on LR
  ... LR-conditioned C-specs ...
  br label %exit

exit:
  call void @rpg_rt_close_all()
  %exit_byte = and i32 (load RPGRET), 255       ; RPGRET test hook
  ret i32 %exit_byte
}
```

This mirrors the manual's 11-step cycle at the level Phase 3 supports:

| Manual cycle step | Phase 3 implementation |
|-------------------|-------------------------|
| read record (step 3) | `rpg_rt_read_next` at `cycle.head` |
| EOF → set LR (steps 10/27) | branch to `lr.total`, `store i1 true @rpg_lr` |
| extract fields (step 10) | `rpg_rt_get_decimal` per I-spec field at `extract` |
| detail calculations (step 11) | detail C-specs (blank cols 7–8) |
| LR total calculations (step 19) | `lr.total` block runs LR C-specs |
| close & end (step 38/40) | `rpg_rt_close_all` then `ret` |

**Deferred to later phases:** control-level break detection (L1–L9, steps 4–5),
total calculations at non-LR control breaks, and total/detail output (O-specs).
A program **without** a primary file still uses the Phase 2 linear form
(C-specs run once in order).

### Record format (porting decision)

The RPG manual is silent on physical record layout (real System/36 files are
OS-managed record files). For Linux, `rpgc` treats a sequential DISK file as
**fixed-length records** of the F-spec's record length, where a newline fills
out the rest of the record:

* bytes are read up to `reclen`; a `\n` (or `\r`/`\r\n`) before that is
  consumed and the remainder is space-padded,
* a line longer than `reclen` is **split** across successive records,
* an exactly-full record followed by a newline consumes that one newline
  (so an 80-column card image terminated by `\n` is one record, not two),
* immediate EOF with no bytes read is end-of-file.

Numeric fields are decoded as **plain ASCII digits** (`strtol`-style); the
EBCDIC zoned-decimal sign zone does not apply to ASCII text.

### WORKSTN backend (porting decision)  [WORKSTN support — implemented]

Same category of porting decision as the DISK record format above: the
manual assumes a real 5250 display station and SDA-authored display formats,
neither of which exist on Linux. Two pieces stand in for them:

* **Display formats** are authored as a project-specific `.dspf` text file
  (no DDS/SDA equivalent existed to be compatible with) parsed at compile
  time by `compiler/src/sspec.cpp`/`dspec.cpp` for validation and reclen
  computation, and independently at *runtime* by a small duplicate parser in
  `runtime/rpg_runtime.c` (the C runtime doesn't link the compiler's C++
  frontend, so it can't share that parser) — both backends need the field
  layout to know where on screen to render which buffer bytes.
* **The terminal itself** is a Linux tty driven with ANSI/VT100 escapes
  (cursor positioning, SGR color/reverse/blink), with a second **headless**
  backend for the non-interactive test harness (`RPG_WORKSTN_MODE=headless`,
  a line-oriented script + screen dump, see `docs/SPEC_MAP.md`'s WORKSTN
  section for the full contract). Backend selection is one environment
  variable read once at `rpg_rt_ws_open` time — see `runtime/rpg_runtime.h`.

The record-identification mechanism needed **no new code at all**: the
manual's own worked example (Chapter 7, Figure 59) shows a WORKSTN record's
type identified by an ordinary record-identification code (I-spec cols
21–41) matched against a byte the display format itself embeds as a
literal at a fixed buffer position — exactly `ispec.cpp`'s existing DISK
mechanism (`eval_record_code_match`/`emit_record_selection`), reused
unchanged. This was the biggest open design question going in, and turned
out to not be a new mechanism at all.

---

## 5. Data representation  [Phase 2–4]

* **Numeric fields** (`ADD`/`SUB`/`MULT`/`DIV`/`Z-ADD` operands) are unpacked
  from their column positions into `i32` globals in Phase 2 (`i64` + decimal
  scaling arrives in Phase 4 with `MULT`/`DIV`). RPG II packed/zoned decimal
  is handled by the runtime on read; inside the generated code everything is
  plain integer arithmetic. The result length + decimal-places columns drive
  the truncation/scaling back into the stored field.
* **Integer literals** in factor1/factor2 (`ADD 5 R`) become `ConstantInt`s,
  so `ADD`/`Z-ADD` against constants incur zero memory traffic for the operand.
* **Character fields** become `[N x i8]` globals/locals; `MOVE` is a `memcpy`
  (possibly with padding/truncation).
* Field lifetimes are global for Phase 2 (the simplest correct semantics);
  locality is left to the LLVM optimizer.

### Test hook: `RPGRET`  [Phase 2]

To make arithmetic results observable without file I/O (which lands in Phase
3), `@main` returns:

* the **low byte of a field named `RPGRET`** (`and i32 %v, 255`) if the program
  defines one, so a computation like `Z-ADD 40 RPGRET` / `ADD 2 RPGRET` yields
  exit code 42; otherwise
* the **LR latch** (`zext i1 @rpg_lr to i32`), so `SETON LR` yields exit code 1.

This is a test affordance only; real RPG programs signal completion via LR and
produce output through O-specs (Phase 3+).

---

## 6. Runtime library (`librpgruntime.a`)  [Phase 3 — implemented]

A small C library the final ELF links against. Current responsibilities:

| Function              | Role                                                   |
|-----------------------|--------------------------------------------------------|
| `rpg_rt_open_input`   | open a sequential text file, return a file id          |
| `rpg_rt_set_reclen`   | declare the fixed record length for a file             |
| `rpg_rt_read_next`    | fetch the next fixed-length record (newline-tolerant)  |
| `rpg_rt_get_decimal`  | decode ASCII-digit columns of a record into an integer |
| `rpg_rt_open_output`  | open a sequential output file (truncated)              |
| `rpg_rt_line_begin`   | start an output line buffer of N spaces                |
| `rpg_rt_line_put_str` | place a string right-justified to an end position      |
| `rpg_rt_line_put_num` | place a numeric value as decimal, right-justified      |
| `rpg_rt_emit_line`    | write the current line + N trailing newlines           |
| `rpg_rt_close_all`    | close every open file (called on LR)                   |

It is deliberately **C, not C++**, so the final link with `clang` needs no
C++ runtime beyond what `rpgc`'s own generated code uses (none — generated code
is pure LLVM IR with C linkage).

---

## 6.1 Output (O-specs)  [Phase 7 — implemented]

O-specs describe what a program writes. A record line (type H/D/T) starts an
output line; field lines name fields or quoted constants and their end
positions. The generated code builds each line through the runtime's
line-buffer API:

```
call rpg_rt_line_begin(132)            ; clear the line buffer to spaces
call rpg_rt_line_put_str("Amount =", 8, 20)  ; right-justify to col 20
call rpg_rt_line_put_num(%amt, 30)     ; numeric field to col 30
call rpg_rt_emit_line(%report_fid, 1)  ; write + 1 trailing newline
```

Each output record line is gated by its conditioning indicators (a conditional
branch skips the whole line if the indicators are off). Timing in the cycle:
* **D** lines emit after detail calculations, once per record.
* **T** lines emit at LR total time (and will also fire on control breaks once
  L1–L9 land).
* **H** (heading) lines and the 1P first-page indicator are deferred.

A PRINTER file maps to a plain text file named after the F-spec filename.

---

## 7. Code-generation & linking pipeline  [Phase 5 — implemented]

`--emit-exe` (the default) drives the full chain from inside `rpgc`:

1. Build the `llvm::Module` in memory (codegen).
2. (Optional) run the LLVM new-pass-manager optimization pipeline at `-O<level>`
   (`PassBuilder::buildPerModuleDefaultPipeline`).
3. Write the module to a temp `.ll` (in a unique temp dir).
4. Invoke `llc-19 -filetype=obj -relocation-model=pic` → relocatable object.
5. Invoke `clang-19 -no-pie <obj> -o <out> -L<libdir> -lrpgruntime` → ELF.
6. Clean up temp files unless `-v` / `--save-temps`.

`--emit-ir` stops after step 2 (the `.ll` is the output). `--emit-asm` runs
`llc -filetype=asm`; `--emit-obj` runs `llc -filetype=obj`. Each early stop is
useful for debugging and the test harness.

Two portability details baked into the driver:
* `-relocation-model=pic` on `llc`: the optimizer at `-O2`+ merges string
  globals into `.rodata.str*`, and Ubuntu's default PIE linker requires PIC
  objects to relocate them.
* `-no-pie` on the link: RPG programs are standalone executables; classic
  non-PIE linking matches the batch-program model and sidesteps the PIE/PIC
  mismatch entirely.

One correctness note on the optimizer: the `-O2`/`-O3` default pipelines run
passes (GlobalDCE, function deletion) that remove IR elements while pointers to
them are still cached in the analysis managers. The driver therefore calls
`clear()` on all four analysis managers after the pipeline runs, before they go
out of scope — without this, the manager destructors hit a use-after-free.

---

## 8. Phase roadmap

* **Phase 1 — Environment & boilerplate** *(done)*: project skeleton, CMake +
  LLVM linkage, CLI that accepts a file, runtime stub, smoke test.
* **Phase 2 — C-spec math & indicators** *(done)*: column-based parser,
  `ADD`/`Z-ADD`/`SETON`/`SETOF`, `[100 x i1]` indicator array + LR latch,
  conditioning indicators (cols 9–17) and resulting indicators (cols 54–59),
  `--emit-ir`/`--emit-exe` driver. Verified by four compile-and-run tests.
* **Phase 3 — RPG cycle & I/O** *(done)*: F-spec + I-spec parsing, the implicit
  cycle (fetch → extract fields → detail calcs → LR total → close), fixed-
  length sequential file reading and ASCII numeric decoding in the runtime.
  Verified by a sum-across-records test.
* **Phase 4 — Full C-specs** *(done)*: `COMP` (three-way compare, HI/LO/EQ),
  `GOTO`/`TAG` (forward and backward branches), `MOVE`/`MOVEL`. GOTO/TAG uses
  a two-phase block emitter so forward references resolve. Verified by three
  compile-and-run tests.
* **Phase 5 — Codegen & linking** *(done)*: `llc`-based object/assembly
  emission, the LLVM new-pass-manager optimization pipeline (`-O0`–`-O3`),
  `clang` linking to a final ELF with the runtime, `--save-temps`, robust
  error handling, and a combined cycle+COMP+GOTO end-to-end test.
* **Phase 6 — Arithmetic & structured ops** *(done)*: `SUB`/`MULT`/`DIV`/`MVR`
  (completing the arithmetic set), and the structured operations `IFxx`/`ELSE`/
  `END`, `DOWxx`/`DOUxx`/`END` (with the `xx` operators GT/LT/EQ/NE/GE/LE).
  Structured ops nest via a block stack in `emit_spec_chain`; DOW tests at the
  top, DOU at the bottom. MVR uses a hidden global to dodge SSA dominance
  issues across basic blocks inside IF/DO bodies.
* **Phase 7 — O-specs / output** *(done)*: O-spec parsing (record lines with
  type H/D/T, field lines with field names/quoted constants and end positions),
  runtime line-buffer output (`open_output`, `line_begin`, `line_put_str/num`,
  `emit_line`), and codegen that emits D output at detail time and T output at
  LR total time. Verified by a report test with detail lines + grand total.
* **Phase 8 — Control levels & subroutines** *(done)*: L1–L9 control-break
  detection in the cycle (cascade-set on break, all-on at LR, ascending-level
  total-time ordering), and `EXSR`/`BEGSR`/`ENDSR` subroutines compiled as
  separate LLVM functions sharing the program's globals. Verified by a
  control-break report (subtotals + grand total) and a subroutine-call test.
* **Phase 9 — Arrays, character fields & built-ins** *(done)*: alphanumeric
  fields (I-spec col 52 blank → `[N x i8]` globals) with character MOVE/MOVEL
  (right/left-justified byte copy) and character output; E-spec numeric arrays
  (compile-time data via `** ` records, and run-time) with `ARR,INDEX` element
  access; `XFOOT` (array sum) and `SQRT`. Verified by character-field, array,
  and XFOOT tests.
* **Phase 10 — String ops, search & formatting** *(done)*: alphanumeric `COMP`
  (character comparison via the runtime), `LOKUP` (array search with HI/LO/EQ
  indicators and index update), `MOVEA` (left-justified byte move), and O-spec
  edit codes (`1`–`4`, `J`–`Q`) for comma/decimal/sign number formatting.
  Verified by char-COMP, LOKUP, MOVEA, and edit-code tests.
