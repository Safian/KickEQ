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

// clap_testhost: a tiny CLAP host plus the helpers of the conformance suite.
// Platform code (dlopen / LoadLibrary) lives here in tools/, never in the plugin.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <clap/clap.h>

#include "kickeq/params.h"

namespace th
{
// ---------------------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------------------

struct Report
{
    int passed = 0, failed = 0;
    std::string section;
};
Report& report();
void section (const char* name);
/** Prints "PASS" / "FAIL" for one check; detail is only printed for failures. */
void check (bool ok, const char* name, const std::string& detail = {});
std::string fmt (const char* f, ...);

// ---------------------------------------------------------------------------------------------------------
// Loader: static (kickeq_get_plugin_factory) or dynamic (.clap through dlopen / LoadLibrary)
// ---------------------------------------------------------------------------------------------------------

class Loader
{
public:
    Loader() = default;
    ~Loader();
    Loader (const Loader&) = delete;
    Loader& operator= (const Loader&) = delete;

    bool openStatic();
    bool openDynamic (const std::string& path, std::string& error);

    const void* getFactory (const char* id) const;
    const clap_plugin_factory_t* plugins() const { return static_cast<const clap_plugin_factory_t*> (getFactory (CLAP_PLUGIN_FACTORY_ID)); }
    const clap_preset_discovery_factory_t* presets() const
    {
        return static_cast<const clap_preset_discovery_factory_t*> (getFactory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    }
    bool isStatic() const { return handle == nullptr && entry == nullptr; }

private:
    void* handle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
};

// ---------------------------------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------------------------------

class TestHost
{
public:
    explicit TestHost (bool offerExtensions = true);
    const clap_host_t* get() const { return &host; }

    std::atomic<int> callbackRequests { 0 }, tailChanged { 0 }, latencyChanged { 0 }, paramsRescan { 0 },
        presetLoaded { 0 }, presetErrors { 0 }, logTotal { 0 }, logErrors { 0 }, mainChecks { 0 }, audioChecks { 0 },
        violations { 0 }, outEventsPushed { 0 };
    std::thread::id mainId;
    bool offer;

private:
    clap_host_t host {};
};

/** Marks the calling thread as "the audio thread" for the host's thread-check extension. */
struct AudioScope
{
    AudioScope();
    ~AudioScope();
};

// ---------------------------------------------------------------------------------------------------------
// Plugin instance wrapper
// ---------------------------------------------------------------------------------------------------------

struct ParamEvent
{
    uint32_t time;
    uint32_t id;
    double value;
};

struct Instance
{
    Instance (TestHost& host, const clap_plugin_factory_t* factory);
    ~Instance();
    Instance (const Instance&) = delete;
    Instance& operator= (const Instance&) = delete;

    bool ok() const { return plugin != nullptr; }
    bool activate (double sampleRate, uint32_t minFrames = 1, uint32_t maxFrames = 4096);
    void deactivate();

    // main thread helpers
    double value (uint32_t id) const;
    std::array<double, kickeq::numParams> allValues() const;
    /** Host-side id of a core parameter id (identity unless the JUCE profile is on: ids are hashes there). */
    uint32_t hostId (uint32_t coreId) const;
    void setParams (const std::vector<ParamEvent>& events); // clap.params flush
    void setParam (uint32_t id, double v) { setParams ({ { 0, id, v } }); }
    std::vector<uint8_t> saveState (size_t maxChunk = 0, bool* okOut = nullptr) const;
    bool loadState (const std::vector<uint8_t>& blob, size_t maxChunk = 0, int64_t failAfter = -1) const;
    void pump(); // calls on_main_thread like a host main loop would after request_callback

    /** process(): channels = 1 or 2; in/out pointers may be identical (in place). */
    clap_process_status process (const float* inL, const float* inR, float* outL, float* outR, uint32_t frames,
                                 const std::vector<ParamEvent>& events = {}, uint64_t* constantMask = nullptr);

    TestHost& host;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_latency_t* latency = nullptr;
    const clap_plugin_tail_t* tail = nullptr;
    const clap_plugin_audio_ports_t* ports = nullptr;
    const clap_plugin_audio_ports_config_t* portsConfig = nullptr;
    const clap_plugin_render_t* render = nullptr;
    const clap_plugin_remote_controls_t* remote = nullptr;
    const clap_plugin_preset_load_t* presetLoad = nullptr;
    const void* analyzer = nullptr; // kickeq_clap_plugin_analyzer_t
    bool activated = false;
    bool processing = false;
    uint32_t channels = 2;
    int64_t steady = 0;
    std::vector<std::pair<uint32_t, uint32_t>> idMap; // core id -> host id (JUCE profile)
};

// ---------------------------------------------------------------------------------------------------------
// Signals and reference helpers
// ---------------------------------------------------------------------------------------------------------

struct Noise
{
    uint32_t s;
    explicit Noise (uint32_t seed = 1) : s (seed != 0 ? seed : 1) {}
    /** xorshift32; exact binary fractions in [-0.5, 0.5), identical on every platform. */
    float next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float> (static_cast<int32_t> (s)) * (1.0f / 4294967296.0f);
    }
};

std::vector<float> makeNoise (size_t n, uint32_t seed);
bool bitEqual (const std::vector<float>& a, const std::vector<float>& b, size_t* firstDiff = nullptr);
bool allFinite (const std::vector<float>& a);

/** The fixed state used for the cross-shell baseline (every band on, all types / placements, auto gain). */
std::vector<uint8_t> baselineState();

// conformance.cpp
struct Options
{
    std::string dumpOutput, dumpState;
    std::string dumpSaved; // --dump-saved: the blob the plugin itself saves after loading the baseline state
    std::string stateFile; // --state-file: a blob saved by another shell, loaded and checked (with --baseline-only)
    bool noThreads = false;    // skip the multi-threaded section (for emulators without working threads)
    bool juce = false;         // profile for the JUCE shell's CLAP (clap-juce-extensions wrapper), see conformance.cpp
    bool baselineOnly = false; // only the cross-shell baseline (fixed state + 5 s noise), for --dump-output
};

/** Compares two --dump-output files (interleaved float32): bit-identical, or the largest absolute difference. */
int compareDumps (const std::string& a, const std::string& b);

/** Selects the JUCE profile before any Instance is created (host-side parameter ids are mapped by name). */
void setJuceProfile (bool on);
int runConformance (Loader& loader, const Options& options);
} // namespace th
