# RPG II Spec Column Map

Authoritative column positions, extracted from the IBM *System/36-Compatible
RPG II User's Guide and Reference* (SC09-1818-00). See
`docs/ref/manual_layout.txt` for the raw extraction.

All positions are **1-based, inclusive**. Column 6 always holds the form type
(`F`, `I`, `C`, `O`). Columns 1–5 are optional sequence; 60–74 comments; 75–80
program-id.

## C-spec (Calculation) — form type `C` in col 6

| Field                    | Cols    | Notes                                                    |
|--------------------------|---------|----------------------------------------------------------|
| Control Level            | 7–8     | `L0`/`L1`–`L9`/`LR`/`SR`/`AN`/`OR`/blank                 |
| Conditioning Indicators  | **9–17**| 3 AND-groups, each `[N]II` (9–11, 12–14, 15–17)          |
| Factor 1                 | 18–27   | begins col 18                                            |
| Operation                | 28–32   | begins col 28                                            |
| Factor 2                 | 33–42   | begins col 33                                            |
| Result Field             | 43–48   | begins col 43 (alpha first)                              |
| Field Length             | 49–51   | 1–256 or blank                                           |
| Decimal Positions        | 52      | 0–9 or blank                                             |
| Half-Adjust              | 53      | `H` or blank                                             |
| Resulting Indicators     | **54–59**| HI/LO/EQ = 54–55/56–57/58–59 (2 chars each)             |
| Comments                 | 60–74   |                                                          |
| Program ID               | 75–80   |                                                          |

### Conditioning-indicator group format (cols 9–17)
Three 3-column groups, ANDed together:
```
col:  9 10 11 | 12 13 14 | 15 16 17
      [N] II  | [N] II   | [N] II
```
Col 9/12/15 = `N` (negation) or blank; 10–11/13–14/16–17 = 2-char indicator.

## F-spec (File Description) — form type `F` in col 6

| Field             | Cols    | Entry                                            |
|-------------------|---------|--------------------------------------------------|
| Filename          | 7–14    | begins col 7 (alpha first)                       |
| File Type         | 15      | I / O / U / C                                    |
| File Designation  | 16      | P / S / F / C / R / T / D                        |
| End of File       | 17      | E (must reach EOF before program can end) / blank|
| Sequence          | 18      | A (ascending) / D (descending) / blank           |
| File Format       | 19      | F (fixed) common; also V/S/M/D/E                 |
| Record Length     | 24–27   | ends col 27                                      |
| Mode of Processing| 28      | blank / L (within limits) / R (random)           |
| Key Length        | 29–30   | length of the key / record-address field         |
| Record-Addr Type  | 31      | blank / A (zoned key) / I (RRN) / P (packed key) |
| Organization      | 32      | blank / I (indexed) / T (address-output)         |
| Overflow Indicator| 33–34   | OA–OG, OV                                        |
| Key Start         | 35–38   | 1-based record position of the key field         |
| Extension Code    | 39      | E / L                                            |
| Device            | 40–46   | DISK / WORKSTN / PRINTER / SPECIAL / ...         |
| File Condition    | 71–72   | blank / U1–U8 (external indicator)               |

Only `DISK` and `PRINTER` devices are implemented; `WORKSTN`/`SPECIAL`/`CONSOLE`
are a hard compile error (E8). Designation `R` (record-address files) is parsed
into `FileDesign::RecordAddr` but has no codegen support and is also a hard
compile error (E5) rather than a silently-inert F-spec entry.

## I-spec (Input) — form type `I` in col 6

Record-identification line and field-description line share col 6. Field line:
| Field             | Cols    | Notes                            |
|-------------------|---------|----------------------------------|
| Packed/Binary     | 43      | P / B / blank                    |
| Field From        | 44–47   | beginning record position        |
| Field To          | 48–51   | ending record position           |
| Decimal Positions | 52      | 0–9 numeric; blank = alphameric  |
| Field Name        | 53–58   | begins col 53 (alpha first)      |
| Control Level     | 59–60   | L1–L9                            |
| Matching Fields   | 61–62   | M1–M9                            |
| Field Indicators  | 65–70   | 65–66 plus, 67–68 minus, 69–70 zero |

