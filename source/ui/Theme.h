#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Design tokens taken from the SignalPath Figma Make source (src/index.css).
// The logical design canvas is 1380 x 914 px; the editor scales it to the window.
namespace Theme
{
inline constexpr int designWidth = 1380;
inline constexpr int designHeight = 914;

inline const juce::Colour window { 0xff111617 }, topbar { 0xff151a1c }, modebar { 0xff101415 }, chainPanel { 0xff0e1213 },
    footer { 0xff0a0e0f }, well { 0xff0c1011 }, card { 0xff121718 }, cardDark { 0xff0e1314 }, tabTrack { 0xff090c0d },
    border { 0xff293134 }, borderSoft { 0xff242b2d }, borderCard { 0xff2b3436 }, borderStrong { 0xff30383b },
    text { 0xffdce5e8 }, textBright { 0xffe4ebed }, textMid { 0xffa9b4b6 }, textDim { 0xff798487 }, textMuted { 0xff657174 },
    textFaint { 0xff465154 },
    accent { 0xff54fbcc }, accentDark { 0xff071411 }, purple { 0xffa899ff }, orange { 0xfff2a967 }, coral { 0xffff806f }, amber { 0xfff5c95e };

inline juce::Font sans (float size, bool bold = false, float kerning = 0.0f)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(), size, bold ? juce::Font::bold : juce::Font::plain).withKerningFactor (kerning));
}

inline juce::Font mono (float size, float kerning = 0.0f)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain).withKerningFactor (kerning));
}

inline juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
inline juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

inline juce::String dbText (float v, int decimals = 1)
{
    if (v <= -99.0f)
        return u8 ("−∞");
    return (v < 0 ? u8 ("−") : juce::String()) + juce::String (std::abs (v), decimals);
}

// Draws a rounded panel with the design's 1 px border.
inline void card_ (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour stroke, float radius = 6.0f)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    g.setColour (stroke);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

inline void eyebrow (juce::Graphics& g, const juce::String& t, juce::Rectangle<int> r, juce::Justification j = juce::Justification::topLeft)
{
    g.setColour (textMuted);
    g.setFont (mono (9.0f, 0.18f));
    g.drawText (t.toUpperCase(), r, j, false);
}
}
