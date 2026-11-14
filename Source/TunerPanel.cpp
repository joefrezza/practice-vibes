#include "TunerPanel.h"
#include "Theme.h"

namespace pv
{

namespace
{
    const juce::Colour inTuneColour { 0xff5cc871 };
    const juce::Colour offColour    { 0xffff9f43 };

    constexpr float inTuneCents = 3.0f;

    // Standard tuning, low to high (MIDI notes) and the string numbers guitarists use.
    constexpr std::array<int, 6> standardStrings { 40, 45, 50, 55, 59, 64 };
}

TunerPanel::TunerPanel (AppEngine& e)
    : engine (e), tuner (*e.tuner)
{
    referenceSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    referenceSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    referenceSlider.setRange (Tuner::minReference, Tuner::maxReference, 1.0);
    referenceSlider.setValue (tuner.referenceA4, juce::dontSendNotification);
    referenceSlider.setDoubleClickReturnValue (true, 440.0);
    referenceSlider.onValueChange = [this]
    {
        tuner.referenceA4 = (float) referenceSlider.getValue();
        referenceLabel.setText ("A4 = " + juce::String ((int) tuner.referenceA4) + " Hz", juce::dontSendNotification);
        engine.saveTunerSettings();
        repaint();
    };
    referenceLabel.setText ("A4 = " + juce::String ((int) tuner.referenceA4) + " Hz", juce::dontSendNotification);
    referenceLabel.setTooltip ("Reference pitch. Double-click the slider for 440 Hz.");
    addAndMakeVisible (referenceSlider);
    addAndMakeVisible (referenceLabel);

    muteToggle.setToggleState (tuner.muteWhileTuning, juce::dontSendNotification);
    muteToggle.onClick = [this]
    {
        tuner.muteWhileTuning = muteToggle.getToggleState();
        engine.saveTunerSettings();
        applyMute();
    };
    addAndMakeVisible (muteToggle);

    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    addAndMakeVisible (hintLabel);

    tuner.clear();
    tuner.active = true;
    applyMute();
    startTimerHz (30);
}

TunerPanel::~TunerPanel()
{
    stopTimer();
    tuner.active = false;
    engine.chain->getOutputStage().silenced = false;
}

void TunerPanel::applyMute()
{
    engine.chain->getOutputStage().silenced = tuner.muteWhileTuning;
}

//==============================================================================
void TunerPanel::timerCallback()
{
    const auto reading = tuner.read();
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;

    inputLevel = reading.level;
    shownFrequency = smoother.add (reading, now);

    if (shownFrequency > 0.0f)
    {
        const auto newNote = NoteInfo::fromFrequency (shownFrequency, tuner.referenceA4);

        // Jump to a new note, ease within the same one (the needle should not twitch).
        needleCents = newNote.midiNote != note.midiNote ? newNote.cents
                                                        : needleCents + 0.35f * (newNote.cents - needleCents);
        note = newNote;
        hintLabel.setText ({}, juce::dontSendNotification);
    }
    else
    {
        hintLabel.setText (inputLevel < PitchDetector::minLevel ? "Play a single string (no input signal)"
                                                                : "Play a single string and let it ring",
                           juce::dontSendNotification);
    }

    repaint (noteArea.getUnion (meterArea).getUnion (stringsArea));
}

//==============================================================================
void TunerPanel::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    const bool hasNote = shownFrequency > 0.0f && note.midiNote >= 0;
    const bool inTune = hasNote && std::abs (needleCents) <= inTuneCents;
    const auto noteColour = ! hasNote ? juce::Colours::white.withAlpha (0.25f)
                                      : inTune ? inTuneColour : juce::Colours::white;

    // Note name with its octave; flat / sharp arrows either side.
    auto area = noteArea.toFloat();
    auto centre = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 260.0f), area.getHeight());
    const auto bigSize = juce::jmin (area.getHeight() * 0.8f, 130.0f);

    g.setColour (noteColour);
    g.setFont (juce::FontOptions (bigSize).withStyle ("Bold"));
    g.drawText (hasNote ? note.name : "-", centre, juce::Justification::centred);

    if (hasNote)
    {
        const auto nameWidth = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), note.name);
        g.setFont (juce::FontOptions (bigSize * 0.3f));
        g.drawText (juce::String (note.octave), centre.withTrimmedLeft (centre.getWidth() * 0.5f + nameWidth * 0.5f + 6.0f)
                                                       .withTrimmedTop (bigSize * 0.35f),
                    juce::Justification::centredLeft);

        auto arrow = [&] (juce::Rectangle<float> r, bool pointRight, bool lit)
        {
            juce::Path p;
            const auto c = r.getCentre();
            const auto s = juce::jmin (r.getWidth(), r.getHeight()) * 0.25f;

            if (pointRight)
                p.addTriangle (c.x - s, c.y - s, c.x - s, c.y + s, c.x + s, c.y);
            else
                p.addTriangle (c.x + s, c.y - s, c.x + s, c.y + s, c.x - s, c.y);

            g.setColour (lit ? offColour : juce::Colours::white.withAlpha (0.06f));
            g.fillPath (p);
        };

        // Flat: tune up (arrow points right, towards the centre); sharp: tune down.
        arrow (area.withRight (centre.getX()).withTrimmedLeft (area.getWidth() * 0.15f), true, ! inTune && needleCents < 0.0f);
        arrow (area.withLeft (centre.getRight()).withTrimmedRight (area.getWidth() * 0.15f), false, ! inTune && needleCents > 0.0f);
    }

    paintMeter (g, meterArea.toFloat());
    paintStrings (g, stringsArea.toFloat());
}

