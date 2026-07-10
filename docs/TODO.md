# RPG II Compiler — Finish-Out Todo List

Complete inventory of remaining work to handle any RPG II program. Organized
by area and priority. Each item notes what it is and rough effort. Current
state: 54 tests passing across Phases 1–10 and Sections A–E.

---

## A. Missing operation codes (C-spec) — DONE

- [x] **A1. `Z-SUB`** — `r = -F2` (negate). Same pattern as Z-ADD.
- [x] **A2. `DO` / `END` (counted loop)** — `DO S E`: body runs while the index
      (default start 1) is ≤ the limit, incrementing by END's factor 2 (default
      1) each pass. New DO frame type in the block stack.
- [x] **A3. `CASxx` / `END` (case/select)** — a CAS group is a run of CASxx ops
      followed by one END; each CASxx compares F1/F2 and, if true, calls the
      subroutine named in the result field. Blank-xx CAS is the unconditional
      default. CAS frames share one merge block.
- [x] **A4. `EXCPT` / exception (E) output** — `EXCPT name` writes the type-E
      O-records whose EXCPT name (cols 32–37) matches; blank factor 2 selects
      the unnamed E-records. Emitted at calc time via emit_one_record (shared
      with cycle output). O-spec parser now carries the filename forward across
      continuation record lines.
- [x] **A5. `TESTZ` / `TESTB`** — TESTZ tests the "zone" of the leftmost char of
      the result field (HI=plus, LO=minus, EQ=zero; ASCII uses the manual's
      explicit char sets `&`/A–I, `-`/J–R, else). TESTB tests bits named by a
      factor-2 literal `'025'` or a 1-char field mask (HI=all off, LO=mixed,
      EQ=all on).

---

## B. Tables and prerun-time arrays (E-spec) — DONE

- [x] **B6. Tables (`TAB`-prefixed names)** — no direct indexing; a
      "last-selected element" shadow (`rpgs_<name>`, 1-based, default 1) updated
      by LOKUP. A bare table name in any operand resolves to the shadow-selected
      element; a related table named in the result field advances in lockstep.
      Detected by the TAB prefix (case-insensitive); indexed `TAB,ID` form still
      works as an ordinary array ref.
- [x] **B7. Prerun-time arrays/tables** — loaded from a file at program start
      (cols 11–18 filename) via `rpg_rt_load_arrays`, emitted once at the top of
      `main`'s entry block (before the cycle / calc chain).
- [x] **B8. Alternating arrays** (cols 46–57) — a partner array/table named in
      cols 46–51 is parsed and emitted as its own global; compile-time and
      prerun-time data interleave on each record (A1 B1 A2 B2 …).

---

## C. Numeric data model — DONE

The stored representation stays a single signed `i32`, but it is now a *scaled
integer* (stored = true × 10^decimals), so decimal arithmetic is exact with no
new IR type.

- [x] **C9. Packed-decimal (`P`) and binary (`B`) field storage** — I-spec col 43
      is parsed (`P`/`B`/blank). `rpg_rt_get_packed` decodes BCD-with-sign-nibble;
      `rpg_rt_get_binary` decodes big-endian int16/int32. The cycle extract
      branches on the format. The decoded value flows into the existing i32 store
      (the byte layout encodes the same scaled digits the zoned path would).
      O-spec col 44 is parsed but treated as a no-op (DISK/ICF only).
- [x] **C10. Numeric↔character MOVE with sign-overpunch** — `rpg_rt_overpunch_in`
      (char→numeric) and `rpg_rt_overpunch_out` (numeric→char) implement the
      zone-sign convention: A–I = positive digits 1–9, J–R = negative digits 1–9,
      plain 0–9 = positive. Wired into `emit_move` for both directions.
- [x] **C11. Decimal scaling** — ADD/SUB align operands to `max(dec1,dec2)`;
      MULT's result scale is `dec1+dec2`; DIV scales the numerator up to retain
      precision; all adjust to the result field's decimals (col 52). Half-adjust
      (col 53 = `H`) rounds at the first dropped digit. Z-ADD/Z-SUB rescale.
      Output is decimal-aware (`rpg_rt_line_put_num_dec`, `rpg_rt_edit_dec`).

---

## D. Output specs (O-spec) — finish Phase 7 gaps — DONE

- [x] **D12. Heading (H) lines + 1P first-page indicator** — the `1P` indicator
      (reserved index -11, an `rpg_1p` i1 global on at start) gates a heading
      pass run once before the cycle; it prints all H records (plus any
      1P-conditioned detail), then turns 1P off.
