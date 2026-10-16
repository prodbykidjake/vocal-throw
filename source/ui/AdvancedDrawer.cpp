#include "AdvancedDrawer.h"
#include "../Parameters.h"
#include "Fonts.h"

namespace vox
{
namespace
{
    constexpr int pad = 18;
    constexpr int gap = 12;

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
}

void AdvancedDrawer::resized()
{
    const auto colW = (width - 2 * pad - gap) / 2;
    auto row = [&] (ValueSlider& left, ValueSlider& right, int y) {
        left.setBounds (pad, y, colW, ValueSlider::height);
        right.setBounds (pad + colW + gap, y, colW, ValueSlider::height);
    };
    row (maxCut, wetTrim, 44);
    row (detailLow, detailHigh, 158);
    row (attackLow, attackHigh, 158 + 71);
    row (releaseLow, releaseHigh, 158 + 142);
}

void AdvancedDrawer::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto area = getLocalBounds().toFloat();
    juce::Path p;
    p.addRoundedRectangle (area.getX(), area.getY(), area.getWidth() + 12.0f, area.getHeight(), 10.0f);
    juce::DropShadow (pal.shade.withAlpha (0.22f), 30, { -4, 12 }).drawForPath (g, p);

    g.setColour (pal.panel.withAlpha (0.97f));
    g.fillPath (p);
    g.setColour (theme.sheen (0.25f));
    g.strokePath (p, juce::PathStrokeType (1.0f));

    const auto ink = pal.label;
    fonts::draw (g, "ADVANCED", { (float) pad, 16.0f, 200.0f, 14.0f }, fonts::make (700, 10.0f, 0.08f), ink, juce::Justification::centredLeft);
    fonts::draw (g, "TILT", { (float) pad, 126.0f, 200.0f, 14.0f }, fonts::make (700, 10.0f, 0.08f), ink, juce::Justification::centredLeft);
    fonts::draw (g, "Low acts below 500 Hz, high above 2 kHz.", { (float) pad, 138.0f, (float) width - 2.0f * pad, 14.0f },
        fonts::make (400, 10.0f), pal.soft, juce::Justification::centredLeft);

    g.setColour (pal.shade.withAlpha (0.13f));
    g.fillRect (pad, 114, width - 2 * pad, 1);
}
} // namespace vox
