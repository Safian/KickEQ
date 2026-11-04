/*
 * KickEQ - stereo 4-band parametric EQ (VST3 + CLAP)
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

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace kickeq
{
enum class BandType
{
    lowShelf,
    peak,
    highShelf
};

/** Normalised biquad coefficients (a0 == 1), RBJ "Audio EQ Cookbook" formulas. */
struct BiquadCoefficients
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static BiquadCoefficients make (BandType type, double sampleRate, double freqHz, double gainDb, double q) noexcept
    {
        constexpr double pi = 3.14159265358979323846;

        const double f  = std::clamp (freqHz, 10.0, 0.49 * sampleRate);
        const double Q  = std::max (q, 0.05);
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * pi * f / sampleRate;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double alpha = sw / (2.0 * Q);

        double b0, b1, b2, a0, a1, a2;

        switch (type)
        {
            case BandType::lowShelf:
            {
                const double t = 2.0 * std::sqrt (A) * alpha;
                b0 =        A * ((A + 1.0) - (A - 1.0) * cw + t);
                b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
                b2 =        A * ((A + 1.0) - (A - 1.0) * cw - t);
                a0 =             (A + 1.0) + (A - 1.0) * cw + t;
                a1 = -2.0     * ((A - 1.0) + (A + 1.0) * cw);
                a2 =             (A + 1.0) + (A - 1.0) * cw - t;
                break;
            }
            case BandType::highShelf:
            {
                const double t = 2.0 * std::sqrt (A) * alpha;
                b0 =         A * ((A + 1.0) + (A - 1.0) * cw + t);
                b1 = -2.0 *  A * ((A - 1.0) + (A + 1.0) * cw);
                b2 =         A * ((A + 1.0) + (A - 1.0) * cw - t);
                a0 =              (A + 1.0) - (A - 1.0) * cw + t;
                a1 =  2.0     * ((A - 1.0) - (A + 1.0) * cw);
                a2 =              (A + 1.0) - (A - 1.0) * cw - t;
                break;
            }
            case BandType::peak:
            default:
            {
                b0 = 1.0 + alpha * A;
                b1 = -2.0 * cw;
                b2 = 1.0 - alpha * A;
                a0 = 1.0 + alpha / A;
                a1 = -2.0 * cw;
                a2 = 1.0 - alpha / A;
                break;
            }
        }

        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }
};

/** Stereo biquad (transposed direct form II, double precision state). */
class StereoBiquad
{
public:
    void setCoefficients (const BiquadCoefficients& c) noexcept { coeffs = c; }

    void reset() noexcept
    {
        for (auto& s : state)
            s = { 0.0, 0.0 };
    }

    void process (int channel, float* data, int numSamples) noexcept
    {
        auto& s = state[static_cast<size_t> (channel)];
        double z1 = s[0], z2 = s[1];

        for (int i = 0; i < numSamples; ++i)
        {
            const double x = data[i];
            const double y = coeffs.b0 * x + z1;
            z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
            z2 = coeffs.b2 * x - coeffs.a2 * y;
            data[i] = static_cast<float> (y);
        }

        // flush denormals in the state
        s[0] = std::abs (z1) < 1.0e-30 ? 0.0 : z1;
        s[1] = std::abs (z2) < 1.0e-30 ? 0.0 : z2;
    }

private:
    BiquadCoefficients coeffs;
    std::array<std::array<double, 2>, 2> state {};
};
} // namespace kickeq
