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

#include "PluginProcessor.h"

#include <cstring>

#include "Editor.h"

namespace
{
constexpr int analyzerBins = 512;
}

KickEQProcessor::KickEQProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      engine (new kickeq::EqEngine()),
      analyzer (new kickeq::SpectrumAnalyzer (analyzerBins))
{
    // every parameter comes from the core table; the bands become parameter groups (CLAP module / VST3 unit)
    int index = 0;
    for (; index < kickeq::numGlobalParams; ++index)
    {
        auto p = std::make_unique<CoreParameter> (index);
        parameters[static_cast<size_t> (index)] = p.get();
        addParameter (p.release());
    }

    for (int b = 0; b < kickeq::numBands; ++b)
    {
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> (
            "band" + juce::String (b), kickeq::paramInfo (kickeq::bandParamIndex (b, kickeq::BandParam::on)).module, "|");
        for (int k = 0; k < kickeq::paramsPerBand; ++k, ++index)
        {
            auto p = std::make_unique<CoreParameter> (index);
            parameters[static_cast<size_t> (index)] = p.get();
            group->addChild (std::move (p));
        }
        addParameterGroup (std::move (group));
    }

    jassert (index == kickeq::numParams);
    analyzer->prepare (48000.0);
}

KickEQProcessor::~KickEQProcessor() = default;

void KickEQProcessor::prepareToPlay (double newSampleRate, int samplesPerBlock)
{
    sampleRate.store (newSampleRate, std::memory_order_relaxed);
    engine->prepare (newSampleRate, samplesPerBlock);
    analyzer->prepare (newSampleRate); // not processing: no concurrent push()
    tailSamples.store (0, std::memory_order_relaxed);
}

void KickEQProcessor::acquireAnalyzer() noexcept
{
    analyzerClients.fetch_add (1);
    analyzer->setEnabled (true);
}

void KickEQProcessor::releaseAnalyzer() noexcept
{
    if (analyzerClients.fetch_sub (1) <= 1)
        analyzer->setEnabled (false);
}

double KickEQProcessor::getDisplaySampleRate() const noexcept
{
    const double sr = sampleRate.load (std::memory_order_relaxed);
    return sr > 0.0 ? sr : 48000.0;
}

bool KickEQProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

double KickEQProcessor::getTailLengthSeconds() const
{
    return static_cast<double> (tailSamples.load (std::memory_order_relaxed)) / getDisplaySampleRate();
}

void KickEQProcessor::readValues (kickeq::ParamValues& values) const noexcept
{
    for (size_t i = 0; i < parameters.size(); ++i)
        values.values[i] = parameters[i]->getPlain();
}

void KickEQProcessor::applyValues (const kickeq::ParamValues& values)
{
    for (int i = 0; i < kickeq::numParams; ++i)
    {
        CoreParameter& p = *parameters[static_cast<size_t> (i)];
        const float v = values.get (i);
        if (!juce::exactlyEqual (v, p.getPlain()))
            p.setPlainNotifyingHost (v);
    }
}

void KickEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2);
    if (numSamples <= 0 || numChannels <= 0)
        return;

    for (int ch = numChannels; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    readValues (audioValues);

    float* left = buffer.getWritePointer (0);
    float* right = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    if (!analyzer->isEnabled())
    {
        engine->process (audioValues, left, right, numSamples);
    }
    else
    {
        // the analyser wants the input as well: keep a copy while the engine works in place
        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin (preChunk, numSamples - done);
            std::memcpy (pre[0], left + done, static_cast<size_t> (n) * sizeof (float));
            if (right != nullptr)
                std::memcpy (pre[1], right + done, static_cast<size_t> (n) * sizeof (float));
            engine->process (audioValues, left + done, right != nullptr ? right + done : nullptr, n);
            analyzer->push (pre[0], right != nullptr ? pre[1] : nullptr, left + done,
                            right != nullptr ? right + done : nullptr, n);
            done += n;
        }
    }

    tailSamples.store (engine->getTailSamples(), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* KickEQProcessor::createEditor()
{
    return new KickEQEditor (*this);
}

// ---- programs (factory presets) -------------------------------------------------------------------------------

int KickEQProcessor::findMatchingPreset() const noexcept
{
    kickeq::ParamValues current, preset;
    readValues (current);
    current.clamp();
    for (int i = 0; i < kickeq::numPresets; ++i)
    {
        kickeq::applyPreset (i, preset);
        if (preset == current)
            return i;
    }
    return -1;
}

int KickEQProcessor::getCurrentProgram()
{
    const int match = findMatchingPreset();
    return match >= 0 ? match : lastProgram.load (std::memory_order_relaxed);
}

void KickEQProcessor::setCurrentProgram (int index)
{
    kickeq::ParamValues values;
    if (!kickeq::applyPreset (index, values))
        return;
    lastProgram.store (index, std::memory_order_relaxed);
    applyValues (values);
}

const juce::String KickEQProcessor::getProgramName (int index)
{
    return index >= 0 && index < kickeq::numPresets ? juce::String (kickeq::factoryPresets[index].name) : juce::String();
}

// ---- state ----------------------------------------------------------------------------------------------------------

kickeq::ui::EditorSettings KickEQProcessor::getEditorSettings() const noexcept
{
    kickeq::ui::EditorSettings s;
    s.analyzerSource = uiSource.load (std::memory_order_relaxed);
    s.analyzerRange = uiRange.load (std::memory_order_relaxed);
    s.slopeComp = uiSlope.load (std::memory_order_relaxed);
    s.curveRange = uiCurve.load (std::memory_order_relaxed);
    return s;
}

void KickEQProcessor::setEditorSettingsInternal (const kickeq::ui::EditorSettings& s) noexcept
{
    uiSource.store (s.analyzerSource, std::memory_order_relaxed);
    uiRange.store (s.analyzerRange, std::memory_order_relaxed);
    uiSlope.store (s.slopeComp, std::memory_order_relaxed);
    uiCurve.store (s.curveRange, std::memory_order_relaxed);
    analyzer->setSource (static_cast<kickeq::AnalyzerSource> (s.analyzerSource));
}

void KickEQProcessor::setEditorSettings (const kickeq::ui::EditorSettings& s)
{
    if (s == getEditorSettings())
        return;
    setEditorSettingsInternal (s);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void KickEQProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    kickeq::ParamValues values;
    readValues (values);

    uint8_t chunk[kickeq::ui::EditorSettings::chunkSize];
    getEditorSettings().toChunk (chunk);
    kickeq::UiChunk ui;
    ui.data = chunk;
    ui.size = static_cast<uint32_t> (sizeof chunk);

    const size_t needed = kickeq::saveState (values, nullptr, 0, &ui);
    destData.setSize (needed);
    if (kickeq::saveState (values, static_cast<uint8_t*> (destData.getData()), needed, &ui) != needed)
        destData.reset();
}

void KickEQProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    kickeq::ParamValues values;
    kickeq::UiChunk ui;
    if (!kickeq::loadState (static_cast<const uint8_t*> (data), static_cast<size_t> (sizeInBytes), values, &ui))
        return; // not a KickEQ state: leave everything as it is

    setEditorSettingsInternal (kickeq::ui::EditorSettings::fromChunk (ui.data, ui.size));
    for (int i = 0; i < kickeq::numParams; ++i)
        parameters[static_cast<size_t> (i)]->setPlainNotifyingHost (values.get (i)); // exact plain value, host + UI notified
}
