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

// The conformance suite of the native CLAP shell. Every check prints PASS or FAIL; the exit code is non-zero
// when any check failed. The same suite runs against the static library (--static) and the loaded .clap.

// JUCE profile (--juce)
// ---------------------
// The same suite runs against the CLAP built from the JUCE shell (clap-juce-extensions wrapper around
// juce/PluginProcessor). Parameter ids are String::hashCode of the core string ids there, so Instance maps core ids
// to host ids by name (testhost.cpp); everything else is the native expectation unless one of the following
// legitimate differences of the wrapper applies (each place is marked "JUCE" in the code):
//
//   * descriptor: url is not published; get_plugin_descriptor (1) returns the descriptor instead of null; the
//     wrapper does not validate a null host / null plugin id (no host does that); clap.gui is offered.
//   * extensions the JUCE shell does not have: audio-ports-config (the layout is the host's choice; mono is a VST3
//     layout, tested by kickeq_juce_shell_test), remote-controls, preset-discovery / preset-load (factory presets
//     are JUCE programs), the analyser vendor extension (the editor draws the analyser itself).
//   * params: flags (only bool parameters are STEPPED, nothing is ENUM or BYPASS); global parameters are at the top
//     level of the JUCE parameter tree (empty CLAP module path); value_to_text uses strncpy without a terminator
//     for tiny buffers, text_to_value cannot reject text, get_value of an unknown id is not validated.
//   * values: a plain value that passes through the wrapper becomes a float normalised value and back, so float
//     parameters read back through clap.params agree within 1e-5 (relative), bool / enum exactly. The processor
//     stores exact plain values (juce/CoreParameter.h): a state blob loads with bit-identical values, which the
//     cross-shell tests verify.
//   * state: the wrapper cannot report a rejected blob (setStateInformation is void) so load returns true; the
//     processor ignores invalid blobs, which is what is checked (parameters untouched). The UI01 chunk is owned and
//     rewritten by the JUCE shell, so the "foreign chunk survives" check does not apply.
//   * tail: reported on demand (no clap_host_tail::changed); process returns CLAP_PROCESS_CONTINUE and leaves
//     constant_mask alone; parameter changes made inside the plugin are reported as output events.
//   * events: the wrapper does not split at the event but in sub-blocks of a multiple of 64 samples
//     (CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES 64); refBlockJuce reproduces that rule for the reference engine.
//
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "kickeq/clap_analyzer.h"
#include "kickeq/eq_curve.h"
#include "kickeq/eq_engine.h"
#include "kickeq/presets.h"
#include "kickeq/state.h"
#include "testhost.h"

#ifndef KICKEQ_EXPECTED_VERSION
#error "KICKEQ_EXPECTED_VERSION must be defined"
#endif

