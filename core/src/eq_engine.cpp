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

#include "kickeq/eq_engine.h"

#include <cfloat>
#include <cmath>
#include <memory>
#include <new>

#include "kickeq/eq_curve.h"

namespace kickeq
{
using detail::BandState;
using detail::Voice;

namespace detail
{
#if KICKEQ_LINEAR_PHASE
/** Everything the linear-phase mode needs, in one heap block. */
struct LinearStage
{
    PartitionedConvolver convolver;
    DesignLink link;
    std::unique_ptr<LinearPhaseDesigner> designer; // only created by designSynchronously
    uint64_t synchronouslyDesigned = 0;
    float wet[2][EqEngine::segmentLength] = {};
    float dry[2][EqEngine::segmentLength] = {};
};
#else
struct LinearStage
{
};
#endif
} // namespace detail

namespace
{
constexpr double smoothSeconds = 0.030; // parameter smoothing (section 4.3)
constexpr double fadeSeconds = 0.020;   // band enable / type change / bypass cross-fade
constexpr double maxTailSeconds = 10.0;

double log2Hz (double f) noexcept { return std::log2 (f > 1.0e-3 ? f : 1.0e-3); }
double dbToLinear (double db) noexcept { return std::pow (10.0, db / 20.0); }

/** NaN / Inf -> 0, denormals -> 0; everything else (including -0) untouched. */
void sanitise (float* data, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x = data[i];
        const float a = std::fabs (x);
        if (!(a <= FLT_MAX) || (a < FLT_MIN && a != 0.0f))
            data[i] = 0.0f;
    }
}

/** Double -> float output with the last line of defence: non-finite / absurd values are replaced by 0
    (and reported), denormals are flushed. */
float toOutput (double v, bool& fault) noexcept
{
    if (!(std::fabs (v) < 1.0e30))
    {
        fault = true;
        return 0.0f;
    }
    const float f = static_cast<float> (v);
    return (f != 0.0f && std::fabs (f) < FLT_MIN) ? 0.0f : f;
}

int normalisedSlope (const BandParams& p) noexcept { return isCutType (p.type) ? p.slopeIndex : 0; }

#if KICKEQ_LINEAR_PHASE
/** True when a band that is on in either snapshot differs between them (what changes the response). */
bool bandsDiffer (const ParamValues& a, const ParamValues& b) noexcept
{
    for (int i = 0; i < numBands; ++i)
    {
        if (a.get (i, BandParam::on) < 0.5f && b.get (i, BandParam::on) < 0.5f)
            continue;
        for (int k = 0; k < paramsPerBand; ++k)
            if (a.get (numGlobalParams + i * paramsPerBand + k) != b.get (numGlobalParams + i * paramsPerBand + k))
                return true;
    }
    return false;
}
#endif
} // namespace

EqEngine::EqEngine() noexcept { prepare (48000.0, 512); }

EqEngine::~EqEngine() = default;

int EqEngine::latencyForMode (int mode) const noexcept
{
    return linear != nullptr ? phaseModeLatency (mode) : 0;
}

void EqEngine::prepare (double newSampleRate, int maxBlock) noexcept
{
    (void) maxBlock;
    double sr = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    sr = sr < 1000.0 ? 1000.0 : (sr > 1.0e7 ? 1.0e7 : sr);
    sampleRate = sr;

    const double smooth = std::floor (smoothSeconds * sr + 0.5);
    smoothSamples = smooth < 1.0 ? 1u : static_cast<uint32_t> (smooth);
    const double fade = std::floor (fadeSeconds * sr + 0.5);
    fadeSamples = fade < 1.0 ? 1 : static_cast<int> (fade);
    fadeInverse = 1.0 / static_cast<double> (fadeSamples);

#if KICKEQ_LINEAR_PHASE
    if (linear == nullptr)
        linear.reset (new (std::nothrow) detail::LinearStage());
#endif

    reset();
}

