#include "helpers/signal_helpers.h"
#include <PluginEditor.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Renders the editor at the design's 1480 x 860 in each state so it can be compared with the Figma export.
// Set VOX_SNAPSHOT_DIR to a folder to keep the PNGs.
namespace
{
void save (const juce::Image& image, const juce::String& name)
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VOX_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
        return;

    auto file = juce::File (dir).getChildFile (name + ".png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    png.writeImageToStream (image, stream);
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

    juce::Image snapshot() { return editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f); }

    PluginProcessor plugin;
    std::unique_ptr<PluginEditor> editor;
};
} // namespace

TEST_CASE ("The editor renders at the design size in every state", "[editor][snapshot]")
{
    Harness h;
    REQUIRE (h.editor->getWidth() == 1480);

    auto image = h.snapshot();
    REQUIRE (image.isValid());
    CHECK (image.getWidth() == 1480);
    CHECK (image.getHeight() == 860);
    save (image, "juce-default");

    SECTION ("with the Vocal silk preset, which uses the values shown in the design")
    {
        h.plugin.setCurrentProgram (0);
        save (h.snapshot(), "juce-vocal-silk");
    }

    SECTION ("delta")
    {
        testing_helpers::setParam (h.plugin, ParamIDs::delta, 1.0f);
        auto delta = h.snapshot();
        save (delta, "juce-delta");
        CHECK (delta.getPixelAt (200, 540) != image.getPixelAt (200, 540)); // the rail went warm
    }

    SECTION ("bypass")
    {
        testing_helpers::setParam (h.plugin, ParamIDs::bypass, 1.0f);
        auto bypassed = h.snapshot();
        save (bypassed, "juce-bypass");
        CHECK (bypassed.getPixelAt (200, 540).getSaturation() < image.getPixelAt (200, 540).getSaturation());
    }

    SECTION ("advanced drawer and popovers")
    {
        h.editor->getContent().setDrawerOpen (true, false);
        save (h.snapshot(), "juce-drawer");

        h.editor->getContent().showPopover (vox::Popover::Kind::settings);
        save (h.snapshot(), "juce-settings");
        h.editor->getContent().showPopover (vox::Popover::Kind::help);
        save (h.snapshot(), "juce-help");
    }

    SECTION ("the window keeps the design's aspect ratio when scaled")
    {
        h.editor->setSize (1036, 602);
        auto small = h.editor->createComponentSnapshot (h.editor->getLocalBounds(), true, 1.0f);
        CHECK (small.getWidth() == 1036);
        save (small, "juce-small");
        CHECK (h.plugin.getUiScale() == Catch::Approx (0.7f).margin (0.01f));
    }
}

TEST_CASE ("Every look renders in each state and is saved with the session", "[editor][snapshot]")
{
    for (int i = 0; i < vox::numLooks; ++i)
    {
        const auto look = (vox::Look) i;
        const auto name = "look-" + juce::String (vox::lookName (look)).toLowerCase();

        Harness h;
        h.editor->getContent().setLook (look);
        CHECK (h.plugin.getLook() == i);

        // some resonant material, so the graph shows the look's reduction colours
        auto signal = testing_helpers::whiteNoise (24000, 0.05f);
        testing_helpers::addSine (signal, 350.0, 0.3f);
        testing_helpers::addSine (signal, 2800.0, 0.25f);
        testing_helpers::addSine (signal, 6500.0, 0.15f);
        testing_helpers::run (h.plugin, signal);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);

        auto image = h.snapshot();
        save (image, name);

        testing_helpers::setParam (h.plugin, ParamIDs::delta, 1.0f);
        auto delta = h.snapshot();
        save (delta, name + "-delta");
        CHECK (delta.getPixelAt (200, 540) != image.getPixelAt (200, 540));
        testing_helpers::setParam (h.plugin, ParamIDs::delta, 0.0f);

        testing_helpers::setParam (h.plugin, ParamIDs::bypass, 1.0f);
        save (h.snapshot(), name + "-bypass");
        testing_helpers::setParam (h.plugin, ParamIDs::bypass, 0.0f);

        h.editor->getContent().setDrawerOpen (true, false);
        h.editor->getContent().showPopover (vox::Popover::Kind::settings);
        save (h.snapshot(), name + "-panels");

        // the look comes back with the session, and a new editor opens in it
        juce::MemoryBlock state;
        h.plugin.getStateInformation (state);
        PluginProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (restored.getLook() == i);
    }
}
