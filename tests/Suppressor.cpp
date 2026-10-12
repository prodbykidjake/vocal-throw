#include "helpers/signal_helpers.h"
#include "helpers/test_helpers.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <dsp/MinPhaseFilter.h>
#include <dsp/ReductionCurve.h>

using namespace testing_helpers;

namespace
{
constexpr size_t kTotal = 96000;       // 2 s
constexpr size_t kMeasureStart = 48000; // measure the second half, once everything has settled
constexpr size_t kMeasureLen = 48000;
constexpr double kToneHz = 3000.0;      // exactly on bin 128 at 48 kHz

std::vector<float> noiseWithTone (float toneAmplitude, float noiseRms = 0.05f)
{
    auto x = whiteNoise ((int) kTotal, noiseRms);
    addSine (x, kToneHz, toneAmplitude);
    return x;
}

double toneReductionDb (const std::vector<float>& in, const std::vector<float>& out)
{
    return toDb (tonePower (in, kMeasureStart, kMeasureLen, kToneHz) / tonePower (out, kMeasureStart + 0, kMeasureLen, kToneHz));
}

double controlReductionDb (const std::vector<float>& in, const std::vector<float>& out)
{
    return toDb (bandPower (in, kMeasureStart, kMeasureLen, 1000.0, 2000.0) / bandPower (out, kMeasureStart, kMeasureLen, 1000.0, 2000.0));
}
} // namespace

TEST_CASE ("Parameters exist with sane defaults", "[params]")
{
    PluginProcessor plugin;
    for (const auto* id : { ParamIDs::mode, ParamIDs::depth, ParamIDs::detail, ParamIDs::attack, ParamIDs::release, ParamIDs::maxCut,
             ParamIDs::wetTrim, ParamIDs::mix, ParamIDs::outGain, ParamIDs::stereoMode, ParamIDs::link, ParamIDs::delta, ParamIDs::bypass })
        CHECK (plugin.apvts.getParameter (id) != nullptr);

    CHECK (getParam (plugin, ParamIDs::mix) == Catch::Approx (100.0f));
    CHECK (getParam (plugin, ParamIDs::maxCut) == Catch::Approx (12.0f));
    CHECK (getParam (plugin, ParamIDs::bypass) == Catch::Approx (0.0f));
}

TEST_CASE ("Minimum-phase filter", "[filter]")
{
    const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
    vs::MinPhaseFilter filter;
    filter.prepare (cfg);
    std::vector<float> curve ((size_t) cfg.numBins, 0.0f);

    SECTION ("a flat 0 dB curve is the identity")
    {
        filter.setCurve (curve.data());
        auto x = whiteNoise (4096, 0.1f);
        for (size_t i = 0; i < x.size(); ++i)
            CHECK (filter.process (x[i]) == Catch::Approx (x[i]).margin (1.0e-4));
    }

    SECTION ("a flat 12 dB cut scales the signal")
    {
        std::fill (curve.begin(), curve.end(), 12.0f);
        filter.setCurve (curve.data());
        auto x = whiteNoise (8192, 0.1f);
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            y[i] = filter.process (x[i]);

        const auto expected = juce::Decibels::decibelsToGain (-12.0f);
        for (size_t i = 4096; i < x.size(); ++i)
            CHECK (y[i] == Catch::Approx (x[i] * expected).margin (2.0e-3));
    }

    SECTION ("a notch has the requested depth, and the kernel is minimum phase (energy up front)")
    {
        // 20 dB, about 1/3 octave wide around bin 128 (3 kHz)
        for (int k = 96; k <= 160; ++k)
            curve[(size_t) k] = 20.0f * 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) (k - 128) / 32.0f));

        filter.setCurve (curve.data());
        std::vector<float> response;
        filter.getCurrentResponse (response);

        CHECK (juce::Decibels::gainToDecibels (response[128]) == Catch::Approx (-20.0f).margin (1.5f));
        CHECK (juce::Decibels::gainToDecibels (response[20]) == Catch::Approx (0.0f).margin (0.5f));
        CHECK (juce::Decibels::gainToDecibels (response[600]) == Catch::Approx (0.0f).margin (0.5f));
    }
}

TEST_CASE ("Latency is reported and real", "[latency]")
{
    PluginProcessor plugin;
    setParam (plugin, ParamIDs::depth, 0.0f);

    std::vector<float> impulse (8192, 0.0f);
    impulse[100] = 1.0f;
    const auto out = run (plugin, impulse);

    const auto peak = std::distance (out.begin(), std::max_element (out.begin(), out.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }));
    CHECK (plugin.getLatencySamples() == 1024);
    CHECK ((int) peak == 100 + plugin.getLatencySamples());
    CHECK (out[(size_t) peak] == Catch::Approx (1.0f).margin (1.0e-3));
}

