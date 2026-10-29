#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float echoBeats[] = { 0.25f, 0.5f, 0.75f, 1.0f };
    constexpr float toneOpenHz = 20000.0f;
    constexpr float toneDarkHz = 1200.0f;
    constexpr int chunkSize = 512;

    // Throw opens the echo and reverb sends this far and lets the echo ring this long
    constexpr float throwEchoSend = 0.85f;
    constexpr float throwSpaceSend = 0.7f;
    constexpr float throwFeedback = 0.68f;
    constexpr float reverbLevel = 0.4f;

    // ducking: the dry vocal's level between these maps to no ducking ... full Duck depth
    constexpr float duckFloorDb = -50.0f;
    constexpr float duckRangeDb = 26.0f;

    struct Preset
    {
        const char* name;
        float stack, spread, octave, tone, dirt, echo, echoTime, space, duck, harmony, interval;
    };

    // echoTime is an index: 0 = 1/16, 1 = 1/8, 2 = 1/8 dotted, 3 = 1/4
    // interval is an index: 0 = 3rd up, 1 = 5th up, 2 = 3rd + 5th, 3 = 3rd down
    // presets leave Key and Scale alone, since those belong to the song
    constexpr Preset presets[] = {
        { "Default",        3.0f, 0.50f, 0.00f, 1.00f, 0.00f, 0.20f, 1.0f, 0.20f, 0.00f, 0.00f, 0.0f },
        { "Double",         2.0f, 0.30f, 0.00f, 1.00f, 0.00f, 0.10f, 1.0f, 0.15f, 0.00f, 0.00f, 0.0f },
        { "Wide Stack",     6.0f, 1.00f, 0.00f, 1.00f, 0.10f, 0.25f, 1.0f, 0.30f, 0.20f, 0.00f, 0.0f },
        { "Octave Whisper", 3.0f, 0.60f, 0.70f, 0.55f, 0.00f, 0.30f, 2.0f, 0.45f, 0.30f, 0.00f, 0.0f },
        { "Echo Out",       3.0f, 0.50f, 0.00f, 0.80f, 0.00f, 0.65f, 2.0f, 0.25f, 0.60f, 0.00f, 0.0f },
        { "Dirty Hype",     4.0f, 0.70f, 0.30f, 1.00f, 0.60f, 0.20f, 0.0f, 0.15f, 0.00f, 0.00f, 0.0f },
        { "Harmony 3rds",   2.0f, 0.40f, 0.00f, 1.00f, 0.00f, 0.15f, 1.0f, 0.25f, 0.30f, 0.70f, 0.0f },
        { "Choir Hype",     4.0f, 0.70f, 0.20f, 0.90f, 0.00f, 0.20f, 2.0f, 0.35f, 0.40f, 0.60f, 2.0f },
        { "Throw Ready",    3.0f, 0.50f, 0.00f, 1.00f, 0.00f, 0.15f, 2.0f, 0.20f, 0.70f, 0.00f, 0.0f },
    };
    constexpr int numPresets = static_cast<int> (sizeof (presets) / sizeof (presets[0]));
}

//==============================================================================
PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createLayout())
{
    // look the parameters up once, so the audio thread never searches by name
    const auto find = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    raw = { find (ParamIDs::stack), find (ParamIDs::spread), find (ParamIDs::octave), find (ParamIDs::tone),
            find (ParamIDs::dirt), find (ParamIDs::echo), find (ParamIDs::echoTime), find (ParamIDs::space),
            find (ParamIDs::gain), find (ParamIDs::active), find (ParamIDs::duck), find (ParamIDs::throwOn),
            find (ParamIDs::harmony), find (ParamIDs::interval), find (ParamIDs::key), find (ParamIDs::scale) };

    for (auto& peak : scopePeaks)
        peak.store (0.0f);
}

