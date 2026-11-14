#include "TunerPanel.h"
#include "Theme.h"

namespace pv
{

namespace
{
    const juce::Colour inTuneColour { 0xff5cc871 };
    const juce::Colour offColour    { 0xffff9f43 };

    constexpr float inTuneCents = 3.0f;
    constexpr float strumInTuneCents = 4.0f;       // the strum check is a quick overview
    constexpr float retuneHintCents = 250.0f;      // "nearest string" hint up to this far away

    constexpr float strobePeriod = 36.0f;          // px per light + dark stripe pair (fine band)
    constexpr float strobePxPerCent = 10.0f;       // fine band speed: 1 cent = 10 px/s

    juce::String noteName (int midi)    { return juce::MidiMessage::getMidiNoteName (midi, true, true, 4); }

    juce::String semitonesText (float cents, bool wide)
    {
        const auto st = juce::String (std::abs (cents) / 100.0f, 1);
        return st + (wide ? " semitones " : " st ") + (cents < 0.0f ? "flat" : "sharp");
    }
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
        strum.reset();
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

    tuningLabel.setText ("Tuning", juce::dontSendNotification);
    addAndMakeVisible (tuningLabel);

    const auto& presets = tuningPresets();

    for (int i = 0; i < (int) presets.size(); ++i)
        tuningCombo.addItem (presets[(size_t) i].name, i + 1);

    tuningCombo.addItem ("Custom", (int) presets.size() + 1);
    tuningCombo.setItemEnabled ((int) presets.size() + 1, false);
    tuningCombo.setTooltip ("Click a string below to change its note (makes a custom tuning).");
    tuningCombo.onChange = [this]
    {
        const auto index = tuningCombo.getSelectedId() - 1;

        if (juce::isPositiveAndBelow (index, (int) tuningPresets().size()))
        {
            tuner.strings = tuningPresets()[(size_t) index].strings;
            tuningChanged();
        }
    };
    addAndMakeVisible (tuningCombo);
    updateTuningCombo();

    auto setUpPair = [this] (juce::TextButton& left, juce::TextButton& right, int group, bool rightOn)
    {
        for (auto* b : { &left, &right })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (group);
            b->setColour (juce::TextButton::buttonOnColourId, theme::accent);
            addAndMakeVisible (*b);
        }

        left.setConnectedEdges (juce::Button::ConnectedOnRight);
        right.setConnectedEdges (juce::Button::ConnectedOnLeft);
        (rightOn ? right : left).setToggleState (true, juce::dontSendNotification);
    };

    setUpPair (singleButton, strumButton, 1, tuner.strumMode);
    setUpPair (needleButton, strobeButton, 2, tuner.strobe);
    singleButton.setTooltip ("Tune one string at a time");
    strumButton.setTooltip ("Strum all six open strings once and see every string at the same time");
    strobeButton.setTooltip ("A band that drifts while out of tune and stands still when in tune: finest for the last cent");

    for (auto* b : { &singleButton, &strumButton, &needleButton, &strobeButton })
        b->onClick = [this]
        {
            const auto strumMode = strumButton.getToggleState(), strobe = strobeButton.getToggleState();

            if (strumMode != tuner.strumMode || strobe != tuner.strobe)
            {
                tuner.strumMode = strumMode;
                tuner.strobe = strobe;
                engine.saveTunerSettings();
                applyMode();
            }
        };

    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    addAndMakeVisible (hintLabel);

    tuner.active = true;
    applyMute();
    applyMode();
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

void TunerPanel::applyMode()
{
    needleButton.setVisible (! tuner.strumMode);
    strobeButton.setVisible (! tuner.strumMode);

    tuner.clear();
    smoother.reset();
    strum.reset();
    shownFrequency = 0.0f;
    note = {};
    lastTick = juce::Time::getMillisecondCounterHiRes() / 1000.0;

    // The strobe needs smooth motion; the needle and the strum view don't.
    startTimerHz (tuner.strobe && ! tuner.strumMode ? 60 : 30);
    repaint();
}

void TunerPanel::updateTuningCombo()
{
    const auto& presets = tuningPresets();
    int id = (int) presets.size() + 1;

    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].strings == tuner.strings)
            id = i + 1;

    tuningCombo.setSelectedId (id, juce::dontSendNotification);
}

