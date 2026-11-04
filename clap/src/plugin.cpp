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

// The native CLAP plugin: maps the CLAP host API onto kickeq::EqEngine / ParamTable / state blob / analyser.
// No GUI toolkit, no exceptions or RTTI, nothing allocated or locked on the audio thread.
//
// Threading model
//   main thread   : init/destroy/activate/deactivate, params get_*/text conversion, state, preset load,
//                   audio-ports, remote-controls, render, vendor analyser extension, on_main_thread.
//   audio thread  : process (and params.flush while the plugin is active), reset.
// Shared state between them: the parameter values (std::atomic<float> per parameter, plain values), the
// tail length, the "render offline" flag and the analyser pointer. Everything else belongs to one thread.

#include "plugin.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include <clap/clap.h>

#include "kickeq/analyzer.h"
#include "kickeq/clap_analyzer.h"
#include "kickeq/eq_engine.h"
#include "kickeq/params.h"
#include "kickeq/presets.h"
#include "kickeq/state.h"

#include "denormals.h"

#if !defined(KICKEQ_THREAD_CHECKS) && !defined(NDEBUG)
#define KICKEQ_THREAD_CHECKS 1
#endif

namespace kickeq::native
{
namespace
{
static_assert (std::atomic<float>::is_always_lock_free, "parameter values are shared through lock-free atomics");
static_assert (std::atomic<uint32_t>::is_always_lock_free, "lock-free atomics required");
static_assert (std::atomic<bool>::is_always_lock_free, "lock-free atomics required");
static_assert (std::atomic<SpectrumAnalyzer*>::is_always_lock_free, "lock-free atomics required");

constexpr const char* pluginId = "com.safian.kickeq";
constexpr const char* const pluginFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                                                  CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    pluginId,
    "KickEQ",
    "Safian",
    "https://github.com/Safian/KickEQ",
    "https://github.com/Safian/KickEQ",
    "https://github.com/Safian/KickEQ/issues",
    KICKEQ_VERSION_STRING,
    "Parametric equalizer with 8 bands (bell, shelves, cuts up to 48 dB/oct, notch, band pass) and mid/side placement",
    pluginFeatures
};

// Parameter block of one process() call is applied at 32-sample granularity by the engine; the shell hands
// the engine every span between two events as one call.
constexpr uint32_t preChunk = 2048;     // analyser path: input copy kept while the engine works in place
constexpr size_t maxStateBytes = 4u << 20;
constexpr uint32_t maxUiChunk = 4096;
constexpr uint32_t pageCount = numBands + 1; // one page per band + one global page

void copyString (char* dst, size_t cap, const char* src) noexcept
{
    if (cap == 0)
        return;
    size_t i = 0;
    for (; src != nullptr && src[i] != '\0' && i + 1 < cap; ++i)
        dst[i] = src[i];
    dst[i] = '\0';
}

inline float toFloat (double v) noexcept
{
    // avoid UB for out-of-range doubles; NaN passes through and becomes the default in clampParam
    return static_cast<float> (v > 1.0e30 ? 1.0e30 : (v < -1.0e30 ? -1.0e30 : v));
}

class Plugin
{
public:
    explicit Plugin (const clap_host_t* h) noexcept
        : host (h), engine (new (std::nothrow) EqEngine())
    {
        plugin.desc = &descriptor;
        plugin.plugin_data = this;
        plugin.init = &Plugin::cInit;
        plugin.destroy = &Plugin::cDestroy;
        plugin.activate = &Plugin::cActivate;
        plugin.deactivate = &Plugin::cDeactivate;
        plugin.start_processing = &Plugin::cStartProcessing;
        plugin.stop_processing = &Plugin::cStopProcessing;
        plugin.reset = &Plugin::cReset;
        plugin.process = &Plugin::cProcess;
        plugin.get_extension = &Plugin::cGetExtension;
        plugin.on_main_thread = &Plugin::cOnMainThread;

        for (int i = 0; i < numParams; ++i)
            shared[i].store (paramInfo (i).def, std::memory_order_relaxed);
    }

    ~Plugin() { delete analyzer.load (std::memory_order_acquire); }

    Plugin (const Plugin&) = delete;
    Plugin& operator= (const Plugin&) = delete;

    bool valid() const noexcept { return engine != nullptr; }
    const clap_plugin_t* clapPlugin() const noexcept { return &plugin; }

private:
    // ----- plumbing --------------------------------------------------------------------------------------

