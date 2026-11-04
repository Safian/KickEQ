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

// Real-time safety of the linear-phase path: nothing is allocated while EqEngine::process () runs, not even in the
// calls that switch the phase mode, hand over a kernel set, change the FIR length, bypass or run mono. The global
// operator new / delete of this executable count every allocation made while `counting` is set.
//
// (Locks and system calls are excluded by construction: the audio-side code uses only atomics and plain memory;
// this executable is the check for the heap, the part that is easy to break by accident.)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "kickeq/eq_engine.h"
#include "kickeq/linear_phase.h"
#include "kickeq/params.h"

namespace
{
std::atomic<bool> counting { false };
std::atomic<long> allocations { 0 };
std::atomic<long> frees { 0 };

void* allocate (std::size_t n)
{
    if (counting.load (std::memory_order_relaxed))
        allocations.fetch_add (1, std::memory_order_relaxed);
    void* p = std::malloc (n != 0 ? n : 1);
    if (p == nullptr)
        std::abort();
    return p;
}

void release (void* p) noexcept
{
    if (p != nullptr && counting.load (std::memory_order_relaxed))
        frees.fetch_add (1, std::memory_order_relaxed);
    std::free (p);
}
} // namespace

void* operator new (std::size_t n) { return allocate (n); }
void* operator new[] (std::size_t n) { return allocate (n); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept { return allocate (n); }
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept { return allocate (n); }
void operator delete (void* p) noexcept { release (p); }
void operator delete[] (void* p) noexcept { release (p); }
void operator delete (void* p, std::size_t) noexcept { release (p); }
void operator delete[] (void* p, std::size_t) noexcept { release (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { release (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { release (p); }

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

float next (uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float> (static_cast<double> (s) / 2147483648.0 - 1.0) * 0.5f;
}

struct Audio
{
    EqEngine engine;
    LinearPhaseDesigner designer;
    uint64_t designed = 0;
    uint32_t seed = 7;
    long processCalls = 0;
    float l[4096] = {}, r[4096] = {};

    /** One audio-thread call (counted), then one design-thread step (not counted). */
    void block (const ParamValues& pv, int n, bool mono = false)
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = next (seed);
            r[i] = next (seed);
        }
        counting.store (true);
        engine.process (pv, l, mono ? nullptr : r, n);
        counting.store (false);
        ++processCalls;
        designed = serviceDesignRequest (*engine.designLink(), designer, designed);
    }
};

void setBell (ParamValues& pv, int b, float freq, float gain, Placement place = Placement::stereo)
{
    pv.set (b, BandParam::on, 1.0f);
    pv.set (b, BandParam::type, 0.0f);
    pv.set (b, BandParam::freq, freq);
    pv.set (b, BandParam::gain, gain);
    pv.set (b, BandParam::q, 1.0f);
    pv.set (b, BandParam::place, static_cast<float> (place));
}
} // namespace

int main()
{
    if (!linearPhaseOn())
    {
        std::printf ("linear phase disabled in this build: nothing to check\n");
        return 0;
    }

    // the counter itself works: one counted allocation is seen
    {
        counting.store (true);
        void* probe = ::operator new (16); // a direct call: a new expression may be elided by the optimiser
        counting.store (false);
        check (allocations.load() == 1, "operator new replacement is active");
        ::operator delete (probe);
        allocations.store (0);
        frees.store (0);
    }

    auto audio = std::make_unique<Audio>();
    audio->engine.prepare (48000.0, 512);

    ParamValues pv;
    setBell (pv, 2, 1000.0f, 6.0f);
    setBell (pv, 4, 3000.0f, -4.0f, Placement::mid);
    setBell (pv, 5, 700.0f, 3.0f, Placement::left);

    const long before = allocations.load();

    // minimum phase
    for (int i = 0; i < 40; ++i)
        audio->block (pv, 256);

    // switch to each linear mode (warm-up, cross-fade, kernel hand-over), moving parameters, several block sizes
    for (int mode : { 1, 2, 3, 1 })
    {
        pv.set (paramPhaseMode, static_cast<float> (mode));
        const int sizes[] = { 128, 7, 1000, 64, 4096, 1 };
        for (int rep = 0; rep < 60; ++rep)
        {
            pv.set (2, BandParam::gain, 6.0f + static_cast<float> (rep % 7));
            pv.set (4, BandParam::freq, 2000.0f + 40.0f * static_cast<float> (rep));
            audio->block (pv, sizes[rep % 6]);
        }
        check (audio->engine.getLatencySamples() == phaseModeLatency (mode), "latency follows the mode");
    }

    // bypass, mono, output / auto gain, band structure changes in linear phase
    pv.set (paramBypass, 1.0f);
    for (int i = 0; i < 20; ++i)
        audio->block (pv, 333);
    pv.set (paramBypass, 0.0f);
    pv.set (paramAutoGain, 1.0f);
    pv.set (paramOutGain, -4.0f);
    for (int i = 0; i < 20; ++i)
        audio->block (pv, 500, i % 2 == 0);
    pv.set (3, BandParam::on, 1.0f);
    pv.set (3, BandParam::type, 5.0f);
    pv.set (2, BandParam::place, static_cast<float> (Placement::side));
    for (int i = 0; i < 20; ++i)
        audio->block (pv, 256);

    // back to minimum phase, then straight to another length through the minimum
    pv.set (paramPhaseMode, 0.0f);
    for (int i = 0; i < 40; ++i)
        audio->block (pv, 256);
    pv.set (paramPhaseMode, 3.0f);
    for (int i = 0; i < 40; ++i)
        audio->block (pv, 512);
    pv.set (paramPhaseMode, 2.0f);
    for (int i = 0; i < 120; ++i)
        audio->block (pv, 512);

    // reset and re-initialisation in the linear mode
    counting.store (true);
    audio->engine.reset();
    counting.store (false);
    for (int i = 0; i < 20; ++i)
        audio->block (pv, 256);

    const long counted = allocations.load() - before;
    std::printf ("process calls: %ld, allocations on the audio side: %ld (frees %ld)\n", audio->processCalls, counted, frees.load());
    check (counted == 0, "no allocation while process () runs");
    check (audio->engine.appliedKernelSequence() != 0, "kernels were handed over");
    check (audio->engine.getLatencySamples() == 2048, "final latency");

    std::printf (failures == 0 ? "ok\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
