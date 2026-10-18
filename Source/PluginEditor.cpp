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
}

SpectrumAnalyzerEditor::SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      fft (SpectrumAnalyzerProcessor::fftOrder),
      window (fftSize, juce::dsp::WindowingFunction<float>::hann)
{
    smoothed.fill (floorDb);
    peaks.fill (floorDb);
    auto& s = audioProcessor.settings;

    labelTexts = { juce::String ("Color mode"), juce::String ("Color"), juce::String ("Fill"),
                   juce::String ("Line thickness"), juce::String ("Background"),
                   juce::String ("Tilt"), juce::String ("Speed"), juce::String ("Range") };

    // "Settings" button with gear icon
    settingsButton.onClick = [this] { setPanelVisible (settingsButton.getToggleState()); };
    addAndMakeVisible (settingsButton);

    // colour mode: single colour / rainbow
    modeBox.addItem ("Single color", 1);
    modeBox.addItem ("Rainbow", 2);
    modeBox.setSelectedId (s.colourMode.load() + 1, juce::dontSendNotification);
    modeBox.onChange = [this]
    {
        audioProcessor.settings.colourMode = modeBox.getSelectedId() - 1;
        updateSwatchSelection();
    };
    addChildComponent (modeBox);

    // colours to pick from
    updateSwatchSelection();
    swatches.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.singleColour = c.getARGB();
        hueSlider.setValue ((double) c.getHue(), juce::dontSendNotification);
        modeBox.setSelectedId (1, juce::sendNotification);
    };
    addChildComponent (swatches);

    // any colour, using hue (the rainbow bar is drawn behind the slider)
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

    setPanelVisible (false);

    setResizable (true, true);
    setResizeLimits (580, 380, 1600, 900);
    setSize (780, 440);

    startTimerHz (30);   // refresh 30 times per second
}

SpectrumAnalyzerEditor::~SpectrumAnalyzerEditor()
{
    stopTimer();
}

// In rainbow mode no colour circle stays selected
void SpectrumAnalyzerEditor::updateSwatchSelection()
{
    if (audioProcessor.settings.colourMode.load() == 1)
        swatches.clearSelection();
    else
        swatches.setSelectedFromColour (juce::Colour (audioProcessor.settings.singleColour.load()));
}

void SpectrumAnalyzerEditor::setPanelVisible (bool shouldBeVisible)
{
    const std::array<juce::Component*, 11> controls = { &modeBox, &swatches, &hueSlider, &animateToggle,
                                                       &fillBox, &thicknessSlider, &backgroundBox,
                                                       &peakToggle, &tiltBox, &speedBox, &rangeBox };
    for (auto* c : controls)
        c->setVisible (shouldBeVisible);

    repaint();
}

juce::Rectangle<float> SpectrumAnalyzerEditor::getPlotBounds() const
{
    auto bounds = getLocalBounds().toFloat();
    bounds.removeFromTop (32.0f);
    auto plot = bounds.reduced (10.0f);
    plot.removeFromLeft (30.0f);
    plot.removeFromBottom (18.0f);
    return plot;
}

void SpectrumAnalyzerEditor::resized()
{
    settingsButton.setBounds (getWidth() - 116, 4, 106, 26);

    panelBounds = juce::Rectangle<int> (getWidth() - 530, 38, 520, 306);
    auto inner = panelBounds.reduced (14, 12);
    auto left = inner.removeFromLeft (236);
    inner.removeFromLeft (20);
    auto right = inner;

    int labelIndex = 0;
    auto nextLabel = [&] (juce::Rectangle<int>& column)
    {
        labelRects[(size_t) labelIndex++] = column.removeFromTop (16);
    };

    // left column: look
    nextLabel (left);                               // Color mode
    modeBox.setBounds (left.removeFromTop (24));
    left.removeFromTop (8);

    nextLabel (left);                               // Color
    swatches.setBounds (left.removeFromTop (24));
    left.removeFromTop (2);
    hueSlider.setBounds (left.removeFromTop (18));
    left.removeFromTop (8);

    animateToggle.setBounds (left.removeFromTop (24));
    left.removeFromTop (8);

    nextLabel (left);                               // Fill
    fillBox.setBounds (left.removeFromTop (24));
    left.removeFromTop (8);

    nextLabel (left);                               // Line thickness
    thicknessSlider.setBounds (left.removeFromTop (18));
    left.removeFromTop (8);

    nextLabel (left);                               // Background
    backgroundBox.setBounds (left.removeFromTop (24));

    // right column: analyzer
    peakToggle.setBounds (right.removeFromTop (24));
    right.removeFromTop (8);

    nextLabel (right);                              // Tilt
    tiltBox.setBounds (right.removeFromTop (24));
    right.removeFromTop (8);

    nextLabel (right);                              // Speed
    speedBox.setBounds (right.removeFromTop (24));
    right.removeFromTop (8);

    nextLabel (right);                              // Range
    rangeBox.setBounds (right.removeFromTop (24));
}

