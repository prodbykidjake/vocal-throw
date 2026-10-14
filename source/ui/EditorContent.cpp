#include "EditorContent.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
namespace
{
    juce::Colour rgba (int r, int g, int b, float a) { return juce::Colour::fromFloatRGBA ((float) r / 255.0f, (float) g / 255.0f, (float) b / 255.0f, a); }
    juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

    constexpr int F = layout::frame;
    constexpr int railW = layout::mainRailWidth;
    constexpr int utilW = layout::utilityRailWidth;
    constexpr int workX = F + railW + utilW; // 422
    constexpr int bodyH = layout::designHeight - 2 * F - layout::bottomBarHeight; // 808
}

EditorContent::EditorContent (PluginProcessor& p, Theme& t)
    : processor (p),
      theme (t),
      deltaAttachment (*p.apvts.getParameter (ParamIDs::delta), [this] (float v) { deltaOn = v >= 0.5f; updateTheme(); }, p.apvts.undoManager),
      bypassAttachment (*p.apvts.getParameter (ParamIDs::bypass), [this] (float v) { bypassOn = v >= 0.5f; updateTheme(); }, p.apvts.undoManager),
      mode (t, p.apvts, ParamIDs::mode, { "SOFT", "HARD" }),
      depth (t, p.apvts, ParamIDs::depth, "DEPTH", fmt::percent, true),
      detail (t, p.apvts, ParamIDs::detail, "DETAIL", fmt::percent),
      selectivity (t, p.apvts, ParamIDs::selectivity, "SELECTIVITY", fmt::percent),
      balance (t, p.apvts, ParamIDs::stereoBalance, "BALANCE", fmt::balance),
      stereo (t, p.apvts, ParamIDs::stereoMode, { "L/R", "M/S" }),
      link (t, p.apvts, ParamIDs::link, "LINK", fmt::percent),
      attack (t, p.apvts, ParamIDs::attack, "ATTACK", fmt::percent, false, true),
      release (t, p.apvts, ParamIDs::release, "RELEASE", fmt::percent, false, true),
      quality (t, p.apvts, ParamIDs::quality, { "Normal", "High", "Ultra" }, true, 10.0f, 400),
      lock (t, p.apvts),
      sidechain (t, p.apvts, ParamIDs::sidechain, CircleIconToggle::Icon::power, "External sidechain"),
      keyListen (t, p.apvts, ParamIDs::keyListen, CircleIconToggle::Icon::keyListen, "Key listen: hear the sidechain key"),
      meter (t, p),
      mix (t, p.apvts, ParamIDs::mix, "MIX", fmt::percent),
      trim (t, p.apvts, ParamIDs::outGain, "TRIM", fmt::decibels),
      delta (t, p.apvts, ParamIDs::delta, "DELTA"),
      bypass (t, p.apvts, ParamIDs::bypass, "BYPASS"),
      topBar (t, p),
      spectrum (t, p),
      bandEditor (t, p.apvts),
      drawer (t, p.apvts),
      popover (t)
{
    stereo.setTooltip ("Stereo mode: left/right or mid/side");
    quality.setTooltip ("Quality: how often the filter is refreshed (more is smoother, and costs more CPU)");
    delta.setTooltip ("Delta: hear only what is being removed");
    bypass.setTooltip ("Bypass (soft, click-free)");

    for (auto* c : std::initializer_list<juce::Component*> { &mode, &depth, &detail, &selectivity, &balance, &stereo, &link, &attack, &release, &quality,
             &lock, &sidechain, &keyListen, &meter, &mix, &trim, &delta, &bypass, &spectrum, &bandEditor, &topBar, &drawer, &popover })
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
    const juce::Rectangle<int> target (drawerOpen ? layout::designWidth - F - AdvancedDrawer::width : layout::designWidth, F + layout::topBarHeight,
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
    popover.setTopLeftPosition (layout::designWidth - F - 10 - Popover::width, F + 50);
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
    // main rail
    mode.setBounds (26, 104, 232, 32);
    depth.setBounds (26, 156, 232, Knob::heightFor (true));
    detail.setBounds (26, 402, 109, Knob::heightFor (false));
    selectivity.setBounds (149, 402, 109, Knob::heightFor (false));
    stereo.setBounds (26, 620, 90, 30);
    balance.setBounds (148, 598, 110, Knob::heightFor (false));
    link.setBounds (26, 733, 232, ValueSlider::height);

    // utility rail
    attack.setBounds (F + railW + 8, 39, utilW - 16, Knob::heightFor (false));
    release.setBounds (F + railW + 8, 182, utilW - 16, Knob::heightFor (false));
    quality.setBounds (292, 363, 120, 30);
    lock.setBounds (392, 334, 20, 20);
    sidechain.setBounds (292, 451, 45, 45);
    keyListen.setBounds (343, 451, 45, 45);
    meter.setBounds (292, 500, 120, 12);
    mix.setBounds (292, 572, 120, ValueSlider::height);
    trim.setBounds (292, 644, 120, ValueSlider::height);
    delta.setBounds (292, 713, 120, 28);
    bypass.setBounds (292, 749, 120, 28);

    // work area
    topBar.setBounds (workX, F, layout::designWidth - F - workX, layout::topBarHeight);
    const juce::Rectangle<int> graphArea (workX, F + layout::topBarHeight, layout::designWidth - F - workX, bodyH - layout::topBarHeight);
    spectrum.setBounds (graphArea);
    bandEditor.setBounds (workX + (graphArea.getWidth() - BandEditor::width) / 2, F + bodyH - 18 - BandEditor::height, BandEditor::width, BandEditor::height);

    if (! animator.isAnimating())
        drawer.setBounds (drawerOpen ? layout::designWidth - F - AdvancedDrawer::width : layout::designWidth, F + layout::topBarHeight,
            AdvancedDrawer::width, AdvancedDrawer::height);
}

void EditorContent::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto sectionBorder = rgba (72, 100, 112, 0.13f);
    const auto railBorder = rgba (69, 91, 103, 0.12f);

    g.fillAll (pal.graph);

    // main rail
    const juce::Rectangle<int> mainRail (F, F, railW, bodyH);
    g.setColour (pal.rail);
    g.fillRect (mainRail);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.15f), (float) F, 0.0f, juce::Colours::white.withAlpha (0.0f), (float) (F + railW / 2), 0.0f, false));
    g.fillRect (mainRail);
    g.setColour (railBorder);
    g.fillRect (F + railW - 1, F, 1, bodyH);

    // brand
    icons::stroke (g, icons::wave(), juce::Rectangle<float> (26.0f, 12.0f, 34.0f, 18.0f), pal.accent, 2.0f);
    fonts::draw (g, "VOX SMOOVE", { 26.0f, 31.0f, 232.0f, 26.0f }, fonts::make (700, 26.0f, 0.09f), theme.adj (juce::Colour (0xff33444d)), juce::Justification::centredLeft);
    fonts::draw (g, utf8 ("PLAYBOX AUDIO \xc2\xb7 ") + VERSION, { 26.0f, 64.0f, 232.0f, 13.5f }, fonts::make (400, 9.0f, 0.12f), pal.muted, juce::Justification::centredLeft);

    // stereo section
    g.setColour (rgba (71, 99, 111, 0.13f));
    g.fillRect (26, 549, 232, 1);
    fonts::draw (g, "STEREO", { 26.0f, 563.0f, 200.0f, 24.0f }, fonts::make (700, 10.0f, 0.08f), theme.adj (juce::Colour (0xff58707c)), juce::Justification::centredLeft);
    fonts::draw (g, "MODE", { 26.0f, 598.0f, 100.0f, 13.5f }, fonts::make (400, 9.0f, 0.08f), pal.muted, juce::Justification::centredLeft);

    // utility rail
    const juce::Rectangle<int> utilityRail (F + railW, F, utilW, bodyH);
    g.setColour (pal.railDeep);
    g.fillRect (utilityRail);
    g.setColour (railBorder);
    g.fillRect (F + railW + utilW - 1, F, 1, bodyH);

    g.setColour (rgba (103, 137, 151, 0.1f));
    g.fillRect (F + railW, F, utilW, 29);
    const auto titleColour = theme.adj (juce::Colour (0xff58707c));
    const auto titleFont = fonts::make (700, 10.0f, 0.08f);
    fonts::draw (g, "SPEED", { 292.0f, 10.0f, 120.0f, 15.0f }, titleFont, titleColour, juce::Justification::centredLeft);

    auto section = [&] (int top, int height) {
        g.setColour (rgba (133, 164, 177, 0.2f));
        g.fillRect (F + railW, top, utilW - 1, height);
        g.setColour (sectionBorder);
        g.fillRect (F + railW, top, utilW - 1, 1);
    };
    section (319, 86);  // quality
    section (405, 119); // sidechain
    section (524, 265); // output
    fonts::draw (g, "QUALITY", { 292.0f, 332.0f, 100.0f, 24.0f }, titleFont, titleColour, juce::Justification::centredLeft);
    fonts::draw (g, "SIDECHAIN", { 292.0f, 418.0f, 120.0f, 24.0f }, titleFont, titleColour, juce::Justification::centredLeft);
    fonts::draw (g, "OUTPUT", { 292.0f, 537.0f, 120.0f, 24.0f }, titleFont, titleColour, juce::Justification::centredLeft);

    // work area backdrop (the spectrum covers it from here on)
    g.setColour (pal.graph);
    g.fillRect (workX, F, layout::designWidth - F - workX, bodyH);

    // bottom bar
    const juce::Rectangle<int> bottom (F, F + bodyH, layout::designWidth - 2 * F, layout::bottomBarHeight);
    g.setColour (pal.bottomBar);
    g.fillRect (bottom);
    const auto dim = juce::Colour (0xff77878f);
    fonts::draw (g, "PLAYBOX AUDIO", bottom.toFloat().withTrimmedLeft (16.0f), fonts::make (400, 8.0f, 0.12f), dim, juce::Justification::centredLeft);
    fonts::draw (g, "VOX SMOOVE", bottom.toFloat(), fonts::make (700, 18.0f, 0.07f), juce::Colour (0xffedf4f6));
    fonts::draw (g, theme.isBypassed() ? "BYPASSED" : (theme.isDelta() ? "DELTA" : "ACTIVE"), bottom.toFloat().withTrimmedRight (18.0f),
        fonts::make (400, 8.0f, 0.12f), dim, juce::Justification::centredRight);

    // the light frame around the plugin
    g.setColour (rgba (236, 245, 248, 0.72f));
    g.drawRect (0, 0, layout::designWidth, layout::designHeight, F);
}
} // namespace vox
