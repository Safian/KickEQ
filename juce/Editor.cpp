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

#include "Editor.h"

#ifndef KICKEQ_VERSION_STRING
#define KICKEQ_VERSION_STRING "0"
#endif

using namespace kickeq::ui;

KickEQEditor::KickEQEditor (KickEQProcessor& p)
    : juce::AudioProcessorEditor (p), kickProcessor (p), canvas (p), bandPanel (p)
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);
    setTitle ("KickEQ");

    // ---- preset menu (core factory presets) -----------------------------------------------------------------
    for (int i = 0; i < kickeq::numPresets; ++i)
        presetBox.addItem (kickeq::factoryPresets[i].name, i + 1);
    presetBox.setTextWhenNothingSelected ("Custom");
    presetBox.setTitle ("Preset");
    presetBox.setScrollWheelEnabled (false);
    presetBox.onChange = [this] {
        if (updatingControls)
            return;
        const int id = presetBox.getSelectedId();
        if (id > 0)
            kickProcessor.setCurrentProgram (id - 1);
    };
    addAndMakeVisible (presetBox);

    // ---- global parameters --------------------------------------------------------------------------------------
    bypassButton.setTitle ("Bypass");
    bypassButton.setColour (juce::ToggleButton::tickColourId, colours::bypassOn);
    autoGainButton.setTitle ("Auto gain");
    autoGainButton.setColour (juce::ToggleButton::tickColourId, colours::autoGainOn);
    addAndMakeVisible (bypassButton);
    addAndMakeVisible (autoGainButton);
    bypassAttachment = std::make_unique<juce::ButtonParameterAttachment> (p.getCoreParameter (kickeq::paramBypass), bypassButton);
    autoGainAttachment = std::make_unique<juce::ButtonParameterAttachment> (p.getCoreParameter (kickeq::paramAutoGain), autoGainButton);

    outputSlider.setSliderStyle (juce::Slider::LinearBar);
    outputSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    outputSlider.setTitle ("Output gain");
    outputSlider.setScrollWheelEnabled (false);
    outputSlider.setColour (juce::Slider::trackColourId, colours::accent);
    addAndMakeVisible (outputSlider);
    outputAttachment = std::make_unique<juce::SliderParameterAttachment> (p.getCoreParameter (kickeq::paramOutGain), outputSlider);

    // ---- display controls (editor state, saved in the UI01 chunk) -----------------------------------------
    const auto fill = [this] (juce::ComboBox& box, const juce::String& title, std::initializer_list<juce::String> items) {
        int id = 1;
        for (const auto& item : items)
            box.addItem (item, id++);
        box.setTitle (title);
        box.setScrollWheelEnabled (false);
        box.onChange = [this] { settingsFromControls(); };
        addAndMakeVisible (box);
    };
    fill (sourceBox, "Analyser source", { "L + R", "Mid", "Side" });
    fill (rangeBox, "Analyser range", { "-60 dB", "-90 dB", "-120 dB" });
    fill (slopeBox, "Analyser slope compensation", { "0 dB/oct", "3 dB/oct", "4.5 dB/oct" });
    const juce::String pm = juce::String (juce::CharPointer_UTF8 ("\xc2\xb1"));
    fill (curveRangeBox, "Curve range", { pm + "6 dB", pm + "12 dB", pm + "24 dB", pm + "30 dB" });
    fill (viewBox, "Curve channel view", { "Stereo", "Left", "Right", "Mid", "Side" });
    viewBox.setSelectedId (1, juce::dontSendNotification);

    viewBox.onChange = [this] {
        canvas.setView (static_cast<kickeq::ChannelView> (juce::jmax (0, viewBox.getSelectedId() - 1)));
    };

    // ---- canvas and band strip ------------------------------------------------------------------------------------
    addAndMakeVisible (canvas);
    addAndMakeVisible (bandPanel);
    canvas.onSelectedBandChanged = [this] (int band) { bandPanel.setSelectedBand (band); };
    bandPanel.onBandChosen = [this] (int band) { canvas.setSelectedBand (band); };

    syncControlsFromProcessor();

    // the analyser only runs while an editor is open
    kickProcessor.acquireAnalyzer();

    setResizable (true, true);
    setResizeLimits (minWidth, minHeight, 3840, 2160);
    setSize (defaultWidth, defaultHeight);

    startTimerHz (30);
}

KickEQEditor::~KickEQEditor()
{
    stopTimer();
    kickProcessor.releaseAnalyzer();
    setLookAndFeel (nullptr);
}

void KickEQEditor::previewBand (int band, kickeq::ChannelView view)
{
    canvas.setSelectedBand (band);
    canvas.setHoverBand (band);
    viewBox.setSelectedId (static_cast<int> (view) + 1, juce::sendNotificationSync);
}

