#include "Footswitch.h"

namespace pv
{

//==============================================================================
MidiTrigger MidiTrigger::fromMessage (const juce::MidiMessage& m)
{
    MidiTrigger t;

    if (m.isController())
        t = { Type::controller, m.getChannel(), m.getControllerNumber() };
    else if (m.isNoteOnOrOff())
        t = { Type::note, m.getChannel(), m.getNoteNumber() };
    else if (m.isProgramChange())
        t = { Type::program, m.getChannel(), m.getProgramChangeNumber() };

    return t;
}

juce::String MidiTrigger::describe() const
{
    const auto ch = " (ch " + juce::String (channel) + ")";

    switch (type)
    {
        case Type::controller: return "CC " + juce::String (number) + ch;
        case Type::note:       return "Note " + juce::MidiMessage::getMidiNoteName (number, true, true, 3) + ch;
        case Type::program:    return "Program " + juce::String (number) + ch;
        case Type::none:       break;
    }

    return "not learned";
}

juce::String MidiTrigger::toString() const
{
    switch (type)
    {
        case Type::controller: return "cc:" + juce::String (channel) + ":" + juce::String (number);
        case Type::note:       return "note:" + juce::String (channel) + ":" + juce::String (number);
        case Type::program:    return "pc:" + juce::String (channel) + ":" + juce::String (number);
        case Type::none:       break;
    }

    return {};
}

MidiTrigger MidiTrigger::fromString (const juce::String& text)
{
    const auto parts = juce::StringArray::fromTokens (text, ":", {});

    if (parts.size() != 3)
        return {};

    MidiTrigger t;
    t.type = parts[0] == "cc" ? Type::controller : parts[0] == "note" ? Type::note : parts[0] == "pc" ? Type::program : Type::none;
    t.channel = juce::jlimit (1, 16, parts[1].getIntValue());
    t.number = juce::jlimit (0, 127, parts[2].getIntValue());
    return t;
}

//==============================================================================
juce::String FootswitchAction::describe() const
{
    const auto t = "Track " + juce::String (track + 1);

    switch (kind)
    {
        case Kind::trackPress: return t + ": record / overdub / play";
        case Kind::trackClear: return t + ": clear";
        case Kind::trackMute:  return t + ": mute / unmute";
        case Kind::undo:       return "Undo last overdub";
        case Kind::muteAll:    return "Mute / unmute all tracks";
        case Kind::clearAll:   return "Clear all tracks";
        case Kind::metronome:  return "Start / stop metronome";
        case Kind::bypass:     return "Guitar rig: bypass selected plugin";
        case Kind::tuner:      return "Tuner on / off";
        case Kind::none:       break;
    }

    return "Nothing";
}

juce::String FootswitchAction::toString() const
{
    switch (kind)
    {
        case Kind::trackPress: return "press:" + juce::String (track);
        case Kind::trackClear: return "clear:" + juce::String (track);
        case Kind::trackMute:  return "mute:" + juce::String (track);
        case Kind::undo:       return "undo";
        case Kind::muteAll:    return "muteAll";
        case Kind::clearAll:   return "clearAll";
        case Kind::metronome:  return "metronome";
        case Kind::bypass:     return "bypass";
        case Kind::tuner:      return "tuner";
        case Kind::none:       break;
    }

    return "none";
}

FootswitchAction FootswitchAction::fromString (const juce::String& text)
{
    const auto name = text.upToFirstOccurrenceOf (":", false, false);
    const auto track = juce::jlimit (0, 15, text.fromFirstOccurrenceOf (":", false, false).getIntValue());

    if (name == "press")     return { Kind::trackPress, track };
    if (name == "clear")     return { Kind::trackClear, track };
    if (name == "mute")      return { Kind::trackMute, track };
    if (name == "undo")      return { Kind::undo, 0 };
    if (name == "muteAll")   return { Kind::muteAll, 0 };
    if (name == "clearAll")  return { Kind::clearAll, 0 };
    if (name == "metronome") return { Kind::metronome, 0 };
    if (name == "bypass")    return { Kind::bypass, 0 };
    if (name == "tuner")     return { Kind::tuner, 0 };
    return {};
}

std::vector<FootswitchAction> FootswitchAction::all (int numTracks)
{
    std::vector<FootswitchAction> list { { Kind::none, 0 } };

    for (auto kind : { Kind::trackPress, Kind::trackClear, Kind::trackMute })
        for (int t = 0; t < numTracks; ++t)
            list.push_back ({ kind, t });

    for (auto kind : { Kind::undo, Kind::muteAll, Kind::clearAll, Kind::metronome, Kind::bypass, Kind::tuner })
        list.push_back ({ kind, 0 });

    return list;
}

//==============================================================================
Footswitch::Footswitch()
{
    setDefaultActions();
}

Footswitch::~Footswitch()
{
    stopTimer();
}

void Footswitch::setDefaultActions()
{
    for (int i = 0; i < numSwitches; ++i)
    {
        switches[(size_t) i].tap  = { FootswitchAction::Kind::trackPress, i };
        switches[(size_t) i].hold = { FootswitchAction::Kind::trackClear, i };
    }

    changed();
}

void Footswitch::setTapAction (int index, FootswitchAction action)
{
    switches[(size_t) index].tap = action;
    changed();
}

void Footswitch::setHoldAction (int index, FootswitchAction action)
{
    switches[(size_t) index].hold = action;
    changed();
}

void Footswitch::forget (int index)
{
    if (pending == index)
        pending = -1;

    switches[(size_t) index].trigger = {};
    switches[(size_t) index].momentary = true;
    runtime[(size_t) index] = {};
    changed();
}

void Footswitch::changed()
{
    publish();

    if (onChange)
        onChange();
}

int Footswitch::encode (const MidiTrigger& t) noexcept
{
    return t.isValid() ? ((int) t.type << 16) | (t.channel << 8) | t.number : 0;
}

void Footswitch::publish()
{
    for (int i = 0; i < numSwitches; ++i)
        publishedTriggers[(size_t) i] = encode (switches[(size_t) i].trigger);

    publishedLearning = learning >= 0 || pending >= 0;
}

bool Footswitch::isFootswitchMessage (const juce::MidiMessage& message) const noexcept
{
    const auto code = encode (MidiTrigger::fromMessage (message));

    if (code == 0)
        return false;

    if (publishedLearning.load())
        return true;

    for (auto& published : publishedTriggers)
        if (published.load() == code)
            return true;

    return false;
}

//==============================================================================
double Footswitch::timeNow()
{
    return juce::Time::getMillisecondCounterHiRes() * 0.001;
}

bool Footswitch::handleMidi (const juce::MidiMessage& message)
{
    // MIDI input timestamps use the same clock; a message that waited in the queue is dated
    // from its arrival, so holds are measured from the real press.
    const auto now = timeNow();
    const auto stamp = message.getTimeStamp();
    return handleMidi (message, stamp > 0.0 && stamp <= now && now - stamp < 2.0 ? stamp : now);
}

bool Footswitch::handleMidi (const juce::MidiMessage& message, double now)
{
    lastNow = now;
    const auto trigger = MidiTrigger::fromMessage (message);

    if (! trigger.isValid())
        return false;

    const auto pressed = message.isController() ? message.getControllerValue() >= 64
                       : message.isNoteOnOrOff() ? message.isNoteOn()
                                                 : true;

    // Just learned a CC switch: a quick release means it is momentary.
    if (pending >= 0 && switches[(size_t) pending].trigger == trigger)
    {
        if (! pressed)
            finishPending (true);

        return true;
    }

    if (learning >= 0)
    {
        const auto index = learning;
        learning = -1;

        for (int i = 0; i < numSwitches; ++i)
            if (i != index && switches[(size_t) i].trigger == trigger)
                switches[(size_t) i].trigger = {};      // one MIDI message drives one switch

        auto& sw = switches[(size_t) index];
        sw.trigger = trigger;
        runtime[(size_t) index] = {};
        runtime[(size_t) index].activity = now;

        if (trigger.type == MidiTrigger::Type::controller && pressed)
        {
            sw.momentary = false;       // until a release shows up
            pending = index;
            pendingSince = now;
            startTimerHz (30);
        }
        else
        {
            // A CC that starts at 0 is a latching switch that was "on"; notes have note-offs.
            sw.momentary = trigger.type == MidiTrigger::Type::note;
            juce::Logger::writeToLog ("Footswitch " + switchName (index) + " learned: " + trigger.describe()
                                      + (sw.momentary ? " (momentary)" : " (one message per stomp)"));
        }

        changed();
        return true;
    }

    for (int i = 0; i < numSwitches; ++i)
    {
        auto& sw = switches[(size_t) i];

        if (sw.trigger != trigger)
            continue;

        auto& rt = runtime[(size_t) i];

        if (! sw.momentary)
        {
            rt.activity = now;
            fire (i, sw.tap, "tap", now);
        }
        else if (pressed)
        {
            rt.down = true;
            rt.holdFired = false;
            rt.downTime = now;
            rt.activity = now;
            fire (i, sw.tap, "tap", now);

            if (sw.hold.kind != FootswitchAction::Kind::none)
                startTimerHz (30);
        }
        else
        {
            rt.down = false;
        }

        return true;
    }

    return false;
}

void Footswitch::update (double now)
{
    lastNow = now;
    bool waiting = false;

    if (pending >= 0)
    {
        if (now - pendingSince >= learnReleaseWindow)
            finishPending (false);
        else
            waiting = true;
    }

    for (int i = 0; i < numSwitches; ++i)
    {
        auto& rt = runtime[(size_t) i];
        const auto& sw = switches[(size_t) i];

        if (! rt.down || rt.holdFired || ! sw.momentary || sw.hold.kind == FootswitchAction::Kind::none)
            continue;

        if (now - rt.downTime >= holdSeconds)
        {
            rt.holdFired = true;
            rt.activity = now;
            fire (i, sw.hold, "hold", 0.0);
        }
        else
        {
            waiting = true;
        }
    }

    if (! waiting)
        stopTimer();
}

void Footswitch::timerCallback()
{
    update (timeNow());
}

void Footswitch::finishPending (bool momentary)
{
    const auto index = pending;
    pending = -1;

    if (index < 0)
        return;

    auto& sw = switches[(size_t) index];
    sw.momentary = momentary;
    juce::Logger::writeToLog ("Footswitch " + switchName (index) + " learned: " + sw.trigger.describe()
                              + (momentary ? " (momentary: tap + hold)" : " (latching: every stomp is a tap)"));
    changed();
}

void Footswitch::fire (int index, const FootswitchAction& action, const char* gesture, double pressSeconds)
{
    if (action.kind == FootswitchAction::Kind::none)
        return;

    juce::Logger::writeToLog ("Footswitch " + switchName (index) + " " + gesture + ": " + action.describe());

    if (perform)
        perform (action, pressSeconds);
}

//==============================================================================
void Footswitch::startLearning (int index)
{
    if (pending >= 0)
        finishPending (false);

    learning = juce::isPositiveAndBelow (index, numSwitches) ? index : -1;
    changed();
}

void Footswitch::cancelLearning()
{
    if (learning >= 0)
    {
        learning = -1;
        changed();
    }
}

double Footswitch::secondsSinceActivity (int index) const
{
    return timeNow() - runtime[(size_t) index].activity;
}

std::unique_ptr<juce::XmlElement> Footswitch::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("PRACTICE_VIBES_FOOTSWITCH");

    for (int i = 0; i < numSwitches; ++i)
    {
        const auto& sw = switches[(size_t) i];
        auto* e = xml->createNewChildElement ("SWITCH");
        e->setAttribute ("index", i);
        e->setAttribute ("trigger", sw.trigger.toString());
        e->setAttribute ("momentary", sw.momentary);
        e->setAttribute ("tap", sw.tap.toString());
        e->setAttribute ("hold", sw.hold.toString());
    }

    return xml;
}

void Footswitch::fromXml (const juce::XmlElement& xml)
{
    for (auto* e : xml.getChildWithTagNameIterator ("SWITCH"))
    {
        const auto i = e->getIntAttribute ("index", -1);

        if (! juce::isPositiveAndBelow (i, numSwitches))
            continue;

        auto& sw = switches[(size_t) i];
        sw.trigger = MidiTrigger::fromString (e->getStringAttribute ("trigger"));
        sw.momentary = e->getBoolAttribute ("momentary", true);
        sw.tap = FootswitchAction::fromString (e->getStringAttribute ("tap", sw.tap.toString()));
        sw.hold = FootswitchAction::fromString (e->getStringAttribute ("hold", sw.hold.toString()));
    }

    publish();
}

} // namespace pv
