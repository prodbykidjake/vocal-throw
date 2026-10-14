#include "helpers/signal_helpers.h"
#include <PluginEditor.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace testing_helpers;
using ParamIDs::BandParam;

namespace
{
juce::MouseEvent makeEvent (juce::Component& c, juce::Point<float> pos, bool dragged = false, int clicks = 1)
{
    auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (source, pos, juce::ModifierKeys::leftButtonModifier, juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
        juce::MouseInputSource::defaultTiltY, &c, &c, now, pos, now, clicks, dragged);
}

struct Harness
{
    Harness()
    {
        plugin.setUiScale (1.0f);
        editor.reset (dynamic_cast<PluginEditor*> (plugin.createEditorAndMakeActive()));
        REQUIRE (editor != nullptr);
        editor->setSize (1480, 860);
    }

    ~Harness()
    {
        plugin.editorBeingDeleted (editor.get());
        editor.reset();
    }

    vox::SpectrumView& spectrum() { return editor->getContent().getSpectrum(); }
    vox::BandEditor& bands() { return editor->getContent().getBandEditor(); }
    float value (int band, BandParam p) { return getParam (plugin, ParamIDs::bandParamID (band, p)); }

    PluginProcessor plugin;
    std::unique_ptr<PluginEditor> editor;
};
} // namespace

TEST_CASE ("The graph's axes", "[spectrum]")
{
    Harness h;
    auto& s = h.spectrum();

    for (const auto hz : { 20.0f, 100.0f, 1000.0f, 5000.0f, 20000.0f })
        CHECK (s.hzForX (s.xForHz (hz)) == Catch::Approx (hz).epsilon (0.001));

    CHECK (s.xForHz (20.0f) < s.xForHz (1000.0f));
    CHECK (s.xForHz (1000.0f) < s.xForHz (20000.0f));
    CHECK (s.xForHz (20000.0f) < (float) s.getWidth());

    CHECK (s.yForDb (0.0f) == Catch::Approx (s.zeroLineY()));
    CHECK (s.yForDb (6.0f) < s.yForDb (0.0f)); // boost is up
    CHECK (s.yForDb (-6.0f) > s.yForDb (0.0f));
    CHECK (s.dbForY (s.yForDb (-9.5f)) == Catch::Approx (-9.5f).margin (0.01f));
    CHECK (s.dbForY (-1000.0f) == Catch::Approx (vox::SpectrumView::maxDb)); // clamped to the scale
}

TEST_CASE ("Nodes are found where the bands are", "[spectrum]")
{
    Harness h;
    auto& s = h.spectrum();

    // the default bands: low cut, neutral bell, high cut
    for (int band = 0; band < 3; ++band)
        CHECK (s.nodeAt (s.nodePosition (band)) == band);

    CHECK (s.nodeAt (s.nodePosition (1).translated (0.0f, 200.0f)) == -1);
    CHECK (s.nodeAt (s.nodePosition (3)) != 3); // an inactive band has no node

    // the neutral bell is selected to start with, and the band editor follows
    CHECK (s.getSelectedBand() == 1);
    CHECK (h.bands().getBand() == 1);
}

TEST_CASE ("Adding bands", "[spectrum]")
{
    Harness h;
    auto& s = h.spectrum();

    const auto added = s.addBand (1000.0f, 6.0f);
    CHECK (added == 3); // the first free slot
    CHECK (h.value (4, BandParam::active) > 0.5f);
    CHECK (h.value (4, BandParam::freq) == Catch::Approx (1000.0f).margin (1.0f));
    CHECK (h.value (4, BandParam::gain) == Catch::Approx (6.0f).margin (0.1f));
    CHECK (juce::roundToInt (h.value (4, BandParam::shape)) == (int) vs::BandShape::bell);
    CHECK (s.getSelectedBand() == 3);
    CHECK (h.bands().getBand() == 3);

    SECTION ("a double-click on empty space adds a bell there")
    {
        const juce::Point<float> where (s.xForHz (2000.0f), s.yForDb (-4.0f));
        s.mouseDoubleClick (makeEvent (s, where, false, 2));
        CHECK (s.getSelectedBand() == 4);
        CHECK (h.value (5, BandParam::freq) == Catch::Approx (2000.0f).epsilon (0.02));
        CHECK (h.value (5, BandParam::gain) == Catch::Approx (-4.0f).margin (0.2f));
    }

    SECTION ("there are at most eight")
    {
        for (int i = 0; i < 10; ++i)
            s.addBand (500.0f + 100.0f * (float) i, 0.0f);
        int active = 0;
        for (int band = 1; band <= ParamIDs::numBands; ++band)
            active += h.value (band, BandParam::active) > 0.5f ? 1 : 0;
        CHECK (active == 8);
        CHECK (s.addBand (1000.0f, 0.0f) == -1);
    }
}

