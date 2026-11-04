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

#include "EditorStrip.h"

#include "EditorTheme.h"

namespace kickeq::ui
{
/** Round button with the band number, filled with the band colour while the band is on. */
class BandPanel::Chip final : public juce::Button
{
public:
    Chip (KickEQProcessor& p, int bandIndex) : juce::Button ("Band " + juce::String (bandIndex + 1)), processor (p), band (bandIndex)
    {
        setTitle ("Band " + juce::String (bandIndex + 1));
        setClickingTogglesState (false);
    }

    void setSelectedFlag (bool s)
    {
        if (isSelected != s)
        {
            isSelected = s;
            repaint();
        }
    }

    void updateOn()
    {
        const bool on = processor.getCoreParameter (band, kickeq::BandParam::on).getPlain() >= 0.5f;
        if (on != isOn)
        {
            isOn = on;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.5f);
        const juce::Colour c = bandColour (band);
        if (isSelected)
        {
            g.setColour (c.withAlpha (0.25f));
            g.fillRoundedRectangle (r.expanded (1.0f), 6.0f);
        }
        if (isOn)
        {
            g.setColour (c.withAlpha (highlighted || down ? 1.0f : 0.92f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (juce::Colours::black.withAlpha (0.8f));
        }
        else
        {
            g.setColour (colours::panelRaised.brighter (highlighted ? 0.1f : 0.0f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (c.withAlpha (isSelected ? 0.95f : 0.45f));
            g.drawRoundedRectangle (r, 5.0f, 1.2f);
        }
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText (juce::String (band + 1), getLocalBounds(), juce::Justification::centred, false);
        if (isSelected)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawRoundedRectangle (r.expanded (1.0f), 6.0f, 1.2f);
        }
    }

private:
    KickEQProcessor& processor;
    int band;
    bool isSelected = false, isOn = false;
};

BandPanel::BandPanel (KickEQProcessor& p) : processor (p)
{
    setTitle ("Band controls");

    for (int b = 0; b < kickeq::numBands; ++b)
    {
        auto chip = std::make_unique<Chip> (p, b);
        chip->onClick = [this, b] {
            setSelectedBand (b);
            if (onBandChosen)
                onBandChosen (b);
        };
        addAndMakeVisible (*chip);
        chips[static_cast<size_t> (b)] = std::move (chip);
    }

    const auto setupCombo = [this] (juce::ComboBox& box, const juce::String& title, const char* const* labels, int count) {
        for (int i = 0; i < count; ++i)
            box.addItem (labels[i], i + 1);
        box.setTitle (title);
        box.setScrollWheelEnabled (false);
        addAndMakeVisible (box);
    };
    setupCombo (typeBox, "Band type", kickeq::bandTypeLabels, kickeq::numBandTypes);
    setupCombo (slopeBox, "Band slope", kickeq::slopeLabels, kickeq::numSlopes);
    setupCombo (placeBox, "Band placement", kickeq::placementLabels, kickeq::numPlacements);

    const auto setupSlider = [this] (juce::Slider& s, const juce::String& title) {
        s.setSliderStyle (juce::Slider::LinearBar);
        s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
        s.setTitle (title);
        s.setScrollWheelEnabled (false);
        addAndMakeVisible (s);
    };
    setupSlider (freqSlider, "Band frequency");
    setupSlider (gainSlider, "Band gain");
    setupSlider (qSlider, "Band Q");

    onButton.setTitle ("Band enabled");
    onButton.setColour (juce::ToggleButton::tickColourId, colours::accent);
    addAndMakeVisible (onButton);

    attachControls();
    setSelectedBand (0);
}

BandPanel::~BandPanel() = default;

void BandPanel::attachControls()
{
    // drop the old attachments first: they reference the controls and the previous band's parameters
    onAttachment.reset();
    typeAttachment.reset();
    slopeAttachment.reset();
    placeAttachment.reset();
    freqAttachment.reset();
    gainAttachment.reset();
    qAttachment.reset();

    using K = kickeq::BandParam;
    onAttachment = std::make_unique<juce::ButtonParameterAttachment> (processor.getCoreParameter (selected, K::on), onButton);
    typeAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (processor.getCoreParameter (selected, K::type), typeBox);
    slopeAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (processor.getCoreParameter (selected, K::slope), slopeBox);
    placeAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (processor.getCoreParameter (selected, K::place), placeBox);
    freqAttachment = std::make_unique<juce::SliderParameterAttachment> (processor.getCoreParameter (selected, K::freq), freqSlider);
    gainAttachment = std::make_unique<juce::SliderParameterAttachment> (processor.getCoreParameter (selected, K::gain), gainSlider);
    qAttachment = std::make_unique<juce::SliderParameterAttachment> (processor.getCoreParameter (selected, K::q), qSlider);

    const juce::Colour c = bandColour (selected);
    for (juce::Slider* s : { &freqSlider, &gainSlider, &qSlider })
        s->setColour (juce::Slider::trackColourId, c);
    onButton.setColour (juce::ToggleButton::tickColourId, c);
}

void BandPanel::setSelectedBand (int band)
{
    band = juce::jlimit (0, kickeq::numBands - 1, band);
    if (band != selected)
    {
        selected = band;
        attachControls();
    }
    for (int b = 0; b < kickeq::numBands; ++b)
        chips[static_cast<size_t> (b)]->setSelectedFlag (b == selected);
    updateState();
    repaint();
}

void BandPanel::updateState()
{
    using K = kickeq::BandParam;
    for (auto& chip : chips)
        chip->updateOn();

    const auto type = static_cast<kickeq::BandType> (static_cast<int> (processor.getCoreParameter (selected, K::type).getPlain()));
    const int slope = static_cast<int> (processor.getCoreParameter (selected, K::slope).getPlain());
    const bool cut = type == kickeq::BandType::lowCut || type == kickeq::BandType::highCut;
    const bool hasGain = type == kickeq::BandType::bell || type == kickeq::BandType::lowShelf || type == kickeq::BandType::highShelf;
    const bool qMatters = !cut || slope == 1; // cuts: Q only shapes the 12 dB/oct section

    gainSlider.setEnabled (hasGain);
    slopeBox.setEnabled (cut);
    qSlider.setEnabled (qMatters);
}

void BandPanel::paint (juce::Graphics& g)
{
    g.setColour (colours::panel);
    g.fillRect (getLocalBounds());
    g.setColour (colours::outline);
    g.drawHorizontalLine (0, 0.0f, static_cast<float> (getWidth()));

    static const char* captions[7] = { "", "Type", "Frequency", "Gain", "Q", "Slope", "Placement" };
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    for (size_t i = 0; i < captionBounds.size(); ++i)
    {
        g.setColour (colours::textDim);
        g.drawText (captions[i], captionBounds[i], juce::Justification::bottomLeft, false);
    }

    // hint line next to the chips (only when there is room)
    static const juce::String hint ("Drag: frequency / gain     Wheel or Shift-drag: Q     Double-click: add / remove     Right-click: menu");
    const juce::Font hintFont (juce::FontOptions (11.0f));
    const int hintX = chips[7]->getRight() + 16;
    if (static_cast<float> (getWidth() - hintX - 10) > juce::GlyphArrangement::getStringWidth (hintFont, hint))
    {
        g.setColour (colours::textFaint);
        g.setFont (hintFont);
        g.drawText (hint, juce::Rectangle<int> (hintX, 6, getWidth() - hintX - 10, 24), juce::Justification::centredRight, false);
    }
}

void BandPanel::resized()
{
    auto area = getLocalBounds().reduced (10, 6);

    auto chipRow = area.removeFromTop (26);
    for (auto& chip : chips)
    {
        chip->setBounds (chipRow.removeFromLeft (30));
        chipRow.removeFromLeft (4);
    }

    area.removeFromTop (4);
    auto captionRow = area.removeFromTop (14);
    auto controlRow = area.removeFromTop (24);

    // relative widths of the seven columns
    static const float weights[7] = { 0.075f, 0.165f, 0.175f, 0.145f, 0.125f, 0.16f, 0.155f };
    const int gap = 6;
    const int total = controlRow.getWidth() - gap * 6;
    int x = controlRow.getX();
    juce::Component* controls[7] = { &onButton, &typeBox, &freqSlider, &gainSlider, &qSlider, &slopeBox, &placeBox };
    for (size_t i = 0; i < 7; ++i)
    {
        const int w = i == 6 ? controlRow.getRight() - x : juce::roundToInt (weights[i] * static_cast<float> (total));
        controls[i]->setBounds (x, controlRow.getY(), w, controlRow.getHeight());
        captionBounds[i] = juce::Rectangle<int> (x + 1, captionRow.getY(), w, captionRow.getHeight());
        x += w + gap;
    }
}
} // namespace kickeq::ui
