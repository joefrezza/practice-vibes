#include "LooperPanel.h"
#include "FootswitchPanel.h"
#include "AppPaths.h"
#include "LoopFiles.h"
#include "Theme.h"

namespace pv
{

namespace
{
    const juce::Colour recordColour  { 0xffd64545 };
    const juce::Colour armedColour   { 0xffe0a030 };
    const juce::Colour playColour    { 0xff3f9e5a };
    const juce::Colour overdubColour { 0xffb455c8 };

    void styleLabel (juce::Label& label, const juce::String& text, float alpha = 0.85f)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (alpha));
    }
}

//==============================================================================
/** Beat dots for the current bar (same look as the Practice tab's). */
class LooperPanel::BeatDots final : public juce::Component
{
public:
    explicit BeatDots (MetronomeProcessor& m) : metronome (m) {}

    void update()
    {
        const auto beat = metronome.isRunning() ? metronome.getCurrentBeat() : -1;
        const auto beats = beat >= 0 ? metronome.getCurrentBeatsPerBar() : metronome.beatsPerBar.load();

        if (beat != lastBeat || beats != lastBeats)
        {
            lastBeat = beat;
            lastBeats = beats;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto beats = juce::jmax (1, lastBeats);
        const auto size = juce::jmin ((float) getHeight(), (float) getWidth() / (float) beats - 6.0f);

        for (int i = 0; i < beats; ++i)
        {
            const auto x = (float) i * ((float) getWidth() / (float) beats);
            auto circle = juce::Rectangle<float> (x, 0.0f, size, size).withY (((float) getHeight() - size) * 0.5f);
            g.setColour (i == lastBeat ? (i == 0 ? theme::highlight : theme::accent.brighter (0.4f))
                                       : juce::Colours::white.withAlpha (0.12f));
            g.fillEllipse (circle.reduced (2.0f));
        }
    }

private:
    MetronomeProcessor& metronome;
    int lastBeat = -1, lastBeats = 4;
};

//==============================================================================
/** One track: the pedal button, mute, clear, volume, and a loop progress bar. */
class LooperPanel::TrackRow final : public juce::Component
{
public:
    TrackRow (Looper& l, int index) : looper (l), track (index)
    {
        nameLabel.setText (juce::String (track + 1), juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (26.0f).withStyle ("Bold"));
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
        addAndMakeVisible (nameLabel);

        pedalLabel.setFont (juce::FontOptions (12.0f));
        pedalLabel.setJustificationType (juce::Justification::centred);
        pedalLabel.setColour (juce::Label::textColourId, theme::highlight);
        pedalLabel.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (pedalLabel);

        pedalButton.onClick = [this] { looper.pressTrack (track); };
        pedalButton.setTooltip ("Record, then overdub / play (key " + juce::String (track + 1) + ")");
        addAndMakeVisible (pedalButton);

        muteButton.onClick = [this] { looper.toggleMute (track); };
        addAndMakeVisible (muteButton);

        clearButton.onClick = [this] { looper.clearTrack (track); };
        addAndMakeVisible (clearButton);

        volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
        volumeSlider.setRange (-40.0, 6.0, 0.5);
        volumeSlider.setTextValueSuffix (" dB");
        volumeSlider.setValue (juce::Decibels::gainToDecibels (looper.getGain (track)), juce::dontSendNotification);
        volumeSlider.setDoubleClickReturnValue (true, 0.0);
        volumeSlider.onValueChange = [this]
        {
            looper.setTrackGain (track, juce::Decibels::decibelsToGain ((float) volumeSlider.getValue(), -40.0f));
        };
        addAndMakeVisible (volumeSlider);
    }

    /** The footswitch(es) whose tap plays this track, e.g. "A"; empty = none. */
    void setPedal (const juce::String& switches, const juce::String& tooltip)
    {
        if (pedalLabel.getText() != switches)
        {
            pedalLabel.setText (switches, juce::dontSendNotification);
            pedalLabel.setTooltip (tooltip);
            resized();
        }
    }

