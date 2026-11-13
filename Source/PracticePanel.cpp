#include "PracticePanel.h"
#include "Theme.h"

namespace pv
{

//==============================================================================
class PracticePanel::BeatIndicator final : public juce::Component
{
public:
    explicit BeatIndicator (MetronomeProcessor& m) : metronome (m) {}

    void update()
    {
        const auto beat = metronome.getCurrentBeat();
        const auto beats = metronome.beatsPerBar.load();

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
            const auto lit = i == lastBeat;

            g.setColour (lit ? (i == 0 ? theme::highlight : theme::accent.brighter (0.4f))
                             : juce::Colours::white.withAlpha (0.12f));
            g.fillEllipse (circle.reduced (2.0f));
        }
    }

private:
    MetronomeProcessor& metronome;
    int lastBeat = -1, lastBeats = 4;
};

//==============================================================================
namespace
{
    void styleLabel (juce::Label& label, const juce::String& text, float alpha = 0.85f)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (alpha));
    }

    void styleHeader (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (theme::headerFont());
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));
    }

    void styleHorizontal (juce::Slider& slider, double min, double max, double step, double value, const juce::String& suffix)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 26);
        slider.setRange (min, max, step);
        slider.setValue (value, juce::dontSendNotification);
        slider.setTextValueSuffix (suffix);
        slider.setDoubleClickReturnValue (true, value);
    }

    void styleIncDec (juce::Slider& slider, double min, double max, double step, double value)
    {
        slider.setSliderStyle (juce::Slider::IncDecButtons);
        slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 74, 30);
        slider.setRange (min, max, step);
        slider.setValue (value, juce::dontSendNotification);
    }
}

