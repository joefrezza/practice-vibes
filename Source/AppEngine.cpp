#include "AppEngine.h"
#include "AppPaths.h"

namespace pv
{

AppEngine::AppEngine()
{
    juce::addDefaultFormatsToManager (formatManager);

    juce::StringArray formatNames;

    for (auto* format : formatManager.getFormats())
        formatNames.add (format->getName());

    juce::Logger::writeToLog ("Plugin formats: " + (formatNames.isEmpty() ? juce::String ("none") : formatNames.joinIntoString (", ")));

    chain = std::make_unique<PluginChain> (deviceManager, formatManager);
    catalogue = std::make_unique<PluginCatalogue> (formatManager);
    midi = std::make_unique<MidiMonitor> (deviceManager);
}

AppEngine::~AppEngine()
{
    alive.reset();
    midi.reset();
    catalogue.reset();
    chain.reset();
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
        const auto error = deviceManager.initialise (granted ? 2 : 0, 2, savedState.get(), true);

        if (error.isNotEmpty())
            juce::Logger::writeToLog ("Audio device error: " + error);

        logAudioDeviceInfo();

        if (auto chainXml = juce::parseXML (getChainFile()))
            chain->restoreFromXml (*chainXml);
    });
}

void AppEngine::saveState()
{
    if (auto deviceXml = deviceManager.createStateXml())
        deviceXml->writeTo (getDeviceStateFile());

    if (auto chainXml = chain->createStateXml())
        if (chainXml->writeTo (getChainFile()))
            juce::Logger::writeToLog ("Saved chain to " + getChainFile().getFullPathName());
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