    void update()
    {
        const auto state = looper.getState (track);
        const auto muted = looper.isMuted (track);
        const auto bars = looper.getBars (track);
        const auto position = looper.getPosition (track);
        const auto peak = looper.takePeak (track);

        juce::String text;
        juce::Colour colour;

        switch (state)
        {
            case Looper::State::empty:       text = "Record";                                   colour = theme::panel.brighter (0.25f); break;
            case Looper::State::armed:       text = "Waiting for bar line...";                  colour = armedColour;   break;
            case Looper::State::recording:   text = "Recording - tap to stop";                  colour = recordColour;  break;
            case Looper::State::playing:     text = muted ? "Muted - tap to play" : "Playing - tap to overdub";
                                             colour = muted ? theme::panel.brighter (0.4f) : playColour; break;
            case Looper::State::overdubbing: text = "Overdubbing - tap to stop";                colour = overdubColour; break;
        }

        pedalButton.setButtonText (text);
        pedalButton.setColour (juce::TextButton::buttonColourId, colour);
        muteButton.setButtonText (muted ? "Unmute" : "Mute");
        muteButton.setEnabled (state == Looper::State::playing || state == Looper::State::overdubbing);
        clearButton.setEnabled (state != Looper::State::empty);

        const auto level = juce::jmax (peak, meterLevel * 0.85f);

        if (state != lastState || bars != lastBars || std::abs (position - lastPosition) > 0.002f
            || muted != lastMuted || std::abs (level - meterLevel) > 0.01f)
        {
            lastState = state;
            lastBars = bars;
            lastPosition = position;
            lastMuted = muted;
            meterLevel = level;
            repaint (progressArea);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);

        auto area = progressArea.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (area, 4.0f);

        const auto colour = lastState == Looper::State::recording   ? recordColour
                          : lastState == Looper::State::overdubbing ? overdubColour
                          : lastState == Looper::State::armed       ? armedColour
                          : lastMuted                               ? juce::Colours::grey
                                                                    : playColour;

        if (lastPosition >= 0.0f)
        {
            g.setColour (colour.withAlpha (0.55f));
            g.fillRoundedRectangle (area.withWidth (area.getWidth() * juce::jlimit (0.0f, 1.0f, lastPosition)), 4.0f);
        }

        if (lastBars > 1 && lastState != Looper::State::recording)
        {
            g.setColour (juce::Colours::white.withAlpha (0.25f));

            for (int b = 1; b < lastBars; ++b)
            {
                const auto x = area.getX() + area.getWidth() * (float) b / (float) lastBars;
                g.drawVerticalLine (juce::roundToInt (x), area.getY() + 3.0f, area.getBottom() - 3.0f);
            }
        }

        // Output level along the bottom edge
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.fillRect (area.removeFromBottom (3.0f).withWidth (area.getWidth() * juce::jlimit (0.0f, 1.0f, meterLevel)));

        juce::String info;

        if (lastState == Looper::State::empty)
            info = "empty";
        else if (lastState == Looper::State::armed)
            info = "starts on the next bar line";
        else if (lastState == Looper::State::recording)
            info = lastBars > 0 ? "bar " + juce::String (lastBars) : juce::String();
        else
            info = juce::String (lastBars) + (lastBars == 1 ? " bar" : " bars");

        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (info, progressArea.reduced (10, 0), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 6);
        auto name = area.removeFromLeft (44);

        if (pedalLabel.getText().isNotEmpty())
            pedalLabel.setBounds (name.removeFromBottom (juce::jmin (18, name.getHeight() / 3)));
        else
            pedalLabel.setBounds ({});

        nameLabel.setBounds (name);
        pedalButton.setBounds (area.removeFromLeft (juce::jmin (260, area.getWidth() / 3)).reduced (3));
        clearButton.setBounds (area.removeFromRight (80).reduced (3));
        muteButton.setBounds (area.removeFromRight (90).reduced (3));
        volumeSlider.setBounds (area.removeFromRight (juce::jmin (280, area.getWidth() / 3)).reduced (3, 0).withSizeKeepingCentre (juce::jmin (280, area.getWidth() / 3) - 6, 32));
        progressArea = area.reduced (8, 6);
    }

private:
    Looper& looper;
    const int track;

    juce::Label nameLabel, pedalLabel;
    juce::TextButton pedalButton, muteButton { "Mute" }, clearButton { "Clear" };
    juce::Slider volumeSlider;
    juce::Rectangle<int> progressArea;

