#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cstdint>

// Parte "de audio": recebe o som, guarda as ultimas amostras e deixa o som passar intacto.
class SpectrumAnalyzerProcessor : public juce::AudioProcessor
{
public:
    static constexpr int fftOrder = 12;            // 2^12 = 4096 amostras por analise
    static constexpr int fftSize  = 1 << fftOrder;

    SpectrumAnalyzerProcessor();
    ~SpectrumAnalyzerProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Spectrum Analyzer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // Guarda/le as definicoes visuais no projeto da DAW
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Usado pela interface para ler as ultimas amostras (da mais antiga para a mais recente)
    void copyLatestSamples (float* destination) const;
    double getCurrentSampleRate() const { return currentSampleRate.load(); }

    // Definicoes visuais (mudadas na interface, guardadas com o projeto)
    struct Settings
    {
        std::atomic<int>      colourMode    { 0 };            // 0 = cor unica, 1 = arco-iris
        std::atomic<uint32_t> singleColour  { 0xff35e0c2 };
        std::atomic<int>      fillStyle     { 0 };            // 0 = gradiente, 1 = solido, 2 = so linha
        std::atomic<float>    lineThickness { 1.6f };
        std::atomic<int>      background    { 0 };            // 0..3
        std::atomic<bool>     animate       { false };
        std::atomic<bool>     peakHold      { false };
        std::atomic<int>      tilt          { 0 };            // 0 = flat, 1 = 3 dB/oct, 2 = 4.5 dB/oct
        std::atomic<int>      speed         { 1 };            // 0 = fast, 1 = medium, 2 = slow
        std::atomic<int>      range         { 1 };            // 0 = 60 dB, 1 = 90 dB, 2 = 120 dB
    } settings;

private:
    std::array<std::atomic<float>, fftSize> ring;
    std::atomic<int> writeIndex { 0 };
    std::atomic<double> currentSampleRate { 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerProcessor)
};