## O-spec (Output) — form type `O` in col 6

Two line types per file: a **record line** followed by **field lines**.

**Record line** (distinguished by a non-blank Type in col 15):
| Field                    | Cols    | Notes                                              |
|--------------------------|---------|----------------------------------------------------|
| Filename                 | 7–14    | first record line per file; may be omitted later   |
| AND/OR continuation      | 14–16   | `AND`/`OR` on a separate line, no Type of its own; extends/adds an OR-of-AND conditioning group (F1) |
| Type                     | 15      | **H** / **D** / **T** / **E** (Heading/Detail/Total/Exception) |
| Fetch Overflow / Release | 16      | `F` = poll this file's overflow latch immediately after this line (F2); `R` = release a WORKSTN/ICF device (hard error, unsupported); mutually exclusive with the ADD/DEL/UPDATE mnemonic below |
| Record Op (disk)         | 16–18   | `ADD`/`DEL`/`UPDATE` (G25); takes precedence over col 16 F/R when it matches |
| Space Before             | 17      | 0–3 lines                                          |
| Space After              | 18      | 0–3; all-blank 17–22 ⇒ single-space after          |
| Skip Before              | 19–20   | line number 01–99 / A0–B2                          |
| Skip After               | 21–22   | line number                                        |
| Record Conditioning Inds | 23–31   | 3 groups `[N]II` (23–25 / 26–28 / 29–31); AND/OR continuation lines add more groups/indicators (F1) |
| EXCPT name               | 32–37   | type E only                                        |

**Field line** (cols 7–31 blank):
| Field                | Cols    | Notes                                            |
|----------------------|---------|--------------------------------------------------|
| Field Conditioning   | 23–31   | per-field indicators                             |
| Field Name           | 32–37   | field name, or blank if a constant follows       |
| Edit Code            | 38      | leading-zero / sign / punctuation control        |
| Blank After          | 39      | `B` resets field to blanks/zeros after output    |
| End Position         | 40–43   | rightmost output position (right-justified); blank = pack after previous |
| Packed/Binary        | 44      | Not implemented (P/B disk-output encoding, manual 88929-88950); not parsed at all, left as an explicit gap (see the "Section C additions" note below) |
| Constant/Edit Word   | 45–70   | quoted `'...'` constant (field name cols blank)  |

Type timing: **H** prints at heading time (headings with 1P print once at
start); **D** prints once per record at detail time; **T** prints at total time
(control breaks and LR). Phase 7 implements D and T; H is deferred.

## Operation semantics (Phase 2–6 subset)

