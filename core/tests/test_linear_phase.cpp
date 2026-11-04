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

// Linear-phase mode (ARCHITECTURE.md section 4.7): FFT, kernel hand-over, designer, convolver and the engine path.

#include "kickeq/config.h"

#if KICKEQ_LINEAR_PHASE

#include <chrono>
#include <thread>

#include "kickeq/design_thread.h"
#include "kickeq/eq_curve.h"
#include "kickeq/fft.h"
#include "kickeq/linear_phase.h"
#include "test_util.h"

using namespace tu;

namespace
{
const int modes[3] = { 1, 2, 3 };
int lengthOf (int mode) { return phaseModeLength (mode); }

ParamValues withMode (ParamValues pv, int mode)
{
    pv.set (paramPhaseMode, static_cast<float> (mode));
    return pv;
}

/** An engine plus a designer that plays the part of the design thread, deterministically: service () runs between
    the blocks of the "audio thread". */
struct Rig
{
    std::unique_ptr<EqEngine> engine;
    LinearPhaseDesigner designer;
    uint64_t last = 0;

    explicit Rig (double fs) : engine (makeEngine (fs)) {}

    void service() { last = serviceDesignRequest (*engine->designLink(), designer, last); }

    void run (const ParamValues& pv, float* l, float* r, int n, int block, bool serviceBetween)
    {
        for (int pos = 0; pos < n; pos += block)
        {
            const int len = std::min (block, n - pos);
            engine->process (pv, l + pos, r != nullptr ? r + pos : nullptr, len);
            if (serviceBetween)
                service();
        }
    }
};

/** Engine with the kernels of `pv` already designed (what a design thread that kept up would have produced). */
std::unique_ptr<EqEngine> makeLinear (double fs, const ParamValues& pv)
{
    auto e = makeEngine (fs);
    e->designSynchronously (pv);
    return e;
}

/** Output channel `outCh` for an impulse of amplitude 1 at sample 0 on input channel `inCh`, `len` samples. */
std::vector<float> impulseResponse (const ParamValues& pv, double fs, int len, int inCh, int outCh, int block = 256)
{
    auto e = makeLinear (fs, pv);
    std::vector<float> l (static_cast<size_t> (len), 0.0f), r (static_cast<size_t> (len), 0.0f);
    (inCh == 0 ? l : r)[0] = 1.0f;
    processBlocks (*e, pv, l.data(), r.data(), len, block);
    return outCh == 0 ? l : r;
}

std::complex<double> dtft (const std::vector<float>& h, double omega)
{
    double re = 0.0, im = 0.0;
    for (size_t n = 0; n < h.size(); ++n)
    {
        re += h[n] * std::cos (omega * static_cast<double> (n));
        im -= h[n] * std::sin (omega * static_cast<double> (n));
    }
    return { re, im };
}

double analyticDb (const ParamValues& pv, double fs, double f, ChannelView view = ChannelView::stereo)
{
    const float fq = static_cast<float> (f);
    float mag = 0.0f;
    computeResponse (pv, fs, &fq, &mag, nullptr, 1, view);
    return mag;
}

/** Features of a test band are at least this many FFT bins wide at the given N (an FIR of length N cannot resolve
    narrower ones; the specification's accuracy statement starts at 4 bins). */
double featureHz (double base, double fs, int mode, double minBins = 12.0) { return std::max (base, minBins * fs / lengthOf (mode)); }

std::vector<double> logFreqs (double lo, double hi, int n)
{
    std::vector<double> f (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        f[static_cast<size_t> (i)] = lo * std::pow (hi / lo, static_cast<double>(i) / (n - 1));
    return f;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------
// FFT inverse
// ---------------------------------------------------------------------------------------------------------

TEST (linear_fft_inverse_is_the_inverse_of_forward)
{
    Rng rng (3);
    for (int size = 4; size <= 16384; size *= 2)
    {
        RealFft fft;
        CHECK (fft.init (size));
        std::vector<float> x (static_cast<size_t> (size)), re (static_cast<size_t> (size / 2 + 1)), im (re.size()), y (x.size());
        for (auto& v : x)
            v = rng.uniform();
        fft.forward (x.data(), re.data(), im.data());
        fft.inverse (re.data(), im.data(), y.data());
        double worst = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            worst = std::max (worst, static_cast<double> (std::fabs (x[i] - y[i])));
        if (worst > 2.0e-5)
            std::printf ("    size %d: worst round trip error %g\n", size, worst);
        CHECK (worst < 2.0e-5);
    }

    // a known spectrum: the DFT of a unit impulse at position 5 is e^{-2 pi i 5 k / n}
    RealFft fft;
    CHECK (fft.init (64));
    std::vector<float> re (33), im (33), y (64);
    for (int k = 0; k <= 32; ++k)
    {
        re[static_cast<size_t> (k)] = static_cast<float> (std::cos (2.0 * kPi * 5.0 * k / 64.0));
        im[static_cast<size_t> (k)] = static_cast<float> (-std::sin (2.0 * kPi * 5.0 * k / 64.0));
    }
    fft.inverse (re.data(), im.data(), y.data());
    for (int i = 0; i < 64; ++i)
        CHECK_NEAR (y[static_cast<size_t> (i)], i == 5 ? 1.0 : 0.0, 1.0e-6);
}

// ---------------------------------------------------------------------------------------------------------
// Kernel mailbox and design link
// ---------------------------------------------------------------------------------------------------------

TEST (linear_kernel_mailbox_hands_over_the_newest_set)
{
    KernelMailbox box;
    CHECK (box.current() == nullptr && box.previous() == nullptr);
    CHECK (!box.fetch (1, 48000.0)); // nothing published

    const auto publish = [&] (uint64_t seq, int mode, double rate)
    {
        KernelSet& s = box.writeSlot();
        s.sequence = seq;
        s.mode = mode;
        s.sampleRate = rate;
        s.numPartitions = 14;
        s.identity = false;
        s.partition (0, 0)[0] = static_cast<float> (seq); // payload stamp
        box.publish();
    };

    publish (1, 1, 48000.0);
    CHECK (box.fetch (1, 48000.0));
    CHECK (box.current() != nullptr && box.current()->sequence == 1 && box.previous() == nullptr);
    CHECK (box.appliedSequence() == 1);
    CHECK (!box.fetch (1, 48000.0)); // nothing new

    publish (2, 1, 48000.0);
    publish (3, 1, 48000.0); // 2 is overtaken before the reader looks
    CHECK (box.fetch (1, 48000.0));
    CHECK (box.current()->sequence == 3 && box.current()->partition (0, 0)[0] == 3.0f);
    CHECK (box.previous() != nullptr && box.previous()->sequence == 1 && box.previous()->partition (0, 0)[0] == 1.0f);

    publish (4, 2, 48000.0); // another mode: dropped, current stays
    CHECK (!box.fetch (1, 48000.0));
    CHECK (box.current()->sequence == 3 && box.previous() == nullptr);
    publish (5, 1, 44100.0); // another rate: dropped
    CHECK (!box.fetch (1, 48000.0));
    CHECK (box.current()->sequence == 3);

    // a long random sequence never exposes a half written or recycled set
    Rng rng (11);
    uint64_t seq = 10, newest = 3;
    for (int i = 0; i < 5000; ++i)
    {
        if (rng.integer (0, 2) != 0)
        {
            publish (++seq, 1, 48000.0);
            newest = seq;
        }
        if (rng.integer (0, 1) != 0 && box.fetch (1, 48000.0))
        {
            CHECK (box.current()->sequence == newest);
            CHECK (box.current()->partition (0, 0)[0] == static_cast<float> (newest));
            if (box.previous() != nullptr)
                CHECK (box.previous()->partition (0, 0)[0] == static_cast<float> (box.previous()->sequence));
        }
    }

    box.dropCurrent();
    CHECK (box.current() == nullptr && box.appliedSequence() == 0);
}

TEST (linear_design_link_snapshots_are_consistent_and_sequenced)
{
    DesignLink link;
    ParamValues v;
    double sr = 0.0;
    CHECK (link.requestSequence() == 0 && link.readRequest (v, sr) == 0);

    ParamValues a;
    setBand (a, 3, true, BandType::bell, 1234.0f, 4.5f, 1.5f);
    a.set (paramPhaseMode, 2.0f);
    link.request (a, 44100.0);
    CHECK (link.requestSequence() == 1);
    CHECK (link.readRequest (v, sr) == 1 && sr == 44100.0 && v == a);

    a.set (3, BandParam::gain, -7.0f);
    link.request (a, 96000.0);
    CHECK (link.readRequest (v, sr) == 2 && sr == 96000.0 && v == a);
}

// ---------------------------------------------------------------------------------------------------------
// Designer
// ---------------------------------------------------------------------------------------------------------

TEST (linear_designer_is_deterministic_symmetric_and_flags_flat_sets)
{
    LinearPhaseDesigner d1, d2;
    KernelSet s1, s2;

    for (int mode : modes)
    {
        ParamValues pv;
        setBand (pv, 2, true, BandType::bell, 1500.0f, 7.0f, 1.2f);
        setBand (pv, 4, true, BandType::highShelf, 6000.0f, -4.0f, 0.71f);
        pv.set (paramPhaseMode, static_cast<float> (mode));

        d1.design (pv, 48000.0, 5, s1);
        d2.design (pv, 48000.0, 5, s2);
        CHECK (s1.mode == mode && s1.numPartitions == linear::partitionsForMode (mode));
        CHECK (!s1.identity && !s1.crossTerms && s1.sequence == 5 && s1.sampleRate == 48000.0);

        const int c = linear::centreTap (mode);
        CHECK (d1.tapCount() == 2 * c + 1);
        for (int k : { linear::kernelLL, linear::kernelRR })
        {
            const float* t = d1.taps (k);
            CHECK (t != nullptr);
            bool symmetric = t[0] == 0.0f && t[2 * c] == 0.0f; // the Blackman window is zero at both ends
            for (int j = 0; j <= c; ++j)
                symmetric = symmetric && t[c - j] == t[c + j];
            CHECK (symmetric);
            CHECK (t[c] > 0.5f); // a small boost / cut: the centre tap dominates
            for (int p = 0; p < s1.numPartitions && p < 6; ++p)
                CHECK (std::memcmp (s1.partition (k, p), s2.partition (k, p), sizeof (float) * linear::partitionStride) == 0);
        }
        CHECK (d1.taps (linear::kernelLR) == nullptr && d1.taps (linear::kernelRL) == nullptr);
        CHECK (std::memcmp (d1.taps (linear::kernelLL), d2.taps (linear::kernelLL), sizeof (float) * static_cast<size_t> (2 * c + 1)) == 0);

        // an LL kernel that equals the RR kernel (no Left / Right band) is bit-equal
        CHECK (std::memcmp (d1.taps (linear::kernelLL), d1.taps (linear::kernelRR), sizeof (float) * static_cast<size_t> (2 * c + 1)) == 0);

        // flat sets are recognised: all bands off, and bands that do nothing
        ParamValues flat;
        flat.set (paramPhaseMode, static_cast<float> (mode));
        d1.design (flat, 48000.0, 6, s1);
        CHECK (s1.identity && !s1.crossTerms && s1.mode == mode);
        setBand (flat, 1, true, BandType::bell, 800.0f, 0.0f, 1.0f);
        setBand (flat, 5, true, BandType::lowShelf, 200.0f, 0.0f, 0.71f);
        d1.design (flat, 48000.0, 7, s1);
        CHECK (s1.identity);
        setBand (flat, 5, true, BandType::lowShelf, 200.0f, 0.5f, 0.71f);
        d1.design (flat, 48000.0, 8, s1);
        CHECK (!s1.identity);

        // a Mid / Side band turns on the cross terms
        ParamValues ms;
        setBand (ms, 0, true, BandType::bell, 1000.0f, 6.0f, 1.0f, 1, Placement::mid);
        ms.set (paramPhaseMode, static_cast<float> (mode));
        d1.design (ms, 48000.0, 9, s1);
        CHECK (!s1.identity && s1.crossTerms);
        CHECK (d1.taps (linear::kernelLR) != nullptr && d1.taps (linear::kernelRL) != nullptr);
    }

    // minimum phase asks for nothing
    ParamValues minimum;
    d1.design (minimum, 48000.0, 1, s1);
    CHECK (s1.mode == 0);
}

// ---------------------------------------------------------------------------------------------------------
// Latency, null, phase linearity
// ---------------------------------------------------------------------------------------------------------

TEST (linear_latency_is_exactly_half_n_for_every_host_block_size)
{
    for (int mode : modes)
    {
        const int half = lengthOf (mode) / 2;
        for (int block : { 1, 7, 128, 4096 })
        {
            // flat (identity kernel): an impulse at sample 0 comes out at sample N / 2, bit exact, nothing else
            ParamValues pv = withMode (ParamValues(), mode);
            auto e = makeLinear (48000.0, pv);
            const int len = half + 600;
            std::vector<float> l (static_cast<size_t> (len), 0.0f), r (static_cast<size_t> (len), 0.0f);
            l[0] = 1.0f;
            r[3] = -0.5f;
            processBlocks (*e, pv, l.data(), r.data(), len, block);
            bool exact = true;
            for (int i = 0; i < len; ++i)
            {
                exact = exact && l[static_cast<size_t> (i)] == (i == half ? 1.0f : 0.0f);
                exact = exact && r[static_cast<size_t> (i)] == (i == half + 3 ? -0.5f : 0.0f);
            }
            CHECK (exact);
            CHECK (e->getLatencySamples() == half);
            CHECK (e->latencyForMode (mode) == half);
        }
    }

    // a real filter: the response is symmetric around N / 2 (checked through the phase below as well)
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 2000.0f, 9.0f, 1.0f);
    for (int mode : modes)
    {
        const int half = lengthOf (mode) / 2;
        const auto ir = impulseResponse (withMode (pv, mode), 48000.0, lengthOf (mode) + 400, 0, 0, 7);
        double worst = 0.0, peak = 0.0;
        for (int d = 1; d < half - 128; ++d)
        {
            worst = std::max (worst, static_cast<double> (std::fabs (ir[static_cast<size_t> (half + d)] - ir[static_cast<size_t> (half - d)])));
            peak = std::max (peak, static_cast<double> (std::fabs (ir[static_cast<size_t> (half + d)])));
        }
        CHECK (worst < 2.0e-6);
        CHECK (peak > 1.0e-3);
        // nothing before the earliest tap (B) of the FIR or after its last: the response lives in [B, N - B]
        bool outside = true;
        for (int i = 0; i < linear::hop; ++i)
            outside = outside && ir[static_cast<size_t> (i)] == 0.0f;
        for (int i = lengthOf (mode) - linear::hop + 2; i < static_cast<int> (ir.size()); ++i)
            outside = outside && std::fabs (ir[static_cast<size_t> (i)]) < 1.0e-7f;
        CHECK (outside);
    }
}

TEST (linear_group_delay_is_constant_and_equal_to_n_over_2)
{
    ParamValues pv;
    setBand (pv, 1, true, BandType::bell, 1500.0f, 8.0f, 1.0f);
    setBand (pv, 4, true, BandType::highShelf, 5000.0f, -6.0f, 0.71f);
    setBand (pv, 6, true, BandType::lowCut, 120.0f, 0.0f, 0.707f, 3);

    for (int mode : modes)
    {
        const int N = lengthOf (mode);
        const auto ir = impulseResponse (withMode (pv, mode), 48000.0, N + 400, 0, 0, 128);
        int tested = 0;
        double worst = 0.0;
        for (double f : logFreqs (150.0, 18000.0, 60))
        {
            const double w = 2.0 * kPi * f / 48000.0;
            std::complex<double> h = dtft (ir, w);
            if (std::abs (h) < 0.05)
                continue;
            // zero phase after removing a delay of exactly N / 2 samples: the imaginary part vanishes
            h *= std::polar (1.0, w * N / 2.0);
            worst = std::max (worst, std::fabs (h.imag()) / std::abs (h));
            CHECK (h.real() > 0.0);
            ++tested;
        }
        // 1e-3 rad of phase error at 18 kHz is 0.01 samples of group delay
        if (worst > 1.0e-3)
            std::printf ("    N %d: worst phase error %g rad\n", N, worst);
        CHECK (worst < 1.0e-3);
        CHECK (tested > 40);
    }
}

TEST (linear_flat_is_bit_identical_delayed_input)
{
    const auto in = noise (30000, 21, 0.9f);
    for (int mode : modes)
    {
        const int half = lengthOf (mode) / 2;

        ParamValues bandsOff = withMode (ParamValues(), mode);
        ParamValues flatBands = bandsOff; // enabled bands that do nothing
        setBand (flatBands, 0, true, BandType::bell, 400.0f, 0.0f, 1.0f);
        setBand (flatBands, 3, true, BandType::highShelf, 4000.0f, 0.0f, 0.71f);
        setBand (flatBands, 5, true, BandType::bell, 900.0f, 0.0f, 2.0f, 1, Placement::mid);

        for (const ParamValues& pv : { bandsOff, flatBands })
            for (int block : { 1, 33, 480, 5000 })
            {
                auto l = in, r = in;
                std::reverse (r.begin(), r.end());
                const auto rIn = r;
                auto e = makeLinear (44100.0, pv);
                processBlocks (*e, pv, l.data(), r.data(), 30000, block);
                bool exact = true;
                for (int i = 0; i < 30000; ++i)
                {
                    const float expectL = i >= half ? in[static_cast<size_t> (i - half)] : 0.0f;
                    const float expectR = i >= half ? rIn[static_cast<size_t> (i - half)] : 0.0f;
                    exact = exact && l[static_cast<size_t> (i)] == expectL && r[static_cast<size_t> (i)] == expectR;
                }
                CHECK (exact);
            }

        // mono entry point
        auto m = in;
        auto e = makeLinear (44100.0, bandsOff);
        for (int pos = 0; pos < 30000; pos += 1000)
            e->processMono (bandsOff, m.data() + pos, 1000);
        bool exact = true;
        for (int i = 0; i < 30000; ++i)
            exact = exact && m[static_cast<size_t> (i)] == (i >= half ? in[static_cast<size_t> (i - half)] : 0.0f);
        CHECK (exact);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Magnitude
// ---------------------------------------------------------------------------------------------------------

namespace
{
// Below this level the windowed FIR is not compared: the response of a steep skirt (a 24 dB/oct cut, the centre of a
// notch) changes by tens of dB within the main lobe of the window (+-3 bins of a Blackman window), so the smoothing
// the window applies dominates the deviation there. 4 x (sr / N) is a bound for smooth features, not for those.
constexpr double stopBandDb = -24.0;

struct Case
{
    const char* name;
    BandType type;
    double freq, gain, q;
    int slope;
    double minBins = 12.0; // the corner / centre frequency is at least this many FFT bins (sr / N) wide
};

const Case cases[] = {
    { "bell +9 dB", BandType::bell, 1500.0, 9.0, 1.2, 1 },
    { "bell -12 dB", BandType::bell, 3000.0, -12.0, 0.8, 1 },
    { "low shelf +6 dB", BandType::lowShelf, 400.0, 6.0, 0.71, 1 },
    { "high shelf -5 dB", BandType::highShelf, 5000.0, -5.0, 0.71, 1 },
    { "low cut 12 dB/oct", BandType::lowCut, 300.0, 0.0, 0.707, 1, 60.0 },
    { "low cut 48 dB/oct", BandType::lowCut, 300.0, 0.0, 0.707, 5, 90.0 },
    { "high cut 36 dB/oct", BandType::highCut, 6000.0, 0.0, 0.707, 4, 40.0 },
    { "high cut 12 dB/oct", BandType::highCut, 8000.0, 0.0, 0.707, 1 },
    { "notch", BandType::notch, 2500.0, 0.0, 0.7, 1 },
    { "band pass", BandType::bandPass, 1200.0, 0.0, 0.8, 1 },
};
} // namespace

TEST (linear_magnitude_matches_the_analytic_minimum_phase_curve)
{
    int measurements = 0;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (int mode : modes)
        {
            const int N = lengthOf (mode);
            const double lo = 4.0 * fs / N, hi = 0.4 * fs;
            for (const Case& c : cases)
            {
                ParamValues pv;
                setBand (pv, 3, true, c.type, static_cast<float> (featureHz (c.freq, fs, mode, c.minBins)), static_cast<float> (c.gain),
                         static_cast<float> (c.q), c.slope);
                const auto ir = impulseResponse (withMode (pv, mode), fs, N + 400, 0, 0, 128);

                double worst = 0.0, atHz = 0.0;
                for (double f : logFreqs (lo, hi, 48))
                {
                    const double ref = analyticDb (pv, fs, f);
                    if (ref < stopBandDb)
                        continue; // deep notch / stop band: see stopBandDb
                    const double got = toDb (std::abs (dtft (ir, 2.0 * kPi * f / fs)));
                    if (std::fabs (got - ref) > worst)
                    {
                        worst = std::fabs (got - ref);
                        atHz = f;
                    }
                    ++measurements;
                }
                if (worst > 0.15)
                    std::printf ("    fs %.0f N %d %s @ %.0f Hz: worst deviation %.3f dB at %.0f Hz\n", fs, N, c.name,
                                 featureHz (c.freq, fs, mode, c.minBins), worst, atHz);
                CHECK (worst <= 0.15);
            }
        }
    CHECK (measurements > 3000);
}

TEST (linear_sine_and_noise_through_the_engine)
{
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 1000.0f, 9.0f, 1.0f);
    setBand (pv, 5, true, BandType::highShelf, 6000.0f, -6.0f, 0.71f);
    const double fs = 48000.0;

    for (int mode : modes)
    {
        const int N = lengthOf (mode);
        const ParamValues lin = withMode (pv, mode);
        for (double f : { 250.0, 1000.0, 2400.0, 9000.0 })
        {
            auto e = makeLinear (fs, lin);
            const int total = N + 24000;
            std::vector<float> l (static_cast<size_t> (total)), r;
            for (int i = 0; i < total; ++i)
                l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (2.0 * kPi * f * i / fs));
            r = l;
            processBlocks (*e, lin, l.data(), r.data(), total, 480);
            const double amp = lockInAmplitude (l.data() + N, 20000, 2.0 * kPi * f / fs);
            const double expected = analyticDb (pv, fs, f);
            CHECK_NEAR (toDb (amp / 0.5), expected, 0.05);
            // both channels carry the same
            CHECK (bitIdentical (l, r));
        }
    }

    // white noise through the partitioned convolver equals the direct convolution with the designed taps
    ParamValues ms;
    setBand (ms, 0, true, BandType::bell, 1200.0f, 8.0f, 1.0f, 1, Placement::mid);
    setBand (ms, 1, true, BandType::bell, 3000.0f, -7.0f, 1.5f, 1, Placement::side);
    setBand (ms, 2, true, BandType::lowShelf, 300.0f, 5.0f, 0.71f, 1, Placement::left);
    setBand (ms, 3, true, BandType::bell, 700.0f, -6.0f, 1.0f, 1, Placement::right);
    setBand (ms, 4, true, BandType::highShelf, 8000.0f, 3.0f, 0.71f);
    for (int mode : modes)
    {
        const ParamValues lin = withMode (ms, mode);
        LinearPhaseDesigner designer;
        KernelSet set;
        designer.design (lin, fs, 1, set);
        CHECK (set.crossTerms && !set.identity);

        const int n = 6000;
        const auto inL = noise (n, 31, 0.5f), inR = noise (n, 32, 0.5f);
        auto outL = inL, outR = inR;
        auto e = makeLinear (fs, lin);
        processBlocks (*e, lin, outL.data(), outR.data(), n, 100);

        const int c = linear::centreTap (mode);
        const int half = lengthOf (mode) / 2;
        // reference y_o[m] = sum_i sum_j taps_{o,i}[j] x_i[m - B - j], B = hop; skip the first block (kernel hand-over)
        double worst = 0.0, scale = 0.0;
        for (int m = half + 3 * linear::hop; m < n; m += 3)
        {
            for (int o = 0; o < 2; ++o)
            {
                double acc = 0.0;
                for (int i = 0; i < 2; ++i)
                {
                    const float* t = designer.taps (2 * o + i);
                    const std::vector<float>& x = i == 0 ? inL : inR;
                    for (int j = 1; j < 2 * c; ++j)
                    {
                        const int idx = m - linear::hop - j;
                        if (idx >= 0)
                            acc += static_cast<double> (t[j]) * x[static_cast<size_t> (idx)];
                    }
                }
                const double got = (o == 0 ? outL : outR)[static_cast<size_t> (m)];
                worst = std::max (worst, std::fabs (got - acc));
                scale = std::max (scale, std::fabs (acc));
            }
        }
        if (worst > 2.0e-5)
            std::printf ("    N %d: convolver vs direct convolution %g (signal peak %g)\n", lengthOf (mode), worst, scale);
        CHECK (worst < 2.0e-5);
        CHECK (scale > 0.1);
    }
}

TEST (linear_placements_match_an_explicit_matrix_reference)
{
    // Mid, Side, Left, Right and Stereo bands together; the reference multiplies explicit 2x2 matrices per frequency
    // from the single band responses (eq_curve.h), independent of the designer's grouping.
    const double fs = 48000.0;
    ParamValues pv;
    setBand (pv, 0, true, BandType::bell, 1500.0f, 8.0f, 1.0f, 1, Placement::mid);
    setBand (pv, 1, true, BandType::bell, 4000.0f, -9.0f, 1.2f, 1, Placement::side);
    setBand (pv, 2, true, BandType::bell, 900.0f, 6.0f, 0.9f, 1, Placement::left);
    setBand (pv, 3, true, BandType::highShelf, 3000.0f, -5.0f, 0.71f, 1, Placement::right);
    setBand (pv, 4, true, BandType::lowShelf, 500.0f, 3.5f, 0.71f);
    setBand (pv, 5, true, BandType::bell, 2200.0f, -4.0f, 1.0f, 1, Placement::mid);

    const auto bandMag = [&] (int b, double f)
    {
        const BandParams p = pv.band (b);
        const float fq = static_cast<float> (f);
        float db = 0.0f;
        computeBandResponse (p, fs, &fq, &db, nullptr, 1);
        return std::pow (10.0, db / 20.0);
    };
    const auto placementMag = [&] (Placement place, double f)
    {
        double m = 1.0;
        for (int b = 0; b < numBands; ++b)
            if (pv.band (b).on && pv.band (b).place == place)
                m *= bandMag (b, f);
        return m;
    };

    for (int mode : modes)
    {
        const int N = lengthOf (mode);
        const ParamValues lin = withMode (pv, mode);
        std::vector<float> h[2][2]; // [out][in]
        for (int in = 0; in < 2; ++in)
            for (int out = 0; out < 2; ++out)
                h[out][in] = impulseResponse (lin, fs, N + 400, in, out, 128);

        double worstDb = 0.0;
        for (double f : logFreqs (std::max (400.0, 12.0 * fs / N), 0.4 * fs, 40))
        {
            const double st = placementMag (Placement::stereo, f), l = placementMag (Placement::left, f) * st,
                         r = placementMag (Placement::right, f) * st, m = placementMag (Placement::mid, f),
                         s = placementMag (Placement::side, f);
            // Dec diag (m, s) Enc diag (l, r)
            const double a = 0.5 * (m + s), b = 0.5 * (m - s);
            const double ref[2][2] = { { a * l, b * r }, { b * l, a * r } };
            for (int out = 0; out < 2; ++out)
                for (int in = 0; in < 2; ++in)
                {
                    const std::complex<double> got = dtft (h[out][in], 2.0 * kPi * f / fs) * std::polar (1.0, 2.0 * kPi * f / fs * N / 2.0);
                    const double refMag = ref[out][in];
                    // the entry is real (zero phase): compare magnitude with sign
                    CHECK (std::fabs (got.imag()) < 2.0e-3 * (1.0 + std::fabs (refMag)));
                    if (std::fabs (refMag) > 0.05)
                        worstDb = std::max (worstDb, std::fabs (toDb (std::fabs (got.real())) - toDb (std::fabs (refMag))));
                    else
                        CHECK (std::fabs (got.real() - refMag) < 0.01);
                    CHECK ((got.real() >= 0.0) == (refMag >= 0.0) || std::fabs (refMag) < 0.05);
                }
        }
        if (worstDb > 0.15)
            std::printf ("    N %d: matrix entries deviate by %.3f dB\n", N, worstDb);
        CHECK (worstDb <= 0.15);
    }

    // Left / Right bands (no mixing of the channels) equal the channel views of eq_curve (minimum phase) exactly
    ParamValues lr;
    setBand (lr, 0, true, BandType::bell, 1500.0f, 8.0f, 1.0f, 1, Placement::left);
    setBand (lr, 1, true, BandType::highShelf, 4000.0f, -6.0f, 0.71f, 1, Placement::right);
    setBand (lr, 2, true, BandType::bell, 800.0f, 3.0f, 1.0f);
    const auto irLeft = impulseResponse (withMode (lr, 2), fs, 4096 + 400, 0, 0, 128);
    const auto irRight = impulseResponse (withMode (lr, 2), fs, 4096 + 400, 1, 1, 128);
    const auto irLeakage = impulseResponse (withMode (lr, 2), fs, 4096 + 400, 0, 1, 128);
    for (double f : logFreqs (500.0, 15000.0, 20))
    {
        CHECK_NEAR (toDb (std::abs (dtft (irLeft, 2.0 * kPi * f / fs))), analyticDb (lr, fs, f, ChannelView::left), 0.15);
        CHECK_NEAR (toDb (std::abs (dtft (irRight, 2.0 * kPi * f / fs))), analyticDb (lr, fs, f, ChannelView::right), 0.15);
    }
    for (float x : irLeakage)
        CHECK (x == 0.0f); // no Mid / Side band: the channels stay separate (bit exact zero crosstalk)
}

// ---------------------------------------------------------------------------------------------------------
// Block size independence, bypass, gains
// ---------------------------------------------------------------------------------------------------------

TEST (linear_output_is_independent_of_the_host_block_size)
{
    ParamValues pv;
    setBand (pv, 1, true, BandType::bell, 1800.0f, 7.0f, 1.0f);
    setBand (pv, 2, true, BandType::bell, 600.0f, -5.0f, 1.0f, 1, Placement::mid);
    setBand (pv, 6, true, BandType::highCut, 9000.0f, 0.0f, 0.707f, 3);
    pv.set (paramOutGain, -3.0f);
    const auto in = noise (20000, 77, 0.4f);

    for (int mode : modes)
    {
        const ParamValues lin = withMode (pv, mode);
        std::vector<float> ref;
        for (int block : { 4096, 1, 7, 32, 64, 127, 128, 129, 1000 })
        {
            auto l = in, r = in;
            auto e = makeLinear (48000.0, lin);
            processBlocks (*e, lin, l.data(), r.data(), 20000, block);
            if (ref.empty())
                ref = l;
            CHECK (bitIdentical (l, ref));
            CHECK (allFinite (l.data(), 20000));
        }
    }
}

TEST (linear_bypass_crossfade_uses_the_delayed_dry_signal)
{
    const double fs = 48000.0;
    for (int mode : modes)
    {
        const int half = lengthOf (mode) / 2;
        ParamValues active;
        setBand (active, 3, true, BandType::bell, 1000.0f, 9.0f, 2.0f);
        active = withMode (active, mode);
        ParamValues bypassed = active;
        bypassed.set (paramBypass, 1.0f);

        const int total = half + 48000;
        const int stepAt = half + 20000;
        const double w = 2.0 * kPi * 1000.0 / fs;
        std::vector<float> in (static_cast<size_t> (total)), l, r;
        for (int i = 0; i < total; ++i)
            in[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (w * i));
        l = in;
        r = in;

        Rig rig (fs);
        rig.engine->designSynchronously (active);
        rig.run (active, l.data(), r.data(), stepAt, 256, true);
        rig.run (bypassed, l.data() + stepAt, r.data() + stepAt, total - stepAt, 256, true);

        // before: the filter is audible (+9 dB at 1 kHz), after the 20 ms fade: exactly the delayed input
        CHECK_NEAR (toDb (lockInAmplitude (l.data() + stepAt - 4800, 4800, w) / 0.5), 9.0, 0.1);
        bool exact = true;
        for (int i = stepAt + 1100; i < total; ++i)
            exact = exact && l[static_cast<size_t> (i)] == in[static_cast<size_t> (i - half)];
        CHECK (exact);

        // click free through the fade: the output is a blend of the processed tone and the delayed tone
        double maxJump = 0.0;
        for (int i = stepAt - 200; i < stepAt + 1500; ++i)
            maxJump = std::max (maxJump, static_cast<double> (std::fabs (l[static_cast<size_t> (i)] - l[static_cast<size_t> (i) - 1])));
        CHECK (maxJump < 0.5 * 2.82 * w * 1.1);

        // release: back to the processed tone
        Rig rig2 (fs);
        rig2.engine->designSynchronously (active);
        std::vector<float> l2 = in, r2 = in;
        rig2.run (bypassed, l2.data(), r2.data(), stepAt, 256, true);
        rig2.run (active, l2.data() + stepAt, r2.data() + stepAt, total - stepAt, 256, true);
        CHECK_NEAR (toDb (lockInAmplitude (l2.data() + total - 4800, 4800, w) / 0.5), 9.0, 0.1);
        // while bypassed from the start the output is the delayed input, bit exact
        bool exactStart = true;
        for (int i = half; i < stepAt - 200; ++i)
            exactStart = exactStart && l2[static_cast<size_t> (i)] == in[static_cast<size_t> (i - half)];
        CHECK (exactStart);
    }
}

TEST (linear_output_gain_auto_gain_latency_and_tail)
{
    const double fs = 48000.0;
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 1000.0f, 12.0f, 1.0f);
    const double w = 2.0 * kPi * 1000.0 / fs;

    for (int mode : modes)
    {
        const int N = lengthOf (mode);
        const ParamValues base = withMode (pv, mode);
        auto run = [&] (const ParamValues& v)
        {
            auto e = makeLinear (fs, v);
            const int total = N + 24000;
            std::vector<float> l (static_cast<size_t> (total)), r;
            for (int i = 0; i < total; ++i)
                l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (w * i));
            r = l;
            processBlocks (*e, v, l.data(), r.data(), total, 333);
            return std::make_pair (toDb (lockInAmplitude (l.data() + N + 2000, 20000, w) / 0.5), e->getTailSamples());
        };

        const auto plain = run (base);
        CHECK_NEAR (plain.first, 12.0, 0.05);
        CHECK (plain.second >= static_cast<uint32_t> (N));

        ParamValues gained = base;
        gained.set (paramOutGain, -7.0f);
        CHECK_NEAR (run (gained).first, 5.0, 0.05);

        ParamValues autoGained = base;
        autoGained.set (paramAutoGain, 1.0f);
        CHECK_NEAR (run (autoGained).first, 12.0 + (-computeAutoGainDb (base, fs)), 0.06);

        // flat: the tail is the delay line plus the FIR
        const ParamValues flat = withMode (ParamValues(), mode);
        CHECK (run (flat).second == static_cast<uint32_t> (N));
    }
}

