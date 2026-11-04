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

#include <memory>

#include "EditorCanvas.h"
#include "EditorStrip.h"
#include "EditorTheme.h"
#include "PluginProcessor.h"
#include "kickeq/analyzer.h"

/** The KickEQ editor: toolbar, spectrum + curve canvas, per-band strip (ARCHITECTURE.md section 6).
    Software rendered, resizable (720 x 420 .. 3840 x 2160, default 960 x 560). */
class KickEQEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int defaultWidth = 960, defaultHeight = 560;
    static constexpr int minWidth = 720, minHeight = 420;

    explicit KickEQEditor (KickEQProcessor&);
    ~KickEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** One 30 Hz tick: services the analyser, refreshes curve and controls, repaints. Public so that tools
        (the snapshot generator) can drive the editor without a running message loop. */
    void refreshFrame();

    /** Selects and highlights a band and sets the curve's channel view, as if the mouse hovered its node (used by
        the snapshot tool to render the interactive state). */
    void previewBand (int band, kickeq::ChannelView view);

    /** The canvas and the band strip (for tests that drive the editor with synthetic mouse events). */
    kickeq::ui::EqCanvas& getCanvas() noexcept { return canvas; }
    kickeq::ui::BandPanel& getBandPanel() noexcept { return bandPanel; }

private:
    void timerCallback() override { refreshFrame(); }
    void settingsFromControls();
    void syncControlsFromProcessor();

    KickEQProcessor& kickProcessor;
    kickeq::ui::KickLookAndFeel lookAndFeel; // must outlive the child components

    kickeq::ui::EqCanvas canvas;
    kickeq::ui::BandPanel bandPanel;

    juce::ComboBox presetBox;
    juce::ToggleButton bypassButton { "Bypass" }, autoGainButton { "Auto Gain" };
    juce::Slider outputSlider;
    juce::ComboBox sourceBox, rangeBox, slopeBox, curveRangeBox, viewBox;
    juce::ComboBox phaseBox; // phase_mode: Minimum / Linear 2048 / 4096 / 8192 (hidden when the core has no linear phase)

    std::unique_ptr<juce::ButtonParameterAttachment> bypassAttachment, autoGainAttachment;
    std::unique_ptr<juce::ComboBoxParameterAttachment> phaseAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> outputAttachment;

    kickeq::SpectrumFrame frame;
    kickeq::ui::EditorSettings shownSettings;
    int shownPreset = -2;
    int shownLatencySamples = -1;
    double shownRate = 0.0;
    bool updatingControls = false;

    juce::Rectangle<int> headerArea, toolbarArea;
    juce::Rectangle<float> outputCaption, analyserCaption, slopeCaption, curveCaption, viewCaption, latencyCaption;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickEQEditor)
};
