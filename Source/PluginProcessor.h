#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// Simple second-order filter (used for the loudness K-weighting and the scope low-pass)
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = 0.0; z2 = 0.0; }
};

// Second-order low-pass filter (RBJ cookbook)
inline void designLowpassBiquad (Biquad& bq, double fs, double fc, double q)
{
    const double w0 = 2.0 * juce::MathConstants<double>::pi * fc / fs;
    const double cosw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;

    bq.b0 = (1.0 - cosw) * 0.5 / a0;
    bq.b1 = (1.0 - cosw) / a0;
    bq.b2 = bq.b0;
    bq.a1 = -2.0 * cosw / a0;
    bq.a2 = (1.0 - alpha) / a0;
    bq.reset();
}

// Transport information from the DAW (tempo, position), sampled at the start of each audio block
struct HostInfo
{
    bool valid = false;           // the host gave us a musical position
    bool playing = false;
    double ppqStart = 0.0;        // position in quarter notes at the start of the block
    double bpm = 120.0;
    int sigNum = 4;
    int sigDen = 4;
    int64_t absStart = 0;         // absolute sample count at the start of the block
};

// Audio part: receives the sound (and the sidechain), keeps the latest samples, measures loudness
// and lets the sound pass untouched.
class SpectrumAnalyzerProcessor : public juce::AudioProcessor
{
public:
    static constexpr int maxFftOrder = 14;                 // largest analysis: 2^14 = 16384 samples
    static constexpr int maxFftSize  = 1 << maxFftOrder;
    static constexpr int scopeRingSize = 1 << 17;          // scope history (decimated samples)

    static constexpr int numScopeFilters = 5;
    static constexpr float scopeCutoffs[numScopeFilters] = { 100.0f, 150.0f, 200.0f, 300.0f, 500.0f };

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

    // Spectrum: copies the latest `count` samples of a channel (0 = left, 1 = right), oldest first
    void copyLatestSamples (float* destination, int channel, int count) const;
    double getCurrentSampleRate() const { return currentSampleRate.load(); }

    // Scope: low-passed, decimated mono signals. channel 0 = input, 1 = sidechain; k = decimated index
    float readScopeSample (int channel, int64_t k) const
    {
        if (k < 0)
            return 0.0f;

        const auto& ring = (channel == 0) ? scopeRingMain : scopeRingSide;
        return ring[(size_t) (k & (scopeRingSize - 1))].load (std::memory_order_relaxed);
    }

    int64_t getAbsoluteWritePosition() const { return absWrite.load (std::memory_order_acquire); }
    int getScopeDecimation() const { return scopeDecimation.load(); }
    double getScopeRate() const { return currentSampleRate.load() / (double) scopeDecimation.load(); }
    bool isSidechainActive() const { return sidechainActive.load(); }

    HostInfo getHostInfo() const
    {
        const juce::SpinLock::ScopedLockType lock (hostLock);
        return hostInfo;
    }

    // Loudness (ITU-R BS.1770 / EBU R128), read by the interface
    std::atomic<float> loudnessMomentary  { -120.0f };    // LUFS, 400 ms
    std::atomic<float> loudnessShortTerm  { -120.0f };    // LUFS, 3 s
    std::atomic<float> loudnessIntegrated { -120.0f };    // LUFS, gated, since the last reset
    std::atomic<float> samplePeakDb       { -120.0f };    // dBFS, highest sample since the last reset
    std::atomic<bool>  resetLoudnessRequested { false };

    // Visual settings (changed in the interface, saved with the project)
    struct Settings
    {
        std::atomic<int>      colourMode      { 0 };            // 0 = single colour, 1 = rainbow (Mix mode)
        std::atomic<uint32_t> singleColour    { 0xff35e0c2 };   // colour of the Mix curve
        std::atomic<uint32_t> colourStereoA   { 0xff3aa0ff };   // colour of curve A (left / mid)
        std::atomic<uint32_t> colourB         { 0xffff4fb8 };   // colour of curve B (right / side)
        std::atomic<int>      fillStyle       { 0 };            // 0 = gradient, 1 = solid, 2 = line only
        std::atomic<float>    lineThickness   { 1.6f };
        std::atomic<int>      background      { 0 };            // 0..3
        std::atomic<bool>     animate         { false };
        std::atomic<bool>     peakHold        { false };
        std::atomic<int>      tilt            { 0 };            // 0 = flat, 1 = 3 dB/oct, 2 = 4.5 dB/oct
        std::atomic<int>      speed           { 1 };            // 0 = fast, 1 = medium, 2 = slow
        std::atomic<int>      range           { 1 };            // 0 = 60 dB, 1 = 90 dB, 2 = 120 dB
        std::atomic<int>      stereoMode      { 0 };            // 0 = mix, 1 = left/right, 2 = mid/side
        std::atomic<int>      fftSizeIndex    { 1 };            // 0..3 = 2048, 4096, 8192, 16384
        std::atomic<bool>     showAnalyzer    { true };
        std::atomic<bool>     showScope       { false };
        std::atomic<bool>     showLoudness    { false };
        std::atomic<int>      scopeTime       { 10 };           // index in the scope time list (10 = 1/4 note)
        std::atomic<int>      scopeFilter     { 2 };            // index in scopeCutoffs (2 = 200 Hz)
        std::atomic<uint32_t> scopeColour     { 0xff35e0c2 };   // scope: input (bass)
        std::atomic<uint32_t> scopeColourSide { 0xffff9a3c };   // scope: sidechain (kick)
        std::atomic<int>      meterMode       { 0 };            // 0 = gradient (green-yellow-red), 1 = solid colour
        std::atomic<uint32_t> meterColour     { 0xff7be04a };
    } settings;

private:
    void setupKFilter (double sampleRate);
    void resetLoudnessState();
    void finishSubBlock();

    std::array<std::atomic<float>, maxFftSize> ringL;
    std::array<std::atomic<float>, maxFftSize> ringR;
    std::array<std::atomic<float>, scopeRingSize> scopeRingMain;
    std::array<std::atomic<float>, scopeRingSize> scopeRingSide;

    std::atomic<int64_t> absWrite { 0 };                    // samples written so far
    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<int> scopeDecimation { 8 };
    std::atomic<bool> sidechainActive { false };

    // scope filters (audio thread only): input and sidechain, two cascaded stages each
    Biquad scopeLpMain[2], scopeLpSide[2];
    int appliedScopeFilter = -1;

    mutable juce::SpinLock hostLock;
    HostInfo hostInfo;

    // loudness measurement (audio thread only)
    static constexpr int histBins = 751;                    // -70.0 ... +5.0 LUFS in 0.1 steps
    Biquad kShelf[2], kHighPass[2];                         // K-weighting, one set per channel
    double subBlockSumSq = 0.0;                             // energy of the current 100 ms block
    int subBlockCount = 0;
    int subBlockLength = 4800;
    std::array<double, 30> subEnergies {};                  // last 3 s in 100 ms steps
    int subWrite = 0;
    int subFilled = 0;
    std::array<double, histBins> histEnergy {};
    std::array<double, histBins> histCount {};
    double peakLinear = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerProcessor)
};
