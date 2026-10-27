#include "EditorContent.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
namespace
{
    using namespace layout;
    constexpr int leftX = margin;
    constexpr int rightX = designWidth - margin - columnWidth;
    constexpr int dockY = designHeight - margin - dockHeight;
}

void GlassPanel::paint (juce::Graphics& g)
{
    theme.paintGlass (g, getLocalBounds().toFloat().reduced (1.0f));
    fonts::draw (g, title, { 20.0f, 14.0f, (float) getWidth() - 40.0f, 16.0f }, fonts::make (600, 11.0f, 0.24f), theme.p().accent,
        juce::Justification::centredLeft);
}

EditorContent::EditorContent (PluginProcessor& p, Theme& t)
    : processor (p),
      theme (t),
      deltaAttachment (*p.apvts.getParameter (ParamIDs::delta), [this] (float v) { deltaOn = v >= 0.5f; updateTheme(); }, p.apvts.undoManager),
      bypassAttachment (*p.apvts.getParameter (ParamIDs::bypass), [this] (float v) { bypassOn = v >= 0.5f; updateTheme(); }, p.apvts.undoManager),
      spectrum (t, p),
      detectPanel (t, "detect"),
      stereoPanel (t, "stereo"),
      timePanel (t, "time"),
      outputPanel (t, "output"),
      sidechainPanel (t, "sidechain"),
      qualityPanel (t, "quality"),
      mode (t, p.apvts, ParamIDs::mode, { "SOFT", "HARD" }),
      depth (t, p.apvts, ParamIDs::depth, "DEPTH", fmt::percent, true),
      detail (t, p.apvts, ParamIDs::detail, "DETAIL", fmt::percent),
      selectivity (t, p.apvts, ParamIDs::selectivity, "SELECTIVITY", fmt::percent),
      balance (t, p.apvts, ParamIDs::stereoBalance, "BALANCE", fmt::balance),
      stereo (t, p.apvts, ParamIDs::stereoMode, { "L/R", "M/S" }),
      link (t, p.apvts, ParamIDs::link, "LINK", fmt::percent),
      attack (t, p.apvts, ParamIDs::attack, "ATTACK", fmt::percent),
      release (t, p.apvts, ParamIDs::release, "RELEASE", fmt::percent),
      quality (t, p.apvts, ParamIDs::quality, { "Normal", "High", "Ultra" }, true, 12.0f, 500),
      lock (t, p.apvts),
      sidechain (t, p.apvts, ParamIDs::sidechain, CircleIconToggle::Icon::power, "External sidechain"),
      keyListen (t, p.apvts, ParamIDs::keyListen, CircleIconToggle::Icon::keyListen, "Key listen: hear the sidechain key"),
      meter (t, p),
      mix (t, p.apvts, ParamIDs::mix, "MIX", fmt::percent),
      trim (t, p.apvts, ParamIDs::outGain, "TRIM", fmt::decibels),
      delta (t, p.apvts, ParamIDs::delta, "DELTA"),
      bypass (t, p.apvts, ParamIDs::bypass, "BYPASS"),
      topBar (t, p),
      bandEditor (t, p.apvts),
      drawer (t, p.apvts),
      popover (t)
{
    stereo.setTooltip ("Stereo mode: left/right or mid/side");
    quality.setTooltip ("Quality: how often the filter is refreshed (more is smoother, and costs more CPU)");
    delta.setTooltip ("Delta: hear only what is being removed");
    bypass.setTooltip ("Bypass (soft, click-free)");

    balance.setBipolar (true);
    trim.setBipolar (true);

    // back to front
    for (auto* c : std::initializer_list<juce::Component*> { &spectrum, &detectPanel, &stereoPanel, &timePanel, &outputPanel, &sidechainPanel, &qualityPanel,
             &mode, &depth, &detail, &selectivity, &balance, &stereo, &link, &attack, &release, &quality, &lock, &sidechain, &keyListen, &meter,
             &mix, &trim, &delta, &bypass, &bandEditor, &topBar, &drawer, &popover })
        addAndMakeVisible (c);

    popover.setVisible (false);
    popover.onClose = [this] { popover.setVisible (false); };
    popover.onScale = [this] (float s) {
        if (onScaleRequested)
            onScaleRequested (s);
        popover.show (popover.getKind(), s);
    };

    topBar.onAdvanced = [this] { toggleDrawer(); };
    topBar.onHelp = [this] { togglePopover (Popover::Kind::help); };
    topBar.onSettings = [this] { togglePopover (Popover::Kind::settings); };

    spectrum.onSelectionChanged = [this] (int band) { bandEditor.setBand (band); };
    {
        // start on the neutral bell, like the design's selected band
        int first = -1;
        for (int b = ParamIDs::numBands - 1; b >= 0; --b)
            if (processor.apvts.getRawParameterValue (ParamIDs::bandParamID (b + 1, ParamIDs::BandParam::active))->load() >= 0.5f)
                first = b;
        const auto bell = processor.apvts.getRawParameterValue (ParamIDs::bandParamID (2, ParamIDs::BandParam::active))->load() >= 0.5f;
        spectrum.setSelectedBand (bell ? 1 : first);
    }

    theme.addChangeListener (this);
    deltaAttachment.sendInitialUpdate();
    bypassAttachment.sendInitialUpdate();
    updateTheme();
}

