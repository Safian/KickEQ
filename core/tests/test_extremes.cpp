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

// Parameter extremes, hostile input, filter design invariants.

#include "kickeq/eq_curve.h"
#include "test_util.h"

using namespace tu;

namespace
{
const double extremeSampleRates[] = { 8000.0,  11025.0, 16000.0,  22050.0,  32000.0,  44100.0, 48000.0,
                                      88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0 };

/** Largest magnitude (linear) of the stereo view over a dense log grid. */
double peakGain (const ParamValues& pv, double fs)
{
    constexpr int n = 4000;
    static float freqs[n];
    static float mags[n];
    for (int i = 0; i < n; ++i)
        freqs[i] = static_cast<float> (5.0 * std::pow (0.5 * fs / 5.0, static_cast<double> (i) / (n - 1)));
    double peak = 0.0;
    for (ChannelView v : { ChannelView::stereo, ChannelView::left, ChannelView::right, ChannelView::mid, ChannelView::side })
    {
        computeResponse (pv, fs, freqs, mags, nullptr, n, v);
        for (int i = 0; i < n; ++i)
            peak = std::max (peak, std::pow (10.0, static_cast<double> (mags[i]) / 20.0));
    }
    return std::max (peak, 1.0);
}

bool stable (const BiquadCoefficients& c)
{
    return std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2) && std::isfinite (c.a1)
           && std::isfinite (c.a2) && std::fabs (c.a2) < 1.0 && std::fabs (c.a1) < 1.0 + c.a2;
}

/** Runs noise + silence through the engine; returns false on non-finite / subnormal / unbounded output. */
bool runAndCheck (const ParamValues& pv, double fs, double bound, uint32_t seed)
{
    auto e = makeEngine (fs);
    auto l = noise (4096, seed);
    auto r = noise (4096, seed + 1);
    l.resize (6144, 0.0f);
    r.resize (6144, 0.0f);
    processBlocks (*e, pv, l.data(), r.data(), 6144, 997);

    bool ok = allFinite (l.data(), 6144) && allFinite (r.data(), 6144) && !anySubnormal (l.data(), 6144)
              && !anySubnormal (r.data(), 6144);
    for (int i = 0; i < 6144; ++i)
        ok = ok && std::fabs (l[static_cast<size_t> (i)]) <= bound && std::fabs (r[static_cast<size_t> (i)]) <= bound;
    return ok;
}
} // namespace

TEST (extremes_design_is_always_finite_and_stable)
{
    const double qs[] = { -1.0, 0.0, 0.05, 0.1, 0.707, 18.0, 20.0, 1000.0 };
    const double gains[] = { -1000.0, -48.0, -24.0, 0.0, 24.0, 48.0, 1000.0 };
    int count = 0;
    for (double fs : extremeSampleRates)
    {
        const double freqs[] = { -5.0, 0.0, 1.0, 10.0, 100.0, 0.49 * fs, 0.5 * fs, fs, 1.0e9 };
        for (int t = 0; t < numBandTypes; ++t)
            for (double f : freqs)
                for (double q : qs)
                    for (double g : gains)
                        for (int slope = 0; slope < numSlopes; slope += (t >= 3 && t <= 4) ? 1 : 5)
                        {
                            const BandDesign d = designBand (static_cast<BandType> (t), fs, f, g, q, slope);
                            bool ok = d.numSections >= 1 && d.numSections <= maxSections;
                            for (int s = 0; s < d.numSections; ++s)
                                ok = ok && stable (d.sections[static_cast<size_t> (s)]);
                            if (!ok)
                                std::printf ("    unstable design: type %d fs %.0f f %g q %g g %g slope %d\n", t, fs, f, q, g, slope);
                            CHECK (ok);
                            ++count;
                        }
    }
    CHECK (count > 1000);
}

TEST (extremes_each_band_type_at_range_limits_all_sample_rates)
{
    uint32_t seed = 100;
    for (double fs : extremeSampleRates)
        for (int t = 0; t < numBandTypes; ++t)
            for (float f : { 10.0f, 100.0f, static_cast<float> (0.49 * fs), 30000.0f })
                for (float q : { 0.1f, 18.0f })
                    for (float g : { -24.0f, 24.0f })
                    {
                        ParamValues pv;
                        setBand (pv, 3, true, static_cast<BandType> (t), f, g, q, 5);
                        const double bound = 8.0 * peakGain (pv, fs) + 1.0;
                        const bool ok = runAndCheck (pv, fs, bound, seed++);
                        if (!ok)
                            std::printf ("    FAIL type %d fs %.0f f %g q %g g %g (bound %g)\n", t, fs, f, q, g, bound);
                        CHECK (ok);
                    }
}

TEST (extremes_random_parameter_sets_off_grid)
{
    Rng rng (777);
    for (int iter = 0; iter < 200; ++iter)
    {
        const double fs = extremeSampleRates[rng.integer (0, 12)];
        ParamValues pv;
        for (int i = 0; i < numParams; ++i)
        {
            const ParamInfo& info = paramInfo (i);
            // a quarter of the values sit exactly on min or max, the rest anywhere in between (off-grid)
            const int pick = rng.integer (0, 3);
            float v = pick == 0 ? info.min : (pick == 1 ? info.max : static_cast<float> (rng.range (info.min, info.max)));
            if (info.kind == Kind::Bool && i != paramBypass)
                v = rng.integer (0, 3) != 0 ? 1.0f : 0.0f; // mostly on
            pv.set (i, v);
        }
        pv.set (paramBypass, 0.0f);
        const double bound = 20.0 * peakGain (pv, fs) * std::pow (10.0, std::fabs (pv.outGainDb()) / 20.0) + 1.0;
        CHECK (runAndCheck (pv, fs, bound, 900u + static_cast<uint32_t> (iter)));
    }
}

