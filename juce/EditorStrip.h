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
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

#include "PluginProcessor.h"

namespace kickeq::ui
{
/** The bottom strip: band chips (select / show on-state) and a compact panel for the selected band. Every
    control is attached to the band's parameters with the juce parameter attachments (proper gestures). */
class BandPanel final : public juce::Component
{
public:
    explicit BandPanel (KickEQProcessor&);
    ~BandPanel() override;

    void setSelectedBand (int band);
    int getSelectedBand() const noexcept { return selected; }
    /** Greys out controls that have no effect for the band's type and repaints the chips. */
    void updateState();

    /** The control attached to a parameter of the selected band (on: ToggleButton, type / slope / place: ComboBox,
        freq / gain / q: Slider). */
    juce::Component& getControl (kickeq::BandParam k) noexcept;

    std::function<void (int)> onBandChosen;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Chip;

    void attachControls();

    KickEQProcessor& processor;
    int selected = 0;

    std::array<std::unique_ptr<Chip>, static_cast<size_t> (kickeq::numBands)> chips;
    juce::ToggleButton onButton { "On" };
    juce::ComboBox typeBox, slopeBox, placeBox;
    juce::Slider freqSlider, gainSlider, qSlider;

    std::unique_ptr<juce::ButtonParameterAttachment> onAttachment;
    std::unique_ptr<juce::ComboBoxParameterAttachment> typeAttachment, slopeAttachment, placeAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> freqAttachment, gainAttachment, qAttachment;

    std::array<juce::Rectangle<int>, 7> captionBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandPanel)
};
} // namespace kickeq::ui
