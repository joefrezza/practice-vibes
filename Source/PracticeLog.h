#pragma once

#include <juce_core/juce_core.h>
#include <deque>
#include <optional>
#include <vector>

namespace pv
{

/** What was on screen while practising: enough to put it back with one click next time. */
struct PracticeSetup
{
    juce::String songPath;              // relative to the library folder (portable between devices), or absolute
    juce::String youTubeId, youTubeTitle;
    double speed = 1.0, semitones = 0.0;
    double loopStart = 0.0, loopEnd = 0.0;
    bool loopOn = false;

    bool metronome = false;             // the metronome was running
    double bpm = 0.0;
    int beatsPerBar = 4, subdivision = 0;

    bool hasMedia() const noexcept      { return songPath.isNotEmpty() || youTubeId.isNotEmpty(); }
    bool isEmpty() const noexcept       { return ! hasMedia() && ! metronome; }

    void toXml (juce::XmlElement&) const;
    static PracticeSetup fromXml (const juce::XmlElement&);

    /** Paths inside the library are stored relative to it, so another device finds the same file. */
    static juce::String portablePath (const juce::File& file, const juce::File& libraryRoot);
    static juce::File resolvePath (const juce::String& path, const juce::File& libraryRoot);
};

/** One stretch of practice on one topic (or free practice: no topic). */
struct PracticeEntry
{
    juce::String id;                    // UUID
    juce::String device;                // which device wrote it (each device writes only its own files)
    juce::String topicId, topicTitle;   // empty topicId = free practice
    juce::String songName;
    juce::Time start, end;
    double playedSeconds = 0.0;         // time the guitar was actually heard
    double maxBpm = 0.0;                // fastest metronome tempo played along to (0 = metronome not used)
    double maxSpeed = 0.0;              // fastest song/video speed played along to (0 = no song)
    bool looper = false;
    juce::String note;
    PracticeSetup setup;

    bool isFree() const noexcept        { return topicId.isEmpty(); }
};

/** Everybody's practice history, kept in the library folder (".practice", ignored by the topic scan).

    Each device writes only its own files (one per year: log-<device>-<year>.xml) and reads everyone's, so a
    cloud drive never has two devices editing the same file.
*/
class PracticeLog
{
public:
    PracticeLog (const juce::File& folder, const juce::String& deviceId);

    const juce::File& getFolder() const noexcept                   { return folder; }
    const juce::String& getDeviceId() const noexcept               { return device; }

    /** Re-reads every device's files. */
    void reload();

    /** Names, sizes and dates of the log files: changes when any device writes. */
    juce::String computeSignature() const;

    /** Adds the entry, or replaces the one with the same id, and writes this device's file for its year. */
    bool save (const PracticeEntry&);

    /** Oldest first. */
    const std::vector<PracticeEntry>& getEntries() const noexcept   { return entries; }

    /** The most recent entry for the topic, or nullptr. */
    const PracticeEntry* latestFor (const juce::String& topicId) const;

    /** The most recent entry for the topic whose setup can be restored, or nullptr. */
    const PracticeEntry* latestSetupFor (const juce::String& topicId) const;

    struct TopicSummary
    {
        juce::Time last;                // zero = never practised
        double totalSeconds = 0.0;
        int sessions = 0;
        double lastBpm = 0.0, lastSpeed = 0.0;   // from the latest entries that used them
    };

    TopicSummary summarise (const juce::String& topicId) const;

    /** Played time of entries starting in [from, to). */
    double playedBetween (juce::Time from, juce::Time to) const;

    /** A stable id for this device (made once, kept in the app's data folder). */
    static juce::String getThisDeviceId();

private:
    juce::File fileFor (int year) const;
    bool writeYear (int year);

    juce::File folder;
    juce::String device;
    std::vector<PracticeEntry> entries;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PracticeLog)
};

/** Decides from the guitar input level whether you are playing.

    A level counts as a note when it's clearly above the input's own noise floor (the quietest level of the last
    half minute: hum and hiss stay out) and above an absolute minimum. Short pauses between phrases still count.
*/
class PlayDetector
{
public:
    /** `peak`: the input's peak level over the last `seconds`. Returns how many of those seconds count as played. */
    double process (float peak, double seconds);