- [x] **D13. Skip-before / skip-after** (cols 19–22) — parsed into `skip_before`
      / `skip_after` (line numbers; `01-99`, `A0-A9`, `B0-B2` decoded).
      `rpg_rt_skip` advances to the line and form-feeds + bumps the page counter
      when skipping to a lower line (new page). Emitted before/after the line.
- [x] **D14. PAGE / PAGE1–PAGE7 field** — the reserved output field names print
      the per-file page counter via `rpg_rt_page` (page 1 on the first page;
      PAGE1-7 select the nth-opened file's counter).
- [x] **D15. Per-field conditioning indicators** on O-spec field lines
      (cols 23–31) — each field is gated by its own conditions; a field whose
      conditions don't hold is omitted while the rest of the line prints.
- [x] **D16. Edit words** (cols 45–70) — a numeric field with a quoted pattern
      and blank col 38 is formatted by `rpg_rt_edit_word` (replaceable blanks,
      first `0`/`*` suppression stop, `-`/`CR` sign, `&` forced blank).

---

## E. Input specs (I-spec) — finish Phase 3 gaps — DONE

- [x] **E17. Record-identification codes** (cols 21–41) — each record type
      carries up to three 7-column code-sets (position / Not / C-Z-D / char),
      with AND/OR continuation lines. At read time the record buffer is matched
      against each type's codes; the matching type's record-identifying
      indicator (cols 19–20) turns on, and non-matching records are skipped. A
      field's field-record-relation (cols 63–64) gates extraction to a type.
- [x] **E18. Field indicators** (cols 65–70) — plus/minus/zero indicators fire
      at read time when a numeric field is >0 / <0 / ==0 (alphameric fields fire
      the zero indicator on all-blank). Mirrors the arithmetic +/−/0 latch.
- [x] **E19. Look-ahead fields** — a `**` record-id line marks the following
      field lines as look-ahead; they decode from the next record via
      `rpg_rt_peek_next` (a per-file one-record peek cache) and fill with 9s at
      end-of-file.

---

## F. Cycle & matching

- [ ] **F20. Matching records (MR indicator, M1–M9)** — multi-file merge on
      match fields. One of RPG's more complex features. ~4 hr.
- [ ] **F21. Secondary files** (F-spec designation `S`) — multiple input files
      feeding the cycle. Depends on matching records. ~3 hr.
- [ ] **F22. Overflow processing** (OA–OG, OV) — printer form-overflow
      indicator + overflow-conditioned output. ~1.5 hr.
- [ ] **F23. First-cycle total-time correctness** — verify the exact "totals
      bypassed until first control field" rule edge cases. ~1 hr.

---

## G. File handling

- [ ] **G24. Indexed/sequential-by-key file access** (F-spec cols 31–32
      record-address type) — SETLL, READE, CHAIN, READ ops. ~3 hr.
- [ ] **G25. Update files** (type `U`) + WRITE/UPDATE/DELETE ops + O-spec
      ADD/DEL. ~2 hr.
- [ ] **G26. WORKSTN / interactive** — out of scope for a batch compiler;
      documenting only. — *skip*.

---

## H. Tooling & hardening

- [ ] **H27. GOTO boundary enforcement** — reject cross-boundary jumps
      (detail↔total, L0–L9↔LR, into/out of subroutines). Currently unvalidated.
      ~1 hr.
- [ ] **H28. Subroutine recursion detection** — RPG II forbids it; currently
      unenforced. ~45 min.
- [ ] **H29. Better diagnostics** — column-precise errors for all spec types
      (some only warn). ~1 hr.
- [ ] **H30. `-O2`-stability fuzzing** — run all tests at `-O2`/`-O3`; fix any
      optimizer-exposed IR bugs. ~1 hr.
- [ ] **H31. A larger real-world RPG II sample** — find/write a 100+ line
      program exercising many features at once. ~2 hr.

---

## Recommended sequencing

1. **Quick wins first**: H27 (GOTO boundary checks) — small, rounds out the
   common opcode set. *(Section A's quick wins — Z-SUB, DO, TESTZ/TESTB — are
   done; D15 per-field O-spec indicators too.)*
2. **High-impact middle**: output is now complete (D12 headings/1P, D13 skip,
   D14 PAGE, D16 edit words all done). *(B6 tables, B7 prerun-time, and B8
   alternating arrays from this band are done too.)*
3. **Heavy lifting**: F20/F21 (matching records + secondary files), G24 (indexed
   access) — where "any RPG II program" fully lands. *(C9 packed/binary, C10
   sign-overpunch, and C11 decimal scaling from this band are done.)*
