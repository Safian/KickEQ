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

// Own radix-2 real FFT (ARCHITECTURE.md section 5.1): float, precomputed twiddles, no dependency.
// A real transform of size N is computed as a complex transform of size N / 2 plus one split pass.

#pragma once

#include <vector>

namespace kickeq
{
class RealFft
{
public:
    static constexpr int minSize = 4;
    static constexpr int maxSize = 16384;

    RealFft() = default;

    /** Allocates the tables for a power-of-two size in [minSize, maxSize]. Returns false (and leaves the
        object unusable) otherwise. Everything after init() is allocation free. */
    bool init (int size);

    int size() const noexcept { return n; }
    int numBins() const noexcept { return n / 2 + 1; }

    /** Forward transform of n real samples into n / 2 + 1 complex bins (X[k] = sum x[j] e^{-2 pi i j k / n}).
        in, re and im must not alias each other. */
    void forward (const float* in, float* re, float* im) noexcept;

private:
    int n = 0;
    int m = 0; // n / 2, size of the complex transform
    std::vector<float> twRe, twIm; // e^{-2 pi i k / n}, k = 0 .. n / 2
    std::vector<int> bitReverse;   // permutation for the size m transform
    std::vector<float> zRe, zIm;   // work buffers
};
} // namespace kickeq
