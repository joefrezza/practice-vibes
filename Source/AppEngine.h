#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "MetronomeProcessor.h"
#include "MidiMonitor.h"
#include "PlayerProcessor.h"
#include "PluginCatalogue.h"
#include "PluginChain.h"

namespace pv
{

/** Owns the non-UI parts of the app: audio device, plugin formats, the chain, MIDI. */
class AppEngine final : private juce::ChangeListener
{
public:
    AppEngine();
    ~AppEngine();

    /** Requests microphone permission, opens the audio device and restores the saved chain. */
    void start();

    /** Writes the audio device settings and the plugin chain to disk. */
    void saveState();

    void logAudioDeviceInfo();

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;

    std::unique_ptr<PluginChain> chain;
    std::unique_ptr<PluginCatalogue> catalogue;
    std::unique_ptr<MidiMonitor> midi;

    // Owned by the audio graph inside `chain`; valid while `chain` exists.
    PlayerProcessor* player = nullptr;
    MetronomeProcessor* metronome = nullptr;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    /** Name of the system default output for the current device type ("" if unknown). */
    juce::String getSystemDefaultOutputName() const;

    // While true, the output device tracks the OS default output (like normal apps do).
    // It becomes false when the user picks a different output in the audio settings,
    // and true again when they pick the current default.
    bool followSystemOutput = true;
    juce::String lastOutputName;

    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppEngine)
};

} // namespace pv