TEST_CASE ("Silence and extreme settings stay finite", "[robustness]")
{
    PluginProcessor plugin;

    SECTION ("silence in, silence out")
    {
        const auto out = run (plugin, std::vector<float> (24000, 0.0f));
        CHECK (allFinite (out));
        CHECK (juce::exactlyEqual (*std::max_element (out.begin(), out.end()), 0.0f));
    }

    SECTION ("everything maxed")
    {
        for (const auto* id : { ParamIDs::depth, ParamIDs::detail, ParamIDs::attack, ParamIDs::release })
            setParam (plugin, id, 100.0f);
        setParam (plugin, ParamIDs::maxCut, 40.0f);
        setParam (plugin, ParamIDs::mode, 1.0f);
        setParam (plugin, ParamIDs::stereoMode, 1.0f);
        setParam (plugin, ParamIDs::wetTrim, 12.0f);
        setParam (plugin, ParamIDs::outGain, 18.0f);
        CHECK (allFinite (run (plugin, noiseWithTone (0.3f, 0.2f))));
    }

    SECTION ("everything minimal and fast")
    {
        for (const auto* id : { ParamIDs::depth, ParamIDs::detail, ParamIDs::attack, ParamIDs::release, ParamIDs::link })
            setParam (plugin, id, 0.0f);
        setParam (plugin, ParamIDs::maxCut, 1.0f);
        CHECK (allFinite (run (plugin, noiseWithTone (0.3f, 0.2f))));
    }

    SECTION ("full-scale square wave")
    {
        std::vector<float> square (48000);
        for (size_t i = 0; i < square.size(); ++i)
            square[i] = (i / 50) % 2 == 0 ? 1.0f : -1.0f;
        setParam (plugin, ParamIDs::depth, 100.0f);
        CHECK (allFinite (run (plugin, square)));
    }
}

TEST_CASE ("Passthrough paths are transparent", "[passthrough]")
{
    PluginProcessor plugin;
    const auto in = noiseWithTone (0.05f);

    SECTION ("mix 0 is a latency-aligned copy")
    {
        setParam (plugin, ParamIDs::mix, 0.0f);
        const auto out = run (plugin, in);
        for (size_t i = 0; i + 1024 < in.size(); ++i)
            REQUIRE (juce::exactlyEqual (out[i + 1024], in[i]));
    }

    SECTION ("bypass is a latency-aligned copy")
    {
        setParam (plugin, ParamIDs::bypass, 1.0f);
        const auto out = run (plugin, in);
        for (size_t i = 0; i + 1024 < in.size(); ++i)
            REQUIRE (juce::exactlyEqual (out[i + 1024], in[i]));
    }

    SECTION ("depth 0 leaves the signal alone")
    {
        setParam (plugin, ParamIDs::depth, 0.0f);
        const auto out = run (plugin, in);
        for (size_t i = 0; i + 1024 < in.size(); ++i)
            REQUIRE (out[i + 1024] == Catch::Approx (in[i]).margin (1.0e-3));
    }
}

TEST_CASE ("A narrow resonance is reduced, the rest is left alone", "[detection]")
{
    PluginProcessor plugin;
    const auto in = noiseWithTone (0.01f);

    setParam (plugin, ParamIDs::depth, 100.0f);
    setParam (plugin, ParamIDs::detail, 70.0f);
    setParam (plugin, ParamIDs::maxCut, 40.0f);
    const auto out = run (plugin, in);

    // the output is delayed, so compare against the input shifted by the latency
    std::vector<float> aligned (in.size(), 0.0f);
    std::copy (in.begin(), in.end() - 1024, aligned.begin() + 1024);

    const auto tone = toneReductionDb (aligned, out);
    const auto control = controlReductionDb (aligned, out);
    INFO ("tone " << tone << " dB, control " << control << " dB");
    CHECK (tone > 6.0);
    CHECK (std::abs (control) < 3.0);
}

TEST_CASE ("Max cut caps the reduction", "[maxcut]")
{
    PluginProcessor plugin;
    const auto in = noiseWithTone (0.05f);
    std::vector<float> aligned (in.size(), 0.0f);
    std::copy (in.begin(), in.end() - 1024, aligned.begin() + 1024);

    setParam (plugin, ParamIDs::depth, 100.0f);
    setParam (plugin, ParamIDs::detail, 70.0f);
    setParam (plugin, ParamIDs::maxCut, 6.0f);
    const auto out = run (plugin, in);

    const auto tone = toneReductionDb (aligned, out);
    INFO ("tone " << tone << " dB");
    CHECK (tone > 2.0);
    CHECK (tone < 6.0 + 1.5);
}

TEST_CASE ("Soft ignores input level, Hard does not", "[modes]")
{
    const auto loud = noiseWithTone (0.01f);
    auto quiet = loud;
    for (auto& s : quiet)
        s *= 0.125f; // -18 dB

    auto reductionFor = [] (float modeIndex, const std::vector<float>& in) {
        PluginProcessor plugin;
        setParam (plugin, ParamIDs::mode, modeIndex);
        setParam (plugin, ParamIDs::depth, 60.0f);
        setParam (plugin, ParamIDs::detail, 70.0f);
        setParam (plugin, ParamIDs::maxCut, 40.0f);
        const auto out = run (plugin, in);
        std::vector<float> aligned (in.size(), 0.0f);
        std::copy (in.begin(), in.end() - 1024, aligned.begin() + 1024);
        return toneReductionDb (aligned, out);
    };

    const auto softLoud = reductionFor (0.0f, loud);
    const auto softQuiet = reductionFor (0.0f, quiet);
    const auto hardLoud = reductionFor (1.0f, loud);
    const auto hardQuiet = reductionFor (1.0f, quiet);
    INFO ("soft " << softLoud << " / " << softQuiet << ", hard " << hardLoud << " / " << hardQuiet);

    CHECK (std::abs (softLoud - softQuiet) < 1.5);
    CHECK (hardLoud - hardQuiet > 2.0);
}

