#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float echoBeats[] = { 0.25f, 0.5f, 0.75f, 1.0f };
    constexpr float toneOpenHz = 20000.0f;
    constexpr float toneDarkHz = 1200.0f;
}

//==============================================================================
PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createLayout())
{
    formatManager.registerBasicFormats();

    // look the parameters up once, so the audio thread never searches by name
    const auto find = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    raw = { find (ParamIDs::stack), find (ParamIDs::spread), find (ParamIDs::octave), find (ParamIDs::tone),
            find (ParamIDs::dirt), find (ParamIDs::echo), find (ParamIDs::echoTime), find (ParamIDs::space),
            find (ParamIDs::gain), find (ParamIDs::oneShot) };
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

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::stack, 1 }, "Stack", 1, StackEngine::maxStack, 3));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::spread, 1 }, "Spread", NormalisableRange<float> (0.0f, 1.0f), 0.5f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::octave, 1 }, "Octave", NormalisableRange<float> (0.0f, 1.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::tone, 1 }, "Tone", NormalisableRange<float> (0.0f, 1.0f), 1.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::dirt, 1 }, "Dirt", NormalisableRange<float> (0.0f, 1.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::echo, 1 }, "Echo", NormalisableRange<float> (0.0f, 1.0f), 0.2f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::echoTime, 1 }, "Echo Time", juce::StringArray { "1/16", "1/8", "1/8 dotted", "1/4" }, 1));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::space, 1 }, "Space", NormalisableRange<float> (0.0f, 1.0f), 0.2f, juce::AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::gain, 1 }, "Gain", NormalisableRange<float> (-24.0f, 6.0f), 0.0f, juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float value, int) { return juce::String (value, 1) + " dB"; })));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::oneShot, 1 }, "One-Shot", true));

    return layout;
}

//==============================================================================
const juce::String PluginProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PluginProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PluginProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double PluginProcessor::getTailLengthSeconds() const
{
    // the echo repeats and the reverb ring on after the last note
    return 6.0;
}

int PluginProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int PluginProcessor::getCurrentProgram()
{
    return 0;
}

void PluginProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String PluginProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    // Steinberg's VST3 validator fails plugins whose single default program has no name
    return "Default";
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
    echo.prepare (sampleRate);
    reverb.setSampleRate (sampleRate);
    reverb.reset();
    toneL.reset();
    toneR.reset();

    const auto smooth = [sampleRate] (juce::SmoothedValue<float>& value, float start)
    {
        value.reset (sampleRate, 0.03);
        value.setCurrentAndTargetValue (start);
    };

    smooth (echoMix, raw.echo->load());
    smooth (spaceMix, raw.space->load());
    smooth (dirtAmount, raw.dirt->load());
    smooth (outputGain, juce::Decibels::decibelsToGain (raw.gain->load()));

    // the engine only points at the sample, so hand it over again after a re-prepare
    {
        const juce::ScopedLock lock (sampleLock);

        if (displaySample != nullptr)
            engine.setSample (displaySample.get());
    }
}