//==============================================================================
PracticePanel::PracticePanel (PlayerProcessor& p, MetronomeProcessor& m)
    : player (p), metronome (m), waveform (p)
{
    setWantsKeyboardFocus (true);

    // Song ------------------------------------------------------------------
    styleHeader (songHeader, "Song");
    styleLabel (songLabel, "No song loaded", 0.7f);
    songLabel.setMinimumHorizontalScale (0.6f);
    styleLabel (timeLabel, "0:00.0 / 0:00.0");
    timeLabel.setJustificationType (juce::Justification::centredRight);
    timeLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 17.0f, juce::Font::plain));

    for (auto* c : std::initializer_list<juce::Component*> { &songHeader, &songLabel, &timeLabel, &openButton, &waveform,
                                                             &rewindButton, &playButton, &stopButton, &setAButton, &setBButton,
                                                             &loopButton, &clearLoopButton, &speedSlider, &pitchSlider,
                                                             &zoomOutButton, &zoomInButton, &fitButton, &zoomLoopButton,
                                                             &songVolumeSlider, &speedLabel, &pitchLabel, &songVolumeLabel, &hintLabel })
        addAndMakeVisible (c);

    openButton.onClick = [this] { openFile(); };
    rewindButton.onClick = [this] { player.seekToSeconds (player.isLoopEnabled() ? player.getLoopStartSeconds() : 0.0); };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] { player.stop(); };

    waveform.onSeek = [this] (double seconds) { player.seekToSeconds (seconds); };
    waveform.onLoopEdited = [this] (double start, double end)
    {
        player.setLoopSeconds (start, end);
        updateLoopButtons();
    };

    zoomOutButton.onClick = [this] { waveform.zoomBy (0.5); };
    zoomInButton.onClick = [this] { waveform.zoomBy (2.0); };
    fitButton.onClick = [this] { waveform.zoomToFit(); };
    zoomLoopButton.onClick = [this]
    {
        if (player.hasLoop())
            waveform.zoomToRange (player.getLoopStartSeconds(), player.getLoopEndSeconds());
    };
    waveform.onLoopSelected = [this] (double start, double end)
    {
        player.setLoopSeconds (start, end);
        player.setLoopEnabled (true);
        updateLoopButtons();
    };

    setAButton.onClick = [this]
    {
        pendingLoopStart = player.getPositionSeconds();
        setAButton.setButtonText ("A = " + formatTime (pendingLoopStart));
    };

    setBButton.onClick = [this]
    {
        const auto start = pendingLoopStart >= 0.0 ? pendingLoopStart : 0.0;
        const auto end = player.getPositionSeconds();

        if (std::abs (end - start) > 0.05)
        {
            player.setLoopSeconds (start, end);
            player.setLoopEnabled (true);
        }

        pendingLoopStart = -1.0;
        updateLoopButtons();
    };

    loopButton.setClickingTogglesState (true);
    loopButton.onClick = [this]
    {
        player.setLoopEnabled (loopButton.getToggleState());
        updateLoopButtons();
    };

    clearLoopButton.onClick = [this]
    {
        player.setLoopEnabled (false);
        player.setLoopSeconds (0.0, 0.0);
        pendingLoopStart = -1.0;
        updateLoopButtons();
    };

    styleLabel (speedLabel, "Speed");
    styleHorizontal (speedSlider, 25.0, 150.0, 1.0, 100.0, " %");
    speedSlider.onValueChange = [this] { player.setSpeed (speedSlider.getValue() / 100.0); };

    styleLabel (pitchLabel, "Pitch");
    styleHorizontal (pitchSlider, -12.0, 12.0, 0.1, 0.0, " st");
    pitchSlider.onValueChange = [this] { player.setSemitones (pitchSlider.getValue()); };

    styleLabel (songVolumeLabel, "Volume");
    styleHorizontal (songVolumeSlider, -40.0, 6.0, 0.5, 0.0, " dB");
    songVolumeSlider.onValueChange = [this] { player.gain = juce::Decibels::decibelsToGain ((float) songVolumeSlider.getValue(), -40.0f); };

    styleLabel (hintLabel, "Tap the waveform to jump, drag to loop, drag a loop edge to adjust it. Zoom: pinch, or Ctrl+wheel. Double-click a slider to reset it.", 0.45f);
    hintLabel.setMinimumHorizontalScale (0.5f);

    updateLoopButtons();

    // Metronome --------------------------------------------------------------
    beatIndicator = std::make_unique<BeatIndicator> (metronome);

    for (auto* c : std::initializer_list<juce::Component*> { &metronomeHeader, &bpmDisplay, &metronomeButton, beatIndicator.get(),
                                                             &bpmSlider, &clickVolumeSlider, &bpmLabel, &beatsLabel,
                                                             &clickVolumeLabel, &beatsBox, &trainerToggle, &stepSlider,
                                                             &intervalSlider, &maxBpmSlider, &unitBox, &stepLabel,
                                                             &everyLabel, &maxLabel })
        addAndMakeVisible (c);

    styleHeader (metronomeHeader, "Metronome");

    bpmDisplay.setFont (juce::FontOptions (34.0f).withStyle ("Bold"));
    bpmDisplay.setColour (juce::Label::textColourId, theme::highlight);
    bpmDisplay.setJustificationType (juce::Justification::centredRight);

    metronomeButton.onClick = [this]
    {
        if (metronome.isRunning())
            metronome.stop();
        else
            metronome.start();
    };

    styleLabel (bpmLabel, "Tempo");
    styleHorizontal (bpmSlider, 30.0, 300.0, 1.0, metronome.startBpm.load(), " bpm");
    bpmSlider.onValueChange = [this]
    {
        metronome.startBpm = bpmSlider.getValue();
        metronome.tempoChangedByUser();
    };

    styleLabel (beatsLabel, "Beats per bar");

    for (int i = 1; i <= 12; ++i)
        beatsBox.addItem (juce::String (i), i);

    beatsBox.setSelectedId (metronome.beatsPerBar.load(), juce::dontSendNotification);
    beatsBox.onChange = [this] { metronome.beatsPerBar = juce::jmax (1, beatsBox.getSelectedId()); };

    styleLabel (clickVolumeLabel, "Click volume");
    styleHorizontal (clickVolumeSlider, -40.0, 6.0, 0.5, juce::Decibels::gainToDecibels (metronome.gain.load()), " dB");
    clickVolumeSlider.onValueChange = [this] { metronome.gain = juce::Decibels::decibelsToGain ((float) clickVolumeSlider.getValue(), -40.0f); };

    trainerToggle.setToggleState (metronome.trainerEnabled.load(), juce::dontSendNotification);
    trainerToggle.onClick = [this]
    {
        metronome.trainerEnabled = trainerToggle.getToggleState();
        updateTrainerControls();
    };

    styleLabel (stepLabel, "Add");
    styleIncDec (stepSlider, -20.0, 20.0, 1.0, metronome.trainerStepBpm.load());
    stepSlider.setTextValueSuffix (" bpm");
    stepSlider.onValueChange = [this] { metronome.trainerStepBpm = stepSlider.getValue(); };

    styleLabel (everyLabel, "every");
    styleIncDec (intervalSlider, 0.5, 60.0, 0.5, metronome.trainerInterval.load());
    intervalSlider.onValueChange = [this] { metronome.trainerInterval = intervalSlider.getValue(); };

    unitBox.addItem ("bars", 1);
    unitBox.addItem ("minutes", 2);
    unitBox.setSelectedId (metronome.trainerUnit.load() + 1, juce::dontSendNotification);
    unitBox.onChange = [this] { metronome.trainerUnit = unitBox.getSelectedId() - 1; };

    styleLabel (maxLabel, "up to");
    styleIncDec (maxBpmSlider, 30.0, 300.0, 1.0, metronome.trainerMaxBpm.load());
    maxBpmSlider.setTextValueSuffix (" bpm");
    maxBpmSlider.onValueChange = [this] { metronome.trainerMaxBpm = maxBpmSlider.getValue(); };

    updateTrainerControls();
    startTimerHz (30);
}