void KickEQEditor::syncControlsFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (updatingControls, true);

    const EditorSettings s = kickProcessor.getEditorSettings();
    if (s != shownSettings || shownPreset == -2)
    {
        shownSettings = s;
        sourceBox.setSelectedId (s.analyzerSource + 1, juce::dontSendNotification);
        rangeBox.setSelectedId (s.analyzerRange + 1, juce::dontSendNotification);
        slopeBox.setSelectedId (s.slopeComp + 1, juce::dontSendNotification);
        curveRangeBox.setSelectedId (s.curveRange + 1, juce::dontSendNotification);
        canvas.setSettings (s);
    }

    const int preset = kickProcessor.findMatchingPreset();
    if (preset != shownPreset)
    {
        shownPreset = preset;
        presetBox.setSelectedId (preset >= 0 ? preset + 1 : 0, juce::dontSendNotification);
    }
}

void KickEQEditor::settingsFromControls()
{
    if (updatingControls)
        return;
    EditorSettings s;
    s.analyzerSource = juce::jmax (0, sourceBox.getSelectedId() - 1);
    s.analyzerRange = juce::jmax (0, rangeBox.getSelectedId() - 1);
    s.slopeComp = juce::jmax (0, slopeBox.getSelectedId() - 1);
    s.curveRange = juce::jmax (0, curveRangeBox.getSelectedId() - 1);
    kickProcessor.setEditorSettings (s);
    shownSettings = s;
    canvas.setSettings (s);
}

void KickEQEditor::refreshFrame()
{
    auto& analyzer = kickProcessor.getAnalyzer();
    analyzer.process();
    const bool newFrame = analyzer.pull (frame);
    if (newFrame)
        canvas.setSpectrum (frame);

    const bool curveChanged = canvas.refreshCurve();
    if (curveChanged)
        bandPanel.updateState();
    syncControlsFromProcessor();

    if (newFrame || curveChanged)
        canvas.repaint();
}

void KickEQEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);

    // header
    g.setColour (colours::panel);
    g.fillRect (headerArea);
    g.setColour (colours::outline);
    g.drawHorizontalLine (headerArea.getBottom() - 1, 0.0f, static_cast<float> (getWidth()));

    g.setColour (colours::text);
    g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    g.drawText ("Kick", juce::Rectangle<int> (14, headerArea.getY(), 44, headerArea.getHeight()), juce::Justification::centredLeft, false);
    g.setColour (colours::accent);
    g.drawText ("EQ", juce::Rectangle<int> (52, headerArea.getY(), 30, headerArea.getHeight()), juce::Justification::centredLeft, false);
    g.setColour (colours::textFaint);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText ("v" KICKEQ_VERSION_STRING, juce::Rectangle<int> (84, headerArea.getY() + 4, 56, headerArea.getHeight() - 8),
                juce::Justification::centredLeft, false);

    g.setColour (colours::textDim);
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    g.drawText ("Output", outputCaption, juce::Justification::centredRight, false);

    // toolbar
    g.setColour (colours::background);
    g.fillRect (toolbarArea);
    g.setColour (colours::textDim);
    g.drawText ("Analyser", analyserCaption, juce::Justification::centredRight, false);
    g.drawText ("Slope", slopeCaption, juce::Justification::centredRight, false);
    g.drawText ("Curve", curveCaption, juce::Justification::centredRight, false);
    g.drawText ("View", viewCaption, juce::Justification::centredRight, false);
}

void KickEQEditor::resized()
{
    auto area = getLocalBounds();
    headerArea = area.removeFromTop (38);
    auto stripArea = area.removeFromBottom (82);
    toolbarArea = area.removeFromTop (30);

    bandPanel.setBounds (stripArea);
    canvas.setBounds (area.reduced (8, 0).withTrimmedBottom (4));

    // header, right to left: output gain, auto gain, bypass, preset
    auto h = headerArea.reduced (10, 7);
    outputSlider.setBounds (h.removeFromRight (150));
    h.removeFromRight (6);
    outputCaption = h.removeFromRight (46).toFloat();
    h.removeFromRight (14);
    autoGainButton.setBounds (h.removeFromRight (96));
    h.removeFromRight (6);
    bypassButton.setBounds (h.removeFromRight (82));
    h.removeFromRight (14);
    presetBox.setBounds (h.removeFromRight (juce::jmin (210, juce::jmax (120, h.getWidth() - 160))));

    // toolbar: analyser controls on the left, curve controls on the right
    auto t = toolbarArea.reduced (10, 3);
    analyserCaption = t.removeFromLeft (52).toFloat();
    t.removeFromLeft (6);
    sourceBox.setBounds (t.removeFromLeft (76));
    t.removeFromLeft (6);
    rangeBox.setBounds (t.removeFromLeft (84));
    t.removeFromLeft (6);
    slopeCaption = t.removeFromLeft (38).toFloat();
    t.removeFromLeft (6);
    slopeBox.setBounds (t.removeFromLeft (94));

    viewBox.setBounds (t.removeFromRight (84));
    t.removeFromRight (6);
    viewCaption = t.removeFromRight (36).toFloat();
    t.removeFromRight (10);
    curveRangeBox.setBounds (t.removeFromRight (84));
    t.removeFromRight (6);
    curveCaption = t.removeFromRight (38).toFloat();
}
