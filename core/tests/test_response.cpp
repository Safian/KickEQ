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

// Frequency response of every band type through the real engine: sine measurements after the transient,
// compared with the analytic eq_curve.h and with the independent reference model of test_util.h.

#include "kickeq/eq_curve.h"
#include "test_util.h"

using namespace tu;

namespace
{
const double sampleRates[] = { 44100.0, 48000.0, 96000.0 };

struct Config
{
    BandType type;
    float freq, gain, q;
    int slope;
};

/** Frequencies around fc, always including DC and Nyquist. */
std::vector<double> probeFrequencies (double fs, double fc)
{
    std::vector<double> f = { 0.0, 0.5 * fs };
    const double ratios[] = { 0.125, 0.25, 0.5, 0.9, 1.0, 1.1, 2.0, 4.0, 8.0 };
    for (double r : ratios)
        if (fc * r >= 10.0 && fc * r < 0.45 * fs)
            f.push_back (fc * r);
    return f;
}

double curveDb (const ParamValues& pv, double fs, double freq, ChannelView view = ChannelView::stereo)
{
    const float f = static_cast<float> (freq);
    float mag = 0.0f;
    computeResponse (pv, fs, &f, &mag, nullptr, 1, view);
    return static_cast<double> (mag);
}

double settleFor (const Config& c)
{
    return c.freq < 300.0f ? 1.5 : 0.4;
}
} // namespace

TEST (response_band_types_vs_curve_and_reference)
{
    const Config configs[] = {
        { BandType::bell, 1000.0f, 12.0f, 1.0f, 1 },    { BandType::bell, 250.0f, -9.0f, 0.5f, 1 },
        { BandType::bell, 5000.0f, 6.0f, 4.0f, 1 },     { BandType::bell, 100.0f, 15.0f, 2.0f, 1 },
        { BandType::lowShelf, 200.0f, 12.0f, 0.71f, 1 }, { BandType::lowShelf, 1000.0f, -9.0f, 1.0f, 1 },
        { BandType::highShelf, 4000.0f, 12.0f, 0.71f, 1 }, { BandType::highShelf, 8000.0f, -12.0f, 0.5f, 1 },
        { BandType::notch, 1000.0f, 0.0f, 2.0f, 1 },    { BandType::notch, 3000.0f, 0.0f, 8.0f, 1 },
        { BandType::bandPass, 1000.0f, 0.0f, 1.0f, 1 }, { BandType::bandPass, 500.0f, 0.0f, 4.0f, 1 },
        { BandType::lowCut, 800.0f, 0.0f, 0.7071f, 1 }, { BandType::highCut, 3000.0f, 0.0f, 1.5f, 1 },
    };

    for (double fs : sampleRates)
        for (const Config& c : configs)
        {
            ParamValues pv;
            setBand (pv, 3, true, c.type, c.freq, c.gain, c.q, c.slope);

            for (double f : probeFrequencies (fs, c.freq))
            {
                const double expected = ref::bandGainDb (c.type, fs, c.freq, c.gain, c.q, c.slope, f);
                const double viaCurve = curveDb (pv, fs, f);
                const double measured = measureGainDb (pv, fs, f, settleFor (c));

                const bool okCurve = matchesDb (viaCurve, expected, 0.002) || (expected < -90.0 && viaCurve < -90.0);
                const bool okEngine = matchesDb (measured, expected, 0.01);
                if (!okCurve || !okEngine)
                    std::printf ("    %s fs=%.0f fc=%.0f gain=%.1f q=%.2f f=%.2f: ref %.4f curve %.4f engine %.4f\n",
                                 typeName (c.type), fs, c.freq, c.gain, c.q, f, expected, viaCurve, measured);
                CHECK (okCurve);
                CHECK (okEngine);
            }
        }
}

