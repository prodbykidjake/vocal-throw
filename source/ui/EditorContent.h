#pragma once

#include "../PluginProcessor.h"
#include "AdvancedDrawer.h"
#include "Controls.h"
#include "BandEditor.h"
#include "Popover.h"
#include "SpectrumView.h"
#include "TopBar.h"
#include "Widgets.h"

namespace vox
{
/** A titled glass panel floating over the graph. The controls inside it are siblings laid on top. */
class GlassPanel : public juce::Component
{
public:
    GlassPanel (Theme& t, const juce::String& panelTitle) : theme (t), title (panelTitle) { setInterceptsMouseClicks (true, false); }
    void paint (juce::Graphics&) override;

private:
    Theme& theme;
    juce::String title;
};

/** Everything inside the plugin window, laid out in design pixels on the fixed 1480 x 860 canvas. The
    editor scales this one component to the real window size.

    The spectrum fills the canvas; the controls sit on glass panels in a column on either side, the band
    editor is a dock along the bottom, and the header floats over the top. */
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

    SpectrumView& getSpectrum() noexcept { return spectrum; }
    BandEditor& getBandEditor() noexcept { return bandEditor; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    void updateTheme();
    void toggleDrawer();
    void togglePopover (Popover::Kind kind);

    PluginProcessor& processor;
    Theme& theme;
    bool deltaOn = false, bypassOn = false;
    juce::ParameterAttachment deltaAttachment, bypassAttachment;

    // the graph, then the panels, then the controls on them
    SpectrumView spectrum;
    GlassPanel detectPanel, stereoPanel, timePanel, outputPanel, sidechainPanel, qualityPanel;

    // left column
    SegmentedControl mode;
    Knob depth, detail, selectivity, balance;
    ChoiceButton stereo;
    ValueSlider link;

    // right column
    Knob attack, release;
    ChoiceButton quality;
    LockButton lock;
    CircleIconToggle sidechain, keyListen;
    MiniMeter meter;
    ValueSlider mix, trim;
    RoundToggle delta, bypass;

    // dock, header and overlays
    TopBar topBar;
    BandEditor bandEditor;
    AdvancedDrawer drawer;
    Popover popover;
    juce::ComponentAnimator animator;
    bool drawerOpen = false;
};
} // namespace vox
