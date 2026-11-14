#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ChordView.h"
#include "MetronomeProcessor.h"
#include "PlayerProcessor.h"
#include "WaveformView.h"

namespace pv
{

/** The "Practice" tab: song player (speed, pitch, A-B loop) and metronome. */
class PracticePanel final : public juce::Component,
                            private juce::Timer
{
public:
    PracticePanel (PlayerProcessor&, MetronomeProcessor&);
    ~PracticePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class BeatIndicator;

    void timerCallback() override;
    void openFile();
    void togglePlay();
    void updateLoopButtons();
    void updateTrainerControls();
    void startChordAnalysis (std::shared_ptr<const LoadedSong>);

    static juce::String formatTime (double seconds);

    PlayerProcessor& player;
    MetronomeProcessor& metronome;

    std::unique_ptr<juce::FileChooser> fileChooser;

    // Song
    juce::Label songHeader, songLabel, timeLabel;
    juce::TextButton openButton { "Open audio file" };
    WaveformView waveform;
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" }, fitButton { "Fit" }, zoomLoopButton { "Zoom to loop" };
    juce::TextButton snapButton;
    void updateSnapButton();
    juce::TextButton rewindButton { "|<" }, playButton { "Play" }, stopButton { "Stop" },
                     setAButton { "Set A" }, setBButton { "Set B" }, loopButton { "Loop" }, clearLoopButton { "Clear loop" };
    double pendingLoopStart = -1.0;
    std::shared_ptr<const LoadedSong> shownSong;   // detects songs loaded from elsewhere (e.g. command line)

    // Chords: analysed in the background whenever a song is loaded.
    ChordView chordView;
    juce::ThreadPool chordPool { 1 };
    std::shared_ptr<std::atomic<bool>> chordCancel;

    juce::Slider speedSlider, pitchSlider, songVolumeSlider;
    juce::Label speedLabel, pitchLabel, songVolumeLabel, hintLabel;

    // Metronome
    juce::Label metronomeHeader, bpmDisplay;
    juce::TextButton metronomeButton { "Start metronome" };
    std::unique_ptr<BeatIndicator> beatIndicator;
    juce::Slider bpmSlider, clickVolumeSlider;
    juce::Label bpmLabel, beatsLabel, clickVolumeLabel;
    juce::ComboBox beatsBox, subdivisionBox, soundBox;
    juce::Label subdivisionLabel, soundLabel, subLevelLabel;
    juce::Slider subLevelSlider;

    juce::ToggleButton trainerToggle { "Speed trainer" };
    juce::ToggleButton followToggle { "Follow song beats" };
    void updateFollowControls();
    bool tempoWasLocked = false, songGridFound = false;
    juce::Slider stepSlider, intervalSlider, maxBpmSlider;
    juce::ComboBox unitBox;
    juce::Label stepLabel, everyLabel, maxLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PracticePanel)
};

} // namespace pv
