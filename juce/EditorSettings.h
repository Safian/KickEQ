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

// Editor state that is saved with the plugin state but is not an automatable parameter
// (ARCHITECTURE.md sections 5.2 and 8.1): the "UI01" chunk of the state blob.
//
//   byte 0  chunk layout version (1)
//   byte 1  analyser source          (kickeq::AnalyzerSource: 0 L+R, 1 Mid, 2 Side)
//   byte 2  analyser range index     (analyzerRangesDb)
//   byte 3  slope compensation index (slopeCompDbPerOct)
//   byte 4  curve range index        (curveRangesDb)
//
// Later versions append bytes; a reader ignores what it does not know and takes the default for every
// value that is missing or out of range.

#pragma once

#include <cstddef>
#include <cstdint>

namespace kickeq::ui
{
inline constexpr float analyzerRangesDb[3] = { 60.0f, 90.0f, 120.0f };
inline constexpr float slopeCompDbPerOct[3] = { 0.0f, 3.0f, 4.5f };
inline constexpr float curveRangesDb[4] = { 6.0f, 12.0f, 24.0f, 30.0f };

struct EditorSettings
{
    int analyzerSource = 0;  // AnalyzerSource
    int analyzerRange = 1;   // index into analyzerRangesDb  (default -90 dB)
    int slopeComp = 1;       // index into slopeCompDbPerOct (default 3 dB/oct)
    int curveRange = 1;      // index into curveRangesDb     (default +-12 dB)

    bool operator== (const EditorSettings& o) const noexcept
    {
        return analyzerSource == o.analyzerSource && analyzerRange == o.analyzerRange && slopeComp == o.slopeComp
               && curveRange == o.curveRange;
    }
    bool operator!= (const EditorSettings& o) const noexcept { return !(*this == o); }

    static constexpr size_t chunkSize = 5;

    void toChunk (uint8_t (&out)[chunkSize]) const noexcept
    {
        out[0] = 1;
        out[1] = static_cast<uint8_t> (analyzerSource);
        out[2] = static_cast<uint8_t> (analyzerRange);
        out[3] = static_cast<uint8_t> (slopeComp);
        out[4] = static_cast<uint8_t> (curveRange);
    }

    /** Parses a UI01 chunk; anything missing or invalid keeps the default. */
    static EditorSettings fromChunk (const uint8_t* data, uint32_t size) noexcept
    {
        EditorSettings s;
        if (data == nullptr || size < 1 || data[0] < 1)
            return s;
        const auto take = [&] (uint32_t pos, int count, int& dst) {
            if (pos < size && data[pos] < count)
                dst = data[pos];
        };
        take (1, 3, s.analyzerSource);
        take (2, 3, s.analyzerRange);
        take (3, 3, s.slopeComp);
        take (4, 4, s.curveRange);
        return s;
    }
};
} // namespace kickeq::ui