void EqEngine::reset() noexcept
{
    for (auto& b : bands)
        b = BandState();

    outGain = detail::Smoother();
    autoGain = detail::Smoother();
    autoGainDb = 0.0;
    autoGainValid = false;
    gainDirty = false;
    gainA = gainB = 1.0;
    gainRamp = false;
    bypassMix = bypassFrom = bypassTarget = 1.0;
    bypassDir = 1.0;
    bypassSamples = 0;
    bypassActive = false;
    sampleCount = 0;
    initialised = false;
    dspActive = false;
    tailSamples = 0;
    lastTarget.setDefaults();
    lastRequested.setDefaults();
    path = Path::minimum;
    stageMode = 0;
    mixPos = 0;
    warmSamples = 0;
    targetMode.store (0, std::memory_order_relaxed);
#if KICKEQ_LINEAR_PHASE
    if (linear != nullptr)
    {
        linear->convolver.clear();
        linear->link.kernels.dropCurrent();
    }
#endif
}

// ---------------------------------------------------------------------------------------------------------
// Control side
// ---------------------------------------------------------------------------------------------------------

void EqEngine::updateControl (const ParamValues& target) noexcept
{
#if KICKEQ_LINEAR_PHASE
    if (linear != nullptr)
    {
        const int mode = target.phaseMode();
        targetMode.store (mode, std::memory_order_relaxed);

        if (!initialised && mode != 0)
        {
            initLinear (target, mode);
            return;
        }

        if (initialised)
        {
            if (path == Path::minimum && mode != 0)
                beginMinToLinear (target, mode);
            else if (path == Path::linear && mode != stageMode)
                beginLinearToMin (target);

            if (path == Path::linear)
            {
                updateControlLinear (target);
                return;
            }
            if (path != Path::minimum)
                requestDesignIfChanged (target); // the kernels keep following the parameters during a transition
        }
    }
#endif
    updateControlMin (target);
}

void EqEngine::updateControlMin (const ParamValues& target) noexcept
{
    const bool wantBypass = target.bypass();

    if (!initialised)
    {
        initialised = true;
        if (wantBypass)
        {
            dspActive = false;
            bypassMix = bypassFrom = bypassTarget = 0.0;
            bypassActive = false;
            lastTarget = target;
            tailSamples = 0;
        }
        else
        {
            snapBuild (target);
            bypassMix = bypassFrom = bypassTarget = 1.0;
            bypassActive = false;
        }
        return;
    }

    if (!dspActive)
    {
        // bypassed at rest: nothing runs, the state is rebuilt from the target when the bypass is released
        lastTarget = target;
        if (!wantBypass)
        {
            snapBuild (target);
            bypassMix = 0.0;
            startBypassRamp (1.0);
        }
        return;
    }

    if (wantBypass != (bypassTarget == 0.0))
        startBypassRamp (wantBypass ? 0.0 : 1.0);

    applyTargets (target);
}

void EqEngine::startBypassRamp (double to) noexcept
{
    const double now = bypassNow();
    bypassFrom = now;
    bypassMix = now;
    bypassTarget = to;
    bypassDir = to > now ? 1.0 : -1.0;
    bypassSamples = 0;
    bypassActive = (to != now);
}

double EqEngine::bypassNow() const noexcept
{
    if (!bypassActive)
        return bypassMix;
    const double v = bypassFrom + bypassDir * bypassSamples * fadeInverse;
    return bypassDir > 0.0 ? (v > bypassTarget ? bypassTarget : v) : (v < bypassTarget ? bypassTarget : v);
}

void EqEngine::dropAll() noexcept
{
    for (auto& b : bands)
    {
        for (auto& v : b.voices)
        {
            v.used = false;
            v.base = 0.0;
            v.filter.reset();
        }
        b.newest = -1;
        b.fading = false;
        b.fadeProgress = 0;
    }
}

void EqEngine::resetStates() noexcept
{
    for (auto& b : bands)
        for (auto& v : b.voices)
            v.filter.reset();
}

