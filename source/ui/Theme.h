#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Colours, sizes and the light/delta/bypassed look of the editor.

    The editor is drawn in "design pixels" on a fixed 1480 x 860 canvas (see layout) and scaled to the real
    window with a transform, so every number here matches the Figma export directly. */
namespace vox
{
namespace layout
{
    inline constexpr int designWidth = 1480;
    inline constexpr int designHeight = 860;
    inline constexpr int frame = 2; // the light border around the plugin
    inline constexpr int mainRailWidth = 280;
    inline constexpr int utilityRailWidth = 140;
    inline constexpr int topBarHeight = 42;
    inline constexpr int bottomBarHeight = 48;
} // namespace layout

struct Palette
{
    juce::Colour ink { 0xff40525d };
    juce::Colour muted { 0xff6e838f };
    juce::Colour rail { 0xffc1d3db };
    juce::Colour railDeep { 0xffafc5cf };
    juce::Colour graph { 0xffb7cad2 };
    juce::Colour graphDeep { 0xff9fb9c5 };
    juce::Colour accent { 0xff32a9ae };
    juce::Colour coral { 0xffec6c68 };
    juce::Colour charcoal { 0xff34434b };
    juce::Colour white { 0xfff8fbfc };
    juce::Colour topBar { 0xff6f8998 };
    juce::Colour bottomBar { 0xff1d2428 };
};

/** Shared by every component. Delta swaps to a warm palette, bypass desaturates everything except the
    bottom bar, as in the design. */
class Theme : public juce::ChangeBroadcaster
{
public:
    Theme() { rebuild(); }

    void setState (bool newDelta, bool newBypassed)
    {
        if (newDelta == delta && newBypassed == bypassed)
            return;

        delta = newDelta;
        bypassed = newBypassed;
        rebuild();
        sendChangeMessage();
    }

    bool isDelta() const noexcept { return delta; }
    bool isBypassed() const noexcept { return bypassed; }
    const Palette& p() const noexcept { return palette; }

    /** Applies the bypass desaturation to any colour that isn't a palette entry. */
    juce::Colour adj (juce::Colour c) const noexcept { return bypassed ? c.withMultipliedSaturation (0.45f) : c; }

    /** The colour of band `index` (0-based) in the depth curve. */
    static juce::Colour bandColour (int index) noexcept
    {
        static const juce::Colour colours[] = {
            juce::Colour (0xff45a9b2), juce::Colour (0xffffd36b), juce::Colour (0xfff0a85d), juce::Colour (0xffec777e),
            juce::Colour (0xff9d72c6), juce::Colour (0xff6fbf8b), juce::Colour (0xff5b8bd9), juce::Colour (0xffc96ea8)
        };
        return colours[(size_t) juce::jlimit (0, 7, index)];
    }

private:
    void rebuild()
    {
        palette = Palette();
        if (delta)
        {
            palette.accent = juce::Colour (0xffe88b68);
            palette.graph = juce::Colour (0xffcdbdb5);
            palette.graphDeep = juce::Colour (0xffbca79e);
            palette.rail = juce::Colour (0xffdacbc3);
            palette.railDeep = juce::Colour (0xffcdb9b0);
        }

        if (bypassed)
        {
            for (auto* c : { &palette.ink, &palette.muted, &palette.rail, &palette.railDeep, &palette.graph, &palette.graphDeep,
                     &palette.accent, &palette.coral, &palette.charcoal, &palette.white, &palette.topBar })
                *c = c->withMultipliedSaturation (0.45f);
        }
    }

    Palette palette;
    bool delta = false;
    bool bypassed = false;
};
} // namespace vox
