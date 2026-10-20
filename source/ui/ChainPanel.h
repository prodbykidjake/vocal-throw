#pragma once

#include "../dsp/Compressor.h"
#include "../dsp/ParametricEQ.h"
#include "../dsp/Saturator.h"
#include "DirectionPanel.h"
#include "Widgets.h"

namespace chain_ui
{
inline float raw (PluginProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
inline bool on (PluginProcessor& p, const char* id) { return raw (p, id) > 0.5f; }

inline EqParams readEq (PluginProcessor& p)
{
    EqParams e;
    e.hpEnabled = on (p, ParamIDs::eqHpEnable);
    e.hpFreq = raw (p, ParamIDs::eqHpFreq);
    for (size_t i = 0; i < 4; ++i)
        e.bands[i] = { raw (p, ParamIDs::eqFreq[i]), raw (p, ParamIDs::eqGain[i]), raw (p, ParamIDs::eqQ[i]) };
    return e;
}

inline juce::String freqText (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, 1) + " kHz" : juce::String ((int) std::lround (f)) + " Hz"; }
inline juce::String signedText (float v, int d = 1) { return (v < 0 ? Theme::u8 ("−") : juce::String ("+")) + juce::String (std::abs (v), d); }
}

// In / Out gain readout. Drag vertically to change, double-click to reset.
class GainNode : public juce::Component
{
public:
    GainNode (PluginProcessor& p, const char* paramId, const juce::String& title, bool accent)
        : processor (p), label (title), accentValue (accent), param (p.apvts.getParameter (paramId)),
          attachment (*param, [this] (float) { repaint(); }, &p.undoManager)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void setWarning (bool w)
    {
        if (w != warning)
        {
            warning = w;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        Theme::card_ (g, getLocalBounds().toFloat(), juce::Colour (0xff121718), warning ? Theme::coral.withAlpha (0.6f) : juce::Colour (0xff293234), 6.0f);
        g.setColour (juce::Colour (0xff637073));
        g.setFont (Theme::mono (8.5f));
        g.drawText (label, getLocalBounds().removeFromTop (24), juce::Justification::centred, false);
        const auto v = param->convertFrom0to1 (param->getValue());
        g.setColour (warning ? Theme::coral : (accentValue ? Theme::accent : Theme::text));
        g.setFont (Theme::mono (11.5f));
        g.drawText (Theme::dbText (std::abs (v) < 0.05f ? 0.0f : v), getLocalBounds().withTrimmedTop (22).withTrimmedBottom (14), juce::Justification::centred, false);
        g.setColour (juce::Colour (0xff5f6c6f));
        g.setFont (Theme::mono (8.0f));
        g.drawText ("dB", getLocalBounds().removeFromBottom (15), juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        attachment.beginGesture();
        startValue = param->convertFrom0to1 (param->getValue());
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto v = juce::jlimit (Params::gainMinDb, Params::gainMaxDb, startValue - (float) e.getDistanceFromDragStartY() * 0.25f);
        attachment.setValueAsPartOfGesture (v);
    }
    void mouseUp (const juce::MouseEvent&) override { attachment.endGesture(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { attachment.setValueAsCompleteGesture (0.0f); }

private:
    PluginProcessor& processor;
    juce::String label;
    bool accentValue;
    bool warning = false;
    juce::RangedAudioParameter* param;
    juce::ParameterAttachment attachment;
    float startValue = 0.0f;
};

// One chain stage: power toggle, icon, live summary, status. Click the body to select it for the advanced controls.
class ModuleCard : public juce::Component
{
public:
    ModuleCard (PluginProcessor& p, const char* enableId, const juce::String& moduleName, juce::Colour tone, int index,
                std::function<juce::String()> summary, std::function<std::pair<juce::String, bool>()> status, std::function<void (int)> onSelect)
        : processor (p), name (moduleName), colour (tone), moduleIndex (index), summaryFn (std::move (summary)), statusFn (std::move (status)), selectFn (std::move (onSelect)),
          attachment (*p.apvts.getParameter (enableId), [this] (float) { repaint(); }, &p.undoManager), paramId (enableId)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setSelected (bool s)
    {
        selected = s;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const bool enabled = chain_ui::on (processor, paramId);
        auto r = getLocalBounds().toFloat();
        g.setOpacity (1.0f);
        const float a = enabled ? 1.0f : 0.55f;
        Theme::card_ (g, r, juce::Colour (0xff111617), selected ? Theme::accent.withAlpha (0.55f) : (enabled ? juce::Colour (0xff374346) : juce::Colour (0xff2c3537)), 6.0f);

        auto power = juce::Rectangle<float> (r.getX() + 11.0f, r.getCentreY() - 9.0f, 18.0f, 18.0f);
        drawPowerGlyph (g, power.reduced (1.0f), (enabled ? Theme::accent : juce::Colour (0xff596669)).withAlpha (1.0f));
        auto tile = juce::Rectangle<float> (r.getX() + 37.0f, r.getCentreY() - 14.0f, 28.0f, 28.0f);
        g.setColour (colour.withAlpha (0.09f * (enabled ? 1.0f : 0.7f)));
        g.fillRoundedRectangle (tile, 5.0f);
        g.setColour (colour.withAlpha (a));
        drawModuleGlyph (g, tile.reduced (6.0f));

        g.setColour (Theme::text.withAlpha (a));
        g.setFont (Theme::sans (11.5f, true));
        g.drawText (name, (int) r.getX() + 73, (int) r.getY() + 12, getWidth() - 130, 16, juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (0xff626f72).withAlpha (std::min (1.0f, a + 0.2f)));
        g.setFont (Theme::mono (8.5f));
        g.drawText (summaryFn(), (int) r.getX() + 73, (int) r.getY() + 30, getWidth() - 120, 13, juce::Justification::centredLeft, true);

        const auto [text, acting] = statusFn();
        g.setColour (acting ? Theme::accent : juce::Colour (0xff626f72));
        g.setFont (Theme::mono (8.5f));
        g.drawText (text, getWidth() - 50, 12, 40, 14, juce::Justification::centredRight, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        if (e.getPosition().x < 36)
            attachment.setValueAsCompleteGesture (chain_ui::on (processor, paramId) ? 0.0f : 1.0f);
        else
            selectFn (moduleIndex);
    }

private:
    void drawModuleGlyph (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        const auto cx = r.getCentreX(), cy = r.getCentreY(), s = r.getWidth() * 0.5f;
        const juce::PathStrokeType stroke (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        juce::Path p;
        if (moduleIndex == 0) // EQ curve
        {
            p.startNewSubPath (cx - s, cy + s * 0.3f);
            p.cubicTo (cx - s * 0.3f, cy + s * 0.3f, cx - s * 0.4f, cy - s * 0.9f, cx, cy - s * 0.6f);
            p.cubicTo (cx + s * 0.4f, cy - s * 0.3f, cx + s * 0.4f, cy + s * 0.3f, cx + s, cy + s * 0.3f);
            g.strokePath (p, stroke);
        }
        else if (moduleIndex == 1) // compressor: bow-tie
        {
            p.addTriangle (cx - s, cy - s * 0.8f, cx - s, cy + s * 0.8f, cx, cy);
            p.addTriangle (cx + s, cy - s * 0.8f, cx + s, cy + s * 0.8f, cx, cy);
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }
        else // saturation: soft clip curve
        {
            p.startNewSubPath (cx - s, cy + s * 0.8f);
            p.cubicTo (cx - s * 0.2f, cy + s * 0.8f, cx - s * 0.5f, cy - s * 0.6f, cx, cy - s * 0.6f);
            p.cubicTo (cx + s * 0.5f, cy - s * 0.6f, cx + s * 0.2f, cy + s * 0.8f, cx + s, cy + s * 0.8f);
            g.strokePath (p, stroke);
        }
    }

    PluginProcessor& processor;
    juce::String name;
    juce::Colour colour;
    int moduleIndex;
    std::function<juce::String()> summaryFn;
    std::function<std::pair<juce::String, bool>()> statusFn;
    std::function<void (int)> selectFn;
    juce::ParameterAttachment attachment;
    const char* paramId;
    bool selected = false;
};

// EQ response (draggable band points), compressor transfer curve with live gain reduction, saturation curve.
class CurveView : public juce::Component
{
public:
    CurveView (PluginProcessor& p, std::function<void (int)> onBandPicked) : processor (p), bandPicked (std::move (onBandPicked)) {}

    void setModule (int m, int band)
    {
        module = m;
        selectedBand = band;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        Theme::card_ (g, r, juce::Colour (0xff0d1213), juce::Colour (0xff273033), 4.0f);
        g.reduceClipRegion (getLocalBounds().reduced (1));
        if (module == 0)
            paintEq (g, r);
        else if (module == 1)
            paintComp (g, r);
        else
            paintSat (g, r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragBand = -1;
        if (module != 0)
            return;
        const auto eq = chain_ui::readEq (processor);
        float best = 16.0f;
        for (int i = 0; i < 4; ++i)
        {
            const auto d = eqPoint (eq, i).getDistanceFrom (e.position);
            if (d < best)
            {
                best = d;
                dragBand = i;
            }
        }
        if (dragBand >= 0)
        {
            bandPicked (dragBand + 1);
            processor.apvts.getParameter (ParamIDs::eqFreq[dragBand])->beginChangeGesture();
            processor.apvts.getParameter (ParamIDs::eqGain[dragBand])->beginChangeGesture();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragBand < 0)
            return;
        const auto b = (size_t) dragBand;
        auto* fp = processor.apvts.getParameter (ParamIDs::eqFreq[b]);
        auto* gp = processor.apvts.getParameter (ParamIDs::eqGain[b]);
        const auto f = xToFreq (e.position.x);
        fp->setValueNotifyingHost (fp->convertTo0to1 (f));
        gp->setValueNotifyingHost (gp->convertTo0to1 (yToDb (e.position.y)));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragBand >= 0)
        {
            processor.apvts.getParameter (ParamIDs::eqFreq[(size_t) dragBand])->endChangeGesture();
            processor.apvts.getParameter (ParamIDs::eqGain[(size_t) dragBand])->endChangeGesture();
        }
        dragBand = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // double-click a point to flatten that band
        if (module != 0)
            return;
        const auto eq = chain_ui::readEq (processor);
        for (int i = 0; i < 4; ++i)
            if (eqPoint (eq, i).getDistanceFrom (e.position) < 16.0f)
            {
                auto* gp = processor.apvts.getParameter (ParamIDs::eqGain[(size_t) i]);
                gp->beginChangeGesture();
                gp->setValueNotifyingHost (gp->convertTo0to1 (0.0f));
                gp->endChangeGesture();
            }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (module != 0 || selectedBand < 1)
            return;
        auto* qp = processor.apvts.getParameter (ParamIDs::eqQ[(size_t) selectedBand - 1]);
        qp->beginChangeGesture();
        qp->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, qp->getValue() + w.deltaY * 0.15f));
        qp->endChangeGesture();
        juce::ignoreUnused (e);
    }

private:
    static constexpr float minHz = 20.0f, maxHz = 20000.0f, rangeDb = 15.0f;

    float freqToX (float f) const { return 6.0f + (getWidth() - 12.0f) * std::log (f / minHz) / std::log (maxHz / minHz); }
    float xToFreq (float x) const { return minHz * std::exp (std::log (maxHz / minHz) * juce::jlimit (0.0f, 1.0f, (x - 6.0f) / (getWidth() - 12.0f))); }
    float dbToY (float db) const { return getHeight() * 0.5f - db / rangeDb * (getHeight() * 0.5f - 8.0f); }
    float yToDb (float y) const { return juce::jlimit (-12.0f, 12.0f, -(y - getHeight() * 0.5f) / (getHeight() * 0.5f - 8.0f) * rangeDb); }
    juce::Point<float> eqPoint (const EqParams& e, int i) const { return { freqToX (e.bands[(size_t) i].freq), dbToY (e.bands[(size_t) i].gainDb) }; }

    void paintEq (juce::Graphics& g, juce::Rectangle<float>)
    {
        const double fs = processor.getSampleRate() > 0 ? processor.getSampleRate() : 48000.0;
        g.setColour (juce::Colours::white.withAlpha (0.035f));
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            g.drawVerticalLine ((int) freqToX (f), 2.0f, (float) getHeight() - 2.0f);
        for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
        {
            g.setColour (juce::Colours::white.withAlpha (juce::exactlyEqual (db, 0.0f) ? 0.08f : 0.035f));
            g.drawHorizontalLine ((int) dbToY (db), 2.0f, (float) getWidth() - 2.0f);
        }
        g.setColour (juce::Colour (0xff4a5659));
        g.setFont (Theme::mono (8.0f));
        for (auto [f, t] : { std::pair { 100.0f, "100" }, { 1000.0f, "1k" }, { 10000.0f, "10k" } })
            g.drawText (t, (int) freqToX (f) + 3, getHeight() - 12, 24, 10, juce::Justification::centredLeft, false);

        const auto eq = chain_ui::readEq (processor);
        const bool enabled = chain_ui::on (processor, ParamIDs::eqEnable);
        juce::Path curve;
        const int steps = std::max (8, getWidth() / 3);
        for (int i = 0; i <= steps; ++i)
        {
            const auto f = xToFreq (6.0f + (getWidth() - 12.0f) * (float) i / (float) steps);
            const auto y = dbToY (juce::jlimit (-rangeDb, rangeDb, (float) ParametricEQ::responseDb (eq, f, fs)));
            if (i == 0) curve.startNewSubPath (freqToX (f), y); else curve.lineTo (freqToX (f), y);
        }
        const auto tone = Theme::accent.withAlpha (enabled ? 1.0f : 0.35f);
        juce::Path fill (curve);
        fill.lineTo (freqToX (maxHz), dbToY (0.0f));
        fill.lineTo (freqToX (minHz), dbToY (0.0f));
        fill.closeSubPath();
        g.setColour (tone.withAlpha (0.07f));
        g.fillPath (fill);
        g.setColour (tone);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));

        for (int i = 0; i < 4; ++i)
        {
            const auto pt = eqPoint (eq, i);
            const bool sel = selectedBand == i + 1;
            const bool flat = std::abs (eq.bands[(size_t) i].gainDb) < 0.05f;
            const auto rad = sel ? 5.5f : 4.0f;
            if (sel)
            {
                g.setColour (tone.withAlpha (0.35f));
                g.fillEllipse (pt.x - 9.0f, pt.y - 9.0f, 18.0f, 18.0f);
            }
            g.setColour (juce::Colour (0xff0f1516));
            g.fillEllipse (pt.x - rad - 1.5f, pt.y - rad - 1.5f, 2 * rad + 3.0f, 2 * rad + 3.0f);
            g.setColour (flat ? tone.withAlpha (0.45f) : tone);
            g.fillEllipse (pt.x - rad, pt.y - rad, 2 * rad, 2 * rad);
        }
    }

    void paintComp (juce::Graphics& g, juce::Rectangle<float>)
    {
        const float thr = chain_ui::raw (processor, ParamIDs::compThreshold), ratio = chain_ui::raw (processor, ParamIDs::compRatio),
                    knee = chain_ui::raw (processor, ParamIDs::compKnee), makeup = chain_ui::raw (processor, ParamIDs::compMakeup);
        const bool enabled = chain_ui::on (processor, ParamIDs::compEnable);
        const auto plot = getLocalBounds().toFloat().reduced (14.0f, 10.0f).withTrimmedRight (30.0f);
        auto map = [&] (float inDb, float outDb) {
            return juce::Point<float> (plot.getX() + plot.getWidth() * (inDb + 60.0f) / 60.0f, plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (outDb + 60.0f) / 60.0f));
        };
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        for (float v : { -48.0f, -36.0f, -24.0f, -12.0f })
        {
            g.drawVerticalLine ((int) map (v, 0).x, plot.getY(), plot.getBottom());
            g.drawHorizontalLine ((int) map (0, v).y, plot.getX(), plot.getRight());
        }
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawLine (map (-60, -60).x, map (-60, -60).y, map (0, 0).x, map (0, 0).y, 1.0f);

        juce::Path curve;
        for (int i = 0; i <= 120; ++i)
        {
            const auto in = -60.0f + 60.0f * (float) i / 120.0f;
            const auto out = in - Compressor::reductionDb (in, thr, ratio, knee) + makeup;
            const auto pt = map (in, out);
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        const auto tone = Theme::purple.withAlpha (enabled ? 1.0f : 0.35f);
        g.setColour (tone);
        g.strokePath (curve, juce::PathStrokeType (2.0f));
        g.setColour (tone.withAlpha (0.5f));
        g.drawVerticalLine ((int) map (thr, 0).x, plot.getY(), plot.getBottom());

        // live gain reduction bar
        const auto gr = enabled ? processor.getGainReductionDb() : 0.0f;
        auto bar = juce::Rectangle<float> ((float) getWidth() - 22.0f, plot.getY(), 8.0f, plot.getHeight());
        g.setColour (juce::Colour (0xff1d2527));
        g.fillRoundedRectangle (bar, 2.0f);
        const auto h = bar.getHeight() * juce::jlimit (0.0f, 1.0f, gr / 18.0f);
        g.setColour (Theme::purple);
        g.fillRoundedRectangle (bar.withHeight (h), 2.0f);
        g.setColour (juce::Colour (0xff6b777a));
        g.setFont (Theme::mono (8.0f));
        g.drawText (juce::String (gr, 1) + " dB", (int) bar.getX() - 22, getHeight() - 14, 50, 11, juce::Justification::centred, false);
    }

    void paintSat (juce::Graphics& g, juce::Rectangle<float>)
    {
        const float driveDb = chain_ui::raw (processor, ParamIDs::satDrive), mix = chain_ui::raw (processor, ParamIDs::satMix) * 0.01f;
        const bool enabled = chain_ui::on (processor, ParamIDs::satEnable);
        const auto plot = getLocalBounds().toFloat().reduced (14.0f, 10.0f);
        const auto k = juce::Decibels::decibelsToGain (driveDb);
        const auto c = Saturator::compensation (k);
        auto map = [&] (float x, float y) { return juce::Point<float> (plot.getCentreX() + x * plot.getWidth() * 0.5f, plot.getCentreY() - y * plot.getHeight() * 0.5f); };
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
        g.drawVerticalLine ((int) plot.getCentreX(), plot.getY(), plot.getBottom());
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawLine (map (-1, -1).x, map (-1, -1).y, map (1, 1).x, map (1, 1).y, 1.0f);
        juce::Path curve;
        for (int i = 0; i <= 160; ++i)
        {
            const auto x = -1.0f + 2.0f * (float) i / 160.0f;
            const auto y = juce::jlimit (-1.0f, 1.0f, x + mix * (c * std::tanh (k * x) - x));
            const auto pt = map (x, y);
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        g.setColour (Theme::orange.withAlpha (enabled ? 1.0f : 0.35f));
        g.strokePath (curve, juce::PathStrokeType (2.0f));
    }

    PluginProcessor& processor;
    std::function<void (int)> bandPicked;
    int module = 0, selectedBand = 1, dragBand = -1;
};

class ChainPanel : public juce::Component
{
public:
    static constexpr int closedHeight = 134;
    static constexpr int advancedHeight = 144;

    ChainPanel (PluginProcessor& p, juce::ValueTree stateTree, std::function<void()> onLayoutChange)
        : processor (p), state (std::move (stateTree)), layoutChanged (std::move (onLayoutChange)),
          inNode (p, ParamIDs::inputGain, "IN", false), outNode (p, ParamIDs::outputGain, "OUT", true),
          curve (p, [this] (int band) { selectBand (band); })
    {
        using namespace chain_ui;
        auto select = [this] (int m) { selectModule (m); };
        cards.push_back (std::make_unique<ModuleCard> (
            p, ParamIDs::eqEnable, "EQ", Theme::accent, 0,
            [this] { return eqSummary(); },
            [this] { const auto flat = eqFlat(); return std::pair<juce::String, bool> { ! on (processor, ParamIDs::eqEnable) ? "OFF" : (flat ? "FLAT" : "ON"), on (processor, ParamIDs::eqEnable) && ! flat }; }, select));
        cards.push_back (std::make_unique<ModuleCard> (
            p, ParamIDs::compEnable, "Compressor", Theme::purple, 1,
            [this] { return compSummary(); },
            [this] { const bool e = on (processor, ParamIDs::compEnable); return std::pair<juce::String, bool> { e ? "ON" : "OFF", e && processor.getGainReductionDb() > 0.1f }; }, select));
        cards.push_back (std::make_unique<ModuleCard> (
            p, ParamIDs::satEnable, "Saturation", Theme::orange, 2,
            [this] { return satSummary(); },
            [this] { const bool e = on (processor, ParamIDs::satEnable); return std::pair<juce::String, bool> { e ? "ON" : "OFF", e && raw (processor, ParamIDs::satMix) > 0.5f }; }, select));
        for (auto& c : cards)
            addAndMakeVisible (*c);
        addAndMakeVisible (inNode);
        addAndMakeVisible (outNode);
        addChildComponent (curve);
        for (auto& k : knobs)
            addChildComponent (k);
        for (auto& c : chips)
        {
            c.setStyle (DesignButton::Style::segment);
            c.setTextFont (Theme::mono (8.5f));
            c.setClickingTogglesState (false);
            addChildComponent (c);
        }
        const char* chipNames[5] = { "HP", "1 LOW", "2 BELL", "3 BELL", "4 HIGH" };
        for (int i = 0; i < 5; ++i)
        {
            chips[(size_t) i].setButtonText (chipNames[i]);
            chips[(size_t) i].onClick = [this, i] { selectBand (i); };
        }
        hpToggle.setTextFont (Theme::mono (8.5f));
        hpToggle.onClick = [this] {
            auto* prm = processor.apvts.getParameter (ParamIDs::eqHpEnable);
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->getValue() > 0.5f ? 0.0f : 1.0f);
            prm->endChangeGesture();
        };
        addChildComponent (hpToggle);

        toggleAdvanced.setStyle (DesignButton::Style::ghost);
        toggleAdvanced.setTextFont (Theme::sans (10.0f));
        toggleAdvanced.iconOnRight = true;
        toggleAdvanced.icon = [this] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { drawChevron (g, r, c, advancedOpen()); };
        toggleAdvanced.onClick = [this] {
            state.setProperty ("advancedOpen", ! advancedOpen(), nullptr);
            refreshLayout();
            layoutChanged();
        };
        addAndMakeVisible (toggleAdvanced);
        refreshLayout();
    }

    // Output true-peak (dBTP) held by the editor; shown in the summary and flags the OUT node when it would clip.
    void setOutputPeak (float db)
    {
        outputPeakDb = db;
        outNode.setWarning (db > -0.1f);
    }

    bool advancedOpen() const { return (bool) state.getProperty ("advancedOpen", false); }
    int preferredHeight() const { return closedHeight + (advancedOpen() ? advancedHeight : 0); }

    void setAdvancedOpen (bool open)
    {
        state.setProperty ("advancedOpen", open, nullptr);
        refreshLayout();
    }

    void refreshLayout()
    {
        const bool open = advancedOpen();
        toggleAdvanced.setButtonText (open ? "Hide advanced controls" : "Show advanced controls");
        curve.setVisible (open);
        for (auto& k : knobs)
            k.setVisible (false);
        for (auto& c : chips)
            c.setVisible (false);
        hpToggle.setVisible (false);
        if (open)
            rebindKnobs();
        resized();
        repaint();
    }

    void refreshLive()
    {
        for (auto& c : cards)
            c->repaint();
        inNode.repaint();
        outNode.repaint();
        if (curve.isVisible())
            curve.repaint();
        for (size_t i = 0; i < chips.size(); ++i)
            chips[i].setToggleState (selectedBand() == (int) i, juce::dontSendNotification);
        if (advancedOpen() && selectedModule() == 0)
            hpToggle.setButtonText (chain_ui::on (processor, ParamIDs::eqHpEnable) ? "HP ON" : "HP OFF");
        repaint (0, 0, getWidth(), 50);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::chainPanel);
        Theme::eyebrow (g, "PROCESSING CHAIN", { 24, 18, 170, 12 });
        int active = 0;
        for (auto id : { ParamIDs::eqEnable, ParamIDs::compEnable, ParamIDs::satEnable })
            active += chain_ui::on (processor, id) ? 1 : 0;
        const bool bypassed = chain_ui::on (processor, ParamIDs::bypass);
        const auto off = processor.getLoudnessOffsetDb();
        juce::String summary = juce::String (active) + (active == 1 ? " module active" : " modules active") + Theme::u8 (" · ")
                               + (bypassed ? juce::String ("Globally bypassed")
                                           : "output " + chain_ui::signedText (off) + " dB vs input (K-weighted)")
                               + (outputPeakDb > -90.0f ? Theme::u8 (" · ") + "output peak " + Theme::dbText (outputPeakDb) + " dBTP" + (outputPeakDb > -0.1f ? juce::String (" (over 0 dBFS)") : juce::String()) : juce::String());
        g.setColour (juce::Colour (0xff586467));
        g.setFont (Theme::sans (10.0f));
        g.drawText (summary, 206, 17, 800, 14, juce::Justification::centredLeft, false);

        // connectors
        const auto row = chainRow();
        auto line = [&] (int x0, int x1, bool activeLine) {
            const auto y = (float) row.getCentreY();
            if (activeLine)
            {
                juce::ColourGradient grad (juce::Colour (0xff344042), (float) x0, y, juce::Colour (0xff344042), (float) x1, y, false);
                grad.addColour (0.5, juce::Colour (0xff54c8aa));
                g.setGradientFill (grad);
            }
            else
                g.setColour (juce::Colour (0xff2d3739));
            g.fillRect ((float) x0, y, (float) (x1 - x0), 1.0f);
        };
        line (inNode.getRight(), cards[0]->getX(), true);
        for (size_t i = 0; i < 2; ++i)
            line (cards[i]->getRight(), cards[i + 1]->getX(), chain_ui::on (processor, i == 0 ? ParamIDs::eqEnable : ParamIDs::compEnable));
        line (cards[2]->getRight(), outNode.getX(), true);

        if (advancedOpen())
        {
            auto box = advancedBox().toFloat();
            Theme::card_ (g, box, juce::Colour (0xff111718), juce::Colour (0xff293234), 7.0f);
            g.setColour (juce::Colour (0xff687477));
            g.setFont (Theme::mono (8.5f, 0.1f));
            const char* names[3] = { "EQUALIZER", "COMPRESSOR", "SATURATION" };
            g.drawText (names[selectedModule()], advancedBox().getX() + 18, advancedBox().getY() + 7, 200, 12, juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        toggleAdvanced.setBounds (getWidth() - 24 - 160, 12, 160, 22);
        const auto row = chainRow();
        const int nodeW = 62, cardW = 230;
        const int free = row.getWidth() - 2 * nodeW - 3 * cardW;
        const int conn = free / 4;
        int x = row.getX();
        inNode.setBounds (x, row.getY(), nodeW, row.getHeight());
        x += nodeW + conn;
        for (auto& c : cards)
        {
            c->setBounds (x, row.getY(), cardW, row.getHeight());
            x += cardW + conn;
        }
        outNode.setBounds (row.getRight() - nodeW, row.getY(), nodeW, row.getHeight());

        if (advancedOpen())
        {
            const auto box = advancedBox();
            const int top = box.getY() + 26;
            int kx = box.getX() + 14;
            for (size_t i = 0; i < knobs.size(); ++i)
                knobs[i].setBounds (kx + (int) i * 72, top + (selectedModule() == 0 ? 16 : 4), 68, 84);
            // EQ band chips
            int cx = box.getX() + 14;
            for (size_t i = 0; i < chips.size(); ++i)
            {
                chips[i].setBounds (cx, box.getY() + 22, 58, 20);
                cx += 62;
            }
            hpToggle.setBounds (cx + 8, box.getY() + 22, 56, 20);
            const int curveX = box.getX() + 14 + 7 * 72 + 14;
            curve.setBounds (curveX, box.getY() + 12, box.getRight() - 14 - curveX, box.getHeight() - 24);
        }
    }

private:
    juce::Rectangle<int> chainRow() const { return { 24, 57, getWidth() - 48, 57 }; }
    juce::Rectangle<int> advancedBox() const { return { 24, 128, getWidth() - 48, advancedHeight - 14 }; }

    int selectedModule() const { return juce::jlimit (0, 2, (int) state.getProperty ("selectedModule", 0)); }
    int selectedBand() const { return juce::jlimit (0, 4, (int) state.getProperty ("eqBand", 1)); }

    void selectModule (int m)
    {
        state.setProperty ("selectedModule", m, nullptr);
        if (! advancedOpen())
            state.setProperty ("advancedOpen", true, nullptr);
        refreshLayout();
        layoutChanged();
    }

    void selectBand (int band)
    {
        state.setProperty ("selectedModule", 0, nullptr);
        state.setProperty ("eqBand", band, nullptr);
        refreshLayout();
    }

    void rebindKnobs()
    {
        for (auto& c : cards)
            c->setSelected (false);
        cards[(size_t) selectedModule()]->setSelected (true);
        for (auto& k : knobs)
            k.unbind();
        auto& a = processor.apvts;
        const int m = selectedModule();
        if (m == 0)
        {
            const int band = selectedBand();
            if (band == 0)
                knobs[0].bind (a, ParamIDs::eqHpFreq, "FREQUENCY", "Hz", 0);
            else
            {
                knobs[0].bind (a, ParamIDs::eqFreq[band - 1], "FREQUENCY", "Hz", 0);
                knobs[1].bind (a, ParamIDs::eqGain[band - 1], "GAIN", "dB", 1);
                knobs[2].bind (a, ParamIDs::eqQ[band - 1], "Q", "", 2);
            }
            for (auto& c : chips)
                c.setVisible (true);
            hpToggle.setVisible (band == 0);
            curve.setModule (0, band);
        }
        else if (m == 1)
        {
            knobs[0].bind (a, ParamIDs::compThreshold, "THRESHOLD", "dB", 1);
            knobs[1].bind (a, ParamIDs::compRatio, "RATIO", ":1", 1);
            knobs[2].bind (a, ParamIDs::compAttack, "ATTACK", "ms", 1);
            knobs[3].bind (a, ParamIDs::compRelease, "RELEASE", "ms", 0);
            knobs[4].bind (a, ParamIDs::compKnee, "KNEE", "dB", 1);
            knobs[5].bind (a, ParamIDs::compMakeup, "MAKEUP", "dB", 1);
            knobs[6].bind (a, ParamIDs::compMix, "MIX", "%", 0);
            curve.setModule (1, 0);
        }
        else
        {
            knobs[0].bind (a, ParamIDs::satDrive, "DRIVE", "dB", 1);
            knobs[1].bind (a, ParamIDs::satMix, "MIX", "%", 0);
            curve.setModule (2, 0);
        }
        resized();
    }

    bool eqFlat() const
    {
        const auto e = chain_ui::readEq (processor);
        if (e.hpEnabled)
            return false;
        for (const auto& b : e.bands)
            if (std::abs (b.gainDb) > 0.05f)
                return false;
        return true;
    }

    juce::String eqSummary() const
    {
        const auto e = chain_ui::readEq (processor);
        int count = 0, loudest = -1;
        for (int i = 0; i < 4; ++i)
            if (std::abs (e.bands[(size_t) i].gainDb) > 0.05f)
            {
                ++count;
                if (loudest < 0 || std::abs (e.bands[(size_t) i].gainDb) > std::abs (e.bands[(size_t) loudest].gainDb))
                    loudest = i;
            }
        juce::String text;
        if (loudest >= 0)
            text = chain_ui::signedText (e.bands[(size_t) loudest].gainDb) + " dB " + Theme::u8 ("·") + " " + chain_ui::freqText (e.bands[(size_t) loudest].freq) + (count > 1 ? " +" + juce::String (count - 1) : "");
        if (e.hpEnabled)
            text += (text.isEmpty() ? "" : " ") + juce::String ("HP ") + chain_ui::freqText (e.hpFreq);
        return text.isEmpty() ? "Flat" : text;
    }

    juce::String compSummary() const
    {
        const auto gr = processor.getGainReductionDb();
        return juce::String (chain_ui::raw (processor, ParamIDs::compRatio), 1) + ":1 " + Theme::u8 ("·") + " " + juce::String (chain_ui::raw (processor, ParamIDs::compAttack), 0) + " ms"
               + (chain_ui::on (processor, ParamIDs::compEnable) ? " " + Theme::u8 ("·") + " " + juce::String (gr, 1) + " dB GR" : juce::String());
    }

    juce::String satSummary() const
    {
        return "Soft " + Theme::u8 ("·") + " " + juce::String (chain_ui::raw (processor, ParamIDs::satDrive), 0) + " dB drive " + Theme::u8 ("·") + " " + juce::String (chain_ui::raw (processor, ParamIDs::satMix), 0) + "%";
    }

    PluginProcessor& processor;
    juce::ValueTree state;
    std::function<void()> layoutChanged;
    GainNode inNode, outNode;
    float outputPeakDb = -100.0f;
    std::vector<std::unique_ptr<ModuleCard>> cards;
    CurveView curve;
    std::array<Knob, 7> knobs;
    std::array<DesignButton, 5> chips;
    DesignButton hpToggle { "HP OFF", DesignButton::Style::outline }, toggleAdvanced { "Show advanced controls", DesignButton::Style::ghost };
};
