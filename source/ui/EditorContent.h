#pragma once

#include "../PluginProcessor.h"
#include "AdvancedDrawer.h"
#include "Controls.h"
#include "DepthCurveOverlay.h"
#include "Popover.h"
#include "ReductionGraph.h"
#include "TopBar.h"
#include "Widgets.h"

namespace vox
{
/** Everything inside the plugin window, laid out in design pixels on the fixed 1480 x 860 canvas. The
    editor scales this one component to the real window size. */
class EditorContent : public juce::Component, private juce::ChangeListener
{
public:
    EditorContent (PluginProcessor& processor, Theme& theme);
    ~EditorContent() override;

    /** Called when the settings card asks for another window size (0.7, 0.85 or 1.0). */
    std::function<void (float)> onScaleRequested;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** The Advanced drawer; animated unless told otherwise (the tests want it in place straight away). */
    void setDrawerOpen (bool open, bool animate = true);
    bool isDrawerOpen() const noexcept { return drawerOpen; }
    void showPopover (Popover::Kind kind);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void updateTheme();
    void toggleDrawer();
    void togglePopover (Popover::Kind kind);

    PluginProcessor& processor;
    Theme& theme;
    bool deltaOn = false, bypassOn = false;
    juce::ParameterAttachment deltaAttachment, bypassAttachment;

    // main rail
    SegmentedControl mode;
    Knob depth, detail, selectivity, balance;
    ChoiceButton stereo;
    ValueSlider link;

    // utility rail
    Knob attack, release;
    ChoiceButton quality;
    LockButton lock;
    CircleIconToggle sidechain, keyListen;
    MiniMeter meter;
    ValueSlider mix, trim;
    RoundToggle delta, bypass;

    // work area
    TopBar topBar;
    ReductionGraph graph;
    DepthCurveOverlay overlay;
    AdvancedDrawer drawer;
    Popover popover;
    juce::ComponentAnimator animator;
    bool drawerOpen = false;
};
} // namespace vox
