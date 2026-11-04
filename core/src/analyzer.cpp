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

#include "kickeq/analyzer.h"

#include <algorithm>
#include <cmath>

namespace kickeq
{
namespace
{
constexpr uint32_t freshBit = 4u;
constexpr double releaseSeconds = 0.3;
constexpr double smoothingOctaves = 1.0 / 6.0;
constexpr double pi = 3.14159265358979323846;
} // namespace

// ---------------------------------------------------------------------------------------------------------
// SpscFloatRing
// ---------------------------------------------------------------------------------------------------------

SpscFloatRing::SpscFloatRing (size_t capacityPowerOfTwo)
    : buffer (capacityPowerOfTwo), mask (capacityPowerOfTwo - 1)
{
}

size_t SpscFloatRing::writeSpace() const noexcept
{
    const uint32_t used = head.load (std::memory_order_relaxed) - tail.load (std::memory_order_acquire);
    return buffer.size() - used;
}

void SpscFloatRing::write (const float* src, size_t n) noexcept
{
    const uint32_t h = head.load (std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i)
        buffer[(h + i) & mask] = src[i];
    head.store (h + static_cast<uint32_t> (n), std::memory_order_release);
}

size_t SpscFloatRing::readAvailable() const noexcept
{
    return head.load (std::memory_order_acquire) - tail.load (std::memory_order_relaxed);
}

size_t SpscFloatRing::read (float* dst, size_t n) noexcept
{
    const uint32_t t = tail.load (std::memory_order_relaxed);
    const size_t avail = head.load (std::memory_order_acquire) - t;
    const size_t count = n < avail ? n : avail;
    for (size_t i = 0; i < count; ++i)
        dst[i] = buffer[(t + i) & mask];
    tail.store (t + static_cast<uint32_t> (count), std::memory_order_release);
    return count;
}

// ---------------------------------------------------------------------------------------------------------
// SpectrumAnalyzer
// ---------------------------------------------------------------------------------------------------------

float SpectrumAnalyzer::displayFrequency (int i, int numBins) noexcept
{
    if (numBins <= 1)
        return minFrequencyHz;
    const double t = static_cast<double> (i) / static_cast<double> (numBins - 1);
    return static_cast<float> (minFrequencyHz * std::pow (static_cast<double> (maxFrequencyHz) / minFrequencyHz, t));
}

SpectrumAnalyzer::SpectrumAnalyzer (int numDisplayBins)
    : bins (std::min (std::max (numDisplayBins, 8), 4096)), pre (ringCapacity), post (ringCapacity)
{
    fft.init (fftSize);

    window.assign (fftSize, 0.0f);
    double sum = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        // 4-term Blackman-Harris, periodic
        const double x = 2.0 * pi * i / fftSize;
        const double w = 0.35875 - 0.48829 * std::cos (x) + 0.14128 * std::cos (2.0 * x) - 0.01168 * std::cos (3.0 * x);
        window[static_cast<size_t> (i)] = static_cast<float> (w);
        sum += w;
    }
    // a sine of amplitude A gives |X| = A * sum / 2 at its peak bin; scale so that 1.0 -> 0 dB
    powerScale = static_cast<float> (4.0 / (sum * sum));

    fftIn.assign (fftSize, 0.0f);
    fftRe.assign (fftSize / 2 + 1, 0.0f);
    fftIm.assign (fftSize / 2 + 1, 0.0f);
    chunkPre.assign (hopSize, 0.0f);
    chunkPost.assign (hopSize, 0.0f);
    cumulative.assign (fftSize / 2 + 1, 0.0);
    lowPos.assign (static_cast<size_t> (bins), -1.0);
    highPos.assign (static_cast<size_t> (bins), -1.0);

    pre.history.assign (fftSize, 0.0f);
    post.history.assign (fftSize, 0.0f);
    pre.smooth.assign (fftSize / 2 + 1, 0.0f);
    post.smooth.assign (fftSize / 2 + 1, 0.0f);

    for (auto& f : frames)
    {
        f.preDb.assign (static_cast<size_t> (bins), floorDb);
        f.postDb.assign (static_cast<size_t> (bins), floorDb);
    }

    prepare (48000.0);
}

