#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float minFreq = 20.0f;
    constexpr float maxFreq = 20000.0f;
    constexpr float floorDb = -120.0f;   // lowest level the analysis keeps
    constexpr float maxDb   = 0.0f;

    constexpr float attackTable[]  = { 0.8f, 0.5f, 0.3f };    // fast, medium, slow
    constexpr float releaseTable[] = { 0.3f, 0.12f, 0.05f };
    constexpr float tiltTable[]    = { 0.0f, 3.0f, 4.5f };    // dB per octave
    constexpr float rangeTable[]   = { -60.0f, -90.0f, -120.0f };
    constexpr float peakFallDb     = 0.3f;                    // per frame (about 9 dB/s)

    // scope window: a fixed time, or a note length that follows the tempo of the DAW
    // (quarters < 0 means "bars": -1 = one bar, -2 = two bars)
    struct ScopeOption { const char* label; double quarters; double milliseconds; };

    constexpr ScopeOption scopeOptions[] = {
        { "10 ms",  0.0, 10.0 },  { "20 ms", 0.0, 20.0 },  { "50 ms", 0.0, 50.0 },  { "100 ms", 0.0, 100.0 },
        { "1/32",   0.125, 0.0 }, { "1/16T", 1.0 / 6.0, 0.0 }, { "1/16", 0.25, 0.0 },
        { "1/8T",   1.0 / 3.0, 0.0 }, { "1/8", 0.5, 0.0 }, { "1/4T", 2.0 / 3.0, 0.0 },
        { "1/4",    1.0, 0.0 },   { "1/2", 2.0, 0.0 },     { "1 bar", -1.0, 0.0 }, { "2 bars", -2.0, 0.0 }
    };
    constexpr int numScopeOptions = 14;

    constexpr float meterColumnWidth = 230.0f;
    constexpr float meterMinDb = -48.0f;

    const juce::Colour backgrounds[] = { juce::Colour (0xff101418),   // graphite
                                         juce::Colour (0xff000000),   // black
                                         juce::Colour (0xff0b1226),   // midnight blue
                                         juce::Colour (0xff170f26) }; // dark purple

    juce::String frequencyText (float f)
    {
        if (f >= 1000.0f) return juce::String (f / 1000.0f, 2) + " kHz";
        if (f >= 100.0f)  return juce::String ((int) std::lround (f)) + " Hz";
        return juce::String (f, 1) + " Hz";
    }

    // nearest musical note (A4 = 440 Hz)
    juce::String noteName (float freq)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int midi = (int) std::lround (69.0 + 12.0 * std::log2 ((double) freq / 440.0));
        const int index = ((midi % 12) + 12) % 12;
        const int octave = midi / 12 - 1;
        return juce::String (names[index]) + juce::String (octave);
    }

    // colour of a meter segment: green (low) -> yellow -> orange -> red (0 dB, clipping)
    juce::Colour colourForDb (float db)
    {
        struct Stop { float db; juce::uint32 argb; };
        static const Stop stops[] = { { -48.0f, 0xff1fc86e }, { -24.0f, 0xff63d84a }, { -14.0f, 0xffc4e04a },
                                      { -9.0f,  0xffffd84a }, { -5.0f,  0xffff9a3c }, { -2.0f,  0xffff5a3a },
                                      { -0.5f,  0xffff2d2d } };

        if (db <= stops[0].db)
            return juce::Colour (stops[0].argb);

        for (int i = 1; i < 7; ++i)
        {
            if (db <= stops[i].db)
            {
                const float t = (db - stops[i - 1].db) / (stops[i].db - stops[i - 1].db);
                return juce::Colour (stops[i - 1].argb).interpolatedWith (juce::Colour (stops[i].argb), t);
            }
        }

        return juce::Colour (stops[6].argb);
    }

    juce::String meterText (float value)
    {
        if (value <= -70.0f)
            return "-inf";

        return juce::String (value, 1);
    }
}

