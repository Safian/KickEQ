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

// kickeq_editor_interaction_test - proves that the mouse and keyboard interaction of the editor canvas (EqCanvas)
// works, with synthetic juce::MouseEvents: node dragging (frequency / gain), mouse wheel and shift-drag (Q),
// double-click add / remove, Delete key, hover and hit-testing, the right-click menu, and the
// juce::ParameterAttachment round trip between host and editor.
//
// The processor and the editor are created without a window (no peer, no display needed on macOS / Windows), the
// editor is sized, and the canvas' own mouse handlers are called with events that carry the real main mouse source.
// The 30 Hz timer never fires (no message loop runs): the host-side "next refresh" is KickEQEditor::refreshFrame(),
// called by hand. Every check reads the parameters of the processor and counts begin / end change gestures with a
// juce::AudioProcessorParameter::Listener on each parameter. Exit code 0 when every check passed.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "Editor.h"
#include "EditorSettings.h"
#include "PluginProcessor.h"
#include "kickeq/params.h"

namespace
{
using kickeq::BandParam;
using kickeq::BandType;
using kickeq::ui::EqCanvas;

int passed = 0, failed = 0;

void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok)
    {
        ++passed;
        std::printf ("PASS  %s\n", name.toRawUTF8());
    }
    else
    {
        ++failed;
        std::printf ("FAIL  %s%s%s\n", name.toRawUTF8(), detail.isEmpty() ? "" : "  --  ", detail.toRawUTF8());
    }
}

bool near (float a, float b, float relTol, float absTol = 0.0f)
{
    return std::abs (a - b) <= juce::jmax (absTol, relTol * juce::jmax (std::abs (a), std::abs (b)));
}

bool same (float a, float b) { return juce::exactlyEqual (a, b); }

juce::String num (float v) { return juce::String (v, 4); }

// ---- gesture log: counts begin / end change gestures and value changes per parameter ------------------------------

struct GestureLog final : juce::AudioProcessorParameter::Listener
{
    struct Entry
    {
        int begins = 0, ends = 0, changes = 0, changesOutsideGesture = 0, depth = 0, maxDepth = 0;
    };
    std::array<Entry, static_cast<size_t> (kickeq::numParams)> entries {};

    void parameterValueChanged (int index, float) override
    {
        Entry& e = entries[static_cast<size_t> (index)];
        ++e.changes;
        if (e.depth == 0)
            ++e.changesOutsideGesture;
    }

    void parameterGestureChanged (int index, bool starting) override
    {
        Entry& e = entries[static_cast<size_t> (index)];
        if (starting)
        {
            ++e.begins;
            ++e.depth;
            e.maxDepth = juce::jmax (e.maxDepth, e.depth);
        }
        else
        {
            ++e.ends;
            --e.depth;
        }
    }

    void reset() { entries = {}; }
    const Entry& of (int band, BandParam k) const { return entries[static_cast<size_t> (kickeq::bandParamIndex (band, k))]; }

    /** Every gesture that began has ended, none nested, no negative depth. */
    bool balanced() const
    {
        for (const Entry& e : entries)
            if (e.begins != e.ends || e.depth != 0 || e.maxDepth > 1)
                return false;
        return true;
    }

    /** Total number of begun gestures on all parameters. */
    int totalBegins() const
    {
        int n = 0;
        for (const Entry& e : entries)
            n += e.begins;
        return n;
    }

    /** Total number of value changes on all parameters. */
    int totalChanges() const
    {
        int n = 0;
        for (const Entry& e : entries)
            n += e.changes;
        return n;
    }

    /** Value changes that did not happen inside a gesture (a host would see them as untouched automation). */
    int totalChangesOutsideGesture() const
    {
        int n = 0;
        for (const Entry& e : entries)
            n += e.changesOutsideGesture;
        return n;
    }
};

// ---- the processor + editor under test ------------------------------------------------------------------------------

struct Rig
{
    KickEQProcessor processor;
    GestureLog log;
    std::unique_ptr<juce::AudioProcessorEditor> base;
    kickeq::ui::EqCanvas* canvas = nullptr;
    kickeq::ui::BandPanel* strip = nullptr;
    KickEQEditor* editor = nullptr;

    Rig()
    {
        base.reset (processor.createEditor());
        editor = dynamic_cast<KickEQEditor*> (base.get());
        jassert (editor != nullptr);
        editor->setSize (KickEQEditor::defaultWidth, KickEQEditor::defaultHeight);
        canvas = &editor->getCanvas();
        strip = &editor->getBandPanel();
        for (auto* p : processor.getParameters())
            p->addListener (&log);
    }

    ~Rig()
    {
        for (auto* p : processor.getParameters())
            p->removeListener (&log);
    }

    void destroyEditor()
    {
        canvas = nullptr;
        strip = nullptr;
        editor = nullptr;
        base.reset();
    }

    /** What the 30 Hz timer would do. */
    void refresh() { editor->refreshFrame(); }

    float plain (int band, BandParam k) const { return const_cast<KickEQProcessor&> (processor).getCoreParameter (band, k).getPlain(); }
    bool isOn (int band) const { return plain (band, BandParam::on) >= 0.5f; }

    /** Sets parameters from the "host" side, then lets the editor refresh and forgets what the set-up logged. */
    void apply (const kickeq::ParamValues& v)
    {
        processor.applyValues (v);
        refresh();
        log.reset();
    }

    void setCurveRange (int index)
    {
        kickeq::ui::EditorSettings s = processor.getEditorSettings();
        s.curveRange = index;
        processor.setEditorSettings (s);
        refresh();
    }
};

void setBand (kickeq::ParamValues& v, int b, BandType type, float freq, float gain, float q, int slope = 1)
{
    v.set (b, BandParam::on, 1.0f);
    v.set (b, BandParam::type, static_cast<float> (type));
    v.set (b, BandParam::freq, freq);
    v.set (b, BandParam::gain, gain);
    v.set (b, BandParam::q, q);
    v.set (b, BandParam::slope, static_cast<float> (slope));
}

/** Frequency <-> x with the documented mapping: logarithmic over the full range of the frequency parameter, linear
    across the plot rectangle. Independent of the canvas implementation. */
float expectedX (const EqCanvas& c, float hz)
{
    const kickeq::ParamInfo& info = kickeq::paramInfo (kickeq::bandParamIndex (0, BandParam::freq));
    const juce::Rectangle<float> plot = c.getPlotBounds();
    return plot.getX() + plot.getWidth() * std::log (hz / info.min) / std::log (info.max / info.min);
}

float expectedHz (const EqCanvas& c, float x)
{
    const kickeq::ParamInfo& info = kickeq::paramInfo (kickeq::bandParamIndex (0, BandParam::freq));
    const juce::Rectangle<float> plot = c.getPlotBounds();
    const float t = juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth());
    return info.min * std::pow (info.max / info.min, t);
}

float expectedY (const EqCanvas& c, float gainDb, float rangeDb)
{
    const juce::Rectangle<float> plot = c.getPlotBounds();
    return plot.getCentreY() - gainDb / rangeDb * plot.getHeight() * 0.5f;
}