| Op      | Factor1 | Factor2 | Result        | Resulting indicators (54–59)              |
|---------|---------|---------|---------------|-------------------------------------------|
| `ADD`   | optional| required| required      | +/−/Z on result sign                      |
| `SUB`   | optional| required| required      | +/−/Z; `r = F1 - F2` (or `r - F2`)        |
| `MULT`  | optional| required| required      | +/−/Z; `r = F1*F2` (result len = F1+F2)   |
| `DIV`   | optional| required| required      | +/−/Z; `r = F1/F2` quotient; F2≠0         |
| `MVR`   | blank   | blank   | required      | +/−/Z; `r` = remainder of preceding DIV   |
| `Z-ADD` | (unused)| required| required      | +/−/Z; `r = F2` (clears result first)     |
| `Z-SUB` | (unused)| required| required      | +/−/Z; `r = -F2` (negate)                 |
| `SETON` | blank   | blank   | blank         | turn ON indicators named in 54–59 (up to 3)|
| `SETOF` | blank   | blank   | blank         | turn OFF indicators named in 54–59 (up to 3)|
| `COMP`  | required| required| blank         | HI if F1>F2, LO if F1<F2, EQ if F1==F2    |
| `GOTO`  | blank   | required(label)| blank  | none; branches to the named TAG          |
| `TAG`   | required(label)| blank| blank    | none; a position marker (no conditioning)|
| `MOVE`  | blank   | required| required      | none; right-justified copy               |
| `MOVEL` | blank   | required| required      | none; left-justified copy                |
| `IFxx`  | required| required| blank         | none; opens a then-group (closes with END)|
| `ELSE`  | blank   | blank   | blank         | none; else-branch of the current IF      |
| `DOWxx` | required| required| blank         | none; do-while: test-at-top, 0+ iters    |
| `DOUxx` | required| required| blank         | none; do-until: test-at-bottom, 1+ iters  |
| `DO`    | opt(start)| req(limit)| opt(index)| none; counted loop, body runs while index ≤ limit |
| `CASxx` | optional| optional| required(sub) | opt HI/LO/EQ; calls sub in result if F1xxF2 |
| `END`   | opt(incr)| blank  | blank         | none; closes IF/DOW/DOU/DO/CAS (incr only for DO) |
| `EXSR`  | blank   | required(name)| blank    | none; call the named subroutine          |
| `EXCPT` | blank   | opt(name)| blank       | none; write type-E O-records matching name|
| `BEGSR` | required(name)| blank| blank    | none; begin subroutine (F1 = name)       |
| `ENDSR` | opt(label)| blank| blank      | none; return from subroutine             |
| `XFOOT` | blank   | required(array)| required| +/−/Z; sum all elements into result     |
| `SQRT`  | blank   | required| required    | none; √(F2) → result, half-adjusted      |
| `LOKUP` | required| required(array)| blank | HI/LO/EQ; search array for F1, update index |
| `MOVEA` | blank   | required| required      | none; left-justified byte move (array↔field) |
| `TESTZ` | blank   | blank   | required(char)| HI plus zone, LO minus zone, EQ other (leftmost char) |
| `TESTB` | blank   | required| required(char)| HI all-off, LO mixed, EQ all-on for masked bits |
| `CHAIN` | required| required(file)| blank | cols 54-55 no-record; random read by key or RRN |
| `SETLL` | required| required(file)| blank | none; position file at first key >= F1 |
| `READE` | required| required(file)| blank | cols 58-59 EOF/unequal; read next if key == F1 |
| `READ`  | blank   | required(file)| blank | cols 58-59 EOF; read next (full-procedural/demand) |

Numeric comparisons (`COMP`, `IFxx`, `DOWxx`, `DOUxx`, `CASxx`) align factor 1
and factor 2 at their implied decimal point before comparing: if the two
operands have different decimal-position counts, the one with fewer decimals
is scaled up to match the other before the `icmp`, so e.g. a 2-decimal field
holding 1.50 correctly compares less than a 0-decimal field holding 2 (rather
than a raw-storage compare of 150 vs. 2).

**Phase 9 additions.** Alphanumeric fields (I-spec col 52 blank) compile to
`[N x i8]` globals; `MOVE`/`MOVEL` do right/left-justified byte copies; quoted
literals `'...'` work as character operands. E-spec numeric arrays (cols 27-32
name, 36-39 count, 40-42 entry length, 44 decimals) support compile-time data
(after the source, introduced by `** ` records) and run-time initialisation;
element access uses `ARRAY,INDEX` (1-based) in any factor/result field. `XFOOT`
sums an array; `SQRT` computes a half-adjusted square root.

**Phase 10 additions.** Alphanumeric `COMP` compares two character operands
(left-aligned, blank-padded) via the runtime, setting HI/LO/EQ. `LOKUP` searches
a numeric array for factor1, setting HI/LO/EQ indicators and updating a
variable index on a match. `MOVEA` does a left-justified byte move between
character operands (field↔field, array↔field). O-spec numeric fields accept an
**edit code** (col 38) applied via the runtime formatter, per Table 8 of the
manual (62103-62330): the code is one of four letter groups differing only in
sign style — `1`-`4` print no sign, `A`-`D` print a trailing `CR`, `J`-`M`
print a trailing minus, `N`-`Q` print a leading sign — and within each group
of four, the four codes vary independently by comma/decimal-point punctuation
(the first two of each four get commas and a forced decimal point; the last
two get neither unless the field has decimal positions) and by zero-balance
behavior (the 2nd and 4th code of each four blank out an exactly-zero value
instead of printing `.00`/`0`). So `1`/`A`/`J`/`N` and `2`/`B`/`K`/`O` are
comma+decimal, with the second of each pair also zero-blanking; `3`/`C`/`L`/`P`
and `4`/`D`/`M`/`Q` are plain digits, with the second of each pair also
zero-blanking.