// ---------------------------------------------------------------------------------------------------------
// Mode switching and sweeps
// ---------------------------------------------------------------------------------------------------------

namespace
{
/** Largest sample-to-sample step of `x` in [from, to). */
double maxStep (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = std::max(from, 1); i < to; ++i)
        m = std::max (m, static_cast<double> (std::fabs (x[static_cast<size_t> (i)] - x[static_cast<size_t> (i) - 1])));
    return m;
}
} // namespace

TEST (linear_mode_switching_is_click_free_and_reaches_the_new_path)
{
    const double fs = 48000.0;
    const double w = 2.0 * kPi * 1000.0 / fs;
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 1000.0f, 9.0f, 1.0f);
    setBand (pv, 4, true, BandType::bell, 4000.0f, -6.0f, 1.0f, 1, Placement::side);

    const int seg = 24000;
    const int total = 7 * seg;
    std::vector<float> l (static_cast<size_t> (total)), r;
    for (int i = 0; i < total; ++i)
        l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (w * i));
    r = l;

    // minimum -> 2048 -> minimum -> 8192 -> 4096 (via minimum) -> minimum, a design thread that keeps up
    const int sequence[7] = { 0, 1, 0, 3, 2, 2, 0 };
    Rig rig (fs);
    for (int s = 0; s < 7; ++s)
    {
        const ParamValues v = withMode (pv, sequence[s]);
        rig.run (v, l.data() + s * seg, r.data() + s * seg, seg, 96, true);
        CHECK (allFinite (l.data() + s * seg, seg));
    }

    // 1 kHz, amplitude 0.5, the band is a +9 dB bell: 0.5 * 2.8 at the output once settled; every segment ends settled
    const double expected = 0.5 * std::pow (10.0, 9.0 / 20.0);
    for (int s = 0; s < 7; ++s)
        CHECK_NEAR (lockInAmplitude (l.data() + (s + 1) * seg - 2000, 2000, w), expected, 0.02 * expected);

    // the whole run is continuous: no step beyond the natural slope of the tone (+ a fraction for the cross-fades)
    const double natural = expected * w;
    CHECK (maxStep (l, 1, total) <= natural * 1.3);
    CHECK (rig.engine->getLatencySamples() == 0);
}

