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

// The EQ engine (ARCHITECTURE.md sections 4.2 - 4.5).
//
// Time structure: the engine owns an absolute sample counter. Everything that is scheduled (30 ms parameter
// smoothing, coefficient updates every 32 samples while smoothing, 20 ms band / bypass cross-fades) is a
// function of that counter and never of the block boundaries, so the output only depends on the parameter
// targets and the position at which they are handed over (block-size independence). A new target takes
// effect at the start of the next 32-sample cell at the latest.
//
// The class contains all its memory (no heap, no locks, no syscalls anywhere); instances share no state.

#pragma once

#include <cstdint>

#include "kickeq/biquad.h"
#include "kickeq/params.h"

namespace kickeq
{
namespace detail
{
/** Linear ramp of a (transformed) value over `duration` samples, evaluated at absolute sample times. */
struct Smoother
{
    double start = 0.0, target = 0.0, value = 0.0;
    uint64_t startTime = 0;
    bool active = false;

    void snap (double v) noexcept
    {
        start = target = value = v;
        active = false;
    }

    double valueAt (uint64_t t, uint32_t duration) const noexcept
    {
        if (!active)
            return value;
        const uint64_t elapsed = t - startTime;
        if (elapsed >= duration)
            return target;
        return start + (target - start) * (static_cast<double> (elapsed) / static_cast<double> (duration));
    }

    void setTarget (double newTarget, uint64_t now, uint32_t duration) noexcept
    {
        if (newTarget == target)
            return;
        value = valueAt (now, duration);
        start = value;
        target = newTarget;
        startTime = now;
        active = true;
    }

    void update (uint64_t t, uint32_t duration) noexcept
    {
        if (!active)
            return;
        value = valueAt (t, duration);
        if (t - startTime >= duration)
            active = false;
    }
};

constexpr int maxVoices = 4; // one current + up to three fading-out filters per band

/** One filter instance of a band. A band normally owns one voice (weight 1); a type / slope / placement
    change or an enable / disable adds or retires voices that are cross-faded over 20 ms. */
struct Voice
{
    BandFilter filter;
    Placement place = Placement::stereo;
    double base = 0.0; // weight at the start of the running cross-fade (retiring voices)
    bool used = false;
};

struct BandState
{
    bool on = false;
    BandType type = BandType::bell;
    Placement place = Placement::stereo;
    int slope = 0; // slope index for cuts, 0 for other types (the slope is irrelevant there)
    Smoother freq, gain, q; // log2 (Hz), dB, log2 (Q)
    Voice voices[maxVoices];
    int newest = -1;       // index of the voice that follows the parameters, -1 when none
    int fadeProgress = 0;  // samples elapsed in the running cross-fade
    bool fading = false;
};
} // namespace detail

class EqEngine
{
public:
    /** Segments (and the scratch buffers) are at most this long; longer blocks are chunked internally. */
    static constexpr int segmentLength = 32;

    EqEngine() noexcept;

    /** Sets the sample rate (clamped to >= 1 kHz) and resets everything. maxBlock is accepted for shell
        symmetry; blocks of any size are processed. Not real-time safe in the sense that it may be slow, but
        it does not allocate. */
    void prepare (double sampleRate, int maxBlock) noexcept;

    /** Clears all filter state and cross-fades. The next process() call snaps to its target (no ramp). */
    void reset() noexcept;

    /** Processes numSamples (>= 0, any size) in place. `right` may be nullptr: the engine then runs
        dual-mono on `left`. The target is read once per call. */
    void process (const ParamValues& target, float* left, float* right, int numSamples) noexcept;

    /** Mono entry point: dual-mono (L = R = data), the left output is written back. */
    void processMono (const ParamValues& target, float* data, int numSamples) noexcept
    {
        process (target, data, nullptr, numSamples);
    }

    int getLatencySamples() const noexcept { return 0; }

    /** Samples the output keeps ringing after the input stopped, for the current parameters
        (min (10 s, 13.8 Q / (pi f)) of the slowest enabled section, plus the fade times). 0 when bypassed. */
    uint32_t getTailSamples() const noexcept { return tailSamples; }

    double getSampleRate() const noexcept { return sampleRate; }

    /** The auto gain G in dB most recently computed (0 until auto gain was requested). */
    double getAutoGainDb() const noexcept { return autoGainDb; }

private:
    void updateControl (const ParamValues& target) noexcept;
    void snapBuild (const ParamValues& target) noexcept;
    void applyTargets (const ParamValues& target) noexcept;
    void retargetBand (detail::BandState& band, const BandParams& p, int slope) noexcept;
    void designNewest (detail::BandState& band) noexcept;
    void dropAll() noexcept;
    void resetStates() noexcept;
    void startBypassRamp (double to) noexcept;
    double bypassNow() const noexcept;
    void tick (uint64_t time) noexcept;
    void processSegment (float* left, float* right, int n, uint32_t gridPos) noexcept;
    void applyBand (int bandIndex, Placement place, int lane, int n) noexcept;
    void updateTail (const ParamValues& target) noexcept;

    double sampleRate = 48000.0;
    uint32_t smoothSamples = 1440; // 30 ms
    int fadeSamples = 960;         // 20 ms
    double fadeInverse = 1.0 / 960.0;

    uint64_t sampleCount = 0;
    bool initialised = false;
    bool dspActive = false; // false while bypassed at rest: no state, plain pass through
    ParamValues lastTarget;

    detail::BandState bands[numBands];
    detail::Smoother outGain, autoGain; // dB
    double autoGainDb = 0.0;
    bool autoGainValid = false;
    bool gainDirty = false;
    double gainA = 1.0, gainB = 1.0; // linear output gain at the start / end of the current 32-sample cell
    bool gainRamp = false;

    double bypassMix = 1.0; // 1 = processed, 0 = dry
    double bypassFrom = 1.0, bypassTarget = 1.0, bypassDir = 1.0;
    int bypassSamples = 0;
    bool bypassActive = false;

    uint32_t tailSamples = 0;

    double work[2][segmentLength] = {};
    double scratch[segmentLength] = {};
    double dryLane[segmentLength] = {};
    double fadeRamp[numBands][segmentLength] = {};
};
} // namespace kickeq
