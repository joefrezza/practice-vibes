#include "WaveformView.h"
#include "Theme.h"

namespace pv
{

namespace
{
    constexpr int overviewHeight = 30;
    constexpr int rulerHeight = 18;
    constexpr float edgeGrabDistance = 14.0f;
    const auto waveformColour = juce::Colour (0xff8fb3d9);

    juce::String formatRulerTime (double seconds, double step)
    {
        const auto minutes = (int) (seconds / 60.0);
        const auto rest = seconds - minutes * 60.0;
        const auto decimals = step < 0.1 ? 2 : (step < 1.0 ? 1 : 0);
        auto secondsText = juce::String (rest, decimals);

        if (rest < 10.0)
            secondsText = "0" + secondsText;

        return juce::String (minutes) + ":" + secondsText;
    }
}

WaveformView::WaveformView (PlayerProcessor& p)
    : player (p)
{
    setOpaque (true);
}

//==============================================================================
void WaveformView::songChanged()
{
    song = player.getSong();
    zoomToFit();
}

bool WaveformView::isZoomed() const
{
    return song != nullptr && viewLength < song->getLengthSeconds() - 1.0e-6;
}

double WaveformView::getMinViewLength() const
{
    return 0.02;   // 20 ms across the whole width: individual samples are visible
}

void WaveformView::setView (double start, double length)
{
    if (song == nullptr)
    {
        viewStart = 0.0;
        viewLength = 1.0;
        repaint();
        return;
    }

    const auto total = song->getLengthSeconds();
    viewLength = juce::jlimit (juce::jmin (getMinViewLength(), total), total, length);
    viewStart = juce::jlimit (0.0, total - viewLength, start);
    repaint();
}

void WaveformView::zoomAround (double factor, float x)
{
    const auto anchor = xToSeconds (x);
    const auto proportion = mainArea.getWidth() > 0 ? (double) (x - (float) mainArea.getX()) / mainArea.getWidth() : 0.5;
    const auto newLength = viewLength / factor;
    setView (anchor - proportion * newLength, newLength);
}

void WaveformView::zoomBy (double factor)
{
    zoomAround (factor, (float) mainArea.getCentreX());
}

void WaveformView::zoomToFit()
{
    setView (0.0, song != nullptr ? song->getLengthSeconds() : 1.0);
}

void WaveformView::zoomToRange (double startSeconds, double endSeconds)
{
    const auto length = juce::jmax (getMinViewLength(), endSeconds - startSeconds);
    const auto margin = length * 0.05;
    setView (startSeconds - margin, length + 2.0 * margin);
}

double WaveformView::xToSeconds (float x) const
{
    if (mainArea.getWidth() <= 0)
        return viewStart;

    return viewStart + (double) (x - (float) mainArea.getX()) / mainArea.getWidth() * viewLength;
}

float WaveformView::secondsToX (double seconds) const
{
    return (float) mainArea.getX() + (float) ((seconds - viewStart) / viewLength * mainArea.getWidth());
}

void WaveformView::refresh()
{
    if (song == nullptr)
        return;

    // Page-flip to follow the playhead, but only if it was on screen until now:
    // if the user scrolled elsewhere on purpose, leave the view alone.
    const auto position = player.getPositionSeconds();
    const auto visible = position >= viewStart && position <= viewStart + viewLength;

    if (player.isPlaying() && ! visible && playheadWasVisible && isZoomed()
         && gesture != Gesture::pinching && gesture != Gesture::overview)
    {
        setView (position - viewLength * 0.05, viewLength);
        playheadWasVisible = true;
    }
    else
    {
        playheadWasVisible = visible;
    }

    repaint();
}

//==============================================================================
void WaveformView::resized()
{
    auto area = getLocalBounds();
    overviewArea = area.removeFromBottom (overviewHeight);
    area.removeFromBottom (4);
    mainArea = area;
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    g.setColour (theme::panel);
    g.fillRect (mainArea);
    g.fillRect (overviewArea);

    if (song == nullptr)
    {
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (juce::FontOptions (18.0f));
        g.drawText ("Open an audio file to start", mainArea, juce::Justification::centred);
        return;
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (mainArea);
        paintWaveform (g, mainArea);
        paintTimeRuler (g, mainArea.withHeight (rulerHeight));
    }

    paintOverview (g, overviewArea);
}

void WaveformView::paintWaveform (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto top = (float) area.getY() + rulerHeight;
    const auto height = (float) area.getBottom() - top;
    const auto mid = top + height * 0.5f;
    const auto halfHeight = height * 0.5f - 3.0f;

    // Loop region
    if (player.hasLoop())
    {
        const auto x1 = secondsToX (player.getLoopStartSeconds());
        const auto x2 = secondsToX (player.getLoopEndSeconds());
        g.setColour (theme::accent.withAlpha (player.isLoopEnabled() ? 0.35f : 0.15f));
        g.fillRect (juce::Rectangle<float> (x1, (float) area.getY(), x2 - x1, (float) area.getHeight()));
        g.setColour (theme::accent.brighter (0.3f));
        g.fillRect (juce::Rectangle<float> (x1 - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight()));
        g.fillRect (juce::Rectangle<float> (x2 - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight()));
    }

    // Waveform
    const auto sampleRate = song->sampleRate;
    const auto numSamples = song->audio.getNumSamples();
    const auto width = area.getWidth();
    const auto samplesPerPixel = viewLength * sampleRate / juce::jmax (1, width);
    const auto* left = song->audio.getReadPointer (0);
    const auto* right = song->audio.getReadPointer (1);
    auto mono = [left, right] (int i) { return 0.5f * (left[i] + right[i]); };

    g.setColour (waveformColour);

    if (samplesPerPixel < 1.0)
    {
        // Very close: draw the actual sample curve.
        juce::Path path;
        const auto first = juce::jmax (0, (int) std::floor (viewStart * sampleRate));
        const auto last = juce::jmin (numSamples - 1, (int) std::ceil ((viewStart + viewLength) * sampleRate));

        for (int i = first; i <= last; ++i)
        {
            const auto x = secondsToX (i / sampleRate);
            const auto y = mid - mono (i) * halfHeight;

            if (i == first)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }

        g.strokePath (path, juce::PathStrokeType (1.5f));
    }
    else
    {
        // Pick the coarsest cache level that is still at least as fine as one pixel.
        const LoadedSong::PeakLevel* level = nullptr;

        for (auto& candidate : song->peakLevels)
            if (candidate.samplesPerBucket <= samplesPerPixel)
                level = &candidate;

        for (int x = 0; x < width; ++x)
        {
            const auto s0 = (juce::int64) ((viewStart * sampleRate) + x * samplesPerPixel);
            const auto s1 = (juce::int64) ((viewStart * sampleRate) + (x + 1) * samplesPerPixel);

            if (s0 >= numSamples || s1 <= 0)
                continue;

            auto lo = 0.0f, hi = 0.0f;

            if (level != nullptr)
            {
                const auto b0 = (size_t) juce::jmax<juce::int64> (0, s0 / level->samplesPerBucket);
                const auto b1 = (size_t) juce::jmax<juce::int64> ((juce::int64) b0 + 1, s1 / level->samplesPerBucket);

                for (auto b = b0; b < b1 && b < level->mins.size(); ++b)
                {
                    lo = juce::jmin (lo, level->mins[b]);
                    hi = juce::jmax (hi, level->maxs[b]);
                }
            }
            else
            {
                for (auto i = juce::jmax<juce::int64> (0, s0); i < juce::jmin<juce::int64> (numSamples, juce::jmax (s1, s0 + 1)); ++i)
                {
                    lo = juce::jmin (lo, mono ((int) i));
                    hi = juce::jmax (hi, mono ((int) i));
                }
            }

            const auto y1 = mid - hi * halfHeight;
            const auto y2 = mid - lo * halfHeight;
            g.fillRect (juce::Rectangle<float> ((float) (area.getX() + x), y1, 1.0f, juce::jmax (1.0f, y2 - y1)));
        }
    }

    // Selection being dragged
    if (gesture == Gesture::selecting)
    {
        const auto x1 = juce::jmin (dragStartX, dragEndX);
        const auto x2 = juce::jmax (dragStartX, dragEndX);
        g.setColour (juce::Colours::white.withAlpha (0.2f));
        g.fillRect (juce::Rectangle<float> (x1, (float) area.getY(), x2 - x1, (float) area.getHeight()));
    }

    // Playhead
    g.setColour (juce::Colours::white);
    g.fillRect (juce::Rectangle<float> (secondsToX (player.getPositionSeconds()) - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight()));
}

void WaveformView::paintTimeRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (area);