TEST (linear_parameter_sweeps_are_click_free)
{
    const double fs = 48000.0;
    const double w = 2.0 * kPi * 1000.0 / fs;
    for (int mode : { 1, 3 })
    {
        const int total = 48000 + lengthOf (mode);
        std::vector<float> l (static_cast<size_t> (total)), r;
        for (int i = 0; i < total; ++i)
            l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (w * i));
        r = l;

        Rig rig (fs);
        ParamValues pv = withMode (ParamValues(), mode);
        setBand (pv, 2, true, BandType::bell, 1000.0f, 0.0f, 1.0f);
        rig.engine->designSynchronously (pv);

        // sweep gain 0 -> +12 -> -12 and the centre frequency 700 Hz -> 1.4 kHz, a new target every 128 samples
        const int hopLen = 128;
        for (int pos = 0; pos < total; pos += hopLen)
        {
            const double t = static_cast<double> (pos) / 48000.0; // 0 .. 1 s
            pv.set (2, BandParam::gain, static_cast<float> (12.0 * std::sin (2.0 * kPi * 1.5 * t)));
            pv.set (2, BandParam::freq, static_cast<float> (1000.0 * std::pow (2.0, std::sin (2.0 * kPi * 2.0 * t))));
            rig.run (pv, l.data() + pos, r.data() + pos, std::min (hopLen, total - pos), hopLen, true);
        }

        CHECK (allFinite (l.data(), total));
        // the tone is at most 12 dB louder than the input: 0.5 * 4 = 2.0 -> natural slope 2.0 w, plus the cross-fade
        // between neighbouring kernels (never more than a few percent of the step)
        const double limit = 2.0 * w * 1.15;
        const double step = maxStep (l, lengthOf (mode) / 2 + 2000, total);
        if (step > limit)
            std::printf ("    N %d: max step %.5f (limit %.5f)\n", lengthOf (mode), step, limit);
        CHECK (step <= limit);
        CHECK (rig.engine->appliedKernelSequence() > 10);
    }

    // a hard step of the gain (no smoothing in this path): the hop cross-fade makes it continuous, not click free by
    // construction of a ramp, but the step stays within the natural slope of the larger of the two tones
    {
        Rig rig (fs);
        ParamValues a = withMode (ParamValues(), 1), b = a;
        setBand (a, 2, true, BandType::bell, 1000.0f, 0.0f, 1.0f);
        setBand (b, 2, true, BandType::bell, 1000.0f, 12.0f, 1.0f);
        const int n = 20000;
        std::vector<float> l (static_cast<size_t> (n)), r;
        for (int i = 0; i < n; ++i)
            l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (w * i));
        r = l;
        rig.engine->designSynchronously (a);
        rig.run (a, l.data(), r.data(), 10000, 128, true);
        rig.run (b, l.data() + 10000, r.data() + 10000, n - 10000, 128, true);
        CHECK (maxStep (l, 10000 + 1024 - 200, 10000 + 1024 + 2000) <= 0.5 * 4.0 * w * 1.05 + 0.001);
    }
}