PluginProcessor::~PluginProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::createLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterChoice;
    using juce::AudioParameterFloat;
    using juce::AudioParameterInt;
    using juce::NormalisableRange;
    using juce::ParameterID;

    const auto percent = [] (float value, int) { return juce::String (juce::roundToInt (value * 100.0f)) + "%"; };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::stack, 1 }, "Voices", 1, StackEngine::maxStack, 3));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::spread, 1 }, "Spread", NormalisableRange<float> (0.0f, 1.0f), 0.5f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::octave, 1 }, "High Hype", NormalisableRange<float> (0.0f, 1.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::tone, 1 }, "Tone", NormalisableRange<float> (0.0f, 1.0f), 1.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::dirt, 1 }, "Dirt", NormalisableRange<float> (0.0f, 1.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::echo, 1 }, "Echo", NormalisableRange<float> (0.0f, 1.0f), 0.2f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::echoTime, 1 }, "Echo Time", juce::StringArray { "1/16", "1/8", "1/8 dotted", "1/4" }, 1));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::space, 1 }, "Reverb", NormalisableRange<float> (0.0f, 1.0f), 0.2f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::gain, 1 }, "Output", NormalisableRange<float> (-24.0f, 6.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float value, int) { return juce::String (value, 1) + " dB"; })));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::active, 1 }, "Stack On", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::duck, 1 }, "Duck", NormalisableRange<float> (0.0f, 1.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::throwOn, 1 }, "Throw", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::harmony, 1 }, "Harmony", NormalisableRange<float> (0.0f, 1.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::interval, 1 }, "Interval", juce::StringArray { "3rd Up", "5th Up", "3rd + 5th", "3rd Down" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::key, 1 }, "Key", juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::scale, 1 }, "Scale", juce::StringArray { "Major", "Minor" }, 0));

    return layout;
}

//==============================================================================
const juce::String PluginProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PluginProcessor::acceptsMidi() const
{
    return false;
}

bool PluginProcessor::producesMidi() const
{
    return false;
}

bool PluginProcessor::isMidiEffect() const
{
    return false;
}

double PluginProcessor::getTailLengthSeconds() const
{
    // the echo repeats and the reverb ring on after the vocal stops (longest after a Throw)
    return 8.0;
}

int PluginProcessor::getNumPrograms()
{
    return numPresets;
}

int PluginProcessor::getCurrentProgram()
{
    return currentProgram;
}

void PluginProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= numPresets)
        return;

    currentProgram = index;
    const auto& preset = presets[index];

    const auto set = [this] (const char* id, float value)
    {
        if (auto* parameter = apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };

    set (ParamIDs::stack, preset.stack);
    set (ParamIDs::spread, preset.spread);
    set (ParamIDs::octave, preset.octave);
    set (ParamIDs::tone, preset.tone);
    set (ParamIDs::dirt, preset.dirt);
    set (ParamIDs::echo, preset.echo);
    set (ParamIDs::echoTime, preset.echoTime);
    set (ParamIDs::space, preset.space);
    set (ParamIDs::duck, preset.duck);
    set (ParamIDs::harmony, preset.harmony);
    set (ParamIDs::interval, preset.interval);
}

const juce::String PluginProcessor::getProgramName (int index)
{
    // Steinberg's VST3 validator fails plugins whose programs have no name
    if (index < 0 || index >= numPresets)
        return "Default";

    return presets[index].name;
}

void PluginProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    currentSampleRate = sampleRate;

    engine.prepare (sampleRate);
    engine.setParams (readStackParams());
    echo.prepare (sampleRate);
    pitchTracker.prepare (sampleRate);
    harmonyPrimed = false;
    toneL.reset();
    toneR.reset();

    // parameters first, so the reverb's internal gains start where they should rather than ramping up
    juce::Reverb::Parameters reverbParams;
    reverbParams.wetLevel = reverbLevel;
    reverbParams.dryLevel = 0.0f;
    reverb.setParameters (reverbParams);
    reverb.setSampleRate (sampleRate);
    reverb.reset();

    for (auto* scratch : { &reverbL, &reverbR, &wetL, &wetR, &mono, &duckGains })
        scratch->assign (static_cast<size_t> (chunkSize), 0.0f);

    const auto smooth = [sampleRate] (juce::SmoothedValue<float>& value, float start)
    {
        value.reset (sampleRate, 0.03);
        value.setCurrentAndTargetValue (start);
    };

    smooth (echoMix, raw.echo->load());
    smooth (spaceMix, raw.space->load());
    smooth (dirtAmount, raw.dirt->load());
    smooth (outputGain, juce::Decibels::decibelsToGain (raw.gain->load()));
    smooth (activeMix, raw.active->load() >= 0.5f ? 1.0f : 0.0f);
    smooth (duckDepth, raw.duck->load());

    const auto throwing = raw.throwOn->load() >= 0.5f ? 1.0f : 0.0f;
    throwSend.reset (sampleRate, 0.01);
    throwSend.setCurrentAndTargetValue (throwing);
    throwTail = throwing;

    const auto coefficient = [sampleRate] (double seconds) { return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sampleRate))); };
    throwTailAttack = coefficient (0.01);
    throwTailRelease = coefficient (1.5);
    duckAttack = coefficient (0.003);
    duckRelease = coefficient (0.045);
    duckEnvelope = 0.0f;
    duckHoldSamples = static_cast<int> (0.12 * sampleRate);
    duckHold = 0;

    scopeStride = juce::jmax (1, static_cast<int> (sampleRate * 0.035));
    scopePeak = 0.0f;
    scopeCount = 0;
}

