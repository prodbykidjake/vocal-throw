#include "SpectrumView.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
namespace
{
    constexpr float sidePad = 28.0f;      // the frequency axis starts and ends this far inside the plot area
    constexpr float zeroFraction = 0.42f; // the zero line, as a fraction of the plot height (reduction hangs below it)
    constexpr float topRoom = 34.0f;      // +18 dB sits this far below the top of the plot
    constexpr float dbPerGrid = 6.0f;
    constexpr float freqLabelHeight = 22.0f;
    constexpr float dotStep = 22.0f;      // the background dot grid

    const char* const shapeNames[] = { "Low Cut", "Low Shelf", "High Shelf", "High Cut", "Bell", "Bandpass", "Band Reject", "Tilt" };


    juce::String frequencyLabel (float hz) { return hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k" : juce::String (juce::roundToInt (hz)); }
}

SpectrumView::SpectrumView (Theme& t, PluginProcessor& p) : theme (t), processor (p), apvts (p.apvts)
{
    for (int b = 0; b < ParamIDs::numBands; ++b)
        for (int q = 0; q < ParamIDs::numBandParams; ++q)
            params[(size_t) b][(size_t) q] = apvts.getParameter (ParamIDs::bandParamID (b + 1, (BandParam) q));

    setOpaque (true);
    startTimerHz (30);
}

//==============================================================================
void SpectrumView::setPlotInsets (juce::BorderSize<int> insets)
{
    plotInsets = insets;
    repaint();
}

juce::Rectangle<float> SpectrumView::plotArea() const noexcept { return plotInsets.subtractedFrom (getLocalBounds()).toFloat(); }
float SpectrumView::pxPerDb() const noexcept { return juce::jmax (1.0f, (plotArea().getHeight() * zeroFraction - topRoom) / maxDb); }
float SpectrumView::zeroLineY() const noexcept { return plotArea().getY() + plotArea().getHeight() * zeroFraction; }

float SpectrumView::xForHz (float hz) const noexcept
{
    const auto plot = plotArea();
    return plot.getX() + sidePad + (plot.getWidth() - 2.0f * sidePad) * std::log (juce::jmax (hz, 1.0f) / 20.0f) / std::log (1000.0f);
}

float SpectrumView::hzForX (float x) const noexcept
{
    const auto plot = plotArea();
    return 20.0f * std::pow (1000.0f, (x - plot.getX() - sidePad) / (plot.getWidth() - 2.0f * sidePad));
}

float SpectrumView::yForDb (float db) const noexcept { return zeroLineY() - juce::jlimit (-maxDb, maxDb, db) * pxPerDb(); }
float SpectrumView::dbForY (float y) const noexcept { return juce::jlimit (-maxDb, maxDb, (zeroLineY() - y) / pxPerDb()); }

juce::Rectangle<float> SpectrumView::addButtonArea() const noexcept
{
    const auto plot = plotArea();
    return { plot.getRight() - 16.0f - 32.0f, plot.getBottom() - 16.0f - 32.0f - freqLabelHeight, 32.0f, 32.0f };
}

//==============================================================================
float SpectrumView::getValue (int band, BandParam p) const
{
    auto* param = params[(size_t) band][(size_t) p];
    return param->convertFrom0to1 (param->getValue());
}

void SpectrumView::setValue (int band, BandParam p, float plainValue)
{
    auto* param = params[(size_t) band][(size_t) p];
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

vs::BandSettings SpectrumView::readBand (int band) const
{
    vs::BandSettings s;
    s.enabled = isActive (band) && getValue (band, BandParam::enabled) >= 0.5f;
    s.shape = (vs::BandShape) juce::jlimit (0, vs::numBandShapes - 1, juce::roundToInt (getValue (band, BandParam::shape)));
    s.freqHz = getValue (band, BandParam::freq);
    s.gainDb = getValue (band, BandParam::gain);
    s.q = getValue (band, BandParam::q);
    s.balance = getValue (band, BandParam::balance) / 50.0f;
    return s;
}

vs::BandArray SpectrumView::readAll() const
{
    vs::BandArray all;
    for (int b = 0; b < ParamIDs::numBands; ++b)
        all[(size_t) b] = readBand (b);
    return all;
}

juce::Point<float> SpectrumView::nodePosition (int band) const
{
    const auto s = readBand (band);
    return { xForHz (s.freqHz), yForDb (vs::shapeUsesGain (s.shape) ? s.gainDb : 0.0f) };
}

int SpectrumView::nodeAt (juce::Point<float> p) const
{
    int best = -1;
    auto bestDistance = nodeRadius + 4.0f;
    for (int b = 0; b < ParamIDs::numBands; ++b)
    {
        if (! isActive (b))
            continue;

        const auto d = nodePosition (b).getDistanceFrom (p);
        if (d <= bestDistance)
        {
            best = b;
            bestDistance = d;
        }
    }
    return best;
}

void SpectrumView::setSelectedBand (int band)
{
    select (band);
}

void SpectrumView::select (int band)
{
    if (band == selected)
        return;

    selected = band;
    if (onSelectionChanged)
        onSelectionChanged (selected);
    repaint();
}

int SpectrumView::addBand (float hz, float gainDb)
{
    for (int b = 0; b < ParamIDs::numBands; ++b)
    {
        if (isActive (b))
            continue;

        apvts.undoManager->beginNewTransaction ("Add band");
        setValue (b, BandParam::shape, (float) (int) vs::BandShape::bell);
        setValue (b, BandParam::freq, juce::jlimit (20.0f, 20000.0f, hz));
        setValue (b, BandParam::gain, juce::jlimit (-18.0f, 18.0f, gainDb));
        setValue (b, BandParam::q, 1.0f);
        setValue (b, BandParam::balance, 0.0f);
        setValue (b, BandParam::enabled, 1.0f);
        setValue (b, BandParam::active, 1.0f);
        select (b);
        return b;
    }
    return -1;
}

void SpectrumView::timerCallback()
{
    processor.getReductionDisplay (reduction.data());

    // quick to rise, slower to fall, so short cuts stay visible
    for (size_t i = 0; i < shown.size(); ++i)
        shown[i] = reduction[i] > shown[i] ? reduction[i] : shown[i] + 0.3f * (reduction[i] - shown[i]);

    // a deleted band can't stay selected
    if (selected >= 0 && ! isActive (selected))
    {
        int next = -1;
        for (int b = 0; b < ParamIDs::numBands && next < 0; ++b)
            if (isActive (b))
                next = b;
        select (next);
    }

    // only the plot moves; the panels floating over the rest don't need repainting 30 times a second
    repaint (plotArea().toNearestInt());
}

//==============================================================================
void SpectrumView::resized() {}

void SpectrumView::paintBackground (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto& pal = theme.p();
    const auto w = (float) getWidth();
    const auto h = (float) getHeight();

    // deep vertical gradient with a soft light behind the plot
    g.setGradientFill (juce::ColourGradient (pal.bg, 0.0f, 0.0f, pal.bgDeep, 0.0f, h, false));
    g.fillAll();
    juce::ColourGradient glow (pal.bgGlow.withAlpha (0.9f), plot.getCentreX(), zeroLineY(), pal.bgGlow.withAlpha (0.0f), plot.getCentreX() + plot.getWidth() * 0.55f,
        zeroLineY(), true);
    g.setGradientFill (glow);
    g.fillAll();

    // the dot grid, brighter on the plot's dB lines
    const auto clip = g.getClipBounds().toFloat();
    g.setColour (pal.edge.withAlpha (0.06f));
    for (auto y = std::fmod (zeroLineY(), dotStep); y < h; y += dotStep)
    {
        if (y < clip.getY() - 2.0f || y > clip.getBottom() + 2.0f)
            continue;
        for (auto x = std::fmod (plot.getCentreX(), dotStep); x < w; x += dotStep)
            if (x >= clip.getX() - 2.0f && x <= clip.getRight() + 2.0f)
                g.fillRect (x - 0.75f, y - 0.75f, 1.5f, 1.5f);
    }
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto bypassed = theme.isBypassed();
    const auto plot = plotArea();
    const auto zeroY = zeroLineY();
    const auto ppd = pxPerDb();

    paintBackground (g, plot);

    if (bypassed)
        g.beginTransparencyLayer (0.55f);

    // standard frequencies: hairlines and tiny labels along the bottom of the plot
    const auto labelY = plot.getBottom() - freqLabelHeight;
    const auto labelFont = fonts::make (500, 10.0f, 0.08f);
    for (const int hz : { 50, 100, 200, 500, 1000, 2000, 5000, 10000 })
    {
        const auto x = xForHz ((float) hz);
        const auto major = hz == 100 || hz == 1000 || hz == 10000;
        g.setGradientFill (juce::ColourGradient (pal.edge.withAlpha (0.0f), x, plot.getY() + 20.0f, pal.edge.withAlpha (major ? 0.07f : 0.035f), x, zeroY, false));
        g.fillRect (x - 0.5f, plot.getY() + 20.0f, 1.0f, zeroY - plot.getY() - 20.0f);
        g.setGradientFill (juce::ColourGradient (pal.edge.withAlpha (major ? 0.07f : 0.035f), x, zeroY, pal.edge.withAlpha (0.0f), x, labelY, false));
        g.fillRect (x - 0.5f, zeroY, 1.0f, labelY - zeroY);
    }
    for (const float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
        fonts::draw (g, frequencyLabel (hz), juce::Rectangle<float> (44.0f, freqLabelHeight).withCentre ({ xForHz (hz), labelY + freqLabelHeight * 0.5f }),
            labelFont, pal.inkDim);

    // dB scale on the right edge of the plot
    const auto minus = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
    for (int i = 3; i >= -3; --i)
    {
        if (i == 0)
            continue;
        const auto db = i * (int) dbPerGrid;
        const auto y = zeroY - (float) i * dbPerGrid * ppd;
        const auto text = db > 0 ? "+" + juce::String (db) : minus + juce::String (-db);
        fonts::draw (g, text, juce::Rectangle<float> (plot.getRight() - 40.0f, y - 8.0f, 30.0f, 16.0f), labelFont, pal.inkDim, juce::Justification::centredRight);
        g.setColour (pal.edge.withAlpha (0.12f));
        g.fillRect (plot.getRight() - 6.0f, y - 0.5f, 4.0f, 1.0f);
    }

    // zero line: a hairline that fades out towards the sides
    {
        juce::ColourGradient line (pal.ink.withAlpha (0.0f), plot.getX(), zeroY, pal.ink.withAlpha (0.0f), plot.getRight(), zeroY, false);
        line.addColour (0.15, pal.ink.withAlpha (0.18f));
        line.addColour (0.85, pal.ink.withAlpha (0.18f));
        g.setGradientFill (line);
        g.fillRect (plot.getX(), zeroY - 0.5f, plot.getWidth(), 1.0f);
    }

    // the gain reduction: a gradient wash and a glowing line hanging below the zero line
    {
        juce::Path line;
        float deepest = zeroY;
        line.startNewSubPath (plot.getX() + sidePad, zeroY);
        for (int i = 0; i < (int) shown.size(); ++i)
        {
            const auto x = xForHz (vs::ResonanceSuppressor::displayPointFrequency (i));
            const auto y = juce::jmin (zeroY + shown[(size_t) i] * ppd, labelY - 6.0f);
            deepest = juce::jmax (deepest, y);
            line.lineTo (x, y);
        }
        line.lineTo (plot.getRight() - sidePad, zeroY);

        juce::Path wash (line);
        wash.closeSubPath();
        g.setGradientFill (juce::ColourGradient (pal.accent.withAlpha (0.32f), 0.0f, zeroY, pal.accent2.withAlpha (0.0f), 0.0f,
            juce::jmax (zeroY + 24.0f, deepest), false));
        g.fillPath (wash);

        // with nothing being reduced the line would just sit on the zero line
        if (deepest - zeroY > 0.75f)
            theme.strokeGlow (g, line, pal.accent, 1.8f);
    }

    // the depth curve, a violet dashed line
    {
        const auto bands = readAll();
        juce::Path curve;
        const int steps = juce::jmax (2, (int) plot.getWidth() / 3);
        const auto left = plot.getX() + sidePad;
        const auto span = plot.getWidth() - 2.0f * sidePad;
        for (int i = 0; i <= steps; ++i)
        {
            const auto x = left + (float) i / (float) steps * span;
            const auto weight = vs::DepthCurve::weightAt (bands, hzForX (x));
            const auto y = yForDb (20.0f * std::log10 (juce::jmax (weight, 1.0e-3f)));
            if (i == 0)
                curve.startNewSubPath (x, y);
            else
                curve.lineTo (x, y);
        }

        if (pal.glow > 0.0f)
        {
            g.setColour (pal.accent2.withAlpha (0.1f));
            g.strokePath (curve, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        juce::Path dashed;
        const float dashes[] = { 4.0f, 5.0f };
        juce::PathStrokeType (1.4f).createDashedStroke (dashed, curve, dashes, 2);
        g.setColour (pal.accent2.withAlpha (0.9f));
        g.fillPath (dashed);
    }

    // nodes: rings in the band colours, the selected one drawn last with a halo
    std::vector<int> order;
    for (int b = 0; b < ParamIDs::numBands; ++b)
        if (isActive (b) && b != selected)
            order.push_back (b);
    if (selected >= 0 && isActive (selected))
        order.push_back (selected);

    for (const auto b : order)
    {
        const auto s = readBand (b);
        const auto centre = nodePosition (b);
        const auto emphasised = b == selected || b == hovered;
        const auto radius = nodeRadius * (emphasised ? 0.82f : 0.7f);
        const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        const auto colour = theme.adj (Theme::bandColour (b));

        if (b == selected && pal.glow > 0.0f)
        {
            g.setColour (colour.withAlpha (0.12f));
            g.fillEllipse (circle.expanded (10.0f));
            g.setColour (colour.withAlpha (0.35f));
            g.drawEllipse (circle.expanded (6.0f), 1.0f);
        }

        g.setColour (pal.bgDeep.withAlpha (0.85f));
        g.fillEllipse (circle);
        if (s.enabled)
        {
            g.setColour (colour.withAlpha (0.22f));
            g.fillEllipse (circle);
            g.setColour (colour);
            g.drawEllipse (circle.reduced (1.0f), 2.0f);
            g.fillEllipse (circle.withSizeKeepingCentre (5.0f, 5.0f));
        }
        else
        {
            juce::Path ring;
            ring.addEllipse (circle.reduced (1.0f));
            juce::Path dashedRing;
            const float ringDashes[] = { 3.0f, 3.0f };
            juce::PathStrokeType (1.6f).createDashedStroke (dashedRing, ring, ringDashes, 2);
            g.setColour (colour.withAlpha (0.6f));
            g.fillPath (dashedRing);
        }
    }

    // "+" add band
    {
        const auto area = addButtonArea();
        g.setColour (pal.glass.withAlpha (0.85f));
        g.fillEllipse (area);
        g.setColour (overAdd ? pal.accent.withAlpha (0.7f) : pal.edge.withAlpha (0.14f));
        g.drawEllipse (area.reduced (0.5f), 1.0f);
        icons::stroke (g, icons::plus(), area.withSizeKeepingCentre (12.0f, 12.0f), overAdd ? pal.accent : pal.inkSoft, 1.5f);
    }

    // read-out for the node being dragged or hovered
    const auto info = dragging ? selected : hovered;
    if (info >= 0 && isActive (info))
    {
        const auto s = readBand (info);
        auto text = juce::String (shapeNames[(int) s.shape]).toLowerCase() + "   " + (s.freqHz >= 1000.0f ? juce::String (s.freqHz / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (s.freqHz)) + " Hz");
        if (vs::shapeUsesGain (s.shape))
            text << "   " << (s.gainDb > 0.0f ? "+" : "") << juce::String (s.gainDb, 1) << " dB";

        const auto pos = nodePosition (info);
        auto box = juce::Rectangle<float> (196.0f, 26.0f).withCentre ({ pos.x, pos.y - 36.0f });
        box = box.withX (juce::jlimit (plot.getX() + 4.0f, plot.getRight() - 4.0f - box.getWidth(), box.getX())).withY (juce::jmax (plot.getY() + 4.0f, box.getY()));
        g.setColour (pal.glass.withAlpha (0.92f));
        g.fillRoundedRectangle (box, 13.0f);
        g.setColour (theme.adj (Theme::bandColour (info)).withAlpha (0.6f));
        g.drawRoundedRectangle (box.reduced (0.5f), 13.0f, 1.0f);
        fonts::draw (g, text, box, fonts::make (500, 11.0f, 0.04f), pal.ink);
    }

    if (bypassed)
        g.endTransparencyLayer();
}

//==============================================================================
void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = nodeAt (e.position);
    const auto add = addButtonArea().contains (e.position);
    if (hit != hovered || add != overAdd)
    {
        hovered = hit;
        overAdd = add;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::DraggingHandCursor : (add ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
        repaint();
    }
}

void SpectrumView::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    overAdd = false;
    repaint();
}

void SpectrumView::beginDrag (int band)
{
    dragging = true;
    apvts.undoManager->beginNewTransaction ("Move band");
    for (auto p : { BandParam::freq, BandParam::gain })
        params[(size_t) band][(size_t) p]->beginChangeGesture();
}

void SpectrumView::endDrag()
{
    if (! dragging)
        return;

    dragging = false;
    for (auto p : { BandParam::freq, BandParam::gain })
        params[(size_t) selected][(size_t) p]->endChangeGesture();
    repaint();
}

void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    if (addButtonArea().contains (e.position))
    {
        // the first of a few spread-out frequencies that has no band close to it
        for (const float hz : { 1000.0f, 3000.0f, 300.0f, 6000.0f, 150.0f, 10000.0f, 600.0f, 2000.0f })
        {
            bool free = true;
            for (int b = 0; b < ParamIDs::numBands; ++b)
                free = free && (! isActive (b) || std::abs (std::log2 (getValue (b, BandParam::freq) / hz)) > 0.4f);
            if (free)
            {
                addBand (hz, 0.0f);
                return;
            }
        }
        addBand (1000.0f, 0.0f);
        return;
    }

    const auto hit = nodeAt (e.position);
    select (hit);
    repaint();

    if (hit < 0)
        return;

    if (e.mods.isPopupMenu())
        showMenu (hit);
    else
        beginDrag (hit);
}

void SpectrumView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || selected < 0)
        return;

    setValue (selected, BandParam::freq, juce::jlimit (20.0f, 20000.0f, hzForX (e.position.x)));
    if (vs::shapeUsesGain ((vs::BandShape) juce::roundToInt (getValue (selected, BandParam::shape))))
        setValue (selected, BandParam::gain, dbForY (e.position.y));
}

void SpectrumView::mouseUp (const juce::MouseEvent&)
{
    endDrag();
}

void SpectrumView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (addButtonArea().contains (e.position))
        return;

