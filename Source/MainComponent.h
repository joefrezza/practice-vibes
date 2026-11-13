#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "AppEngine.h"
#include "AppLog.h"

namespace pv
{

/** Feasibility-test UI: plugin catalogue, chain, levels, MIDI monitor and log.
    Deliberately plain - its job is to prove the hard parts work on the iPad.
*/
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    MainComponent (AppEngine&, AppLog&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ListModel;
    class LevelMeter;
    class Overlay;

    struct MidiTrigger
    {
        enum class Type { none, controller, note, program };
        Type type = Type::none;
        int channel = 0;
        int number = 0;

        static MidiTrigger fromMessage (const juce::MidiMessage&);
        bool matches (const juce::MidiMessage&) const;
        juce::String describe() const;
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void refreshCatalogue();
    void refreshChain();
    void refreshInputChannels();
    void refreshMidiDevices();
    void handleMidi (const juce::MidiMessage&, const juce::String& source);

    void addSelectedPlugin();
    void openEditor (int chainIndex);
    void openAudioSettings();
    void closeOverlay();
    int selectedChainIndex() const;

    AppEngine& engine;
    AppLog& appLog;

    std::vector<PluginCatalogue::Entry> catalogueEntries;

    juce::Label titleLabel, catalogueHeader, chainHeader, levelsHeader, midiHeader, logHeader;
    juce::TextButton audioSettingsButton { "Audio settings" },
                     bluetoothButton     { "Bluetooth MIDI" },
                     saveButton          { "Save chain" };

    std::unique_ptr<ListModel> catalogueModel, chainModel;
    juce::ListBox catalogueList, chainList;
    juce::TextButton addButton { "Add to chain" }, scanButton { "Scan VST3" };
    juce::Label catalogueStatus;

    juce::TextButton editButton { "Open UI" }, bypassButton { "Bypass" },
                     upButton { "Up" }, downButton { "Down" }, removeButton { "Remove" };

    juce::ComboBox inputChannelBox;
    std::unique_ptr<LevelMeter> inputMeter, outputMeter;
    juce::Slider inputGainSlider, outputGainSlider;
    juce::Label inputGainLabel, outputGainLabel;
    juce::ToggleButton monitorButton { "Monitor output (use headphones!)" };

    juce::Label midiDevicesLabel, lastMidiLabel;
    juce::TextButton learnButton { "Learn footswitch: toggle bypass" };
    MidiTrigger bypassTrigger;
    bool learning = false;

    juce::TextEditor logView;
    bool logDirty = true;

    std::unique_ptr<Overlay> overlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace pv