void SpectrumAnalyzer::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    releaseAlpha = static_cast<float> (std::exp (-static_cast<double> (hopSize) / (sampleRate * releaseSeconds)));

    // display bin ranges (1/6 octave wide, centred on the bin frequency) in units of FFT bins
    const double binWidthHz = sampleRate / fftSize;
    const double halfWidth = std::pow (2.0, smoothingOctaves * 0.5);
    const double nyquistBin = fftSize / 2;
    for (int i = 0; i < bins; ++i)
    {
        const double f = displayFrequency (i, bins);
        const double lo = f / halfWidth / binWidthHz;
        const double hi = f * halfWidth / binWidthHz;
        if (lo >= nyquistBin)
        {
            lowPos[static_cast<size_t> (i)] = -1.0;
            highPos[static_cast<size_t> (i)] = -1.0;
        }
        else
        {
            lowPos[static_cast<size_t> (i)] = lo;
            highPos[static_cast<size_t> (i)] = std::min (hi, nyquistBin);
        }
    }

    resetHistory();
}

void SpectrumAnalyzer::resetHistory()
{
    for (Channel* c : { &pre, &post })
    {
        std::fill (c->history.begin(), c->history.end(), 0.0f);
        std::fill (c->smooth.begin(), c->smooth.end(), 0.0f);
        c->writePos = 0;
    }
    sinceHop = 0;
}

void SpectrumAnalyzer::push (const float* preLeft, const float* preRight, const float* postLeft, const float* postRight,
                             int numSamples) noexcept
{
    if (numSamples <= 0 || !enabledFlag.load (std::memory_order_acquire))
        return;

    const size_t n = static_cast<size_t> (numSamples);
    if (std::min (pre.ring.writeSpace(), post.ring.writeSpace()) < n)
    {
        dropped.store (dropped.load (std::memory_order_relaxed) + static_cast<uint32_t> (n), std::memory_order_relaxed);
        return;
    }

    const bool side = sourceValue.load (std::memory_order_relaxed) == static_cast<int> (AnalyzerSource::side);

    float tmp[256];
    const auto mix = [&] (SpscFloatRing& ring, const float* l, const float* r, size_t pos, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const float a = l != nullptr ? l[pos + i] : 0.0f;
            const float b = r != nullptr ? r[pos + i] : (side ? 0.0f : a);
            tmp[i] = side ? 0.5f * (a - b) : 0.5f * (a + b);
        }
        ring.write (tmp, count);
    };

    for (size_t pos = 0; pos < n; pos += 256)
    {
        const size_t count = std::min<size_t> (256, n - pos);
        mix (pre.ring, preLeft, preRight, pos, count);
        mix (post.ring, postLeft, postRight, pos, count);
    }
}

void SpectrumAnalyzer::process() noexcept
{
    const bool enabled = enabledFlag.load (std::memory_order_acquire);
    bool produced = false;

    // The work of one call is bounded by what is pending *now*: a producer faster than real time (offline
    // bounce, a validator) must not keep this loop, which runs on the UI thread, from ever finishing. Only the
    // newest fftSize + hopSize samples are kept (one fresh analysis window plus one hop), older ones are dropped:
    // the display always shows the present, never a backlog.
    size_t budget = std::min (pre.ring.readAvailable(), post.ring.readAvailable());
    const size_t maxBacklog = static_cast<size_t> (fftSize) + static_cast<size_t> (hopSize);
    while (budget > maxBacklog)
    {
        const size_t drop = std::min (budget - maxBacklog, static_cast<size_t> (hopSize));
        pre.ring.read (chunkPre.data(), drop);
        post.ring.read (chunkPost.data(), drop);
        budget -= drop;
    }

    while (budget > 0)
    {
        const size_t avail = std::min (std::min (pre.ring.readAvailable(), post.ring.readAvailable()), budget);
        if (avail == 0)
            break;

        const size_t take = std::min (avail, static_cast<size_t> (hopSize - sinceHop));
        budget -= take;
        pre.ring.read (chunkPre.data(), take);
        post.ring.read (chunkPost.data(), take);

        if (!enabled)
            continue; // discard, keep both rings aligned

        for (size_t i = 0; i < take; ++i)
        {
            pre.history[static_cast<size_t> (pre.writePos)] = chunkPre[i];
            post.history[static_cast<size_t> (post.writePos)] = chunkPost[i];
            pre.writePos = (pre.writePos + 1) & (fftSize - 1);
            post.writePos = (post.writePos + 1) & (fftSize - 1);
        }

        sinceHop += static_cast<int> (take);
        if (sinceHop == hopSize)
        {
            sinceHop = 0;
            analyse (pre);
            analyse (post);
            produced = true;
        }
    }

    if (produced)
        publish();
}

