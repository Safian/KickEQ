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

// Parameter smoothing and cross-fades: no clicks when a parameter, a type, a placement, the bypass or the
// enable flag steps, and the new state is reached within the specified times.

#include "kickeq/eq_curve.h"
#include "test_util.h"

using namespace tu;

namespace
{
constexpr double fs = 48000.0;
constexpr int stepAt = 24000; // 0.5 s
constexpr int total = 48000;
constexpr double toneHz = 1000.0;
constexpr double toneOmega = 2.0 * kPi * toneHz / fs;

struct StepResult
{
    double maxJump = 0.0;    // largest sample-to-sample difference in [stepAt - 480, stepAt + 0.4 s]
    double ampBefore = 0.0;  // steady amplitude before the step (linear)
    double ampAfter = 0.0;   // steady amplitude 0.4 .. 0.5 s after the step
    std::vector<float> out;
};

/** A 1 kHz tone (amplitude 0.5, identical on both channels) through the engine, parameters stepping at 0.5 s. */
StepResult runStep (const ParamValues& before, const ParamValues& after)
{
    auto e = makeEngine (fs);
    std::vector<float> l (total), r;
    for (int i = 0; i < total; ++i)
        l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (toneOmega * i));
    r = l;

    processBlocks (*e, before, l.data(), r.data(), stepAt, 256);
    processBlocks (*e, after, l.data() + stepAt, r.data() + stepAt, total - stepAt, 256);

    StepResult res;
    for (int i = stepAt - 480; i < stepAt + 19200; ++i)
        res.maxJump = std::max (res.maxJump, static_cast<double> (std::fabs (l[static_cast<size_t> (i)] - l[static_cast<size_t> (i) - 1])));
    res.ampBefore = lockInAmplitude (l.data() + stepAt - 9600, 9600, toneOmega);
    res.ampAfter = lockInAmplitude (l.data() + total - 4800, 4800, toneOmega);
    res.out = std::move (l);
    return res;
}

/** The step must be smooth: no jump beyond what a sine of the larger of the two steady amplitudes makes. */
void expectClickFree (const char* what, const ParamValues& before, const ParamValues& after, double expectedAfterDb,
                      bool checkBefore = false, double expectedBeforeDb = 0.0, bool structural = false)
{
    const StepResult r = runStep (before, after);
    const double natural = std::max (r.ampBefore, r.ampAfter) * toneOmega;
    // Parameter steps (gain, freq, Q, output gain, bypass): no jump beyond the natural slope of the tone
    // (+5 %) plus -60 dBFS. Structural changes (type, slope, placement) fade in a new filter that starts
    // from zero state; its start-up transient is weighted by the linear fade and adds at most ~30 % to the
    // slope. A real discontinuity would be a jump of the order of the tone's amplitude (> 7x the natural slope).
    const double limit = natural * (structural ? 1.35 : 1.05) + 0.001;
    if (r.maxJump > limit || std::fabs (toDb (r.ampAfter / 0.5) - expectedAfterDb) > 0.03)
        std::printf ("    %s: max jump %.5f (limit %.5f), after %.4f dB (expected %.4f)\n", what, r.maxJump, limit,
                     toDb (r.ampAfter / 0.5), expectedAfterDb);
    CHECK (r.maxJump <= limit);
    CHECK_NEAR (toDb (r.ampAfter / 0.5), expectedAfterDb, 0.03);
    if (checkBefore)
        CHECK_NEAR (toDb (r.ampBefore / 0.5), expectedBeforeDb, 0.03);
}

double refDb (const ParamValues& pv, double freq)
{
    double db = 0.0;
    for (int b = 0; b < numBands; ++b)
    {
        const BandParams p = pv.band (b);
        if (p.on)
            db += ref::bandGainDb (p.type, fs, p.freqHz, p.gainDb, p.q, p.slopeIndex, freq);
    }
    return db;
}
} // namespace

TEST (smoothing_gain_step_is_click_free_and_settles)
{
    ParamValues a, b;
    setBand (a, 2, true, BandType::bell, 1000.0f, 0.0f, 1.0f);
    setBand (b, 2, true, BandType::bell, 1000.0f, 12.0f, 1.0f);
    expectClickFree ("gain 0 -> 12", a, b, 12.0, true, 0.0);
    expectClickFree ("gain 12 -> -18", b, [&] { ParamValues c; setBand (c, 2, true, BandType::bell, 1000.0f, -18.0f, 1.0f); return c; }(), -18.0);
}

