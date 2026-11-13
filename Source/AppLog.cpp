#include "AppLog.h"
#include "AppPaths.h"

namespace pv
{

AppLog::AppLog()
    : file (getLogFile())
{
    if (file.existsAsFile())
    {
        auto previous = file.getSiblingFile ("PracticeVibes.old.log");
        previous.deleteFile();
        file.moveFileTo (previous);
    }

    stream = std::make_unique<juce::FileOutputStream> (file);

    if (stream->failedToOpen())
        stream.reset();

    juce::Logger::setCurrentLogger (this);
}

AppLog::~AppLog()
{
    juce::Logger::setCurrentLogger (nullptr);
}

juce::String AppLog::getText() const
{
    const juce::ScopedLock sl (lock);
    return recent.joinIntoString ("\n");
}

void AppLog::logMessage (const juce::String& message)
{
    const auto line = juce::Time::getCurrentTime().toString (false, true, true, true) + juce::String::formatted (".%03d  ", (int) (juce::Time::getCurrentTime().toMilliseconds() % 1000)) + message;

    {
        const juce::ScopedLock sl (lock);

        recent.add (line);

        if (recent.size() > 500)
            recent.removeRange (0, recent.size() - 500);

        if (stream != nullptr)
        {
            stream->writeText (line + "\n", false, false, nullptr);
            stream->flush();
        }
    }

    DBG (line);
    sendChangeMessage();
}

} // namespace pv
