#include "AdvancedDrawer.h"
#include "../Parameters.h"
#include "Fonts.h"

namespace vox
{
namespace
{
    constexpr int pad = 22;
    constexpr int gap = 18;

    juce::String dot (const char* utf8) { return juce::String (juce::CharPointer_UTF8 (utf8)); }
}

AdvancedDrawer::AdvancedDrawer (Theme& t, juce::AudioProcessorValueTreeState& s)
    : theme (t),
      maxCut (t, s, ParamIDs::maxCut, "MAX CUT", fmt::decibels),
      wetTrim (t, s, ParamIDs::wetTrim, "WET TRIM", fmt::decibels),
      detailLow (t, s, ParamIDs::detailLow, dot ("DETAIL \xc2\xb7 LOW"), fmt::signedPercent),
      detailHigh (t, s, ParamIDs::detailHigh, dot ("DETAIL \xc2\xb7 HIGH"), fmt::signedPercent),
      attackLow (t, s, ParamIDs::attackLow, dot ("ATTACK \xc2\xb7 LOW"), fmt::signedPercent),
      attackHigh (t, s, ParamIDs::attackHigh, dot ("ATTACK \xc2\xb7 HIGH"), fmt::signedPercent),
      releaseLow (t, s, ParamIDs::releaseLow, dot ("RELEASE \xc2\xb7 LOW"), fmt::signedPercent),
      releaseHigh (t, s, ParamIDs::releaseHigh, dot ("RELEASE \xc2\xb7 HIGH"), fmt::signedPercent)
{
    for (auto* c : { &maxCut, &wetTrim, &detailLow, &detailHigh, &attackLow, &attackHigh, &releaseLow, &releaseHigh })
        addAndMakeVisible (c);
    for (auto* c : { &wetTrim, &detailLow, &detailHigh, &attackLow, &attackHigh, &releaseLow, &releaseHigh })
        c->setBipolar (true);
}

void AdvancedDrawer::resized()
{
    const auto colW = (width - 2 * pad - gap) / 2;
    auto row = [&] (ValueSlider& left, ValueSlider& right, int y) {
        left.setBounds (pad, y, colW, ValueSlider::height);
        right.setBounds (pad + colW + gap, y, colW, ValueSlider::height);
    };
    row (maxCut, wetTrim, 50);
    row (detailLow, detailHigh, 162);
    row (attackLow, attackHigh, 162 + 54);
    row (releaseLow, releaseHigh, 162 + 108);
}

void AdvancedDrawer::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    theme.paintGlass (g, getLocalBounds().toFloat().reduced (1.0f), 16.0f, 0.94f);

    const auto title = fonts::make (600, 11.0f, 0.22f);
    fonts::draw (g, "advanced", { (float) pad, 16.0f, 200.0f, 16.0f }, title, pal.accent, juce::Justification::centredLeft);
    fonts::draw (g, "tilt", { (float) pad, 120.0f, 200.0f, 16.0f }, title, pal.accent, juce::Justification::centredLeft);
    fonts::draw (g, "low acts below 500 Hz, high above 2 kHz", { (float) pad, 136.0f, (float) width - 2.0f * pad, 14.0f },
        fonts::make (400, 10.0f, 0.04f), pal.inkDim, juce::Justification::centredLeft);

    g.setColour (pal.edge.withAlpha (0.07f));
    g.fillRect (pad, 108, width - 2 * pad, 1);
}
} // namespace vox
