#include "helpers/signal_helpers.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace testing_helpers;

namespace
{
constexpr size_t kTotal = 96000;
constexpr double kToneHz = 3000.0;
constexpr int kLatency = 1024;

using Layout = juce::AudioProcessor::BusesLayout;

Layout makeLayout (juce::AudioChannelSet main, std::optional<juce::AudioChannelSet> key)
{
    Layout l;
    l.inputBuses.add (main);
    if (key)
        l.inputBuses.add (*key);
    l.outputBuses.add (main);
    return l;
}

/** Enables the sidechain bus (stereo or mono key) and returns the number of key channels. */
int enableKey (PluginProcessor& plugin, int keyChannels)
{
    const auto keySet = keyChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    REQUIRE (plugin.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), keySet)));
    REQUIRE (plugin.isKeyConnected());
    return keyChannels;
}

struct Result
{
    std::vector<float> left, right;
};

/** Runs main (L = R) and a key through the plugin in host-sized blocks. keyR empty = use keyL for both (or mono). */
Result runWithKey (PluginProcessor& plugin, const std::vector<float>& main, const std::vector<float>& keyL, const std::vector<float>& keyR = {})
{
    const auto totalIn = plugin.getTotalNumInputChannels();
    plugin.prepareToPlay (kSampleRate, kBlockSize);

    Result out { std::vector<float> (main.size()), std::vector<float> (main.size()) };
    juce::AudioBuffer<float> buffer (juce::jmax (totalIn, 2), kBlockSize);
    juce::MidiBuffer midi;

    for (size_t pos = 0; pos + (size_t) kBlockSize <= main.size(); pos += (size_t) kBlockSize)
    {
        for (int c = 0; c < 2; ++c)
            std::copy (main.begin() + (long) pos, main.begin() + (long) pos + kBlockSize, buffer.getWritePointer (c));

        const auto& right = keyR.empty() ? keyL : keyR;
        if (totalIn > 2)
            std::copy (keyL.begin() + (long) pos, keyL.begin() + (long) pos + kBlockSize, buffer.getWritePointer (2));
        if (totalIn > 3)
            std::copy (right.begin() + (long) pos, right.begin() + (long) pos + kBlockSize, buffer.getWritePointer (3));

        plugin.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + kBlockSize, out.left.begin() + (long) pos);
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + kBlockSize, out.right.begin() + (long) pos);
    }
    return out;
}

std::vector<float> delayed (const std::vector<float>& x)
{
    std::vector<float> d (x.size(), 0.0f);
    std::copy (x.begin(), x.end() - kLatency, d.begin() + kLatency);
    return d;
}

double reductionAt (const std::vector<float>& in, const std::vector<float>& out, double hz)
{
    return toDb (tonePower (delayed (in), 48000, 48000, hz) / tonePower (out, 48000, 48000, hz));
}

void useStrongSettings (PluginProcessor& plugin)
{
    setParam (plugin, ParamIDs::depth, 100.0f);
    setParam (plugin, ParamIDs::detail, 70.0f);
    setParam (plugin, ParamIDs::maxCut, 40.0f);
}

std::vector<float> keyWithTone()
{
    auto key = whiteNoise ((int) kTotal, 0.05f, 777);
    addSine (key, kToneHz, 0.01f);
    return key;
}
} // namespace

TEST_CASE ("Sidechain bus layouts", "[sidechain][buses]")
{
    PluginProcessor plugin;
    using Set = juce::AudioChannelSet;

    CHECK (plugin.getBusCount (true) == 2);
    CHECK_FALSE (plugin.isKeyConnected()); // off by default

    CHECK (plugin.isBusesLayoutSupported (makeLayout (Set::stereo(), std::nullopt)));
    CHECK (plugin.isBusesLayoutSupported (makeLayout (Set::stereo(), Set::disabled())));
    CHECK (plugin.isBusesLayoutSupported (makeLayout (Set::stereo(), Set::mono())));
    CHECK (plugin.isBusesLayoutSupported (makeLayout (Set::stereo(), Set::stereo())));
    CHECK (plugin.isBusesLayoutSupported (makeLayout (Set::mono(), Set::mono())));
    CHECK_FALSE (plugin.isBusesLayoutSupported (makeLayout (Set::create5point1(), std::nullopt)));
    CHECK_FALSE (plugin.isBusesLayoutSupported (makeLayout (Set::stereo(), Set::create5point1())));
}

