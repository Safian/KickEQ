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

// Spectrum analyser data path (ARCHITECTURE.md section 5.1). The core creates no threads.
//
//   audio thread     push (pre, post, n)   -> mono sum -> wait-free SPSC float rings (2^16 samples each)
//   analyser thread  process ()            -> Blackman-Harris window (N = 4096, hop = 1024) -> real FFT -> power
//                                             -> ballistics (instant attack, ~300 ms release)
//                                             -> 1/6-octave smoothing -> log-frequency display bins
//                                             -> SpectrumFrame, published with an atomic index swap
//   UI thread        pull (frame)          -> latest unseen frame, or false
//
// Levels are dBFS: a full-scale sine reads 0 dB at its peak bin before the 1/6-octave smoothing (which
// averages power, so a pure tone reads lower after smoothing; noise is unaffected).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kickeq/fft.h"

namespace kickeq
{
static_assert (std::atomic<uint32_t>::is_always_lock_free, "the analyser rings need lock-free 32 bit atomics");
static_assert (std::atomic<bool>::is_always_lock_free, "the analyser flags need lock-free atomics");

/** Wait-free single-producer / single-consumer ring of floats. Capacity is a power of two. */
class SpscFloatRing
{
public:
    explicit SpscFloatRing (size_t capacityPowerOfTwo);

    size_t capacity() const noexcept { return buffer.size(); }

    // producer thread
    size_t writeSpace() const noexcept;
    /** Writes n samples; precondition: n <= writeSpace(). */
    void write (const float* src, size_t n) noexcept;

    // consumer thread
    size_t readAvailable() const noexcept;
    /** Reads up to n samples, returns the count. */
    size_t read (float* dst, size_t n) noexcept;

private:
    std::vector<float> buffer;
    size_t mask;
    std::atomic<uint32_t> head { 0 }; // total samples written (producer)
    std::atomic<uint32_t> tail { 0 }; // total samples read (consumer)
};

/** What the analyser listens to. LeftPlusRight and Mid both analyse (L + R) / 2 (mono sum), Side (L - R) / 2. */
enum class AnalyzerSource : int { leftPlusRight, mid, side };

struct SpectrumFrame
{
    std::vector<float> preDb;  // input spectrum, dBFS per display bin
    std::vector<float> postDb; // output spectrum
    uint32_t sequence = 0;     // increments with every published frame
    double sampleRate = 0.0;
};

class SpectrumAnalyzer
{
public:
    static constexpr int fftSize = 4096;
    static constexpr int hopSize = 1024;
    static constexpr size_t ringCapacity = size_t (1) << 16;
    static constexpr float floorDb = -200.0f;
    static constexpr float minFrequencyHz = 10.0f;
    static constexpr float maxFrequencyHz = 30000.0f;

    /** numDisplayBins (clamped to 8 .. 4096) log-spaced bins from 10 Hz to 30 kHz. Allocates everything. */
    explicit SpectrumAnalyzer (int numDisplayBins = 512);

    /** Sets the sample rate and clears the history. Analyser / UI thread, while push() is not running. */
    void prepare (double sampleRate);

    int numBins() const noexcept { return bins; }
    /** Centre frequency of display bin i. */
    float binFrequencyHz (int i) const noexcept { return displayFrequency (i, bins); }
    static float displayFrequency (int i, int numBins) noexcept;

    void setEnabled (bool enabled) noexcept { enabledFlag.store (enabled, std::memory_order_release); }
    bool isEnabled() const noexcept { return enabledFlag.load (std::memory_order_acquire); }
    void setSource (AnalyzerSource s) noexcept { sourceValue.store (static_cast<int> (s), std::memory_order_relaxed); }

    /** AUDIO THREAD. Wait-free. Mono-sums the selected source of the input (pre) and output (post) and
        appends it to the rings; right pointers may be nullptr (mono: R = L). Does nothing when disabled and
        drops the whole block when the rings lack space (pre and post stay aligned). */
    void push (const float* preLeft, const float* preRight, const float* postLeft, const float* postRight,
               int numSamples) noexcept;

    /** ANALYSER THREAD. Consumes everything pushed so far (any number of hops) and publishes at most one
        frame. While disabled it discards the pushed samples. Does not allocate. */
    void process() noexcept;

    /** UI thread (may be the same as the analyser thread). Copies the latest frame if it has not been seen
        yet. Returns false when there is no new frame. `frame` is resized on first use only. */
    bool pull (SpectrumFrame& frame);

    /** Number of samples push() dropped because the rings were full. */
    uint32_t droppedSamples() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    struct Channel
    {
        explicit Channel (size_t ringSize) : ring (ringSize) {}
        SpscFloatRing ring;
        std::vector<float> history; // last fftSize samples (circular)
        std::vector<float> smooth;  // power after ballistics, fftSize / 2 + 1 bins
        int writePos = 0;
    };

    void resetHistory();
    void analyse (Channel& c) noexcept;
    void mapToDisplay (const std::vector<float>& power, std::vector<float>& outDb) noexcept;
    void publish() noexcept;

    int bins;
    double sampleRate = 48000.0;
    float releaseAlpha = 0.0f;

    std::atomic<bool> enabledFlag { false };
    std::atomic<int> sourceValue { 0 };
    std::atomic<uint32_t> dropped { 0 };

    Channel pre, post;
    int sinceHop = 0;

    RealFft fft;
    std::vector<float> window, fftIn, fftRe, fftIm, chunkPre, chunkPost;
    float powerScale = 1.0f;

    std::vector<double> cumulative;      // integral of the linearly interpolated power
    std::vector<double> lowPos, highPos; // display bin range in FFT bin units; lowPos < 0 = above Nyquist

    // triple buffer: the producer (process) owns writeSlot, the consumer (pull) owns readSlot, `latest`
    // holds the third slot plus a "fresh" flag (bit 2)
    SpectrumFrame frames[3];
    std::atomic<uint32_t> latest { 1 };
    uint32_t writeSlot = 0, readSlot = 2;
    uint32_t sequence = 0;
};
} // namespace kickeq
