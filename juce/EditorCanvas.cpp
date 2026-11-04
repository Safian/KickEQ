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

#include "EditorCanvas.h"

#include <cmath>

#include "EditorTheme.h"

namespace kickeq::ui
{
namespace
{
constexpr float minHz = 10.0f;
constexpr float maxHz = 30000.0f;
constexpr float marginLeft = 40.0f, marginRight = 40.0f, marginTop = 10.0f, marginBottom = 22.0f;
constexpr float nodeRadius = 8.5f;
constexpr float hitRadius = 12.0f;

bool typeHasGain (kickeq::BandType t) noexcept
{
    return t == kickeq::BandType::bell || t == kickeq::BandType::lowShelf || t == kickeq::BandType::highShelf;
}

bool typeIsCut (kickeq::BandType t) noexcept
{
    return t == kickeq::BandType::lowCut || t == kickeq::BandType::highCut;
}

juce::String paramText (int tableIndex, float plain)
{
    char buffer[48];
    kickeq::valueToText (kickeq::paramInfo (tableIndex), plain, buffer, sizeof buffer);
    return juce::String (juce::CharPointer_UTF8 (buffer));
}

juce::String freqLabel (float hz)
{
    if (hz >= 1000.0f)
        return juce::String (hz / 1000.0f, 0) + "k";
    return juce::String (static_cast<int> (hz));
}
} // namespace

EqCanvas::EqCanvas (KickEQProcessor& p) : processor (p)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setTitle ("Equaliser display");
    setDescription ("Spectrum analyser and equaliser curve. Drag a band node to change frequency and gain, "
                    "use the mouse wheel to change Q, double-click to add or remove a band.");

    for (int i = 0; i < kickeq::numParams; ++i)
        attachments[static_cast<size_t> (i)] =
            std::make_unique<juce::ParameterAttachment> (p.getCoreParameter (i), [this] (float) { repaint(); });

    settings = {};
    const EditorSettings s = p.getEditorSettings();
    setSettings (s);
}

EqCanvas::~EqCanvas()
{
    endDrag(); // never leave an open change gesture behind (the editor can be closed while the mouse is held)
}

// ---- geometry -------------------------------------------------------------------------------------------------

float EqCanvas::freqToX (float hz) const noexcept
{
    return plot.getX() + plot.getWidth() * std::log (juce::jmax (hz, 1.0e-3f) / minHz) / logSpan;
}

float EqCanvas::xToFreq (float x) const noexcept
{
    const float t = plot.getWidth() > 0.0f ? (x - plot.getX()) / plot.getWidth() : 0.0f;
    return minHz * std::exp (juce::jlimit (0.0f, 1.0f, t) * logSpan);
}

float EqCanvas::curveDbToY (float db) const noexcept
{
    return plot.getCentreY() - db / curveRange * plot.getHeight() * 0.5f;
}