TEST_CASE ("The key drives the reduction", "[sidechain]")
{
    // main is noise only; the key is noise plus a 3 kHz tone
    const auto main = whiteNoise ((int) kTotal, 0.05f, 4242);
    const auto key = keyWithTone();

    SECTION ("sidechain on: the main is cut where the key has a resonance, and only there")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin);
        setParam (plugin, ParamIDs::sidechain, 1.0f);
        enableKey (plugin, 2);

        const auto out = runWithKey (plugin, main, key);
        const auto tone = reductionAt (main, out.left, kToneHz);
        const auto control = toDb (bandPower (delayed (main), 48000, 48000, 1000.0, 2000.0) / bandPower (out.left, 48000, 48000, 1000.0, 2000.0));
        INFO ("tone " << tone << " dB, control " << control << " dB");
        CHECK (tone > 6.0);
        CHECK (std::abs (control) < 3.0);
        CHECK (allFinite (out.left));
        CHECK (plugin.getKeyLevel() > 0.0f);
    }

    SECTION ("sidechain off: the key is ignored")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin);
        enableKey (plugin, 2);

        const auto out = runWithKey (plugin, main, key);
        CHECK (std::abs (reductionAt (main, out.left, kToneHz)) < 4.0);
        CHECK (plugin.getKeyLevel() == Catch::Approx (0.0f).margin (1.0e-6));
    }

    SECTION ("a mono key works too")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin);
        setParam (plugin, ParamIDs::sidechain, 1.0f);
        enableKey (plugin, 1);

        const auto out = runWithKey (plugin, main, key);
        CHECK (reductionAt (main, out.left, kToneHz) > 6.0);
        CHECK (reductionAt (main, out.right, kToneHz) > 6.0);
    }

    SECTION ("M/S with unlinked channels still produces finite output")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin);
        setParam (plugin, ParamIDs::sidechain, 1.0f);
        setParam (plugin, ParamIDs::stereoMode, 1.0f);
        setParam (plugin, ParamIDs::link, 0.0f);
        enableKey (plugin, 2);

        const auto out = runWithKey (plugin, main, key, whiteNoise ((int) kTotal, 0.05f, 1));
        CHECK (allFinite (out.left));
        CHECK (allFinite (out.right));
    }
}

TEST_CASE ("Sidechain on without a routed key falls back to the main signal", "[sidechain]")
{
    auto main = whiteNoise ((int) kTotal, 0.05f, 4242);
    addSine (main, kToneHz, 0.01f);

    PluginProcessor off;
    useStrongSettings (off);
    const auto reference = run (off, main);

    PluginProcessor on;
    useStrongSettings (on);
    setParam (on, ParamIDs::sidechain, 1.0f);
    REQUIRE_FALSE (on.isKeyConnected());
    const auto out = run (on, main);

    for (size_t i = 0; i < main.size(); i += 53)
        REQUIRE (out[i] == Catch::Approx (reference[i]).margin (1.0e-6));
    CHECK (reductionAt (main, out, kToneHz) > 6.0); // the main's own resonance is still handled
}

TEST_CASE ("Key listen plays the delayed key", "[sidechain][keylisten]")
{
    const auto main = whiteNoise ((int) kTotal, 0.05f, 4242);
    const auto keyL = whiteNoise ((int) kTotal, 0.1f, 11);
    const auto keyR = whiteNoise ((int) kTotal, 0.1f, 12);

    PluginProcessor plugin;
    setParam (plugin, ParamIDs::sidechain, 1.0f);
    setParam (plugin, ParamIDs::keyListen, 1.0f);
    enableKey (plugin, 2);

    const auto out = runWithKey (plugin, main, keyL, keyR);
    for (size_t i = 0; i + kLatency < kTotal; ++i)
    {
        REQUIRE (out.left[i + kLatency] == Catch::Approx (keyL[i]).margin (1.0e-6));
        REQUIRE (out.right[i + kLatency] == Catch::Approx (keyR[i]).margin (1.0e-6));
    }
}

TEST_CASE ("Key listen does nothing without the sidechain", "[sidechain][keylisten]")
{
    const auto in = whiteNoise ((int) kTotal, 0.05f, 4242);

    PluginProcessor plugin;
    setParam (plugin, ParamIDs::keyListen, 1.0f);
    setParam (plugin, ParamIDs::mix, 0.0f);
    const auto out = run (plugin, in);
    for (size_t i = 0; i + kLatency < in.size(); i += 17)
        REQUIRE (out[i + kLatency] == Catch::Approx (in[i]).margin (1.0e-6));
}
