#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

namespace pv
{

/** The MIDI message a footswitch sends: a controller, a note or a program change on one channel. */
struct MidiTrigger
{
    enum class Type { none, controller, note, program };
    Type type = Type::none;
    int channel = 0;
    int number = 0;

    /** Any CC value, note on/off or program change; none for other messages. */
    static MidiTrigger fromMessage (const juce::MidiMessage&);

    bool isValid() const noexcept                   { return type != Type::none; }
    bool matches (const juce::MidiMessage& m) const { return isValid() && fromMessage (m) == *this; }
    bool operator== (const MidiTrigger& o) const noexcept { return type == o.type && channel == o.channel && number == o.number; }
    bool operator!= (const MidiTrigger& o) const noexcept { return ! (*this == o); }

    juce::String describe() const;      // "CC 20 (ch 1)"
    juce::String toString() const;      // "cc:1:20" (saved in the settings)
    static MidiTrigger fromString (const juce::String&);
};

/** Something a footswitch can do. */
struct FootswitchAction
{
    enum class Kind { none, trackPress, trackClear, trackMute, undo, muteAll, clearAll, metronome, bypass };
    Kind kind = Kind::none;
    int track = 0;

    bool operator== (const FootswitchAction& o) const noexcept
    {
        return kind == o.kind && (track == o.track || ! usesTrack());
    }

    bool usesTrack() const noexcept { return kind == Kind::trackPress || kind == Kind::trackClear || kind == Kind::trackMute; }

    juce::String describe() const;      // for menus and the log
    juce::String toString() const;      // "press:0" (saved in the settings)
    static FootswitchAction fromString (const juce::String&);

    /** Every action, in menu order. */
    static std::vector<FootswitchAction> all (int numTracks);
};

/** Turns MIDI from a pedal board into actions: a tap and, for momentary switches, a hold.

    A switch is learned by pressing it. Momentary switches (CC 127 on press / 0 on release,
    or notes) fire the tap action at once on press - so recording starts exactly on the stomp -
    and the hold action as well when held for holdSeconds. Latching CC switches (127, 0, 127...
    one value per stomp) and program changes only have a tap: every message is a stomp.
    Whether a CC switch is momentary is detected while learning (a release follows the press).

    Message thread only.
*/
class Footswitch final : private juce::Timer
{
public:
    static constexpr int numSwitches = 4;
    static constexpr double holdSeconds = 1.0;
    static constexpr double learnReleaseWindow = 3.0;   // a release within this time = momentary switch

    struct Switch
    {
        MidiTrigger trigger;
        bool momentary = true;
        FootswitchAction tap, hold;
    };

    Footswitch();
    ~Footswitch() override;

    /** Track N on switch N: tap = record / overdub / play, hold = clear. Learned MIDI is kept. */
    void setDefaultActions();

    const Switch& getSwitch (int index) const       { return switches[(size_t) index]; }
    void setTapAction (int index, FootswitchAction);
    void setHoldAction (int index, FootswitchAction);
    void forget (int index);

    static juce::String switchName (int index)      { return juce::String::charToString ((juce::juce_wchar) ('A' + index)); }

    /** Returns true if the message belonged to a switch (or was taken by learning). */
    bool handleMidi (const juce::MidiMessage&);

    /** Same, with an explicit time in seconds (tests). */
    bool handleMidi (const juce::MidiMessage&, double nowSeconds);

    /** Fires hold actions that are due. Called by an internal timer; public for tests. */
    void update (double nowSeconds);

    // Learning: the next switch pressed is assigned to `index`.
    void startLearning (int index);
    void cancelLearning();
    int getLearningIndex() const noexcept           { return learning; }
    int getPendingIndex() const noexcept            { return pending; }     // just learned, waiting to see a release (-1 = none)

    /** Seconds since the switch last did something (UI lamp); large when never. */
    double secondsSinceActivity (int index) const;
    bool isDown (int index) const noexcept          { return runtime[(size_t) index].down; }

    /** `pressSeconds`: when the switch was pressed (Time::getMillisecondCounterHiRes() / 1000),
        or 0 for actions without a precise moment (holds). */
    std::function<void (FootswitchAction, double pressSeconds)> perform;
    std::function<void()> onChange;                 // learned or settings changed: save and refresh the UI

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement&);

private:
    struct Runtime
    {
        bool down = false;
        bool holdFired = false;
        double downTime = 0.0;
        double activity = -1.0e9;
    };

    void timerCallback() override;
    static double timeNow();
    void fire (int index, const FootswitchAction&, const char* gesture, double pressSeconds);
    void finishPending (bool momentary);
    void changed();

    std::array<Switch, numSwitches> switches;
    std::array<Runtime, numSwitches> runtime;
    double lastNow = 0.0;

    int learning = -1;
    int pending = -1;           // switch learned, waiting for a release (momentary) or the window to end
    double pendingSince = 0.0;
};

} // namespace pv