void EqEngine::designNewest (BandState& band) noexcept
{
    Voice& v = band.voices[band.newest];
    v.filter.setDesign (designBand (band.type, sampleRate, std::exp2 (band.freq.value), band.gain.value,
                                    std::exp2 (band.q.value), band.slope));
}

void EqEngine::snapBuild (const ParamValues& t) noexcept
{
    buildBands (t);
    snapGains (t);

    lastTarget = t;
    updateTail (t);
    dspActive = true;
}

void EqEngine::buildBands (const ParamValues& t) noexcept
{
    dropAll();

    for (int i = 0; i < numBands; ++i)
    {
        const BandParams p = t.band (i);
        BandState& b = bands[i];
        b.on = p.on;
        b.type = p.type;
        b.place = p.place;
        b.slope = normalisedSlope (p);
        b.freq.snap (log2Hz (p.freqHz));
        b.gain.snap (p.gainDb);
        b.q.snap (log2Hz (p.q));

        if (p.on)
        {
            Voice& v = b.voices[0];
            v.used = true;
            v.place = p.place;
            v.base = 1.0;
            b.newest = 0;
            designNewest (b);
        }
    }
}

void EqEngine::snapGains (const ParamValues& t) noexcept
{
    outGain.snap (t.outGainDb());
    if (t.autoGain())
    {
        autoGainDb = computeAutoGainDb (t, sampleRate);
        autoGainValid = true;
        autoGain.snap (-autoGainDb);
    }
    else
    {
        autoGainValid = false;
        autoGain.snap (0.0);
    }

    gainA = gainB = dbToLinear (outGain.value + autoGain.value);
    gainRamp = false;
    gainDirty = false;
}

void EqEngine::retargetBand (BandState& band, const BandParams& p, int slope) noexcept
{
    // Weights of the running cross-fade (r): newest = r, every other voice = base * (1 - r). They are frozen
    // into `base` so a new cross-fade starts exactly where the previous one stood (the weights keep summing
    // to 1 while the band is enabled), however often the structure changes.
    const double r = band.fading ? std::fmin (1.0, band.fadeProgress * fadeInverse) : 1.0;

    for (int i = 0; i < detail::maxVoices; ++i)
    {
        Voice& v = band.voices[i];
        if (!v.used)
            continue;
        v.base = (i == band.newest) ? r : v.base * (1.0 - r);
        if (v.base < 1.0e-9)
        {
            v.used = false;
            v.base = 0.0;
            v.filter.reset();
        }
    }
    band.newest = -1;

    if (p.on)
    {
        int slot = -1;
        for (int i = 0; i < detail::maxVoices && slot < 0; ++i)
            if (!band.voices[i].used)
                slot = i;
        if (slot < 0) // pathological: more than three overlapping changes within 20 ms; drop the quietest voice
        {
            slot = 0;
            for (int i = 1; i < detail::maxVoices; ++i)
                if (band.voices[i].base < band.voices[slot].base)
                    slot = i;
        }

        Voice& v = band.voices[slot];
        v.used = true;
        v.filter.reset();
        v.place = p.place;
        v.base = 0.0;
        band.newest = slot;
        band.type = p.type;
        band.place = p.place;
        band.slope = slope;
        designNewest (band);
        band.fading = true;
        band.fadeProgress = 0;
    }
    else
    {
        bool any = false;
        for (const auto& v : band.voices)
            any = any || v.used;
        band.fading = any;
        band.fadeProgress = 0;
    }
}

