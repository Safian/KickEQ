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

// State blob: round trip, compatibility rules, fuzzed / truncated / malicious blobs. Blobs are always held in
// exactly sized heap vectors so AddressSanitizer catches every out-of-bounds read.

#include "kickeq/state.h"
#include "test_util.h"

using namespace tu;
using namespace kickeq;

namespace
{
ParamValues randomValues (Rng& rng)
{
    ParamValues pv;
    for (int i = 0; i < numParams; ++i)
    {
        const ParamInfo& info = paramInfo (i);
        pv.set (i, static_cast<float> (rng.range (info.min, info.max)));
    }
    return pv;
}

std::vector<uint8_t> save (const ParamValues& pv, const UiChunk* ui = nullptr)
{
    const size_t need = saveState (pv, nullptr, 0, ui);
    std::vector<uint8_t> blob (need);
    CHECK (saveState (pv, blob.data(), blob.size(), ui) == need);
    return blob;
}

/** Exactly sized copy, so any read past the end is an ASan error. */
bool loadExact (const std::vector<uint8_t>& blob, size_t length, ParamValues& pv, UiChunk* ui = nullptr)
{
    std::vector<uint8_t> exact (blob.begin(), blob.begin() + static_cast<std::ptrdiff_t> (length));
    return loadState (exact.data(), exact.size(), pv, ui);
}

void putU32 (std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back (static_cast<uint8_t> (v >> (8 * i)));
}

void putF32 (std::vector<uint8_t>& b, float f)
{
    uint32_t bits;
    std::memcpy (&bits, &f, 4);
    putU32 (b, bits);
}

std::vector<uint8_t> header (uint32_t version, uint32_t numParamsInBlob)
{
    std::vector<uint8_t> b = { 'K', 'E', 'Q', 'S' };
    putU32 (b, version);
    putU32 (b, numParamsInBlob);
    return b;
}

bool inRangeAndFinite (const ParamValues& pv)
{
    for (int i = 0; i < numParams; ++i)
    {
        const float v = pv.values[static_cast<size_t> (i)];
        if (!std::isfinite (v) || v < paramInfo (i).min || v > paramInfo (i).max)
            return false;
    }
    return true;
}
} // namespace

TEST (state_round_trip_is_exact)
{
    Rng rng (42);
    for (int iter = 0; iter < 200; ++iter)
    {
        const ParamValues in = randomValues (rng);
        const auto blob = save (in);

        ParamValues out;
        out.set (paramOutGain, -17.0f); // must be overwritten
        CHECK (loadState (blob.data(), blob.size(), out));
        CHECK (out == in);
    }

    // defaults round trip as well, and the layout is the documented one
    const ParamValues defaults;
    const auto blob = save (defaults);
    CHECK (blob.size() == 12u + 60u * 8u + 4u);
    CHECK (std::memcmp (blob.data(), "KEQS", 4) == 0);
    CHECK (blob[4] == 1 && blob[5] == 0 && blob[6] == 0 && blob[7] == 0);   // version 1, little endian
    CHECK (blob[8] == 60 && blob[9] == 0);                                  // numParams
    CHECK (blob[12] == 0 && blob[13] == 0);                                 // first id: 0 (bypass)
    CHECK (blob[blob.size() - 4] == 0 && blob[blob.size() - 1] == 0);       // no chunks
}

TEST (state_ui_chunk_passes_through_opaquely)
{
    const ParamValues pv;
    std::vector<uint8_t> payload (1000);
    Rng rng (5);
    for (auto& b : payload)
        b = static_cast<uint8_t> (rng.next());

    UiChunk in;
    in.data = payload.data();
    in.size = static_cast<uint32_t> (payload.size());
    const auto blob = save (pv, &in);

    ParamValues out;
    UiChunk got;
    CHECK (loadState (blob.data(), blob.size(), out, &got));
    CHECK (got.size == payload.size());
    CHECK (got.data != nullptr && std::memcmp (got.data, payload.data(), payload.size()) == 0);
    CHECK (got.data >= blob.data() && got.data + got.size <= blob.data() + blob.size()); // points into the blob

    // without a UI chunk the output is empty; passing no UiChunk pointer is fine
    const auto plain = save (pv);
    UiChunk none;
    none.size = 77;
    CHECK (loadState (plain.data(), plain.size(), out, &none));
    CHECK (none.size == 0 && none.data == nullptr);
    CHECK (loadState (blob.data(), blob.size(), out, nullptr));
}

TEST (state_save_size_query_and_small_buffer)
{
    const ParamValues pv;
    const size_t need = saveState (pv, nullptr, 0);
    CHECK (need == 12u + 60u * 8u + 4u);

    std::vector<uint8_t> small (need - 1, 0xAB);
    CHECK (saveState (pv, small.data(), small.size()) == need);
    bool untouched = true;
    for (uint8_t b : small)
        untouched = untouched && b == 0xAB;
    CHECK (untouched); // nothing written when the buffer is too small

    std::vector<uint8_t> big (need + 16, 0xCD);
    CHECK (saveState (pv, big.data(), big.size()) == need);
    CHECK (big[need] == 0xCD); // no write past the blob
}