SpectrumAnalyzerEditor::SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p)
{
    auto& s = audioProcessor.settings;

    rebuildFft (2048 << juce::jlimit (0, 3, s.fftSizeIndex.load()));
    lastStereoMode = juce::jlimit (0, 2, s.stereoMode.load());

    labelTexts = { juce::String ("Color mode"), juce::String ("Color (Mix)"),
                   juce::String ("Stereo colors (L / Mid  -  R / Side)"),
                   juce::String ("Fill"), juce::String ("Line thickness"), juce::String ("Background"),
                   juce::String ("Tilt"), juce::String ("Speed"), juce::String ("Range"),
                   juce::String ("Channels"), juce::String ("FFT size"),
                   juce::String ("Scope window"), juce::String ("Scope filter"),
                   juce::String ("Scope colors (input / sidechain)"),
                   juce::String ("Meter style"), juce::String ("Meter color (solid)") };

    // header: show / hide sections
    analyzerPill.setToggleState (s.showAnalyzer.load(), juce::dontSendNotification);
    analyzerPill.onClick = [this]
    {
        audioProcessor.settings.showAnalyzer = analyzerPill.getToggleState();
        hoverActive = false;
        repaint();
    };
    addAndMakeVisible (analyzerPill);

    scopePill.setToggleState (s.showScope.load(), juce::dontSendNotification);
    scopePill.onClick = [this] { audioProcessor.settings.showScope = scopePill.getToggleState(); repaint(); };
    addAndMakeVisible (scopePill);

    metersPill.setToggleState (s.showLoudness.load(), juce::dontSendNotification);
    metersPill.onClick = [this] { audioProcessor.settings.showLoudness = metersPill.getToggleState(); repaint(); };
    addAndMakeVisible (metersPill);

    // "Settings" button with gear icon
    settingsButton.onClick = [this] { updatePanelVisibility(); };
    addAndMakeVisible (settingsButton);

    // tabs inside the settings panel
    tabLook.setButtonText ("Look");
    tabAnalyzer.setButtonText ("Analyzer");
    tabScope.setButtonText ("Scope");
    tabMeter.setButtonText ("Meter");

    for (auto* tab : { &tabLook, &tabAnalyzer, &tabScope, &tabMeter })
    {
        tab->setClickingTogglesState (true);
        tab->setRadioGroupId (101);
        tab->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff262d36));
        tab->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff4a5868));
        addChildComponent (*tab);
    }

    tabLook.setToggleState (true, juce::dontSendNotification);
    tabLook.onClick     = [this] { panelPage = 0; updatePanelVisibility(); };
    tabAnalyzer.onClick = [this] { panelPage = 1; updatePanelVisibility(); };
    tabScope.onClick    = [this] { panelPage = 2; updatePanelVisibility(); };
    tabMeter.onClick    = [this] { panelPage = 3; updatePanelVisibility(); };

    // ---- Look: colour of the spectrum (Mix mode): single colour or rainbow
    modeBox.addItem ("Single color", 1);
    modeBox.addItem ("Rainbow", 2);
    modeBox.setSelectedId (s.colourMode.load() + 1, juce::dontSendNotification);
    modeBox.onChange = [this]
    {
        audioProcessor.settings.colourMode = modeBox.getSelectedId() - 1;
        updateSwatchSelection();
        enforceDistinctColours();
    };
    addChildComponent (modeBox);

    swatches.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.singleColour = c.getARGB();
        hueSlider.setValue ((double) c.getHue(), juce::dontSendNotification);
        modeBox.setSelectedId (1, juce::sendNotification);
        enforceDistinctColours();
    };
    addChildComponent (swatches);

    // any colour for Mix, using hue (the rainbow bar is drawn behind the slider)
    hueSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    hueSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    hueSlider.setRange (0.0, 1.0, 0.001);
    hueSlider.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
    hueSlider.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
    hueSlider.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    hueSlider.setValue ((double) juce::Colour (s.singleColour.load()).getHue(), juce::dontSendNotification);
    hueSlider.onValueChange = [this]
    {
        audioProcessor.settings.singleColour =
            juce::Colour::fromHSV ((float) hueSlider.getValue(), 0.75f, 1.0f, 1.0f).getARGB();
        swatches.clearSelection();
        modeBox.setSelectedId (1, juce::sendNotification);
        enforceDistinctColours();
    };
    addChildComponent (hueSlider);

    // ---- Look: colours of the two-curve modes (Left/Right and Mid/Side share this pair)
    swatchesStereoA.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.colourStereoA = c.getARGB();
        enforceDistinctColours();
    };
    addChildComponent (swatchesStereoA);

    swatchesB.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.colourB = c.getARGB();
        enforceDistinctColours();
    };
    addChildComponent (swatchesB);

    // animate rainbow
    animateToggle.setButtonText ("Animate rainbow");
    animateToggle.setToggleState (s.animate.load(), juce::dontSendNotification);
    animateToggle.onClick = [this] { audioProcessor.settings.animate = animateToggle.getToggleState(); };
    addChildComponent (animateToggle);

    // fill style
    fillBox.addItem ("Gradient", 1);
    fillBox.addItem ("Solid", 2);
    fillBox.addItem ("Line only", 3);
    fillBox.setSelectedId (s.fillStyle.load() + 1, juce::dontSendNotification);
    fillBox.onChange = [this] { audioProcessor.settings.fillStyle = fillBox.getSelectedId() - 1; };
    addChildComponent (fillBox);

    // line thickness
    thicknessSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    thicknessSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    thicknessSlider.setRange (0.5, 4.0, 0.1);
    thicknessSlider.setColour (juce::Slider::backgroundColourId, juce::Colours::white.withAlpha (0.25f));
    thicknessSlider.setColour (juce::Slider::trackColourId, juce::Colours::white.withAlpha (0.85f));
    thicknessSlider.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    thicknessSlider.setValue ((double) s.lineThickness.load(), juce::dontSendNotification);
    thicknessSlider.onValueChange = [this]
    {
        audioProcessor.settings.lineThickness = (float) thicknessSlider.getValue();
    };
    addChildComponent (thicknessSlider);

    // background
    backgroundBox.addItem ("Graphite", 1);
    backgroundBox.addItem ("Black", 2);
    backgroundBox.addItem ("Midnight blue", 3);
    backgroundBox.addItem ("Dark purple", 4);
    backgroundBox.setSelectedId (s.background.load() + 1, juce::dontSendNotification);
    backgroundBox.onChange = [this] { audioProcessor.settings.background = backgroundBox.getSelectedId() - 1; };
    addChildComponent (backgroundBox);

    // ---- Analyzer page
    peakToggle.setButtonText ("Peak hold");
    peakToggle.setToggleState (s.peakHold.load(), juce::dontSendNotification);
    peakToggle.onClick = [this] { audioProcessor.settings.peakHold = peakToggle.getToggleState(); };
    addChildComponent (peakToggle);

    tiltBox.addItem ("Flat (0 dB/oct)", 1);
    tiltBox.addItem ("3 dB/oct", 2);
    tiltBox.addItem ("4.5 dB/oct", 3);
    tiltBox.setSelectedId (s.tilt.load() + 1, juce::dontSendNotification);
    tiltBox.onChange = [this] { audioProcessor.settings.tilt = tiltBox.getSelectedId() - 1; };
    addChildComponent (tiltBox);

    speedBox.addItem ("Fast", 1);
    speedBox.addItem ("Medium", 2);
    speedBox.addItem ("Slow", 3);
    speedBox.setSelectedId (s.speed.load() + 1, juce::dontSendNotification);
    speedBox.onChange = [this] { audioProcessor.settings.speed = speedBox.getSelectedId() - 1; };
    addChildComponent (speedBox);

    rangeBox.addItem ("60 dB", 1);
    rangeBox.addItem ("90 dB", 2);
    rangeBox.addItem ("120 dB", 3);
    rangeBox.setSelectedId (s.range.load() + 1, juce::dontSendNotification);
    rangeBox.onChange = [this] { audioProcessor.settings.range = rangeBox.getSelectedId() - 1; };
    addChildComponent (rangeBox);

    channelsBox.addItem ("Mix (mono)", 1);
    channelsBox.addItem ("Left / Right", 2);
    channelsBox.addItem ("Mid / Side", 3);
    channelsBox.setSelectedId (s.stereoMode.load() + 1, juce::dontSendNotification);
    channelsBox.onChange = [this] { audioProcessor.settings.stereoMode = channelsBox.getSelectedId() - 1; };
    addChildComponent (channelsBox);

    fftBox.addItem ("2048", 1);
    fftBox.addItem ("4096", 2);
    fftBox.addItem ("8192", 3);
    fftBox.addItem ("16384", 4);
    fftBox.setSelectedId (s.fftSizeIndex.load() + 1, juce::dontSendNotification);
    fftBox.onChange = [this] { audioProcessor.settings.fftSizeIndex = fftBox.getSelectedId() - 1; };
    addChildComponent (fftBox);

    // ---- Scope page: window (time or note length), filter, solid colours only
    for (int i = 0; i < numScopeOptions; ++i)
        scopeTimeBox.addItem (scopeOptions[i].label, i + 1);
    scopeTimeBox.setSelectedId (juce::jlimit (0, numScopeOptions - 1, s.scopeTime.load()) + 1, juce::dontSendNotification);
    scopeTimeBox.onChange = [this] { audioProcessor.settings.scopeTime = scopeTimeBox.getSelectedId() - 1; };
    addChildComponent (scopeTimeBox);

    for (int i = 0; i < SpectrumAnalyzerProcessor::numScopeFilters; ++i)
        scopeFilterBox.addItem ("Low-pass " + juce::String ((int) SpectrumAnalyzerProcessor::scopeCutoffs[i]) + " Hz", i + 1);
    scopeFilterBox.setSelectedId (juce::jlimit (0, SpectrumAnalyzerProcessor::numScopeFilters - 1, s.scopeFilter.load()) + 1,
                                  juce::dontSendNotification);
    scopeFilterBox.onChange = [this] { audioProcessor.settings.scopeFilter = scopeFilterBox.getSelectedId() - 1; };
    addChildComponent (scopeFilterBox);

    swatchesScopeA.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.scopeColour = c.getARGB();
        enforceDistinctColours();
    };
    addChildComponent (swatchesScopeA);

    swatchesScopeB.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.scopeColourSide = c.getARGB();
        enforceDistinctColours();
    };
    addChildComponent (swatchesScopeB);

    // ---- Meter page: gradient or solid colour
    meterModeBox.addItem ("Gradient (green - yellow - red)", 1);
    meterModeBox.addItem ("Solid color", 2);
    meterModeBox.setSelectedId (s.meterMode.load() + 1, juce::dontSendNotification);
    meterModeBox.onChange = [this] { audioProcessor.settings.meterMode = meterModeBox.getSelectedId() - 1; };
    addChildComponent (meterModeBox);

    swatchesMeter.setSelectedFromColour (juce::Colour (s.meterColour.load()));
    swatchesMeter.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.meterColour = c.getARGB();
        meterModeBox.setSelectedId (2, juce::sendNotification);
    };
    addChildComponent (swatchesMeter);

    updateSwatchSelection();
    enforceDistinctColours();
    updatePanelVisibility();

    setResizable (true, true);
    setResizeLimits (700, 480, 1800, 1100);
    setSize (880, 600);

    startTimerHz (30);   // refresh 30 times per second
}

SpectrumAnalyzerEditor::~SpectrumAnalyzerEditor()
{
    stopTimer();
}

// Sets up the FFT for a new analysis size and clears the curves
void SpectrumAnalyzerEditor::rebuildFft (int newSize)
{
    fftSize = newSize;
    numBins = newSize / 2;

    int order = 0;
    while ((1 << order) < newSize)
        ++order;

    fft = std::make_unique<juce::dsp::FFT> (order);
    // normalise = false: a full-scale sine wave then reads exactly 0 dB
    window = std::make_unique<juce::dsp::WindowingFunction<float>> ((size_t) newSize,
                                                                    juce::dsp::WindowingFunction<float>::hann,
                                                                    false);

    smoothedA.fill (floorDb);
    smoothedB.fill (floorDb);
    peaksA.fill (floorDb);
    peaksB.fill (floorDb);
    lastReadoutX = -1.0f;
}

