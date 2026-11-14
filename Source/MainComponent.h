#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "AppEngine.h"
#include "AppLog.h"

namespace pv
{

class LibraryPanel;
class LooperPanel;
class PracticePanel;
class RigPanel;
class YouTubePlayer;

/** Top-level UI: title bar, tabs (Practice / Topics / Looper / Guitar rig / Log) and a full-window
    overlay used for plugin UIs and the audio settings.
*/
class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    MainComponent (AppEngine&, AppLog&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class LogPanel;
    class Overlay;

    void openAudioSettings();
    void toggleTuner();
    void showOverlay (const juce::String& title, std::unique_ptr<juce::Component>);
    void closeOverlay();

    void timerCallback() override;
    void layoutYouTube();
    juce::Rectangle<int> defaultCompactBounds() const;

    AppEngine& engine;

    juce::Label titleLabel;
    juce::TextButton tunerButton         { "Tuner" },
                     audioSettingsButton { "Audio settings" },
                     bluetoothButton     { "Bluetooth MIDI" },
                     saveButton          { "Save" };

    std::unique_ptr<PracticePanel> practicePanel;
    std::unique_ptr<LibraryPanel> libraryPanel;
    std::unique_ptr<LooperPanel> looperPanel;
    std::unique_ptr<RigPanel> rigPanel;
    std::unique_ptr<LogPanel> logPanel;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    std::unique_ptr<Overlay> overlay;

    // Docked over the Practice tab's video area, or floating ("compact"); never reparented (native web view).
    std::unique_ptr<YouTubePlayer> youtube;
    bool youTubeAutoCompact = false;           // compacted because you left the Practice tab while it played
    juce::Rectangle<int> lastCompactBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace pv
