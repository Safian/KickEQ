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

#include "testhost.h"

#include <cstdarg>
#include <cstring>

#include "kickeq/clap_native.h"
#include "kickeq/state.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace th
{
// ---------------------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------------------

Report& report()
{
    static Report r;
    return r;
}

void section (const char* name)
{
    report().section = name;
    std::printf ("\n== %s\n", name);
}

std::string fmt (const char* f, ...)
{
    char buf[512];
    va_list ap;
    va_start (ap, f);
    std::vsnprintf (buf, sizeof buf, f, ap);
    va_end (ap);
    return buf;
}

void check (bool ok, const char* name, const std::string& detail)
{
    if (ok)
    {
        ++report().passed;
        std::printf ("PASS  %s\n", name);
    }
    else
    {
        ++report().failed;
        std::printf ("FAIL  %s%s%s\n", name, detail.empty() ? "" : "  --  ", detail.c_str());
    }
    std::fflush (stdout);
}

// ---------------------------------------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------------------------------------

bool Loader::openStatic()
{
    return kickeq_get_plugin_factory() != nullptr && kickeq_get_preset_discovery_factory() != nullptr;
}

#if defined(_WIN32)
bool Loader::openDynamic (const std::string& path, std::string& error)
{
    HMODULE h = LoadLibraryA (path.c_str());
    if (h == nullptr)
    {
        error = fmt ("LoadLibrary failed (%lu)", static_cast<unsigned long> (GetLastError()));
        return false;
    }
    handle = h;
    entry = reinterpret_cast<const clap_plugin_entry_t*> (GetProcAddress (h, "clap_entry"));
    if (entry == nullptr)
    {
        error = "no clap_entry symbol";
        return false;
    }
    if (!clap_version_is_compatible (entry->clap_version) || !entry->init (path.c_str()))
    {
        error = "incompatible clap_entry or init failed";
        entry = nullptr;
        return false;
    }
    return true;
}

Loader::~Loader()
{
    if (entry != nullptr)
        entry->deinit();
    if (handle != nullptr)
        FreeLibrary (static_cast<HMODULE> (handle));
}
#else
namespace
{
/** macOS: a .clap is a bundle directory; the module is Contents/MacOS/<executable>. */
std::string resolveModulePath (const std::string& path)
{
    struct stat st {};
    if (::stat (path.c_str(), &st) != 0 || !S_ISDIR (st.st_mode))
        return path;

    const std::string dir = path + "/Contents/MacOS";
    std::string found;
    if (DIR* d = ::opendir (dir.c_str()))
    {
        while (const dirent* e = ::readdir (d))
            if (e->d_name[0] != '.')
            {
                found = dir + "/" + e->d_name;
                break;
            }
        ::closedir (d);
    }
    return found.empty() ? path : found;
}
} // namespace

bool Loader::openDynamic (const std::string& path, std::string& error)
{
    const std::string module = resolveModulePath (path);
    handle = ::dlopen (module.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        const char* e = ::dlerror();
        error = e != nullptr ? e : "dlopen failed";
        return false;
    }
    entry = static_cast<const clap_plugin_entry_t*> (::dlsym (handle, "clap_entry"));
    if (entry == nullptr)
    {
        error = "no clap_entry symbol";
        return false;
    }
    if (!clap_version_is_compatible (entry->clap_version) || !entry->init (path.c_str()))
    {
        error = "incompatible clap_entry or init failed";
        entry = nullptr;
        return false;
    }
    return true;
}

Loader::~Loader()
{
    if (entry != nullptr)
        entry->deinit();
    if (handle != nullptr)
        ::dlclose (handle);
}
#endif

const void* Loader::getFactory (const char* id) const
{
    if (entry != nullptr)
        return entry->get_factory (id);
    if (handle != nullptr)
        return nullptr;
    if (std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return kickeq_get_plugin_factory();
    if (std::strcmp (id, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0)
        return kickeq_get_preset_discovery_factory();
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------------------------------

namespace
{
thread_local bool tlAudioThread = false;

TestHost& hostOf (const clap_host_t* h) { return *static_cast<TestHost*> (h->host_data); }

bool CLAP_ABI isMainThread (const clap_host_t* h)
{
    TestHost& t = hostOf (h);
    ++t.mainChecks;
    const bool ok = std::this_thread::get_id() == t.mainId;
    if (!ok)
        ++t.violations;
    return ok;
}

bool CLAP_ABI isAudioThread (const clap_host_t* h)
{
    TestHost& t = hostOf (h);
    ++t.audioChecks;
    if (!tlAudioThread)
        ++t.violations;
    return tlAudioThread;
}

const clap_host_thread_check_t threadCheckExt = { &isMainThread, &isAudioThread };

void CLAP_ABI hostLog (const clap_host_t* h, clap_log_severity severity, const char* msg)
{
    TestHost& t = hostOf (h);
    ++t.logTotal;
    if (severity >= CLAP_LOG_ERROR)
        ++t.logErrors;
    std::printf ("      [plugin log %d] %s\n", static_cast<int> (severity), msg != nullptr ? msg : "");
}
const clap_host_log_t logExt = { &hostLog };

void CLAP_ABI hostTailChanged (const clap_host_t* h)
{
    TestHost& t = hostOf (h);
    ++t.tailChanged;
    if (!tlAudioThread) // clap_host_tail::changed is an [audio-thread] call
        ++t.violations;
}
const clap_host_tail_t tailExt = { &hostTailChanged };

void CLAP_ABI hostLatencyChanged (const clap_host_t* h) { ++hostOf (h).latencyChanged; }
const clap_host_latency_t latencyExt = { &hostLatencyChanged };

void CLAP_ABI hostParamsRescan (const clap_host_t* h, clap_param_rescan_flags)
{
    TestHost& t = hostOf (h);
    ++t.paramsRescan;
    if (std::this_thread::get_id() != t.mainId) // clap_host_params::rescan is a [main-thread] call
        ++t.violations;
}
void CLAP_ABI hostParamsClear (const clap_host_t*, clap_id, clap_param_clear_flags) {}
void CLAP_ABI hostParamsRequestFlush (const clap_host_t*) {}
const clap_host_params_t paramsExt = { &hostParamsRescan, &hostParamsClear, &hostParamsRequestFlush };

void CLAP_ABI hostStateMarkDirty (const clap_host_t*) {}
const clap_host_state_t stateExt = { &hostStateMarkDirty };

void CLAP_ABI hostPresetError (const clap_host_t* h, uint32_t, const char*, const char*, int32_t, const char*)
{
    ++hostOf (h).presetErrors;
}
void CLAP_ABI hostPresetLoaded (const clap_host_t* h, uint32_t, const char*, const char*) { ++hostOf (h).presetLoaded; }
const clap_host_preset_load_t presetLoadExt = { &hostPresetError, &hostPresetLoaded };

void CLAP_ABI hostRcChanged (const clap_host_t*) {}
void CLAP_ABI hostRcSuggest (const clap_host_t*, clap_id) {}
const clap_host_remote_controls_t remoteExt = { &hostRcChanged, &hostRcSuggest };

const void* CLAP_ABI hostGetExtension (const clap_host_t* h, const char* id)
{
    if (!hostOf (h).offer || id == nullptr)
        return nullptr;
    if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0) return &threadCheckExt;
    if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &logExt;
    if (std::strcmp (id, CLAP_EXT_TAIL) == 0) return &tailExt;
    if (std::strcmp (id, CLAP_EXT_LATENCY) == 0) return &latencyExt;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &paramsExt;
    if (std::strcmp (id, CLAP_EXT_STATE) == 0) return &stateExt;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0) return &presetLoadExt;
    if (std::strcmp (id, CLAP_EXT_REMOTE_CONTROLS) == 0) return &remoteExt;
    return nullptr;
}

void CLAP_ABI hostRequestRestart (const clap_host_t*) {}
void CLAP_ABI hostRequestProcess (const clap_host_t*) {}
void CLAP_ABI hostRequestCallback (const clap_host_t* h) { ++hostOf (h).callbackRequests; }
} // namespace

TestHost::TestHost (bool offerExtensions) : mainId (std::this_thread::get_id()), offer (offerExtensions)
{
    host.clap_version = CLAP_VERSION;
    host.host_data = this;
    host.name = "kickeq-testhost";
    host.vendor = "Safian";
    host.url = "https://github.com/Safian/KickEQ";
    host.version = "1";
    host.get_extension = &hostGetExtension;
    host.request_restart = &hostRequestRestart;
    host.request_process = &hostRequestProcess;
    host.request_callback = &hostRequestCallback;
}

AudioScope::AudioScope() { tlAudioThread = true; }
AudioScope::~AudioScope() { tlAudioThread = false; }

// ---------------------------------------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------------------------------------

namespace
{
struct EventList
{
    std::vector<clap_event_param_value_t> events;

    explicit EventList (const std::vector<ParamEvent>& src)
    {
        events.reserve (src.size());
        for (const ParamEvent& e : src)
        {
            clap_event_param_value_t ev {};
            ev.header.size = sizeof ev;
            ev.header.time = e.time;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.header.flags = 0;
            ev.param_id = e.id;
            ev.cookie = nullptr;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = e.value;
            events.push_back (ev);
        }
    }

    static uint32_t CLAP_ABI size (const clap_input_events_t* l)
    {
        return static_cast<uint32_t> (static_cast<const EventList*> (l->ctx)->events.size());
    }
    static const clap_event_header_t* CLAP_ABI get (const clap_input_events_t* l, uint32_t i)
    {
        const EventList& e = *static_cast<const EventList*> (l->ctx);
        return i < e.events.size() ? &e.events[i].header : nullptr;
    }
};

std::atomic<TestHost*> currentHostForOutEvents { nullptr };

bool CLAP_ABI tryPush (const clap_output_events_t*, const clap_event_header_t*)
{
    if (TestHost* h = currentHostForOutEvents.load())
        ++h->outEventsPushed;
    return true;
}

struct MemIn
{
    const std::vector<uint8_t>* data;
    size_t pos = 0;
    size_t maxChunk;
    int64_t failAfter; // bytes after which read() returns -1; < 0: never
};

int64_t CLAP_ABI memRead (const clap_istream_t* s, void* buf, uint64_t size)
{
    MemIn& m = *static_cast<MemIn*> (s->ctx);
    if (m.failAfter >= 0 && static_cast<int64_t> (m.pos) >= m.failAfter)
        return -1;
    size_t n = m.data->size() - m.pos;
    if (n > size)
        n = static_cast<size_t> (size);
    if (m.maxChunk != 0 && n > m.maxChunk)
        n = m.maxChunk;
    if (m.failAfter >= 0 && static_cast<int64_t> (m.pos + n) > m.failAfter)
        n = static_cast<size_t> (m.failAfter) - m.pos;
    if (n > 0)
        std::memcpy (buf, m.data->data() + m.pos, n);
    m.pos += n;
    return static_cast<int64_t> (n);
}

struct MemOut
{
    std::vector<uint8_t> data;
    size_t maxChunk;
};

int64_t CLAP_ABI memWrite (const clap_ostream_t* s, const void* buf, uint64_t size)
{
    MemOut& m = *static_cast<MemOut*> (s->ctx);
    if (m.maxChunk == static_cast<size_t> (-1))
        return -1; // failing stream
    size_t n = static_cast<size_t> (size);
    if (m.maxChunk != 0 && n > m.maxChunk)
        n = m.maxChunk;
    const uint8_t* p = static_cast<const uint8_t*> (buf);
    m.data.insert (m.data.end(), p, p + n);
    return static_cast<int64_t> (n);
}
} // namespace

Instance::Instance (TestHost& h, const clap_plugin_factory_t* factory) : host (h)
{
    plugin = factory->create_plugin (factory, h.get(), "com.safian.kickeq");
    if (plugin == nullptr)
        return;
    if (!plugin->init (plugin))
    {
        plugin->destroy (plugin);
        plugin = nullptr;
        return;
    }
    const auto ext = [this] (const char* id) { return plugin->get_extension (plugin, id); };
    params = static_cast<const clap_plugin_params_t*> (ext (CLAP_EXT_PARAMS));
    state = static_cast<const clap_plugin_state_t*> (ext (CLAP_EXT_STATE));
    latency = static_cast<const clap_plugin_latency_t*> (ext (CLAP_EXT_LATENCY));
    tail = static_cast<const clap_plugin_tail_t*> (ext (CLAP_EXT_TAIL));
    ports = static_cast<const clap_plugin_audio_ports_t*> (ext (CLAP_EXT_AUDIO_PORTS));
    portsConfig = static_cast<const clap_plugin_audio_ports_config_t*> (ext (CLAP_EXT_AUDIO_PORTS_CONFIG));
    render = static_cast<const clap_plugin_render_t*> (ext (CLAP_EXT_RENDER));
    remote = static_cast<const clap_plugin_remote_controls_t*> (ext (CLAP_EXT_REMOTE_CONTROLS));
    presetLoad = static_cast<const clap_plugin_preset_load_t*> (ext (CLAP_EXT_PRESET_LOAD));
    analyzer = ext ("com.safian.kickeq/analyzer/1");
}

Instance::~Instance()
{
    if (plugin == nullptr)
        return;
    if (processing)
    {
        AudioScope a;
        plugin->stop_processing (plugin);
    }
    if (activated)
        plugin->deactivate (plugin);
    plugin->destroy (plugin);
}

bool Instance::activate (double sampleRate, uint32_t minFrames, uint32_t maxFrames)
{
    if (!plugin->activate (plugin, sampleRate, minFrames, maxFrames))
        return false;
    activated = true;
    AudioScope a;
    if (!plugin->start_processing (plugin))
        return false;
    processing = true;
    return true;
}

void Instance::deactivate()
{
    if (processing)
    {
        AudioScope a;
        plugin->stop_processing (plugin);
        processing = false;
    }
    if (activated)
    {
        plugin->deactivate (plugin);
        activated = false;
    }
}

double Instance::value (uint32_t id) const
{
    double v = -12345.0;
    if (params == nullptr || !params->get_value (plugin, id, &v))
        return -12345.0;
    return v;
}

std::array<double, kickeq::numParams> Instance::allValues() const
{
    std::array<double, kickeq::numParams> a {};
    for (int i = 0; i < kickeq::numParams; ++i)
        a[static_cast<size_t> (i)] = value (kickeq::paramInfo (i).id);
    return a;
}

void Instance::setParams (const std::vector<ParamEvent>& events)
{
    EventList list (events);
    clap_input_events_t in = { &list, &EventList::size, &EventList::get };
    clap_output_events_t out = { nullptr, &tryPush };
    currentHostForOutEvents = &host;
    if (processing)
    {
        AudioScope a; // flush while active: audio thread
        params->flush (plugin, &in, &out);
    }
    else
    {
        params->flush (plugin, &in, &out);
    }
}

std::vector<uint8_t> Instance::saveState (size_t maxChunk, bool* okOut) const
{
    MemOut m { {}, maxChunk };
    clap_ostream_t s = { &m, &memWrite };
    const bool ok = state->save (plugin, &s);
    if (okOut != nullptr)
        *okOut = ok;
    return m.data;
}

bool Instance::loadState (const std::vector<uint8_t>& blob, size_t maxChunk, int64_t failAfter) const
{
    MemIn m { &blob, 0, maxChunk, failAfter };
    clap_istream_t s = { &m, &memRead };
    return state->load (plugin, &s);
}

void Instance::pump() { plugin->on_main_thread (plugin); }

clap_process_status Instance::process (const float* inL, const float* inR, float* outL, float* outR, uint32_t frames,
                                       const std::vector<ParamEvent>& events, uint64_t* constantMask)
{
    EventList list (events);
    clap_input_events_t in = { &list, &EventList::size, &EventList::get };
    clap_output_events_t out = { nullptr, &tryPush };
    currentHostForOutEvents = &host;

    const uint32_t ch = outR != nullptr ? 2 : 1;
    float* inData[2] = { const_cast<float*> (inL), const_cast<float*> (inR) };
    float* outData[2] = { outL, outR };
    clap_audio_buffer_t ib {};
    ib.data32 = inData;
    ib.channel_count = ch;
    ib.constant_mask = ~uint64_t (0); // a hostile "everything is constant" flag must be harmless
    clap_audio_buffer_t ob {};
    ob.data32 = outData;
    ob.channel_count = ch;
    ob.constant_mask = ~uint64_t (0);

    clap_process_t p {};
    p.steady_time = steady;
    p.frames_count = frames;
    p.transport = nullptr;
    p.audio_inputs = &ib;
    p.audio_outputs = &ob;
    p.audio_inputs_count = 1;
    p.audio_outputs_count = 1;
    p.in_events = &in;
    p.out_events = &out;
    steady += frames;

    AudioScope a;
    const clap_process_status st = plugin->process (plugin, &p);
    if (constantMask != nullptr)
        *constantMask = ob.constant_mask;
    return st;
}

// ---------------------------------------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------------------------------------

std::vector<float> makeNoise (size_t n, uint32_t seed)
{
    Noise g (seed);
    std::vector<float> v (n);
    for (float& x : v)
        x = g.next();
    return v;
}

bool bitEqual (const std::vector<float>& a, const std::vector<float>& b, size_t* firstDiff)
{
    if (a.size() != b.size())
    {
        if (firstDiff != nullptr)
            *firstDiff = a.size() < b.size() ? a.size() : b.size();
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        uint32_t x = 0, y = 0;
        std::memcpy (&x, &a[i], 4);
        std::memcpy (&y, &b[i], 4);
        if (x != y)
        {
            if (firstDiff != nullptr)
                *firstDiff = i;
            return false;
        }
    }
    return true;
}

bool allFinite (const std::vector<float>& a)
{
    for (float x : a)
        if (!(x - x == 0.0f))
            return false;
    return true;
}

std::vector<uint8_t> baselineState()
{
    using namespace kickeq;
    ParamValues v;
    const auto band = [&v] (int b, BandType t, float f, float g, float q, Placement p, int slope) {
        v.set (b, BandParam::on, 1.0f);
        v.set (b, BandParam::type, static_cast<float> (t));
        v.set (b, BandParam::freq, f);
        v.set (b, BandParam::gain, g);
        v.set (b, BandParam::q, q);
        v.set (b, BandParam::place, static_cast<float> (p));
        v.set (b, BandParam::slope, static_cast<float> (slope));
    };
    band (0, BandType::lowCut, 40.0f, 0.0f, 0.707f, Placement::stereo, 3);
    band (1, BandType::lowShelf, 120.0f, 4.0f, 0.8f, Placement::stereo, 1);
    band (2, BandType::bell, 300.0f, -3.0f, 1.2f, Placement::left, 1);
    band (3, BandType::bell, 1000.0f, 5.0f, 2.0f, Placement::mid, 1);
    band (4, BandType::notch, 2500.0f, 0.0f, 4.0f, Placement::side, 1);
    band (5, BandType::bell, 6000.0f, 3.0f, 1.0f, Placement::right, 1);
    band (6, BandType::highShelf, 9000.0f, -2.5f, 0.71f, Placement::stereo, 1);
    band (7, BandType::highCut, 16000.0f, 0.0f, 0.707f, Placement::stereo, 2);
    v.set (paramOutGain, -1.5f);
    v.set (paramAutoGain, 1.0f);

    const size_t n = saveState (v, nullptr, 0);
    std::vector<uint8_t> blob (n);
    saveState (v, blob.data(), blob.size());
    return blob;
}
} // namespace th