TEST (linear_reset_and_sample_rate_change_restart_cleanly)
{
    const double fs = 48000.0;
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 1000.0f, 6.0f, 1.0f);
    const ParamValues lin = withMode (pv, 1);

    Rig rig (fs);
    rig.engine->designSynchronously (lin);
    auto l = noise (8000, 5, 0.5f), r = l;
    rig.run (lin, l.data(), r.data(), 8000, 200, true);

    // reset: history and kernels are dropped, the run starts from silence again and reproduces the first one bit for bit
    rig.engine->reset();
    rig.engine->designSynchronously (lin);
    auto l2 = noise (8000, 5, 0.5f), r2 = l2;
    rig.run (lin, l2.data(), r2.data(), 8000, 200, true);
    bool silent = true;
    for (int i = 0; i < linear::hop; ++i)
        silent = silent && l2[static_cast<size_t> (i)] == 0.0f; // the first block has nothing to emit yet
    CHECK (silent);
    CHECK (bitIdentical (l, l2));

    // a new sample rate: kernels of the old rate are not used
    rig.engine->prepare (96000.0, 512);
    rig.engine->designSynchronously (lin);
    auto l3 = noise (8000, 5, 0.5f), r3 = l3;
    processBlocks (*rig.engine, lin, l3.data(), r3.data(), 8000, 128);
    CHECK (allFinite (l3.data(), 8000));
    CHECK (rig.engine->appliedKernelSequence() != 0);
}