TEST (response_gain_at_corner_is_exact)
{
    for (double fs : sampleRates)
    {
        const float fc = 1000.0f;
        struct Case { BandType type; float gain, q; int slope; double expected; } cases[] = {
            { BandType::bell, 12.0f, 1.0f, 1, 12.0 },       { BandType::bell, -18.0f, 3.0f, 1, -18.0 },
            { BandType::bell, 24.0f, 0.1f, 1, 24.0 },       { BandType::lowShelf, 12.0f, 0.71f, 1, 6.0 },
            { BandType::lowShelf, -20.0f, 1.0f, 1, -10.0 }, { BandType::highShelf, 18.0f, 0.71f, 1, 9.0 },
            { BandType::highShelf, -6.0f, 2.0f, 1, -3.0 },  { BandType::bandPass, 0.0f, 3.0f, 1, 0.0 },
            { BandType::lowCut, 0.0f, 0.70711f, 1, -3.0103 }, { BandType::highCut, 0.0f, 0.70711f, 1, -3.0103 },
            { BandType::lowCut, 0.0f, 1.0f, 0, -3.0103 },   { BandType::highCut, 0.0f, 1.0f, 0, -3.0103 },
            { BandType::lowCut, 0.0f, 1.0f, 2, -3.0103 },   { BandType::highCut, 0.0f, 1.0f, 3, -3.0103 },
            { BandType::lowCut, 0.0f, 1.0f, 4, -3.0103 },   { BandType::highCut, 0.0f, 1.0f, 5, -3.0103 },
        };

        for (const auto& c : cases)
        {
            ParamValues pv;
            setBand (pv, 0, true, c.type, fc, c.gain, c.q, c.slope);
            CHECK_NEAR (measureGainDb (pv, fs, fc), c.expected, 0.02);
            CHECK_NEAR (curveDb (pv, fs, fc), c.expected, 0.02);
        }
    }
}

TEST (response_cuts_all_slopes_and_slope_per_octave)
{
    for (double fs : sampleRates)
        for (int slope = 0; slope < 6; ++slope)
            for (int high = 0; high < 2; ++high)
            {
                const BandType type = high ? BandType::highCut : BandType::lowCut;
                const int n = ref::order (slope);
                const float fc = 200.0f;
                ParamValues pv;
                setBand (pv, 5, true, type, fc, 0.0f, 0.70711f, slope);

                // every point against the independent Butterworth reference
                for (double f : probeFrequencies (fs, fc))
                {
                    const double expected = ref::bandGainDb (type, fs, fc, 0.0, 0.70711, slope, f);
                    const double measured = measureGainDb (pv, fs, f, 0.6);
                    const double viaCurve = curveDb (pv, fs, f);
                    const bool ok = matchesDb (measured, expected, 0.02) && matchesDb (viaCurve, expected, 0.005);
                    if (!ok)
                        std::printf ("    %s slope=%d fs=%.0f f=%.2f: ref %.4f curve %.4f engine %.4f\n", typeName (type),
                                     slope, fs, f, expected, viaCurve, measured);
                    CHECK (ok);
                }

                // nominal slope: 6 dB per octave and order, measured over one octave in the asymptotic range
                // (2 -> 3 octaves from the corner for orders <= 3, 1 -> 2 octaves above; the first-order
                // response has not reached its asymptote one octave from the corner)
                const double first = (n <= 3) ? 4.0 : 2.0;
                const double fa = high ? fc * first : fc / first;
                const double fb = high ? fa * 2.0 : fa / 2.0;
                const double ga = measureGainDb (pv, fs, fa, 0.6);
                const double gb = measureGainDb (pv, fs, fb, 0.6);
                CHECK_NEAR (ga - gb, 6.0206 * n, 0.5);
            }
}

