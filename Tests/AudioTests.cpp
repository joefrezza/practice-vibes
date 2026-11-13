// Offline checks of the song player and metronome DSP. Run: PracticeVibesTests(.exe)
// Exit code 0 = all passed.

#include "../Source/ChordAnalyzer.h"
#include "../Source/MetronomeProcessor.h"
#include "../Source/PlayerProcessor.h"

#include <iostream>

using namespace pv;

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& what)
    {
        std::cout << (condition ? "  PASS  " : "  FAIL  ") << what << std::endl;

        if (! condition)
            ++failures;
    }

    std::shared_ptr<LoadedSong> makeSine (double sampleRate, double frequency, double seconds)
    {
        auto song = std::make_shared<LoadedSong>();
        song->sampleRate = sampleRate;
        song->name = "sine";
        const auto length = (int) (sampleRate * seconds);
        song->audio.setSize (2, length);

        for (int i = 0; i < length; ++i)
        {
            const auto value = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / sampleRate);
            song->audio.setSample (0, i, value);
            song->audio.setSample (1, i, value);
        }

        return song;
    }

    /** Renders `seconds` of audio, returning only the final `measureSeconds`. */
    juce::AudioBuffer<float> render (juce::AudioProcessor& processor, double sampleRate, double seconds, double measureSeconds)
    {
        constexpr int blockSize = 512;
        const auto total = (int) (seconds * sampleRate);
        const auto keep = (int) (measureSeconds * sampleRate);
        juce::AudioBuffer<float> block (2, blockSize), kept (2, keep);
        juce::MidiBuffer midi;

        for (int done = 0; done < total; done += blockSize)
        {
            block.clear();
            processor.processBlock (block, midi);

            for (int i = 0; i < blockSize; ++i)
            {
                const auto index = done + i - (total - keep);

                if (index >= 0 && index < keep)
                    kept.setSample (0, index, block.getSample (0, i));
            }
        }

        return kept;
    }

    double estimateFrequency (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        int crossings = 0;
        const auto* data = buffer.getReadPointer (0);

        for (int i = 1; i < buffer.getNumSamples(); ++i)
            if (data[i - 1] <= 0.0f && data[i] > 0.0f)
                ++crossings;

        return crossings / (buffer.getNumSamples() / sampleRate);
    }

    int countClicks (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        int clicks = 0;
        int quietSamples = 1000000;
        const auto* data = buffer.getReadPointer (0);
        const auto minGap = (int) (0.05 * sampleRate);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            if (std::abs (data[i]) > 0.05f)
            {
                if (quietSamples > minGap)
                    ++clicks;

                quietSamples = 0;
            }
            else
            {
                ++quietSamples;
            }
        }

        return clicks;
    }

    void testPlayer()
    {
        std::cout << "Song player" << std::endl;
        constexpr double deviceRate = 48000.0;
        constexpr double songRate = 44100.0;   // deliberately different from the device

        {
            PlayerProcessor player;
            player.prepareToPlay (deviceRate, 512);
            player.setSong (makeSine (songRate, 440.0, 10.0));
            player.play();
            const auto out = render (player, deviceRate, 2.0, 1.0);
            const auto f = estimateFrequency (out, deviceRate);
            check (std::abs (f - 440.0) < 5.0, "pitch preserved across sample rates: " + juce::String (f, 1) + " Hz (want 440)");
            check (std::abs (player.getPositionSeconds() - 2.0) < 0.15, "position after 2 s at 1x: " + juce::String (player.getPositionSeconds(), 2) + " s");
        }

        {
            PlayerProcessor player;
            player.prepareToPlay (deviceRate, 512);
            player.setSong (makeSine (songRate, 440.0, 10.0));
            player.setSemitones (12.0);
            player.play();
            const auto f = estimateFrequency (render (player, deviceRate, 2.0, 1.0), deviceRate);
            check (std::abs (f - 880.0) < 10.0, "+12 semitones: " + juce::String (f, 1) + " Hz (want 880)");
        }

        {
            PlayerProcessor player;
            player.prepareToPlay (deviceRate, 512);
            player.setSong (makeSine (songRate, 440.0, 10.0));
            player.setSpeed (0.5);
            player.play();
            const auto out = render (player, deviceRate, 4.0, 1.0);
            const auto f = estimateFrequency (out, deviceRate);
            check (std::abs (f - 440.0) < 5.0, "half speed keeps pitch: " + juce::String (f, 1) + " Hz (want 440)");
            check (std::abs (player.getPositionSeconds() - 2.0) < 0.2, "half speed position after 4 s: " + juce::String (player.getPositionSeconds(), 2) + " s (want ~2)");
        }

        {
            PlayerProcessor player;
            player.prepareToPlay (deviceRate, 512);
            player.setSong (makeSine (songRate, 440.0, 10.0));
            player.setLoopSeconds (3.0, 4.0);
            player.setLoopEnabled (true);
            player.play();

            constexpr int blockSize = 512;
            juce::AudioBuffer<float> block (2, blockSize);
            juce::MidiBuffer midi;
            double minPos = 1.0e9, maxPos = -1.0;

            for (int done = 0; done < (int) (6.0 * deviceRate); done += blockSize)
            {
                player.processBlock (block, midi);

                if (done > (int) (0.5 * deviceRate))
                {
                    minPos = juce::jmin (minPos, player.getPositionSeconds());
                    maxPos = juce::jmax (maxPos, player.getPositionSeconds());
                }
            }

            check (minPos >= 2.9 && maxPos <= 4.1, "loop stays within 3-4 s: saw " + juce::String (minPos, 2) + " - " + juce::String (maxPos, 2));
            check (player.isPlaying(), "still playing while looping");
        }

        {
            PlayerProcessor player;
            player.prepareToPlay (deviceRate, 512);
            player.setSong (makeSine (songRate, 440.0, 1.0));
            player.play();
            render (player, deviceRate, 2.0, 0.1);
            check (! player.isPlaying(), "stops at the end of the song");
        }
    }

    void testFileLoading()
    {
        std::cout << "File loading" << std::endl;

        // Write a 2-second mono 44.1 kHz WAV to a temp file.
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("pv-test-song.wav");
        file.deleteFile();

        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0)
                                                                                       .withNumChannels (1)
                                                                                       .withBitsPerSample (16));
            check (writer != nullptr, "test WAV writer created");

            if (writer == nullptr)
                return;

            auto source = makeSine (44100.0, 220.0, 2.0);
            writer->writeFromAudioSampleBuffer (source->audio, 0, source->audio.getNumSamples());
        }

        PlayerProcessor player;
        juce::String result = "pending";
        player.loadAsync (juce::URL (file), [&result] (const juce::String& error) { result = error; });

        for (int i = 0; i < 100 && result == "pending"; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        check (result.isEmpty(), "WAV file loads without error" + (result.isEmpty() ? juce::String() : ": " + result));

        if (auto song = player.getSong())
        {
            check (std::abs (song->getLengthSeconds() - 2.0) < 0.01, "length is 2 s: " + juce::String (song->getLengthSeconds(), 3));
            check (song->audio.getNumChannels() == 2, "mono file is played as stereo");
            check (std::abs (song->audio.getSample (1, 1000) - song->audio.getSample (0, 1000)) < 1.0e-6, "both channels identical");

            check (! song->peakLevels.empty(), "waveform cache built: " + juce::String ((int) song->peakLevels.size()) + " level(s)");

            if (! song->peakLevels.empty())
            {
                const auto& finest = song->peakLevels.front();
                const auto maxPeak = *std::max_element (finest.maxs.begin(), finest.maxs.end());
                const auto minPeak = *std::min_element (finest.mins.begin(), finest.mins.end());
                check (maxPeak > 0.45f && maxPeak < 0.55f && minPeak < -0.45f && minPeak > -0.55f,
                       "waveform cache min/max: " + juce::String (minPeak, 2) + " / " + juce::String (maxPeak, 2));

                const auto& coarsest = song->peakLevels.back();
                check (std::abs (*std::max_element (coarsest.maxs.begin(), coarsest.maxs.end()) - maxPeak) < 1.0e-6f,
                       "coarse levels keep the same peak");
            }
        }
        else
        {
            check (false, "song available after loading");
        }

        file.deleteFile();
    }

    void testMetronome()
    {
        std::cout << "Metronome" << std::endl;
        constexpr double rate = 48000.0;

        {
            MetronomeProcessor metronome;
            metronome.prepareToPlay (rate, 512);
            metronome.startBpm = 120.0;
            metronome.start();
            const auto clicks = countClicks (render (metronome, rate, 4.0, 4.0), rate);
            check (clicks == 8, "120 bpm for 4 s = 8 clicks: got " + juce::String (clicks));
        }

        for (auto [sub, expected, name] : { std::tuple { MetronomeProcessor::Subdivision::eighths, 16, "8ths" },
                                            std::tuple { MetronomeProcessor::Subdivision::triplets, 24, "triplets" },
                                            std::tuple { MetronomeProcessor::Subdivision::sixteenths, 32, "16ths" } })
        {
            MetronomeProcessor metronome;
            metronome.prepareToPlay (rate, 512);
            metronome.startBpm = 120.0;
            metronome.subdivision = (int) sub;
            metronome.subdivisionLevel = 0.5f;
            metronome.start();
            const auto clicks = countClicks (render (metronome, rate, 4.0, 4.0), rate);
            check (clicks == expected, juce::String ("120 bpm ") + name + " for 4 s = " + juce::String (expected) + " clicks: got " + juce::String (clicks));
        }

        {
            // Downbeat > beat > subdivision
            MetronomeProcessor metronome;
            metronome.prepareToPlay (rate, 512);
            metronome.startBpm = 120.0;
            metronome.subdivision = (int) MetronomeProcessor::Subdivision::eighths;
            metronome.subdivisionLevel = 0.5f;
            metronome.start();
            const auto out = render (metronome, rate, 1.0, 1.0);
            auto peakAround = [&out, rate] (double seconds)
            {
                const auto start = (int) (seconds * rate);
                return out.getMagnitude (0, start, (int) (0.02 * rate));
            };
            const auto downbeat = peakAround (0.0), beat = peakAround (0.5), sub = peakAround (0.25);
            check (downbeat > beat && beat > sub && sub > 0.01f,
                   "levels downbeat > beat > subdivision: " + juce::String (downbeat, 2) + " / " + juce::String (beat, 2) + " / " + juce::String (sub, 2));
        }

        for (auto [snd, name] : { std::pair { MetronomeProcessor::Sound::click, "click" }, std::pair { MetronomeProcessor::Sound::drums, "drums" } })
        {
            MetronomeProcessor metronome;
            metronome.prepareToPlay (rate, 512);
            metronome.sound = (int) snd;
            metronome.subdivision = (int) MetronomeProcessor::Subdivision::sixteenths;
            metronome.start();
            const auto out = render (metronome, rate, 2.0, 2.0);
            const auto peak = out.getMagnitude (0, 0, out.getNumSamples());
            bool finite = true;

            for (int i = 0; i < out.getNumSamples(); ++i)
                finite = finite && std::isfinite (out.getSample (0, i));

            check (finite && peak > 0.1f && peak <= 1.0f, juce::String ("sound '") + name + "' renders, peak " + juce::String (peak, 2));
        }

        {
            MetronomeProcessor metronome;
            metronome.prepareToPlay (rate, 512);
            metronome.startBpm = 120.0;
            metronome.beatsPerBar = 4;
            metronome.subdivision = (int) MetronomeProcessor::Subdivision::triplets;   // trainer must still count bars
            metronome.trainerEnabled = true;
            metronome.trainerStepBpm = 10.0;
            metronome.trainerInterval = 1.0;
            metronome.trainerUnit = (int) MetronomeProcessor::IntervalUnit::bars;
            metronome.trainerMaxBpm = 140.0;
            metronome.start();

            render (metronome, rate, 1.9, 0.1);   // inside bar 1
            check (std::abs (metronome.getCurrentBpm() - 120.0) < 0.01, "first bar stays at start tempo: " + juce::String (metronome.getCurrentBpm()));

            render (metronome, rate, 0.3, 0.1);   // just past the second downbeat (2.0 s)
            check (std::abs (metronome.getCurrentBpm() - 130.0) < 0.01, "steps +10 after one bar: " + juce::String (metronome.getCurrentBpm()));

            render (metronome, rate, 10.0, 0.1);
            check (std::abs (metronome.getCurrentBpm() - 140.0) < 0.01, "capped at max 140: " + juce::String (metronome.getCurrentBpm()));
        }
    }

    struct TestChord { int root; bool minor; int bass; };   // bass: pitch class, -1 = root

    /** A synthetic band at 120 bpm: kick on every beat, bass + harmonic-rich triads, one chord per bar. */
    std::vector<float> synthesiseBand (const std::vector<TestChord>& progression, int bars, double sampleRate)
    {
        const auto beatSeconds = 0.5;
        const auto length = (int) (sampleRate * beatSeconds * 4 * bars);
        std::vector<float> audio ((size_t) length, 0.0f);
        auto midiToHz = [] (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); };
        juce::Random random (42);

        for (int i = 0; i < length; ++i)
        {
            const auto t = i / sampleRate;
            const auto beat = (int) (t / beatSeconds);
            const auto sinceBeat = t - beat * beatSeconds;
            const auto& chord = progression[(size_t) ((beat / 4) % (int) progression.size())];
            const auto triadBase = 60 + (chord.root > 4 ? chord.root - 12 : chord.root);
            const int notes[] = { triadBase, triadBase + (chord.minor ? 3 : 4), triadBase + 7 };
            auto sample = 0.0;

            for (auto note : notes)
                for (int h = 1; h <= 5; ++h)
                    sample += 0.08 / h * std::sin (juce::MathConstants<double>::twoPi * midiToHz (note) * h * t);

            const auto bassNote = 36 + (chord.bass >= 0 ? chord.bass : chord.root);
            sample += 0.2 * std::sin (juce::MathConstants<double>::twoPi * midiToHz (bassNote) * t) * std::exp (-sinceBeat * 3.0);
            sample += 0.5 * std::exp (-sinceBeat * 40.0) * (std::sin (juce::MathConstants<double>::twoPi * 60.0 * sinceBeat)
                                                             + 0.3 * (random.nextFloat() * 2.0f - 1.0f));
            audio[(size_t) i] = (float) (0.5 * sample);
        }

        return audio;
    }

    void testChordAnalysis()
    {
        std::cout << "Chord analysis" << std::endl;

        constexpr double sampleRate = 44100.0, bpm = 120.0;
        constexpr int bars = 16;
        const auto beatSeconds = 60.0 / bpm;
        const std::vector<TestChord> progression { { 0, false, -1 }, { 9, true, -1 }, { 5, false, -1 }, { 7, false, -1 } };
        const auto audio = synthesiseBand (progression, bars, sampleRate);
        const auto length = (int) audio.size();
        const auto track = analyseChords (audio.data(), length, sampleRate);

        const auto tempoOk = std::abs (track.bpm - bpm) < 3.0 || std::abs (track.bpm - bpm / 2) < 1.5 || std::abs (track.bpm - bpm * 2) < 6.0;
        check (tempoOk, "tempo detected: " + juce::String (track.bpm, 1) + " bpm (want 120, or half/double)");
        check (std::abs (track.tuningCents) < 10.0, "tuning near 0 cents: " + juce::String (track.tuningCents, 1));

        auto correct = 0, total = 0;

        for (int beat = 4; beat < bars * 4 - 1; ++beat)   // skip the first bar while things settle
        {
            const auto t = (beat + 0.5) * beatSeconds;
            const auto& truth = progression[(size_t) ((beat / 4) % 4)];
            const auto index = track.chordIndexAt (t);
            ++total;

            if (index >= 0)
            {
                const auto& c = track.chords[(size_t) index];
                const auto minor = c.quality == ChordTrack::minor || c.quality == ChordTrack::minor7;
                correct += (c.root == truth.root && minor == truth.minor) ? 1 : 0;
            }
        }

        check (correct >= total * 9 / 10, "chords correct on " + juce::String (correct) + " of " + juce::String (total) + " beats");

        auto slashes = 0;

        for (const auto& c : track.chords)
            slashes += c.bass >= 0 ? 1 : 0;

        check (slashes == 0, "no slash chords when the bass plays roots: " + juce::String (slashes));

        juce::StringArray names;

        for (const auto& c : track.chords)
            names.add (track.chordName (c));

        std::cout << "        detected: " << names.joinIntoString (" ").substring (0, 120) << std::endl;

        auto downbeatsOk = 0, downbeatsSeen = 0;

        for (size_t i = 0; i < track.beats.size(); ++i)
        {
            if (track.beats[i] < 2.0 || ! track.isDownbeat ((int) i))
                continue;

            ++downbeatsSeen;
            const auto barPosition = std::fmod (track.beats[i], beatSeconds * 4);
            downbeatsOk += (barPosition < 0.1 || barPosition > beatSeconds * 4 - 0.1) ? 1 : 0;
        }

        check (downbeatsSeen > 0 && downbeatsOk >= downbeatsSeen * 9 / 10,
               "downbeats on bar starts: " + juce::String (downbeatsOk) + " of " + juce::String (downbeatsSeen));

        ChordTrack::Chord bFlat { 0.0, 1.0, 10, ChordTrack::major, -1, 0, 4 };
        ChordTrack spelling;
        spelling.keyRoot = 5;   // F major: flats
        check (spelling.chordName (bFlat) == "Bb" && spelling.chordName (bFlat, 2) == "C", "names follow key and transpose: "
               + spelling.chordName (bFlat) + ", " + spelling.chordName (bFlat, 2));
        spelling.keyRoot = 4;   // E major: sharps
        check (spelling.chordName (bFlat) == "A#", "sharp spelling in E major: " + spelling.chordName (bFlat));
    }

    /** Descending bass line C | G/B | Am | C/G: inversions must keep their chord and show the bass. */
    void testSlashChords()
    {
        std::cout << "Slash chords" << std::endl;

        constexpr double sampleRate = 44100.0;
        constexpr int bars = 16;
        const std::vector<TestChord> progression { { 0, false, -1 }, { 7, false, 11 }, { 9, true, -1 }, { 0, false, 7 } };
        const auto audio = synthesiseBand (progression, bars, sampleRate);
        const auto track = analyseChords (audio.data(), (int) audio.size(), sampleRate);

        auto correct = 0, total = 0;

        for (int bar = 1; bar < bars - 1; ++bar)
        {
            const auto& truth = progression[(size_t) (bar % 4)];
            const auto index = track.chordIndexAt ((bar * 4 + 2) * 0.5);
            ++total;

            if (index >= 0)
            {
                const auto& c = track.chords[(size_t) index];
                const auto minor = c.quality == ChordTrack::minor || c.quality == ChordTrack::minor7;
                correct += (c.root == truth.root && minor == truth.minor && c.bass == truth.bass) ? 1 : 0;
            }
        }

        juce::StringArray names;

        for (const auto& c : track.chords)
            names.add (track.chordName (c));

        check (correct >= total - 1, "chord and bass correct in " + juce::String (correct) + " of " + juce::String (total) + " bars");
        std::cout << "        detected: " << names.joinIntoString (" ").substring (0, 120) << std::endl;

        ChordTrack::Chord gOverB { 0.0, 1.0, 7, ChordTrack::major, 11, 0, 4 };
        ChordTrack spelling;
        check (spelling.chordName (gOverB) == "G/B" && spelling.chordName (gOverB, 3) == "Bb/D",
               "slash names transpose: " + spelling.chordName (gOverB) + ", " + spelling.chordName (gOverB, 3));
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testPlayer();
    testFileLoading();
    testMetronome();
    testChordAnalysis();
    testSlashChords();

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