void EqEngine::applyTargets (const ParamValues& t) noexcept
{
    const uint64_t now = sampleCount;
    bool dirty = false;

    for (int i = 0; i < numBands; ++i)
    {
        const BandParams p = t.band (i);
        BandState& b = bands[i];
        const int slope = normalisedSlope (p);

        bool hasVoices = false;
        for (const auto& v : b.voices)
            hasVoices = hasVoices || v.used;

        const double lf = log2Hz (p.freqHz), lq = log2Hz (p.q);
        if (!hasVoices)
        {
            b.freq.snap (lf);
            b.gain.snap (p.gainDb);
            b.q.snap (lq);
        }
        else
        {
            b.freq.setTarget (lf, now, smoothSamples);
            b.gain.setTarget (p.gainDb, now, smoothSamples);
            b.q.setTarget (lq, now, smoothSamples);
        }

        const bool structural = (p.on != b.on) || (p.on && (p.type != b.type || p.place != b.place || slope != b.slope));
        if (structural)
            retargetBand (b, p, slope);
        if (!p.on)
        {
            b.type = p.type;
            b.place = p.place;
            b.slope = slope;
        }
        b.on = p.on;

        if (p.on || lastTarget.band (i).on)
            for (int k = 0; k < paramsPerBand; ++k)
                if (t.get (numGlobalParams + i * paramsPerBand + k) != lastTarget.get (numGlobalParams + i * paramsPerBand + k))
                    dirty = true;
    }

    applyGainTargets (t, dirty);

    if (dirty)
        updateTail (t);
    lastTarget = t;
}

void EqEngine::applyGainTargets (const ParamValues& t, bool dirty) noexcept
{
    const uint64_t now = sampleCount;
    outGain.setTarget (t.outGainDb(), now, smoothSamples);

    double autoTarget = 0.0;
    if (t.autoGain())
    {
        if (!autoGainValid || dirty)
        {
            autoGainDb = computeAutoGainDb (t, sampleRate);
            autoGainValid = true;
        }
        autoTarget = -autoGainDb;
    }
    else if (dirty)
    {
        autoGainValid = false;
    }
    autoGain.setTarget (autoTarget, now, smoothSamples);

    if (outGain.active || autoGain.active)
        gainDirty = true;
}

void EqEngine::updateTail (const ParamValues& t) noexcept
{
    double seconds = 0.0;
    bool any = false;

    for (int i = 0; i < numBands; ++i)
    {
        const BandParams p = t.band (i);
        if (!p.on)
            continue;
        any = true;

        const double fMax = designMaxFreqRatio * sampleRate;
        double f = p.freqHz;
        f = f < designMinFreqHz ? designMinFreqHz : (f > fMax ? fMax : f);
        double q = p.q < designMinQ ? designMinQ : (p.q > designMaxQ ? designMaxQ : p.q);

        switch (p.type)
        {
            case BandType::bell:
                q *= std::fmax (1.0, std::pow (10.0, p.gainDb / 40.0)); // boosted peaks ring longer
                break;
            case BandType::lowCut:
            case BandType::highCut:
            {
                const int order = slopeOrder (p.slopeIndex);
                q = order == 1 ? 0.5 : (order == 2 ? q : butterworthQ (order, 1));
                break;
            }
            default:
                break;
        }

        const double s = 13.8 * q / (detail::pi * f);
        seconds = s > seconds ? s : seconds;
    }

    // linear phase: the FIR adds its N samples (the delayed signal and the kernel's own length)
    const uint32_t firLength = linear != nullptr ? static_cast<uint32_t> (phaseModeLength (targetMode.load (std::memory_order_relaxed))) : 0u;

    if (!any)
    {
        tailSamples = firLength;
        return;
    }

    seconds = (seconds < maxTailSeconds ? seconds : maxTailSeconds) + fadeSeconds + smoothSeconds;
    tailSamples = static_cast<uint32_t> (std::ceil (seconds * sampleRate)) + firLength;
}

// ---------------------------------------------------------------------------------------------------------
// Audio side
// ---------------------------------------------------------------------------------------------------------

void EqEngine::tick (uint64_t time) noexcept
{
    tickBands (time);
    tickGains (time);
}

void EqEngine::tickBands (uint64_t time) noexcept
{
    for (auto& b : bands)
    {
        if (!(b.freq.active || b.gain.active || b.q.active))
            continue;
        b.freq.update (time, smoothSamples);
        b.gain.update (time, smoothSamples);
        b.q.update (time, smoothSamples);
        if (b.newest >= 0)
            designNewest (b);
    }
}