    Looper::State lastState = Looper::State::empty;
    int lastBars = 0;
    float lastPosition = -1.0f, meterLevel = 0.0f;
    bool lastMuted = false;
};

//==============================================================================
LooperPanel::LooperPanel (AppEngine& e)
    : engine (e), looper (*e.looper), metronome (*e.metronome)
{
    loadSettings();
    setWantsKeyboardFocus (true);

    header.setText ("Looper", juce::dontSendNotification);
    header.setFont (theme::headerFont());
    header.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));
    addAndMakeVisible (header);

    styleLabel (sourceLabel, "Record");
    addAndMakeVisible (sourceLabel);

    for (auto* b : { &afterFxButton, &beforeFxButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1);
        b->setColour (juce::TextButton::buttonOnColourId, theme::accent);
        addAndMakeVisible (*b);
    }

    afterFxButton.setTooltip ("Record the guitar as you hear it, with the rig's effects.");
    beforeFxButton.setTooltip ("Record the dry guitar; loops are played through the rig, so changing the tone changes them too.");
    (looper.source.load() == (int) Looper::Source::beforeFx ? beforeFxButton : afterFxButton).setToggleState (true, juce::dontSendNotification);
    afterFxButton.onClick = [this] { looper.source = (int) Looper::Source::afterFx; saveSettings(); };
    beforeFxButton.onClick = [this] { looper.source = (int) Looper::Source::beforeFx; saveSettings(); };

    styleLabel (lengthLabel, "Length");
    addAndMakeVisible (lengthLabel);
    lengthBox.addItem ("Free (stop on a bar line)", 100);

    for (auto bars : { 1, 2, 3, 4, 6, 8, 12, 16 })
        lengthBox.addItem (juce::String (bars) + (bars == 1 ? " bar" : " bars"), bars);

    lengthBox.setSelectedId (looper.fixedBars.load() > 0 ? looper.fixedBars.load() : 100, juce::dontSendNotification);
    lengthBox.onChange = [this]
    {
        const auto id = lengthBox.getSelectedId();
        looper.fixedBars = id == 100 ? 0 : id;
        saveSettings();
    };
    addAndMakeVisible (lengthBox);

    undoButton.onClick = [this] { looper.undo(); };
    clearAllButton.onClick = [this] { looper.clearAll(); };
    muteAllButton.onClick = [this] { looper.toggleMuteAll(); };

    footswitchButton.setTooltip ("Control the looper with a MIDI pedal: learn its switches, choose tap and hold actions.");
    footswitchButton.onClick = [this]
    {
        if (showOverlay)
            showOverlay ("Footswitch", std::make_unique<FootswitchPanel> (engine));
    };

    for (auto* b : { &undoButton, &clearAllButton, &muteAllButton, &metronomeButton, &footswitchButton })
        addAndMakeVisible (*b);

    // Metronome (shared with the Practice tab)
    styleLabel (tempoLabel, "Tempo");
    tempoSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    tempoSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 26);
    tempoSlider.setRange (30.0, 300.0, 1.0);
    tempoSlider.setTextValueSuffix (" bpm");
    tempoSlider.setValue (metronome.startBpm.load(), juce::dontSendNotification);
    tempoSlider.onValueChange = [this]
    {
        metronome.startBpm = tempoSlider.getValue();
        metronome.tempoChangedByUser();
    };

    styleLabel (beatsLabel, "Beats per bar");

    for (int i = 1; i <= 12; ++i)
        beatsBox.addItem (juce::String (i), i);

    beatsBox.setSelectedId (metronome.beatsPerBar.load(), juce::dontSendNotification);
    beatsBox.onChange = [this] { metronome.beatsPerBar = juce::jmax (1, beatsBox.getSelectedId()); };

    metronomeButton.onClick = [this]
    {
        if (metronome.isRunning())
            metronome.stop();
        else
            metronome.start();
    };

    tempoDisplay.setFont (juce::FontOptions (30.0f).withStyle ("Bold"));
    tempoDisplay.setColour (juce::Label::textColourId, theme::highlight);
    tempoDisplay.setJustificationType (juce::Justification::centredRight);

    beatDots = std::make_unique<BeatDots> (metronome);

    for (auto* c : std::initializer_list<juce::Component*> { &tempoLabel, &tempoSlider, &beatsLabel, &beatsBox, &tempoDisplay, beatDots.get() })
        addAndMakeVisible (*c);

    // Latency
    styleLabel (offsetLabel, "Latency fine-tune");
    offsetSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    offsetSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 26);
    offsetSlider.setRange (-100.0, 100.0, 0.5);
    offsetSlider.setTextValueSuffix (" ms");
    offsetSlider.setValue (looper.latencyOffsetMs.load(), juce::dontSendNotification);
    offsetSlider.setDoubleClickReturnValue (true, 0.0);
    offsetSlider.setTooltip ("If overdubs land late, move this up; if early, down.");
    offsetSlider.onValueChange = [this] { looper.latencyOffsetMs = (float) offsetSlider.getValue(); };
    offsetSlider.onDragEnd = [this] { saveSettings(); };
    styleLabel (latencyLabel, {}, 0.6f);
    latencyLabel.setMinimumHorizontalScale (0.5f);
    styleLabel (hintLabel, {}, 0.7f);
    hintLabel.setJustificationType (juce::Justification::topLeft);

    calibrateButton.setTooltip ("Measure the latency: play short muted strums right on the clicks "
                                "(or connect the audio output to the input with a cable).");
    calibrateButton.onClick = [this]
    {
        if (calibrating)
        {
            calibrationMessage = "Calibration cancelled.";
            endCalibration();
        }
        else
        {
            startCalibration();
        }
    };

    saveButton.setTooltip ("Save every track as a WAV (plus a mix) in Documents > Practice Vibes > Loops, to load again later or use in a DAW.");
    saveButton.onClick = [this] { saveLoops(); };
    loadButton.setTooltip ("Load saved loops (the looper must be empty).");
    loadButton.onClick = [this] { showLoadMenu(); };

    for (auto* c : std::initializer_list<juce::Component*> { &offsetLabel, &offsetSlider, &calibrateButton, &latencyLabel, &hintLabel,
                                                             &saveButton, &loadButton })
        addAndMakeVisible (*c);

    for (int t = 0; t < Looper::numTracks; ++t)
    {
        rows[(size_t) t] = std::make_unique<TrackRow> (looper, t);
        addAndMakeVisible (*rows[(size_t) t]);
    }

    // Drum machine
    styleLabel (drumsLabel, "Drums");
    styleLabel (drumStatus, {}, 0.6f);
    drumBox.setTooltip ("An instrument plugin with its own patterns (drum machine, groove player). "
                        "It plays with the metronome: same tempo and bars, starts and stops with it. "
                        "Notes from MIDI pads or keyboards go to it too (not the footswitch's own messages).");
    drumBox.onChange = [this] { drumChosen(); };
    drumEditButton.onClick = [this] { openDrumEditor(); };

    drumVolume.setSliderStyle (juce::Slider::LinearHorizontal);
    drumVolume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    drumVolume.setRange (0.0, 1.5, 0.01);
    drumVolume.setValue (engine.chain->getDrumStage().gain.load(), juce::dontSendNotification);
    drumVolume.setDoubleClickReturnValue (true, 0.8);
    drumVolume.setTooltip ("Drum machine volume");
    drumVolume.onValueChange = [this] { engine.chain->getDrumStage().gain = (float) drumVolume.getValue(); };

    clicksMutedButton.setTooltip ("Silence the metronome clicks (the clock keeps running, so the drums and loops stay in time).");
    clicksMutedButton.setToggleState (metronome.clicksMuted.load(), juce::dontSendNotification);
    clicksMutedButton.onClick = [this]
    {
        metronome.clicksMuted = clicksMutedButton.getToggleState();
        saveSettings();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &drumsLabel, &drumBox, &drumEditButton, &drumVolume, &clicksMutedButton, &drumStatus })
        addAndMakeVisible (*c);

    engine.catalogue->addChangeListener (this);
    refreshDrumList();

    startTimerHz (30);
}

