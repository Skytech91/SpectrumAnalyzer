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

    setupKFilter (44100.0);
}

void SpectrumAnalyzerProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    subBlockLength = juce::jmax (1, (int) std::lround (0.1 * sampleRate));
    setupKFilter (sampleRate);
    resetLoudnessState();
}

bool SpectrumAnalyzerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

// K-weighting filters from ITU-R BS.1770 (shelving filter + high-pass), for any sample rate
void SpectrumAnalyzerProcessor::setupKFilter (double fs)
{
    const double pi = juce::MathConstants<double>::pi;

    // stage 1: high shelf
    double f0 = 1681.974450955533;
    double G  = 3.999843853973347;
    double Q  = 0.7071752369554196;
    double K  = std::tan (pi * f0 / fs);
    const double Vh = std::pow (10.0, G / 20.0);
    const double Vb = std::pow (Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;

    Biquad shelf;
    shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
    shelf.b1 = 2.0 * (K * K - Vh) / a0;
    shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
    shelf.a1 = 2.0 * (K * K - 1.0) / a0;
    shelf.a2 = (1.0 - K / Q + K * K) / a0;

    // stage 2: high-pass
    f0 = 38.13547087602444;
    Q  = 0.5003270373238773;
    K  = std::tan (pi * f0 / fs);
    const double a0h = 1.0 + K / Q + K * K;

    Biquad hp;
    hp.b0 = 1.0;
    hp.b1 = -2.0;
    hp.b2 = 1.0;
    hp.a1 = 2.0 * (K * K - 1.0) / a0h;
    hp.a2 = (1.0 - K / Q + K * K) / a0h;

    for (int ch = 0; ch < 2; ++ch)
    {
        kShelf[ch] = shelf;
        kHighPass[ch] = hp;
    }
}

void SpectrumAnalyzerProcessor::resetLoudnessState()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        kShelf[ch].reset();
        kHighPass[ch].reset();
    }

    subBlockSumSq = 0.0;
    subBlockCount = 0;
    subEnergies.fill (0.0);
    subWrite = 0;
    subFilled = 0;
    histEnergy.fill (0.0);
    histCount.fill (0.0);
    peakLinear = 0.0;

    loudnessMomentary  = -120.0f;
    loudnessShortTerm  = -120.0f;
    loudnessIntegrated = -120.0f;
    samplePeakDb       = -120.0f;
}

// Called every 100 ms: updates momentary, short-term and integrated loudness
void SpectrumAnalyzerProcessor::finishSubBlock()
{
    const double energy = subBlockSumSq / (double) subBlockLength;
    subBlockSumSq = 0.0;
    subBlockCount = 0;

    subEnergies[(size_t) subWrite] = energy;
    subWrite = (subWrite + 1) % 30;
    subFilled = juce::jmin (subFilled + 1, 30);

    auto meanOfLast = [this] (int n)
    {
        n = juce::jmin (n, subFilled);
        double sum = 0.0;
        for (int k = 1; k <= n; ++k)
            sum += subEnergies[(size_t) ((subWrite - k + 30) % 30)];
        return sum / (double) n;
    };

    auto toLoudness = [] (double e)
    {
        return e > 1.0e-12 ? juce::jmax (-120.0, -0.691 + 10.0 * std::log10 (e)) : -120.0;
    };

    const double momentaryEnergy  = meanOfLast (4);     // 400 ms
    const double shortTermEnergy  = meanOfLast (30);    // 3 s

    loudnessMomentary.store ((float) toLoudness (momentaryEnergy));
    loudnessShortTerm.store ((float) toLoudness (shortTermEnergy));

    // integrated: 400 ms blocks, absolute gate at -70 LUFS, relative gate 10 LU below the average
    if (subFilled >= 4)
    {
        const double blockLoudness = toLoudness (momentaryEnergy);

        if (blockLoudness >= -70.0)
        {
            const int index = juce::jlimit (0, histBins - 1, (int) std::lround ((blockLoudness + 70.0) * 10.0));
            histEnergy[(size_t) index] += momentaryEnergy;
            histCount[(size_t) index] += 1.0;
        }

        double totalEnergy = 0.0, totalCount = 0.0;
        for (int i = 0; i < histBins; ++i)
        {
            totalEnergy += histEnergy[(size_t) i];
            totalCount  += histCount[(size_t) i];
        }

        if (totalCount > 0.0)
        {
            const double relativeThreshold = toLoudness (totalEnergy / totalCount) - 10.0;

            double gatedEnergy = 0.0, gatedCount = 0.0;
            for (int i = 0; i < histBins; ++i)
            {
                if (-70.0 + 0.1 * (double) i >= relativeThreshold)
                {
                    gatedEnergy += histEnergy[(size_t) i];
                    gatedCount  += histCount[(size_t) i];
                }
            }

            loudnessIntegrated.store (gatedCount > 0.0 ? (float) toLoudness (gatedEnergy / gatedCount) : -120.0f);
        }
    }
}

void SpectrumAnalyzerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    if (resetLoudnessRequested.exchange (false))
        resetLoudnessState();

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

    // Loudness: K-weighting, then the energy is collected in 100 ms blocks
    const int measuredChannels = (numChannels > 1) ? 2 : 1;

    for (int i = 0; i < numSamples; ++i)
    {
        double energy = 0.0;

        for (int ch = 0; ch < measuredChannels; ++ch)
        {
            const double x = (double) (ch == 0 ? left[i] : right[i]);
            peakLinear = juce::jmax (peakLinear, std::abs (x));

            const double k = kHighPass[ch].process (kShelf[ch].process (x));
            energy += k * k;
        }

        subBlockSumSq += energy;

        if (++subBlockCount >= subBlockLength)
            finishSubBlock();
    }

    samplePeakDb.store (peakLinear > 1.0e-6 ? (float) (20.0 * std::log10 (peakLinear)) : -120.0f);
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
    xml.setAttribute ("colourMode",     settings.colourMode.load());
    xml.setAttribute ("singleColour",   juce::Colour (settings.singleColour.load()).toString());
    xml.setAttribute ("colourStereoA",  juce::Colour (settings.colourStereoA.load()).toString());
    xml.setAttribute ("colourB",        juce::Colour (settings.colourB.load()).toString());
    xml.setAttribute ("fillStyle",      settings.fillStyle.load());
    xml.setAttribute ("lineThickness",  (double) settings.lineThickness.load());
    xml.setAttribute ("background",     settings.background.load());
    xml.setAttribute ("animate",        settings.animate.load() ? 1 : 0);
    xml.setAttribute ("peakHold",       settings.peakHold.load() ? 1 : 0);
    xml.setAttribute ("tilt",           settings.tilt.load());
    xml.setAttribute ("speed",          settings.speed.load());
    xml.setAttribute ("range",          settings.range.load());
    xml.setAttribute ("stereoMode",     settings.stereoMode.load());
    xml.setAttribute ("fftSizeIndex",   settings.fftSizeIndex.load());
    xml.setAttribute ("showScope",      settings.showScope.load() ? 1 : 0);
    xml.setAttribute ("scopeTime",      settings.scopeTime.load());
    xml.setAttribute ("scopeFilter",    settings.scopeFilter.load());
    xml.setAttribute ("showLoudness",   settings.showLoudness.load() ? 1 : 0);
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
            settings.colourStereoA = juce::Colour::fromString (xml->getStringAttribute ("colourStereoA", "ff35e0c2")).getARGB();
            settings.colourB       = juce::Colour::fromString (xml->getStringAttribute ("colourB", "ffff4fb8")).getARGB();
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
            settings.showScope     = xml->getIntAttribute ("showScope", 0) != 0;
            settings.scopeTime     = xml->getIntAttribute ("scopeTime", 1);
            settings.scopeFilter   = xml->getIntAttribute ("scopeFilter", 1);
            settings.showLoudness  = xml->getIntAttribute ("showLoudness", 0) != 0;
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