// In rainbow mode no Mix colour circle stays selected
void SpectrumAnalyzerEditor::updateSwatchSelection()
{
    if (audioProcessor.settings.colourMode.load() == 1)
        swatches.clearSelection();
    else
        swatches.setSelectedFromColour (juce::Colour (audioProcessor.settings.singleColour.load()));
}

// Colour rules:
//  - the Left/Right and Mid/Side colours can never be the same as the solid colour of the spectrum (Mix),
//    nor the same as each other;
//  - the two scope colours (input and sidechain) can never be the same.
// A colour that is not allowed is crossed out in the swatches, and changed automatically if needed.
void SpectrumAnalyzerEditor::enforceDistinctColours()
{
    auto& s = audioProcessor.settings;
    const auto& palette = SwatchRow::palette();

    auto sameColour = [] (juce::Colour a, juce::Colour b) { return a.getARGB() == b.getARGB(); };

    // keeps `wanted` if it is allowed, otherwise the first allowed colour of the palette
    auto pick = [&] (juce::Colour wanted, const std::vector<juce::Colour>& forbidden)
    {
        auto isForbidden = [&] (juce::Colour c)
        {
            for (const auto& f : forbidden)
                if (sameColour (f, c))
                    return true;
            return false;
        };

        if (! isForbidden (wanted))
            return wanted;

        for (const auto& c : palette)
            if (! isForbidden (c))
                return c;

        return wanted;
    };

    // spectrum colour (solid) versus the two-curve colours
    const bool mixSolid = (s.colourMode.load() == 0);
    const juce::Colour mixColour (s.singleColour.load());

    std::vector<juce::Colour> forbiddenA, forbiddenB;
    if (mixSolid)
    {
        forbiddenA.push_back (mixColour);
        forbiddenB.push_back (mixColour);
    }

    const juce::Colour a = pick (juce::Colour (s.colourStereoA.load()), forbiddenA);
    forbiddenB.push_back (a);
    const juce::Colour b = pick (juce::Colour (s.colourB.load()), forbiddenB);

    s.colourStereoA = a.getARGB();
    s.colourB = b.getARGB();

    std::vector<juce::Colour> disabledForA, disabledForB;
    if (mixSolid)
    {
        disabledForA.push_back (mixColour);
        disabledForB.push_back (mixColour);
    }
    disabledForA.push_back (b);
    disabledForB.push_back (a);

    swatchesStereoA.setDisabledColours (disabledForA);
    swatchesB.setDisabledColours (disabledForB);
    swatchesStereoA.setSelectedFromColour (a);
    swatchesB.setSelectedFromColour (b);

    // scope colours
    const juce::Colour scopeInput (s.scopeColour.load());
    const juce::Colour scopeSide = pick (juce::Colour (s.scopeColourSide.load()), { scopeInput });
    s.scopeColourSide = scopeSide.getARGB();

    swatchesScopeA.setDisabledColours ({ scopeSide });
    swatchesScopeB.setDisabledColours ({ scopeInput });
    swatchesScopeA.setSelectedFromColour (scopeInput);
    swatchesScopeB.setSelectedFromColour (scopeSide);
}

// Shows the controls of the current page of the settings panel (or hides everything)
void SpectrumAnalyzerEditor::updatePanelVisibility()
{
    const bool open = settingsButton.getToggleState();

    auto show = [] (std::initializer_list<juce::Component*> list, bool visible)
    {
        for (auto* c : list)
            c->setVisible (visible);
    };

    show ({ &tabLook, &tabAnalyzer, &tabScope, &tabMeter }, open);

    show ({ &modeBox, &swatches, &hueSlider, &swatchesStereoA, &swatchesB,
            &animateToggle, &fillBox, &thicknessSlider, &backgroundBox }, open && panelPage == 0);

    show ({ &peakToggle, &tiltBox, &speedBox, &rangeBox, &channelsBox, &fftBox }, open && panelPage == 1);

    show ({ &scopeTimeBox, &scopeFilterBox, &swatchesScopeA, &swatchesScopeB }, open && panelPage == 2);

    show ({ &meterModeBox, &swatchesMeter }, open && panelPage == 3);

    resized();
    repaint();
}

// ---------------------------------------------------------------- layout

juce::Rectangle<float> SpectrumAnalyzerEditor::getContentBounds() const
{
    auto bounds = getLocalBounds().toFloat();
    bounds.removeFromTop (32.0f);
    return bounds;
}

// everything except the column of meters
juce::Rectangle<float> SpectrumAnalyzerEditor::getMainArea() const
{
    auto area = getContentBounds();

    if (audioProcessor.settings.showLoudness.load())
        area.removeFromRight (meterColumnWidth);

    return area;
}

