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
#include <vector>

#include "EditorSettings.h"
#include "PluginProcessor.h"
#include "kickeq/analyzer.h"
#include "kickeq/eq_curve.h"

namespace kickeq::ui
{
/** The main canvas: live spectrum (pre grey, post line + fill), the EQ response curve with a translucent fill per
    band, and one draggable node per enabled band. All parameter edits go through juce::ParameterAttachment so
    the host sees proper begin / end change gestures. */
class EqCanvas final : public juce::Component
{
public:
    explicit EqCanvas (KickEQProcessor&);
    ~EqCanvas() override;

    /** Channel the curve describes (Stereo / L / R / M / S). */
    void setView (kickeq::ChannelView newView);
    kickeq::ChannelView getView() const noexcept { return view; }

    /** Analyser range / slope compensation / curve range changed. */
    void setSettings (const EditorSettings& newSettings);

    /** Hands over a new analyser frame (message thread); rebuilds the analyser paths. */
    void setSpectrum (const kickeq::SpectrumFrame& frame);

    /** Re-reads the parameters and rebuilds the curve paths when they changed. Returns true if anything changed. */
    bool refreshCurve();

    int getSelectedBand() const noexcept { return selectedBand; }
    void setSelectedBand (int band);
    std::function<void (int)> onSelectedBandChanged;
    /** Highlights a band as if the mouse hovered its node (-1: none). */
    void setHoverBand (int band)
    {
        hoverBand = band;
        repaint();
    }
    int getHoverBand() const noexcept { return hoverBand; }
    /** Band whose node is being dragged (-1: none). */
    int getDragBand() const noexcept { return dragBand; }

    /** The plot rectangle (canvas coordinates) and the centre of a band's node; the node is drawn, and hit, here. */
    juce::Rectangle<float> getPlotBounds() const noexcept { return plot; }
    juce::Point<float> getNodeCentre (int band) const noexcept { return nodePosition (band); }

    // ---- right-click menu ------------------------------------------------------------------------------------------
    // Item ids: 1 = disable the band, 100 + i = type i, 200 + i = slope index i, 300 + i = placement i; 0 = dismissed.
    static constexpr int menuDisableBand = 1, menuTypeBase = 100, menuSlopeBase = 200, menuPlacementBase = 300;

    /** The menu of a band (type / slope / placement submenus with the current choice ticked, "Disable band"). */
    juce::PopupMenu buildBandMenu (int band) const;
    /** Applies an id returned by the menu to a band's parameters (one complete automation gesture). */
    void applyMenuChoice (int band, int result);
    /** Shows a band menu and reports the chosen id (0 = dismissed). Empty: the default, an asynchronous popup menu
        at the mouse position. Tests set it to present the menu without a window. */
    std::function<void (juce::PopupMenu, std::function<void (int)>)> menuPresenter;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    juce::MouseCursor getMouseCursor() override;

private:
    enum class DragMode { none, move, q };

    // geometry
    float freqToX (float hz) const noexcept;
    float xToFreq (float x) const noexcept;
    float curveDbToY (float db) const noexcept;
    float yToCurveDb (float y) const noexcept;
    float analyzerDbToY (float db) const noexcept;
    juce::Point<float> nodePosition (int band) const noexcept;
    int bandAt (juce::Point<float> p) const noexcept;
    bool bandAffectsView (int band) const noexcept;

    // caches
    void updateGeometry();
    void rebuildCurve();
    void rebuildBinMapping();
    void rebuildAnalyzerPaths();
    void renderGrid (juce::Graphics&) const;
    void drawNode (juce::Graphics&, int band) const;
    void drawReadout (juce::Graphics&, int band) const;
    void drawHoverRange (juce::Graphics&, int band) const;

    // editing
    juce::ParameterAttachment& attachment (int band, kickeq::BandParam k);
    void setBandValue (int band, kickeq::BandParam k, float plain);
    void beginDrag (int band, const juce::MouseEvent&);
    void endDrag();
    void addBandAt (juce::Point<float> p);
    void disableBand (int band);
    void showBandMenu (int band);

    KickEQProcessor& processor;
    std::array<std::unique_ptr<juce::ParameterAttachment>, static_cast<size_t> (kickeq::numParams)> attachments;

    // state shown
    kickeq::ParamValues values, shownValues;
    bool haveValues = false;
    double shownRate = 0.0;
    kickeq::ChannelView view = kickeq::ChannelView::stereo;
    EditorSettings settings;
    int selectedBand = 0;
    int hoverBand = -1;

    // geometry
    juce::Rectangle<float> plot;
    float logSpan = 1.0f;
    float curveRange = 12.0f, analyzerRange = 90.0f, slopeComp = 3.0f;

    // grid cache
    juce::Image gridImage;
    float gridScale = 0.0f;
    juce::Rectangle<float> gridPlot;
    float gridCurveRange = 0.0f, gridAnalyzerRange = 0.0f;

    // curve
    std::vector<float> curveFreqs, totalDb;
    std::array<std::vector<float>, static_cast<size_t> (kickeq::numBands)> bandDb;
    juce::Path totalPath;
    std::array<juce::Path, static_cast<size_t> (kickeq::numBands)> bandFill;

    // analyser
    kickeq::SpectrumFrame frame;
    bool haveFrame = false;
    std::vector<float> binX, binTilt;
    int binsUsed = 0;
    juce::Path preFill, postFill, postLine, preLine; // preLine: scratch for building preFill

    // interaction
    DragMode dragMode = DragMode::none;
    int dragBand = -1;
    juce::Point<float> grabOffset;
    float dragStartQ = 1.0f;
    bool dragHasGain = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCanvas)
};
} // namespace kickeq::ui