    const auto hit = nodeAt (e.position);
    if (hit >= 0)
    {
        apvts.undoManager->beginNewTransaction ("Bypass band");
        setValue (hit, BandParam::enabled, getValue (hit, BandParam::enabled) >= 0.5f ? 0.0f : 1.0f);
        return;
    }

    addBand (hzForX (e.position.x), dbForY (e.position.y));
}

void SpectrumView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto hit = nodeAt (e.position);
    if (hit < 0)
        return;

    setValue (hit, BandParam::q, getValue (hit, BandParam::q) * std::exp (wheel.deltaY * 0.8f));
    select (hit);
}

void SpectrumView::showMenu (int band)
{
    juce::PopupMenu menu;
    const auto current = juce::roundToInt (getValue (band, BandParam::shape));
    for (int i = 0; i < vs::numBandShapes; ++i)
        menu.addItem (i + 1, shapeNames[i], true, i == current);

    menu.addSeparator();
    const auto enabled = getValue (band, BandParam::enabled) >= 0.5f;
    menu.addItem (100, enabled ? "Bypass" : "Enable");
    menu.addItem (101, "Delete");

    juce::Component::SafePointer<SpectrumView> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, band] (int result) {
        if (safeThis == nullptr || result == 0)
            return;

        safeThis->apvts.undoManager->beginNewTransaction ("Edit band");
        if (result == 101)
        {
            safeThis->setValue (band, BandParam::active, 0.0f);
        }
        else if (result == 100)
        {
            safeThis->setValue (band, BandParam::enabled, safeThis->getValue (band, BandParam::enabled) >= 0.5f ? 0.0f : 1.0f);
        }
        else
        {
            const auto shape = (vs::BandShape) (result - 1);
            safeThis->setValue (band, BandParam::shape, (float) (int) shape);
            if (shape == vs::BandShape::lowCut || shape == vs::BandShape::highCut)
                safeThis->setValue (band, BandParam::q, 2.0f); // cuts want a steeper slope than a bell's Q
        }
    });
}
} // namespace vox
