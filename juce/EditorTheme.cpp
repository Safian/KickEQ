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

#include "EditorTheme.h"

#include <cmath>

namespace kickeq::ui
{
KickLookAndFeel::KickLookAndFeel()
{
    using C = juce::LookAndFeel_V4::ColourScheme::UIColour;
    auto scheme = getDarkColourScheme();
    scheme.setUIColour (C::windowBackground, colours::background);
    scheme.setUIColour (C::widgetBackground, colours::panelRaised);
    scheme.setUIColour (C::menuBackground, colours::panelRaised);
    scheme.setUIColour (C::outline, colours::outline);
    scheme.setUIColour (C::defaultText, colours::text);
    scheme.setUIColour (C::defaultFill, colours::accent);
    scheme.setUIColour (C::highlightedText, juce::Colours::black);
    scheme.setUIColour (C::highlightedFill, colours::accent);
    scheme.setUIColour (C::menuText, colours::text);
    setColourScheme (scheme);

    setColour (juce::Slider::textBoxTextColourId, colours::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::backgroundColourId, colours::panelRaised);
    setColour (juce::ComboBox::outlineColourId, colours::outline);
    setColour (juce::ComboBox::textColourId, colours::text);
    setColour (juce::ComboBox::arrowColourId, colours::textDim);
    setColour (juce::PopupMenu::backgroundColourId, colours::panelRaised);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colours::accent.withAlpha (0.85f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    setColour (juce::TextEditor::backgroundColourId, colours::plotBack);
    setColour (juce::TextEditor::textColourId, colours::text);
    setColour (juce::TextEditor::outlineColourId, colours::accent);
    setColour (juce::TextEditor::focusedOutlineColourId, colours::accent);
    setColour (juce::Label::textColourId, colours::text);
}

juce::Font KickLookAndFeel::getComboBoxFont (juce::ComboBox&) { return juce::Font (juce::FontOptions (12.5f)); }
juce::Font KickLookAndFeel::getPopupMenuFont() { return juce::Font (juce::FontOptions (13.0f)); }
juce::Font KickLookAndFeel::getLabelFont (juce::Label& l)
{
    // the value box of a slider is a Label child: use the same size as the combo boxes
    if (dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr)
        return juce::Font (juce::FontOptions (12.5f));
    return LookAndFeel_V4::getLabelFont (l);
}

void KickLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                        juce::Slider& slider)
{
    if (style != juce::Slider::LinearBar)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y), static_cast<float> (width),
                                                static_cast<float> (height))
                            .reduced (0.5f);
    const auto accent = slider.findColour (juce::Slider::trackColourId);
    const bool enabled = slider.isEnabled();

    g.setColour (colours::plotBack);
    g.fillRoundedRectangle (bounds, 4.0f);

    // fill from the left edge to the value; bipolar ranges (gain in dB) fill from the zero position
    const float valuePos = juce::jlimit (bounds.getX(), bounds.getRight(), sliderPos);
    float originPos = bounds.getX();
    if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
        originPos = juce::jlimit (bounds.getX(), bounds.getRight(),
                                  bounds.getX() + bounds.getWidth() * static_cast<float> (slider.valueToProportionOfLength (0.0)));
    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (bounds, 4.0f);
        g.reduceClipRegion (clip);
        g.setColour (accent.withAlpha (enabled ? 0.42f : 0.14f));
        g.fillRect (juce::Rectangle<float> (juce::jmin (originPos, valuePos), bounds.getY(),
                                            std::abs (valuePos - originPos), bounds.getHeight()));
        g.setColour (accent.withAlpha (enabled ? 0.95f : 0.3f));
        g.fillRect (juce::Rectangle<float> (valuePos - 0.75f, bounds.getY(), 1.5f, bounds.getHeight()));
        if (originPos > bounds.getX() + 1.0f)
        {
            g.setColour (colours::textFaint);
            // zero marker: two short ticks, so it does not cut through the value text
            g.fillRect (juce::Rectangle<float> (originPos - 0.5f, bounds.getY() + 1.0f, 1.0f, 4.0f));
            g.fillRect (juce::Rectangle<float> (originPos - 0.5f, bounds.getBottom() - 5.0f, 1.0f, 4.0f));
        }
    }

    g.setColour (colours::outline);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
}

juce::Label* KickLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (juce::Font (juce::FontOptions (12.5f)));
    return label;
}

void KickLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown, int, int, int, int,
                                    juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)).reduced (0.5f);
    const bool enabled = box.isEnabled();
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (isButtonDown ? 0.15f : 0.0f));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    // chevron
    const float cx = static_cast<float> (width) - 11.0f;
    const float cy = static_cast<float> (height) * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.5f, cy - 1.5f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 3.5f, cy - 1.5f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (enabled ? 1.0f : 0.35f));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void KickLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (7, 1, box.getWidth() - 22, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void KickLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();
    const bool enabled = button.isEnabled();
    const auto tint = button.findColour (juce::ToggleButton::tickColourId);

    auto fill = on ? tint.withAlpha (0.2f) : colours::panelRaised;
    if (highlighted || down)
        fill = fill.brighter (0.08f);
    g.setColour (fill);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (on ? tint.withAlpha (enabled ? 0.9f : 0.4f) : colours::outline);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    // LED
    const float ledSize = 6.0f;
    const auto led = juce::Rectangle<float> (bounds.getX() + 8.0f, bounds.getCentreY() - ledSize * 0.5f, ledSize, ledSize);
    g.setColour (on ? tint : colours::textFaint);
    g.fillEllipse (led);

    g.setColour ((on ? colours::text : colours::textDim).withAlpha (enabled ? 1.0f : 0.4f));
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText (button.getButtonText(), bounds.withTrimmedLeft (20.0f).withTrimmedRight (4.0f), juce::Justification::centredLeft);
}
} // namespace kickeq::ui
