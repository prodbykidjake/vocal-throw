#include <PluginProcessor.h>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;

void setParam (PluginProcessor& p, const char* id, float plainValue)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void prepare (PluginProcessor& p, int channels = 2, int block = blockSize)
{
    p.setPlayConfigDetails (channels, channels, sampleRate, block);
    p.prepareToPlay (sampleRate, block);
}

juce::AudioBuffer<float> makeNoise (int channels, int samples, unsigned seed)
{
    juce::Random rng ((juce::int64) seed);
    juce::AudioBuffer<float> b (channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            b.setSample (ch, i, rng.nextFloat() * 1.6f - 0.8f);
    return b;
}

void run (PluginProcessor& p, juce::AudioBuffer<float>& b)
{
    juce::MidiBuffer midi;
    p.processBlock (b, midi);
}

// Runs several blocks so parameter smoothing settles, returns the last block (input and output).
std::pair<juce::AudioBuffer<float>, juce::AudioBuffer<float>> settleAndCapture (PluginProcessor& p, int channels)
{
    for (int i = 0; i < 20; ++i)
    {
        auto b = makeNoise (channels, blockSize, 100 + (unsigned) i);
        run (p, b);
    }
    auto in = makeNoise (channels, blockSize, 999);
    auto out = in;
    run (p, out);
    return { in, out };
}
}

TEST_CASE ("Defaults are unity and bit-exact", "[gain]")
{
    PluginProcessor p;
    prepare (p);
    auto [in, out] = settleAndCapture (p, 2);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < blockSize; ++i)
            REQUIRE (juce::exactlyEqual (out.getSample (ch, i), in.getSample (ch, i)));
}

TEST_CASE ("Input and output gain are accurate", "[gain]")
{
    PluginProcessor p;
    prepare (p);

    const auto [inDb, outDb] = GENERATE (std::pair { 6.0f, 0.0f }, std::pair { 0.0f, -12.0f }, std::pair { 3.0f, -9.0f }, std::pair { 24.0f, 24.0f }, std::pair { -24.0f, -24.0f });
    setParam (p, ParamIDs::inputGain, inDb);
    setParam (p, ParamIDs::outputGain, outDb);

    auto [in, out] = settleAndCapture (p, 2);
    const auto expected = juce::Decibels::decibelsToGain (inDb + outDb);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < blockSize; ++i)
            REQUIRE (out.getSample (ch, i) == Catch::Approx (in.getSample (ch, i) * expected).margin (1.0e-5 * expected));
}

TEST_CASE ("Parameter change is smoothed, not stepped", "[gain]")
{
    PluginProcessor p;
    prepare (p);
    setParam (p, ParamIDs::inputGain, 0.0f);
    settleAndCapture (p, 2);

    setParam (p, ParamIDs::inputGain, 12.0f);
    juce::AudioBuffer<float> b (2, blockSize);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < blockSize; ++i)
            b.setSample (ch, i, 0.1f);
    run (p, b);

    float maxStep = 0.0f;
    for (int i = 1; i < blockSize; ++i)
        maxStep = std::max (maxStep, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
    CHECK (maxStep < 0.01f); // a hard step to +12 dB would jump by ~0.3
    CHECK (b.getSample (0, 0) < b.getSample (0, blockSize - 1));
}

TEST_CASE ("Bypass is bit-exact once settled and fades without a click", "[bypass]")
{
    PluginProcessor p;
    prepare (p);
    setParam (p, ParamIDs::inputGain, 9.0f);
    setParam (p, ParamIDs::outputGain, -3.0f);
    settleAndCapture (p, 2);

    setParam (p, ParamIDs::bypass, 1.0f);

    // transition block: no sample-to-sample jump larger than the signal itself could produce
    juce::AudioBuffer<float> sine (2, blockSize);
    for (int i = 0; i < blockSize; ++i)
        for (int ch = 0; ch < 2; ++ch)
            sine.setSample (ch, i, 0.3f * std::sin (0.05f * (float) i));
    auto transition = sine;
    run (p, transition);
    float maxJump = 0.0f;
    for (int i = 1; i < blockSize; ++i)
        maxJump = std::max (maxJump, std::abs (transition.getSample (0, i) - transition.getSample (0, i - 1)));
    CHECK (maxJump < 0.1f);

    auto [in, out] = settleAndCapture (p, 2);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < blockSize; ++i)
            REQUIRE (juce::exactlyEqual (out.getSample (ch, i), in.getSample (ch, i)));

    // and processing resumes when un-bypassed
    setParam (p, ParamIDs::bypass, 0.0f);
    auto [in2, out2] = settleAndCapture (p, 2);
    const auto expected = juce::Decibels::decibelsToGain (6.0f);
    CHECK (out2.getSample (0, 10) == Catch::Approx (in2.getSample (0, 10) * expected).margin (1.0e-5));
}