void EqEngine::tickGains (uint64_t time) noexcept
{
    if (gainDirty)
    {
        outGain.update (time, smoothSamples);
        autoGain.update (time, smoothSamples);
        const double dbA = outGain.value + autoGain.value;
        const double dbB = outGain.valueAt (time + segmentLength, smoothSamples)
                           + autoGain.valueAt (time + segmentLength, smoothSamples);
        gainA = dbToLinear (dbA);
        gainB = dbB == dbA ? gainA : dbToLinear (dbB);
        gainRamp = gainB != gainA;
        gainDirty = outGain.active || autoGain.active;
    }
}

void EqEngine::process (const ParamValues& target, float* left, float* right, int numSamples) noexcept
{
    updateControl (target);

    if (numSamples <= 0 || left == nullptr)
        return;

    sanitise (left, numSamples);
    if (right != nullptr)
        sanitise (right, numSamples);

    int pos = 0;
    while (pos < numSamples)
    {
        const uint32_t grid = static_cast<uint32_t> (sampleCount & (segmentLength - 1));
        const int seg = numSamples - pos < static_cast<int> (segmentLength - grid) ? numSamples - pos
                                                                                   : static_cast<int> (segmentLength - grid);
        if (path == Path::minimum)
        {
            if (dspActive)
            {
                if (grid == 0)
                    tick (sampleCount);
                processSegment (left + pos, right != nullptr ? right + pos : nullptr, seg, grid);
            }
        }
#if KICKEQ_LINEAR_PHASE
        else
        {
            if (grid == 0)
            {
                if (dspActive)
                    tickBands (sampleCount);
                tickGains (sampleCount);
            }
            processSegmentMixed (left + pos, right != nullptr ? right + pos : nullptr, seg, grid);
        }
#endif
        sampleCount += static_cast<uint64_t> (seg);
        pos += seg;
    }
}

void EqEngine::applyBand (int bandIndex, Placement place, int lane, int n) noexcept
{
    BandState& band = bands[bandIndex];

    int count = 0;
    int idx[detail::maxVoices];
    for (int i = 0; i < detail::maxVoices; ++i)
        if (band.voices[i].used && band.voices[i].place == place)
            idx[count++] = i;
    if (count == 0)
        return;

    double* data = work[lane];

    if (!band.fading && count == 1)
    {
        band.voices[idx[0]].filter.process (lane, data, n);
        return;
    }

    // cross-fade: out = x + sum_v e_v (y_v - x) with the weights e_v of the running fade
    for (int i = 0; i < n; ++i)
        dryLane[i] = data[i];

    for (int c = 0; c < count; ++c)
    {
        const int vi = idx[c];
        Voice& v = band.voices[vi];
        for (int i = 0; i < n; ++i)
            scratch[i] = dryLane[i];
        v.filter.process (lane, scratch, n);

        const double* r = fadeRamp[bandIndex];
        if (vi == band.newest)
            for (int i = 0; i < n; ++i)
                data[i] += r[i] * (scratch[i] - dryLane[i]);
        else
            for (int i = 0; i < n; ++i)
                data[i] += v.base * (1.0 - r[i]) * (scratch[i] - dryLane[i]);
    }
}

