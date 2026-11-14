// Offline checks of the song player, metronome and looper DSP. Run: PracticeVibesTests(.exe)
// Exit code 0 = all passed.

#include "../Source/BarClockPlayHead.h"
#include "../Source/ChordAnalyzer.h"
#include "../Source/Looper.h"
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
    std::vector<float> synthesiseBand (const std::vector<TestChord>& progression, int bars, double sampleRate,
                                       int beatsPerBar = 4, double beatSeconds = 0.5)
    {
        const auto length = (int) (sampleRate * beatSeconds * beatsPerBar * bars);
        std::vector<float> audio ((size_t) length, 0.0f);
        auto midiToHz = [] (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); };
        juce::Random random (42);

        for (int i = 0; i < length; ++i)
        {
            const auto t = i / sampleRate;
            const auto beat = (int) (t / beatSeconds);
            const auto sinceBeat = t - beat * beatSeconds;
            const auto& chord = progression[(size_t) ((beat / beatsPerBar) % (int) progression.size())];
            const auto triadBase = 60 + (chord.root > 4 ? chord.root - 12 : chord.root);
            const int notes[] = { triadBase, triadBase + (chord.minor ? 3 : 4), triadBase + 7 };
            auto sample = 0.0;

            for (auto note : notes)
                for (int h = 1; h <= 5; ++h)
                    sample += 0.08 / h * std::sin (juce::MathConstants<double>::twoPi * midiToHz (note) * h * t);

            const auto bassNote = 36 + (chord.bass >= 0 ? chord.bass : chord.root);
            sample += 0.2 * std::sin (juce::MathConstants<double>::twoPi * midiToHz (bassNote) * t) * std::exp (-sinceBeat * 3.0);
            sample += (beat % beatsPerBar == 0 ? 0.7 : 0.5) * std::exp (-sinceBeat * 40.0) * (std::sin (juce::MathConstants<double>::twoPi * 60.0 * sinceBeat)
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

        {
            ChordTrack grid;   // 4/4 at 120 bpm, first downbeat on the second beat (pickup)
            for (int i = 0; i < 17; ++i)
            {
                grid.beats.push_back (0.25 + i * 0.5);
                grid.beatPositions.push_back ((i + 3) % 4);
            }

            using U = ChordTrack::GridUnit;
            check (std::abs (grid.snapToGrid (2.6, U::bar) - 2.75) < 1.0e-9 && std::abs (grid.snapToGrid (1.6, U::bar) - 0.75) < 1.0e-9
                       && std::abs (grid.snapToGrid (1.6, U::beat) - 1.75) < 1.0e-9 && grid.snapToGrid (0.2, U::bar) == 0.0,
                   "snap to bars/beats: 2.6->" + juce::String (grid.snapToGrid (2.6, U::bar)) + " 1.6->" + juce::String (grid.snapToGrid (1.6, U::bar))
                       + " (beat " + juce::String (grid.snapToGrid (1.6, U::beat)) + ")");
            check (std::abs (grid.neighbourGridLine (0.75, U::bar, 1) - 2.75) < 1.0e-9 && std::abs (grid.neighbourGridLine (2.75, U::bar, -1) - 0.75) < 1.0e-9
                       && grid.neighbourGridLine (8.25, U::bar, 1) < 0.0,
                   "neighbouring bar lines");
        }

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

    /** Odd meters: chords change once per bar of 3, 5 or 7 beats. */
    void testMeters()
    {
        std::cout << "Meters" << std::endl;

        const std::vector<TestChord> progression { { 0, false, -1 }, { 9, true, -1 }, { 5, false, -1 }, { 7, false, -1 } };

        struct Case { int beatsPerBar; double beatSeconds; const char* name; };

        for (auto c : { Case { 4, 0.5, "4/4" }, Case { 3, 0.5, "3/4" }, Case { 5, 0.5, "5/4" },
                        Case { 7, 0.3, "7/8" }, Case { 7, 0.25, "7/8" }, Case { 5, 0.25, "5/8" } })
        {
            const auto bars = juce::jmax (12, 96 / c.beatsPerBar);
            const auto audio = synthesiseBand (progression, bars, 44100.0, c.beatsPerBar, c.beatSeconds);
            const auto track = analyseChords (audio.data(), (int) audio.size(), 44100.0);
            check (track.timeSignature() == c.name, juce::String (c.name) + " (pulse " + juce::String (60.0 / c.beatSeconds, 0)
                                                        + " bpm): detected " + track.timeSignature() + " at " + juce::String (track.bpm, 1) + " bpm");
        }

        // Meter change: 16 bars of 7/4, 16 of 4/4, 16 of 7/4 (like "Money").
        {
            auto audio = synthesiseBand (progression, 16, 44100.0, 7, 0.5);
            const auto middle = synthesiseBand ({ { 2, false, -1 }, { 7, false, -1 } }, 16, 44100.0, 4, 0.5);
            audio.insert (audio.end(), middle.begin(), middle.end());
            const auto end = synthesiseBand (progression, 16, 44100.0, 7, 0.5);
            audio.insert (audio.end(), end.begin(), end.end());
            const auto track = analyseChords (audio.data(), (int) audio.size(), 44100.0);

            auto meterAt = [&track] (double seconds)
            {
                const auto& b = track.beats;
                const auto index = (int) std::distance (b.begin(), std::upper_bound (b.begin(), b.end(), seconds)) - 1;
                return track.timeSignatureAt (index);
            };

            const auto first = meterAt (28.0), second = meterAt (56.0 + 16.0), third = meterAt (56.0 + 32.0 + 28.0);
            check (first == "7/4" && second == "4/4" && third == "7/4",
                   "meter change 7/4 -> 4/4 -> 7/4: detected " + first + " -> " + second + " -> " + third);
        }
    }

    //==============================================================================
    /** Drives a metronome + looper the way the app's audio callback does. */
    struct LooperRig
    {
        static constexpr double rate = 48000.0;
        MetronomeProcessor metronome;
        Looper looper { metronome };
        juce::int64 time = 0;
        std::vector<float> out;     // output (left channel) by graph time
        bool beforeFx = false;
        int blockSize = 480;        // deliberately not a divisor of the bar length

        LooperRig()
        {
            metronome.prepareToPlay (rate, 1024);
            looper.prepare (rate, 1024);
            metronome.gain = 0.0f;
            metronome.startBpm = 120.0;     // 4/4: a bar is 2 s = 96000 samples
        }

        template <typename Input>
        void runUntil (double seconds, Input input)
        {
            const auto until = (juce::int64) (seconds * rate);
            juce::AudioBuffer<float> click (2, 1024), audio (2, 1024);
            juce::MidiBuffer midi;

            if ((juce::int64) out.size() < until)
                out.resize ((size_t) until, 0.0f);

            while (time < until)
            {
                const auto n = (int) juce::jmin ((juce::int64) blockSize, until - time);
                metronome.advanceClock (time, n);
                looper.beginBlock (time, n);

                for (int i = 0; i < n; ++i)
                {
                    const auto v = input (time + i);
                    audio.setSample (0, i, v);
                    audio.setSample (1, i, v);
                }

                if (beforeFx)
                    looper.processBeforeFx (audio.getWritePointer (0), n);
                else
                    looper.processAfterFx (audio.getWritePointer (0), audio.getWritePointer (1), n);

                click.setSize (2, n, false, false, true);
                metronome.processBlock (click, midi);

                for (int i = 0; i < n; ++i)
                    out[(size_t) (time + i)] = audio.getSample (0, i);

                time += n;
            }
        }

        /** Largest difference between the output and `expected` over [from, to) seconds. */
        template <typename Expected>
        float maxError (double from, double to, Expected expected) const
        {
            float worst = 0.0f;

            for (auto g = (juce::int64) (from * rate); g < (juce::int64) (to * rate); ++g)
                worst = juce::jmax (worst, std::abs (out[(size_t) g] - expected (g)));

            return worst;
        }
    };

    float noiseAt (juce::int64 g, int seed = 1)
    {
        return (float) (((g * 7919 + seed * 104729) % 10007) / 10007.0 - 0.5);
    }

    void testLooper()
    {
        std::cout << "Looper" << std::endl;
        constexpr double rate = LooperRig::rate;
        constexpr double bar = 2.0 * rate;
        const auto silence = [] (juce::int64) { return 0.0f; };

        // The loop holds what was played from `origin`, and repeats with `period`.
        auto loopOf = [] (double origin, double period, int seed = 1)
        {
            return [origin, period, seed] (juce::int64 g)
            {
                auto offset = std::fmod ((double) g - origin, period);
                offset = offset < 0 ? offset + period : offset;
                return noiseAt ((juce::int64) origin + (juce::int64) offset, seed);
            };
        };

        {
            // Fixed 2 bars, pressed 0.1 s late: starts retroactively on the first bar line.
            LooperRig rig;
            rig.looper.fixedBars = 2;
            rig.metronome.start();
            rig.runUntil (0.1, [] (juce::int64 g) { return noiseAt (g); });
            rig.looper.pressTrack (0);
            rig.runUntil (4.05, [] (juce::int64 g) { return noiseAt (g); });
            check (rig.looper.getState (0) == Looper::State::playing, "fixed 2-bar loop plays after 2 bars");
            check (rig.looper.getBars (0) == 2, "loop is 2 bars: " + juce::String (rig.looper.getBars (0)));
            check (rig.metronome.isTempoLocked(), "tempo locked while loops exist");
            rig.runUntil (12.0, silence);
            const auto error = rig.maxError (4.0, 12.0, loopOf (0.0, 2 * bar));
            check (error < 1.0e-6f, "late press: loop starts on the bar line and repeats exactly (error " + juce::String (error) + ")");
        }

        {
            // Latency compensation: the guitar arrives 25 ms late, the loop must still sit on the grid.
            LooperRig rig;
            const auto latency = 1200;
            rig.looper.fixedBars = 1;
            rig.looper.deviceLatencySamples = latency;
            rig.metronome.start();
            rig.runUntil (1.9, silence);
            rig.looper.pressTrack (0);      // early press: waits for the bar line at 2 s
            auto late = [latency] (juce::int64 g) { return g >= 96000 + latency && g < 2 * 96000 + latency ? noiseAt (g - latency) : 0.0f; };
            rig.runUntil (8.0, late);
            const auto error = rig.maxError (4.0, 8.0, loopOf (bar, bar));
            check (error < 1.0e-6f, "latency-compensated loop sits on the grid (error " + juce::String (error) + ")");
        }

        {
            // Free length: stop pressed 0.1 s after the 2nd bar line -> exactly 2 bars.
            LooperRig rig;
            rig.metronome.start();
            rig.runUntil (0.02, [] (juce::int64 g) { return noiseAt (g); });
            rig.looper.pressTrack (0);
            rig.runUntil (4.1, [] (juce::int64 g) { return noiseAt (g); });
            check (rig.looper.getState (0) == Looper::State::recording, "free recording keeps going");
            rig.looper.pressTrack (0);
            rig.runUntil (12.0, silence);
            check (rig.looper.getBars (0) == 2, "free stop rounds to 2 bars: " + juce::String (rig.looper.getBars (0)));
            const auto error = rig.maxError (4.2, 12.0, loopOf (0.0, 2 * bar));
            check (error < 1.0e-6f, "free loop repeats exactly (error " + juce::String (error) + ")");
        }

        {
            // Count-in: metronome stopped, the press starts it and recording begins one bar later.
            LooperRig rig;
            rig.looper.fixedBars = 1;
            rig.runUntil (0.5, silence);
            rig.looper.pressTrack (0);
            rig.runUntil (2.0, silence);
            check (rig.metronome.isRunning(), "pressing record starts the metronome");
            check (rig.looper.getState (0) == Looper::State::armed, "armed during the count-in bar");
            rig.runUntil (8.0, [] (juce::int64 g) { return noiseAt (g); });
            const auto start = 0.5 * rate + bar;    // first block after the press starts the metronome
            const auto error = rig.maxError (5.0, 8.0, loopOf (start, bar));
            check (error < 1.0e-6f, "recording starts after one bar of count-in (error " + juce::String (error) + ")");
        }

        {
            // Overdub, undo, and the merge of an older overdub.
            LooperRig rig;
            rig.looper.fixedBars = 1;
            rig.metronome.start();
            rig.looper.pressTrack (0);
            rig.runUntil (2.0, [] (juce::int64 g) { return noiseAt (g); });
            rig.runUntil (3.0, silence);

            // Overdub input repeats with the loop, so what lands at each loop position is predictable.
            rig.looper.pressTrack (0);      // overdub for exactly one cycle, 3 s -> 5 s
            rig.runUntil (5.0, [] (juce::int64 g) { return noiseAt (g % 96000, 2); });
            rig.looper.pressTrack (0);
            check (rig.looper.canUndo(), "an overdub can be undone");
            rig.runUntil (8.0, silence);

            auto base = loopOf (0.0, bar);
            auto dub = loopOf (0.0, bar, 2);
            auto both = [&] (juce::int64 g) { return base (g) + dub (g); };
            auto error = rig.maxError (6.0, 8.0, both);
            check (error < 1.0e-5f, "overdub adds to the loop (error " + juce::String (error) + ")");

            rig.looper.undo();
            rig.runUntil (10.0, silence);
            error = rig.maxError (8.1, 10.0, base);
            check (error < 1.0e-6f, "undo removes the overdub (error " + juce::String (error) + ")");

            // Two overdubs: starting the second merges the first; undo then removes only the second.
            rig.looper.pressTrack (0);
            rig.runUntil (12.0, [] (juce::int64 g) { return noiseAt (g % 96000, 2); });
            rig.looper.pressTrack (0);
            rig.looper.pressTrack (0);
            rig.runUntil (14.0, [] (juce::int64 g) { return noiseAt (g % 96000, 3); });
            rig.looper.pressTrack (0);
            rig.runUntil (16.0, silence);
            rig.looper.undo();
            rig.runUntil (20.0, silence);
            error = rig.maxError (16.1, 20.0, both);
            check (error < 1.0e-5f, "first overdub merged, second undone (error " + juce::String (error) + ")");
        }

        {
            // Second track: 2 bars over a 1-bar loop, started late in a bar; both stay on the grid.
            LooperRig rig;
            rig.looper.fixedBars = 1;
            rig.metronome.start();
            rig.looper.pressTrack (0);
            rig.runUntil (2.0, [] (juce::int64 g) { return noiseAt (g); });
            rig.looper.fixedBars = 2;
            rig.runUntil (5.0, silence);
            rig.looper.pressTrack (1);      // 1 s into a bar: waits for 6 s
            rig.runUntil (10.0, [] (juce::int64 g) { return g >= 6 * 48000 ? noiseAt (g, 4) : 0.0f; });
            rig.runUntil (16.0, silence);
            auto a = loopOf (0.0, bar);
            auto b = loopOf (6.0 * rate, 2 * bar, 4);
            const auto error = rig.maxError (10.0, 16.0, [&] (juce::int64 g) { return a (g) + b (g); });
            check (error < 1.0e-5f, "two tracks of different lengths stay on the grid (error " + juce::String (error) + ")");

            // Stop and restart the metronome: the clicks come back on the loop's bar lines.
            rig.metronome.stop();
            rig.runUntil (16.73, silence);
            rig.metronome.start();
            rig.runUntil (17.0, silence);
            const auto barStart = rig.metronome.getClockBarStart();
            check (std::abs (std::fmod (barStart, bar)) < 1.0e-6 && std::abs (rig.metronome.getCurrentBpm() - 120.0) < 1.0e-6,
                   "restarted metronome follows the loop grid (bar start " + juce::String (barStart / rate, 4) + " s)");

            rig.looper.clearAll();
            rig.runUntil (17.1, silence);
            check (rig.looper.isEmpty() && ! rig.metronome.isTempoLocked(), "clear all frees the tempo");
        }

        {
            // Before FX: loops are mixed into the dry signal that feeds the plugins.
            LooperRig rig;
            rig.beforeFx = true;
            rig.looper.source = (int) Looper::Source::beforeFx;
            rig.looper.fixedBars = 1;
            rig.metronome.start();
            rig.looper.pressTrack (0);
            rig.runUntil (2.0, [] (juce::int64 g) { return noiseAt (g); });
            rig.runUntil (6.0, [] (juce::int64 g) { return 0.25f * noiseAt (g, 5); });
            auto expected = [loop = loopOf (0.0, bar)] (juce::int64 g) { return loop (g) + 0.25f * noiseAt (g, 5); };
            const auto error = rig.maxError (2.0, 6.0, expected);
            check (rig.looper.getActiveSource() == Looper::Source::beforeFx && error < 1.0e-5f,
                   "before-FX loop plays under the live dry guitar (error " + juce::String (error) + ")");
        }
    }


    /** Metronome following the song's beat grid through the player, at two speeds,
        in both processing orders, and across a loop jump. */
    void testPlayHead()
    {
        std::cout << "Play head (bar clock -> plugins)" << std::endl;

        const double rate = 48000.0;
        const int block = 480;
        MetronomeProcessor metronome;
        metronome.prepareToPlay (rate, block);
        metronome.startBpm = 120.0;
        metronome.beatsPerBar = 4;

        juce::int64 time = 0;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;

        // Like the app: advance the clock, read the play head, then run the graph.
        auto step = [&]
        {
            metronome.advanceClock (time, block);
            const auto s = BarClockPlayHead::compute (metronome, time, rate);
            metronome.processBlock (buffer, midi);
            time += block;
            return s;
        };

        auto s = step();
        check (! s.playing && std::abs (s.bpm - 120.0) < 1.0e-9 && s.numerator == 4, "stopped: not playing, shows the set tempo and meter");

        metronome.start();
        s = step();
        check (s.playing && std::abs (s.ppq) < 1.0e-9 && s.ppqOfBarStart == 0.0, "starts at ppq 0, bar 1");

        // 2 s at 120 bpm = 4 beats = one bar
        for (int i = 1; i < 200; ++i)
            s = step();

        s = step();
        check (std::abs (s.ppq - 4.0) < 1.0e-6 && std::abs (s.ppqOfBarStart - 4.0) < 1.0e-9 && s.barCount == 1,
               "after 2 s: ppq " + juce::String (s.ppq, 4) + ", bar start " + juce::String (s.ppqOfBarStart) + " (want 4, 4)");

        // Tempo changes (user + trainer steps) keep the position continuous and bars whole.
        metronome.trainerEnabled = true;
        metronome.trainerStepBpm = 7.0;
        metronome.trainerInterval = 1.0;
        metronome.trainerUnit = (int) MetronomeProcessor::IntervalUnit::bars;
        metronome.trainerMaxBpm = 200.0;
        metronome.startBpm = 90.0;
        metronome.tempoChangedByUser();

        bool continuous = true, barsOk = true;
        auto previous = s;

        for (int i = 0; i < 2000; ++i)
        {
            s = step();
            const auto delta = s.ppq - previous.ppq;
            const auto slowest = block / (rate * 60.0 / 90.0), fastest = block / (rate * 60.0 / 200.0);

            if (continuous && ! (delta > slowest * 0.999 && delta < fastest * 1.001))
                std::cout << "    jump at block " << i << ": delta " << delta << " ppq " << s.ppq << " bpm " << s.bpm << " prev bpm " << previous.bpm << std::endl;

            continuous = continuous && delta > slowest * 0.999 && delta < fastest * 1.001;
            barsOk = barsOk && std::fmod (s.ppqOfBarStart, 4.0) < 1.0e-6 && s.ppq - s.ppqOfBarStart >= -1.0e-6
                            && s.ppq - s.ppqOfBarStart < 4.0 + 1.0e-6;
            previous = s;
        }

        check (continuous, "ppq advances smoothly through tempo changes (now " + juce::String (s.bpm, 1) + " bpm)");
        check (barsOk, "bar starts stay on whole 4/4 bars");

        metronome.stop();
        s = step();
        check (! s.playing, "metronome stopped: plugins see the transport stop");
    }

    void testFollowSong()
    {
        std::cout << "Metronome follows song" << std::endl;
        constexpr double rate = 48000.0;
        constexpr int blockSize = 512;

        auto grid = std::make_shared<ChordTrack>();

        for (int i = 0; i < 40; ++i)
        {
            grid->beats.push_back (0.5 * i);
            grid->beatPositions.push_back (i % 4);
            grid->beatMeters.push_back (4);
        }

        struct Case { double speed; bool playerFirst; bool loop; const char* name; };

        for (auto c : { Case { 1.0, true, false, "1x, player first" }, Case { 1.0, false, false, "1x, metronome first" },
                        Case { 0.75, true, false, "0.75x" }, Case { 1.0, true, true, "loop 1-3 s" } })
        {
            PlayerProcessor player;
            MetronomeProcessor metronome;
            player.prepareToPlay (rate, blockSize);
            metronome.prepareToPlay (rate, blockSize);
            player.setSong (makeSine (44100.0, 220.0, 12.0));
            player.setSpeed (c.speed);

            if (c.loop)
            {
                player.setLoopSeconds (1.0, 3.0);
                player.setLoopEnabled (true);
            }

            metronome.setPlayer (&player);
            metronome.setSongGrid (grid);
            metronome.followSong = true;
            metronome.sound = (int) MetronomeProcessor::Sound::beep;
            metronome.start();
            player.play();

            juce::AudioBuffer<float> playerBlock (2, blockSize), clickBlock (2, blockSize);
            juce::MidiBuffer midi;
            std::vector<float> clicks;
            std::vector<std::pair<juce::int64, double>> heard;   // (sample index at block end, heard song time)
            const auto totalBlocks = (int) (7.0 * rate / blockSize);

            for (int b = 0; b < totalBlocks; ++b)
            {
                if (c.playerFirst)
                {
                    player.processBlock (playerBlock, midi);
                    metronome.processBlock (clickBlock, midi);
                }
                else
                {
                    metronome.processBlock (clickBlock, midi);
                    player.processBlock (playerBlock, midi);
                }

                heard.push_back ({ (juce::int64) (b + 1) * blockSize, player.getPositionSeconds() });

                for (int i = 0; i < blockSize; ++i)
                    clicks.push_back (clickBlock.getSample (0, i));
            }

            // Click onsets, and the song time heard at each.
            std::vector<double> songTimes;
            std::vector<float> peaks;
            auto quiet = 1 << 30;

            for (size_t i = 0; i < clicks.size(); ++i)
            {
                if (std::abs (clicks[i]) > 0.05f)
                {
                    if (quiet > (int) (0.05 * rate))
                    {
                        const auto block = i / blockSize;
                        const auto t = heard[block].second - (double) (heard[block].first - (juce::int64) i) * c.speed / rate;
                        songTimes.push_back (t);
                        auto peak = 0.0f;

                        for (size_t j = i; j < juce::jmin (clicks.size(), i + 200); ++j)
                            peak = juce::jmax (peak, std::abs (clicks[j]));

                        peaks.push_back (peak);
                    }

                    quiet = 0;
                }
                else
                {
                    ++quiet;
                }
            }

            // Skip the first second (the stretcher fills up), then every click must sit on a beat.
            auto onBeat = 0, considered = 0, accentsOk = 0, loopStarts = 0;

            for (size_t k = 0; k < songTimes.size(); ++k)
            {
                if (songTimes[k] < 0.9 && ! c.loop)
                    continue;

                ++considered;
                const auto nearest = std::round (songTimes[k] / 0.5) * 0.5;
                onBeat += std::abs (songTimes[k] - nearest) < 0.015 ? 1 : 0;

                const auto isDownbeat = ((int) std::lround (nearest / 0.5)) % 4 == 0;
                accentsOk += (isDownbeat == (peaks[k] > 0.55f)) ? 1 : 0;
                loopStarts += c.loop && std::abs (songTimes[k] - 1.0) < 0.03 ? 1 : 0;
            }

            check (considered > 4 && onBeat == considered && accentsOk == considered,
                   juce::String (c.name) + ": " + juce::String (onBeat) + "/" + juce::String (considered) + " clicks on beats, "
                       + juce::String (accentsOk) + " with the right accent");

            if (c.loop)
                check (loopStarts >= 2, "loop restart clicks on its first beat: " + juce::String (loopStarts) + " times");

            if (c.speed != 1.0)
                check (std::abs (metronome.getCurrentBpm() - 120.0 * c.speed) < 1.0, "shown tempo follows speed: " + juce::String (metronome.getCurrentBpm(), 1));
        }
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
    testMeters();
    testFollowSong();
    testLooper();
    testPlayHead();

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
