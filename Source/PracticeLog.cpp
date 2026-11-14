#include "PracticeLog.h"
#include "AppPaths.h"

namespace pv
{

//==============================================================================
void PracticeSetup::toXml (juce::XmlElement& e) const
{
    if (songPath.isNotEmpty())   e.setAttribute ("song", songPath);
    if (youTubeId.isNotEmpty())  e.setAttribute ("youtube", youTubeId);
    if (youTubeTitle.isNotEmpty()) e.setAttribute ("youtubeTitle", youTubeTitle);

    if (hasMedia())
    {
        e.setAttribute ("speed", speed);
        e.setAttribute ("semitones", semitones);

        if (loopEnd > loopStart)
        {
            e.setAttribute ("loopStart", loopStart);
            e.setAttribute ("loopEnd", loopEnd);
            e.setAttribute ("loopOn", loopOn);
        }
    }

    if (metronome)
    {
        e.setAttribute ("metronome", true);
        e.setAttribute ("bpm", bpm);
        e.setAttribute ("beats", beatsPerBar);
        e.setAttribute ("subdivision", subdivision);
    }
}

PracticeSetup PracticeSetup::fromXml (const juce::XmlElement& e)
{
    PracticeSetup s;
    s.songPath = e.getStringAttribute ("song");
    s.youTubeId = e.getStringAttribute ("youtube");
    s.youTubeTitle = e.getStringAttribute ("youtubeTitle");
    s.speed = juce::jlimit (0.25, 2.0, e.getDoubleAttribute ("speed", 1.0));
    s.semitones = juce::jlimit (-12.0, 12.0, e.getDoubleAttribute ("semitones", 0.0));
    s.loopStart = juce::jmax (0.0, e.getDoubleAttribute ("loopStart", 0.0));
    s.loopEnd = juce::jmax (s.loopStart, e.getDoubleAttribute ("loopEnd", 0.0));
    s.loopOn = e.getBoolAttribute ("loopOn", false);
    s.metronome = e.getBoolAttribute ("metronome", false);
    s.bpm = juce::jlimit (0.0, 400.0, e.getDoubleAttribute ("bpm", 0.0));
    s.beatsPerBar = juce::jlimit (1, 16, e.getIntAttribute ("beats", 4));
    s.subdivision = juce::jlimit (0, 3, e.getIntAttribute ("subdivision", 0));
    return s;
}

juce::String PracticeSetup::portablePath (const juce::File& file, const juce::File& libraryRoot)
{
    if (file == juce::File())
        return {};

    if (libraryRoot != juce::File() && file.isAChildOf (libraryRoot))
        return file.getRelativePathFrom (libraryRoot).replaceCharacter ('\\', '/');

    return file.getFullPathName();
}

juce::File PracticeSetup::resolvePath (const juce::String& path, const juce::File& libraryRoot)
{
    if (path.isEmpty())
        return {};

    if (juce::File::isAbsolutePath (path))
        return juce::File (path);

    if (libraryRoot == juce::File() || path.contains ("..") || path.startsWithChar ('/') || path.startsWithChar ('\\'))
        return {};

    return libraryRoot.getChildFile (path);
}

//==============================================================================
namespace
{
    std::unique_ptr<juce::XmlElement> entryToXml (const PracticeEntry& p)
    {
        auto e = std::make_unique<juce::XmlElement> ("ENTRY");
        e->setAttribute ("id", p.id);

        if (p.topicId.isNotEmpty())
        {
            e->setAttribute ("topic", p.topicId);
            e->setAttribute ("topicTitle", p.topicTitle);
        }

        if (p.songName.isNotEmpty())
            e->setAttribute ("songName", p.songName);

        e->setAttribute ("start", p.start.toISO8601 (true));
        e->setAttribute ("end", p.end.toISO8601 (true));
        e->setAttribute ("played", juce::roundToInt (p.playedSeconds));

        if (p.maxBpm > 0.0)    e->setAttribute ("maxBpm", juce::String (p.maxBpm, 1));
        if (p.maxSpeed > 0.0)  e->setAttribute ("maxSpeed", juce::String (p.maxSpeed, 3));
        if (p.looper)          e->setAttribute ("looper", true);

        if (! p.setup.isEmpty())
            p.setup.toXml (*e->createNewChildElement ("SETUP"));

        if (p.note.isNotEmpty())
            e->createNewChildElement ("NOTE")->addTextElement (p.note);

        return e;
    }