Subroutines sit after all detail/total calcs. Each compiles to a separate
LLVM function sharing the program's globals (fields, indicators). `EXSR`
calls it; `ENDSR` returns to the caller. No recursion; no GOTO across the
subroutine boundary.

**Section B additions — tables, prerun-time, alternating arrays.** An E-spec
name beginning with `TAB` (case-insensitive) is a *table* rather than an
array; tables have no explicit indexing and instead carry a hidden 1-based
current-element shadow (`rpgs_<name>`, default 1). `LOKUP` of a bare table
name advances the shadow to the matched element, and a *related* table named
in the result field advances in lockstep (its corresponding element becomes
current). A bare table name in any factor/result field resolves to the
shadow-selected element; the explicit `TABLE,INDEX` form still works as an
ordinary array ref. **Prerun-time** arrays/tables (cols 11–18 filename) are
loaded once at the top of `main` via the `rpg_rt_load_arrays` runtime helper,
before the cycle or calc chain runs. Prerun-time numeric data defaults to
zoned-decimal ASCII; col 43 (blank/`P`/`B`) selects packed-decimal or binary
instead, decoded via `rpg_rt_get_packed`/`rpg_rt_get_binary` the same as an
ordinary I-spec field. **Alternating** arrays/tables (cols 46–57: 46–51
partner name, 52–54 entry length, 55 packed/binary format (mirrors col 43),
56 decimals, 57 sequence) are parsed and emitted as their own global; their
compile-time and prerun-time data interleave on each record (A1 B1 A2 B2 …).

**Section C additions — the numeric data model.** Every numeric field is stored
as a single signed `i32` that is a *scaled integer*: the stored value equals the
true value × 10^decimals, where `decimals` is the field's decimal-position count
(I-spec col 52, or C-spec col 52 for an inline-defined result). Arithmetic
honors this scale: ADD/SUB align operands to `max(dec1,dec2)` before computing;
MULT's result scale is `dec1+dec2`; DIV scales the numerator up to retain
precision, then all ops adjust to the result field's scale. **Half-adjust**
(C-spec col 53 = `H`) rounds by adding 5 at the first dropped digit before
truncating; it applies to ADD/SUB/MULT/DIV (and SQRT, which is always
half-adjusted). `Z-ADD`/`Z-SUB` rescale factor 2 to the result's decimals.

I-spec **col 43** selects the input field's byte encoding: blank = zoned
(ASCII digits, the default), `P` = packed-decimal (two BCD digits per byte, sign
nibble F=+/D=− in the low-order byte), `B` = binary (big-endian; 2-byte int16
or 4-byte int32). Packed and binary fields are decoded at read time by
`rpg_rt_get_packed` / `rpg_rt_get_binary` into the same scaled-integer
representation. O-spec col 44 (the output-side packed-decimal/binary
equivalent, DISK/ICF only) is not parsed at all -- an explicit, documented
gap rather than a parsed-but-inert column (see the O-spec table above).

**Sign-overpunch** governs MOVE between alphameric and numeric operands
(Section C, C10): the last digit of the character string carries the sign via
its zone — `A`–`I` = positive digits 1–9, `J`–`R` = negative digits 1–9, plain
`0`–`9` = positive. `rpg_rt_overpunch_in` decodes a character string to a signed
value (used by char→numeric MOVE); `rpg_rt_overpunch_out` encodes a value back
(numeric→char MOVE). Output formatting is decimal-aware via
`rpg_rt_line_put_num_dec` and `rpg_rt_edit_dec`, which emit the field's
fractional digits from the scaled integer.

