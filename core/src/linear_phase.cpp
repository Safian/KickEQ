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

#include "kickeq/linear_phase.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kickeq
{
using namespace linear;

namespace
{
constexpr double pi = 3.14159265358979323846;

// A kernel set whose every bin is within this distance of "pure delay" is flagged identity (-140 dB), the cross
// terms are dropped when they are below -180 dB.
constexpr double identityTolerance = 1.0e-7;
constexpr double crossTolerance = 1.0e-9;

uint32_t floatBits (float v) noexcept
{
    uint32_t b;
    std::memcpy (&b, &v, sizeof b);
    return b;
}

float bitsFloat (uint32_t b) noexcept
{
    float v;
    std::memcpy (&v, &b, sizeof v);
    return v;
}

/** Blackman window over 2c + 1 taps, evaluated at the offset d from the centre; exactly 0 at d = +-c. */
double blackman (int d, int c) noexcept
{
    if (d >= c || d <= -c)
        return 0.0;
    const double x = pi * d / c;
    return 0.42 + 0.5 * std::cos (x) + 0.08 * std::cos (2.0 * x);
}

/** max (a, b) as a plain comparison (std::fmax in this reduction loop makes GCC 12.2's vectoriser crash with an internal
    compiler error at -O3). */
double largest (double a, double b) noexcept { return b > a ? b : a; }

struct BandEval
{
    BandDesign design;
    int placement = 0; // index of Placement: stereo, left, right, mid, side
};
} // namespace

// ---------------------------------------------------------------------------------------------------------
// KernelSet / KernelMailbox / DesignLink
// ---------------------------------------------------------------------------------------------------------

KernelSet::KernelSet()
    : spectra (static_cast<size_t> (numKernels) * maxPartitions * partitionStride, 0.0f)
{
}

KernelMailbox::KernelMailbox()
    : slots (4), middle (1u), applied (0)
{
}

void KernelMailbox::publish() noexcept
{
    const uint32_t old = middle.exchange (static_cast<uint32_t> (writeIdx) | dirtyBit, std::memory_order_acq_rel);
    writeIdx = static_cast<int> (old & 3u);
}

bool KernelMailbox::fetch (int mode, double sampleRate) noexcept
{
    if ((middle.load (std::memory_order_acquire) & dirtyBit) == 0)
        return false;

    // give the spare to the mailbox, take what the writer published
    const uint32_t got = middle.exchange (static_cast<uint32_t> (spareIdx), std::memory_order_acq_rel);
    const int idx = static_cast<int> (got & 3u);
    const KernelSet& set = slots[static_cast<size_t> (idx)];

    if (set.mode != mode || set.sampleRate != sampleRate)
    {
        spareIdx = idx; // stale (other mode / rate): recycle it
        prevIdx = -1;
        return false;
    }

    prevIdx = haveCurrent ? curIdx : -1;
    spareIdx = curIdx;
    curIdx = idx;
    haveCurrent = true;
    applied.store (set.sequence, std::memory_order_relaxed);
    return true;
}

DesignLink::DesignLink() noexcept
{
    for (auto& w : payload)
        w.store (0u, std::memory_order_relaxed);
}

void DesignLink::request (const ParamValues& values, double sampleRate) noexcept
{
    const uint64_t v = version.load (std::memory_order_relaxed);
    version.store (v + 1, std::memory_order_relaxed); // odd: writing
    std::atomic_thread_fence (std::memory_order_release);

    for (int i = 0; i < numParams; ++i)
        payload[i].store (floatBits (values.get (i)), std::memory_order_relaxed);
    uint64_t rateBits;
    std::memcpy (&rateBits, &sampleRate, sizeof rateBits);
    payload[numParams].store (static_cast<uint32_t> (rateBits), std::memory_order_relaxed);
    payload[numParams + 1].store (static_cast<uint32_t> (rateBits >> 32), std::memory_order_relaxed);

    version.store (v + 2, std::memory_order_release);
}

uint64_t DesignLink::readRequest (ParamValues& values, double& sampleRate) const noexcept
{
    for (;;)
    {
        const uint64_t v1 = version.load (std::memory_order_acquire);
        if ((v1 & 1u) != 0)
            continue; // the producer is inside request (a few stores)

        for (int i = 0; i < numParams; ++i)
            values.values[static_cast<size_t> (i)] = bitsFloat (payload[i].load (std::memory_order_relaxed));
        const uint64_t lo = payload[numParams].load (std::memory_order_relaxed);
        const uint64_t hi = payload[numParams + 1].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);

        if (version.load (std::memory_order_relaxed) == v1)
        {
            const uint64_t rateBits = lo | (hi << 32);
            std::memcpy (&sampleRate, &rateBits, sizeof sampleRate);
            return v1 >> 1;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Designer
// ---------------------------------------------------------------------------------------------------------

LinearPhaseDesigner::LinearPhaseDesigner()
{
    bigFft[0].init (4096);
    bigFft[1].init (8192);
    bigFft[2].init (16384);
    partFft.init (fftSize);

    const size_t bins = 16384 / 2 + 1;
    for (auto& m : mags)
        m.assign (bins, 1.0);
    for (auto& s : spec)
        s.assign (bins, 0.0f);
    fftIn.assign (16384, 0.0f);
    fftRe.assign (bins, 0.0f);
    fftIm.assign (bins, 0.0f);
    partIn.assign (static_cast<size_t> (fftSize), 0.0f);
    for (auto& t : tapBuffers)
        t.assign (static_cast<size_t> (maxLength - 2 * hop + 1), 0.0f);
}

void LinearPhaseDesigner::design (const ParamValues& values, double sampleRate, uint64_t sequence, KernelSet& out)
{
    for (bool& v : tapsValid)
        v = false;
    tapLength = 0;

    out.sequence = sequence;
    out.sampleRate = sampleRate;
    out.identity = true;
    out.crossTerms = false;

    const int mode = values.phaseMode();
    if (mode <= 0)
    {
        out.mode = 0;
        out.numPartitions = 0;
        return;
    }

    const int N = phaseModeLength (mode);
    const int M = 2 * N;                  // design grid: twice the FIR length against time aliasing
    const int K = M / 2 + 1;              // bins 0 .. M / 2
    const int c = centreTap (mode);       // centre tap of the 2c + 1 tap FIR
    const int P = partitionsForMode (mode);
    out.mode = mode;
    out.numPartitions = P;

    // ---- target magnitude per placement at every bin ------------------------------------------------------
    BandEval bands[numBands];
    int numActive = 0;
    for (int b = 0; b < numBands; ++b)
    {
        const BandParams p = values.band (b);
        if (!p.on)
            continue;
        bands[numActive].design = designBand (p.type, sampleRate, p.freqHz, p.gainDb, p.q, p.slopeIndex);
        bands[numActive].placement = static_cast<int> (p.place);
        ++numActive;
    }

    for (int k = 0; k < K; ++k)
    {
        const double omega = 2.0 * pi * k / M;
        const double c1 = std::cos (omega), s1 = std::sin (omega);
        const double c2 = std::cos (2.0 * omega), s2 = std::sin (2.0 * omega);

        double power[numPlacements] = { 1.0, 1.0, 1.0, 1.0, 1.0 };
        for (int b = 0; b < numActive; ++b)
        {
            const BandDesign& d = bands[b].design;
            double bandPower = 1.0;
            for (int s = 0; s < d.numSections; ++s)
            {
                const BiquadCoefficients& q = d.sections[static_cast<size_t> (s)];
                const double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
                const double dr = 1.0 + q.a1 * c1 + q.a2 * c2, di = -(q.a1 * s1 + q.a2 * s2);
                bandPower *= (nr * nr + ni * ni) / (dr * dr + di * di);
            }
            power[bands[b].placement] *= bandPower;
        }
        for (int pl = 0; pl < numPlacements; ++pl)
            mags[pl][static_cast<size_t> (k)] = std::sqrt (power[pl]);
    }

    // ---- 2x2 matrix per bin: out = Dec diag (Hm, Hs) Enc diag (Hl Hst, Hr Hst) in -----------------------------
    // a = (Hm + Hs) / 2, b = (Hm - Hs) / 2:  L' = a Hl Hst L + b Hr Hst R,  R' = b Hl Hst L + a Hr Hst R
    double deviation = 0.0, cross = 0.0;
    for (int k = 0; k < K; ++k)
    {
        const size_t i = static_cast<size_t> (k);
        const double hst = mags[static_cast<int> (Placement::stereo)][i];
        const double hl = mags[static_cast<int> (Placement::left)][i] * hst;
        const double hr = mags[static_cast<int> (Placement::right)][i] * hst;
        const double hm = mags[static_cast<int> (Placement::mid)][i];
        const double hs = mags[static_cast<int> (Placement::side)][i];
        const double a = 0.5 * (hm + hs), b = 0.5 * (hm - hs);

        const double ll = a * hl, lr = b * hr, rl = b * hl, rr = a * hr;
        spec[kernelLL][i] = static_cast<float> (ll);
        spec[kernelLR][i] = static_cast<float> (lr);
        spec[kernelRL][i] = static_cast<float> (rl);
        spec[kernelRR][i] = static_cast<float> (rr);

        deviation = largest (deviation, std::fabs (ll - 1.0));
        deviation = largest (deviation, std::fabs (rr - 1.0));
        cross = largest (cross, std::fabs (lr));
        cross = largest (cross, std::fabs (rl));
    }

    const bool crossTerms = cross >= crossTolerance;
    if (deviation < identityTolerance && !crossTerms)
        return; // flat: a pure delay (identity = true)

    // ---- zero-phase IFFT, window, partition spectra ---------------------------------------------------------
    RealFft& fft = bigFft[mode - 1];
    const double invM = 1.0 / M;
    bool finite = true;
    tapLength = 2 * c + 1;

    for (int kernel = 0; kernel < numKernels; ++kernel)
    {
        if ((kernel == kernelLR || kernel == kernelRL) && !crossTerms)
            continue;

        // real, even spectrum -> real, even impulse response (the forward transform of an even sequence is real)
        const std::vector<float>& s = spec[kernel];
        for (int k = 0; k < K; ++k)
            fftIn[static_cast<size_t> (k)] = s[static_cast<size_t> (k)];
        for (int k = 1; k < M / 2; ++k)
            fftIn[static_cast<size_t> (M - k)] = s[static_cast<size_t> (k)];
        fft.forward (fftIn.data(), fftRe.data(), fftIm.data());

        std::vector<float>& taps = tapBuffers[kernel];
        for (int d = 0; d <= c; ++d)
        {
            const float v = static_cast<float> (static_cast<double> (fftRe[static_cast<size_t> (d)]) * invM * blackman (d, c));
            finite = finite && std::isfinite (v);
            taps[static_cast<size_t> (c + d)] = v;
            taps[static_cast<size_t> (c - d)] = v;
        }
        tapsValid[kernel] = true;
    }

    if (!finite)
    {
        for (bool& v : tapsValid)
            v = false;
        return; // cannot happen with finite parameters; fall back to a pure delay rather than emit garbage
    }

    for (int kernel = 0; kernel < numKernels; ++kernel)
    {
        if (!tapsValid[kernel])
            continue;
        const std::vector<float>& taps = tapBuffers[kernel];
        for (int p = 0; p < P; ++p)
        {
            for (int j = 0; j < hop; ++j)
                partIn[static_cast<size_t> (j)] = taps[static_cast<size_t> (p * hop + j)];
            for (int j = hop; j < fftSize; ++j)
                partIn[static_cast<size_t> (j)] = 0.0f;
            float* re = out.partition (kernel, p);
            partFft.forward (partIn.data(), re, re + binStride);
        }
    }

    out.identity = false;
    out.crossTerms = crossTerms;
}

uint64_t serviceDesignRequest (DesignLink& link, LinearPhaseDesigner& designer, uint64_t lastDesigned)
{
    ParamValues values;
    double sampleRate = 0.0;
    const uint64_t seq = link.readRequest (values, sampleRate);
    if (seq == 0 || seq == lastDesigned)
        return lastDesigned;
    if (values.phaseMode() <= 0 || !(sampleRate > 0.0))
        return seq; // minimum phase asks for nothing

    designer.design (values, sampleRate, seq, link.kernels.writeSlot());
    link.kernels.publish();
    return seq;
}

// ---------------------------------------------------------------------------------------------------------
// Convolver
// ---------------------------------------------------------------------------------------------------------

PartitionedConvolver::PartitionedConvolver()
{
    fft.init (fftSize);
    for (auto& r : ring)
        r.assign (ringSize, 0.0f);
    for (auto& f : fdl)
        f.assign (static_cast<size_t> (maxPartitions) * partitionStride, 0.0f);
    accRe.assign (binStride, 0.0f);
    accIm.assign (binStride, 0.0f);
    timeBuf.assign (static_cast<size_t> (fftSize), 0.0f);
}

void PartitionedConvolver::clear() noexcept
{
    for (auto& r : ring)
        std::fill (r.begin(), r.end(), 0.0f);
    for (auto& f : fdl)
        std::fill (f.begin(), f.end(), 0.0f);
    std::memset (block, 0, sizeof block);
    std::memset (previous, 0, sizeof previous);
    std::memset (outBlock, 0, sizeof outBlock);
    sampleCount = 0;
    fill = 0;
    fdlPos = 0;
}

void PartitionedConvolver::start (int mode, double sampleRate) noexcept
{
    currentMode = mode < 0 || mode >= numPhaseModes ? 0 : mode;
    rate = sampleRate;
    partitions = partitionsForMode (currentMode);
    halfLatency = static_cast<uint32_t> (phaseModeLatency (currentMode));
    clear();
}

void PartitionedConvolver::identityOutput (float out[2][hop]) const noexcept
{
    // output block k is emitted while input block k + 1 arrives: sample (sampleCount + j) leaves as x[. - N / 2]
    for (int ch = 0; ch < 2; ++ch)
        for (int j = 0; j < hop; ++j)
            out[ch][j] = ring[ch][(sampleCount + static_cast<uint32_t> (j) - halfLatency) & ringMask];
}

void PartitionedConvolver::computeOutput (const KernelSet* set, float out[2][hop]) noexcept
{
    if (set == nullptr || set->identity || set->mode != currentMode || set->sampleRate != rate
        || set->numPartitions != partitions)
    {
        identityOutput (out);
        return;
    }

    for (int o = 0; o < 2; ++o)
    {
        std::memset (accRe.data(), 0, sizeof (float) * binStride);
        std::memset (accIm.data(), 0, sizeof (float) * binStride);
        float* ar = accRe.data();
        float* ai = accIm.data();

        for (int i = 0; i < 2; ++i)
        {
            if (i != o && !set->crossTerms)
                continue;
            const int kernel = 2 * o + i;
            for (int p = 0; p < partitions; ++p)
            {
                int idx = fdlPos - p;
                if (idx < 0)
                    idx += partitions;
                const float* xr = fdl[i].data() + static_cast<size_t> (idx) * partitionStride;
                const float* xi = xr + binStride;
                const float* kr = set->partition (kernel, p);
                const float* ki = kr + binStride;
                for (int b = 0; b < numBins; ++b)
                {
                    ar[b] += kr[b] * xr[b] - ki[b] * xi[b];
                    ai[b] += kr[b] * xi[b] + ki[b] * xr[b];
                }
            }
        }

        ai[0] = 0.0f;
        ai[hop] = 0.0f;
        fft.inverse (ar, ai, timeBuf.data());
        for (int j = 0; j < hop; ++j)
            out[o][j] = timeBuf[static_cast<size_t> (hop + j)]; // overlap-save: the first block is wrap-around
    }
}

void PartitionedConvolver::processBlock (KernelMailbox* kernels) noexcept
{
    // spectrum of [previous block, this block] of both input channels goes into the delay line
    if (++fdlPos >= partitions)
        fdlPos = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        std::memcpy (timeBuf.data(), previous[ch], sizeof (float) * hop);
        std::memcpy (timeBuf.data() + hop, block[ch], sizeof (float) * hop);
        float* re = fdl[ch].data() + static_cast<size_t> (fdlPos) * partitionStride;
        fft.forward (timeBuf.data(), re, re + binStride);
        std::memcpy (previous[ch], block[ch], sizeof (float) * hop);
    }

    const bool fade = kernels != nullptr && kernels->fetch (currentMode, rate);
    const KernelSet* set = kernels != nullptr ? kernels->current() : nullptr;

    if (!fade)
    {
        computeOutput (set, outBlock);
        return;
    }

    // a new kernel set: cross-fade the old and the new result over this block
    computeOutput (set, newOut);
    computeOutput (kernels->previous(), oldOut);
    for (int ch = 0; ch < 2; ++ch)
        for (int j = 0; j < hop; ++j)
        {
            const float w = static_cast<float> (j + 1) * (1.0f / static_cast<float> (hop));
            outBlock[ch][j] = oldOut[ch][j] + w * (newOut[ch][j] - oldOut[ch][j]);
        }
}

void PartitionedConvolver::process (KernelMailbox* kernels, const float* inL, const float* inR, float* wetL,
                                    float* wetR, float* dryL, float* dryR, int n) noexcept
{
    float* const ringL = ring[0].data();
    float* const ringR = ring[1].data();

    for (int i = 0; i < n; ++i)
    {
        const float xl = inL[i], xr = inR[i];
        const uint32_t wp = sampleCount & ringMask;
        ringL[wp] = xl;
        ringR[wp] = xr;
        block[0][fill] = xl;
        block[1][fill] = xr;

        wetL[i] = outBlock[0][fill];
        wetR[i] = outBlock[1][fill];
        const uint32_t rp = (sampleCount - halfLatency) & ringMask;
        dryL[i] = ringL[rp];
        dryR[i] = ringR[rp];

        ++sampleCount;
        if (++fill == hop)
        {
            fill = 0;
            processBlock (kernels);
        }
    }
}
} // namespace kickeq
