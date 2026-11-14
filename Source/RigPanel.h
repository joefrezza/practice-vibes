#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "AppEngine.h"

namespace pv
{

/** The "Guitar rig" tab: plugin catalogue, plugin chain, levels and MIDI footswitch. */
class RigPanel final : public juce::Component,
                       private juce::Timer,
                       private juce::ChangeListener
{
public:
    explicit RigPanel (AppEngine&);
    ~RigPanel() override;

    /** Supplied by the owner: shows a component full-window (plugin UIs). */
    std::function<void (const juce::String& title, std::unique_ptr<juce::Component>)> showOverlay;
    std::function<void()> hideOverlay;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ListModel;
    class LevelMeter;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void refreshCatalogue();
    void refreshChain();
    void refreshInputChannels();
    void refreshMidiDevices();
    void handleMidi (const juce::MidiMessage&, const juce::String& source);

    void addSelectedPlugin();
    void openEditor (int chainIndex);
    int selectedChainIndex() const;

    AppEngine& engine;

    std::vector<PluginCatalogue::Entry> catalogueEntries;

    juce::Label catalogueHeader, chainHeader, levelsHeader, midiHeader;

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
    juce::ToggleButton monitorButton { "Monitor guitar (use headphones!)" };

    juce::Label midiDevicesLabel, lastMidiLabel;
    juce::TextButton footswitchButton { "Footswitch setup..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigPanel)
};

} // namespace pv