bool EqEngine::computeMinWet (const float* l, const float* r, int n, bool force) noexcept
{
    bool anyVoice = false, anyMidSide = false;
    for (int b = 0; b < numBands; ++b)
    {
        BandState& band = bands[b];
        for (const auto& v : band.voices)
            if (v.used)
            {
                anyVoice = true;
                anyMidSide = anyMidSide || v.place == Placement::mid || v.place == Placement::side;
            }
        if (band.fading)
            for (int i = 0; i < n; ++i)
            {
                const double x = (band.fadeProgress + i) * fadeInverse;
                fadeRamp[b][i] = x > 1.0 ? 1.0 : x;
            }
    }

    // nothing to do: the (sanitised) input already is the output, bit for bit
    if (!force && !anyVoice && !gainRamp && gainA == 1.0 && !bypassActive)
        return false;

    double* w0 = work[0];
    double* w1 = work[1];
    const bool mono = (r == nullptr);
    for (int i = 0; i < n; ++i)
    {
        w0[i] = l[i];
        w1[i] = mono ? l[i] : r[i];
    }

    // Stereo bands on both channels, then Left / Right bands, then Mid / Side bands in the M/S domain
    for (int b = 0; b < numBands; ++b)
    {
        applyBand (b, Placement::stereo, 0, n);
        applyBand (b, Placement::stereo, 1, n);
    }
    for (int b = 0; b < numBands; ++b)
    {
        applyBand (b, Placement::left, 0, n);
        applyBand (b, Placement::right, 1, n);
    }
    if (anyMidSide)
    {
        for (int i = 0; i < n; ++i)
        {
            const double m = 0.5 * (w0[i] + w1[i]);
            const double s = 0.5 * (w0[i] - w1[i]);
            w0[i] = m;
            w1[i] = s;
        }
        for (int b = 0; b < numBands; ++b)
        {
            applyBand (b, Placement::mid, 0, n);
            applyBand (b, Placement::side, 1, n);
        }
        for (int i = 0; i < n; ++i)
        {
            const double m = w0[i], s = w1[i];
            w0[i] = m + s;
            w1[i] = m - s;
        }
    }
    return true;
}

bool EqEngine::finishSegment (const double* dry0, const double* dry1, float* l, float* r, int n, uint32_t gridPos) noexcept
{
    // output gain (linear interpolation across the 32-sample cell), bypass cross-fade, conversion; the processed
    // signal is in work[], the unprocessed (possibly delayed) one in dry0 / dry1
    const double* w0 = work[0];
    const double* w1 = work[1];
    const bool mono = (r == nullptr);
    bool fault = false;
    for (int i = 0; i < n; ++i)
    {
        double y0 = w0[i], y1 = w1[i];

        if (gainRamp)
        {
            const double g = gainA + (gainB - gainA) * (static_cast<double> (gridPos + static_cast<uint32_t> (i)) * (1.0 / segmentLength));
            y0 *= g;
            y1 *= g;
        }
        else
        {
            y0 *= gainA;
            y1 *= gainA;
        }

        if (bypassActive)
        {
            double mix = bypassFrom + bypassDir * (bypassSamples + i) * fadeInverse;
            mix = bypassDir > 0.0 ? (mix > bypassTarget ? bypassTarget : mix) : (mix < bypassTarget ? bypassTarget : mix);
            const double d0 = dry0[i];
            const double d1 = mono ? d0 : dry1[i];
            y0 = d0 + mix * (y0 - d0);
            y1 = d1 + mix * (y1 - d1);
        }
        else if (bypassMix == 0.0)
        {
            // bypassed at rest while the linear-phase path runs (minimum phase stops processing instead): the
            // output is the dry signal, delayed by the latency like the processed one
            y0 = dry0[i];
            y1 = mono ? y0 : dry1[i];
        }

        l[i] = toOutput (y0, fault);
        if (!mono)
            r[i] = toOutput (y1, fault);
    }
    return fault;
}

void EqEngine::advanceMinFades (int n) noexcept
{
    for (int b = 0; b < numBands; ++b)
    {
        BandState& band = bands[b];
        if (!band.fading)
            continue;
        band.fadeProgress += n;
        if (band.fadeProgress >= fadeSamples)
        {
            for (int i = 0; i < detail::maxVoices; ++i)
            {
                Voice& v = band.voices[i];
                if (v.used && i != band.newest)
                {
                    v.used = false;
                    v.base = 0.0;
                    v.filter.reset();
                }
            }
            if (band.newest >= 0)
                band.voices[band.newest].base = 1.0;
            band.fading = false;
            band.fadeProgress = 0;
        }
    }
}

