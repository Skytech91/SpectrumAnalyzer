#pragma once
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <functional>
#include <memory>
#include <vector>

// Row of colour circles: click one to pick a colour. Colours that are not allowed are crossed out.
class SwatchRow : public juce::Component
{
public:
    static const std::vector<juce::Colour>& palette()
    {
        static const std::vector<juce::Colour> colours = {
            juce::Colour (0xff35e0c2), juce::Colour (0xff3aa0ff), juce::Colour (0xff8a5cff),
            juce::Colour (0xffff4fb8), juce::Colour (0xffff4d4d), juce::Colour (0xffff9a3c),
            juce::Colour (0xffffd84a), juce::Colour (0xff7be04a), juce::Colour (0xffffffff),
            juce::Colour (0xff9aa5b1) };
        return colours;
    }

    std::function<void (juce::Colour)> onPick;

    void setSelectedFromColour (juce::Colour c)
    {
        selected = -1;
        for (size_t i = 0; i < palette().size(); ++i)
            if (palette()[i].getARGB() == c.getARGB())
                selected = (int) i;
        repaint();
    }

    void clearSelection()
    {
        selected = -1;
        repaint();
    }

    void setDisabledColours (std::vector<juce::Colour> colours)
    {
        disabled = std::move (colours);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        for (size_t i = 0; i < palette().size(); ++i)
        {
            const auto r = swatchBounds ((int) i);
            const bool off = isDisabled ((int) i);

            g.setColour (palette()[i].withAlpha (off ? 0.25f : 1.0f));
            g.fillEllipse (r);

            if (off)
            {
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.drawLine (r.getX() + 2.0f, r.getBottom() - 2.0f, r.getRight() - 2.0f, r.getY() + 2.0f, 1.2f);
            }

            if ((int) i == selected)
            {
                g.setColour (juce::Colours::white);
                g.drawEllipse (r.expanded (2.5f), 1.5f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (size_t i = 0; i < palette().size(); ++i)
        {
            if (swatchBounds ((int) i).expanded (2.0f).contains (e.position))
            {
                if (isDisabled ((int) i))
                    return;

                selected = (int) i;
                if (onPick)
                    onPick (palette()[i]);
                repaint();
                return;
            }
        }
    }

private:
    bool isDisabled (int i) const
    {
        for (const auto& d : disabled)
            if (d.getARGB() == palette()[(size_t) i].getARGB())
                return true;
        return false;
    }

    juce::Rectangle<float> swatchBounds (int i) const
    {
        const float slot = (float) getWidth() / (float) palette().size();
        const float d = juce::jmin (slot - 5.0f, (float) getHeight() - 6.0f);
        return { (float) i * slot + (slot - d) * 0.5f, ((float) getHeight() - d) * 0.5f, d, d };
    }

    std::vector<juce::Colour> disabled;
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

// Small rounded on/off button used in the header to show or hide a section
class PillToggle : public juce::Button
{
public:
    explicit PillToggle (const juce::String& text) : juce::Button (text)
    {
        setButtonText (text);
        setClickingTogglesState (true);
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();

        auto bg = on ? juce::Colour (0xff3a4654) : juce::Colour (0xff1b2128);
        if (isMouseOver)
            bg = bg.brighter (0.1f);

        g.setColour (bg);
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        g.setColour (juce::Colours::white.withAlpha (on ? 0.35f : 0.15f));
        g.drawRoundedRectangle (b, b.getHeight() * 0.5f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (on ? 0.95f : 0.5f));
        g.setFont (12.0f);
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
    }
};

// Visual part: spectrum analyzer, scope, loudness meters and the settings panel.
class SpectrumAnalyzerEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor&);
    ~SpectrumAnalyzerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int maxFftSize = SpectrumAnalyzerProcessor::maxFftSize;
    static constexpr int maxBins = maxFftSize / 2;
    using Spectrum = std::array<float, maxBins>;
    using ScopeBuffer = std::array<float, SpectrumAnalyzerProcessor::scopeRingSize / 2>;

    void timerCallback() override;
    void rebuildFft (int newSize);
    void updateSpectrum (const float* samples, Spectrum& smoothedOut, Spectrum& peaksOut,
                         float attack, float release, bool peaksOn);
    void updateScope();
    float levelAt (const Spectrum& data, float f0, float f1, float binHz) const;
    juce::ColourGradient makeHorizontalGradient (juce::Rectangle<float> area, float alpha) const;

    // layout (the analyzer, the scope and the meters share the window)
    juce::Rectangle<float> getContentBounds() const;
    juce::Rectangle<float> getMainArea() const;
    float getScopeHeight (juce::Rectangle<float> mainArea) const;
    juce::Rectangle<float> getAnalyzerArea() const;
    juce::Rectangle<float> getPlotBounds() const;
    juce::Rectangle<float> getScopeBounds() const;
    juce::Rectangle<float> getMetersBounds() const;
    juce::Rectangle<int> getResetRect() const;

    void drawAnalyzer (juce::Graphics& g);
    void drawScope (juce::Graphics& g, juce::Rectangle<float> area);
    void drawMeters (juce::Graphics& g, juce::Rectangle<float> area);

    void enforceDistinctColours();
    void updateSwatchSelection();
    void updatePanelVisibility();

    SpectrumAnalyzerProcessor& audioProcessor;

    // current analysis size (changes with the "FFT size" setting)
    int fftSize = 4096;
    int numBins = 2048;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;

    std::array<float, maxFftSize * 2> fftData {};
    std::array<float, maxFftSize> bufL {};
    std::array<float, maxFftSize> bufR {};

    // curve A = mix / left / mid, curve B = right / side
    Spectrum smoothedA {}, smoothedB {}, peaksA {}, peaksB {};
    int lastStereoMode = 0;

    // scope: input (bass) and sidechain (kick)
    ScopeBuffer scopeData {}, scopeDataSide {};
    int scopeCount = 0;
    int scopeFilled = 0;
    bool scopeSynced = false;
    bool scopeSideActive = false;
    juce::String scopeLabel;

    // header: show / hide sections
    PillToggle analyzerPill { juce::String ("Analyzer") };
    PillToggle scopePill    { juce::String ("Scope") };
    PillToggle metersPill   { juce::String ("Meters") };

    // settings panel
    GearButton settingsButton;
    juce::TextButton tabLook, tabAnalyzer, tabScope, tabMeter;
    int panelPage = 0;

    juce::ComboBox modeBox, fillBox, backgroundBox, tiltBox, speedBox, rangeBox, channelsBox, fftBox,
                   scopeTimeBox, scopeFilterBox, meterModeBox;
    SwatchRow swatches, swatchesStereoA, swatchesB, swatchesScopeA, swatchesScopeB, swatchesMeter;
    juce::Slider hueSlider, thicknessSlider;
    juce::ToggleButton animateToggle, peakToggle;
    juce::Rectangle<int> panelBounds, scopeHintRect;
    std::array<juce::Rectangle<int>, 16> labelRects;
    std::array<juce::String, 16> labelTexts;

    // legend (two-curve modes): click a name to hide / show that curve
    juce::Rectangle<int> legendA, legendB;
    bool showA = true, showB = true;

    float hueOffset = 0.0f;

    // mouse position
    juce::Point<float> hoverPos;
    bool hoverActive = false;

    // readout text (stabilised so it does not jitter)
    juce::String readoutLine1, readoutLine2;
    float readoutLevelA = -120.0f;
    float readoutLevelB = -120.0f;
    float lastReadoutX = -1.0f;
    int readoutTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzerEditor)
};
