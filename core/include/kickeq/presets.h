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

// Factory presets (ARCHITECTURE.md section 8.2): constexpr parameter lists, sparse overrides on the defaults.

#pragma once

#include <array>
#include <cstdint>

#include "kickeq/params.h"

namespace kickeq
{
struct PresetParam
{
    uint16_t index; // table index
    float value;    // plain value
};

struct Preset
{
    const char* name;
    const PresetParam* params;
    int numParams;
};

namespace presetdetail
{
constexpr float f (BandType t) noexcept { return static_cast<float> (t); }

// A band with gain: on, type, freq, gain, q
#define KICKEQ_PB(b, t_, fr_, gn_, qq_)                                                           \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::on)), 1.0f },                           \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::type)), presetdetail::f (t_) },         \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::freq)), fr_ },                          \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::gain)), gn_ },                          \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::q)), qq_ }

// A cut band: on, type, freq, slope index (0 = 6, 1 = 12, 2 = 18, 3 = 24, 4 = 36, 5 = 48 dB/oct)
#define KICKEQ_PC(b, t_, fr_, sl_)                                                                 \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::on)), 1.0f },                           \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::type)), presetdetail::f (t_) },         \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::freq)), fr_ },                          \
    { static_cast<uint16_t> (bandParamIndex (b, BandParam::slope)), sl_ }

inline constexpr PresetParam kickPunch[] = {
    KICKEQ_PC (0, BandType::lowCut, 30.0f, 3.0f),
    KICKEQ_PB (1, BandType::bell, 60.0f, 4.5f, 1.1f),
    KICKEQ_PB (2, BandType::bell, 300.0f, -4.0f, 1.2f),
    KICKEQ_PB (4, BandType::bell, 3500.0f, 3.0f, 1.0f)
};

inline constexpr PresetParam subClean[] = {
    KICKEQ_PC (0, BandType::lowCut, 28.0f, 4.0f),
    KICKEQ_PB (2, BandType::bell, 250.0f, -3.0f, 1.5f),
    KICKEQ_PB (3, BandType::bell, 90.0f, 2.0f, 1.4f)
};

inline constexpr PresetParam vocalPresence[] = {
    KICKEQ_PC (0, BandType::lowCut, 90.0f, 1.0f),
    KICKEQ_PB (2, BandType::bell, 300.0f, -2.5f, 1.0f),
    KICKEQ_PB (4, BandType::bell, 3000.0f, 3.0f, 0.9f),
    KICKEQ_PB (6, BandType::highShelf, 10000.0f, 2.0f, 0.71f)
};

inline constexpr PresetParam air[] = {
    KICKEQ_PB (5, BandType::bell, 6000.0f, 1.5f, 0.8f),
    KICKEQ_PB (6, BandType::highShelf, 12000.0f, 4.0f, 0.6f)
};

inline constexpr PresetParam telephone[] = {
    KICKEQ_PC (0, BandType::lowCut, 400.0f, 3.0f),
    KICKEQ_PB (3, BandType::bell, 1500.0f, 4.0f, 1.0f),
    KICKEQ_PC (7, BandType::highCut, 3400.0f, 3.0f)
};

inline constexpr PresetParam midScoop[] = {
    KICKEQ_PB (2, BandType::bell, 400.0f, -3.0f, 1.0f),
    KICKEQ_PB (3, BandType::bell, 800.0f, -5.0f, 0.7f)
};

inline constexpr PresetParam bassBoost[] = {
    KICKEQ_PB (1, BandType::lowShelf, 100.0f, 5.0f, 0.71f),
    KICKEQ_PB (2, BandType::bell, 60.0f, 2.0f, 1.0f)
};

inline constexpr PresetParam deMud[] = {
    KICKEQ_PC (0, BandType::lowCut, 40.0f, 1.0f),
    KICKEQ_PB (2, BandType::bell, 250.0f, -4.0f, 1.4f),
    KICKEQ_PB (3, BandType::bell, 400.0f, -2.5f, 2.0f)
};

inline constexpr PresetParam masteringTilt[] = {
    KICKEQ_PC (0, BandType::lowCut, 25.0f, 1.0f),
    KICKEQ_PB (1, BandType::lowShelf, 200.0f, 1.5f, 0.5f),
    KICKEQ_PB (6, BandType::highShelf, 4000.0f, -1.5f, 0.5f)
};

#undef KICKEQ_PB
#undef KICKEQ_PC

template <size_t N>
constexpr int count (const PresetParam (&)[N]) noexcept { return static_cast<int> (N); }
} // namespace presetdetail

constexpr int numPresets = 10;

inline constexpr Preset factoryPresets[numPresets] = {
    { "Flat", nullptr, 0 },
    { "Kick Punch", presetdetail::kickPunch, presetdetail::count (presetdetail::kickPunch) },
    { "Sub Clean", presetdetail::subClean, presetdetail::count (presetdetail::subClean) },
    { "Vocal Presence", presetdetail::vocalPresence, presetdetail::count (presetdetail::vocalPresence) },
    { "Air", presetdetail::air, presetdetail::count (presetdetail::air) },
    { "Telephone", presetdetail::telephone, presetdetail::count (presetdetail::telephone) },
    { "Mid Scoop", presetdetail::midScoop, presetdetail::count (presetdetail::midScoop) },
    { "Bass Boost", presetdetail::bassBoost, presetdetail::count (presetdetail::bassBoost) },
    { "De-Mud", presetdetail::deMud, presetdetail::count (presetdetail::deMud) },
    { "Mastering Tilt", presetdetail::masteringTilt, presetdetail::count (presetdetail::masteringTilt) }
};

/** Resets `values` to the defaults and applies the preset's overrides (a recall therefore also resets bypass,
    output gain and auto gain, which no preset sets). Returns false (values untouched) for an index outside
    0 .. numPresets - 1. */
inline bool applyPreset (int index, ParamValues& values) noexcept
{
    if (index < 0 || index >= numPresets)
        return false;

    const Preset& p = factoryPresets[index];
    values.setDefaults();
    for (int i = 0; i < p.numParams; ++i)
        values.set (p.params[i].index, p.params[i].value);
    return true;
}
} // namespace kickeq
