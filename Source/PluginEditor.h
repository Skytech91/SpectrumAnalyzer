#pragma once
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <functional>
#include <vector>

// Row of colour circles: click one to pick a colour
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

// Toggle button with a gear icon and the word "Settings"
class GearButton : public juce::Button
{
public:
    GearButton() : juce::Button ("Settings")
    {
        setClickingTogglesState (true);
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);

        auto bg = getToggleState() ? juce::Colour (0xff3a4654) : juce::Colour (0xff262d36);
        if (isMouseOver) bg = bg.brighter (0.12f);
        if (isButtonDown) bg = bg.brighter (0.1f);

        g.setColour (bg);
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawRoundedRectangle (b, 6.0f, 1.0f);

        // gear icon: body + 8 teeth + hole
        const float r = 8.0f;
        const float cx = b.getX() + 12.0f + r;
        const float cy = b.getCentreY();

        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.fillEllipse (cx - r * 0.72f, cy - r * 0.72f, r * 1.44f, r * 1.44f);

        for (int i = 0; i < 8; ++i)
        {
            juce::Path tooth;
            tooth.addRectangle (-r * 0.2f, -r, r * 0.4f, r * 0.5f);
            g.fillPath (tooth, juce::AffineTransform::rotation ((float) i * juce::MathConstants<float>::pi / 4.0f)
                                   .translated (cx, cy));
        }

        g.setColour (bg);
        g.fillEllipse (cx - r * 0.3f, cy - r * 0.3f, r * 0.6f, r * 0.6f);

        // text
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (13.0f);
        g.drawText ("Settings", getLocalBounds().withTrimmedLeft (40), juce::Justification::centredLeft);
    }
};

// Visual part: does the FFT, draws the spectrum and holds the settings panel.
class SpectrumAnalyzerEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor&);
    ~SpectrumAnalyzerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // hover readout
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    static constexpr int fftSize = SpectrumAnalyzerProcessor::fftSize;
    static constexpr int numBins = fftSize / 2;

    void timerCallback() override;
    float levelAt (const std::array<float, numBins>& data, float f0, float f1, float binHz) const;
    juce::ColourGradient makeHorizontalGradient (juce::Rectangle<float> plot, float alpha) const;
    juce::Rectangle<float> getPlotBounds() const;
    void updateSwatchSelection();
    void setPanelVisible (bool shouldBeVisible);

    SpectrumAnalyzerProcessor& audioProcessor;
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::array<float, fftSize * 2> fftData {};
    std::array<float, numBins> smoothed {};
    std::array<float, numBins> peaks {};

    // settings panel
    GearButton settingsButton;
    juce::ComboBox modeBox, fillBox, backgroundBox, tiltBox, speedBox, rangeBox;
    SwatchRow swatches;
    juce::Slider hueSlider, thicknessSlider;
    juce::ToggleButton animateToggle, peakToggle;
    juce::Rectangle<int> panelBounds;
    std::array<juce::Rectangle<int>, 8> labelRects;
    std::array<juce::String, 8> labelTexts;

    float hueOffset = 0.0f;

    // mouse position
    juce::Point<float> hoverPos;
    bool hoverActive = false;

    // readout text (stabilised so it does not jitter)
    juce::String readoutLine1, readoutLine2;
    float readoutLevel = -120.0f;
    float lastReadoutX = -1.0f;
    int readoutTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerEditor)
};
