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

// Stereo / mid-side signal flow (ARCHITECTURE.md 4.4): the engine against the analytic channel views and
// against an explicit-matrix reference implementation with its own RBJ biquads.

#include "kickeq/eq_curve.h"
#include "test_util.h"

using namespace tu;

namespace
{

/** Channel-view measurement through the engine, see ChannelView. */
double measureView (const ParamValues& pv, double fs, double freq, ChannelView view)
{
    auto e = makeEngine (fs);
    const double omega = 2.0 * kPi * freq / fs;
    const int settle = static_cast<int> (0.6 * fs);
    const int measure = static_cast<int> (std::ceil (std::max (20.0, 0.1 * freq) * fs / freq));
    const int total = settle + measure;

    std::vector<float> l (static_cast<size_t> (total), 0.0f), r (static_cast<size_t> (total), 0.0f);
    for (int i = 0; i < total; ++i)
    {
        const float s = static_cast<float> (0.5 * std::sin (omega * i));
        switch (view)
        {
            case ChannelView::left:   l[static_cast<size_t> (i)] = s; break;
            case ChannelView::right:  r[static_cast<size_t> (i)] = s; break;
            case ChannelView::side:   l[static_cast<size_t> (i)] = s; r[static_cast<size_t> (i)] = -s; break;
            case ChannelView::mid:
            case ChannelView::stereo:
            default:                  l[static_cast<size_t> (i)] = s; r[static_cast<size_t> (i)] = s; break;
        }
    }
    processBlocks (*e, pv, l.data(), r.data(), total, 256);

    std::vector<float> out (static_cast<size_t> (measure));
    for (int i = 0; i < measure; ++i)
    {
        const float a = l[static_cast<size_t> (settle + i)], b = r[static_cast<size_t> (settle + i)];
        switch (view)
        {
            case ChannelView::left:   out[static_cast<size_t> (i)] = a; break;
            case ChannelView::right:  out[static_cast<size_t> (i)] = b; break;
            case ChannelView::mid:    out[static_cast<size_t> (i)] = 0.5f * (a + b); break;
            case ChannelView::side:   out[static_cast<size_t> (i)] = 0.5f * (a - b); break;
            case ChannelView::stereo:
            default:                  out[static_cast<size_t> (i)] = a; break;
        }
    }
    return toDb (lockInAmplitude (out.data(), measure, omega) / 0.5);
}

// ---- explicit reference model -------------------------------------------------------------------------
// Direct form I biquads from the RBJ cookbook, applied band by band with explicit M/S matrices.

struct RefBiquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};

    double run (int ch, double x)
    {
        const double y = b0 * x + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
        x2[ch] = x1[ch];
        x1[ch] = x;
        y2[ch] = y1[ch];
        y1[ch] = y;
        return y;
    }
};

