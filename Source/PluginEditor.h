#pragma once
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <functional>
#include <vector>

// Fila de circulos de cor onde se clica para escolher uma cor
class SwatchRow : public juce::Component
{
public:
    SwatchRow()
    {
        colours = { juce::Colour (0xff35e0c2), juce::Colour (0xff3aa0ff), juce::Colour (0xff8a5cff),
                    juce::Colour (0xffff4fb8), juce::Colour (0xffff4d4d), juce::Colour (0xffff9a3c),
                    juce::Colour (0xffffd84a), juce::Colour (0xff7be04a), juce::Colour (0xffffffff),
                    juce::Colour (0xff9aa5b1) };
    }

    std::function<void (juce::Colour)> onPick;

    void setSelectedFromColour (juce::Colour c)
    {
        selected = -1;
        for (size_t i = 0; i < colours.size(); ++i)
            if (colours[i].getARGB() == c.getARGB())
                selected = (int) i;
        repaint();
    }

    void clearSelection()
    {
        selected = -1;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        for (size_t i = 0; i < colours.size(); ++i)
        {
            const auto r = swatchBounds ((int) i);
            g.setColour (colours[i]);
            g.fillEllipse (r);

            if ((int) i == selected)
            {
                g.setColour (juce::Colours::white);
                g.drawEllipse (r.expanded (2.5f), 1.5f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (size_t i = 0; i < colours.size(); ++i)
        {
            if (swatchBounds ((int) i).expanded (2.0f).contains (e.position))
            {
                selected = (int) i;
                if (onPick)
                    onPick (colours[i]);
                repaint();
                return;
            }
        }
    }

private:
    juce::Rectangle<float> swatchBounds (int i) const
    {
        const float slot = (float) getWidth() / (float) colours.size();
        const float d = juce::jmin (slot - 5.0f, (float) getHeight() - 6.0f);
        return { (float) i * slot + (slot - d) * 0.5f, ((float) getHeight() - d) * 0.5f, d, d };
    }

    std::vector<juce::Colour> colours;
    int selected = -1;
};

// Parte "visual": faz a FFT, desenha o espetro e tem o painel de definicoes.
class SpectrumAnalyzerEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor&);
    ~SpectrumAnalyzerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // leitura ao passar o rato
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    static constexpr int fftSize = SpectrumAnalyzerProcessor::fftSize;
    static constexpr int numBins = fftSize / 2;

    void timerCallback() override;
    float levelBetween (float f0, float f1, float binHz) const;
    juce::ColourGradient makeHorizontalGradient (juce::Rectangle<float> plot, float alpha) const;
    juce::Rectangle<float> getPlotBounds() const;
    void updateSwatchSelection();
    void setPanelVisible (bool shouldBeVisible);

    SpectrumAnalyzerProcessor& audioProcessor;
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::array<float, fftSize * 2> fftData {};
    std::array<float, numBins> smoothed {};

    // painel de definicoes
    juce::TextButton settingsButton;
    juce::ComboBox modeBox, fillBox, backgroundBox;
    SwatchRow swatches;
    juce::Slider hueSlider, thicknessSlider;
    juce::ToggleButton animateToggle;
    juce::Rectangle<int> panelBounds;
    std::array<juce::Rectangle<int>, 5> labelRects;
    std::array<juce::String, 5> labelTexts;

    float hueOffset = 0.0f;

    // posicao do rato
    juce::Point<float> hoverPos;
    bool hoverActive = false;

    // texto da caixinha de leitura (estabilizado para nao tremer)
    juce::String readoutLine1, readoutLine2;
    float readoutLevel = -90.0f;
    float lastReadoutX = -1.0f;
    int readoutTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerEditor)
};