float SpectrumAnalyzerEditor::getScopeHeight (juce::Rectangle<float> mainArea) const
{
    return juce::jlimit (120.0f, juce::jmax (120.0f, mainArea.getHeight() - 150.0f), mainArea.getHeight() * 0.38f);
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getAnalyzerArea() const
{
    const auto& s = audioProcessor.settings;

    if (! s.showAnalyzer.load())
        return {};

    auto area = getMainArea();

    if (s.showScope.load())
        area.removeFromBottom (getScopeHeight (area));

    return area;
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getPlotBounds() const
{
    const auto area = getAnalyzerArea();

    if (area.isEmpty())
        return {};

    auto plot = area.reduced (10.0f);
    plot.removeFromLeft (30.0f);
    plot.removeFromBottom (18.0f);
    return plot;
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getScopeBounds() const
{
    const auto& s = audioProcessor.settings;

    if (! s.showScope.load())
        return {};

    auto area = getMainArea();

    if (s.showAnalyzer.load())
        area = area.removeFromBottom (getScopeHeight (area));

    auto bounds = area.reduced (10.0f, 6.0f);

    if (s.showAnalyzer.load())
        bounds.removeFromLeft (30.0f);     // lines up with the analyzer plot

    return bounds;
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getMetersBounds() const
{
    if (! audioProcessor.settings.showLoudness.load())
        return {};

    auto content = getContentBounds();
    return content.removeFromRight (meterColumnWidth).reduced (4.0f, 6.0f);
}

juce::Rectangle<int> SpectrumAnalyzerEditor::getResetRect() const
{
    const auto inner = getMetersBounds().toNearestInt().reduced (12, 10);
    return juce::Rectangle<int> (inner.getRight() - 46, inner.getY(), 46, 16);
}

juce::Rectangle<int> SpectrumAnalyzerEditor::getClipRect() const
{
    const auto reset = getResetRect();
    return juce::Rectangle<int> (reset.getX() - 8 - 64, reset.getY(), 64, 16);
}

void SpectrumAnalyzerEditor::resized()
{
    settingsButton.setBounds (getWidth() - 116, 4, 106, 26);

    analyzerPill.setBounds (136, 6, 74, 22);
    scopePill.setBounds (214, 6, 56, 22);
    metersPill.setBounds (274, 6, 62, 22);

    legendA = juce::Rectangle<int> (getWidth() - 116 - 190, 5, 84, 24);
    legendB = juce::Rectangle<int> (getWidth() - 116 - 100, 5, 92, 24);

    const int pageHeights[] = { 410, 328, 282, 152 };
    panelBounds = juce::Rectangle<int> (getWidth() - 290, 38, 280, pageHeights[juce::jlimit (0, 3, panelPage)]);

    auto inner = panelBounds.reduced (14, 12);

    auto tabs = inner.removeFromTop (24);
    inner.removeFromTop (8);
    const int tabWidth = tabs.getWidth() / 4;
    tabLook.setBounds (tabs.removeFromLeft (tabWidth));
    tabAnalyzer.setBounds (tabs.removeFromLeft (tabWidth));
    tabScope.setBounds (tabs.removeFromLeft (tabWidth));
    tabMeter.setBounds (tabs);

    int labelIndex = 0;
    auto nextLabel = [&] (juce::Rectangle<int>& column)
    {
        labelRects[(size_t) labelIndex++] = column.removeFromTop (16);
    };

    // page 0: look
    {
        auto col = inner;

        nextLabel (col);                                // Color mode
        modeBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Color (Mix)
        swatches.setBounds (col.removeFromTop (24));
        col.removeFromTop (2);
        hueSlider.setBounds (col.removeFromTop (18));
        col.removeFromTop (8);

        nextLabel (col);                                // Stereo colors
        swatchesStereoA.setBounds (col.removeFromTop (24));
        col.removeFromTop (4);
        swatchesB.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        animateToggle.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Fill
        fillBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Line thickness
        thicknessSlider.setBounds (col.removeFromTop (18));
        col.removeFromTop (8);

        nextLabel (col);                                // Background
        backgroundBox.setBounds (col.removeFromTop (24));
    }

    // page 1: analyzer
    {
        auto col = inner;

        peakToggle.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Tilt
        tiltBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Speed
        speedBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Range
        rangeBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Channels
        channelsBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // FFT size
        fftBox.setBounds (col.removeFromTop (24));
    }

    // page 2: scope
    {
        auto col = inner;

        nextLabel (col);                                // Scope window
        scopeTimeBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Scope filter
        scopeFilterBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Scope colors
        swatchesScopeA.setBounds (col.removeFromTop (24));
        col.removeFromTop (4);
        swatchesScopeB.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        scopeHintRect = col.removeFromTop (48);
    }

    // page 3: meter
    {
        auto col = inner;

        nextLabel (col);                                // Meter style
        meterModeBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Meter color
        swatchesMeter.setBounds (col.removeFromTop (24));
    }
}

// ---------------------------------------------------------------- mouse

void SpectrumAnalyzerEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto& s = audioProcessor.settings;

    const bool overPanel = settingsButton.getToggleState() && panelBounds.contains (e.getPosition());
    const bool wasActive = hoverActive;

    hoverPos = e.position;
    hoverActive = getPlotBounds().contains (e.position) && ! overPanel;

    if (hoverActive && ! wasActive)
        lastReadoutX = -1.0f;

    const bool overLegend = s.showAnalyzer.load() && s.stereoMode.load() != 0
                            && (legendA.contains (e.getPosition()) || legendB.contains (e.getPosition()));

    const bool overReset = s.showLoudness.load()
                           && (getResetRect().contains (e.getPosition()) || getClipRect().contains (e.getPosition()));

    setMouseCursor ((overLegend || overReset) ? juce::MouseCursor::PointingHandCursor
                                              : (hoverActive ? juce::MouseCursor::CrosshairCursor
                                                             : juce::MouseCursor::NormalCursor));
}

void SpectrumAnalyzerEditor::mouseDrag (const juce::MouseEvent& e)
{
    mouseMove (e);
}

void SpectrumAnalyzerEditor::mouseExit (const juce::MouseEvent&)
{
    hoverActive = false;
    lastReadoutX = -1.0f;
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void SpectrumAnalyzerEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto& s = audioProcessor.settings;

    // loudness meters: "Reset" restarts the integrated loudness and the peak
    if (s.showLoudness.load() && getResetRect().contains (e.getPosition()))
    {
        audioProcessor.resetLoudnessRequested = true;
        return;
    }

    // loudness meters: clicking the CLIP lamp clears only the clip counter
    if (s.showLoudness.load() && getClipRect().contains (e.getPosition()))
    {
        audioProcessor.resetClipRequested = true;
        return;
    }

    // legend: hide / show a curve
    if (! s.showAnalyzer.load() || s.stereoMode.load() == 0)
        return;

    if (legendA.contains (e.getPosition()))
        showA = ! showA;
    else if (legendB.contains (e.getPosition()))
        showB = ! showB;
}

// ---------------------------------------------------------------- analysis

// FFT of one channel -> dB, smoothed, plus peak hold
void SpectrumAnalyzerEditor::updateSpectrum (const float* samples, Spectrum& smoothedOut, Spectrum& peaksOut,
                                             float attack, float release, bool peaksOn)
{
    std::copy (samples, samples + fftSize, fftData.begin());
    std::fill (fftData.begin() + fftSize, fftData.begin() + 2 * fftSize, 0.0f);

    window->multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft->performFrequencyOnlyForwardTransform (fftData.data());

    const float scale = 4.0f / (float) fftSize;

    for (int i = 0; i < numBins; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] * scale, floorDb);
        float& s = smoothedOut[(size_t) i];
        s += (db - s) * (db > s ? attack : release);

        float& pk = peaksOut[(size_t) i];
        pk = peaksOn ? std::max (s, pk - peakFallDb) : s;
    }
}

// Scope: the input (bass) and the sidechain (kick), low-passed. With a note length (1/4, 1/8 ...)
// and the DAW playing, the window is lined up with the beat grid, so the same beat always sits
// at the same place. Otherwise it is aligned on a rising zero crossing so it stays still.
void SpectrumAnalyzerEditor::updateScope()
{
    const auto& settings = audioProcessor.settings;

    const double rate = audioProcessor.getScopeRate();                   // decimated sample rate
    const int decimation = audioProcessor.getScopeDecimation();
    const double fs = audioProcessor.getCurrentSampleRate();
    const int64_t nowAbs = audioProcessor.getAbsoluteWritePosition();
    const int64_t nowK = nowAbs / decimation;                            // decimated samples written so far
    const HostInfo host = audioProcessor.getHostInfo();

    const auto& option = scopeOptions[juce::jlimit (0, numScopeOptions - 1, settings.scopeTime.load())];
    const int maxWindow = SpectrumAnalyzerProcessor::scopeRingSize / 2 - 32;
    const double bpm = (host.valid && host.bpm > 1.0) ? host.bpm : 120.0;

    // length of the window
    double windowSeconds;

    if (option.milliseconds > 0.0)
    {
        windowSeconds = option.milliseconds * 0.001;
    }
    else
    {
        double quarters = option.quarters;
        if (quarters < 0.0)                                              // bars
            quarters = -quarters * (double) host.sigNum * 4.0 / (double) juce::jmax (1, host.sigDen);

        windowSeconds = quarters * 60.0 / bpm;
    }

    const int windowSamples = juce::jlimit (16, maxWindow, (int) std::lround (windowSeconds * rate));
    const bool beatSynced = (option.milliseconds <= 0.0) && host.valid && host.playing && host.bpm > 1.0;
    const bool sideActive = audioProcessor.isSidechainActive();

    int filled = windowSamples;        // how much of the window is already drawn with fresh data

    if (beatSynced)
    {
        // where are we inside the current window?
        const double samplesPerQuarterAbs = 60.0 / host.bpm * fs;
        const double nowPpq = host.ppqStart + (double) (nowAbs - host.absStart) / samplesPerQuarterAbs;
        const double samplesPerQuarter = 60.0 / host.bpm * rate;
        const double windowQuarters = (double) windowSamples / samplesPerQuarter;
        const double startPpq = std::floor (nowPpq / windowQuarters) * windowQuarters;
        const double elapsed = juce::jlimit (0.0, windowQuarters, nowPpq - startPpq);

        filled = juce::jlimit (0, windowSamples, (int) std::lround (elapsed * samplesPerQuarter));
        const int64_t startK = nowK - filled;

        for (int i = 0; i < windowSamples; ++i)
        {
            // the part not reached yet shows the previous cycle
            const int64_t k = (i < filled) ? startK + i : startK + i - windowSamples;
            scopeData[(size_t) i]     = audioProcessor.readScopeSample (0, k);
            scopeDataSide[(size_t) i] = audioProcessor.readScopeSample (1, k);
        }
    }
    else
    {
        // trigger on the sidechain (kick) if it carries signal, otherwise on the input
        int triggerChannel = 0;

        if (sideActive)
        {
            float peak = 0.0f;
            for (int i = 0; i < windowSamples; ++i)
                peak = std::max (peak, std::abs (audioProcessor.readScopeSample (1, nowK - 1 - i)));

            if (peak > 0.003f)
                triggerChannel = 1;
        }

        int64_t start = nowK - windowSamples;

        for (int64_t c = nowK - windowSamples; c >= nowK - 2 * (int64_t) windowSamples && c > 0; --c)
        {
            if (audioProcessor.readScopeSample (triggerChannel, c - 1) < 0.0f
                && audioProcessor.readScopeSample (triggerChannel, c) >= 0.0f)
            {
                start = c;
                break;
            }
        }

        for (int i = 0; i < windowSamples; ++i)
        {
            scopeData[(size_t) i]     = audioProcessor.readScopeSample (0, start + i);
            scopeDataSide[(size_t) i] = audioProcessor.readScopeSample (1, start + i);
        }
    }

    scopeCount = windowSamples;
    scopeFilled = filled;
    scopeSynced = beatSynced;
    scopeSideActive = sideActive;
    scopeLabel = option.label;
}

void SpectrumAnalyzerEditor::timerCallback()
{
    const auto& settings = audioProcessor.settings;

    const int mode = juce::jlimit (0, 2, settings.stereoMode.load());

    if (settings.showAnalyzer.load())
    {
        // FFT size changed in the settings?
        const int wantedSize = 2048 << juce::jlimit (0, 3, settings.fftSizeIndex.load());
        if (wantedSize != fftSize)
            rebuildFft (wantedSize);

        // channel mode changed? start curve B from scratch
        if (mode != lastStereoMode)
        {
            smoothedB.fill (floorDb);
            peaksB.fill (floorDb);
            lastStereoMode = mode;
        }

        // 1. latest samples of left and right
        audioProcessor.copyLatestSamples (bufL.data(), 0, fftSize);
        audioProcessor.copyLatestSamples (bufR.data(), 1, fftSize);

        // mix and mid/side: A = (L+R)/2 (mid), B = (L-R)/2 (side)
        if (mode != 1)
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const float l = bufL[(size_t) i];
                const float r = bufR[(size_t) i];
                bufL[(size_t) i] = 0.5f * (l + r);
                bufR[(size_t) i] = 0.5f * (l - r);
            }
        }

        // 2. FFT -> dB -> smoothing (speed setting) and peaks
        const int speedIndex = juce::jlimit (0, 2, settings.speed.load());
        const float attack  = attackTable[speedIndex];
        const float release = releaseTable[speedIndex];
        const bool peaksOn = settings.peakHold.load();

        updateSpectrum (bufL.data(), smoothedA, peaksA, attack, release, peaksOn);
        if (mode != 0)
            updateSpectrum (bufR.data(), smoothedB, peaksB, attack, release, peaksOn);
    }

    // 3. scope
    if (settings.showScope.load())
        updateScope();

    // 4. animated rainbow: the colours slide slowly
    if (settings.animate.load() && settings.colourMode.load() == 1)
        hueOffset = std::fmod (hueOffset + 0.0025f, 1.0f);

    // 5. hover readout: levels are smoothed and the text only changes ~5 times per second
    if (hoverActive && settings.showAnalyzer.load())
    {
        const auto plot = getPlotBounds();
        const int width = (int) plot.getWidth();

        if (width >= 2)
        {
            const float x = juce::jlimit (plot.getX(), plot.getRight(), hoverPos.x);
            const float t0 = (x - plot.getX()) / plot.getWidth();
            const float t1 = t0 + 1.0f / (float) width;
            const float ratio = maxFreq / minFreq;
            const float f0 = minFreq * std::pow (ratio, t0);
            const float f1 = minFreq * std::pow (ratio, t1);
            const float binHz = (float) (audioProcessor.getCurrentSampleRate() / (double) fftSize);
            const float levelA = levelAt (smoothedA, f0, f1, binHz);
            const float levelB = (mode != 0) ? levelAt (smoothedB, f0, f1, binHz) : floorDb;

            const bool moved = std::abs (x - lastReadoutX) > 0.5f;
            lastReadoutX = x;

            if (moved)                                           // mouse moved: respond at once
            {
                readoutLevelA = levelA;
                readoutLevelB = levelB;
            }
            else                                                 // mouse still: steady values
            {
                readoutLevelA += (levelA - readoutLevelA) * 0.1f;
                readoutLevelB += (levelB - readoutLevelB) * 0.1f;
            }

            if (moved || ++readoutTick >= 6)
            {
                readoutTick = 0;
                readoutLine1 = frequencyText (f0) + "   " + noteName (f0);

                if (mode == 0)
                {
                    readoutLine2 = juce::String (readoutLevelA, 1) + " dB";
                }
                else
                {
                    const juce::String nameA = (mode == 1) ? "L" : "M";
                    const juce::String nameB = (mode == 1) ? "R" : "S";
                    readoutLine2 = nameA + " " + juce::String (readoutLevelA, 1) + "   "
                                 + nameB + " " + juce::String (readoutLevelB, 1) + " dB";
                }
            }
        }
    }

    // 6. meters: live peak, peak-hold markers and the clipping flash
    if (settings.showLoudness.load())
    {
        const float peakLinear = audioProcessor.peakSincePoll.exchange (0.0f);
        const float peakDb = peakLinear > 1.0e-6f ? 20.0f * std::log10 (peakLinear) : -120.0f;
        meterPeakLive = (peakDb >= meterPeakLive) ? peakDb : std::max (peakDb, meterPeakLive - 1.5f);

        const float levels[4] = { audioProcessor.loudnessMomentary.load(), audioProcessor.loudnessShortTerm.load(),
                                  audioProcessor.loudnessIntegrated.load(), meterPeakLive };

        for (size_t i = 0; i < 4; ++i)
        {
            if (levels[i] >= meterHold[i])
            {
                meterHold[i] = levels[i];
                meterHoldFrames[i] = 45;                                    // hold for 1.5 s
            }
            else if (meterHoldFrames[i] > 0)
            {
                --meterHoldFrames[i];
            }
            else
            {
                meterHold[i] = std::max (levels[i], meterHold[i] - 0.5f);   // then fall slowly
            }
        }

        const uint32_t clips = audioProcessor.clipCount.load();
        if (clips > lastClipCount)
            clipFlashFrames = 24;                                           // flash for about 0.8 s
        lastClipCount = clips;

        if (clipFlashFrames > 0)
            --clipFlashFrames;
    }

    repaint();
}