    static Plugin& self (const clap_plugin_t* p) noexcept { return *static_cast<Plugin*> (p->plugin_data); }

    template <typename T>
    const T* hostExt (const char* id) const noexcept
    {
        return host != nullptr && host->get_extension != nullptr ? static_cast<const T*> (host->get_extension (host, id))
                                                                 : nullptr;
    }

    void threadViolation ([[maybe_unused]] const char* where, [[maybe_unused]] const char* expected) const noexcept
    {
#if KICKEQ_THREAD_CHECKS
        if (hostLog != nullptr && hostLog->log != nullptr)
        {
            char msg[160];
            std::snprintf (msg, sizeof msg, "KickEQ: %s called off the %s thread", where, expected);
            hostLog->log (host, CLAP_LOG_PLUGIN_MISBEHAVING, msg);
        }
        else
        {
            assert (false && "CLAP thread-check violation");
        }
#endif
    }

    void checkMainThread ([[maybe_unused]] const char* where) const noexcept
    {
#if KICKEQ_THREAD_CHECKS
        if (hostThreadCheck != nullptr && hostThreadCheck->is_main_thread != nullptr
            && !hostThreadCheck->is_main_thread (host))
            threadViolation (where, "main");
#endif
    }

    void checkAudioThread ([[maybe_unused]] const char* where) const noexcept
    {
#if KICKEQ_THREAD_CHECKS
        if (hostThreadCheck != nullptr && hostThreadCheck->is_audio_thread != nullptr
            && !hostThreadCheck->is_audio_thread (host))
            threadViolation (where, "audio");
#endif
    }

    // ----- shared parameter values -------------------------------------------------------------------------

    void readShared (ParamValues& v) const noexcept
    {
        for (int i = 0; i < numParams; ++i)
            v.values[static_cast<size_t> (i)] = shared[i].load (std::memory_order_relaxed);
    }

    void writeShared (const ParamValues& v) noexcept
    {
        for (int i = 0; i < numParams; ++i)
            shared[i].store (v.get (i), std::memory_order_relaxed);
    }

    /** Parameter value event from the host (audio thread, or flush). Unknown ids are ignored. */
    void storeParamEvent (const clap_event_param_value_t& ev, ParamValues* audioCopy) noexcept
    {
        const int idx = indexOfId (ev.param_id);
        if (idx < 0)
            return;
        const float v = clampParam (paramInfo (idx), toFloat (ev.value));
        shared[idx].store (v, std::memory_order_relaxed);
        if (audioCopy != nullptr)
            audioCopy->values[static_cast<size_t> (idx)] = v;
    }

