#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Colours, sizes and the look/delta/bypassed state of the editor.

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

/** Every colour the editor paints with. The defaults are the Figma design (the Mist look). Translucent
    overlays use `shade` (darkening), `tint` (section washes) or `surface` (raised fields) with the alpha
    the design gives them, so a look only has to pick the base colours. */
struct Palette
{
    // text
    juce::Colour title { 0xff33444d };
    juce::Colour ink { 0xff40525d };
    juce::Colour label { 0xff58707c }; // section and knob captions
    juce::Colour soft { 0xff667c87 };  // secondary captions, hints
    juce::Colour muted { 0xff6e838f };

    // areas
    juce::Colour rail { 0xffc1d3db };
    juce::Colour railDeep { 0xffafc5cf };
    juce::Colour graph { 0xffb7cad2 };
    juce::Colour graphDeep { 0xff9fb9c5 };
    juce::Colour topBar { 0xff6f8998 };
    juce::Colour topBarInk { 0xfff4f9fa };
    juce::Colour bottomBar { 0xff1d2428 };
    juce::Colour barText { 0xffedf4f6 };
    juce::Colour barDim { 0xff77878f };
    juce::Colour panel { 0xffc4d6dd };   // band editor and drawer
    juce::Colour popover { 0xffe4eef1 }; // popovers and popup menus
    juce::Colour frame { 0xffecf5f8 };

    // overlays
    juce::Colour shade { 0xff486470 };
    juce::Colour tint { 0xff6f8f9c };
    juce::Colour surface { 0xfff0f7f9 };
    juce::Colour outline { 0xfff4f9fa }; // rings and icons of controls that are off
    juce::Colour sheen { 0xffffffff };
    float sheenAmount = 1.0f; // dark looks tone the highlights down

    // accents
    juce::Colour accent { 0xff32a9ae };
    juce::Colour coral { 0xffec6c68 }; // zero line and the round toggles
    juce::Colour onAccent { 0xffffffff };
    juce::Colour charcoal { 0xff34434b }; // knob faces and the read-out box
    juce::Colour knobArc { 0xfff8fbfc };

    // graph
    juce::Colour graphInk { 0xfffafeff }; // labels, grid, reduction line, node rings
    juce::Colour curve { 0xff3c5561 };    // the dashed depth curve
    juce::Colour reductionTop { 0xffc6e8eb };
    juce::Colour reductionBottom { 0xff77b3bf };
};

/** The alternative looks, chosen in Settings and saved with the session. */
enum class Look
{
    mist,
    midnight,
    graphite,
    sand,
    neon
};

inline constexpr int numLooks = 5;

inline const char* lookName (Look look) noexcept
{
    static const char* const names[] = { "Mist", "Midnight", "Graphite", "Sand", "Neon" };
    return names[(size_t) juce::jlimit (0, numLooks - 1, (int) look)];
}