// Level (in dB) for a frequency range, including the tilt setting
float SpectrumAnalyzerEditor::levelAt (const Spectrum& data, float f0, float f1, float binHz) const
{
    const float b0 = f0 / binHz;
    const float b1 = f1 / binHz;
    float m;

    if (b1 - b0 < 1.0f)
    {
        // low frequencies: several pixels per bin -> interpolate
        const float b = juce::jlimit (0.0f, (float) (numBins - 2), 0.5f * (b0 + b1));
        const int i = (int) b;
        const float frac = b - (float) i;
        m = data[(size_t) i] * (1.0f - frac) + data[(size_t) i + 1] * frac;
    }
    else
    {
        // high frequencies: several bins per pixel -> keep the largest
        const int lo = juce::jlimit (0, numBins - 1, (int) std::ceil (b0));
        const int hi = juce::jlimit (0, numBins - 1, (int) std::floor (b1));

        m = floorDb;
        for (int i = lo; i <= hi; ++i)
            m = std::max (m, data[(size_t) i]);
    }

    const float tilt = tiltTable[juce::jlimit (0, 2, audioProcessor.settings.tilt.load())];
    return m + tilt * std::log2 (std::sqrt (f0 * f1) / 1000.0f);
}

// Colour of the Mix curve along the horizontal axis (single colour or rainbow)
juce::ColourGradient SpectrumAnalyzerEditor::makeHorizontalGradient (juce::Rectangle<float> area, float alpha) const
{
    const auto& s = audioProcessor.settings;

    if (s.colourMode.load() == 0)
    {
        const auto c = juce::Colour (s.singleColour.load()).withAlpha (alpha);
        return juce::ColourGradient (c, area.getX(), 0.0f, c, area.getRight(), 0.0f, false);
    }

    const int stops = 12;
    const float offset = hueOffset;
    auto colourAt = [offset, alpha] (float t)
    {
        return juce::Colour::fromHSV (std::fmod (offset + 0.8f * t, 1.0f), 0.7f, 1.0f, alpha);
    };

    juce::ColourGradient grad (colourAt (0.0f), area.getX(), 0.0f,
                               colourAt (1.0f), area.getRight(), 0.0f, false);
    for (int i = 1; i < stops; ++i)
        grad.addColour ((double) i / (double) stops, colourAt ((float) i / (float) stops));

    return grad;
}

// ---------------------------------------------------------------- drawing

