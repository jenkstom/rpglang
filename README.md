# RPG II → LLVM Compiler (`rpgc`)

A self-contained, working **RPG II** compiler for Linux that uses **LLVM 19** as
its backend. Written in C++ against the LLVM C++ API; generated programs link
against a small C runtime (`librpgruntime.a`).

> Status: **Phase 10 complete** — alphanumeric `COMP`, `LOKUP` (array search),
> `MOVEA` (array↔field moves), and O-spec edit codes now work, rounding out the
> string/search/formatting feature set. The compiler covers the full RPG II
> core: the program cycle with file I/O, control levels (L1–L9) and subroutines,
> O-spec output with edit codes, the full arithmetic set, structured ops,
> alphanumeric fields, arrays (`ARR,INDEX`, `XFOOT`, `LOKUP`), indicators,
> `llc`+`clang` ELF linking, and optimization (`-O0`–`-O3`). See
> `docs/ARCHITECTURE.md` for the design.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requirements (already installed for this project):
- `llvm-19-dev`, `clang-19`, `lld-19` (LLVM 19.1.7)
- `cmake ≥ 3.16`, `gcc/g++ ≥ 13`, `make`

Artifacts land in `build/bin/rpgc` and `build/lib/librpgruntime.a`.

## Use

```bash
build/bin/rpgc --help
build/bin/rpgc --version                 # show linked LLVM version
build/bin/rpgc --emit-ir tests/math.rpg  # emit LLVM IR
build/bin/rpgc --emit-obj tests/math.rpg # emit a relocatable object (llc)
build/bin/rpgc tests/math.rpg            # compile to a Linux ELF (default)
./math; echo $?                          # -> 42
build/bin/rpgc -O2 tests/goto.rpg        # optimize (-O0..-O3)
```

Emission modes: `--emit-ir` (`.ll`), `--emit-asm` (`.s`), `--emit-obj` (`.o`),
`--emit-exe` (default). Add `-O<level>` to optimize, `--save-temps` to keep
intermediates, `-v` for verbose tool output.

## Test

```bash
./tests/run_tests.sh
```

## Layout

```
compiler/   C++ sources + CMakeLists for rpgc
runtime/    C sources for librpgruntime.a
tests/      RPG II sample programs + run_tests.sh
docs/       ARCHITECTURE.md (indicator mapping, RPG cycle, IR design)
```