/** The palette of a look, with or without delta (the warm "what's being removed" variant). */
inline Palette paletteFor (Look look, bool delta)
{
    Palette p;
    auto c = [] (juce::uint32 argb) { return juce::Colour (argb); };

    switch (look)
    {
        case Look::mist:
            if (delta)
            {
                p.accent = c (0xffe88b68);
                p.graph = c (0xffcdbdb5);
                p.graphDeep = c (0xffbca79e);
                p.rail = c (0xffdacbc3);
                p.railDeep = c (0xffcdb9b0);
                p.reductionTop = c (0xfff3ad8a);
                p.reductionBottom = c (0xffeb8663);
            }
            break;

        case Look::midnight:
            p.title = c (0xffeef5f8);
            p.ink = c (0xffdce7ec);
            p.label = c (0xff9fb3bd);
            p.soft = c (0xff8aa0ab);
            p.muted = c (0xff70858f);
            p.rail = c (0xff1b2329);
            p.railDeep = c (0xff161d22);
            p.graph = c (0xff11171b);
            p.graphDeep = c (0xff0c1114);
            p.topBar = c (0xff222c33);
            p.topBarInk = c (0xffe6eff3);
            p.bottomBar = c (0xff080b0d);
            p.barText = c (0xffe6eff3);
            p.barDim = c (0xff5d6d75);
            p.panel = c (0xff1f282e);
            p.popover = c (0xff253038);
            p.frame = c (0xff2a353d);
            p.shade = c (0xff000000);
            p.tint = c (0xff7f9fae);
            p.surface = c (0xff2c3840);
            p.outline = c (0xff8ea2ac);
            p.sheenAmount = 0.35f;
            p.accent = c (0xff3fc6c9);
            p.coral = c (0xffef6f6b);
            p.charcoal = c (0xff0b1013);
            p.knobArc = c (0xffcfe0e6);
            p.graphInk = c (0xffb9c9d0);
            p.curve = c (0xff9fb4be);
            p.reductionTop = c (0xff3fc6c9);
            p.reductionBottom = c (0xff1b6f7a);
            if (delta)
            {
                p.accent = c (0xffef8a63);
                p.graph = c (0xff1d1613);
                p.graphDeep = c (0xff150f0d);
                p.rail = c (0xff2a201c);
                p.railDeep = c (0xff221a17);
                p.reductionTop = c (0xffef8a63);
                p.reductionBottom = c (0xff7a3a26);
            }
            break;

        case Look::graphite:
            p.title = c (0xfff3f0ea);
            p.ink = c (0xffe6e3dd);
            p.label = c (0xffaaa59c);
            p.soft = c (0xff948f87);
            p.muted = c (0xff7c7871);
            p.rail = c (0xff2b2b2e);
            p.railDeep = c (0xff242427);
            p.graph = c (0xff1f1f22);
            p.graphDeep = c (0xff18181a);
            p.topBar = c (0xff343438);
            p.topBarInk = c (0xffece9e3);
            p.bottomBar = c (0xff0d0d0e);
            p.barText = c (0xffece9e3);
            p.barDim = c (0xff6b6862);
            p.panel = c (0xff2f2f33);
            p.popover = c (0xff36363a);
            p.frame = c (0xff3a3a3e);
            p.shade = c (0xff000000);
            p.tint = c (0xff9a958c);
            p.surface = c (0xff3c3c40);
            p.outline = c (0xff9a958c);
            p.sheenAmount = 0.3f;
            p.accent = c (0xfff2a33a);
            p.coral = c (0xffe5604f);
            p.onAccent = c (0xff1a1a1c);
            p.charcoal = c (0xff121214);
            p.knobArc = c (0xfff2a33a);
            p.graphInk = c (0xffcfcac2);
            p.curve = c (0xffb5b0a7);
            p.reductionTop = c (0xfff2a33a);
            p.reductionBottom = c (0xff6d4512);
            if (delta)
            {
                p.accent = c (0xff59b7e6);
                p.knobArc = c (0xff59b7e6);
                p.graph = c (0xff1b1f24);
                p.graphDeep = c (0xff15181c);
                p.rail = c (0xff262b31);
                p.railDeep = c (0xff20252a);
                p.reductionTop = c (0xff59b7e6);
                p.reductionBottom = c (0xff1d4a63);
            }
            break;

        case Look::sand:
            p.title = c (0xff3b322b);
            p.ink = c (0xff4a3f36);
            p.label = c (0xff7a6a5c);
            p.soft = c (0xff8a7a6c);
            p.muted = c (0xff958676);
            p.rail = c (0xffe6ddd0);
            p.railDeep = c (0xffdbd0c0);
            p.graph = c (0xffc2b39c);
            p.graphDeep = c (0xffae9e86);
            p.topBar = c (0xff9a8774);
            p.topBarInk = c (0xfffbf6ef);
            p.bottomBar = c (0xff2a231e);
            p.barText = c (0xfff4ede4);
            p.barDim = c (0xff8f8174);
            p.panel = c (0xffe3d8c9);
            p.popover = c (0xfff1eadf);
            p.frame = c (0xfff4ede4);
            p.shade = c (0xff5a4636);
            p.tint = c (0xff9c8670);
            p.surface = c (0xfffbf6ef);
            p.outline = c (0xfffbf7f1);
            p.accent = c (0xffd06a4a);
            p.coral = c (0xff3a8f8b);
            p.charcoal = c (0xff3a312b);
            p.knobArc = c (0xfffbf7f1);
            p.graphInk = c (0xfffffaf3);
            p.curve = c (0xff5a4636);
            p.reductionTop = c (0xffeaa27c);
            p.reductionBottom = c (0xffb5573a);
            if (delta)
            {
                p.accent = c (0xff4f9a95);
                p.coral = c (0xffd06a4a);
                p.graph = c (0xffb1bdb7);
                p.graphDeep = c (0xffa4b1ab);
                p.rail = c (0xffd6ddd8);
                p.railDeep = c (0xffc8d1cb);
                p.reductionTop = c (0xff8fcac3);
                p.reductionBottom = c (0xff3f857e);
            }
            break;

        case Look::neon:
            p.title = c (0xfffbf6ff);
            p.ink = c (0xffece2ff);
            p.label = c (0xffb7a3d9);
            p.soft = c (0xffa08cc4);
            p.muted = c (0xff8a77ad);
            p.rail = c (0xff241a38);
            p.railDeep = c (0xff1e1530);
            p.graph = c (0xff1a1329);
            p.graphDeep = c (0xff140e20);
            p.topBar = c (0xff2e2147);
            p.topBarInk = c (0xfff4ecff);
            p.bottomBar = c (0xff0a0612);
            p.barText = c (0xfff4ecff);
            p.barDim = c (0xff6e5f8c);
            p.panel = c (0xff2a1f40);
            p.popover = c (0xff30244a);
            p.frame = c (0xff36294f);
            p.shade = c (0xff000000);
            p.tint = c (0xffb08cff);
            p.surface = c (0xff3a2c55);
            p.outline = c (0xffa08cc4);
            p.sheenAmount = 0.3f;
            p.accent = c (0xffff4fd8);
            p.coral = c (0xff4fe3ff);
            p.charcoal = c (0xff100a1c);
            p.knobArc = c (0xffff4fd8);
            p.graphInk = c (0xffe6d9ff);
            p.curve = c (0xffc9b2ff);
            p.reductionTop = c (0xffff4fd8);
            p.reductionBottom = c (0xff6b1d73);
            if (delta)
            {
                p.accent = c (0xff4fe3ff);
                p.coral = c (0xffff4fd8);
                p.knobArc = c (0xff4fe3ff);
                p.graph = c (0xff101a2a);
                p.graphDeep = c (0xff0c1420);
                p.rail = c (0xff17243a);
                p.railDeep = c (0xff131e31);
                p.reductionTop = c (0xff4fe3ff);
                p.reductionBottom = c (0xff155f73);
            }
            break;
    }
    return p;
}

