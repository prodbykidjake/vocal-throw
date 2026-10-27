#include "EditorContent.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
namespace
{
    using namespace layout;
    constexpr int dockY = designHeight - margin - dockHeight;

    // where the tiles float, in design px. Deliberately uneven; the zero line (y ~346) and the area just
    // under it, where the reduction shows, are kept clear, and the left edge stays free for the dB scale.
    const juce::Rectangle<int> sidechainArea { 84, 84, 200, 104 };
    const juce::Rectangle<int> qualityArea { 140, 212, 210, 92 };
    const juce::Rectangle<int> depthArea { 80, 452, 270, 286 };
    const juce::Rectangle<int> detailArea { 400, 84, 300, 162 };
    const juce::Rectangle<int> modeArea { 760, 100, 240, 76 };
    const juce::Rectangle<int> outputArea { 1216, 84, 224, 212 };
    const juce::Rectangle<int> timeArea { 1160, 556, 280, 150 };
    const juce::Rectangle<int> stereoArea { 420, 600, 380, 130 };
    const juce::Rectangle<int> switchesArea { 880, 640, 250, 84 };

    /** A rectangle relative to a tile's top-left corner. */
    juce::Rectangle<int> in (juce::Rectangle<int> tile, int x, int y, int w, int h) { return { tile.getX() + x, tile.getY() + y, w, h }; }
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
      sidechainTile (t, "sidechain"),
      qualityTile (t, "quality"),
      depthTile (t, "depth"),
      detailTile (t, "detect"),
      modeTile (t, "mode"),
      outputTile (t, "output"),
      timeTile (t, "time"),
      stereoTile (t, "stereo"),
      switchesTile (t, "monitor"),
      mode (t, p.apvts, ParamIDs::mode, { "SOFT", "HARD" }),
      depth (t, p.apvts, ParamIDs::depth, "DEPTH", fmt::percent, Knob::Size::large),
      detail (t, p.apvts, ParamIDs::detail, "DETAIL", fmt::percent, Knob::Size::medium),
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
      mix (t, p.apvts, ParamIDs::mix, "MIX", fmt::percent, Knob::Size::medium),
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
    for (auto* c : std::initializer_list<juce::Component*> { &spectrum, &sidechainTile, &qualityTile, &depthTile, &detailTile, &modeTile, &outputTile, &timeTile, &stereoTile, &switchesTile,
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
    const juce::Rectangle<int> target (drawerOpen ? designWidth - margin - AdvancedDrawer::width : designWidth, headerHeight + 12,
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
    spectrum.setPlotInsets ({ headerHeight, margin, designHeight - dockY + 12, margin });
    topBar.setBounds (margin, 0, designWidth - 2 * margin, headerHeight);
    bandEditor.setBounds (margin, dockY, BandEditor::width, BandEditor::height);
    spectrum.setAddButtonPosition ({ (float) timeArea.getRight() - 16.0f, (float) timeArea.getY() - 30.0f });

    using Size = Knob::Size;
    const auto small = Knob::heightFor (Size::small);
    const auto medium = Knob::heightFor (Size::medium);

    sidechainTile.setBounds (sidechainArea);
    sidechain.setBounds (in (sidechainArea, 16, 42, 42, 42));
    keyListen.setBounds (in (sidechainArea, 64, 42, 42, 42));
    meter.setBounds (in (sidechainArea, 120, 52, 64, 22));

    qualityTile.setBounds (qualityArea);
    quality.setBounds (in (qualityArea, 20, 44, 130, 32));
    lock.setBounds (in (qualityArea, 162, 44, 32, 32));

    depthTile.setBounds (depthArea);
    depth.setBounds (in (depthArea, 0, 48, depthArea.getWidth(), Knob::heightFor (Size::large)));

    detailTile.setBounds (detailArea);
    detail.setBounds (in (detailArea, 12, 28, 150, medium));
    selectivity.setBounds (in (detailArea, 170, 50, 118, small));

    modeTile.setBounds (modeArea);
    mode.setBounds (in (modeArea, 20, 34, modeArea.getWidth() - 40, 30));

    outputTile.setBounds (outputArea);
    mix.setBounds (in (outputArea, 0, 32, outputArea.getWidth(), medium));
    trim.setBounds (in (outputArea, 24, 164, outputArea.getWidth() - 48, ValueSlider::height));

    timeTile.setBounds (timeArea);
    attack.setBounds (in (timeArea, 14, 40, 118, small));
    release.setBounds (in (timeArea, timeArea.getWidth() - 14 - 118, 40, 118, small));

    stereoTile.setBounds (stereoArea);
    stereo.setBounds (in (stereoArea, 24, 56, 90, 30));
    balance.setBounds (in (stereoArea, 124, 28, 112, small));
    link.setBounds (in (stereoArea, 250, 50, 106, ValueSlider::height));

    switchesTile.setBounds (switchesArea);
    delta.setBounds (in (switchesArea, 20, 38, 100, 32));
    bypass.setBounds (in (switchesArea, 130, 38, 100, 32));

    if (! animator.isAnimating())
        drawer.setBounds (drawerOpen ? designWidth - margin - AdvancedDrawer::width : designWidth, headerHeight + 12, AdvancedDrawer::width,
            AdvancedDrawer::height);
}

void EditorContent::paint (juce::Graphics& g)
{
    // the spectrum covers everything; this only shows while it is being laid out
    g.fillAll (theme.p().bgDeep);
}
} // namespace vox
