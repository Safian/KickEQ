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

// Colours and look and feel of the editor (dark theme).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace kickeq::ui
{
namespace colours
{
inline const juce::Colour background   { 0xff13151a };
inline const juce::Colour panel        { 0xff1a1d24 };
inline const juce::Colour panelRaised  { 0xff232731 };
inline const juce::Colour outline      { 0xff303642 };
inline const juce::Colour plotBack     { 0xff0f1115 };
inline const juce::Colour gridMinor    { 0xff1b1f27 };
inline const juce::Colour gridMajor    { 0xff2a303b };
inline const juce::Colour zeroLine     { 0xff4a5262 };
inline const juce::Colour text         { 0xffe2e6ee };
inline const juce::Colour textDim      { 0xff8d96a6 };
inline const juce::Colour textFaint    { 0xff5d6676 };
inline const juce::Colour accent       { 0xff4cc2ff };
inline const juce::Colour analyserPost { 0xff4cc2ff };
inline const juce::Colour analyserPre  { 0xff8a93a3 };
inline const juce::Colour curve        { 0xfff2f4f8 };
inline const juce::Colour bypassOn     { 0xffffa63d };
inline const juce::Colour autoGainOn   { 0xff5be3a1 };
} // namespace colours

/** Eight clearly distinguishable band colours (also usable as chip / slider accents). */
inline juce::Colour bandColour (int band)
{
    static const juce::Colour table[8] = { juce::Colour (0xffff6b6b), juce::Colour (0xffff9f43), juce::Colour (0xffffd24a),
                                           juce::Colour (0xff4ddf9a), juce::Colour (0xff3dd5e8), juce::Colour (0xff5b9bff),
                                           juce::Colour (0xffa876ff), juce::Colour (0xfff26fd0) };
    return table[band & 7];
}

class KickLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    KickLookAndFeel();

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getLabelFont (juce::Label&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;
};
} // namespace kickeq::ui
