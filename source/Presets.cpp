#include "Presets.h"
#include "Parameters.h"
#include "dsp/DepthCurve.h"

namespace
{
using vs::BandShape;

struct BandSpec
{
    int band; // 1..8
    BandShape shape;
    float freq, gainDb, q;
    float balance = 0.0f;
};

struct Preset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values;
    std::vector<BandSpec> bands;
    int quality;
};

const std::vector<Preset>& all()
{
    // Values mirror the parameters' own units (percent, dB, Hz, choice index).
    static const std::vector<Preset> presets = {
        { "Vocal silk",
          { { ParamIDs::mode, 0 }, { ParamIDs::depth, 40 }, { ParamIDs::detail, 58 }, { ParamIDs::selectivity, 46 },
              { ParamIDs::attack, 32 }, { ParamIDs::release, 64 }, { ParamIDs::maxCut, 6 }, { ParamIDs::link, 100 } },
          { { 2, BandShape::bell, 4600.0f, 4.0f, 1.2f } },
          0 },
        { "Gentle master",
          { { ParamIDs::mode, 0 }, { ParamIDs::depth, 22 }, { ParamIDs::detail, 30 }, { ParamIDs::selectivity, 55 },
              { ParamIDs::attack, 70 }, { ParamIDs::release, 75 }, { ParamIDs::maxCut, 3 }, { ParamIDs::stereoMode, 1 },
              { ParamIDs::link, 100 } },
          { { 1, BandShape::lowCut, 60.0f, 0.0f, 1.5f }, { 3, BandShape::highCut, 18000.0f, 0.0f, 2.0f } },
          1 },
        { "Air control",
          { { ParamIDs::mode, 0 }, { ParamIDs::depth, 45 }, { ParamIDs::detail, 70 }, { ParamIDs::selectivity, 40 },
              { ParamIDs::attack, 25 }, { ParamIDs::release, 45 }, { ParamIDs::maxCut, 8 }, { ParamIDs::link, 100 } },
          { { 1, BandShape::lowCut, 3500.0f, 0.0f, 3.0f }, { 2, BandShape::highShelf, 9000.0f, 6.0f, 1.0f },
              { 3, BandShape::highCut, 18000.0f, 0.0f, 2.0f } },
          1 },
        { "Clean dialogue",
          { { ParamIDs::mode, 0 }, { ParamIDs::depth, 55 }, { ParamIDs::detail, 66 }, { ParamIDs::selectivity, 50 },
              { ParamIDs::attack, 22 }, { ParamIDs::release, 40 }, { ParamIDs::maxCut, 10 }, { ParamIDs::link, 100 } },
          { { 1, BandShape::lowCut, 120.0f, 0.0f, 2.0f }, { 2, BandShape::bell, 2500.0f, 6.0f, 0.8f },
              { 4, BandShape::bell, 6500.0f, 4.0f, 1.2f } },
          0 },
    };
    return presets;
}

void setPlain (juce::AudioProcessorValueTreeState& state, const juce::String& id, float value)
{
    if (auto* p = state.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void setDefault (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    if (auto* p = state.getParameter (id))
        p->setValueNotifyingHost (p->getDefaultValue());
}
} // namespace

namespace presets
{
int count() noexcept { return (int) all().size(); }

juce::String name (int index)
{
    return juce::isPositiveAndBelow (index, count()) ? juce::String (all()[(size_t) index].name) : juce::String ("Default");
}

void apply (juce::AudioProcessorValueTreeState& state, int index)
{
    if (! juce::isPositiveAndBelow (index, count()))
        return;

    const auto& preset = all()[(size_t) index];

    // everything sound-shaping starts from its default
    for (const auto* id : { ParamIDs::mode, ParamIDs::depth, ParamIDs::detail, ParamIDs::selectivity, ParamIDs::attack,
             ParamIDs::release, ParamIDs::maxCut, ParamIDs::wetTrim, ParamIDs::stereoMode, ParamIDs::link,
             ParamIDs::stereoBalance, ParamIDs::detailLow, ParamIDs::detailHigh, ParamIDs::attackLow,
             ParamIDs::attackHigh, ParamIDs::releaseLow, ParamIDs::releaseHigh })
        setDefault (state, id);

    for (int band = 1; band <= ParamIDs::numBands; ++band)
        for (int p = 0; p < ParamIDs::numBandParams; ++p)
            setDefault (state, ParamIDs::bandParamID (band, (ParamIDs::BandParam) p));

    for (const auto& [id, value] : preset.values)
        setPlain (state, id, value);

    using ParamIDs::BandParam;
    for (const auto& b : preset.bands)
    {
        setPlain (state, ParamIDs::bandParamID (b.band, BandParam::active), 1.0f);
        setPlain (state, ParamIDs::bandParamID (b.band, BandParam::enabled), 1.0f);
        setPlain (state, ParamIDs::bandParamID (b.band, BandParam::shape), (float) (int) b.shape);
        setPlain (state, ParamIDs::bandParamID (b.band, BandParam::freq), b.freq);
        setPlain (state, ParamIDs::bandParamID (b.band, BandParam::gain), b.gainDb);
        setPlain (state, ParamIDs::bandParamID (b.band, BandParam::q), b.q);
        setPlain (state, ParamIDs::bandParamID (b.band, BandParam::balance), b.balance);
    }

    // the lock keeps whatever quality the user chose
    const auto* lock = state.getRawParameterValue (ParamIDs::qualityLock);
    if (lock == nullptr || lock->load() < 0.5f)
        setPlain (state, ParamIDs::quality, (float) preset.quality);
}
} // namespace presets