void PluginProcessor::releaseResources()
{
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto monoSet = juce::AudioChannelSet::mono();
    const auto stereoSet = juce::AudioChannelSet::stereo();

    // mono vocal in, stereo stack out is the main use; mono/mono and stereo/stereo work too
    return (in == monoSet && out == stereoSet) || (in == monoSet && out == monoSet) || (in == stereoSet && out == stereoSet);
}

StackParams PluginProcessor::readStackParams() const
{
    StackParams params;
    params.stack = juce::roundToInt (raw.stack->load());
    params.spread = raw.spread->load();
    params.octave = raw.octave->load();
    return params;
}

void PluginProcessor::updateHarmony (int numSamples, StackParams& params)
{
    pitchTracker.process (mono.data(), numSamples);

    const auto key = juce::jlimit (0, 11, juce::roundToInt (raw.key->load()));
    const auto minor = raw.scale->load() >= 0.5f;
    const auto steps = Harmony::stepsFor (static_cast<Harmony::Interval> (juce::jlimit (0, 3, juce::roundToInt (raw.interval->load()))));

    // the tracker holds the last pitched note through breaths and consonants; before anything is heard, use the key's root
    const auto heard = pitchTracker.getFrequency();
    const auto sung = heard > 0.0f ? Harmony::midiFromHz (heard) : static_cast<float> (60 + key);

    for (size_t k = 0; k < Harmony::maxVoices; ++k)
        harmonyTarget[k] = static_cast<float> (Harmony::semitonesFor (sung, key, minor, steps.steps[k]));

    // glide to a new harmony note over about 20 ms rather than jumping
    const auto glide = harmonyPrimed ? 1.0f - std::exp (-static_cast<float> (numSamples) / static_cast<float> (0.02 * currentSampleRate)) : 1.0f;
    harmonyPrimed = true;

    for (size_t k = 0; k < Harmony::maxVoices; ++k)
        harmonySemitones[k] += glide * (harmonyTarget[k] - harmonySemitones[k]);

    params.harmony = raw.harmony->load();
    params.harmonyCount = steps.count;
    params.harmonySemitones = harmonySemitones;

    sungNoteForUi.store (pitchTracker.isVoiced() ? sung : -1.0f, std::memory_order_relaxed);
    harmonyCountForUi.store (steps.count, std::memory_order_relaxed);

    for (size_t k = 0; k < Harmony::maxVoices; ++k)
        harmonySemitonesForUi[k].store (juce::roundToInt (harmonyTarget[k]), std::memory_order_relaxed);
}

PluginProcessor::HarmonyState PluginProcessor::getHarmonyState() const
{
    HarmonyState state;
    state.sungNote = sungNoteForUi.load (std::memory_order_relaxed);
    state.count = harmonyCountForUi.load (std::memory_order_relaxed);

    for (size_t k = 0; k < Harmony::maxVoices; ++k)
        state.semitones[k] = harmonySemitonesForUi[k].load (std::memory_order_relaxed);

    return state;
}

std::pair<float, float> PluginProcessor::takeOutputPeaks()
{
    return { outputPeakL.exchange (0.0f), outputPeakR.exchange (0.0f) };
}