    std::optional<PracticeEntry> entryFromXml (const juce::XmlElement& e, const juce::String& device)
    {
        PracticeEntry p;
        p.id = e.getStringAttribute ("id");
        p.device = device;
        p.topicId = e.getStringAttribute ("topic");
        p.topicTitle = e.getStringAttribute ("topicTitle");
        p.songName = e.getStringAttribute ("songName");
        p.start = juce::Time::fromISO8601 (e.getStringAttribute ("start"));
        p.end = juce::Time::fromISO8601 (e.getStringAttribute ("end"));
        p.playedSeconds = juce::jmax (0.0, e.getDoubleAttribute ("played"));
        p.maxBpm = juce::jmax (0.0, e.getDoubleAttribute ("maxBpm"));
        p.maxSpeed = juce::jmax (0.0, e.getDoubleAttribute ("maxSpeed"));
        p.looper = e.getBoolAttribute ("looper");

        if (auto* s = e.getChildByName ("SETUP"))
            p.setup = PracticeSetup::fromXml (*s);

        if (auto* n = e.getChildByName ("NOTE"))
            p.note = n->getAllSubText();

        if (p.id.isEmpty() || p.start == juce::Time())
            return std::nullopt;

        if (p.end < p.start)
            p.end = p.start;

        return p;
    }

