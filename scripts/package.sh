#!/usr/bin/env bash
# KickEQ - package build outputs for one platform
# Copyright (C) 2026 Safian Szabolcs
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: scripts/package.sh <platform> <build-dir> <out-dir>
#
# <platform> is a manifest platform key (linux-armv7, linux-aarch64, linux-x86_64, macos-arm64,
# macos-universal, windows-x64). Creates <out-dir>/KickEQ-<platform>.tar.gz containing
#   native/KickEQ-native.clap   (always: the headless CLAP shell)
#   VST3/KickEQ.vst3, CLAP/KickEQ.clap   (only when the JUCE desktop build exists in <build-dir>)
#   kickeq.manifest.json, LICENSE, NOTICE, THIRD_PARTY.md

set -euo pipefail

platform="${1:?platform}"
build="${2:?build dir}"
out="${3:?out dir}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/KickEQ-$platform" "$out"
root="$stage/KickEQ-$platform"

native="$(find "$build" -maxdepth 4 -name 'KickEQ-native.clap' | head -n 1)"
[[ -n "$native" ]] || { echo "KickEQ-native.clap not found under $build" >&2; exit 1; }
mkdir -p "$root/native"
cp -R "$native" "$root/native/"

juce="$build/KickEQ_artefacts/Release"
if [[ -d "$juce/VST3" ]]; then
    mkdir -p "$root/VST3" "$root/CLAP"
    cp -R "$juce/VST3/KickEQ.vst3" "$root/VST3/"
    cp -R "$juce/CLAP/KickEQ.clap" "$root/CLAP/"
fi

manifest="$(find "$build" -maxdepth 3 -name 'kickeq.manifest.json' | head -n 1)"
[[ -n "$manifest" ]] && cp "$manifest" "$root/"
cp "$repo_root/LICENSE" "$repo_root/NOTICE" "$repo_root/THIRD_PARTY.md" "$root/"

tar -C "$stage" -czf "$out/KickEQ-$platform.tar.gz" "KickEQ-$platform"
ls -l "$out/KickEQ-$platform.tar.gz"