void PluginProcessor::renderEffects (const float* dry, float* left, float* right, int numSamples)
{
    const auto toneAmount = raw.tone->load();
    const auto toneOpen = toneAmount >= 0.999f;
    const auto cutoff = toneDarkHz * std::pow (toneOpenHz / toneDarkHz, toneAmount);
    toneL.setCutoff (cutoff, currentSampleRate);
    toneR.setCutoff (cutoff, currentSampleRate);

    echoMix.setTargetValue (raw.echo->load());
    spaceMix.setTargetValue (raw.space->load());
    dirtAmount.setTargetValue (raw.dirt->load());
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (raw.gain->load()));
    duckDepth.setTargetValue (raw.duck->load());

    const auto throwing = raw.throwOn->load() >= 0.5f ? 1.0f : 0.0f;
    throwSend.setTargetValue (throwing);

    double bpm = 120.0;

    if (auto* host = getPlayHead())
        if (auto position = host->getPosition())
            if (auto hostBpm = position->getBpm())
                bpm = juce::jlimit (30.0, 300.0, *hostBpm);

    const auto timeIndex = juce::jlimit (0, 3, juce::roundToInt (raw.echoTime->load()));
    echo.setDelaySeconds (echoBeats[timeIndex] * static_cast<float> (60.0 / bpm));

    auto lowestDuck = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto idx = static_cast<size_t> (i);

        // fully open, Tone is out of the way entirely so the vocal stays untouched
        auto l = toneOpen ? toneL.track (left[i]) : toneL.process (left[i]);
        auto r = toneOpen ? toneR.track (right[i]) : toneR.process (right[i]);

        const auto dirt = dirtAmount.getNextValue();

        if (dirt > 0.001f)
        {
            // dividing by the drive keeps quiet parts at the same level, so Dirt adds grit rather than volume
            const auto drive = 1.0f + dirt * 7.0f;
            const auto makeup = (1.0f + 0.6f * dirt) / drive;
            l = std::tanh (l * drive) * makeup;
            r = std::tanh (r * drive) * makeup;
        }

        // Duck: the echo and reverb dip while the dry vocal is loud and bloom back in the gaps
        const auto level = std::abs (dry[i]);
        if (level > duckEnvelope)
        {
            duckEnvelope += duckAttack * (level - duckEnvelope);
            duckHold = duckHoldSamples;
        }
        else if (duckHold > 0)
        {
            --duckHold;
        }
        else
        {
            duckEnvelope += duckRelease * (level - duckEnvelope);
        }
        const auto loudness = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (duckEnvelope, -100.0f) - duckFloorDb) / duckRangeDb);

        // Throw: open the sends wide, ring longer, and don't duck, so the throw is heard over the vocal
        throwTail += (throwing > throwTail ? throwTailAttack : throwTailRelease) * (throwing - throwTail);
        const auto send = throwSend.getNextValue();
        const auto duckGain = 1.0f - duckDepth.getNextValue() * loudness * (1.0f - throwTail);
        duckGains[idx] = duckGain;
        lowestDuck = juce::jmin (lowestDuck, duckGain);

        const auto echoAmount = echoMix.getNextValue();
        const auto echoSend = echoAmount + send * juce::jmax (0.0f, throwEchoSend - echoAmount);
        const auto feedback = PingPongEcho::defaultFeedback + throwTail * (throwFeedback - PingPongEcho::defaultFeedback);
        echo.process (l, r, echoSend, duckGain, feedback);

        // the reverb is fed through a send too, so a Throw's tail keeps ringing after the send closes
        const auto spaceAmount = spaceMix.getNextValue();
        const auto spaceSend = spaceAmount + send * juce::jmax (0.0f, throwSpaceSend - spaceAmount);
        reverbL[idx] = l * spaceSend;
        reverbR[idx] = r * spaceSend;

        left[i] = l;
        right[i] = r;
    }

    duckGainForUi.store (lowestDuck, std::memory_order_relaxed);

    // the reverb runs wet-only into the scratch buffers, then gets added back
    juce::Reverb::Parameters reverbParams;
    reverbParams.roomSize = 0.7f;
    reverbParams.damping = 0.5f;
    reverbParams.width = 1.0f;
    reverbParams.dryLevel = 0.0f;
    reverbParams.wetLevel = reverbLevel;
    reverbParams.freezeMode = 0.0f;
    reverb.setParameters (reverbParams);
    reverb.processStereo (reverbL.data(), reverbR.data(), numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto idx = static_cast<size_t> (i);
        const auto g = outputGain.getNextValue();
        left[i] = softClip ((left[i] + reverbL[idx] * duckGains[idx]) * g);
        right[i] = softClip ((right[i] + reverbR[idx] * duckGains[idx]) * g);
    }
}