LooperPanel::~LooperPanel()
{
    if (calibrating)
        endCalibration();

    engine.catalogue->removeChangeListener (this);
    saveSettings();
}

//==============================================================================
void LooperPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshDrumList();
}

void LooperPanel::refreshDrumList()
{
    drumEntries.clear();

    for (auto& entry : engine.catalogue->getEntries())
        if (entry.kind == "Instrument" || entry.kind == "Generator")
            drumEntries.push_back (entry);

    // The loaded plugin stays selectable even when the catalogue does not list it (yet).
    const auto current = engine.chain->getDrumPluginIdentifier();
    bool listed = current.isEmpty();

    for (auto& entry : drumEntries)
        listed = listed || entry.description.createIdentifierString() == current;

    if (! listed)
        if (auto* plugin = engine.chain->getDrumPlugin())
            drumEntries.insert (drumEntries.begin(), { plugin->getPluginDescription(), "Instrument" });

    drumBox.clear (juce::dontSendNotification);
    drumBox.addItem ("None", 1);

    for (size_t i = 0; i < drumEntries.size(); ++i)
    {
        const auto& d = drumEntries[i].description;
        drumBox.addItem (d.name + (d.manufacturerName.isNotEmpty() ? " (" + d.manufacturerName + ")" : juce::String()), (int) i + 2);
    }

    shownDrumIdentifier = "-";      // re-selected by the timer
}

