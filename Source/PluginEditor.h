#pragma once
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>

// Parte "visual": faz a FFT e desenha o espetro.
class SpectrumAnalyzerEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor&);
    ~SpectrumAnalyzerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override {}

private:
    static constexpr int fftSize = SpectrumAnalyzerProcessor::fftSize;
    static constexpr int numBins = fftSize / 2;

    void timerCallback() override;
    float levelBetween (float f0, float f1, float binHz) const;

    SpectrumAnalyzerProcessor& audioProcessor;
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::array<float, fftSize * 2> fftData {};
    std::array<float, numBins> smoothed {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerEditor)
};
