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

// Linear-phase mode (ARCHITECTURE.md section 4.7): kernel design (off the audio thread), the hand-over of kernels
// to the audio thread, and the uniform-partitioned FFT convolution that runs on it.
//
//   audio thread   EqEngine -> DesignLink::request (parameter snapshot, wait-free)
//                          <- KernelMailbox::fetch  (finished kernels, wait-free)
//   design thread  DesignLink::readRequest -> LinearPhaseDesigner::design -> KernelMailbox::publish
//
// Latency scheme (exactly N / 2 samples for every host block size)
// ----------------------------------------------------------------
// The convolver works on blocks of B = 128 samples. A block can only be convolved once its last sample has been
// received, so the block structure alone costs one block: output block k (the response to input samples
// kB .. kB + B - 1) is emitted while input block k + 1 is received, i.e. every tap j of the FIR is delayed by B + j.
// The convolver therefore uses an FIR whose centre tap sits at c = N / 2 - B instead of N / 2; the sum
// B + c is exactly N / 2, independent of how the host splits its buffers (the convolver counts samples, not calls).
// The FIR has 2c + 1 taps with a Blackman window that is zero at both ends, so only taps 1 .. 2c - 1 are non-zero and
// P = (N - 2B) / B partitions of B taps are needed (14 / 30 / 62). The cost is a window that is 2B taps (256) shorter
// than N: 12 % of the resolution at N = 2048, 3 % at N = 8192.
//
// A kernel set that is a pure delay (all bands flat) is flagged `identity`: the convolver then copies the input
// history at exactly N / 2 samples back instead of running an FFT, so a flat linear-phase EQ is bit-transparent
// (the output is the input delayed by N / 2 samples). The same delayed copy is the "dry" signal of the bypass
// cross-fade.
//
// Threading: nothing here creates a thread. EqEngine (audio thread) calls PartitionedConvolver and the reader side of
// KernelMailbox / the producer side of DesignLink; a DesignThread (design_thread.h) or designSynchronously() runs the
// designer. No allocation, lock or system call on the audio side; every buffer is allocated in the constructors.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kickeq/config.h"
#include "kickeq/fft.h"
#include "kickeq/params.h"

namespace kickeq
{
namespace linear
{
constexpr int hop = 128;                           // B: block size of the convolver
constexpr int fftSize = 2 * hop;                   // overlap-save transform size
constexpr int numBins = hop + 1;                   // complex bins of a real transform of fftSize
constexpr int binStride = 136;                     // padded bin count (re block, then im block)
constexpr int partitionStride = 2 * binStride;     // floats per partition spectrum
constexpr int maxLength = 8192;                    // N of the largest phase mode
constexpr int maxPartitions = maxLength / hop - 2; // 62
constexpr int numKernels = 4;

/** Kernel k maps input channel i to output channel o as k = 2 * o + i. */
enum KernelIndex : int { kernelLL = 0, kernelLR = 1, kernelRL = 2, kernelRR = 3 };

/** Number of partitions of a phase mode (0 for minimum phase). */
constexpr int partitionsForMode (int mode) noexcept { return mode > 0 ? phaseModeLength (mode) / hop - 2 : 0; }

/** Index of the centre tap of the FIR: N / 2 - B. */
constexpr int centreTap (int mode) noexcept { return phaseModeLength (mode) / 2 - hop; }

static_assert (partitionsForMode (3) == maxPartitions, "largest mode defines the storage");
static_assert (partitionStride >= 2 * numBins, "padded spectrum fits");
} // namespace linear

/** The spectra of one complete EQ: up to four partitioned FIR kernels (LL, LR, RL, RR). Written by the designer,
    read-only on the audio thread once published. */
struct KernelSet
{
    uint64_t sequence = 0;     // sequence number of the parameter snapshot it was designed from
    int mode = 0;              // phase mode (FIR length N = phaseModeLength (mode)); 0 = invalid / empty
    double sampleRate = 0.0;
    int numPartitions = 0;
    bool identity = true;      // pure delay of N / 2 samples: no spectra
    bool crossTerms = false;   // LR / RL are used (a Mid / Side band is active)

