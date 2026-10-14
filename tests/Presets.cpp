#include "helpers/signal_helpers.h"
#include <Presets.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace testing_helpers;
using ParamIDs::BandParam;

namespace
{
float depthOf (PluginProcessor& p) { return getParam (p, ParamIDs::depth); }
bool isBandActive (PluginProcessor& p, int band) { return getParam (p, ParamIDs::bandParamID (band, BandParam::active)) > 0.5f; }
} // namespace

TEST_CASE ("Factory presets are exposed as programs", "[presets]")
{
    PluginProcessor plugin;

    REQUIRE (plugin.getNumPrograms() == 4);
    CHECK (plugin.getProgramName (0) == "Vocal silk");
    CHECK (plugin.getProgramName (1) == "Gentle master");
    CHECK (plugin.getProgramName (2) == "Air control");
    CHECK (plugin.getProgramName (3) == "Clean dialogue");

    // the VST3 validator fails plugins with unnamed programs
    for (int i = 0; i < plugin.getNumPrograms(); ++i)
        CHECK (plugin.getProgramName (i).isNotEmpty());

    SECTION ("loading a preset sets its values")
    {
        plugin.setCurrentProgram (1);
        CHECK (plugin.getCurrentProgram() == 1);
        CHECK (depthOf (plugin) == Catch::Approx (22.0f).margin (0.2f));
        CHECK (getParam (plugin, ParamIDs::maxCut) == Catch::Approx (3.0f).margin (0.2f));
        CHECK (juce::roundToInt (getParam (plugin, ParamIDs::stereoMode)) == 1); // M/S

        plugin.setCurrentProgram (0);
        CHECK (depthOf (plugin) == Catch::Approx (40.0f).margin (0.2f));
        CHECK (juce::roundToInt (getParam (plugin, ParamIDs::stereoMode)) == 0); // back to L/R, not left over
    }

    SECTION ("bands come from the preset and nothing is left over from the last one")
    {
        plugin.setCurrentProgram (3); // Clean dialogue: bands 1, 2 and 4
        CHECK (isBandActive (plugin, 4));
        CHECK (getParam (plugin, ParamIDs::bandParamID (1, BandParam::freq)) == Catch::Approx (120.0f).margin (1.0f));
        CHECK (getParam (plugin, ParamIDs::bandParamID (2, BandParam::gain)) == Catch::Approx (6.0f).margin (0.2f));

        plugin.setCurrentProgram (0);
        CHECK_FALSE (isBandActive (plugin, 4));
        CHECK (getParam (plugin, ParamIDs::bandParamID (2, BandParam::freq)) == Catch::Approx (4600.0f).margin (5.0f));
    }

    SECTION ("sound-shaping leftovers are reset")
    {
        setParam (plugin, ParamIDs::maxCut, 33.0f);
        setParam (plugin, ParamIDs::attackLow, 70.0f);
        setParam (plugin, ParamIDs::stereoBalance, 20.0f);
        plugin.setCurrentProgram (0);
        CHECK (getParam (plugin, ParamIDs::maxCut) == Catch::Approx (6.0f).margin (0.2f));
        CHECK (getParam (plugin, ParamIDs::attackLow) == Catch::Approx (0.0f).margin (0.2f));
        CHECK (getParam (plugin, ParamIDs::stereoBalance) == Catch::Approx (0.0f).margin (0.6f));
    }

    SECTION ("mix, output gain, bypass, delta and sidechain are left alone")
    {
        setParam (plugin, ParamIDs::mix, 55.0f);
        setParam (plugin, ParamIDs::outGain, 3.0f);
        setParam (plugin, ParamIDs::bypass, 1.0f);
        setParam (plugin, ParamIDs::delta, 1.0f);
        setParam (plugin, ParamIDs::sidechain, 1.0f);
        plugin.setCurrentProgram (2);
        CHECK (getParam (plugin, ParamIDs::mix) == Catch::Approx (55.0f).margin (0.2f));
        CHECK (getParam (plugin, ParamIDs::outGain) == Catch::Approx (3.0f).margin (0.2f));
        CHECK (getParam (plugin, ParamIDs::bypass) > 0.5f);
        CHECK (getParam (plugin, ParamIDs::delta) > 0.5f);
        CHECK (getParam (plugin, ParamIDs::sidechain) > 0.5f);
    }

    SECTION ("out-of-range programs are ignored")
    {
        plugin.setCurrentProgram (2);
        plugin.setCurrentProgram (9);
        plugin.setCurrentProgram (-1);
        CHECK (plugin.getCurrentProgram() == 2);
    }
}