float EqCanvas::yToCurveDb (float y) const noexcept
{
    return (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * curveRange;
}

float EqCanvas::analyzerDbToY (float db) const noexcept
{
    return plot.getBottom() - (db + analyzerRange) / analyzerRange * plot.getHeight();
}

juce::Point<float> EqCanvas::nodePosition (int band) const noexcept
{
    const kickeq::BandParams p = values.band (band);
    const float gain = typeHasGain (p.type) ? p.gainDb : 0.0f;
    return { freqToX (p.freqHz), juce::jlimit (plot.getY(), plot.getBottom(), curveDbToY (gain)) };
}

bool EqCanvas::bandAffectsView (int band) const noexcept
{
    using V = kickeq::ChannelView;
    using P = kickeq::Placement;
    const P place = values.band (band).place;
    if (place == P::stereo)
        return true;
    switch (view)
    {
        case V::stereo: return place == P::mid || place == P::left;
        case V::left:   return place != P::right;
        case V::right:  return place != P::left;
        case V::mid:    return place == P::mid;
        case V::side:   return place == P::side;
    }
    return true;
}

int EqCanvas::bandAt (juce::Point<float> p) const noexcept
{
    int best = -1;
    float bestDistance = hitRadius * hitRadius;
    for (int b = 0; b < kickeq::numBands; ++b)
    {
        if (!values.band (b).on)
            continue;
        const float d = nodePosition (b).getDistanceSquaredFrom (p);
        // the selected band wins ties so overlapping nodes stay reachable
        if (d < bestDistance || (b == selectedBand && d <= bestDistance && best >= 0))
        {
            bestDistance = d;
            best = b;
        }
    }
    return best;
}

void EqCanvas::updateGeometry()
{
    plot = getLocalBounds().toFloat();
    plot = juce::Rectangle<float> (marginLeft, marginTop, plot.getWidth() - marginLeft - marginRight,
                                   plot.getHeight() - marginTop - marginBottom);
    logSpan = std::log (maxHz / minHz);

    const int n = juce::jlimit (128, 1024, static_cast<int> (plot.getWidth() / 2.0f));
    curveFreqs.resize (static_cast<size_t> (n));
    totalDb.resize (static_cast<size_t> (n));
    for (auto& v : bandDb)
        v.resize (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        curveFreqs[static_cast<size_t> (i)] = minHz * std::exp (logSpan * static_cast<float> (i) / static_cast<float> (n - 1));

    haveValues = false; // force a curve rebuild
    rebuildBinMapping();
}

void EqCanvas::resized()
{
    updateGeometry();
    rebuildAnalyzerPaths();
}

void EqCanvas::setSettings (const EditorSettings& newSettings)
{
    settings = newSettings;
    curveRange = curveRangesDb[juce::jlimit (0, 3, settings.curveRange)];
    analyzerRange = analyzerRangesDb[juce::jlimit (0, 2, settings.analyzerRange)];
    slopeComp = slopeCompDbPerOct[juce::jlimit (0, 2, settings.slopeComp)];
    haveValues = false;
    rebuildBinMapping();
    rebuildAnalyzerPaths();
    repaint();
}

void EqCanvas::setView (kickeq::ChannelView newView)
{
    if (view == newView)
        return;
    view = newView;
    haveValues = false;
    repaint();
}

void EqCanvas::setSelectedBand (int band)
{
    band = juce::jlimit (0, kickeq::numBands - 1, band);
    if (band == selectedBand)
        return;
    selectedBand = band;
    if (onSelectedBandChanged)
        onSelectedBandChanged (band);
    repaint();
}

// ---- caches ----------------------------------------------------------------------------------------------------

void EqCanvas::rebuildBinMapping()
{
    const int n = haveFrame ? static_cast<int> (frame.postDb.size()) : processor.getAnalyzer().numBins();
    binX.resize (static_cast<size_t> (n));
    binTilt.resize (static_cast<size_t> (n));
    binsUsed = 0;
    const double nyquist = 0.5 * (haveFrame && frame.sampleRate > 0.0 ? frame.sampleRate : processor.getDisplaySampleRate());
    for (int i = 0; i < n; ++i)
    {
        const float f = kickeq::SpectrumAnalyzer::displayFrequency (i, n);
        binX[static_cast<size_t> (i)] = freqToX (f);
        binTilt[static_cast<size_t> (i)] = slopeComp * std::log2 (f / 1000.0f);
        if (static_cast<double> (f) <= nyquist)
            binsUsed = i + 1;
    }
}

void EqCanvas::setSpectrum (const kickeq::SpectrumFrame& f)
{
    const bool layoutChanged = !haveFrame || f.postDb.size() != frame.postDb.size() || !juce::exactlyEqual (f.sampleRate, frame.sampleRate);
    frame = f;
    haveFrame = !frame.postDb.empty() && frame.preDb.size() == frame.postDb.size();
    if (layoutChanged)
        rebuildBinMapping();
    rebuildAnalyzerPaths();
}

void EqCanvas::rebuildAnalyzerPaths()
{
    preFill.clear();
    postFill.clear();
    postLine.clear();
    if (!haveFrame || binsUsed < 2 || binX.size() != frame.postDb.size())
        return;

    const float bottom = plot.getBottom() + 1.0f;
    const auto build = [&] (const std::vector<float>& db, juce::Path& line, juce::Path* fill) {
        for (int i = 0; i < binsUsed; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            const float level = db[k] <= -150.0f ? -1.0e4f : db[k] + binTilt[k];
            const float y = juce::jlimit (plot.getY() - 4.0f, bottom, analyzerDbToY (level));
            if (i == 0)
                line.startNewSubPath (binX[k], y);
            else
                line.lineTo (binX[k], y);
        }
        if (fill != nullptr)
        {
            *fill = line;
            fill->lineTo (binX[static_cast<size_t> (binsUsed - 1)], bottom);
            fill->lineTo (binX[0], bottom);
            fill->closeSubPath();
        }
    };

    preLine.clear();
    build (frame.preDb, preLine, &preFill);
    build (frame.postDb, postLine, &postFill);
}

void EqCanvas::rebuildCurve()
{
    const double sr = processor.getDisplaySampleRate();
    const int n = static_cast<int> (curveFreqs.size());
    if (n < 2)
        return;

    kickeq::computeResponse (values, sr, curveFreqs.data(), totalDb.data(), nullptr, n, view);

    const float zeroY = curveDbToY (0.0f);
    totalPath.clear();
    for (int i = 0; i < n; ++i)
    {
        const float x = freqToX (curveFreqs[static_cast<size_t> (i)]);
        const float y = curveDbToY (juce::jlimit (-90.0f, 90.0f, totalDb[static_cast<size_t> (i)]));
        if (i == 0)
            totalPath.startNewSubPath (x, y);
        else
            totalPath.lineTo (x, y);
    }

    for (int b = 0; b < kickeq::numBands; ++b)
    {
        juce::Path& fill = bandFill[static_cast<size_t> (b)];
        fill.clear();
        const kickeq::BandParams bp = values.band (b);
        if (!bp.on)
            continue;
        std::vector<float>& db = bandDb[static_cast<size_t> (b)];
        kickeq::computeBandResponse (bp, sr, curveFreqs.data(), db.data(), nullptr, n);
        fill.startNewSubPath (freqToX (curveFreqs.front()), zeroY);
        for (int i = 0; i < n; ++i)
            fill.lineTo (freqToX (curveFreqs[static_cast<size_t> (i)]),
                         curveDbToY (juce::jlimit (-90.0f, 90.0f, db[static_cast<size_t> (i)])));
        fill.lineTo (freqToX (curveFreqs.back()), zeroY);
        fill.closeSubPath();
    }
}

bool EqCanvas::refreshCurve()
{
    processor.readValues (values);
    values.clamp();
    const double sr = processor.getDisplaySampleRate();
    if (haveValues && values == shownValues && juce::exactlyEqual (sr, shownRate))
        return false;
    rebuildCurve();
    shownValues = values;
    shownRate = sr;
    haveValues = true;
    return true;
}

void EqCanvas::renderGrid (juce::Graphics& g) const
{
    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());
    g.fillAll (colours::background);
    g.setColour (colours::plotBack);
    g.fillRoundedRectangle (plot.expanded (0.0f, 0.0f), 3.0f);

    const juce::Font small (juce::FontOptions (10.5f));
    g.setFont (small);

    // frequency grid
    static const float majors[] = { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float f = decade * static_cast<float> (m);
            if (f <= minHz || f >= maxHz)
                continue;
            const float x = std::round (freqToX (f)) + 0.5f;
            g.setColour (m == 1 ? colours::gridMajor : colours::gridMinor);
            g.drawVerticalLine (static_cast<int> (x), plot.getY(), plot.getBottom());
        }
    for (float f : majors)
    {
        const float x = freqToX (f);
        g.setColour (colours::textDim);
        g.drawText (freqLabel (f), juce::Rectangle<float> (x - 18.0f, plot.getBottom() + 4.0f, 36.0f, 14.0f),
                    juce::Justification::centred, false);
    }

    // curve (dB) grid and left labels
    const float step = curveRange <= 6.0f ? 2.0f : (curveRange <= 12.0f ? 3.0f : (curveRange <= 24.0f ? 6.0f : 10.0f));
    for (int k = -static_cast<int> (curveRange / step); k <= static_cast<int> (curveRange / step); ++k)
    {
        const float db = static_cast<float> (k) * step;
        const float y = std::round (curveDbToY (db)) + 0.5f;
        g.setColour (k == 0 ? colours::zeroLine : colours::gridMajor);
        g.drawHorizontalLine (static_cast<int> (y), plot.getX(), plot.getRight());
        if (std::abs (db) < curveRange - 0.01f)
        {
            g.setColour (k == 0 ? colours::text : colours::textDim);
            juce::String text = db > 0.0f ? "+" + juce::String (static_cast<int> (db)) : juce::String (static_cast<int> (db));
            g.drawText (text, juce::Rectangle<float> (0.0f, y - 7.0f, plot.getX() - 6.0f, 14.0f), juce::Justification::centredRight, false);
        }
    }

    // analyser (dBFS) labels on the right, with ticks
    const float aStep = analyzerRange <= 60.0f ? 10.0f : 20.0f;
    g.setColour (colours::analyserPost.withAlpha (0.75f));
    for (float db = 0.0f; db > -analyzerRange + 1.0f; db -= aStep)
    {
        const float y = analyzerDbToY (db);
        g.fillRect (plot.getRight(), std::round (y), 3.0f, 1.0f);
        g.drawText (juce::String (static_cast<int> (db)), juce::Rectangle<float> (plot.getRight() + 6.0f, y - 7.0f, marginRight - 8.0f, 14.0f),
                    juce::Justification::centredLeft, false);
    }

    g.setColour (colours::textFaint);
    g.drawText ("dB", juce::Rectangle<float> (0.0f, 0.0f, plot.getX() - 6.0f, marginTop + 2.0f), juce::Justification::centredRight, false);
    g.setColour (colours::analyserPost.withAlpha (0.5f));
    g.drawText ("dBFS", juce::Rectangle<float> (plot.getRight() + 4.0f, h - marginBottom, marginRight, marginBottom - 4.0f),
                juce::Justification::centredLeft, false);
    g.setColour (colours::textFaint);
    g.drawText ("Hz", juce::Rectangle<float> (0.0f, h - marginBottom, plot.getX() - 6.0f, marginBottom - 4.0f),
                juce::Justification::centredRight, false);

    g.setColour (colours::outline);
    g.drawRoundedRectangle (plot, 3.0f, 1.0f);
    juce::ignoreUnused (w);
}

// ---- painting --------------------------------------------------------------------------------------------------

void EqCanvas::drawHoverRange (juce::Graphics& g, int band) const
{
    const std::vector<float>& db = bandDb[static_cast<size_t> (band)];
    float peak = 0.0f;
    for (float v : db)
        peak = juce::jmax (peak, std::abs (v));
    const float threshold = juce::jlimit (0.25f, 3.0f, 0.5f * peak);
    int first = -1, last = -1;
    for (int i = 0; i < static_cast<int> (db.size()); ++i)
        if (std::abs (db[static_cast<size_t> (i)]) >= threshold)
        {
            if (first < 0)
                first = i;
            last = i;
        }
    if (first < 0)
        return;

    const float x1 = freqToX (curveFreqs[static_cast<size_t> (first)]);
    const float x2 = freqToX (curveFreqs[static_cast<size_t> (last)]);
    const juce::Colour c = bandColour (band);
    g.setColour (c.withAlpha (0.10f));
    g.fillRect (juce::Rectangle<float> (x1, plot.getY(), juce::jmax (1.0f, x2 - x1), plot.getHeight()));
    g.setColour (c.withAlpha (0.35f));
    if (first > 0)
        g.drawVerticalLine (static_cast<int> (x1), plot.getY(), plot.getBottom());
    if (last < static_cast<int> (db.size()) - 1)
        g.drawVerticalLine (static_cast<int> (x2), plot.getY(), plot.getBottom());
}

void EqCanvas::drawNode (juce::Graphics& g, int band) const
{
    static const juce::String labels[8] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    const juce::Point<float> p = nodePosition (band);
    const juce::Colour c = bandColour (band);
    const bool selected = band == selectedBand;
    const bool active = hoverBand == band || dragBand == band;
    const float r = nodeRadius + (active ? 1.5f : 0.0f);
    const auto circle = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p);

    if (selected)
    {
        g.setColour (c.withAlpha (0.28f));
        g.fillEllipse (circle.expanded (4.0f));
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawEllipse (circle.expanded (2.5f), 1.2f);
    }

    if (bandAffectsView (band))
    {
        g.setColour (c);
        g.fillEllipse (circle);
        g.setColour (colours::background.withAlpha (0.85f));
        g.drawEllipse (circle, 1.2f);
        g.setColour (juce::Colours::black.withAlpha (0.82f));
    }
    else
    {
        g.setColour (colours::plotBack);
        g.fillEllipse (circle);
        g.setColour (c.withAlpha (0.8f));
        g.drawEllipse (circle.reduced (0.6f), 1.5f);
        g.setColour (c);
    }
    g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    g.drawText (labels[band & 7], circle, juce::Justification::centred, false);
}