void LooperPanel::drumChosen()
{
    const auto index = drumBox.getSelectedId() - 2;
    drumStatus.setText ({}, juce::dontSendNotification);

    if (index < 0)
    {
        if (hideOverlay && engine.chain->getDrumPlugin() != nullptr)
            hideOverlay();      // the drum UI may be open

        engine.chain->removeDrumPlugin();
        drumLoading = false;
        engine.saveState();
        return;
    }

    if (! juce::isPositiveAndBelow (index, (int) drumEntries.size()))
        return;

    const auto description = drumEntries[(size_t) index].description;

    if (description.createIdentifierString() == engine.chain->getDrumPluginIdentifier())
        return;

    if (hideOverlay && engine.chain->getDrumPlugin() != nullptr)
        hideOverlay();      // the old plugin's editor must go before the plugin does

    drumLoading = true;
    engine.chain->setDrumPlugin (description, [safeThis = SafePointer<LooperPanel> (this)] (const juce::String& error)
    {
        if (safeThis == nullptr)
            return;

        safeThis->drumLoading = false;
        safeThis->shownDrumIdentifier = "-";

        if (error.isNotEmpty())
            safeThis->drumStatus.setText ("Could not load: " + error, juce::dontSendNotification);
        else
            safeThis->engine.saveState();
    });
}

void LooperPanel::openDrumEditor()
{
    auto* plugin = engine.chain->getDrumPlugin();

    if (plugin == nullptr || ! showOverlay)
        return;

    if (hideOverlay)
        hideOverlay();      // a plugin must never have two editors at once

    std::unique_ptr<juce::Component> editor;

    if (plugin->hasEditor())
        editor.reset (plugin->createEditorAndMakeActive());

    if (editor == nullptr)
        editor = std::make_unique<juce::GenericAudioProcessorEditor> (*plugin);

    showOverlay ("Drums: " + engine.chain->getDrumPluginName(), std::move (editor));
}

void LooperPanel::updatePedalBadges()
{
    auto& footswitch = *engine.footswitch;

    for (int t = 0; t < Looper::numTracks; ++t)
    {
        juce::StringArray letters, details;

        for (int i = 0; i < Footswitch::numSwitches; ++i)
        {
            const auto& sw = footswitch.getSwitch (i);
            const auto tapsThis = sw.tap.kind == FootswitchAction::Kind::trackPress && sw.tap.track == t;

            if (! sw.trigger.isValid() || ! tapsThis)
                continue;

            letters.add (Footswitch::switchName (i));
            details.add ("Footswitch " + Footswitch::switchName (i) + ": tap = record / overdub / play"
                         + (sw.momentary && sw.hold.kind != FootswitchAction::Kind::none ? ", hold = " + sw.hold.describe() : juce::String()));
        }

        rows[(size_t) t]->setPedal (letters.isEmpty() ? juce::String() : "pedal " + letters.joinIntoString ("+"),
                                    details.joinIntoString ("\n"));
    }
}

//==============================================================================
void LooperPanel::showFileMessage (const juce::String& text)
{
    fileMessage = text;
    fileMessageMs = juce::Time::getMillisecondCounterHiRes();
}

void LooperPanel::saveLoops()
{
    LoopSet set;
    juce::String error;

    if (! looper.copyLoops (set, error))
    {
        showFileMessage ("Not saved: " + error);
        return;
    }

    const auto folder = LoopFiles::createSessionFolder (set);
    const auto result = LoopFiles::save (set, folder);

    if (result.failed())
    {
        showFileMessage ("Not saved: " + result.getErrorMessage());
        juce::Logger::writeToLog ("Saving loops failed: " + result.getErrorMessage());
        return;
    }

    juce::Logger::writeToLog ("Saved " + juce::String (set.getNumLoops()) + " loop(s) to " + folder.getFullPathName());
    showFileMessage ("Saved " + juce::String (set.getNumLoops()) + (set.getNumLoops() == 1 ? " loop" : " loops")
                     + " as \"" + folder.getFileName() + "\" (Loops folder: track WAVs + mix.wav).");
}

void LooperPanel::showLoadMenu()
{
    const auto sessions = LoopFiles::listSessions();
    juce::PopupMenu menu;

    if (sessions.isEmpty())
        menu.addItem (-1, "No saved loops yet", false);

    if (! looper.isEmpty())
        menu.addSectionHeader ("Clear all loops before loading");

    for (int i = 0; i < juce::jmin (30, sessions.size()); ++i)
        menu.addItem (i + 1, LoopFiles::describe (sessions[i]), looper.isEmpty() && ! looper.isLoading());

   #if ! JUCE_IOS
    menu.addSeparator();
    menu.addItem (1000, "Show the Loops folder");
   #endif

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (loadButton),
                        [safeThis = SafePointer<LooperPanel> (this), sessions] (int chosen)
    {
        if (safeThis == nullptr || chosen == 0)
            return;

        if (chosen == 1000)
            LoopFiles::getLoopsFolder().revealToUser();
        else if (juce::isPositiveAndBelow (chosen - 1, sessions.size()))
            safeThis->loadLoops (sessions[chosen - 1]);
    });
}