void EqEngine::advanceBypass (int n) noexcept
{
    if (!bypassActive)
        return;

    bypassSamples += n;
    const double now = bypassNow();
    if (now == bypassTarget)
    {
        bypassActive = false;
        bypassMix = bypassTarget;
        // bypassed at rest: stop the minimum-phase filters (the linear-phase path keeps running its delay line)
        if (bypassMix == 0.0 && path == Path::minimum)
        {
            dropAll();
            dspActive = false;
            tailSamples = 0;
        }
    }
}

void EqEngine::processSegment (float* l, float* r, int n, uint32_t gridPos) noexcept
{
    if (!computeMinWet (l, r, n, false))
        return;

    const bool mono = (r == nullptr);
    for (int i = 0; i < n; ++i)
    {
        dryBuf[0][i] = l[i];
        dryBuf[1][i] = mono ? l[i] : r[i];
    }

    if (finishSegment (dryBuf[0], dryBuf[1], l, r, n, gridPos))
        resetStates();

    advanceMinFades (n);
    advanceBypass (n);
}

// ---------------------------------------------------------------------------------------------------------
// Linear-phase path
// ---------------------------------------------------------------------------------------------------------

#if KICKEQ_LINEAR_PHASE
DesignLink* EqEngine::designLink() noexcept { return linear != nullptr ? &linear->link : nullptr; }

void EqEngine::requestDesign (const ParamValues& values) noexcept
{
    if (linear != nullptr)
        linear->link.request (values, sampleRate);
}

uint64_t EqEngine::paramSequence() const noexcept
{
    return linear != nullptr ? linear->link.requestSequence() : 0;
}

uint64_t EqEngine::appliedKernelSequence() const noexcept
{
    return linear != nullptr ? linear->link.kernels.appliedSequence() : 0;
}

void EqEngine::designSynchronously (const ParamValues& values)
{
    if (linear == nullptr)
        return;
    if (!linear->designer)
        linear->designer.reset (new (std::nothrow) LinearPhaseDesigner());
    if (!linear->designer)
        return;
    linear->link.request (values, sampleRate);
    linear->synchronouslyDesigned = serviceDesignRequest (linear->link, *linear->designer, linear->synchronouslyDesigned);
}

void EqEngine::postDesignRequest (const ParamValues& target) noexcept
{
    // the kernels must match the mode the convolver runs, not the one the target may already ask for
    ParamValues request = target;
    request.set (paramPhaseMode, static_cast<float> (stageMode));
    linear->link.request (request, sampleRate);
    lastRequested = request;
}

void EqEngine::requestDesignIfChanged (const ParamValues& target) noexcept
{
    if (bandsDiffer (lastRequested, target) || lastRequested.phaseMode() != stageMode)
        postDesignRequest (target);
}

void EqEngine::initLinear (const ParamValues& t, int mode) noexcept
{
    initialised = true;
    stageMode = mode;
    linear->link.kernels.dropCurrent();
    linear->convolver.start (mode, sampleRate);
    path = Path::linear;
    mixPos = 0;
    warmSamples = 0;

    bypassMix = bypassFrom = bypassTarget = t.bypass() ? 0.0 : 1.0;
    bypassActive = false;
    dspActive = false; // the minimum-phase filters do not run
    snapGains (t);
    lastTarget = t;
    updateTail (t);
    postDesignRequest (t);
}

void EqEngine::beginMinToLinear (const ParamValues& t, int mode) noexcept
{
    stageMode = mode;
    linear->link.kernels.dropCurrent();
    linear->convolver.start (mode, sampleRate);
    path = Path::minToLinear;
    mixPos = 0;
    warmSamples = phaseModeLatency (mode);
    postDesignRequest (t);
    updateTail (t);
}

