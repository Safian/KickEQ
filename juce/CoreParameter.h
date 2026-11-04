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

#include <atomic>

#include "kickeq/params.h"

/** One JUCE parameter generated from a row of kickeq::paramTable.
 *
 *  Stock AudioParameterFloat / Bool / Choice keep the value as a float *normalised* number and convert back with
 *  the range functions, so a plain value that goes through a host (CLAP plain -> normalised -> plain) comes back
 *  one ulp off. This class stores the plain value in a lock-free atomic instead:
 *
 *   - the audio thread reads exactly the value that was written by a state load (the native shell reads the same
 *     number, so both shells render bit-identical audio for the same state blob),
 *   - setValue (n) ignores a host echo of toNormalized (plain), so values do not drift when a host writes back
 *     the normalised value it just read.
 *
 *  The NormalisableRange uses the core's toNormalized / fromNormalized (log and dB scales), the text conversion is
 *  the core's valueToText / textToValue, enum and bool labels are the core labels. */
class CoreParameter final : public juce::RangedAudioParameter
{
public:
    explicit CoreParameter (int tableIndex)
        : juce::RangedAudioParameter (juce::ParameterID { kickeq::paramInfo (tableIndex).strId, 1 },
                                      kickeq::paramInfo (tableIndex).name,
                                      juce::AudioProcessorParameterWithIDAttributes()
                                          .withLabel (kickeq::paramInfo (tableIndex).unit)
                                          .withAutomatable ((kickeq::paramInfo (tableIndex).flags & kickeq::flagAutomatable) != 0)),
          index (tableIndex),
          info (kickeq::paramInfo (tableIndex)),
          range (makeRange (kickeq::paramInfo (tableIndex))),
          plain (kickeq::paramInfo (tableIndex).def)
    {
    }

    int getTableIndex() const noexcept { return index; }
    const kickeq::ParamInfo& getInfo() const noexcept { return info; }

    /** Exact plain value, safe from any thread. */
    float getPlain() const noexcept { return plain.load (std::memory_order_relaxed); }

    /** Stores a plain value (clamped, stepped parameters rounded) without telling anyone. */
    void setPlain (float v) noexcept { plain.store (kickeq::clampParam (info, v), std::memory_order_relaxed); }

    /** Stores a plain value and notifies host and listeners (message thread). */
    void setPlainNotifyingHost (float v)
    {
        setPlain (v);
        setValueNotifyingHost (getValue());
    }

    /** A complete automation gesture for a one-off change (menus, buttons). */
    void setPlainAsGesture (float v)
    {
        beginChangeGesture();
        setPlainNotifyingHost (v);
        endChangeGesture();
    }

    // ---- juce::RangedAudioParameter / AudioProcessorParameter -------------------------------------------------
    const juce::NormalisableRange<float>& getNormalisableRange() const override { return range; }

    float getValue() const override { return kickeq::toNormalized (info, getPlain()); }

    void setValue (float newValue) override
    {
        if (!(newValue == newValue))
            return;
        if (juce::exactlyEqual (newValue, getValue()))
            return; // echo of the value we already hold: keep the exact plain value
        plain.store (kickeq::fromNormalized (info, newValue), std::memory_order_relaxed);
    }

    float getDefaultValue() const override { return kickeq::toNormalized (info, info.def); }

    bool isDiscrete() const override { return info.kind != kickeq::Kind::Float; }
    bool isBoolean() const override { return info.kind == kickeq::Kind::Bool; }

    juce::String getText (float normalised, int maximumStringLength) const override
    {
        char text[64];
        kickeq::valueToText (info, kickeq::fromNormalized (info, normalised), text, sizeof text);
        const juce::String s { juce::CharPointer_UTF8 (text) };
        return maximumStringLength > 0 ? s.substring (0, maximumStringLength) : s;
    }

    float getValueForText (const juce::String& text) const override
    {
        float v = 0.0f;
        if (kickeq::textToValue (info, text.toRawUTF8(), v))
            return kickeq::toNormalized (info, v);
        return getValue();
    }

    juce::StringArray getAllValueStrings() const override
    {
        juce::StringArray all;
        if (info.kind != kickeq::Kind::Float && info.enumLabels != nullptr)
            for (int i = 0; i < info.numEnumLabels; ++i)
                all.add (info.enumLabels[i]);
        return all;
    }

private:
    static juce::NormalisableRange<float> makeRange (const kickeq::ParamInfo& p)
    {
        const kickeq::ParamInfo* table = &p; // points into the constexpr table: static storage, outlives the range
        juce::NormalisableRange<float> r (
            p.min, p.max,
            [table] (float, float, float normalised) { return kickeq::fromNormalized (*table, normalised); },
            [table] (float, float, float plainValue) { return kickeq::toNormalized (*table, plainValue); },
            [table] (float, float, float v) { return kickeq::clampParam (*table, v); });
        r.interval = p.kind == kickeq::Kind::Float ? 0.0f : 1.0f;
        return r;
    }

    const int index;
    const kickeq::ParamInfo& info;
    const juce::NormalisableRange<float> range;
    std::atomic<float> plain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoreParameter)
};