TEST (response_dc_and_nyquist_limits)
{
    for (double fs : sampleRates)
    {
        ParamValues pv;
        setBand (pv, 0, true, BandType::lowShelf, 300.0f, 12.0f, 0.71f);
        CHECK_NEAR (measureGainDb (pv, fs, 0.0), 12.0, 0.01);
        CHECK_NEAR (measureGainDb (pv, fs, 0.5 * fs), 0.0, 0.01);

        setBand (pv, 0, true, BandType::highShelf, 3000.0f, -12.0f, 0.71f);
        CHECK_NEAR (measureGainDb (pv, fs, 0.0), 0.0, 0.01);
        CHECK_NEAR (measureGainDb (pv, fs, 0.5 * fs), -12.0, 0.01);

        setBand (pv, 0, true, BandType::bell, 3000.0f, 12.0f, 1.0f);
        CHECK_NEAR (measureGainDb (pv, fs, 0.0), 0.0, 0.01);
        CHECK_NEAR (measureGainDb (pv, fs, 0.5 * fs), 0.0, 0.01);

        setBand (pv, 0, true, BandType::lowCut, 100.0f, 0.0f, 0.7071f, 3);
        CHECK (measureGainDb (pv, fs, 0.0) < -100.0);
        CHECK_NEAR (measureGainDb (pv, fs, 0.5 * fs), 0.0, 0.01);

        setBand (pv, 0, true, BandType::highCut, 5000.0f, 0.0f, 0.7071f, 3);
        CHECK_NEAR (measureGainDb (pv, fs, 0.0), 0.0, 0.01);
        CHECK (measureGainDb (pv, fs, 0.5 * fs) < -40.0);

        setBand (pv, 0, true, BandType::notch, 1000.0f, 0.0f, 2.0f);
        CHECK_NEAR (measureGainDb (pv, fs, 0.0), 0.0, 0.01);
        CHECK_NEAR (measureGainDb (pv, fs, 0.5 * fs), 0.0, 0.01);
        CHECK (measureGainDb (pv, fs, 1000.0) < -80.0);

        setBand (pv, 0, true, BandType::bandPass, 1000.0f, 0.0f, 2.0f);
        CHECK (measureGainDb (pv, fs, 0.0) < -60.0);
        CHECK (measureGainDb (pv, fs, 0.5 * fs) < -20.0);
    }
}

TEST (response_cascade_of_all_bands_matches_curve)
{
    for (double fs : sampleRates)
    {
        ParamValues pv;
        setBand (pv, 0, true, BandType::lowCut, 40.0f, 0.0f, 0.7071f, 3);
        setBand (pv, 1, true, BandType::lowShelf, 120.0f, 4.0f, 0.71f);
        setBand (pv, 2, true, BandType::bell, 300.0f, -5.0f, 1.2f);
        setBand (pv, 3, true, BandType::bell, 900.0f, 3.0f, 2.0f);
        setBand (pv, 4, true, BandType::notch, 2500.0f, 0.0f, 4.0f);
        setBand (pv, 5, true, BandType::bandPass, 6000.0f, 0.0f, 0.3f);
        setBand (pv, 6, true, BandType::highShelf, 9000.0f, -4.0f, 0.71f);
        setBand (pv, 7, true, BandType::highCut, 15000.0f, 0.0f, 0.7071f, 2);

        for (double f : { 25.0, 60.0, 150.0, 400.0, 900.0, 1500.0, 2400.0, 3300.0, 5000.0, 7000.0, 11000.0, 14000.0 })
        {
            if (f >= 0.45 * fs)
                continue;
            double expected = 0.0;
            for (int b = 0; b < 8; ++b)
            {
                const BandParams p = pv.band (b);
                expected += ref::bandGainDb (p.type, fs, p.freqHz, p.gainDb, p.q, p.slopeIndex, f);
            }
            const double measured = measureGainDb (pv, fs, f, 0.8);
            const double viaCurve = curveDb (pv, fs, f);
            CHECK_NEAR (viaCurve, expected, 0.005);
            if (expected > -80.0)
                CHECK_NEAR (measured, expected, 0.02);
        }
    }
}

// Spike investigation (ARCHITECTURE.md 11.2): the smoke host measured 5.94 dB instead of 6.05 dB for a low
// shelf at its maximum because its window was shorter than the settling time. After settling it is 6.054 dB.
TEST (response_spike_low_shelf_plus18_at_100hz)
{
    ParamValues pv;
    setBand (pv, 0, true, BandType::lowShelf, 80.0f, 18.0f, 0.71f);
    CHECK_NEAR (measureGainDb (pv, 48000.0, 100.0, 2.0), 6.054, 0.01);
    CHECK_NEAR (curveDb (pv, 48000.0, 100.0), 6.054, 0.01);
}
