#include "PluginProcessor.h"
#include "PluginEditor.h"

SpectrumAnalyzerProcessor::SpectrumAnalyzerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& s : ring)
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

    int index = writeIndex.load (std::memory_order_relaxed);

    // Junta os canais num so (media) e guarda no buffer circular.
    // O som NAO e alterado: o plugin so "olha" para ele.
    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += buffer.getReadPointer (ch)[i];

        ring[(size_t) index].store (sum / (float) numChannels, std::memory_order_relaxed);
        index = (index + 1) & (fftSize - 1);
    }

    writeIndex.store (index, std::memory_order_release);
}

void SpectrumAnalyzerProcessor::copyLatestSamples (float* destination) const
{
    const int start = writeIndex.load (std::memory_order_acquire);

    for (int i = 0; i < fftSize; ++i)
        destination[i] = ring[(size_t) ((start + i) & (fftSize - 1))].load (std::memory_order_relaxed);
}

juce::AudioProcessorEditor* SpectrumAnalyzerProcessor::createEditor()
{
    return new SpectrumAnalyzerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpectrumAnalyzerProcessor();
}
