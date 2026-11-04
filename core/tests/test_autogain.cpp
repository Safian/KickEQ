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

// Auto gain (ARCHITECTURE.md 4.5): the pink-noise weighted RMS gain is compensated within 0.5 dB.

#include "kickeq/eq_curve.h"
#include "test_util.h"

using namespace tu;

namespace
{
constexpr double fs = 44100.0; // the pink filter below is designed for 44.1 kHz

/** Pink noise (Paul Kellet's refined filter, +-0.05 dB from 9.2 Hz to Nyquist at 44.1 kHz), limited to
    20 Hz .. 20 kHz by a 2nd order Butterworth high pass and a 1st order low pass. */
std::vector<float> pinkNoise (int n, uint32_t seed)
{
    Rng rng (seed);
    std::vector<float> out (static_cast<size_t> (n));

    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

    // high pass 20 Hz (RBJ, Q = 1/sqrt 2), direct form I
    const double w0 = 2.0 * kPi * 20.0 / fs, cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.70710678);
    const double h0 = (1 + cw) / 2 / (1 + alpha), h1 = -(1 + cw) / (1 + alpha), h2 = h0;
    const double ha1 = -2 * cw / (1 + alpha), ha2 = (1 - alpha) / (1 + alpha);
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    // low pass 20 kHz, one pole
    const double lp = std::exp (-2.0 * kPi * 20000.0 / fs);
    double lpState = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double w = static_cast<double> (rng.uniform());
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;

        const double hp = h0 * pink + h1 * x1 + h2 * x2 - ha1 * y1 - ha2 * y2;
        x2 = x1;
        x1 = pink;
        y2 = y1;
        y1 = hp;
        lpState = (1.0 - lp) * hp + lp * lpState;
        out[static_cast<size_t> (i)] = static_cast<float> (0.05 * lpState);
    }
    return out;
}

double rms (const std::vector<float>& l, const std::vector<float>& r, size_t from)
{
    double s = 0.0;
    for (size_t i = from; i < l.size(); ++i)
        s += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
    return std::sqrt (s / (2.0 * static_cast<double>(l.size() - from)));
}

/** dB change of the output level on uncorrelated pink noise when auto gain is on, relative to the input. */
double levelChangeWithAutoGain (ParamValues pv, const std::vector<float>& inL, const std::vector<float>& inR, double* autoDb = nullptr)
{
    pv.set (paramAutoGain, 1.0f);
    auto e = makeEngine (fs);
    auto l = inL, r = inR;
    processBlocks (*e, pv, l.data(), r.data(), static_cast<int> (l.size()), 512);
    if (autoDb != nullptr)
        *autoDb = e->getAutoGainDb();
    const size_t skip = static_cast<size_t> (fs); // 1 s: transient + smoothing
    return toDb (rms (l, r, skip) / rms (inL, inR, skip));
}

double levelChangeWithoutAutoGain (ParamValues pv, const std::vector<float>& inL, const std::vector<float>& inR)
{
    pv.set (paramAutoGain, 0.0f);
    auto e = makeEngine (fs);
    auto l = inL, r = inR;
    processBlocks (*e, pv, l.data(), r.data(), static_cast<int> (l.size()), 512);
    const size_t skip = static_cast<size_t> (fs);
    return toDb (rms (l, r, skip) / rms (inL, inR, skip));
}
} // namespace