TEST_CASE ("The quality lock protects the chosen quality from presets", "[presets][quality]")
{
    PluginProcessor plugin;
    REQUIRE (getParam (plugin, ParamIDs::qualityLock) > 0.5f); // locked by default

    plugin.setCurrentProgram (1); // wants High
    CHECK (juce::roundToInt (getParam (plugin, ParamIDs::quality)) == 0);

    setParam (plugin, ParamIDs::qualityLock, 0.0f);
    plugin.setCurrentProgram (1);
    CHECK (juce::roundToInt (getParam (plugin, ParamIDs::quality)) == 1);
    plugin.setCurrentProgram (0); // wants Normal
    CHECK (juce::roundToInt (getParam (plugin, ParamIDs::quality)) == 0);
}

TEST_CASE ("Undo and redo of parameter changes", "[presets][undo]")
{
    PluginProcessor plugin;
    const auto original = depthOf (plugin);

    plugin.undoManager.beginNewTransaction ("test");
    setParam (plugin, ParamIDs::depth, 90.0f);
    plugin.apvts.copyState(); // flushes the parameter into the state tree, where the undo history lives

    REQUIRE (plugin.undoManager.canUndo());
    plugin.undoManager.undo();
    CHECK (depthOf (plugin) == Catch::Approx (original).margin (0.2f));

    REQUIRE (plugin.undoManager.canRedo());
    plugin.undoManager.redo();
    CHECK (depthOf (plugin) == Catch::Approx (90.0f).margin (0.2f));
}

TEST_CASE ("A preset load is a single undo step", "[presets][undo]")
{
    PluginProcessor plugin;
    setParam (plugin, ParamIDs::depth, 70.0f);
    plugin.apvts.copyState();
    plugin.undoManager.clearUndoHistory();

    plugin.setCurrentProgram (1);
    plugin.apvts.copyState();
    REQUIRE (plugin.undoManager.canUndo());
    CHECK (depthOf (plugin) == Catch::Approx (22.0f).margin (0.2f));

    plugin.undoManager.undo();
    CHECK (depthOf (plugin) == Catch::Approx (70.0f).margin (0.2f));
    CHECK (juce::roundToInt (getParam (plugin, ParamIDs::stereoMode)) == 0);
}

TEST_CASE ("A/B comparison slots", "[presets][ab]")
{
    PluginProcessor plugin;
    REQUIRE (plugin.getActiveSlot() == 0);

    setParam (plugin, ParamIDs::depth, 10.0f);

    SECTION ("an unused slot starts as a copy of the current settings")
    {
        plugin.switchToSlot (1);
        CHECK (plugin.getActiveSlot() == 1);
        CHECK (depthOf (plugin) == Catch::Approx (10.0f).margin (0.2f));
    }

    SECTION ("each slot keeps its own settings")
    {
        plugin.switchToSlot (1);
        setParam (plugin, ParamIDs::depth, 80.0f);
        plugin.switchToSlot (0);
        CHECK (depthOf (plugin) == Catch::Approx (10.0f).margin (0.2f));
        plugin.switchToSlot (1);
        CHECK (depthOf (plugin) == Catch::Approx (80.0f).margin (0.2f));
    }

    SECTION ("a->b copies the active slot over the other one")
    {
        plugin.switchToSlot (1);
        setParam (plugin, ParamIDs::depth, 80.0f);
        plugin.switchToSlot (0);
        setParam (plugin, ParamIDs::depth, 33.0f);
        plugin.copyActiveToOtherSlot();
        plugin.switchToSlot (1);
        CHECK (depthOf (plugin) == Catch::Approx (33.0f).margin (0.2f));
    }

    SECTION ("both slots survive saving and loading a session")
    {
        plugin.switchToSlot (1);
        setParam (plugin, ParamIDs::depth, 80.0f);
        plugin.setCurrentProgram (2);
        setParam (plugin, ParamIDs::depth, 80.0f);

        juce::MemoryBlock state;
        plugin.getStateInformation (state);

        PluginProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (restored.getActiveSlot() == 1);
        CHECK (restored.getCurrentProgram() == 2);
        CHECK (depthOf (restored) == Catch::Approx (80.0f).margin (0.2f));
        restored.switchToSlot (0);
        CHECK (depthOf (restored) == Catch::Approx (10.0f).margin (0.2f));
    }
}

TEST_CASE ("Older plain parameter states still load", "[presets][state]")
{
    PluginProcessor source;
    setParam (source, ParamIDs::depth, 61.0f);

    juce::MemoryBlock legacy;
    if (auto xml = source.apvts.copyState().createXml())
        PluginProcessor::copyXmlToBinary (*xml, legacy);
    REQUIRE (legacy.getSize() > 0);

    PluginProcessor target;
    target.setStateInformation (legacy.getData(), (int) legacy.getSize());
    CHECK (depthOf (target) == Catch::Approx (61.0f).margin (0.2f));
}
