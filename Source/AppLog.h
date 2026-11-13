#pragma once

#include <juce_events/juce_events.h>

namespace pv
{

/** Application-wide log.

    Installs itself as JUCE's current Logger, so juce::Logger::writeToLog()
    works from anywhere. Lines are kept in memory for the on-screen log view and
    appended to a file (see getLogFile()). The previous session's log is kept
    alongside as "PracticeVibes.old.log".
*/
class AppLog final : public juce::Logger,
                     public juce::ChangeBroadcaster
{
public:
    AppLog();
    ~AppLog() override;

    /** The most recent lines, newest last. Thread-safe. */
    juce::String getText() const;

    juce::File getFile() const { return file; }

protected:
    void logMessage (const juce::String& message) override;

private:
    juce::File file;
    std::unique_ptr<juce::FileOutputStream> stream;
    mutable juce::CriticalSection lock;
    juce::StringArray recent;

    JUCE_DECLARE_NON_COPYABLE (AppLog)
};

} // namespace pv
