#include "helpers/signal_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <ui/Controls.h>
#include <ui/Fonts.h>
#include <ui/Icons.h>
#include <ui/VoxLookAndFeel.h>

// Renders every control primitive on both rail colours. Set VOX_GALLERY_PATH to save the PNG and look at it.
namespace
{
class Gallery : public juce::Component
{
public:
    Gallery (vox::Theme& t, PluginProcessor& p) : theme (t)
    {
        using namespace vox;
        setLookAndFeel (&laf);
        laf.setDefaultSansSerifTypeface (fonts::make (400, 12.0f).getTypefacePtr());

        large = std::make_unique<Knob> (theme, p.apvts, ParamIDs::depth, "DEPTH", fmt::percent, true);
        small = std::make_unique<Knob> (theme, p.apvts, ParamIDs::detail, "DETAIL", fmt::percent);
        deepSmall = std::make_unique<Knob> (theme, p.apvts, ParamIDs::attack, "ATTACK", fmt::percent, false, true);
        balance = std::make_unique<Knob> (theme, p.apvts, ParamIDs::stereoBalance, "BALANCE", fmt::balance);
        mix = std::make_unique<ValueSlider> (theme, p.apvts, ParamIDs::mix, "MIX", fmt::percent);
        trim = std::make_unique<ValueSlider> (theme, p.apvts, ParamIDs::outGain, "TRIM", fmt::decibels);
        mode = std::make_unique<SegmentedControl> (theme, p.apvts, ParamIDs::mode, juce::StringArray { "SOFT", "HARD" });
        stereo = std::make_unique<ChoiceButton> (theme, p.apvts, ParamIDs::stereoMode, juce::StringArray { "L/R", "M/S" });
        quality = std::make_unique<ChoiceButton> (theme, p.apvts, ParamIDs::quality, juce::StringArray { "Normal", "High", "Ultra" }, true, 10.0f, 400);
        delta = std::make_unique<RoundToggle> (theme, p.apvts, ParamIDs::delta, "DELTA");
        bypass = std::make_unique<RoundToggle> (theme, p.apvts, ParamIDs::bypass, "BYPASS");
        sidechain = std::make_unique<CircleIconToggle> (theme, p.apvts, ParamIDs::sidechain, CircleIconToggle::Icon::power, "sidechain");
        keyListen = std::make_unique<CircleIconToggle> (theme, p.apvts, ParamIDs::keyListen, CircleIconToggle::Icon::keyListen, "key");

        for (auto* c : std::initializer_list<juce::Component*> { large.get(), small.get(), deepSmall.get(), balance.get(), mix.get(), trim.get(),
                 mode.get(), stereo.get(), quality.get(), delta.get(), bypass.get(), sidechain.get(), keyListen.get() })
            addAndMakeVisible (c);
    }

    ~Gallery() override { setLookAndFeel (nullptr); }