TEST (smoothing_frequency_and_q_steps_are_click_free)
{
    ParamValues a, b, c;
    setBand (a, 1, true, BandType::bell, 1000.0f, 12.0f, 4.0f);
    setBand (b, 1, true, BandType::bell, 2000.0f, 12.0f, 4.0f);
    expectClickFree ("freq 1k -> 2k", a, b, refDb (b, toneHz));
    expectClickFree ("freq 2k -> 1k", b, a, refDb (a, toneHz));

    setBand (c, 1, true, BandType::bell, 1000.0f, 12.0f, 0.3f);
    expectClickFree ("Q 4 -> 0.3", a, c, refDb (c, toneHz));
    expectClickFree ("Q 0.3 -> 4", c, a, refDb (a, toneHz));

    ParamValues lc1, lc2;
    setBand (lc1, 0, true, BandType::lowCut, 100.0f, 0.0f, 0.7071f, 1);
    setBand (lc2, 0, true, BandType::lowCut, 3000.0f, 0.0f, 0.7071f, 1);
    expectClickFree ("low cut 100 Hz -> 3 kHz", lc1, lc2, refDb (lc2, toneHz));
}

TEST (smoothing_output_gain_and_auto_gain_steps_are_click_free)
{
    ParamValues a, b;
    expectClickFree ("out gain 0 -> +12", a, [&] { ParamValues c; c.set (paramOutGain, 12.0f); return c; }(), 12.0);
    expectClickFree ("out gain +6 -> -24", [&] { ParamValues c; c.set (paramOutGain, 6.0f); return c; }(),
                     [&] { ParamValues c; c.set (paramOutGain, -24.0f); return c; }(), -24.0);

    setBand (a, 2, true, BandType::bell, 1000.0f, 12.0f, 1.0f);
    b = a;
    b.set (paramAutoGain, 1.0f);
    // auto gain compensates the pink-weighted mean of the response, not the response at 1 kHz
    const double g = computeAutoGainDb (a, fs);
    expectClickFree ("auto gain off -> on", a, b, 12.0 - g);
    expectClickFree ("auto gain on -> off", b, a, 12.0);
}

TEST (smoothing_band_enable_disable_type_slope_placement_crossfades_are_click_free)
{
    ParamValues off, on;
    setBand (off, 3, false, BandType::bell, 1000.0f, 9.0f, 2.0f);
    setBand (on, 3, true, BandType::bell, 1000.0f, 9.0f, 2.0f);
    expectClickFree ("band enable", off, on, 9.0, true, 0.0, true);
    expectClickFree ("band disable", on, off, 0.0, true, 9.0, true);

    ParamValues bell, notch, shelf;
    setBand (bell, 3, true, BandType::bell, 1000.0f, 9.0f, 2.0f);
    setBand (notch, 3, true, BandType::notch, 1200.0f, 0.0f, 2.0f);
    setBand (shelf, 3, true, BandType::highShelf, 1000.0f, -8.0f, 0.71f);
    expectClickFree ("type bell -> notch", bell, notch, refDb (notch, toneHz), false, 0.0, true);
    expectClickFree ("type notch -> shelf", notch, shelf, refDb (shelf, toneHz), false, 0.0, true);
    expectClickFree ("type shelf -> bell", shelf, bell, refDb (bell, toneHz), false, 0.0, true);

    ParamValues cut12, cut48;
    setBand (cut12, 0, true, BandType::lowCut, 800.0f, 0.0f, 0.7071f, 1);
    setBand (cut48, 0, true, BandType::lowCut, 800.0f, 0.0f, 0.7071f, 5);
    expectClickFree ("slope 12 -> 48", cut12, cut48, refDb (cut48, toneHz), false, 0.0, true);
    expectClickFree ("slope 48 -> 12", cut48, cut12, refDb (cut12, toneHz), false, 0.0, true);

    // the tone is the same on both channels: a Mid band acts fully, a Left band on that channel only
    ParamValues stereo, mid, left;
    setBand (stereo, 3, true, BandType::bell, 1000.0f, 9.0f, 2.0f, 1, Placement::stereo);
    setBand (mid, 3, true, BandType::bell, 1000.0f, 9.0f, 2.0f, 1, Placement::mid);
    setBand (left, 3, true, BandType::bell, 1000.0f, 9.0f, 2.0f, 1, Placement::left);
    expectClickFree ("placement stereo -> mid", stereo, mid, 9.0, false, 0.0, true);
    expectClickFree ("placement mid -> stereo", mid, stereo, 9.0, false, 0.0, true);
    expectClickFree ("placement stereo -> left", stereo, left, 9.0, false, 0.0, true);
    ParamValues side = mid;
    side.set (3, BandParam::place, static_cast<float> (Placement::side));
    expectClickFree ("placement mid -> side (L = R: no effect)", mid, side, 0.0, false, 0.0, true);
}