PracticePanel::~PracticePanel()
{
    stopTimer();
}

//==============================================================================
juce::String PracticePanel::formatTime (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const auto minutes = (int) (seconds / 60.0);
    const auto rest = seconds - minutes * 60.0;
    return juce::String (minutes) + ":" + (rest < 10.0 ? "0" : "") + juce::String (rest, 1);
}

void PracticePanel::openFile()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Choose a song",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg");

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& chooser)
    {
        const auto url = chooser.getURLResult();

        if (url.isEmpty())
            return;

        songLabel.setText ("Loading " + juce::URL::removeEscapeChars (url.getFileName()) + "...", juce::dontSendNotification);
        openButton.setEnabled (false);

        player.loadAsync (url, [safeThis = SafePointer<PracticePanel> (this)] (const juce::String& error)
        {
            if (safeThis == nullptr)
                return;

            safeThis->openButton.setEnabled (true);

            if (error.isNotEmpty())
            {
                safeThis->songLabel.setText ("Could not load: " + error, juce::dontSendNotification);
                return;
            }

            // The timer notices the new song and updates the waveform and labels.
        });
    });
}

void PracticePanel::togglePlay()
{
    if (player.isPlaying())
        player.pause();
    else
        player.play();
}

void PracticePanel::updateLoopButtons()
{
    loopButton.setEnabled (player.hasLoop());
    loopButton.setToggleState (player.isLoopEnabled(), juce::dontSendNotification);
    loopButton.setButtonText (player.hasLoop() ? "Loop " + formatTime (player.getLoopStartSeconds())
                                                     + " - " + formatTime (player.getLoopEndSeconds())
                                               : juce::String ("Loop"));
    loopButton.setColour (juce::TextButton::buttonOnColourId, theme::accent);
    clearLoopButton.setEnabled (player.hasLoop());
    zoomLoopButton.setEnabled (player.hasLoop());

    if (pendingLoopStart < 0.0)
        setAButton.setButtonText ("Set A");

    waveform.repaint();
}

void PracticePanel::updateTrainerControls()
{
    const auto on = trainerToggle.getToggleState();

    for (auto* c : std::initializer_list<juce::Component*> { &stepSlider, &intervalSlider, &maxBpmSlider, &unitBox,
                                                             &stepLabel, &everyLabel, &maxLabel })
        c->setEnabled (on);
}

bool PracticePanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }

    return false;
}

void PracticePanel::timerCallback()
{
    if (auto song = player.getSong(); song != shownSong)
    {
        shownSong = song;
        songLabel.setText (song != nullptr ? song->name : juce::String ("No song loaded"), juce::dontSendNotification);
        pendingLoopStart = -1.0;
        waveform.songChanged();
        updateLoopButtons();
    }

    const auto hasSong = player.hasSong();

    for (auto* b : { &rewindButton, &playButton, &stopButton, &setAButton, &setBButton, &zoomOutButton, &zoomInButton, &fitButton })
        b->setEnabled (hasSong);

    playButton.setButtonText (player.isPlaying() ? "Pause" : "Play");
    timeLabel.setText (formatTime (player.getPositionSeconds()) + " / " + formatTime (player.getLengthSeconds()),
                       juce::dontSendNotification);

    if (hasSong)
        waveform.refresh();

    metronomeButton.setButtonText (metronome.isRunning() ? "Stop metronome" : "Start metronome");
    bpmDisplay.setText (juce::String (juce::roundToInt (metronome.isRunning() ? metronome.getCurrentBpm()
                                                                               : metronome.startBpm.load())) + " bpm",
                        juce::dontSendNotification);
    beatIndicator->update();
}

