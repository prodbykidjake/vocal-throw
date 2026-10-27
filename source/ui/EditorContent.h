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
/** One continuous glass strip split into titled sections by hairline dividers. The controls of each
    section are siblings laid on top, placed with sectionBounds(). */
class GlassStrip : public juce::Component
{
public:
    struct Section
    {
        juce::String title;
        int width;
    };

    GlassStrip (Theme& t, std::vector<Section> stripSections) : theme (t), sections (std::move (stripSections)) { setInterceptsMouseClicks (true, false); }

    /** Section `index` in the parent's coordinates (the strip must already have its bounds). */
    juce::Rectangle<int> sectionBounds (int index) const;
    void paint (juce::Graphics&) override;

private:
    Theme& theme;
    std::vector<Section> sections;
};

/** Everything inside the plugin window, laid out in design pixels on the fixed 1480 x 860 canvas. The
    editor scales this one component to the real window size.

    The header floats over the top, then a glass strip of controls, the graph, a second strip and the band
    editor dock along the bottom. */
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

    // the graph, then the strips, then the controls on them
    SpectrumView spectrum;
    GlassStrip topStrip, bottomStrip;

    SegmentedControl mode;
    Knob depth, detail, selectivity, balance;
    ChoiceButton stereo;
    ValueSlider link;

    Knob attack, release;
    ChoiceButton quality;
    LockButton lock;
    CircleIconToggle sidechain, keyListen;
    MiniMeter meter;
    Knob mix;
    ValueSlider trim;
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
