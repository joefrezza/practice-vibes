#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "AppEngine.h"
#include "AppLog.h"

namespace pv
{

class PracticePanel;
class RigPanel;

/** Top-level UI: title bar, tabs (Practice / Guitar rig / Log) and a full-window
    overlay used for plugin UIs and the audio settings.
*/
class MainComponent final : public juce::Component
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
    void showOverlay (const juce::String& title, std::unique_ptr<juce::Component>);
    void closeOverlay();

    AppEngine& engine;

    juce::Label titleLabel;
    juce::TextButton audioSettingsButton { "Audio settings" },
                     bluetoothButton     { "Bluetooth MIDI" },
                     saveButton          { "Save" };

    std::unique_ptr<PracticePanel> practicePanel;
    std::unique_ptr<RigPanel> rigPanel;
    std::unique_ptr<LogPanel> logPanel;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    std::unique_ptr<Overlay> overlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace pv