void PluginProcessor::releaseResources()
{
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

StackParams PluginProcessor::readStackParams() const
{
    StackParams params;
    params.stack = juce::roundToInt (raw.stack->load());
    params.spread = raw.spread->load();
    params.octave = raw.octave->load();
    params.oneShot = raw.oneShot->load() >= 0.5f;
    return params;
}

void PluginProcessor::renderEffects (float* left, float* right, int numSamples)
{
    const auto toneAmount = raw.tone->load();
    const auto cutoff = toneDarkHz * std::pow (toneOpenHz / toneDarkHz, toneAmount);
    toneL.setCutoff (cutoff, currentSampleRate);
    toneR.setCutoff (cutoff, currentSampleRate);

    echoMix.setTargetValue (raw.echo->load());
    spaceMix.setTargetValue (raw.space->load());
    dirtAmount.setTargetValue (raw.dirt->load());
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (raw.gain->load()));

    double bpm = 120.0;

    if (auto* host = getPlayHead())
        if (auto position = host->getPosition())
            if (auto hostBpm = position->getBpm())
                bpm = juce::jlimit (30.0, 300.0, *hostBpm);

    const auto timeIndex = juce::jlimit (0, 3, juce::roundToInt (raw.echoTime->load()));
    echo.setDelaySeconds (echoBeats[timeIndex] * static_cast<float> (60.0 / bpm));

    for (int i = 0; i < numSamples; ++i)
    {
        auto l = toneL.process (left[i]);
        auto r = toneR.process (right[i]);

        const auto dirt = dirtAmount.getNextValue();

        if (dirt > 0.001f)
        {
            const auto drive = 1.0f + dirt * 7.0f;
            const auto norm = 1.0f / std::tanh (drive);
            l = std::tanh (l * drive) * norm;
            r = std::tanh (r * drive) * norm;
        }

        echo.process (l, r, echoMix.getNextValue());

        left[i] = l;
        right[i] = r;
    }

    juce::Reverb::Parameters reverbParams;
    reverbParams.roomSize = 0.7f;
    reverbParams.damping = 0.5f;
    reverbParams.width = 1.0f;
    reverbParams.dryLevel = 1.0f;
    reverbParams.wetLevel = spaceMix.getTargetValue() * 0.4f;
    reverbParams.freezeMode = 0.0f;
    reverb.setParameters (reverbParams);
    reverb.processStereo (left, right, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto g = outputGain.getNextValue();
        left[i] = softClip (left[i] * g);
        right[i] = softClip (right[i] * g);
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear();

    if (buffer.getNumChannels() < 2)
        return;

    const auto numSamples = buffer.getNumSamples();
    keyboardState.processNextMidiBuffer (midiMessages, 0, numSamples, true);

    if (auto* incoming = pendingSample.exchange (nullptr))
        engine.setSample (incoming);

    const auto params = readStackParams();
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);

    // render up to each MIDI event so notes start on the exact sample
    int position = 0;

    for (const auto metadata : midiMessages)
    {
        const auto eventPosition = juce::jlimit (position, numSamples, metadata.samplePosition);

        if (eventPosition > position)
        {
            engine.render (left + position, right + position, eventPosition - position);
            position = eventPosition;
        }

        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            engine.noteOn (message.getNoteNumber(), message.getFloatVelocity(), params);
        else if (message.isNoteOff())
            engine.noteOff (message.getNoteNumber());
        else if (message.isAllNotesOff() || message.isAllSoundOff())
            engine.allNotesOff();
    }

    if (position < numSamples)
        engine.render (left + position, right + position, numSamples - position);

    renderEffects (left, right, numSamples);
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
bool PluginProcessor::loadSampleFromFile (const juce::File& file, juce::String& errorMessage)
{
    auto loaded = SampleData::fromFile (file, formatManager, errorMessage);

    if (loaded == nullptr)
        return false;

    setSample (std::move (loaded));
    return true;
}

void PluginProcessor::setSample (std::shared_ptr<const SampleData> newSample)
{
    if (newSample == nullptr)
        return;

    {
        const juce::ScopedLock lock (sampleLock);
        displaySample = newSample;
        recentSamples[recentIndex] = newSample;
        recentIndex = (recentIndex + 1) % recentSamples.size();
    }

    pendingSample.store (newSample.get());
}

std::shared_ptr<const SampleData> PluginProcessor::getSample() const
{
    const juce::ScopedLock lock (sampleLock);
    return displaySample;
}

//==============================================================================
void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    if (auto sample = getSample())
    {
        juce::ValueTree sampleTree ("Sample");
        sampleTree.setProperty ("name", sample->name, nullptr);
        sampleTree.setProperty ("sampleRate", sample->sampleRate, nullptr);
        sampleTree.setProperty ("data", sample->toBase64(), nullptr);
        state.appendChild (sampleTree, nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml (*xml);

    if (! state.hasType (apvts.state.getType()))
        return;

    auto sampleTree = state.getChildWithName ("Sample");

    if (sampleTree.isValid())
    {
        state.removeChild (sampleTree, nullptr);

        if (auto restored = SampleData::fromBase64 (sampleTree["data"].toString(), sampleTree["sampleRate"], sampleTree["name"].toString()))
            setSample (std::move (restored));
    }

    apvts.replaceState (state);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
