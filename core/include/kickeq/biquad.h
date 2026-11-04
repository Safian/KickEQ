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

// Filter design (ARCHITECTURE.md section 4.1) and the filter structure (section 4.2).
//
// NOTE: this header defines kickeq::BandType and kickeq::BiquadCoefficients like the 0.1.0 spike header
// Source/Biquad.h does. The two must not be included in the same translation unit; the JUCE shell drops
// Source/Biquad.h when it is migrated onto the core. The numbers of Bell and the shelves are identical.

#pragma once

#include <array>
#include <cmath>

namespace kickeq
{
enum class BandType : int
{
    bell,
    lowShelf,
    highShelf,
    lowCut,
    highCut,
    notch,
    bandPass
};

constexpr int numBandTypes = 7;
constexpr int numSlopes = 6;

/** Slope enum index (0..5) -> dB per octave: 6, 12, 18, 24, 36, 48. */
constexpr int slopeDbPerOctave (int slopeIndex) noexcept
{
    constexpr int table[numSlopes] = { 6, 12, 18, 24, 36, 48 };
    return table[slopeIndex < 0 ? 0 : (slopeIndex >= numSlopes ? numSlopes - 1 : slopeIndex)];
}

/** Butterworth order of a cut slope (slope / 6 dB per octave). */
constexpr int slopeOrder (int slopeIndex) noexcept { return slopeDbPerOctave (slopeIndex) / 6; }

constexpr bool isCutType (BandType t) noexcept { return t == BandType::lowCut || t == BandType::highCut; }

/** Normalised biquad coefficients (a0 == 1). First-order sections have b2 == a2 == 0. */
struct BiquadCoefficients
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

// Design limits applied inside the design functions, independently of the parameter ranges (section 4.1).
constexpr double designMinFreqHz = 10.0;
constexpr double designMaxFreqRatio = 0.49; // x sample rate
constexpr double designMinQ = 0.05;
constexpr double designMaxQ = 20.0;
constexpr double designMaxGainDb = 48.0;

constexpr int maxSections = 4; // 48 dB/oct cut = 4 biquads

namespace detail
{
constexpr double pi = 3.14159265358979323846;

inline double clampDouble (double v, double lo, double hi) noexcept
{
    // NaN falls through to lo: the design must never produce NaN coefficients
    return v > lo ? (v < hi ? v : hi) : lo;
}

struct DesignCommon
{
    double A;     // 10^(gain / 40)
    double w0;    // centre / corner frequency in rad/sample
    double cw, sw;
    double alpha; // sin (w0) / (2 Q)
    double Q;
};

inline DesignCommon makeCommon (double sampleRate, double freqHz, double gainDb, double q) noexcept
{
    const double fMax = designMaxFreqRatio * sampleRate;
    const double fMin = designMinFreqHz < fMax ? designMinFreqHz : fMax;
    const double f = clampDouble (freqHz, fMin, fMax);

    DesignCommon c;
    c.Q = clampDouble (q, designMinQ, designMaxQ);
    c.A = std::pow (10.0, clampDouble (gainDb, -designMaxGainDb, designMaxGainDb) / 40.0);
    c.w0 = 2.0 * pi * f / sampleRate;
    c.cw = std::cos (c.w0);
    c.sw = std::sin (c.w0);
    c.alpha = c.sw / (2.0 * c.Q);
    return c;
}

inline BiquadCoefficients normalise (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    BiquadCoefficients r;
    r.b0 = b0 / a0;
    r.b1 = b1 / a0;
    r.b2 = b2 / a0;
    r.a1 = a1 / a0;
    r.a2 = a2 / a0;
    return r;
}
} // namespace detail

// ---------------------------------------------------------------------------------------------------------
// Single sections. All take the sample rate, the (clamped) corner/centre frequency and Q. RBJ "Audio EQ
// Cookbook" formulas; the centre frequency is exact (bilinear transform with w0 = 2 pi f / fs).
// ---------------------------------------------------------------------------------------------------------

inline BiquadCoefficients designBell (double sampleRate, double freqHz, double gainDb, double q) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, gainDb, q);
    return detail::normalise (1.0 + c.alpha * c.A, -2.0 * c.cw, 1.0 - c.alpha * c.A,
                              1.0 + c.alpha / c.A, -2.0 * c.cw, 1.0 - c.alpha / c.A);
}