float expectedGainDb (const EqCanvas& c, float y, float rangeDb)
{
    const juce::Rectangle<float> plot = c.getPlotBounds();
    return juce::jlimit (-24.0f, 24.0f, (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * rangeDb);
}

// ---- synthetic mouse ------------------------------------------------------------------------------------------------

/** Builds the events a window would deliver to the canvas and calls the canvas' handlers directly (the same calls
    Component::internalMouseDown / Drag / Up / Move / Wheel make). */
struct Mouse
{
    EqCanvas& canvas;
    juce::Point<float> downPos;
    juce::Time downTime { 1000 };
    bool dragged = false;
    juce::ModifierKeys heldMods;

    explicit Mouse (EqCanvas& c) : canvas (c) {}

    juce::MouseEvent event (juce::Point<float> pos, juce::ModifierKeys mods, int clicks, bool wasDragged) const
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                                 juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                 juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                 juce::MouseInputSource::defaultTiltY, &canvas, &canvas, juce::Time (1000 + 10),
                                 downPos, downTime, clicks, wasDragged);
    }

    static juce::ModifierKeys withButton (bool shift, bool right)
    {
        int flags = right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier;
        if (shift)
            flags |= juce::ModifierKeys::shiftModifier;
        return juce::ModifierKeys (flags);
    }

    void move (juce::Point<float> pos) { canvas.mouseMove (event (pos, juce::ModifierKeys(), 0, false)); }
    void exit (juce::Point<float> pos) { canvas.mouseExit (event (pos, juce::ModifierKeys(), 0, false)); }

    void down (juce::Point<float> pos, bool shift = false, bool right = false, int clicks = 1)
    {
        downPos = pos;
        dragged = false;
        heldMods = withButton (shift, right);
        canvas.mouseDown (event (pos, heldMods, clicks, false));
    }

    void drag (juce::Point<float> pos)
    {
        dragged = true;
        canvas.mouseDrag (event (pos, heldMods, 1, true));
    }

    void up (juce::Point<float> pos, int clicks = 1)
    {
        const juce::ModifierKeys released = juce::ModifierKeys (heldMods.getRawFlags() & juce::ModifierKeys::shiftModifier);
        canvas.mouseUp (event (pos, released, clicks, dragged));
    }

    /** Press, move through the given points (one drag event each), release at the last one. */
    void dragPath (juce::Point<float> start, std::initializer_list<juce::Point<float>> path, bool shift = false)
    {
        down (start, shift);
        juce::Point<float> last = start;
        for (const auto& p : path)
        {
            drag (p);
            last = p;
        }
        up (last);
    }

    /** What the window sends for a double-click: down, up, down (2 clicks), up (2 clicks), then mouseDoubleClick. */
    void doubleClick (juce::Point<float> pos, bool right = false)
    {
        down (pos, false, right, 1);
        up (pos, 1);
        down (pos, false, right, 2);
        up (pos, 2);
        canvas.mouseDoubleClick (event (pos, withButton (false, right), 2, false));
    }

    void wheel (juce::Point<float> pos, float deltaY, bool reversed = false)
    {
        juce::MouseWheelDetails d;
        d.deltaX = 0.0f;
        d.deltaY = deltaY;
        d.isReversed = reversed;
        d.isSmooth = false;
        d.isInertial = false;
        canvas.mouseWheelMove (event (pos, juce::ModifierKeys(), 0, false), d);
    }
};

// ---- test data --------------------------------------------------------------------------------------------------------

/** Four bell bands well apart, plus one of each other type. Bands 0-6 on, band 7 off. */
kickeq::ParamValues standardSetting()
{
    kickeq::ParamValues v;
    setBand (v, 0, BandType::bell, 100.0f, 6.0f, 1.0f);
    setBand (v, 1, BandType::bell, 1000.0f, -4.0f, 2.0f);
    setBand (v, 2, BandType::lowCut, 30.0f, 0.0f, 0.707f);
    setBand (v, 3, BandType::highCut, 15000.0f, 0.0f, 0.707f);
    setBand (v, 4, BandType::notch, 3000.0f, 0.0f, 4.0f);
    setBand (v, 5, BandType::bandPass, 600.0f, 0.0f, 1.5f);
    setBand (v, 6, BandType::lowShelf, 200.0f, 3.0f, 0.71f);
    return v;
}

constexpr float defaultRangeDb = 12.0f; // EditorSettings default: +-12 dB

// ---- case 1: dragging ----------------------------------------------------------------------------------------------

