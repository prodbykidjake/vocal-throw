#include "helpers/dsp_test_utils.h"
#include <PluginProcessor.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <dsp/Meters.h>
#include <engine/PresetManager.h>
#include <engine/RecommendationSession.h>

using Catch::Approx;

namespace
{
constexpr double fs = 48000.0;
constexpr int block = 480;

void prepare (PluginProcessor& p, int channels = 2)
{
    p.setPlayConfigDetails (channels, channels, fs, block);
    p.prepareToPlay (fs, block);
}

void setParam (PluginProcessor& p, const char* id, float v)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (v));
}

// Largest absolute difference between two parameter snapshots (plain values).
float maxDifference (const std::map<std::string, float>& a, const std::map<std::string, float>& b)
{
    float worst = a.size() == b.size() ? 0.0f : 1.0e9f;
    for (const auto& [id, v] : a)
        worst = std::max (worst, b.count (id) ? std::abs (v - b.at (id)) : 1.0e9f);
    return worst;
}

float value (PluginProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

// Plays `buffer` through the processor in host-sized blocks and returns the processed audio.
juce::AudioBuffer<float> run (PluginProcessor& p, const juce::AudioBuffer<float>& in)
{
    auto out = in;
    juce::MidiBuffer midi;
    for (int s = 0; s + block <= in.getNumSamples(); s += block)
    {
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), out.getNumChannels(), s, block);
        p.processBlock (view, midi);
    }
    return out;
}

// pink-ish noise plus a tone that stands well above it
juce::AudioBuffer<float> resonantSignal (int seconds, double toneHz, float toneAmp)
{
    juce::Random rng (99);
    const int n = (int) fs * seconds;
    juce::AudioBuffer<float> b (2, n);
    for (int ch = 0; ch < 2; ++ch)
    {
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (int i = 0; i < n; ++i)
        {
            const float white = rng.nextFloat() * 2.0f - 1.0f;
            b0 = 0.99886f * b0 + white * 0.0555179f;
            b1 = 0.99332f * b1 + white * 0.0750759f;
            b2 = 0.96900f * b2 + white * 0.1538520f;
            b3 = 0.86650f * b3 + white * 0.3104856f;
            b4 = 0.55000f * b4 + white * 0.5329522f;
            b5 = -0.7616f * b5 - white * 0.0168980f;
            const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.11f;
            b6 = white * 0.115926f;
            b.setSample (ch, i, 0.25f * pink + toneAmp * (float) std::sin (2.0 * std::numbers::pi * toneHz * i / fs));
        }
    }
    return b;
}

double bandRmsDb (const juce::AudioBuffer<float>& b, double hz)
{
    // single-bin DFT (Goertzel-like) over the last second
    const int n = (int) fs;
    const int start = b.getNumSamples() - n;
    double re = 0, im = 0;
    for (int i = 0; i < n; ++i)
    {
        const double x = b.getSample (0, start + i);
        re += x * std::cos (2.0 * std::numbers::pi * hz * i / fs);
        im += x * std::sin (2.0 * std::numbers::pi * hz * i / fs);
    }
    return 20.0 * std::log10 (std::sqrt (re * re + im * im) / n + 1.0e-12);
}
}

TEST_CASE ("Undo and redo restore parameter values", "[session][undo]")
{
    PluginProcessor p;
    p.setParamValues ({ { ParamIDs::inputGain, 5.0f } }, "first");
    p.setParamValues ({ { ParamIDs::inputGain, -7.0f } }, "second");
    CHECK (value (p, ParamIDs::inputGain) == Approx (-7.0f).margin (0.01));
    p.undoManager.undo();
    CHECK (value (p, ParamIDs::inputGain) == Approx (5.0f).margin (0.01));
    p.undoManager.undo();
    CHECK (value (p, ParamIDs::inputGain) == Approx (0.0f).margin (0.01));
    p.undoManager.redo();
    CHECK (value (p, ParamIDs::inputGain) == Approx (5.0f).margin (0.01));
}

