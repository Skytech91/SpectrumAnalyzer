#include "PluginProcessor.h"
#include "PluginEditor.h"

SpectrumAnalyzerProcessor::SpectrumAnalyzerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& s : ringL)
        s.store (0.0f);
    for (auto& s : ringR)
        s.store (0.0f);
}

void SpectrumAnalyzerProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
}

bool SpectrumAnalyzerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

void SpectrumAnalyzerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    const float* left  = buffer.getReadPointer (0);
    const float* right = numChannels > 1 ? buffer.getReadPointer (1) : left;

    int index = writeIndex.load (std::memory_order_relaxed);

    // Keeps the latest left and right samples in two circular buffers.
    // The sound is NOT changed: the plugin only "looks" at it.
    for (int i = 0; i < numSamples; ++i)
    {
        ringL[(size_t) index].store (left[i],  std::memory_order_relaxed);
        ringR[(size_t) index].store (right[i], std::memory_order_relaxed);
        index = (index + 1) & (maxFftSize - 1);
    }

    writeIndex.store (index, std::memory_order_release);
}

void SpectrumAnalyzerProcessor::copyLatestSamples (float* destination, int channel, int count) const
{
    const auto& ring = (channel == 0) ? ringL : ringR;
    const int start = writeIndex.load (std::memory_order_acquire);

    for (int i = 0; i < count; ++i)
        destination[i] = ring[(size_t) ((start - count + i + maxFftSize) & (maxFftSize - 1))]
                             .load (std::memory_order_relaxed);
}

void SpectrumAnalyzerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("MugenSpectrumSettings");
    xml.setAttribute ("colourMode",    settings.colourMode.load());
    xml.setAttribute ("singleColour",  juce::Colour (settings.singleColour.load()).toString());
    xml.setAttribute ("fillStyle",     settings.fillStyle.load());
    xml.setAttribute ("lineThickness", (double) settings.lineThickness.load());
    xml.setAttribute ("background",    settings.background.load());
    xml.setAttribute ("animate",       settings.animate.load() ? 1 : 0);
    xml.setAttribute ("peakHold",      settings.peakHold.load() ? 1 : 0);
    xml.setAttribute ("tilt",          settings.tilt.load());
    xml.setAttribute ("speed",         settings.speed.load());
    xml.setAttribute ("range",         settings.range.load());
    xml.setAttribute ("stereoMode",    settings.stereoMode.load());
    xml.setAttribute ("fftSizeIndex",  settings.fftSizeIndex.load());
    copyXmlToBinary (xml, destData);
}

void SpectrumAnalyzerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName ("MugenSpectrumSettings"))
        {
            settings.colourMode    = xml->getIntAttribute ("colourMode", 0);
            settings.singleColour  = juce::Colour::fromString (xml->getStringAttribute ("singleColour", "ff35e0c2")).getARGB();
            settings.fillStyle     = xml->getIntAttribute ("fillStyle", 0);
            settings.lineThickness = (float) xml->getDoubleAttribute ("lineThickness", 1.6);
            settings.background    = xml->getIntAttribute ("background", 0);
            settings.animate       = xml->getIntAttribute ("animate", 0) != 0;
            settings.peakHold      = xml->getIntAttribute ("peakHold", 0) != 0;
            settings.tilt          = xml->getIntAttribute ("tilt", 0);
            settings.speed         = xml->getIntAttribute ("speed", 1);
            settings.range         = xml->getIntAttribute ("range", 1);
            settings.stereoMode    = xml->getIntAttribute ("stereoMode", 0);
            settings.fftSizeIndex  = xml->getIntAttribute ("fftSizeIndex", 1);
        }
    }
}

juce::AudioProcessorEditor* SpectrumAnalyzerProcessor::createEditor()
{
    return new SpectrumAnalyzerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpectrumAnalyzerProcessor();
}
