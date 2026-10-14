#pragma once

#include "../dsp/DepthCurve.h"
#include <juce_gui_basics/juce_gui_basics.h>

/** Icons as vector paths in a unit box (0..1 on both axes, y down). DM Sans has none of the design's
    Unicode glyphs, so they are drawn here. Stroke them (rounded ends, about 1/12 of the box) unless noted. */
namespace vox::icons
{
juce::Path power();                       // stroke
juce::Path keyListen();                   // stroke
juce::Path lock (bool locked);            // stroke
juce::Path chevron (bool pointsRight);    // stroke
juce::Path plus();                        // stroke
juce::Path gear();                        // fill
juce::Path bandShape (vs::BandShape shape); // stroke
juce::Path wave();                        // the logo (the design's 42 x 22 viewBox, scaled to the unit box), stroke

/** Scales an icon path into a rectangle and strokes it. */
void stroke (juce::Graphics& g, const juce::Path& unitPath, juce::Rectangle<float> area, juce::Colour colour, float thickness);
} // namespace vox::icons
