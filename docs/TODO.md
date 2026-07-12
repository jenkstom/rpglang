# RPG II Compiler — Finish-Out Todo List

This replaces the previous version of this file, which declared "every item in
the TODO list is now closed." That was true of the original A–H item list, but
a systematic audit against the actual reference manual (SC09-1818-00,
`docs/ref/manual_text.txt`) — five chapter-scoped passes cross-reading the
manual against `compiler/src` and `runtime/rpg_runtime.c` — found that several
"done" areas compile clean but produce silently wrong output, and that two
whole manual chapters have no implementation and were never mentioned in this
file or in `docs/ARCHITECTURE.md`/`docs/SPEC_MAP.md`.

Items below are grouped by kind and roughly ordered by real-world impact
within each group. Each item cites the manual line range that establishes the
correct behavior and the source location that needs to change. `[BUG]` means
the code runs and produces a wrong result with no diagnostic; `[MISSING]`
means the manual defines a construct the compiler has no code path for at
all; `[DEVIATION]` means implemented but diverging from the documented rule,
generally at lower real-world frequency than the BUG items.

---

## A. Critical correctness bugs — fix first

These are in code paths nearly every nontrivial RPG II program exercises.
None of them fail to compile or crash; they silently produce wrong output or
take the wrong branch.

- [ ] **A1. Edit-coded output fields overwrite everything printed earlier on
      the line.** `codegen.cpp:1670-1676` passes the field's absolute output
      column (`end_pos`) as the *width* argument to `rpg_rt_edit_dec`
      (`runtime/rpg_runtime.c:661`), which left-pads the formatted number out
      to that many characters; `rpg_rt_line_put_str`/`place_right`
      (`rpg_runtime.c:977-996`) then right-aligns the whole padded buffer to
      `end_pos`, so it starts writing at column `end_pos - width = 1` —
      blanking columns 1 through the field. Reproduced with the manual's own
      "label constant + edit-coded amount" pattern (Ch. 16, Figures 166–168):
      the label vanished. Fix: pass the formatted string's actual length, not
      `end_pos`, as `width` (mirror what `emit_edit_word_field` already does
      correctly for edit words).

- [ ] **A2. Numeric comparisons don't align decimal points.**
      `eval_cmp_op` (`codegen.cpp:2852-2870`) — shared by `COMP`, `IFxx`,
      `DOWxx`, `DOUxx`, and `CASxx` — runs a raw `icmp` on the two operands'
      scaled-integer storage with no rescale to a common decimal point
      (manual 104817-104822 requires alignment at the implied decimal point).
      A 2-decimal field holding 1.50 vs. a 0-decimal field holding 2 compares
      as `150 > 2` (true) instead of `1.50 < 2`. Fix: reuse the
      `operand_decimals`/`scale_to` helpers already used by ADD/SUB before
      the `ICmp`.

- [ ] **A3. AND/OR record-identification continuation lines break field
      routing.** `ispec.cpp:79-128` sets `current_file` from cols 7–14
      *before* checking whether the line is an AND/OR continuation (which has
      blank cols 7–14 by definition), so the continuation-merge check
      compares an empty string against the real filename and never matches.
      The code-set is pushed as a spurious empty-named record type, and
      `current_file` is left blank for any field lines that follow — breaking
      `fld.file` association used by the multifile cycle
      (`codegen.cpp:1135-1143,1375`) and CHAIN/READ field decode
      (`codegen.cpp:3253`). No test exercises this. Fix: check for the
      AND/OR relation and merge into `out.records.back()` *before*
      overwriting `current_file`, and only update `current_file` on a real
      (non-continuation) record-id line.

- [ ] **A4. External/halt/function-key indicators silently vanish from
      AND-condition groups instead of evaluating false.** `cspec.cpp:32-56`
      (`ind_token`) returns `0` for `U1`–`U8`, `H1`–`H9`, `KA`–`KY`;
      `parse_conditions` (`cspec.cpp:61-78`) only appends a condition when the
      index is nonzero, so a calc conditioned solely on e.g. `U1` compiles as
      **unconditional** rather than gated on a switch that defaults off. This
      is a semantic inversion, not a no-op. Fix: give these indicators real
      backing globals (or at minimum a stable "always off unless set" store)
      so they participate in conditioning like any other indicator, instead
      of being dropped from the AND-list.