void EqEngine::beginLinearToMin (const ParamValues& t) noexcept
{
    path = Path::linearToMin;
    mixPos = 0;
    warmSamples = 0;
    if (t.bypass() && bypassMix == 0.0 && !bypassActive)
    {
        dropAll();
        dspActive = false; // bypassed at rest: nothing to build
    }
    else
    {
        buildBands (t);
        dspActive = true;
    }
    lastTarget = t;
    updateTail (t);
}

void EqEngine::updateControlLinear (const ParamValues& t) noexcept
{
    const bool wantBypass = t.bypass();
    if (wantBypass != (bypassTarget == 0.0))
        startBypassRamp (wantBypass ? 0.0 : 1.0);

    const bool dirty = bandsDiffer (lastTarget, t);
    applyGainTargets (t, dirty);
    if (dirty)
        updateTail (t);
    requestDesignIfChanged (t);
    lastTarget = t;
}

double EqEngine::pathWeight (int offset) const noexcept
{
    switch (path)
    {
        case Path::linear:
            return 1.0;
        case Path::minToLinear:
        {
            const int t = mixPos + offset - warmSamples;
            if (t <= 0)
                return 0.0;
            const double w = t * fadeInverse;
            return w > 1.0 ? 1.0 : w;
        }
        case Path::linearToMin:
        {
            const double w = 1.0 - (mixPos + offset) * fadeInverse;
            return w < 0.0 ? 0.0 : w;
        }
        case Path::minimum:
        default:
            return 0.0;
    }
}

void EqEngine::finishTransition() noexcept
{
    if (path == Path::minToLinear && mixPos - warmSamples >= fadeSamples)
    {
        path = Path::linear;
        dropAll();
        dspActive = false;
    }
    else if (path == Path::linearToMin && mixPos >= fadeSamples)
    {
        path = Path::minimum;
        stageMode = 0;
        if (bypassMix == 0.0 && !bypassActive)
        {
            dropAll();
            dspActive = false; // bypassed at rest
        }
        updateTail (lastTarget);
    }
}

void EqEngine::processSegmentMixed (float* l, float* r, int n, uint32_t gridPos) noexcept
{
    detail::LinearStage& ls = *linear;
    const bool mono = (r == nullptr);
    const float* inR = mono ? l : r;

    ls.convolver.process (&ls.link.kernels, l, inR, ls.wet[0], ls.wet[1], ls.dry[0], ls.dry[1], n);

    double* w0 = work[0];
    double* w1 = work[1];

    if (path == Path::linear)
    {
        for (int i = 0; i < n; ++i)
        {
            w0[i] = ls.wet[0][i];
            w1[i] = ls.wet[1][i];
            dryBuf[0][i] = ls.dry[0][i];
            dryBuf[1][i] = ls.dry[1][i];
        }
    }
    else
    {
        // both paths run: out = (1 - m) minimum + m linear, for the processed and for the dry signal alike
        computeMinWet (l, r, n, true);
        for (int i = 0; i < n; ++i)
        {
            const double m = pathWeight (i);
            const double k = 1.0 - m;
            const double in1 = mono ? static_cast<double> (l[i]) : static_cast<double> (r[i]);
            w0[i] = k * w0[i] + m * ls.wet[0][i];
            w1[i] = k * w1[i] + m * ls.wet[1][i];
            dryBuf[0][i] = k * l[i] + m * ls.dry[0][i];
            dryBuf[1][i] = k * in1 + m * ls.dry[1][i];
        }
    }

    if (finishSegment (dryBuf[0], dryBuf[1], l, r, n, gridPos))
    {
        resetStates();
        ls.convolver.clear();
    }

    if (path != Path::linear)
        advanceMinFades (n);
    advanceBypass (n);

    mixPos += n;
    finishTransition();
}
#else
DesignLink* EqEngine::designLink() noexcept { return nullptr; }
void EqEngine::requestDesign (const ParamValues&) noexcept {}
uint64_t EqEngine::paramSequence() const noexcept { return 0; }
uint64_t EqEngine::appliedKernelSequence() const noexcept { return 0; }
void EqEngine::designSynchronously (const ParamValues&) {}
#endif
} // namespace kickeq
