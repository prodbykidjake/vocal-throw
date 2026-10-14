#pragma once

#include "../Parameters.h"
#include "../dsp/DepthCurve.h"

/** Interactive depth curve drawn over the reduction graph (same frequency axis).

    Draws the sensitivity weighting in dB (+-24 dB, centre = neutral) with up to 8 draggable nodes:
    drag = frequency and depth, wheel = Q/slope, double-click empty space = add a bell, double-click a
    node = bypass it, right-click a node = shape menu / delete. All edits go through the host parameters. */
class DepthCurveOverlay : public juce::Component, private juce::Timer
{
public:
    static constexpr float maxDb = 18.0f;

    explicit DepthCurveOverlay (juce::AudioProcessorValueTreeState& state);

    void paint (juce::Graphics& g) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // coordinate mapping, shared with the graph underneath
    float xForHz (float hz) const noexcept;
    float hzForX (float x) const noexcept;
    float yForDb (float db) const noexcept;
    float dbForY (float y) const noexcept;

    int getSelectedBand() const noexcept { return selected; }

private:
    using BandParam = ParamIDs::BandParam;

    juce::Rectangle<float> plotArea() const noexcept { return getLocalBounds().toFloat().reduced (1.0f); }
    void timerCallback() override { repaint(); }

    float getValue (int band, BandParam p) const;
    void setValue (int band, BandParam p, float plainValue);
    bool isActive (int band) const { return getValue (band, BandParam::active) >= 0.5f; }
    vs::BandSettings readBand (int band) const;
    vs::BandArray readAll() const;
    juce::Point<float> nodePosition (int band) const;
    int nodeAt (juce::Point<float> p) const;
    void showMenu (int band);
    void beginDrag (int band);
    void endDrag();

    juce::AudioProcessorValueTreeState& apvts;
    std::array<std::array<juce::RangedAudioParameter*, (size_t) ParamIDs::numBandParams>, ParamIDs::numBands> params {};
    int selected = -1;
    int hovered = -1;
    bool dragging = false;
};