    juce::Time startOfDay (juce::Time t)
    {
        return juce::Time (t.getYear(), t.getMonth(), t.getDayOfMonth(), 0, 0, 0, 0, true);
    }
}

PracticeLog::PracticeLog (const juce::File& f, const juce::String& deviceId)
    : folder (f), device (deviceId)
{
    reload();
}

juce::File PracticeLog::fileFor (int year) const
{
    return folder.getChildFile ("log-" + device + "-" + juce::String (year) + ".xml");
}

void PracticeLog::reload()
{
    entries.clear();

    if (! folder.isDirectory())
        return;

    for (auto& file : folder.findChildFiles (juce::File::findFiles, false, "log-*.xml"))
    {
        auto xml = juce::parseXMLIfTagMatches (file, "PRACTICELOG");

        if (xml == nullptr)
            continue;

        const auto fileDevice = xml->getStringAttribute ("device");

        for (auto* e : xml->getChildWithTagNameIterator ("ENTRY"))
            if (auto entry = entryFromXml (*e, fileDevice))
                if (std::none_of (entries.begin(), entries.end(), [&] (const PracticeEntry& x) { return x.id == entry->id; }))
                    entries.push_back (std::move (*entry));
    }

    std::stable_sort (entries.begin(), entries.end(), [] (const PracticeEntry& a, const PracticeEntry& b) { return a.start < b.start; });
}

juce::String PracticeLog::computeSignature() const
{
    juce::String sig;

    if (folder.isDirectory())
        for (auto& file : folder.findChildFiles (juce::File::findFiles, false, "log-*.xml"))
            sig << file.getFileName() << '|' << file.getSize() << '|' << file.getLastModificationTime().toMilliseconds() << '\n';

    return juce::String::toHexString (sig.hashCode64());
}

bool PracticeLog::save (const PracticeEntry& entry)
{
    auto copy = entry;
    copy.device = device;

    auto it = std::find_if (entries.begin(), entries.end(), [&] (const PracticeEntry& x) { return x.id == copy.id; });
    int oldYear = 0;

    if (it != entries.end())
    {
        oldYear = it->start.getYear();
        *it = copy;
    }
    else
    {
        entries.push_back (copy);
    }

    std::stable_sort (entries.begin(), entries.end(), [] (const PracticeEntry& a, const PracticeEntry& b) { return a.start < b.start; });

    auto ok = writeYear (copy.start.getYear());

    if (oldYear != 0 && oldYear != copy.start.getYear())
        ok = writeYear (oldYear) && ok;

    return ok;
}

bool PracticeLog::writeYear (int year)
{
    juce::XmlElement xml ("PRACTICELOG");
    xml.setAttribute ("device", device);

    for (auto& e : entries)
        if (e.device == device && e.start.getYear() == year)
            xml.addChildElement (entryToXml (e).release());

    if (! folder.createDirectory())
        return false;

    // writeTo goes through a temporary file: a cloud client never uploads a half-written log.
    return xml.writeTo (fileFor (year));
}

const PracticeEntry* PracticeLog::latestFor (const juce::String& topicId) const
{
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        if (it->topicId == topicId)
            return &*it;

    return nullptr;
}

const PracticeEntry* PracticeLog::latestSetupFor (const juce::String& topicId) const
{
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        if (it->topicId == topicId && ! it->setup.isEmpty())
            return &*it;

    return nullptr;
}

PracticeLog::TopicSummary PracticeLog::summarise (const juce::String& topicId) const
{
    TopicSummary s;

    for (auto& e : entries)
    {
        if (e.topicId != topicId)
            continue;

        ++s.sessions;
        s.totalSeconds += e.playedSeconds;
        s.last = juce::jmax (s.last, e.end);

        if (e.maxBpm > 0.0)    s.lastBpm = e.maxBpm;      // entries are oldest first: the last one wins
        if (e.maxSpeed > 0.0)  s.lastSpeed = e.maxSpeed;
    }

    return s;
}

double PracticeLog::playedBetween (juce::Time from, juce::Time to) const
{
    double total = 0.0;

    for (auto& e : entries)
        if (e.start >= from && e.start < to)
            total += e.playedSeconds;

    return total;
}

std::vector<const PracticeEntry*> PracticeLog::historyFor (const juce::String& topicId) const
{
    std::vector<const PracticeEntry*> result;

    for (auto& e : entries)
        if (e.topicId == topicId)
            result.push_back (&e);

    return result;
}

juce::Time PracticeLog::startOfWeek (juce::Time t)
{
    const auto daysSinceMonday = (t.getDayOfWeek() + 6) % 7;   // getDayOfWeek: 0 = Sunday
    const auto day = startOfDay (t);
    // Step back by calendar days (not 24 h blocks) so a daylight-saving change can't shift midnight.
    return juce::Time (day.getYear(), day.getMonth(), day.getDayOfMonth() - daysSinceMonday, 0, 0, 0, 0, true);
}

std::vector<double> PracticeLog::weeklySeconds (const juce::String& topicId, juce::Time now, int weeks) const
{
    std::vector<double> result ((size_t) juce::jmax (0, weeks), 0.0);

    if (weeks <= 0)
        return result;

    const auto thisWeek = startOfWeek (now);
    std::vector<juce::Time> starts ((size_t) weeks);

    for (int i = 0; i < weeks; ++i)
        starts[(size_t) i] = juce::Time (thisWeek.getYear(), thisWeek.getMonth(), thisWeek.getDayOfMonth() - 7 * (weeks - 1 - i), 0, 0, 0, 0, true);

    for (auto& e : entries)
    {
        if (e.topicId != topicId || e.start < starts.front())
            continue;

        for (int i = weeks - 1; i >= 0; --i)
        {
            if (e.start >= starts[(size_t) i])
            {
                result[(size_t) i] += e.playedSeconds;
                break;
            }
        }
    }

    return result;
}

juce::String PracticeLog::getThisDeviceId()
{
    const auto file = getAppDataFolder().getChildFile ("device.xml");

    if (auto xml = juce::parseXMLIfTagMatches (file, "DEVICE"))
    {
        const auto id = xml->getStringAttribute ("id");

        if (id.length() >= 8 && id.containsOnly ("0123456789abcdef"))
            return id;
    }

    const auto id = juce::Uuid().toString().substring (0, 12);
    juce::XmlElement xml ("DEVICE");
    xml.setAttribute ("id", id);
    xml.writeTo (file);
    return id;
}

//==============================================================================
void PlayDetector::reset()
{
    history.clear();
    historySeconds = 0.0;
    sinceLastNote = 1.0e9;
}

double PlayDetector::process (float peak, double seconds)
{
    if (seconds <= 0.0)
        return 0.0;

    // Noise floor: the quietest moment of the last half minute, capped (a loud "floor" means you never paused).
    auto floor = floorCap;

    for (auto& h : history)
        floor = juce::jmin (floor, h.first);

    const auto threshold = juce::jmax (absoluteThreshold, floor * aboveFloor);

    history.emplace_back (peak, seconds);
    historySeconds += seconds;

    while (historySeconds > floorWindowSeconds && history.size() > 1)
    {
        historySeconds -= history.front().second;
        history.pop_front();
    }

    if (peak > threshold)
        sinceLastNote = 0.0;
    else
        sinceLastNote += seconds;

    return sinceLastNote <= graceSeconds ? seconds : 0.0;
}

//==============================================================================
void PracticeTracker::setLog (PracticeLog* newLog)
{
    if (newLog == log)
        return;

    finish();
    log = newLog;
}

void PracticeTracker::setActiveTopic (const juce::String& newId, const juce::String& title)
{
    if (newId == topicId)
    {
        topicTitle = title;
        return;
    }

    finish();
    lastFinished.reset();
    topicId = newId;
    topicTitle = title;
    activeSeconds = 0.0;
    pendingNote = {};
}

void PracticeTracker::open (juce::Time now)
{
    PracticeEntry e;
    e.id = juce::Uuid().toDashedString();
    e.topicId = topicId;
    e.topicTitle = topicTitle;
    e.start = e.end = now;
    e.note = pendingNote;
    pendingNote = {};
    current = std::move (e);
    saved = false;
    unsavedSeconds = 0.0;

    if (noteDirty)
        saveNote();   // an edit to the previous entry's note

    lastFinished.reset();
}

void PracticeTracker::tick (double seconds, float inputPeak, const Activity& activity, juce::Time now)
{
    seconds = juce::jlimit (0.0, 5.0, seconds);   // the app was suspended: don't count the gap
    const auto played = detector.process (inputPeak, seconds);

    sinceFinished += seconds;
    sinceNoteEdit += seconds;

    if (noteDirty && sinceNoteEdit > 1.5)
        saveNote();

    if (played <= 0.0)
    {
        silentSeconds += seconds;

        if (current && silentSeconds > idleGapSeconds)
            finish();

        return;
    }

    silentSeconds = 0.0;

    if (! current)
        open (now);

    auto& e = *current;
    e.playedSeconds += played;
    e.end = now;
    e.topicTitle = topicTitle;
    activeSeconds += played;
    unsavedSeconds += played;

    if (activity.metronomeRunning && activity.bpm > 0.0)
        e.maxBpm = juce::jmax (e.maxBpm, activity.bpm);

    if (activity.songPlaying && activity.speed > 0.0)
        e.maxSpeed = juce::jmax (e.maxSpeed, activity.speed);

    if (activity.songName.isNotEmpty())
        e.songName = activity.songName;

    e.looper = e.looper || activity.looperActive;

    if (captureSetup != nullptr)
        e.setup = captureSetup();

    if (unsavedSeconds >= saveEverySeconds && e.playedSeconds >= minimumSeconds)
        flush();
}

void PracticeTracker::flush()
{
    if (! current || log == nullptr || current->playedSeconds < minimumSeconds)
        return;

    unsavedSeconds = 0.0;

    if (log->save (*current))
        saved = true;
    else
        juce::Logger::writeToLog ("Practice log: could not write to " + log->getFolder().getFullPathName());

    if (onSaved)
        onSaved();
}

void PracticeTracker::finish()
{
    if (noteDirty && ! current)
        saveNote();

    noteDirty = false;
    flush();

    if (current && saved)
    {
        lastFinished = current;
        sinceFinished = 0.0;
    }

    current.reset();
    saved = false;
    unsavedSeconds = 0.0;
}

void PracticeTracker::setNote (const juce::String& note)
{
    sinceNoteEdit = 0.0;

    if (current)
    {
        current->note = note;
        noteDirty = saved;   // not in the log yet: it goes in with the entry
    }
    else if (lastFinished && sinceFinished < noteSeconds)
    {
        lastFinished->note = note;
        noteDirty = true;
    }
    else
    {
        pendingNote = note;
    }
}

juce::String PracticeTracker::getNote() const
{
    if (current)
        return current->note;

    if (lastFinished && sinceFinished < noteSeconds)
        return lastFinished->note;

    return pendingNote;
}

void PracticeTracker::saveNote()
{
    noteDirty = false;

    if (current)
    {
        if (saved)
            flush();

        return;
    }

    if (lastFinished && log != nullptr)
    {
        log->save (*lastFinished);

        if (onSaved)
            onSaved();
    }
}

//==============================================================================
juce::String formatPracticeDuration (double seconds)
{
    const auto minutes = juce::roundToInt (seconds / 60.0);

    if (seconds < 1.0)
        return "0 min";

    if (seconds < 30.0)
        return "< 1 min";

    if (minutes < 60)
        return juce::String (minutes) + " min";

    const auto m = minutes % 60;
    return juce::String (minutes / 60) + " h " + (m < 10 ? "0" : "") + juce::String (m) + " min";
}

juce::String formatPracticeAgo (juce::Time when, juce::Time now)
{
    if (when == juce::Time())
        return "never";

    const auto days = juce::roundToInt ((startOfDay (now) - startOfDay (when)).inDays());

    if (days <= 0)   return "today";
    if (days == 1)   return "yesterday";
    if (days < 14)   return juce::String (days) + " days ago";
    if (days < 60)   return juce::String (days / 7) + " weeks ago";

    return juce::String (days / 30) + " months ago";
}

} // namespace pv
