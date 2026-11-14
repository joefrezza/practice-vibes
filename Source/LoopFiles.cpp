#include "LoopFiles.h"

namespace pv
{

namespace
{
    const juce::String sessionFileName = "session.xml";
    const juce::String rootTag = "PRACTICE_VIBES_LOOPS";

    juce::String trackFileName (int track)     { return "track " + juce::String (track + 1) + ".wav"; }

    juce::Result writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

        if (stream == nullptr)
            return juce::Result::fail ("Can't write " + file.getFullPathName());

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                                                    .withNumChannels (audio.getNumChannels())
                                                                                    .withBitsPerSample (32));     // float: overdubs may exceed 0 dBFS

        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
            return juce::Result::fail ("Can't write " + file.getFileName());

        return juce::Result::ok();
    }

    juce::Result readWav (const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate)
    {
        juce::AudioFormatManager formats;
        formats.registerFormat (new juce::WavAudioFormat(), true);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            return juce::Result::fail ("Can't read " + file.getFileName());

        audio.setSize ((int) juce::jlimit (1u, 2u, reader->numChannels), (int) reader->lengthInSamples);
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, audio.getNumChannels() > 1);
        sampleRate = reader->sampleRate;
        return juce::Result::ok();
    }

    juce::int64 gcd (juce::int64 a, juce::int64 b)     { return b == 0 ? a : gcd (b, a % b); }
}

juce::File LoopFiles::getLoopsFolder()
{
   #if JUCE_IOS
    auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Loops");
   #else
    auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Practice Vibes").getChildFile ("Loops");
   #endif

    folder.createDirectory();
    return folder;
}

juce::Array<juce::File> LoopFiles::listSessions()
{
    juce::Array<juce::File> sessions;

    for (const auto& entry : juce::RangedDirectoryIterator (getLoopsFolder(), false, "*", juce::File::findDirectories))
        if (entry.getFile().getChildFile (sessionFileName).existsAsFile())
            sessions.add (entry.getFile());

    std::sort (sessions.begin(), sessions.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getChildFile (sessionFileName).getLastModificationTime() > b.getChildFile (sessionFileName).getLastModificationTime();
    });

    return sessions;
}

juce::File LoopFiles::createSessionFolder (const LoopSet& set, const juce::File& parent)
{
    const auto name = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M") + " - " + juce::String (juce::roundToInt (set.getBpm()))
                      + " bpm " + juce::String (set.beatsPerBar) + "-4";
    auto folder = parent.getNonexistentChildFile (name, {}, true);
    folder.createDirectory();
    return folder;
}

juce::AudioBuffer<float> LoopFiles::renderMix (const LoopSet& set)
{
    juce::int64 bars = 1;

    for (auto& loop : set.loops)
        if (loop.bars > 0 && ! loop.muted)
            bars = bars / gcd (bars, loop.bars) * loop.bars;

    const auto maxBars = juce::jmax (1, (int) std::floor (600.0 * set.sampleRate / juce::jmax (1.0, set.barSamples)));

    if (bars > maxBars)
    {
        bars = 1;

        for (auto& loop : set.loops)
            bars = juce::jmax (bars, (juce::int64) loop.bars);
    }

    juce::AudioBuffer<float> mix (2, juce::jmax (1, (int) std::round ((double) bars * set.barSamples)));
    mix.clear();

    for (auto& loop : set.loops)
    {
        const auto length = loop.audio.getNumSamples();

        if (loop.bars <= 0 || loop.muted || length == 0)
            continue;

        for (int c = 0; c < 2; ++c)
        {
            const auto* in = loop.audio.getReadPointer (juce::jmin (c, loop.audio.getNumChannels() - 1));
            auto* out = mix.getWritePointer (c);

            // The loop's own length is a whole number of bars: place each repeat on its bar line.
            for (juce::int64 start = 0; start < (juce::int64) bars; start += loop.bars)
            {
                const auto offset = (int) std::round ((double) start * set.barSamples);
                const auto count = juce::jmin (length, mix.getNumSamples() - offset);

                for (int i = 0; i < count; ++i)
                    out[offset + i] += in[i] * loop.gain;
            }
        }
    }

    return mix;
}

