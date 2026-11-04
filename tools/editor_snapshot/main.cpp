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

// kickeq_editor_snapshot - renders the KickEQ editor offscreen to PNG files.
//
//   kickeq_editor_snapshot [OUTPUT_DIR] [--check] [--ms] [--bypass] [--phase MODE] [--hover BAND] [--prefix NAME]
//
// Instantiates the processor and its editor without any window, loads a multi-band setting, pushes one second of
// pink-ish noise through processBlock so that the analyser has data, drives the editor's 30 Hz refresh by hand and
// writes editor-default.png (960 x 560) and editor-small.png (720 x 420, the minimum size) with
// createComponentSnapshot. --check additionally verifies that the images exist, have the expected size and are
// not blank (used as a ctest smoke test); the exit code is non-zero on failure.
//   --ms       mid/side demo: bands 4 and 5 on Mid / Side, curve view Mid
//   --bypass   bypass on (the curve is dimmed)
//   --phase N  phase mode 0..3 (Minimum / Linear 2048 / 4096 / 8192; ignored when the core has no linear phase)
//   --hover N  highlight band N (1..8) like a hovering mouse (default 4)
//   --prefix   file name prefix (default "editor")

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "Editor.h"
#include "PluginProcessor.h"

namespace
{
/** Paul Kellet's economy pink noise filter on white noise from a xorshift generator. */
struct PinkNoise
{
    uint32_t state;
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;

    explicit PinkNoise (uint32_t seed) : state (seed) {}

    float next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const float white = static_cast<float> (static_cast<int32_t> (state)) * (1.0f / 2147483648.0f);
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        return (b0 + b1 + b2 + white * 0.1848f) * 0.11f;
    }
};

kickeq::ParamValues demoSetting (bool midSide, bool bypass, int phase)
{
    using namespace kickeq;
    ParamValues v;
    const auto band = [&v] (int b, BandType t, float f, float g, float q, Placement p = Placement::stereo, int slope = 1) {
        v.set (b, BandParam::on, 1.0f);
        v.set (b, BandParam::type, static_cast<float> (t));
        v.set (b, BandParam::freq, f);
        v.set (b, BandParam::gain, g);
        v.set (b, BandParam::q, q);
        v.set (b, BandParam::place, static_cast<float> (p));
        v.set (b, BandParam::slope, static_cast<float> (slope));
    };
    band (0, BandType::lowCut, 32.0f, 0.0f, 0.707f, Placement::stereo, 3);
    band (1, BandType::lowShelf, 90.0f, 4.5f, 0.71f);
    band (2, BandType::bell, 280.0f, -4.0f, 1.3f);
    band (3, BandType::bell, 1100.0f, 3.0f, 1.8f);
    band (4, BandType::bell, 3200.0f, -2.5f, 2.4f);
    band (5, BandType::bell, 6500.0f, 2.0f, 1.0f);
    band (6, BandType::highShelf, 11000.0f, 3.5f, 0.71f);
    band (7, BandType::highCut, 20000.0f, 0.0f, 0.707f, Placement::stereo, 1);
    if (midSide)
    {
        v.set (3, BandParam::place, static_cast<float> (Placement::mid));
        v.set (4, BandParam::place, static_cast<float> (Placement::side));
        v.set (5, BandParam::place, static_cast<float> (Placement::left));
    }
    v.set (paramBypass, bypass ? 1.0f : 0.0f);
    v.set (paramPhaseMode, static_cast<float> (phase));
    return v;
}

bool writePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (!out.openedOk())
        return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream (image, out);
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::File outDir = juce::File::getCurrentWorkingDirectory();
    bool check = false, midSide = false, bypass = false;
    int hover = 3, phase = 0;
    juce::String prefix = "editor";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--check") == 0)
            check = true;
        else if (std::strcmp (argv[i], "--ms") == 0)
            midSide = true;
        else if (std::strcmp (argv[i], "--bypass") == 0)
            bypass = true;
        else if (std::strcmp (argv[i], "--phase") == 0 && i + 1 < argc)
            phase = juce::jlimit (0, kickeq::numPhaseModes - 1, std::atoi (argv[++i]));
        else if (std::strcmp (argv[i], "--hover") == 0 && i + 1 < argc)
            hover = juce::jlimit (1, kickeq::numBands, std::atoi (argv[++i])) - 1;
        else if (std::strcmp (argv[i], "--prefix") == 0 && i + 1 < argc)
            prefix = juce::String::fromUTF8 (argv[++i]);
        else
            outDir = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[i]));
    }
    if (!outDir.createDirectory())
    {
        std::fprintf (stderr, "cannot create %s\n", outDir.getFullPathName().toRawUTF8());
        return 1;
    }

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    KickEQProcessor processor;
    processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);
    processor.applyValues (demoSetting (midSide, bypass, phase));

    std::unique_ptr<juce::AudioProcessorEditor> base (processor.createEditor());
    auto* editor = dynamic_cast<KickEQEditor*> (base.get());
    if (editor == nullptr)
        return 1;

    // one second of audio so the analyser (enabled by the editor) has a full spectrum
    PinkNoise noiseL (12345u), noiseR (67890u);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int done = 0; done < static_cast<int> (sampleRate); done += blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, noiseL.next());
            buffer.setSample (1, i, noiseR.next());
        }
        processor.processBlock (buffer, midi);
        if ((done / blockSize) % 8 == 7)
            editor->refreshFrame(); // what the 30 Hz timer would do while the audio runs
    }
    editor->refreshFrame();
    editor->previewBand (hover, midSide ? kickeq::ChannelView::mid : kickeq::ChannelView::stereo);

    struct Shot { const char* name; int w, h; };
    int failures = 0;
    for (const Shot& shot : { Shot { "default", KickEQEditor::defaultWidth, KickEQEditor::defaultHeight },
                              Shot { "small", KickEQEditor::minWidth, KickEQEditor::minHeight } })
    {
        editor->setSize (shot.w, shot.h);
        editor->refreshFrame();
        const juce::Image image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        const juce::File file = outDir.getChildFile (prefix + "-" + shot.name + ".png");
        const bool written = image.isValid() && writePng (image, file);
        std::printf ("%s %s (%d x %d px)\n", written ? "wrote" : "FAILED", file.getFullPathName().toRawUTF8(),
                     image.getWidth(), image.getHeight());
        if (!written)
        {
            ++failures;
            continue;
        }

        if (check)
        {
            // size and "not blank": the canvas plus the controls must produce plenty of distinct colours
            juce::Array<juce::uint32> seen;
            for (int y = 0; y < image.getHeight(); y += 7)
                for (int x = 0; x < image.getWidth(); x += 7)
                    seen.addIfNotAlreadyThere (image.getPixelAt (x, y).getARGB());
            const bool sizeOk = image.getWidth() == shot.w * 2 && image.getHeight() == shot.h * 2;
            const bool contentOk = seen.size() > 40 && file.getSize() > 20000;
            std::printf ("  check: size %s, content %s (%d distinct colours, %lld bytes)\n", sizeOk ? "ok" : "WRONG",
                         contentOk ? "ok" : "BLANK", seen.size(), static_cast<long long> (file.getSize()));
            failures += (sizeOk ? 0 : 1) + (contentOk ? 0 : 1);
        }
    }

    base.reset(); // editor first (it switches the analyser off), then the processor
    return failures == 0 ? 0 : 1;
}
