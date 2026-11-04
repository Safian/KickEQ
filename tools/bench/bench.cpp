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

// kickeq_bench - CPU cost of the native shell's process() path (ARCHITECTURE.md section 9).
//
// The three scenarios run through the CLAP API of the statically linked plugin (clap_plugin::process), at
// 48 kHz with 128 frame blocks, stereo, analyser disabled:
//   rest    8 bands on (2 cuts at 24 dB/oct, shelves, bells), parameters at rest      budget 3 % of one core
//   moving  the same, every band's frequency / gain / Q plus the output gain move      budget 6 % of one core
//           with sample-accurate events in every block
//   worst   8 cuts (4 low, 4 high) at 48 dB/oct, at rest                               budget 6 % (the "moving"
//           budget: section 9 names no separate figure for the worst case)
//   linear  the "rest" and the "moving" set in linear-phase mode, N = 4096 (latency 2048), with a running design
//           thread: reported only, no budget is defined yet (ARCHITECTURE.md section 4.7); skipped when the core is
//           built without KICKEQ_LINEAR_PHASE. Only the audio thread's process () calls are timed (the design thread
//           runs on another core); the blocks are paced at 1 ms so that kernels arrive at a realistic rate
//           (one per ~12 ms of wall time, i.e. every few blocks while parameters move). The cost of one kernel
//           design is measured separately and printed below the table.
//           `--no-threads` runs the linear-phase scenarios on a bare EqEngine instead of the shell (no design thread;
//           a designer step between blocks plays its part): for emulators where thread creation crashes (qemu-user).
// Output: ns per sample frame (both channels) and the share of one core at 48 kHz. `--check` exits with 1
// when a scenario costs more than 25 times its budget on this machine: a loose regression guard that also
// holds under QEMU user emulation; the real figures come from the MPC hardware.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <clap/clap.h>

#include "kickeq/clap_native.h"
#include "kickeq/params.h"

#include "kickeq/eq_engine.h"
#if KICKEQ_LINEAR_PHASE
#include "kickeq/linear_phase.h"
#endif

namespace
{
using namespace kickeq;

constexpr double sampleRate = 48000.0;
constexpr uint32_t blockSize = 128;
constexpr int warmupBlocks = 750;      // 2 s
constexpr int measureBlocks = 750;     // 2 s per repetition
constexpr int repetitions = 5;
constexpr int patternBlocks = 256;     // the moving pattern repeats every 256 blocks
constexpr double guardFactor = 25.0;

const void* CLAP_ABI noExtension (const clap_host_t*, const char*) { return nullptr; }
void CLAP_ABI noop (const clap_host_t*) {}

struct EventBlock
{
    std::vector<clap_event_param_value_t> events;
};

struct Events
{
    std::vector<EventBlock> blocks;
    const EventBlock* current = nullptr;

