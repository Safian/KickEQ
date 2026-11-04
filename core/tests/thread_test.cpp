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

// Threads against the linear-phase hand-over: the kernel mailbox, the design request seqlock, and a real DesignThread
// feeding an engine that is processed from another thread. Meant to be run under ThreadSanitizer as well
// (cmake -DCMAKE_CXX_FLAGS=-fsanitize=thread), where it must report nothing; without a sanitizer it is a stress test
// of the consistency of every hand-over.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "kickeq/design_thread.h"
#include "kickeq/eq_engine.h"
#include "kickeq/linear_phase.h"
#include "kickeq/params.h"

using namespace kickeq;

namespace
{
int failures = 0;

void check (bool ok, const char* what)
{
    if (!ok)
    {
        ++failures;
        std::printf ("FAILED: %s\n", what);
    }
}

using Clock = std::chrono::steady_clock;

/** Writer publishes sets stamped with a running number in the first and the last float they carry; the reader must
    always see matching stamps (a recycled or half written slot would break that). */
void mailboxStress()
{
    KernelMailbox box;
    std::atomic<bool> stop { false };

    std::thread writer ([&] {
        uint64_t n = 0;
        while (!stop.load (std::memory_order_relaxed))
        {
            ++n;
            KernelSet& s = box.writeSlot();
            s.sequence = n;
            s.mode = 1;
            s.sampleRate = 48000.0;
            s.numPartitions = linear::partitionsForMode (1);
            s.identity = false;
            const float stamp = static_cast<float> (n & 0xFFFFFF);
            s.partition (0, 0)[0] = stamp;
            s.partition (linear::numKernels - 1, linear::maxPartitions - 1)[linear::partitionStride - 1] = stamp;
            box.publish();
        }
    });

    uint64_t fetched = 0, lastSeq = 0;
    bool consistent = true, monotone = true, previousOk = true;
    const auto end = Clock::now() + std::chrono::milliseconds (800);
    while (Clock::now() < end)
    {
        if (box.fetch (1, 48000.0))
        {
            ++fetched;
            const KernelSet* c = box.current();
            const float a = c->partition (0, 0)[0];
            const float b = c->partition (linear::numKernels - 1, linear::maxPartitions - 1)[linear::partitionStride - 1];
            consistent = consistent && a == b && a == static_cast<float> (c->sequence & 0xFFFFFF);
            monotone = monotone && c->sequence > lastSeq;
            lastSeq = c->sequence;
            if (const KernelSet* p = box.previous())
            {
                // the previous set must stay intact while the reader still looks at it
                for (int spin = 0; spin < 200; ++spin)
                    previousOk = previousOk && p->partition (0, 0)[0] == p->partition (linear::numKernels - 1, linear::maxPartitions - 1)[linear::partitionStride - 1];
            }
        }
    }
    stop.store (true);
    writer.join();

    check (fetched > 100, "mailbox: reader fetched sets");
    check (consistent, "mailbox: sets are never torn");
    check (monotone, "mailbox: sets arrive in order");
    check (previousOk, "mailbox: the previous set stays intact while it is in use");
    std::printf ("mailbox: %llu sets fetched, last sequence %llu\n", static_cast<unsigned long long> (fetched),
                 static_cast<unsigned long long> (lastSeq));
}

/** Producer writes snapshots whose 60 values and sample rate all carry the same running number. */
void linkStress()
{
    DesignLink link;
    std::atomic<bool> stop { false };

    std::thread producer ([&] {
        uint64_t n = 0;
        while (!stop.load (std::memory_order_relaxed))
        {
            ++n;
            ParamValues v;
            v.set (paramPhaseMode, 1.0f);
            for (int b = 0; b < numBands; ++b)
            {
                v.set (b, BandParam::freq, 100.0f + static_cast<float> (n % 1000));
                v.set (b, BandParam::gain, static_cast<float> (n % 20));
                v.set (b, BandParam::q, 1.0f + static_cast<float> (n % 7));
            }
            link.request (v, 1000.0 + static_cast<double> (n % 1000));
        }
    });

    uint64_t reads = 0, lastSeq = 0;
    bool consistent = true, monotone = true;
    const auto end = Clock::now() + std::chrono::milliseconds (800);
    while (Clock::now() < end)
    {
        ParamValues v;
        double sr = 0.0;
        const uint64_t seq = link.readRequest (v, sr);
        if (seq == 0)
            continue;
        ++reads;
        const uint64_t n = seq;
        for (int b = 0; b < numBands; ++b)
        {
            consistent = consistent && v.get (b, BandParam::freq) == 100.0f + static_cast<float> (n % 1000)
                         && v.get (b, BandParam::gain) == static_cast<float> (n % 20)
                         && v.get (b, BandParam::q) == 1.0f + static_cast<float> (n % 7);
        }
        consistent = consistent && sr == 1000.0 + static_cast<double> (n % 1000);
        monotone = monotone && seq >= lastSeq;
        lastSeq = seq;
    }
    stop.store (true);
    producer.join();

    check (reads > 1000, "link: snapshots were read");
    check (consistent, "link: snapshots are consistent");
    check (monotone, "link: sequence never goes back");
    std::printf ("link: %llu snapshots read, %llu requests posted\n", static_cast<unsigned long long> (reads),
                 static_cast<unsigned long long> (link.requestSequence()));
}

/** An engine processed from this thread while a DesignThread follows it and the parameters keep moving. */
void engineWithDesignThread()
{
    EqEngine engine;
    engine.prepare (48000.0, 256);
    DesignThread thread;
    check (thread.start (*engine.designLink()), "design thread starts");
    thread.setActive (true);

    ParamValues pv;
    pv.set (paramPhaseMode, 2.0f);
    for (int b : { 1, 3, 5 })
    {
        pv.set (b, BandParam::on, 1.0f);
        pv.set (b, BandParam::freq, 300.0f * static_cast<float> (b));
        pv.set (b, BandParam::gain, 6.0f);
    }
    pv.set (4, BandParam::on, 1.0f);
    pv.set (4, BandParam::place, static_cast<float> (Placement::mid));

    std::vector<float> l (256), r (256);
    uint32_t seed = 3;
    bool finite = true;
    uint64_t lastApplied = 0;
    bool appliedMonotone = true;
    const auto end = Clock::now() + std::chrono::milliseconds (1500);
    int block = 0;
    while (Clock::now() < end)
    {
        for (int i = 0; i < 256; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            l[static_cast<size_t> (i)] = static_cast<float> (static_cast<int32_t> (seed)) * (0.4f / 2147483648.0f);
            r[static_cast<size_t> (i)] = l[static_cast<size_t> (i)];
        }
        // moving parameters, and a mode round trip in the middle
        pv.set (3, BandParam::gain, static_cast<float> ((block % 24) - 12));
        pv.set (1, BandParam::freq, 300.0f + static_cast<float> (block % 500));
        if (block == 300)
            pv.set (paramPhaseMode, 0.0f);
        if (block == 360)
            pv.set (paramPhaseMode, 3.0f);
        engine.process (pv, l.data(), r.data(), 256);
        for (float x : l)
            finite = finite && x == x && x > -100.0f && x < 100.0f;
        const uint64_t applied = engine.appliedKernelSequence();
        // a mode change drops the set in use (0), otherwise the sequence only grows
        if (applied != 0 && applied < lastApplied && block != 300 && block != 301 && block != 360 && block != 361)
            appliedMonotone = false;
        if (applied != 0)
            lastApplied = applied;
        ++block;
        std::this_thread::sleep_for (std::chrono::microseconds (700));
    }

    thread.stop();
    check (finite, "engine output stays finite with a running design thread");
    check (appliedMonotone, "applied kernel sequence only grows within a mode");
    check (engine.appliedKernelSequence() != 0, "kernels reached the audio thread");
    std::printf ("engine: %d blocks, %llu designs, %llu requests\n", block, static_cast<unsigned long long> (thread.designsDone()),
                 static_cast<unsigned long long> (engine.paramSequence()));
}
} // namespace

int main()
{
    if (!linearPhaseOn())
    {
        std::printf ("linear phase disabled in this build: nothing to check\n");
        return 0;
    }

    mailboxStress();
    linkStress();
    engineWithDesignThread();

    std::printf (failures == 0 ? "ok\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
