/*
 * KickEQ - stereo parametric EQ (VST3 + CLAP)
 * Copyright (C) 2026 Safian Szabolcs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// The versioned, shell-independent state blob (ARCHITECTURE.md section 8.1). All integers little endian.
//
//   "KEQS" | u32 version (1) | u32 numParams | { u32 id, f32 plainValue } * numParams
//          | u32 numChunks | { u32 tag, u32 size, bytes } * numChunks
//
// Compatibility: unknown ids and unknown chunks are ignored, missing ids keep their default, out-of-range
// values are clamped, NaN / Inf values are rejected per value (the parameter keeps its default). A reader
// accepts every version >= 1: later versions only add ids and chunks and keep this layout.

#pragma once

#include <cstddef>
#include <cstdint>

#include "kickeq/params.h"

namespace kickeq
{
inline constexpr uint32_t stateVersion = 1;

constexpr uint32_t makeChunkTag (char a, char b, char c, char d) noexcept
{
    return static_cast<uint32_t> (static_cast<uint8_t> (a)) | (static_cast<uint32_t> (static_cast<uint8_t> (b)) << 8)
           | (static_cast<uint32_t> (static_cast<uint8_t> (c)) << 16)
           | (static_cast<uint32_t> (static_cast<uint8_t> (d)) << 24);
}

/** Editor state (analyser mode / range, window size): opaque to the core, owned by the shell. */
inline constexpr uint32_t chunkTagUi = makeChunkTag ('U', 'I', '0', '1');

/** Opaque bytes of the "UI01" chunk. On load, data points into the blob that was passed to loadState and is
    valid as long as that blob is. size == 0 / data == nullptr means "no chunk". */
struct UiChunk
{
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

/** Serialises `values` (and an optional UI chunk) into out. Returns the number of bytes the blob needs.
    Nothing is written when out is nullptr or cap is smaller than that, so a size query is
    saveState (values, nullptr, 0, ui); the call succeeded iff the result is <= cap. */
size_t saveState (const ParamValues& values, uint8_t* out, size_t cap, const UiChunk* ui = nullptr) noexcept;

/** Parses a blob. Never reads outside [data, data + size). Returns false (and leaves `values` and `ui`
    untouched) when the blob is not a valid KickEQ state: wrong magic, version 0, or truncated anywhere.
    On success `values` starts from the defaults, is overwritten by every recognised id, and is clamped. */
bool loadState (const uint8_t* data, size_t size, ParamValues& values, UiChunk* ui = nullptr) noexcept;
} // namespace kickeq