void PluginProcessor::pushScope (const float* input, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        scopePeak = juce::jmax (scopePeak, std::abs (input[i]));

        if (++scopeCount >= scopeStride)
        {
            const auto index = scopeWrite.load (std::memory_order_relaxed);
            scopePeaks[static_cast<size_t> (index)].store (scopePeak, std::memory_order_relaxed);
            scopeWrite.store ((index + 1) % scopeSize, std::memory_order_release);
            scopePeak = 0.0f;
            scopeCount = 0;
        }
    }
}

void PluginProcessor::getInputHistory (float* dest, int count) const
{
    count = juce::jmin (count, scopeSize);
    const auto newest = scopeWrite.load (std::memory_order_acquire);

    for (int i = 0; i < count; ++i)
    {
        const auto index = (newest - count + i + scopeSize) % scopeSize;
        dest[i] = scopePeaks[static_cast<size_t> (index)].load (std::memory_order_relaxed);
    }
}

void PluginProcessor::processChunk (const float* inL, const float* inR, float* outL, float* outR, int numSamples)
{
    activeMix.setTargetValue (raw.active->load() >= 0.5f ? 1.0f : 0.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto idx = static_cast<size_t> (i);
        mono[idx] = 0.5f * (inL[i] + inR[i]);
        wetL[idx] = inL[i];
        wetR[idx] = inR[i];
    }

    pushScope (mono.data(), numSamples);

    auto params = readStackParams();
    updateHarmony (numSamples, params);
    engine.setParams (params);

    // the original vocal is already in wetL/wetR; the engine adds the copies
    engine.process (mono.data(), wetL.data(), wetR.data(), numSamples);
    renderEffects (mono.data(), wetL.data(), wetR.data(), numSamples);

    // Stack On crossfades between the processed sound and the untouched input
    float peakL = 0.0f, peakR = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto idx = static_cast<size_t> (i);
        const auto amount = activeMix.getNextValue();
        outL[i] = inL[i] + amount * (wetL[idx] - inL[i]);
        outR[i] = inR[i] + amount * (wetR[idx] - inR[i]);
        peakL = juce::jmax (peakL, std::abs (outL[i]));
        peakR = juce::jmax (peakR, std::abs (outR[i]));
    }

    // the UI takes (and clears) these, so keep the loudest since it last looked
    if (peakL > outputPeakL.load (std::memory_order_relaxed))
        outputPeakL.store (peakL, std::memory_order_relaxed);

    if (peakR > outputPeakR.load (std::memory_order_relaxed))
        outputPeakR.store (peakR, std::memory_order_relaxed);
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const auto numIn = getTotalNumInputChannels();
    const auto numOut = getTotalNumOutputChannels();
    const auto numSamples = buffer.getNumSamples();

    if (numIn == 0 || numOut == 0 || buffer.getNumChannels() < juce::jmax (numIn, numOut) || mono.empty())
        return;

    auto* left = buffer.getWritePointer (0);
    auto* right = numOut > 1 ? buffer.getWritePointer (1) : nullptr;

    // inputs and outputs share the buffer, so work through it in chunks via the scratch buffers
    std::array<float, chunkSize> inL {}, inR {}, outL {}, outR {};

    for (int start = 0; start < numSamples; start += chunkSize)
    {
        const auto count = juce::jmin (chunkSize, numSamples - start);
        const auto* srcL = buffer.getReadPointer (0, start);
        const auto* srcR = numIn > 1 ? buffer.getReadPointer (1, start) : srcL;

        std::copy (srcL, srcL + count, inL.begin());
        std::copy (srcR, srcR + count, inR.begin());

        processChunk (inL.data(), inR.data(), outL.data(), outR.data(), count);

        if (right != nullptr)
        {
            std::copy (outL.begin(), outL.begin() + count, left + start);
            std::copy (outR.begin(), outR.begin() + count, right + start);
        }
        else
        {
            for (int i = 0; i < count; ++i)
                left[start + i] = 0.5f * (outL[static_cast<size_t> (i)] + outR[static_cast<size_t> (i)]);
        }
    }

    for (auto channel = numOut; channel < buffer.getNumChannels(); ++channel)
        buffer.clear (channel, 0, numSamples);
}

//==============================================================================
bool PluginProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor (*this);
}

//==============================================================================
void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml (*xml);

    if (state.hasType (apvts.state.getType()))
        apvts.replaceState (state);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