namespace th
{
namespace
{
using namespace kickeq;

struct Ctx
{
    Loader& loader;
    TestHost& host;
    const clap_plugin_factory_t* factory;
    const Options& options;
};

bool streq (const char* a, const char* b) { return a != nullptr && b != nullptr && std::strcmp (a, b) == 0; }

// JUCE profile (--juce): the JUCE CLAP wrapper (clap-juce-extensions) hands plain values to the processor as a float
// *normalised* number, so a float parameter read back through clap.params differs from the value that was written by
// a few ulp (the processor itself stores the exact plain value, see juce/CoreParameter.h). Float parameters are
// therefore compared with a relative tolerance in that profile; bool and enum parameters stay exact.
bool near (const Ctx& c, uint32_t id, double got, double want)
{
    if (!c.options.juce)
        return got == want;
    const int i = indexOfId (id);
    if (i < 0 || paramInfo (i).kind != Kind::Float)
        return got == want;
    return std::fabs (got - want) <= 1.0e-5 * std::max (1.0, std::fabs (want));
}

struct Rng
{
    uint32_t s;
    explicit Rng (uint32_t seed) : s (seed != 0 ? seed : 1) {}
    uint32_t u32()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    double uni() { return static_cast<double> (u32()) / 4294967296.0; }
};

std::vector<uint8_t> blobFor (const ParamValues& v)
{
    std::vector<uint8_t> b (saveState (v, nullptr, 0));
    saveState (v, b.data(), b.size());
    return b;
}

ParamValues presetValues (int index)
{
    ParamValues v;
    applyPreset (index, v);
    return v;
}

uint32_t idOf (int band, BandParam k) { return bandParamId (band, k); }

struct Stereo
{
    std::vector<float> l, r;
    explicit Stereo (size_t n = 0) : l (n), r (n) {}
    bool operator== (const Stereo& o) const { return bitEqual (l, o.l) && bitEqual (r, o.r); }
};

Stereo noiseStereo (size_t n, uint32_t seed) { Stereo s (0); s.l = makeNoise (n, seed); s.r = makeNoise (n, seed * 7u + 3u); return s; }

/** Reference: the core engine, driven exactly like the shell splits a block at its events. */
void refBlock (EqEngine& eng, ParamValues& pv, float* l, float* r, uint32_t frames, const std::vector<ParamEvent>& ev)
{
    uint32_t pos = 0;
    for (const ParamEvent& e : ev)
    {
        const uint32_t t = e.time < frames ? e.time : frames;
        if (t > pos)
        {
            eng.process (pv, l + pos, r != nullptr ? r + pos : nullptr, static_cast<int> (t - pos));
            pos = t;
        }
        const int idx = indexOfId (e.id);
        if (idx >= 0)
            pv.set (idx, static_cast<float> (e.value));
    }
    if (frames > pos)
        eng.process (pv, l + pos, r != nullptr ? r + pos : nullptr, static_cast<int> (frames - pos));
}

/** The JUCE wrapper converts the plain value of an event to a float normalised value before the processor sees it;
    CoreParameter turns it back with the core's fromNormalized, so the engine gets fromNormalized (toNormalized (v)). */
void applyViaNormalised (ParamValues& pv, const ParamEvent& e)
{
    const int idx = indexOfId (e.id);
    if (idx < 0)
        return;
    const ParamInfo& info = paramInfo (idx);
    const float normalised = toNormalized (info, static_cast<float> (e.value));
    pv.set (idx, fromNormalized (info, normalised));
}

/** Reference for the JUCE CLAP wrapper (clap-juce-extensions, built with CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES 64,
    CLAP_ALWAYS_SPLIT_BLOCK off): it does not split exactly at an event but works in sub-blocks that are a multiple
    of 64 samples long. A sub-block starting at n ends at the first multiple of 64 samples (counted from n) that
    reaches the next parameter event at or after n + 64 (or at the end of the host block); every event that lies
    inside the sub-block is applied before it is processed. The engine itself is block-size independent, so the
    only difference to the native shell is the point in time at which an event reaches the engine. */
void refBlockJuce (EqEngine& eng, ParamValues& pv, float* l, float* r, uint32_t frames, const std::vector<ParamEvent>& ev)
{
    constexpr uint32_t resolution = 64;
    size_t next = 0;
    uint32_t n = 0;
    while (n < frames)
    {
        const uint32_t remaining = frames - n;
        uint32_t untilNext = remaining;
        for (size_t e = next; e < ev.size(); ++e)
            if (ev[e].time >= n + resolution)
            {
                untilNext = ev[e].time - n;
                break;
            }
        uint32_t len = remaining;
        if (remaining > resolution)
        {
            const uint32_t smallBlocks = (untilNext + resolution - 1) / resolution;
            len = smallBlocks * resolution < remaining ? smallBlocks * resolution : remaining;
        }
        while (next < ev.size() && ev[next].time < n + len)
        {
            applyViaNormalised (pv, ev[next]);
            ++next;
        }
        eng.process (pv, l + n, r != nullptr ? r + n : nullptr, static_cast<int> (len));
        n += len;
    }
    for (; next < ev.size(); ++next) // events at or beyond the end of the block
        applyViaNormalised (pv, ev[next]);
}

std::unique_ptr<EqEngine> makeRef (double rate)
{
    auto e = std::make_unique<EqEngine>();
    e->prepare (rate, 512);
    return e;
}

/** Runs `in` through the shell block by block (block sizes from `blocks`, repeated). */
Stereo shellRun (Instance& inst, const Stereo& in, const std::vector<uint32_t>& blocks, bool inPlace)
{
    Stereo out = inPlace ? in : Stereo (in.l.size());
    size_t pos = 0, bi = 0;
    while (pos < in.l.size())
    {
        uint32_t n = blocks[bi++ % blocks.size()];
        if (n > in.l.size() - pos)
            n = static_cast<uint32_t> (in.l.size() - pos);
        if (inPlace)
            inst.process (out.l.data() + pos, out.r.data() + pos, out.l.data() + pos, out.r.data() + pos, n);
        else
            inst.process (in.l.data() + pos, in.r.data() + pos, out.l.data() + pos, out.r.data() + pos, n);
        pos += n;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------

void testDescriptor (Ctx& c)
{
    section ("descriptor and factory");
    const clap_plugin_factory_t* f = c.factory;
    check (f != nullptr, "plugin factory is available");
    if (f == nullptr)
        return;
    check (f->get_plugin_count (f) == 1, "exactly one plugin");
    const clap_plugin_descriptor_t* d = f->get_plugin_descriptor (f, 0);
    check (d != nullptr, "descriptor 0 exists");
    if (!c.options.juce) // the JUCE wrapper returns its descriptor for any index
        check (f->get_plugin_descriptor (f, 1) == nullptr, "descriptor 1 does not exist");
    if (d == nullptr)
        return;
    check (clap_version_is_compatible (d->clap_version), "descriptor clap_version compatible");
    check (streq (d->id, "com.safian.kickeq"), "id com.safian.kickeq", d->id != nullptr ? d->id : "(null)");
    check (streq (d->name, "KickEQ"), "name KickEQ");
    check (streq (d->vendor, "Safian"), "vendor Safian");
    if (!c.options.juce) // the JUCE wrapper publishes JucePlugin_ManufacturerWebsite (not set for this plugin)
        check (streq (d->url, "https://github.com/Safian/KickEQ"), "url");
    check (streq (d->version, KICKEQ_EXPECTED_VERSION), "version from CMake project()", d->version != nullptr ? d->version : "(null)");
    check (d->description != nullptr && d->description[0] != '\0', "description present");

    std::vector<std::string> feats;
    for (const char* const* p = d->features; p != nullptr && *p != nullptr; ++p)
        feats.emplace_back (*p);
    check (feats == std::vector<std::string> { "audio-effect", "equalizer", "stereo" }, "features: audio-effect equalizer stereo");

    check (f->create_plugin (f, c.host.get(), "com.example.other") == nullptr, "create_plugin with a foreign id fails");
    if (!c.options.juce) // the JUCE wrapper dereferences the host / id without checking (not called that way by hosts)
    {
        check (f->create_plugin (f, nullptr, "com.safian.kickeq") == nullptr, "create_plugin without host fails");
        check (f->create_plugin (f, c.host.get(), nullptr) == nullptr, "create_plugin without id fails");
    }
    check (c.loader.getFactory ("com.example.no-such-factory") == nullptr, "unknown factory id returns null");
    if (!c.options.juce) // no clap.preset-discovery in the JUCE shell (factory presets are JUCE programs there)
        check (c.loader.presets() != nullptr, "preset discovery factory is exposed");

    Instance inst (c.host, f);
    check (inst.ok() && inst.plugin->desc == d, "plugin->desc is the factory descriptor");
    if (c.options.juce) // the JUCE shell is the one that offers an editor
        check (inst.plugin != nullptr && inst.plugin->get_extension (inst.plugin, CLAP_EXT_GUI) != nullptr, "clap.gui is offered");
    else
        check (inst.plugin != nullptr && inst.plugin->get_extension (inst.plugin, "clap.gui") == nullptr, "clap.gui is not offered");
    check (inst.plugin != nullptr && inst.plugin->get_extension (inst.plugin, "com.example.unknown") == nullptr, "unknown extension is null");
}

void testLifecycle (Ctx& c)
{
    section ("lifecycle");
    bool ok = true;
    for (int i = 0; i < 100; ++i)
    {
        Instance inst (c.host, c.factory);
        ok = ok && inst.ok();
    }
    check (ok, "100x create / init / destroy");

    ok = true;
    for (int i = 0; i < 20; ++i)
    {
        Instance inst (c.host, c.factory);
        const double rates[3] = { 44100.0, 48000.0, 96000.0 };
        ok = ok && inst.ok() && inst.activate (rates[i % 3], 1, 512);
        std::vector<float> a = makeNoise (512, 5), b (512);
        inst.process (a.data(), a.data(), b.data(), b.data(), 512);
        inst.deactivate();
        ok = ok && inst.activate (rates[(i + 1) % 3], 16, 4096);
        inst.process (a.data(), a.data(), b.data(), b.data(), 512);
        ok = ok && allFinite (b);
    }
    check (ok, "20x activate / process / deactivate / re-activate at another rate");

    if (!c.options.juce) // the JUCE wrapper accepts any rate (hosts never pass 0)
    {
        Instance inst (c.host, c.factory);
        check (!inst.plugin->activate (inst.plugin, 0.0, 1, 512), "activate with sample rate 0 fails");
        check (!inst.plugin->activate (inst.plugin, -48000.0, 1, 512), "activate with a negative sample rate fails");
    }

    {
        Instance i0 (c.host, c.factory), i1 (c.host, c.factory), i2 (c.host, c.factory), i3 (c.host, c.factory);
        i0.setParam (idOf (0, BandParam::freq), 77.0);
        i1.setParam (idOf (0, BandParam::freq), 88.0);
        i2.setParam (paramInfo (paramOutGain).id, 5.0);
        const uint32_t f0 = idOf (0, BandParam::freq), og = paramInfo (paramOutGain).id;
        bool indep = near (c, f0, i0.value (f0), 77.0) && near (c, f0, i1.value (f0), 88.0)
                     && near (c, f0, i2.value (f0), 30.0) && near (c, f0, i3.value (f0), 30.0)
                     && near (c, og, i2.value (og), 5.0) && near (c, og, i0.value (og), 0.0);
        check (indep, "4 simultaneous instances keep independent parameters");
    }

    {
        TestHost bare (false); // a host that offers no extensions at all
        Instance inst (bare, c.factory);
        bool bareOk = inst.ok() && inst.activate (48000.0);
        std::vector<float> a = makeNoise (256, 9), b (256);
        inst.process (a.data(), a.data(), b.data(), b.data(), 256);
        bareOk = bareOk && bitEqual (a, b);
        bareOk = bareOk && inst.loadState (inst.saveState());
        inst.pump();
        check (bareOk, "works with a host that offers no extensions");
    }
}

void testParams (Ctx& c)
{
    section ("clap.params");
    Instance inst (c.host, c.factory);
    check (inst.params != nullptr, "params extension present");
    if (inst.params == nullptr)
        return;
    const clap_plugin_t* p = inst.plugin;
    const clap_plugin_params_t* pe = inst.params;

    check (pe->count (p) == static_cast<uint32_t> (numParams), "count == core table", fmt ("%u", pe->count (p)));

    std::set<clap_id> ids;
    std::string firstBad;
    // JUCE profile: ids are String::hashCode of the string id (the host-side ids differ from the core ids)
    const auto javaHash = [] (const char* text) {
        int32_t h = 0;
        for (; *text != '\0'; ++text)
            h = static_cast<int32_t> (31u * static_cast<uint32_t> (h) + static_cast<uint32_t> (static_cast<unsigned char> (*text)));
        return static_cast<uint32_t> (h);
    };
    bool hashOk = true, mappedOk = true;
    bool infoOk = true, orderOk = true, flagsOk = true, rangesOk = true, namesOk = true, defaultsOk = true;
    for (int i = 0; i < numParams; ++i)
    {
        clap_param_info_t info {};
        if (!pe->get_info (p, static_cast<uint32_t> (i), &info))
        {
            infoOk = false;
            continue;
        }
        const ParamInfo& t = paramInfo (i);
        ids.insert (info.id);
        hashOk = hashOk && info.id == javaHash (t.strId);
        mappedOk = mappedOk && inst.hostId (t.id) == info.id;
        orderOk = orderOk && (c.options.juce ? true : info.id == t.id);
        // JUCE profile: the bands are parameter groups (CLAP module "Band N"); the three global parameters sit at the
        // top level of the JUCE parameter tree, which the wrapper reports as an empty module path
        const bool moduleOk = c.options.juce && std::strcmp (t.module, "Global") == 0 ? info.module[0] == '\0'
                                                                                       : streq (info.module, t.module) && info.module[0] != '\0';
        namesOk = namesOk && streq (info.name, t.name) && moduleOk && info.name[0] != '\0';
        rangesOk = rangesOk && info.min_value == static_cast<double> (t.min) && info.max_value == static_cast<double> (t.max)
                   && info.min_value < info.max_value && std::isfinite (info.min_value) && std::isfinite (info.max_value);
        defaultsOk = defaultsOk && near (c, t.id, info.default_value, static_cast<double> (t.def)) && info.default_value >= info.min_value
                     && info.default_value <= info.max_value;
        const bool stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        // JUCE profile: the wrapper flags only bool parameters as stepped, never as enum or bypass
        const bool wantStepped = c.options.juce ? t.kind == Kind::Bool : t.kind != Kind::Float;
        const bool wantEnum = !c.options.juce && t.kind == Kind::Enum;
        const bool wantBypass = !c.options.juce && i == paramBypass;
        // phase_mode changes the latency: not automatable; without the feature it is hidden and read-only
        const bool wantAutomatable = (t.flags & flagAutomatable) != 0;
        const bool unavailable = (t.flags & flagNeedsLinearPhase) != 0 && !linearPhaseOn();
        flagsOk = flagsOk && ((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0) == wantAutomatable && stepped == wantStepped
                  && ((info.flags & CLAP_PARAM_IS_ENUM) != 0) == wantEnum
                  && ((info.flags & CLAP_PARAM_IS_BYPASS) != 0) == wantBypass
                  // JUCE profile: the wrapper has no hidden / read-only flags; the JUCE shell keeps the parameter non-automatable
                  // and pins its value to Minimum instead (the editor hides the box)
                  && (c.options.juce || ((info.flags & CLAP_PARAM_IS_HIDDEN) != 0) == unavailable)
                  && (c.options.juce || ((info.flags & CLAP_PARAM_IS_READONLY) != 0) == unavailable);
        if (!flagsOk && firstBad.empty())
            firstBad = fmt ("param %d (%s) flags 0x%x", i, t.strId, info.flags);
    }
    check (infoOk, "get_info succeeds for every index");
    check (ids.size() == static_cast<size_t> (numParams), "ids are unique");
    if (c.options.juce)
    {
        check (hashOk, "JUCE: parameter id == String::hashCode of the core string id");
        check (mappedOk, "JUCE: every core parameter maps to exactly one host parameter (by name)");
    }
    else
    {
        check (orderOk, "index order == core table order");
    }
    check (namesOk, "names and module paths == core table, non-empty");
    check (rangesOk, "min / max == core table, finite");
    check (defaultsOk, "default values == core table, inside range");
    check (flagsOk, "flags: automatable (not phase_mode); stepped for bool+enum; enum; bypass only on bypass; phase_mode hidden+read-only only without linear phase", firstBad);
    clap_param_info_t dummy {};
    check (!pe->get_info (p, static_cast<uint32_t> (numParams), &dummy), "get_info out of range fails");
    double out = 0;
    if (!c.options.juce) // the JUCE wrapper indexes its id map without checking
        check (!pe->get_value (p, 0xDEAD, &out) && !pe->get_value (p, 4, &out), "get_value of an unknown / unused id fails");

    bool defOk = true;
    for (int i = 0; i < numParams; ++i)
        defOk = defOk && near (c, paramInfo (i).id, inst.value (paramInfo (i).id), static_cast<double> (paramInfo (i).def));
    check (defOk, "get_value of a fresh instance == defaults");

    // text conversion round trips
    bool textOk = true, idemOk = true, smallBufOk = true, junkOk = true;
    std::string detail;
    for (int i = 0; i < numParams; ++i)
    {
        const ParamInfo& t = paramInfo (i);
        if ((t.flags & flagNeedsLinearPhase) != 0 && !linearPhaseOn())
            continue; // pinned to its default in this build: no round trip of other values
        std::vector<double> values;
        if (t.kind != Kind::Float)
            for (int v = static_cast<int> (t.min); v <= static_cast<int> (t.max); ++v)
                values.push_back (v);
        else
            values = { (double) t.min, (double) t.def, (double) t.max, 0.5 * ((double) t.min + (double) t.max),
                       (double) t.min + 0.1234 * ((double) t.max - (double) t.min) };
        for (double v : values)
        {
            char a[128] = {}, b[128] = {};
            double back = 0;
            const bool o1 = pe->value_to_text (p, inst.hostId (t.id), v, a, sizeof a);
            const bool o2 = o1 && pe->text_to_value (p, inst.hostId (t.id), a, &back);
            const bool o3 = o2 && pe->value_to_text (p, inst.hostId (t.id), back, b, sizeof b);
            const bool exact = t.kind == Kind::Float || back == v;
            const bool idem = o3 && std::strcmp (a, b) == 0;
            if (!(o1 && o2 && a[0] != '\0' && exact && idem) && detail.empty())
                detail = fmt ("%s v=%g text='%s' back=%g again='%s'", t.strId, v, a, back, b);
            textOk = textOk && o1 && o2 && a[0] != '\0' && exact;
            idemOk = idemOk && idem;
        }
        if (!c.options.juce) // the JUCE wrapper uses strncpy (no terminator when truncated) and cannot reject text
        {
            char tiny[3] = { 'x', 'x', 'x' };
            smallBufOk = smallBufOk && pe->value_to_text (p, t.id, t.def, tiny, 3) && std::strlen (tiny) < 3;
            double d = 0;
            junkOk = junkOk && !pe->text_to_value (p, t.id, "zzz not a value", &d);
        }
    }
    check (textOk, "value_to_text -> text_to_value round trip (exact for bool/enum)", detail);
    check (idemOk, "text is stable: value_to_text (text_to_value (text)) == text", detail);
    if (!c.options.juce)
    {
        check (smallBufOk, "value_to_text honours a tiny buffer (NUL terminated)");
        check (junkOk, "text_to_value rejects garbage");
    }

    // events through flush (plugin not processing)
    bool maxOk = true, minOk = true, midOk = true, clampOk = true, nanOk = true;
    for (int i = 0; i < numParams; ++i)
    {
        const ParamInfo& t = paramInfo (i);
        // (a parameter the build does not have, phase_mode without linear phase, always reads its default)
        inst.setParam (t.id, t.max);
        maxOk = maxOk && near (c, t.id, inst.value (t.id), static_cast<double> (clampParam (t, t.max)));
        inst.setParam (t.id, t.min);
        minOk = minOk && near (c, t.id, inst.value (t.id), static_cast<double> (clampParam (t, t.min)));
        const double mid = t.min + 0.37 * (t.max - t.min);
        inst.setParam (t.id, mid);
        const float expect = clampParam (t, static_cast<float> (mid));
        midOk = midOk && near (c, t.id, inst.value (t.id), static_cast<double> (expect));
        inst.setParam (t.id, 1.0e300);
        clampOk = clampOk && near (c, t.id, inst.value (t.id), static_cast<double> (clampParam (t, t.max)));
        inst.setParam (t.id, -1.0e300);
        clampOk = clampOk && near (c, t.id, inst.value (t.id), static_cast<double> (clampParam (t, t.min)));
        inst.setParam (t.id, std::nan (""));
        nanOk = nanOk && near (c, t.id, inst.value (t.id), static_cast<double> (t.def));
    }
    check (maxOk && minOk, "flush: PARAM_VALUE at min / max is applied");
    check (midOk, "flush: mid values are applied (stepped ones rounded)");
    check (clampOk, "flush: out-of-range values are clamped");
    check (nanOk, "flush: NaN becomes the default");

    const auto before = inst.allValues();
    inst.setParams ({ { 0, 0xDEAD, 1.0 }, { 0, 4, 1.0 } });
    check (before == inst.allValues(), "flush: unknown and unused ids are ignored");

    // events of another event space, or of another type, are not parameter changes
    inst.setParam (paramInfo (paramOutGain).id, 0.0);
    {
        // a foreign space id must be ignored: build the event by hand
        clap_event_param_value_t ev {};
        ev.header.size = sizeof ev;
        ev.header.space_id = 77;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.param_id = inst.hostId (paramInfo (paramOutGain).id);
        ev.value = 12.0;
        struct L
        {
            const clap_event_header_t* h;
        } list { &ev.header };
        clap_input_events_t in = { &list,
                                   [] (const clap_input_events_t*) -> uint32_t { return 1; },
                                   [] (const clap_input_events_t* l, uint32_t) -> const clap_event_header_t* {
                                       return static_cast<const L*> (l->ctx)->h;
                                   } };
        clap_output_events_t sink = { nullptr, [] (const clap_output_events_t*, const clap_event_header_t*) -> bool { return true; } };
        pe->flush (p, &in, &sink);
        check (near (c, paramInfo (paramOutGain).id, inst.value (paramInfo (paramOutGain).id), 0.0), "flush: events of a foreign event space are ignored");
    }

    // set while active (flush on the audio thread)
    Instance act (c.host, c.factory);
    act.activate (48000.0);
    act.setParam (paramInfo (paramOutGain).id, -6.0);
    check (near (c, paramInfo (paramOutGain).id, act.value (paramInfo (paramOutGain).id), -6.0), "flush while active applies events");
}

void testState (Ctx& c)
{
    section ("clap.state");
    Instance a (c.host, c.factory), b (c.host, c.factory);
    check (a.state != nullptr, "state extension present");
    if (a.state == nullptr)
        return;

    // distinct, non-default values everywhere
    Rng rng (99);
    for (int i = 0; i < numParams; ++i)
    {
        const ParamInfo& t = paramInfo (i);
        a.setParam (t.id, t.min + rng.uni() * (t.max - t.min));
    }
    const auto valuesA = a.allValues();

    bool saved = false;
    const std::vector<uint8_t> blob = a.saveState (0, &saved);
    check (saved && blob.size() > 16, "save succeeds", fmt ("%zu bytes", blob.size()));
    ParamValues viaCore;
    check (loadState (blob.data(), blob.size(), viaCore), "the saved blob is a valid core state");
    bool coreEq = true;
    for (int i = 0; i < numParams; ++i)
        coreEq = coreEq && near (c, paramInfo (i).id, valuesA[static_cast<size_t> (i)], static_cast<double> (viaCore.get (i)));
    check (coreEq, "core loadState of the blob == plugin parameter values");

    c.host.paramsRescan = 0;
    check (b.loadState (blob), "load into a fresh instance");
    if (!c.options.juce) // the JUCE wrapper announces changes through the processor listener instead
        check (c.host.paramsRescan.load() == 1, "state load asks the host to rescan the parameter values (main thread)");
    check (b.allValues() == valuesA, "state save -> load: identical parameters");
    check (b.saveState() == blob, "save is deterministic (save -> load -> save is identical)");

    bool okWrite = false;
    const std::vector<uint8_t> chunked = a.saveState (3, &okWrite);
    check (okWrite && chunked == blob, "save through a stream that writes 3 bytes at a time");
    bool failedSave = true;
    a.saveState (static_cast<size_t> (-1), &failedSave);
    check (!failedSave, "save fails when the stream fails");

    bool shortOk = true;
    for (size_t chunk : { size_t (1), size_t (3), size_t (7), size_t (4096) })
    {
        Instance fresh (c.host, c.factory);
        shortOk = shortOk && fresh.loadState (blob, chunk) && fresh.allValues() == valuesA;
    }
    check (shortOk, "load through streams that return 1 / 3 / 7 / 4096 bytes per read");

    // truncation, garbage, stream errors: fail gracefully, parameters untouched
    {
        Instance victim (c.host, c.factory);
        victim.setParam (idOf (3, BandParam::freq), 1234.0);
        auto snap = victim.allValues();
        bool allFail = true, untouched = true;
        for (size_t len = 0; len < blob.size(); ++len)
        {
            std::vector<uint8_t> cut (blob.begin(), blob.begin() + static_cast<std::ptrdiff_t> (len));
            const bool r = victim.loadState (cut, len % 5 + 1);
            // the JUCE wrapper cannot report a rejected state (setStateInformation is void): it returns true
            // for every non-empty stream; the processor ignores an invalid blob, which `untouched` verifies
            allFail = allFail && (c.options.juce ? (len != 0 || !r) : !r);
            untouched = untouched && victim.allValues() == snap;
        }
        check (allFail, "every truncated state is rejected", "");
        check (untouched, "rejected loads leave the parameters untouched");

        bool noCrash = true;
        Rng gr (4711);
        for (int i = 0; i < 300; ++i)
        {
            std::vector<uint8_t> junk (gr.u32() % 700);
            for (uint8_t& x : junk)
                x = static_cast<uint8_t> (gr.u32());
            if (i % 3 == 0 && junk.size() > 4)
                std::memcpy (junk.data(), "KEQS", 4);
            const bool r = victim.loadState (junk);
            const auto now = victim.allValues();
            for (int k = 0; k < numParams; ++k)
                noCrash = noCrash && now[static_cast<size_t> (k)] >= static_cast<double> (paramInfo (k).min)
                          && now[static_cast<size_t> (k)] <= static_cast<double> (paramInfo (k).max);
            // a blob the core rejects must leave the parameters untouched, whatever the shell returns for it
            ParamValues parsed;
            const bool valid = loadState (junk.data(), junk.size(), parsed);
            if (!valid && (!r || c.options.juce))
                noCrash = noCrash && now == snap;
            snap = now; // (very unlikely) accepted blob: it becomes the reference for later rejections
        }
        check (noCrash, "300 garbage / random-header blobs: no crash, values always in range");

        bool fuzzOk = true;
        for (int i = 0; i < 1000; ++i)
        {
            std::vector<uint8_t> m = blob;
            const uint32_t flips = 1 + gr.u32() % 4;
            for (uint32_t k = 0; k < flips; ++k)
                m[gr.u32() % m.size()] = static_cast<uint8_t> (gr.u32());
            victim.loadState (m);
            const auto now = victim.allValues();
            for (int k = 0; k < numParams; ++k)
                fuzzOk = fuzzOk && now[static_cast<size_t> (k)] >= static_cast<double> (paramInfo (k).min)
                         && now[static_cast<size_t> (k)] <= static_cast<double> (paramInfo (k).max);
        }
        check (fuzzOk, "1000 bit-flipped states: no crash, values always in range");

        victim.loadState (blob);
        Instance v2 (c.host, c.factory);
        v2.setParam (idOf (3, BandParam::freq), 1234.0);
        const auto snap2 = v2.allValues();
        check (!v2.loadState (blob, 0, 40) && v2.allValues() == snap2, "stream read error mid-state: load fails, values untouched");
    }

    // a foreign editor chunk is preserved across a load / save (JUCE shell compatibility)
    if (!c.options.juce) // the JUCE shell owns the UI01 chunk: it parses its own layout and writes it back normalised
    {
        const uint8_t chunk[5] = { 1, 2, 3, 4, 5 };
        UiChunk ui;
        ui.data = chunk;
        ui.size = 5;
        const size_t n = saveState (ParamValues {}, nullptr, 0, &ui);
        std::vector<uint8_t> withUi (n);
        saveState (ParamValues {}, withUi.data(), n, &ui);
        Instance x (c.host, c.factory);
        const bool loaded = x.loadState (withUi);
        const std::vector<uint8_t> again = x.saveState();
        ParamValues tmp;
        UiChunk back;
        const bool parsed = loadState (again.data(), again.size(), tmp, &back);
        check (loaded && parsed && back.size == 5 && std::memcmp (back.data, chunk, 5) == 0, "editor chunk (UI01) of a foreign state survives load -> save");
    }

    // forward compatibility: unknown ids ignored (hand-made blob with an extra id)
    {
        std::vector<uint8_t> raw;
        const auto put32 = [&raw] (uint32_t v) { for (int i = 0; i < 4; ++i) raw.push_back (static_cast<uint8_t> (v >> (8 * i))); };
        for (const char ch : "KEQS")
            if (ch != '\0')
                raw.push_back (static_cast<uint8_t> (ch));
        put32 (7); // a later version is accepted
        put32 (2);
        put32 (paramInfo (paramOutGain).id);
        const float g = -4.0f;
        uint32_t gb = 0;
        std::memcpy (&gb, &g, 4);
        put32 (gb);
        put32 (0xBEEF); // unknown id
        put32 (0x3f800000u);
        put32 (0);
        Instance x (c.host, c.factory);
        check (x.loadState (raw) && near (c, paramInfo (paramOutGain).id, x.value (paramInfo (paramOutGain).id), -4.0), "state of a later version with an unknown id loads");
    }
}

void testLatencyTail (Ctx& c)
{
    section ("clap.latency and clap.tail");
    Instance inst (c.host, c.factory);
    check (inst.latency != nullptr && inst.tail != nullptr, "latency and tail extensions present");
    if (inst.latency == nullptr || inst.tail == nullptr)
        return;
    check (inst.latency->get (inst.plugin) == 0, "latency 0 before activate");
    inst.activate (48000.0, 1, 1024);
    check (inst.latency->get (inst.plugin) == 0, "latency 0 when active");
    check (inst.tail->get (inst.plugin) == 0, "tail 0 for a flat EQ");

    ParamValues v;
    v.set (2, BandParam::on, 1.0f);
    v.set (2, BandParam::freq, 40.0f);
    v.set (2, BandParam::q, 18.0f);
    v.set (2, BandParam::gain, 6.0f);
    check (inst.loadState (blobFor (v)), "load a narrow low bell");

    auto ref = makeRef (48000.0);
    ParamValues pv = v;
    std::vector<float> in = makeNoise (512, 3), outL (512), outR (512), rl = in, rr = in;
    c.host.tailChanged = 0;
    inst.process (in.data(), in.data(), outL.data(), outR.data(), 512);
    ref->process (pv, rl.data(), rr.data(), 512);
    const uint32_t tail = inst.tail->get (inst.plugin);
    check (tail > 0 && tail == ref->getTailSamples(), "tail == core EqEngine tail after processing", fmt ("%u vs %u", tail, ref->getTailSamples()));
    // the JUCE wrapper reports the tail only when asked (no clap_host_tail::changed): not checked in that profile
    if (!c.options.juce)
        check (c.host.tailChanged.load() == 1, "material tail change -> host tail.changed from the audio thread (once)", fmt ("%d", c.host.tailChanged.load()));

    inst.process (in.data(), in.data(), outL.data(), outR.data(), 512);
    if (!c.options.juce)
        check (c.host.tailChanged.load() == 1, "unchanged tail: no further tail.changed");

    inst.setParam (paramInfo (paramBypass).id, 1.0);
    for (int i = 0; i < 4; ++i)
        inst.process (in.data(), in.data(), outL.data(), outR.data(), 512);
    if (c.options.juce)
    {
        check (inst.tail->get (inst.plugin) == 0, "bypass: tail drops to 0", fmt ("tail %u", inst.tail->get (inst.plugin)));
    }
    else
    {
        check (inst.tail->get (inst.plugin) == 0 && c.host.tailChanged.load() == 2, "bypass: tail drops to 0 and the host is told", fmt ("tail %u changed %d", inst.tail->get (inst.plugin), c.host.tailChanged.load()));
        inst.pump(); // on_main_thread is a harmless no-op
        check (c.host.tailChanged.load() == 2, "on_main_thread does not call back into the host");
    }
}

void testPorts (Ctx& c)
{
    section ("clap.audio-ports and audio-ports-config");
    Instance inst (c.host, c.factory);
    // the JUCE wrapper offers audio-ports only: the layout (stereo, or mono in the VST3) is the host's choice there
    check (inst.ports != nullptr && (c.options.juce || inst.portsConfig != nullptr), c.options.juce ? "audio-ports present" : "audio-ports and audio-ports-config present");
    if (inst.ports == nullptr || (!c.options.juce && inst.portsConfig == nullptr))
        return;
    const clap_plugin_t* p = inst.plugin;
    check (inst.ports->count (p, true) == 1 && inst.ports->count (p, false) == 1, "1 input port, 1 output port");
    clap_audio_port_info_t in {}, out {};
    check (inst.ports->get (p, 0, true, &in) && inst.ports->get (p, 0, false, &out), "get port 0 in / out");
    check (in.channel_count == 2 && out.channel_count == 2 && streq (in.port_type, CLAP_PORT_STEREO) && streq (out.port_type, CLAP_PORT_STEREO), "stereo in / out");
    check ((in.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0 && (out.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0, "ports are main ports");
    if (!c.options.juce) // the JUCE wrapper does not declare in-place pairs
        check (in.in_place_pair == out.id && out.in_place_pair == in.id, "in-place pair links input and output");
    check (in.name[0] != '\0', "port has a name");
    clap_audio_port_info_t none {};
    check (!inst.ports->get (p, 1, true, &none) && !inst.ports->get (p, 1, false, &none), "port 1 does not exist");
    if (c.options.juce)
        return;

    check (inst.portsConfig->count (p) == 2, "2 audio-ports configs");
    clap_audio_ports_config_t cfg0 {}, cfg1 {};
    check (inst.portsConfig->get (p, 0, &cfg0) && inst.portsConfig->get (p, 1, &cfg1), "configs readable");
    check (cfg0.main_input_channel_count == 2 && cfg0.main_output_channel_count == 2 && streq (cfg0.main_input_port_type, CLAP_PORT_STEREO), "config 0 = stereo");
    check (cfg1.main_input_channel_count == 1 && cfg1.main_output_channel_count == 1 && streq (cfg1.main_output_port_type, CLAP_PORT_MONO), "config 1 = mono");
    check (cfg0.id != cfg1.id && cfg0.has_main_input && cfg0.has_main_output && cfg0.input_port_count == 1, "config ids differ, main ports present");
    check (!inst.portsConfig->select (p, 99), "select of an unknown config fails");
    check (inst.portsConfig->select (p, cfg1.id), "select mono while inactive");
    clap_audio_port_info_t mono {};
    check (inst.ports->get (p, 0, true, &mono) && mono.channel_count == 1 && streq (mono.port_type, CLAP_PORT_MONO), "port 0 is mono after select");
    check (inst.portsConfig->select (p, cfg0.id), "select stereo again");
    inst.activate (48000.0);
    check (!inst.portsConfig->select (p, cfg1.id), "select while active is refused");
}

void testRemoteControls (Ctx& c)
{
    section ("clap.remote-controls");
    Instance inst (c.host, c.factory);
    check (inst.remote != nullptr, "remote-controls extension present");
    if (inst.remote == nullptr)
        return;
    const uint32_t n = inst.remote->count (inst.plugin);
    check (n == static_cast<uint32_t> (numBands + 1), "one page per band + one global page", fmt ("%u pages", n));

    std::multiset<clap_id> seen;
    std::set<clap_id> pageIds;
    bool namesOk = true, shapeOk = true;
    for (uint32_t i = 0; i < n; ++i)
    {
        clap_remote_controls_page_t pg {};
        if (!inst.remote->get (inst.plugin, i, &pg))
        {
            shapeOk = false;
            continue;
        }
        pageIds.insert (pg.page_id);
        namesOk = namesOk && pg.section_name[0] != '\0' && pg.page_name[0] != '\0';
        for (clap_id id : pg.param_ids)
            if (id != CLAP_INVALID_ID)
            {
                seen.insert (id);
                shapeOk = shapeOk && indexOfId (id) >= 0;
            }
        if (i < static_cast<uint32_t> (numBands))
        {
            // band pages: on / type / freq / gain / q / place / slope, in this order
            for (int k = 0; k < paramsPerBand; ++k)
                shapeOk = shapeOk && pg.param_ids[k] == bandParamId (static_cast<int> (i), static_cast<BandParam> (k));
            shapeOk = shapeOk && pg.param_ids[7] == CLAP_INVALID_ID;
        }
        else
        {
            shapeOk = shapeOk && pg.param_ids[0] == paramInfo (paramBypass).id && pg.param_ids[1] == paramInfo (paramOutGain).id
                      && pg.param_ids[2] == paramInfo (paramAutoGain).id
                      && pg.param_ids[3] == (linearPhaseOn() ? paramInfo (paramPhaseMode).id : CLAP_INVALID_ID);
        }
    }
    check (shapeOk, "pages: band pages hold on/type/freq/gain/q/place/slope, global page bypass/out_gain/auto_gain/phase_mode");
    check (namesOk, "pages have section and page names");
    check (pageIds.size() == n, "page ids are unique");
    const size_t available = static_cast<size_t> (numParams) - (linearPhaseOn() ? 0u : 1u); // phase_mode is unavailable without linear phase
    bool coverOk = seen.size() == available;
    for (int i = 0; i < numParams; ++i)
        coverOk = coverOk && seen.count (paramInfo (i).id) == ((paramInfo (i).flags & flagNeedsLinearPhase) != 0 && !linearPhaseOn() ? 0u : 1u);
    check (coverOk, "the pages cover every parameter exactly once", fmt ("%zu entries", seen.size()));
    clap_remote_controls_page_t none {};
    check (!inst.remote->get (inst.plugin, n, &none), "page index out of range fails");
}

struct Recorder
{
    std::vector<std::string> names, keys;
    int pluginIdsOk = 0, flagsOk = 0, creators = 0, errors = 0;
    std::vector<clap_preset_discovery_location_t> locations;
    std::vector<std::string> locNames;
};

void testPresets (Ctx& c)
{
    section ("clap.preset-discovery and clap.preset-load");
    const clap_preset_discovery_factory_t* pf = c.loader.presets();
    check (pf != nullptr, "preset discovery factory present");
    if (pf == nullptr)
        return;
    check (pf->count (pf) == 1, "one provider");
    const clap_preset_discovery_provider_descriptor_t* d = pf->get_descriptor (pf, 0);
    check (d != nullptr && d->id != nullptr && d->name != nullptr && pf->get_descriptor (pf, 1) == nullptr, "provider descriptor");
    if (d == nullptr)
        return;

    Recorder rec;
    clap_preset_discovery_indexer_t ix {};
    ix.clap_version = CLAP_VERSION;
    ix.name = "kickeq-testhost";
    ix.indexer_data = &rec;
    ix.declare_filetype = [] (const clap_preset_discovery_indexer_t*, const clap_preset_discovery_filetype_t*) -> bool { return true; };
    ix.declare_location = [] (const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_location_t* l) -> bool {
        Recorder& r = *static_cast<Recorder*> (i->indexer_data);
        r.locations.push_back (*l);
        r.locNames.emplace_back (l->name != nullptr ? l->name : "");
        return true;
    };
    ix.declare_soundpack = [] (const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) -> bool { return true; };
    ix.get_extension = [] (const clap_preset_discovery_indexer_t*, const char*) -> const void* { return nullptr; };

    check (pf->create (pf, &ix, "com.example.other") == nullptr, "create with a foreign provider id fails");
    const clap_preset_discovery_provider_t* prov = pf->create (pf, &ix, d->id);
    check (prov != nullptr, "create provider");
    if (prov == nullptr)
        return;
    check (prov->init (prov), "provider init");
    check (rec.locations.size() == 1 && rec.locations[0].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
               && rec.locations[0].location == nullptr && (rec.locations[0].flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0,
           "init declares one factory content location of kind plugin (location null)");

    clap_preset_discovery_metadata_receiver_t rx {};
    rx.receiver_data = &rec;
    rx.on_error = [] (const clap_preset_discovery_metadata_receiver_t* r, int32_t, const char*) { ++static_cast<Recorder*> (r->receiver_data)->errors; };
    rx.begin_preset = [] (const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key) -> bool {
        Recorder& rc = *static_cast<Recorder*> (r->receiver_data);
        rc.names.emplace_back (name != nullptr ? name : "");
        rc.keys.emplace_back (key != nullptr ? key : "");
        return true;
    };
    rx.add_plugin_id = [] (const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id) {
        if (streq (id->abi, "clap") && streq (id->id, "com.safian.kickeq"))
            ++static_cast<Recorder*> (r->receiver_data)->pluginIdsOk;
    };
    rx.set_flags = [] (const clap_preset_discovery_metadata_receiver_t* r, uint32_t f) {
        if ((f & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0)
            ++static_cast<Recorder*> (r->receiver_data)->flagsOk;
    };
    rx.add_creator = [] (const clap_preset_discovery_metadata_receiver_t* r, const char*) { ++static_cast<Recorder*> (r->receiver_data)->creators; };
    rx.set_soundpack_id = [] (const clap_preset_discovery_metadata_receiver_t*, const char*) {};
    rx.set_description = [] (const clap_preset_discovery_metadata_receiver_t*, const char*) {};
    rx.set_timestamps = [] (const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
    rx.add_feature = [] (const clap_preset_discovery_metadata_receiver_t*, const char*) {};
    rx.add_extra_info = [] (const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};

    check (!prov->get_metadata (prov, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nonexistent", &rx), "get_metadata of a file location is refused");
    check (prov->get_metadata (prov, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &rx), "get_metadata (plugin location)");
    check (rec.names.size() == static_cast<size_t> (numPresets), "lists 10 presets", fmt ("%zu", rec.names.size()));
    bool namesOk = rec.names.size() == static_cast<size_t> (numPresets);
    for (size_t i = 0; namesOk && i < rec.names.size(); ++i)
        namesOk = rec.names[i] == factoryPresets[i].name && !rec.keys[i].empty();
    check (namesOk, "preset names == core factory presets, load keys present");
    check (rec.pluginIdsOk == numPresets && rec.flagsOk == numPresets && rec.creators == numPresets, "every preset has plugin id, factory flag and creator");

    // loading
    Instance inst (c.host, c.factory);
    check (inst.presetLoad != nullptr, "preset-load extension present");
    if (inst.presetLoad == nullptr)
    {
        prov->destroy (prov);
        return;
    }
    c.host.presetLoaded = 0;
    c.host.presetErrors = 0;
    bool loadOk = true, valuesOk = true;
    for (size_t i = 0; i < rec.keys.size(); ++i)
    {
        inst.setParam (paramInfo (paramOutGain).id, 7.0); // a recall also resets the globals
        const bool r = inst.presetLoad->from_location (inst.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, rec.keys[i].c_str());
        loadOk = loadOk && r;
        ParamValues expect;
        applyPreset (static_cast<int> (i), expect);
        for (int k = 0; k < numParams; ++k)
            valuesOk = valuesOk && inst.value (paramInfo (k).id) == static_cast<double> (expect.get (k));
    }
    check (loadOk, "from_location succeeds for all 10 load keys");
    check (valuesOk, "loaded parameters == core applyPreset, for each preset");
    check (c.host.presetLoaded.load() == numPresets, "host preset-load.loaded called for each", fmt ("%d", c.host.presetLoaded.load()));
    check (c.host.paramsRescan.load() > 0, "host is told to rescan parameter values");
    inst.presetLoad->from_location (inst.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "Kick Punch");
    check (inst.value (idOf (1, BandParam::on)) == 1.0 && inst.value (idOf (1, BandParam::freq)) == 60.0, "Kick Punch changes parameters (band 2 on at 60 Hz)");
    inst.presetLoad->from_location (inst.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "Flat");
    check (inst.value (idOf (1, BandParam::on)) == 0.0, "Flat switches the bands off again");
    inst.presetLoad->from_location (inst.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "3");
    check (inst.value (idOf (4, BandParam::on)) == 1.0, "a decimal index is accepted as load key (3 = Vocal Presence)");
    check (!inst.presetLoad->from_location (inst.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "No Such Preset")
               && !inst.presetLoad->from_location (inst.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, nullptr)
               && !inst.presetLoad->from_location (inst.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/x.preset", "Flat"),
           "unknown key / null key / file location fail");
    check (c.host.presetErrors.load() == 3, "host preset-load.on_error called for the failures", fmt ("%d", c.host.presetErrors.load()));
    prov->destroy (prov);
}

// ---------------------------------------------------------------------------------------------------------
// Processing
// ---------------------------------------------------------------------------------------------------------

void testProcessFlat (Ctx& c)
{
    section ("process: flat EQ is transparent");
    for (const bool inPlace : { false, true })
    {
        Instance inst (c.host, c.factory);
        inst.activate (48000.0);
        const Stereo in = noiseStereo (48000 * 3, 21);
        const Stereo out = shellRun (inst, in, { 256, 100, 1024, 7, 33 }, inPlace);
        check (out == in, inPlace ? "default state: output bit-identical to input (in place)" : "default state: output bit-identical to input (out of place)");
    }

    Instance inst (c.host, c.factory);
    inst.activate (48000.0);
    std::vector<float> a = makeNoise (64, 1), b (64);
    uint64_t mask = ~uint64_t (0);
    const clap_process_status st = inst.process (a.data(), a.data(), b.data(), b.data(), 64, {}, &mask);
    if (c.options.juce) // the JUCE wrapper always answers CLAP_PROCESS_CONTINUE and leaves constant_mask alone
    {
        check (st == CLAP_PROCESS_CONTINUE, "returns CLAP_PROCESS_CONTINUE");
        check (inst.process (a.data(), a.data(), b.data(), b.data(), 0) != CLAP_PROCESS_ERROR, "frames_count == 0 is accepted");
    }
    else
    {
        check (st == CLAP_PROCESS_CONTINUE_IF_NOT_QUIET, "returns CLAP_PROCESS_CONTINUE_IF_NOT_QUIET");
        check (mask == 0, "output constant_mask is cleared even when the host flagged the input as constant");
        check (inst.process (a.data(), a.data(), b.data(), b.data(), 0) == CLAP_PROCESS_CONTINUE_IF_NOT_QUIET, "frames_count == 0 is accepted");
    }
    (void) mask;
    // the JUCE wrapper reports parameter changes made inside the plugin (state load) to the host as output events
    if (!c.options.juce)
        check (c.host.outEventsPushed.load() == 0, "no output events are emitted");
}

void testProcessEvents (Ctx& c)
{
    section ("process: sample-accurate parameter events");
    const double rate = 48000.0;
    for (const int startPreset : { 1, 5 })
    {
        const ParamValues start = presetValues (startPreset);
        Instance inst (c.host, c.factory);
        inst.activate (rate, 1, 4096);
        inst.loadState (blobFor (start));

        auto ref = makeRef (rate);
        ParamValues pv = start;

        // three blocks with events: duplicate times, time 0, the last sample, one past the end (clamped)
        const uint32_t blockLen[3] = { 3000, 1024, 777 };
        const std::vector<std::vector<ParamEvent>> events = {
            { { 100, idOf (1, BandParam::gain), -6.0 }, { 100, idOf (1, BandParam::freq), 200.0 },
              { 777, paramInfo (paramBypass).id, 1.0 }, { 1500, idOf (2, BandParam::on), 0.0 }, { 2999, paramInfo (paramOutGain).id, 3.0 } },
            { { 0, paramInfo (paramBypass).id, 0.0 }, { 0, idOf (4, BandParam::gain), 9.0 }, { 512, idOf (4, BandParam::type), 4.0 },
              { 1023, idOf (3, BandParam::q), 5.0 }, { 5000, idOf (3, BandParam::q), 0.5 } },
            { { 10, 0xBAD, 1.0 }, { 300, idOf (7, BandParam::on), 1.0 }, { 300, idOf (7, BandParam::freq), 50.0 }, { 776, paramInfo (paramAutoGain).id, 1.0 } }
        };

        bool same = true;
        std::string detail;
        uint32_t seed = 40;
        for (int blk = 0; blk < 3; ++blk)
        {
            Stereo in = noiseStereo (blockLen[blk], seed++);
            Stereo out (blockLen[blk]);
            Stereo want = in;
            inst.process (in.l.data(), in.r.data(), out.l.data(), out.r.data(), blockLen[blk], events[static_cast<size_t> (blk)]);
            if (c.options.juce)
                refBlockJuce (*ref, pv, want.l.data(), want.r.data(), blockLen[blk], events[static_cast<size_t> (blk)]);
            else
                refBlock (*ref, pv, want.l.data(), want.r.data(), blockLen[blk], events[static_cast<size_t> (blk)]);
            size_t at = 0;
            if (!bitEqual (out.l, want.l, &at) || !bitEqual (out.r, want.r, &at))
            {
                same = false;
                detail = fmt ("block %d first difference at %zu", blk, at);
            }
        }
        if (c.options.juce)
            check (same, startPreset == 1 ? "events: output == core engine split like the JUCE CLAP wrapper (64-sample sub-blocks; Kick Punch start)"
                                          : "events: output == core engine split like the JUCE CLAP wrapper (64-sample sub-blocks; Telephone start)",
                   detail);
        else
            check (same, startPreset == 1 ? "events split the block exactly like the core engine (Kick Punch start)"
                                          : "events split the block exactly like the core engine (Telephone start)",
                   detail);
        bool valuesOk = near (c, paramInfo (paramAutoGain).id, inst.value (paramInfo (paramAutoGain).id), 1.0)
                        && near (c, idOf (3, BandParam::q), inst.value (idOf (3, BandParam::q)), 0.5) // 5000 > frames: applied at the end
                        && near (c, idOf (7, BandParam::freq), inst.value (idOf (7, BandParam::freq)), 50.0);
        check (valuesOk, "get_value reflects events applied during process");
    }

    // an event at the first sample must change the result compared to the same block without it
    {
        Instance a (c.host, c.factory), b (c.host, c.factory);
        a.activate (rate);
        b.activate (rate);
        const Stereo in = noiseStereo (512, 77);
        Stereo oa (512), ob (512);
        a.process (in.l.data(), in.r.data(), oa.l.data(), oa.r.data(), 512, { { 0, idOf (2, BandParam::on), 1.0 }, { 0, idOf (2, BandParam::gain), 12.0 } });
        b.process (in.l.data(), in.r.data(), ob.l.data(), ob.r.data(), 512);
        check (!(oa == ob) && near (c, idOf (2, BandParam::on), a.value (idOf (2, BandParam::on)), 1.0), "an event at frame 0 takes effect in the same block");
    }

    // frames_count == 0 with events still applies them
    {
        Instance inst (c.host, c.factory);
        inst.activate (rate);
        float dummy[1] = { 0 };
        inst.process (dummy, dummy, dummy, dummy, 0, { { 0, paramInfo (paramOutGain).id, -9.0 } });
        // the JUCE wrapper applies events after its (empty) sample loop, so this holds there as well
        check (near (c, paramInfo (paramOutGain).id, inst.value (paramInfo (paramOutGain).id), -9.0), "events in a block with frames_count == 0 are applied");
    }
}

void testProcessBlocks (Ctx& c)
{
    section ("process: block sizes, in place, mono, offline");
    const double rate = 48000.0;
    const ParamValues pv = presetValues(8); // De-Mud: cuts + bells
    const std::vector<uint8_t> blob = blobFor (pv);
    const Stereo in = noiseStereo (150000, 5);

    Stereo want = in;
    {
        auto ref = makeRef (rate);
        ParamValues p = pv;
        ref->process (p, want.l.data(), want.r.data(), static_cast<int> (want.l.size()));
    }

    {
        Instance inst (c.host, c.factory);
        inst.activate (rate, 1, 4096);
        inst.loadState (blob);
        // random block sizes 1 .. 4096 plus zero length blocks
        Rng rng (2024);
        std::vector<uint32_t> blocks;
        for (int i = 0; i < 400; ++i)
            blocks.push_back (i % 17 == 0 ? 0u : 1u + rng.u32() % 4096u);
        Stereo got = in;
        Stereo outp (in.l.size());
        size_t pos = 0, bi = 0;
        bool fin = true;
        while (pos < in.l.size())
        {
            uint32_t n = blocks[bi++ % blocks.size()];
            if (n > in.l.size() - pos)
                n = static_cast<uint32_t> (in.l.size() - pos);
            inst.process (in.l.data() + pos, in.r.data() + pos, outp.l.data() + pos, outp.r.data() + pos, n);
            pos += n;
        }
        fin = allFinite (outp.l) && allFinite (outp.r);
        check (fin, "random block sizes 0 / 1 .. 4096: finite output");
        check (outp == want, "random block sizes == core engine processing the whole signal in one call (bit-identical)");
        (void) got;
    }

    {
        Instance a (c.host, c.factory), b (c.host, c.factory);
        a.activate (rate);
        b.activate (rate);
        a.loadState (blob);
        b.loadState (blob);
        const Stereo oop = shellRun (a, in, { 480 }, false);
        const Stereo ip = shellRun (b, in, { 480 }, true);
        check (oop == ip, "in place == out of place (bit-identical)");
        check (oop == want, "active EQ: shell output == core engine (bit-identical)");
        check (allFinite (ip.l) && allFinite (ip.r), "active EQ: finite output");
    }

    {
        // NaN / Inf in the input are sanitised by the engine
        Instance inst (c.host, c.factory);
        inst.activate (rate);
        inst.loadState (blob);
        Stereo bad = noiseStereo (4096, 3);
        bad.l[10] = std::nanf ("");
        bad.r[20] = INFINITY;
        bad.l[900] = -INFINITY;
        const Stereo out = shellRun (inst, bad, { 512 }, false);
        check (allFinite (out.l) && allFinite (out.r), "NaN / Inf input: finite output");
    }

    {
        // after reset() the filters are silent
        Instance inst (c.host, c.factory);
        inst.activate (rate);
        inst.loadState (blob);
        Stereo loud = noiseStereo (2048, 8), tmp (2048);
        inst.process (loud.l.data(), loud.r.data(), tmp.l.data(), tmp.r.data(), 2048);
        {
            AudioScope a;
            inst.plugin->reset (inst.plugin);
        }
        Stereo zeros (2048), out (2048);
        inst.process (zeros.l.data(), zeros.r.data(), out.l.data(), out.r.data(), 2048);
        bool silent = true;
        for (size_t i = 0; i < 2048; ++i)
            silent = silent && out.l[i] == 0.0f && out.r[i] == 0.0f;
        check (silent, "reset () clears the filter state (silence in, silence out)");
    }

    if (!c.options.juce) // JUCE shell: mono is a VST3 layout, exercised by kickeq_juce_shell_test
    {
        // mono
        Instance inst (c.host, c.factory);
        check (inst.portsConfig->select (inst.plugin, 1), "select the mono ports config");
        inst.activate (rate);
        inst.loadState (blob);
        std::vector<float> mono = makeNoise (20000, 12), out (20000), inplace = mono, want1 = mono;
        for (size_t pos = 0; pos < mono.size(); pos += 500)
            inst.process (mono.data() + pos, nullptr, out.data() + pos, nullptr, 500);
        {
            auto ref = makeRef (rate);
            ParamValues p = pv;
            ref->processMono (p, want1.data(), static_cast<int> (want1.size()));
        }
        check (bitEqual (out, want1), "mono buffers: output == core processMono (bit-identical)");

        Instance inst2 (c.host, c.factory);
        inst2.portsConfig->select (inst2.plugin, 1);
        inst2.activate (rate);
        inst2.loadState (blob);
        for (size_t pos = 0; pos < inplace.size(); pos += 500)
            inst2.process (inplace.data() + pos, nullptr, inplace.data() + pos, nullptr, 500);
        check (bitEqual (inplace, want1), "mono buffers in place == out of place");
    }

    {
        // offline render mode
        Instance rt (c.host, c.factory), off (c.host, c.factory);
        check (off.render != nullptr && !off.render->has_hard_realtime_requirement (off.plugin), "render: no hard realtime requirement");
        if (!c.options.juce) // the JUCE wrapper maps every mode != realtime to non-realtime
            check (!off.render->set (off.plugin, 5), "render.set rejects an unknown mode");
        check (off.render->set (off.plugin, CLAP_RENDER_OFFLINE), "render.set (offline)");
        rt.activate (rate);
        off.activate (rate);
        rt.loadState (blob);
        off.loadState (blob);
        const Stereo a = shellRun (rt, in, { 777 }, false);
        const Stereo b = shellRun (off, in, { 777 }, false);
        check (a == b && b == want, "offline render mode: same audio as realtime (bit-identical)");
        check (off.render->set (off.plugin, CLAP_RENDER_REALTIME), "render.set (realtime)");
    }
}

/** Silence through the shell for ~1.5 s of wall time, so that the design thread has delivered its kernels (the output
    is silent either way, so when exactly the audio side picks them up does not matter) before any signal comes. */
constexpr int preludeBlocks = 60, preludeBlock = 256;
void shellPrelude (Instance& inst)
{
    std::vector<float> zero (preludeBlock, 0.0f), out (preludeBlock);
    for (int i = 0; i < preludeBlocks; ++i)
    {
        inst.process (zero.data(), zero.data(), out.data(), out.data(), preludeBlock);
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
}

void testBaseline (Ctx& c)
{
    section ("cross-shell baseline: fixed state + 5 s noise vs core EqEngine (minimum phase and linear phase)");
    const std::vector<uint8_t> blob = baselineState();
    if (!c.options.dumpState.empty())
    {
        if (std::FILE* f = std::fopen (c.options.dumpState.c_str(), "wb"))
        {
            std::fwrite (blob.data(), 1, blob.size(), f);
            std::fclose (f);
        }
    }

    struct Run
    {
        double rate;
        int mode;
    };
    // minimum phase at both rates, then linear phase 2048 at 44.1 kHz and 8192 at 48 kHz (without the feature the
    // state's phase mode reads back as Minimum and the same runs repeat the minimum-phase comparison)
    const Run runs[4] = { { 44100.0, 0 }, { 48000.0, 0 }, { 44100.0, 1 }, { 48000.0, 3 } };

    std::vector<float> dump;
    for (const Run& run : runs)
    {
        if (run.mode != 0 && c.options.noThreads && linearPhaseOn())
        {
            std::printf ("SKIP  linear phase baseline at %.0f Hz (--no-threads: the design thread cannot be created)\n", run.rate);
            continue;
        }
        const double rate = run.rate;
        const bool linearRun = run.mode != 0 && linearPhaseOn();
        const std::vector<uint8_t> runBlob = baselineState (run.mode);
        const size_t n = static_cast<size_t> (rate * 5.0);
        const Stereo in = noiseStereo (n, 1234);
        Instance inst (c.host, c.factory);
        const bool loaded = inst.loadState (runBlob); // before activation: the activation runs (and reports) the loaded phase mode
        inst.activate (rate, 1, 512);
        if (rate == 48000.0 && run.mode == 0 && !c.options.dumpSaved.empty())
        {
            const std::vector<uint8_t> saved = inst.saveState();
            if (std::FILE* f = std::fopen (c.options.dumpSaved.c_str(), "wb"))
            {
                std::fwrite (saved.data(), 1, saved.size(), f);
                std::fclose (f);
            }
            else
            {
                check (false, "write --dump-saved file");
            }
        }
        if (linearRun)
            shellPrelude (inst);
        const Stereo out = shellRun (inst, in, { 512 }, false);

        ParamValues pv;
        loadState (runBlob.data(), runBlob.size(), pv);
        Stereo want = in;
        auto ref = makeRef (rate);
        if (linearRun)
        {
            // the kernels are designed in this thread before the same silent prelude
            ref->designSynchronously (pv);
            Stereo silence (preludeBlocks * preludeBlock);
            for (int b = 0; b < preludeBlocks; ++b)
                ref->process (pv, silence.l.data() + b * preludeBlock, silence.r.data() + b * preludeBlock, preludeBlock);
        }
        ref->process (pv, want.l.data(), want.r.data(), static_cast<int> (n));
        size_t at = 0;
        const bool eq = bitEqual (out.l, want.l, &at) && bitEqual (out.r, want.r, &at);
        const char* name = run.mode == 0 ? (rate == 44100.0 ? "44.1 kHz: shell output == core reference, bit-identical"
                                                            : "48 kHz: shell output == core reference, bit-identical")
                                         : (rate == 44100.0 ? "44.1 kHz, linear phase 2048: shell output == core reference, bit-identical"
                                                            : "48 kHz, linear phase 8192: shell output == core reference, bit-identical");
        check (loaded && eq && allFinite (out.l), name, fmt ("first difference at %zu", at));
        if (linearRun)
            check (inst.latency->get (inst.plugin) == static_cast<uint32_t> (phaseModeLatency (run.mode)), "reports the latency N / 2",
                   fmt ("%u", inst.latency->get (inst.plugin)));
        for (size_t i = 0; i < n; ++i)
        {
            dump.push_back (out.l[i]);
            dump.push_back (out.r[i]);
        }
    }

    if (!c.options.dumpOutput.empty())
    {
        if (std::FILE* f = std::fopen (c.options.dumpOutput.c_str(), "wb"))
        {
            std::fwrite (dump.data(), sizeof (float), dump.size(), f);
            std::fclose (f);
            std::printf ("      wrote %zu interleaved float32 frames (5 s each: 44.1 kHz, 48 kHz, 44.1 kHz linear 2048, 48 kHz linear 8192) to %s\n", dump.size() / 2, c.options.dumpOutput.c_str());
        }
        else
        {
            check (false, "write --dump-output file");
        }
    }
}

/** A blob saved by the other shell: loading it must reproduce every parameter value bit-exactly (the plain values
    are what the audio thread reads), whichever shell wrote it. */
void testForeignState (Ctx& c)
{
    section ("state blob saved by another shell: identical parameter values");
    std::vector<uint8_t> blob;
    if (std::FILE* f = std::fopen (c.options.stateFile.c_str(), "rb"))
    {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread (buf, 1, sizeof buf, f)) > 0)
            blob.insert (blob.end(), buf, buf + n);
        std::fclose (f);
    }
    check (!blob.empty(), "state file is readable", c.options.stateFile);
    ParamValues want;
    check (!blob.empty() && loadState (blob.data(), blob.size(), want), "the file is a valid core state blob");

    Instance inst (c.host, c.factory);
    check (inst.ok() && inst.loadState (blob), "the plugin accepts the blob");
    const std::vector<uint8_t> saved = inst.saveState();
    ParamValues got;
    UiChunk ui;
    check (loadState (saved.data(), saved.size(), got, &ui), "the plugin saves a valid core state blob again");
    bool exact = true;
    int nonDefault = 0;
    for (int i = 0; i < numParams; ++i)
    {
        const float a = want.get (i), b = got.get (i);
        exact = exact && std::memcmp (&a, &b, sizeof a) == 0;
        nonDefault += a != paramInfo (i).def ? 1 : 0;
    }
    check (exact, "all parameter values are bit-identical after load -> save", fmt ("%d of %d differ from the defaults", nonDefault, numParams));
    check (nonDefault > 10, "the blob is not trivial (many non-default parameters)");
    bool viaParams = true;
    for (int i = 0; i < numParams; ++i)
        viaParams = viaParams && near (c, paramInfo (i).id, inst.value (paramInfo (i).id), static_cast<double> (want.get (i)));
    check (viaParams, "clap.params get_value reports the blob values");
    if (c.options.juce)
        check (ui.data != nullptr && ui.size >= 1, "the JUCE shell stores its editor state in the UI01 chunk");
}

// ---------------------------------------------------------------------------------------------------------
// Linear phase
// ---------------------------------------------------------------------------------------------------------

namespace
{
double lockInGainDb (const std::vector<float>& x, size_t from, size_t len, double omega, double inputAmp)
{
    double sr = 0.0, si = 0.0, sw = 0.0;
    for (size_t i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * (static_cast<double> (i) + 0.5) / static_cast<double> (len));
        sr += w * x[from + i] * std::cos (omega * static_cast<double> (i + from));
        si += w * x[from + i] * std::sin (omega * static_cast<double> (i + from));
        sw += w;
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (sr * sr + si * si) / sw / inputAmp);
}

/** True when out[i] == in[i - delay] (0 before) for every i, bit for bit. */
bool isDelayedCopy (const Stereo& in, const Stereo& out, size_t delay)
{
    for (size_t i = 0; i < in.l.size(); ++i)
    {
        const float wl = i >= delay ? in.l[i - delay] : 0.0f, wr = i >= delay ? in.r[i - delay] : 0.0f;
        if (out.l[i] != wl || out.r[i] != wr)
            return false;
    }
    return true;
}
} // namespace

void testLinearPhase (Ctx& c)
{
    section ("linear phase: latency, restart request, design thread");
    const uint32_t phaseId = paramInfo (paramPhaseMode).id;

    if (!linearPhaseOn())
    {
        Instance inst (c.host, c.factory);
        inst.setParam (phaseId, 3.0);
        check (inst.value (phaseId) == 0.0, "built without linear phase: phase_mode cannot leave Minimum");
        inst.activate (48000.0, 1, 512);
        check (inst.latency->get (inst.plugin) == 0, "built without linear phase: latency stays 0");
        ParamValues v;
        v.set (paramPhaseMode, 2.0f);
        check (inst.loadState (blobFor (v)) && inst.value (phaseId) == 0.0, "built without linear phase: a state with a linear mode loads as Minimum");
        return;
    }

    // ---- before activation: the latency follows the parameter, the host is told, no restart is needed ----------
    Instance a (c.host, c.factory);
    check (a.latency->get (a.plugin) == 0, "latency 0 in minimum phase");
    c.host.latencyChanged = 0;
    c.host.restartRequests = 0;
    a.setParam (phaseId, 2.0);
    a.pump(); // on_main_thread, as a host does after request_callback
    check (a.latency->get (a.plugin) == 2048, "phase_mode Linear 4096 (before activation): latency 2048", fmt ("%u", a.latency->get (a.plugin)));
    if (!c.options.juce)
        // CLAP: latency.changed is only allowed inside activate; while deactivated there is nothing to restart
        check (c.host.latencyChanged.load() == 0 && c.host.restartRequests.load() == 0, "mode change while deactivated: no host call at all", fmt ("changed %d restart %d", c.host.latencyChanged.load(), c.host.restartRequests.load()));

    // ---- flat EQ in linear phase: output == input delayed by exactly N / 2, bit for bit ------------------------------
    a.activate (48000.0, 1, 512);
    // the latency of this activation differs from the previous one: latency.changed is called, inside activate (the
    // JUCE wrapper does the same when the processor's latency changed)
    check (c.host.latencyChanged.load() == 1, "latency.changed called once, inside activate", fmt ("%d", c.host.latencyChanged.load()));
    const Stereo in = noiseStereo (20000, 55);
    check (isDelayedCopy (in, shellRun (a, in, { 256, 100, 1024, 7, 33 }, false), 2048), "flat, Linear 4096: output == input delayed by 2048 samples (bit-identical)");
    check (a.latency->get (a.plugin) == 2048, "latency 2048 while active");

    // ---- mode change while active (event on the audio thread): flags only there, restart request from the main thread ----
    c.host.latencyChanged = 0;
    c.host.restartRequests = 0;
    c.host.callbackRequests = 0;
    {
        Stereo out (2048), blk = noiseStereo (2048, 56);
        a.process (blk.l.data(), blk.r.data(), out.l.data(), out.r.data(), 2048, { { 700, phaseId, 3.0 } });
        check (allFinite (out.l) && allFinite (out.r), "mode change while processing: output stays finite");
        if (!c.options.juce)
        {
            check (c.host.callbackRequests.load() == 1, "audio thread only asks for a main thread callback", fmt ("%d", c.host.callbackRequests.load()));
            check (c.host.restartRequests.load() == 0 && c.host.latencyChanged.load() == 0, "nothing is called on the host from the audio thread", fmt ("restart %d changed %d", c.host.restartRequests.load(), c.host.latencyChanged.load()));
        }
        a.pump();
        if (c.options.juce) // the wrapper requests the restart itself (latency change of the processor)
        {
            check (c.host.restartRequests.load() >= 1, "JUCE: request_restart after the latency changed", fmt ("restart %d", c.host.restartRequests.load()));
            check (a.latency->get (a.plugin) == 4096, "latency 4096 after the change");
        }
        else
        {
            check (c.host.restartRequests.load() == 1 && c.host.latencyChanged.load() == 0, "on_main_thread: request_restart (latency.changed only inside activate)", fmt ("changed %d restart %d", c.host.latencyChanged.load(), c.host.restartRequests.load()));
            check (a.latency->get (a.plugin) == 2048, "while activated clap.latency keeps reporting the running configuration (2048)", fmt ("%u", a.latency->get (a.plugin)));
            a.pump();
            check (c.host.restartRequests.load() == 1, "a second callback does not repeat the request");
        }
    }

    // ---- the host restarts the plugin: the new latency is in effect ----------------------------------------------------
    c.host.latencyChanged = 0;
    a.deactivate();
    a.activate (48000.0, 1, 512);
    check (c.host.latencyChanged.load() == 1, "restart: latency.changed inside activate", fmt ("%d", c.host.latencyChanged.load()));
    check (a.latency->get (a.plugin) == 4096, "restart: latency 4096");
    check (isDelayedCopy (in, shellRun (a, in, { 480 }, false), 4096), "after the restart: output == input delayed by 4096 samples (bit-identical)");

    // ---- state load: a different mode asks for a restart, the same mode does not -------------------------------------------
    c.host.latencyChanged = 0;
    c.host.restartRequests = 0;
    ParamValues minimum;
    check (a.loadState (blobFor (minimum)), "load a Minimum state while active");
    if (c.options.juce)
    {
        a.pump();
        check (c.host.restartRequests.load() >= 1, "JUCE: state load with another phase mode: request_restart", fmt ("restart %d", c.host.restartRequests.load()));
        check (a.latency->get (a.plugin) == 0, "latency 0 again");
    }
    else
    {
        check (c.host.latencyChanged.load() == 0 && c.host.restartRequests.load() == 1, "state load with another phase mode: request_restart (no latency.changed outside activate)", fmt ("changed %d restart %d", c.host.latencyChanged.load(), c.host.restartRequests.load()));
        check (a.latency->get (a.plugin) == 4096, "still reporting the running configuration until the restart");
    }
    {
        Stereo out = shellRun (a, in, { 300 }, false); // the engine cross-fades to the minimum-phase path meanwhile
        check (allFinite (out.l) && allFinite (out.r), "finite output while the engine follows the mode change");
    }
    a.deactivate();
    a.activate (48000.0, 1, 512);
    check (a.latency->get (a.plugin) == 0, "after the restart: latency 0");
    check (isDelayedCopy (in, shellRun (a, in, { 480 }, false), 0), "after the restart in Minimum: bit-transparent, no latency");
    c.host.latencyChanged = 0;
    c.host.restartRequests = 0;
    check (a.loadState (blobFor (minimum)), "load the same state again");
    a.pump();
    check (c.host.restartRequests.load() == 0 && c.host.latencyChanged.load() == 0, "same mode: no latency change, no restart");

    // ---- presets keep the phase mode -----------------------------------------------------------------------------------------
    if (a.presetLoad != nullptr) // (the JUCE shell has no preset-load extension: its programs are tested by kickeq_juce_shell_test)
    {
        a.setParam (phaseId, 1.0);
        a.pump();
        a.presetLoad->from_location (a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "Kick Punch");
        check (a.value (phaseId) == 1.0 && a.value (idOf (1, BandParam::on)) == 1.0, "a preset recall keeps the phase mode");
    }

    // ---- a real filter through the real design thread ---------------------------------------------------------------------------
    for (const int mode : { 1, 3 })
    {
        ParamValues v;
        v.set (paramPhaseMode, static_cast<float> (mode));
        v.set (3, BandParam::on, 1.0f);
        v.set (3, BandParam::type, 0.0f);
        v.set (3, BandParam::freq, 1000.0f);
        v.set (3, BandParam::gain, 9.0f);
        v.set (3, BandParam::q, 1.0f);
        Instance b (c.host, c.factory);
        b.loadState (blobFor (v));
        b.activate (48000.0, 1, 512);
        check (b.latency->get (b.plugin) == static_cast<uint32_t> (phaseModeLatency (mode)), "latency of the loaded state");
        shellPrelude (b);

        const size_t n = static_cast<size_t> (phaseModeLength (mode)) + 24000;
        Stereo tone (n), out (n);
        for (size_t i = 0; i < n; ++i)
            tone.l[i] = tone.r[i] = static_cast<float> (0.5 * std::sin (2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double> (i) / 48000.0));
        out = shellRun (b, tone, { 480 }, false);
        const double gain = lockInGainDb (out.l, static_cast<size_t> (phaseModeLength (mode)) + 2000, 20000, 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0, 0.5);
        float freq = 1000.0f, mag = 0.0f;
        computeResponse (v, 48000.0, &freq, &mag, nullptr, 1, ChannelView::stereo);
        check (std::fabs (gain - mag) < 0.05, mode == 1 ? "Linear 2048 through the design thread: +9 dB bell at 1 kHz within 0.05 dB"
                                                         : "Linear 8192 through the design thread: +9 dB bell at 1 kHz within 0.05 dB",
               fmt ("%.3f dB vs %.3f dB", gain, mag));
        check (bitEqual (out.l, out.r), "both channels identical");
    }
}

void testAnalyzer (Ctx& c)
{
    section ("vendor extension com.safian.kickeq/analyzer/1");
    Instance inst (c.host, c.factory);
    const auto* an = static_cast<const kickeq_clap_plugin_analyzer_t*> (inst.analyzer);
    check (an != nullptr, "extension present");
    if (an == nullptr)
        return;
    const clap_plugin_t* p = inst.plugin;
    const uint32_t bins = an->num_bins (p);
    check (bins == KICKEQ_CLAP_ANALYZER_NUM_BINS && bins == 512, "num_bins == 512");
    bool mono = true;
    for (uint32_t i = 1; i < bins; ++i)
        mono = mono && an->bin_frequency_hz (p, i) > an->bin_frequency_hz (p, i - 1);
    check (mono && std::fabs (an->bin_frequency_hz (p, 0) - 10.0f) < 0.5f && std::fabs (an->bin_frequency_hz (p, bins - 1) / 30000.0f - 1.0f) < 0.02f,
           "bin frequencies increase from 10 Hz to 30 kHz");
    check (an->bin_frequency_hz (p, bins) == 0.0f, "bin_frequency_hz out of range returns 0");

    std::vector<float> pre (bins), post (bins);
    check (!an->get_frame (p, pre.data(), post.data(), bins), "get_frame before the analyser was enabled: false");

    inst.activate (48000.0, 1, 480);
    ParamValues pv;
    pv.set (3, BandParam::on, 1.0f);
    pv.set (3, BandParam::type, 0.0f);
    pv.set (3, BandParam::freq, 1000.0f);
    pv.set (3, BandParam::gain, 6.0f);
    pv.set (3, BandParam::q, 1.0f);
    inst.loadState (blobFor (pv));
    an->set_enabled (p, true);

    std::vector<float> sine (480), out (480);
    double phase = 0.0;
    bool got = false;
    for (int blk = 0; blk < 125; ++blk) // 1.25 s
    {
        for (float& x : sine)
        {
            x = static_cast<float> (0.5 * std::sin (phase));
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
        }
        inst.process (sine.data(), sine.data(), out.data(), out.data(), 480);
        if (blk % 4 == 3)
            got = an->get_frame (p, pre.data(), post.data(), bins) || got;
    }
    got = an->get_frame (p, pre.data(), post.data(), bins);
    check (got, "get_frame returns a frame after ~1 s of audio");
    const auto peakOf = [bins] (const std::vector<float>& v) { return static_cast<uint32_t> (std::max_element (v.begin(), v.begin() + bins) - v.begin()); };
    const uint32_t pk = peakOf (pre), pk2 = peakOf (post);
    const float f1 = an->bin_frequency_hz (p, pk), f2 = an->bin_frequency_hz (p, pk2);
    check (std::fabs (f1 / 1000.0f - 1.0f) < 0.03f, "pre spectrum peaks at the 1 kHz bin", fmt ("%.1f Hz", f1));
    check (std::fabs (f2 / 1000.0f - 1.0f) < 0.03f, "post spectrum peaks at the 1 kHz bin", fmt ("%.1f Hz", f2));
    std::vector<float> sorted (pre.begin(), pre.end());
    std::sort (sorted.begin(), sorted.end());
    check (pre[pk] - sorted[bins / 2] > 30.0f && pre[pk] > -40.0f, "pre peak stands out of the noise floor", fmt ("peak %.1f dB median %.1f dB", pre[pk], sorted[bins / 2]));
    const float gain = post[pk2] - pre[pk];
    check (gain > 3.0f && gain < 9.0f, "post is about +6 dB above pre at 1 kHz (band 4 bell)", fmt ("%.2f dB", gain));
    check (!an->get_frame (p, pre.data(), post.data(), 100), "get_frame with the wrong bin count: false");
    check (an->get_frame (p, nullptr, post.data(), bins) && an->get_frame (p, pre.data(), nullptr, bins), "either output pointer may be null");
    an->set_enabled (p, false);
    an->set_enabled (p, false);

    // offline: the analyser stays idle
    Instance off (c.host, c.factory);
    const auto* an2 = static_cast<const kickeq_clap_plugin_analyzer_t*> (off.analyzer);
    off.render->set (off.plugin, CLAP_RENDER_OFFLINE);
    off.activate (48000.0, 1, 480);
    an2->set_enabled (off.plugin, true);
    phase = 0.0;
    for (int blk = 0; blk < 100; ++blk)
    {
        for (float& x : sine)
        {
            x = static_cast<float> (0.5 * std::sin (phase));
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
        }
        off.process (sine.data(), sine.data(), out.data(), out.data(), 480);
    }
    check (!an2->get_frame (off.plugin, pre.data(), post.data(), bins), "offline render: the analyser produces no frame");
}

void testConcurrency (Ctx& c)
{
    section ("concurrency: automation on the audio thread vs main thread access");
    Instance inst (c.host, c.factory);
    inst.activate (48000.0, 1, 1024);
    inst.loadState (blobFor (presetValues (1)));
    const auto* an = static_cast<const kickeq_clap_plugin_analyzer_t*> (inst.analyzer);

    std::atomic<bool> done { false };
    std::atomic<bool> audioFinite { true };
    std::thread audio ([&] {
        Rng rng (31337);
        Noise noise (5);
        std::vector<float> in (1024), out (1024);
        for (int it = 0; it < 700; ++it)
        {
            const uint32_t frames = 1 + rng.u32() % 1023;
            for (uint32_t i = 0; i < frames; ++i)
                in[i] = noise.next();
            std::vector<ParamEvent> ev;
            for (uint32_t k = 0; k < rng.u32() % 6; ++k)
            {
                const ParamInfo& t = paramInfo (static_cast<int> (rng.u32() % numParams));
                ev.push_back ({ rng.u32() % frames, t.id, t.min + rng.uni() * (t.max - t.min) });
            }
            std::sort (ev.begin(), ev.end(), [] (const ParamEvent& a, const ParamEvent& b) { return a.time < b.time; });
            inst.process (in.data(), in.data(), out.data(), out.data(), frames, ev);
            for (uint32_t i = 0; i < frames; ++i)
                if (!(out[i] - out[i] == 0.0f))
                    audioFinite = false;
        }
        done = true;
    });

    bool rangeOk = true, stateOk = true;
    std::vector<float> pre (512), post (512);
    int rounds = 0;
    while (!done.load())
    {
        for (int i = 0; i < numParams; ++i)
        {
            const double v = inst.value (paramInfo (i).id);
            rangeOk = rangeOk && v >= static_cast<double> (paramInfo (i).min) && v <= static_cast<double> (paramInfo (i).max);
        }
        const std::vector<uint8_t> blob = inst.saveState();
        stateOk = stateOk && !blob.empty();
        if (rounds % 3 == 0)
            stateOk = stateOk && inst.loadState (blob);
        if (an != nullptr)
        {
            if (rounds % 5 == 0)
                an->set_enabled (inst.plugin, (rounds / 5) % 2 == 0);
            an->get_frame (inst.plugin, pre.data(), post.data(), 512);
        }
        (void) inst.tail->get (inst.plugin);
        char text[64];
        inst.params->value_to_text (inst.plugin, inst.hostId (paramInfo (paramOutGain).id), 1.5, text, sizeof text);
        ++rounds;
        std::this_thread::yield();
    }
    audio.join();
    check (rangeOk, "get_value stays inside the parameter ranges while automation runs", fmt ("%d rounds", rounds));
    check (stateOk, "state save / load on the main thread while the audio thread runs");
    check (audioFinite.load(), "audio stays finite under concurrent automation and state loads");
}

void testHostChecks (Ctx& c)
{
    section ("host-side checks");
    check (c.host.violations.load() == 0, "no thread-check violations seen by the host",
           fmt ("%d violations (main checks %d, audio checks %d)", c.host.violations.load(), c.host.mainChecks.load(), c.host.audioChecks.load()));
    check (c.host.logErrors.load() == 0, "plugin logged no errors");
    // JUCE wrapper: parameter changes made inside the plugin (state load) are reported to the host as output events
    if (!c.options.juce)
        check (c.host.outEventsPushed.load() == 0, "plugin pushed no output events at all");
    std::printf ("      info: thread-check calls from the plugin: main %d, audio %d (0 when the plugin is built without KICKEQ_THREAD_CHECKS)\n",
                 c.host.mainChecks.load(), c.host.audioChecks.load());
}
} // namespace

int runConformance (Loader& loader, const Options& options)
{
    TestHost host;
    Ctx c { loader, host, loader.plugins(), options };

    if (options.baselineOnly)
    {
        if (c.factory == nullptr)
            return 1;
        testBaseline (c);
        if (!options.stateFile.empty())
            testForeignState (c);
        const Report& r = report();
        std::printf ("\n%d checks passed, %d failed\n", r.passed, r.failed);
        return r.failed == 0 && r.passed > 0 ? 0 : 1;
    }

    testDescriptor (c);
    if (c.factory != nullptr)
    {
        testLifecycle (c);
        testParams (c);
        testState (c);
        testLatencyTail (c);
        if (options.noThreads && linearPhaseOn())
        {
            section ("linear phase: latency, restart request, design thread");
            std::printf ("SKIP  linear phase (--no-threads: the design thread cannot be created)\n");
        }
        else
        {
            testLinearPhase (c);
        }
        testPorts (c);
        if (!options.juce) // JUCE shell: no remote controls, preset discovery or analyser vendor extension
        {
            testRemoteControls (c);
            testPresets (c);
        }
        testProcessFlat (c);
        testProcessEvents (c);
        testProcessBlocks (c);
        testBaseline (c);
        if (!options.juce)
            testAnalyzer (c);
        if (options.noThreads)
        {
            section ("concurrency: automation on the audio thread vs main thread access");
            std::printf ("SKIP  multi-threaded section (--no-threads)\n");
        }
        else
        {
            testConcurrency (c);
        }
    }
    testHostChecks (c);

    const Report& r = report();
    std::printf ("\n%d checks passed, %d failed\n", r.passed, r.failed);
    return r.failed == 0 && r.passed > 0 ? 0 : 1;
}
} // namespace th