TEST_CASE ("A/B slots keep independent settings", "[session][ab]")
{
    PluginProcessor p;
    setParam (p, ParamIDs::outputGain, -3.0f);
    REQUIRE (p.getActiveSlot() == 0);

    p.switchABSlot(); // to B: starts as a copy of A
    CHECK (p.getActiveSlot() == 1);
    CHECK (value (p, ParamIDs::outputGain) == Approx (-3.0f).margin (0.01));

    setParam (p, ParamIDs::outputGain, 6.0f);
    setParam (p, ParamIDs::compEnable, 1.0f);
    p.switchABSlot(); // back to A
    CHECK (p.getActiveSlot() == 0);
    CHECK (value (p, ParamIDs::outputGain) == Approx (-3.0f).margin (0.01));
    CHECK (value (p, ParamIDs::compEnable) < 0.5f);

    p.switchABSlot(); // B again
    CHECK (value (p, ParamIDs::outputGain) == Approx (6.0f).margin (0.01));
    CHECK (value (p, ParamIDs::compEnable) > 0.5f);

    // slots survive a session save/restore
    juce::MemoryBlock saved;
    p.getStateInformation (saved);
    PluginProcessor q;
    q.setStateInformation (saved.getData(), (int) saved.getSize());
    CHECK (q.getActiveSlot() == 1);
    q.switchABSlot();
    CHECK (value (q, ParamIDs::outputGain) == Approx (-3.0f).margin (0.01));
}

TEST_CASE ("Full session state round-trips every parameter", "[session][state]")
{
    PluginProcessor a;
    juce::Random rng (3);
    std::map<std::string, float> chosen;
    for (const auto& [id, v] : a.getParamValues())
    {
        auto* param = a.apvts.getParameter (id);
        const auto norm = rng.nextFloat();
        param->setValueNotifyingHost (norm);
        chosen[id] = param->convertFrom0to1 (param->getValue());
    }
    a.apvts.state.setProperty (PluginProcessor::mode_, 2, nullptr);
    a.apvts.state.setProperty (PluginProcessor::goal_, 7, nullptr);

    juce::MemoryBlock saved;
    a.getStateInformation (saved);
    PluginProcessor b;
    b.setStateInformation (saved.getData(), (int) saved.getSize());
    for (const auto& [id, v] : chosen)
    {
        CAPTURE (id);
        CHECK (value (b, id.c_str()) == Approx (v).epsilon (1.0e-4).margin (1.0e-4));
    }
    CHECK ((int) b.apvts.state.getProperty (PluginProcessor::mode_) == 2);
    CHECK ((int) b.apvts.state.getProperty (PluginProcessor::goal_) == 7);
}

