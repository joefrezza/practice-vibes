#include "ChordView.h"
#include "Theme.h"

namespace pv
{

void ChordView::setTrack (std::shared_ptr<const ChordTrack> newTrack, const juce::String& newStatus)
{
    track = std::move (newTrack);
    status = newStatus;
    chordIndex = beatIndex = -1;
    repaint();
}

void ChordView::update (double positionSeconds, int semitones)
{
    if (track == nullptr)
        return;

    const auto newChord = track->chordIndexAt (positionSeconds);
    const auto& beats = track->beats;
    const auto newBeat = (int) std::distance (beats.begin(), std::upper_bound (beats.begin(), beats.end(), positionSeconds)) - 1;

    if (newChord != chordIndex || newBeat != beatIndex || semitones != transpose)
    {
        chordIndex = newChord;
        beatIndex = newBeat;
        transpose = semitones;
        repaint();
    }
}

void ChordView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);

    if (track == nullptr || track->chords.empty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText (status.isNotEmpty() ? status : juce::String ("Chords appear here after you open a song."),
                          getLocalBounds().reduced (12), juce::Justification::centred, 3);
        return;
    }

    const auto& chords = track->chords;
    const auto current = juce::jlimit (0, (int) chords.size() - 1, chordIndex < 0 ? 0 : chordIndex);
    const auto& now = chords[(size_t) current];
    const auto started = chordIndex >= 0;

    auto area = getLocalBounds().reduced (12, 8);
    auto footer = area.removeFromBottom (18);
    auto strip = area.removeFromBottom (juce::jmin (28, area.getHeight() / 4));
    auto left = area.removeFromLeft (area.getWidth() * 55 / 100);
    auto right = area.withTrimmedLeft (12);

    auto caption = [&g] (juce::Rectangle<int>& r, const juce::String& text)
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (juce::FontOptions (12.0f).withStyle ("Bold"));
        g.drawText (text, r.removeFromTop (16), juce::Justification::centredLeft);
    };

    // Now ----------------------------------------------------------------------
    caption (left, "NOW");
    auto dotsRow = left.removeFromBottom (14);
    const auto bigSize = juce::jlimit (24.0f, 84.0f, (float) left.getHeight() * 0.9f);
    g.setColour (now.root < 0 ? juce::Colours::white.withAlpha (0.4f) : theme::highlight.withAlpha (started ? 1.0f : 0.5f));
    g.setFont (juce::FontOptions (bigSize).withStyle ("Bold"));
    g.drawFittedText (track->chordName (now, transpose), left, juce::Justification::centredLeft, 1, 0.6f);

    // One dot per beat of the current chord, lit up to the current beat.
    {
        const auto numDots = juce::jlimit (1, 16, now.numBeats);
        const auto done = started ? juce::jlimit (0, numDots, beatIndex - now.startBeat + 1) : 0;
        const auto size = 9.0f;

        for (int i = 0; i < numDots; ++i)
        {
            const auto isDownbeat = track->isDownbeat (now.startBeat + i);
            g.setColour (i < done ? (isDownbeat ? theme::highlight : theme::accent.brighter (0.4f))
                                  : juce::Colours::white.withAlpha (isDownbeat ? 0.25f : 0.12f));
            g.fillEllipse ((float) dotsRow.getX() + (float) i * (size + 5.0f), (float) dotsRow.getCentreY() - size * 0.5f, size, size);
        }
    }

    // Next ---------------------------------------------------------------------
    const auto nextIndex = started ? current + 1 : current;

    caption (right, "NEXT");

    if (nextIndex < (int) chords.size())
    {
        const auto& next = chords[(size_t) nextIndex];
        auto countRow = right.removeFromBottom (20);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::FontOptions (juce::jlimit (18.0f, 48.0f, (float) right.getHeight() * 0.7f)).withStyle ("Bold"));
        g.drawFittedText (track->chordName (next, transpose), right, juce::Justification::centredLeft, 1, 0.6f);

        const auto beatsLeft = next.startBeat - juce::jmax (0, beatIndex);
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (beatsLeft <= 1 ? juce::String ("next beat") : "in " + juce::String (beatsLeft) + " beats",
                    countRow, juce::Justification::centredLeft);
    }

    // Then: the chords after "next", with their length in beats ---------------
    {
        g.setFont (juce::FontOptions (15.0f));
        auto x = (float) strip.getX();
        const auto y = (float) strip.getY(), h = (float) strip.getHeight();

        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.drawText ("then", juce::Rectangle<float> (x, y, 40.0f, h), juce::Justification::centredLeft);
        x += 42.0f;

        for (auto i = nextIndex + 1; i < (int) chords.size(); ++i)
        {
            const auto& c = chords[(size_t) i];
            const auto text = track->chordName (c, transpose);
            const auto width = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 26.0f;

            if (x + width > (float) strip.getRight())
                break;

            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.drawText (text, juce::Rectangle<float> (x, y, width, h), juce::Justification::centredLeft);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (c.numBeats), juce::Rectangle<float> (x + width - 24.0f, y, 20.0f, h * 0.6f),
                        juce::Justification::centredLeft);
            g.setFont (juce::FontOptions (15.0f));
            x += width;
        }
    }

    // Footer -------------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Key " + track->keyName (transpose) + "  |  ~" + juce::String (juce::roundToInt (track->bpm)) + " bpm  |  "
                    + track->timeSignatureAt (juce::jmax (0, beatIndex)) + (track->hasMeterChanges() ? " (changes)" : "")
                    + "  |  detected automatically, may contain mistakes",
                footer, juce::Justification::centredLeft, true);
}

} // namespace pv