//==============================================================================
void PracticePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void PracticePanel::resized()
{
    auto area = getLocalBounds().reduced (12, 8);

    // Song header row
    auto top = area.removeFromTop (44);
    songHeader.setBounds (top.removeFromLeft (70));
    openButton.setBounds (top.removeFromLeft (170).reduced (3));
    timeLabel.setBounds (top.removeFromRight (190));
    top.removeFromRight (12);
    zoomLoopButton.setBounds (top.removeFromRight (130).reduced (3));
    fitButton.setBounds (top.removeFromRight (60).reduced (3));
    zoomInButton.setBounds (top.removeFromRight (48).reduced (3));
    zoomOutButton.setBounds (top.removeFromRight (48).reduced (3));
    songLabel.setBounds (top.reduced (8, 0));

    // Waveform (includes the overview strip at its bottom)
    waveform.setBounds (area.removeFromTop (juce::jmax (170, area.getHeight() * 38 / 100)).reduced (0, 4));

    // Transport
    auto transport = area.removeFromTop (52).reduced (0, 4);
    const auto small = 80;
    rewindButton.setBounds (transport.removeFromLeft (60).reduced (2, 0));
    playButton.setBounds (transport.removeFromLeft (110).reduced (2, 0));
    stopButton.setBounds (transport.removeFromLeft (small).reduced (2, 0));
    transport.removeFromLeft (20);
    clearLoopButton.setBounds (transport.removeFromRight (110).reduced (2, 0));
    loopButton.setBounds (transport.removeFromRight (220).reduced (2, 0));
    setBButton.setBounds (transport.removeFromRight (small).reduced (2, 0));
    setAButton.setBounds (transport.removeFromRight (120).reduced (2, 0));

    area.removeFromTop (10);

    // Two columns: song controls | metronome
    const auto gap = 24;
    auto left = area.removeFromLeft ((area.getWidth() - gap) / 2);
    area.removeFromLeft (gap);
    auto right = area;

    auto sliderRow = [] (juce::Rectangle<int>& column, juce::Label& label, juce::Component& control, int height = 40)
    {
        auto row = column.removeFromTop (height);
        label.setBounds (row.removeFromLeft (110));
        control.setBounds (row.reduced (0, 4));
    };

    sliderRow (left, speedLabel, speedSlider);
    sliderRow (left, pitchLabel, pitchSlider);
    sliderRow (left, songVolumeLabel, songVolumeSlider);
    hintLabel.setBounds (left.removeFromTop (30));

    // Metronome column
    auto metroTop = right.removeFromTop (48);
    metronomeHeader.setBounds (metroTop.removeFromLeft (110));
    bpmDisplay.setBounds (metroTop.removeFromRight (160));
    metronomeButton.setBounds (metroTop.removeFromLeft (180).reduced (3));
    beatIndicator->setBounds (metroTop.reduced (12, 10));

    sliderRow (right, bpmLabel, bpmSlider);

    {
        auto row = right.removeFromTop (40);
        beatsLabel.setBounds (row.removeFromLeft (110));
        beatsBox.setBounds (row.removeFromLeft (80).reduced (0, 5));
        row.removeFromLeft (20);
        clickVolumeLabel.setBounds (row.removeFromLeft (100));
        clickVolumeSlider.setBounds (row.reduced (0, 4));
    }

    trainerToggle.setBounds (right.removeFromTop (36));

    {
        auto row = right.removeFromTop (44);
        row.removeFromLeft (24);
        stepLabel.setBounds (row.removeFromLeft (44));
        stepSlider.setBounds (row.removeFromLeft (170).reduced (0, 3));
        row.removeFromLeft (12);
        everyLabel.setBounds (row.removeFromLeft (52));
        intervalSlider.setBounds (row.removeFromLeft (150).reduced (0, 3));
        unitBox.setBounds (row.removeFromLeft (110).reduced (6, 6));
    }

    {
        auto row = right.removeFromTop (44);
        row.removeFromLeft (24);
        maxLabel.setBounds (row.removeFromLeft (44));
        maxBpmSlider.setBounds (row.removeFromLeft (170).reduced (0, 3));
    }
}

} // namespace pv