void testDragFrequencyAndGain()
{
    Rig rig;
    rig.apply (standardSetting());
    EqCanvas& c = *rig.canvas;
    Mouse mouse (c);

    // horizontal drag of the bell at 1 kHz: frequency follows the log x axis, gain stays
    {
        const int band = 1;
        const juce::Point<float> start = c.getNodeCentre (band);
        check (near (start.x, expectedX (c, 1000.0f), 1.0e-4f, 0.01f), "drag: node x is the log-frequency position of its band",
               num (start.x) + " vs " + num (expectedX (c, 1000.0f)));
        check (near (start.y, expectedY (c, -4.0f, defaultRangeDb), 1.0e-4f, 0.01f), "drag: node y is the dB position of its band");

        const juce::Point<float> target (start.x + 140.0f, start.y);
        rig.log.reset();
        mouse.dragPath (start, { { start.x + 50.0f, start.y }, { start.x + 100.0f, start.y }, target });
        const float hz = rig.plain (band, BandParam::freq);
        check (near (hz, expectedHz (c, target.x), 2.0e-3f), "drag right: frequency = xToFreq(x)", num (hz) + " vs " + num (expectedHz (c, target.x)));
        check (hz > 1000.0f, "drag right: frequency goes up");
        check (near (rig.plain (band, BandParam::gain), -4.0f, 1.0e-4f, 1.0e-4f), "horizontal drag: gain unchanged", num (rig.plain (band, BandParam::gain)));
        check (c.getSelectedBand() == band, "mouse down on a node selects the band");
        check (c.getDragBand() == -1, "mouse up ends the drag");

        // exactly one gesture on frequency and on gain, none elsewhere
        bool one = rig.log.of (band, BandParam::freq).begins == 1 && rig.log.of (band, BandParam::freq).ends == 1
                   && rig.log.of (band, BandParam::gain).begins == 1 && rig.log.of (band, BandParam::gain).ends == 1;
        check (one, "drag: one begin/end gesture on frequency and one on gain");
        check (rig.log.totalBegins() == 2, "drag: no other parameter was touched", juce::String (rig.log.totalBegins()));
        check (rig.log.balanced(), "drag: gestures balanced and not nested");
        check (rig.log.totalChangesOutsideGesture() == 0, "drag: every value change happened inside a gesture");
        check (rig.log.of (band, BandParam::freq).changes >= 3, "drag: the host saw a value change per drag event");
    }

    // grab offset: grabbing the node away from its centre does not make it jump
    {
        const int band = 0;
        c.setSelectedBand (0);
        const juce::Point<float> centre = c.getNodeCentre (band);
        const juce::Point<float> grab (centre.x + 5.0f, centre.y - 4.0f);
        const float before = rig.plain (band, BandParam::freq);
        mouse.down (grab);
        mouse.drag (grab);
        check (near (rig.plain (band, BandParam::freq), before, 1.0e-3f), "drag: grabbing off-centre does not move the band until the mouse moves");
        check (near (rig.plain (band, BandParam::gain), 6.0f, 1.0e-3f, 1.0e-3f), "drag: grabbing off-centre keeps the gain");
        mouse.drag ({ grab.x + 60.0f, grab.y });
        mouse.up ({ grab.x + 60.0f, grab.y });
        check (near (rig.plain (band, BandParam::freq), expectedHz (c, centre.x + 60.0f), 2.0e-3f), "drag: off-centre grab keeps the offset (freq = xToFreq(x - offset))");
    }

    // vertical drag: gain follows the dB axis for every curve range, frequency stays
    for (int rangeIndex = 0; rangeIndex < 4; ++rangeIndex)
    {
        const float range = kickeq::ui::curveRangesDb[rangeIndex];
        rig.setCurveRange (rangeIndex);
        const int band = 6; // low shelf: has gain
        // park the band at 0 dB so that the node is inside the plot for every range
        rig.processor.getCoreParameter (band, BandParam::gain).setPlainNotifyingHost (0.0f);
        rig.refresh();
        rig.log.reset();
        const float f0 = rig.plain (band, BandParam::freq);
        const juce::Point<float> start = c.getNodeCentre (band);
        const juce::Point<float> target (start.x, start.y - 0.5f * c.getPlotBounds().getHeight() * 0.5f); // half way to the top
        mouse.dragPath (start, { { start.x, start.y - 20.0f }, target });
        const float g = rig.plain (band, BandParam::gain);
        const float want = expectedGainDb (c, target.y, range);
        check (near (g, want, 2.0e-3f, 2.0e-3f), "vertical drag, curve range +-" + juce::String (range, 0) + " dB: gain = yToDb(y)", num (g) + " vs " + num (want));
        check (near (g, range * 0.5f, 2.0e-3f), "vertical drag up by a quarter of the plot height = half the range", num (g));
        check (near (rig.plain (band, BandParam::freq), f0, 1.0e-4f), "vertical drag: frequency unchanged");
        check (rig.log.balanced() && rig.log.of (band, BandParam::gain).begins == 1, "vertical drag: one gain gesture, balanced");
    }
    rig.setCurveRange (1);

    // vertical drag downwards lowers the gain
    {
        const int band = 0;
        rig.processor.getCoreParameter (band, BandParam::gain).setPlainNotifyingHost (0.0f);
        rig.refresh();
        const juce::Point<float> start = c.getNodeCentre (band);
        mouse.dragPath (start, { { start.x, start.y + 30.0f } });
        check (rig.plain (band, BandParam::gain) < -1.0f, "vertical drag down: gain goes down", num (rig.plain (band, BandParam::gain)));
    }

    // cut / notch / band-pass nodes: vertical movement never changes the gain, and no gain gesture is started
    for (int band : { 2, 3, 4, 5 })
    {
        rig.refresh();
        rig.log.reset();
        const float gain = rig.plain (band, BandParam::gain);
        const float f0 = rig.plain (band, BandParam::freq);
        const juce::Point<float> start = c.getNodeCentre (band);
        check (near (start.y, c.getPlotBounds().getCentreY(), 1.0e-4f, 0.01f), "band without gain sits on the 0 dB line", "band " + juce::String (band + 1));
        mouse.dragPath (start, { { start.x - 30.0f, start.y - 80.0f }, { start.x - 30.0f, start.y + 90.0f }, { start.x - 40.0f, start.y - 10.0f } });
        const juce::String name = "band " + juce::String (band + 1) + " (type " + juce::String (static_cast<int> (rig.plain (band, BandParam::type))) + ")";
        check (same (rig.plain (band, BandParam::gain), gain), "no-gain type: vertical drag leaves the gain alone, " + name);
        check (rig.plain (band, BandParam::freq) < f0, "no-gain type: horizontal drag still moves the frequency, " + name);
        check (rig.log.of (band, BandParam::gain).begins == 0 && rig.log.of (band, BandParam::gain).changes == 0,
               "no-gain type: no gesture and no change on the gain parameter, " + name);
        check (rig.log.of (band, BandParam::freq).begins == 1 && rig.log.balanced(), "no-gain type: one frequency gesture, balanced, " + name);
    }

    // clamping: dragging far outside the canvas stops at the parameter limits
    {
        const int band = 1;
        rig.refresh();
        rig.log.reset();
        const juce::Point<float> start = c.getNodeCentre (band);
        mouse.dragPath (start, { { -5000.0f, -5000.0f } });
        check (same (rig.plain (band, BandParam::freq), 10.0f), "drag far left: frequency clamps at the minimum (10 Hz)", num (rig.plain (band, BandParam::freq)));
        check (same (rig.plain (band, BandParam::gain), 24.0f), "drag far above: gain clamps at +24 dB", num (rig.plain (band, BandParam::gain)));
        mouse.dragPath (c.getNodeCentre (band), { { 9000.0f, 9000.0f } });
        check (same (rig.plain (band, BandParam::freq), 30000.0f), "drag far right: frequency clamps at the maximum (30 kHz)", num (rig.plain (band, BandParam::freq)));
        check (same (rig.plain (band, BandParam::gain), -24.0f), "drag far below: gain clamps at -24 dB", num (rig.plain (band, BandParam::gain)));
        check (rig.log.balanced() && rig.log.of (band, BandParam::freq).begins == 2, "clamped drags: gestures balanced");

        // a clamped node is still drawn inside the plot, still hit, and can be dragged back
        const juce::Point<float> parked = c.getNodeCentre (band);
        check (c.getPlotBounds().contains (parked) || near (parked.y, c.getPlotBounds().getBottom(), 1.0e-4f, 0.01f),
               "node at 30 kHz / -24 dB is drawn inside the plot (edge)", num (parked.x) + "," + num (parked.y));
        mouse.move (parked);
        check (c.getHoverBand() == band, "node at the canvas edge is reachable (hover)");
        mouse.dragPath (parked, { { parked.x - 200.0f, parked.y - 40.0f } });
        check (rig.plain (band, BandParam::freq) < 30000.0f, "node at the canvas edge can be dragged back", num (rig.plain (band, BandParam::freq)));
    }

    // a node whose gain is beyond the curve range is drawn on the plot edge; grabbing it must not change the value
    {
        const int band = 0;
        rig.processor.getCoreParameter (band, BandParam::gain).setPlainNotifyingHost (20.0f); // range is +-12 dB
        rig.refresh();
        const juce::Point<float> edge = c.getNodeCentre (band);
        check (near (edge.y, c.getPlotBounds().getY(), 1.0e-4f, 0.01f), "node beyond the curve range is drawn on the top plot edge");
        mouse.down (edge);
        mouse.drag (edge); // press and "move" by nothing
        check (near (rig.plain (band, BandParam::gain), 20.0f, 1.0e-4f), "grabbing a node that is beyond the curve range does not change its gain",
               num (rig.plain (band, BandParam::gain)));
        mouse.drag ({ edge.x, edge.y + 10.0f });
        mouse.up ({ edge.x, edge.y + 10.0f });
        check (rig.plain (band, BandParam::gain) < 20.0f, "dragging it down lowers the gain");
        rig.processor.getCoreParameter (band, BandParam::gain).setPlainNotifyingHost (6.0f);
        rig.refresh();
    }

    // an extreme setting: 10 Hz node at the left edge of the plot is reachable too
    {
        const int band = 4;
        rig.processor.getCoreParameter (band, BandParam::freq).setPlainNotifyingHost (10.0f);
        rig.refresh();
        const juce::Point<float> p = c.getNodeCentre (band);
        mouse.move (p);
        check (c.getHoverBand() == band && near (p.x, c.getPlotBounds().getX(), 1.0e-4f, 0.01f), "node at 10 Hz sits on the left plot edge and is reachable");
        mouse.move ({ p.x - 11.0f, p.y });
        check (c.getHoverBand() == band, "node at the left edge: hit radius reaches into the margin");
    }
}

void testDragRobustness()
{
    // a second mouse down while a drag is running ends the first gesture before it begins a new one
    {
        Rig rig;
        rig.apply (standardSetting());
        EqCanvas& c = *rig.canvas;
        Mouse mouse (c);
        const juce::Point<float> a = c.getNodeCentre (0), b = c.getNodeCentre (1);
        mouse.down (a);
        mouse.drag ({ a.x + 10.0f, a.y });
        mouse.down (b); // e.g. a second button while the first is held, or a lost mouse-up
        mouse.drag ({ b.x + 10.0f, b.y });
        mouse.up ({ b.x + 10.0f, b.y });
        check (rig.log.balanced(), "mouse down during a drag: gestures stay balanced and not nested");
        check (rig.log.totalChangesOutsideGesture() == 0, "mouse down during a drag: no value change outside a gesture");
    }

    // closing the editor in the middle of a drag must not leave an open gesture behind
    {
        Rig rig;
        rig.apply (standardSetting());
        Mouse mouse (*rig.canvas);
        const juce::Point<float> a = rig.canvas->getNodeCentre (0);
        mouse.down (a);
        mouse.drag ({ a.x + 10.0f, a.y - 10.0f });
        rig.destroyEditor();
        check (rig.log.balanced(), "editor destroyed during a drag: gestures balanced");
    }

    // mouse up / drag without a mouse down are harmless
    {
        Rig rig;
        rig.apply (standardSetting());
        EqCanvas& c = *rig.canvas;
        Mouse mouse (c);
        mouse.drag ({ 300.0f, 100.0f });
        mouse.up ({ 300.0f, 100.0f });
        check (rig.log.totalBegins() == 0 && rig.log.totalChanges() == 0, "drag / up without a down on a node: nothing happens");
        mouse.down ({ 300.0f, 100.0f }); // empty space: no hit
        mouse.drag ({ 400.0f, 120.0f });
        mouse.up ({ 400.0f, 120.0f });
        check (rig.log.totalBegins() == 0 && rig.log.totalChanges() == 0, "press and drag on empty canvas: nothing happens");
    }
}

// ---- case 2: Q (wheel and shift-drag) -------------------------------------------------------------------------------

