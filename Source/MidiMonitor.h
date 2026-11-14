#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace pv
{

/** Enables every MIDI input (including Bluetooth ones once paired), keeps doing
    so as devices come and go, and forwards incoming messages to the message thread.
*/
class MidiMonitor final : private juce::MidiInputCallback
{
public:
    /** `realtime` (optional) is called on the MIDI input thread for every message, before the
        hop to the message thread: for sending notes to the audio graph without extra delay. */
    explicit MidiMonitor (juce::AudioDeviceManager&,
                          std::function<void (const juce::MidiMessage&)> realtime = {});
    ~MidiMonitor() override;

    void enableAllInputs();
    juce::StringArray getEnabledInputNames() const;

    /** Called on the message thread for each incoming message (clock/active-sensing filtered out). */
    std::function<void (const juce::MidiMessage&, const juce::String& sourceName)> onMessage;

    /** Called on the message thread when MIDI devices are connected or disconnected. */
    std::function<void()> onDevicesChanged;

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

    juce::AudioDeviceManager& deviceManager;
    const std::function<void (const juce::MidiMessage&)> realtime;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    juce::MidiDeviceListConnection deviceListConnection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMonitor)
};

} // namespace pv