**Section D additions — output spec gaps.** Heading (H) lines and any detail
line conditioned by the **1P** first-page indicator print once at program start,
before the cycle; 1P turns off afterward (it is a new reserved indicator index).
**Skip-before/skip-after** (O-spec cols 19–20 / 21–22) advance the output to an
absolute page line via `rpg_rt_skip`; a skip to a line at or before the current
position starts a new page (form-feed, page counter incremented). The
**PAGE** / **PAGE1**–**PAGE7** reserved output field names print a per-file page
counter (page 1 on the first page) via `rpg_rt_page`. **Per-field conditioning
indicators** (O-spec field-line cols 23–31) gate individual fields on a line —
a field whose conditions don't hold is omitted while the rest print. **Edit
words** (cols 45–70, quoted, with col 38 blank) format a numeric field via
`rpg_rt_edit_word`: blanks are replaceable (filled by source digits
right-aligned), the first `0` stops zero-suppression, the first `*` does
check-protection, a trailing `-` or `CR` is a sign (printed only if negative),
and `&` forces a literal blank.

**Section E additions — input spec gaps.** A primary file may contain multiple
record types distinguished by **record-identification codes** (I-spec cols
21–41): three 7-column sets (position / Not / C-Z-D / character), with AND/OR
continuation lines. At read time each record type's codes are matched against
the record buffer; the matching type's record-identifying indicator (cols
19–20) turns on, and records matching no type are skipped. A field's
**field-record-relation** (cols 63–64) ties it to a record type so it is
extracted only when that indicator is on. **Field indicators** (cols 65–66 /
67–68 / 69–70) turn on at read time when a numeric field is positive / negative
/ zero (for an alphameric field, the 69–70 indicator fires on all-blank). A
`**` record-identification line marks **look-ahead fields**: the field lines
that follow are decoded from the *next* (uncommitted) record via
`rpg_rt_peek_next`, and fill with 9s at end-of-file.

**Section F additions — cycle & matching.** When a program declares a **primary
file plus one or more secondary files** (F-spec designation `S`), it is compiled
to a separate multifile cycle rather than the single-file cycle. Each input
file's current record is held in its own buffer (`rpg_rec_<file>`, with a `got_`
valid flag); each cycle selects one record to process. An input field tagged
**M1** (I-spec cols 61–62) is a match field — files carrying M1 are merged in
ascending key order (ties keep the higher-priority file, primary first), and the
**MR** indicator turns on when the selected record's M1 equals another held
record's M1. Files *without* an M1 are processed by priority (primary fully,
then secondaries in F-spec order). The selected record is copied into the shared
`rpg_rec` and only that file's fields are decoded, so the rest of the cycle
(total/detail calc and output) is identical to the single-file path. Only a
single numeric M1 field is supported; combined M1–M9 keys and alphameric match
fields are future work.

**Overflow** (F22): a PRINTER file may carry an overflow indicator in **F-spec
cols 33–34** (`OA`–`OG` or `OV`). The overflow line is taken from a line-counter
**L-spec** (form type `L`: cols 7–14 filename, 15–17 lines per page, 20–22
overflow line), defaulting to six lines from the bottom of a 66-line page. The
runtime latches overflow when printed output reaches the overflow line; after
total output each cycle the compiler polls the latch (`rpg_rt_take_overflow`),
turns the overflow indicator on, runs the overflow-conditioned Heading/Detail/
Total output, and turns the indicator off. (Indicator-driven; automatic form-
feed advance without an assigned indicator is future work.)

**Section G additions — file handling.** `CHAIN`/`SETLL`/`READE`/`READ` provide
keyed and positioned access to DISK files (C-specs). An indexed file declares
its key on the F-spec (cols 29–30 length, 35–38 start); the runtime builds an
in-memory key→offset index on first use, and `CHAIN` binary-searches it (or,
with no key, reads by relative-record number). `SETLL` positions at the lower
bound; `READ`/`READE` advance sequentially (`READE` only while the key matches).
The no-record indicator is cols 54–55; EOF/unequal is cols 58–59.

**Update files** (F-spec type `U`, G25) open `r+` for in-place rewrite. The
record operation is driven by the **O-spec**: `ADD` in cols 16–18 appends,
type `U` in col 15 (or `UPDATE` semantics) rewrites the last-read record, and
`DEL` in cols 16–18 marks it deleted (filled with 0xFF). ADD (cycle append)
and CHAIN-based UPDATE are exercised by the tests.

