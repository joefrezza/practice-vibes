#include "WaveformView.h"
#include "Theme.h"

namespace pv
{

WaveformView::WaveformView (PlayerProcessor& p)
    : player (p)
{
    setOpaque (true);
}

void WaveformView::songChanged()
{
    song = player.getSong();
    repaint();
}

double WaveformView::xToSeconds (float x) const
{
    if (song == nullptr || getWidth() <= 0)
        return 0.0;

    return juce::jlimit (0.0, song->getLengthSeconds(), (double) x / getWidth() * song->getLengthSeconds());
}

float WaveformView::secondsToX (double seconds) const
{
    if (song == nullptr || song->getLengthSeconds() <= 0.0)
        return 0.0f;

    return (float) (seconds / song->getLengthSeconds() * getWidth());
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    if (song == nullptr)
    {
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (juce::FontOptions (18.0f));
        g.drawText ("Open an audio file to start", getLocalBounds(), juce::Justification::centred);
        return;
    }

    const auto width = getWidth();
    const auto height = (float) getHeight();
    const auto mid = height * 0.5f;

    // Loop region (under the waveform)
    if (player.hasLoop())
    {
        const auto x1 = secondsToX (player.getLoopStartSeconds());
        const auto x2 = secondsToX (player.getLoopEndSeconds());
        g.setColour (theme::accent.withAlpha (player.isLoopEnabled() ? 0.35f : 0.15f));
        g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, height));
        g.setColour (theme::accent);
        g.drawVerticalLine ((int) x1, 0.0f, height);
        g.drawVerticalLine ((int) x2, 0.0f, height);
    }

    // Waveform
    const auto& peaks = song->peaks;
    const auto numPeaks = (int) peaks.size();
    g.setColour (juce::Colour (0xff8fb3d9));

    for (int x = 0; x < width && numPeaks > 0; ++x)
    {
        const auto begin = x * numPeaks / width;
        const auto end = juce::jmax (begin + 1, (x + 1) * numPeaks / width);
        float peak = 0.0f;

        for (int i = begin; i < end && i < numPeaks; ++i)
            peak = juce::jmax (peak, peaks[(size_t) i]);

        const auto h = juce::jmax (1.0f, peak * (height - 8.0f));
        g.drawVerticalLine (x, mid - h * 0.5f, mid + h * 0.5f);
    }

    // Selection being dragged
    if (dragging)
    {
        const auto x1 = juce::jmin (dragStartX, dragEndX);
        const auto x2 = juce::jmax (dragStartX, dragEndX);
        g.setColour (juce::Colours::white.withAlpha (0.2f));
        g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, height));
    }

    // Playhead
    g.setColour (juce::Colours::white);
    g.fillRect (juce::Rectangle<float> (secondsToX (player.getPositionSeconds()) - 1.0f, 0.0f, 2.0f, height));
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    dragStartX = dragEndX = e.position.x;
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    dragEndX = juce::jlimit (0.0f, (float) getWidth(), e.position.x);

    if (std::abs (dragEndX - dragStartX) > 10.0f)
        dragging = true;

    if (dragging)
        repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (song == nullptr)
        return;

    if (dragging)
    {
        dragging = false;

        if (onLoopSelected)
            onLoopSelected (xToSeconds (juce::jmin (dragStartX, dragEndX)), xToSeconds (juce::jmax (dragStartX, dragEndX)));
    }
    else if (onSeek)
    {
        onSeek (xToSeconds (e.position.x));
    }

    repaint();
}

} // namespace pv