void testQ()
{
    Rig rig;
    rig.apply (standardSetting());
    EqCanvas& c = *rig.canvas;
    Mouse mouse (c);

    // wheel up over a node: Q grows (narrower), by exp(1.2 * delta)
    {
        const int band = 1;
        const float q0 = rig.plain (band, BandParam::q);
        const juce::Point<float> p = c.getNodeCentre (band);
        mouse.wheel (p, 0.1f);
        const float q1 = rig.plain (band, BandParam::q);
        check (q1 > q0, "wheel up over a node: Q goes up (narrower)", num (q0) + " -> " + num (q1));
        check (near (q1, q0 * std::exp (0.12f), 2.0e-3f), "wheel: Q changes by exp(1.2 * delta)", num (q1) + " vs " + num (q0 * std::exp (0.12f)));
        mouse.wheel (p, -0.1f);
        check (near (rig.plain (band, BandParam::q), q0, 2.0e-3f), "wheel down undoes wheel up", num (rig.plain (band, BandParam::q)));
        mouse.wheel (p, 0.1f, true);
        check (rig.plain (band, BandParam::q) < q0 * 1.001f, "wheel with the reversed flag (natural scrolling) inverts the direction", num (rig.plain (band, BandParam::q)));
        mouse.wheel (p, -0.1f, true);
        check (near (rig.plain (band, BandParam::q), q0, 2.0e-3f), "reversed wheel down = wheel up");

        check (rig.log.of (band, BandParam::q).begins == 4 && rig.log.of (band, BandParam::q).ends == 4, "wheel: one q gesture per wheel event");
        check (rig.log.totalBegins() == 4 && rig.log.balanced(), "wheel: only Q is touched, gestures balanced");
        check (rig.log.totalChangesOutsideGesture() == 0, "wheel: every change happened inside a gesture");
        check (same (rig.plain (band, BandParam::freq), 1000.0f) && near (rig.plain (band, BandParam::gain), -4.0f, 1.0e-4f, 1.0e-4f), "wheel: frequency and gain untouched");
        check (c.getSelectedBand() == band, "wheel over a node selects the band");
    }

    // clamps at 0.1 and 18
    {
        const int band = 0;
        const juce::Point<float> p = c.getNodeCentre (band);
        for (int i = 0; i < 40; ++i)
            mouse.wheel (p, 0.5f);
        check (same (rig.plain (band, BandParam::q), 18.0f), "wheel: Q clamps at 18", num (rig.plain (band, BandParam::q)));
        for (int i = 0; i < 80; ++i)
            mouse.wheel (p, -0.5f);
        check (same (rig.plain (band, BandParam::q), 0.1f), "wheel: Q clamps at 0.1", num (rig.plain (band, BandParam::q)));
        mouse.wheel (p, 0.5f);
        check (rig.plain (band, BandParam::q) > 0.1f, "wheel: Q leaves the lower clamp again");
        check (rig.log.balanced(), "wheel clamp runs: gestures balanced");
    }

    // wheel over empty canvas or with a zero delta: nothing changes
    {
        rig.log.reset();
        mouse.wheel ({ 500.0f, 20.0f }, 0.5f);
        mouse.wheel (c.getNodeCentre (1), 0.0f);
        check (rig.log.totalBegins() == 0 && rig.log.totalChanges() == 0, "wheel over empty canvas / zero delta: no parameter change, no gesture");
    }

    // wheel on a node that is not selected goes to that node (the nearest one)
    {
        c.setSelectedBand (0);
        const float qOther = rig.plain (6, BandParam::q);
        mouse.wheel (c.getNodeCentre (6), 0.2f);
        check (rig.plain (6, BandParam::q) > qOther && c.getSelectedBand() == 6, "wheel goes to the node under the mouse, not to the selected band");
    }

    // shift-drag: Q only, upwards = narrower (higher Q), exp(-0.012 * dy)
    {
        const int band = 1;
        rig.processor.getCoreParameter (band, BandParam::q).setPlainNotifyingHost (2.0f);
        rig.refresh();
        rig.log.reset();
        const float f0 = rig.plain (band, BandParam::freq), g0 = rig.plain (band, BandParam::gain);
        const juce::Point<float> p = c.getNodeCentre (band);
        mouse.dragPath (p, { { p.x + 5.0f, p.y - 20.0f }, { p.x + 9.0f, p.y - 50.0f } }, true);
        const float q = rig.plain (band, BandParam::q);
        check (q > 2.0f, "shift-drag up: Q goes up (narrower)", num (q));
        check (near (q, 2.0f * std::exp (50.0f * 0.012f), 2.0e-3f), "shift-drag: Q = Q0 * exp(-0.012 * dy)", num (q) + " vs " + num (2.0f * std::exp (0.6f)));
        check (same (rig.plain (band, BandParam::freq), f0) && same (rig.plain (band, BandParam::gain), g0), "shift-drag: frequency and gain untouched, even with horizontal movement");
        check (rig.log.of (band, BandParam::q).begins == 1 && rig.log.totalBegins() == 1 && rig.log.balanced(),
               "shift-drag: exactly one gesture, on Q");
        check (rig.log.totalChangesOutsideGesture() == 0, "shift-drag: every change inside the gesture");

        mouse.dragPath (c.getNodeCentre (band), { { p.x, p.y + 40.0f } }, true);
        check (rig.plain (band, BandParam::q) < q, "shift-drag down: Q goes down (wider)");
        mouse.dragPath (c.getNodeCentre (band), { { p.x, p.y - 4000.0f } }, true);
        check (same (rig.plain (band, BandParam::q), 18.0f), "shift-drag far up: Q clamps at 18");
        mouse.dragPath (c.getNodeCentre (band), { { p.x, p.y + 4000.0f } }, true);
        check (same (rig.plain (band, BandParam::q), 0.1f), "shift-drag far down: Q clamps at 0.1");
        check (rig.log.balanced() && rig.log.of (band, BandParam::freq).begins == 0 && rig.log.of (band, BandParam::gain).begins == 0,
               "shift-drag runs: gestures balanced, never on freq / gain");
    }

    // shift-drag on a no-gain band behaves the same
    {
        const int band = 4; // notch
        const float q0 = rig.plain (band, BandParam::q);
        const juce::Point<float> p = c.getNodeCentre (band);
        mouse.dragPath (p, { { p.x, p.y - 25.0f } }, true);
        check (rig.plain (band, BandParam::q) > q0, "shift-drag on a notch: Q changes");
    }
}

// ---- case 3: double-click, Delete -----------------------------------------------------------------------------------