TEST (smoothing_bypass_crossfade_is_click_free_and_ends_bit_identical)
{
    ParamValues active, bypassed;
    setBand (active, 3, true, BandType::bell, 1000.0f, 9.0f, 2.0f);
    bypassed = active;
    bypassed.set (paramBypass, 1.0f);

    expectClickFree ("bypass on", active, bypassed, 0.0, true, 9.0);
    expectClickFree ("bypass off", bypassed, active, 9.0, true, 0.0);

    // after the fade the output is exactly the input
    const StepResult r = runStep (active, bypassed);
    bool exact = true;
    for (int i = stepAt + 1100; i < total; ++i)
        exact = exact && r.out[static_cast<size_t> (i)] == static_cast<float> (0.5 * std::sin (toneOmega * i));
    CHECK (exact);
}

TEST (smoothing_rapid_automation_of_structure_stays_continuous)
{
    // type / placement / enable changing every few milliseconds: overlapping cross-fades must neither click
    // nor blow up
    auto e = makeEngine (fs);
    const int n = 48000;
    std::vector<float> l (n), r;
    for (int i = 0; i < n; ++i)
        l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (toneOmega * i));
    r = l;

    Rng rng (99);
    double maxJump = 0.0;
    float prev = 0.0f;
    for (int pos = 0; pos < n; pos += 100)
    {
        ParamValues pv;
        setBand (pv, 4, rng.integer (0, 3) != 0, static_cast<BandType> (rng.integer (0, 6)),
                 static_cast<float> (rng.range (300.0, 3000.0)), static_cast<float> (rng.range (-12.0, 12.0)),
                 static_cast<float> (rng.range (0.5, 4.0)), rng.integer (0, 5), static_cast<Placement> (rng.integer (0, 4)));
        e->process (pv, l.data() + pos, r.data() + pos, 100);
        for (int i = pos; i < pos + 100; ++i)
        {
            if (pos > 0 || i > 0)
                maxJump = std::max (maxJump, static_cast<double> (std::fabs (l[static_cast<size_t> (i)] - prev)));
            prev = l[static_cast<size_t> (i)];
        }
    }
    CHECK (allFinite (l.data(), n));
    // amplitude stays within the +-12 dB range of the bands (plus a band pass / shelf are <= 0 dB .. 12 dB)
    CHECK (maxJump < 0.5 * 4.0 * 3.0 * toneOmega);
}

TEST (smoothing_ramp_follows_30ms_linear_in_db)
{
    // gain 0 -> +12 dB at t = 0 (after a snapped start): halfway (15 ms) the gain is about +6 dB, at 10 ms
    // about +4 dB, and from 35 ms on it is 12 dB
    auto e = makeEngine (fs);
    ParamValues a, b;
    setBand (a, 2, true, BandType::bell, 1000.0f, 0.0f, 1.0f);
    setBand (b, 2, true, BandType::bell, 1000.0f, 12.0f, 1.0f);

    const int pre = 4800, n = 4800;
    std::vector<float> l (pre + n), r;
    for (int i = 0; i < pre + n; ++i)
        l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (toneOmega * i));
    r = l;
    processBlocks (*e, a, l.data(), r.data(), pre, 256);
    processBlocks (*e, b, l.data() + pre, r.data() + pre, n, 256);

    const auto gainAround = [&] (double ms, double halfWidthMs)
    {
        const int from = pre + static_cast<int> ((ms - halfWidthMs) * fs / 1000.0);
        const int len = static_cast<int> (2.0 * halfWidthMs * fs / 1000.0);
        return toDb (lockInAmplitude (l.data() + from, len, toneOmega) / 0.5);
    };

    CHECK_NEAR (gainAround (10.0, 2.0), 4.0, 1.2);
    CHECK_NEAR (gainAround (15.0, 2.0), 6.0, 1.2);
    CHECK_NEAR (gainAround (20.0, 2.0), 8.0, 1.2);
    CHECK_NEAR (gainAround (40.0, 5.0), 12.0, 0.1);
}

TEST (smoothing_no_trigonometry_state_changes_at_rest)
{
    // at rest the coefficients are not touched: two runs, one with a redundant (identical) target handed over
    // every call and one handed over once, are bit-identical
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 1000.0f, 6.0f, 1.0f);
    setBand (pv, 5, true, BandType::lowCut, 80.0f, 0.0f, 0.7071f, 3);
    const auto in = noise (20000, 77, 0.5f);

    auto a = in, aR = in, b = in, bR = in;
    auto e1 = makeEngine (fs), e2 = makeEngine (fs);
    processBlocks (*e1, pv, a.data(), aR.data(), 20000, 64);
    e2->process (pv, b.data(), bR.data(), 20000);
    CHECK (bitIdentical (a, b));
    CHECK (bitIdentical (aR, bR));
}