- [ ] **A5. Edit codes 1–4 print `CR` on negative values; comma pairing is
      inverted for 2/3; codes A–D never get commas at all.**
      `runtime/rpg_runtime.c:668-681`: `sign_style` defaults to CR-on-negative
      and is only overridden for J–Q, so codes 1–4 (which the manual defines
      as unsigned — Table 8) print a trailing `CR` anyway. The comma flag is
      set for codes `'1'`/`'3'` where the manual pairs commas with `1`/`2`
      (verified by direct output comparison — code `2` prints no comma when
      it should, code `3` prints one when it shouldn't). Codes `A`–`D` are
      never remapped into the comma-aware switch and fall to `default`
      (`use_comma=0`), so `A` and `C` render identically, as do `B` and `D`.
      Fix: rebuild the edit-code table from Table 8 (manual 62103-62330) —
      comma set = `{1,2,A,B}`, no-comma set = `{3,4,C,D}`; sign style
      none for `1-4`/`A-D` per the "No Sign" column, only J–Q get a sign
      suffix and N–Q a leading sign.

- [ ] **A6. Zero-balance blank behavior — the entire distinguishing feature
      of half the edit-code pairs — is unimplemented.** Manual Table 8: every
      adjacent code pair (1/2, 3/4, A/B, C/D, J/K, L/M, N/O, P/Q) differs only
      in whether a zero value prints (`.00`/`0`) or blanks out completely.
      None of this exists in `rpg_rt_edit_dec`
      (`runtime/rpg_runtime.c:661-732`) — a zero-valued field always prints
      digits. Fix: add the print/blank branch keyed off the low bit of the
      code pairing, alongside A5's table rebuild.

- [ ] **A7. Alphameric match fields are decoded as numeric garbage.**
      `decode_m1` (`codegen.cpp:1197-1208`) unconditionally calls the numeric
      decoder on the M1 field's bytes regardless of `fld.decimals`, even
      though the manual explicitly permits alphameric match fields (line
      52312-52313 — matching on a customer code is a common, legitimate
      pattern). Fix: branch on the field's type and do a byte-range compare
      for alphameric M1 fields instead of feeding them through
      `get_decimal_`/`get_packed_`/`get_binary_`.

- [ ] **A8. `has_m1` is computed per file, not per record type.**
      `codegen.cpp:1134-1143` — the first field in a file tagged `M1` decides
      match-field handling for the *whole file*, but the manual explicitly
      allows a file to mix matched and unmatched record types (line
      52214-52215). A file with a matched header type and an unmatched
      trailer type currently forces the trailer's unrelated bytes through
      key-comparison logic. Fix: track `has_m1` per record type, not per
      file.

- [ ] **A9. Alphameric E-spec arrays/tables are parsed, then silently
      discarded at codegen.** `codegen.cpp:271` (and the prerun-time load
      path at `codegen.cpp:309`) gates array/table global creation on
      `decimals >= 0` with no `else` — an alphameric array (the manual's own
      headline array/table example is a month-name table, Ch. 20) is either
      unfindable at `LOKUP` time (`codegen.cpp:3274-3278` errors "LOKUP
      requires an array in factor 2") or silently rebinds the name to an
      unrelated auto-created scalar field elsewhere. Fix: emit an `[N x i8]`
      array of fixed-width character elements for the alphameric case,
      mirroring the numeric path.

- [ ] **A10. MVR's remainder isn't rescaled to its own result field's
      decimal positions.** `emit_mvr` (`codegen.cpp:2519-2537`) stores the
      raw remainder i32 straight into MVR's result pointer without consulting
      `c.result_dec`/`syms_.field_decimals(c.result)`. When MVR's result
      field has different decimals than the preceding DIV's result field (a
      valid, documented pattern — manual 123342-123367, Figure 306), the
      stored value's implied decimal point is wrong. Fix: rescale the saved
      remainder to MVR's own result-field decimals before storing, the same
      way Z-ADD/Z-SUB already rescale factor 2.

- [ ] **A11. `LOKUP` HI/LO doesn't find the nearest element and doesn't
      update the index on an inexact match.** `rpg_rt_lokup`
      (`runtime/rpg_runtime.c:563-575`) only records whether *any* element
      anywhere in the array satisfies "greater than"/"less than" the key,
      rather than the nearest one, and only advances `*idx` (and the
      table-shadow current element) on an exact match. Manual 113147-113162
      requires the nearest-value semantics and an index update on a
      successful HI/LO match too. Fix: rewrite `rpg_rt_lokup` to track the
      nearest qualifying element's index during the scan (assuming array
      order per the E-spec ascending/descending flag — see B2) and update the
      index whenever HI or LO fires, not only on EQ.

- [ ] **A12. `CASxx`'s unconditional form (`CAS` with a blank comparison
      operator) ignores its own line's conditioning indicators (cols 9–17).**
      `codegen.cpp:2207-2219` branches to the subroutine call unconditionally
      whenever `cind` (the parsed AND-group) is nonempty but the comparison
      is blank — the manual's unconditional-CAS rule (105580-105586) only
      makes the *comparison* unconditional, not the indicator gate. Fix:
      still wrap the unconditional-CAS branch in the same `br i1 %cind`
      gating every other conditioned C-spec uses.

