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

// Spectrum analyser: SPSC ring stress, peak position / level, enable / source handling, frame hand-off.

#include <atomic>
#include <thread>

#include "kickeq/analyzer.h"
#include "test_util.h"

using namespace tu;
using namespace kickeq;

namespace
{
/** Feeds a sine (amplitude ampPre on the input, ampPost on the output) and returns the last frame. */
bool feedSine (SpectrumAnalyzer& a, double fs, double freq, float ampPre, float ampPost, double seconds, SpectrumFrame& frame,
               bool sideSignal = false)
{
    const int blockSize = 256;
    const int blocks = static_cast<int> (seconds * fs / blockSize);
    std::vector<float> preL (blockSize), preR (blockSize), postL (blockSize), postR (blockSize);
    bool got = false;
    long long t = 0;

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i, ++t)
        {
            const float s = static_cast<float> (std::sin (2.0 * kPi * freq * static_cast<double> (t) / fs));
            preL[static_cast<size_t> (i)] = ampPre * s;
            preR[static_cast<size_t> (i)] = sideSignal ? -ampPre * s : ampPre * s;
            postL[static_cast<size_t> (i)] = ampPost * s;
            postR[static_cast<size_t> (i)] = sideSignal ? -ampPost * s : ampPost * s;
        }
        a.push (preL.data(), preR.data(), postL.data(), postR.data(), blockSize);
        if (b % 7 == 0)
        {
            a.process();
            got = a.pull (frame) || got;
        }
    }
    a.process();
    got = a.pull (frame) || got;
    return got;
}

int peakBin (const std::vector<float>& db)
{
    return static_cast<int> (std::max_element (db.begin(), db.end()) - db.begin());
}
} // namespace

TEST (analyzer_ring_spsc_stress_preserves_order)
{
    SpscFloatRing ring (1 << 12);
    CHECK (ring.capacity() == 4096);
    CHECK (ring.writeSpace() == 4096);
    CHECK (ring.readAvailable() == 0);

    const int target = 10000000;
    std::atomic<bool> producerDone { false };
    std::atomic<int> produced { 0 };

    std::thread producer ([&]
    {
        Rng rng (1);
        std::vector<float> block (700);
        int next = 0; // value of the next sample that is accepted
        while (next < target)
        {
            const int n = rng.integer (1, 700);
            if (ring.writeSpace() < static_cast<size_t> (n))
                continue; // full: retry later (the real analyser would drop the block)
            for (int i = 0; i < n; ++i)
                block[static_cast<size_t> (i)] = static_cast<float> ((next + i) % 16777216); // exact in float
            ring.write (block.data(), static_cast<size_t> (n));
            next += n;
            produced.store (next, std::memory_order_relaxed);
        }
        producerDone.store (true);
    });

    long long consumed = 0;
    bool inOrder = true;
    std::vector<float> buf (500);
    Rng rng (2);
    for (;;)
    {
        const bool done = producerDone.load(); // read before draining: everything written before `done` is visible
        const size_t got = ring.read (buf.data(), static_cast<size_t> (rng.integer (1, 500)));
        for (size_t i = 0; i < got; ++i)
            inOrder = inOrder && buf[i] == static_cast<float> ((consumed + static_cast<long long> (i)) % 16777216);
        consumed += static_cast<long long> (got);
        if (done && got == 0)
            break;
    }
    producer.join();

    CHECK (inOrder);
    CHECK (consumed >= target);
    CHECK (consumed == produced.load());
    CHECK (ring.readAvailable() == 0);
}

TEST (analyzer_sine_peak_lands_in_the_right_display_bin_and_levels_scale)
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (double freq : { 100.0, 440.0, 1000.0, 5000.0, 12000.0 })
        {
            SpectrumAnalyzer a (512);
            a.prepare (fs);
            a.setEnabled (true);
            SpectrumFrame frame;
            CHECK (feedSine (a, fs, freq, 0.5f, 0.25f, 1.0, frame));
            CHECK (frame.preDb.size() == 512 && frame.postDb.size() == 512);

            // the display bin of the peak is within one display bin of the tone (1.6 % at 512 bins), or within
            // half an FFT bin (fs / 4096) where the FFT itself is coarser than that (low frequencies)
            const int pk = peakBin (frame.preDb);
            const double tolHz = std::max (freq * 0.0158 * 1.5, 0.55 * fs / SpectrumAnalyzer::fftSize);
            if (std::fabs (a.binFrequencyHz (pk) - freq) > tolHz)
                std::printf ("    fs %.0f f %.0f: peak bin %d is %.1f Hz\n", fs, freq, pk, a.binFrequencyHz (pk));
            CHECK (std::fabs (a.binFrequencyHz (pk) - freq) <= tolHz);
            CHECK (peakBin (frame.postDb) == pk);

            // level: -6 dB tone (before the 1/6 octave power smoothing, which can only lower it); pre / post
            // differ by exactly 6.02 dB
            const float peakPre = frame.preDb[static_cast<size_t> (pk)], peakPost = frame.postDb[static_cast<size_t> (pk)];
            CHECK (peakPre <= -5.5f && peakPre >= -45.0f);
            CHECK_NEAR (peakPre - peakPost, 6.0206, 0.1);
            CHECK (frame.sampleRate == fs);
        }
}

