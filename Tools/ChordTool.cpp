// Developer tool: runs the chord analysis on audio files and prints the result.
// Usage: ChordTool <file> [<file> ...]

#include "../Source/ChordAnalyzer.h"
#include <juce_audio_utils/juce_audio_utils.h>

#include <iostream>

namespace pv { extern bool chordAnalyzerDebug; extern float slashBassRatio; extern float meterMargin; extern float riffWeight; extern float meterSwitchCost; }

namespace
{
    juce::String formatTime (double seconds)
    {
        const auto minutes = (int) (seconds / 60.0);
        const auto rest = seconds - minutes * 60.0;
        return juce::String (minutes) + ":" + (rest < 10.0 ? "0" : "") + juce::String (rest, 1);
    }

    void analyseFile (juce::AudioFormatManager& formats, const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
        {
            std::cout << "Cannot read " << file.getFullPathName() << std::endl;
            return;
        }

        const auto length = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> audio ((int) juce::jmax (1u, reader->numChannels), length);
        reader->read (&audio, 0, length, 0, true, true);

        std::vector<float> mono ((size_t) length, 0.0f);

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < length; ++i)
                mono[(size_t) i] += audio.getSample (ch, i) / (float) audio.getNumChannels();

        const auto startTime = juce::Time::getMillisecondCounterHiRes();
        const auto track = pv::analyseChords (mono.data(), length, reader->sampleRate);
        const auto elapsed = juce::Time::getMillisecondCounterHiRes() - startTime;

        std::cout << "=== " << file.getFileName() << "  (" << formatTime (length / reader->sampleRate) << ", analysed in "
                  << juce::roundToInt (elapsed) << " ms)" << std::endl
                  << "tempo " << juce::String (track.bpm, 1) << " bpm, tuning " << juce::String (track.tuningCents, 1)
                  << " cents, key " << track.keyName() << ", " << track.timeSignature() << std::endl;

        if (pv::chordAnalyzerDebug)
        {
            juce::String positions;

            for (auto p : track.beatPositions)
                positions << p;

            std::cout << "positions " << positions << std::endl;
        }

        {
            auto problems = 0;
            for (size_t k = 0; k < track.chords.size(); ++k)
            {
                const auto& c = track.chords[k];
                const auto gap = k > 0 && track.chords[k - 1].startBeat + track.chords[k - 1].numBeats != c.startBeat;
                if (c.numBeats < 1 || c.end <= c.start || gap)
                {
                    ++problems;
                    std::cout << "BAD chord " << (int) k << " " << track.chordName (c) << " beats " << c.startBeat << "+" << c.numBeats
                              << " time " << c.start << "-" << c.end << std::endl;
                }
            }
            if (problems > 0)
                std::cout << problems << " inconsistent chord(s)" << std::endl;
        }

        // One line per bar: chords with their beat counts.
        juce::String line;
        auto bar = 0;

        for (const auto& chord : track.chords)
        {
            for (int b = chord.startBeat; b < chord.startBeat + chord.numBeats; ++b)
            {
                if (track.isDownbeat (b) || b == 0)
                {
                    if (line.isNotEmpty())
                        std::cout << line << std::endl;

                    ++bar;
                    line = juce::String (bar).paddedLeft (' ', 4) + "  " + formatTime (track.beats[(size_t) b]).paddedLeft (' ', 7)
                           + " " + track.timeSignatureAt (b).paddedRight (' ', 4) + "|";

                    for (int k = 0; k < track.beatPositions[(size_t) b]; ++k)
                        line += "      ";
                }

                line += (b == chord.startBeat ? " " + track.chordName (chord) : juce::String (" .")).paddedRight (' ', 8);
            }
        }

        if (line.isNotEmpty())
            std::cout << line << std::endl;

        std::cout << std::endl;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    if (argc < 2)
    {
        std::cout << "Usage: ChordTool <audio file> [...]" << std::endl;
        return 1;
    }

    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--debug")
            pv::chordAnalyzerDebug = true;
        else if (juce::String (argv[i]).startsWith ("--switch="))
            pv::meterSwitchCost = juce::String (argv[i]).fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        else if (juce::String (argv[i]).startsWith ("--riff="))
            pv::riffWeight = juce::String (argv[i]).fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        else if (juce::String (argv[i]).startsWith ("--meter="))
            pv::meterMargin = juce::String (argv[i]).fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        else if (juce::String (argv[i]).startsWith ("--slash="))
            pv::slashBassRatio = juce::String (argv[i]).fromFirstOccurrenceOf ("=", false, false).getFloatValue();

    for (int i = 1; i < argc; ++i)
        if (! juce::String (argv[i]).startsWith ("--"))
            analyseFile (formats, juce::File (juce::String::fromUTF8 (argv[i])));

    return 0;
}
