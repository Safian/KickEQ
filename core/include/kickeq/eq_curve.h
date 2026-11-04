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

// Analytic response of the whole cascade (ARCHITECTURE.md sections 4.4 - 4.6). Single source for the UI
// curve, the auto-gain and the unit tests. Uses exactly the same band design as the engine.

#pragma once

#include "kickeq/biquad.h"
#include "kickeq/params.h"

namespace kickeq
{
/** Which signal the curve describes (same fixed signal flow as the engine, section 4.4):

    stereo  response to a centre (L = R) signal, measured at the left output. With only Stereo bands this is
            the response of every channel; a Mid band affects it fully, a Side band not at all.
    left    L-in -> L-out (R silent)
    right   R-in -> R-out (L silent)
    mid     M-in -> M-out (L = R input)
    side    S-in -> S-out (L = -R input)

    Bypass, output gain and auto gain are not part of the curve. */
enum class ChannelView : int { stereo, left, right, mid, side };

struct Complex
{
    double re = 0.0, im = 0.0;
};

/** H (e^{j omega}) of one section. */
Complex sectionResponse (const BiquadCoefficients& c, double omega) noexcept;
/** Product over the sections of a band. */
Complex bandResponse (const BandDesign& d, double omega) noexcept;

/** Evaluates the response of the enabled bands of `params` at n frequencies (Hz). Frequencies above Nyquist
    are evaluated at Nyquist, frequencies <= 0 at DC. magDb (floor -300 dB) and phaseRad (-pi..pi) may be
    nullptr. Does not allocate. */
void computeResponse (const ParamValues& params, double sampleRate, const float* freqsHz, float* magDb,
                      float* phaseRad, int n, ChannelView view) noexcept;

/** Same for a single band regardless of its enable flag (for per-band curves in a UI). */
void computeBandResponse (const BandParams& band, double sampleRate, const float* freqsHz, float* magDb,
                          float* phaseRad, int n) noexcept;

constexpr int autoGainPoints = 96;
constexpr double autoGainMaxDb = 12.0;

/** Pink-noise weighted RMS gain G of the current response in dB (section 4.5):
    G = 10 log10 (sum w_i P (f_i) / sum w_i) over 96 log-spaced points 20 Hz .. 20 kHz (limited to Nyquist
    for sample rates below 40 kHz), equal weights (equal energy per octave), clamped to +-12 dB. P is the
    power gain for uncorrelated equal-power noise on L and R averaged over both outputs, which equals |H|^2
    when only Stereo bands are active. The engine applies -G when auto_gain is on. */
double computeAutoGainDb (const ParamValues& params, double sampleRate) noexcept;
} // namespace kickeq
