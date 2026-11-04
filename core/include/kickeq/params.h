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

// The parameter description of KickEQ as a constexpr table (ARCHITECTURE.md section 3).
// Shells and UIs only iterate this table; plain values are stored in a ParamValues.
//
// Parameter index (position in the table / in ParamValues) is NOT the stable id: persistent identity is
// ParamInfo::id (CLAP param id) / ParamInfo::strId. Table index == id for the four global parameters.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kickeq/biquad.h"
#include "kickeq/config.h"

namespace kickeq
{
enum class Kind : uint8_t { Float, Bool, Enum };

/** Mapping between plain value and the normalised 0..1 range. Db is linear in the dB value (all dB parameters
    are stored as dB); it exists so a UI can pick dB-specific text and drawing. */
enum class Scale : uint8_t { Linear, Log, Db };

inline constexpr uint32_t flagAutomatable = 1u << 0;
inline constexpr uint32_t flagStepped = 1u << 1;
inline constexpr uint32_t flagHiddenInSimpleView = 1u << 2;
/** The parameter only exists when the core is built with KICKEQ_LINEAR_PHASE: otherwise its value is forced to the
    default and shells report it hidden and read-only. */
inline constexpr uint32_t flagNeedsLinearPhase = 1u << 3;

/** True when this build of the core can run the linear-phase mode (CMake option KICKEQ_LINEAR_PHASE). */
inline constexpr bool linearPhaseEnabled = KICKEQ_LINEAR_PHASE != 0;
/** The same as a plain function, for conditions: `if (linearPhaseOn ())` does not trigger the "conditional expression
    is constant" warning (MSVC C4127) that the constexpr variable would in a build with -Werror. */
inline bool linearPhaseOn() noexcept { return linearPhaseEnabled; }

struct ParamInfo
{
    uint32_t id;                   // stable numeric id (CLAP param id); never reused
    const char* strId;             // stable string id ("b2_freq"), used by JUCE
    const char* name;              // "Band 3 Frequency"
    const char* module;            // "Band 3" (CLAP module path, UI grouping)
    Kind kind;
    Scale scale;
    float min, max, def;
    const char* unit;              // "Hz", "dB", ""
    const char* const* enumLabels; // Bool: "Off", "On"; Enum: one label per value; Float: nullptr
    int numEnumLabels;
    uint32_t flags;
};

enum class Placement : int { stereo, left, right, mid, side };
constexpr int numPlacements = 5;

constexpr int numBands = 8;
constexpr int numGlobalParams = 4;
constexpr int paramsPerBand = 7;
constexpr int numParams = numGlobalParams + numBands * paramsPerBand; // 60

/** Table indices of the global parameters. */
enum GlobalParam : int { paramBypass = 0, paramOutGain = 1, paramAutoGain = 2, paramPhaseMode = 3 };

/** Values of the phase_mode parameter: minimum phase (biquad cascade, zero latency) or linear phase with an FIR of
    N = 2048 / 4096 / 8192 taps and a latency of exactly N / 2 samples (ARCHITECTURE.md section 4.7). */
enum class PhaseMode : int { minimum, linear2048, linear4096, linear8192 };
constexpr int numPhaseModes = 4;

/** FIR length N of a phase_mode value (0 for minimum phase; out-of-range values count as minimum). */
constexpr int phaseModeLength (int mode) noexcept
{
    return mode == 1 ? 2048 : (mode == 2 ? 4096 : (mode == 3 ? 8192 : 0));
}

/** Latency in samples of a phase_mode value: N / 2 (0 for minimum phase). */
constexpr int phaseModeLatency (int mode) noexcept { return phaseModeLength (mode) / 2; }

/** Offset of a band parameter inside a band (k of the id scheme 0x100 + 0x10 * band + k). */
enum class BandParam : int { on, type, freq, gain, q, place, slope };

constexpr int bandParamIndex (int band, BandParam k) noexcept
{
    return numGlobalParams + band * paramsPerBand + static_cast<int> (k);
}

constexpr uint32_t bandParamId (int band, BandParam k) noexcept
{
    return 0x100u + 0x10u * static_cast<uint32_t> (band) + static_cast<uint32_t> (k);
}

// ---------------------------------------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------------------------------------

inline constexpr const char* const boolLabels[2] = { "Off", "On" };
inline constexpr const char* const bandTypeLabels[numBandTypes] =
    { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" };
inline constexpr const char* const placementLabels[numPlacements] = { "Stereo", "Left", "Right", "Mid", "Side" };
inline constexpr const char* const slopeLabels[numSlopes] =
    { "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
inline constexpr const char* const phaseModeLabels[numPhaseModes] =
    { "Minimum", "Linear 2048", "Linear 4096", "Linear 8192" };

namespace detail
{
struct BandStrings
{
    const char* strId[paramsPerBand];
    const char* name[paramsPerBand];
    const char* module;
};

#define KICKEQ_BAND_STRINGS(B, N)                                                                          \
    { { "b" #B "_on", "b" #B "_type", "b" #B "_freq", "b" #B "_gain", "b" #B "_q", "b" #B "_place",        \
        "b" #B "_slope" },                                                                                  \
      { "Band " #N " On", "Band " #N " Type", "Band " #N " Frequency", "Band " #N " Gain", "Band " #N " Q", \
        "Band " #N " Placement", "Band " #N " Slope" },                                                     \
      "Band " #N }

inline constexpr BandStrings bandStrings[numBands] = {
    KICKEQ_BAND_STRINGS (0, 1), KICKEQ_BAND_STRINGS (1, 2), KICKEQ_BAND_STRINGS (2, 3), KICKEQ_BAND_STRINGS (3, 4),
    KICKEQ_BAND_STRINGS (4, 5), KICKEQ_BAND_STRINGS (5, 6), KICKEQ_BAND_STRINGS (6, 7), KICKEQ_BAND_STRINGS (7, 8)
};

#undef KICKEQ_BAND_STRINGS

struct BandDefault
{
    BandType type;
    float freq;
    float q;
};

// Default layout (all bands disabled): b0 Low Cut 30 Hz, b1 Low Shelf 100 Hz, b2..b5 Bell 250 Hz / 800 Hz /
// 2.5 kHz / 6 kHz, b6 High Shelf 10 kHz, b7 High Cut 18 kHz.
inline constexpr BandDefault bandDefaults[numBands] = {
    { BandType::lowCut,    30.0f,    0.707f },
    { BandType::lowShelf,  100.0f,   0.71f },
    { BandType::bell,      250.0f,   1.0f },
    { BandType::bell,      800.0f,   1.0f },
    { BandType::bell,      2500.0f,  1.0f },
    { BandType::bell,      6000.0f,  1.0f },
    { BandType::highShelf, 10000.0f, 0.71f },
    { BandType::highCut,   18000.0f, 0.707f }
};

constexpr ParamInfo makeInfo (uint32_t id, const char* strId, const char* name, const char* module, Kind kind,
                              Scale scale, float mn, float mx, float def, const char* unit,
                              const char* const* labels, int numLabels, uint32_t flags) noexcept
{
    return ParamInfo { id, strId, name, module, kind, scale, mn, mx, def, unit, labels, numLabels, flags };
}

constexpr std::array<ParamInfo, numParams> makeParamTable() noexcept
{
    std::array<ParamInfo, numParams> t {};

    t[paramBypass]   = makeInfo (0, "bypass", "Bypass", "Global", Kind::Bool, Scale::Linear, 0.0f, 1.0f, 0.0f, "",
                                 boolLabels, 2, flagAutomatable | flagStepped);
    t[paramOutGain]  = makeInfo (1, "out_gain", "Output Gain", "Global", Kind::Float, Scale::Db, -24.0f, 24.0f, 0.0f,
                                 "dB", nullptr, 0, flagAutomatable);
    t[paramAutoGain] = makeInfo (2, "auto_gain", "Auto Gain", "Global", Kind::Bool, Scale::Linear, 0.0f, 1.0f, 0.0f, "",
                                 boolLabels, 2, flagAutomatable | flagStepped);
    // Not automatable on purpose: it changes the reported latency (hosts must restart / re-query it).
    t[paramPhaseMode] = makeInfo (3, "phase_mode", "Phase Mode", "Global", Kind::Enum, Scale::Linear, 0.0f,
                                  static_cast<float> (numPhaseModes - 1), 0.0f, "", phaseModeLabels, numPhaseModes,
                                  flagStepped | flagNeedsLinearPhase);

    for (int b = 0; b < numBands; ++b)
    {
        const BandStrings& s = bandStrings[b];
        const BandDefault& d = bandDefaults[b];
        const auto at = [&] (BandParam k) { return static_cast<int> (k); };

        t[bandParamIndex (b, BandParam::on)] =
            makeInfo (bandParamId (b, BandParam::on), s.strId[at (BandParam::on)], s.name[at (BandParam::on)], s.module,
                      Kind::Bool, Scale::Linear, 0.0f, 1.0f, 0.0f, "", boolLabels, 2, flagAutomatable | flagStepped);
        t[bandParamIndex (b, BandParam::type)] =
            makeInfo (bandParamId (b, BandParam::type), s.strId[at (BandParam::type)], s.name[at (BandParam::type)],
                      s.module, Kind::Enum, Scale::Linear, 0.0f, static_cast<float> (numBandTypes - 1),
                      static_cast<float> (d.type), "", bandTypeLabels, numBandTypes, flagAutomatable | flagStepped);
        t[bandParamIndex (b, BandParam::freq)] =
            makeInfo (bandParamId (b, BandParam::freq), s.strId[at (BandParam::freq)], s.name[at (BandParam::freq)],
                      s.module, Kind::Float, Scale::Log, 10.0f, 30000.0f, d.freq, "Hz", nullptr, 0, flagAutomatable);
        t[bandParamIndex (b, BandParam::gain)] =
            makeInfo (bandParamId (b, BandParam::gain), s.strId[at (BandParam::gain)], s.name[at (BandParam::gain)],
                      s.module, Kind::Float, Scale::Db, -24.0f, 24.0f, 0.0f, "dB", nullptr, 0, flagAutomatable);
        t[bandParamIndex (b, BandParam::q)] =
            makeInfo (bandParamId (b, BandParam::q), s.strId[at (BandParam::q)], s.name[at (BandParam::q)], s.module,
                      Kind::Float, Scale::Log, 0.1f, 18.0f, d.q, "", nullptr, 0, flagAutomatable);
        t[bandParamIndex (b, BandParam::place)] =
            makeInfo (bandParamId (b, BandParam::place), s.strId[at (BandParam::place)], s.name[at (BandParam::place)],
                      s.module, Kind::Enum, Scale::Linear, 0.0f, static_cast<float> (numPlacements - 1), 0.0f, "",
                      placementLabels, numPlacements, flagAutomatable | flagStepped | flagHiddenInSimpleView);
        t[bandParamIndex (b, BandParam::slope)] =
            makeInfo (bandParamId (b, BandParam::slope), s.strId[at (BandParam::slope)], s.name[at (BandParam::slope)],
                      s.module, Kind::Enum, Scale::Linear, 0.0f, static_cast<float> (numSlopes - 1), 1.0f, "",
                      slopeLabels, numSlopes, flagAutomatable | flagStepped | flagHiddenInSimpleView);
    }

    return t;
}
} // namespace detail

inline constexpr std::array<ParamInfo, numParams> paramTable = detail::makeParamTable();

static_assert (paramTable[bandParamIndex (2, BandParam::freq)].id == 0x122, "id scheme 0x100 + 0x10 * band + k");
static_assert (paramTable[numParams - 1].id == 0x176, "last parameter is band 7 slope");

constexpr const ParamInfo& paramInfo (int index) noexcept { return paramTable[static_cast<size_t> (index)]; }

// ---------------------------------------------------------------------------------------------------------
// Helpers over the table
// ---------------------------------------------------------------------------------------------------------

/** Clamps a plain value into the range of the parameter; NaN becomes the default; stepped parameters are
    rounded to the nearest integer. */
inline float clampParam (const ParamInfo& info, float v) noexcept
{
    if ((info.flags & flagNeedsLinearPhase) != 0 && !linearPhaseOn())
        return info.def; // not available in this build: always the default (minimum phase)
    if (!(v == v))
        return info.def;
    v = v < info.min ? info.min : (v > info.max ? info.max : v);
    if ((info.flags & flagStepped) != 0)
    {
        const float r = static_cast<float> (static_cast<int> (v + 0.5f)); // v >= min >= 0
        v = r < info.min ? info.min : (r > info.max ? info.max : r);
    }
    return v;
}

/** Plain value -> 0..1 (Bool/Enum: index / (count - 1)). */
float toNormalized (const ParamInfo& info, float plain) noexcept;
/** 0..1 -> plain value (clamped; Bool/Enum: nearest index). */
float fromNormalized (const ParamInfo& info, float normalized) noexcept;

/** Formats a plain value as text, e.g. "1.25 kHz", "-3.0 dB", "0.71", "Bell", "24 dB/oct", "On".
    Writes at most cap bytes including the terminating NUL (when cap > 0) and returns the text length. */
size_t valueToText (const ParamInfo& info, float plain, char* out, size_t cap) noexcept;

/** Parses user text ("1.25k", "1250 Hz", "-3 dB", "bell", "24", "on"). The result is clamped into the range.
    Returns false when the text cannot be interpreted; plain is untouched then. */
bool textToValue (const ParamInfo& info, const char* text, float& plain) noexcept;

/** Table index of a CLAP/persistent id, or -1. */
int indexOfId (uint32_t id) noexcept;
/** Table index of a string id, or -1. */
int indexOfStrId (const char* strId) noexcept;

// ---------------------------------------------------------------------------------------------------------
// Typed view of one band
// ---------------------------------------------------------------------------------------------------------

struct BandParams
{
    bool on = false;
    BandType type = BandType::bell;
    float freqHz = 1000.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
    Placement place = Placement::stereo;
    int slopeIndex = 1;
};

/** Plain parameter values, indexed by table index. Accessors never return out-of-range values, whatever was
    written into `values`. */
struct ParamValues
{
    std::array<float, numParams> values {};

    ParamValues() noexcept { setDefaults(); }

    void setDefaults() noexcept
    {
        for (int i = 0; i < numParams; ++i)
            values[static_cast<size_t> (i)] = paramInfo (i).def;
    }

    /** Clamps every value into its range (NaN -> default, stepped -> integer). */
    void clamp() noexcept
    {
        for (int i = 0; i < numParams; ++i)
            values[static_cast<size_t> (i)] = clampParam (paramInfo (i), values[static_cast<size_t> (i)]);
    }

    float get (int index) const noexcept { return clampParam (paramInfo (index), values[static_cast<size_t> (index)]); }
    void set (int index, float plain) noexcept { values[static_cast<size_t> (index)] = clampParam (paramInfo (index), plain); }

    float get (int band, BandParam k) const noexcept { return get (bandParamIndex (band, k)); }
    void set (int band, BandParam k, float plain) noexcept { set (bandParamIndex (band, k), plain); }

    bool bypass() const noexcept { return get (paramBypass) >= 0.5f; }
    float outGainDb() const noexcept { return get (paramOutGain); }
    bool autoGain() const noexcept { return get (paramAutoGain) >= 0.5f; }
    /** 0 = minimum phase, 1..3 = linear phase 2048 / 4096 / 8192 (see PhaseMode). */
    int phaseMode() const noexcept { return static_cast<int> (get (paramPhaseMode)); }

    BandParams band (int b) const noexcept
    {
        BandParams p;
        p.on = get (b, BandParam::on) >= 0.5f;
        p.type = static_cast<BandType> (static_cast<int> (get (b, BandParam::type)));
        p.freqHz = get (b, BandParam::freq);
        p.gainDb = get (b, BandParam::gain);
        p.q = get (b, BandParam::q);
        p.place = static_cast<Placement> (static_cast<int> (get (b, BandParam::place)));
        p.slopeIndex = static_cast<int> (get (b, BandParam::slope));
        return p;
    }

    bool operator== (const ParamValues& o) const noexcept { return values == o.values; }
    bool operator!= (const ParamValues& o) const noexcept { return !(values == o.values); }
};
} // namespace kickeq