void LooperPanel::loadLoops (const juce::File& folder)
{
    LoopSet set;
    const auto result = LoopFiles::load (folder, set);
    juce::String error;

    if (result.failed())
        error = result.getErrorMessage();
    else
        looper.loadLoops (set, error);

    if (error.isNotEmpty())
    {
        showFileMessage ("Not loaded: " + error);
        juce::Logger::writeToLog ("Loading loops from " + folder.getFullPathName() + " failed: " + error);
        return;
    }

    // Record mode and tempo come from the session.
    (set.source == Looper::Source::beforeFx ? beforeFxButton : afterFxButton).setToggleState (true, juce::dontSendNotification);
    saveSettings();
    juce::Logger::writeToLog ("Loaded loops from " + folder.getFullPathName());
    showFileMessage ("Loaded \"" + folder.getFileName() + "\": the loops play from the metronome's first downbeat.");
}

//==============================================================================
namespace
{
    constexpr int warmUpHits = 2;           // the first strums are often off: ignored
    constexpr int calibrationHits = 12;
    constexpr double calibrationTimeoutMs = 45000.0;
}

void LooperPanel::startCalibration()
{
    calibrating = true;
    calibrationLags.clear();
    calibrationStartMs = juce::Time::getMillisecondCounterHiRes();
    calibrationMessage.clear();

    // The clicks must be audible and follow the bar clock.
    followSongBefore = metronome.followSong.load();
    clicksMutedBefore = metronome.clicksMuted.load();
    metronome.followSong = false;
    metronome.clicksMuted = false;
    stopMetronomeAfterCalibration = ! metronome.isRunning();

    if (stopMetronomeAfterCalibration)
        metronome.start();

    looper.calibrator.start();
    calibrateButton.setButtonText ("Cancel");
}

void LooperPanel::endCalibration()
{
    looper.calibrator.stop();
    calibrating = false;
    calibrationMessageMs = juce::Time::getMillisecondCounterHiRes();
    metronome.followSong = followSongBefore;
    metronome.clicksMuted = clicksMutedBefore;

    if (stopMetronomeAfterCalibration)
        metronome.stop();

    calibrateButton.setButtonText ("Calibrate...");
}

void LooperPanel::updateCalibration()
{
    if (! calibrating)
        return;

    looper.calibrator.readLags (calibrationLags);
    const auto hits = (int) calibrationLags.size() - warmUpHits;

    if (hits < calibrationHits)
    {
        if (juce::Time::getMillisecondCounterHiRes() - calibrationStartMs > calibrationTimeoutMs)
        {
            calibrationMessage = calibrationLags.empty()
                                   ? "Calibration stopped: no strums detected. Check that the guitar input is working (meter in the Guitar rig tab)."
                                   : "Calibration stopped: only " + juce::String (calibrationLags.size()) + " strums detected in 45 s. Try again.";
            endCalibration();
        }

        return;
    }

    const auto result = LatencyCalibrator::analyse ({ calibrationLags.begin() + warmUpHits, calibrationLags.end() });
    const auto rate = juce::jmax (1.0, looper.getSampleRate());
    const auto deviceMs = looper.deviceLatencySamples.load() * 1000.0 / rate;

    if (result.ok)
    {
        const auto offset = juce::jlimit (-100.0, 100.0, result.lagMs - deviceMs);
        offsetSlider.setValue (offset, juce::sendNotificationSync);
        saveSettings();
        calibrationMessage = "Calibrated: your strums arrive " + juce::String (result.lagMs, 1) + " ms after the click (spread "
                             + juce::String (result.spreadMs, 1) + " ms); the audio device reports " + juce::String (deviceMs, 1)
                             + " ms, so the fine-tune is now " + juce::String (offset, 1) + " ms.";
    }
    else
    {
        calibrationMessage = "Calibration failed: " + result.problem + ". Try again with short muted strums right on the clicks.";
    }

    endCalibration();
}

//==============================================================================
void LooperPanel::loadSettings()
{
    if (auto xml = juce::parseXML (getLooperSettingsFile()))
    {
        looper.source = juce::jlimit (0, 1, xml->getIntAttribute ("source", 0));
        looper.fixedBars = juce::jlimit (0, 16, xml->getIntAttribute ("fixedBars", 0));
        looper.latencyOffsetMs = (float) juce::jlimit (-100.0, 100.0, xml->getDoubleAttribute ("latencyOffsetMs", 0.0));
        metronome.clicksMuted = xml->getBoolAttribute ("clicksMuted", false);
    }
}

void LooperPanel::saveSettings() const
{
    juce::XmlElement xml ("PRACTICE_VIBES_LOOPER");
    xml.setAttribute ("source", looper.source.load());
    xml.setAttribute ("fixedBars", looper.fixedBars.load());
    xml.setAttribute ("latencyOffsetMs", (double) looper.latencyOffsetMs.load());
    xml.setAttribute ("clicksMuted", metronome.clicksMuted.load());
    xml.writeTo (getLooperSettingsFile());
}

