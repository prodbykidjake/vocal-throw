#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace vox
{
/** The graph that fills the editor: the live gain reduction (a glowing line hanging below the zero line),
    the dashed depth curve and its draggable band nodes, on a log-frequency axis from 20 Hz to 20 kHz and a
    +-18 dB scale. The background covers the whole component; the axes use the plot area, which is the
    component minus the insets set by the editor (the glass panels float over the rest).

    Interaction: drag a node = frequency and depth, wheel over a node = Q, double-click empty space = add a
    bell, double-click a node = bypass it, right-click a node = shape menu / delete, "+" = add a band.
    Every edit goes through the host parameters. */
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    SpectrumView (Theme& theme, PluginProcessor& processor);

    /** Called when the selected band changes (-1 = none). */
    std::function<void (int)> onSelectionChanged;

    int getSelectedBand() const noexcept { return selected; }
    void setSelectedBand (int band);

    /** Keeps the axes clear of the panels floating over the graph. */
    void setPlotInsets (juce::BorderSize<int> insets);
    juce::Rectangle<float> plotArea() const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // axis mapping (also used by the tests)
    float xForHz (float hz) const noexcept;
    float hzForX (float x) const noexcept;
    float yForDb (float db) const noexcept;
    float dbForY (float y) const noexcept;
    float zeroLineY() const noexcept;
    static constexpr float maxDb = 18.0f;

    /** Where a band's node sits, and which node (if any) is under a point. */
    juce::Point<float> nodePosition (int band) const;
    int nodeAt (juce::Point<float> p) const;
    int addBand (float hz, float gainDb);

    static constexpr float nodeRadius = 14.0f;

private:
    using BandParam = ParamIDs::BandParam;

    void timerCallback() override;
    float getValue (int band, BandParam p) const;
    void setValue (int band, BandParam p, float plainValue);
    bool isActive (int band) const { return getValue (band, BandParam::active) >= 0.5f; }
    vs::BandSettings readBand (int band) const;
    vs::BandArray readAll() const;
    void showMenu (int band);
    void beginDrag (int band);
    void endDrag();
    void select (int band);
    juce::Rectangle<float> addButtonArea() const noexcept;
    float pxPerDb() const noexcept;
    void paintBackground (juce::Graphics& g, juce::Rectangle<float> plot);

    Theme& theme;
    PluginProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    std::array<std::array<juce::RangedAudioParameter*, (size_t) ParamIDs::numBandParams>, ParamIDs::numBands> params {};

    std::array<float, vs::ResonanceSuppressor::numDisplayPoints> reduction {}, shown {};
    int selected = -1;
    int hovered = -1;
    bool dragging = false;
    bool overAdd = false;
    juce::BorderSize<int> plotInsets;
};
} // namespace vox