/** Gain at the corner frequency is half the design gain in dB (the shelf mid point). */
inline BiquadCoefficients designLowShelf (double sampleRate, double freqHz, double gainDb, double q) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, gainDb, q);
    const double A = c.A, cw = c.cw;
    const double t = 2.0 * std::sqrt (A) * c.alpha;
    return detail::normalise (       A * ((A + 1.0) - (A - 1.0) * cw + t),
                              2.0  * A * ((A - 1.0) - (A + 1.0) * cw),
                                     A * ((A + 1.0) - (A - 1.0) * cw - t),
                                          (A + 1.0) + (A - 1.0) * cw + t,
                              -2.0     * ((A - 1.0) + (A + 1.0) * cw),
                                          (A + 1.0) + (A - 1.0) * cw - t);
}

inline BiquadCoefficients designHighShelf (double sampleRate, double freqHz, double gainDb, double q) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, gainDb, q);
    const double A = c.A, cw = c.cw;
    const double t = 2.0 * std::sqrt (A) * c.alpha;
    return detail::normalise (        A * ((A + 1.0) + (A - 1.0) * cw + t),
                              -2.0 *  A * ((A - 1.0) + (A + 1.0) * cw),
                                      A * ((A + 1.0) + (A - 1.0) * cw - t),
                                           (A + 1.0) - (A - 1.0) * cw + t,
                              2.0      * ((A - 1.0) - (A + 1.0) * cw),
                                           (A + 1.0) - (A - 1.0) * cw - t);
}

inline BiquadCoefficients designNotch (double sampleRate, double freqHz, double q) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, 0.0, q);
    return detail::normalise (1.0, -2.0 * c.cw, 1.0, 1.0 + c.alpha, -2.0 * c.cw, 1.0 - c.alpha);
}

/** Constant 0 dB peak gain band pass. */
inline BiquadCoefficients designBandPass (double sampleRate, double freqHz, double q) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, 0.0, q);
    return detail::normalise (c.alpha, 0.0, -c.alpha, 1.0 + c.alpha, -2.0 * c.cw, 1.0 - c.alpha);
}

/** Second-order high pass (the section of a low cut). Gain at the corner is Q. */
inline BiquadCoefficients designHighPass2 (double sampleRate, double freqHz, double q) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, 0.0, q);
    return detail::normalise ((1.0 + c.cw) * 0.5, -(1.0 + c.cw), (1.0 + c.cw) * 0.5,
                              1.0 + c.alpha, -2.0 * c.cw, 1.0 - c.alpha);
}

/** Second-order low pass (the section of a high cut). Gain at the corner is Q. */
inline BiquadCoefficients designLowPass2 (double sampleRate, double freqHz, double q) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, 0.0, q);
    return detail::normalise ((1.0 - c.cw) * 0.5, 1.0 - c.cw, (1.0 - c.cw) * 0.5,
                              1.0 + c.alpha, -2.0 * c.cw, 1.0 - c.alpha);
}

/** First-order high pass, -3.01 dB at the (pre-warped) corner. */
inline BiquadCoefficients designHighPass1 (double sampleRate, double freqHz) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, 0.0, 1.0);
    const double K = std::tan (0.5 * c.w0);
    BiquadCoefficients r;
    r.b0 = 1.0 / (1.0 + K);
    r.b1 = -r.b0;
    r.a1 = (K - 1.0) / (K + 1.0);
    return r;
}

/** First-order low pass, -3.01 dB at the (pre-warped) corner. */
inline BiquadCoefficients designLowPass1 (double sampleRate, double freqHz) noexcept
{
    const auto c = detail::makeCommon (sampleRate, freqHz, 0.0, 1.0);
    const double K = std::tan (0.5 * c.w0);
    BiquadCoefficients r;
    r.b0 = K / (1.0 + K);
    r.b1 = r.b0;
    r.a1 = (K - 1.0) / (K + 1.0);
    return r;
}

/** Q of the k-th (1-based) complex pole pair of an n-th order Butterworth filter:
    Q_k = 1 / (2 sin ((2k - 1) pi / (2n))), k = 1 .. floor (n / 2).
    For even n this equals the 1 / (2 cos ((2k - 1) pi / (2n))) of the spec (same set of values); the sine form
    is also correct for odd n (n = 3: Q = 1), where the cosine form of the spec is not. */
inline double butterworthQ (int order, int pairIndex) noexcept
{
    return 1.0 / (2.0 * std::sin ((2.0 * pairIndex - 1.0) * detail::pi / (2.0 * order)));
}

// ---------------------------------------------------------------------------------------------------------
// A complete band: up to four cascaded sections.
// ---------------------------------------------------------------------------------------------------------

struct BandDesign
{
    std::array<BiquadCoefficients, maxSections> sections {};
    int numSections = 0;
};