void SpectrumAnalyzerEditor::mouseMove (const juce::MouseEvent& e)
{
    const bool overPanel = settingsButton.getToggleState() && panelBounds.contains (e.getPosition());
    const bool wasActive = hoverActive;

    hoverPos = e.position;
    hoverActive = getPlotBounds().contains (e.position) && ! overPanel;

    if (hoverActive && ! wasActive)
        lastReadoutX = -1.0f;

    setMouseCursor (hoverActive ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
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

void SpectrumAnalyzerEditor::timerCallback()
{
    const auto& settings = audioProcessor.settings;

    // 1. get the latest audio samples
    audioProcessor.copyLatestSamples (fftData.data());
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    // 2. apply the window (Hann) and do the FFT
    window.multiplyWithWindowingTable (fftData.data(), fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    // 3. convert to dB and smooth (speed setting), update peaks
    const int speedIndex = juce::jlimit (0, 2, settings.speed.load());
    const float attack  = attackTable[speedIndex];
    const float release = releaseTable[speedIndex];
    const bool peaksOn = settings.peakHold.load();

    for (int i = 0; i < numBins; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] * 4.0f / (float) fftSize, floorDb);
        float& s = smoothed[(size_t) i];
        s += (db - s) * (db > s ? attack : release);

        float& pk = peaks[(size_t) i];
        pk = peaksOn ? std::max (s, pk - peakFallDb) : s;
    }

    // 4. animated rainbow: the colours slide slowly
    if (settings.animate.load() && settings.colourMode.load() == 1)
        hueOffset = std::fmod (hueOffset + 0.0025f, 1.0f);

    // 5. hover readout: level is smoothed and the text only changes ~5 times per second
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
            const float level = levelAt (smoothed, f0, f1, binHz);

            const bool moved = std::abs (x - lastReadoutX) > 0.5f;
            lastReadoutX = x;

            if (moved)
                readoutLevel = level;                             // mouse moved: respond at once
            else
                readoutLevel += (level - readoutLevel) * 0.1f;    // mouse still: steady value

            if (moved || ++readoutTick >= 6)
            {
                readoutTick = 0;
                readoutLine1 = frequencyText (f0) + "   " + noteName (f0);
                readoutLine2 = juce::String (readoutLevel, 1) + " dB";
            }
        }
    }

    repaint();
}

// Level (in dB) for a frequency range, including the tilt setting
float SpectrumAnalyzerEditor::levelAt (const std::array<float, numBins>& data, float f0, float f1, float binHz) const
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

// Colour of the curve along the frequency axis (left -> right)
juce::ColourGradient SpectrumAnalyzerEditor::makeHorizontalGradient (juce::Rectangle<float> plot, float alpha) const
{
    const auto& s = audioProcessor.settings;

    if (s.colourMode.load() == 0)
    {
        const auto c = juce::Colour (s.singleColour.load()).withAlpha (alpha);
        return juce::ColourGradient (c, plot.getX(), 0.0f, c, plot.getRight(), 0.0f, false);
    }

    const int stops = 12;
    const float offset = hueOffset;
    auto colourAt = [offset, alpha] (float t)
    {
        return juce::Colour::fromHSV (std::fmod (offset + 0.8f * t, 1.0f), 0.7f, 1.0f, alpha);
    };

    juce::ColourGradient grad (colourAt (0.0f), plot.getX(), 0.0f,
                               colourAt (1.0f), plot.getRight(), 0.0f, false);
    for (int i = 1; i < stops; ++i)
        grad.addColour ((double) i / (double) stops, colourAt ((float) i / (float) stops));

    return grad;
}