    static uint32_t CLAP_ABI size (const clap_input_events_t* l)
    {
        const Events& e = *static_cast<const Events*> (l->ctx);
        return e.current != nullptr ? static_cast<uint32_t> (e.current->events.size()) : 0;
    }
    static const clap_event_header_t* CLAP_ABI get (const clap_input_events_t* l, uint32_t i)
    {
        const Events& e = *static_cast<const Events*> (l->ctx);
        return &e.current->events[i].header;
    }
};

bool CLAP_ABI pushOut (const clap_output_events_t*, const clap_event_header_t*) { return true; }

clap_event_param_value_t makeEvent (uint32_t time, int index, double value)
{
    clap_event_param_value_t ev {};
    ev.header.size = sizeof ev;
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.param_id = paramInfo (index).id;
    ev.note_id = -1;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.value = value;
    return ev;
}

void setBand (ParamValues& v, int b, BandType t, float f, float g, float q, int slope)
{
    v.set (b, BandParam::on, 1.0f);
    v.set (b, BandParam::type, static_cast<float> (t));
    v.set (b, BandParam::freq, f);
    v.set (b, BandParam::gain, g);
    v.set (b, BandParam::q, q);
    v.set (b, BandParam::slope, static_cast<float> (slope));
}

ParamValues typical()
{
    ParamValues v;
    setBand (v, 0, BandType::lowCut, 35.0f, 0.0f, 0.707f, 3);
    setBand (v, 1, BandType::lowShelf, 110.0f, 3.0f, 0.71f, 1);
    setBand (v, 2, BandType::bell, 250.0f, -3.0f, 1.2f, 1);
    setBand (v, 3, BandType::bell, 800.0f, 2.5f, 1.0f, 1);
    setBand (v, 4, BandType::bell, 2500.0f, 4.0f, 1.5f, 1);
    setBand (v, 5, BandType::bell, 6000.0f, -2.0f, 0.9f, 1);
    setBand (v, 6, BandType::highShelf, 10000.0f, 2.0f, 0.71f, 1);
    setBand (v, 7, BandType::highCut, 18000.0f, 0.0f, 0.707f, 3);
    return v;
}

ParamValues worstCase()
{
    ParamValues v;
    for (int b = 0; b < numBands; ++b)
        setBand (v, b, b < 4 ? BandType::lowCut : BandType::highCut, b < 4 ? 30.0f + 40.0f * static_cast<float> (b) : 9000.0f + 1500.0f * static_cast<float> (b), 0.0f, 0.707f, 5);
    return v;
}

struct Scenario
{
    const char* name;
    double budgetPercent; // 0: reported only
    ParamValues values;
    bool moving;
    bool paced = false;   // linear phase: time process () calls only, 1 ms apart, with a prelude for the design thread
};

struct Result
{
    double nsPerFrame;
    double percentCore;
    bool finite;
};

Result run (const clap_plugin_factory_t* factory, const Scenario& sc)
{
    clap_host_t host {};
    host.clap_version = CLAP_VERSION;
    host.name = "kickeq_bench";
    host.get_extension = &noExtension;
    host.request_restart = &noop;
    host.request_process = &noop;
    host.request_callback = &noop;

    const clap_plugin_t* p = factory->create_plugin (factory, &host, "com.safian.kickeq");
    if (p == nullptr || !p->init (p))
        return { 0.0, 0.0, false };

    if (sc.values.phaseMode() != 0)
    {
        // the phase mode is read when the plugin is activated (it changes the latency): set it before, through a flush
        const auto* params = static_cast<const clap_plugin_params_t*> (p->get_extension (p, CLAP_EXT_PARAMS));
        Events setup;
        setup.blocks.resize (1);
        setup.blocks[0].events.push_back (makeEvent (0, paramPhaseMode, static_cast<double> (sc.values.get (paramPhaseMode))));
        setup.current = &setup.blocks[0];
        clap_input_events_t in = { &setup, &Events::size, &Events::get };
        clap_output_events_t out = { nullptr, &pushOut };
        params->flush (p, &in, &out);
    }

    if (!p->activate (p, sampleRate, blockSize, blockSize) || !p->start_processing (p))
        return { 0.0, 0.0, false };

    // initial values: one event block at time 0 for every parameter
    Events ev;
    ev.blocks.resize (static_cast<size_t> (patternBlocks) + 1);
    for (int i = 0; i < numParams; ++i)
        ev.blocks[0].events.push_back (makeEvent (0, i, static_cast<double> (sc.values.get (i))));

    if (sc.moving)
    {
        for (int blk = 0; blk < patternBlocks; ++blk)
        {
            EventBlock& eb = ev.blocks[static_cast<size_t> (blk) + 1];
            for (int half = 0; half < 2; ++half)
            {
                const double phase = (static_cast<double> (blk) + 0.5 * half) / patternBlocks;
                for (int b = 0; b < numBands; ++b)
                {
                    const double ph = 6.283185307179586 * (phase * (1.0 + b) + 0.13 * b);
                    const double f0 = static_cast<double> (sc.values.get (b, BandParam::freq));
                    const double g0 = static_cast<double> (sc.values.get (b, BandParam::gain));
                    const double q0 = static_cast<double> (sc.values.get (b, BandParam::q));
                    eb.events.push_back (makeEvent (half == 0 ? 0u : blockSize / 2, bandParamIndex (b, BandParam::freq), f0 * (1.0 + 0.3 * std::sin (ph))));
                    eb.events.push_back (makeEvent (half == 0 ? 0u : blockSize / 2, bandParamIndex (b, BandParam::gain), g0 + 6.0 * std::sin (1.3 * ph)));
                    eb.events.push_back (makeEvent (half == 0 ? 0u : blockSize / 2, bandParamIndex (b, BandParam::q), q0 * (1.0 + 0.4 * std::sin (0.7 * ph))));
                }
                eb.events.push_back (makeEvent (half == 0 ? 0u : blockSize / 2, paramOutGain, 3.0 * std::sin (2.0 * 6.283185307179586 * phase)));
            }
        }
    }

    std::vector<float> in (blockSize * 2), out (blockSize * 2);
    uint32_t seed = 12345;
    for (float& x : in)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        x = static_cast<float> (static_cast<int32_t> (seed)) * (1.0f / 4294967296.0f);
    }
    float* inData[2] = { in.data(), in.data() + blockSize };
    float* outData[2] = { out.data(), out.data() + blockSize };
    clap_audio_buffer_t ib {}, ob {};
    ib.data32 = inData;
    ib.channel_count = 2;
    ob.data32 = outData;
    ob.channel_count = 2;
    clap_input_events_t inEvents = { &ev, &Events::size, &Events::get };
    clap_output_events_t outEvents = { nullptr, &pushOut };
    clap_process_t proc {};
    proc.frames_count = blockSize;
    proc.audio_inputs = &ib;
    proc.audio_outputs = &ob;
    proc.audio_inputs_count = 1;
    proc.audio_outputs_count = 1;
    proc.in_events = &inEvents;
    proc.out_events = &outEvents;