void EqCanvas::drawReadout (juce::Graphics& g, int band) const
{
    const kickeq::BandParams p = values.band (band);
    juce::String text = paramText (kickeq::bandParamIndex (band, kickeq::BandParam::freq), p.freqHz);
    if (typeHasGain (p.type))
        text << "   " << paramText (kickeq::bandParamIndex (band, kickeq::BandParam::gain), p.gainDb);
    text << "   Q " << paramText (kickeq::bandParamIndex (band, kickeq::BandParam::q), p.q);

    const juce::Font font (juce::FontOptions (11.5f));
    const float textWidth = juce::GlyphArrangement::getStringWidth (font, text);
    juce::Rectangle<float> box (textWidth + 14.0f, 20.0f);
    const juce::Point<float> n = nodePosition (band);
    box.setPosition (n.x + 14.0f, n.y - 30.0f);
    if (box.getRight() > plot.getRight() - 2.0f)
        box.setX (n.x - 14.0f - box.getWidth());
    if (box.getY() < plot.getY() + 2.0f)
        box.setY (n.y + 14.0f);
    box.setX (juce::jmax (plot.getX() + 2.0f, box.getX()));

    g.setColour (colours::panelRaised.withAlpha (0.94f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (bandColour (band).withAlpha (0.9f));
    g.drawRoundedRectangle (box, 4.0f, 1.0f);
    g.setColour (colours::text);
    g.setFont (font);
    g.drawText (text, box, juce::Justification::centred, false);
}

void EqCanvas::paint (juce::Graphics& g)
{
    refreshCurve();

    // static layer: grid and labels, cached per size, scale and range
    const float scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (!gridImage.isValid() || !juce::exactlyEqual (scale, gridScale) || plot != gridPlot
        || !juce::exactlyEqual (curveRange, gridCurveRange) || !juce::exactlyEqual (analyzerRange, gridAnalyzerRange))
    {
        gridImage = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt (static_cast<float> (getWidth()) * scale)),
                                 juce::jmax (1, juce::roundToInt (static_cast<float> (getHeight()) * scale)), true);
        juce::Graphics ig (gridImage);
        ig.addTransform (juce::AffineTransform::scale (scale));
        renderGrid (ig);
        gridScale = scale;
        gridPlot = plot;
        gridCurveRange = curveRange;
        gridAnalyzerRange = analyzerRange;
    }
    g.drawImage (gridImage, 0, 0, getWidth(), getHeight(), 0, 0, gridImage.getWidth(), gridImage.getHeight());

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.getSmallestIntegerContainer());

        // analyser
        if (haveFrame)
        {
            g.setColour (colours::analyserPre.withAlpha (0.20f));
            g.fillPath (preFill);
            g.setColour (colours::analyserPost.withAlpha (0.12f));
            g.fillPath (postFill);
            g.setColour (colours::analyserPost.withAlpha (0.85f));
            g.strokePath (postLine, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));
        }

        // hovered band: the frequency range it acts on
        if (hoverBand >= 0 && values.band (hoverBand).on)
            drawHoverRange (g, hoverBand);

        // per band fills
        for (int b = 0; b < kickeq::numBands; ++b)
        {
            if (!values.band (b).on)
                continue;
            const bool strong = b == selectedBand || b == hoverBand;
            const float alpha = (strong ? 0.26f : 0.13f) * (bandAffectsView (b) ? 1.0f : 0.45f);
            g.setColour (bandColour (b).withAlpha (alpha));
            g.fillPath (bandFill[static_cast<size_t> (b)]);
            if (strong)
            {
                g.setColour (bandColour (b).withAlpha (0.8f));
                g.strokePath (bandFill[static_cast<size_t> (b)], juce::PathStrokeType (1.0f));
            }
        }

        // total curve (dimmed while bypassed)
        const bool bypassed = values.bypass();
        g.setColour ((bypassed ? colours::textDim : colours::curve).withAlpha (0.16f));
        g.strokePath (totalPath, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour ((bypassed ? colours::textDim : colours::curve).withAlpha (bypassed ? 0.6f : 0.95f));
        g.strokePath (totalPath, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // nodes, selected one last (on top)
    for (int b = 0; b < kickeq::numBands; ++b)
        if (b != selectedBand && values.band (b).on)
            drawNode (g, b);
    if (values.band (selectedBand).on)
        drawNode (g, selectedBand);

    const int readoutBand = dragBand >= 0 ? dragBand : hoverBand;
    if (readoutBand >= 0 && values.band (readoutBand).on)
        drawReadout (g, readoutBand);
}

// ---- editing ---------------------------------------------------------------------------------------------------

juce::ParameterAttachment& EqCanvas::attachment (int band, kickeq::BandParam k)
{
    return *attachments[static_cast<size_t> (kickeq::bandParamIndex (band, k))];
}

void EqCanvas::setBandValue (int band, kickeq::BandParam k, float plain)
{
    attachment (band, k).setValueAsPartOfGesture (plain);
}

void EqCanvas::beginDrag (int band, const juce::MouseEvent& e)
{
    const kickeq::BandParams p = values.band (band);
    dragBand = band;
    dragMode = e.mods.isShiftDown() ? DragMode::q : DragMode::move;
    dragHasGain = typeHasGain (p.type);
    dragStartQ = p.q;
    // relative to the unclamped position: a gain beyond the curve range is drawn on the plot edge, but grabbing the node
    // must not make the value jump to the edge
    juce::Point<float> node = nodePosition (band);
    if (dragHasGain)
        node.y = curveDbToY (p.gainDb);
    grabOffset = e.position - node;

    if (dragMode == DragMode::move)
    {
        attachment (band, kickeq::BandParam::freq).beginGesture();
        if (dragHasGain)
            attachment (band, kickeq::BandParam::gain).beginGesture();
    }
    else
    {
        attachment (band, kickeq::BandParam::q).beginGesture();
    }
}

void EqCanvas::endDrag()
{
    if (dragBand < 0)
        return;
    if (dragMode == DragMode::move)
    {
        attachment (dragBand, kickeq::BandParam::freq).endGesture();
        if (dragHasGain)
            attachment (dragBand, kickeq::BandParam::gain).endGesture();
    }
    else if (dragMode == DragMode::q)
    {
        attachment (dragBand, kickeq::BandParam::q).endGesture();
    }
    dragBand = -1;
    dragMode = DragMode::none;
    repaint();
}

void EqCanvas::addBandAt (juce::Point<float> p)
{
    for (int b = 0; b < kickeq::numBands; ++b)
    {
        if (values.band (b).on)
            continue;
        const kickeq::BandParams bp = values.band (b);
        attachment (b, kickeq::BandParam::freq).setValueAsCompleteGesture (xToFreq (p.x));
        if (typeHasGain (bp.type))
            attachment (b, kickeq::BandParam::gain).setValueAsCompleteGesture (juce::jlimit (-24.0f, 24.0f, yToCurveDb (p.y)));
        attachment (b, kickeq::BandParam::on).setValueAsCompleteGesture (1.0f);
        setSelectedBand (b);
        refreshCurve();
        repaint();
        return;
    }
}

void EqCanvas::disableBand (int band)
{
    attachment (band, kickeq::BandParam::on).setValueAsCompleteGesture (0.0f);
    hoverBand = -1;
    refreshCurve();
    repaint();
}

juce::PopupMenu EqCanvas::buildBandMenu (int band) const
{
    const kickeq::BandParams p = values.band (band);
    juce::PopupMenu menu;
    menu.addSectionHeader ("Band " + juce::String (band + 1));

    juce::PopupMenu types, slopes, places;
    for (int i = 0; i < kickeq::numBandTypes; ++i)
        types.addItem (menuTypeBase + i, kickeq::bandTypeLabels[i], true, static_cast<int> (p.type) == i);
    for (int i = 0; i < kickeq::numSlopes; ++i)
        slopes.addItem (menuSlopeBase + i, kickeq::slopeLabels[i], true, p.slopeIndex == i);
    for (int i = 0; i < kickeq::numPlacements; ++i)
        places.addItem (menuPlacementBase + i, kickeq::placementLabels[i], true, static_cast<int> (p.place) == i);

    menu.addSubMenu ("Type", types);
    menu.addSubMenu ("Slope", slopes, typeIsCut (p.type));
    menu.addSubMenu ("Placement", places);
    menu.addSeparator();
    menu.addItem (menuDisableBand, "Disable band");
    return menu;
}

void EqCanvas::applyMenuChoice (int band, int result)
{
    if (result == 0)
        return;
    if (result == menuDisableBand)
        disableBand (band);
    else if (result >= menuTypeBase && result < menuSlopeBase)
        attachment (band, kickeq::BandParam::type).setValueAsCompleteGesture (static_cast<float> (result - menuTypeBase));
    else if (result >= menuSlopeBase && result < menuPlacementBase)
        attachment (band, kickeq::BandParam::slope).setValueAsCompleteGesture (static_cast<float> (result - menuSlopeBase));
    else if (result >= menuPlacementBase)
        attachment (band, kickeq::BandParam::place).setValueAsCompleteGesture (static_cast<float> (result - menuPlacementBase));
}

void EqCanvas::showBandMenu (int band)
{
    juce::PopupMenu menu = buildBandMenu (band);
    juce::Component::SafePointer<EqCanvas> safe (this);
    std::function<void (int)> done = [safe, band] (int result) {
        if (safe != nullptr)
            safe->applyMenuChoice (band, result);
    };

    if (menuPresenter)
    {
        menuPresenter (std::move (menu), std::move (done));
        return;
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition().withMinimumWidth (140),
                        std::move (done));
}

// ---- mouse and keyboard ---------------------------------------------------------------------------------------

void EqCanvas::mouseMove (const juce::MouseEvent& e)
{
    const int hit = bandAt (e.position);
    if (hit != hoverBand)
    {
        hoverBand = hit;
        repaint();
    }
}

void EqCanvas::mouseExit (const juce::MouseEvent&)
{
    if (hoverBand != -1 && dragBand < 0)
    {
        hoverBand = -1;
        repaint();
    }
}

void EqCanvas::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    endDrag(); // a second press while a drag is running (another button, a lost mouse-up): close the first gesture
    const int hit = bandAt (e.position);
    if (hit < 0)
        return;

    setSelectedBand (hit);
    hoverBand = hit;
    if (e.mods.isPopupMenu())
    {
        showBandMenu (hit);
        return;
    }
    beginDrag (hit, e);
}