juce::Result LoopFiles::save (const LoopSet& set, const juce::File& folder)
{
    if (set.getNumLoops() == 0)
        return juce::Result::fail ("There are no loops to save.");

    if (! folder.createDirectory())
        return juce::Result::fail ("Can't create " + folder.getFullPathName());

    juce::XmlElement xml (rootTag);
    xml.setAttribute ("version", 1);
    xml.setAttribute ("sampleRate", set.sampleRate);
    xml.setAttribute ("barSamples", set.barSamples);
    xml.setAttribute ("beatsPerBar", set.beatsPerBar);
    xml.setAttribute ("bpm", set.getBpm());
    xml.setAttribute ("source", set.source == Looper::Source::beforeFx ? "beforeFx" : "afterFx");

    for (int t = 0; t < Looper::numTracks; ++t)
    {
        const auto& loop = set.loops[(size_t) t];

        if (loop.bars <= 0)
            continue;

        const auto result = writeWav (folder.getChildFile (trackFileName (t)), loop.audio, set.sampleRate);

        if (result.failed())
            return result;

        auto* track = xml.createNewChildElement ("TRACK");
        track->setAttribute ("index", t);
        track->setAttribute ("file", trackFileName (t));
        track->setAttribute ("bars", loop.bars);
        track->setAttribute ("muted", loop.muted);
        track->setAttribute ("gain", (double) loop.gain);
    }

    const auto mix = writeWav (folder.getChildFile ("mix.wav"), renderMix (set), set.sampleRate);

    if (mix.failed())
        return mix;

    // Written last: a folder without it is an incomplete save and isn't listed.
    if (! xml.writeTo (folder.getChildFile (sessionFileName)))
        return juce::Result::fail ("Can't write " + sessionFileName);

    return juce::Result::ok();
}

juce::Result LoopFiles::load (const juce::File& folder, LoopSet& set)
{
    const auto xml = juce::parseXML (folder.getChildFile (sessionFileName));

    if (xml == nullptr || ! xml->hasTagName (rootTag))
        return juce::Result::fail ("No looper session in " + folder.getFileName());

    set = {};
    set.sampleRate = xml->getDoubleAttribute ("sampleRate", 48000.0);
    set.barSamples = xml->getDoubleAttribute ("barSamples", 0.0);
    set.beatsPerBar = juce::jlimit (1, 12, xml->getIntAttribute ("beatsPerBar", 4));
    set.source = xml->getStringAttribute ("source") == "beforeFx" ? Looper::Source::beforeFx : Looper::Source::afterFx;

    if (set.sampleRate <= 0.0 || set.barSamples <= 0.0)
        return juce::Result::fail ("The session has no valid tempo.");

    for (auto* track : xml->getChildWithTagNameIterator ("TRACK"))
    {
        const auto index = track->getIntAttribute ("index", -1);

        if (! juce::isPositiveAndBelow (index, Looper::numTracks))
            continue;

        auto& loop = set.loops[(size_t) index];
        double fileRate = 0.0;
        const auto result = readWav (folder.getChildFile (track->getStringAttribute ("file", trackFileName (index))), loop.audio, fileRate);

        if (result.failed())
            return result;

        if (std::abs (fileRate - set.sampleRate) > 0.5)
            return juce::Result::fail (track->getStringAttribute ("file") + " has a different sample rate than the session.");

        loop.bars = juce::jmax (1, track->getIntAttribute ("bars", 1));
        loop.muted = track->getBoolAttribute ("muted", false);
        loop.gain = (float) juce::jlimit (0.0, 2.0, track->getDoubleAttribute ("gain", 1.0));
    }

    if (set.getNumLoops() == 0)
        return juce::Result::fail ("The session has no loops.");

    return juce::Result::ok();
}

juce::String LoopFiles::describe (const juce::File& folder)
{
    juce::String text = folder.getFileName();

    if (auto xml = juce::parseXML (folder.getChildFile (sessionFileName)))
    {
        const auto loops = xml->getNumChildElements();
        text << " (" << loops << (loops == 1 ? " loop)" : " loops)");
    }

    return text;
}

} // namespace pv
