#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "GainMeterProcessor.h"

namespace pv
{

/** The app's audio graph:

        device input (one selected channel)
          -> input stage (gain + meter, mono)
          -> plugin 1 -> plugin 2 -> ...        (AUv3 on iPad, VST3 on Windows)
          -> guitar output stage (gain + mute + meter, stereo)  --+
                                                                   +-> master (meter) -> device output
        other sources (song player, metronome...)  ---------------+

    All methods must be called on the message thread.
*/
class PluginChain final : private juce::ChangeListener
{
public:
    PluginChain (juce::AudioDeviceManager&, juce::AudioPluginFormatManager&);
    ~PluginChain() override;

    int getNumPlugins() const noexcept                  { return (int) slots.size(); }
    juce::String getPluginName (int index) const;
    juce::AudioPluginInstance* getPlugin (int index) const;
    bool isBypassed (int index) const;
    void setBypassed (int index, bool shouldBeBypassed);

    using LoadCallback = std::function<void (const juce::String& errorOrEmpty)>;

    /** Loads a plugin asynchronously (required for AUv3) and appends it to the chain. */
    void addPlugin (const juce::PluginDescription&, LoadCallback onDone = {});

    void removePlugin (int index);
    void movePlugin (int index, int delta);
    void clear();

    void setInputChannel (int channel);
    int getInputChannel() const noexcept                { return inputChannel; }

    GainMeterProcessor& getInputStage() const;
    GainMeterProcessor& getOutputStage() const;     // guitar output (monitor on/off)
    GainMeterProcessor& getMasterStage() const;

    /** Adds a stereo source (e.g. the song player) that is mixed into the master output.
        The graph owns the processor; the returned pointer stays valid while this chain exists.
    */
    template <typename ProcessorType>
    ProcessorType* addSource (std::unique_ptr<ProcessorType> processor)
    {
        auto* raw = processor.get();
        sources.push_back (graph.addNode (std::move (processor), std::nullopt, juce::AudioProcessorGraph::UpdateKind::none));
        rebuildConnections();
        return raw;
    }

    /** Serialises the chain, including every plugin's own state. */
    std::unique_ptr<juce::XmlElement> createStateXml() const;

    /** Replaces the chain with one previously saved by createStateXml(). */
    void restoreFromXml (const juce::XmlElement&, std::function<void()> onDone = {});

    /** Called (on the message thread) whenever the chain's contents change. */
    std::function<void()> onChange;

private:
    using NodePtr = juce::AudioProcessorGraph::Node::Ptr;
    using NodeCallback = std::function<void (NodePtr, const juce::String& error)>;

    struct Slot
    {
        NodePtr node;
        juce::PluginDescription description;
    };

    struct PendingSlot
    {
        juce::PluginDescription description;
        juce::MemoryBlock state;
        bool bypassed = false;
    };

    void loadPlugin (const juce::PluginDescription&, NodeCallback);
    void restoreNext (std::shared_ptr<std::vector<PendingSlot>>, size_t index, std::function<void()> onDone);
    void rebuildConnections();
    void notifyChange();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::AudioDeviceManager& deviceManager;
    juce::AudioPluginFormatManager& formatManager;

    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;

    NodePtr audioInNode, audioOutNode, inputStageNode, outputStageNode, masterNode;
    std::vector<Slot> slots;
    std::vector<NodePtr> sources;
    int inputChannel = 0;

    // Async plugin-creation callbacks check this so they never touch a deleted chain.
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

} // namespace pv