void testAddRemove()
{
    // double-click on empty canvas enables the first disabled band at that frequency / gain
    {
        Rig rig;
        kickeq::ParamValues v;
        setBand (v, 0, BandType::bell, 100.0f, 3.0f, 1.0f);
        setBand (v, 1, BandType::bell, 5000.0f, -3.0f, 1.0f);
        // band 2 is off, band 3 is on, band 4 is off -> the new band must be band 2
        setBand (v, 3, BandType::bell, 800.0f, 0.0f, 1.0f);
        rig.apply (v);
        EqCanvas& c = *rig.canvas;
        Mouse mouse (c);

        check (!rig.isOn (2), "set-up: band 3 is off");
        const juce::Rectangle<float> plot = c.getPlotBounds();
        const juce::Point<float> where (plot.getX() + plot.getWidth() * 0.5f, plot.getY() + plot.getHeight() * 0.25f);
        mouse.doubleClick (where);
        check (rig.isOn (2), "double-click on empty canvas: the first disabled band (3) is enabled");
        check (!rig.isOn (4), "double-click: only one band is enabled");
        const BandType type = static_cast<BandType> (static_cast<int> (rig.plain (2, BandParam::type)));
        check (near (rig.plain (2, BandParam::freq), expectedHz (c, where.x), 2.0e-3f), "double-click: new band at the clicked frequency",
               num (rig.plain (2, BandParam::freq)) + " vs " + num (expectedHz (c, where.x)));
        if (type == BandType::bell || type == BandType::lowShelf || type == BandType::highShelf)
            check (near (rig.plain (2, BandParam::gain), expectedGainDb (c, where.y, defaultRangeDb), 2.0e-3f, 1.0e-3f),
                   "double-click: new band at the clicked gain", num (rig.plain (2, BandParam::gain)) + " vs " + num (expectedGainDb (c, where.y, defaultRangeDb)));
        else
            check (same (rig.plain (2, BandParam::gain), 0.0f), "double-click: a band without gain keeps 0 dB");
        check (c.getSelectedBand() == 2 && rig.strip->getSelectedBand() == 2, "double-click: the new band is selected, canvas and strip");
        check (rig.log.of (2, BandParam::on).begins == 1 && rig.log.of (2, BandParam::on).ends == 1, "double-click add: one gesture on 'on'");
        check (rig.log.of (2, BandParam::freq).begins == 1 && rig.log.balanced(), "double-click add: one gesture on frequency, balanced");
        check (rig.log.totalChangesOutsideGesture() == 0, "double-click add: all changes inside gestures");
        check (rig.log.of (0, BandParam::freq).changes == 0 && rig.log.of (1, BandParam::on).changes == 0 && rig.log.of (3, BandParam::on).changes == 0,
               "double-click add: other bands untouched");
        c.setSelectedBand (2);
        check (near (c.getNodeCentre (2).x, where.x, 2.0e-3f, 0.05f), "double-click add: the node is under the mouse");

        // the node now exists: a second double-click at the same spot removes it again (and does not add another)
        mouse.doubleClick (c.getNodeCentre (2));
        check (!rig.isOn (2), "double-click on a node: the band is disabled");
        check (!rig.isOn (4), "double-click on a node does not add a band");
        check (c.getHoverBand() == -1, "disabling a band clears the hover highlight");
        check (rig.log.balanced(), "double-click remove: gestures balanced");
        check (rig.plain (2, BandParam::freq) > 0.0f && near (rig.plain (2, BandParam::freq), expectedHz (c, where.x), 2.0e-3f), "disabled band keeps its frequency");
    }

    // double-click on a node disables it and changes nothing else; the node disappears (not hit any more)
    {
        Rig rig;
        rig.apply (standardSetting());
        EqCanvas& c = *rig.canvas;
        Mouse mouse (c);
        const int band = 1;
        const juce::Point<float> p = c.getNodeCentre (band);
        mouse.doubleClick (p);
        check (!rig.isOn (band), "double-click on a node disables the band");
        check (same (rig.plain (band, BandParam::freq), 1000.0f) && near (rig.plain (band, BandParam::gain), -4.0f, 1.0e-4f, 1.0e-4f),
               "double-click on a node: frequency and gain are untouched (no accidental drag)");
        check (rig.log.of (band, BandParam::on).begins == 1 && rig.log.balanced(), "double-click on a node: gestures balanced, one on 'on'");
        mouse.move (p);
        check (c.getHoverBand() == -1, "a disabled band's node is not hit any more");
        check (rig.isOn (0) && rig.isOn (2) && rig.isOn (6), "other bands stay on");
    }

    // Delete / Backspace with a selected band disables it
    {
        Rig rig;
        rig.apply (standardSetting());
        EqCanvas& c = *rig.canvas;
        c.setSelectedBand (0);
        rig.log.reset();
        check (c.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)), "Delete key is handled");
        check (!rig.isOn (0), "Delete: the selected band is disabled");
        check (rig.isOn (1) && rig.isOn (2), "Delete: other bands untouched");
        check (rig.log.of (0, BandParam::on).begins == 1 && rig.log.balanced() && rig.log.totalBegins() == 1, "Delete: one gesture on 'on'");

        // again on the already disabled band: handled, nothing happens
        rig.log.reset();
        check (c.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)), "Delete on a disabled band is handled");
        check (rig.log.totalBegins() == 0 && rig.log.totalChanges() == 0, "Delete on a disabled band: no gesture, no change");

        c.setSelectedBand (3);
        check (c.keyPressed (juce::KeyPress (juce::KeyPress::backspaceKey)) && !rig.isOn (3), "Backspace also disables the selected band");
        check (!c.keyPressed (juce::KeyPress ('a')) && !c.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)), "other keys are not handled");
        check (rig.isOn (1), "other keys change nothing");

        // select by mouse then delete
        Mouse mouse (c);
        mouse.down (c.getNodeCentre (4));
        mouse.up (c.getNodeCentre (4));
        check (c.getSelectedBand() == 4, "click selects band 5");
        c.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        check (!rig.isOn (4) && rig.isOn (5), "click then Delete removes exactly the clicked band");
    }

    // all 8 bands on: double-click on empty canvas does nothing
    {
        Rig rig;
        kickeq::ParamValues v;
        for (int b = 0; b < kickeq::numBands; ++b)
            setBand (v, b, BandType::bell, 100.0f * std::pow (1.9f, static_cast<float> (b)), 0.0f, 1.0f);
        rig.apply (v);
        EqCanvas& c = *rig.canvas;
        Mouse mouse (c);
        const juce::Rectangle<float> plot = c.getPlotBounds();
        mouse.doubleClick ({ plot.getX() + 5.0f, plot.getY() + 5.0f });
        mouse.doubleClick ({ plot.getRight() - 5.0f, plot.getBottom() - 5.0f });
        bool allOn = true;
        for (int b = 0; b < kickeq::numBands; ++b)
            allOn = allOn && rig.isOn (b);
        check (allOn, "all 8 bands on: double-click on empty canvas does not change a band");
        check (rig.log.totalChanges() == 0, "all 8 bands on: double-click on empty canvas changes no parameter at all", juce::String (rig.log.totalChanges()));
        check (rig.log.balanced(), "all 8 bands on: gestures balanced");
    }

    // double-click outside the plot (margins) or with the right button never adds a band
    {
        Rig rig;
        rig.apply (standardSetting());
        EqCanvas& c = *rig.canvas;
        Mouse mouse (c);
        const juce::Rectangle<float> plot = c.getPlotBounds();
        mouse.doubleClick ({ 5.0f, plot.getCentreY() });
        mouse.doubleClick ({ plot.getCentreX(), 2.0f });
        mouse.doubleClick ({ plot.getCentreX(), static_cast<float> (c.getHeight()) - 3.0f });
        check (!rig.isOn (7), "double-click in the margins does not add a band");
        mouse.doubleClick ({ plot.getCentreX(), plot.getY() + 30.0f }, true);
        check (!rig.isOn (7), "right double-click does not add a band");
        check (rig.log.totalChanges() == 0, "margin / right double-clicks change nothing");
    }

    // enabling by double-click in the last free slot, then the canvas is full
    {
        Rig rig;
        rig.apply (standardSetting()); // band 7 free
        EqCanvas& c = *rig.canvas;
        Mouse mouse (c);
        const juce::Rectangle<float> plot = c.getPlotBounds();
        mouse.doubleClick ({ plot.getX() + plot.getWidth() * 0.9f, plot.getY() + plot.getHeight() * 0.8f });
        check (rig.isOn (7), "double-click fills the last free band (8)");
        rig.log.reset();
        mouse.doubleClick ({ plot.getX() + plot.getWidth() * 0.55f, plot.getY() + plot.getHeight() * 0.15f });
        check (rig.log.totalChanges() == 0, "after the 8th band, another double-click on empty canvas does nothing");
    }
}

// ---- case 4: hover and hit-testing ----------------------------------------------------------------------------------

