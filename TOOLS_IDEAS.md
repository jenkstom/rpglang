# RPG Analyze — Specification

A single command-line tool, **`rpg-analyze`**, that analyzes RPG II source files.
It unifies every analysis idea in the project into one binary. Each analysis
function is a **module** that can run on its own, or be combined with others to
produce a comprehensive synthesized report.

This document is a complete handoff spec for a developer to implement the tool.

---

## 1. Goals

- **One binary, many functions.** No suite of separate commands to remember.
- **Composable.** Run a single module, a hand-picked subset, or the full report.
- **Synthesis, not just data.** The headline output is a prioritized **findings
  list** — issues backed by evidence from the modules above them.
- **Reuse the compiler frontend.** The compiler already parses F/I/C/E/O/H specs
  into a typed `Program` struct (see `compiler/src/program.h`). `rpg-analyze`
  consumes that same parsed representation rather than re-parsing source text.
- **Machine-readable.** A `--json` mode emits structured output so portfolio-level
  tools (migration scoring, dashboards) can aggregate many files.

## 2. Non-goals (v1)

- No mutation of source files. (A separate `format` subcommand may lint/normalize,
  but the analysis core is read-only.)
- No execution of the program. Analysis is static.
- No database or GUI. CLI + files only.

---

## 3. CLI design

```
rpg-analyze [global options] [command] [command options] <file...>
```

### 3.1 Commands

| Command     | Default? | Description                                                       |
|-------------|----------|-------------------------------------------------------------------|
| `report`    | yes      | Run analysis modules and emit a synthesized report + findings.    |
| `decode`    |          | Decode one or more raw spec lines into labeled columns.           |
| `search`    |          | Structured column-aware query across files.                       |
| `diff`      |          | Semantic structural diff between two programs.                    |
| `format`    |          | Lint / canonicalize formatting (read-only check in v1).           |
| `docgen`    |          | Generate a Markdown reference page for one program.               |
| `callgraph` |          | Inter-program CALL/shared-file dependency graph (multi-file).     |
| `duplicate` |          | Clone / copy-paste detector across many files.                    |
| `portfolio` |          | Codebase-wide metrics dashboard across many files.                |
| `migrate`   |          | Migration-difficulty scoring across many files.                   |

**Any analysis module name is also a valid command**, as an alias for
`report --module <name>`. So `rpg-analyze xref prog.rpg` runs only the `xref`
module. This is the primary way to "run one function separately."

When no command is given, `report` is assumed.

### 3.2 Global options

| Flag                | Description                                                        |
|---------------------|--------------------------------------------------------------------|
| `--json`            | Emit machine-readable JSON instead of text.                        |
| `--no-color`        | Disable ANSI color.                                                |
| `--quiet` / `-q`    | Suppress section bodies; print only the findings list.             |
| `--severity LEVEL`  | Minimum severity to show: `error`, `warn`, `info`.                 |
| `--compiler-feats F`| Path to a features file (default `docs/SPEC_MAP.md` + the root-level `*_PLAN.md` documents for still-unimplemented features) used by the `compat` module. |
| `-h` / `--help`     | Help.                                                              |
| `-V` / `--version`  | Version.                                                           |

### 3.3 `report` options (module selection)

Module selection is the core composition mechanism. Repeatable flags let you
build any combination.

| Flag                    | Description                                                       |
|-------------------------|-------------------------------------------------------------------|
| `-m` / `--module NAME`  | Enable one module (repeatable).                                   |
| `-a` / `--all`          | Enable every analysis module (the default).                       |
| `--no-module NAME`      | Disable one module (use with `--all` to exclude).                 |
| `--no-findings`         | Suppress the synthesized findings section; print module output only. |
| `--section-order a,b,c` | Custom ordering of report sections.                               |
| `-o` / `--output FILE`  | Write report to a file instead of stdout.                         |

**Semantics:**
- No `-m` and no `-a` → `--all`.
- `-m xref -m indicators` → only those two.
- `-a --no-module compat` → everything except `compat`.
- A module used as a top-level command (`rpg-analyze xref FILE`) is equivalent to
  `rpg-analyze report -m xref FILE`.

### 3.4 Exit codes

| Code | Meaning                                                      |
|------|--------------------------------------------------------------|
| 0    | Analysis complete; no ERROR-severity findings.               |
| 1    | Analysis complete; one or more ERROR findings emitted.       |
| 2    | Could not parse the input program (hard syntax error).       |
| 3    | Bad CLI usage / unknown module / file not found.             |