void TunerPanel::tuningChanged()
{
    engine.saveTunerSettings();
    updateTuningCombo();
    strum.reset();
    repaint();
}

void TunerPanel::showStringMenu (int index)
{
    const auto current = tuner.strings[(size_t) index];
    juce::PopupMenu menu;
    menu.addSectionHeader ("String " + juce::String (6 - index));

    for (int n = current + 7; n >= current - 7; --n)
        if (n >= Tuner::minStringNote && n <= Tuner::maxStringNote)
            menu.addItem (n + 1, noteName (n) + (n == current ? "" : "   (" + juce::String (n > current ? "+" : "")
                                                                            + juce::String (n - current) + ")"),
                          true, n == current);

    juce::Component::SafePointer<TunerPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (stringCell (index).toNearestInt())),
                        [safe, index] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            safe->tuner.strings[(size_t) index] = result - 1;
                            safe->tuningChanged();
                        });
}

void TunerPanel::mouseUp (const juce::MouseEvent& e)
{
    for (int i = 0; i < 6; ++i)
        if (stringCell (i).contains (e.position))
            showStringMenu (i);
}

//==============================================================================
void TunerPanel::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const auto dt = (float) juce::jlimit (0.0, 0.1, now - lastTick);
    lastTick = now;

    if (tuner.strumMode)
    {
        if (auto result = tuner.readStrum())
        {
            strum = result;
            strumStrings = tuner.strings;
        }

        if (strum && strumStrings != tuner.strings)
            strum.reset();

        hintLabel.setText (tuner.isStrumPending() ? "Listening..."
                           : strum ? "Strum again to check again"
                                   : "Strum all six open strings once and let them ring",
                           juce::dontSendNotification);
        repaint (noteArea.getUnion (meterArea).getUnion (stringsArea));
        return;
    }

    const auto reading = tuner.read();
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

        // Strobe: sharp drifts right, flat drifts left. Capped below half a stripe per frame,
        // or the eye sees it running backwards.
        const auto maxSpeed = 0.45f * strobePeriod * 60.0f;
        strobePhase += dt * juce::jlimit (-maxSpeed, maxSpeed, needleCents * strobePxPerCent);
        strobePhase = std::fmod (strobePhase, strobePeriod * 8.0f);
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

    if (tuner.strumMode)
    {
        paintStrum (g, noteArea.getUnion (meterArea).toFloat());
    }
    else
    {
        paintNote (g, noteArea.toFloat());
        paintMeter (g, meterArea.toFloat());
    }

    paintStrings (g, stringsArea.toFloat());
}

void TunerPanel::paintNote (juce::Graphics& g, juce::Rectangle<float> area)
{
    const bool hasNote = shownFrequency > 0.0f && note.midiNote >= 0;
    const bool inTune = hasNote && std::abs (needleCents) <= inTuneCents;
    const auto noteColour = ! hasNote ? juce::Colours::white.withAlpha (0.25f)
                                      : inTune ? inTuneColour : juce::Colours::white;

    // Note name with its octave; flat / sharp arrows either side.
    auto centre = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 260.0f), area.getHeight());
    const auto bigSize = juce::jmin (area.getHeight() * 0.8f, 130.0f);

    g.setColour (noteColour);
    g.setFont (juce::FontOptions (bigSize).withStyle ("Bold"));
    g.drawText (hasNote ? note.name : "-", centre, juce::Justification::centred);

    if (! hasNote)
        return;

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

