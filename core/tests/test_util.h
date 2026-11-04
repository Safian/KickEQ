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

// Shared helpers of the core tests: signal generators, measurement, and an independent reference model of the
// filters (written from the RBJ cookbook analog prototypes + bilinear transform, NOT from core/biquad.h).

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "kickeq/eq_engine.h"
#include "kickeq/params.h"
#include "test.h"

namespace tu
{
using namespace kickeq;

constexpr double kPi = 3.14159265358979323846;

inline double toDb (double lin) { return 20.0 * std::log10 (lin > 1.0e-300 ? lin : 1.0e-300); }

/** Deterministic xorshift32 noise source. */
struct Rng
{
    uint32_t s;
    explicit Rng (uint32_t seed = 1) : s (seed != 0 ? seed : 1) {}
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    /** uniform in [-1, 1) */
    float uniform() { return static_cast<float> (static_cast<double> (next()) / 2147483648.0 - 1.0); }
    double range (double lo, double hi) { return lo + (hi - lo) * (static_cast<double> (next()) / 4294967296.0); }
    int integer (int lo, int hi) { return lo + static_cast<int> (next() % static_cast<uint32_t> (hi - lo + 1)); }
};

inline std::vector<float> noise (int n, uint32_t seed, float amp = 1.0f)
{
    Rng rng (seed);
    std::vector<float> v (static_cast<size_t> (n));
    for (auto& x : v)
        x = amp * rng.uniform();
    return v;
}

inline bool bitIdentical (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0);
}

inline std::unique_ptr<EqEngine> makeEngine (double fs, int maxBlock = 512)
{
    auto e = std::make_unique<EqEngine>();
    e->prepare (fs, maxBlock);
    return e;
}

inline void setBand (ParamValues& pv, int b, bool on, BandType type, float freq, float gain, float q, int slope = 1,
                     Placement place = Placement::stereo)
{
    pv.set (b, BandParam::on, on ? 1.0f : 0.0f);
    pv.set (b, BandParam::type, static_cast<float> (type));
    pv.set (b, BandParam::freq, freq);
    pv.set (b, BandParam::gain, gain);
    pv.set (b, BandParam::q, q);
    pv.set (b, BandParam::slope, static_cast<float> (slope));
    pv.set (b, BandParam::place, static_cast<float> (place));
}

/** Processes a stereo signal in blocks of `blockSize` (the last one shorter). */
inline void processBlocks (EqEngine& e, const ParamValues& pv, float* l, float* r, int n, int blockSize)
{
    for (int pos = 0; pos < n; pos += blockSize)
    {
        const int len = std::min (blockSize, n - pos);
        e.process (pv, l + pos, r != nullptr ? r + pos : nullptr, len);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------------------------------------

/** Amplitude of the component at omega (rad/sample), Hann-windowed lock-in over the whole buffer. */
inline double lockInAmplitude (const float* x, int n, double omega)
{
    double sr = 0.0, si = 0.0, sw = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / n);
        sr += w * x[i] * std::cos (omega * i);
        si += w * x[i] * std::sin (omega * i);
        sw += w;
    }
    return 2.0 * std::sqrt (sr * sr + si * si) / sw;
}

/** Steady state gain (dB) of the engine for the parameter set at a frequency. freq <= 0 measures DC, freq >=
    fs / 2 the Nyquist frequency (alternating signal). The signal is a sine of amplitude 0.5 on both channels. */
inline double measureGainDb (const ParamValues& pv, double fs, double freq, double settleSec = 0.4)
{
    auto e = makeEngine (fs);
    const bool dc = freq <= 0.0;
    const bool nyquist = freq >= 0.5 * fs;
    const double f = dc ? 0.0 : (nyquist ? 0.5 * fs : freq);
    const double omega = 2.0 * kPi * f / fs;

    const int settle = static_cast<int> (settleSec * fs);
    const double cycles = std::max (20.0, 0.1 * f);
    const int measure = dc || nyquist ? static_cast<int> (0.1 * fs) : static_cast<int> (std::ceil (cycles * fs / f));
    const int total = settle + measure;

    std::vector<float> l (static_cast<size_t> (total)), r;
    for (int i = 0; i < total; ++i)
    {
        if (dc)
            l[static_cast<size_t> (i)] = 0.5f;
        else if (nyquist)
            l[static_cast<size_t> (i)] = (i & 1) ? -0.5f : 0.5f;
        else
            l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (omega * i));
    }
    r = l;

    processBlocks (*e, pv, l.data(), r.data(), total, 480);

    const float* y = l.data() + settle;
    double amp;
    if (dc)
    {
        double sum = 0.0;
        for (int i = 0; i < measure; ++i)
            sum += y[i];
        amp = std::fabs (sum / measure);
    }
    else if (nyquist)
    {
        double sum = 0.0;
        for (int i = 0; i < measure; ++i)
            sum += ((settle + i) & 1) ? -y[i] : y[i];
        amp = std::fabs (sum / measure);
    }
    else
    {
        amp = lockInAmplitude (y, measure, omega);
    }
    return toDb (amp / 0.5);
}

