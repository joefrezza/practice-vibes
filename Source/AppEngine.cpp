#include "AppEngine.h"
#include "AppPaths.h"

namespace pv
{

namespace
{
    // Our own attribute in the device-state XML; JUCE ignores unknown attributes.
    const juce::Identifier followAttribute { "pvFollowSystemOutput" };
}

AppEngine::AppEngine()
{
    juce::addDefaultFormatsToManager (formatManager);

    juce::StringArray formatNames;

    for (auto* format : formatManager.getFormats())
        formatNames.add (format->getName());

    juce::Logger::writeToLog ("Plugin formats: " + (formatNames.isEmpty() ? juce::String ("none") : formatNames.joinIntoString (", ")));

    chain = std::make_unique<PluginChain> (deviceManager, formatManager);
    player = chain->addSource (std::make_unique<PlayerProcessor>());
    metronome = chain->addSource (std::make_unique<MetronomeProcessor>());

    if (metronome != nullptr && player != nullptr)
        metronome->setPlayer (player);

    looper = std::make_unique<Looper> (*metronome);
    chain->setLooperNodes (looper->createBeforeFxNode(), looper->createAfterFxNode());
    chain->setBlockListener (this);
    chain->setPlayHead (&playHead);
    startTimerHz (2);
    catalogue = std::make_unique<PluginCatalogue> (formatManager);
    midi = std::make_unique<MidiMonitor> (deviceManager);
}

AppEngine::~AppEngine()
{
    stopTimer();
    alive.reset();
    deviceManager.removeChangeListener (this);
    midi.reset();
    catalogue.reset();
    player = nullptr;
    metronome = nullptr;
    chain.reset();      // stops audio and deletes the looper's nodes before the looper
    looper.reset();
    deviceManager.closeAudioDevice();
}

void AppEngine::start()
{
    std::weak_ptr<bool> weakAlive = alive;

    juce::RuntimePermissions::request (juce::RuntimePermissions::recordAudio, [this, weakAlive] (bool granted)
    {
        if (weakAlive.expired())
            return;

        if (! granted)
            juce::Logger::writeToLog ("Audio input permission was NOT granted: the guitar input will be silent");

        auto savedState = juce::parseXML (getDeviceStateFile());

        if (savedState != nullptr)
        {
            followSystemOutput = savedState->getBoolAttribute (followAttribute, false);

            // The saved output name is stale when following: open today's default instead.
            if (followSystemOutput)
                for (auto* type : deviceManager.getAvailableDeviceTypes())
                    if (type->getTypeName() == savedState->getStringAttribute ("deviceType"))
                    {
                        type->scanForDevices();
                        const auto defaultName = type->getDeviceNames (false)[type->getDefaultDeviceIndex (false)];

                        if (defaultName.isNotEmpty())
                            savedState->setAttribute ("audioOutputDeviceName", defaultName);
                    }
        }

        const auto error = deviceManager.initialise (granted ? 2 : 0, 2, savedState.get(), true);

        if (error.isNotEmpty())
            juce::Logger::writeToLog ("Audio device error: " + error);

        lastOutputName = deviceManager.getAudioDeviceSetup().outputDeviceName;
        juce::Logger::writeToLog (followSystemOutput ? "Output follows the system default output"
                                                     : "Output fixed to the device chosen in the audio settings");
        deviceManager.addChangeListener (this);

        logAudioDeviceInfo();

        if (auto chainXml = juce::parseXML (getChainFile()))
            chain->restoreFromXml (*chainXml);
    });
}

void AppEngine::saveState()
{
    if (auto deviceXml = deviceManager.createStateXml())
    {
        deviceXml->setAttribute (followAttribute, followSystemOutput);
        deviceXml->writeTo (getDeviceStateFile());
    }

    if (auto chainXml = chain->createStateXml())
        if (chainXml->writeTo (getChainFile()))
            juce::Logger::writeToLog ("Saved chain to " + getChainFile().getFullPathName());
}

juce::String AppEngine::getSystemDefaultOutputName() const
{
    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
        return type->getDeviceNames (false)[type->getDefaultDeviceIndex (false)];

    return {};
}

// Called (asynchronously) after any device change: the user editing the audio settings,
// a device appearing/disappearing, or the OS default output changing (JUCE's WASAPI type
// reports that as a device-list change, but keeps the old device open).
void AppEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    const auto defaultName = getSystemDefaultOutputName();
    const bool outputChanged = setup.outputDeviceName != lastOutputName;
    lastOutputName = setup.outputDeviceName;

    if (! followSystemOutput)
    {
        // Picking the current default (or JUCE falling back to it after the chosen
        // device was unplugged) goes back to following.
        if (outputChanged && defaultName.isNotEmpty() && setup.outputDeviceName == defaultName)
        {
            followSystemOutput = true;
            juce::Logger::writeToLog ("Output follows the system default output again");
        }

        return;
    }

    if (outputChanged && setup.outputDeviceName != defaultName)
    {
        followSystemOutput = false;
        juce::Logger::writeToLog ("Output fixed to " + setup.outputDeviceName + " (chosen in the audio settings)");
        return;
    }

    if (defaultName.isEmpty() || setup.outputDeviceName == defaultName)
        return;

    juce::Logger::writeToLog ("System default output changed: switching to " + defaultName);
    setup.outputDeviceName = defaultName;
    lastOutputName = defaultName;

    // Not "chosen": the saved state keeps meaning "follow the default".
    const auto error = deviceManager.setAudioDeviceSetup (setup, false);

    if (error.isNotEmpty())
        juce::Logger::writeToLog ("Could not switch output: " + error);
}

void AppEngine::beginBlock (juce::int64 blockStart, int numSamples)
{
    // Audio thread: the bar clock first, then what reads this block's bar lines.
    metronome->advanceClock (blockStart, numSamples);
    playHead.update (*metronome, blockStart);
    looper->beginBlock (blockStart, numSamples);
}

void AppEngine::timerCallback()
{
    int deviceLatency = 0;

    if (auto* device = deviceManager.getCurrentAudioDevice())
        deviceLatency = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();

    looper->deviceLatencySamples = deviceLatency;
    looper->chainLatencySamples = chain->getPluginLatencySamples();
}

void AppEngine::logAudioDeviceInfo()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        juce::Logger::writeToLog ("No audio device open");
        return;
    }

    auto describeChannels = [] (const juce::StringArray& names, const juce::BigInteger& active)
    {
        juce::StringArray result;

        for (int i = 0; i < names.size(); ++i)
            if (active[i])
                result.add (names[i]);

        return result.isEmpty() ? juce::String ("none") : result.joinIntoString (", ");
    };

    juce::Logger::writeToLog ("Audio device: " + device->getTypeName() + " / " + device->getName()
                              + " | " + juce::String (device->getCurrentSampleRate(), 0) + " Hz"
                              + " | buffer " + juce::String (device->getCurrentBufferSizeSamples())
                              + " | latency in/out " + juce::String (device->getInputLatencyInSamples())
                              + "/" + juce::String (device->getOutputLatencyInSamples()) + " samples");
    juce::Logger::writeToLog ("  inputs: " + describeChannels (device->getInputChannelNames(), device->getActiveInputChannels()));
    juce::Logger::writeToLog ("  outputs: " + describeChannels (device->getOutputChannelNames(), device->getActiveOutputChannels()));
}

} // namespace pv