/** Designs one band. gainDb is ignored by cuts, notch and band pass. slopeIndex (0..5) is only used by cuts.
    For the 12 dB/oct cut the Q parameter sets the resonance of the section; for all other slopes Q is
    ignored and the sections are a Butterworth cascade (first-order section first when the order is odd). */
inline BandDesign designBand (BandType type, double sampleRate, double freqHz, double gainDb, double q,
                              int slopeIndex) noexcept
{
    BandDesign d;
    switch (type)
    {
        case BandType::lowShelf:  d.sections[0] = designLowShelf (sampleRate, freqHz, gainDb, q);  d.numSections = 1; break;
        case BandType::highShelf: d.sections[0] = designHighShelf (sampleRate, freqHz, gainDb, q); d.numSections = 1; break;
        case BandType::notch:     d.sections[0] = designNotch (sampleRate, freqHz, q);             d.numSections = 1; break;
        case BandType::bandPass:  d.sections[0] = designBandPass (sampleRate, freqHz, q);          d.numSections = 1; break;
        case BandType::lowCut:
        case BandType::highCut:
        {
            const bool high = (type == BandType::lowCut); // a low cut is a high pass
            const int order = slopeOrder (slopeIndex);
            int n = 0;

            if ((order & 1) != 0)
                d.sections[static_cast<size_t> (n++)] = high ? designHighPass1 (sampleRate, freqHz)
                                                             : designLowPass1 (sampleRate, freqHz);

            for (int k = 1; k <= order / 2; ++k)
            {
                const double sq = (order == 2) ? q : butterworthQ (order, k);
                d.sections[static_cast<size_t> (n++)] = high ? designHighPass2 (sampleRate, freqHz, sq)
                                                             : designLowPass2 (sampleRate, freqHz, sq);
            }

            d.numSections = n;
            break;
        }
        case BandType::bell:
        default:
            d.sections[0] = designBell (sampleRate, freqHz, gainDb, q);
            d.numSections = 1;
            break;
    }
    return d;
}

// ---------------------------------------------------------------------------------------------------------
// Filter structure: transposed direct form II, double state, per channel.
// ---------------------------------------------------------------------------------------------------------

class BandFilter
{
public:
    static constexpr int maxChannels = 2;

    BandFilter() noexcept { reset(); }

    /** Replaces the coefficients. The state of sections that exist before and after is kept; the state of
        sections that are new (or that disappear) is zeroed. */
    void setDesign (const BandDesign& d) noexcept
    {
        for (int s = count < d.numSections ? count : d.numSections; s < maxSections; ++s)
            for (int ch = 0; ch < maxChannels; ++ch)
                z[ch][s][0] = z[ch][s][1] = 0.0;

        sections = d.sections;
        count = d.numSections;
    }

    void reset() noexcept
    {
        for (auto& ch : z)
            for (auto& s : ch)
                s[0] = s[1] = 0.0;
    }

    int numSections() const noexcept { return count; }

    /** In-place processing of one channel. State below 1e-30 is flushed to zero after every sample, so the
        result does not depend on how a stream is split into calls. */
    void process (int channel, double* data, int numSamples) noexcept
    {
        for (int s = 0; s < count; ++s)
        {
            const auto& c = sections[static_cast<size_t> (s)];
            double z1 = z[channel][s][0], z2 = z[channel][s][1];

            for (int i = 0; i < numSamples; ++i)
            {
                const double x = data[i];
                const double y = c.b0 * x + z1;
                z1 = c.b1 * x - c.a1 * y + z2;
                z2 = c.b2 * x - c.a2 * y;
                z1 = std::fabs (z1) < 1.0e-30 ? 0.0 : z1;
                z2 = std::fabs (z2) < 1.0e-30 ? 0.0 : z2;
                data[i] = y;
            }

            z[channel][s][0] = z1;
            z[channel][s][1] = z2;
        }
    }

    /** Float convenience overload (the cascade itself runs in double). */
    void process (int channel, float* data, int numSamples) noexcept
    {
        double tmp[64];
        for (int pos = 0; pos < numSamples; pos += 64)
        {
            const int n = numSamples - pos < 64 ? numSamples - pos : 64;
            for (int i = 0; i < n; ++i)
                tmp[i] = data[pos + i];
            process (channel, tmp, n);
            for (int i = 0; i < n; ++i)
                data[pos + i] = static_cast<float> (tmp[i]);
        }
    }

private:
    std::array<BiquadCoefficients, maxSections> sections {};
    int count = 0;
    double z[maxChannels][maxSections][2];
};
} // namespace kickeq