void SpectrumAnalyzerEditor::drawAnalyzer (juce::Graphics& g)
{
    const auto& s = audioProcessor.settings;
    const auto bg = backgrounds[juce::jlimit (0, 3, s.background.load())];

    const auto plot = getPlotBounds();
    const int mode = juce::jlimit (0, 2, s.stereoMode.load());
    const bool stereo = (mode != 0);

    // in the two-curve modes both curves use their own pair of colours
    const auto colourA = juce::Colour (s.colourStereoA.load());
    const auto colourB = juce::Colour (s.colourB.load());

    const float displayMinDb = rangeTable[juce::jlimit (0, 2, s.range.load())];

    const auto xForFreq = [&] (float f)
    {
        return plot.getX() + plot.getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq);
    };
    const auto yForDb = [&] (float db)
    {
        return plot.getBottom() - plot.getHeight() * (db - displayMinDb) / (maxDb - displayMinDb);
    };

    const int width = (int) plot.getWidth();
    const float binHz = (float) (audioProcessor.getCurrentSampleRate() / (double) fftSize);
    const float ratio = maxFreq / minFreq;

    auto buildCurve = [&] (const Spectrum& data)
    {
        juce::Path p;
        for (int px = 0; px <= width; ++px)
        {
            const float f0 = minFreq * std::pow (ratio, (float) px / (float) width);
            const float f1 = minFreq * std::pow (ratio, (float) (px + 1) / (float) width);
            const float x = plot.getX() + (float) px;
            const float y = yForDb (levelAt (data, f0, f1, binHz));

            if (px == 0) p.startNewSubPath (x, y);
            else         p.lineTo (x, y);
        }
        return p;
    };

    auto closeUnder = [&] (const juce::Path& curve)
    {
        juce::Path p (curve);
        p.lineTo (plot.getRight(), plot.getBottom());
        p.lineTo (plot.getX(), plot.getBottom());
        p.closeSubPath();
        return p;
    };

    // spectrum curves
    juce::Path curveA, curveB;

    if (width >= 2)
    {
        curveA = buildCurve (smoothedA);
        if (stereo)
            curveB = buildCurve (smoothedB);

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());

        const int fillStyle = s.fillStyle.load();

        if (stereo)
        {
            // two curves: soft vertical fades, so the overlap stays readable
            if (fillStyle != 2)
            {
                auto fadeFill = [&] (const juce::Path& curve, juce::Colour c, float topAlpha)
                {
                    g.setGradientFill (juce::ColourGradient (c.withAlpha (topAlpha), 0.0f, plot.getY(),
                                                             c.withAlpha (0.0f), 0.0f, plot.getBottom(), false));
                    g.fillPath (closeUnder (curve));
                };

                if (showB) fadeFill (curveB, colourB, 0.45f);
                if (showA) fadeFill (curveA, colourA, 0.55f);
            }
        }
        else
        {
            const auto filledA = closeUnder (curveA);

            if (fillStyle == 0)          // gradient: strong colour on top, fades into the background
            {
                g.setGradientFill (makeHorizontalGradient (plot, 0.75f));
                g.fillPath (filledA);

                juce::Graphics::ScopedSaveState inner (g);
                g.reduceClipRegion (filledA, juce::AffineTransform());
                g.setGradientFill (juce::ColourGradient (bg.withAlpha (0.0f), 0.0f, plot.getY(),
                                                         bg.withAlpha (0.96f), 0.0f, plot.getBottom(), false));
                g.fillRect (plot);
            }
            else if (fillStyle == 1)     // solid
            {
                g.setGradientFill (makeHorizontalGradient (plot, 0.35f));
                g.fillPath (filledA);
            }
        }
    }

    // frequency grid
    const float freqs[]      = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    const char* freqLabels[] = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };
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

    // dB grid
    for (int db = 0; db > (int) displayMinDb + 9; db -= 20)
    {
        const float y = yForDb ((float) db);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawText (juce::String (db), juce::Rectangle<int> ((int) plot.getX() - 34, (int) y - 7, 30, 14),
                    juce::Justification::centredRight);
    }

    // curve lines (both solid; in the two-curve modes each has its own colour)
    if (width >= 2)
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());

        const juce::PathStrokeType stroke (s.lineThickness.load());

        if (! stereo)
        {
            g.setGradientFill (makeHorizontalGradient (plot, 1.0f));
            g.strokePath (curveA, stroke);
        }
        else
        {
            if (showB) { g.setColour (colourB); g.strokePath (curveB, stroke); }
            if (showA) { g.setColour (colourA); g.strokePath (curveA, stroke); }
        }
    }

    // peak hold lines (in the two-curve modes: in the colour of each curve)
    if (width >= 2 && s.peakHold.load())
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());

        if (! stereo)
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.strokePath (buildCurve (peaksA), juce::PathStrokeType (1.0f));
        }
        else
        {
            if (showB) { g.setColour (colourB.withAlpha (0.6f)); g.strokePath (buildCurve (peaksB), juce::PathStrokeType (1.0f)); }
            if (showA) { g.setColour (colourA.withAlpha (0.6f)); g.strokePath (buildCurve (peaksA), juce::PathStrokeType (1.0f)); }
        }
    }

    // hover readout: frequency, note and level(s)
    if (hoverActive && width >= 2)
    {
        const float x = juce::jlimit (plot.getX(), plot.getRight(), hoverPos.x);
        const float t0 = (x - plot.getX()) / plot.getWidth();
        const float t1 = t0 + 1.0f / (float) width;
        const float f0 = minFreq * std::pow (ratio, t0);
        const float f1 = minFreq * std::pow (ratio, t1);
        const float yA = juce::jlimit (plot.getY(), plot.getBottom(), yForDb (levelAt (smoothedA, f0, f1, binHz)));

        // vertical line and dot(s) on the curve(s)
        g.setColour (juce::Colours::white.withAlpha (0.3f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());

        if (! stereo || showA)
        {
            g.setColour (stereo ? colourA : juce::Colours::white);
            g.fillEllipse (x - 3.5f, yA - 3.5f, 7.0f, 7.0f);
        }

        if (stereo && showB)
        {
            const float yB = juce::jlimit (plot.getY(), plot.getBottom(), yForDb (levelAt (smoothedB, f0, f1, binHz)));
            g.setColour (colourB);
            g.fillEllipse (x - 3.5f, yB - 3.5f, 7.0f, 7.0f);
        }

        // box follows the mouse (not the curve), so it does not jump
        juce::Rectangle<float> box (0.0f, 0.0f, stereo ? 210.0f : 134.0f, 40.0f);
        float bx = hoverPos.x + 16.0f;
        if (bx + box.getWidth() > plot.getRight())
            bx = hoverPos.x - 16.0f - box.getWidth();
        float by = hoverPos.y - box.getHeight() - 12.0f;
        if (by < plot.getY())
            by = hoverPos.y + 18.0f;
        box.setPosition (bx, by);

        g.setColour (juce::Colour (0xee1a2027));
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawRoundedRectangle (box, 6.0f, 1.0f);

        auto inner = box.toNearestInt().reduced (9, 5);
        g.setFont (12.0f);
        g.setColour (juce::Colours::white);
        g.drawText (readoutLine1, inner.removeFromTop (16), juce::Justification::centredLeft);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawText (readoutLine2, inner, juce::Justification::centredLeft);
    }
}