bool LooperPanel::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();

    if (c >= '1' && c < '1' + Looper::numTracks)
    {
        looper.pressTrack (c - '1');
        return true;
    }

    return false;
}

void LooperPanel::timerCallback()
{
    for (auto& row : rows)
        row->update();

    updatePedalBadges();
    beatDots->update();

    const auto empty = looper.isEmpty();
    const auto locked = metronome.isTempoLocked();
    const auto beforeFx = looper.getActiveSource() == Looper::Source::beforeFx;

    // The record mode can only change while the looper is empty.
    afterFxButton.setEnabled (empty);
    beforeFxButton.setEnabled (empty);

    if (! empty)
        (beforeFx ? beforeFxButton : afterFxButton).setToggleState (true, juce::dontSendNotification);

    tempoSlider.setEnabled (! locked);
    beatsBox.setEnabled (! locked);
    tempoLabel.setEnabled (! locked);
    beatsLabel.setEnabled (! locked);

    // The Practice tab edits the same metronome.
    if (! tempoSlider.isMouseButtonDown())
        tempoSlider.setValue (metronome.startBpm.load(), juce::dontSendNotification);

    if (beatsBox.getSelectedId() != metronome.beatsPerBar.load() && ! beatsBox.isPopupActive())
        beatsBox.setSelectedId (metronome.beatsPerBar.load(), juce::dontSendNotification);

    undoButton.setEnabled (looper.canUndo());
    clearAllButton.setEnabled (! empty);
    muteAllButton.setEnabled (! empty);
    metronomeButton.setButtonText (metronome.isRunning() ? "Stop metronome" : "Start metronome");

    const auto running = metronome.isRunning();
    tempoDisplay.setText (juce::String (juce::roundToInt (running ? metronome.getCurrentBpm() : metronome.startBpm.load())) + " bpm",
                          juce::dontSendNotification);

    const auto rate = juce::jmax (1.0, looper.getSampleRate());
    latencyLabel.setText ("Compensation " + juce::String (looper.getCompensationSamples() * 1000.0 / rate, 1)
                              + " ms (device + plugins + fine-tune)",
                          juce::dontSendNotification);

    updateCalibration();

    if (! calibrating && calibrationMessage.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() - calibrationMessageMs > 30000.0)
        calibrationMessage.clear();

    if (fileMessage.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() - fileMessageMs > 15000.0)
        fileMessage.clear();

    saveButton.setEnabled (! empty);

    juce::String hint;

    if (calibrating)
    {
        const auto hits = juce::jmax (0, (int) calibrationLags.size() - warmUpHits);
        hint = "Calibrating: play a short muted strum exactly on every click (or connect the output to the input with a cable). "
               + juce::String (juce::jmin (hits, calibrationHits)) + " / " + juce::String (calibrationHits)
               + (calibrationLags.size() < (size_t) warmUpHits ? " (warming up)" : "");
    }
    else if (fileMessage.isNotEmpty())
    {
        hint = fileMessage;
    }
    else if (calibrationMessage.isNotEmpty())
    {
        hint = calibrationMessage;
    }
    else if (looper.isLoading())
    {
        hint = "Loaded loops start with the metronome.";
    }
    else if (locked)
        hint = "Tempo and bar length are locked while there are loops. Clear all to change them.";
    else if (empty)
        hint = "Tap Record (or press 1-4): recording starts on the nearest bar line. "
               "If the metronome is stopped it starts, with one bar of count-in.";

    if (beforeFx && engine.chain->getOutputStage().muted.load())
        hint << (hint.isEmpty() ? "" : "\n")
             << "Before-FX loops play through the rig: switch Monitor on in the Guitar rig tab to hear them.";

    if (metronome.followSong.load() && locked)
        hint << (hint.isEmpty() ? "" : "\n") << "\"Follow song beats\" is paused while there are loops.";

    hintLabel.setText (hint, juce::dontSendNotification);

    // Drum machine: follow the chain (it is restored at start-up, after this panel exists).
    const auto drumId = engine.chain->getDrumPluginIdentifier();

    if (! drumLoading && drumId != shownDrumIdentifier && ! drumBox.isPopupActive())
    {
        int selected = drumId.isEmpty() ? 1 : 0;

        for (size_t i = 0; i < drumEntries.size() && selected == 0; ++i)
            if (drumEntries[i].description.createIdentifierString() == drumId)
                selected = (int) i + 2;

        if (selected == 0)
        {
            refreshDrumList();      // loaded but not listed: the next tick selects it
        }
        else
        {
            drumBox.setSelectedId (selected, juce::dontSendNotification);
            shownDrumIdentifier = drumId;
        }
    }

    const auto hasDrums = engine.chain->getDrumPlugin() != nullptr;
    drumEditButton.setEnabled (hasDrums && ! drumLoading);
    drumBox.setEnabled (! drumLoading);

    if (! drumVolume.isMouseButtonDown())
        drumVolume.setValue (engine.chain->getDrumStage().gain.load(), juce::dontSendNotification);

    if (clicksMutedButton.getToggleState() != metronome.clicksMuted.load())
        clicksMutedButton.setToggleState (metronome.clicksMuted.load(), juce::dontSendNotification);

    juce::String status;

    if (drumLoading)
        status = "Loading...";
    else if (drumStatus.getText().startsWith ("Could not"))
        status = drumStatus.getText();
    else if (hasDrums)
    {
        status = metronome.isRunning() ? "Playing along with the metronome" : "Starts and stops with the metronome";

        if (engine.chain->getDrumPlugin()->acceptsMidi())
            status << " - plays MIDI notes";
    }
    else if (drumEntries.empty())
        status = engine.catalogue->canScan() ? "No instruments found: Scan VST3 in the Guitar rig tab" : "No instrument plugins found";

    drumStatus.setText (status, juce::dontSendNotification);
}

