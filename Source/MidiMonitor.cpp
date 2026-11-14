#include "MidiMonitor.h"

namespace pv
{

MidiMonitor::MidiMonitor (juce::AudioDeviceManager& dm, std::function<void (const juce::MidiMessage&)> realtimeCallback)
    : deviceManager (dm), realtime (std::move (realtimeCallback))
{
    deviceManager.addMidiInputDeviceCallback ({}, this);   // {} = all enabled inputs

    deviceListConnection = juce::MidiDeviceListConnection::make ([this]
    {
        juce::Logger::writeToLog ("MIDI device list changed");
        enableAllInputs();

        if (onDevicesChanged)
            onDevicesChanged();
    });

    enableAllInputs();
}

MidiMonitor::~MidiMonitor()
{
    alive.reset();
    deviceListConnection.reset();
    deviceManager.removeMidiInputDeviceCallback ({}, this);
}

void MidiMonitor::enableAllInputs()
{
    for (auto& info : juce::MidiInput::getAvailableDevices())
    {
        if (! deviceManager.isMidiInputDeviceEnabled (info.identifier))
        {
            deviceManager.setMidiInputDeviceEnabled (info.identifier, true);
            juce::Logger::writeToLog ("MIDI input enabled: " + info.name);
        }
    }
}

juce::StringArray MidiMonitor::getEnabledInputNames() const
{
    juce::StringArray names;

    for (auto& info : juce::MidiInput::getAvailableDevices())
        if (deviceManager.isMidiInputDeviceEnabled (info.identifier))
            names.add (info.name);

    return names;
}

void MidiMonitor::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message)
{
    if (message.isActiveSense() || message.isMidiClock())
        return;

    if (realtime)
        realtime (message);

    const auto sourceName = source != nullptr ? source->getName() : juce::String ("unknown");
    std::weak_ptr<bool> weakAlive = alive;

    juce::MessageManager::callAsync ([this, weakAlive, message, sourceName]
    {
        if (! weakAlive.expired() && onMessage)
            onMessage (message, sourceName);
    });
}

} // namespace pv