TEST (linear_extreme_parameters_and_sample_rates_stay_finite_and_bounded)
{
    // random parameter sets over the whole range (all types, placements, slopes, +-24 dB, Q 0.1 .. 18, 10 Hz .. 30 kHz)
    // at sample rates from 8 kHz to 384 kHz, stereo and mono, with NaN / Inf in the input: finite output, no blow-up
    Rng rng (2718);
    for (double fs : { 8000.0, 22050.0, 48000.0, 192000.0, 384000.0 })
        for (int mode : modes)
        {
            Rig rig (fs);
            for (int round = 0; round < 3; ++round)
            {
                ParamValues pv = withMode (ParamValues(), mode);
                for (int b = 0; b < numBands; ++b)
                {
                    const BandType type = static_cast<BandType> (rng.integer (0, 6));
                    // log-uniform frequency and Q
                    const float freq = static_cast<float> (10.0 * std::pow (3000.0, rng.range (0.0, 1.0)));
                    const float q = static_cast<float> (0.1 * std::pow (180.0, rng.range (0.0, 1.0)));
                    setBand (pv, b, rng.integer (0, 3) != 0, type, freq, static_cast<float> (rng.range (-24.0, 24.0)), q,
                             rng.integer (0, 5), static_cast<Placement> (rng.integer (0, 4)));
                }
                if (round == 2)
                    pv.set (paramAutoGain, 1.0f);

                const int n = 6000;
                auto l = noise (n, 100 + static_cast<uint32_t> (round), 0.5f), r = l;
                l[10] = std::nanf ("");
                r[20] = INFINITY;
                rig.engine->designSynchronously (pv);
                rig.run (pv, l.data(), r.data(), n, 128, true);
                CHECK (allFinite (l.data(), n) && allFinite (r.data(), n));
                CHECK (!anySubnormal (l.data(), n));

                // mono entry point on the same engine
                auto m = noise (2000, 9, 0.5f);
                rig.engine->processMono (pv, m.data(), 2000);
                CHECK (allFinite (m.data(), 2000));

                // everything the designer produced is finite and the engine's own bound holds (< 1e30 or silenced)
                float peak = 0.0f;
                for (int i = 0; i < n; ++i)
                    peak = std::max (peak, std::max (std::fabs (l[static_cast<size_t> (i)]), std::fabs (r[static_cast<size_t> (i)])));
                CHECK (peak < 1.0e30f);
            }
        }
}