TEST (extremes_hostile_input_stays_finite_and_clean)
{
    ParamValues pv;
    setBand (pv, 0, true, BandType::lowCut, 20.0f, 0.0f, 18.0f, 5);
    setBand (pv, 1, true, BandType::bell, 60.0f, 24.0f, 18.0f);
    setBand (pv, 2, true, BandType::highShelf, 9000.0f, 24.0f, 0.1f, 1, Placement::mid);
    setBand (pv, 3, true, BandType::notch, 400.0f, 0.0f, 18.0f, 1, Placement::side);
    pv.set (paramAutoGain, 1.0f);

    const int n = 20000;
    enum { silence, impulse, dc, fullScaleNoise, nanInf, denormals, fullScaleSquare, numKinds };

    for (int kind = 0; kind < numKinds; ++kind)
    {
        std::vector<float> l (static_cast<size_t> (n), 0.0f), r;
        Rng rng (5 + static_cast<uint32_t> (kind));
        for (int i = 0; i < n; ++i)
        {
            float x = 0.0f;
            switch (kind)
            {
                case impulse: x = i == 100 ? 1.0f : 0.0f; break;
                case dc: x = 1.0f; break;
                case fullScaleNoise: x = rng.uniform(); break;
                case nanInf:
                {
                    const int m = i % 7;
                    x = m == 0 ? std::numeric_limits<float>::quiet_NaN()
                               : (m == 1 ? std::numeric_limits<float>::infinity()
                                         : (m == 2 ? -std::numeric_limits<float>::infinity() : 0.25f * rng.uniform()));
                    break;
                }
                case denormals: x = (i & 1) ? 1.0e-40f : -3.0e-41f; break;
                case fullScaleSquare: x = ((i / 50) & 1) ? 1.0f : -1.0f; break;
                default: break;
            }
            l[static_cast<size_t> (i)] = x;
        }
        r = l;
        std::reverse (r.begin(), r.end());

        auto e = makeEngine (48000.0);
        processBlocks (*e, pv, l.data(), r.data(), n, 128);
        CHECK (allFinite (l.data(), n));
        CHECK (allFinite (r.data(), n));
        CHECK (!anySubnormal (l.data(), n));
        CHECK (!anySubnormal (r.data(), n));
    }
}

TEST (extremes_nan_input_equals_zero_input)
{
    // NaN / Inf / denormal samples are sanitised to 0 before they reach any filter: the output is the same
    // as for a signal in which those samples are 0, and nothing is poisoned afterwards.
    ParamValues pv;
    setBand (pv, 0, true, BandType::bell, 500.0f, 12.0f, 2.0f);
    setBand (pv, 1, true, BandType::highCut, 3000.0f, 0.0f, 0.7071f, 4, Placement::mid);

    const int n = 16384;
    auto dirty = noise (n, 71, 0.5f);
    auto clean = dirty;
    Rng rng (3);
    for (int k = 0; k < 400; ++k)
    {
        const size_t i = static_cast<size_t> (rng.integer (0, n - 1));
        const int kind = rng.integer (0, 3);
        dirty[i] = kind == 0 ? std::numeric_limits<float>::quiet_NaN()
                             : (kind == 1 ? std::numeric_limits<float>::infinity()
                                          : (kind == 2 ? -std::numeric_limits<float>::infinity() : 1.0e-42f));
        clean[i] = 0.0f;
    }
    auto dirtyR = dirty, cleanR = clean;

    auto a = makeEngine (48000.0), b = makeEngine (48000.0);
    processBlocks (*a, pv, dirty.data(), dirtyR.data(), n, 500);
    processBlocks (*b, pv, clean.data(), cleanR.data(), n, 500);
    CHECK (bitIdentical (dirty, clean));
    CHECK (bitIdentical (dirtyR, cleanR));
}

TEST (extremes_long_decay_produces_no_denormals)
{
    ParamValues pv;
    setBand (pv, 0, true, BandType::bell, 12.0f, 24.0f, 18.0f);
    setBand (pv, 1, true, BandType::lowCut, 10.0f, 0.0f, 18.0f, 5);
    setBand (pv, 2, true, BandType::highCut, 30.0f, 0.0f, 0.7071f, 5);

    auto e = makeEngine (48000.0);
    std::vector<float> l (480000, 0.0f);
    std::vector<float> r;
    l[0] = 1.0f;
    r = l;
    processBlocks (*e, pv, l.data(), r.data(), 480000, 480);
    CHECK (allFinite (l.data(), 480000));
    CHECK (!anySubnormal (l.data(), 480000));
    CHECK (!anySubnormal (r.data(), 480000));
}

TEST (extremes_zero_and_oversized_blocks)
{
    ParamValues pv;
    setBand (pv, 0, true, BandType::bell, 500.0f, 6.0f, 1.0f);
    auto e = makeEngine (48000.0, 64); // maxBlock 64, then blocks of 100000

    float dummy = 0.25f;
    e->process (pv, &dummy, &dummy, 0);
    e->process (pv, nullptr, nullptr, 0);
    e->process (pv, &dummy, &dummy, -5);
    CHECK (dummy == 0.25f);

    std::vector<float> l = noise (100000, 9), r = noise (100000, 10);
    e->process (pv, l.data(), r.data(), 100000);
    CHECK (allFinite (l.data(), 100000));
    CHECK (e->getLatencySamples() == 0);
    CHECK (e->getTailSamples() > 0);
}