TEST_CASE ("Presets: factory presets are valid, user presets round-trip", "[session][presets]")
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("signalpath_test_presets_" + juce::String (juce::Random::getSystemRandom().nextInt()));
    PluginProcessor p;
    PresetManager presets (p, dir);

    SECTION ("every factory preset only names real parameters and in-range values")
    {
        for (const auto& [name, values] : PresetManager::factoryPresets())
            for (const auto& [id, v] : values)
            {
                auto* param = p.apvts.getParameter (id);
                CAPTURE (name, id);
                REQUIRE (param != nullptr);
                const auto range = param->getNormalisableRange();
                REQUIRE (v >= range.start - 1.0e-4f);
                REQUIRE (v <= range.end + 1.0e-4f);
            }
    }
    SECTION ("loading a factory preset is one undo step and fully defines the sound")
    {
        setParam (p, ParamIDs::satEnable, 1.0f); // something the preset does not set
        REQUIRE (presets.load (juce::String::fromUTF8 ("Drum Bus · Punch")));
        CHECK (value (p, ParamIDs::compAttack) == Approx (28.0f).margin (0.05));
        CHECK (juce::String (p.apvts.state.getProperty (PluginProcessor::presetName_).toString()) == juce::String::fromUTF8 ("Drum Bus · Punch"));
        REQUIRE (presets.load ("Init"));
        CHECK (value (p, ParamIDs::compEnable) < 0.5f);
        CHECK (value (p, ParamIDs::satEnable) < 0.5f);
        p.undoManager.undo(); // back to the drum preset
        CHECK (value (p, ParamIDs::compAttack) == Approx (28.0f).margin (0.05));
    }
    SECTION ("presets leave level staging and bypass alone")
    {
        setParam (p, ParamIDs::inputGain, 4.0f);
        setParam (p, ParamIDs::bypass, 1.0f);
        presets.load (juce::String::fromUTF8 ("Vocal · Starting Point"));
        CHECK (value (p, ParamIDs::inputGain) == Approx (4.0f).margin (0.01));
        CHECK (value (p, ParamIDs::bypass) > 0.5f);
    }
    SECTION ("save, list, load and delete a user preset")
    {
        setParam (p, ParamIDs::compEnable, 1.0f);
        setParam (p, ParamIDs::compRatio, 7.5f);
        REQUIRE (presets.save ("My Vocal"));
        setParam (p, ParamIDs::compRatio, 2.0f);
        REQUIRE (presets.load ("My Vocal"));
        CHECK (value (p, ParamIDs::compRatio) == Approx (7.5f).margin (0.02));

        bool found = false;
        for (const auto& pr : presets.list())
            found |= (pr.name == "My Vocal" && ! pr.factory);
        CHECK (found);
        CHECK (presets.remove ("My Vocal"));
        CHECK_FALSE (presets.load ("My Vocal"));
    }
    SECTION ("bad names and corrupt files are rejected safely")
    {
        CHECK_FALSE (presets.save ("///"));
        CHECK_FALSE (presets.save ("Init")); // cannot shadow a factory preset
        dir.createDirectory();
        dir.getChildFile ("Broken.spreset").replaceWithText ("<not valid");
        CHECK_FALSE (presets.load ("Broken"));
        dir.getChildFile ("Foreign.spreset").replaceWithText ("<Other/>");
        CHECK_FALSE (presets.load ("Foreign"));
        CHECK_FALSE (presets.load ("../../etc/passwd"));
    }
    dir.deleteRecursively();
}

TEST_CASE ("Recommendation session: analyse, preview, reject, apply, undo on real audio", "[session][e2e]")
{
    PluginProcessor p;
    prepare (p);
    RecommendationSession session (p);

    SECTION ("without audio there is nothing to recommend")
    {
        session.analyse (Goal::reducedHarshness, Source::other, 0.6f);
        CHECK (session.getRecommendation().outcome == Outcome::insufficientSignal);
        CHECK_FALSE (session.preview());
    }

    // 8 seconds of audio with a harsh 4.2 kHz resonance played through the real audio path
    const auto input = resonantSignal (8, 4200.0, 0.02f);
    run (p, input);
    const auto before = p.getParamValues();

    session.analyse (Goal::reducedHarshness, Source::other, 0.7f);
    const auto& rec = session.getRecommendation();
    REQUIRE (rec.outcome == Outcome::suggested);
    CHECK (maxDifference (p.getParamValues(), before) < 0.01f); // analysing never touches parameters

    SECTION ("reject restores exactly the previous settings")
    {
        REQUIRE (session.preview());
        CHECK (session.getState() == RecommendationSession::State::previewing);
        CHECK (value (p, ParamIDs::eqGain[2]) < -1.0f);
        REQUIRE (session.reject());
        CHECK (maxDifference (p.getParamValues(), before) < 0.01f);
        CHECK (session.getState() == RecommendationSession::State::ready);
    }
    SECTION ("apply changes the audio where it should and stays undoable")
    {
        const auto dry = run (p, input);
        REQUIRE (session.apply());
        CHECK (session.getState() == RecommendationSession::State::applied);
        const auto wet = run (p, input);

        const auto cutDb = bandRmsDb (wet, 4200.0) - bandRmsDb (dry, 4200.0);
        CHECK (cutDb < -1.0);                                                       // resonance reduced
        CHECK (std::abs (bandRmsDb (wet, 500.0) - bandRmsDb (dry, 500.0)) < 0.5);   // the rest is untouched

        p.undoManager.undo();
        CHECK (maxDifference (p.getParamValues(), before) < 0.01f);
    }
    SECTION ("the user can edit any recommended value afterwards")
    {
        REQUIRE (session.apply());
        setParam (p, ParamIDs::eqGain[2], 0.0f);
        CHECK (value (p, ParamIDs::eqGain[2]) == Approx (0.0f).margin (0.01));
    }
}