    int counter = 0;
    const auto block = [&] {
        ev.current = counter == 0 ? &ev.blocks[0]
                                  : (sc.moving ? &ev.blocks[static_cast<size_t> (counter % patternBlocks) + 1] : &ev.blocks[patternBlocks]);
        proc.steady_time += blockSize;
        p->process (p, &proc);
        ++counter;
    };

    using Clock = std::chrono::steady_clock;
    const auto pace = [&] { std::this_thread::sleep_for (std::chrono::milliseconds (1)); };

    if (sc.paced)
    {
        // the design thread needs wall time for the first kernel: ~1.5 s of paced blocks, then the usual warm-up
        for (int i = 0; i < 1500; ++i)
        {
            block();
            pace();
        }
    }
    for (int i = 0; i < warmupBlocks; ++i)
    {
        block();
        if (sc.paced)
            pace();
    }

    double best = 1.0e300;
    for (int r = 0; r < repetitions; ++r)
    {
        double ns = 0.0;
        if (sc.paced)
        {
            for (int i = 0; i < measureBlocks; ++i)
            {
                const auto t0 = Clock::now();
                block();
                ns += std::chrono::duration<double, std::nano> (Clock::now() - t0).count();
                pace();
            }
        }
        else
        {
            const auto t0 = Clock::now();
            for (int i = 0; i < measureBlocks; ++i)
                block();
            ns = std::chrono::duration<double, std::nano> (Clock::now() - t0).count();
        }
        best = std::min (best, ns);
    }

    bool finite = true;
    for (float x : out)
        finite = finite && std::isfinite (x);

    p->stop_processing (p);
    p->deactivate (p);
    p->destroy (p);

    const double perFrame = best / (static_cast<double> (measureBlocks) * blockSize);
    return { perFrame, perFrame * sampleRate / 1.0e9 * 100.0, finite };
}
#if KICKEQ_LINEAR_PHASE
/** The linear-phase scenarios without the shell and without a thread: process () of a bare engine is timed, a designer
    step runs between the blocks at the pace of the design thread (one design per 12 ms of audio while parameters move).
    The parameters move like in the shell scenario (two target changes per block). */
Result runDirect (const Scenario& sc)
{
    EqEngine engine;
    engine.prepare (sampleRate, static_cast<int> (blockSize));
    LinearPhaseDesigner designer;
    uint64_t designed = 0;
    const int designEveryBlocks = static_cast<int> (0.012 * sampleRate / blockSize) + 1; // ~12 ms

    std::vector<float> l (blockSize), r (blockSize);
    uint32_t seed = 12345;
    const auto fillNoise = [&] {
        for (uint32_t i = 0; i < blockSize; ++i)
        {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            l[i] = static_cast<float> (static_cast<int32_t> (seed)) * (1.0f / 4294967296.0f);
            r[i] = l[i];
        }
    };

    const auto moved = [&] (int blk, int half) {
        ParamValues v = sc.values;
        if (!sc.moving)
            return v;
        const double phase = (static_cast<double> (blk % patternBlocks) + 0.5 * half) / patternBlocks;
        for (int b = 0; b < numBands; ++b)
        {
            const double ph = 6.283185307179586 * (phase * (1.0 + b) + 0.13 * b);
            v.set (b, BandParam::freq, static_cast<float> (static_cast<double> (sc.values.get (b, BandParam::freq)) * (1.0 + 0.3 * std::sin (ph))));
            v.set (b, BandParam::gain, static_cast<float> (static_cast<double> (sc.values.get (b, BandParam::gain)) + 6.0 * std::sin (1.3 * ph)));
            v.set (b, BandParam::q, static_cast<float> (static_cast<double> (sc.values.get (b, BandParam::q)) * (1.0 + 0.4 * std::sin (0.7 * ph))));
        }
        v.set (paramOutGain, static_cast<float> (3.0 * std::sin (2.0 * 6.283185307179586 * phase)));
        return v;
    };

    int counter = 0;
    double processNs = 0.0;
    const auto block = [&] (bool timed) {
        fillNoise();
        const ParamValues a = moved (counter, 0), b = moved (counter, 1);
        const auto t0 = std::chrono::steady_clock::now();
        engine.process (a, l.data(), r.data(), static_cast<int> (blockSize / 2));
        engine.process (b, l.data() + blockSize / 2, r.data() + blockSize / 2, static_cast<int> (blockSize / 2));
        if (timed)
            processNs += std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - t0).count();
        ++counter;
        if (counter % designEveryBlocks == 0 || counter == 1)
            designed = serviceDesignRequest (*engine.designLink(), designer, designed);
    };

    for (int i = 0; i < 100; ++i) // the first kernel and the warm-up
        block (false);
    for (int i = 0; i < warmupBlocks; ++i)
        block (false);

    double best = 1.0e300;
    for (int rep = 0; rep < repetitions; ++rep)
    {
        processNs = 0.0;
        for (int i = 0; i < measureBlocks; ++i)
            block (true);
        best = std::min (best, processNs);
    }

    bool finite = true;
    for (float x : l)
        finite = finite && std::isfinite (x);
    const double perFrame = best / (static_cast<double> (measureBlocks) * blockSize);
    return { perFrame, perFrame * sampleRate / 1.0e9 * 100.0, finite };
}
#endif
} // namespace

