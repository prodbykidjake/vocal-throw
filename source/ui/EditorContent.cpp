#include "EditorContent.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
namespace
{
    using namespace layout;
    constexpr int dockY = designHeight - margin - dockHeight;

    // the two strips framing the graph; section widths add up to the width between the margins
    constexpr int topY = 72, topH = 156;
    constexpr int bottomY = 572, bottomH = 168;
    enum TopSection { sidechainSection, qualitySection, detectSection, modeSection, outputSection };
    enum BottomSection { depthSection, stereoSection, monitorSection, timeSection };

    /** A w x h rectangle centred in a section, nudged by dx and dy. */
    juce::Rectangle<int> centred (juce::Rectangle<int> section, int w, int h, int dx = 0, int dy = 0)
    {
        return juce::Rectangle<int> (w, h).withCentre (section.getCentre()).translated (dx, dy);
    }
}

juce::Rectangle<int> GlassStrip::sectionBounds (int index) const
{
    auto x = getX();
    for (int i = 0; i < index; ++i)
        x += sections[(size_t) i].width;
    return { x, getY(), sections[(size_t) index].width, getHeight() };
}

void GlassStrip::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    theme.paintGlass (g, getLocalBounds().toFloat().reduced (1.0f), 18.0f, 0.82f);

    float x = 0.0f;
    for (size_t i = 0; i < sections.size(); ++i)
    {
        if (i > 0)
        {
            g.setColour (pal.edge.withAlpha (0.08f));
            g.fillRect (x, 18.0f, 1.0f, (float) getHeight() - 36.0f);
        }
        fonts::draw (g, sections[i].title, { x + 20.0f, 14.0f, (float) sections[i].width - 40.0f, 16.0f }, fonts::make (600, 11.0f, 0.24f), pal.accent,
            juce::Justification::centredLeft);
        x += (float) sections[i].width;
    }
}

EditorContent::EditorContent (PluginProcessor& p, Theme& t)
    : processor (p),
      theme (t),
      deltaAttachment (*p.apvts.getParameter (ParamIDs::delta), [this] (float v) { deltaOn = v >= 0.5f; updateTheme(); }, p.apvts.undoManager),
      bypassAttachment (*p.apvts.getParameter (ParamIDs::bypass), [this] (float v) { bypassOn = v >= 0.5f; updateTheme(); }, p.apvts.undoManager),
      spectrum (t, p),
      topStrip (t, { { "sidechain", 200 }, { "quality", 210 }, { "detect", 340 }, { "mode", 260 }, { "output", 422 } }),
      bottomStrip (t, { { "depth", 300 }, { "stereo", 420 }, { "monitor", 280 }, { "time", 432 } }),
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
    for (auto* c : std::initializer_list<juce::Component*> { &spectrum, &topStrip, &bottomStrip,
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
    const juce::Rectangle<int> target (drawerOpen ? designWidth - margin - AdvancedDrawer::width : designWidth, topY + topH + 12,
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
    spectrum.setPlotInsets ({ topY + topH + 12, margin, designHeight - bottomY + 12, margin });
    topBar.setBounds (margin, 0, designWidth - 2 * margin, headerHeight);
    bandEditor.setBounds (margin, dockY, BandEditor::width, BandEditor::height);
    topStrip.setBounds (margin, topY, designWidth - 2 * margin, topH);
    bottomStrip.setBounds (margin, bottomY, designWidth - 2 * margin, bottomH);

    using Size = Knob::Size;
    const auto small = Knob::heightFor (Size::small);
    const auto medium = Knob::heightFor (Size::medium);
    const auto section = [] (const GlassStrip& strip, int index) { return strip.sectionBounds (index); };

    // top: sidechain, quality, detect, mode, output
    auto s = section (topStrip, sidechainSection);
    sidechain.setBounds (centred (s, 42, 42, -66, 10));
    keyListen.setBounds (centred (s, 42, 42, -18, 10));
    meter.setBounds (centred (s, 64, 22, 46, 10));

    s = section (topStrip, qualitySection);
    quality.setBounds (centred (s, 130, 32, -20, 10));
    lock.setBounds (centred (s, 32, 32, 69, 10));

    s = section (topStrip, detectSection);
    detail.setBounds (centred (s, 150, medium, -70, 8));
    selectivity.setBounds (centred (s, 118, small, 92, 14));

    s = section (topStrip, modeSection);
    mode.setBounds (centred (s, 200, 32, 0, 10));

    s = section (topStrip, outputSection);
    mix.setBounds (centred (s, 150, medium, -104, 8));
    trim.setBounds (centred (s, 190, ValueSlider::height, 92, 12));

    // bottom: depth, stereo, monitor, time
    s = section (bottomStrip, depthSection);
    depth.setBounds (centred (s, s.getWidth(), Knob::heightFor (Size::large), 0, 6));

    s = section (bottomStrip, stereoSection);
    stereo.setBounds (centred (s, 90, 30, -140, 10));
    balance.setBounds (centred (s, 112, small, -20, 14));
    link.setBounds (centred (s, 120, ValueSlider::height, 128, 12));

    s = section (bottomStrip, monitorSection);
    delta.setBounds (centred (s, 104, 32, -58, 10));
    bypass.setBounds (centred (s, 104, 32, 58, 10));

    s = section (bottomStrip, timeSection);
    attack.setBounds (centred (s, 118, small, -80, 14));
    release.setBounds (centred (s, 118, small, 80, 14));

    if (! animator.isAnimating())
        drawer.setBounds (drawerOpen ? designWidth - margin - AdvancedDrawer::width : designWidth, topY + topH + 12, AdvancedDrawer::width,
            AdvancedDrawer::height);
}

void EditorContent::paint (juce::Graphics& g)
{
    // the spectrum covers everything; this only shows while it is being laid out
    g.fillAll (theme.p().bgDeep);
}
} // namespace vox
