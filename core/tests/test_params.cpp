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

// ParamTable invariants, scaling and text conversion.

#include <set>
#include <string>

#include "test_util.h"

using namespace tu;
using namespace kickeq;

namespace
{
std::string text (int index, float v)
{
    char buf[64];
    valueToText (paramInfo (index), v, buf, sizeof buf);
    return buf;
}

bool parse (int index, const char* s, float& out) { return textToValue (paramInfo (index), s, out); }
} // namespace

TEST (params_table_has_the_specified_shape)
{
    CHECK (numParams == 59);
    CHECK (paramTable.size() == 59);

    CHECK (std::string (paramInfo (0).strId) == "bypass" && paramInfo (0).id == 0 && paramInfo (0).kind == Kind::Bool);
    CHECK (std::string (paramInfo (1).strId) == "out_gain" && paramInfo (1).id == 1 && paramInfo (1).kind == Kind::Float);
    CHECK (paramInfo (1).min == -24.0f && paramInfo (1).max == 24.0f && paramInfo (1).def == 0.0f);
    CHECK (std::string (paramInfo (2).strId) == "auto_gain" && paramInfo (2).id == 2 && paramInfo (2).kind == Kind::Bool);

    const char* const defaultNames[numBands] = { "Low Cut", "Low Shelf", "Bell", "Bell", "Bell", "Bell", "High Shelf", "High Cut" };
    const float defaultFreq[numBands] = { 30, 100, 250, 800, 2500, 6000, 10000, 18000 };
    const int defaultType[numBands] = { 3, 1, 0, 0, 0, 0, 2, 4 };
    (void) defaultNames;

    for (int b = 0; b < numBands; ++b)
    {
        const std::string prefix = "b" + std::to_string (b) + "_";
        const char* const keys[7] = { "on", "type", "freq", "gain", "q", "place", "slope" };
        for (int k = 0; k < paramsPerBand; ++k)
        {
            const ParamInfo& p = paramInfo (bandParamIndex (b, static_cast<BandParam> (k)));
            CHECK (p.id == 0x100u + 0x10u * static_cast<uint32_t> (b) + static_cast<uint32_t> (k));
            CHECK (std::string (p.strId) == prefix + keys[k]);
            CHECK (std::string (p.module) == "Band " + std::to_string (b + 1));
            CHECK (std::string (p.name).rfind ("Band " + std::to_string (b + 1) + " ", 0) == 0);
        }

        const ParamInfo& on = paramInfo (bandParamIndex (b, BandParam::on));
        CHECK (on.kind == Kind::Bool && on.def == 0.0f); // fresh instance is bit-transparent
        const ParamInfo& type = paramInfo (bandParamIndex (b, BandParam::type));
        CHECK (type.kind == Kind::Enum && type.numEnumLabels == 7 && type.min == 0.0f && type.max == 6.0f);
        CHECK (static_cast<int> (type.def) == defaultType[b]);
        const ParamInfo& freq = paramInfo (bandParamIndex (b, BandParam::freq));
        CHECK (freq.kind == Kind::Float && freq.scale == Scale::Log && freq.min == 10.0f && freq.max == 30000.0f);
        CHECK (freq.def == defaultFreq[b]);
        CHECK (std::string (freq.unit) == "Hz");
        const ParamInfo& gain = paramInfo (bandParamIndex (b, BandParam::gain));
        CHECK (gain.scale == Scale::Db && gain.min == -24.0f && gain.max == 24.0f && gain.def == 0.0f);
        const ParamInfo& q = paramInfo (bandParamIndex (b, BandParam::q));
        CHECK (q.scale == Scale::Log && q.min == 0.1f && q.max == 18.0f);
        const int t = defaultType[b];
        CHECK_NEAR (q.def, (t == 1 || t == 2) ? 0.71 : (t == 3 || t == 4 ? 0.707 : 1.0), 1.0e-6);
        const ParamInfo& place = paramInfo (bandParamIndex (b, BandParam::place));
        CHECK (place.kind == Kind::Enum && place.numEnumLabels == 5 && place.def == 0.0f);
        const ParamInfo& slope = paramInfo (bandParamIndex (b, BandParam::slope));
        CHECK (slope.kind == Kind::Enum && slope.numEnumLabels == 6 && slope.def == 1.0f);
        CHECK (std::string (slope.enumLabels[0]) == "6 dB/oct" && std::string (slope.enumLabels[5]) == "48 dB/oct");
    }

    CHECK (std::string (paramInfo (bandParamIndex (2, BandParam::freq)).name) == "Band 3 Frequency");
    CHECK (std::string (paramInfo (bandParamIndex (0, BandParam::type)).enumLabels[0]) == "Bell");
    CHECK (std::string (paramInfo (bandParamIndex (0, BandParam::type)).enumLabels[6]) == "Band Pass");
    CHECK (std::string (paramInfo (bandParamIndex (0, BandParam::place)).enumLabels[4]) == "Side");
}