TEST (state_compatibility_unknown_missing_nan_and_range)
{
    // missing ids take the default; unknown ids and unknown chunks are ignored; out of range is clamped;
    // NaN / Inf are rejected per value; later versions are accepted
    std::vector<uint8_t> b = header (2, 7);
    putU32 (b, 1);      putF32 (b, 30.0f);                                             // out_gain: clamped to 24
    putU32 (b, 0x102);  putF32 (b, 1500.0f);                                           // b0_freq
    putU32 (b, 0xDEAD); putF32 (b, 5.0f);                                              // unknown id
    putU32 (b, 3);      putF32 (b, 1.0f);                                              // reserved phase_mode: unknown
    putU32 (b, 0x113);  putF32 (b, std::numeric_limits<float>::quiet_NaN());           // b1_gain: NaN -> default
    putU32 (b, 0x123);  putF32 (b, std::numeric_limits<float>::infinity());            // b2_gain: Inf -> default
    putU32 (b, 0x120);  putF32 (b, 1.0f);                                              // b2_on
    putU32 (b, 2);                                                                     // two chunks
    putU32 (b, makeChunkTag ('X', 'Y', 'Z', '9')); putU32 (b, 3); b.push_back (1); b.push_back (2); b.push_back (3);
    putU32 (b, chunkTagUi); putU32 (b, 2); b.push_back (9); b.push_back (8);

    ParamValues pv;
    UiChunk ui;
    CHECK (loadState (b.data(), b.size(), pv, &ui));
    CHECK (pv.get (paramOutGain) == 24.0f);
    CHECK (pv.get (0, BandParam::freq) == 1500.0f);
    CHECK (pv.get (1, BandParam::gain) == 0.0f);
    CHECK (pv.get (2, BandParam::gain) == 0.0f);
    CHECK (pv.get (2, BandParam::on) == 1.0f);
    CHECK (pv.get (3, BandParam::freq) == paramInfo (bandParamIndex (3, BandParam::freq)).def); // missing: default
    CHECK (ui.size == 2 && ui.data[0] == 9 && ui.data[1] == 8);
    CHECK (inRangeAndFinite (pv));

    // an older writer with fewer parameters, and no trailing chunk region content
    std::vector<uint8_t> old = header (1, 1);
    putU32 (old, 1);
    putF32 (old, -5.0f);
    putU32 (old, 0);
    ParamValues pv2;
    CHECK (loadState (old.data(), old.size(), pv2));
    CHECK (pv2.get (paramOutGain) == -5.0f);

    // stepped parameters are rounded, enum values clamped
    std::vector<uint8_t> st = header (1, 2);
    putU32 (st, 0x101);  putF32 (st, 99.0f);  // b0_type -> 6
    putU32 (st, 0x100);  putF32 (st, 0.4f);   // b0_on -> 0
    putU32 (st, 0);
    CHECK (loadState (st.data(), st.size(), pv2));
    CHECK (pv2.get (0, BandParam::type) == 6.0f);
    CHECK (pv2.get (0, BandParam::on) == 0.0f);
}

TEST (state_rejects_invalid_headers_and_leaves_output_untouched)
{
    ParamValues pv;
    pv.set (paramOutGain, 7.0f);
    const ParamValues before = pv;

    CHECK (!loadState (nullptr, 0, pv));
    CHECK (!loadState (nullptr, 100, pv));
    std::vector<uint8_t> shortBlob (11, 0);
    CHECK (!loadState (shortBlob.data(), shortBlob.size(), pv));

    auto bad = header (1, 0);
    putU32 (bad, 0);
    bad[0] = 'X';
    CHECK (!loadState (bad.data(), bad.size(), pv));

    auto v0 = header (0, 0);
    putU32 (v0, 0);
    CHECK (!loadState (v0.data(), v0.size(), pv));

    // huge counts must not run out of bounds or allocate
    auto huge = header (1, 0xFFFFFFFFu);
    CHECK (!loadState (huge.data(), huge.size(), pv));

    auto hugeChunks = header (1, 0);
    putU32 (hugeChunks, 0xFFFFFFFFu);
    CHECK (!loadState (hugeChunks.data(), hugeChunks.size(), pv));

    auto hugeChunkSize = header (1, 0);
    putU32 (hugeChunkSize, 1);
    putU32 (hugeChunkSize, chunkTagUi);
    putU32 (hugeChunkSize, 0xFFFFFFF0u);
    hugeChunkSize.push_back (1);
    CHECK (!loadState (hugeChunkSize.data(), hugeChunkSize.size(), pv));

    CHECK (pv == before);
}