void SpectrumAnalyzerEditor::drawScope (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto& s = audioProcessor.settings;

    // solid colours only: input (bass) and sidechain (kick)
    const auto colourInput = juce::Colour (s.scopeColour.load());
    const auto colourSide  = juce::Colour (s.scopeColourSide.load());

    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.fillRoundedRectangle (area, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (area, 6.0f, 1.0f);

    // grid: centre line and +-0.5
    const float cy = area.getCentreY();
    const float half = area.getHeight() * 0.5f - 8.0f;

    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.drawHorizontalLine ((int) cy, area.getX(), area.getRight());
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawHorizontalLine ((int) (cy - half * 0.5f), area.getX(), area.getRight());
    g.drawHorizontalLine ((int) (cy + half * 0.5f), area.getX(), area.getRight());

    // title
    const int filterIndex = juce::jlimit (0, SpectrumAnalyzerProcessor::numScopeFilters - 1, s.scopeFilter.load());
    const juce::String title = "SCOPE   " + scopeLabel + (scopeSynced ? "  (beat sync)" : "  (free)")
                             + "   low-pass " + juce::String ((int) SpectrumAnalyzerProcessor::scopeCutoffs[filterIndex]) + " Hz";

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (11.0f);
    g.drawText (title, area.toNearestInt().reduced (8, 3).removeFromTop (14), juce::Justification::centredLeft);

    // legend (top right)
    {
        auto legend = area.toNearestInt().reduced (8, 3).removeFromTop (14);
        auto entry = [&] (const juce::String& name, juce::Colour c)
        {
            auto r = legend.removeFromRight (74);
            g.setColour (c);
            g.drawLine ((float) r.getX(), (float) r.getCentreY(), (float) r.getX() + 14.0f, (float) r.getCentreY(), 2.5f);
            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.drawText (name, r.withTrimmedLeft (18), juce::Justification::centredLeft);
        };

        if (scopeSideActive)
            entry ("Sidechain", colourSide);

        entry ("Input", colourInput);
    }

    if (scopeCount < 2)
        return;

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (area.toNearestInt());

    const float thickness = juce::jmax (1.2f, s.lineThickness.load());

    auto xFor = [&] (int i)
    {
        return area.getX() + area.getWidth() * (float) i / (float) (scopeCount - 1);
    };
    auto yFor = [&] (float v)
    {
        return cy - juce::jlimit (-1.0f, 1.0f, v) * half;
    };

    // draws samples [i0, i1) as a line (few samples per pixel) or as a min/max band (many samples per pixel)
    auto drawRange = [&] (const ScopeBuffer& data, int i0, int i1, juce::Colour colour)
    {
        if (i1 - i0 < 2)
            return;

        g.setColour (colour);

        if (area.getWidth() / (float) scopeCount >= 0.5f)
        {
            juce::Path p;
            for (int i = i0; i < i1; ++i)
            {
                if (i == i0) p.startNewSubPath (xFor (i), yFor (data[(size_t) i]));
                else         p.lineTo (xFor (i), yFor (data[(size_t) i]));
            }
            g.strokePath (p, juce::PathStrokeType (thickness));
        }
        else
        {
            const int x0 = (int) std::floor (xFor (i0));
            const int x1 = (int) std::ceil (xFor (i1 - 1));

            for (int px = x0; px <= x1; ++px)
            {
                const int s0 = juce::jlimit (i0, i1 - 1, (int) std::floor (((float) px - area.getX()) / area.getWidth() * (float) (scopeCount - 1)));
                const int s1 = juce::jlimit (i0, i1 - 1, (int) std::ceil (((float) (px + 1) - area.getX()) / area.getWidth() * (float) (scopeCount - 1)));

                float lo = data[(size_t) s0], hi = lo;
                for (int i = s0; i <= s1; ++i)
                {
                    lo = std::min (lo, data[(size_t) i]);
                    hi = std::max (hi, data[(size_t) i]);
                }

                const float yTop = yFor (hi);
                g.fillRect ((float) px, yTop, 1.0f, juce::jmax (1.0f, yFor (lo) - yTop));
            }
        }
    };

    auto drawTrace = [&] (const ScopeBuffer& data, juce::Colour colour)
    {
        if (scopeSynced && scopeFilled < scopeCount)
        {
            drawRange (data, scopeFilled, scopeCount, colour.withAlpha (0.3f));   // previous cycle, dimmed
            drawRange (data, 0, scopeFilled, colour);                              // current cycle
        }
        else
        {
            drawRange (data, 0, scopeCount, colour);
        }
    };

    if (scopeSideActive)
        drawTrace (scopeDataSide, colourSide);       // the kick goes underneath ...

    drawTrace (scopeData, colourInput);              // ... the bass on top

    // position marker while the DAW plays
    if (scopeSynced && scopeFilled > 0 && scopeFilled < scopeCount)
    {
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawVerticalLine ((int) xFor (scopeFilled), area.getY(), area.getBottom());
    }
}

void SpectrumAnalyzerEditor::drawMeters (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto& s = audioProcessor.settings;
    const juce::Colour red (0xffff2d2d);

    const uint32_t clips = audioProcessor.clipCount.load();
    const bool clipped = clips > 0;
    const float flash = (float) clipFlashFrames / 24.0f;      // 1 right after a new clip, then fades out

    // card: soft glass look, the border glows red when the signal clips
    if (flash > 0.0f)
    {
        g.setColour (red.withAlpha (0.14f * flash));
        g.fillRoundedRectangle (area.expanded (3.0f), 13.0f);
    }

    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.07f), 0.0f, area.getY(),
                                             juce::Colours::white.withAlpha (0.02f), 0.0f, area.getBottom(), false));
    g.fillRoundedRectangle (area, 10.0f);

    g.setColour (flash > 0.0f ? red.withAlpha (0.35f + 0.65f * flash) : juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (area, 10.0f, flash > 0.0f ? 1.6f : 1.0f);

    auto inner = area.toNearestInt().reduced (12, 10);

    // header: title, CLIP lamp and Reset
    auto header = inner.removeFromTop (16);
    g.setFont (11.0f);
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.drawText ("LOUDNESS", header, juce::Justification::centredLeft);

    const auto lamp = getClipRect().toFloat();

    if (clipped)
    {
        g.setColour (red.withAlpha (0.20f + 0.40f * flash));
        g.fillRoundedRectangle (lamp.expanded (3.0f), 7.0f);
        g.setColour (red.withAlpha (0.9f));
        g.fillRoundedRectangle (lamp, 4.0f);
        g.setColour (juce::Colours::white);
        g.setFont (10.0f);
        g.drawText ("CLIP " + (clips > 999 ? juce::String (">999") : juce::String ((int) clips)),
                    lamp.toNearestInt(), juce::Justification::centred);
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (lamp, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (10.0f);
        g.drawText ("CLIP", lamp.toNearestInt(), juce::Justification::centred);
    }

    const auto resetRect = getResetRect();
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.fillRoundedRectangle (resetRect.toFloat(), 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.setFont (10.0f);
    g.drawText ("Reset", resetRect, juce::Justification::centred);

    inner.removeFromTop (10);

    // what to show: bars move live; the numbers are the measured values (peak: highest since Reset)
    const float samplePeak = audioProcessor.samplePeakDb.load();
    const float barValues[4] = { audioProcessor.loudnessMomentary.load(), audioProcessor.loudnessShortTerm.load(),
                                 audioProcessor.loudnessIntegrated.load(), meterPeakLive };
    const float textValues[4] = { barValues[0], barValues[1], barValues[2], samplePeak };
    const char* names[4] = { "MOM", "SHORT", "INT", "PEAK" };
    const char* units[4] = { "LUFS", "LUFS", "LUFS", "dBFS" };

    auto footer = inner.removeFromBottom (30);
    auto valuesRow = inner.removeFromTop (18);
    inner.removeFromTop (6);
    inner.removeFromBottom (4);

    const auto scaleArea = inner.removeFromLeft (26);
    const int columnWidth = inner.getWidth() / 4;
    const float barTop = (float) inner.getY();
    const float barHeight = (float) inner.getHeight();
    const float barBottom = barTop + barHeight;

    // scale: a tick every 6 dB, a number every 12 dB
    for (int mark = 0; mark >= -48; mark -= 6)
    {
        const float y = barBottom - ((float) mark - meterMinDb) / (0.0f - meterMinDb) * barHeight;
        const bool major = (mark % 12 == 0);

        g.setColour (juce::Colours::white.withAlpha (major ? 0.10f : 0.05f));
        g.drawHorizontalLine ((int) y, (float) inner.getX(), (float) inner.getRight());

        if (major)
        {
            g.setFont (10.0f);
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawText (juce::String (mark), juce::Rectangle<int> (scaleArea.getX(), (int) y - 6, scaleArea.getWidth() - 4, 12),
                        juce::Justification::centredRight);
        }
    }

    // segments: the bar is made of small blocks that light up one by one
    const int segments = juce::jlimit (20, 48, (int) (barHeight / 6.0f));
    const float segmentDb = (0.0f - meterMinDb) / (float) segments;
    const float segmentHeight = barHeight / (float) segments;
    const float gap = juce::jmax (1.0f, segmentHeight * 0.24f);

    for (int i = 0; i < 4; ++i)
    {
        const int columnX = inner.getX() + i * columnWidth;
        const float centreX = (float) columnX + (float) columnWidth * 0.5f;
        const juce::Rectangle<float> bar (centreX - 11.0f, barTop, 22.0f, barHeight);
        const float value = barValues[i];
        const bool peakColumn = (i == 3);
        const bool columnClipping = peakColumn && clipped;

        // red glow around the peak bar right after a clip
        if (peakColumn && flash > 0.0f)
        {
            g.setColour (red.withAlpha (0.28f * flash));
            g.fillRoundedRectangle (bar.expanded (6.0f, 4.0f), 7.0f);
        }

        for (int seg = 0; seg < segments; ++seg)
        {
            const float segCentreDb = meterMinDb + ((float) seg + 0.5f) * segmentDb;
            const float y = barBottom - (float) (seg + 1) * segmentHeight;
            const juce::Rectangle<float> block (bar.getX(), y + gap * 0.5f, bar.getWidth(), segmentHeight - gap);
            const bool lit = (value > -70.0f) && (value >= segCentreDb);

            if (! lit)
            {
                g.setColour (juce::Colours::white.withAlpha (0.07f));
                g.fillRoundedRectangle (block, 1.5f);
                continue;
            }

            // gradient: green -> red. Solid: the chosen colour, but the top segments are always red
            juce::Colour colour = (s.meterMode.load() == 0) ? colourForDb (segCentreDb)
                                                            : juce::Colour (s.meterColour.load());
            if (segCentreDb >= -1.0f)
                colour = red;

            g.setColour (colour.withAlpha (0.20f));
            g.fillRoundedRectangle (block.expanded (2.5f, 1.0f), 3.0f);     // glow
            g.setColour (colour);
            g.fillRoundedRectangle (block, 1.5f);
        }

        // peak-hold marker (not for the integrated value, which moves slowly)
        if (i != 2 && meterHold[(size_t) i] > value + 0.5f && meterHold[(size_t) i] > meterMinDb)
        {
            const float level = juce::jlimit (meterMinDb, 0.0f, meterHold[(size_t) i]);
            const float y = barBottom - (level - meterMinDb) / (0.0f - meterMinDb) * barHeight;

            g.setColour ((level >= -0.5f ? red : juce::Colours::white).withAlpha (0.95f));
            g.fillRoundedRectangle (bar.getX() - 3.0f, y - 1.0f, bar.getWidth() + 6.0f, 2.0f, 1.0f);
        }

        // clip bar above the peak meter: lit while clipped
        if (columnClipping)
        {
            g.setColour (red.withAlpha (0.35f));
            g.fillRoundedRectangle (bar.getX() - 2.0f, bar.getY() - 7.0f, bar.getWidth() + 4.0f, 6.0f, 3.0f);
            g.setColour (red);
            g.fillRoundedRectangle (bar.getX(), bar.getY() - 6.0f, bar.getWidth(), 4.0f, 2.0f);
        }

        // value, name and unit
        g.setFont (13.0f);
        g.setColour (columnClipping ? juce::Colour (0xffff5a5a) : juce::Colours::white);
        g.drawText (meterText (textValues[i]), juce::Rectangle<int> (columnX, valuesRow.getY(), columnWidth, 18),
                    juce::Justification::centred);

        g.setFont (10.0f);
        g.setColour (columnClipping ? juce::Colour (0xffff5a5a) : juce::Colours::white.withAlpha (0.7f));
        g.drawText (names[i], juce::Rectangle<int> (columnX, footer.getY(), columnWidth, 14), juce::Justification::centred);
        g.setFont (9.0f);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawText (units[i], juce::Rectangle<int> (columnX, footer.getY() + 14, columnWidth, 13), juce::Justification::centred);
    }
}

void SpectrumAnalyzerEditor::paint (juce::Graphics& g)
{
    const auto& s = audioProcessor.settings;
    g.fillAll (backgrounds[juce::jlimit (0, 3, s.background.load())]);

    const bool analyzerOn = s.showAnalyzer.load();
    const bool scopeOn = s.showScope.load();
    const bool metersOn = s.showLoudness.load();

    const int mode = juce::jlimit (0, 2, s.stereoMode.load());
    const bool stereo = (mode != 0);

    // signature
    const auto header = getLocalBounds().toFloat().removeFromTop (32.0f);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (14.0f);
    g.drawText ("Made by Mugen", header.toNearestInt().withTrimmedLeft (14).withWidth (120), juce::Justification::centredLeft);

    // legend (two-curve modes): click a name to hide / show that curve
    if (analyzerOn && stereo)
    {
        const auto colourA = juce::Colour (s.colourStereoA.load());
        const auto colourB = juce::Colour (s.colourB.load());

        auto drawLegend = [&] (juce::Rectangle<int> r, juce::Colour c, const juce::String& name, bool visible)
        {
            const float ly = (float) r.getCentreY();
            g.setColour (c.withAlpha (visible ? 1.0f : 0.25f));
            g.drawLine ((float) r.getX() + 4.0f, ly, (float) r.getX() + 22.0f, ly, 2.5f);
            g.setColour (juce::Colours::white.withAlpha (visible ? 0.85f : 0.3f));
            g.setFont (12.0f);
            g.drawText (name, r.withTrimmedLeft (28), juce::Justification::centredLeft);
        };

        drawLegend (legendA, colourA, (mode == 1) ? "L" : "Mid", showA);
        drawLegend (legendB, colourB, (mode == 1) ? "R" : "Side", showB);
    }

    if (analyzerOn)
        drawAnalyzer (g);

    if (scopeOn)
    {
        if (analyzerOn)
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawHorizontalLine ((int) getScopeBounds().getY() - 6, 0.0f, getMainArea().getRight());
        }

        drawScope (g, getScopeBounds());
    }

    if (metersOn)
        drawMeters (g, getMetersBounds());

    if (! analyzerOn && ! scopeOn && ! metersOn)
    {
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (14.0f);
        g.drawText ("Turn on Analyzer, Scope or Meters above", getContentBounds().toNearestInt(), juce::Justification::centred);
    }

    // settings panel (background and texts; the controls are drawn on top)
    if (settingsButton.getToggleState())
    {
        g.setColour (juce::Colour (0xf01a2027));
        g.fillRoundedRectangle (panelBounds.toFloat(), 10.0f);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (panelBounds.toFloat(), 10.0f, 1.0f);

        // which labels belong to the current page
        const int firstLabels[] = { 0, 6, 11, 14 };
        const int lastLabels[]  = { 5, 10, 13, 15 };
        const int page = juce::jlimit (0, 3, panelPage);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (12.0f);
        for (int i = firstLabels[page]; i <= lastLabels[page]; ++i)
        {
            juce::String text = labelTexts[(size_t) i];
            if (i == 0 && stereo && s.colourMode.load() == 1)
                text = "Color mode (rainbow: Mix only)";

            g.drawText (text, labelRects[(size_t) i], juce::Justification::centredLeft);
        }

        // sidechain hint (Scope page)
        if (page == 2)
        {
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (11.0f);
            g.drawFittedText ("Sidechain: in your DAW, send the kick to this plugin's sidechain input. "
                              "The kick is drawn under the bass.",
                              scopeHintRect, juce::Justification::topLeft, 4);
        }

        // rainbow bar behind the hue slider
        if (hueSlider.isVisible())
        {
            const auto bar = hueSlider.getBounds().toFloat()
                                 .withSizeKeepingCentre ((float) hueSlider.getWidth() - 16.0f, 6.0f);
            juce::ColourGradient hueGrad (juce::Colour::fromHSV (0.0f, 0.75f, 1.0f, 1.0f), bar.getX(), 0.0f,
                                          juce::Colour::fromHSV (1.0f, 0.75f, 1.0f, 1.0f), bar.getRight(), 0.0f, false);
            for (int i = 1; i < 6; ++i)
                hueGrad.addColour ((double) i / 6.0, juce::Colour::fromHSV ((float) i / 6.0f, 0.75f, 1.0f, 1.0f));
            g.setGradientFill (hueGrad);
            g.fillRoundedRectangle (bar, 3.0f);
        }
    }
}
