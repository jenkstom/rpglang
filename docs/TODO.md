# RPG II Compiler — Finish-Out Todo List

Complete inventory of remaining work to handle any RPG II program. Organized
by area and priority. Each item notes what it is and rough effort. Current
state: 31 tests passing across Phases 1–10.

---

## A. Missing operation codes (C-spec)

- [ ] **A1. `Z-SUB`** — `r = -F2` (negate). Same pattern as Z-ADD. ~15 min.
- [ ] **A2. `DO` / `END` (counted loop)** — `DO S E`: body runs (E−S+1) times.
      Distinct from DOW/DOU; needs its own frame type in the block stack. ~1 hr.
- [ ] **A3. `CASxx` / `END` (case/select)** — multi-way branch to subroutines.
      Needs CAS-group parsing + END matching. Depends on subroutines (work).
      ~1.5 hr.
- [ ] **A4. `EXCPT` / exception (E) output** — `EXCPT name` writes O-spec type-E
      lines. Needs E-type O-records + an emission path called from calc time.
      ~1 hr.
- [ ] **A5. `TESTZ` / `TESTB`** — test zone/bits of a character field, set
      indicators. Niche; ~30 min each.

---

## B. Tables and prerun-time arrays (E-spec)

- [ ] **B6. Tables (`TAB`-prefixed names)** — no direct indexing; a
      "last-selected element" shadow updated by LOKUP. Needs per-table shadow
      globals + name-prefix handling in operand resolution. ~2 hr.
- [ ] **B7. Prerun-time arrays/tables** — loaded from a file at program start
      (cols 11–18 filename). Needs runtime loader + a load phase before the
      cycle. ~1.5 hr.
- [ ] **B8. Alternating arrays** (cols 46–57) — paired array/table definitions.
      Niche. ~45 min.

---

## C. Numeric data model

- [ ] **C9. Packed-decimal (`P`) and binary (`B`) field storage** — I-spec col 43
      / O-spec col 44. Currently everything is zoned-ASCII-i32. Needs decode/
      encode in the runtime + width tracking in the symbol table. ~3 hr (the
      biggest single item).
- [ ] **C10. Numeric↔character MOVE with sign-overpunch** — EBCDIC zone-sign
      (J–R = negative) on the last digit of alphameric→numeric MOVE. ASCII-
      specific mapping decision needed. ~1.5 hr.
- [ ] **C11. Decimal scaling** — honor result field decimals (col 52) for
      MULT/DIV and half-adjust (col 53) rounding on all arithmetic. Currently
      integer-only. ~2 hr.

---

## D. Output specs (O-spec) — finish Phase 7 gaps

- [ ] **D12. Heading (H) lines + 1P first-page indicator** — print once at
      program start. Needs a 1P indicator + an output pass before the cycle.
      ~1 hr.
- [ ] **D13. Skip-before / skip-after** (cols 19–22) — line-number pagination,
      form-feed on lower skip. Runtime helper + O-spec parse. ~1 hr.
- [ ] **D14. PAGE / PAGE1–PAGE7 field** — auto-incrementing page counter
      special fields. ~45 min.
- [ ] **D15. Per-field conditioning indicators** on O-spec field lines
      (cols 23–31) — gate individual fields, currently ignored. ~30 min.
- [ ] **D16. Edit words** (cols 45–70 alternative to edit codes) — pattern-based
      number formatting. ~2 hr.

---

## E. Input specs (I-spec) — finish Phase 3 gaps

- [ ] **E17. Record-identification codes** (cols 21–41) — multi-record-type
      files, select record by content match. Currently single-record-type only.
      ~2 hr.
- [ ] **E18. Field indicators** (cols 65–70) — set indicator on +/−/0 or blank
      per field at read time. ~1 hr.
- [ ] **E19. Look-ahead fields** — read fields from the next record before
      committing the current. Niche. ~2 hr.

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

1. **Quick wins first**: A1 (Z-SUB), A2 (DO), A5 (TESTZ/TESTB), D15 (per-field
   O-spec indicators), H27 (GOTO boundary checks) — small, round out the common
   opcode set.
2. **High-impact middle**: A3 (CASxx), A4 (EXCPT), D12 (H-lines/1P), B6 (tables),
   D13/D14 (skip/PAGE) — make output and selection complete.
3. **Heavy lifting**: C9 (packed/binary), C11 (decimal scaling), F20/F21
   (matching records + secondary files), G24 (indexed access) — where "any RPG
   II program" fully lands.
