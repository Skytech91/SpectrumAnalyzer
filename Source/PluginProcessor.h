#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cstdint>

// Audio part: receives the sound, keeps the latest left/right samples and lets the sound pass untouched.
class SpectrumAnalyzerProcessor : public juce::AudioProcessor
{
public:
    static constexpr int maxFftOrder = 14;                 // largest analysis: 2^14 = 16384 samples
    static constexpr int maxFftSize  = 1 << maxFftOrder;

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

    // Saves/loads the visual settings with the DAW project
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Used by the interface: copies the latest `count` samples of a channel
    // (0 = left, 1 = right), from oldest to newest
    void copyLatestSamples (float* destination, int channel, int count) const;
    double getCurrentSampleRate() const { return currentSampleRate.load(); }

    // Visual settings (changed in the interface, saved with the project)
    struct Settings
    {
        std::atomic<int>      colourMode    { 0 };            // 0 = single colour, 1 = rainbow
        std::atomic<uint32_t> singleColour  { 0xff35e0c2 };
        std::atomic<uint32_t> colourB       { 0xffff4fb8 };   // colour of curve B (right / side)
        std::atomic<int>      fillStyle     { 0 };            // 0 = gradient, 1 = solid, 2 = line only
        std::atomic<float>    lineThickness { 1.6f };
        std::atomic<int>      background    { 0 };            // 0..3
        std::atomic<bool>     animate       { false };
        std::atomic<bool>     peakHold      { false };
        std::atomic<int>      tilt          { 0 };            // 0 = flat, 1 = 3 dB/oct, 2 = 4.5 dB/oct
        std::atomic<int>      speed         { 1 };            // 0 = fast, 1 = medium, 2 = slow
        std::atomic<int>      range         { 1 };            // 0 = 60 dB, 1 = 90 dB, 2 = 120 dB
        std::atomic<int>      stereoMode    { 0 };            // 0 = mix, 1 = left/right, 2 = mid/side
        std::atomic<int>      fftSizeIndex  { 1 };            // 0..3 = 2048, 4096, 8192, 16384
    } settings;

private:
    std::array<std::atomic<float>, maxFftSize> ringL;
    std::array<std::atomic<float>, maxFftSize> ringR;
    std::atomic<int> writeIndex { 0 };
    std::atomic<double> currentSampleRate { 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerProcessor)
};
