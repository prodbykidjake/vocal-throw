#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
PluginProcessor::PluginProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                       .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
{
    for (int b = 0; b < ParamIDs::numBands; ++b)
        for (int p = 0; p < ParamIDs::numBandParams; ++p)
            bandValues[(size_t) b][(size_t) p] = apvts.getRawParameterValue (ParamIDs::bandParamID (b + 1, (ParamIDs::BandParam) p));
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
    juce::ignoreUnused (samplesPerBlock);

    suppressor.prepare (sampleRate, getMainBusNumInputChannels());
    suppressor.setParams (readParams());
    setLatencySamples (suppressor.getLatencySamples());
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

    // The optional sidechain key can be off, mono or stereo
    if (layouts.inputBuses.size() > 1)
    {
        const auto& key = layouts.getChannelSet (true, 1);
        if (! key.isDisabled() && key != juce::AudioChannelSet::mono() && key != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
  #endif
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);

    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    suppressor.setParams (readParams());

    // getBusBuffer only builds a view onto the host's channels, nothing is allocated
    auto main = getBusBuffer (buffer, true, 0);
    if (isKeyConnected())
    {
        auto key = getBusBuffer (buffer, true, 1);
        suppressor.process (main, &key);
    }
    else
    {
        suppressor.process (main);
    }
}

bool PluginProcessor::isKeyConnected() const
{
    return getBusCount (true) > 1 && getBus (true, 1)->isEnabled();
}

vs::SuppressorParams PluginProcessor::readParams() const noexcept
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    vs::SuppressorParams p;
    p.hard = get (ParamIDs::mode) >= 0.5f;
    p.depth = get (ParamIDs::depth);
    p.detail = get (ParamIDs::detail);
    p.attack = get (ParamIDs::attack);
    p.release = get (ParamIDs::release);
    p.maxCutDb = get (ParamIDs::maxCut);
    p.wetTrimDb = get (ParamIDs::wetTrim);
    p.mix = get (ParamIDs::mix);
    p.outGainDb = get (ParamIDs::outGain);
    p.midSide = get (ParamIDs::stereoMode) >= 0.5f;
    p.link = get (ParamIDs::link);
    p.delta = get (ParamIDs::delta) >= 0.5f;
    p.bypass = get (ParamIDs::bypass) >= 0.5f;
    p.selectivity = get (ParamIDs::selectivity);
    p.balance = get (ParamIDs::stereoBalance);
    p.quality = juce::jlimit (0, 2, (int) std::lround (get (ParamIDs::quality)));
    p.sidechain = get (ParamIDs::sidechain) >= 0.5f;
    p.keyListen = get (ParamIDs::keyListen) >= 0.5f;
    p.detailTiltLow = get (ParamIDs::detailLow);
    p.detailTiltHigh = get (ParamIDs::detailHigh);
    p.attackTiltLow = get (ParamIDs::attackLow);
    p.attackTiltHigh = get (ParamIDs::attackHigh);
    p.releaseTiltLow = get (ParamIDs::releaseLow);
    p.releaseTiltHigh = get (ParamIDs::releaseHigh);

    for (size_t b = 0; b < p.bands.size(); ++b)
    {
        const auto& v = bandValues[b];
        auto& band = p.bands[b];
        using ParamIDs::BandParam;
        band.enabled = v[(size_t) BandParam::active]->load() >= 0.5f && v[(size_t) BandParam::enabled]->load() >= 0.5f;
        band.shape = (vs::BandShape) juce::jlimit (0, vs::numBandShapes - 1, (int) std::lround (v[(size_t) BandParam::shape]->load()));
        band.freqHz = v[(size_t) BandParam::freq]->load();
        band.gainDb = v[(size_t) BandParam::gain]->load();
        band.q = v[(size_t) BandParam::q]->load();
        band.balance = v[(size_t) BandParam::balance]->load() / 50.0f;
    }
    return p;
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
