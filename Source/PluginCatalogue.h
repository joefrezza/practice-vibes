#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace pv
{

/** The list of plugins the user can add to the chain.

    - AudioUnits (AUv3 on iPad) are listed instantly from the system registry,
      without loading each plugin. A plugin is only instantiated when chosen.
    - Desktop formats (VST3 on Windows) need a one-off scan, which runs on a
      background thread and is cached to disk.
*/
class PluginCatalogue final : public juce::ChangeBroadcaster,
                              private juce::Thread
{
public:
    struct Entry
    {
        juce::PluginDescription description;
        juce::String kind;      // "Effect", "Instrument", "MIDI FX", ...
    };

    explicit PluginCatalogue (juce::AudioPluginFormatManager&);
    ~PluginCatalogue() override;

    /** Rebuilds the list (fast). Message thread only. */
    void refresh();

    /** True if some installed format needs a directory scan (i.e. on desktop). */
    bool canScan() const;

    void startScan();
    bool isScanning() const     { return isThreadRunning(); }

    juce::String getStatus() const;
    std::vector<Entry> getEntries() const;

private:
    void run() override;
    void setStatus (const juce::String&);

    juce::AudioPluginFormatManager& formatManager;
    juce::KnownPluginList scannedList;

    mutable juce::CriticalSection lock;
    std::vector<Entry> entries;
    juce::String status;

    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginCatalogue)
};

} // namespace pv
