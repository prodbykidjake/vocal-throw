#pragma once

#include "../Parameters.h"
#include "../dsp/DepthCurve.h"
#include "Controls.h"

namespace vox
{
/** The glass dock along the bottom that edits the selected band: power dot, frequency, sensitivity,
    Q and balance, and the shape buttons. It re-binds its controls whenever the selection changes. */
class BandEditor : public juce::Component, private juce::Timer
{
public:
    BandEditor (Theme& theme, juce::AudioProcessorValueTreeState& state);
    ~BandEditor() override;

    /** Selects which band is edited (-1 = none). */
    void setBand (int band);
    int getBand() const noexcept { return band; }
    ValueSlider& getFrequency() noexcept { return frequency; }
    ValueSlider& getSensitivity() noexcept { return sensitivity; }

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 1432;
    static constexpr int height = 84;

private:
    class ShapeButton;
    class PowerButton;

    void timerCallback() override;
    void refresh();
    juce::RangedAudioParameter* param (ParamIDs::BandParam which) const;
    void setParam (ParamIDs::BandParam which, float plainValue);

    Theme& theme;
    juce::AudioProcessorValueTreeState& apvts;
    int band = -1;

    ValueSlider frequency, sensitivity, q, balance;
    std::array<std::unique_ptr<ShapeButton>, (size_t) vs::numBandShapes> shapes;
    std::unique_ptr<PowerButton> power;

    bool shownActive = false, shownEnabled = true;
    int shownShape = -1;
};
} // namespace vox