    void resized() override
    {
        large->setBounds (30, 10, 240, vox::Knob::heightFor (true));
        small->setBounds (290, 10, 110, vox::Knob::heightFor (false));
        balance->setBounds (290, 150, 110, vox::Knob::heightFor (false));
        mode->setBounds (30, 255, 232, 32);
        stereo->setBounds (30, 300, 90, 30);
        delta->setBounds (150, 300, 110, 28);
        bypass->setBounds (150, 335, 110, 28);
        deepSmall->setBounds (440, 10, 110, vox::Knob::heightFor (false));
        quality->setBounds (440, 150, 119, 30);
        sidechain->setBounds (440, 195, 40, 40);
        keyListen->setBounds (490, 195, 40, 40);
        mix->setBounds (440, 250, 119, vox::ValueSlider::height);
        trim->setBounds (440, 320, 119, vox::ValueSlider::height);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme.p();
        g.setColour (pal.rail);
        g.fillRect (0, 0, 420, getHeight());
        g.setColour (pal.railDeep);
        g.fillRect (420, 0, getWidth() - 420, getHeight());

        // the wave logo and the icon set
        vox::icons::stroke (g, vox::icons::wave(), juce::Rectangle<float> (34.0f, 18.0f).translated (290.0f, 330.0f), pal.accent, 2.0f);
        vox::fonts::draw (g, "VOX SMOOVE", { 290.0f, 352.0f, 130.0f, 30.0f }, vox::fonts::make (700, 18.0f, 0.09f), juce::Colour (0xff33444d), juce::Justification::centredLeft);

        for (int shape = 0; shape < vs::numBandShapes; ++shape)
            vox::icons::stroke (g, vox::icons::bandShape ((vs::BandShape) shape), juce::Rectangle<float> (20.0f, 22.0f).translated (580.0f + 36.0f * (float) (shape % 4), 20.0f + 36.0f * (float) (shape / 4)), pal.accent, 2.0f);
        vox::icons::stroke (g, vox::icons::lock (true), juce::Rectangle<float> (18.0f, 18.0f).translated (580.0f, 100.0f), pal.muted, 1.5f);
        vox::icons::stroke (g, vox::icons::lock (false), juce::Rectangle<float> (18.0f, 18.0f).translated (610.0f, 100.0f), pal.muted, 1.5f);
        vox::icons::stroke (g, vox::icons::chevron (false), juce::Rectangle<float> (18.0f, 18.0f).translated (640.0f, 100.0f), pal.muted, 1.5f);
        vox::icons::stroke (g, vox::icons::chevron (true), juce::Rectangle<float> (18.0f, 18.0f).translated (670.0f, 100.0f), pal.muted, 1.5f);
        vox::icons::stroke (g, vox::icons::plus(), juce::Rectangle<float> (18.0f, 18.0f).translated (700.0f, 100.0f), pal.muted, 1.5f);
        g.setColour (pal.muted);
        juce::Path gear = vox::icons::gear();
        g.fillPath (gear, juce::AffineTransform::scale (20.0f).translated (580.0f, 130.0f));

        const char* sample = "Aa 0123456789 +18 \xe2\x88\x92" "6 \xc2\xb7 \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 \xe2\x80\xb9 \xe2\x80\xba";
        vox::fonts::draw (g, juce::CharPointer_UTF8 (sample), { 580.0f, 170.0f, 150.0f, 20.0f }, vox::fonts::make (500, 12.0f), pal.ink, juce::Justification::centredLeft);
    }

private:
    vox::Theme& theme;
    vox::VoxLookAndFeel laf { theme };
    std::unique_ptr<vox::Knob> large, small, deepSmall, balance;
    std::unique_ptr<vox::ValueSlider> mix, trim;
    std::unique_ptr<vox::SegmentedControl> mode;
    std::unique_ptr<vox::ChoiceButton> stereo, quality;
    std::unique_ptr<vox::RoundToggle> delta, bypass;
    std::unique_ptr<vox::CircleIconToggle> sidechain, keyListen;
};

void save (const juce::Image& image, const juce::String& envVar, const juce::String& suffix)
{
    const auto base = juce::SystemStats::getEnvironmentVariable (envVar, {});
    if (base.isEmpty())
        return;

    juce::File file (base.replace (".png", suffix + ".png"));
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    png.writeImageToStream (image, stream);
}
} // namespace

TEST_CASE ("UI primitives render", "[ui]")
{
    PluginProcessor plugin;
    vox::Theme theme;
    Gallery gallery (theme, plugin);
    gallery.setSize (760, 430);

    auto image = gallery.createComponentSnapshot (gallery.getLocalBounds(), true, 1.0f);
    REQUIRE (image.isValid());
    CHECK (image.getWidth() == 760);
    save (image, "VOX_GALLERY_PATH", "");

    SECTION ("delta and bypass restyle without rebuilding")
    {
        theme.setState (true, false);
        auto delta = gallery.createComponentSnapshot (gallery.getLocalBounds(), true, 1.0f);
        save (delta, "VOX_GALLERY_PATH", "-delta");
        CHECK (delta.getPixelAt (5, 5) != image.getPixelAt (5, 5)); // the rail went warm

        theme.setState (false, true);
        auto bypassed = gallery.createComponentSnapshot (gallery.getLocalBounds(), true, 1.0f);
        save (bypassed, "VOX_GALLERY_PATH", "-bypass");
        CHECK (bypassed.getPixelAt (5, 5).getSaturation() < image.getPixelAt (5, 5).getSaturation());
    }
}