- [ ] **A13. Floating currency symbol is unsupported, and an edit code
      combined with a quoted `$`/`*` fill on the same O-spec line is
      mis-parsed as an unrelated constant.** `ospec.cpp:164-172` only treats
      cols 45-70 as an edit word when `fld.edit_code == 0`; when a field name,
      an edit code, *and* a quoted `$`/`*` are all present (manual
      62678-62762, Figures 166-168 — a documented, distinct feature), the
      field-name/edit-code association is silently dropped and the quoted
      text becomes a standalone constant. Separately, `rep` in
      `rpg_rt_edit_word` (`runtime/rpg_runtime.c:775`) doesn't treat `$` as
      replaceable, so it never floats to the first significant digit even in
      plain edit words. Fix: add an `OField` flag for "floating fill
      character following an edit code," parse it in `ospec.cpp`, and extend
      `rpg_rt_edit_word`'s replaceable-character set to include the
      configured fill character.

---

## B. Deviations worth closing (lower frequency, still real) — DONE

- [x] **B1. MR and field indicators are set at extract-time instead of after
      total-time.** Manual steps 24-26 set MR/field indicators *after* total
      calc/output runs, so total-time conditioning on MR or a field indicator
      should see the previous record's state. The compiler sets both during
      `extract` (`codegen.cpp`, `emit_one_input_field` and the multifile MR
      assignment), before total-time runs. Low frequency (most programs
      condition detail, not total, calcs on these) but a genuine ordering
      bug against the documented 26-step cycle.
      Fixed: split `emit_one_input_field` (extraction only) from a new
      `emit_field_indicators`/`emit_field_indicators_for`, called once at the
      entry to `detail.calcs` (after total time and the multifile MR store)
      in both `generate_cycle` and the multifile cycle; CHAIN/READ/READE's
      `decode_file_fields` sets indicators immediately since those are
      calc-time procedural ops with no later "total time" to defer to.

- [x] **B2. E-spec ascending/descending sequence flag is parsed and then
      never read again.** `espec.cpp:47-48,68-69` sets
      `.ascending`/`.alt_ascending`, but `grep` across `codegen.cpp` shows
      nothing ever consults them; `rpg_rt_lokup` always does a full linear
      scan. Needed for A11's nearest-match fix to be meaningful on a
      descending-sequence table (manual 80417-80459).
      Already implemented (found done alongside A11): `emit_lokup` reads
      `a.ascending`/`a.alt_ascending` and passes it to `rpg_rt_lokup`, which
      tracks the nearest HI/LO element per direction.

- [x] **B3. Sign/decimal-blind comparison for match and control fields is
      missing.** The manual states a match field of −5 matches +5 (line
      84315-84318) and the same for control-level fields (53882-53887) —
      comparison should be digits-only, sign ignored. `decode_m1`'s key
      compare and the control-break compare (`codegen.cpp:964,
      1299-1300`) both use full signed equality/ordering, so a legitimately
      negative control or match field breaks or mismatches when it
      shouldn't.
      Fixed: added `emit_abs_i32`; applied at M1 key decode (both the
      single-type and per-record-type/A8 `decode_m1` paths) and at both
      control-break comparison sites. Decimal-blindness was already correct
      by construction (the decoded value is the raw digit magnitude,
      unscaled). Regression test: `tests/signblind.rpg`.

- [x] **B4. Alphameric look-ahead fields aren't 9-filled at EOF.** The manual
      requires *every* look-ahead field, numeric or alphameric, to fill with
      `9`s at end-of-file (83344-83346). `codegen.cpp:936-946` explicitly
      skips fields with `decimals < 0`.
      Fixed: the EOF fill loop in `generate_cycle` now byte-fills alphameric
      look-ahead fields with `'9'` alongside the existing numeric fill.
      Regression test: `tests/la_alpha_eof.rpg`.

- [x] **B5. `SETON`/`SETOF` can touch indicators the manual says they
      cannot.** Manual 104980-104982: SETON must not set `1P`, `MR`, `L0`, or
      `KA`–`KY`; SETOF must not clear `1P`, `MR`, `L0`, or `LR`. `L0` is
      already correctly un-storable (`cspec.cpp:42`), but `1P`→`-11` and
      `MR`→`-12` are fully storable via `store_resolved`
      (`codegen.cpp:156-168`). Low practical impact.
      Fixed: `emit_seton` now skips 1P/MR unconditionally, KA-KY on SETON,
      and LR on SETOF (L0 was already excluded via the existing
      `slot.indicator != 0` guard). Regression test: `tests/seton_restrict.rpg`.

- [x] **B6. F-spec composite/noncontiguous keys (`EXTK`) silently resolve to
      key position 0.** `fspec.cpp:96-99` does `std::stoi` on the key-start
      column; `"EXTK"` throws, is caught, and `key_start` is left at its
      default with no diagnostic (manual 77556, 77569). At minimum this
      should be a hard parse error until composite-key support (see C-group)
      exists.
      Fixed: a non-numeric (or partially-numeric) cols 35-38 value is now a
      hard `DiagKind::Error`. Regression test: `tests/neg_extk.rpg`.

---

## C. Missing operation codes

