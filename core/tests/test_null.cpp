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

// Null tests: with all bands off or bypass on the output is bit-identical to the input; flat bands (gain 0)
// differ by less than -140 dBFS.

#include "test_util.h"

using namespace tu;

namespace
{
const double sampleRates[] = { 44100.0, 48000.0, 96000.0 };

double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max (m, std::fabs (static_cast<double> (a[i]) - static_cast<double> (b[i])));
    return m;
}
} // namespace

TEST (null_all_bands_off_is_bit_identical)
{
    for (double fs : sampleRates)
    {
        const auto inL = noise (20000, 11);
        const auto inR = noise (20000, 12);

        ParamValues pv; // defaults: every band disabled
        // band settings of disabled bands must not matter
        setBand (pv, 2, false, BandType::bell, 1000.0f, 24.0f, 18.0f);
        setBand (pv, 5, false, BandType::highCut, 100.0f, 0.0f, 1.0f, 5, Placement::mid);

        auto e = makeEngine (fs);
        auto l = inL, r = inR;
        processBlocks (*e, pv, l.data(), r.data(), 20000, 333);
        CHECK (bitIdentical (l, inL));
        CHECK (bitIdentical (r, inR));
    }
}

TEST (null_bypass_is_bit_identical_even_with_active_bands)
{
    for (double fs : sampleRates)
    {
        const auto inL = noise (20000, 21);
        const auto inR = noise (20000, 22);

        ParamValues pv;
        setBand (pv, 0, true, BandType::lowCut, 200.0f, 0.0f, 0.7071f, 5);
        setBand (pv, 1, true, BandType::bell, 1000.0f, 12.0f, 2.0f, 1, Placement::mid);
        setBand (pv, 2, true, BandType::highShelf, 5000.0f, -9.0f, 0.71f, 1, Placement::left);
        pv.set (paramBypass, 1.0f);
        pv.set (paramOutGain, 6.0f);
        pv.set (paramAutoGain, 1.0f);

        auto e = makeEngine (fs);
        auto l = inL, r = inR;
        processBlocks (*e, pv, l.data(), r.data(), 20000, 480);
        CHECK (bitIdentical (l, inL));
        CHECK (bitIdentical (r, inR));
    }
}

TEST (null_bypass_after_fade_is_bit_identical_again)
{
    const double fs = 48000.0;
    const int n = 48000;
    const auto inL = noise (n, 31);
    const auto inR = noise (n, 32);

    ParamValues pv;
    setBand (pv, 1, true, BandType::bell, 1000.0f, 12.0f, 2.0f);
    setBand (pv, 3, true, BandType::highCut, 4000.0f, 0.0f, 0.7071f, 3, Placement::side);

    auto e = makeEngine (fs);
    auto l = inL, r = inR;
    processBlocks (*e, pv, l.data(), r.data(), 12000, 256); // processing for 250 ms: output differs
    CHECK (maxAbsDiff (std::vector<float> (l.begin(), l.begin() + 12000), std::vector<float> (inL.begin(), inL.begin() + 12000)) > 0.01);

    pv.set (paramBypass, 1.0f);
    processBlocks (*e, pv, l.data() + 12000, r.data() + 12000, n - 12000, 256);
    // 20 ms fade = 960 samples; afterwards the signal is the input, bit for bit
    const size_t from = 12000 + 1100;
    CHECK (std::memcmp (l.data() + from, inL.data() + from, (n - from) * sizeof (float)) == 0);
    CHECK (std::memcmp (r.data() + from, inR.data() + from, (n - from) * sizeof (float)) == 0);

    // and back: the processed signal returns after the fade
    pv.set (paramBypass, 0.0f);
    auto l2 = noise (24000, 33), r2 = noise (24000, 34);
    auto ref = l2;
    processBlocks (*e, pv, l2.data(), r2.data(), 24000, 256);
    CHECK (maxAbsDiff (std::vector<float> (l2.begin() + 4000, l2.end()), std::vector<float> (ref.begin() + 4000, ref.end())) > 0.01);
}

TEST (null_flat_bands_are_below_minus_140_dbfs)
{
    for (double fs : sampleRates)
    {
        const auto inL = noise (30000, 41);
        const auto inR = noise (30000, 42);

        ParamValues pv;
        setBand (pv, 0, true, BandType::bell, 200.0f, 0.0f, 1.0f);
        setBand (pv, 1, true, BandType::lowShelf, 100.0f, 0.0f, 0.71f);
        setBand (pv, 2, true, BandType::highShelf, 8000.0f, 0.0f, 0.71f, 1, Placement::left);
        setBand (pv, 3, true, BandType::bell, 1000.0f, 0.0f, 4.0f, 1, Placement::right);
        setBand (pv, 4, true, BandType::bell, 3000.0f, 0.0f, 2.0f, 1, Placement::mid);
        setBand (pv, 5, true, BandType::lowShelf, 400.0f, 0.0f, 1.0f, 1, Placement::side);
        setBand (pv, 6, true, BandType::bell, 50.0f, 0.0f, 18.0f);
        setBand (pv, 7, true, BandType::highShelf, 15000.0f, 0.0f, 0.1f, 1, Placement::mid);

        auto e = makeEngine (fs);
        auto l = inL, r = inR;
        processBlocks (*e, pv, l.data(), r.data(), 30000, 480);
        const double worst = std::max (maxAbsDiff (l, inL), maxAbsDiff (r, inR));
        CHECK (toDb (worst) < -140.0);

        // enabling flat bands with a smooth cross-fade also stays flat
        ParamValues off;
        auto e2 = makeEngine (fs);
        auto l2 = inL, r2 = inR;
        processBlocks (*e2, off, l2.data(), r2.data(), 10000, 480);
        processBlocks (*e2, pv, l2.data() + 10000, r2.data() + 10000, 20000, 480);
        CHECK (toDb (std::max (maxAbsDiff (l2, inL), maxAbsDiff (r2, inR))) < -140.0);
    }
}

TEST (null_flat_bands_with_unity_output_gain_and_auto_gain_stay_flat)
{
    const auto inL = noise (30000, 51);
    auto l = inL, r = inL;
    ParamValues pv;
    setBand (pv, 0, true, BandType::bell, 200.0f, 0.0f, 1.0f);
    setBand (pv, 4, true, BandType::bell, 3000.0f, 0.0f, 2.0f, 1, Placement::mid);
    pv.set (paramAutoGain, 1.0f);
    auto e = makeEngine (48000.0);
    processBlocks (*e, pv, l.data(), r.data(), 30000, 100);
    CHECK (toDb (maxAbsDiff (l, inL)) < -140.0);
}

TEST (null_mono_entry_point_is_bit_identical_when_off)
{
    const auto in = noise (5000, 61);
    auto x = in;
    ParamValues pv;
    auto e = makeEngine (48000.0);
    e->processMono (pv, x.data(), 5000);
    CHECK (bitIdentical (x, in));
}