`--severity` and CI mode: in CI you typically want non-zero only on `error`.

---

## 4. Analysis modules

Every module has a stable **ID** (used in `-m`, in JSON output, and in finding
evidence). The 20 modules below are the building blocks of the report. Each is
specified with: the question it answers, its inputs, what it outputs, and the
findings it can emit.

### Module contract

Each module is a pure function over the shared IR:

```
ModuleResult run(const ProgramIR& ir, const ModuleOptions& opts);

struct ModuleResult {
    string          id;
    string          title;
    vector<Section> sections;   // rendered output blocks (text or JSON nodes)
    vector<Finding> findings;   // issues merged into the report's findings list
};
```

Modules must be **independent and orderable**: a module may depend on the shared
IR and on the *output* of modules declared earlier in the catalog order, never on
later ones. The report runner executes modules in catalog order (or in
`--section-order`).

### 4.1 `summary` — program profile

**Answers:** "What even is this program?" — the triage overview.
**Inputs:** whole IR.
**Outputs:** program-id, spec line counts by type (F/I/C/E/O/H/L), file count,
indicator count, array/table count, subroutine count, presence of control levels
/ matching fields / external indicators / multifile, total C-spec count.
**Findings:** none (pure descriptor).

### 4.2 `files` — file description map

**Answers:** "What files does this touch, and how?"
**Inputs:** `Program.files` (FSpecs), L-specs.
**Outputs:** per file: name, type (I/O/U/C), designation, device, format,
record length, key length/start, organization, overflow indicator, external
condition indicator, EOF-required flag.
**Findings:**
- `FILES-WORKSTN` (ERROR) — WORKSTN/SPECIAL/CONSOLE device, unsupported.
- `FILES-RECADDR` (ERROR) — record-address file designation, unsupported.
- `FILES-NOSIZE` (WARN) — record length blank/zero on a fixed-format file.

### 4.3 `recordmap` — record layout reconstruction

**Answers:** "What is the exact byte layout of each record?"
**Inputs:** `in_records`, `in_fields`, FSpec record lengths.
**Outputs:** per file/record-type, an ASCII byte map: field name, start, end,
length, data format (zoned/packed/binary/alpha), decimals. Packed/binary lengths
are decoded to true byte widths.
**Findings:**
- `BUF-OVERLAP` (ERROR) — two fields occupy overlapping byte ranges.
- `BUF-OVERRUN` (ERROR) — a field extends past the file's declared record length.
- `BUF-GAP` (INFO) — unassigned gap between fields (possible missing field).

### 4.4 `fields` — field definition catalog

**Answers:** "What symbols exist and how are they declared?"
**Inputs:** `in_fields`, `ds_subfields`, `arrays`, C-spec inline results.
**Outputs:** per symbol: name, kind (input/DS-subfield/array/table/inline),
type, length, decimals, data format, owning file or DS, definition line.
**Findings:**
- `FIELD-DUPNAME` (WARN) — same name defined in two places with differing attrs.
- `FIELD-TEMPNAME` (INFO) — heuristic single-letter/temp names (W1, T9).

### 4.5 `xref` — field cross-reference

**Answers:** "Where is each field defined, and where is it used?"
**Inputs:** whole IR.
**Outputs:** per symbol, a list of reference sites: spec type, line, role
(factor1 / factor2 / result / condition / output), read or write.
**Findings:**
- `DEAD-FIELD` (INFO) — defined but never referenced.
- `XREF-WRITEONLY` (WARN) — result field written but never read.

### 4.6 `indicators` — indicator lifecycle trace

**Answers:** "How does each indicator get turned on/off and tested?"
**Inputs:** CSpecs (conditions, SETON/SETOFF, resulting indicators), ISpec field
indicators, OSpec record/field conditioning, FSpec overflow/condition indicators.
**Outputs:** per indicator: every SET, CLEAR, TEST site with line + role; a
classification (general 01–99 / control-level L1–L9 / overflow OA–OG,OV /
external U1–U8 / KA–KY / 1P / LR).
**Findings:**
- `IND-UNSET` (WARN) — tested but never set (always default state).
- `IND-UNUSED` (INFO) — set but never tested.
- `IND-LRMISSING` (see `termination`).

