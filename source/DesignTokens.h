#pragma once

#include <juce_graphics/juce_graphics.h>

// Colours, fonts and layout numbers from the Stack Buddy Figma design.
// All layout numbers are in design pixels: the editor draws at 1180 x 570 and scales to fit.
namespace Design
{
    constexpr int width = 1180;
    constexpr int height = 570;

    inline const juce::Colour outerBackground { 0xff11120f };
    inline const juce::Colour shellBackground { 0xff1b1d19 };
    inline const juce::Colour shellBorder { 0xff353831 };
    inline const juce::Colour headerBackground { 0xff1a1d1a };
    inline const juce::Colour sourceBackground { 0xff151714 };
    inline const juce::Colour waveBackground { 0xff1b1f18 };
    inline const juce::Colour waveGrid { 0xff282c26 };

    inline const juce::Colour stackPanel { 0xff242520 };
    inline const juce::Colour stackPanelBorder { 0xff525842 };
    inline const juce::Colour colorPanel { 0xff21251f };
    inline const juce::Colour colorPanelBorder { 0xff353833 };
    inline const juce::Colour spacePanel { 0xff252623 };
    inline const juce::Colour spacePanelBorder { 0xff3a3a3d };

    inline const juce::Colour lime { 0xffc6f93f };
    inline const juce::Colour red { 0xfffe755a };
    inline const juce::Colour purple { 0xff9c75f6 };

    inline const juce::Colour text { 0xfff4f1e9 };
    inline const juce::Colour textDim { 0xff9a9c90 };
    inline const juce::Colour textFaint { 0xff686b60 };
    inline const juce::Colour control { 0xff252822 };
    inline const juce::Colour controlBorder { 0xff3b3e37 };
    inline const juce::Colour knobTrack { 0xff2d302a };

    // Space Grotesk, embedded in the plugin
    juce::Font medium (float heightInPixels);
    juce::Font bold (float heightInPixels);

    // small spaced-out capitals, used for labels and readouts
    juce::Font mono (float heightInPixels, bool isBold = false);
}
