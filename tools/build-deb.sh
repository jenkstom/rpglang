#!/usr/bin/env bash
# =============================================================================
# tools/build-deb.sh
#
# Builds the "proper" Debian source+binary package from debian/ at the repo
# root, using dpkg-buildpackage + debhelper (the same path `apt-get build-dep`
# / a PPA / a Debian mentors upload would use). For a quick local .deb without
# debhelper, see tools/build-cpack-deb.sh instead.
#
# Usage: tools/build-deb.sh
# Output: repo-root/../rpg2-compiler_<version>_<arch>.{deb,changes,buildinfo}
#         are copied into build/pkg-deb/ for convenience.
# =============================================================================
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

missing=()
for tool in dpkg-buildpackage dh fakeroot; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "error: missing required tool(s): ${missing[*]}" >&2
    echo "install with:" >&2
    echo "  sudo apt-get install -y debhelper devscripts fakeroot" >&2
    exit 1
fi

echo "==> dpkg-buildpackage -us -uc -b (unsigned binary build)"
dpkg-buildpackage -us -uc -b

out_dir="$repo_root/build/pkg-deb"
mkdir -p "$out_dir"
# dpkg-buildpackage drops its output one directory above the source tree.
shopt -s nullglob
artifacts=("$repo_root"/../rpg2-compiler_*.{deb,changes,buildinfo})
shopt -u nullglob
if [ "${#artifacts[@]}" -eq 0 ]; then
    echo "warning: build reported success but no rpg2-compiler_* artifacts were found in $repo_root/.." >&2
    exit 1
fi
mv -f "${artifacts[@]}" "$out_dir/"

echo "==> built:"
ls -1 "$out_dir"
