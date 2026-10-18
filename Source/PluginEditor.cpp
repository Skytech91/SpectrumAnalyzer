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

    constexpr int   scopeTimeMs[]    = { 20, 50, 100 };
    constexpr float scopeFilterHz[]  = { 120.0f, 200.0f, 0.0f };   // 0 = full range

    constexpr float stripBorder = 10.0f;
    constexpr float loudnessWidth = 210.0f;

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

    // second-order low-pass filter (RBJ cookbook)
    void designLowpass (Biquad& bq, double fs, double fc, double q)
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

    juce::String loudnessText (float value, const juce::String& unit)
    {
        if (value <= -70.0f)
            return "-inf " + unit;

        return juce::String (value, 1) + " " + unit;
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
                   juce::String ("Scope time"), juce::String ("Scope filter") };

    // "Settings" button with gear icon
    settingsButton.onClick = [this] { updatePanelVisibility(); };
    addAndMakeVisible (settingsButton);

    // tabs inside the settings panel
    tabLook.setButtonText ("Look");
    tabAnalyzer.setButtonText ("Analyzer");
    tabScope.setButtonText ("Scope / Meter");

    for (auto* tab : { &tabLook, &tabAnalyzer, &tabScope })
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

    // colour mode: single colour / rainbow (Mix mode)
    modeBox.addItem ("Single color", 1);
    modeBox.addItem ("Rainbow", 2);
    modeBox.setSelectedId (s.colourMode.load() + 1, juce::dontSendNotification);
    modeBox.onChange = [this]
    {
        audioProcessor.settings.colourMode = modeBox.getSelectedId() - 1;
        updateSwatchSelection();
    };
    addChildComponent (modeBox);

    // colours for the Mix curve
    updateSwatchSelection();
    swatches.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.singleColour = c.getARGB();
        hueSlider.setValue ((double) c.getHue(), juce::dontSendNotification);
        modeBox.setSelectedId (1, juce::sendNotification);
    };
    addChildComponent (swatches);

    // colours for the two-curve modes (Left/Right and Mid/Side share this pair)
    swatchesStereoA.setSelectedFromColour (juce::Colour (s.colourStereoA.load()));
    swatchesStereoA.onPick = [this] (juce::Colour c) { audioProcessor.settings.colourStereoA = c.getARGB(); };
    addChildComponent (swatchesStereoA);

    swatchesB.setSelectedFromColour (juce::Colour (s.colourB.load()));
    swatchesB.onPick = [this] (juce::Colour c) { audioProcessor.settings.colourB = c.getARGB(); };
    addChildComponent (swatchesB);

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
    };
    addChildComponent (hueSlider);

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

    // peak hold
    peakToggle.setButtonText ("Peak hold");
    peakToggle.setToggleState (s.peakHold.load(), juce::dontSendNotification);
    peakToggle.onClick = [this] { audioProcessor.settings.peakHold = peakToggle.getToggleState(); };
    addChildComponent (peakToggle);

    // tilt (slope compensation)
    tiltBox.addItem ("Flat (0 dB/oct)", 1);
    tiltBox.addItem ("3 dB/oct", 2);
    tiltBox.addItem ("4.5 dB/oct", 3);
    tiltBox.setSelectedId (s.tilt.load() + 1, juce::dontSendNotification);
    tiltBox.onChange = [this] { audioProcessor.settings.tilt = tiltBox.getSelectedId() - 1; };
    addChildComponent (tiltBox);

    // speed (smoothing)
    speedBox.addItem ("Fast", 1);
    speedBox.addItem ("Medium", 2);
    speedBox.addItem ("Slow", 3);
    speedBox.setSelectedId (s.speed.load() + 1, juce::dontSendNotification);
    speedBox.onChange = [this] { audioProcessor.settings.speed = speedBox.getSelectedId() - 1; };
    addChildComponent (speedBox);

    // dB range
    rangeBox.addItem ("60 dB", 1);
    rangeBox.addItem ("90 dB", 2);
    rangeBox.addItem ("120 dB", 3);
    rangeBox.setSelectedId (s.range.load() + 1, juce::dontSendNotification);
    rangeBox.onChange = [this] { audioProcessor.settings.range = rangeBox.getSelectedId() - 1; };
    addChildComponent (rangeBox);

    // channels: mix / left-right / mid-side
    channelsBox.addItem ("Mix (mono)", 1);
    channelsBox.addItem ("Left / Right", 2);
    channelsBox.addItem ("Mid / Side", 3);
    channelsBox.setSelectedId (s.stereoMode.load() + 1, juce::dontSendNotification);
    channelsBox.onChange = [this] { audioProcessor.settings.stereoMode = channelsBox.getSelectedId() - 1; };
    addChildComponent (channelsBox);

    // FFT size
    fftBox.addItem ("2048", 1);
    fftBox.addItem ("4096", 2);
    fftBox.addItem ("8192", 3);
    fftBox.addItem ("16384", 4);
    fftBox.setSelectedId (s.fftSizeIndex.load() + 1, juce::dontSendNotification);
    fftBox.onChange = [this] { audioProcessor.settings.fftSizeIndex = fftBox.getSelectedId() - 1; };
    addChildComponent (fftBox);

    // scope
    scopeToggle.setButtonText ("Show scope");
    scopeToggle.setToggleState (s.showScope.load(), juce::dontSendNotification);
    scopeToggle.onClick = [this] { audioProcessor.settings.showScope = scopeToggle.getToggleState(); repaint(); };
    addChildComponent (scopeToggle);

    scopeTimeBox.addItem ("20 ms", 1);
    scopeTimeBox.addItem ("50 ms", 2);
    scopeTimeBox.addItem ("100 ms", 3);
    scopeTimeBox.setSelectedId (s.scopeTime.load() + 1, juce::dontSendNotification);
    scopeTimeBox.onChange = [this] { audioProcessor.settings.scopeTime = scopeTimeBox.getSelectedId() - 1; };
    addChildComponent (scopeTimeBox);

    scopeFilterBox.addItem ("Low-pass 120 Hz", 1);
    scopeFilterBox.addItem ("Low-pass 200 Hz", 2);
    scopeFilterBox.addItem ("Full range", 3);
    scopeFilterBox.setSelectedId (s.scopeFilter.load() + 1, juce::dontSendNotification);
    scopeFilterBox.onChange = [this] { audioProcessor.settings.scopeFilter = scopeFilterBox.getSelectedId() - 1; };
    addChildComponent (scopeFilterBox);

    // loudness meter
    loudnessToggle.setButtonText ("Show loudness meter (LUFS)");
    loudnessToggle.setToggleState (s.showLoudness.load(), juce::dontSendNotification);
    loudnessToggle.onClick = [this] { audioProcessor.settings.showLoudness = loudnessToggle.getToggleState(); repaint(); };
    addChildComponent (loudnessToggle);

    updatePanelVisibility();

    setResizable (true, true);
    setResizeLimits (600, 470, 1600, 1000);
    setSize (800, 580);

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

