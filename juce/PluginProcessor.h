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

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include "CoreParameter.h"
#include "EditorSettings.h"
#include "kickeq/analyzer.h"
#include "kickeq/config.h"
#include "kickeq/eq_engine.h"
#include "kickeq/params.h"
#include "kickeq/presets.h"
#include "kickeq/state.h"

#if KICKEQ_LINEAR_PHASE
#include "kickeq/design_thread.h"
#endif

/** The JUCE shell of KickEQ: maps the JUCE plugin API onto kickeq::EqEngine, kickeq::paramTable, the core state
    blob and the core spectrum analyser (ARCHITECTURE.md sections 2.1, 3, 6, 8). */
class KickEQProcessor final : public juce::AudioProcessor,
                              private juce::AsyncUpdater,
                              private juce::AudioProcessorParameter::Listener
{
public:
    KickEQProcessor();
    ~KickEQProcessor() override;

    // ---- juce::AudioProcessor ----------------------------------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override { engine->reset(); }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        processBlock (buffer, midi); // bypass is a cross-fade inside the engine
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "KickEQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return parameters[kickeq::paramBypass]; }

    int getNumPrograms() override { return kickeq::numPresets; }
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- for the editor ----------------------------------------------------------------------------------------
    CoreParameter& getCoreParameter (int tableIndex) noexcept { return *parameters[static_cast<size_t> (tableIndex)]; }
    CoreParameter& getCoreParameter (int band, kickeq::BandParam k) noexcept
    {
        return getCoreParameter (kickeq::bandParamIndex (band, k));
    }

    /** The current plain values of all parameters (any thread). */
    void readValues (kickeq::ParamValues& values) const noexcept;
    /** Sets all parameters (message thread), notifying the host for every value that changes. */
    void applyValues (const kickeq::ParamValues& values);

    kickeq::SpectrumAnalyzer& getAnalyzer() noexcept { return *analyzer; }
    /** The analyser only runs while at least one editor needs it (the audio thread skips it otherwise). */
    void acquireAnalyzer() noexcept;
    void releaseAnalyzer() noexcept;

    kickeq::ui::EditorSettings getEditorSettings() const noexcept;
    /** Changes the editor state; tells the host that the saved state changed (message thread). */
    void setEditorSettings (const kickeq::ui::EditorSettings& settings);

    /** Index of the factory preset whose values equal the current parameters, or -1. */
    int findMatchingPreset() const noexcept;

    /** Sample rate for display purposes (48 kHz before the first prepareToPlay). */
    double getDisplaySampleRate() const noexcept;

    /** The phase mode parameter as an index (0 = minimum, 1..3 = linear 2048 / 4096 / 8192). Any thread. */
    int getPhaseMode() const noexcept;
    /** Latency in samples of a phase mode at the current sample rate setting (what getLatencySamples () reports once
        the mode has been applied). */
    int getLatencyForMode (int mode) const noexcept;

private:
    void setEditorSettingsInternal (const kickeq::ui::EditorSettings& settings) noexcept;

    // phase mode: the parameter listener raises an async update, which (message thread) tells the host about the new
    // latency (AudioProcessor::setLatencySamples -> updateHostDisplay) and wakes / parks the design thread
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int, bool) override {}
    void handleAsyncUpdate() override;
    void applyPhaseMode();
    void syncDesignThread (int mode);
    void stopDesignThread();

    static constexpr int preChunk = 2048; // analyser path: input copy kept while the engine works in place

    std::array<CoreParameter*, static_cast<size_t> (kickeq::numParams)> parameters {}; // owned by the processor
    std::unique_ptr<kickeq::EqEngine> engine;
#if KICKEQ_LINEAR_PHASE
    kickeq::DesignThread designThread; // after the engine: joined before the engine is destroyed
    std::mutex designMutex;            // start / stop / wake of the design thread (never taken by the audio thread)
    bool prepared = false;             // guarded by designMutex: between prepareToPlay and releaseResources
#endif
    std::unique_ptr<kickeq::SpectrumAnalyzer> analyzer;

    kickeq::ParamValues audioValues;       // audio thread working copy
    std::atomic<uint32_t> tailSamples { 0 };
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<int> lastProgram { 0 };
    std::atomic<int> analyzerClients { 0 };
    float pre[2][preChunk] = {};

    std::atomic<int> uiSource { 0 }, uiRange { 1 }, uiSlope { 1 }, uiCurve { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickEQProcessor)
};
