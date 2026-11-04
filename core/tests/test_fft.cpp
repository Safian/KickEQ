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

// FFT against a naive DFT.

#include <complex>

#include "kickeq/fft.h"
#include "test_util.h"

using namespace tu;

namespace
{
std::complex<double> dftBin (const std::vector<float>& x, int k)
{
    const size_t n = x.size();
    double re = 0.0, im = 0.0;
    for (size_t j = 0; j < n; ++j)
    {
        // reduce the angle index first to keep the argument small and exact
        const size_t idx = (j * static_cast<size_t> (k)) % n;
        const double a = -2.0 * kPi * static_cast<double> (idx) / static_cast<double> (n);
        re += x[j] * std::cos (a);
        im += x[j] * std::sin (a);
    }
    return { re, im };
}
} // namespace

TEST (fft_rejects_invalid_sizes)
{
    kickeq::RealFft f;
    for (int bad : { -4, 0, 1, 2, 3, 5, 6, 100, 4095, 16385, 32768, 1 << 20 })
        CHECK (!f.init (bad));
    for (int good : { 4, 8, 16, 1024, 4096, 16384 })
    {
        CHECK (f.init (good));
        CHECK (f.size() == good);
        CHECK (f.numBins() == good / 2 + 1);
    }
}

TEST (fft_matches_naive_dft_for_all_power_of_two_sizes)
{
    Rng rng (4242);
    for (int n = 4; n <= 16384; n *= 2)
    {
        kickeq::RealFft fft;
        CHECK (fft.init (n));

        std::vector<float> x (static_cast<size_t> (n));
        for (auto& v : x)
            v = rng.uniform();

        std::vector<float> re (static_cast<size_t> (n / 2 + 1)), im (re.size());
        fft.forward (x.data(), re.data(), im.data());

        double maxMag = 0.0;
        for (size_t k = 0; k < re.size(); ++k)
            maxMag = std::max (maxMag, std::hypot (static_cast<double> (re[k]), static_cast<double> (im[k])));

        // all bins up to 2048; larger sizes: DC, Nyquist and 48 pseudo-random bins
        std::vector<int> bins;
        if (n <= 2048)
            for (int k = 0; k <= n / 2; ++k)
                bins.push_back (k);
        else
        {
            bins = { 0, 1, n / 4, n / 2 - 1, n / 2 };
            for (int i = 0; i < 48; ++i)
                bins.push_back (rng.integer (0, n / 2));
        }

        double worst = 0.0;
        for (int k : bins)
        {
            const auto ref = dftBin (x, k);
            worst = std::max (worst, std::hypot (ref.real() - re[static_cast<size_t> (k)], ref.imag() - im[static_cast<size_t> (k)]));
        }
        if (worst > 2.0e-5 * (maxMag + 1.0))
            std::printf ("    n=%d: worst error %.3g (max magnitude %.3g)\n", n, worst, maxMag);
        CHECK (worst <= 2.0e-5 * (maxMag + 1.0));

        // the DC and Nyquist bins of a real signal are real
        CHECK (im[0] == 0.0f || std::fabs (im[0]) < 1.0e-6f * static_cast<float> (maxMag + 1.0));
        CHECK (std::fabs (im[static_cast<size_t> (n / 2)]) < 1.0e-5f * static_cast<float> (maxMag + 1.0));
    }
}

TEST (fft_known_signals_and_parseval)
{
    const int n = 1024;
    kickeq::RealFft fft;
    CHECK (fft.init (n));
    std::vector<float> re (n / 2 + 1), im (n / 2 + 1);

    // impulse -> flat spectrum of 1
    std::vector<float> x (n, 0.0f);
    x[0] = 1.0f;
    fft.forward (x.data(), re.data(), im.data());
    for (size_t k = 0; k < re.size(); ++k)
    {
        CHECK_NEAR (re[k], 1.0, 1.0e-6);
        CHECK_NEAR (im[k], 0.0, 1.0e-6);
    }

    // cosine of bin 37 with amplitude 1 -> n / 2 in that bin only
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = static_cast<float> (std::cos (2.0 * kPi * 37.0 * i / n));
    fft.forward (x.data(), re.data(), im.data());
    for (int k = 0; k <= n / 2; ++k)
    {
        const double mag = std::hypot (static_cast<double> (re[static_cast<size_t> (k)]), static_cast<double> (im[static_cast<size_t> (k)]));
        CHECK_NEAR (mag, k == 37 ? n / 2.0 : 0.0, 2.0e-3);
    }

    // Parseval on noise: sum |x|^2 = (|X0|^2 + |X(n/2)|^2 + 2 sum |Xk|^2) / n
    Rng rng (7);
    double energy = 0.0;
    for (auto& v : x)
    {
        v = rng.uniform();
        energy += static_cast<double> (v) * v;
    }
    fft.forward (x.data(), re.data(), im.data());
    double spectral = 0.0;
    for (int k = 0; k <= n / 2; ++k)
    {
        const double p = static_cast<double> (re[static_cast<size_t> (k)]) * re[static_cast<size_t> (k)]
                         + static_cast<double> (im[static_cast<size_t> (k)]) * im[static_cast<size_t> (k)];
        spectral += (k == 0 || k == n / 2) ? p : 2.0 * p;
    }
    CHECK_NEAR (spectral / n, energy, 1.0e-4 * energy);
}