TEST (params_table_invariants)
{
    std::set<uint32_t> ids;
    std::set<std::string> strIds, names;
    for (int i = 0; i < numParams; ++i)
    {
        const ParamInfo& p = paramInfo (i);
        CHECK (ids.insert (p.id).second);
        CHECK (strIds.insert (p.strId).second);
        CHECK (names.insert (p.name).second);
        CHECK (p.min < p.max);
        CHECK (p.def >= p.min && p.def <= p.max);
        CHECK (p.unit != nullptr && p.module != nullptr && p.name != nullptr && p.strId != nullptr);
        CHECK ((p.flags & flagAutomatable) != 0);

        if (p.kind == Kind::Float)
        {
            CHECK ((p.flags & flagStepped) == 0);
            CHECK (p.enumLabels == nullptr && p.numEnumLabels == 0);
            if (p.scale == Scale::Log)
                CHECK (p.min > 0.0f);
        }
        else
        {
            CHECK ((p.flags & flagStepped) != 0);
            CHECK (p.min == 0.0f && p.max == static_cast<float> (p.numEnumLabels - 1));
            CHECK (p.def == std::floor (p.def));
            for (int l = 0; l < p.numEnumLabels; ++l)
                CHECK (p.enumLabels[l] != nullptr && p.enumLabels[l][0] != '\0');
        }

        // lookups
        CHECK (indexOfId (p.id) == i);
        CHECK (indexOfStrId (p.strId) == i);
    }

    CHECK (indexOfId (3) == -1); // phase_mode is reserved, absent for now
    CHECK (indexOfId (4) == -1);
    CHECK (indexOfId (0xFF) == -1);
    CHECK (indexOfId (0x0FF) == -1);
    CHECK (indexOfId (0x107) == -1);
    CHECK (indexOfId (0x17F) == -1);
    CHECK (indexOfId (0x180) == -1);
    CHECK (indexOfId (0x1000) == -1);
    CHECK (indexOfId (0xFFFFFFFFu) == -1);
    CHECK (indexOfStrId ("nope") == -1);
    CHECK (indexOfStrId ("") == -1);
    CHECK (indexOfStrId (nullptr) == -1);
    CHECK (indexOfStrId ("b7_slope") == numParams - 1);
}