TEST (state_every_truncation_is_rejected_without_overreading)
{
    Rng rng (9);
    const ParamValues in = randomValues (rng);
    std::vector<uint8_t> payload (50, 0x5A);
    UiChunk ui;
    ui.data = payload.data();
    ui.size = static_cast<uint32_t> (payload.size());
    const auto blob = save (in, &ui);

    ParamValues sentinel;
    sentinel.set (paramOutGain, 3.0f);
    for (size_t len = 0; len < blob.size(); ++len)
    {
        ParamValues out = sentinel;
        UiChunk got;
        const bool ok = loadExact (blob, len, out, &got);
        CHECK (!ok);
        CHECK (out == sentinel); // untouched on failure
    }
    ParamValues out;
    CHECK (loadExact (blob, blob.size(), out));
    CHECK (out == in);
}

TEST (state_fuzzed_blobs_never_misbehave)
{
    Rng rng (31337);
    const ParamValues base = randomValues (rng);
    std::vector<uint8_t> payload (64, 0x11);
    UiChunk ui;
    ui.data = payload.data();
    ui.size = static_cast<uint32_t> (payload.size());
    const auto valid = save (base, &ui);

    int accepted = 0, rejected = 0;

    // bit flips / byte stomps in a valid blob
    for (int iter = 0; iter < 20000; ++iter)
    {
        auto b = valid;
        const int edits = rng.integer (1, 6);
        for (int e = 0; e < edits; ++e)
        {
            const size_t pos = static_cast<size_t> (rng.integer (0, static_cast<int> (b.size()) - 1));
            switch (rng.integer (0, 2))
            {
                case 0: b[pos] ^= static_cast<uint8_t> (1u << rng.integer (0, 7)); break;
                case 1: b[pos] = static_cast<uint8_t> (rng.next()); break;
                default: b[pos] = rng.integer (0, 1) ? 0xFF : 0x00; break;
            }
        }
        if (rng.integer (0, 4) == 0)
            b.resize (static_cast<size_t> (rng.integer (0, static_cast<int> (b.size()))));

        ParamValues out;
        UiChunk got;
        std::vector<uint8_t> exact = b; // exact size allocation
        if (loadState (exact.data(), exact.size(), out, &got))
        {
            ++accepted;
            CHECK (inRangeAndFinite (out));
            CHECK (got.size == 0 || (got.data >= exact.data() && got.data + got.size <= exact.data() + exact.size()));
        }
        else
            ++rejected;
    }

    // pure garbage of random length, with and without a valid magic
    for (int iter = 0; iter < 20000; ++iter)
    {
        std::vector<uint8_t> b (static_cast<size_t> (rng.integer (0, 400)));
        for (auto& x : b)
            x = static_cast<uint8_t> (rng.next());
        if (b.size() >= 12 && rng.integer (0, 1))
        {
            std::memcpy (b.data(), "KEQS", 4);
            b[4] = 1; b[5] = b[6] = b[7] = 0;
            b[9] = b[10] = b[11] = 0; // plausible parameter counts: the parser has to walk real data
            b[8] = static_cast<uint8_t> (rng.integer (0, 40));
        }
        ParamValues out;
        if (loadState (b.data(), b.size(), out))
        {
            ++accepted;
            CHECK (inRangeAndFinite (out));
        }
        else
            ++rejected;
    }

    CHECK (accepted > 100);
    CHECK (rejected > 100);
}

TEST (state_older_blobs_without_phase_mode_load_as_minimum)
{
    // a blob written before phase_mode existed: 59 entries, no id 3
    std::vector<uint8_t> blob = header (1, static_cast<uint32_t> (numParams - 1));
    for (int i = 0; i < numParams; ++i)
    {
        if (i == paramPhaseMode)
            continue;
        putU32 (blob, paramInfo (i).id);
        putF32 (blob, i == paramOutGain ? -6.0f : paramInfo (i).def);
    }
    putU32 (blob, 0); // no chunks

    ParamValues pv;
    pv.set (paramPhaseMode, 2.0f); // loading starts from the defaults: Minimum again
    CHECK (loadState (blob.data(), blob.size(), pv));
    CHECK (pv.get (paramPhaseMode) == 0.0f && pv.phaseMode() == 0);
    CHECK (pv.outGainDb() == -6.0f);
}

TEST (state_phase_mode_round_trips)
{
    for (int mode = 0; mode < numPhaseModes; ++mode)
    {
        ParamValues in;
        in.set (paramPhaseMode, static_cast<float> (mode));
        const auto blob = save (in);
        ParamValues out;
        CHECK (loadState (blob.data(), blob.size(), out));
        // a build without the linear-phase mode stores and loads Minimum only
        CHECK (out.phaseMode() == (linearPhaseOn() ? mode : 0));
        CHECK (out == in);
    }

    // out-of-range stored values are clamped, not rejected
    std::vector<uint8_t> blob = header (1, 1);
    putU32 (blob, 3);
    putF32 (blob, 17.0f);
    putU32 (blob, 0);
    ParamValues pv;
    CHECK (loadState (blob.data(), blob.size(), pv));
    CHECK (pv.phaseMode() == (linearPhaseOn() ? 3 : 0));
}
