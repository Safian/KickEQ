/*
 * KickEQ - stereo 4-band parametric EQ (VST3 + CLAP)
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

namespace
{
struct BandSpec
{
    const char* idPrefix;
    const char* displayName;
    kickeq::BandType type;
    float freqMin, freqMax, freqDefault;
    float qDefault;
};

constexpr std::array<BandSpec, KickEQProcessor::numBands> bandSpecs { {
    { "ls", "Low Shelf",  kickeq::BandType::lowShelf,  20.0f,   2000.0f,   80.0f, 0.71f },
    { "p1", "Peak 1",     kickeq::BandType::peak,      40.0f,   5000.0f,  250.0f, 1.0f  },
    { "p2", "Peak 2",     kickeq::BandType::peak,     200.0f,  16000.0f, 3000.0f, 1.0f  },
    { "hs", "High Shelf", kickeq::BandType::highShelf, 1000.0f, 20000.0f, 8000.0f, 0.71f },
} };

constexpr int subBlock = 32; // coefficient update interval (samples) while a parameter is moving
const juce::ParameterID bypassId { "bypass", 1 };

juce::String paramId (const BandSpec& s, const char* suffix)
{
    return juce::String (s.idPrefix) + "_" + suffix;
}

juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre)
{
    juce::NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (centre);
    return r;
}
} // namespace

KickEQProcessor::KickEQProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "KickEQState", createParameterLayout())
{
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& spec = bandSpecs[i];
        auto& band = bands[i];
        band.type = spec.type;
        band.freq = apvts.getRawParameterValue (paramId (spec, "freq"));
        band.gain = apvts.getRawParameterValue (paramId (spec, "gain"));
        band.q    = apvts.getRawParameterValue (paramId (spec, "q"));
    }

    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (bypassId.getParamID()));
    jassert (bypassParam != nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout KickEQProcessor::createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    for (const auto& s : bandSpecs)
    {
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { paramId (s, "freq"), 1 }, String (s.displayName) + " Freq",
            skewedRange (s.freqMin, s.freqMax, std::sqrt (s.freqMin * s.freqMax)), s.freqDefault,
            AudioParameterFloatAttributes().withLabel ("Hz")));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { paramId (s, "gain"), 1 }, String (s.displayName) + " Gain",
            NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f,
            AudioParameterFloatAttributes().withLabel ("dB")));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { paramId (s, "q"), 1 }, String (s.displayName) + " Q",
            skewedRange (0.1f, 10.0f, 1.0f), s.qDefault));
    }

    layout.add (std::make_unique<AudioParameterBool> (bypassId, "Bypass", false));

    return layout;
}

void KickEQProcessor::updateCoefficients (Band& band, float freq, float gainDb, float q) noexcept
{
    band.filter.setCoefficients (kickeq::BiquadCoefficients::make (band.type, currentSampleRate, freq, gainDb, q));
}

void KickEQProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    dryBuffer.setSize (2, std::max (samplesPerBlock, 512), false, false, true);

    for (auto& band : bands)
    {
        band.freqSmooth.reset (sampleRate, 0.03);
        band.gainSmooth.reset (sampleRate, 0.03);
        band.qSmooth.reset (sampleRate, 0.03);
        band.freqSmooth.setCurrentAndTargetValue (band.freq->load());
        band.gainSmooth.setCurrentAndTargetValue (band.gain->load());
        band.qSmooth.setCurrentAndTargetValue (band.q->load());
        updateCoefficients (band, band.freq->load(), band.gain->load(), band.q->load());
        band.filter.reset();
    }

    wetMix.reset (sampleRate, 0.02);
    wetMix.setCurrentAndTargetValue (bypassParam->get() ? 0.0f : 1.0f);
    filtersNeedReset = false;
}

bool KickEQProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void KickEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = std::min (buffer.getNumChannels(), 2);
    const int capacity    = dryBuffer.getNumSamples();

    if (capacity == 0 || numChannels == 0)
        return; // not prepared

    wetMix.setTargetValue (bypassParam->get() ? 0.0f : 1.0f);

    for (auto& band : bands)
    {
        band.freqSmooth.setTargetValue (band.freq->load());
        band.gainSmooth.setTargetValue (band.gain->load());
        band.qSmooth.setTargetValue (band.q->load());
    }

    for (int offset = 0; offset < numSamples; offset += capacity)
    {
        const int len = std::min (capacity, numSamples - offset);
        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), numChannels, offset, len);
        processChunk (chunk);
    }
}

void KickEQProcessor::processChunk (juce::AudioBuffer<float>& chunk) noexcept
{
    const int numSamples  = chunk.getNumSamples();
    const int numChannels = chunk.getNumChannels();

    const bool fullyBypassed = ! wetMix.isSmoothing() && wetMix.getTargetValue() <= 0.0f;
    if (fullyBypassed)
    {
        // Audio passes through untouched; keep the smoothers running so the filters
        // are correct the moment the effect is switched back on.
        for (auto& band : bands)
        {
            band.freqSmooth.skip (numSamples);
            band.gainSmooth.skip (numSamples);
            band.qSmooth.skip (numSamples);
        }
        filtersNeedReset = true;
        return;
    }

    if (filtersNeedReset)
    {
        for (auto& band : bands)
        {
            updateCoefficients (band, band.freqSmooth.getCurrentValue(), band.gainSmooth.getCurrentValue(),
                                band.qSmooth.getCurrentValue());
            band.filter.reset();
        }
        filtersNeedReset = false;
    }

    const bool crossfading = wetMix.isSmoothing();
    if (crossfading)
        for (int ch = 0; ch < numChannels; ++ch)
            dryBuffer.copyFrom (ch, 0, chunk, ch, 0, numSamples);

    for (int pos = 0; pos < numSamples; pos += subBlock)
    {
        const int len = std::min (subBlock, numSamples - pos);

        for (auto& band : bands)
        {
            if (band.freqSmooth.isSmoothing() || band.gainSmooth.isSmoothing() || band.qSmooth.isSmoothing())
            {
                const float f = band.freqSmooth.skip (len);
                const float g = band.gainSmooth.skip (len);
                const float q = band.qSmooth.skip (len);
                updateCoefficients (band, f, g, q);
            }

            for (int ch = 0; ch < numChannels; ++ch)
                band.filter.process (ch, chunk.getWritePointer (ch) + pos, len);
        }
    }

    if (crossfading)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float wet = wetMix.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* out = chunk.getWritePointer (ch);
                out[i] = out[i] * wet + dryBuffer.getReadPointer (ch)[i] * (1.0f - wet);
            }
        }
    }
}

juce::AudioProcessorEditor* KickEQProcessor::createEditor()
{
    // Generic parameter GUI only - spectrum analyser / custom editor come later.
    return new juce::GenericAudioProcessorEditor (*this);
}

void KickEQProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void KickEQProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// This creates new instances of the plugin (entry point used by the VST3 and CLAP wrappers).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KickEQProcessor();
}