TEST (params_values_clamp_and_defaults)
{
    ParamValues pv;
    for (int i = 0; i < numParams; ++i)
        CHECK (pv.values[static_cast<size_t> (i)] == paramInfo (i).def);

    pv.set (paramOutGain, 100.0f);
    CHECK (pv.outGainDb() == 24.0f);
    pv.set (paramOutGain, -100.0f);
    CHECK (pv.outGainDb() == -24.0f);
    pv.set (paramOutGain, std::numeric_limits<float>::quiet_NaN());
    CHECK (pv.outGainDb() == 0.0f);
    pv.set (paramOutGain, std::numeric_limits<float>::infinity());
    CHECK (pv.outGainDb() == 24.0f);
    pv.set (0, BandParam::type, 2.6f);
    CHECK (pv.get (0, BandParam::type) == 3.0f);
    pv.set (0, BandParam::type, 99.0f);
    CHECK (pv.get (0, BandParam::type) == 6.0f);

    // accessors are safe even when somebody wrote garbage straight into the array
    pv.values[static_cast<size_t> (paramOutGain)] = 1.0e30f;
    CHECK (pv.outGainDb() == 24.0f);
    pv.values[static_cast<size_t> (bandParamIndex (3, BandParam::freq))] = std::numeric_limits<float>::quiet_NaN();
    CHECK (pv.band (3).freqHz == 800.0f);
    pv.values[static_cast<size_t> (bandParamIndex (3, BandParam::type))] = -5.0f;
    CHECK (pv.band (3).type == BandType::bell);
    pv.clamp();
    CHECK (pv.values[static_cast<size_t> (paramOutGain)] == 24.0f);

    pv.setDefaults();
    CHECK (pv == ParamValues());
    const BandParams b0 = pv.band (0);
    CHECK (!b0.on && b0.type == BandType::lowCut && b0.freqHz == 30.0f && b0.slopeIndex == 1 && b0.place == Placement::stereo);
}

TEST (params_normalised_scaling)
{
    for (int i = 0; i < numParams; ++i)
    {
        const ParamInfo& p = paramInfo (i);
        CHECK_NEAR (toNormalized (p, p.min), 0.0, 1.0e-6);
        CHECK_NEAR (toNormalized (p, p.max), 1.0, 1.0e-6);
        CHECK (fromNormalized (p, 0.0f) == p.min);
        CHECK (fromNormalized (p, 1.0f) == p.max);
        CHECK (fromNormalized (p, -3.0f) == p.min);
        CHECK (fromNormalized (p, 7.0f) == p.max);
        CHECK (fromNormalized (p, std::numeric_limits<float>::quiet_NaN()) == p.def);

        float prev = -std::numeric_limits<float>::max();
        for (int s = 0; s <= 100; ++s)
        {
            const float n = static_cast<float> (s) / 100.0f;
            const float v = fromNormalized (p, n);
            CHECK (v >= p.min && v <= p.max);
            CHECK (v >= prev - 1.0e-4f * std::fabs (prev)); // monotonic
            prev = v;
            if (p.kind == Kind::Float)
                CHECK_NEAR (toNormalized (p, v), n, 2.0e-5);
        }

        // plain -> normalised -> plain
        if (p.kind == Kind::Float)
        {
            CHECK_NEAR (fromNormalized (p, toNormalized (p, p.def)), p.def, 1.0e-4 * std::fabs (p.def) + 1.0e-5);
        }
        else
        {
            for (int v = 0; v <= static_cast<int> (p.max); ++v)
                CHECK (fromNormalized (p, toNormalized (p, static_cast<float> (v))) == static_cast<float> (v));
        }
    }

    const ParamInfo& freq = paramInfo (bandParamIndex (0, BandParam::freq));
    CHECK_NEAR (toNormalized (freq, std::sqrt (10.0f * 30000.0f)), 0.5, 1.0e-5); // log: geometric centre
    CHECK_NEAR (toNormalized (freq, 100.0f), std::log (10.0) / std::log (3000.0), 1.0e-5);
    const ParamInfo& gain = paramInfo (bandParamIndex (0, BandParam::gain));
    CHECK_NEAR (toNormalized (gain, 0.0f), 0.5, 1.0e-6); // dB: linear in dB
    CHECK_NEAR (toNormalized (gain, 12.0f), 0.75, 1.0e-6);
    const ParamInfo& q = paramInfo (bandParamIndex (0, BandParam::q));
    CHECK_NEAR (fromNormalized (q, 0.5f), std::sqrt (0.1 * 18.0), 1.0e-4);
}