void TunerPanel::paintMeter (juce::Graphics& g, juce::Rectangle<float> area)
{
    const bool hasNote = shownFrequency > 0.0f;
    auto scale = area.removeFromTop (area.getHeight() * 0.62f).reduced (12.0f, 0.0f);
    const auto x = [&] (float cents) { return scale.getX() + (cents + 50.0f) / 100.0f * scale.getWidth(); };

    // In-tune zone.
    g.setColour (inTuneColour.withAlpha (0.18f));
    g.fillRect (juce::Rectangle<float> (x (-inTuneCents), scale.getY(), x (inTuneCents) - x (-inTuneCents), scale.getHeight()));

    // Ticks every 10 cents, labelled every 25... and the centre.
    g.setFont (juce::FontOptions (12.0f));

    for (int c = -50; c <= 50; c += 10)
    {
        const auto tall = c == 0 ? 1.0f : c % 50 == 0 ? 0.7f : 0.45f;
        g.setColour (juce::Colours::white.withAlpha (c == 0 ? 0.8f : 0.35f));
        g.fillRect (juce::Rectangle<float> (x ((float) c) - 1.0f, scale.getBottom() - scale.getHeight() * tall, 2.0f, scale.getHeight() * tall));
    }

    // Needle.
    if (hasNote)
    {
        const auto cents = juce::jlimit (-50.0f, 50.0f, needleCents);
        const auto inTune = std::abs (cents) <= inTuneCents;
        g.setColour (inTune ? inTuneColour : offColour);
        g.fillRoundedRectangle (juce::Rectangle<float> (x (cents) - 3.0f, scale.getY() - 4.0f, 6.0f, scale.getHeight() + 8.0f), 3.0f);
    }

    // Readout: cents and frequency.
    auto text = area.reduced (12.0f, 2.0f);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("-50", text, juce::Justification::topLeft);
    g.drawText ("+50", text, juce::Justification::topRight);

    if (hasNote)
    {
        const auto cents = juce::roundToInt (needleCents);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (18.0f));
        g.drawText ((cents > 0 ? "+" : "") + juce::String (cents) + " cents   " + juce::String (shownFrequency, 1) + " Hz",
                    text, juce::Justification::centred);
    }
}

void TunerPanel::paintStrings (juce::Graphics& g, juce::Rectangle<float> area)
{
    const bool hasNote = shownFrequency > 0.0f;
    const auto cellWidth = area.getWidth() / (float) standardStrings.size();
    const auto refNote = NoteInfo::fromFrequency (shownFrequency, tuner.referenceA4);

    for (size_t i = 0; i < standardStrings.size(); ++i)
    {
        auto cell = juce::Rectangle<float> (area.getX() + cellWidth * (float) i, area.getY(), cellWidth, area.getHeight()).reduced (4.0f);
        const auto midi = standardStrings[i];
        const bool lit = hasNote && refNote.midiNote == midi;
        const bool tuned = lit && std::abs (needleCents) <= inTuneCents;

        g.setColour (lit ? (tuned ? inTuneColour : offColour).withAlpha (0.25f) : theme::background);
        g.fillRoundedRectangle (cell, 6.0f);
        g.setColour (lit ? (tuned ? inTuneColour : offColour) : juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (cell, 6.0f, 1.5f);

        auto top = cell.removeFromTop (cell.getHeight() * 0.6f);
        g.setColour (lit ? juce::Colours::white : juce::Colours::white.withAlpha (0.6f));
        g.setFont (juce::FontOptions (juce::jmin (22.0f, top.getHeight() * 0.8f)).withStyle ("Bold"));
        g.drawText (juce::MidiMessage::getMidiNoteName (midi, true, true, 4), top, juce::Justification::centredBottom);
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("string " + juce::String (6 - (int) i), cell, juce::Justification::centred);
    }
}

//==============================================================================
void TunerPanel::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto bottom = area.removeFromBottom (36);
    muteToggle.setBounds (bottom.removeFromRight (juce::jmin (260, bottom.getWidth() / 2)));
    referenceLabel.setBounds (bottom.removeFromLeft (100));
    referenceSlider.setBounds (bottom.reduced (4, 0));
    area.removeFromBottom (12);

    stringsArea = area.removeFromBottom (juce::jlimit (48, 70, area.getHeight() / 6));
    area.removeFromBottom (12);
    hintLabel.setBounds (area.removeFromBottom (24));
    meterArea = area.removeFromBottom (juce::jlimit (60, 90, area.getHeight() / 3));
    noteArea = area;
}

} // namespace pv
