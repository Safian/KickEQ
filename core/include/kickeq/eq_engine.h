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
// The minimum-phase path contains all its memory (no heap, no locks, no syscalls anywhere); instances share no
// state. With KICKEQ_LINEAR_PHASE the engine additionally owns one heap block (the linear-phase stage: convolver,
// kernel mailbox, ~1.5 MB), allocated once in prepare () and never again: switching the phase mode on the audio
// thread allocates nothing.
//
// Phase mode (ARCHITECTURE.md section 4.7): the mode comes with the parameters (phase_mode). Minimum keeps the
// biquad cascade; Linear 2048 / 4096 / 8192 routes the audio through a partitioned FFT convolution with kernels
// that a design thread computes from the same band parameters (see linear_phase.h, design_thread.h). The reported
// latency is N / 2 samples, exactly, for every host block size. A change of the mode while running is bridged by
// a 20 ms cross-fade between the two paths (the new path first warms up for its latency); the host is expected to
// re-query the latency and restart, the engine just keeps sounding sensible meanwhile.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "kickeq/biquad.h"
#include "kickeq/linear_phase.h"
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

struct LinearStage; // heap block of the linear-phase mode (eq_engine.cpp)
} // namespace detail

class EqEngine
{
public:
    /** Segments (and the scratch buffers) are at most this long; longer blocks are chunked internally. */
    static constexpr int segmentLength = 32;

    EqEngine() noexcept;
    ~EqEngine();

    EqEngine (const EqEngine&) = delete;
    EqEngine& operator= (const EqEngine&) = delete;

    /** Sets the sample rate (clamped to >= 1 kHz) and resets everything. maxBlock is accepted for shell
        symmetry; blocks of any size are processed. Not real-time safe in the sense that it may be slow; the first
        call allocates the linear-phase stage when it is built in (never again afterwards). Stop a DesignThread
        before calling it. */
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

    /** Latency in samples of the phase mode of the most recent target: 0 for minimum phase, N / 2 for linear
        phase (1024 / 2048 / 4096). Any thread. */
    int getLatencySamples() const noexcept { return latencyForMode (targetMode.load (std::memory_order_relaxed)); }

    /** Latency of a phase_mode value in this build (0 when the linear-phase mode is not compiled in or not
        allocated). Any thread; constant after prepare (). */
    int latencyForMode (int mode) const noexcept;

    /** True when the linear-phase mode can run (compiled in and its stage allocated). */
    bool linearPhaseAvailable() const noexcept { return linear != nullptr; }

    // ---- linear-phase design interface (nullptr / no-ops when linearPhaseAvailable () is false) ------------------

    /** The mailbox pair between this engine and a design thread (DesignThread::start takes it). Valid for the life
        of the engine; stop the design thread before prepare () or destruction. */
    DesignLink* designLink() noexcept;

    /** Posts a design request for `values` (single producer: the audio thread posts them itself while the linear
        phase is active; call this from another thread only while no audio thread runs). */
    void requestDesign (const ParamValues& values) noexcept;

    /** Number of design requests posted so far (0 = none). Any thread. */
    uint64_t paramSequence() const noexcept;

    /** Sequence number of the request the kernels in use were designed from (0 = none yet). Any thread. */
    uint64_t appliedKernelSequence() const noexcept;

    /** Reference single-thread design for tests and offline rendering: posts the request, designs it on the calling
        thread and publishes the kernels; the audio side picks them up at its next 128-sample block boundary.
        Allocates the designer on first use; not for the audio thread, and not while a DesignThread runs. */
    void designSynchronously (const ParamValues& values);

    /** Samples the output keeps ringing after the input stopped, for the current parameters
        (min (10 s, 13.8 Q / (pi f)) of the slowest enabled section, plus the fade times). 0 when bypassed. */
    uint32_t getTailSamples() const noexcept { return tailSamples; }

    double getSampleRate() const noexcept { return sampleRate; }

    /** The auto gain G in dB most recently computed (0 until auto gain was requested). */
    double getAutoGainDb() const noexcept { return autoGainDb; }

private:
    enum class Path : uint8_t { minimum, linear, minToLinear, linearToMin };

    void updateControl (const ParamValues& target) noexcept;
    void updateControlMin (const ParamValues& target) noexcept;
    void snapBuild (const ParamValues& target) noexcept;
    void buildBands (const ParamValues& target) noexcept;
    void snapGains (const ParamValues& target) noexcept;
    void applyTargets (const ParamValues& target) noexcept;
    void applyGainTargets (const ParamValues& target, bool dirty) noexcept;
    void retargetBand (detail::BandState& band, const BandParams& p, int slope) noexcept;
    void designNewest (detail::BandState& band) noexcept;
    void dropAll() noexcept;
    void resetStates() noexcept;
    void startBypassRamp (double to) noexcept;
    double bypassNow() const noexcept;
    void tick (uint64_t time) noexcept;
    void tickBands (uint64_t time) noexcept;
    void tickGains (uint64_t time) noexcept;
    void processSegment (float* left, float* right, int n, uint32_t gridPos) noexcept;
    bool computeMinWet (const float* left, const float* right, int n, bool force) noexcept;
    bool finishSegment (const double* dry0, const double* dry1, float* left, float* right, int n, uint32_t gridPos) noexcept;
    void advanceMinFades (int n) noexcept;
    void advanceBypass (int n) noexcept;

    // linear-phase path (eq_engine.cpp, compiled with KICKEQ_LINEAR_PHASE)
    void initLinear (const ParamValues& target, int mode) noexcept;
    void beginMinToLinear (const ParamValues& target, int mode) noexcept;
    void beginLinearToMin (const ParamValues& target) noexcept;
    void updateControlLinear (const ParamValues& target) noexcept;
    void postDesignRequest (const ParamValues& target) noexcept;
    void requestDesignIfChanged (const ParamValues& target) noexcept;
    void processSegmentMixed (float* left, float* right, int n, uint32_t gridPos) noexcept;
    double pathWeight (int offset) const noexcept;
    void finishTransition() noexcept;
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

    std::unique_ptr<detail::LinearStage> linear; // nullptr: linear phase unavailable
    std::atomic<int> targetMode { 0 };           // phase mode of the most recent target (0 = minimum)
    Path path = Path::minimum;
    int stageMode = 0;    // mode the convolver runs while path != minimum
    int mixPos = 0;       // samples since the start of the running path transition
    int warmSamples = 0;  // minToLinear: the new path first runs this long (its latency) before the fade starts
    ParamValues lastRequested;

    double work[2][segmentLength] = {};
    double dryBuf[2][segmentLength] = {};
    double scratch[segmentLength] = {};
    double dryLane[segmentLength] = {};
    double fadeRamp[numBands][segmentLength] = {};
};
} // namespace kickeq
