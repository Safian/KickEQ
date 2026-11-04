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

// Block-size independence: the output only depends on the targets and the sample position at which they are
// handed over, never on how the host chops the stream into blocks.

#include "test_util.h"

using namespace tu;

namespace
{
ParamValues scene1()
{
    ParamValues pv;
    setBand (pv, 0, true, BandType::lowCut, 60.0f, 0.0f, 0.7071f, 3);
    setBand (pv, 1, true, BandType::lowShelf, 120.0f, 5.0f, 0.71f);
    setBand (pv, 2, true, BandType::bell, 400.0f, -6.0f, 1.5f, 1, Placement::left);
    setBand (pv, 3, true, BandType::bell, 1200.0f, 8.0f, 3.0f, 1, Placement::mid);
    setBand (pv, 4, true, BandType::notch, 2500.0f, 0.0f, 6.0f, 1, Placement::side);
    setBand (pv, 5, true, BandType::bandPass, 5000.0f, 0.0f, 0.5f, 1, Placement::right);
    setBand (pv, 6, true, BandType::highShelf, 9000.0f, -5.0f, 0.71f);
    setBand (pv, 7, true, BandType::highCut, 16000.0f, 0.0f, 0.7071f, 4);
    pv.set (paramOutGain, 3.0f);
    return pv;
}

/** Everything that can move moves: frequencies, gains, Q, a type change, a disabled band, a new placement,
    a slope change, auto gain and the output gain. */
ParamValues scene2()
{
    ParamValues pv = scene1();
    setBand (pv, 0, true, BandType::lowCut, 90.0f, 0.0f, 0.7071f, 5);
    setBand (pv, 1, true, BandType::lowShelf, 180.0f, -3.0f, 1.2f);
    setBand (pv, 2, true, BandType::highShelf, 700.0f, 6.0f, 0.71f, 1, Placement::left);   // type change
    setBand (pv, 3, false, BandType::bell, 1200.0f, 8.0f, 3.0f, 1, Placement::mid);        // disabled
    setBand (pv, 4, true, BandType::notch, 2500.0f, 0.0f, 6.0f, 1, Placement::mid);        // placement change
    setBand (pv, 5, true, BandType::bandPass, 3000.0f, 0.0f, 2.0f, 1, Placement::right);
    setBand (pv, 7, true, BandType::highCut, 12000.0f, 0.0f, 0.7071f, 2);                  // slope change
    pv.set (paramAutoGain, 1.0f);
    pv.set (paramOutGain, -2.0f);
    return pv;
}

constexpr int total = 30000;
constexpr int changeAt = 8192;
constexpr int bypassOnAt = 16384;
constexpr int bypassOffAt = 22528;

struct Output
{
    std::vector<float> l, r;
};

/** Runs the scenario with the given block size; targets change at fixed sample positions. */
Output run (int blockSize, int scenario, bool interleaveEmptyCalls, int maxBlock = 512)
{
    auto e = makeEngine (48000.0, maxBlock);
    Output o { noise (total, 1234, 0.8f), noise (total, 4321, 0.8f) };

    const ParamValues a = scene1(), b = scene2();
    ParamValues bypassed = b;
    bypassed.set (paramBypass, 1.0f);

    for (int pos = 0; pos < total;)
    {
        // the segment boundaries are where the target changes
        const ParamValues* target = &a;
        int segmentEnd = changeAt;
        if (pos >= changeAt)
        {
            target = &b;
            segmentEnd = total;
            if (scenario == 1)
            {
                if (pos >= bypassOffAt)
                    segmentEnd = total;
                else if (pos >= bypassOnAt)
                {
                    target = &bypassed;
                    segmentEnd = bypassOffAt;
                }
                else
                    segmentEnd = bypassOnAt;
            }
        }

        const int len = std::min (blockSize, segmentEnd - pos);
        if (interleaveEmptyCalls)
            e->process (*target, o.l.data() + pos, o.r.data() + pos, 0);
        e->process (*target, o.l.data() + pos, o.r.data() + pos, len);
        pos += len;
    }
    return o;
}
} // namespace