### 4.7 `subroutines` — intra-program subroutine map

**Answers:** "What subroutines exist, what do they call, what do they touch?"
**Inputs:** CSpecs (BEGSR/ENDSR/EXSR/CASxx), the indicator/field tables.
**Outputs:** per subroutine: name, entry line, the subroutines it EXSRs, the
indicators it reads vs. writes, the fields it modifies (result writes), and its
cyclomatic-style complexity.
**Findings:**
- `DEAD-SUBR` (INFO) — subroutine never invoked by any EXSR/CASxx.
- `SUBR-COMMCOUPLING` (WARN) — two subroutines communicate only via shared
  indicators (implicit coupling).

### 4.8 `controlflow` — control-flow graph & reachability

**Answers:** "What is the executable structure, and is any of it unreachable?"
**Inputs:** CSpecs (GOTO/TAG/CAB, IF/DOW/DOU/DO/ELSE/END, EXSR).
**Outputs:** a CFG of basic blocks; the GOTO/TAG label table; a reachability
report listing unreachable blocks; nesting-depth profile.
**Findings:**
- `FLOW-UNREACHABLE` (WARN) — a block has no entry path.
- `FLOW-GOTODENSITY` (WARN) — GOTO/TAG ratio above threshold (spaghetti risk).
- `FLOW-CROSSSUBR` (ERROR) — GOTO that crosses a subroutine boundary.

### 4.9 `cycle` — program-cycle / control-level decoder

**Answers:** "What does the implicit RPG cycle do here?"
**Inputs:** `in_fields` control_level & match_field columns, FSpec designations.
**Outputs:** input file sequence and priority; which fields carry L1–L9 (control
breaks) and M1–M9 (matching); reconstructed control-break and multifile-join
logic expressed as explicit steps.
**Findings:**
- `CYC-MULTIMATCH` (WARN) — multiple/combined M-fields beyond the single-M1 the
  compiler supports.
- `CYC-ORPHANCL` (INFO) — control level set on a field but no detail/total calc
  conditioned on that level.

### 4.10 `complexity` — complexity metrics

**Answers:** "How hard is this program, and where?"
**Inputs:** CSpecs.
**Outputs:** total opcode count; opcode frequency histogram; conditioning-indicator
nesting depth (max and mean); GOTO/TAG density; subroutine count; a per-section
cyclomatic-style score; an overall complexity tier (low/med/high/extreme).
**Findings:**
- `CPLX-HIGH` (WARN) — section exceeds complexity threshold.

### 4.11 `deadcode` — unused-definition & dead-path finder

**Answers:** "What can I safely ignore?"
**Inputs:** whole IR; reuses `xref`, `indicators`, `controlflow`, `subroutines`.
**Outputs:** consolidated list of dead fields, dead indicators, dead subroutines,
unreachable blocks, and files opened-but-unused.
**Findings:** `DEAD-FIELD`, `DEAD-SUBR`, `IND-UNUSED`, `FLOW-UNREACHABLE`,
`DEAD-FILE` (INFO — file never read/written).

### 4.12 `security` — risky-pattern scanner

**Answers:** "Where are the latent bugs / unsafe assumptions?"
**Inputs:** CSpecs, FSpecs.
**Outputs:** list of risky constructs with rationale.
**Findings:**
- `SEC-UPDATE-NOREAD` (WARN) — UPDATE/DELET with no preceding READ/CHAIN.
- `SEC-NOEOFIND` (WARN) — READ/CHAIN without an EOF/no-record indicator set.
- `SEC-UNCHECKED-U` (INFO) — external U1–U8 tested but its source is uncontrolled.
- `SEC-UNVALIDATED` (INFO) — input field flows into arithmetic/output unchecked.
- `SEC-DIVBYZERO` (WARN) — DIV whose factor2 is not guarded.

### 4.13 `compat` — compiler-coverage checker

**Answers:** "Will this program compile under *this* compiler?"
**Inputs:** IR (built via the **lenient parse path**, §7.1) + a features descriptor
derived from `docs/SPEC_MAP.md` covering the remaining unimplemented features
(externally-described files).
**Outputs:** list of unsupported opcodes, devices, designations, spec types, and
features the program uses (e.g. WORKSTN, record-address, recursion,
combined M-keys). Each item cites the spec/line. Because parsing is lenient,
`compat` runs even on programs the compiler would reject — which is precisely
the point.
**Findings:**
- `COMPAT-OP` (ERROR) — unsupported opcode (detected via `Op::Unknown` + `op_text`).
- `COMPAT-DEVICE` (ERROR) — unsupported device.
- `COMPAT-FEATURE` (ERROR/WARN) — unsupported feature (e.g. WORKSTN, recursion).
- `COMPAT-IND` (WARN) — raw indicator token that did not resolve (silently dropped
  by the strict parser; recovered from raw columns).

