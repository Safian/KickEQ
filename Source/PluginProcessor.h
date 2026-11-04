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

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

#include "Biquad.h"

class KickEQProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int numBands = 4;

    KickEQProcessor();
    ~KickEQProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        processBlock (buffer, midi); // bypass is handled (with a cross-fade) inside processBlock
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    struct Band
    {
        kickeq::BandType type = kickeq::BandType::peak;
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q    = nullptr;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> freqSmooth;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmooth;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> qSmooth;
        kickeq::StereoBiquad filter;
    };

    void updateCoefficients (Band& band, float freq, float gainDb, float q) noexcept;
    void processChunk (juce::AudioBuffer<float>& chunk) noexcept;

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioParameterBool* bypassParam = nullptr;
    std::array<Band, numBands> bands;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix { 1.0f };
    juce::AudioBuffer<float> dryBuffer;
    double currentSampleRate = 44100.0;
    bool filtersNeedReset = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickEQProcessor)
};