/** Expected-vs-measured comparison with a floor: below -90 dB only "very small" is required. */
inline bool matchesDb (double measured, double expected, double tol)
{
    if (expected < -90.0)
        return measured < -75.0;
    return std::fabs (measured - expected) <= tol;
}

// ---------------------------------------------------------------------------------------------------------
// Independent reference model: analog prototype + bilinear transform with pre-warping at the corner.
//   s = j tan (omega / 2) / tan (omega0 / 2)
// ---------------------------------------------------------------------------------------------------------

namespace ref
{
using cd = std::complex<double>;

inline cd analogS (double fs, double f0, double omega)
{
    return cd (0.0, std::tan (0.5 * omega) / std::tan (kPi * f0 / fs));
}

inline int order (int slopeIndex)
{
    static const int table[6] = { 1, 2, 3, 4, 6, 8 };
    return table[slopeIndex];
}

/** Butterworth cut built from the analog poles (not from the closed form used by the core). */
inline cd cutH (bool highPass, int slopeIndex, double q12, cd s)
{
    const int n = order (slopeIndex);
    cd h (1.0, 0.0);
    for (int k = 1; k <= n; ++k)
    {
        const double theta = kPi * (2.0 * k + n - 1.0) / (2.0 * n);
        const cd p = std::polar (1.0, theta);
        if (p.imag() > 1.0e-9)
        {
            const double q = (n == 2) ? q12 : 1.0 / (-2.0 * p.real());
            const cd den = s * s + s / q + 1.0;
            h *= highPass ? (s * s) / den : cd (1.0, 0.0) / den;
        }
        else if (std::fabs (p.imag()) <= 1.0e-9)
        {
            h *= highPass ? s / (s + 1.0) : cd (1.0, 0.0) / (s + 1.0);
        }
    }
    return h;
}

inline cd bandH (BandType type, double fs, double f0, double gainDb, double q, int slopeIndex, double omega)
{
    const cd s = analogS (fs, f0, omega);
    const double A = std::pow (10.0, gainDb / 40.0);
    switch (type)
    {
        case BandType::bell:      return (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0);
        case BandType::lowShelf:  return A * (s * s + s * (std::sqrt (A) / q) + A) / (A * s * s + s * (std::sqrt (A) / q) + 1.0);
        case BandType::highShelf: return A * (A * s * s + s * (std::sqrt (A) / q) + 1.0) / (s * s + s * (std::sqrt (A) / q) + A);
        case BandType::notch:     return (s * s + 1.0) / (s * s + s / q + 1.0);
        case BandType::bandPass:  return (s / q) / (s * s + s / q + 1.0);
        case BandType::lowCut:    return cutH (true, slopeIndex, q, s);
        case BandType::highCut:   return cutH (false, slopeIndex, q, s);
    }
    return cd (1.0, 0.0);
}

/** Evaluation frequency in rad/sample, DC and Nyquist replaced by values next to them (tan (pi / 2) = inf). */
inline double omegaOf (double fs, double freq)
{
    const double w = 2.0 * kPi * freq / fs;
    return std::min (std::max (w, 1.0e-9), kPi - 1.0e-9);
}

inline double bandGainDb (BandType type, double fs, double f0, double gainDb, double q, int slopeIndex, double freq)
{
    return toDb (std::abs (bandH (type, fs, f0, gainDb, q, slopeIndex, omegaOf (fs, freq))));
}
} // namespace ref

inline const char* typeName (BandType t)
{
    switch (t)
    {
        case BandType::bell:      return "bell";
        case BandType::lowShelf:  return "lowShelf";
        case BandType::highShelf: return "highShelf";
        case BandType::lowCut:    return "lowCut";
        case BandType::highCut:   return "highCut";
        case BandType::notch:     return "notch";
        case BandType::bandPass:  return "bandPass";
    }
    return "?";
}

inline bool allFinite (const float* x, int n)
{
    for (int i = 0; i < n; ++i)
        if (!std::isfinite (x[i]))
            return false;
    return true;
}

inline bool anySubnormal (const float* x, int n)
{
    for (int i = 0; i < n; ++i)
        if (std::fpclassify (x[i]) == FP_SUBNORMAL)
            return true;
    return false;
}
} // namespace tu
