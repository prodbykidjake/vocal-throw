#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
PluginProcessor::PluginProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
      apvts (*this, nullptr, "SignalPathState", Params::createLayout())
{
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::bypass));
    inputGainDb = apvts.getRawParameterValue (ParamIDs::inputGain);
    outputGainDb = apvts.getRawParameterValue (ParamIDs::outputGain);
    jassert (bypassParam != nullptr && inputGainDb != nullptr && outputGainDb != nullptr);
}

PluginProcessor::~PluginProcessor()
{
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
    return 0.0;
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
    maxChunkSize = juce::jmax (1, samplesPerBlock);
    dryBuffer.setSize (2, maxChunkSize, false, false, false);
    dryBuffer.clear();

    inputGain.setGainDb (inputGainDb->load());
    outputGain.setGainDb (outputGainDb->load());
    inputGain.prepare (sampleRate);
    outputGain.prepare (sampleRate);

    wetMix.reset (sampleRate, 0.01);
    wetMix.setCurrentAndTargetValue (bypassParam->get() ? 0.0f : 1.0f);
}

void PluginProcessor::releaseResources()
{
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const auto numChannels = juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels(), buffer.getNumChannels());
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    wetMix.setTargetValue (bypassParam->get() ? 0.0f : 1.0f);
    inputGain.setGainDb (inputGainDb->load());
    outputGain.setGainDb (outputGainDb->load());

    // Hosts may exceed the block size they announced; chunking keeps us allocation-free.
    for (int start = 0; start < buffer.getNumSamples(); start += maxChunkSize)
        processChunk (buffer, start, juce::jmin (maxChunkSize, buffer.getNumSamples() - start), numChannels);
}

void PluginProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int numSamples, int numChannels)
{
    const bool settledBypassed = ! wetMix.isSmoothing() && juce::exactlyEqual (wetMix.getTargetValue(), 0.0f);
    if (settledBypassed)
    {
        wetMix.skip (numSamples);
        inputGain.reset();
        outputGain.reset();
        return; // untouched input: bit-exact bypass
    }

    const bool crossfading = wetMix.isSmoothing() || ! juce::exactlyEqual (wetMix.getTargetValue(), 1.0f);
    if (crossfading)
        for (int ch = 0; ch < numChannels; ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, start, numSamples);

    // Chain: Input Gain -> (EQ -> Compressor -> Saturation, later) -> Output Gain
    inputGain.process (buffer, numChannels, start, numSamples);
    outputGain.process (buffer, numChannels, start, numSamples);

    if (crossfading)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto wet = wetMix.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* out = buffer.getWritePointer (ch, start);
                const auto dry = dryBuffer.getReadPointer (ch)[i];
                out[i] = dry + wet * (out[i] - dry);
            }
        }
    }
}

//==============================================================================
bool PluginProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
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
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
