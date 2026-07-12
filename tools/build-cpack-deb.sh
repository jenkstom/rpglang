#!/usr/bin/env bash
# =============================================================================
# tools/build-cpack-deb.sh
#
# Quick local .deb build via CMake + CPack -- no debhelper/devscripts needed,
# just the same toolchain used to build rpgc itself (cmake, dpkg-dev). Good
# for "give me an installable package right now"; for a package built the
# Debian way (source package, lintian-clean, debian/ changelog), use
# tools/build-deb.sh instead.
#
# Usage: tools/build-cpack-deb.sh [build-dir]   (default build-dir: build)
# Output: <build-dir>/rpg2-compiler_<version>_<arch>.deb
# =============================================================================
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repo_root/build}"

missing=()
for tool in cmake cpack dpkg-deb; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "error: missing required tool(s): ${missing[*]}" >&2
    echo "install with: sudo apt-get install -y cmake dpkg-dev" >&2
    exit 1
fi

echo "==> configuring ($build_dir)"
cmake -S "$repo_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release

echo "==> building"
cmake --build "$build_dir" -j "$(nproc)"

echo "==> packaging (cpack -G DEB)"
( cd "$build_dir" && cpack -G DEB )

echo "==> built:"
ls -1 "$build_dir"/*.deb
