#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float minFreq = 20.0f;
    constexpr float maxFreq = 20000.0f;
    constexpr float minDb   = -90.0f;
    constexpr float maxDb   = 0.0f;
}

SpectrumAnalyzerEditor::SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      fft (SpectrumAnalyzerProcessor::fftOrder),
      window (fftSize, juce::dsp::WindowingFunction<float>::hann)
{
    smoothed.fill (minDb);

    setResizable (true, true);
    setResizeLimits (400, 250, 1600, 900);
    setSize (720, 400);

    startTimerHz (30);   // atualiza 30 vezes por segundo
}

SpectrumAnalyzerEditor::~SpectrumAnalyzerEditor()
{
    stopTimer();
}

void SpectrumAnalyzerEditor::timerCallback()
{
    // 1. buscar as ultimas amostras de audio
    audioProcessor.copyLatestSamples (fftData.data());
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    // 2. aplicar a janela (Hann) e fazer a FFT
    window.multiplyWithWindowingTable (fftData.data(), fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    // 3. converter para dB e suavizar (sobe rapido, desce devagar)
    for (int i = 0; i < numBins; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] * 4.0f / (float) fftSize, minDb);
        float& s = smoothed[(size_t) i];
        s += (db - s) * (db > s ? 0.5f : 0.12f);
    }

    repaint();
}

float SpectrumAnalyzerEditor::levelBetween (float f0, float f1, float binHz) const
{
    const float b0 = f0 / binHz;
    const float b1 = f1 / binHz;

    if (b1 - b0 < 1.0f)
    {
        // zona dos graves: varios pixeis por cada "bin" -> interpolar
        const float b = juce::jlimit (0.0f, (float) (numBins - 2), 0.5f * (b0 + b1));
        const int i = (int) b;
        const float frac = b - (float) i;
        return smoothed[(size_t) i] * (1.0f - frac) + smoothed[(size_t) i + 1] * frac;
    }

    // zona dos agudos: varios "bins" por pixel -> ficar com o maior
    const int lo = juce::jlimit (0, numBins - 1, (int) std::ceil (b0));
    const int hi = juce::jlimit (0, numBins - 1, (int) std::floor (b1));

    float m = minDb;
    for (int i = lo; i <= hi; ++i)
        m = std::max (m, smoothed[(size_t) i]);
    return m;
}

void SpectrumAnalyzerEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101418));

    auto plot = getLocalBounds().toFloat().reduced (10.0f);
    plot.removeFromLeft (30.0f);
    plot.removeFromBottom (18.0f);

    const auto xForFreq = [&] (float f)
    {
        return plot.getX() + plot.getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq);
    };
    const auto yForDb = [&] (float db)
    {
        return plot.getBottom() - plot.getHeight() * (db - minDb) / (maxDb - minDb);
    };

    // grelha de frequencias
    const float freqs[]       = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    const char* freqLabels[]  = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };
    g.setFont (11.0f);
    for (int i = 0; i < 10; ++i)
    {
        const float x = xForFreq (freqs[i]);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawText (freqLabels[i], juce::Rectangle<int> ((int) x - 20, (int) plot.getBottom() + 3, 40, 14),
                    juce::Justification::centred);
    }

    // grelha de dB
    for (int db = 0; db >= -80; db -= 20)
    {
        const float y = yForDb ((float) db);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawText (juce::String (db), juce::Rectangle<int> ((int) plot.getX() - 34, (int) y - 7, 30, 14),
                    juce::Justification::centredRight);
    }

    // curva do espetro
    const int width = (int) plot.getWidth();
    if (width < 2)
        return;

    const float binHz = (float) (audioProcessor.getCurrentSampleRate() / (double) fftSize);
    const float ratio = maxFreq / minFreq;

    juce::Path curve;
    for (int px = 0; px <= width; ++px)
    {
        const float f0 = minFreq * std::pow (ratio, (float) px / (float) width);
        const float f1 = minFreq * std::pow (ratio, (float) (px + 1) / (float) width);
        const float x = plot.getX() + (float) px;
        const float y = yForDb (levelBetween (f0, f1, binHz));

        if (px == 0) curve.startNewSubPath (x, y);
        else         curve.lineTo (x, y);
    }

    juce::Path filled (curve);
    filled.lineTo (plot.getRight(), plot.getBottom());
    filled.lineTo (plot.getX(), plot.getBottom());
    filled.closeSubPath();

    const auto colour = juce::Colour (0xff35e0c2);

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (plot.toNearestInt());

    g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.55f), 0.0f, plot.getY(),
                                             colour.withAlpha (0.02f), 0.0f, plot.getBottom(), false));
    g.fillPath (filled);

    g.setColour (colour);
    g.strokePath (curve, juce::PathStrokeType (1.6f));
}