    KernelSet();

    const float* partition (int kernel, int p) const noexcept
    {
        return spectra.data() + (static_cast<size_t> (kernel) * linear::maxPartitions + static_cast<size_t> (p)) * linear::partitionStride;
    }
    float* partition (int kernel, int p) noexcept
    {
        return spectra.data() + (static_cast<size_t> (kernel) * linear::maxPartitions + static_cast<size_t> (p)) * linear::partitionStride;
    }

private:
    std::vector<float> spectra; // [kernel][partition][re[binStride], im[binStride]]
};

/** Wait-free hand-over of finished kernel sets from one writer (design thread) to one reader (audio thread).
    Four slots: the writer owns one, the mailbox one, the reader two (current + a spare that holds the previous set
    for the cross-fade of the hop in which it was replaced). */
class KernelMailbox
{
public:
    KernelMailbox();

    // ---- writer (design thread) -----------------------------------------------------------------------------
    /** The slot the writer fills; private to the writer until publish(). */
    KernelSet& writeSlot() noexcept { return slots[static_cast<size_t> (writeIdx)]; }
    /** Hands the filled slot over; the writer gets a new private slot. */
    void publish() noexcept;

    // ---- reader (audio thread) ------------------------------------------------------------------------------
    /** Takes the newest published set if there is one. A set that does not match (mode, sampleRate) is dropped.
        Returns true when current() changed; previous() then holds the replaced set (nullptr if there was none)
        until the next call. */
    bool fetch (int mode, double sampleRate) noexcept;
    const KernelSet* current() const noexcept { return haveCurrent ? &slots[static_cast<size_t> (curIdx)] : nullptr; }
    const KernelSet* previous() const noexcept { return prevIdx >= 0 ? &slots[static_cast<size_t> (prevIdx)] : nullptr; }
    /** Forgets the current set (the convolver then runs as a pure delay until a new set arrives). */
    void dropCurrent() noexcept
    {
        haveCurrent = false;
        prevIdx = -1;
        applied.store(0, std::memory_order_relaxed);
    }

    /** Sequence number of the set in use (0 = none); any thread. */
    uint64_t appliedSequence() const noexcept { return applied.load (std::memory_order_relaxed); }

private:
    static constexpr uint32_t dirtyBit = 4u;

    std::vector<KernelSet> slots;
    std::atomic<uint32_t> middle;
    std::atomic<uint64_t> applied;
    int writeIdx = 0;            // writer private
    int curIdx = 2, spareIdx = 3; // reader private
    int prevIdx = -1;
    bool haveCurrent = false;
};

/** Parameter snapshots from the audio thread (single producer) to the design thread, sequence-locked (the payload is
    a set of relaxed atomics, so there is no data race and no allocation), plus the kernel mailbox of the way back. */
class DesignLink
{
public:
    DesignLink() noexcept;

    /** Posts a design request. Single producer: the audio thread, or any thread while no audio thread runs. */
    void request (const ParamValues& values, double sampleRate) noexcept;

    /** Number of requests posted so far (0 = none); any thread. */
    uint64_t requestSequence() const noexcept { return version.load (std::memory_order_acquire) >> 1; }

    /** Copies a consistent snapshot of the newest request; returns its sequence number (0 = none yet). */
    uint64_t readRequest (ParamValues& values, double& sampleRate) const noexcept;

    KernelMailbox kernels;

private:
    static constexpr int payloadWords = numParams + 2;
    std::atomic<uint64_t> version { 0 }; // 2 * requests, odd while the producer writes
    std::atomic<uint32_t> payload[payloadWords];
};

/** Designs the kernels of a parameter set (ARCHITECTURE.md section 4.7). Not real-time safe; allocates only in the
    constructor. Deterministic: the same input gives the same bits. */
class LinearPhaseDesigner
{
public:
    LinearPhaseDesigner();