void SpectrumAnalyzerEditor::paint (juce::Graphics& g)
{
    const auto& s = audioProcessor.settings;
    const auto bg = backgrounds[juce::jlimit (0, 3, s.background.load())];
    g.fillAll (bg);

    const auto plot = getPlotBounds();
    const auto header = getLocalBounds().toFloat().removeFromTop (32.0f);

    // signature
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (14.0f);
    g.drawText ("Made by Mugen", header.toNearestInt().withTrimmedLeft (14), juce::Justification::centredLeft);

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

    auto buildCurve = [&] (const std::array<float, numBins>& data)
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

    // spectrum curve
    juce::Path curve, filled;

    if (width >= 2)
    {
        curve = buildCurve (smoothed);

        filled = curve;
        filled.lineTo (plot.getRight(), plot.getBottom());
        filled.lineTo (plot.getX(), plot.getBottom());
        filled.closeSubPath();

        // fill under the curve
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());

        const int fillStyle = s.fillStyle.load();

        if (fillStyle == 0)          // gradient: strong colour on top, fades into the background
        {
            g.setGradientFill (makeHorizontalGradient (plot, 0.75f));
            g.fillPath (filled);

            juce::Graphics::ScopedSaveState inner (g);
            g.reduceClipRegion (filled, juce::AffineTransform());
            g.setGradientFill (juce::ColourGradient (bg.withAlpha (0.0f), 0.0f, plot.getY(),
                                                     bg.withAlpha (0.96f), 0.0f, plot.getBottom(), false));
            g.fillRect (plot);
        }
        else if (fillStyle == 1)     // solid
        {
            g.setGradientFill (makeHorizontalGradient (plot, 0.35f));
            g.fillPath (filled);
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

    // curve line
    if (width >= 2)
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());
        g.setGradientFill (makeHorizontalGradient (plot, 1.0f));
        g.strokePath (curve, juce::PathStrokeType (s.lineThickness.load()));
    }

    // peak hold line
    if (width >= 2 && s.peakHold.load())
    {
        const auto peakCurve = buildCurve (peaks);

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.strokePath (peakCurve, juce::PathStrokeType (1.0f));
    }

    // hover readout: frequency, note and level
    if (hoverActive && width >= 2)
    {
        const float x = juce::jlimit (plot.getX(), plot.getRight(), hoverPos.x);
        const float t0 = (x - plot.getX()) / plot.getWidth();
        const float t1 = t0 + 1.0f / (float) width;
        const float f0 = minFreq * std::pow (ratio, t0);
        const float f1 = minFreq * std::pow (ratio, t1);
        const float y = juce::jlimit (plot.getY(), plot.getBottom(), yForDb (levelAt (smoothed, f0, f1, binHz)));

        // vertical line and dot on the curve
        g.setColour (juce::Colours::white.withAlpha (0.3f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (juce::Colours::white);
        g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);

        // box follows the mouse (not the curve), so it does not jump
        juce::Rectangle<float> box (0.0f, 0.0f, 134.0f, 40.0f);
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

        // divider between the two columns
        const float dividerX = (float) (panelBounds.getX() + 14 + 236 + 10);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawVerticalLine ((int) dividerX, (float) panelBounds.getY() + 14.0f, (float) panelBounds.getBottom() - 14.0f);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (12.0f);
        for (size_t i = 0; i < labelRects.size(); ++i)
            g.drawText (labelTexts[i], labelRects[i], juce::Justification::centredLeft);

        // rainbow bar behind the hue slider
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