void EqCanvas::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0)
        return;

    if (dragMode == DragMode::move)
    {
        const juce::Point<float> p = e.position - grabOffset;
        setBandValue (dragBand, kickeq::BandParam::freq, xToFreq (p.x));
        if (dragHasGain)
            setBandValue (dragBand, kickeq::BandParam::gain, juce::jlimit (-24.0f, 24.0f, yToCurveDb (p.y)));
    }
    else
    {
        // upwards = narrower (higher Q), like turning a knob clockwise
        const float dy = static_cast<float> (e.getDistanceFromDragStartY());
        setBandValue (dragBand, kickeq::BandParam::q, dragStartQ * std::exp (-dy * 0.012f));
    }
    refreshCurve();
    repaint();
}

void EqCanvas::mouseUp (const juce::MouseEvent& e)
{
    endDrag();
    const int hit = bandAt (e.position);
    if (hit != hoverBand)
        hoverBand = hit;
    repaint();
}

void EqCanvas::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    endDrag();
    const int hit = bandAt (e.position);
    if (hit >= 0)
        disableBand (hit);
    else if (plot.contains (e.position))
        addBandAt (e.position);
}

void EqCanvas::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int hit = bandAt (e.position);
    if (hit < 0)
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }
    const float delta = wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f);
    if (delta == 0.0f)
        return;
    const float q = values.band (hit).q * std::exp (delta * 1.2f);
    attachment (hit, kickeq::BandParam::q).setValueAsCompleteGesture (q);
    setSelectedBand (hit);
    refreshCurve();
    repaint();
}

bool EqCanvas::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (values.band (selectedBand).on)
            disableBand (selectedBand);
        return true;
    }
    return false;
}

juce::MouseCursor EqCanvas::getMouseCursor()
{
    if (hoverBand >= 0 || dragBand >= 0)
        return juce::MouseCursor::PointingHandCursor;
    return juce::MouseCursor::NormalCursor;
}
} // namespace kickeq::ui