RefBiquad cookbook (BandType type, double fs, double f0, double dBgain, double Q)
{
    const double A = std::pow (10.0, dBgain / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / (2.0 * Q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type)
    {
        case BandType::bell:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case BandType::lowShelf:
            b0 = A * ((A + 1) - (A - 1) * cw + 2 * std::sqrt (A) * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - 2 * std::sqrt (A) * alpha);
            a0 = (A + 1) + (A - 1) * cw + 2 * std::sqrt (A) * alpha;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - 2 * std::sqrt (A) * alpha;
            break;
        case BandType::highShelf:
            b0 = A * ((A + 1) + (A - 1) * cw + 2 * std::sqrt (A) * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - 2 * std::sqrt (A) * alpha);
            a0 = (A + 1) - (A - 1) * cw + 2 * std::sqrt (A) * alpha;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - 2 * std::sqrt (A) * alpha;
            break;
        case BandType::notch:
            b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case BandType::bandPass:
            b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case BandType::lowCut: // 12 dB/oct high pass
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case BandType::highCut: // 12 dB/oct low pass
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
    }

    RefBiquad r;
    r.b0 = b0 / a0; r.b1 = b1 / a0; r.b2 = b2 / a0; r.a1 = a1 / a0; r.a2 = a2 / a0;
    return r;
}

void referenceProcess (const ParamValues& pv, double fs, std::vector<double>& l, std::vector<double>& r)
{
    std::vector<RefBiquad> filters (numBands);
    for (int b = 0; b < numBands; ++b)
    {
        const BandParams p = pv.band (b);
        filters[static_cast<size_t> (b)] = cookbook (p.type, fs, p.freqHz, p.gainDb, p.q);
    }

    const size_t n = l.size();
    for (size_t i = 0; i < n; ++i)
    {
        double L = l[i], R = r[i];
        for (int b = 0; b < numBands; ++b)
            if (pv.band (b).on && pv.band (b).place == Placement::stereo)
            {
                auto& f = filters[static_cast<size_t> (b)];
                L = f.run (0, L);
                R = f.run (1, R);
            }
        for (int b = 0; b < numBands; ++b)
        {
            const BandParams p = pv.band (b);
            if (p.on && p.place == Placement::left)
                L = filters[static_cast<size_t> (b)].run (0, L);
            if (p.on && p.place == Placement::right)
                R = filters[static_cast<size_t> (b)].run (1, R);
        }

        // explicit matrix: [M S]^T = 1/2 [[1 1] [1 -1]] [L R]^T ; decode [L R]^T = [[1 1] [1 -1]] [M S]^T
        double M = 0.5 * (L + R), S = 0.5 * (L - R);
        for (int b = 0; b < numBands; ++b)
        {
            const BandParams p = pv.band (b);
            if (p.on && p.place == Placement::mid)
                M = filters[static_cast<size_t> (b)].run (0, M);
            if (p.on && p.place == Placement::side)
                S = filters[static_cast<size_t> (b)].run (1, S);
        }
        l[i] = M + S;
        r[i] = M - S;
    }
}
} // namespace

TEST (stereo_channel_views_match_the_analytic_response)
{
    for (double fs : { 44100.0, 48000.0 })
    {
        ParamValues pv;
        setBand (pv, 0, true, BandType::bell, 300.0f, 9.0f, 1.0f, 1, Placement::stereo);
        setBand (pv, 1, true, BandType::bell, 700.0f, -8.0f, 2.0f, 1, Placement::left);
        setBand (pv, 2, true, BandType::highShelf, 2000.0f, 6.0f, 0.71f, 1, Placement::right);
        setBand (pv, 3, true, BandType::bell, 1000.0f, 7.0f, 1.5f, 1, Placement::mid);
        setBand (pv, 4, true, BandType::lowShelf, 500.0f, -7.0f, 0.71f, 1, Placement::side);
        setBand (pv, 5, true, BandType::lowCut, 100.0f, 0.0f, 0.7071f, 3, Placement::left);
        setBand (pv, 6, true, BandType::highCut, 8000.0f, 0.0f, 0.7071f, 1, Placement::mid);

        for (ChannelView view : { ChannelView::stereo, ChannelView::left, ChannelView::right, ChannelView::mid, ChannelView::side })
            for (double f : { 120.0, 400.0, 900.0, 1500.0, 3000.0, 6000.0 })
            {
                const float ff = static_cast<float> (f);
                float mag = 0.0f;
                computeResponse (pv, fs, &ff, &mag, nullptr, 1, view);
                const double measured = measureView (pv, fs, f, view);
                if (std::fabs (measured - static_cast<double> (mag)) > 0.02)
                    std::printf ("    view %d fs %.0f f %.0f: curve %.4f engine %.4f\n", static_cast<int> (view), fs, f, mag, measured);
                CHECK_NEAR (measured, static_cast<double> (mag), 0.02);
            }
    }
}