    bool isPlaying() const noexcept     { return sinceLastNote <= graceSeconds; }
    void reset();

    static constexpr float absoluteThreshold = 0.0056f;    // -45 dBFS
    static constexpr float floorCap = 0.01f;               // -40 dBFS: a louder "floor" is playing, not noise
    static constexpr float aboveFloor = 3.16f;             // +10 dB
    static constexpr double graceSeconds = 5.0;
    static constexpr double floorWindowSeconds = 30.0;

private:
    std::deque<std::pair<float, double>> history;          // (peak, seconds) of the last floorWindowSeconds
    double historySeconds = 0.0;
    double sinceLastNote = 1.0e9;
};

/** Turns "what's going on in the app" into log entries: which topic is active, how long you actually played,
    how fast, and the setup to restore next time. Runs on the message thread from a timer.
*/
class PracticeTracker
{
public:
    PracticeTracker() = default;
    ~PracticeTracker()                                              { finish(); }

    /** Where entries go (nullptr: nothing is logged). Closes the current entry. */
    void setLog (PracticeLog*);
    PracticeLog* getLog() const noexcept                           { return log; }

    /** Empty id = free practice. Closes the current entry if the topic changes. */
    void setActiveTopic (const juce::String& topicId, const juce::String& title);
    const juce::String& getActiveTopicId() const noexcept           { return topicId; }
    const juce::String& getActiveTopicTitle() const noexcept        { return topicTitle; }

    struct Activity
    {
        bool songPlaying = false;       // a song or video is playing
        double speed = 1.0;
        bool metronomeRunning = false;
        double bpm = 0.0;
        bool looperActive = false;
        juce::String songName;
    };

    /** Called regularly: `seconds` since the last call, the guitar input's peak level in that time. */
    void tick (double seconds, float inputPeak, const Activity&, juce::Time now);

    /** What to store as the setup (called while you play). */
    std::function<PracticeSetup()> captureSetup;

    /** Called after an entry was written to the log. */
    std::function<void()> onSaved;

    /** Writes the current entry (keeps it open). */
    void flush();

    /** Writes and closes the current entry. */
    void finish();

    /** The open entry, or nullptr. */
    const PracticeEntry* getCurrent() const noexcept                { return current ? &*current : nullptr; }

    bool isPlaying() const noexcept                                 { return detector.isPlaying(); }

    /** Played time on the active topic since it became active (all its entries). */
    double getActiveSeconds() const noexcept                        { return activeSeconds; }

    /** Note for the current entry; for the one that just ended (within noteMinutes of its end, same topic);
        otherwise for the next one. Written to the log a moment after the last keystroke. */
    void setNote (const juce::String&);
    juce::String getNote() const;

    static constexpr double idleGapSeconds = 300.0;     // longer silence: the next note starts a new entry
    static constexpr double minimumSeconds = 20.0;      // shorter entries aren't kept
    static constexpr double saveEverySeconds = 60.0;
    static constexpr double noteSeconds = 15.0 * 60.0;  // a note this long after the end still belongs to that entry

private:
    void open (juce::Time now);
    void saveNote();

    PracticeLog* log = nullptr;
    juce::String topicId, topicTitle, pendingNote;
    std::optional<PracticeEntry> current;
    PlayDetector detector;
    double silentSeconds = 0.0, unsavedSeconds = 0.0, activeSeconds = 0.0;
    bool saved = false;                                  // the current entry is in the log already

    std::optional<PracticeEntry> lastFinished;           // the entry that just ended (still takes a note)
    double sinceFinished = 0.0, sinceNoteEdit = 0.0;
    bool noteDirty = false;
};

/** "12 min", "1 h 05 min", "< 1 min". */
juce::String formatPracticeDuration (double seconds);

/** "today", "yesterday", "5 days ago", "3 weeks ago", "never". */
juce::String formatPracticeAgo (juce::Time when, juce::Time now);

} // namespace pv