void testHoverAndHit()
{
    Rig rig;
    rig.apply (standardSetting());
    EqCanvas& c = *rig.canvas;
    Mouse mouse (c);

    check (c.getHoverBand() == -1, "hover: none at start");
    check (c.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::NormalCursor), "cursor: normal when nothing is hovered");

    mouse.move (c.getNodeCentre (1));
    check (c.getHoverBand() == 1, "hover: mouse over node 2 highlights band 2");
    check (c.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::PointingHandCursor), "cursor: pointing hand over a node");
    check (c.getSelectedBand() == 0, "hover does not change the selection");

    // hit radius is 12 px (exclusive)
    const juce::Point<float> n1 = c.getNodeCentre (1);
    mouse.move ({ n1.x + 11.5f, n1.y });
    check (c.getHoverBand() == 1, "hover: 11.5 px from the node centre still hits");
    mouse.move ({ n1.x + 12.5f, n1.y });
    check (c.getHoverBand() == -1, "hover: 12.5 px from the node centre misses");
    mouse.move ({ n1.x + 8.0f, n1.y + 8.0f });
    check (c.getHoverBand() == 1, "hover: the hit area is circular (8,8 = 11.3 px hits)");
    mouse.move ({ n1.x + 9.5f, n1.y + 9.5f });
    check (c.getHoverBand() == -1, "hover: (9.5,9.5) = 13.4 px misses");

    mouse.move (c.getNodeCentre (6));
    check (c.getHoverBand() == 6, "hover moves from node to node");
    mouse.move ({ 500.0f, 500.0f });
    check (c.getHoverBand() == -1, "hover: leaving the node clears the highlight");

    mouse.move (c.getNodeCentre (0));
    mouse.exit ({ -5.0f, -5.0f });
    check (c.getHoverBand() == -1, "hover: mouse exit clears the highlight");

    // mouse exit during a drag keeps the highlight of the dragged node
    mouse.down (c.getNodeCentre (0));
    mouse.exit ({ -5.0f, -5.0f });
    check (c.getHoverBand() == 0 && c.getDragBand() == 0, "hover: mouse exit during a drag keeps the highlight");
    mouse.up (c.getNodeCentre (0));
    check (c.getDragBand() == -1, "hover: drag ended");
    // mouse up away from the node updates the highlight
    mouse.down (c.getNodeCentre (0));
    mouse.drag ({ c.getNodeCentre (0).x + 200.0f, c.getNodeCentre (0).y });
    mouse.up (c.getNodeCentre (0));
    check (c.getHoverBand() == 0, "hover: after the drag the highlight follows the moved node");

    // setHoverBand (previewBand path) is reported back
    c.setHoverBand (4);
    check (c.getHoverBand() == 4, "setHoverBand sets the highlight");
    c.setHoverBand (-1);

    // hover on a disabled band's position: nothing
    rig.processor.getCoreParameter (1, BandParam::on).setPlainNotifyingHost (0.0f);
    rig.refresh();
    mouse.move (n1);
    check (c.getHoverBand() == -1, "hover: disabled band is invisible to the mouse");
    rig.processor.getCoreParameter (1, BandParam::on).setPlainNotifyingHost (1.0f);
    rig.refresh();

    // ---- overlapping nodes ---------------------------------------------------------------------------------------------
    // rule implemented by EqCanvas::bandAt: of the enabled bands whose node centre is closer than 12 px, the NEAREST
    // wins; at exactly equal distance (stacked nodes) the SELECTED band wins, otherwise the lowest band number.
    // bands 0 and 1: same gain, 10 px apart in x
    {
        kickeq::ParamValues v;
        setBand (v, 0, BandType::bell, 1000.0f, 3.0f, 1.0f);
        setBand (v, 1, BandType::bell, 1000.0f, 3.0f, 1.0f);
        rig.apply (v);
        const float f0 = 1000.0f;
        const float x0 = expectedX (c, f0);
        // find the frequency 10 px to the right of band 0
        const kickeq::ParamInfo& info = kickeq::paramInfo (kickeq::bandParamIndex (0, BandParam::freq));
        const juce::Rectangle<float> plot = c.getPlotBounds();
        const float f1 = info.min * std::exp ((x0 + 10.0f - plot.getX()) / plot.getWidth() * std::log (info.max / info.min));
        rig.processor.getCoreParameter (1, BandParam::freq).setPlainNotifyingHost (f1);
        rig.refresh();
        const juce::Point<float> p0 = c.getNodeCentre (0), p1 = c.getNodeCentre (1);
        check (near (p1.x - p0.x, 10.0f, 0.01f, 0.05f) && near (p1.y, p0.y, 1.0e-5f, 1.0e-3f), "overlap set-up: nodes 10 px apart", num (p1.x - p0.x));

        c.setSelectedBand (0);
        mouse.move ({ p0.x + 3.0f, p0.y });
        check (c.getHoverBand() == 0, "overlap: 3 px from node 1, 7 px from node 2 -> the nearer (1)");
        mouse.move ({ p0.x + 7.0f, p0.y });
        check (c.getHoverBand() == 1, "overlap: 7 px from node 1, 3 px from node 2 -> the nearer (2), although band 1 is selected");
        mouse.move ({ p0.x + 5.5f, p0.y });
        check (c.getHoverBand() == 1, "overlap: 5.5 px / 4.5 px -> nearer (2)");
        mouse.move ({ p0.x - 10.0f, p0.y });
        check (c.getHoverBand() == 0, "overlap: only node 1 in range -> node 1");
        mouse.move ({ p1.x + 10.0f, p1.y });
        check (c.getHoverBand() == 1, "overlap: only node 2 in range -> node 2");

        // dragging picks the nearest node as well
        c.setSelectedBand (0);
        mouse.down ({ p0.x + 8.0f, p0.y });
        check (c.getDragBand() == 1 && c.getSelectedBand() == 1, "overlap: mouse down picks the nearest node (2) and selects it");
        mouse.up ({ p0.x + 8.0f, p0.y });
        rig.log.reset();

        // exactly stacked nodes: the selected band wins, else the lowest number
        rig.processor.getCoreParameter (1, BandParam::freq).setPlainNotifyingHost (f0);
        rig.refresh();
        const juce::Point<float> s = c.getNodeCentre (0);
        check (c.getNodeCentre (1) == s, "stack set-up: nodes at the same position");
        c.setSelectedBand (3);
        mouse.move (s);
        check (c.getHoverBand() == 0, "stacked nodes, neither selected: the lowest band number wins");
        c.setSelectedBand (1);
        mouse.move (s);
        check (c.getHoverBand() == 1, "stacked nodes, band 2 selected: the selected band wins");
        c.setSelectedBand (0);
        mouse.move ({ s.x + 1.0f, s.y });
        check (c.getHoverBand() == 0, "stacked nodes, band 1 selected: band 1 wins");
        mouse.down (s);
        mouse.up (s);
        check (c.getSelectedBand() == 0, "stacked nodes: clicking keeps the selected band");
    }

    // three nodes inside the hit radius: nearest wins, in any selection state
    {
        kickeq::ParamValues v;
        setBand (v, 0, BandType::bell, 1000.0f, 0.0f, 1.0f);
        setBand (v, 1, BandType::bell, 1000.0f, 0.0f, 1.0f);
        setBand (v, 2, BandType::bell, 1000.0f, 0.0f, 1.0f);
        rig.apply (v);
        const juce::Point<float> base = c.getNodeCentre (0);
        // move band 1 up by 4 dB-equivalent and band 2 down: the pixel offsets follow from the dB axis
        const float pxPerDb = c.getPlotBounds().getHeight() * 0.5f / defaultRangeDb;
        rig.processor.getCoreParameter (1, BandParam::gain).setPlainNotifyingHost (-5.0f / pxPerDb); // 5 px below
        rig.processor.getCoreParameter (2, BandParam::gain).setPlainNotifyingHost (9.0f / pxPerDb);  // 9 px above
        rig.refresh();
        for (int sel : { 0, 1, 2 })
        {
            c.setSelectedBand (sel);
            mouse.move ({ base.x, base.y + 4.0f });
            check (c.getHoverBand() == 1, "three nodes: the pointer is 4 px from node 2, 4 px from node 1 (tie), 13 from node 3");
        }
    }
}

// ---- case 5: right-click menu ---------------------------------------------------------------------------------------

struct MenuItemInfo
{
    int id;
    juce::String text;
    bool enabled, ticked, isSubMenu, isSeparator, isSection;
    std::vector<MenuItemInfo> children;
};

std::vector<MenuItemInfo> describe (const juce::PopupMenu& menu)
{
    std::vector<MenuItemInfo> items;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const juce::PopupMenu::Item& i = it.getItem();
        MenuItemInfo info { i.itemID, i.text, i.isEnabled, i.isTicked, i.subMenu != nullptr, i.isSeparator, i.isSectionHeader, {} };
        if (i.subMenu != nullptr)
            info.children = describe (*i.subMenu);
        items.push_back (std::move (info));
    }
    return items;
}

const MenuItemInfo* find (const std::vector<MenuItemInfo>& items, const juce::String& text)
{
    for (const auto& i : items)
        if (i.text == text)
            return &i;
    return nullptr;
}

int tickedId (const MenuItemInfo& sub)
{
    for (const auto& c : sub.children)
        if (c.ticked)
            return c.id;
    return -1;
}