void SpectrumAnalyzer::analyse (Channel& c) noexcept
{
    for (int i = 0; i < fftSize; ++i)
        fftIn[static_cast<size_t> (i)] = c.history[static_cast<size_t> ((c.writePos + i) & (fftSize - 1))]
                                         * window[static_cast<size_t> (i)];

    fft.forward (fftIn.data(), fftRe.data(), fftIm.data());

    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const size_t i = static_cast<size_t> (k);
        const float p = (fftRe[i] * fftRe[i] + fftIm[i] * fftIm[i]) * powerScale;
        const float s = c.smooth[i];
        c.smooth[i] = p > s ? p : releaseAlpha * s + (1.0f - releaseAlpha) * p; // instant attack
    }
}

void SpectrumAnalyzer::mapToDisplay (const std::vector<float>& power, std::vector<float>& outDb) noexcept
{
    // The 1/6 octave smoothing is an average over a window of constant width in log frequency, i.e. with the
    // measure dx / x: a pure tone then keeps its position (a plain average over Hz divides the tone's energy
    // by a window width proportional to f and would pull the peak down in frequency) and white noise keeps
    // its level. The power between FFT bins is linearly interpolated, so the integral of P (x) / x is
    // available in closed form per segment [k, k + 1]:
    //   k >= 1: p0 ln (1 + t) + (p1 - p0) k (t - ln (1 + t)),  t = (x - k) / k
    //   k == 0: p0 ln x - (p1 - p0) (1 - x)   (relative to x = 1, for 0 < x < 1)
    const int last = fftSize / 2;

    // cumulative[k] = integral from x = 1 to x = k (k >= 1)
    cumulative[0] = 0.0;
    cumulative[1] = 0.0;
    for (int k = 1; k < last; ++k)
    {
        const double p0 = power[static_cast<size_t> (k)], p1 = power[static_cast<size_t> (k) + 1];
        cumulative[static_cast<size_t> (k) + 1] = cumulative[static_cast<size_t> (k)]
                                                  + (p0 * std::log1p (1.0 / k) + (p1 - p0) * k * (1.0 / k - std::log1p (1.0 / k)));
    }

    const auto integral = [&] (double x)
    {
        if (x < 1.0)
        {
            const double p0 = power[0], p1 = power[1];
            return p0 * std::log (x) - (p1 - p0) * (1.0 - x);
        }
        int k = static_cast<int> (x);
        if (k >= last)
            k = last - 1;
        const double p0 = power[static_cast<size_t> (k)], p1 = power[static_cast<size_t> (k) + 1];
        const double t = (x - k) / k;
        return cumulative[static_cast<size_t> (k)] + p0 * std::log1p (t) + (p1 - p0) * k * (t - std::log1p (t));
    };

    for (int i = 0; i < bins; ++i)
    {
        const double lo = lowPos[static_cast<size_t> (i)], hi = highPos[static_cast<size_t> (i)];
        float db = floorDb;
        if (lo > 0.0 && hi > lo)
        {
            const double avg = (integral (hi) - integral (lo)) / std::log (hi / lo);
            if (avg > 1.0e-20)
                db = std::max (floorDb, static_cast<float> (10.0 * std::log10 (avg)));
        }
        outDb[static_cast<size_t> (i)] = db;
    }
}

void SpectrumAnalyzer::publish() noexcept
{
    SpectrumFrame& f = frames[writeSlot];
    mapToDisplay (pre.smooth, f.preDb);
    mapToDisplay (post.smooth, f.postDb);
    f.sequence = ++sequence;
    f.sampleRate = sampleRate;

    const uint32_t old = latest.exchange (writeSlot | freshBit, std::memory_order_acq_rel);
    writeSlot = old & 3u;
}

bool SpectrumAnalyzer::pull (SpectrumFrame& frame)
{
    if ((latest.load (std::memory_order_acquire) & freshBit) == 0)
        return false;

    const uint32_t old = latest.exchange (readSlot, std::memory_order_acq_rel);
    readSlot = old & 3u;

    const SpectrumFrame& f = frames[readSlot];
    frame.preDb = f.preDb;
    frame.postDb = f.postDb;
    frame.sequence = f.sequence;
    frame.sampleRate = f.sampleRate;
    return true;
}
} // namespace kickeq