    // Choose a tick spacing that leaves at least ~90 px between labels.
    static const double steps[] = { 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 };
    auto step = steps[std::size (steps) - 1];

    for (auto candidate : steps)
    {
        if (candidate / viewLength * area.getWidth() >= 90.0)
        {
            step = candidate;
            break;
        }
    }

    g.setFont (juce::FontOptions (12.0f));

    for (auto t = std::ceil (viewStart / step) * step; t <= viewStart + viewLength; t += step)
    {
        const auto x = secondsToX (t);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (x, (float) area.getBottom() - 5.0f, 1.0f, 5.0f));
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.drawText (formatRulerTime (t, step), juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 80.0f, (float) area.getHeight() - 2.0f),
                    juce::Justification::centredLeft, false);
    }
}

void WaveformView::paintOverview (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto total = song->getLengthSeconds();

    if (total <= 0.0 || song->peakLevels.empty())
        return;

    auto toX = [area, total] (double seconds) { return (float) area.getX() + (float) (seconds / total * area.getWidth()); };

    // Loop region
    if (player.hasLoop())
    {
        g.setColour (theme::accent.withAlpha (0.4f));
        const auto x1 = toX (player.getLoopStartSeconds());
        g.fillRect (juce::Rectangle<float> (x1, (float) area.getY(), toX (player.getLoopEndSeconds()) - x1, (float) area.getHeight()));
    }

    // Whole-song waveform from the coarsest cache level
    const auto& level = song->peakLevels.back();
    const auto buckets = level.maxs.size();
    const auto mid = (float) area.getCentreY();
    const auto halfHeight = area.getHeight() * 0.5f - 2.0f;
    g.setColour (waveformColour.withAlpha (0.6f));

    for (int x = 0; x < area.getWidth(); ++x)
    {
        const auto b0 = (size_t) x * buckets / (size_t) area.getWidth();
        const auto b1 = juce::jmax (b0 + 1, (size_t) (x + 1) * buckets / (size_t) area.getWidth());
        auto peak = 0.0f;

        for (auto b = b0; b < b1 && b < buckets; ++b)
            peak = juce::jmax (peak, juce::jmax (level.maxs[b], -level.mins[b]));

        g.fillRect (juce::Rectangle<float> ((float) (area.getX() + x), mid - peak * halfHeight, 1.0f, juce::jmax (1.0f, 2.0f * peak * halfHeight)));
    }

    // Visible window
    if (isZoomed())
    {
        const auto x1 = toX (viewStart);
        const auto x2 = juce::jmax (x1 + 3.0f, toX (viewStart + viewLength));
        auto window = juce::Rectangle<float> (x1, (float) area.getY(), x2 - x1, (float) area.getHeight());
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.fillRect (window);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawRect (window, 1.5f);
    }

    // Playhead
    g.setColour (juce::Colours::white);
    g.fillRect (juce::Rectangle<float> (toX (player.getPositionSeconds()) - 0.5f, (float) area.getY(), 1.5f, (float) area.getHeight()));
}