/** Shared by every component. Delta swaps to the look's delta palette, bypass desaturates everything
    except the bottom bar, as in the design. */
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

    void setLook (Look newLook)
    {
        if (newLook == look)
            return;

        look = newLook;
        rebuild();
        sendChangeMessage();
    }

    Look getLook() const noexcept { return look; }
    bool isDelta() const noexcept { return delta; }
    bool isBypassed() const noexcept { return bypassed; }
    const Palette& p() const noexcept { return palette; }

    /** Applies the bypass desaturation to any colour that isn't a palette entry. */
    juce::Colour adj (juce::Colour c) const noexcept { return bypassed ? c.withMultipliedSaturation (0.45f) : c; }

    /** The highlight colour at the design's alpha, toned down in dark looks. */
    juce::Colour sheen (float alpha) const noexcept { return palette.sheen.withAlpha (alpha * palette.sheenAmount); }

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
        palette = paletteFor (look, delta);

        if (bypassed)
        {
            for (auto* c : { &palette.title, &palette.ink, &palette.label, &palette.soft, &palette.muted, &palette.rail, &palette.railDeep,
                     &palette.graph, &palette.graphDeep, &palette.topBar, &palette.panel, &palette.popover, &palette.shade, &palette.tint,
                     &palette.surface, &palette.outline, &palette.accent, &palette.coral, &palette.charcoal, &palette.knobArc,
                     &palette.curve, &palette.reductionTop, &palette.reductionBottom })
                *c = c->withMultipliedSaturation (0.45f);
        }
    }

    Palette palette;
    Look look = Look::mist;
    bool delta = false;
    bool bypassed = false;
};
} // namespace vox
