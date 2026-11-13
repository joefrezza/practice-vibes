#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "MidiMonitor.h"
#include "PluginCatalogue.h"
#include "PluginChain.h"

namespace pv
{

/** Owns the non-UI parts of the app: audio device, plugin formats, the chain, MIDI. */
class AppEngine final
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

private:
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppEngine)
};

} // namespace pv
