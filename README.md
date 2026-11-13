# Practice Vibes

A free, open-source, all-in-one guitar practice app, mainly for iPad (with a
Windows version).

The goal is to bring together in one place things that today need several apps:
playing along with songs, slowing them down, looping sections, chord
recognition, a progressive metronome, a multi-track looper, AUv3 effect chains
with presets, a drum machine synced to the looper, Bluetooth MIDI footswitch
control, and a place for lesson notes and practice schedules.

> **Status: early feasibility prototype.** The current build exists to prove
> the hardest parts work on a real iPad: hosting AUv3 plugins, Bluetooth MIDI
> footswitches, and installing without a Mac. It is not yet useful for practice.

## Credits

Practice Vibes is being built by *vibe coding*: the code is written by
[Claude](https://www.anthropic.com/claude) (Anthropic's AI) through
[Claude Code](https://claude.com/claude-code), steered by a guitarist who wanted
this app to exist. The name is a nod to that process.

It is built on [JUCE](https://juce.com).

## What works so far

- Song player for audio files (WAV, AIFF, FLAC, MP3, M4A...) with waveform view
- Speed change without pitch change (25-150 %) and pitch change without speed
  change (+/-12 semitones), powered by
  [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)
- A-B looping (drag across the waveform, or Set A / Set B)
- Metronome with a speed trainer (e.g. +2 bpm every minute, up to a limit)
- Guitar input -> chain of plugins -> output, with input/output gain and meters
- AUv3 plugins on iPad, VST3 plugins on Windows, with their own interfaces
- The chain (including each plugin's settings) is saved and restored
- Bluetooth MIDI pairing and a live view of incoming MIDI messages
- A footswitch can be "learned" to toggle bypass on a plugin

## Installing on iPad (no Mac needed)

The app isn't on the App Store. It is installed with
[SideStore](https://sidestore.io), which signs apps with your own free Apple ID.

1. Set up SideStore by following its official guide at
   [docs.sidestore.io](https://docs.sidestore.io). This needs a computer once.
2. On the iPad, open this repository's
   [Releases](../../releases/tag/latest-build) page in Safari and download
   `PracticeVibes.ipa`.
3. In SideStore, go to **My Apps**, tap **+** and pick the downloaded file.

Apps installed with a free Apple ID must be refreshed every 7 days; SideStore
does this from the iPad.

On first launch, allow microphone access (needed for the guitar input).
The log file can be found in the Files app under
**On My iPad > Practice Vibes**.

## Building

Builds for both platforms run automatically on GitHub Actions for every push
(see `.github/workflows/build.yml`).

### Windows

Requirements: Visual Studio 2022 Build Tools (C++ workload), CMake 3.22+,
Ninja, Git. JUCE is downloaded automatically.

From a "x64 Native Tools" command prompt:

```
cmake --preset windows
cmake --build --preset windows-release
```

In VS Code, the CMake Tools extension picks up `CMakePresets.json`.

Offline DSP tests: run `PracticeVibesTests` from the same build folder.

### iOS

Requires macOS with Xcode:

```
cmake --preset ios
cmake --build build/ios --config Release --target PracticeVibes -- -sdk iphoneos
```

## License

[GNU AGPLv3](LICENSE). JUCE is used under its AGPLv3 option.