    /** Designs the EQ described by `values` for FIR length N = phaseModeLength (values.phaseMode ()) into `out`:
        the target magnitude of the whole EQ (the analytic responses of eq_curve.h) per frequency bin of a 2N grid,
        combined to the 2x2 matrix of the signal flow of section 4.4
            M (f) = Dec diag (Hm, Hs) Enc diag (Hl Hst, Hr Hst)           (all magnitudes, zero phase)
        zero-phase IFFT, cut to 2c + 1 taps (c = N / 2 - B), Blackman window, partition spectra. For a minimum-phase
        `values` out is marked empty (mode 0). */
    void design (const ParamValues& values, double sampleRate, uint64_t sequence, KernelSet& out);

    /** Time domain taps (2c + 1, symmetric around centreTap) of the last design, for kernel `kernel`; nullptr
        when that kernel was not computed (identity set, or LR / RL without cross terms). */
    const float* taps (int kernel) const noexcept { return tapsValid[kernel] ? tapBuffers[kernel].data() : nullptr; }
    int tapCount() const noexcept { return tapLength; }

private:
    RealFft bigFft[3];  // 2N = 4096, 8192, 16384
    RealFft partFft;    // linear::fftSize
    std::vector<double> mags[5];
    std::vector<float> spec[linear::numKernels];
    std::vector<float> fftIn, fftRe, fftIm, partIn;
    std::vector<float> tapBuffers[linear::numKernels];
    bool tapsValid[linear::numKernels] = {};
    int tapLength = 0;
};

/** One service step of a design thread: if a request newer than `lastDesigned` is pending and asks for a linear
    mode, designs it into the mailbox and publishes it. Returns the sequence number now considered handled
    (`lastDesigned` when nothing was pending). */
uint64_t serviceDesignRequest (DesignLink& link, LinearPhaseDesigner& designer, uint64_t lastDesigned);

/** Uniform-partitioned overlap-save FFT convolution of a stereo signal with the current kernel set of a mailbox
    (see the latency scheme above). The whole object is preallocated; start() / process() never allocate. */
class PartitionedConvolver
{
public:
    PartitionedConvolver();

    /** (Re)starts for a linear phase mode (1..3) and clears all history. */
    void start (int mode, double sampleRate) noexcept;
    /** Clears all history, keeps the mode. */
    void clear() noexcept;

    int mode() const noexcept { return currentMode; }
    /** N / 2 for the running mode. */
    int latency() const noexcept { return phaseModeLatency (currentMode); }

    /** Processes n samples. Per sample i: wet = the input delayed by exactly latency () and filtered with the
        kernel set that was current when its block was computed; dry = the plain input delayed by latency ().
        `kernels` may be nullptr (pure delay). Outputs must not alias each other; they may alias the inputs. */
    void process (KernelMailbox* kernels, const float* inL, const float* inR, float* wetL, float* wetR, float* dryL,
                  float* dryR, int n) noexcept;

private:
    void processBlock (KernelMailbox* kernels) noexcept;
    void computeOutput (const KernelSet* set, float out[2][linear::hop]) noexcept;
    void identityOutput (float out[2][linear::hop]) const noexcept;

    static constexpr uint32_t ringSize = 8192;
    static constexpr uint32_t ringMask = ringSize - 1;

    RealFft fft;
    int currentMode = 0;
    double rate = 0.0;
    int partitions = 0;
    uint32_t halfLatency = 0;
    uint32_t sampleCount = 0;
    int fill = 0;
    int fdlPos = 0;

    std::vector<float> ring[2];     // input history (dry delay line and identity source)
    std::vector<float> fdl[2];      // spectra of the last P input segments, per input channel
    std::vector<float> accRe, accIm, timeBuf;
    float block[2][linear::hop] = {};
    float previous[2][linear::hop] = {};
    float outBlock[2][linear::hop] = {};
    float newOut[2][linear::hop] = {};
    float oldOut[2][linear::hop] = {};
};
} // namespace kickeq