Grouped by how contained the fix is. This RPG II dialect (System/36-compatible)
has no string ops (CAT/SCAN/CHECK/CHECKR/SUBST) and no date-duration ops
(ADDDUR/SUBDUR/EXTRCT) — those are RPG III/400 additions and are correctly
absent, not a gap.

- [x] **C1. `READP`** (Read Prior Record, manual 123813) — sequential
      backward read through a keyed DISK file, a natural companion to the
      already-implemented `SETLL`/`READE`/`READ` family (`codegen.cpp`
      ~3160-3220, `runtime/rpg_runtime.c`'s indexed-file helpers). Contained:
      same shape as `READ`, opposite direction.
      Fixed: `emit_readp` (`codegen.cpp`) mirrors `emit_read`, calling a new
      `rpg_rt_readp` that walks the same offset index `rpg_rt_read`/
      `rpg_rt_setll` already build, backward from the current cursor (or from
      the last record when the cursor is past EOF). Cols 58-59 turn on at
      beginning-of-file. Regression test: `tests/readp.rpg`.

- [x] **C2. `BITON` / `BITOF`** (105336, 105207) — set/clear individual bits
      in a character field by bit-number literal, the writer half of the
      already-implemented `TESTB` reader (`codegen.cpp:3438-3501`). Without
      these, `TESTB` has nothing that can legitimately set the bits it
      tests.
      Fixed: factored TESTB's mask-building (bit-number literal or field) out
      into `emit_bit_mask`, reused by a new `emit_bit_set(c, on)` that
      OR's (BITON) or AND-NOTs (BITOF) the mask into the result byte in
      place — no runtime call needed. Regression test: `tests/biton.rpg`.

- [x] **C3. `*LIKE DEFN`** (106341) — define a new field with the length/
      decimals of an existing field. Common idiom; its absence forces every
      program to duplicate length/decimal literals by hand. Fits into the
      existing field-declaration path in `symbols.cpp`.
      Fixed: `emit_defn` (`codegen.cpp`) validates factor1 == `*LIKE` (in
      `cspec.cpp`, along with the no-conditioning/no-resulting-indicators
      rule) and declares the result field from factor2's `FieldInfo` —
      decimals copied verbatim for a numeric source, length adjusted by the
      cols 49-51 *signed delta* (not an absolute length, unlike every other
      op) for a character source. Numeric fields in this compiler carry no
      separate digit-length attribute, so the delta only affects character
      fields. Regression test: `tests/defn.rpg` (checked via O-spec edit-code
      print, since a decimal-aligned COMP can't externally distinguish a
      copied-correctly decimals value from a wrong default — both self-
      normalize to the same logical value).

- [x] **C4. `SORTA`** (124481) — sort an array in place, ascending or
      descending per its E-spec sequence flag (ties into B2).
      Fixed: `emit_sorta` reads the array's `ascending`/`alt_ascending` flag
      the same way `emit_lokup` already does (B2) and calls a new
      `rpg_rt_sorta` (in-place insertion sort). Numeric arrays only, matching
      LOKUP's existing alphameric-array restriction. Regression test:
      `tests/sorta.rpg`.

- [x] **C5. `TIME`** (124880) — retrieve time-of-day into a result field.
      Self-contained runtime call.
      Fixed: new `rpg_rt_time` (`<time.h>`-based) returns the current time as
      a 6-digit `hhmmss` integer, stored via `emit_time`. This compiler
      represents numeric fields as native 32-bit scaled integers with no
      separate digit-length attribute (`symbols.h`), so only the
      always-fitting 6-digit time-of-day form is implemented; the manual's
      12-digit time+date variant would need a wider field representation
      than exists anywhere else in this compiler either. Regression test:
      `tests/time_op.rpg`.

- [x] **C6. `MHHZO`/`MHLZO`/`MLHZO`/`MLLZO`** (113217-113324) — move-zone
      operations for packed/zoned sign manipulation. Narrower use case than
      C1-C5; needed for programs doing manual sign-nibble manipulation.
      Fixed: a shared `emit_movezone(c, src_high, dst_high, opname)` moves
      the high nibble of factor2's leftmost/rightmost byte into the
      corresponding byte of the result, ANDed with that byte's existing low
      (digit) nibble — pure IR, no runtime call. Like TESTZ/TESTB's own
      result field, only alphameric operands are supported on either side;
      the manual's numeric-capable cases (MHLZO's result, MLHZO's factor2)
      aren't reachable because this compiler's numeric fields are native
      i32s, not zoned-decimal bytes. Regression test: `tests/movezone.rpg`.

- [ ] **C7. `DEBUG`, `RLABL`, `SET`** (106221, 123972, 124243) — lower
      priority; `DEBUG` is a conditional trace/dump aid, `RLABL` and `SET`
      are edge-case linkage/console operations.
      Investigated but deliberately left unimplemented: each one's manual
      text makes it a no-op or meaningless without a *different* unimplemented
      prerequisite, so implementing the opcode itself in isolation would be
      dead code with no honest test. `DEBUG` requires H-spec column 15 = `1`
      to be active at all ("if this entry is not made, the DEBUG operation
      code ... [is] treated as a comment"); D1 landed H-spec parsing but
      deliberately didn't add column 15, since this compiler has no
      debug/symbol-table generation for a parsed `DEBUG` entry to plug into
      either way — the real prerequisite is that generation existing at all,
      not the H-spec column. `RLABL` only has meaning "specified immediately
      after the EXIT operation that refers to the subroutine" — it's part of
      the C8 CALL/PARM/PLIST/RETRN/EXIT linkage family, not independently
      useful. `SET` is documented as usable "only with input files assigned
      to the device KEYBORD, or with a CONSOLE file" — WORKSTN/CONSOLE-only,
      out of this compiler's documented scope cut. Revisit each alongside its
      actual prerequisite (a debug/symbol-table feature, C8, or a WORKSTN
      effort) rather than stubbing it in now.

- [ ] **C8. Program-linkage family: `CALL`, `PARM`, `PLIST`, `RETRN`,
      `EXIT`** (105441, 123455, 123562, 123924, 110996) — without these, no
      RPG II source that calls an external subprogram can be compiled at
      all. This is the largest item in this group: it needs a real design
      decision for how a "call" maps onto the LLVM/ELF model (static link to
      another compiled RPG object? a documented ABI for parameter passing
      via `PLIST`/`PARM`?) before implementation starts — treat as its own
      design spike, not a quick fix.

- [ ] **C9. `ACQ`/`REL`/`NEXT`/`KEY`/`POST`/`FORCE`/`FREE`/`SHTDN`**
      (105067, 123859, 123403, 113040, 123621, 111276, 111422, 124436) — all
      ICF/WORKSTN-adjacent. Consistent with the already-documented WORKSTN
      scope cut (`TODO.md` G26 in the prior version); only worth doing if
      WORKSTN support is ever undertaken. Do not prioritize.

---

## D. Missing spec types / chapters — DONE

- [x] **D1. Control Specification (H-spec).** No parser existed anywhere
      (`source.cpp`'s `form_type()` never dispatched on `'H'`); an H-spec
      line was silently dropped with zero diagnostic. Manual 68847-70098
      (Ch. 18).
      Fixed: new `hspec.h`/`hspec.cpp` parse every documented column (currency
      symbol col 18, date format/edit/inverted-print 19-21, alternate
      collating sequence 26, inquiry 37, `1P` forms-position 41, file
      translation 43, nonprint-character bypass 45, transparent literal 57,
      program-id 75-80) into a new `Program::hspec`. Two columns actually
      change codegen, per the original scope note: the currency symbol
      (default `$`) is threaded into `rpg_rt_edit_word`'s floating-currency
      detection (A13), replacing the previous hardcoded `'$'`; a non-blank
      program-id (cols 75-80) renames the LLVM module identifier (left alone
      when blank, so the ~70 pre-existing tests without an H-spec are
      unaffected). Every other column is parsed and retained but documented
      as inert rather than silently dropped — date format/`Y` edit
      code/`UDATE`, 1P forms-alignment prompting, inquiry, file translation,
      nonprint-character halts, and transparent literals have no analog
      anywhere else in this batch compiler to plug into yet (`UDATE` and the
      `Y` edit code in particular don't exist at all independent of H-spec;
      wiring a control-specification column into a feature that isn't itself
      implemented would be dead code). Regression test: `tests/hspec_currency.rpg`.

- [x] **D2. Data Structures (Chapter 15).** Field overlay/redefinition —
      letting one record area be viewed under several field layouts. No code
      path recognized the literal `DS` in I-spec cols 19-20 (`ispec.h`
      treated those columns only as a numeric record-id indicator), so a real
      `DS` line was silently misinterpreted rather than rejected. Manual
      61059-61767.
      Fixed: `ispec.cpp` recognizes a `DS` statement line and routes its
      subfield lines (same column shape as ordinary I-spec fields, but
      positions relative to the DS rather than the record) into new
      `ISpecDS`/`ISpecSubfield` lists, keyed by DS index rather than name so
      two anonymous (blank-name) data structures in one program don't
      collide. `codegen.cpp`'s new `declare_data_structures` resolves each
      DS's backing storage — reusing an already-declared field's global for a
      true redefinition (manual 61412-61416: "the physical space reserved for
      that field is in the data structure") when the DS's own name matches an
      existing *alphameric* field, or allocating a fresh `[len x i8]` buffer
      otherwise (numeric fields can't be redefined this way: they're native
      i32 in this compiler with no addressable byte representation to alias
      into, so that's a hard diagnostic instead of silent corruption).
      Alphameric subfields are byte-range GEP views into the shared buffer
      (`SymbolTable::declare_ds_char_subfield`); every existing character-field
      call site already funnels through `resolve_char_operand` for its
      pointer, so read AND write both work with no per-opcode changes beyond
      fixing two MOVE/MOVEL call sites in `codegen.cpp` that had bypassed
      `resolve_char_operand` for the destination pointer. Numeric subfields
      decode-read their bytes via the same `rpg_rt_get_decimal` runtime call
      ordinary zoned I-spec fields use, on every access
      (`SymbolTable::decode_ds_numeric`) — there is no i32-to-zoned-decimal
      encoder anywhere in this compiler to write one back with, so using a
      numeric DS subfield as a calc result field is a hard compile error
      (`get_or_create_field`'s single choke point) rather than silently
      unusable storage. Regression tests: `tests/ds_alpha.rpg` (alphameric
      aliasing across overlapping subfields), `tests/ds_redefine.rpg`
      (redefinition of an existing input field), `tests/ds_numeric.rpg`
      (numeric decode-on-read), `tests/neg_ds_numeric_write.rpg` (numeric
      write is a hard error).

- [x] **D3. Auto Report Feature (Chapter 26).** `/COPY`, a `U`-type Options
      spec, and `*AUTO` heading/output expansion were 100% unimplemented — no
      form-type `U` parser, no `/COPY` handling, and `*AUTO` in an O-spec
      field-name column compiled silently to nothing instead of erroring.
      Manual 89067-103989.
      Fixed the three concretely-named gaps: `/COPY` (manual 90360-90450) is
      real, working source inclusion — `source.cpp`'s new
      `expand_copy_statements` splices a copy member's lines in at the
      `/COPY` line's position before any spec parser runs (cycle-guarded
      against a member copying itself). The documented `LIBRARY,MEMBER`
      addressing is adapted to this compiler's plain-filesystem model: the
      library segment is parsed but not resolved against a System/36
      library catalog (this compiler has none), only the member name is
      looked up (as-is, then `.rpg`, then `.cpy`) next to the main source
      file. New `uspec.h`/`uspec.cpp` recognize form-type `U` (Auto Report
      Option Specifications) and `ospec.cpp` recognizes `*AUTO` in the
      field-name column, but neither is expanded — both are hard compile
      errors instead of a silent no-op, since an Auto Report source program
      typically has no ordinary O-specs of its own (the option specs stand
      in for them), so compiling one as-is would silently produce a program
      with no meaningful output, not just a degraded one. The full auto-report
      preprocessing layer itself (deriving headings/spacing/field placement
      from just a `U` spec, manual "Order of Created Specifications") remains
      unimplemented, consistent with the original note that it's the largest
      single item in this list by manual page count and lowest priority
      unless a specific legacy program library needs it — but that's now an
      explicit, loud limitation instead of a silent one. Regression tests:
      `tests/copytest.rpg` + `tests/copymem.cpy` (`/COPY` splicing),
      `tests/neg_uspec.rpg`, `tests/neg_auto.rpg`.

---

## E. F-spec / E-spec gaps — DONE

- [x] **E1. F-spec cols 71-72, external file conditioning (`U1`-`U8`).** Not
      parsed anywhere — a file that should be skipped entirely when its
      switch is off is instead always processed. Manual 78727. Real
      correctness gap, not a convenience miss.
      Fixed: `fspec.cpp` parses `cond_ind`/`has_cond`. `codegen.cpp` gates
      every read/write against the file's indicator: the primary/secondary
      cycle reads (`emit_conditioned_bool`), CHAIN/SETLL/READ/READE/READP,
      and all O-spec output (`emit_one_record` ANDs `file_cond_ok` into the
      line condition) — off means treated as EOF/no-op, matching the manual,
      with the underlying runtime call skipped entirely rather than just its
      result discarded. Regression tests: `tests/cond_input.rpg`,
      `tests/cond_output.rpg`.

- [x] **E2. F-spec col 18, sequence (`A`/`D`).** Not parsed — multifile
      matching (codegen.cpp's `decode_m1`/selection loop) always assumes
      ascending order with no way to detect a descending-sequence file.
      Manual 77002; ties into B2/B3.
      Fixed: `fspec.cpp` parses `sequence`. `codegen.cpp`'s multifile
      selection loop takes the group's direction from any matching file's
      declared sequence (erroring on disagreement per the manual) and flips
      the running-minimum comparison to a running-maximum for `D`, in both
      the numeric and alphameric (A7) M1 compare paths. Regression test:
      `tests/mrdesc.rpg`.

- [x] **E3. F-spec col 17, "file must reach EOF before program can end."**
      Not read into end-of-job logic; the cycle's EOF condition was "all
      files empty at once," which coincidentally matches the common case but
      diverges when only some files are marked required. Manual 51914-51918.
      Fixed: `fspec.cpp` parses `end_required`. The multifile cycle now
      checks, before the per-cycle file-selection walk, whether every
      `E`-marked file is at EOF; if so it ends the job immediately even if
      other (unmarked) files still hold data. With no file marked `E`, the
      original "every file empty" fallback is unchanged. Regression test:
      `tests/eoreq.rpg`.

- [x] **E4. F-spec composite keys (`EXTK`).** Already covered by B6 (a
      non-numeric cols 35-38 value, including `EXTK`, is a hard compile
      error). No further action; this item was a duplicate pointer, not a
      distinct gap.

- [x] **E5. F-spec record-address files (designation `R`) are a dead end.**
      The designation parsed into an enum (`fspec.h`) but `codegen.cpp` never
      consumed it, and E-spec cols 19-26 ("to filename") weren't parsed at
      all. Manual 79661.
      Resolved per the TODO's own fallback: rather than build the full
      record-address data-flow (a distinct cycle mode this compiler doesn't
      implement), designation `R` is now a hard compile error in
      `fspec.cpp`, the same precedent as EXTK/B6, so it can no longer look
      wired up while doing nothing. Regression test: `tests/neg_recaddr.rpg`.

- [x] **E6. F-spec `addr_type` (col 31: `A`/`P`/`I`) is parsed and never
      read again.** `fspec.cpp:92-93` stores it; nothing branches on it. An
      `I` (RRN-with-declared-key-length) file is indistinguishable from an
      ordinary byte-compared keyed file. Manual 26512-26515.
      Fixed: `codegen.cpp` skips the `rpg_rt_set_key` call for a file whose
      `addr_type` is `I`, regardless of whether cols 29-30 declare a length —
      the runtime already treats "no key configured" as relative-record-number
      access in CHAIN/SETLL/READE (`rpg_runtime.c`), so this was a minimal,
      surgical fix rather than a new runtime code path. Regression test:
      `tests/rrn_i.rpg`.

- [x] **E7. E-spec col 43, packed/binary prerun-time array data.** Not
      parsed — a prerun-time array/table file stored packed or binary was
      decoded as zoned ASCII by `rpg_rt_load_arrays`, silently producing
      garbage. Manual 80373.
      Fixed: `espec.cpp` parses `data_format`/`alt_data_format` (cols 43/55).
      `rpg_rt_load_arrays` takes `fmt_a`/`fmt_b` and decodes each element via
      the same packed/binary decoders ordinary input fields use; for those
      formats the loader also stops treating the file as newline-delimited
      text, since a packed/binary byte can coincidentally equal `\n`/`\r`.
      Regression test: `tests/packed_prerun.rpg`.

- [x] **E8. No device-type validation on the primary file.** A source file
      declaring `WORKSTN`/`SPECIAL`/`CONSOLE` as its primary input device
      compiled without error and attempted to read the device name as an
      ordinary flat text file. The WORKSTN/SPECIAL/CONSOLE scope cut itself
      is reasonable; it should fail loudly at compile time instead of
      miscompiling silently.
      Fixed: `fspec.cpp` reports a hard error for any file (not just primary)
      declaring one of those three devices, since none of them have codegen
      support regardless of designation. Regression test:
      `tests/neg_workstn.rpg`.

---

## F. O-spec gaps — DONE

- [x] **F1. AND/OR continuation lines for more than 3 output-conditioning
      indicators are silently dropped.** A blank-type line carrying only
      indicators falls into `ospec.cpp:176-178`'s catch-all `continue` with
      no diagnostic. Manual 88200-88217, 88491-88493.
      Fixed: `ospec.cpp` checks cols 14-16 for `AND`/`OR` before the
      record/field-line heuristic (same precedent as A3's I-spec fix) and
      merges the continuation line's cols 23-31 indicators into the current
      record. `ORecord::conditions` is now `vector<vector<CondInd>>` — OR-of-
      AND groups, matching the manual precisely: an `AND` line extends the
      *current* group's AND-list, an `OR` line starts a new group, and the
      record fires iff any group is fully satisfied. `codegen.cpp` gained
      `eval_conditions_grouped` (OR of `eval_conditions` over each group);
      `has_overflow_condition`/`has_1p_condition` now scan all groups. A
      plain record with no continuation lines gets exactly one group, so
      existing single-line-conditioned programs are unaffected. Regression
      test: `tests/oand_or.rpg`.

- [x] **F2. Fetch-overflow (`F`) / release (`R`) in O-spec col 16 alone are
      unparsed.** `ospec.cpp` only recognizes 3-char `ADD`/`DEL`/`UPDATE` in
      cols 16-18; the manual's single-character `F`/`R` forms (88310-88356)
      aren't handled. The compiler's own always-on overflow-polling scheme
      covers the common case, so this is lower priority than F1.
      Fixed: `ospec.cpp` reads col 16 alone (independent of the cols 17-18
      space-before/after digits) when it doesn't match the 3-char
      ADD/DEL/UPDATE mnemonic. `R` (release) is a hard compile error, same
      precedent as E8/E5 — it's only meaningful for a WORKSTN display
      station or ICF session, which this compiler doesn't support. `F`
      (fetch overflow) gets real inline semantics in `emit_one_record`: right
      after this specific line prints, poll the file's overflow latch and,
      if set, run that indicator's overflow output immediately (instead of
      waiting for the normal once-per-cycle check in `emit_overflow_check`,
      which runs only after total time/before detail time — a fetch-flagged
      detail line's own overflow would otherwise be serviced a full cycle
      late, or never in a single-cycle program). Per the manual, fetch is
      suppressed when the same line is itself conditioned on that file's
      overflow indicator ("the overflow routine is not fetched"); a new
      `allow_fetch` parameter (false only for records reached from inside an
      overflow-output pass) bounds this to one level of immediate servicing
      per printed line rather than chaining indefinitely. Regression tests:
      `tests/fetch_overflow.rpg`, `tests/neg_orelease.rpg`.

- [x] **F3. `docs/SPEC_MAP.md`'s claim that "O-spec col 44 is parsed but
      treated as a no-op" is inaccurate — it isn't read at all.** No
      `col_trim(t,44,44)` call exists anywhere in `ospec.cpp`, and `OField`
      has no member for it. Fix the doc when E7/packed-binary-output work
      happens, or now if just correcting the record.
      Resolved per the TODO's own fallback: corrected the record now rather
      than waiting on the (larger, separate) output-side packed/binary
      encoding feature. `docs/SPEC_MAP.md`'s O-spec field table and the
      Section C numeric-data-model note both now say col 44 is unparsed and
      point at this as an explicit gap, not a parsed-but-inert column.

---

## G. Documentation corrections (do alongside the code fixes above) — DONE

- [x] Update `docs/SPEC_MAP.md`'s "Operation semantics" table to note the
      decimal-alignment rule for COMP/IFxx/DOWxx/DOUxx/CASxx once A2 lands.
- [x] Update `docs/SPEC_MAP.md`'s edit-code description (currently "1–4 for
      comma/decimal, J–M with a trailing minus, N–Q with a leading sign") to
      reflect the corrected Table 8 mapping once A5/A6 land, including the
      A–D codes it currently omits entirely.
- [ ] ~~Add an explicit "not implemented" line for H-spec and Data
      Structures to `docs/ARCHITECTURE.md`'s scope section~~ — moot: D1/D2
      landed, so the gap this bullet was tracking no longer exists. If
      `docs/ARCHITECTURE.md`'s scope-cut list is ever written, it should
      instead note the *actual* remaining D-group limits: H-spec cols other
      than currency-symbol/program-id are parsed-but-inert (no `UDATE`/`Y`
      edit code, no debug/symbol-table generation, no interactive
      forms-alignment, no file translation, in this compiler); DS numeric
      subfields are read-only; DS local-data-area (`U` in col 18) is
      unimplemented; Auto Report expansion itself (`U`-spec, `*AUTO`) is a
      hard error, not silently degraded.
- [x] ~~Fix F3 above.~~ — done alongside F3 itself.

---

## Recommended sequencing

1. **Group A (critical bugs)** — highest ratio of real-world impact to
   effort; each is a localized fix in an existing code path, no new spec
   surface. A1 (edit-code line overwrite) and A2 (comparison decimal
   alignment) affect the largest fraction of realistic programs and should
   go first.
2. **Group B (deviations)** — same shape as Group A but lower frequency;
   do these once Group A is clean.
3. **Group E/F (F/E/O-spec gaps) — done.** Mostly small, independent parser
   additions; E8 (device validation) was a cheap, high-value addition since
   it converts a silent miscompile into a clear error. Group F's AND/OR
   continuation groups (F1) and inline fetch-overflow (F2) needed real
   codegen changes (grouped OR-of-AND conditioning, a bounded-recursion
   inline overflow poll) rather than pure parsing; F3 was a documentation
   correction.
4. **Group C1-C6 (contained opcodes) — done.** READP, BITON/BITOF, *LIKE
   DEFN, SORTA, TIME, and the MHHZO/MHLZO/MLHZO/MLLZO move-zone family are
   implemented and regression-tested. C7 (DEBUG/RLABL/SET) is deliberately
   still open — each depends on a prerequisite from Group C8 or D below, not
   on more opcode-dispatch work.
5. **Group D (H-spec, Data Structures, Auto Report) — done.** H-spec is
   fully parsed with the two behavior-changing columns (currency symbol,
   program-id) wired into codegen and the rest documented as inert; data
   structures support real byte-aliased subfields (alphameric read/write,
   numeric read); `/COPY` is real source splicing, and `U`-spec/`*AUTO` are
   loud compile errors instead of silent no-ops. **Group C8
   (CALL/PARM/PLIST/RETRN)** remains the one large item still open — it
   needs its own design pass (a real decision for how a "call" maps onto the
   LLVM/ELF model) before implementation starts, not just a bugfix-sized
   change. C7's DEBUG is still correctly blocked: D1 parses H-spec col 15
   into nothing (this compiler has no debug/symbol-table generation for a
   parsed `DEBUG` entry to plug into), and RLABL/SET still depend on C8 and a
   WORKSTN effort respectively, neither of which landed here.

Every item above cites a manual line range and a source location; re-derive
neither from memory when implementing — read both before changing code.
