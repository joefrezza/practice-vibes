#include "YouTubePlayer.h"
#include "Theme.h"
#include "WebView.h"

namespace pv
{

namespace
{
    /** Hosts the player page and YouTube itself may load; anything else opens in the system browser. */
    bool isAllowedInPlayer (const juce::String& url)
    {
        const auto lower = url.trim().toLowerCase();

        if (! lower.startsWith ("http"))
            return true;   // about:blank, data:, blob: ... used inside the player

        if (lower.startsWith (juce::String (YouTubePlayer::pageUrl).toLowerCase()))
            return true;

        const auto host = lower.fromFirstOccurrenceOf ("://", false, false).upToFirstOccurrenceOf ("/", false, false)
                               .upToFirstOccurrenceOf ("?", false, false);

        for (auto* allowed : { "youtube.com", "youtube-nocookie.com", "youtu.be", "ytimg.com", "googlevideo.com", "google.com",
                               "gstatic.com", "googleapis.com", "doubleclick.net", "googlesyndication.com" })
            if (host == allowed || host.endsWith (juce::String (".") + allowed))
                return true;

        return false;
    }
}

//==============================================================================
struct YouTubePlayer::Browser final : public juce::WebBrowserComponent
{
    explicit Browser (YouTubePlayer& o)
        : juce::WebBrowserComponent (webViewOptions()
                                        .withNativeIntegrationEnabled()
                                        .withEventListener ("pvYouTube", [&o] (const juce::var& message) { o.handleMessage (message); })),
          owner (o)
    {
    }

    bool pageAboutToLoad (const juce::String& url) override
    {
        if (isAllowedInPlayer (url))
            return true;

        juce::URL (url).launchInDefaultBrowser();
        return false;
    }

    void newWindowAttemptingToLoad (const juce::String& url) override
    {
        // e.g. the YouTube logo or the video title: the user asked to go to YouTube.
        juce::URL (url).launchInDefaultBrowser();
    }