//==============================================================================
void WaveformView::startPinch()
{
    if (touches.size() < 2)
        return;

    auto it = touches.begin();
    const auto a = it->second;
    const auto b = (++it)->second;

    gesture = Gesture::pinching;
    pinchStartDistance = juce::jmax (10.0f, a.getDistanceFrom (b));
    pinchStartLength = viewLength;
    pinchAnchorSeconds = xToSeconds ((a.x + b.x) * 0.5f);
    repaint();
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (song == nullptr)
        return;

    touches[e.source.getIndex()] = e.position;

    if (touches.size() >= 2)
    {
        startPinch();   // a second finger cancels whatever the first one was doing
        return;
    }

    if (overviewArea.contains (e.getPosition()))
    {
        gesture = Gesture::overview;
        setView (e.position.x / juce::jmax (1, overviewArea.getWidth()) * song->getLengthSeconds() - viewLength * 0.5, viewLength);
        return;
    }

    dragStartX = dragEndX = e.position.x;
    gesture = Gesture::tapOrSelect;

    if (player.hasLoop())
    {
        const auto distanceToStart = std::abs (e.position.x - secondsToX (player.getLoopStartSeconds()));
        const auto distanceToEnd = std::abs (e.position.x - secondsToX (player.getLoopEndSeconds()));

        if (juce::jmin (distanceToStart, distanceToEnd) <= edgeGrabDistance)
            gesture = distanceToStart <= distanceToEnd ? Gesture::movingLoopStart : Gesture::movingLoopEnd;
    }
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (song == nullptr)
        return;

    touches[e.source.getIndex()] = e.position;

    switch (gesture)
    {
        case Gesture::pinching:
        {
            if (touches.size() < 2)
                break;

            auto it = touches.begin();
            const auto a = it->second;
            const auto b = (++it)->second;
            const auto distance = juce::jmax (10.0f, a.getDistanceFrom (b));
            const auto midX = (a.x + b.x) * 0.5f;
            const auto newLength = pinchStartLength * pinchStartDistance / distance;
            const auto proportion = (double) (midX - (float) mainArea.getX()) / juce::jmax (1, mainArea.getWidth());
            setView (pinchAnchorSeconds - proportion * newLength, newLength);
            break;
        }

        case Gesture::overview:
            setView (e.position.x / juce::jmax (1, overviewArea.getWidth()) * song->getLengthSeconds() - viewLength * 0.5, viewLength);
            break;

        case Gesture::movingLoopStart:
        case Gesture::movingLoopEnd:
        {
            const auto t = juce::jlimit (0.0, song->getLengthSeconds(), xToSeconds (e.position.x));
            auto start = player.getLoopStartSeconds();
            auto end = player.getLoopEndSeconds();

            if (gesture == Gesture::movingLoopStart)
                start = juce::jmin (t, end - 0.01);
            else
                end = juce::jmax (t, start + 0.01);

            if (onLoopEdited)
                onLoopEdited (start, end);

            repaint();
            break;
        }

        case Gesture::tapOrSelect:
        case Gesture::selecting:
            dragEndX = juce::jlimit ((float) mainArea.getX(), (float) mainArea.getRight(), e.position.x);

            if (std::abs (dragEndX - dragStartX) > 10.0f)
                gesture = Gesture::selecting;

            repaint();
            break;

        case Gesture::none:
        case Gesture::consumed:
            break;
    }
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    touches.erase (e.source.getIndex());

    if (song == nullptr)
        return;

    if (gesture == Gesture::pinching)
    {
        // Lifting one finger ends the pinch; ignore the other finger until it lifts too.
        gesture = touches.empty() ? Gesture::none : Gesture::consumed;
        return;
    }

    if (gesture == Gesture::selecting && onLoopSelected)
        onLoopSelected (xToSeconds (juce::jmin (dragStartX, dragEndX)), xToSeconds (juce::jmax (dragStartX, dragEndX)));
    else if (gesture == Gesture::tapOrSelect && onSeek)
        onSeek (juce::jlimit (0.0, song->getLengthSeconds(), xToSeconds (e.position.x)));

    if (touches.empty())
        gesture = Gesture::none;

    repaint();
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (song == nullptr)
        return;

    if (e.mods.isCommandDown())
    {
        zoomAround (std::pow (2.0, wheel.deltaY * 4.0), e.position.x);
    }
    else
    {
        // Horizontal scroll (trackpad) or vertical wheel both pan.
        const auto delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        setView (viewStart - delta * viewLength * 0.5, viewLength);
    }
}

void WaveformView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (song != nullptr && scaleFactor > 0.0f)
        zoomAround (scaleFactor, e.position.x);
}

} // namespace pv
