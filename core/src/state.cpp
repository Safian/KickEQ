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

#include "kickeq/state.h"

#include <cmath>
#include <cstring>

namespace kickeq
{
namespace
{
constexpr uint8_t magic[4] = { 'K', 'E', 'Q', 'S' };
constexpr size_t headerSize = 4 + 4 + 4; // magic, version, numParams

void putU32 (uint8_t* p, uint32_t v) noexcept
{
    p[0] = static_cast<uint8_t> (v);
    p[1] = static_cast<uint8_t> (v >> 8);
    p[2] = static_cast<uint8_t> (v >> 16);
    p[3] = static_cast<uint8_t> (v >> 24);
}

uint32_t getU32 (const uint8_t* p) noexcept
{
    return static_cast<uint32_t> (p[0]) | (static_cast<uint32_t> (p[1]) << 8) | (static_cast<uint32_t> (p[2]) << 16)
           | (static_cast<uint32_t> (p[3]) << 24);
}

void putF32 (uint8_t* p, float v) noexcept
{
    uint32_t bits;
    std::memcpy (&bits, &v, sizeof bits);
    putU32 (p, bits);
}

float getF32 (const uint8_t* p) noexcept
{
    const uint32_t bits = getU32 (p);
    float v;
    std::memcpy (&v, &bits, sizeof v);
    return v;
}
} // namespace

size_t saveState (const ParamValues& values, uint8_t* out, size_t cap, const UiChunk* ui) noexcept
{
    const bool hasUi = ui != nullptr && ui->data != nullptr && ui->size > 0;
    const size_t needed = headerSize + static_cast<size_t> (numParams) * 8 + 4 + (hasUi ? 8 + static_cast<size_t> (ui->size) : 0);

    if (out == nullptr || cap < needed)
        return needed;

    uint8_t* p = out;
    std::memcpy (p, magic, 4);
    putU32 (p + 4, stateVersion);
    putU32 (p + 8, static_cast<uint32_t> (numParams));
    p += headerSize;

    for (int i = 0; i < numParams; ++i)
    {
        putU32 (p, paramInfo (i).id);
        putF32 (p + 4, values.get (i)); // clamped: a blob never contains NaN or out-of-range values
        p += 8;
    }

    putU32 (p, hasUi ? 1u : 0u);
    p += 4;
    if (hasUi)
    {
        putU32 (p, chunkTagUi);
        putU32 (p + 4, ui->size);
        std::memcpy (p + 8, ui->data, ui->size);
    }

    return needed;
}

bool loadState (const uint8_t* data, size_t size, ParamValues& values, UiChunk* ui) noexcept
{
    if (data == nullptr || size < headerSize || std::memcmp (data, magic, 4) != 0)
        return false;
    if (getU32 (data + 4) < 1)
        return false;

    const uint32_t numStored = getU32 (data + 8);
    size_t pos = headerSize;

    // the parameter table must fit entirely (8 bytes per entry), then the chunk count
    if (static_cast<uint64_t> (numStored) * 8 > size - pos)
        return false;

    ParamValues parsed;
    for (uint32_t i = 0; i < numStored; ++i)
    {
        const uint32_t id = getU32 (data + pos);
        const float value = getF32 (data + pos + 4);
        pos += 8;

        const int index = indexOfId (id);
        if (index >= 0 && std::isfinite (value))
            parsed.set (index, value); // clamps
    }

    if (size - pos < 4)
        return false;
    const uint32_t numChunks = getU32 (data + pos);
    pos += 4;

    UiChunk foundUi;
    for (uint32_t i = 0; i < numChunks; ++i)
    {
        if (size - pos < 8)
            return false;
        const uint32_t tag = getU32 (data + pos);
        const uint32_t chunkSize = getU32 (data + pos + 4);
        pos += 8;
        if (chunkSize > size - pos)
            return false;

        if (tag == chunkTagUi && foundUi.data == nullptr)
        {
            foundUi.data = data + pos;
            foundUi.size = chunkSize;
        }
        pos += chunkSize;
    }

    values = parsed;
    if (ui != nullptr)
        *ui = foundUi;
    return true;
}
} // namespace kickeq