TEST (analyzer_display_bins_are_log_spaced_10hz_to_30khz)
{
    SpectrumAnalyzer a (512);
    CHECK (a.numBins() == 512);
    CHECK_NEAR (a.binFrequencyHz (0), 10.0, 1.0e-4);
    CHECK_NEAR (a.binFrequencyHz (511), 30000.0, 0.01);
    for (int i = 1; i < 512; ++i)
        CHECK_NEAR (a.binFrequencyHz (i) / a.binFrequencyHz (i - 1), std::pow (3000.0, 1.0 / 511.0), 1.0e-4);

    SpectrumAnalyzer defaults;
    CHECK (defaults.numBins() == 512);
    SpectrumAnalyzer custom (100);
    CHECK (custom.numBins() == 100);
}

TEST (analyzer_disabled_is_a_no_op_and_pull_reports_new_frames_only)
{
    SpectrumAnalyzer a;
    std::vector<float> x (4096, 0.5f);
    SpectrumFrame frame;

    CHECK (!a.isEnabled());
    a.push (x.data(), x.data(), x.data(), x.data(), 4096);
    a.process();
    CHECK (!a.pull (frame)); // nothing was pushed while disabled
    CHECK (a.droppedSamples() == 0);

    a.setEnabled (true);
    CHECK (!a.pull (frame)); // enabled, but no data yet
    a.push (x.data(), x.data(), x.data(), x.data(), 4096);
    a.process();
    CHECK (a.pull (frame));
    const uint32_t seq = frame.sequence;
    CHECK (seq >= 1);
    CHECK (!a.pull (frame)); // same frame is not delivered twice

    a.push (x.data(), x.data(), x.data(), x.data(), 1024);
    a.process();
    CHECK (a.pull (frame));
    CHECK (frame.sequence > seq);

    // while disabled again the pushed samples are never analysed
    a.setEnabled (false);
    a.push (x.data(), x.data(), x.data(), x.data(), 4096);
    a.process();
    CHECK (!a.pull (frame));
}

TEST (analyzer_source_selection_left_plus_right_mid_side)
{
    const double fs = 48000.0;
    SpectrumFrame frame;

    // L = -R: pure side signal
    for (AnalyzerSource src : { AnalyzerSource::leftPlusRight, AnalyzerSource::mid, AnalyzerSource::side })
    {
        SpectrumAnalyzer a (256);
        a.prepare (fs);
        a.setEnabled (true);
        a.setSource (src);
        CHECK (feedSine (a, fs, 1000.0, 0.5f, 0.5f, 0.6, frame, true));
        const float peak = *std::max_element (frame.preDb.begin(), frame.preDb.end());
        if (src == AnalyzerSource::side)
            CHECK (peak > -45.0f);
        else
            CHECK (peak <= SpectrumAnalyzer::floorDb + 0.5f); // L + R = 0: silence
    }

    // L = R: pure mid signal; mono input (right pointers null) behaves like L = R
    for (AnalyzerSource src : { AnalyzerSource::leftPlusRight, AnalyzerSource::mid, AnalyzerSource::side })
    {
        SpectrumAnalyzer a (256);
        a.prepare (fs);
        a.setEnabled (true);
        a.setSource (src);
        CHECK (feedSine (a, fs, 1000.0, 0.5f, 0.5f, 0.6, frame, false));
        const float peak = *std::max_element (frame.postDb.begin(), frame.postDb.end());
        if (src == AnalyzerSource::side)
            CHECK (peak <= SpectrumAnalyzer::floorDb + 0.5f);
        else
            CHECK (peak > -45.0f);
    }

    SpectrumAnalyzer mono (256);
    mono.prepare (fs);
    mono.setEnabled (true);
    std::vector<float> x (48000);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float> (0.5 * std::sin (2.0 * kPi * 1000.0 * static_cast<double> (i) / fs));
    mono.push (x.data(), nullptr, x.data(), nullptr, 48000);
    mono.process();
    CHECK (mono.pull (frame));
    CHECK (*std::max_element (frame.preDb.begin(), frame.preDb.end()) > -45.0f);
}

