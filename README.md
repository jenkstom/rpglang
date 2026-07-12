# rpglang

An **RPG II** compiler and static-analysis toolkit for Linux. Compiles the
column-oriented fixed-format language of IBM System/34, System/36, and AS/400
midrange systems into native ELF executables via **LLVM 19**, with no
proprietary runtime or emulator required.

The compiler (`rpgc`) is written in C++ against the LLVM C++ API. Every compiled
program links against a small C runtime (`librpgruntime.a`) that handles file
I/O, numeric decoding, and output formatting. A companion tool (`rpg-analyze`)
provides 20 static-analysis modules for understanding, auditing, and migrating
legacy RPG II source.

## What is RPG II?

RPG (Report Program Generator) is a business-oriented programming language that
originated on IBM mainframes in the 1960s and matured into RPG II on the
System/34 and System/36. Its defining characteristics:

- **Fixed-column source format** — meaning lives in exact column positions,
  not whitespace-delimited tokens. Column 6 identifies the line type (spec):
  `F` (file), `I` (input), `C` (calculation), `O` (output).
- **The implicit program cycle** — an outer loop that reads records, extracts
  fields, runs calculations, and produces output automatically, without an
  explicit main function.
- **Indicators** — a global array of 99 boolean latches (plus named specials
  like LR, L1–L9) used for conditioning operations and tracking program state.

This project implements a practical subset of RPG II that covers the full core
language: the program cycle with sequential and keyed file I/O, control levels
(L1–L9) and subroutines, the complete arithmetic and structured-operation
sets, alphanumeric fields, arrays and tables, indicators, O-spec output with
edit codes and edit words, and LLVM-based code generation with optimization.

## Quick start

```bash
# Prerequisites: LLVM 19 toolchain + build tools
sudo apt-get install -y build-essential cmake llvm-19-dev clang-19 lld-19

# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Compile and run an RPG program
build/bin/rpgc tests/math.rpg
./math; echo $?                          # -> 42

# Run the test suite
./tests/run_tests.sh
```

## Using the compiler

```bash
rpgc [options] <input.rpg>
```

| Task | Command |
|------|---------|
| Compile to executable (default) | `rpgc program.rpg` |
| Emit LLVM IR | `rpgc --emit-ir program.rpg` |
| Emit assembly | `rpgc --emit-asm program.rpg` |
| Emit object file | `rpgc --emit-obj program.rpg` |
| Optimize | `rpgc -O2 program.rpg` |
| Verbose tool output | `rpgc -v program.rpg` |
| Keep intermediates | `rpgc --save-temps program.rpg` |
| Custom output name | `rpgc -o myprog program.rpg` |
| Override runtime lib | `rpgc --runtime /path/to/librpgruntime.a program.rpg` |

See `rpgc --help` or the man page (`man rpgc`) for the full option list.

### A simple program

```text
     C* Add two numbers and return the result
     C                     Z-ADD40        RPGRET
     C                     ADD  2         RPGRET
```

This C-spec-only program (no file I/O) sets RPGRET to 42. The test hook returns
its low byte as the exit code, so `echo $?` prints 42.

### A program with file I/O and output

```text
     FRPDATA  IP  F    80              DISK
     FREPORT  O   F    132             PRINTER
     IRPDATA  AA  01
     I                                        1   30AMT
     C                     ADD  AMT       TOTAL
     OREPORT  D  1
     O                                   20 'Amount ='
     O                         AMT       30
     OREPORT  T  2    LR
     O                                   20 'Grand total ='
     O                         TOTAL     30
```

This reads numeric amounts from a disk file, prints each one, and writes a
grand total at end-of-file. The F-spec declares files, the I-spec maps record
positions to fields, the C-spec accumulates, and the O-spec formats output
lines.

## Using the analyzer

```bash
rpg-analyze [options] [command] <file...>
```

Run the full synthesized report (all 20 modules):

```bash
rpg-analyze program.rpg
```

Run a single module:

```bash
rpg-analyze indicators program.rpg      # trace indicator lifecycle
rpg-analyze deadcode program.rpg        # find unused fields and dead paths
```

Utility commands:

```bash
rpg-analyze decode program.rpg          # decode spec lines into columns
rpg-analyze search --query "op:COMP" *.rpg  # find all COMP operations
rpg-analyze diff old.rpg new.rpg        # structural diff
rpg-analyze docgen program.rpg          # generate Markdown docs
rpg-analyze callgraph --dot *.rpg       # inter-program dependency graph
rpg-analyze portfolio --html *.rpg      # codebase metrics dashboard
```

JSON output for CI integration:

```bash
rpg-analyze --json --severity warn *.rpg > report.json
```

See `rpg-analyze --help` or the man page (`man rpg-analyze`) for details.

## Installation

### Build from source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix /usr/local
```

### Debian/Ubuntu package

```bash
tools/build-cpack-deb.sh                # quick path (CPack)
# or
tools/build-deb.sh                      # Debian way (dpkg-buildpackage)
sudo apt install ./rpg2-compiler_0.1.0_amd64.deb
```

See [docs/BUILDING.md](docs/BUILDING.md) for full instructions.

## Documentation

| Document | Audience | Content |
|----------|----------|---------|
| **docs/tutorial/** | All | Interactive HTML tutorial covering RPG II from basics through advanced features |
| **docs/BUILDING.md** | All | Build prerequisites, installation, and `.deb` packaging |
| **docs/SPEC_MAP.md** | All | Authoritative column-position reference for every spec type |
| **docs/ARCHITECTURE.md** | Experienced | Internal design: column parser, indicator→IR mapping, program cycle, codegen pipeline |
| **TOOLS_IDEAS.md** | Experienced | Full design rationale for every `rpg-analyze` module and command |

Man pages are installed to `<prefix>/share/man/man1/` and are also available in
`docs/man/`.

## Feature coverage

**Language constructs:**

- F-specs: DISK (sequential, indexed) and PRINTER files; fixed-length records
- I-specs: field extraction, record-identification, control levels, matching
  fields, field indicators, look-ahead
- C-specs: full arithmetic (ADD, SUB, MULT, DIV, MVR, Z-ADD, Z-SUB, SQRT),
  structured ops (IF/ELSE/END, DOW, DOU, DO, CAS), GOTO/TAG, MOVE/MOVEL/MOVEA,
  COMP, SETON/SETOFF, EXSR/BEGSR/ENDSR subroutines, EXCPT, CHAIN/SETLL/READE/READ,
  XFOOT, LOKUP, TESTZ/TESTB, arrays and tables
- Program linkage: CALL/PARM/PLIST/RETRN/FREE (calling another compiled RPG
  program by address, across a multi-file build) and EXIT/RLABL (calling an
  external, non-RPG subroutine)
- O-specs: detail/total/heading/exception output, edit codes and edit words,
  skip/space, overflow, field conditioning, PAGE fields
- E-specs: compile-time and prerun-time arrays/tables, alternating arrays
- H-specs: heading, currency, date editing
- Indicators: 01–99, L0–L9, LR, MR, 1P, OA–OG, OV (H1–H9, U1–U8, KA–KY
  accepted lexically)
- Control-level break processing with cascade (L3 sets L1, L2, L3)
- Multifile cycle with primary/secondary files and matching fields
- `/COPY` include directive

**Compiler capabilities:**

- LLVM IR generation with `-O0`–`-O3` optimization
- `llc` + `clang` ELF linking (or `--emit-ir`/`--emit-asm`/`--emit-obj` for
  intermediate output)
- Scaled-integer arithmetic with decimal alignment and half-adjust rounding
- Zoned, packed, and binary numeric field decoding
- Sign-overpunch encoding for character/numeric conversion

**Not yet implemented:** WORKSTN/display files, DEBUG operation,
externally-described files. See the plan documents (`WRKSTN_PLAN.md`,
`MISC_OPCODES_PLAN.md`) for design status.

## Project layout

```
compiler/   C++ sources for rpgc (the compiler)
runtime/    C sources for librpgruntime.a (file I/O, numeric decoding)
analyze/    C++ sources for rpg-analyze (static analysis toolkit)
tests/      RPG II sample programs + run_tests.sh
docs/       ARCHITECTURE.md, BUILDING.md, SPEC_MAP.md, tutorial/, man/
debian/     dpkg-buildpackage packaging metadata
tools/      build helpers (build-deb.sh, build-cpack-deb.sh)
```

## License

See [LICENSE](LICENSE).
