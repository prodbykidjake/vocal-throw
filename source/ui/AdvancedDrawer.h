#pragma once

#include "Controls.h"

namespace vox
{
/** The slide-out panel for the controls that aren't on the main screen: Max Cut, Wet Trim and the six
    tilts (Detail, Attack and Release, each for the lows and the highs). */
class AdvancedDrawer : public juce::Component
{
public:
    AdvancedDrawer (Theme& theme, juce::AudioProcessorValueTreeState& state);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 300;
    static constexpr int height = 330;

private:
    Theme& theme;
    ValueSlider maxCut, wetTrim;
    ValueSlider detailLow, detailHigh, attackLow, attackHigh, releaseLow, releaseHigh;
};
} // namespace vox
