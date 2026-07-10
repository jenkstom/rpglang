# RPG II Spec Column Map

Authoritative column positions, extracted from the IBM *System/36-Compatible
RPG II User's Guide and Reference* (SC09-1818-00), the `c0918180.pdf` shipped
with this project. See `docs/ref/manual_layout.txt` for the raw extraction.

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
| File Format       | 19      | F (fixed) common; also V/S/M/D/E                 |
| Record Length     | 24–27   | ends col 27                                      |
| Overflow Indicator| 33–34   | OA–OG, OV                                        |
| Extension Code    | 39      | E / L                                            |
| Device            | 40–46   | DISK / WORKSTN / PRINTER / SPECIAL / ...         |

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
| Type                     | 15      | **H** / **D** / **T** / **E** (Heading/Detail/Total/Exception) |
| Space Before             | 17      | 0–3 lines                                          |
| Space After              | 18      | 0–3; all-blank 17–22 ⇒ single-space after          |
| Skip Before              | 19–20   | line number 01–99 / A0–B2                          |
| Skip After               | 21–22   | line number                                        |
| Record Conditioning Inds | 23–31   | 3 groups `[N]II` (23–25 / 26–28 / 29–31)           |
| EXCPT name               | 32–37   | type E only                                        |

**Field line** (cols 7–31 blank):
| Field                | Cols    | Notes                                            |
|----------------------|---------|--------------------------------------------------|
| Field Conditioning   | 23–31   | per-field indicators                             |
| Field Name           | 32–37   | field name, or blank if a constant follows       |
| Edit Code            | 38      | leading-zero / sign / punctuation control        |
| Blank After          | 39      | `B` resets field to blanks/zeros after output    |
| End Position         | 40–43   | rightmost output position (right-justified); blank = pack after previous |
| Packed/Binary        | 44      | P / B (DISK output only)                         |
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
| `END`   | blank   | blank   | blank         | none; closes IF/DOW/DOU (single END op)  |
| `EXSR`  | blank   | required(name)| blank    | none; call the named subroutine          |
| `BEGSR` | required(name)| blank| blank    | none; begin subroutine (F1 = name)       |
| `ENDSR` | opt(label)| blank| blank      | none; return from subroutine             |
| `XFOOT` | blank   | required(array)| required| +/−/Z; sum all elements into result     |
| `SQRT`  | blank   | required| required    | none; √(F2) → result, half-adjusted      |
| `LOKUP` | required| required(array)| blank | HI/LO/EQ; search array for F1, update index |
| `MOVEA` | blank   | required| required      | none; left-justified byte move (array↔field) |

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
**edit code** (col 38: `1`–`4` for comma/decimal formatting, `J`–`M` with a
trailing minus, `N`–`Q` with a leading sign) applied via the runtime formatter.

Subroutines sit after all detail/total calcs. Each compiles to a separate
LLVM function sharing the program's globals (fields, indicators). `EXSR`
calls it; `ENDSR` returns to the caller. No recursion; no GOTO across the
subroutine boundary.

Control levels (L1–L9) are assigned to input fields on the **I-spec, cols
59–60**. The cycle detects a break when a control field's value changes
between records; the broken level and all lower levels turn on (cascade:
L3 → L1, L2, L3). C-specs conditioned by `L1`..`L9` (cols 7–8) run at total
time when that level or higher is on; `L0` runs every total time; `LR` runs
at last record. At LR all of L1–L9 turn on, so the final group's subtotals
print. Total calcs/output run in ascending level order (L0, L1, …, L9, LR).

`xx` for IF/DOW/DOU is one of `GT LT EQ NE GE LE` (compare F1 to F2).

Arithmetic rules: all of ADD/SUB/MULT/DIV let factor 1 be omitted (the result
field substitutes); factor 2 is always required. SUB = F1−F2, MULT = F1×F2,
DIV = F1(dividend)/F2(divisor) → quotient in result. MVR must immediately
follow a DIV and moves the remainder to its result field. Overflow is silent
truncation (max 15 digits, signed). Half-adjust (col 53 `H`) rounds the result
for ADD/SUB/MULT/DIV (not MVR, not DIV-then-MVR).

Structured-op rules: a single `END` operation closes IFxx, DOWxx, and DOUxx
groups (there is no ENDIF/ENDDO in S/36 RPG II). `DOWxx` tests at the top
(body may run zero times); `DOUxx` tests at the bottom (body runs at least
once, exits when the condition becomes true). Groups nest up to 100 deep.
There is no `ANDxx`/`ORxx` operation in this dialect — compound conditions use
AN/OR lines (cols 7–8) over indicators.

## Indicators

- General: `01`–`99`
- Control level: `L0`–`L9` (`L0` always on at total time)
- Last record: `LR`
- Matching record: `MR`
- Halt: `H1`–`H9`
- Overflow: `OA`–`OG`, `OV`  *(no `OF`/`ON` in SC09-1818)*
- External: `U1`–`U8`
- Function keys: `KA`–`KN`, `KP`–`KY` (`KO` reserved)
- First page: `1P` (output only, internal)

For Phase 2 we model `01`–`99` + `LR` as a flat array of `i1`; other specials
are accepted lexically but only `LR` carries behavior yet.
