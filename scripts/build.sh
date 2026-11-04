#!/usr/bin/env bash
# KickEQ - build script for macOS and Linux (JUCE VST3 + CLAP, native CLAP, tools, tests)
# Copyright (C) 2026 Safian Szabolcs
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Usage: scripts/build.sh [--clean] [--debug] [-- <extra cmake configure args>]
#
#   --clean   remove the build directory first
#   --debug   Debug build instead of Release
#
# Environment: BUILD_DIR (default: <repo>/build), GENERATOR (default: Ninja if installed)

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$repo_root/build}"
build_type="Release"
clean=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) clean=1; shift ;;
        --debug) build_type="Debug"; shift ;;
        --) shift; break ;;
        -h|--help) sed -n '19,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

command -v cmake >/dev/null || { echo "cmake not found (need >= 3.22)" >&2; exit 1; }

generator="${GENERATOR:-}"
if [[ -z "$generator" ]] && command -v ninja >/dev/null; then
    generator="Ninja"
fi

if [[ "$clean" == 1 ]]; then
    rm -rf -- "${build_dir:?}"
fi

configure_args=(-S "$repo_root" -B "$build_dir" -DCMAKE_BUILD_TYPE="$build_type")
[[ -n "$generator" ]] && configure_args+=(-G "$generator")

cmake "${configure_args[@]}" "$@"
cmake --build "$build_dir" --config "$build_type" --parallel

echo
echo "Built:"
find "$build_dir/KickEQ_artefacts/$build_type" "$build_dir/native" -maxdepth 2 \( -name '*.vst3' -o -name '*.clap' \) -print 2>/dev/null