    void handleEvent (const clap_event_header_t* h, ParamValues* audioCopy) noexcept
    {
        if (h == nullptr || h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        if (h->type == CLAP_EVENT_PARAM_VALUE && h->size >= sizeof (clap_event_param_value_t))
            storeParamEvent (*reinterpret_cast<const clap_event_param_value_t*> (h), audioCopy);
    }

    // ----- lifecycle -----------------------------------------------------------------------------------------

    bool init() noexcept
    {
        hostThreadCheck = hostExt<clap_host_thread_check_t> (CLAP_EXT_THREAD_CHECK);
        hostLog = hostExt<clap_host_log_t> (CLAP_EXT_LOG);
        hostParams = hostExt<clap_host_params_t> (CLAP_EXT_PARAMS);
        hostTail = hostExt<clap_host_tail_t> (CLAP_EXT_TAIL);
        hostPresetLoad = hostExt<clap_host_preset_load_t> (CLAP_EXT_PRESET_LOAD);
        if (hostPresetLoad == nullptr)
            hostPresetLoad = hostExt<clap_host_preset_load_t> (CLAP_EXT_PRESET_LOAD_COMPAT);
        checkMainThread ("init");
        return true;
    }

    bool activate (double sampleRate, uint32_t, uint32_t maxFrames) noexcept
    {
        checkMainThread ("activate");
        if (!(sampleRate > 0.0) || activated)
            return false;
        rate = sampleRate;
        engine->prepare (sampleRate, static_cast<int> (maxFrames > 1u << 20 ? 1u << 20 : maxFrames));
        reportedTail = 0;
        tail.store (0, std::memory_order_relaxed);
        if (SpectrumAnalyzer* a = analyzer.load (std::memory_order_acquire))
            a->prepare (sampleRate); // not processing: no concurrent push()
        activated = true;
        return true;
    }

    void deactivate() noexcept
    {
        checkMainThread ("deactivate");
        activated = false;
    }

    bool startProcessing() noexcept
    {
        checkAudioThread ("start_processing");
        return true;
    }

    void stopProcessing() noexcept { checkAudioThread ("stop_processing"); }

    void reset() noexcept
    {
        checkAudioThread ("reset");
        engine->reset();
    }

    // ----- process ---------------------------------------------------------------------------------------------

    /** Runs the engine in place on [l, r] (r == nullptr: mono) and feeds the analyser when it is on. */
    void renderSpan (float* l, float* r, uint32_t n, SpectrumAnalyzer* an) noexcept
    {
        if (an == nullptr)
        {
            engine->process (audioValues, l, r, static_cast<int> (n));
            return;
        }

        while (n > 0)
        {
            const uint32_t c = n < preChunk ? n : preChunk;
            std::memcpy (pre[0], l, c * sizeof (float));
            if (r != nullptr)
                std::memcpy (pre[1], r, c * sizeof (float));
            engine->process (audioValues, l, r, static_cast<int> (c));
            an->push (pre[0], r != nullptr ? pre[1] : nullptr, l, r, static_cast<int> (c));
            l += c;
            if (r != nullptr)
                r += c;
            n -= c;
        }
    }

    clap_process_status process (const clap_process_t* proc) noexcept
    {
        checkAudioThread ("process");
        if (proc == nullptr)
            return CLAP_PROCESS_ERROR;

        const ScopedNoDenormals noDenormals;

        const uint32_t frames = proc->frames_count;
        const clap_input_events_t* events = proc->in_events;
        readShared (audioValues);

        // buffers: process in place on the output
        float* out[2] = { nullptr, nullptr };
        uint32_t channels = 0;
        bool haveAudio = false;
        if (proc->audio_outputs != nullptr && proc->audio_outputs_count >= 1)
        {
            clap_audio_buffer_t& ob = proc->audio_outputs[0];
            if (ob.data32 != nullptr && (ob.channel_count == 1 || ob.channel_count == 2)
                && ob.data32[0] != nullptr && (ob.channel_count == 1 || ob.data32[1] != nullptr))
            {
                channels = ob.channel_count;
                out[0] = ob.data32[0];
                out[1] = channels == 2 ? ob.data32[1] : nullptr;
                ob.constant_mask = 0; // the filters ring, so no channel is known to be constant / silent
                haveAudio = true;
            }
        }

        if (haveAudio && frames > 0)
        {
            const clap_audio_buffer_t* ib = proc->audio_inputs_count >= 1 ? proc->audio_inputs : nullptr;
            const bool haveInput = ib != nullptr && ib->data32 != nullptr && ib->channel_count >= 1
                                   && ib->data32[0] != nullptr;
            for (uint32_t c = 0; c < channels; ++c)
            {
                const float* src = nullptr;
                if (haveInput)
                    src = ib->data32[c < ib->channel_count ? c : 0];
                if (src == nullptr)
                    std::memset (out[c], 0, frames * sizeof (float));
                else if (src != out[c])
                    std::memmove (out[c], src, frames * sizeof (float));
            }
        }

        SpectrumAnalyzer* an = analyzer.load (std::memory_order_acquire);
        if (an != nullptr && (offline.load (std::memory_order_relaxed) || !an->isEnabled()))
            an = nullptr;

        // sample-accurate events: render up to each event, apply it, continue (zero length spans are skipped)
        uint32_t pos = 0;
        const uint32_t numEvents = events != nullptr && events->size != nullptr && events->get != nullptr
                                       ? events->size (events)
                                       : 0;
        for (uint32_t e = 0; e < numEvents; ++e)
        {
            const clap_event_header_t* h = events->get (events, e);
            if (h == nullptr)
                continue;
            const uint32_t t = h->time < frames ? h->time : frames;
            if (t > pos)
            {
                if (haveAudio)
                    renderSpan (out[0] + pos, out[1] != nullptr ? out[1] + pos : nullptr, t - pos, an);
                pos = t;
            }
            handleEvent (h, &audioValues);
        }
        if (frames > pos && haveAudio)
            renderSpan (out[0] + pos, out[1] != nullptr ? out[1] + pos : nullptr, frames - pos, an);

        updateTail();

        if (!haveAudio && frames > 0)
            return CLAP_PROCESS_ERROR;
        return CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
    }

    /** Publishes the engine tail and tells the host when it changed materially (clap_host_tail::changed is an
        audio thread call). */
    void updateTail() noexcept
    {
        const uint32_t t = engine->getTailSamples();
        tail.store (t, std::memory_order_relaxed);
        const uint32_t diff = t > reportedTail ? t - reportedTail : reportedTail - t;
        const uint32_t threshold = static_cast<uint32_t> (rate * 0.05) > reportedTail / 4
                                       ? static_cast<uint32_t> (rate * 0.05)
                                       : reportedTail / 4;
        if (diff > threshold)
        {
            reportedTail = t;
            if (hostTail != nullptr && hostTail->changed != nullptr)
                hostTail->changed (host);
        }
    }

    void onMainThread() noexcept { checkMainThread ("on_main_thread"); }

    // ----- audio ports -------------------------------------------------------------------------------------------

    uint32_t audioPortsCount (bool) const noexcept { return 1; }

    bool audioPortsGet (uint32_t index, bool, clap_audio_port_info_t* info) const noexcept
    {
        if (index != 0 || info == nullptr)
            return false;
        info->id = 0;
        copyString (info->name, sizeof info->name, "Main");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = portChannels;
        info->port_type = portChannels == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        info->in_place_pair = 0; // input 0 may share its buffers with output 0
        return true;
    }

    static constexpr uint32_t numConfigs = 2; // 0 = stereo, 1 = mono

    bool configGet (uint32_t index, clap_audio_ports_config_t* c) const noexcept
    {
        if (index >= numConfigs || c == nullptr)
            return false;
        const bool mono = index == 1;
        c->id = index;
        copyString (c->name, sizeof c->name, mono ? "Mono" : "Stereo");
        c->input_port_count = 1;
        c->output_port_count = 1;
        c->has_main_input = true;
        c->main_input_channel_count = mono ? 1 : 2;
        c->main_input_port_type = mono ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        c->has_main_output = true;
        c->main_output_channel_count = mono ? 1 : 2;
        c->main_output_port_type = mono ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        return true;
    }

    bool configSelect (clap_id id) noexcept
    {
        checkMainThread ("audio-ports-config.select");
        if (id >= numConfigs || activated)
            return false;
        portChannels = id == 1 ? 1u : 2u;
        return true;
    }

    // ----- params ------------------------------------------------------------------------------------------------

    bool paramsGetInfo (uint32_t index, clap_param_info_t* out) const noexcept
    {
        checkMainThread ("params.get_info");
        if (index >= static_cast<uint32_t> (numParams) || out == nullptr)
            return false;
        const ParamInfo& p = paramInfo (static_cast<int> (index));
        std::memset (out, 0, sizeof *out);
        out->id = p.id;
        out->flags = 0;
        if ((p.flags & flagAutomatable) != 0)
            out->flags |= CLAP_PARAM_IS_AUTOMATABLE;
        if ((p.flags & flagStepped) != 0 || p.kind != Kind::Float)
            out->flags |= CLAP_PARAM_IS_STEPPED;
        if (p.kind == Kind::Enum)
            out->flags |= CLAP_PARAM_IS_ENUM;
        if (index == static_cast<uint32_t> (paramBypass))
            out->flags |= CLAP_PARAM_IS_BYPASS;
        out->cookie = nullptr;
        copyString (out->name, sizeof out->name, p.name);
        copyString (out->module, sizeof out->module, p.module);
        out->min_value = static_cast<double> (p.min);
        out->max_value = static_cast<double> (p.max);
        out->default_value = static_cast<double> (p.def);
        return true;
    }

    bool paramsGetValue (clap_id id, double* out) const noexcept
    {
        checkMainThread ("params.get_value");
        const int idx = indexOfId (id);
        if (idx < 0 || out == nullptr)
            return false;
        *out = static_cast<double> (shared[idx].load (std::memory_order_relaxed));
        return true;
    }

    bool paramsValueToText (clap_id id, double value, char* buf, uint32_t cap) const noexcept
    {
        checkMainThread ("params.value_to_text");
        const int idx = indexOfId (id);
        if (idx < 0 || buf == nullptr || cap == 0)
            return false;
        const ParamInfo& p = paramInfo (idx);
        valueToText (p, clampParam (p, toFloat (value)), buf, cap);
        return true;
    }

    bool paramsTextToValue (clap_id id, const char* text, double* out) const noexcept
    {
        checkMainThread ("params.text_to_value");
        const int idx = indexOfId (id);
        if (idx < 0 || text == nullptr || out == nullptr)
            return false;
        float v = 0.0f;
        if (!textToValue (paramInfo (idx), text, v))
            return false;
        *out = static_cast<double> (v);
        return true;
    }

    void paramsFlush (const clap_input_events_t* in) noexcept
    {
        // main thread while the plugin is not processing, audio thread otherwise: only the shared atomics
        // are written (process() reloads them at the start of every block)
        if (in == nullptr || in->size == nullptr || in->get == nullptr)
            return;
        const uint32_t n = in->size (in);
        for (uint32_t i = 0; i < n; ++i)
            handleEvent (in->get (in, i), nullptr);
    }

    // ----- state -------------------------------------------------------------------------------------------------

    bool stateSave (const clap_ostream_t* stream) noexcept
    {
        checkMainThread ("state.save");
        if (stream == nullptr || stream->write == nullptr)
            return false;
        ParamValues v;
        readShared (v);
        UiChunk ui;
        ui.data = uiSize > 0 ? uiBytes : nullptr;
        ui.size = uiSize;
        const size_t need = saveState (v, nullptr, 0, &ui);
        std::vector<uint8_t> blob (need);
        if (saveState (v, blob.data(), blob.size(), &ui) != need)
            return false;

        size_t done = 0;
        while (done < blob.size())
        {
            const int64_t w = stream->write (stream, blob.data() + done, blob.size() - done);
            if (w <= 0 || static_cast<uint64_t> (w) > blob.size() - done)
                return false;
            done += static_cast<size_t> (w);
        }
        return true;
    }

    bool stateLoad (const clap_istream_t* stream) noexcept
    {
        checkMainThread ("state.load");
        if (stream == nullptr || stream->read == nullptr)
            return false;

        std::vector<uint8_t> blob;
        for (;;)
        {
            const size_t old = blob.size();
            blob.resize (old + 4096);
            const int64_t r = stream->read (stream, blob.data() + old, 4096);
            if (r < 0 || static_cast<uint64_t> (r) > 4096u)
                return false; // stream error (or a stream returning more than asked)
            blob.resize (old + static_cast<size_t> (r));
            if (r == 0)
                break; // end of stream (short reads are fine: only 0 ends it)
            if (blob.size() > maxStateBytes)
                return false;
        }

        ParamValues v;
        UiChunk ui;
        if (!loadState (blob.data(), blob.size(), v, &ui))
            return false;

        if (ui.data != nullptr && ui.size > 0 && ui.size <= maxUiChunk)
        {
            std::memcpy (uiBytes, ui.data, ui.size);
            uiSize = ui.size;
        }
        else
        {
            uiSize = 0;
        }
        writeShared (v);
        // the loaded values were not set by the host: it has to re-read them
        if (hostParams != nullptr && hostParams->rescan != nullptr)
            hostParams->rescan (host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        return true;
    }

    // ----- remote controls -----------------------------------------------------------------------------------------

    static uint32_t remoteCount() noexcept { return pageCount; }

    static bool remoteGet (uint32_t page, clap_remote_controls_page_t* out) noexcept
    {
        if (page >= pageCount || out == nullptr)
            return false;
        std::memset (out, 0, sizeof *out);
        for (uint32_t& id : out->param_ids)
            id = CLAP_INVALID_ID;
        out->is_for_preset = false;

        if (page < static_cast<uint32_t> (numBands))
        {
            const int b = static_cast<int> (page);
            copyString (out->section_name, sizeof out->section_name, "Bands");
            copyString (out->page_name, sizeof out->page_name, paramInfo (bandParamIndex (b, BandParam::on)).module);
            out->page_id = 1u + page;
            for (int k = 0; k < paramsPerBand; ++k)
                out->param_ids[k] = bandParamId (b, static_cast<BandParam> (k));
        }
        else
        {
            copyString (out->section_name, sizeof out->section_name, "Global");
            copyString (out->page_name, sizeof out->page_name, "Global");
            out->page_id = 0;
            for (int i = 0; i < numGlobalParams; ++i)
                out->param_ids[i] = paramInfo (i).id;
        }
        return true;
    }

    // ----- preset load --------------------------------------------------------------------------------------------------

    bool presetFromLocation (uint32_t kind, const char* location, const char* loadKey) noexcept
    {
        checkMainThread ("preset-load.from_location");
        const int idx = kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? presetIndexForLoadKey (loadKey) : -1;
        ParamValues v;
        if (idx < 0 || !applyPreset (idx, v))
        {
            if (hostPresetLoad != nullptr && hostPresetLoad->on_error != nullptr)
                hostPresetLoad->on_error (host, kind, location, loadKey, 0, "unknown KickEQ preset");
            return false;
        }
        writeShared (v);
        if (hostPresetLoad != nullptr && hostPresetLoad->loaded != nullptr)
            hostPresetLoad->loaded (host, kind, location, loadKey);
        if (hostParams != nullptr && hostParams->rescan != nullptr)
            hostParams->rescan (host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        return true;
    }

    // ----- render ---------------------------------------------------------------------------------------------------------

    bool renderSet (clap_plugin_render_mode mode) noexcept
    {
        checkMainThread ("render.set");
        if (mode != CLAP_RENDER_REALTIME && mode != CLAP_RENDER_OFFLINE)
            return false;
        offline.store (mode == CLAP_RENDER_OFFLINE, std::memory_order_relaxed);
        return true;
    }

    // ----- analyser vendor extension ---------------------------------------------------------------------------------------

    void analyzerSetEnabled (bool enabled) noexcept
    {
        checkMainThread ("analyzer.set_enabled");
        SpectrumAnalyzer* a = analyzer.load (std::memory_order_acquire);
        if (a == nullptr)
        {
            if (!enabled)
                return;
            a = new (std::nothrow) SpectrumAnalyzer (KICKEQ_CLAP_ANALYZER_NUM_BINS);
            if (a == nullptr)
                return;
            a->prepare (rate); // fully built before the audio thread can see it
            analyzer.store (a, std::memory_order_release);
        }
        a->setEnabled (enabled);
    }

    bool analyzerGetFrame (float* preDb, float* postDb, uint32_t n) noexcept
    {
        checkMainThread ("analyzer.get_frame");
        SpectrumAnalyzer* a = analyzer.load (std::memory_order_acquire);
        if (a == nullptr || n != static_cast<uint32_t> (KICKEQ_CLAP_ANALYZER_NUM_BINS))
            return false;
        a->process();
        a->pull (frame);
        if (frame.preDb.size() != n || frame.postDb.size() != n)
            return false; // no frame produced yet
        if (preDb != nullptr)
            std::memcpy (preDb, frame.preDb.data(), n * sizeof (float));
        if (postDb != nullptr)
            std::memcpy (postDb, frame.postDb.data(), n * sizeof (float));
        return true;
    }

    // ----- C trampolines ------------------------------------------------------------------------------------------------------

    static bool CLAP_ABI cInit (const clap_plugin_t* p) { return self (p).init(); }
    static void CLAP_ABI cDestroy (const clap_plugin_t* p) { delete &self (p); }
    static bool CLAP_ABI cActivate (const clap_plugin_t* p, double sr, uint32_t mn, uint32_t mx) { return self (p).activate (sr, mn, mx); }
    static void CLAP_ABI cDeactivate (const clap_plugin_t* p) { self (p).deactivate(); }
    static bool CLAP_ABI cStartProcessing (const clap_plugin_t* p) { return self (p).startProcessing(); }
    static void CLAP_ABI cStopProcessing (const clap_plugin_t* p) { self (p).stopProcessing(); }
    static void CLAP_ABI cReset (const clap_plugin_t* p) { self (p).reset(); }
    static clap_process_status CLAP_ABI cProcess (const clap_plugin_t* p, const clap_process_t* proc) { return self (p).process (proc); }
    static void CLAP_ABI cOnMainThread (const clap_plugin_t* p) { self (p).onMainThread(); }
    static const void* CLAP_ABI cGetExtension (const clap_plugin_t* p, const char* id);

    // extension tables (defined below the class)
    static const clap_plugin_audio_ports_t audioPortsExt;
    static const clap_plugin_audio_ports_config_t audioPortsConfigExt;
    static const clap_plugin_params_t paramsExt;
    static const clap_plugin_state_t stateExt;
    static const clap_plugin_latency_t latencyExt;
    static const clap_plugin_tail_t tailExt;
    static const clap_plugin_render_t renderExt;
    static const clap_plugin_remote_controls_t remoteControlsExt;
    static const clap_plugin_preset_load_t presetLoadExt;
    static const kickeq_clap_plugin_analyzer_t analyzerExt;

    // ----- data -------------------------------------------------------------------------------------------------------------------

    clap_plugin_t plugin {};
    const clap_host_t* host;

    // host extensions (main thread, set in init)
    const clap_host_thread_check_t* hostThreadCheck = nullptr;
    const clap_host_log_t* hostLog = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_tail_t* hostTail = nullptr;
    const clap_host_preset_load_t* hostPresetLoad = nullptr;

    std::unique_ptr<EqEngine> engine; // ~15 KB, allocated once here

    std::atomic<float> shared[numParams]; // plain parameter values, main <-> audio
    ParamValues audioValues;              // audio thread working copy

    std::atomic<uint32_t> tail { 0 };
    uint32_t reportedTail = 0; // audio thread
    std::atomic<bool> offline { false };
    std::atomic<SpectrumAnalyzer*> analyzer { nullptr };
    SpectrumFrame frame; // main thread, last pulled frame
    float pre[2][preChunk] = {};

    double rate = 48000.0;
    bool activated = false;     // main thread
    uint32_t portChannels = 2;  // main thread (selected audio ports config)

    uint8_t uiBytes[maxUiChunk] = {}; // opaque "UI01" chunk of a foreign (JUCE) state, kept for round trips
    uint32_t uiSize = 0;

    friend struct ExtTables;
};

// The extension tables are defined as plain structs of trampolines; they need the private members, so the
// trampolines live in a friend struct.
struct ExtTables
{
    static Plugin& s (const clap_plugin_t* p) noexcept { return *static_cast<Plugin*> (p->plugin_data); }

    // audio-ports
    static uint32_t CLAP_ABI portsCount (const clap_plugin_t* p, bool in) { return s (p).audioPortsCount (in); }
    static bool CLAP_ABI portsGet (const clap_plugin_t* p, uint32_t i, bool in, clap_audio_port_info_t* info) { return s (p).audioPortsGet (i, in, info); }
    // audio-ports-config
    static uint32_t CLAP_ABI cfgCount (const clap_plugin_t*) { return Plugin::numConfigs; }
    static bool CLAP_ABI cfgGet (const clap_plugin_t* p, uint32_t i, clap_audio_ports_config_t* c) { return s (p).configGet (i, c); }
    static bool CLAP_ABI cfgSelect (const clap_plugin_t* p, clap_id id) { return s (p).configSelect (id); }
    // params
    static uint32_t CLAP_ABI parCount (const clap_plugin_t*) { return static_cast<uint32_t> (numParams); }
    static bool CLAP_ABI parInfo (const clap_plugin_t* p, uint32_t i, clap_param_info_t* o) { return s (p).paramsGetInfo (i, o); }
    static bool CLAP_ABI parValue (const clap_plugin_t* p, clap_id id, double* o) { return s (p).paramsGetValue (id, o); }
    static bool CLAP_ABI parToText (const clap_plugin_t* p, clap_id id, double v, char* b, uint32_t c) { return s (p).paramsValueToText (id, v, b, c); }
    static bool CLAP_ABI parToValue (const clap_plugin_t* p, clap_id id, const char* t, double* o) { return s (p).paramsTextToValue (id, t, o); }
    static void CLAP_ABI parFlush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) { s (p).paramsFlush (in); }
    // state
    static bool CLAP_ABI stSave (const clap_plugin_t* p, const clap_ostream_t* o) { return s (p).stateSave (o); }
    static bool CLAP_ABI stLoad (const clap_plugin_t* p, const clap_istream_t* i) { return s (p).stateLoad (i); }
    // latency / tail
    static uint32_t CLAP_ABI latGet (const clap_plugin_t* p) { return static_cast<uint32_t> (s (p).engine->getLatencySamples()); }
    static uint32_t CLAP_ABI tailGet (const clap_plugin_t* p) { return s (p).tail.load (std::memory_order_relaxed); }
    // render
    static bool CLAP_ABI renderRealtimeOnly (const clap_plugin_t*) { return false; }
    static bool CLAP_ABI renderSetMode (const clap_plugin_t* p, clap_plugin_render_mode m) { return s (p).renderSet (m); }
    // remote controls
    static uint32_t CLAP_ABI rcCount (const clap_plugin_t*) { return Plugin::remoteCount(); }
    static bool CLAP_ABI rcGet (const clap_plugin_t*, uint32_t i, clap_remote_controls_page_t* o) { return Plugin::remoteGet (i, o); }
    // preset load
    static bool CLAP_ABI presetLoad (const clap_plugin_t* p, uint32_t k, const char* loc, const char* key) { return s (p).presetFromLocation (k, loc, key); }
    // analyser
    static void CLAP_ABI anEnable (const clap_plugin_t* p, bool e) { s (p).analyzerSetEnabled (e); }
    static uint32_t CLAP_ABI anBins (const clap_plugin_t*) { return static_cast<uint32_t> (KICKEQ_CLAP_ANALYZER_NUM_BINS); }
    static float CLAP_ABI anFreq (const clap_plugin_t*, uint32_t bin)
    {
        return bin < static_cast<uint32_t> (KICKEQ_CLAP_ANALYZER_NUM_BINS)
                   ? SpectrumAnalyzer::displayFrequency (static_cast<int> (bin), KICKEQ_CLAP_ANALYZER_NUM_BINS)
                   : 0.0f;
    }
    static bool CLAP_ABI anFrame (const clap_plugin_t* p, float* a, float* b, uint32_t n) { return s (p).analyzerGetFrame (a, b, n); }
};

const clap_plugin_audio_ports_t Plugin::audioPortsExt = { &ExtTables::portsCount, &ExtTables::portsGet };
const clap_plugin_audio_ports_config_t Plugin::audioPortsConfigExt = { &ExtTables::cfgCount, &ExtTables::cfgGet, &ExtTables::cfgSelect };
const clap_plugin_params_t Plugin::paramsExt = { &ExtTables::parCount, &ExtTables::parInfo, &ExtTables::parValue,
                                                 &ExtTables::parToText, &ExtTables::parToValue, &ExtTables::parFlush };
const clap_plugin_state_t Plugin::stateExt = { &ExtTables::stSave, &ExtTables::stLoad };
const clap_plugin_latency_t Plugin::latencyExt = { &ExtTables::latGet };
const clap_plugin_tail_t Plugin::tailExt = { &ExtTables::tailGet };
const clap_plugin_render_t Plugin::renderExt = { &ExtTables::renderRealtimeOnly, &ExtTables::renderSetMode };
const clap_plugin_remote_controls_t Plugin::remoteControlsExt = { &ExtTables::rcCount, &ExtTables::rcGet };
const clap_plugin_preset_load_t Plugin::presetLoadExt = { &ExtTables::presetLoad };
const kickeq_clap_plugin_analyzer_t Plugin::analyzerExt = { &ExtTables::anEnable, &ExtTables::anBins, &ExtTables::anFreq,
                                                            &ExtTables::anFrame };

const void* CLAP_ABI Plugin::cGetExtension (const clap_plugin_t*, const char* id)
{
    if (id == nullptr)
        return nullptr;
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPortsExt;
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS_CONFIG) == 0) return &audioPortsConfigExt;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &paramsExt;
    if (std::strcmp (id, CLAP_EXT_STATE) == 0) return &stateExt;
    if (std::strcmp (id, CLAP_EXT_LATENCY) == 0) return &latencyExt;
    if (std::strcmp (id, CLAP_EXT_TAIL) == 0) return &tailExt;
    if (std::strcmp (id, CLAP_EXT_RENDER) == 0) return &renderExt;
    if (std::strcmp (id, CLAP_EXT_REMOTE_CONTROLS) == 0 || std::strcmp (id, CLAP_EXT_REMOTE_CONTROLS_COMPAT) == 0)
        return &remoteControlsExt;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0 || std::strcmp (id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0)
        return &presetLoadExt;
    if (std::strcmp (id, KICKEQ_CLAP_EXT_ANALYZER) == 0) return &analyzerExt;
    return nullptr; // clap.gui and everything else: unsupported
}
} // namespace

const clap_plugin_descriptor_t* pluginDescriptor() noexcept { return &descriptor; }

const clap_plugin_t* createPlugin (const clap_host_t* host) noexcept
{
    Plugin* p = new (std::nothrow) Plugin (host);
    if (p == nullptr)
        return nullptr;
    if (!p->valid())
    {
        delete p;
        return nullptr;
    }
    return p->clapPlugin();
}

int presetIndexForLoadKey (const char* loadKey) noexcept
{
    if (loadKey == nullptr || loadKey[0] == '\0')
        return -1;
    for (int i = 0; i < numPresets; ++i)
        if (std::strcmp (factoryPresets[i].name, loadKey) == 0)
            return i;

    // decimal index
    int value = 0;
    for (const char* c = loadKey; *c != '\0'; ++c)
    {
        if (*c < '0' || *c > '9' || value > 1000)
            return -1;
        value = value * 10 + (*c - '0');
    }
    return value < numPresets ? value : -1;
}
} // namespace kickeq::native