TEST_CASE ("Delta is dry minus wet", "[delta]")
{
    const auto in = noiseWithTone (0.01f);

    PluginProcessor wetPlugin;
    setParam (wetPlugin, ParamIDs::depth, 80.0f);
    const auto wet = run (wetPlugin, in);

    PluginProcessor deltaPlugin;
    setParam (deltaPlugin, ParamIDs::depth, 80.0f);
    setParam (deltaPlugin, ParamIDs::delta, 1.0f);
    const auto delta = run (deltaPlugin, in);

    for (size_t i = 0; i + 1024 < in.size(); ++i)
        REQUIRE (wet[i + 1024] + delta[i + 1024] == Catch::Approx (in[i]).margin (1.0e-3));
}

TEST_CASE ("Stereo handling", "[stereo]")
{
    PluginProcessor plugin;
    setParam (plugin, ParamIDs::depth, 80.0f);

    const auto left = noiseWithTone (0.01f);
    const auto right = whiteNoise ((int) kTotal, 0.05f, 99);

    auto runStereo = [&] {
        plugin.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        plugin.prepareToPlay (kSampleRate, kBlockSize);
        juce::AudioBuffer<float> buffer (2, kBlockSize);
        juce::MidiBuffer midi;
        std::vector<float> outL (left.size()), outR (left.size());
        for (size_t pos = 0; pos + kBlockSize <= left.size(); pos += kBlockSize)
        {
            std::copy (left.begin() + (long) pos, left.begin() + (long) pos + kBlockSize, buffer.getWritePointer (0));
            std::copy (right.begin() + (long) pos, right.begin() + (long) pos + kBlockSize, buffer.getWritePointer (1));
            plugin.processBlock (buffer, midi);
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + kBlockSize, outL.begin() + (long) pos);
            std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + kBlockSize, outR.begin() + (long) pos);
        }
        return std::pair { outL, outR };
    };

    SECTION ("L/R, unlinked: a tone on the left is not removed from the right")
    {
        setParam (plugin, ParamIDs::link, 0.0f);
        const auto [outL, outR] = runStereo();
        CHECK (allFinite (outL));
        CHECK (allFinite (outR));
        // the right channel has no resonance, so its level should hardly change (the minimum-phase
        // filter changes the waveform, so compare rms rather than samples)
        auto rms = [] (const std::vector<float>& x, size_t offset) {
            double sum = 0.0;
            size_t n = 0;
            for (size_t i = 24000; i + offset < x.size(); ++i, ++n)
                sum += (double) x[i + offset] * (double) x[i + offset];
            return std::sqrt (sum / (double) n);
        };
        const auto changeDb = 20.0 * std::log10 (rms (outR, 0) / rms (right, 0));
        INFO ("right channel level change " << changeDb << " dB");
        CHECK (std::abs (changeDb) < 3.0);
    }

    SECTION ("M/S with full link produces finite output")
    {
        setParam (plugin, ParamIDs::stereoMode, 1.0f);
        const auto [outL, outR] = runStereo();
        CHECK (allFinite (outL));
        CHECK (allFinite (outR));
    }
}

TEST_CASE ("State round trip", "[state]")
{
    PluginProcessor a;
    setParam (a, ParamIDs::depth, 77.0f);
    setParam (a, ParamIDs::mode, 1.0f);
    setParam (a, ParamIDs::maxCut, 9.0f);

    juce::MemoryBlock state;
    a.getStateInformation (state);

    PluginProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    CHECK (getParam (b, ParamIDs::depth) == Catch::Approx (77.0f).margin (0.2f));
    CHECK (getParam (b, ParamIDs::mode) == Catch::Approx (1.0f));
    CHECK (getParam (b, ParamIDs::maxCut) == Catch::Approx (9.0f).margin (0.2f));
}

TEST_CASE ("Editor opens and lays out", "[editor]")
{
    runWithinPluginEditor ([] (PluginProcessor& plugin) {
        auto* editor = plugin.getActiveEditor();
        REQUIRE (editor != nullptr);
        CHECK (editor->getWidth() > 400);
        CHECK (editor->getHeight() > 300);

        auto snapshot = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        CHECK (snapshot.isValid());

        // set VOX_SNAPSHOT_PATH to look at the editor
        if (const auto path = juce::SystemStats::getEnvironmentVariable ("VOX_SNAPSHOT_PATH", {}); path.isNotEmpty())
        {
            juce::File file (path);
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat png;
            png.writeImageToStream (snapshot, stream);
        }
    });
}