### 4.14 `condlogic` — conditioning-indicator reconstruction

**Answers:** "What do these indicator columns actually mean as logic?"
**Inputs:** CSpec/O-spec conditioning columns.
**Outputs:** each conditioned line translated to readable pseudo-code, e.g.
`if (N20 and 11) ...`; AND/OR continuation lines folded into compound conditions.
**Findings:**
- `COND-DEEPCOND` (INFO) — conditioning nesting beyond a threshold.

### 4.15 `buffer` — buffer integrity check

*(Largely overlaps `recordmap` findings; kept separate because it also covers DS
subfields and array entry lengths.)*
**Findings:** `BUF-OVERLAP`, `BUF-OVERRUN`, `BUF-GAP`, plus
- `BUF-PACKEDLEN` (ERROR) — packed/binary length inconsistent with stated decimals.
- `BUF-DSOVERLAP` (ERROR) — DS subfield overlap.

### 4.16 `termination` — LR / exit analysis

**Answers:** "Can this program terminate correctly?"
**Inputs:** CSpecs (SETON LR), FSpec `end_required`, the CFG.
**Outputs:** every path's termination behavior; whether LR is set on all paths.
**Findings:**
- `TERM-NOLR` (WARN) — no SETON LR anywhere; program may loop forever.
- `TERM-PATHNOLR` (WARN) — a reachable exit path never sets LR.
- `TERM-EOFNOTREQ` (INFO) — primary input lacks end-required but is read in a loop.

### 4.17 `dataflow` — output-field data-flow traces

**Answers:** "For each printed/output value, where did it come from?"
**Inputs:** OSpecs + CSpecs + ISpecs.
**Outputs:** per O-spec output field, a backward trace: output ← result field ←
opcode ← input fields/literals, as far as statically resolvable.
**Findings:**
- `DATA-UNINIT` (WARN) — output field whose source is never assigned on some path.
- `DATA-TRUNC` (INFO) — source width exceeds output field width.

### 4.18 `deps` — external dependency inventory

**Answers:** "What else must exist for this program to run?"
**Inputs:** FSpecs, CALL/CABL opcodes, external indicators U1–U8.
**Outputs:** required input files, output files, called programs, external
indicators the program expects.
**Findings:**
- `DEPS-MISSINGFILE` (INFO, when a manifest is supplied via `--manifest`) — file
  not present in the deployment manifest.

### 4.19 `comments` — comment / TODO mining

**Answers:** "What did the original authors flag?"
**Inputs:** raw source lines (`*`/`**` comments).
**Outputs:** every comment matching markers (TODO, FIXME, HACK, TEMP, XXX, NOTE),
with line and surrounding context; a full comment index optionally.
**Findings:**
- `COMM-MARKER` (INFO) — a flagged marker found.

### 4.20 `smells` — anomaly / smell detection

**Answers:** "What looks like decades of patches?"
**Inputs:** whole IR; heuristic rules.
**Outputs:** list of smells with the triggering evidence.
**Findings:**
- `SMELL-TEMPFLAG` (INFO) — single-letter indicators used as ad-hoc work flags.
- `SMELL-WEIRDOP` (INFO) — unusual opcode combination.
- `SMELL-HA-NODEC` (INFO) — half-adjust set with no decimal positions.
- `SMELL-REDEFINE` (INFO) — DS redefinition patterns worth a human look.

---

## 5. The synthesized report

When running `report` (default or `-a`), the output is a single document with a
fixed section order. Each enabled module contributes one section, in catalog
order (overridable via `--section-order`). After all sections, a **Findings**
section merges every module's findings, de-duplicated and sorted by severity.