void testMenu()
{
    Rig rig;
    rig.apply (standardSetting());
    EqCanvas& c = *rig.canvas;
    Mouse mouse (c);

    // the real right-click path: the menu is handed to a presenter (instead of a window) together with its callback
    int presented = 0;
    juce::PopupMenu lastMenu;
    std::function<void (int)> lastDone;
    c.menuPresenter = [&] (juce::PopupMenu menu, std::function<void (int)> done) {
        ++presented;
        lastMenu = std::move (menu);
        lastDone = std::move (done);
    };

    // right-click on empty canvas: no menu
    mouse.down ({ 600.0f, 40.0f }, false, true);
    mouse.up ({ 600.0f, 40.0f });
    check (presented == 0, "right-click on empty canvas: no menu");

    // right-click on a bell node
    const int band = 1;
    const juce::Point<float> p = c.getNodeCentre (band);
    c.setSelectedBand (0);
    rig.log.reset();
    mouse.down (p, false, true);
    check (presented == 1, "right-click on a node: the band menu is presented");
    check (c.getSelectedBand() == band, "right-click on a node selects the band");
    check (c.getDragBand() == -1, "right-click does not start a drag");
    mouse.drag ({ p.x + 80.0f, p.y - 60.0f });
    mouse.up ({ p.x + 80.0f, p.y - 60.0f });
    check (rig.log.totalBegins() == 0 && rig.log.totalChanges() == 0, "right-click + movement changes no parameter");

    // menu content
    {
        const auto items = describe (lastMenu);
        const MenuItemInfo* header = nullptr;
        for (const auto& i : items)
            if (i.isSection)
                header = &i;
        check (header != nullptr && header->text == "Band 2", "menu: section header 'Band 2'");
        const MenuItemInfo* types = find (items, "Type");
        const MenuItemInfo* slopes = find (items, "Slope");
        const MenuItemInfo* places = find (items, "Placement");
        const MenuItemInfo* disable = find (items, "Disable band");
        check (types != nullptr && slopes != nullptr && places != nullptr && disable != nullptr, "menu: Type, Slope, Placement and Disable band entries");
        if (types != nullptr && slopes != nullptr && places != nullptr && disable != nullptr)
        {
            check (static_cast<int> (types->children.size()) == kickeq::numBandTypes && types->enabled, "menu: one entry per band type");
            check (tickedId (*types) == EqCanvas::menuTypeBase + static_cast<int> (BandType::bell), "menu: current type (Bell) is ticked");
            check (static_cast<int> (places->children.size()) == kickeq::numPlacements && tickedId (*places) == EqCanvas::menuPlacementBase, "menu: placements, Stereo ticked");
            check (static_cast<int> (slopes->children.size()) == kickeq::numSlopes && tickedId (*slopes) == EqCanvas::menuSlopeBase + 1, "menu: slopes, 12 dB/oct ticked");
            check (!slopes->enabled, "menu: the slope submenu is disabled for a bell");
            check (disable->id == EqCanvas::menuDisableBand, "menu: 'Disable band' has id 1");
            bool labelsOk = true;
            for (int i = 0; i < kickeq::numBandTypes; ++i)
                labelsOk = labelsOk && types->children[static_cast<size_t> (i)].text == kickeq::bandTypeLabels[i];
            check (labelsOk, "menu: type labels come from the core table");
        }
    }

    // choosing: type Low Cut
    lastDone (EqCanvas::menuTypeBase + static_cast<int> (BandType::lowCut));
    check (static_cast<int> (rig.plain (band, BandParam::type)) == static_cast<int> (BandType::lowCut), "menu: choosing Low Cut sets the band type");
    check (rig.log.of (band, BandParam::type).begins == 1 && rig.log.balanced() && rig.log.totalBegins() == 1, "menu: type change is one complete gesture on the type parameter");
    rig.refresh();
    check (near (c.getNodeCentre (band).y, c.getPlotBounds().getCentreY(), 1.0e-4f, 0.01f), "menu: a band turned into a cut moves its node to the 0 dB line");
    check (same (rig.plain (band, BandParam::freq), 1000.0f) && near (rig.plain (band, BandParam::gain), -4.0f, 1.0e-4f, 1.0e-4f), "menu: type change keeps freq and gain");
    check (static_cast<juce::ComboBox&> (rig.strip->getControl (BandParam::type)).getSelectedId() == static_cast<int> (BandType::lowCut) + 1,
           "menu: the strip's type box follows");

    // the menu rebuilt for a cut: slope enabled, Low Cut ticked
    {
        const auto items = describe (c.buildBandMenu (band));
        const MenuItemInfo* slopes = find (items, "Slope");
        const MenuItemInfo* types = find (items, "Type");
        check (slopes != nullptr && slopes->enabled, "menu: the slope submenu is enabled for a cut");
        check (types != nullptr && tickedId (*types) == EqCanvas::menuTypeBase + static_cast<int> (BandType::lowCut), "menu: Low Cut is ticked after the change");
    }

    // slope
    rig.log.reset();
    c.applyMenuChoice (band, EqCanvas::menuSlopeBase + 4);
    check (static_cast<int> (rig.plain (band, BandParam::slope)) == 4, "menu: choosing 36 dB/oct sets the slope");
    check (rig.log.of (band, BandParam::slope).begins == 1 && rig.log.balanced() && rig.log.totalBegins() == 1, "menu: slope change = one gesture");
    rig.refresh();
    check (tickedId (*find (describe (c.buildBandMenu (band)), "Slope")) == EqCanvas::menuSlopeBase + 4, "menu: the new slope is ticked");

    // placement
    rig.log.reset();
    c.applyMenuChoice (band, EqCanvas::menuPlacementBase + static_cast<int> (kickeq::Placement::mid));
    check (static_cast<int> (rig.plain (band, BandParam::place)) == static_cast<int> (kickeq::Placement::mid), "menu: choosing Mid sets the placement");
    check (rig.log.of (band, BandParam::place).begins == 1 && rig.log.balanced() && rig.log.totalBegins() == 1, "menu: placement change = one gesture");
    c.applyMenuChoice (band, EqCanvas::menuPlacementBase + static_cast<int> (kickeq::Placement::side));
    check (static_cast<int> (rig.plain (band, BandParam::place)) == static_cast<int> (kickeq::Placement::side), "menu: choosing Side sets the placement");
    for (int t = 0; t < kickeq::numBandTypes; ++t)
    {
        c.applyMenuChoice (band, EqCanvas::menuTypeBase + t);
        if (static_cast<int> (rig.plain (band, BandParam::type)) != t)
            check (false, "menu: every type id reaches the parameter", juce::String (t));
    }
    check (static_cast<int> (rig.plain (band, BandParam::type)) == kickeq::numBandTypes - 1, "menu: every type id reaches the parameter");
    for (int pl = 0; pl < kickeq::numPlacements; ++pl)
    {
        c.applyMenuChoice (band, EqCanvas::menuPlacementBase + pl);
        if (static_cast<int> (rig.plain (band, BandParam::place)) != pl)
            check (false, "menu: every placement id reaches the parameter", juce::String (pl));
    }
    for (int sl = 0; sl < kickeq::numSlopes; ++sl)
    {
        c.applyMenuChoice (band, EqCanvas::menuSlopeBase + sl);
        if (static_cast<int> (rig.plain (band, BandParam::slope)) != sl)
            check (false, "menu: every slope id reaches the parameter", juce::String (sl));
    }
    check (static_cast<int> (rig.plain (band, BandParam::slope)) == kickeq::numSlopes - 1, "menu: every slope id reaches the parameter");
    check (rig.log.balanced(), "menu: all gestures balanced");

    // dismissed menu: nothing
    rig.log.reset();
    c.applyMenuChoice (band, 0);
    check (rig.log.totalBegins() == 0 && rig.log.totalChanges() == 0, "menu: dismissing changes nothing");

    // other bands are untouched by everything above
    check (same (rig.plain (0, BandParam::type), 0.0f) && static_cast<int> (rig.plain (2, BandParam::type)) == static_cast<int> (BandType::lowCut)
               && static_cast<int> (rig.plain (0, BandParam::place)) == 0,
           "menu: choices only affect the right-clicked band");

    // Disable band, through the presenter callback
    mouse.down (c.getNodeCentre (0), false, true);
    mouse.up (c.getNodeCentre (0));
    check (presented == 2, "second right-click presents a second menu");
    lastDone (EqCanvas::menuDisableBand);
    check (!rig.isOn (0) && rig.isOn (1), "menu: 'Disable band' disables the right-clicked band");

    // the callback of a menu outlives the canvas: it must be a no-op, not a crash
    mouse.down (c.getNodeCentre (1), false, true);
    mouse.up (c.getNodeCentre (1));
    check (presented == 3, "third menu presented");
    std::function<void (int)> orphan = lastDone;
    rig.destroyEditor();
    orphan (EqCanvas::menuDisableBand);
    check (rig.isOn (1), "menu callback after the editor was destroyed is ignored");
}

// ---- case 6: parameter attachment round trip ------------------------------------------------------------------------

