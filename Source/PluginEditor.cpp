#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float minFreq = 20.0f;
    constexpr float maxFreq = 20000.0f;
    constexpr float minDb   = -90.0f;
    constexpr float maxDb   = 0.0f;

    // Texto com acentos (UTF-8)
    juce::String pt (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    const juce::Colour backgrounds[] = { juce::Colour (0xff101418),   // grafite
                                         juce::Colour (0xff000000),   // preto
                                         juce::Colour (0xff0b1226),   // azul-noite
                                         juce::Colour (0xff170f26) }; // roxo escuro
}

SpectrumAnalyzerEditor::SpectrumAnalyzerEditor (SpectrumAnalyzerProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      fft (SpectrumAnalyzerProcessor::fftOrder),
      window (fftSize, juce::dsp::WindowingFunction<float>::hann)
{
    smoothed.fill (minDb);
    auto& s = audioProcessor.settings;

    labelTexts = { pt ("Modo de cor"), pt ("Cor"), pt ("Preenchimento"),
                   pt ("Espessura da linha"), pt ("Fundo") };

    // botao "Definicoes"
    settingsButton.setButtonText (pt ("Defini" "\xc3\xa7" "\xc3\xb5" "es"));
    settingsButton.setClickingTogglesState (true);
    settingsButton.onClick = [this] { setPanelVisible (settingsButton.getToggleState()); };
    addAndMakeVisible (settingsButton);

    // modo de cor: cor unica / arco-iris
    modeBox.addItem (pt ("Cor " "\xc3\xba" "nica"), 1);
    modeBox.addItem (pt ("Arco-" "\xc3\xad" "ris"), 2);
    modeBox.setSelectedId (s.colourMode.load() + 1, juce::dontSendNotification);
    modeBox.onChange = [this] { audioProcessor.settings.colourMode = modeBox.getSelectedId() - 1; };
    addChildComponent (modeBox);

    // cores para escolher
    swatches.setSelectedFromColour (juce::Colour (s.singleColour.load()));
    swatches.onPick = [this] (juce::Colour c)
    {
        audioProcessor.settings.singleColour = c.getARGB();
        hueSlider.setValue ((double) c.getHue(), juce::dontSendNotification);
        modeBox.setSelectedId (1, juce::sendNotification);
    };
    addChildComponent (swatches);

    // qualquer cor, pelo "matiz"
    hueSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    hueSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    hueSlider.setRange (0.0, 1.0, 0.001);
    hueSlider.setValue ((double) juce::Colour (s.singleColour.load()).getHue(), juce::dontSendNotification);
    hueSlider.onValueChange = [this]
    {
        audioProcessor.settings.singleColour =
            juce::Colour::fromHSV ((float) hueSlider.getValue(), 0.75f, 1.0f, 1.0f).getARGB();
        swatches.clearSelection();
        modeBox.setSelectedId (1, juce::sendNotification);
    };
    addChildComponent (hueSlider);

    // animar arco-iris
    animateToggle.setButtonText (pt ("Animar arco-" "\xc3\xad" "ris"));
    animateToggle.setToggleState (s.animate.load(), juce::dontSendNotification);
    animateToggle.onClick = [this] { audioProcessor.settings.animate = animateToggle.getToggleState(); };
    addChildComponent (animateToggle);

    // estilo de preenchimento
    fillBox.addItem ("Gradiente", 1);
    fillBox.addItem (pt ("S" "\xc3\xb3" "lido"), 2);
    fillBox.addItem (pt ("S" "\xc3\xb3" " linha"), 3);
    fillBox.setSelectedId (s.fillStyle.load() + 1, juce::dontSendNotification);
    fillBox.onChange = [this] { audioProcessor.settings.fillStyle = fillBox.getSelectedId() - 1; };
    addChildComponent (fillBox);

    // espessura da linha
    thicknessSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    thicknessSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    thicknessSlider.setRange (0.5, 4.0, 0.1);
    thicknessSlider.setValue ((double) s.lineThickness.load(), juce::dontSendNotification);
    thicknessSlider.onValueChange = [this]
    {
        audioProcessor.settings.lineThickness = (float) thicknessSlider.getValue();
    };
    addChildComponent (thicknessSlider);

    // fundo
    backgroundBox.addItem ("Grafite", 1);
    backgroundBox.addItem ("Preto", 2);
    backgroundBox.addItem ("Azul-noite", 3);
    backgroundBox.addItem ("Roxo escuro", 4);
    backgroundBox.setSelectedId (s.background.load() + 1, juce::dontSendNotification);
    backgroundBox.onChange = [this] { audioProcessor.settings.background = backgroundBox.getSelectedId() - 1; };
    addChildComponent (backgroundBox);

    setPanelVisible (false);

    setResizable (true, true);
    setResizeLimits (560, 380, 1600, 900);
    setSize (760, 440);

    startTimerHz (30);   // atualiza 30 vezes por segundo
}

SpectrumAnalyzerEditor::~SpectrumAnalyzerEditor()
{
    stopTimer();
}

void SpectrumAnalyzerEditor::setPanelVisible (bool shouldBeVisible)
{
    const std::array<juce::Component*, 7> controls = { &modeBox, &swatches, &hueSlider, &animateToggle,
                                                       &fillBox, &thicknessSlider, &backgroundBox };
    for (auto* c : controls)
        c->setVisible (shouldBeVisible);

    repaint();
}

void SpectrumAnalyzerEditor::resized()
{
    settingsButton.setBounds (getWidth() - 122, 5, 112, 22);

    panelBounds = juce::Rectangle<int> (getWidth() - 266, 38, 256, 306);
    auto r = panelBounds.reduced (14, 12);

    int labelIndex = 0;
    auto nextLabel = [&] { labelRects[(size_t) labelIndex++] = r.removeFromTop (16); };

    nextLabel();                                   // Modo de cor
    modeBox.setBounds (r.removeFromTop (24));
    r.removeFromTop (8);

    nextLabel();                                   // Cor
    swatches.setBounds (r.removeFromTop (24));
    r.removeFromTop (2);
    hueSlider.setBounds (r.removeFromTop (18));
    r.removeFromTop (8);

    animateToggle.setBounds (r.removeFromTop (24));
    r.removeFromTop (8);

    nextLabel();                                   // Preenchimento
    fillBox.setBounds (r.removeFromTop (24));
    r.removeFromTop (8);

    nextLabel();                                   // Espessura da linha
    thicknessSlider.setBounds (r.removeFromTop (18));
    r.removeFromTop (8);

    nextLabel();                                   // Fundo
    backgroundBox.setBounds (r.removeFromTop (24));
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

    // 4. arco-iris animado: as cores deslizam devagar
    const auto& settings = audioProcessor.settings;
    if (settings.animate.load() && settings.colourMode.load() == 1)
        hueOffset = std::fmod (hueOffset + 0.0025f, 1.0f);

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

// Cor da curva ao longo do eixo das frequencias (esquerda -> direita)
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

    auto bounds = getLocalBounds().toFloat();
    auto header = bounds.removeFromTop (32.0f);
    auto plot = bounds.reduced (10.0f);
    plot.removeFromLeft (30.0f);
    plot.removeFromBottom (18.0f);

    // assinatura
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (14.0f);
    g.drawText ("Made by Mugen", header.toNearestInt().withTrimmedLeft (14), juce::Justification::centredLeft);

    const auto xForFreq = [&] (float f)
    {
        return plot.getX() + plot.getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq);
    };
    const auto yForDb = [&] (float db)
    {
        return plot.getBottom() - plot.getHeight() * (db - minDb) / (maxDb - minDb);
    };

    // curva do espetro
    const int width = (int) plot.getWidth();
    juce::Path curve, filled;

    if (width >= 2)
    {
        const float binHz = (float) (audioProcessor.getCurrentSampleRate() / (double) fftSize);
        const float ratio = maxFreq / minFreq;

        for (int px = 0; px <= width; ++px)
        {
            const float f0 = minFreq * std::pow (ratio, (float) px / (float) width);
            const float f1 = minFreq * std::pow (ratio, (float) (px + 1) / (float) width);
            const float x = plot.getX() + (float) px;
            const float y = yForDb (levelBetween (f0, f1, binHz));

            if (px == 0) curve.startNewSubPath (x, y);
            else         curve.lineTo (x, y);
        }

        filled = curve;
        filled.lineTo (plot.getRight(), plot.getBottom());
        filled.lineTo (plot.getX(), plot.getBottom());
        filled.closeSubPath();

        // preenchimento por baixo da curva
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());

        const int fillStyle = s.fillStyle.load();

        if (fillStyle == 0)          // gradiente: cor forte em cima, desvanece para o fundo
        {
            g.setGradientFill (makeHorizontalGradient (plot, 0.75f));
            g.fillPath (filled);

            juce::Graphics::ScopedSaveState inner (g);
            g.reduceClipRegion (filled, juce::AffineTransform());
            g.setGradientFill (juce::ColourGradient (bg.withAlpha (0.0f), 0.0f, plot.getY(),
                                                     bg.withAlpha (0.96f), 0.0f, plot.getBottom(), false));
            g.fillRect (plot);
        }
        else if (fillStyle == 1)     // solido
        {
            g.setGradientFill (makeHorizontalGradient (plot, 0.35f));
            g.fillPath (filled);
        }
    }

    // grelha de frequencias
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

    // linha da curva
    if (width >= 2)
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());
        g.setGradientFill (makeHorizontalGradient (plot, 1.0f));
        g.strokePath (curve, juce::PathStrokeType (s.lineThickness.load()));
    }

    // painel de definicoes (fundo e textos; os controlos desenham-se por cima)
    if (settingsButton.getToggleState())
    {
        g.setColour (juce::Colour (0xf01a2027));
        g.fillRoundedRectangle (panelBounds.toFloat(), 10.0f);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (panelBounds.toFloat(), 10.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (12.0f);
        for (size_t i = 0; i < labelRects.size(); ++i)
            g.drawText (labelTexts[i], labelRects[i], juce::Justification::centredLeft);
    }
}
