# Building & Packaging

This document covers building `rpgc`/`rpg-analyze` from source, installing
them locally, and producing `.deb` packages for Debian/Ubuntu. It targets
Ubuntu 24.04+/Debian 12+ with LLVM 19; adjust package names for other
releases if your distro ships a different default LLVM version.

## 1. Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake git \
    llvm-19-dev clang-19 lld-19
```

- `cmake` ≥ 3.16, a C++17 compiler (g++ from `build-essential`)
- `llvm-19-dev` — headers + `llvm-config-19`, used at *build* time
- `clang-19`, and `llc` (from `llvm-19`, pulled in by `llvm-19-dev`) — used at
  *run* time: `rpgc` shells out to them to turn LLVM IR into a linked ELF.
  The exact absolute paths (`/usr/bin/clang-19`, `/usr/bin/llc-19`) are baked
  into `rpgc` at configure time, so whatever `rpgc` you build/install must
  have these on the same machine.

## 2. Build from source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Artifacts land in `build/bin/rpgc`, `build/bin/rpg-analyze`, and
`build/lib/librpgruntime.a`. Run them straight from there — no install step
needed for local development:

```bash
build/bin/rpgc tests/math.rpg -o /tmp/math.x && /tmp/math.x; echo $?   # -> 42
build/bin/rpg-analyze --help
```

Run the test suite with:

```bash
./tests/run_tests.sh
```

## 3. Install locally (no packaging)

```bash
cmake --install build --prefix /usr/local     # or omit --prefix for /usr/local default
```

This installs:

| File                                          | Purpose                          |
|------------------------------------------------|-----------------------------------|
| `<prefix>/bin/rpgc`                            | the compiler                     |
| `<prefix>/bin/rpg-analyze`                     | the static analyzer               |
| `<prefix>/lib/rpgc/librpgruntime.a`            | C runtime, linked into every compiled RPG program |
| `<prefix>/include/rpgc/rpg_runtime.h`          | runtime header                    |
| `<prefix>/share/doc/rpg2-compiler/{README.md,LICENSE}` | docs                       |

`rpgc` locates `librpgruntime.a` at runtime by looking next to its own
executable path first (`<prefix>/lib/rpgc/librpgruntime.a` relative to
`<prefix>/bin/rpgc`), falling back to the build-tree path baked in at
compile time. That's what makes the binary relocatable/packageable — see
`default_runtime_lib()` in `compiler/src/main.cpp`. Override it explicitly
with `rpgc --runtime <path>` if you ever need to.

## 4. Building a `.deb` package

There are two paths. Both produce an installable `rpg2-compiler_<version>_<arch>.deb`
containing `rpgc`, `rpg-analyze`, `librpgruntime.a`, the runtime header, and docs.

### 4a. Quick path: CPack

No extra tooling beyond what's needed to build the project already (plus
`dpkg-dev`, which ships `dpkg-deb`/`dpkg-shlibdeps` and is a near-universal
default on Debian/Ubuntu).

```bash
sudo apt-get install -y dpkg-dev   # if not already present
tools/build-cpack-deb.sh
# -> build/rpg2-compiler_0.1.0_amd64.deb
```

Equivalent by hand: `cmake --build build -j && (cd build && cpack -G DEB)`.
Packaging metadata (dependencies, maintainer, description) lives in the
`CPack`/`CPACK_DEBIAN_*` block at the bottom of the top-level `CMakeLists.txt`.

### 4b. The Debian way: `debian/` + `dpkg-buildpackage`

Produces a package the same way a PPA, `debuild`, or `sbuild` would — via
`debhelper`'s `dh` sequencer and the `debian/` metadata at the repo root
(`control`, `changelog`, `rules`, `copyright`, `source/format`).

```bash
sudo apt-get install -y debhelper devscripts fakeroot
tools/build-deb.sh
# -> build/pkg-deb/rpg2-compiler_0.1.0-1_amd64.deb (+ .changes, .buildinfo)
```

Equivalent by hand: `dpkg-buildpackage -us -uc -b` from the repo root (output
lands one directory above the repo). Use this path if you intend to run
`lintian` against the package, upload to a PPA/APT repo, or otherwise want a
package that follows Debian source-package conventions rather than a
CMake/CPack shortcut.

## 5. Installing / removing the built package

```bash
sudo apt install ./rpg2-compiler_0.1.0_amd64.deb   # pulls in llvm-19/clang-19 if missing
rpgc --version
rpg-analyze --help

sudo apt remove rpg2-compiler
```

(`sudo dpkg -i ./rpg2-compiler_*.deb && sudo apt-get install -f` also works if
you'd rather not depend on `apt`'s local-file install support.)

## 6. Package contents at a glance

```
/usr/bin/rpgc
/usr/bin/rpg-analyze
/usr/lib/rpgc/librpgruntime.a
/usr/include/rpgc/rpg_runtime.h
/usr/share/doc/rpg2-compiler/{README.md,LICENSE,changelog.Debian.gz,copyright}
```

Runtime dependencies (declared in both the CPack and `debian/control` paths):
`llvm-19`, `clang-19` — required because `rpgc --emit-exe` (the default mode)
shells out to `llc-19`/`clang-19` to turn LLVM IR into a linked executable.