TEST (params_value_to_text_formats)
{
    const int freq = bandParamIndex (0, BandParam::freq);
    CHECK (text (freq, 1250.0f) == "1.25 kHz");
    CHECK (text (freq, 2500.0f) == "2.5 kHz");
    CHECK (text (freq, 18000.0f) == "18 kHz");
    CHECK (text (freq, 30000.0f) == "30 kHz");
    CHECK (text (freq, 1000.0f) == "1 kHz");
    CHECK (text (freq, 999.0f) == "999 Hz");
    CHECK (text (freq, 30.0f) == "30 Hz");
    CHECK (text (freq, 31.5f) == "31.5 Hz");
    CHECK (text (freq, 10.0f) == "10 Hz");

    const int gain = bandParamIndex (0, BandParam::gain);
    CHECK (text (gain, -3.0f) == "-3.0 dB");
    CHECK (text (gain, 0.0f) == "0.0 dB");
    CHECK (text (gain, 24.0f) == "24.0 dB");
    CHECK (text (gain, -0.04f) == "0.0 dB"); // no "-0.0"
    CHECK (text (gain, 5.55f) == "5.6 dB" || text (gain, 5.55f) == "5.5 dB");
    CHECK (text (paramOutGain, -12.5f) == "-12.5 dB");

    const int q = bandParamIndex (0, BandParam::q);
    CHECK (text (q, 0.71f) == "0.71");
    CHECK (text (q, 1.0f) == "1.00");
    CHECK (text (q, 18.0f) == "18.00");

    CHECK (text (bandParamIndex (0, BandParam::type), 0.0f) == "Bell");
    CHECK (text (bandParamIndex (0, BandParam::type), 3.0f) == "Low Cut");
    CHECK (text (bandParamIndex (0, BandParam::type), 6.0f) == "Band Pass");
    CHECK (text (bandParamIndex (0, BandParam::place), 3.0f) == "Mid");
    CHECK (text (bandParamIndex (0, BandParam::slope), 3.0f) == "24 dB/oct");
    CHECK (text (paramBypass, 1.0f) == "On");
    CHECK (text (paramBypass, 0.0f) == "Off");
    CHECK (text (paramAutoGain, 1.0f) == "On");

    // truncation: never writes past cap, always NUL terminated, returns the stored length
    char small[4] = { 'x', 'x', 'x', 'x' };
    CHECK (valueToText (paramInfo (freq), 1250.0f, small, 4) == 3);
    CHECK (std::string (small) == "1.2");
    char one[1] = { 'x' };
    CHECK (valueToText (paramInfo (freq), 1250.0f, one, 1) == 0);
    CHECK (one[0] == '\0');
    CHECK (valueToText (paramInfo (freq), 1250.0f, nullptr, 0) == 0);
}

