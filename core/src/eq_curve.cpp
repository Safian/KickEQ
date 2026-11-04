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

#include "kickeq/eq_curve.h"

#include <cmath>

namespace kickeq
{
namespace
{
using detail::pi;

Complex mul (Complex a, Complex b) noexcept { return { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; }
Complex add (Complex a, Complex b) noexcept { return { a.re + b.re, a.im + b.im }; }
Complex sub (Complex a, Complex b) noexcept { return { a.re - b.re, a.im - b.im }; }
Complex scale (Complex a, double s) noexcept { return { a.re * s, a.im * s }; }
double norm2 (Complex a) noexcept { return a.re * a.re + a.im * a.im; }

Complex cdiv (Complex a, Complex b) noexcept
{
    const double d = norm2 (b);
    return { (a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d };
}

double omegaFor (double freqHz, double sampleRate) noexcept
{
    double f = freqHz > 0.0 ? freqHz : 0.0;
    if (f > 0.5 * sampleRate)
        f = 0.5 * sampleRate;
    return 2.0 * pi * f / sampleRate;
}

struct Chain
{
    BandDesign designs[numBands];
    Placement place[numBands] = {};
    bool on[numBands] = {};
};

Chain makeChain (const ParamValues& params, double sampleRate) noexcept
{
    Chain c;
    for (int b = 0; b < numBands; ++b)
    {
        const BandParams p = params.band (b);
        c.on[b] = p.on;
        c.place[b] = p.place;
        if (p.on)
            c.designs[b] = designBand (p.type, sampleRate, p.freqHz, p.gainDb, p.q, p.slopeIndex);
    }
    return c;
}

/** Product of all enabled bands of a placement at one frequency. */
Complex placementResponse (const Chain& c, Placement place, double omega) noexcept
{
    Complex h { 1.0, 0.0 };
    for (int b = 0; b < numBands; ++b)
        if (c.on[b] && c.place[b] == place)
            h = mul (h, bandResponse (c.designs[b], omega));
    return h;
}

struct Stages
{
    Complex st, l, r, m, s;
};

Stages stagesAt (const Chain& c, double omega) noexcept
{
    return { placementResponse (c, Placement::stereo, omega), placementResponse (c, Placement::left, omega),
             placementResponse (c, Placement::right, omega), placementResponse (c, Placement::mid, omega),
             placementResponse (c, Placement::side, omega) };
}

Complex viewResponse (const Stages& h, ChannelView view) noexcept
{
    // L' = a L1 + b R1, R' = b L1 + a R1 with a = (Hm + Hs) / 2, b = (Hm - Hs) / 2 (section 4.4)
    const Complex a = scale (add (h.m, h.s), 0.5);
    const Complex b = scale (sub (h.m, h.s), 0.5);
    const Complex avgLr = scale (add (h.l, h.r), 0.5);

    switch (view)
    {
        case ChannelView::left:  return mul (h.st, mul (h.l, a));
        case ChannelView::right: return mul (h.st, mul (h.r, a));
        case ChannelView::mid:   return mul (h.st, mul (h.m, avgLr));
        case ChannelView::side:  return mul (h.st, mul (h.s, avgLr));
        case ChannelView::stereo:
        default:                 return mul (h.st, add (mul (a, h.l), mul (b, h.r)));
    }
}

void writeResult (Complex h, float* magDb, float* phaseRad, int i) noexcept
{
    if (magDb != nullptr)
    {
        const double mag = std::sqrt (norm2 (h));
        magDb[i] = static_cast<float> (20.0 * std::log10 (mag > 1.0e-15 ? mag : 1.0e-15));
    }
    if (phaseRad != nullptr)
        phaseRad[i] = static_cast<float> (std::atan2 (h.im, h.re));
}
} // namespace

Complex sectionResponse (const BiquadCoefficients& c, double omega) noexcept
{
    const double c1 = std::cos (omega), s1 = std::sin (omega);
    const double c2 = std::cos (2.0 * omega), s2 = std::sin (2.0 * omega);
    // z^-1 = c1 - j s1, z^-2 = c2 - j s2
    const Complex num { c.b0 + c.b1 * c1 + c.b2 * c2, -(c.b1 * s1 + c.b2 * s2) };
    const Complex den { 1.0 + c.a1 * c1 + c.a2 * c2, -(c.a1 * s1 + c.a2 * s2) };
    return cdiv (num, den);
}

Complex bandResponse (const BandDesign& d, double omega) noexcept
{
    Complex h { 1.0, 0.0 };
    for (int s = 0; s < d.numSections; ++s)
        h = mul (h, sectionResponse (d.sections[static_cast<size_t> (s)], omega));
    return h;
}

void computeResponse (const ParamValues& params, double sampleRate, const float* freqsHz, float* magDb,
                      float* phaseRad, int n, ChannelView view) noexcept
{
    const Chain chain = makeChain (params, sampleRate);
    for (int i = 0; i < n; ++i)
        writeResult (viewResponse (stagesAt (chain, omegaFor (static_cast<double> (freqsHz[i]), sampleRate)), view),
                     magDb, phaseRad, i);
}

void computeBandResponse (const BandParams& band, double sampleRate, const float* freqsHz, float* magDb,
                          float* phaseRad, int n) noexcept
{
    const BandDesign d = designBand (band.type, sampleRate, band.freqHz, band.gainDb, band.q, band.slopeIndex);
    for (int i = 0; i < n; ++i)
        writeResult (bandResponse (d, omegaFor (static_cast<double> (freqsHz[i]), sampleRate)), magDb, phaseRad, i);
}

double computeAutoGainDb (const ParamValues& params, double sampleRate) noexcept
{
    const Chain chain = makeChain (params, sampleRate);

    bool any = false;
    for (int b = 0; b < numBands; ++b)
        any = any || chain.on[b];
    if (!any)
        return 0.0;

    const double fLo = 20.0;
    const double fHi = std::fmin (20000.0, 0.5 * sampleRate);
    const double ratio = std::log (fHi / fLo);

    double sum = 0.0;
    for (int i = 0; i < autoGainPoints; ++i)
    {
        const double f = fLo * std::exp (ratio * i / (autoGainPoints - 1));
        const Stages h = stagesAt (chain, omegaFor (f, sampleRate));
        // uncorrelated, equal power noise on L and R: P(L') = |a|^2 |Hl|^2 + |b|^2 |Hr|^2 (times |Hst|^2) and
        // P(R') = |b|^2 |Hl|^2 + |a|^2 |Hr|^2; averaged over both outputs
        const double ab = 0.5 * (norm2 (h.m) + norm2 (h.s)); // |a|^2 + |b|^2
        sum += norm2 (h.st) * ab * 0.5 * (norm2 (h.l) + norm2 (h.r));
    }

    const double meanPower = sum / autoGainPoints;
    const double g = 10.0 * std::log10 (meanPower > 1.0e-30 ? meanPower : 1.0e-30);
    return g < -autoGainMaxDb ? -autoGainMaxDb : (g > autoGainMaxDb ? autoGainMaxDb : g);
}
} // namespace kickeq
