#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vox::fonts
{
/** DM Sans from the embedded static fonts. `weight` is 400, 500, 600 or 700 (anything else is rounded to
    the nearest). `px` is the CSS font-size of the design; `tracking` is the CSS letter-spacing in em. */
juce::Font make (int weight, float px, float tracking = 0.0f);

/** Draws one line of text in the given colour, aligned inside bounds. */
void draw (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> bounds, const juce::Font& font,
    juce::Colour colour, juce::Justification justification = juce::Justification::centred);
} // namespace vox::fonts