void TunerPanel::paintMeter (juce::Graphics& g, juce::Rectangle<float> area)
{
    const bool hasNote = shownFrequency > 0.0f;
    auto scale = area.removeFromTop (area.getHeight() * 0.62f).reduced (12.0f, 0.0f);

    if (tuner.strobe)
    {
        paintStrobe (g, scale);
    }
    else
    {
        const auto x = [&] (float cents) { return scale.getX() + (cents + 50.0f) / 100.0f * scale.getWidth(); };

        // In-tune zone.
        g.setColour (inTuneColour.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (x (-inTuneCents), scale.getY(), x (inTuneCents) - x (-inTuneCents), scale.getHeight()));

        // Ticks every 10 cents, taller at the ends and the centre.
        for (int c = -50; c <= 50; c += 10)
        {
            const auto tall = c == 0 ? 1.0f : c % 50 == 0 ? 0.7f : 0.45f;
            g.setColour (juce::Colours::white.withAlpha (c == 0 ? 0.8f : 0.35f));
            g.fillRect (juce::Rectangle<float> (x ((float) c) - 1.0f, scale.getBottom() - scale.getHeight() * tall, 2.0f, scale.getHeight() * tall));
        }

        if (hasNote)
        {
            const auto cents = juce::jlimit (-50.0f, 50.0f, needleCents);
            g.setColour (std::abs (cents) <= inTuneCents ? inTuneColour : offColour);
            g.fillRoundedRectangle (juce::Rectangle<float> (x (cents) - 3.0f, scale.getY() - 4.0f, 6.0f, scale.getHeight() + 8.0f), 3.0f);
        }
    }

    // Readout: cents and frequency.
    auto text = area.reduced (12.0f, 2.0f);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (tuner.strobe ? "<< flat" : "-50", text, juce::Justification::topLeft);
    g.drawText (tuner.strobe ? "sharp >>" : "+50", text, juce::Justification::topRight);

    if (hasNote)
    {
        const auto cents = tuner.strobe ? juce::String (needleCents, 1) : juce::String (juce::roundToInt (needleCents));
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (18.0f));
        g.drawText ((needleCents > 0.0f ? "+" : "") + cents + " cents   " + juce::String (shownFrequency, 1) + " Hz",
                    text, juce::Justification::centred);
    }
}

void TunerPanel::paintStrobe (juce::Graphics& g, juce::Rectangle<float> area)
{
    const bool hasNote = shownFrequency > 0.0f;
    const bool inTune = hasNote && std::abs (needleCents) <= inTuneCents;
    const auto stripe = ! hasNote ? juce::Colours::white.withAlpha (0.12f)
                                  : inTune ? inTuneColour : juce::Colours::white.withAlpha (0.75f);

    g.setColour (theme::background);
    g.fillRoundedRectangle (area, 6.0f);

    juce::Graphics::ScopedSaveState state (g);
    juce::Path clip;
    clip.addRoundedRectangle (area, 6.0f);
    g.reduceClipRegion (clip);

    // Fine band on top; a coarse band (double stripes, a quarter of the speed) below it shows
    // the direction when the fine one moves too fast to follow.
    auto band = [&] (juce::Rectangle<float> r, float period, float phase)
    {
        g.setColour (stripe);
        auto x = r.getX() + std::fmod (phase, period) - period;    // fmod may be negative: starts further left

        for (; x < r.getRight(); x += period)
            g.fillRect (juce::Rectangle<float> (x, r.getY(), period * 0.5f, r.getHeight()));
    };

    auto fine = area.reduced (0.0f, 4.0f);
    auto coarse = fine.removeFromBottom (fine.getHeight() * 0.35f);
    fine.removeFromBottom (4.0f);
    band (fine, strobePeriod, strobePhase);
    band (coarse, strobePeriod * 2.0f, strobePhase * 0.25f);

    // Centre mark.
    g.setColour (theme::panel);
    g.fillRect (area.withSizeKeepingCentre (2.0f, area.getHeight()));
}

