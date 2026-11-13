// Offline checks of the song player and metronome DSP. Run: PracticeVibesTests(.exe)
// Exit code 0 = all passed.

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

            const auto maxPeak = *std::max_element (song->peaks.begin(), song->peaks.end());
            check (maxPeak > 0.4f && maxPeak < 0.6f, "waveform overview computed: max " + juce::String (maxPeak, 2));
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

        {
            MetronomeProcessor metronome;
            metronome.prepareToPlay (rate, 512);
            metronome.startBpm = 120.0;
            metronome.beatsPerBar = 4;
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
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testPlayer();
    testFileLoading();
    testMetronome();

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