TEST_CASE ("Loudness-matched bypass compares at the processed loudness", "[session][loudness]")
{
    PluginProcessor p;
    prepare (p);
    setParam (p, ParamIDs::loudnessMatch, 1.0f);
    setParam (p, ParamIDs::outputGain, -6.0f); // processing makes the signal 6 dB quieter
    const auto input = test::sine (2, (int) fs * 8, fs, 1000.0, 0.3f);

    auto processed = run (p, input); // learns the offset while active
    CHECK (p.getLoudnessOffsetDb() == Approx (-6.0f).margin (0.3f));

    setParam (p, ParamIDs::bypass, 1.0f);
    auto bypassed = run (p, input);
    // the original is now played back ~6 dB lower so that A/B is level-fair
    const auto tail = [] (const juce::AudioBuffer<float>& b) { return test::rmsDb (b, 0, b.getNumSamples() - 24000, 24000); };
    CHECK (tail (bypassed) == Approx (tail (processed)).margin (0.4));

    // with matching off, bypass is the untouched original
    setParam (p, ParamIDs::loudnessMatch, 0.0f);
    auto plain = run (p, input);
    CHECK (tail (plain) == Approx (tail (input)).margin (0.05));
}

TEST_CASE ("Module enable/disable is click-free", "[session][chain]")
{
    PluginProcessor p;
    prepare (p);
    setParam (p, ParamIDs::satDrive, 12.0f);
    setParam (p, ParamIDs::satMix, 100.0f);
    const auto input = test::sine (2, (int) fs, fs, 220.0, 0.2f);
    run (p, input);

    setParam (p, ParamIDs::satEnable, 1.0f);
    const auto out = run (p, input);
    float maxStep = 0.0f;
    for (int i = 1; i < out.getNumSamples(); ++i)
        maxStep = std::max (maxStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
    CHECK (maxStep < 0.1f); // a 220 Hz sine at 0.2 steps at most ~0.006 per sample; a click would be far larger
}

TEST_CASE ("Whole plugin stays finite under parameter fuzzing", "[session][stability]")
{
    juce::Random rng (11);
    for (int channels : { 1, 2 })
    {
        PluginProcessor p;
        prepare (p, channels);
        for (int blockIndex = 0; blockIndex < 300; ++blockIndex)
        {
            for (const auto& [id, v] : p.getParamValues())
                if (rng.nextInt (6) == 0)
                    p.apvts.getParameter (id)->setValueNotifyingHost (rng.nextFloat());
            auto in = test::noise (channels, block, (unsigned) blockIndex, 1.0f);
            juce::MidiBuffer midi;
            p.processBlock (in, midi);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < block; ++i)
                {
                    REQUIRE (std::isfinite (in.getSample (ch, i)));
                    REQUIRE (std::abs (in.getSample (ch, i)) < 1000.0f);
                }
        }
    }
}

TEST_CASE ("Plugin reports zero latency and keeps meters live", "[session][meters]")
{
    PluginProcessor p;
    prepare (p);
    CHECK (p.getLatencySamples() == 0);
    run (p, test::sine (2, (int) fs, fs, 1000.0, 0.5f));
    CHECK (p.inputMeter.takeSamplePeak (0) == Approx (0.5f).margin (0.01));
    CHECK (p.inputMeter.getRms (0) > 0.3f);
    CHECK (p.outputMeter.takeTruePeak (1) > 0.45f);
}
