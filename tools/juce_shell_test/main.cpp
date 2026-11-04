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

// kickeq_juce_shell_test - tests of the JUCE shell that the CLAP conformance host cannot reach: bus layouts (mono),
// the VST3-level parameter objects, programs, host notification, the editor state chunk and the analyser hook.
// The processor is driven directly (no plugin wrapper). Exit code 0 when every check passed.

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "PluginProcessor.h"
#include "kickeq/analyzer.h"
#include "kickeq/eq_engine.h"
#include "kickeq/presets.h"
#include "kickeq/state.h"

namespace
{
int passed = 0, failed = 0;

void check (bool ok, const char* name, const juce::String& detail = {})
{
    if (ok)
    {
        ++passed;
        std::printf ("PASS  %s\n", name);
    }
    else
    {
        ++failed;
        std::printf ("FAIL  %s%s%s\n", name, detail.isEmpty() ? "" : "  --  ", detail.toRawUTF8());
    }
}

struct Rng
{
    uint32_t s;
    explicit Rng (uint32_t seed) : s (seed) {}
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni() { return static_cast<float> (next() >> 8) / 16777216.0f; }
    float bipolar() { return uni() - 0.5f; }
};

bool bitEqual (const float* a, const float* b, int n)
{
    return std::memcmp (a, b, static_cast<size_t> (n) * sizeof (float)) == 0;
}

kickeq::ParamValues randomValues (uint32_t seed)
{
    Rng rng (seed);
    kickeq::ParamValues v;
    for (int i = 0; i < kickeq::numParams; ++i)
    {
        const kickeq::ParamInfo& p = kickeq::paramInfo (i);
        v.set (i, p.min + rng.uni() * (p.max - p.min));
    }
    return v;
}

std::vector<uint8_t> blobOf (const kickeq::ParamValues& v, const uint8_t* ui = nullptr, uint32_t uiSize = 0)
{
    kickeq::UiChunk chunk;
    chunk.data = ui;
    chunk.size = uiSize;
    std::vector<uint8_t> blob (kickeq::saveState (v, nullptr, 0, ui != nullptr ? &chunk : nullptr));
    kickeq::saveState (v, blob.data(), blob.size(), ui != nullptr ? &chunk : nullptr);
    return blob;
}

kickeq::ParamValues valuesOf (const KickEQProcessor& p)
{
    kickeq::ParamValues v;
    p.readValues (v);
    return v;
}

struct Counter final : juce::AudioProcessorParameter::Listener
{
    int changes = 0;
    void parameterValueChanged (int, float) override { ++changes; }
    void parameterGestureChanged (int, bool) override {}
};

juce::AudioProcessor::BusesLayout layoutOf (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add (in);
    l.outputBuses.add (out);
    return l;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    using namespace kickeq;

    KickEQProcessor proc;

    // ---- parameters ----------------------------------------------------------------------------------------------
    {
        const auto& params = proc.getParameters();
        bool idsOk = params.size() == numParams, rangesOk = true, textOk = true, defaultsOk = true, norm = true;
        for (int i = 0; i < numParams && idsOk; ++i)
        {
            auto* p = dynamic_cast<juce::RangedAudioParameter*> (params[i]);
            if (p == nullptr || p->paramID != paramInfo (i).strId || p->getVersionHint() != 1)
            {
                idsOk = false;
                break;
            }
            const ParamInfo& info = paramInfo (i);
            rangesOk = rangesOk && juce::exactlyEqual (p->getNormalisableRange().start, info.min)
                       && juce::exactlyEqual (p->getNormalisableRange().end, info.max);
            defaultsOk = defaultsOk && std::abs (p->convertFrom0to1 (p->getDefaultValue()) - info.def) <= 1.0e-5f * juce::jmax (1.0f, std::abs (info.def))
                         && juce::exactlyEqual (proc.getCoreParameter (i).getPlain(), info.def);
            for (float n : { 0.0f, 0.25f, 0.5f, 0.9f, 1.0f })
            {
                char buf[64];
                valueToText (info, fromNormalized (info, n), buf, sizeof buf);
                textOk = textOk && p->getText (n, 0) == juce::String (juce::CharPointer_UTF8 (buf));
                const float rt = p->convertTo0to1 (p->convertFrom0to1 (n));
                const bool good = std::abs (rt - n) < (info.kind == Kind::Float ? 1.0e-5f : 0.5f / static_cast<float> (info.max - info.min) + 1.0e-5f);
                if (!good)
                    std::printf ("      %s n=%g -> %g\n", info.strId, static_cast<double> (n), static_cast<double> (rt));
                norm = norm && good;
            }
            if (info.kind != Kind::Float)
                textOk = textOk && p->getAllValueStrings().size() == info.numEnumLabels && p->isDiscrete()
                         && p->getNumSteps() == info.numEnumLabels;
        }
        check (idsOk, "59 parameters, ids == core strId (version hint 1)");
        check (rangesOk, "parameter ranges == core table");
        check (defaultsOk, "defaults == core table (plain value exact)");
        check (textOk, "text and enum labels == core valueToText");
        check (norm, "NormalisableRange conversion round trips");
        check (proc.getBypassParameter() == &proc.getCoreParameter (paramBypass), "host bypass parameter is the core bypass parameter");
        check (proc.getNumPrograms() == numPresets && proc.getProgramName (1) == "Kick Punch", "factory presets are exposed as programs");
    }

    // ---- bus layouts -------------------------------------------------------------------------------------------------
    {
        using Set = juce::AudioChannelSet;
        check (proc.checkBusesLayoutSupported (layoutOf (Set::stereo(), Set::stereo())), "layout stereo -> stereo supported");
        check (proc.checkBusesLayoutSupported (layoutOf (Set::mono(), Set::mono())), "layout mono -> mono supported");
        check (!proc.checkBusesLayoutSupported (layoutOf (Set::mono(), Set::stereo())), "layout mono -> stereo refused");
        check (!proc.checkBusesLayoutSupported (layoutOf (Set::stereo(), Set::mono())), "layout stereo -> mono refused");
        check (!proc.checkBusesLayoutSupported (layoutOf (Set::create5point1(), Set::create5point1())), "layout 5.1 refused");
    }

    // ---- state: shell-independent blob ---------------------------------------------------------------------------
    {
        bool exact = true, resave = true;
        for (uint32_t seed = 1; seed <= 20; ++seed)
        {
            const ParamValues want = randomValues (seed);
            const auto blob = blobOf (want);
            proc.setStateInformation (blob.data(), static_cast<int> (blob.size()));
            exact = exact && valuesOf (proc) == want;

            juce::MemoryBlock saved;
            proc.getStateInformation (saved);
            ParamValues again;
            UiChunk ui;
            resave = resave && loadState (static_cast<const uint8_t*> (saved.getData()), saved.getSize(), again, &ui) && again == want
                     && ui.size >= 1;
        }
        check (exact, "setStateInformation: a core blob gives bit-identical parameter values (20 random states)");
        check (resave, "getStateInformation: core blob again, same values, with the UI01 chunk");

        const ParamValues keep = randomValues (77);
        const auto good = blobOf (keep);
        proc.setStateInformation (good.data(), static_cast<int> (good.size()));
        bool untouched = true;
        for (size_t len = 0; len < good.size(); len += 7)
        {
            proc.setStateInformation (good.data(), static_cast<int> (len));
            untouched = untouched && valuesOf (proc) == keep;
        }
        const uint8_t junk[12] = { 'x', 'y', 'z', 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        proc.setStateInformation (junk, sizeof junk);
        proc.setStateInformation (nullptr, 0);
        check (untouched && valuesOf (proc) == keep, "truncated / garbage state: ignored, parameters untouched");

        // editor state chunk round trip (analyser source / range, slope compensation, curve range)
        const uint8_t uiBytes[5] = { 1, 2, 0, 2, 3 };
        const auto withUi = blobOf (keep, uiBytes, sizeof uiBytes);
        proc.setStateInformation (withUi.data(), static_cast<int> (withUi.size()));
        const auto settings = proc.getEditorSettings();
        check (settings.analyzerSource == 2 && settings.analyzerRange == 0 && settings.slopeComp == 2 && settings.curveRange == 3,
               "UI01 chunk: editor settings restored");
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        ParamValues back;
        UiChunk ui;
        loadState (static_cast<const uint8_t*> (saved.getData()), saved.getSize(), back, &ui);
        check (back == keep && ui.size >= 5 && std::memcmp (ui.data, uiBytes, 5) == 0, "UI01 chunk round trip");

        // a state without the chunk (native shell) keeps working: the editor settings fall back to the defaults
        const auto plain = blobOf (keep);
        proc.setStateInformation (plain.data(), static_cast<int> (plain.size()));
        check (proc.getEditorSettings() == kickeq::ui::EditorSettings {}, "state without a UI01 chunk: default editor settings");
    }

    // ---- notification of listeners / host --------------------------------------------------------------------------
    {
        proc.setStateInformation (blobOf (ParamValues {}).data(), static_cast<int> (blobOf (ParamValues {}).size()));
        Counter counter;
        proc.getCoreParameter (paramOutGain).addListener (&counter);
        proc.getCoreParameter (paramAutoGain).addListener (&counter);
        ParamValues v;
        v.set (paramOutGain, -3.5f);
        const auto blob = blobOf (v);
        proc.setStateInformation (blob.data(), static_cast<int> (blob.size()));
        check (counter.changes >= 1 && juce::exactlyEqual (proc.getCoreParameter (paramOutGain).getPlain(), -3.5f),
               "setStateInformation notifies parameter listeners (value exact)");
        proc.getCoreParameter (paramOutGain).removeListener (&counter);
        proc.getCoreParameter (paramAutoGain).removeListener (&counter);

        // a host that echoes the normalised value it read must not drift the plain value
        CoreParameter& freq = proc.getCoreParameter (2, BandParam::freq);
        freq.setPlainNotifyingHost (1234.5f);
        const float n = freq.getValue();
        freq.setValueNotifyingHost (n);
        freq.setValueNotifyingHost (n);
        check (juce::exactlyEqual (freq.getPlain(), 1234.5f), "echo of the normalised value keeps the exact plain value");
        freq.setValue (std::nanf (""));
        check (juce::exactlyEqual (freq.getPlain(), 1234.5f), "NaN from a host is ignored");
    }

    // ---- programs -------------------------------------------------------------------------------------------------------
    {
        bool ok = true;
        for (int i = 0; i < numPresets; ++i)
        {
            proc.setCurrentProgram (i);
            ParamValues want;
            applyPreset (i, want);
            ok = ok && valuesOf (proc) == want && proc.getCurrentProgram() == i && proc.findMatchingPreset() == i;
        }
        check (ok, "setCurrentProgram applies the core preset exactly; getCurrentProgram / findMatchingPreset agree");
        proc.setCurrentProgram (3);
        proc.getCoreParameter (0, BandParam::freq).setPlainNotifyingHost (31.0f);
        check (proc.findMatchingPreset() == -1, "a modified preset no longer matches");
    }

    // ---- audio: stereo, mono, tail, analyser ------------------------------------------------------------------------
    constexpr double rate = 48000.0;
    {
        const ParamValues setup = randomValues (5);
        const auto blob = blobOf (setup);
        proc.setPlayConfigDetails (2, 2, rate, 512);
        proc.prepareToPlay (rate, 512);
        proc.setStateInformation (blob.data(), static_cast<int> (blob.size()));

        Rng rng (9);
        const int total = 60000;
        std::vector<float> inL (total), inR (total), refL, refR;
        for (int i = 0; i < total; ++i)
        {
            inL[static_cast<size_t> (i)] = rng.bipolar();
            inR[static_cast<size_t> (i)] = rng.bipolar();
        }
        refL = inL;
        refR = inR;
        EqEngine ref;
        ref.prepare (rate, 512);
        ParamValues pv = setup;
        ref.process (pv, refL.data(), refR.data(), total);

        std::vector<float> outL = inL, outR = inR;
        juce::MidiBuffer midi;
        const int blocks[5] = { 480, 64, 1, 777, 2048 };
        int pos = 0, bi = 0;
        while (pos < total)
        {
            const int n = juce::jmin (blocks[bi++ % 5], total - pos);
            float* channels[2] = { outL.data() + pos, outR.data() + pos };
            juce::AudioBuffer<float> buffer (channels, 2, n);
            proc.processBlock (buffer, midi);
            pos += n;
        }
        check (bitEqual (outL.data(), refL.data(), total) && bitEqual (outR.data(), refR.data(), total),
               "stereo processBlock (mixed block sizes) == core EqEngine, bit-identical");

        // tail: reported from the engine (a narrow low bell rings for a while)
        check (proc.getTailLengthSeconds() > 0.0 && proc.getTailLengthSeconds() <= 10.0, "tail length comes from the engine",
               juce::String (proc.getTailLengthSeconds()));
        check (proc.getLatencySamples() == 0, "latency 0");

        // mono: dual-mono engine, bit-identical to processMono
        juce::AudioProcessor::BusesLayout mono = layoutOf (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono());
        check (proc.setBusesLayout (mono), "setBusesLayout (mono -> mono)");
        proc.prepareToPlay (rate, 512);
        proc.setStateInformation (blob.data(), static_cast<int> (blob.size()));
        std::vector<float> monoIn (30000), monoOut, monoRef;
        for (float& x : monoIn)
            x = rng.bipolar();
        monoOut = monoIn;
        monoRef = monoIn;
        EqEngine monoEngine;
        monoEngine.prepare (rate, 512);
        ParamValues mv = setup;
        monoEngine.processMono (mv, monoRef.data(), static_cast<int> (monoRef.size()));
        for (size_t p = 0; p < monoOut.size(); p += 500)
        {
            float* channels[1] = { monoOut.data() + p };
            juce::AudioBuffer<float> buffer (channels, 1, 500);
            proc.processBlock (buffer, midi);
        }
        check (proc.getTotalNumInputChannels() == 1 && bitEqual (monoOut.data(), monoRef.data(), static_cast<int> (monoOut.size())),
               "mono processBlock == core processMono, bit-identical");
        check (proc.setBusesLayout (layoutOf (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())), "back to stereo");

        // analyser: idle while no editor has enabled it, running when enabled
        proc.prepareToPlay (rate, 512);
        SpectrumAnalyzer& an = proc.getAnalyzer();
        SpectrumFrame frame;
        std::vector<float> l (512), r (512);
        for (int pass = 0; pass < 2; ++pass)
        {
            an.setEnabled (pass == 1);
            for (int b = 0; b < 100; ++b)
            {
                for (size_t i = 0; i < 512; ++i)
                    l[i] = r[i] = 0.3f * std::sin (0.05f * static_cast<float> (i + static_cast<size_t> (b) * 512));
                float* channels[2] = { l.data(), r.data() };
                juce::AudioBuffer<float> buffer (channels, 2, 512);
                proc.processBlock (buffer, midi);
            }
            an.process();
            const bool got = an.pull (frame);
            check (got == (pass == 1), pass == 0 ? "analyser disabled: nothing is pushed" : "analyser enabled: a frame is produced");
        }
        an.setEnabled (false);
    }

    std::printf ("\n%d checks passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