TEST (params_text_to_value_parsing)
{
    float v = -999.0f;
    const int freq = bandParamIndex (0, BandParam::freq);
    CHECK (parse (freq, "1.25 kHz", v) && v == 1250.0f);
    CHECK (parse (freq, "1.25k", v) && v == 1250.0f);
    CHECK (parse (freq, "1,25 kHz", v) && v == 1250.0f);
    CHECK (parse (freq, "  440 Hz ", v) && v == 440.0f);
    CHECK (parse (freq, "440", v) && v == 440.0f);
    CHECK (parse (freq, "2K", v) && v == 2000.0f);
    CHECK (parse (freq, "99999", v) && v == 30000.0f); // clamped
    CHECK (parse (freq, "1", v) && v == 10.0f);

    const int gain = bandParamIndex (0, BandParam::gain);
    CHECK (parse (gain, "-3.0 dB", v) && v == -3.0f);
    CHECK (parse (gain, "-3", v) && v == -3.0f);
    CHECK (parse (gain, "+4.5dB", v) && v == 4.5f);
    CHECK (parse (gain, "-100 dB", v) && v == -24.0f);

    const int q = bandParamIndex (0, BandParam::q);
    CHECK (parse (q, "0.71", v) && std::fabs (v - 0.71f) < 1.0e-6f);
    CHECK (parse (q, "50", v) && v == 18.0f);

    const int type = bandParamIndex (0, BandParam::type);
    CHECK (parse (type, "Bell", v) && v == 0.0f);
    CHECK (parse (type, "bell", v) && v == 0.0f);
    CHECK (parse (type, "low cut", v) && v == 3.0f);
    CHECK (parse (type, "LowShelf", v) && v == 1.0f);
    CHECK (parse (type, "band pass", v) && v == 6.0f);
    CHECK (parse (type, "2", v) && v == 2.0f);

    const int slope = bandParamIndex (0, BandParam::slope);
    CHECK (parse (slope, "24 dB/oct", v) && v == 3.0f);
    CHECK (parse (slope, "24", v) && v == 3.0f);
    CHECK (parse (slope, "48dB", v) && v == 5.0f);
    CHECK (parse (slope, "6", v) && v == 0.0f);

    CHECK (parse (bandParamIndex (0, BandParam::place), "mid", v) && v == 3.0f);
    CHECK (parse (paramBypass, "on", v) && v == 1.0f);
    CHECK (parse (paramBypass, "Off", v) && v == 0.0f);
    CHECK (parse (paramBypass, "1", v) && v == 1.0f);

    v = 123.0f;
    CHECK (!parse (freq, "", v));
    CHECK (!parse (freq, "abc", v));
    CHECK (!parse (freq, "12 dB", v));
    CHECK (!parse (freq, "1.2.3", v) || v != 123.0f); // either rejected or parsed sensibly, never garbage
    CHECK (!parse (gain, "3 Hz", v));
    CHECK (!parse (type, "triangle", v));
    CHECK (!parse (type, "7", v));
    CHECK (!parse (slope, "25", v));
    CHECK (!parse (slope, "24 foo", v));
    CHECK (!parse (paramBypass, "maybe", v));
    CHECK (!textToValue (paramInfo (freq), nullptr, v));
}

TEST (params_text_round_trip_for_every_parameter)
{
    Rng rng (11);
    for (int i = 0; i < numParams; ++i)
    {
        const ParamInfo& p = paramInfo (i);
        std::vector<float> samples = { p.min, p.max, p.def, 0.5f * (p.min + p.max) };
        for (int k = 0; k < 20; ++k)
            samples.push_back (fromNormalized (p, static_cast<float> (rng.range (0.0, 1.0))));
        if (p.kind != Kind::Float)
            for (int v = 0; v <= static_cast<int> (p.max); ++v)
                samples.push_back (static_cast<float> (v));

        for (float value : samples)
        {
            const float clamped = clampParam (p, value);
            char buf[64];
            const size_t len = valueToText (p, clamped, buf, sizeof buf);
            CHECK (len > 0 && len == std::strlen (buf));

            float back = -12345.0f;
            const bool ok = textToValue (p, buf, back);
            if (!ok)
                std::printf ("    %s: \"%s\" does not parse\n", p.strId, buf);
            CHECK (ok);

            double tol = 0.0; // enums and bools are exact
            if (p.kind == Kind::Float)
            {
                if (std::strcmp (p.unit, "Hz") == 0)
                    tol = clamped >= 1000.0f ? 5.01 : 0.051;
                else if (std::strcmp (p.unit, "dB") == 0)
                    tol = 0.051;
                else
                    tol = 0.0051;
            }
            if (std::fabs (static_cast<double> (back) - clamped) > tol)
                std::printf ("    %s: %.6f -> \"%s\" -> %.6f\n", p.strId, clamped, buf, back);
            CHECK_NEAR (back, clamped, tol);
        }
    }
}
