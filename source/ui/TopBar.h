#pragma once

#include "Widgets.h"

class PluginProcessor;

namespace vox
{
/** Preset arrows and name on the left; undo, redo, A/B, advanced, help and settings on the right. */
class TopBar : public juce::Component, private juce::Timer
{
public:
    TopBar (Theme& theme, PluginProcessor& processor);
    ~TopBar() override;

    std::function<void()> onAdvanced, onHelp, onSettings;

    void setAdvancedOpen (bool open) { advanced.setActiveLook (open); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class PresetTitle;

    void timerCallback() override;
    void step (int direction);

    Theme& theme;
    PluginProcessor& processor;
    TopBarButton previous, next, undo, redo, a, b, copy, advanced, help, settings;
    std::unique_ptr<PresetTitle> title;
    int shownProgram = -1;
};
} // namespace vox
