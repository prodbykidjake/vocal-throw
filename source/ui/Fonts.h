#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vox::fonts
{
/** The four embedded DM Sans weights. Hold a juce::SharedResourcePointer<Typefaces> for as long as fonts are
    being made (the look-and-feel does) so they are loaded once and released before the plugin unloads. */
struct Typefaces
{
    Typefaces();
    juce::Typeface::Ptr regular, medium, semibold, bold;
};

/** DM Sans from the embedded static fonts. `weight` is 400, 500, 600 or 700 (anything else is rounded to
    the nearest). `px` is the CSS font-size of the design; `tracking` is the CSS letter-spacing in em. */
juce::Font make (int weight, float px, float tracking = 0.0f);

/** Width of a line of text in design pixels. */
float width (const juce::Font& font, const juce::String& text);

/** Draws one line of text in the given colour, aligned inside bounds. */
void draw (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> bounds, const juce::Font& font,
    juce::Colour colour, juce::Justification justification = juce::Justification::centred);
} // namespace vox::fonts
