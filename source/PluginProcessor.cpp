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
      apvts (*this, &undoManager, "SignalPathState", Params::createLayout())
{
    for (auto* p : getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            rawParams[withId->paramID.toStdString()] = apvts.getRawParameterValue (withId->paramID);

    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::bypass));
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    h.inputGain = raw (ParamIDs::inputGain);
    h.outputGain = raw (ParamIDs::outputGain);
    h.bypass = raw (ParamIDs::bypass);
    h.loudnessMatch = raw (ParamIDs::loudnessMatch);
    h.eqEnable = raw (ParamIDs::eqEnable);
    h.eqHpEnable = raw (ParamIDs::eqHpEnable);
    h.eqHpFreq = raw (ParamIDs::eqHpFreq);
    for (size_t i = 0; i < 4; ++i)
    {
        h.eqFreq[i] = raw (ParamIDs::eqFreq[i]);
        h.eqGain[i] = raw (ParamIDs::eqGain[i]);
        h.eqQ[i] = raw (ParamIDs::eqQ[i]);
    }
    h.compEnable = raw (ParamIDs::compEnable);
    h.compThreshold = raw (ParamIDs::compThreshold);
    h.compRatio = raw (ParamIDs::compRatio);
    h.compAttack = raw (ParamIDs::compAttack);
    h.compRelease = raw (ParamIDs::compRelease);
    h.compKnee = raw (ParamIDs::compKnee);
    h.compMakeup = raw (ParamIDs::compMakeup);
    h.compMix = raw (ParamIDs::compMix);
    h.satEnable = raw (ParamIDs::satEnable);
    h.satDrive = raw (ParamIDs::satDrive);
    h.satMix = raw (ParamIDs::satMix);
    jassert (bypassParam != nullptr);
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
    currentSampleRate = sampleRate;
    maxChunkSize = juce::jmax (1, samplesPerBlock);
    dryBuffer.setSize (2, maxChunkSize, false, false, false);
    dryBuffer.clear();

    chain.prepare (sampleRate, maxChunkSize);
    chain.snapToParams (readParams());
    inputMeter.prepare (sampleRate);
    outputMeter.prepare (sampleRate);
    inputLoudness.prepare (sampleRate);
    outputLoudness.prepare (sampleRate);

    wetMix.reset (sampleRate, 0.01);
    wetMix.setCurrentAndTargetValue (h.bypass->load() > 0.5f ? 0.0f : 1.0f);
    matchGain.reset (sampleRate, 0.05);
    matchGain.setCurrentAndTargetValue (1.0f);
    loudnessOffsetDb.store (0.0f);
}

void PluginProcessor::releaseResources()
{
}

ChainParams PluginProcessor::readParams() const
{
    auto v = [] (const std::atomic<float>* p) { return p->load (std::memory_order_relaxed); };
    ChainParams p;
    p.inputGainDb = v (h.inputGain);
    p.outputGainDb = v (h.outputGain);
    p.eqEnabled = v (h.eqEnable) > 0.5f;
    p.eq.hpEnabled = v (h.eqHpEnable) > 0.5f;
    p.eq.hpFreq = v (h.eqHpFreq);
    for (size_t i = 0; i < 4; ++i)
        p.eq.bands[i] = { v (h.eqFreq[i]), v (h.eqGain[i]), v (h.eqQ[i]) };
    p.compEnabled = v (h.compEnable) > 0.5f;
    p.comp = { v (h.compThreshold), v (h.compRatio), v (h.compAttack), v (h.compRelease),
               v (h.compKnee), v (h.compMakeup), v (h.compMix) * 0.01f };
    p.satEnabled = v (h.satEnable) > 0.5f;
    p.sat = { v (h.satDrive), v (h.satMix) * 0.01f };
    return p;
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
    const auto startTicks = juce::Time::getHighResolutionTicks();

    const auto numChannels = juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels(), buffer.getNumChannels());
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    if (numChannels == 0 || buffer.getNumSamples() == 0)
        return;

    inputMeter.process (buffer, numChannels);
    inputLoudness.process (buffer, numChannels);
    captureFifo.push (buffer, numChannels);

    wetMix.setTargetValue (h.bypass->load() > 0.5f ? 0.0f : 1.0f);
    chain.setParams (readParams());

    // Hosts may exceed the block size they announced; chunking keeps us allocation-free.
    for (int start = 0; start < buffer.getNumSamples(); start += maxChunkSize)
        processChunk (buffer, start, juce::jmin (maxChunkSize, buffer.getNumSamples() - start), numChannels);

    outputMeter.process (buffer, numChannels);

    const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks);
    const auto available = buffer.getNumSamples() / currentSampleRate;
    const auto previous = cpuLoad.load (std::memory_order_relaxed);
    cpuLoad.store (previous + 0.1f * ((float) (elapsed / available) - previous), std::memory_order_relaxed);
}

void PluginProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int numSamples, int numChannels)
{
    const bool bypassTarget = juce::exactlyEqual (wetMix.getTargetValue(), 0.0f);
    const bool settledBypassed = ! wetMix.isSmoothing() && bypassTarget;
    const bool matchWanted = bypassTarget && h.loudnessMatch->load (std::memory_order_relaxed) > 0.5f;
    matchGain.setTargetValue (matchWanted ? juce::Decibels::decibelsToGain (loudnessOffsetDb.load (std::memory_order_relaxed)) : 1.0f);
    const bool matchActive = matchGain.isSmoothing() || ! juce::exactlyEqual (matchGain.getTargetValue(), 1.0f);

    if (settledBypassed)
    {
        chain.reset();
        wetMix.skip (numSamples);
        if (! matchActive)
        {
            matchGain.skip (numSamples);
            return; // untouched input: bit-exact bypass
        }
        for (int i = 0; i < numSamples; ++i)
        {
            const auto g = matchGain.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch, start)[i] *= g;
        }
        return;
    }

    const bool crossfading = wetMix.isSmoothing() || bypassTarget;
    if (crossfading)
        for (int ch = 0; ch < numChannels; ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, start, numSamples);

    chain.process (buffer, numChannels, start, numSamples);

    // Learn how much louder/quieter the chain makes the signal, so bypass can be level-matched.
    if (! bypassTarget)
    {
        outputLoudness.process (juce::AudioBuffer<float> (buffer.getArrayOfWritePointers(), numChannels, start, numSamples), numChannels);
        const auto in = inputLoudness.getLufs(), out = outputLoudness.getLufs();
        if (in > -70.0f && out > -70.0f)
            loudnessOffsetDb.store (juce::jlimit (-12.0f, 12.0f, out - in), std::memory_order_relaxed);
    }

    if (crossfading)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto wet = wetMix.getNextValue();
            const auto mg = matchGain.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* out = buffer.getWritePointer (ch, start);
                const auto dry = dryBuffer.getReadPointer (ch)[i] * mg;
                out[i] = dry + wet * (out[i] - dry);
            }
        }
    }
    else
        matchGain.skip (numSamples);
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
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            undoManager.clearUndoHistory();
        }
}

std::map<std::string, float> PluginProcessor::getParamValues() const
{
    std::map<std::string, float> out;
    for (const auto& [id, raw] : rawParams)
        if (id != ParamIDs::bypass && id != ParamIDs::loudnessMatch) // presets and A/B never carry these
            out[id] = raw->load();
    return out;
}

void PluginProcessor::setParamValues (const std::map<std::string, float>& values, const juce::String& undoName)
{
    // Edit through the state tree with the undo manager attached: the tree change updates the parameter
    // synchronously (and notifies the host), and is recorded as a single undoable transaction.
    // First flush parameter changes the state tree has not seen yet (APVTS does this on a timer); otherwise
    // writing a value equal to the stale tree value would be a no-op and leave the parameter unchanged.
    [[maybe_unused]] const auto flushed = apvts.copyState();
    undoManager.beginNewTransaction (undoName);
    for (const auto& [id, value] : values)
        if (auto* p = apvts.getParameter (id))
        {
            auto treeValue = apvts.getParameterAsValue (id);
            treeValue.setValue (p->convertFrom0to1 (p->convertTo0to1 (value))); // clamps and snaps to the parameter's range
        }
}

void PluginProcessor::switchABSlot()
{
    // Park the live settings in the current slot, then load the other slot (or keep the live
    // settings the first time the other slot is visited, so B starts as a copy of A).
    const int current = getActiveSlot();
    const int other = 1 - current;
    auto store = [this] (int slot, const std::map<std::string, float>& values) {
        auto node = apvts.state.getOrCreateChildWithName ("AB" + juce::String (slot), nullptr);
        node.removeAllProperties (nullptr);
        for (const auto& [id, v] : values)
            node.setProperty (juce::String (id), v, nullptr);
    };
    store (current, getParamValues());

    auto node = apvts.state.getChildWithName ("AB" + juce::String (other));
    apvts.state.setProperty ("abSlot", other, nullptr);
    if (node.isValid() && node.getNumProperties() > 0)
    {
        std::map<std::string, float> values;
        for (int i = 0; i < node.getNumProperties(); ++i)
            values[node.getPropertyName (i).toString().toStdString()] = (float) node.getProperty (node.getPropertyName (i));
        setParamValues (values, "A/B switch");
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
