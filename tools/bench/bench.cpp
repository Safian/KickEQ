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
// Output: ns per sample frame (both channels) and the share of one core at 48 kHz. `--check` exits with 1
// when a scenario costs more than 25 times its budget on this machine: a loose regression guard that also
// holds under QEMU user emulation; the real figures come from the MPC hardware.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <clap/clap.h>

#include "kickeq/clap_native.h"
#include "kickeq/params.h"

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
    double budgetPercent;
    ParamValues values;
    bool moving;
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
    if (p == nullptr || !p->init (p) || !p->activate (p, sampleRate, blockSize, blockSize) || !p->start_processing (p))
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

    for (int i = 0; i < warmupBlocks; ++i)
        block();

    double best = 1.0e300;
    for (int r = 0; r < repetitions; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < measureBlocks; ++i)
            block();
        const double ns = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - t0).count();
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
} // namespace

int main (int argc, char** argv)
{
    bool check = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--check") == 0)
        {
            check = true;
        }
        else
        {
            std::fprintf (stderr, "usage: %s [--check]\n", argv[0]);
            return 2;
        }
    }

    const clap_plugin_factory_t* factory = kickeq_get_plugin_factory();
    const Scenario scenarios[3] = {
        { "8 bands (2 cuts 24 dB/oct), at rest", 3.0, typical(), false },
        { "8 bands, all parameters moving", 6.0, typical(), true },
        { "worst case: 8 cuts at 48 dB/oct", 6.0, worstCase(), false }
    };

    std::printf ("kickeq_bench: native shell process(), %.0f Hz, %u frames, stereo\n\n", sampleRate, blockSize);
    std::printf ("%-40s %12s %10s %10s %8s\n", "scenario", "ns/frame", "% of core", "budget %", "x budget");
    bool failed = false;
    for (const Scenario& sc : scenarios)
    {
        const Result r = run (factory, sc);
        const double ratio = r.percentCore / sc.budgetPercent;
        std::printf ("%-40s %12.1f %10.3f %10.1f %8.2f%s\n", sc.name, r.nsPerFrame, r.percentCore, sc.budgetPercent, ratio,
                     r.finite ? "" : "  NOT FINITE");
        if (!r.finite || r.nsPerFrame <= 0.0 || (check && ratio > guardFactor))
            failed = true;
    }
    if (check)
        std::printf ("\ncheck: %s (limit %.0fx budget)\n", failed ? "FAILED" : "ok", guardFactor);
    return check && failed ? 1 : 0;
}
