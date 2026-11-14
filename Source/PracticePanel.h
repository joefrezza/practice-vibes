#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ChordView.h"
#include "MetronomeProcessor.h"
#include "PracticeLog.h"
#include "PlayerProcessor.h"
#include "WaveformView.h"

namespace pv
{

class YouTubePlayer;

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

    /** Loads a song into the player (also used by the Topics tab to open audio resources).
        `then`: speed, pitch, loop and metronome to apply once it's loaded (restoring a practice setup). */
    void loadSong (const juce::URL&, std::optional<PracticeSetup> then = std::nullopt);

    /** What's on screen now, for the practice log (songPath: the song's full path). */
    PracticeSetup captureSetup() const;

    /** Speed, pitch, loop (for the song or video showing now) and metronome settings of a saved setup. */
    void applySetup (const PracticeSetup&);

    juce::File getSongFile() const                      { return songFile; }   // the loaded song's file, if local
    bool isMediaPlaying() const;                        // the song or the video is playing
    double getMediaSpeed() const                        { return speedSlider.getValue() / 100.0; }
    juce::String getMediaName() const;

    /** YouTube mode: the video (drawn by MainComponent's YouTubePlayer over getVideoArea()) replaces the waveform;
        transport, A-B loop, speed and volume control the video. */
    void setYouTubePlayer (YouTubePlayer* p)            { youtube = p; }
    void showYouTube();
    void closeYouTube();
    bool isYouTubeMode() const noexcept                 { return youTubeMode; }
    juce::Rectangle<int> getVideoArea() const noexcept  { return videoArea; }
    std::function<void()> onCompactRequested;

private:
    class BeatIndicator;
    class YouTubeTimeline;

    // YouTube mode
    YouTubePlayer* youtube = nullptr;
    bool youTubeMode = false, compactShown = false;
    double lastShownRate = 1.0;
    juce::Rectangle<int> videoArea;
    std::unique_ptr<YouTubeTimeline> youTubeTimeline;
    juce::TextButton compactButton { "Compact player" }, closeVideoButton { "Close video" }, openOnYouTubeButton { "Open on YouTube" };
    void updateModeVisibility();
    double positionSeconds() const;
    bool loopIsEnabled() const;
    bool loopExists() const;
    double loopStartSeconds() const;
    double loopEndSeconds() const;
    void setLoop (double start, double end, bool enabled);

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
    std::shared_ptr<const LoadedSong> songOfFile;  // the song `songFile` belongs to
    juce::File songFile;

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
    juce::Label subdivisionLabel, soundLabel, subLevelLabel, swingLabel;
    juce::Slider subLevelSlider, swingSlider;
    void updateSubdivisionControls();

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
