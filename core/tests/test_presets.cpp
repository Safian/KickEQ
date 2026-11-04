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

// Factory presets.

#include <set>
#include <string>

#include "kickeq/eq_curve.h"
#include "kickeq/presets.h"
#include "test_util.h"

using namespace tu;
using namespace kickeq;

TEST (presets_list_and_names)
{
    CHECK (numPresets == 10);
    const char* const expected[10] = { "Flat", "Kick Punch", "Sub Clean", "Vocal Presence", "Air",
                                       "Telephone", "Mid Scoop", "Bass Boost", "De-Mud", "Mastering Tilt" };
    std::set<std::string> names;
    for (int i = 0; i < numPresets; ++i)
    {
        CHECK (std::string (factoryPresets[i].name) == expected[i]);
        CHECK (names.insert (factoryPresets[i].name).second);
    }
}

TEST (presets_parameters_are_valid_and_applied_on_top_of_the_defaults)
{
    for (int i = 0; i < numPresets; ++i)
    {
        const Preset& preset = factoryPresets[i];
        ParamValues pv;
        pv.set (paramOutGain, 9.0f); // a recall resets everything the preset does not set
        pv.set (paramBypass, 1.0f);
        CHECK (applyPreset (i, pv));

        std::set<int> touched;
        for (int k = 0; k < preset.numParams; ++k)
        {
            const PresetParam& pp = preset.params[k];
            CHECK (pp.index < numParams);
            CHECK (touched.insert (pp.index).second); // no parameter twice
            const ParamInfo& info = paramInfo (pp.index);
            CHECK (pp.value >= info.min && pp.value <= info.max);                        // in range, no clamping needed
            if ((info.flags & flagStepped) != 0)
                CHECK (pp.value == std::floor (pp.value));
            CHECK (pv.values[pp.index] == pp.value);
            CHECK (pp.index >= numGlobalParams);                                         // presets only shape bands
        }
        for (int k = 0; k < numParams; ++k)
            if (touched.count (k) == 0)
                CHECK (pv.values[static_cast<size_t> (k)] == paramInfo (k).def);

        // every enabled band of a non-flat preset comes with its own enable flag
        int enabled = 0;
        for (int b = 0; b < numBands; ++b)
            enabled += pv.band (b).on ? 1 : 0;
        CHECK ((i == 0) == (enabled == 0));
    }

    ParamValues untouched;
    untouched.set (paramOutGain, 3.0f);
    CHECK (!applyPreset (-1, untouched));
    CHECK (!applyPreset (numPresets, untouched));
    CHECK (untouched.outGainDb() == 3.0f);
}

TEST (presets_flat_is_transparent_and_all_presets_process_cleanly)
{
    ParamValues flat;
    CHECK (applyPreset (0, flat));
    CHECK (flat == ParamValues());

    const auto in = noise (24000, 4, 0.5f);
    for (int i = 0; i < numPresets; ++i)
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            ParamValues pv;
            CHECK (applyPreset (i, pv));
            auto l = in, r = in;
            auto e = makeEngine (fs);
            processBlocks (*e, pv, l.data(), r.data(), 24000, 480);
            CHECK (allFinite (l.data(), 24000));
            CHECK (!anySubnormal (l.data(), 24000));
            if (i == 0)
                CHECK (bitIdentical (l, in));
            else
                CHECK (!bitIdentical (l, in));

            // the curve is finite everywhere, and the preset really does something audible
            float freqs[64], mags[64];
            for (int k = 0; k < 64; ++k)
                freqs[k] = static_cast<float> (20.0 * std::pow (1000.0, k / 63.0));
            computeResponse (pv, fs, freqs, mags, nullptr, 64, ChannelView::stereo);
            float spread = 0.0f;
            for (int k = 0; k < 64; ++k)
            {
                CHECK (std::isfinite (mags[k]));
                spread = std::max (spread, std::fabs (mags[k]));
            }
            CHECK ((i == 0) == (spread < 1.0e-6f));
            CHECK (std::fabs (computeAutoGainDb (pv, fs)) <= 12.0);
        }
}