void TunerPanel::paintStrum (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto cellWidth = area.getWidth() / 6.0f;

    for (int i = 0; i < 6; ++i)
    {
        auto cell = juce::Rectangle<float> (area.getX() + cellWidth * (float) i, area.getY(), cellWidth, area.getHeight()).reduced (6.0f, 4.0f);
        const auto reading = strum ? (*strum)[(size_t) i] : StringReading {};
        const bool inTune = reading.heard && std::abs (reading.cents) <= strumInTuneCents;
        const auto colour = ! reading.heard ? juce::Colours::white.withAlpha (0.25f) : inTune ? inTuneColour : offColour;

        // Number and words on top.
        auto top = cell.removeFromTop (juce::jmin (90.0f, cell.getHeight() * 0.35f));
        g.setColour (colour);
        g.setFont (juce::FontOptions (juce::jmin (40.0f, top.getHeight() * 0.55f, cellWidth * 0.32f)).withStyle ("Bold"));
        const auto cents = juce::roundToInt (reading.cents);
        g.drawText (! reading.heard ? "-" : cents > 0 ? "+" + juce::String (cents) : juce::String (cents),
                    top.removeFromTop (top.getHeight() * 0.65f), juce::Justification::centredBottom);
        g.setFont (juce::FontOptions (13.0f));
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawText (! strum ? "" : ! reading.heard ? "not heard" : inTune ? "in tune" : reading.cents < 0.0f ? "flat" : "sharp",
                    top, juce::Justification::centred);

        // Vertical scale, sharp up: -50..+50 cents.
        auto scale = cell.reduced (cell.getWidth() * 0.3f, 8.0f);
        const auto y = [&] (float c) { return scale.getCentreY() - c / 50.0f * scale.getHeight() * 0.5f; };

        g.setColour (theme::background);
        g.fillRoundedRectangle (scale, 4.0f);
        g.setColour (inTuneColour.withAlpha (0.18f));
        g.fillRect (scale.withY (y (strumInTuneCents)).withHeight (y (-strumInTuneCents) - y (strumInTuneCents)));
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.fillRect (scale.withY (y (0.0f) - 1.0f).withHeight (2.0f));

        if (reading.heard)
        {
            const auto c = juce::jlimit (-50.0f, 50.0f, reading.cents);
            g.setColour (colour);
            g.fillRoundedRectangle (scale.expanded (6.0f, 0.0f).withY (y (c) - 3.0f).withHeight (6.0f), 3.0f);
        }
    }
}

juce::Rectangle<float> TunerPanel::stringCell (int index) const
{
    const auto area = stringsArea.toFloat();
    const auto cellWidth = area.getWidth() / 6.0f;
    return juce::Rectangle<float> (area.getX() + cellWidth * (float) index, area.getY(), cellWidth, area.getHeight()).reduced (4.0f);
}

void TunerPanel::paintStrings (juce::Graphics& g, juce::Rectangle<float>)
{
    // Single mode: the string nearest to the note played lights up; when it's more than half a
    // semitone away (retuning to drop D, say), the cell says how far.
    float away = 0.0f;
    const auto nearest = tuner.strumMode ? -1 : nearestString (tuner.strings, shownFrequency, tuner.referenceA4, away, retuneHintCents);

    for (int i = 0; i < 6; ++i)
    {
        auto cell = stringCell (i);
        const auto midi = tuner.strings[(size_t) i];
        const bool lit = i == nearest;
        const bool tuned = lit && std::abs (away) <= inTuneCents;

        g.setColour (lit ? (tuned ? inTuneColour : offColour).withAlpha (0.25f) : theme::background);
        g.fillRoundedRectangle (cell, 6.0f);
        g.setColour (lit ? (tuned ? inTuneColour : offColour) : juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (cell, 6.0f, 1.5f);

        auto top = cell.removeFromTop (cell.getHeight() * 0.6f);
        g.setColour (lit ? juce::Colours::white : juce::Colours::white.withAlpha (0.6f));
        g.setFont (juce::FontOptions (juce::jmin (22.0f, top.getHeight() * 0.8f)).withStyle ("Bold"));
        g.drawText (noteName (midi), top, juce::Justification::centredBottom);

        g.setColour (lit && std::abs (away) >= 50.0f ? offColour : juce::Colours::white.withAlpha (0.4f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (lit && std::abs (away) >= 50.0f ? semitonesText (away, cell.getWidth() > 150.0f)
                                                    : "string " + juce::String (6 - i),
                    cell, juce::Justification::centred);
    }
}

//==============================================================================
void TunerPanel::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto topRow = area.removeFromTop (32);
    tuningLabel.setBounds (topRow.removeFromLeft (60));
    tuningCombo.setBounds (topRow.removeFromLeft (180).reduced (0, 2));
    topRow.removeFromLeft (16);
    strobeButton.setBounds (topRow.removeFromRight (90).reduced (0, 2));
    needleButton.setBounds (topRow.removeFromRight (90).reduced (0, 2));
    topRow.removeFromRight (16);
    const auto modeWidth = juce::jmin (130, topRow.getWidth() / 2);
    singleButton.setBounds (topRow.removeFromLeft (modeWidth).reduced (0, 2));
    strumButton.setBounds (topRow.removeFromLeft (modeWidth).reduced (0, 2));
    area.removeFromTop (8);

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
