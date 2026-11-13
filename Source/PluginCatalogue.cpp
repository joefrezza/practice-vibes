#include "PluginCatalogue.h"
#include "AppPaths.h"

namespace pv
{

namespace
{
    constexpr auto audioUnitFormatName = "AudioUnit";

    /** JUCE's AudioUnit identifiers look like "AudioUnit:Effects/aufx,Sbtl,Manu". */
    juce::String kindFromAudioUnitCategory (const juce::String& category)
    {
        if (category == "Effects")      return "Effect";
        if (category == "Synths")       return "Instrument";
        if (category == "Generators")   return "Generator";
        if (category == "MidiEffects")  return "MIDI FX";
        return "Other";
    }
}

PluginCatalogue::PluginCatalogue (juce::AudioPluginFormatManager& fm)
    : juce::Thread ("Plugin scanner"),
      formatManager (fm)
{
    if (auto xml = juce::parseXML (getPluginCacheFile()))
        scannedList.recreateFromXml (*xml);

    refresh();
}

PluginCatalogue::~PluginCatalogue()
{
    alive.reset();
    stopThread (5000);
}

void PluginCatalogue::refresh()
{
    std::vector<Entry> result;

    for (auto* format : formatManager.getFormats())
    {
        if (format->getName() != audioUnitFormatName)
            continue;

        for (auto& identifier : format->searchPathsForPlugins ({}, true, true))
        {
            juce::PluginDescription description;
            description.pluginFormatName = format->getName();
            description.fileOrIdentifier = identifier;
            description.name = format->getNameOfPluginFromIdentifier (identifier);
            description.descriptiveName = description.name;
            description.category = identifier.fromFirstOccurrenceOf (":", false, false)
                                             .upToFirstOccurrenceOf ("/", false, false);
            description.isInstrument = description.category == "Synths";

            result.push_back ({ description, kindFromAudioUnitCategory (description.category) });
        }
    }

    for (auto& description : scannedList.getTypes())
        result.push_back ({ description, description.isInstrument ? "Instrument" : "Effect" });

    std::sort (result.begin(), result.end(), [] (const Entry& a, const Entry& b)
    {
        if (a.kind != b.kind)
            return a.kind == "Effect" || (b.kind != "Effect" && a.kind < b.kind);

        return a.description.name.compareIgnoreCase (b.description.name) < 0;
    });

    {
        const juce::ScopedLock sl (lock);
        entries = std::move (result);

        if (status.isEmpty())
            status = juce::String ((int) entries.size()) + " plugins available";
    }

    juce::Logger::writeToLog ("Plugin catalogue: " + juce::String ((int) getEntries().size()) + " plugins");
    sendChangeMessage();
}

bool PluginCatalogue::canScan() const
{
    for (auto* format : formatManager.getFormats())
        if (format->getName() != audioUnitFormatName)
            return true;

    return false;
}

void PluginCatalogue::startScan()
{
    if (! isScanning())
        startThread();
}

juce::String PluginCatalogue::getStatus() const
{
    const juce::ScopedLock sl (lock);
    return status;
}

std::vector<PluginCatalogue::Entry> PluginCatalogue::getEntries() const
{
    const juce::ScopedLock sl (lock);
    return entries;
}

void PluginCatalogue::setStatus (const juce::String& newStatus)
{
    {
        const juce::ScopedLock sl (lock);
        status = newStatus;
    }

    sendChangeMessage();
}

void PluginCatalogue::run()
{
    for (auto* format : formatManager.getFormats())
    {
        if (format->getName() == audioUnitFormatName)
            continue;

        juce::Logger::writeToLog ("Scanning " + format->getName() + " plugins in "
                                  + format->getDefaultLocationsToSearch().toString());

        juce::PluginDirectoryScanner scanner (scannedList, *format, format->getDefaultLocationsToSearch(),
                                              true, getDeadMansPedalFile(), true);
        juce::String pluginName;

        while (! threadShouldExit())
        {
            setStatus ("Scanning: " + juce::File (scanner.getNextPluginFileThatWillBeScanned()).getFileName());

            if (! scanner.scanNextFile (true, pluginName))
                break;
        }

        for (auto& failed : scanner.getFailedFiles())
            juce::Logger::writeToLog ("Plugin failed to load during scan: " + failed);
    }

    if (auto xml = scannedList.createXml())
        xml->writeTo (getPluginCacheFile());

    setStatus ("Scan finished: " + juce::String (scannedList.getNumTypes()) + " desktop plugins");

    std::weak_ptr<bool> weakAlive = alive;
    juce::MessageManager::callAsync ([this, weakAlive]
    {
        if (! weakAlive.expired())
            refresh();
    });
}

} // namespace pv
