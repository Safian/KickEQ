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

#include "kickeq/fft.h"

#include <cmath>

namespace kickeq
{
bool RealFft::init (int size)
{
    n = 0;
    if (size < minSize || size > maxSize || (size & (size - 1)) != 0)
        return false;

    const int half = size / 2;
    const double pi = 3.14159265358979323846;

    twRe.assign (static_cast<size_t> (half) + 1, 0.0f);
    twIm.assign (static_cast<size_t> (half) + 1, 0.0f);
    for (int k = 0; k <= half; ++k)
    {
        const double a = -2.0 * pi * k / size;
        twRe[static_cast<size_t> (k)] = static_cast<float> (std::cos (a));
        twIm[static_cast<size_t> (k)] = static_cast<float> (std::sin (a));
    }

    bitReverse.assign (static_cast<size_t> (half), 0);
    int bits = 0;
    while ((1 << bits) < half)
        ++bits;
    for (int i = 0; i < half; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if ((i >> b) & 1)
                r |= 1 << (bits - 1 - b);
        bitReverse[static_cast<size_t> (i)] = r;
    }

    zRe.assign (static_cast<size_t> (half), 0.0f);
    zIm.assign (static_cast<size_t> (half), 0.0f);

    n = size;
    m = half;
    return true;
}

void RealFft::complexTransform() noexcept
{
    float* const zr = zRe.data();
    float* const zi = zIm.data();

    // radix-2 decimation in time; W_len^j = W_n^{j n / len}
    for (int len = 2; len <= m; len <<= 1)
    {
        const int halfLen = len / 2;
        const int step = n / len;
        for (int start = 0; start < m; start += len)
            for (int j = 0; j < halfLen; ++j)
            {
                const float wr = twRe[static_cast<size_t> (j * step)];
                const float wi = twIm[static_cast<size_t> (j * step)];
                const int a = start + j, b = a + halfLen;
                const float tr = zr[b] * wr - zi[b] * wi;
                const float ti = zr[b] * wi + zi[b] * wr;
                zr[b] = zr[a] - tr;
                zi[b] = zi[a] - ti;
                zr[a] += tr;
                zi[a] += ti;
            }
    }
}

void RealFft::forward (const float* in, float* re, float* im) noexcept
{
    float* const zr = zRe.data();
    float* const zi = zIm.data();

    // pack even / odd samples as one complex sequence (bit reversed for the in-place transform)
    for (int i = 0; i < m; ++i)
    {
        const int j = bitReverse[static_cast<size_t> (i)];
        zr[j] = in[2 * i];
        zi[j] = in[2 * i + 1];
    }

    complexTransform();

    // split: X[k] = E[k] + W_n^k O[k] with E = (Z[k] + conj Z[m-k]) / 2, O = (Z[k] - conj Z[m-k]) / (2 i)
    for (int k = 0; k <= m; ++k)
    {
        const int ka = k == m ? 0 : k;
        const int kb = k == 0 ? 0 : m - k;
        const float ar = zr[ka], ai = zi[ka];
        const float br = zr[kb], bi = -zi[kb]; // conj Z[m-k]

        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        const float orr = di, oi = -dr; // D / i

        const float wr = twRe[static_cast<size_t> (k)], wi = twIm[static_cast<size_t> (k)];
        re[k] = er + (orr * wr - oi * wi);
        im[k] = ei + (orr * wi + oi * wr);
    }
}

void RealFft::inverse (const float* re, const float* im, float* out) noexcept
{
    float* const zr = zRe.data();
    float* const zi = zIm.data();

    // Z[k] = E[k] + i O[k] with E[k] = (X[k] + X[k+m]) / 2, O[k] = (X[k] - X[k+m]) W_n^-k / 2 and, for a real
    // signal, X[k+m] = conj X[m-k]. The sequence z = IDFT_m (Z) holds the even samples in its real and the odd
    // samples in its imaginary part. IDFT_m (Z) = conj (DFT_m (conj Z)) / m: conj Z goes in bit reversed order into
    // the forward transform.
    for (int k = 0; k < m; ++k)
    {
        const float ar = re[k], ai = im[k];
        const float br = re[m - k], bi = -im[m - k]; // X[k+m] = conj X[m-k]
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);

        const float wr = twRe[static_cast<size_t> (k)], wi = -twIm[static_cast<size_t> (k)]; // W^-k
        const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;

        // Z = E + i O
        const float zre = er - oi, zim = ei + orr;
        const int j = bitReverse[static_cast<size_t> (k)];
        zr[j] = zre;
        zi[j] = -zim; // conj
    }

    complexTransform();

    const float scale = 1.0f / static_cast<float> (m);
    for (int j = 0; j < m; ++j)
    {
        out[2 * j] = zr[j] * scale;
        out[2 * j + 1] = -zi[j] * scale;
    }
}
} // namespace kickeq