// ---------------------------------------------------------------------------------------------------------
// Design thread
// ---------------------------------------------------------------------------------------------------------

TEST (linear_design_thread_follows_the_engine_and_sleeps_in_minimum_phase)
{
    const double fs = 48000.0;
    auto engine = makeEngine (fs);
    DesignThread thread;
    CHECK (thread.start (*engine->designLink()));
    CHECK (!thread.start (*engine->designLink())); // already running
    CHECK (thread.running());

    // minimum phase: nothing is requested, nothing is designed, even when the thread is told to be active
    ParamValues moved;
    ParamValues pv;
    setBand (pv, 2, true, BandType::bell, 1000.0f, 9.0f, 1.0f);
    thread.setActive (true);
    std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
    // the audio side only picks kernels up while it processes: keep feeding it while waiting
    const auto waitFor = [&] (auto pred)
    {
        for (int i = 0; i < 2000; ++i) // up to 10 s
        {
            if (pred())
                return true;
            processBlocks (*engine, moved, l.data(), r.data(), 256, 128);
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        return false;
    };

    processBlocks (*engine, pv, l.data(), r.data(), 4096, 256);
    std::this_thread::sleep_for (std::chrono::milliseconds (60));
    CHECK (engine->paramSequence() == 0 && thread.designsDone() == 0 && engine->appliedKernelSequence() == 0);

    // switch to linear phase: the engine asks, the thread designs, the engine applies
    const ParamValues lin = withMode (pv, 2);
    moved = lin;
    for (int i = 0; i < 400 && engine->appliedKernelSequence() == 0; ++i)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        processBlocks (*engine, lin, l.data(), r.data(), 1024, 128);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    CHECK (engine->paramSequence() >= 1);
    CHECK (engine->appliedKernelSequence() != 0);

    // a parameter change gets a newer design, throttled to the design interval
    const uint64_t before = thread.designsDone();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i)
    {
        moved.set (2, BandParam::gain, 9.0f + 0.1f * static_cast<float> (i));
        processBlocks (*engine, moved, l.data(), r.data(), 128, 128);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    const double elapsedMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    CHECK (waitFor ([&] { return engine->appliedKernelSequence() == engine->paramSequence(); }));
    const uint64_t designs = thread.designsDone() - before;
    CHECK (designs >= 1);
    CHECK (designs <= static_cast<uint64_t> (elapsedMs / DesignThread::minIntervalMs) + 3); // throttled

    // inactive again: no work
    thread.setActive (false);
    std::this_thread::sleep_for (std::chrono::milliseconds (40));
    const uint64_t frozen = thread.designsDone();
    moved.set (2, BandParam::gain, -3.0f);
    processBlocks (*engine, moved, l.data(), r.data(), 128, 128);
    std::this_thread::sleep_for (std::chrono::milliseconds (60));
    CHECK (thread.designsDone() == frozen);

    thread.stop();
    CHECK (!thread.running());
    thread.stop(); // idempotent
}

#else // !KICKEQ_LINEAR_PHASE

#include "test_util.h"

TEST (linear_phase_disabled_build_is_minimum_phase_only)
{
    using namespace tu;
    CHECK (!linearPhaseOn());
    ParamValues pv;
    pv.set (paramPhaseMode, 3.0f);
    CHECK (pv.phaseMode() == 0 && pv.get (paramPhaseMode) == 0.0f);

    auto e = makeEngine (48000.0);
    CHECK (!e->linearPhaseAvailable() && e->designLink() == nullptr);
    CHECK (e->latencyForMode (3) == 0 && e->getLatencySamples() == 0);
    e->requestDesign (pv); // no-ops
    e->designSynchronously (pv);
    CHECK (e->paramSequence() == 0 && e->appliedKernelSequence() == 0);

    // the whole engine behaves as before: flat is bit transparent with zero latency
    auto in = noise (1000, 3, 0.5f), l = in, r = in;
    e->process (pv, l.data(), r.data(), 1000);
    CHECK (bitIdentical (l, in));
}

#endif // KICKEQ_LINEAR_PHASE
