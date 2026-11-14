#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AppEngine.h"

namespace pv
{

/** The "Looper" tab: four pedal-style tracks locked to the metronome.
    Only the UI lives here: loops keep playing while other tabs are shown.
*/
class LooperPanel final : public juce::Component,
                          private juce::Timer,
                          private juce::ChangeListener
{
public:
    explicit LooperPanel (AppEngine&);
    ~LooperPanel() override;

    /** Supplied by the owner: shows a component full-window (the drum plugin's UI). */
    std::function<void (const juce::String& title, std::unique_ptr<juce::Component>)> showOverlay;
    std::function<void()> hideOverlay;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class TrackRow;
    class BeatDots;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;     // plugin catalogue
    void refreshDrumList();
    void drumChosen();
    void openDrumEditor();
    void loadSettings();
    void saveSettings() const;

    AppEngine& engine;
    Looper& looper;
    MetronomeProcessor& metronome;

    juce::Label header, hintLabel, tempoDisplay, latencyLabel;
    juce::TextButton afterFxButton { "After FX" }, beforeFxButton { "Before FX" };
    juce::Label sourceLabel, lengthLabel, tempoLabel, beatsLabel, offsetLabel;
    juce::ComboBox lengthBox, beatsBox;
    juce::Slider tempoSlider, offsetSlider;
    juce::TextButton metronomeButton { "Start metronome" }, undoButton { "Undo overdub" },
                     muteAllButton { "Mute all" }, clearAllButton { "Clear all" };
    std::unique_ptr<BeatDots> beatDots;

    // Drum machine (an instrument plugin following the bar clock)
    juce::Label drumsLabel, drumStatus;
    juce::ComboBox drumBox;
    juce::TextButton drumEditButton { "Open UI" };
    juce::Slider drumVolume;
    juce::ToggleButton clicksMutedButton { "Mute clicks" };
    std::vector<PluginCatalogue::Entry> drumEntries;
    juce::String shownDrumIdentifier;
    bool drumLoading = false;
    std::array<std::unique_ptr<TrackRow>, Looper::numTracks> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperPanel)
};

} // namespace pv