    YouTubePlayer& owner;
};

//==============================================================================
YouTubePlayer::YouTubePlayer()
{
    playButton.onClick = [this] { togglePlay(); };
    expandButton.onClick = [this] { if (onExpand) onExpand(); };
    closeButton.onClick = [this] { if (onClose) onClose(); };
    expandButton.setTooltip ("Back to the Practice tab");
    closeButton.setTooltip ("Close the video");

    for (auto* b : { &playButton, &expandButton, &closeButton })
        addChildComponent (b);

    setOpaque (true);
}

YouTubePlayer::~YouTubePlayer() = default;

//==============================================================================
void YouTubePlayer::load (const YouTubeLink& link, const juce::String& title)
{
    videoId = link.videoId;
    requestedTitle = title;
    state = {};
    loopStart = loopEnd = 0.0;
    loopEnabled = false;

    const auto start = juce::jmax (0, (int) link.startSeconds);

    if (browser != nullptr && pageReady)
    {
        run ("pv.load('" + videoId + "', " + juce::String (start) + ");");
        run ("pv.loop(0, 0, false);");
        return;
    }

    if (browser == nullptr)
    {
        browser = std::make_unique<Browser> (*this);
        addAndMakeVisible (*browser);
        resized();
    }

    pageReady = false;
    browser->goToURL (juce::String (pageUrl) + "?v=" + videoId + "&start=" + juce::String (start));
}

void YouTubePlayer::unload()
{
    if (browser != nullptr && pageReady)
        run ("pv.pause();");

    videoId = {};
    requestedTitle = {};
    state = {};
    loopStart = loopEnd = 0.0;
    loopEnabled = false;

    if (onStateChanged)
        onStateChanged();
}

void YouTubePlayer::run (const juce::String& script)
{
    if (browser != nullptr)
        browser->evaluateJavascript ("window.pv && " + script);
}

void YouTubePlayer::play()       { run ("pv.play();"); }
void YouTubePlayer::pause()      { run ("pv.pause();"); }

void YouTubePlayer::seek (double seconds)
{
    run ("pv.seek(" + juce::String (juce::jmax (0.0, seconds), 3) + ");");
    state.time = seconds;   // shown right away; the page confirms within 100 ms
}

void YouTubePlayer::setRate (double rate)
{
    if (! state.rates.isEmpty())
    {
        auto best = state.rates.getFirst();

        for (auto r : state.rates)
            if (std::abs (r - rate) < std::abs (best - rate))
                best = r;

        rate = best;
    }

    pendingRate = rate;
    lastRateSend = juce::Time::getMillisecondCounter();
    run ("pv.rate(" + juce::String (rate, 2) + ");");
}

void YouTubePlayer::setVolume (double gain)
{
    pendingVolume = juce::jlimit (0.0, 1.0, gain);
    run ("pv.volume(" + juce::String (pendingVolume * 100.0, 1) + ");");
}

void YouTubePlayer::setLoop (double start, double end, bool enabled)
{
    loopStart = juce::jmax (0.0, start);
    loopEnd = juce::jmax (loopStart, end);
    loopEnabled = enabled;
    sendLoop();
}

void YouTubePlayer::sendLoop()
{
    run ("pv.loop(" + juce::String (loopStart, 3) + ", " + juce::String (loopEnd, 3) + ", "
         + (isLoopEnabled() ? "true" : "false") + ");");
}

//==============================================================================
void YouTubePlayer::handleMessage (const juce::var& message)
{
    const auto type = message["type"].toString();

    if (type == "ready")
    {
        pageReady = true;
        state.ready = true;
        state.errorCode = 0;

        // Settings made before the player was ready.
        run ("pv.volume(" + juce::String (pendingVolume * 100.0, 1) + ");");

        if (pendingRate != 1.0)
            run ("pv.rate(" + juce::String (pendingRate, 2) + ");");

        sendLoop();
    }
    else if (type == "state")
    {
        if (videoId.isEmpty())
            return;   // closed: ignore the last reports of the old video

        state.ready = true;
        state.time = (double) message["time"];
        state.duration = (double) message["duration"];
        state.rate = (double) message["rate"];

        const auto playerState = (int) message["playerState"];
        state.playing = playerState == 1;
        state.buffering = playerState == 3;
        state.ended = playerState == 0;

        if (auto* rates = message["rates"].getArray())
        {
            state.rates.clearQuick();

            for (auto& r : *rates)
                state.rates.add ((double) r);
        }

        // YouTube ignores rate changes during ads and sometimes after loading: insist until it takes.
        if (state.playing && std::abs (state.rate - pendingRate) > 0.001
            && juce::Time::getMillisecondCounter() - lastRateSend > 1000)
        {
            lastRateSend = juce::Time::getMillisecondCounter();
            run ("pv.rate(" + juce::String (pendingRate, 2) + ");");
        }

        const auto title = message["title"].toString();

        if (title.isNotEmpty())
            state.title = title;
    }
    else if (type == "error")
    {
        state.errorCode = (int) message["code"];
        juce::Logger::writeToLog ("YouTube player error " + juce::String (state.errorCode) + " for video " + videoId);
    }

    if (compact)
    {
        playButton.setButtonText (state.playing ? "Pause" : "Play");
        repaint();
    }

    if (onStateChanged)
        onStateChanged();
}

juce::String YouTubePlayer::getErrorText() const
{
    switch (state.errorCode)
    {
        case 0:   return {};
        case 2:   return "This doesn't look like a valid YouTube video link.";
        case 5:   return "The player couldn't play this video.";
        case 100: return "This video was removed or is private.";
        case 101:
        case 150: return "The owner of this video doesn't allow playing it in other apps.";
        case 153: return "YouTube refused the player (missing page origin).";
        default:  return "YouTube player error " + juce::String (state.errorCode) + ".";
    }
}

//==============================================================================
void YouTubePlayer::setCompact (bool shouldBeCompact)
{
    compact = shouldBeCompact;

    for (auto* b : { &playButton, &expandButton, &closeButton })
        b->setVisible (compact);

    playButton.setButtonText (state.playing ? "Pause" : "Play");
    resized();
    repaint();
}

void YouTubePlayer::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    if (compact)
    {
        auto bar = getLocalBounds().removeFromTop (compactBarHeight);
        g.setColour (theme::panel.brighter (0.1f));
        g.fillRect (bar);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (13.5f).withStyle ("Bold"));
        auto text = bar.reduced (10, 0);
        text.removeFromRight (closeButton.getRight() - playButton.getX() + 8);
        g.drawText (getTitle(), text, juce::Justification::centredLeft, true);
        g.setColour (theme::accent);
        g.drawRect (getLocalBounds(), 1);
    }
}

void YouTubePlayer::resized()
{
    auto area = getLocalBounds();

    if (compact)
    {
        auto bar = area.removeFromTop (compactBarHeight).reduced (4, 3);
        closeButton.setBounds (bar.removeFromRight (34));
        bar.removeFromRight (4);
        expandButton.setBounds (bar.removeFromRight (70));
        bar.removeFromRight (4);
        playButton.setBounds (bar.removeFromRight (64));
        area = area.reduced (1, 0).withTrimmedBottom (1);
    }

    if (browser != nullptr)
        browser->setBounds (area);
}

void YouTubePlayer::mouseDown (const juce::MouseEvent& e)
{
    if (compact && e.y < compactBarHeight)
        dragger.startDraggingComponent (this, e);
}

void YouTubePlayer::mouseDrag (const juce::MouseEvent& e)
{
    if (! compact)
        return;

    juce::ComponentBoundsConstrainer keepOnScreen;
    keepOnScreen.setMinimumOnscreenAmounts (compactBarHeight, 120, compactBarHeight, 120);
    dragger.dragComponent (this, e, &keepOnScreen);
}

} // namespace pv