```
═══════════════════════════════════════════════════════════
 RPG ANALYSIS — <program-id>  (<file>)
═══════════════════════════════════════════════════════════

 1. OVERVIEW           summary
 2. FILES              files, recordmap
 3. FIELDS             fields
 4. CROSS-REFERENCE    xref
 5. INDICATORS         indicators
 6. CONTROL FLOW       controlflow, subroutines, condlogic
 7. PROGRAM CYCLE      cycle
 8. COMPLEXITY         complexity
 9. DATA FLOW          dataflow
10. DEPENDENCIES       deps
11. NOTES              comments, smells
12. DEAD CODE          deadcode
13. COMPILER COMPAT    compat
14. SECURITY           security, termination, buffer

═══ FINDINGS (n) ═══
 [ERROR] <ID>  file:line  message
         → evidence: see §<section>
 [WARN]  <ID>  file:line  message
 [INFO]  <ID>  file:line  message

Summary: n error(s), m warning(s), k info.
```

### 5.1 Findings model

Every finding is a structured record:

```
Finding {
  id:        string    // stable check id, e.g. "BUF-OVERLAP"
  severity:  enum      // ERROR | WARN | INFO
  module:    string    // producing module id
  message:   string    // one-line human description
  location:  Location  // { file, line, spec, columns }
  evidence:  [Ref]     // pointers to report sections / other findings
  rule:      string?   // optional doc anchor for the rule (e.g. SPEC_MAP §)
}
```

Severity policy:
- **ERROR** — will not compile, or data-corruption / always-wrong behavior.
- **WARN** — likely bug or maintenance hazard; human must review.
- **INFO** — observation / candidate for cleanup; not actionable on its own.

De-duplication: two findings with the same `id` + `location` are merged (the
`evidence` lists union). `--severity` filters the displayed (and JSON-emitted)
set but does not change computation.

---

## 6. Output formats

### 6.1 Text (default)

Human-readable, ANSI-colored unless `--no-color`. Sections separated by rule.
`--quiet` prints only the FINDINGS block.

### 6.2 JSON (`--json`)

A single JSON object per file (an array of these when multiple files given):

```json
{
  "file": "tests/e2e.rpg",
  "program_id": "E2E",
  "modules_run": ["summary", "files", "xref", "..."],
  "sections": [
    { "id": "summary", "title": "Overview", "data": { "...": "..." } }
  ],
  "findings": [
    { "id": "IND-UNSET", "severity": "WARN",
      "module": "indicators", "message": "Indicator 25 tested but never set",
      "location": { "file": "tests/e2e.rpg", "line": 14, "spec": "C", "columns": "9-11" },
      "evidence": [ { "section": "indicators", "line": 14 } ] }
  ],
  "summary": { "errors": 0, "warnings": 3, "info": 5 }
}
```

The JSON schema is the contract consumed by `portfolio` and `migrate`.

---

## 7. Shared IR

`rpg-analyze` does **not** re-parse source. It reuses the compiler's frontend:

```
rpgc::Program   (compiler/src/program.h)
 ├─ HSpec      hspec
 ├─ vector<FSpec>       files          // compiler/src/fspec.h
 ├─ vector<ISpecRec>    in_records     // compiler/src/ispec.h
 ├─ vector<ISpecField>  in_fields
 ├─ vector<ISpecField>  lookahead_fields
 ├─ vector<ISpecDS>     data_structures
 ├─ vector<ISpecSubfield> ds_subfields
 ├─ vector<CSpec>       calcs          // compiler/src/cspec.h
 ├─ vector<ORecord>     outputs        // compiler/src/ospec.h
 └─ vector<ESpecArray>  arrays         // compiler/src/espec.h
```

The implementation should expose the parser as a library (see §9) so that
`rpg-analyze` calls the existing `parse_*` functions and builds a
`rpgc::Program`. A thin `ProgramIR` wrapper may add analysis conveniences
(symbol table, indicator table, CFG) built once and shared by all modules.

**Key derived structures** (built once from `Program`, cached in `ProgramIR`):
- `SymbolTable` — name → {definition site, attributes, references}.
- `IndicatorTable` — indicator index → {classification, set/test/clear sites}.
- `ControlFlowGraph` — basic blocks from CSpecs.
- `SubroutineTable` — BEGSR/ENDSR ranges + EXSR/CAS edges.

### 7.1 Handling unsupported opcodes & features (lenient parse)

The compiler's parser is **lossy and partial** in two ways that the analyzer
must compensate for. This is the central design constraint of the IR layer:

1. **Unknown opcodes are preserved.** `parse_op()` returns `Op::Unknown`, but
   the raw opcode text survives in `CSpec::op_text` (cspec.cpp:196-198). So
   `compat` can detect unknown opcodes directly from the existing IR. Good.

2. **Unknown indicator tokens are silently dropped.** `ind_token()`
   (cspec.cpp:73) returns `0` for any unrecognized indicator, and
   `parse_conditions` then drops the condition entirely. A calc conditioned on
   an unsupported indicator becomes *invisible* to the `condlogic` and
   `indicators` modules — it looks unconditional. The typed IR has already
   lost the information by the time the analyzer sees it.

3. **Some unsupported features are hard errors** (WORKSTN/SPECIAL/CONSOLE
   devices, record-address files, recursion). These are reported via
   `diagnostics`, but whether `parse_program` *aborts* vs. reports-and-continues
   determines whether the IR is even built. If parsing aborts, `compat` — the
   module whose entire job is to report "this won't compile" — can never run
   on the very inputs that need it most. That is a bootstrapping failure.

**Requirement: the analyzer must use a lenient parse path**, distinct from the
compiler's strict path. Concretely:

- **Never abort.** A parse failure (hard error in the strict compiler) must be
  recorded as a `Finding` of severity `ERROR` (e.g. `COMPAT-FEATURE`) and parsing
  must continue to the extent possible. The analyzer's exit code already
  reflects ERROR findings (§3.4), so this preserves "non-zero on uncompilable"
  semantics without losing the rest of the analysis.
- **Preserve raw source.** `ProgramIR` must retain the original `SourceLine`
  vector alongside the typed structs, so any module can fall back to reading raw
  columns when the normalized IR has dropped information (the indicator case
  above). `condlogic` and `indicators` must consult raw columns 9-17 / 54-59 to
  recover indicator tokens that `parse_conditions` discarded.
- **Capture the unknown.** `compat` enumerates `Op::Unknown` entries (using
  `op_text`), plus any `Device`/`FileDesign` that maps to a hard-error case, plus
  raw indicator tokens that did not resolve.

The library refactor (§9) should expose two entry points:

```
// Strict: aborts on hard errors (current compiler behavior).
Program parse_program(Source);

// Lenient: never aborts; hard errors collected in diagnostics for the
// analyzer to fold into findings. IR built best-effort.
Program parse_program_lenient(Source, Diagnostics&);
```

Modules that need the dropped information read from `ProgramIR::raw_lines`.

---

## 8. Utility & multi-file subcommands

These are separate subcommands (not report modules) because they are interactive,
multi-file, or produce a different artifact. They share the same parser/IR.

### 8.1 `decode` — column decoder
`rpg-analyze decode <file> [line-range]`
Pretty-prints each line with every column field labeled. Reads `SPEC_MAP.md` for
the layout. The first thing you run on an unfamiliar line.

### 8.2 `search` — structured query
`rpg-analyze search <file...> --query '<expr>'`
Query DSL examples:
- `op:COMP ind:20`
- `field:AMT`
- `spec:C cond:N90`
- `cols:30-50`
Returns matching lines with decoded columns. A column-aware `grep`.

### 8.3 `diff` — semantic diff
`rpg-analyze diff <file_a> <file_b>`
Reports field/opcode/indicator-level changes, ignoring whitespace and renumbering.

### 8.4 `format` — linter (v1: check-only)
`rpg-analyze format --check <file...>`
Reports column-overflow, sequence errors, trailing-whitespace, form-type
misalignment. Does not rewrite in v1.

### 8.5 `docgen` — documentation generator
`rpg-analyze docgen <file> [-o out.md]`
Emits a Markdown reference page: comments, files, field table, subroutines,
indicator usage, with links into `xref` output.

### 8.6 `callgraph` — inter-program graph
`rpg-analyze callgraph <file...> [--dot]`
Scans CALL/CABL and shared file names; emits a dependency graph (Graphviz dot or
text tree).

### 8.7 `duplicate` — clone detector
`rpg-analyze duplicate <file...> [--threshold 0.9]`
Finds structurally similar opcode sequences / same-named subroutines across
files; reports with similarity scores.

### 8.8 `portfolio` — codebase dashboard
`rpg-analyze portfolio <file...> [--html]`
Aggregates per-file JSON (`--json` of `report`) into LOC, opcode frequencies,
indicator heatmaps, most-referenced files, and the most complex programs.