Control levels (L1–L9) are assigned to input fields on the **I-spec, cols
59–60**. The cycle detects a break when a control field's value changes
between records; the broken level and all lower levels turn on (cascade:
L3 → L1, L2, L3). C-specs conditioned by `L1`..`L9` (cols 7–8) run at total
time when that level or higher is on; `L0` runs every total time; `LR` runs
at last record. At LR all of L1–L9 turn on, so the final group's subtotals
print. Total calcs/output run in ascending level order (L0, L1, …, L9, LR).
Per the manual's first-cycle rule, the first record establishes the control-
field baseline but does **not** cascade L1–L9 — totals (and hence breaks) are
bypassed until after the first record carrying control fields is processed
(Section F, F23).

`xx` for IF/DOW/DOU is one of `GT LT EQ NE GE LE` (compare F1 to F2).

Arithmetic rules: all of ADD/SUB/MULT/DIV let factor 1 be omitted (the result
field substitutes); factor 2 is always required. SUB = F1−F2, MULT = F1×F2,
DIV = F1(dividend)/F2(divisor) → quotient in result. MVR must immediately
follow a DIV and moves the remainder to its result field. Overflow is silent
truncation (max 15 digits, signed). Half-adjust (col 53 `H`) rounds the result
for ADD/SUB/MULT/DIV (not MVR, not DIV-then-MVR).

Structured-op rules: a single `END` operation closes IFxx, DOWxx, DOUxx, DO,
and CAS groups (there is no ENDIF/ENDDO in S/36 RPG II). `DOWxx` tests at the
top (body may run zero times); `DOUxx` tests at the bottom (body runs at least
once, exits when the condition becomes true). `DO` is a counted loop: it moves
factor 1 (start, default 1) into the index (result field; compiler-generated if
blank), runs the body while the index ≤ factor 2 (limit), and at END adds the
END's factor 2 (increment, default 1) to the index. `CASxx` opens a case group
(one or more CASxx ops then END); each CASxx compares F1/F2 and, if true, calls
the subroutine named in the result field — `CAS` with blank xx is an
unconditional default that runs like EXSR. Groups nest up to 100 deep. There is
no `ANDxx`/`ORxx` operation in this dialect — compound conditions use AN/OR
lines (cols 7–8) over indicators.

**Section A additions.** `Z-SUB` stores the negation of factor 2. `EXCPT` (with
an optional name in factor 2) writes type-E O-records at calculation time: a
named EXCPT writes only E-records whose cols 32–37 name matches; a blank
factor 2 writes only the unnamed E-records. The O-spec parser now carries the
filename forward across continuation record lines. `TESTZ` tests the "zone" of
the leftmost character of the (alphameric) result field — since this is an ASCII
compiler with no EBCDIC zone, the plus zone is the manual's explicit set `&`
and A–I (plus a–i as an ASCII extension), the minus zone is `-` and J–R (plus
j–r), and anything else is the zero zone. `TESTB` tests the bits named by factor
2 (a `'025'`-style bit-number literal where 0 is the leftmost bit, or the ON
bits of a 1-position character field) against the result field: HI if every
tested bit is off, LO if mixed, EQ if every tested bit is on.

## Indicators

- General: `01`–`99`
- Control level: `L0`–`L9` (`L0` always on at total time)
- Last record: `LR`
- Matching record: `MR` *(on when the selected record's M1 equals another held
  record's M1 in the multifile cycle; Section F)*
- Halt: `H1`–`H9`
- Overflow: `OA`–`OG`, `OV`  *(no `OF`/`ON` in SC09-1818); latched by the runtime
  when output reaches the overflow line, polled at total time, Section F)*
- External: `U1`–`U8`
- Function keys: `KA`–`KN`, `KP`–`KY` (`KO` reserved)
- First page: `1P` (output only, internal)

Internally each special is a dedicated `i1` global: `01`–`99` live in a single
`[100 x i1]` array (`@rpg_in`), and `LR`, `L1`–`L9`, `1P`, `MR`, `OA`–`OG`, `OV`
each get their own global. `H1`–`H9`, `U1`–`U8`, and the function-key indicators
are accepted lexically but carry no behavior yet.