// Shows the controls of the current page of the settings panel (or hides everything)
void SpectrumAnalyzerEditor::updatePanelVisibility()
{
    const bool open = settingsButton.getToggleState();

    auto show = [] (std::initializer_list<juce::Component*> list, bool visible)
    {
        for (auto* c : list)
            c->setVisible (visible);
    };

    show ({ &tabLook, &tabAnalyzer, &tabScope }, open);

    show ({ &modeBox, &swatches, &hueSlider, &swatchesStereoA, &swatchesB,
            &animateToggle, &fillBox, &thicknessSlider, &backgroundBox }, open && panelPage == 0);

    show ({ &peakToggle, &tiltBox, &speedBox, &rangeBox, &channelsBox, &fftBox }, open && panelPage == 1);

    show ({ &scopeToggle, &scopeTimeBox, &scopeFilterBox, &loudnessToggle }, open && panelPage == 2);

    resized();
    repaint();
}

// ---------------------------------------------------------------- layout

float SpectrumAnalyzerEditor::getStripHeight() const
{
    const auto& s = audioProcessor.settings;

    if (! (s.showScope.load() || s.showLoudness.load()))
        return 0.0f;

    const float available = (float) getHeight() - 32.0f;
    return juce::jlimit (112.0f, 240.0f, available * 0.34f);
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getPlotBounds() const
{
    auto bounds = getLocalBounds().toFloat();
    bounds.removeFromTop (32.0f);
    bounds.removeFromBottom (getStripHeight());

    auto plot = bounds.reduced (10.0f);
    plot.removeFromLeft (30.0f);
    plot.removeFromBottom (18.0f);
    return plot;
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getStripArea() const
{
    auto bounds = getLocalBounds().toFloat();
    auto strip = bounds.removeFromBottom (getStripHeight());
    return strip.reduced (stripBorder, 6.0f);
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getLoudnessBounds() const
{
    if (! audioProcessor.settings.showLoudness.load())
        return {};

    auto area = getStripArea();
    return area.removeFromRight (juce::jmin (loudnessWidth, area.getWidth()));
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getScopeBounds() const
{
    if (! audioProcessor.settings.showScope.load())
        return {};

    auto area = getStripArea();

    if (audioProcessor.settings.showLoudness.load())
        area.removeFromRight (loudnessWidth + 10.0f);

    area.removeFromLeft (30.0f);     // lines up with the analyzer plot
    return area;
}

juce::Rectangle<int> SpectrumAnalyzerEditor::getResetRect() const
{
    const auto inner = getLoudnessBounds().toNearestInt().reduced (12, 8);
    return juce::Rectangle<int> (inner.getRight() - 46, inner.getY(), 46, 16);
}

void SpectrumAnalyzerEditor::resized()
{
    settingsButton.setBounds (getWidth() - 116, 4, 106, 26);

    legendA = juce::Rectangle<int> (getWidth() - 116 - 190, 5, 84, 24);
    legendB = juce::Rectangle<int> (getWidth() - 116 - 100, 5, 92, 24);

    const int pageHeights[] = { 410, 328, 212 };
    panelBounds = juce::Rectangle<int> (getWidth() - 290, 38, 280, pageHeights[juce::jlimit (0, 2, panelPage)]);

    auto inner = panelBounds.reduced (14, 12);

    auto tabs = inner.removeFromTop (24);
    inner.removeFromTop (8);
    const int tabWidth = tabs.getWidth() / 3;
    tabLook.setBounds (tabs.removeFromLeft (tabWidth));
    tabAnalyzer.setBounds (tabs.removeFromLeft (tabWidth));
    tabScope.setBounds (tabs);

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

    // page 2: scope and loudness meter
    {
        auto col = inner;

        scopeToggle.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Scope time
        scopeTimeBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (8);

        nextLabel (col);                                // Scope filter
        scopeFilterBox.setBounds (col.removeFromTop (24));
        col.removeFromTop (12);

        loudnessToggle.setBounds (col.removeFromTop (24));
    }
}

// ---------------------------------------------------------------- mouse

void SpectrumAnalyzerEditor::mouseMove (const juce::MouseEvent& e)
{
    const bool overPanel = settingsButton.getToggleState() && panelBounds.contains (e.getPosition());
    const bool wasActive = hoverActive;

    hoverPos = e.position;
    hoverActive = getPlotBounds().contains (e.position) && ! overPanel;

    if (hoverActive && ! wasActive)
        lastReadoutX = -1.0f;

    const bool overLegend = audioProcessor.settings.stereoMode.load() != 0
                            && (legendA.contains (e.getPosition()) || legendB.contains (e.getPosition()));

    const bool overReset = audioProcessor.settings.showLoudness.load() && getResetRect().contains (e.getPosition());

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
    // loudness meter: "Reset" restarts the integrated loudness and the peak
    if (audioProcessor.settings.showLoudness.load() && getResetRect().contains (e.getPosition()))
    {
        audioProcessor.resetLoudnessRequested = true;
        return;
    }

    // legend: hide / show a curve
    if (audioProcessor.settings.stereoMode.load() == 0)
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

// Scope: low-passed mono waveform, aligned on a rising zero crossing so it stays still
void SpectrumAnalyzerEditor::updateScope()
{
    const auto& settings = audioProcessor.settings;
    const double fs = audioProcessor.getCurrentSampleRate();

    const int windowSamples = juce::jlimit (64, maxFftSize / 2,
                                            (int) std::lround (fs * 0.001 * scopeTimeMs[juce::jlimit (0, 2, settings.scopeTime.load())]));
    const int preRoll = juce::jlimit (1, maxFftSize / 4, (int) std::lround (fs * 0.05));
    const int extra   = juce::jlimit (1, maxFftSize - windowSamples - preRoll, (int) std::lround (fs * 0.05));
    const int total   = preRoll + extra + windowSamples;

    audioProcessor.copyLatestSamples (bufL.data(), 0, total);
    audioProcessor.copyLatestSamples (bufR.data(), 1, total);

    for (int i = 0; i < total; ++i)
        scopeWork[(size_t) i] = 0.5f * (bufL[(size_t) i] + bufR[(size_t) i]);

    const float cutoff = scopeFilterHz[juce::jlimit (0, 2, settings.scopeFilter.load())];

    if (cutoff > 0.0f)
    {
        Biquad f1, f2;
        designLowpass (f1, fs, (double) cutoff, 0.70710678);
        f2 = f1;

        for (int i = 0; i < total; ++i)
            scopeWork[(size_t) i] = (float) f2.process (f1.process ((double) scopeWork[(size_t) i]));
    }

    // trigger: the latest rising zero crossing that still leaves room for the whole window
    int start = total - windowSamples;
    for (int c = total - windowSamples; c >= preRoll; --c)
    {
        if (scopeWork[(size_t) c - 1] < 0.0f && scopeWork[(size_t) c] >= 0.0f)
        {
            start = c;
            break;
        }
    }

    std::copy (scopeWork.begin() + start, scopeWork.begin() + start + windowSamples, scopeData.begin());
    scopeCount = windowSamples;
}

void SpectrumAnalyzerEditor::timerCallback()
{
    const auto& settings = audioProcessor.settings;

    // FFT size changed in the settings?
    const int wantedSize = 2048 << juce::jlimit (0, 3, settings.fftSizeIndex.load());
    if (wantedSize != fftSize)
        rebuildFft (wantedSize);

    // channel mode changed? start curve B from scratch
    const int mode = juce::jlimit (0, 2, settings.stereoMode.load());
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

    // 3. scope
    if (settings.showScope.load())
        updateScope();

    // 4. animated rainbow: the colours slide slowly
    if (settings.animate.load() && settings.colourMode.load() == 1)
        hueOffset = std::fmod (hueOffset + 0.0025f, 1.0f);

    // 5. hover readout: levels are smoothed and the text only changes ~5 times per second
    if (hoverActive)
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

// Colour along the horizontal axis of an area (left -> right), for the Mix curve and the scope
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

void SpectrumAnalyzerEditor::drawScope (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto& s = audioProcessor.settings;

    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.fillRoundedRectangle (area, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (area, 6.0f, 1.0f);

    // grid: centre line and +-0.5
    const float cy = area.getCentreY();
    const float half = area.getHeight() * 0.5f - 6.0f;

    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.drawHorizontalLine ((int) cy, area.getX(), area.getRight());
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawHorizontalLine ((int) (cy - half * 0.5f), area.getX(), area.getRight());
    g.drawHorizontalLine ((int) (cy + half * 0.5f), area.getX(), area.getRight());

    // title
    const int timeIndex = juce::jlimit (0, 2, s.scopeTime.load());
    const int filterIndex = juce::jlimit (0, 2, s.scopeFilter.load());
    juce::String title = "SCOPE   " + juce::String (scopeTimeMs[timeIndex]) + " ms   ";
    title += (filterIndex == 2) ? juce::String ("full range")
                                : "low-pass " + juce::String ((int) scopeFilterHz[filterIndex]) + " Hz";

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (11.0f);
    g.drawText (title, area.toNearestInt().reduced (8, 3).removeFromTop (14), juce::Justification::centredLeft);

    // waveform
    if (scopeCount >= 2)
    {
        juce::Path wave;
        for (int i = 0; i < scopeCount; ++i)
        {
            const float x = area.getX() + area.getWidth() * (float) i / (float) (scopeCount - 1);
            const float y = cy - juce::jlimit (-1.0f, 1.0f, scopeData[(size_t) i]) * half;

            if (i == 0) wave.startNewSubPath (x, y);
            else        wave.lineTo (x, y);
        }

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());
        g.setGradientFill (makeHorizontalGradient (area, 1.0f));
        g.strokePath (wave, juce::PathStrokeType (juce::jmax (1.2f, s.lineThickness.load())));
    }
}

void SpectrumAnalyzerEditor::drawLoudness (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (area, 8.0f, 1.0f);

    auto inner = area.toNearestInt().reduced (12, 8);

    // header: title and Reset
    auto header = inner.removeFromTop (16);
    g.setFont (11.0f);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.drawText ("LOUDNESS", header, juce::Justification::centredLeft);

    const auto resetRect = getResetRect();
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.fillRoundedRectangle (resetRect.toFloat(), 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.drawText ("Reset", resetRect, juce::Justification::centred);

    auto row = [&] (const juce::String& label, const juce::String& value, juce::Colour valueColour)
    {
        auto r = inner.removeFromTop (16);
        g.setFont (11.0f);
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawText (label, r, juce::Justification::centredLeft);
        g.setFont (12.0f);
        g.setColour (valueColour);
        g.drawText (value, r, juce::Justification::centredRight);
    };

    const float peak = audioProcessor.samplePeakDb.load();

    row ("Momentary",  loudnessText (audioProcessor.loudnessMomentary.load(), "LUFS"),  juce::Colours::white);
    row ("Short-term", loudnessText (audioProcessor.loudnessShortTerm.load(), "LUFS"),  juce::Colours::white);
    row ("Integrated", loudnessText (audioProcessor.loudnessIntegrated.load(), "LUFS"), juce::Colours::white);
    row ("Peak",       loudnessText (peak, "dBFS"), peak >= -0.1f ? juce::Colour (0xffff5a5a) : juce::Colours::white);
}

void SpectrumAnalyzerEditor::paint (juce::Graphics& g)
{
    const auto& s = audioProcessor.settings;
    const auto bg = backgrounds[juce::jlimit (0, 3, s.background.load())];
    g.fillAll (bg);

    const auto plot = getPlotBounds();
    const auto header = getLocalBounds().toFloat().removeFromTop (32.0f);
    const int mode = juce::jlimit (0, 2, s.stereoMode.load());
    const bool stereo = (mode != 0);

    // in the two-curve modes both curves use their own pair of colours
    const auto colourA = juce::Colour (s.colourStereoA.load());
    const auto colourB = juce::Colour (s.colourB.load());

    // signature
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (14.0f);
    g.drawText ("Made by Mugen", header.toNearestInt().withTrimmedLeft (14), juce::Justification::centredLeft);

    // legend (two-curve modes): click a name to hide / show that curve
    if (stereo)
    {
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

    // bottom strip: scope and loudness meter
    if (getStripHeight() > 0.0f)
    {
        const auto strip = getLocalBounds().toFloat().removeFromBottom (getStripHeight());
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawHorizontalLine ((int) strip.getY(), 0.0f, (float) getWidth());

        if (s.showScope.load())
            drawScope (g, getScopeBounds());

        if (s.showLoudness.load())
            drawLoudness (g, getLoudnessBounds());
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

    // settings panel (background and texts; the controls are drawn on top)
    if (settingsButton.getToggleState())
    {
        g.setColour (juce::Colour (0xf01a2027));
        g.fillRoundedRectangle (panelBounds.toFloat(), 10.0f);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (panelBounds.toFloat(), 10.0f, 1.0f);

        // which labels belong to the current page
        const int firstLabel = (panelPage == 0) ? 0 : (panelPage == 1 ? 6 : 11);
        const int lastLabel  = (panelPage == 0) ? 5 : (panelPage == 1 ? 10 : 12);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (12.0f);
        for (int i = firstLabel; i <= lastLabel; ++i)
        {
            juce::String text = labelTexts[(size_t) i];
            if (i == 0 && stereo && s.colourMode.load() == 1)
                text = "Color mode (rainbow: Mix only)";

            g.drawText (text, labelRects[(size_t) i], juce::Justification::centredLeft);
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
