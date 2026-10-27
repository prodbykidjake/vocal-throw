#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Colours, sizes and the delta/bypassed state of the editor.

    The editor is drawn in "design pixels" on a fixed 1480 x 860 canvas (see layout) and scaled to the real
    window with a transform. The look is dark and minimal: the spectrum fills the window and the controls
    float over it on glass panels. */
namespace vox
{
namespace layout
{
    inline constexpr int designWidth = 1480;
    inline constexpr int designHeight = 860;
    inline constexpr int margin = 24;      // between the canvas edge and the panels
    inline constexpr int headerHeight = 60;
    inline constexpr int dockHeight = 84;   // the band editor along the bottom
} // namespace layout

/** Every colour the editor paints with. */
struct Palette
{
    // background
    juce::Colour bg { 0xff080a11 };
    juce::Colour bgDeep { 0xff030408 };
    juce::Colour bgGlow { 0xff141b2e }; // the soft light behind the graph

    // glass panels
    juce::Colour glass { 0xff0d121c };
    juce::Colour edge { 0xffffffff }; // hairline borders, used at low alpha

    // text, white at three strengths
    juce::Colour ink { 0xffeef3ff };
    juce::Colour inkSoft { 0x9eeef3ff };
    juce::Colour inkDim { 0x5ceef3ff };

    // accents
    juce::Colour accent { 0xff5cf2d0 };  // electric mint
    juce::Colour accent2 { 0xff8b7bff }; // violet: the depth curve and secondary highlights
    juce::Colour onAccent { 0xff04110d };
    juce::Colour track { 0x17ffffff };   // the unlit part of arcs and sliders

    float glow = 1.0f; // bypass turns the glow off
};

/** Shared by every component. Delta swaps the accents to warm amber and coral; bypass desaturates and
    dims everything and switches the glow off. */
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

    /** Applies the bypass desaturation to any colour that isn't a palette entry (the band colours). */
    juce::Colour adj (juce::Colour c) const noexcept { return bypassed ? c.withMultipliedSaturation (0.25f).withMultipliedBrightness (0.7f) : c; }

    /** The colour of band `index` (0-based) in the depth curve. */
    static juce::Colour bandColour (int index) noexcept
    {
        static const juce::Colour colours[] = {
            juce::Colour (0xff4fd1ff), juce::Colour (0xffffd166), juce::Colour (0xffff9f5a), juce::Colour (0xffff6b8b),
            juce::Colour (0xffb18cff), juce::Colour (0xff7ee081), juce::Colour (0xff6b9bff), juce::Colour (0xffff7ad9)
        };
        return colours[(size_t) juce::jlimit (0, 7, index)];
    }

    /** Strokes a path as a glowing line: two soft halos under a crisp core. */
    void strokeGlow (juce::Graphics& g, const juce::Path& path, juce::Colour colour, float thickness) const
    {
        if (palette.glow > 0.0f)
        {
            g.setColour (colour.withMultipliedAlpha (0.07f * palette.glow));
            g.strokePath (path, juce::PathStrokeType (thickness * 5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (colour.withMultipliedAlpha (0.16f * palette.glow));
            g.strokePath (path, juce::PathStrokeType (thickness * 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    /** A glass panel: translucent fill, a hairline border and a faint highlight along the top edge. */
    void paintGlass (juce::Graphics& g, juce::Rectangle<float> area, float radius = 14.0f, float opacity = 0.8f) const
    {
        juce::Path p;
        p.addRoundedRectangle (area, radius);
        juce::DropShadow (juce::Colours::black.withAlpha (0.45f), 28, { 0, 10 }).drawForPath (g, p);
        g.setColour (palette.glass.withAlpha (opacity));
        g.fillPath (p);
        g.setGradientFill (juce::ColourGradient (palette.edge.withAlpha (0.045f), 0.0f, area.getY(), palette.edge.withAlpha (0.0f), 0.0f,
            area.getY() + juce::jmin (90.0f, area.getHeight()), false));
        g.fillPath (p);
        g.setColour (palette.edge.withAlpha (0.09f));
        g.strokePath (p, juce::PathStrokeType (1.0f));
    }

private:
    void rebuild()
    {
        palette = Palette();
        if (delta)
        {
            palette.accent = juce::Colour (0xffffb35c);
            palette.accent2 = juce::Colour (0xffff6b8b);
            palette.bgGlow = juce::Colour (0xff22170f);
            palette.onAccent = juce::Colour (0xff1a0e02);
        }

        if (bypassed)
        {
            for (auto* c : { &palette.accent, &palette.accent2, &palette.bgGlow })
                *c = c->withMultipliedSaturation (0.2f).withMultipliedBrightness (0.65f);
            palette.ink = palette.ink.withMultipliedAlpha (0.7f);
            palette.inkSoft = palette.inkSoft.withMultipliedAlpha (0.7f);
            palette.glow = 0.0f;
        }
    }

    Palette palette;
    bool delta = false;
    bool bypassed = false;
};
} // namespace vox