TEST_CASE ("Dragging a node edits its band", "[spectrum]")
{
    Harness h;
    auto& s = h.spectrum();
    const auto start = s.nodePosition (1); // the neutral 3 kHz bell
    const auto original = h.value (2, BandParam::freq);

    const juce::Point<float> target (s.xForHz (5000.0f), s.yForDb (6.0f));
    s.mouseDown (makeEvent (s, start));
    CHECK (s.getSelectedBand() == 1);
    s.mouseDrag (makeEvent (s, target, true));
    s.mouseUp (makeEvent (s, target, true));

    CHECK (h.value (2, BandParam::freq) == Catch::Approx (5000.0f).epsilon (0.02));
    CHECK (h.value (2, BandParam::gain) == Catch::Approx (6.0f).margin (0.2f));

    SECTION ("the whole drag is one undo step")
    {
        h.plugin.apvts.copyState();
        REQUIRE (h.plugin.undoManager.canUndo());
        h.plugin.undoManager.undo();
        CHECK (h.value (2, BandParam::freq) == Catch::Approx (original).epsilon (0.02));
        CHECK (h.value (2, BandParam::gain) == Catch::Approx (0.0f).margin (0.2f));
    }

    SECTION ("a cut only moves in frequency")
    {
        const auto lowCut = s.nodePosition (0);
        s.mouseDown (makeEvent (s, lowCut));
        s.mouseDrag (makeEvent (s, { s.xForHz (150.0f), s.yForDb (12.0f) }, true));
        s.mouseUp (makeEvent (s, { s.xForHz (150.0f), s.yForDb (12.0f) }, true));
        CHECK (h.value (1, BandParam::freq) == Catch::Approx (150.0f).epsilon (0.02));
        CHECK (h.value (1, BandParam::gain) == Catch::Approx (0.0f).margin (0.01f));
    }

    SECTION ("dragging past the edges stays inside 20 Hz to 20 kHz and +-18 dB")
    {
        const auto p = s.nodePosition (1);
        s.mouseDown (makeEvent (s, p));
        s.mouseDrag (makeEvent (s, { (float) s.getWidth() + 500.0f, -500.0f }, true));
        s.mouseUp (makeEvent (s, { (float) s.getWidth() + 500.0f, -500.0f }, true));
        CHECK (h.value (2, BandParam::freq) <= 20000.0f);
        CHECK (h.value (2, BandParam::gain) == Catch::Approx (18.0f).margin (0.1f));
    }
}

TEST_CASE ("Double-clicking a node bypasses it, the wheel changes Q", "[spectrum]")
{
    Harness h;
    auto& s = h.spectrum();
    const auto p = s.nodePosition (1);

    CHECK (h.value (2, BandParam::enabled) > 0.5f);
    s.mouseDoubleClick (makeEvent (s, p, false, 2));
    CHECK (h.value (2, BandParam::enabled) < 0.5f);
    CHECK (h.value (2, BandParam::active) > 0.5f); // bypassed, not removed
    s.mouseDoubleClick (makeEvent (s, p, false, 2));
    CHECK (h.value (2, BandParam::enabled) > 0.5f);

    const auto q = h.value (2, BandParam::q);
    juce::MouseWheelDetails wheel;
    wheel.deltaY = 0.5f;
    s.mouseWheelMove (makeEvent (s, p), wheel);
    CHECK (h.value (2, BandParam::q) > q);
    wheel.deltaY = -2.0f;
    s.mouseWheelMove (makeEvent (s, p), wheel);
    CHECK (h.value (2, BandParam::q) < q);
}

TEST_CASE ("The band editor follows the selected band", "[spectrum][bandeditor]")
{
    Harness h;
    auto& s = h.spectrum();
    auto& editor = h.bands();

    // band 2 (index 1) is selected: its frequency is 3 kHz
    CHECK (editor.getFrequency().getSlider().getValue() == Catch::Approx (3000.0).epsilon (0.01));

    // moving the band elsewhere shows up in the editor
    setParam (h.plugin, ParamIDs::bandParamID (2, BandParam::freq), 4600.0f);
    CHECK (editor.getFrequency().getSlider().getValue() == Catch::Approx (4600.0).epsilon (0.01));

    // and editing in the panel changes the band
    editor.getFrequency().getSlider().setValue (1200.0, juce::sendNotificationSync);
    CHECK (h.value (2, BandParam::freq) == Catch::Approx (1200.0f).epsilon (0.02));

    // selecting another node re-binds the panel to it
    s.mouseDown (makeEvent (s, s.nodePosition (2)));
    s.mouseUp (makeEvent (s, s.nodePosition (2)));
    CHECK (editor.getBand() == 2);
    CHECK (editor.getFrequency().getSlider().getValue() == Catch::Approx (16000.0).epsilon (0.01));

    // a deleted band is no longer selected
    setParam (h.plugin, ParamIDs::bandParamID (3, BandParam::active), 0.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
    CHECK (s.getSelectedBand() != 2);
}