TEST_CASE ("Mono and stereo layouts, small and large blocks", "[layout]")
{
    for (int channels : { 1, 2 })
        for (int block : { 1, 32, 480, 4096 })
        {
            PluginProcessor p;
            prepare (p, channels, block);
            setParam (p, ParamIDs::inputGain, -6.0f);
            for (int i = 0; i < 2000 / block + 2; ++i) // settle the 20 ms ramp (960 samples) at any block size
            {
                auto b = makeNoise (channels, block, 1);
                run (p, b);
            }
            auto in = makeNoise (channels, block, 7);
            auto out = in;
            run (p, out);
            const auto g = juce::Decibels::decibelsToGain (-6.0f);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < block; ++i)
                    REQUIRE (out.getSample (ch, i) == Catch::Approx (in.getSample (ch, i) * g).margin (1.0e-5));
        }
}

TEST_CASE ("Host block larger than announced is handled", "[layout]")
{
    PluginProcessor p;
    prepare (p, 2, 64);
    setParam (p, ParamIDs::outputGain, -6.0f);
    for (int i = 0; i < 100; ++i)
    {
        auto b = makeNoise (2, 64, 1);
        run (p, b);
    }
    auto in = makeNoise (2, 1000, 3);
    auto out = in;
    run (p, out);
    const auto g = juce::Decibels::decibelsToGain (-6.0f);
    for (int i = 0; i < 1000; ++i)
        REQUIRE (out.getSample (1, i) == Catch::Approx (in.getSample (1, i) * g).margin (1.0e-5));
}

TEST_CASE ("State round-trips", "[state]")
{
    juce::MemoryBlock saved;
    {
        PluginProcessor a;
        setParam (a, ParamIDs::inputGain, 7.5f);
        setParam (a, ParamIDs::outputGain, -11.25f);
        setParam (a, ParamIDs::bypass, 1.0f);
        a.getStateInformation (saved);
    }
    REQUIRE (saved.getSize() > 0);

    PluginProcessor b;
    b.setStateInformation (saved.getData(), (int) saved.getSize());
    CHECK (b.apvts.getRawParameterValue (ParamIDs::inputGain)->load() == Catch::Approx (7.5f).margin (0.01));
    CHECK (b.apvts.getRawParameterValue (ParamIDs::outputGain)->load() == Catch::Approx (-11.25f).margin (0.01));
    CHECK (b.apvts.getRawParameterValue (ParamIDs::bypass)->load() > 0.5f);
}

TEST_CASE ("Corrupt or foreign state is ignored safely", "[state]")
{
    PluginProcessor p;
    setParam (p, ParamIDs::inputGain, 5.0f);

    const char junk[] = "not xml at all";
    p.setStateInformation (junk, (int) sizeof (junk));
    p.setStateInformation (nullptr, 0);

    juce::MemoryBlock foreign;
    juce::XmlElement other ("SomethingElse");
    juce::AudioProcessor::copyXmlToBinary (other, foreign);
    p.setStateInformation (foreign.getData(), (int) foreign.getSize());

    CHECK (p.apvts.getRawParameterValue (ParamIDs::inputGain)->load() == Catch::Approx (5.0f).margin (0.01));
}

TEST_CASE ("Parameter IDs and ranges are stable", "[state]")
{
    PluginProcessor p;
    for (auto id : { ParamIDs::inputGain, ParamIDs::outputGain, ParamIDs::bypass })
        CHECK (p.apvts.getParameter (id) != nullptr);
    CHECK (p.getBypassParameter() == p.apvts.getParameter (ParamIDs::bypass));

    auto* g = p.apvts.getParameter (ParamIDs::inputGain);
    CHECK (g->convertFrom0to1 (0.0f) == Catch::Approx (-24.0f));
    CHECK (g->convertFrom0to1 (1.0f) == Catch::Approx (24.0f));
    CHECK (g->convertFrom0to1 (g->getDefaultValue()) == Catch::Approx (0.0f).margin (0.01));
}

TEST_CASE ("Parameter defaults are exact", "[state]")
{
    PluginProcessor p;
    // 0 dB must be exactly 0 (not float dust), on every compiler and optimisation level
    for (auto id : { ParamIDs::inputGain, ParamIDs::outputGain, ParamIDs::eqGain[0], ParamIDs::eqGain[1], ParamIDs::eqGain[2], ParamIDs::eqGain[3], ParamIDs::compMakeup })
    {
        CAPTURE (id);
        CHECK (juce::exactlyEqual (p.apvts.getRawParameterValue (id)->load(), 0.0f));
    }
}