TEST (blocksize_identical_for_all_block_sizes)
{
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        const Output reference = run (4096, scenario, false);
        CHECK (allFinite (reference.l.data(), total));

        // the processing is real (not a pass through)
        const auto in = noise (total, 1234, 0.8f);
        CHECK (!bitIdentical (reference.l, in));

        for (int bs : { 1, 7, 32, 64, 127, 480, 4096, 30000 })
        {
            const Output o = run (bs, scenario, false);
            const bool same = bitIdentical (o.l, reference.l) && bitIdentical (o.r, reference.r);
            if (!same)
                std::printf ("    scenario %d: block size %d differs\n", scenario, bs);
            CHECK (same);
        }
    }
}

TEST (blocksize_empty_calls_and_oversized_blocks_do_not_matter)
{
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        const Output reference = run (4096, scenario, false);
        const Output withEmpty = run (127, scenario, true);
        CHECK (bitIdentical (withEmpty.l, reference.l));
        CHECK (bitIdentical (withEmpty.r, reference.r));

        const Output tiny = run (30000, scenario, false, 16); // blocks far above maxBlock
        CHECK (bitIdentical (tiny.l, reference.l));
        CHECK (bitIdentical (tiny.r, reference.r));
    }
}

TEST (blocksize_mono_entry_point_equals_left_channel_of_dual_mono)
{
    const ParamValues pv = scene1();
    const auto in = noise (10000, 55, 0.7f);

    auto stereoL = in, stereoR = in;
    auto e1 = makeEngine (48000.0);
    processBlocks (*e1, pv, stereoL.data(), stereoR.data(), 10000, 333);

    auto mono = in;
    auto e2 = makeEngine (48000.0);
    for (int pos = 0; pos < 10000; pos += 77)
        e2->processMono (pv, mono.data() + pos, std::min (77, 10000 - pos));

    CHECK (bitIdentical (mono, stereoL));
}

TEST (blocksize_engine_instances_share_no_state)
{
    const ParamValues a = scene1(), b = scene2();
    const auto inA = noise (12000, 5, 0.6f), inB = noise (12000, 6, 0.6f);

    // alone
    auto aloneA = inA, aloneAR = inA, aloneB = inB, aloneBR = inB;
    {
        auto ea = makeEngine (48000.0), eb = makeEngine (44100.0);
        processBlocks (*ea, a, aloneA.data(), aloneAR.data(), 12000, 300);
        processBlocks (*eb, b, aloneB.data(), aloneBR.data(), 12000, 300);
    }

    // interleaved block by block (different sample rates, different parameters)
    auto ia = inA, iar = inA, ib = inB, ibr = inB;
    auto ea = makeEngine (48000.0), eb = makeEngine (44100.0);
    for (int pos = 0; pos < 12000; pos += 300)
    {
        ea->process (a, ia.data() + pos, iar.data() + pos, 300);
        eb->process (b, ib.data() + pos, ibr.data() + pos, 300);
    }
    CHECK (bitIdentical (ia, aloneA));
    CHECK (bitIdentical (iar, aloneAR));
    CHECK (bitIdentical (ib, aloneB));
    CHECK (bitIdentical (ibr, aloneBR));
}

TEST (blocksize_reset_reproduces_the_same_output)
{
    const ParamValues pv = scene2();
    const auto in = noise (9000, 8, 0.5f);
    auto e = makeEngine (48000.0);

    auto first = in, firstR = in;
    processBlocks (*e, pv, first.data(), firstR.data(), 9000, 200);

    e->reset();
    auto second = in, secondR = in;
    processBlocks (*e, pv, second.data(), secondR.data(), 9000, 200);
    CHECK (bitIdentical (first, second));
    CHECK (bitIdentical (firstR, secondR));
}