### 8.9 `migrate` — migration difficulty scoring
`rpg-analyze migrate <file...>`
Consumes per-file JSON; scores each program (unsupported opcodes, indicator
logic, cycle dependence, GOTO density) into tiers (trivial / moderate / rewrite)
and clusters similar programs.

---

## 9. Architecture & reuse

```
compiler/src/         ← existing parsers (parse_fspecs, parse_ispecs,
                        parse_cspecs, parse_ospecs, parse_especs, ...)
       │
       ▼  (expose as a static/shared library: librpgc_parse)
analyze/src/
 ├─ ir.{h,cpp}         ProgramIR + derived tables (SymbolTable, etc.)
 ├─ modules/
 │   ├─ summary.cpp
 │   ├─ files.cpp
 │   ├─ recordmap.cpp
 │   ├─ ... (one file per module)
 │   └─ registry.cpp    module registry: id → factory, catalog order, deps
 ├─ report.cpp         runs modules, orders sections, merges findings
 ├─ findings.cpp       Finding type, de-dup, severity sort, filtering
 ├─ render_text.cpp    text/ANSI renderer
 ├─ render_json.cpp    JSON renderer (schema in §6.2)
 ├─ cmds/              decode, search, diff, format, docgen, callgraph,
 │                     duplicate, portfolio, migrate
 └─ main.cpp           arg parsing, dispatch
```

**Build:** add a `analyze/` CMake target linking `librpgc_parse`. The compiler's
`main.cpp` currently drives parsing+codegen directly; factor the parse step into
a reusable entry point (e.g. `rpgc::Program rpgc::parse_program(Source)`) shared
by both `rpgc` (compiler) and `rpg-analyze`.

**Module registration:** each module registers itself in `registry.cpp` with its
id, title, default section group, and any module-dependencies. Adding a module =
one new file + one registry line; no changes to `report.cpp`.

---

## 10. Examples

```bash
# Full report on one file
rpg-analyze tests/e2e.rpg

# Just the indicator trace
rpg-analyze indicators tests/e2e.rpg
# equivalently:
rpg-analyze report -m indicators tests/e2e.rpg

# Files + fields + xref, no findings section
rpg-analyze report -m files -m fields -m xref --no-findings tests/e2e.rpg

# Everything except compat
rpg-analyze report -a --no-module compat tests/e2e.rpg

# CI: errors-only, JSON, machine-readable
rpg-analyze --json --severity error tests/*.rpg > findings.json

# Decode a single confusing line
rpg-analyze decode tests/e2e.rpg --line 7

# Portfolio dashboard across a library
rpg-analyze --json tests/*.rpg > per-file.json
rpg-analyze portfolio --from per-file.json --html > dashboard.html

# Migration plan
rpg-analyze migrate tests/*.rpg
```

---

## 11. Implementation phasing

Suggested milestones, each independently useful:

1. **Phase A — scaffolding.** Library extraction, `ProgramIR`, CLI skeleton,
   text + JSON renderers, findings model. Ship `summary` module end-to-end.
2. **Phase B — descriptive modules.** `files`, `recordmap`, `fields`, `xref`,
   `indicators`, `condlogic`. (No findings beyond INFO yet.)
3. **Phase C — structural modules.** `controlflow` (+CFG), `subroutines`,
   `cycle`, `complexity`, `deadcode`. ERROR/WARN findings begin.
4. **Phase D — risk modules.** `security`, `termination`, `buffer`, `compat`,
   `dataflow`, `smells`, `comments`, `deps`. Full findings list.
5. **Phase E — utilities.** `decode`, `search`, `diff`, `format`, `docgen`.
6. **Phase F — multi-file.** `callgraph`, `duplicate`, `portfolio`, `migrate`.

---

## 12. Open questions for the developer

- Should `compat`'s feature set be hand-maintained in code, or parsed from
  `docs/SPEC_MAP.md` and the root-level `*_PLAN.md` documents at runtime?
  (Recommend: a small JSON feature descriptor file, `analyze/features.json`,
  hand-curated, cited by SPEC_MAP.)
- `dataflow` traces can be expensive / imprecise in the presence of GOTO and
  indicators. v1 may trace only straight-line + IF paths and mark the rest
  "unresolved" rather than attempt full dataflow.
- Should `format` ever rewrite files? (Keep v1 check-only; rewriting risks
  corrupting column-sensitive fields.)