int main (int argc, char** argv)
{
    bool check = false, noThreads = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--check") == 0)
        {
            check = true;
        }
        else if (std::strcmp (argv[i], "--no-threads") == 0)
        {
            noThreads = true;
        }
        else
        {
            std::fprintf (stderr, "usage: %s [--check] [--no-threads]\n", argv[0]);
            return 2;
        }
    }

    const clap_plugin_factory_t* factory = kickeq_get_plugin_factory();
    std::vector<Scenario> scenarios = {
        { "8 bands (2 cuts 24 dB/oct), at rest", 3.0, typical(), false },
        { "8 bands, all parameters moving", 6.0, typical(), true },
        { "worst case: 8 cuts at 48 dB/oct", 6.0, worstCase(), false }
    };
    if (linearPhaseOn())
    {
        ParamValues rest = typical(), moving = typical();
        rest.set (paramPhaseMode, 2.0f); // Linear 4096
        moving.set (paramPhaseMode, 2.0f);
        scenarios.push_back ({ "linear phase 4096, 8 bands, at rest", 0.0, rest, false, true });
        scenarios.push_back ({ "linear phase 4096, 8 bands, moving", 0.0, moving, true, true });
    }

    std::printf ("kickeq_bench: native shell process(), %.0f Hz, %u frames, stereo\n\n", sampleRate, blockSize);
    std::printf ("%-40s %12s %10s %10s %8s\n", "scenario", "ns/frame", "% of core", "budget %", "x budget");
    bool failed = false;
    for (const Scenario& sc : scenarios)
    {
#if KICKEQ_LINEAR_PHASE
        const Result r = sc.paced && noThreads ? runDirect (sc) : run (factory, sc);
#else
        const Result r = run (factory, sc);
#endif
        if (sc.budgetPercent > 0.0)
        {
            const double ratio = r.percentCore / sc.budgetPercent;
            std::printf ("%-40s %12.1f %10.3f %10.1f %8.2f%s\n", sc.name, r.nsPerFrame, r.percentCore, sc.budgetPercent, ratio,
                         r.finite ? "" : "  NOT FINITE");
            if (!r.finite || r.nsPerFrame <= 0.0 || (check && ratio > guardFactor))
                failed = true;
        }
        else
        {
            std::printf ("%-40s %12.1f %10.3f %10s %8s%s\n", sc.name, r.nsPerFrame, r.percentCore, "(none)", "-",
                         r.finite ? "" : "  NOT FINITE");
            if (!r.finite || r.nsPerFrame <= 0.0)
                failed = true;
        }
    }
    if (!linearPhaseOn())
        std::printf ("%-40s skipped: built without KICKEQ_LINEAR_PHASE\n", "linear phase 4096");
    else
        std::printf ("%-40s reported only: no budget is defined yet (ARCHITECTURE.md section 4.7); process () timing, design thread excluded%s\n",
                     "linear phase 4096", noThreads ? " (--no-threads: bare engine, no shell)" : "");

#if KICKEQ_LINEAR_PHASE
    {
        // cost of one kernel design on the design thread (best of 5)
        LinearPhaseDesigner designer;
        KernelSet set;
        ParamValues pv = typical();
        pv.set (paramPhaseMode, 2.0f);
        double best = 1.0e300;
        for (int i = 0; i < 5; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            designer.design (pv, sampleRate, 1, set);
            best = std::min (best, std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count());
        }
        std::printf ("%-40s %12.2f ms (design thread, one kernel set, N = 4096, 8 bands)\n", "kernel design", best);
    }
#endif
    if (check)
        std::printf ("\ncheck: %s (limit %.0fx budget)\n", failed ? "FAILED" : "ok", guardFactor);
    return check && failed ? 1 : 0;
}