void testHostSide()
{
    Rig rig;
    rig.apply (standardSetting());
    EqCanvas& c = *rig.canvas;
    Mouse mouse (c);

    // a host moves a parameter: the node follows on the next refresh
    {
        const int band = 1;
        const juce::Point<float> before = c.getNodeCentre (band);
        auto& freq = rig.processor.getCoreParameter (band, BandParam::freq);
        auto& gain = rig.processor.getCoreParameter (band, BandParam::gain);
        freq.beginChangeGesture();
        freq.setValueNotifyingHost (freq.convertTo0to1 (4000.0f));
        gain.setValueNotifyingHost (gain.convertTo0to1 (8.0f));
        freq.endChangeGesture();
        check (c.getNodeCentre (band) == before, "host move: the canvas shows the old position until the next refresh");
        rig.refresh();
        const juce::Point<float> after = c.getNodeCentre (band);
        check (near (after.x, expectedX (c, 4000.0f), 1.0e-3f, 0.05f), "host move: node x follows the frequency", num (after.x) + " vs " + num (expectedX (c, 4000.0f)));
        check (near (after.y, expectedY (c, 8.0f, defaultRangeDb), 1.0e-3f, 0.05f), "host move: node y follows the gain");
        check (near (rig.plain (band, BandParam::freq), 4000.0f, 1.0e-4f), "host move: the plain value is exact", num (rig.plain (band, BandParam::freq)));

        // and the mouse now grabs it at the new place, not the old one
        mouse.move (before);
        check (c.getHoverBand() != band, "host move: the old position no longer hits the node");
        mouse.move (after);
        check (c.getHoverBand() == band, "host move: the new position hits the node");
    }

    // a host switches a band on and off: the node appears / disappears
    {
        const int band = 7;
        const juce::Point<float> where (c.getPlotBounds().getCentreX() + 33.0f, c.getPlotBounds().getY() + 41.0f);
        auto& on = rig.processor.getCoreParameter (band, BandParam::on);
        auto& freq = rig.processor.getCoreParameter (band, BandParam::freq);
        auto& type = rig.processor.getCoreParameter (band, BandParam::type);
        type.setPlainNotifyingHost (static_cast<float> (BandType::highShelf));
        freq.setPlainNotifyingHost (expectedHz (c, where.x));
        rig.processor.getCoreParameter (band, BandParam::gain).setPlainNotifyingHost (expectedGainDb (c, where.y, defaultRangeDb));
        rig.refresh();
        mouse.move (where);
        check (c.getHoverBand() == -1, "host: a disabled band has no node");
        on.setPlainNotifyingHost (1.0f);
        rig.refresh();
        mouse.move (where);
        check (c.getHoverBand() == band, "host: enabling a band makes its node appear at the right place");
        on.setPlainNotifyingHost (0.0f);
        rig.refresh();
        mouse.move (where);
        check (c.getHoverBand() == -1, "host: disabling a band removes its node");
    }

    // host automation while the user drags another band: no interference between the two
    {
        const int dragged = 0, automated = 1;
        const juce::Point<float> p = c.getNodeCentre (dragged);
        mouse.down (p);
        mouse.drag ({ p.x + 20.0f, p.y });
        rig.processor.getCoreParameter (automated, BandParam::freq).setPlainNotifyingHost (2500.0f);
        mouse.drag ({ p.x + 40.0f, p.y });
        mouse.up ({ p.x + 40.0f, p.y });
        check (near (rig.plain (automated, BandParam::freq), 2500.0f, 1.0e-4f), "host automation during a drag of another band is kept");
        check (near (rig.plain (dragged, BandParam::freq), expectedHz (c, p.x + 40.0f), 2.0e-3f), "the dragged band follows the mouse regardless");
    }

    // the strip: controls reflect the selected band
    {
        rig.apply (standardSetting());
        const auto slider = [&] (BandParam k) -> juce::Slider& { return static_cast<juce::Slider&> (rig.strip->getControl (k)); };
        const auto combo = [&] (BandParam k) -> juce::ComboBox& { return static_cast<juce::ComboBox&> (rig.strip->getControl (k)); };
        auto& onButton = static_cast<juce::Button&> (rig.strip->getControl (BandParam::on));

        for (int band : { 0, 2, 4, 6 })
        {
            mouse.down (c.getNodeCentre (band));
            mouse.up (c.getNodeCentre (band));
            const juce::String name = "band " + juce::String (band + 1);
            check (c.getSelectedBand() == band && rig.strip->getSelectedBand() == band, "strip follows a click on the node, " + name);
            check (near (static_cast<float> (slider (BandParam::freq).getValue()), rig.plain (band, BandParam::freq), 1.0e-4f), "strip: frequency slider shows the band, " + name);
            check (near (static_cast<float> (slider (BandParam::gain).getValue()), rig.plain (band, BandParam::gain), 1.0e-4f, 1.0e-4f), "strip: gain slider shows the band, " + name);
            check (near (static_cast<float> (slider (BandParam::q).getValue()), rig.plain (band, BandParam::q), 1.0e-4f), "strip: Q slider shows the band, " + name);
            check (combo (BandParam::type).getSelectedId() == static_cast<int> (rig.plain (band, BandParam::type)) + 1, "strip: type box shows the band, " + name);
            check (combo (BandParam::place).getSelectedId() == static_cast<int> (rig.plain (band, BandParam::place)) + 1, "strip: placement box shows the band, " + name);
            check (combo (BandParam::slope).getSelectedId() == static_cast<int> (rig.plain (band, BandParam::slope)) + 1, "strip: slope box shows the band, " + name);
            check (onButton.getToggleState(), "strip: the on button is lit, " + name);
            const BandType t = static_cast<BandType> (static_cast<int> (rig.plain (band, BandParam::type)));
            const bool hasGain = t == BandType::bell || t == BandType::lowShelf || t == BandType::highShelf;
            const bool isCut = t == BandType::lowCut || t == BandType::highCut;
            check (slider (BandParam::gain).isEnabled() == hasGain, "strip: gain slider enabled only for bell / shelf, " + name);
            check (combo (BandParam::slope).isEnabled() == isCut, "strip: slope box enabled only for cuts, " + name);
        }

        // the other direction: the strip's controls move the band and the canvas follows
        {
            const int band = 0;
            c.setSelectedBand (band);
            rig.log.reset();
            slider (BandParam::freq).setValue (500.0, juce::sendNotificationSync);
            check (near (rig.plain (band, BandParam::freq), 500.0f, 1.0e-4f), "strip: moving the frequency slider sets the parameter", num (rig.plain (band, BandParam::freq)));
            rig.refresh();
            check (near (c.getNodeCentre (band).x, expectedX (c, 500.0f), 1.0e-3f, 0.05f), "strip: the canvas node follows the strip slider");
            combo (BandParam::type).setSelectedId (static_cast<int> (BandType::notch) + 1, juce::sendNotificationSync);
            check (static_cast<int> (rig.plain (band, BandParam::type)) == static_cast<int> (BandType::notch), "strip: the type box sets the parameter");
            rig.refresh();
            check (near (c.getNodeCentre (band).y, c.getPlotBounds().getCentreY(), 1.0e-4f, 0.01f), "strip: the node of a notch sits on the 0 dB line");
            check (!slider (BandParam::gain).isEnabled(), "strip: the gain slider is greyed for a notch (after refresh)");
            onButton.setToggleState (false, juce::sendNotificationSync);
            check (!rig.isOn (band), "strip: the on button disables the band");
            rig.refresh();
            mouse.move (c.getNodeCentre (band));
            check (c.getHoverBand() == -1, "strip: the node of a band switched off in the strip is gone");
        }

        // selection by the strip's chip is reflected by the canvas through onBandChosen
        rig.strip->onBandChosen (3);
        check (c.getSelectedBand() == 3 && rig.strip->getSelectedBand() == 3, "strip: choosing a band chip selects it on the canvas");
    }

    // a preset / state load (setCurrentProgram) moves every node on the next refresh
    {
        rig.apply (standardSetting());
        rig.processor.setCurrentProgram (1);
        rig.refresh();
        kickeq::ParamValues now;
        rig.processor.readValues (now);
        bool ok = true;
        for (int b = 0; b < kickeq::numBands; ++b)
        {
            const kickeq::BandParams bp = now.band (b);
            mouse.move (c.getNodeCentre (b));
            if (bp.on)
            {
                const bool atFreq = near (c.getNodeCentre (b).x, expectedX (c, bp.freqHz), 1.0e-3f, 0.05f);
                ok = ok && atFreq;
            }
        }
        check (ok, "preset change: every enabled node sits at its new frequency after the refresh");
    }

    // rendering with the state left behind by all of the above still works
    {
        const juce::Image image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
        check (image.isValid() && image.getWidth() == KickEQEditor::defaultWidth, "rendering the editor after the interaction works");
    }
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testDragFrequencyAndGain();
    testDragRobustness();
    testQ();
    testAddRemove();
    testHoverAndHit();
    testMenu();
    testHostSide();

    std::printf ("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
