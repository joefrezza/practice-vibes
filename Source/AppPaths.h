#pragma once

#include <juce_core/juce_core.h>

namespace pv
{

/** Where the app keeps its own files.

    On iOS this is the app's Documents folder, which (with file sharing enabled)
    is visible in the Files app under "On My iPad > Practice Vibes". That makes
    the log file easy to retrieve when debugging on a device without a Mac.
*/
inline juce::File getAppDataFolder()
{
   #if JUCE_IOS
    auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
   #else
    auto folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Practice Vibes");
   #endif

    folder.createDirectory();
    return folder;
}

inline juce::File getLogFile()            { return getAppDataFolder().getChildFile ("PracticeVibes.log"); }
inline juce::File getChainFile()          { return getAppDataFolder().getChildFile ("chain.xml"); }
inline juce::File getLooperSettingsFile() { return getAppDataFolder().getChildFile ("looper.xml"); }
inline juce::File getFootswitchFile()     { return getAppDataFolder().getChildFile ("footswitch.xml"); }
inline juce::File getDeviceStateFile()    { return getAppDataFolder().getChildFile ("audio-device.xml"); }
inline juce::File getPluginCacheFile()    { return getAppDataFolder().getChildFile ("plugin-cache.xml"); }
inline juce::File getDeadMansPedalFile()  { return getAppDataFolder().getChildFile ("plugin-scan-in-progress.txt"); }

} // namespace pv