TEST (analyzer_overflow_drops_whole_blocks_and_stays_consistent)
{
    SpectrumAnalyzer a (128);
    a.setEnabled (true);
    std::vector<float> x (10000, 0.1f);

    // never processed: 6 blocks fit (65536 / 10000), the other 4 are dropped as a whole
    for (int i = 0; i < 10; ++i)
        a.push (x.data(), x.data(), x.data(), x.data(), 10000);
    CHECK (a.droppedSamples() == 40000);

    SpectrumFrame frame;
    a.process();
    CHECK (a.pull (frame));
    for (float v : frame.preDb)
        CHECK (std::isfinite (v));

    // after draining there is room again
    a.push (x.data(), x.data(), x.data(), x.data(), 10000);
    CHECK (a.droppedSamples() == 40000);
}

TEST (analyzer_producer_consumer_threads_stress)
{
    const double fs = 48000.0;
    SpectrumAnalyzer a (256);
    a.prepare (fs);
    a.setEnabled (true);

    std::atomic<bool> stop { false };
    std::thread producer ([&]
    {
        Rng rng (5);
        std::vector<float> l (1000), r (1000);
        long long t = 0;
        for (int iter = 0; iter < 4000; ++iter)
        {
            const int n = rng.integer (1, 1000);
            for (int i = 0; i < n; ++i, ++t)
            {
                l[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (2.0 * kPi * 2000.0 * static_cast<double> (t) / fs));
                r[static_cast<size_t> (i)] = l[static_cast<size_t> (i)];
            }
            a.push (l.data(), r.data(), l.data(), r.data(), n);
            if ((iter & 63) == 0)
                std::this_thread::yield();
        }
        stop.store (true);
    });

    SpectrumFrame frame;
    uint32_t lastSeq = 0;
    int frames = 0;
    bool valid = true, monotonic = true;
    while (!stop.load())
    {
        a.process();
        while (a.pull (frame))
        {
            ++frames;
            monotonic = monotonic && frame.sequence > lastSeq;
            lastSeq = frame.sequence;
            for (float v : frame.preDb)
                valid = valid && std::isfinite (v);
        }
    }
    producer.join();
    a.process();
    while (a.pull (frame))
    {
        ++frames;
        monotonic = monotonic && frame.sequence > lastSeq;
        lastSeq = frame.sequence;
    }

    CHECK (valid);
    CHECK (monotonic);
    CHECK (frames > 0);
    CHECK (a.droppedSamples() < 4000u * 1000u); // sanity: counter did not run away
}

TEST (analyzer_producer_and_ui_thread_split)
{
    // process() on one thread, pull() on another (the triple buffer hand-off)
    const double fs = 48000.0;
    SpectrumAnalyzer a (256);
    a.prepare (fs);
    a.setEnabled (true);

    std::vector<float> x (480);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float> (0.5 * std::sin (2.0 * kPi * 3000.0 * static_cast<double> (i) / fs));

    std::atomic<bool> stop { false };
    std::atomic<int> processed { 0 };
    std::thread analyser ([&]
    {
        while (!stop.load())
        {
            a.process();
            processed.fetch_add (1);
        }
    });

    std::thread ui ([&]
    {
        SpectrumFrame f;
        uint32_t last = 0;
        while (!stop.load())
            if (a.pull (f))
            {
                // a frame is internally consistent: pre and post carry the same sine
                bool same = f.preDb.size() == f.postDb.size();
                for (size_t i = 0; same && i < f.preDb.size(); ++i)
                    same = f.preDb[i] == f.postDb[i];
                CHECK (same);
                CHECK (f.sequence > last);
                last = f.sequence;
            }
    });

    for (int i = 0; i < 3000; ++i)
    {
        a.push (x.data(), x.data(), x.data(), x.data(), 480);
        if ((i & 15) == 0)
            std::this_thread::yield();
    }
    stop.store(true);
    analyser.join();
    ui.join();
    CHECK (processed.load() > 0);
}