TEST (stereo_flow_equals_explicit_matrix_reference)
{
    const BandType types[] = { BandType::bell, BandType::lowShelf, BandType::highShelf, BandType::notch,
                              BandType::bandPass, BandType::lowCut, BandType::highCut };
    Rng rng (2024);

    for (int iter = 0; iter < 24; ++iter)
    {
        const double fs = (iter % 3 == 0) ? 44100.0 : (iter % 3 == 1 ? 48000.0 : 96000.0);
        ParamValues pv;
        for (int b = 0; b < numBands; ++b)
            setBand (pv, b, rng.integer (0, 5) != 0, types[rng.integer (0, 6)], static_cast<float> (std::exp (rng.range (std::log (40.0), std::log (12000.0)))),
                     static_cast<float> (rng.range (-12.0, 12.0)), static_cast<float> (rng.range (0.4, 4.0)),
                     1, static_cast<Placement> (rng.integer (0, 4)));

        const int n = 12000;
        const auto inL = noise (n, 100u + static_cast<uint32_t> (iter), 0.5f);
        const auto inR = noise (n, 200u + static_cast<uint32_t> (iter), 0.5f);

        auto l = inL, r = inR;
        auto e = makeEngine (fs);
        processBlocks (*e, pv, l.data(), r.data(), n, 480);

        std::vector<double> rl (inL.begin(), inL.end()), rr (inR.begin(), inR.end());
        referenceProcess (pv, fs, rl, rr);

        double peak = 1.0e-9, worst = 0.0;
        for (size_t i = 0; i < static_cast<size_t> (n); ++i)
        {
            peak = std::max ({ peak, std::fabs (rl[i]), std::fabs (rr[i]) });
            worst = std::max ({ worst, std::fabs (rl[i] - l[i]), std::fabs (rr[i] - r[i]) });
        }
        if (worst > 4.0e-7 * peak)
            std::printf ("    iter %d fs %.0f: worst error %.3g (peak %.3g)\n", iter, fs, worst, peak);
        CHECK (worst <= 4.0e-7 * peak);
    }
}

TEST (stereo_side_only_band_leaves_a_mono_signal_untouched_and_mid_leaves_side_untouched)
{
    const int n = 8000;
    const auto mono = noise (n, 3, 0.7f);

    ParamValues sideBand;
    setBand (sideBand, 1, true, BandType::bell, 1000.0f, 12.0f, 1.0f, 1, Placement::side);
    auto l = mono, r = mono;
    auto e = makeEngine (48000.0);
    processBlocks (*e, sideBand, l.data(), r.data(), n, 256);
    CHECK (bitIdentical (l, mono));
    CHECK (bitIdentical (r, mono));

    // L = -R is pure side: a Mid band has no effect (up to the rounding of the encode / decode pair)
    ParamValues midBand;
    setBand (midBand, 1, true, BandType::bell, 1000.0f, 12.0f, 1.0f, 1, Placement::mid);
    l = mono;
    r = mono;
    for (auto& x : r)
        x = -x;
    auto e2 = makeEngine (48000.0);
    processBlocks (*e2, midBand, l.data(), r.data(), n, 256);
    for (int i = 0; i < n; ++i)
    {
        CHECK_NEAR (l[static_cast<size_t> (i)], mono[static_cast<size_t> (i)], 1.0e-7);
        CHECK_NEAR (r[static_cast<size_t> (i)], -mono[static_cast<size_t> (i)], 1.0e-7);
    }
}

TEST (stereo_left_and_right_bands_are_independent)
{
    const int n = 10000;
    const auto inL = noise (n, 31, 0.5f);
    const auto inR = noise (n, 32, 0.5f);

    ParamValues leftOnly;
    setBand (leftOnly, 0, true, BandType::bell, 800.0f, 12.0f, 2.0f, 1, Placement::left);
    auto l = inL, r = inR;
    auto e = makeEngine (48000.0);
    processBlocks (*e, leftOnly, l.data(), r.data(), n, 256);
    CHECK (bitIdentical (r, inR)); // the right channel is untouched, bit for bit
    CHECK (!bitIdentical (l, inL));
}