TEST (autogain_compensates_pink_noise_within_half_a_db)
{
    const int n = static_cast<int> (40.0 * fs);
    const auto inL = pinkNoise (n, 1001), inR = pinkNoise (n, 2002);

    struct Scene { const char* name; ParamValues pv; };
    std::vector<Scene> scenes;

    { ParamValues p; setBand (p, 2, true, BandType::bell, 1000.0f, 9.0f, 1.0f); scenes.push_back ({ "bell +9 @1k", p }); }
    { ParamValues p; setBand (p, 6, true, BandType::highShelf, 3000.0f, -8.0f, 0.71f); scenes.push_back ({ "high shelf -8 @3k", p }); }
    { ParamValues p;
      setBand (p, 0, true, BandType::lowCut, 100.0f, 0.0f, 0.7071f, 3);
      setBand (p, 2, true, BandType::bell, 250.0f, -6.0f, 1.0f);
      scenes.push_back ({ "low cut 24 @100 + bell -6 @250", p }); }
    { ParamValues p;
      setBand (p, 1, true, BandType::lowShelf, 200.0f, 10.0f, 0.71f);
      setBand (p, 6, true, BandType::highShelf, 8000.0f, 6.0f, 0.71f);
      scenes.push_back ({ "low shelf +10 / high shelf +6", p }); }
    { ParamValues p;
      setBand (p, 3, true, BandType::bell, 3000.0f, 12.0f, 8.0f);
      setBand (p, 4, true, BandType::bell, 800.0f, -9.0f, 0.5f);
      setBand (p, 7, true, BandType::highCut, 12000.0f, 0.0f, 0.7071f, 2);
      scenes.push_back ({ "narrow +12, wide -9, high cut", p }); }
    { ParamValues p;
      setBand (p, 3, true, BandType::bell, 1000.0f, 9.0f, 1.0f, 1, Placement::mid);
      setBand (p, 4, true, BandType::bell, 5000.0f, -6.0f, 1.0f, 1, Placement::side);
      scenes.push_back ({ "mid +9 @1k, side -6 @5k", p }); }
    { ParamValues p;
      setBand (p, 3, true, BandType::bell, 1000.0f, 12.0f, 0.5f, 1, Placement::left);
      setBand (p, 4, true, BandType::bell, 4000.0f, -10.0f, 1.0f, 1, Placement::right);
      scenes.push_back ({ "left +12 @1k, right -10 @4k", p }); }

    for (const auto& s : scenes)
    {
        double g = 0.0;
        const double withAuto = levelChangeWithAutoGain (s.pv, inL, inR, &g);
        const double without = levelChangeWithoutAutoGain (s.pv, inL, inR);
        std::printf ("    %-34s level without auto gain %+6.2f dB, with %+6.2f dB, computed G %+6.2f dB\n", s.name, without, withAuto, g);
        CHECK_NEAR (withAuto, 0.0, 0.5);
        CHECK (std::fabs (without) > 0.5); // the scene really changes the level
    }
}

TEST (autogain_is_zero_without_bands_and_clamped_to_12_db)
{
    ParamValues none;
    CHECK (computeAutoGainDb (none, 48000.0) == 0.0);

    ParamValues loud, quiet;
    setBand (loud, 0, true, BandType::bell, 1000.0f, 24.0f, 0.1f);
    setBand (loud, 1, true, BandType::bell, 1000.0f, 24.0f, 0.1f);
    setBand (quiet, 0, true, BandType::bell, 1000.0f, -24.0f, 0.1f);
    setBand (quiet, 1, true, BandType::bell, 1000.0f, -24.0f, 0.1f);
    CHECK_NEAR (computeAutoGainDb (loud, 48000.0), 12.0, 1.0e-12);
    CHECK_NEAR (computeAutoGainDb (quiet, 48000.0), -12.0, 1.0e-12);

    // the engine applies -G (smoothed): +12 dB response is brought back down by 12 dB
    auto e = makeEngine (48000.0);
    std::vector<float> l (96000, 0.0f), r;
    loud.set (paramAutoGain, 1.0f);
    r = l;
    e->process (loud, l.data(), r.data(), 96000);
    CHECK_NEAR (e->getAutoGainDb(), 12.0, 1.0e-12);
}

TEST (autogain_matches_the_analytic_mean_for_a_stereo_band)
{
    // a single stereo band: G is the log-frequency mean of |H|^2 over 96 points 20 Hz .. 20 kHz
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 1000.0f, 12.0f, 1.0f);

    double sum = 0.0;
    for (int i = 0; i < 96; ++i)
    {
        const double f = 20.0 * std::pow (1000.0, i / 95.0);
        sum += std::pow (10.0, ref::bandGainDb (BandType::bell, 48000.0, 1000.0, 12.0, 1.0, 1, f) / 10.0);
    }
    CHECK_NEAR (computeAutoGainDb (pv, 48000.0), 10.0 * std::log10 (sum / 96.0), 0.002);
}