//==============================================================================
void LooperPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void LooperPanel::resized()
{
    auto area = getLocalBounds().reduced (12, 8);

    auto top = area.removeFromTop (44);
    header.setBounds (top.removeFromLeft (90));
    sourceLabel.setBounds (top.removeFromLeft (64));
    afterFxButton.setBounds (top.removeFromLeft (100).reduced (2, 4));
    beforeFxButton.setBounds (top.removeFromLeft (100).reduced (2, 4));
    top.removeFromLeft (16);
    lengthLabel.setBounds (top.removeFromLeft (60));
    lengthBox.setBounds (top.removeFromLeft (220).reduced (2, 6));
    footswitchButton.setBounds (top.removeFromRight (120).reduced (3, 4));
    clearAllButton.setBounds (top.removeFromRight (100).reduced (3, 4));
    muteAllButton.setBounds (top.removeFromRight (100).reduced (3, 4));
    undoButton.setBounds (top.removeFromRight (130).reduced (3, 4));

    area.removeFromTop (4);
    auto metro = area.removeFromTop (44);
    metronomeButton.setBounds (metro.removeFromLeft (170).reduced (2, 4));
    tempoDisplay.setBounds (metro.removeFromRight (130));
    beatDots->setBounds (metro.removeFromRight (160).reduced (4, 8));
    tempoLabel.setBounds (metro.removeFromLeft (70).withTrimmedLeft (12));
    beatsBox.setBounds (metro.removeFromRight (70).reduced (2, 6));
    beatsLabel.setBounds (metro.removeFromRight (110));
    tempoSlider.setBounds (metro.reduced (0, 6));

    area.removeFromTop (4);
    auto drumRow = area.removeFromTop (44);
    drumsLabel.setBounds (drumRow.removeFromLeft (64));
    drumBox.setBounds (drumRow.removeFromLeft (juce::jmin (300, drumRow.getWidth() / 3)).reduced (2, 6));
    drumEditButton.setBounds (drumRow.removeFromLeft (100).reduced (2, 4));
    drumVolume.setBounds (drumRow.removeFromLeft (juce::jmin (200, drumRow.getWidth() / 4)).reduced (6, 6));
    clicksMutedButton.setBounds (drumRow.removeFromLeft (130).reduced (2, 4));
    drumStatus.setBounds (drumRow.reduced (8, 0));

    area.removeFromTop (8);

    auto bottom = area.removeFromBottom (36);
    offsetLabel.setBounds (bottom.removeFromLeft (140));
    offsetSlider.setBounds (bottom.removeFromLeft (juce::jmin (360, bottom.getWidth() / 2)).reduced (0, 4));
    calibrateButton.setBounds (bottom.removeFromLeft (120).reduced (6, 3));
    loadButton.setBounds (bottom.removeFromRight (120).reduced (3));
    saveButton.setBounds (bottom.removeFromRight (110).reduced (3));
    latencyLabel.setBounds (bottom.reduced (12, 0));

    hintLabel.setBounds (area.removeFromBottom (54));

    const auto rowHeight = juce::jlimit (60, 110, area.getHeight() / Looper::numTracks);

    for (auto& row : rows)
        row->setBounds (area.removeFromTop (rowHeight).reduced (0, 3));
}

} // namespace pv