EditorContent::~EditorContent()
{
    theme.removeChangeListener (this);
}

void EditorContent::updateTheme()
{
    theme.setState (deltaOn, bypassOn);
}

void EditorContent::toggleDrawer()
{
    setDrawerOpen (! drawerOpen);
}

void EditorContent::setDrawerOpen (bool open, bool animate)
{
    drawerOpen = open;
    topBar.setAdvancedOpen (drawerOpen);
    const juce::Rectangle<int> target (drawerOpen ? designWidth - margin - AdvancedDrawer::width : designWidth, columnTop,
        AdvancedDrawer::width, AdvancedDrawer::height);

    if (animate)
    {
        animator.animateComponent (&drawer, target, 1.0f, 180, false, 1.0, 1.0);
    }
    else
    {
        animator.cancelAllAnimations (false);
        drawer.setBounds (target);
    }
}

void EditorContent::showPopover (Popover::Kind kind)
{
    popover.show (kind, processor.getUiScale());
    popover.setTopLeftPosition (designWidth - margin - Popover::width, headerHeight - 4);
    popover.setVisible (true);
    popover.toFront (false);
}

void EditorContent::togglePopover (Popover::Kind kind)
{
    if (popover.isVisible() && popover.getKind() == kind)
    {
        popover.setVisible (false);
        return;
    }

    showPopover (kind);
}

//==============================================================================
void EditorContent::resized()
{
    spectrum.setBounds (getLocalBounds());
    spectrum.setPlotInsets ({ headerHeight, leftX + columnWidth + 16, designHeight - dockY + 12, margin + columnWidth + 16 });
    topBar.setBounds (margin, 0, designWidth - 2 * margin, headerHeight);
    bandEditor.setBounds (margin, dockY, BandEditor::width, BandEditor::height);

    // left column: detect, stereo
    detectPanel.setBounds (leftX, columnTop, columnWidth, 398);
    depth.setBounds (leftX, columnTop + 34, columnWidth, Knob::heightFor (true));
    mode.setBounds (leftX + 24, columnTop + 238, columnWidth - 48, 32);
    detail.setBounds (leftX + 14, columnTop + 284, 118, Knob::heightFor (false));
    selectivity.setBounds (leftX + columnWidth - 14 - 118, columnTop + 284, 118, Knob::heightFor (false));

    const auto stereoTop = columnTop + 414;
    stereoPanel.setBounds (leftX, stereoTop, columnWidth, columnBottom - stereoTop);
    stereo.setBounds (leftX + 24, stereoTop + 58, 96, 30);
    balance.setBounds (leftX + columnWidth - 14 - 118, stereoTop + 42, 118, Knob::heightFor (false));
    link.setBounds (leftX + 24, stereoTop + 168, columnWidth - 48, ValueSlider::height);

    // right column: time, output, sidechain, quality
    timePanel.setBounds (rightX, columnTop, columnWidth, 178);
    attack.setBounds (rightX + 14, columnTop + 44, 118, Knob::heightFor (false));
    release.setBounds (rightX + columnWidth - 14 - 118, columnTop + 44, 118, Knob::heightFor (false));

    const auto outputTop = columnTop + 194;
    outputPanel.setBounds (rightX, outputTop, columnWidth, 232);
    mix.setBounds (rightX + 24, outputTop + 46, columnWidth - 48, ValueSlider::height);
    trim.setBounds (rightX + 24, outputTop + 100, columnWidth - 48, ValueSlider::height);
    delta.setBounds (rightX + 24, outputTop + 164, 104, 32);
    bypass.setBounds (rightX + columnWidth - 24 - 104, outputTop + 164, 104, 32);

    const auto sidechainTop = outputTop + 248;
    sidechainPanel.setBounds (rightX, sidechainTop, columnWidth, 110);
    sidechain.setBounds (rightX + 20, sidechainTop + 46, 42, 42);
    keyListen.setBounds (rightX + 68, sidechainTop + 46, 42, 42);
    meter.setBounds (rightX + 128, sidechainTop + 56, columnWidth - 152, 22);

    const auto qualityTop = sidechainTop + 126;
    qualityPanel.setBounds (rightX, qualityTop, columnWidth, columnBottom - qualityTop);
    quality.setBounds (rightX + 24, qualityTop + 44, columnWidth - 92, 32);
    lock.setBounds (rightX + columnWidth - 24 - 32, qualityTop + 44, 32, 32);

    if (! animator.isAnimating())
        drawer.setBounds (drawerOpen ? designWidth - margin - AdvancedDrawer::width : designWidth, columnTop, AdvancedDrawer::width, AdvancedDrawer::height);
}

void EditorContent::paint (juce::Graphics& g)
{
    // the spectrum covers everything; this only shows while it is being laid out
    g.fillAll (theme.p().bgDeep);
}
} // namespace vox
