#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Library.h"

namespace juce { class WebBrowserComponent; }

namespace pv
{

/** YouTube's official embedded player, hosted by the app's helper page on GitHub Pages (a real https origin:
    YouTube refuses embeds from app-internal origins with error 153 - especially on iOS).

    The page reports its state every 100 ms and runs the A-B loop itself; the app sends commands with JavaScript.
    The web view is a native window that always draws above JUCE components, so MainComponent never reparents it
    (that could reload the page): it only moves it - docked over the Practice tab's video area, or floating
    ("compact": YouTube's minimum player size plus a small title bar you can drag).
*/
class YouTubePlayer final : public juce::Component
{
public:
    /** Where the helper page lives (one place to change if it ever moves to another host). */
    static constexpr const char* pageUrl = "https://joefrezza.github.io/practice-vibes/youtube/";

    /** Floating player size: 16:9 video at least 200 px high (YouTube's minimum) plus the title bar. */
    static constexpr int compactVideoWidth = 384, compactVideoHeight = 216, compactBarHeight = 34;

    YouTubePlayer();
    ~YouTubePlayer() override;

    void load (const YouTubeLink&, const juce::String& title);
    void unload();
    bool hasVideo() const noexcept               { return videoId.isNotEmpty(); }

    void play();
    void pause();
    void togglePlay()                            { if (state.playing) pause(); else play(); }
    void seek (double seconds);
    void setRate (double rate);                  // snapped to the rates the video offers
    double getRequestedRate() const noexcept     { return pendingRate; }
    void setVolume (double gain);                // 0..1
    void setLoop (double start, double end, bool enabled);

    struct State
    {
        bool ready = false, playing = false, buffering = false, ended = false;
        double time = 0.0, duration = 0.0, rate = 1.0;
        juce::Array<double> rates;
        juce::String title;
        int errorCode = 0;
    };

    const State& getState() const noexcept       { return state; }
    juce::String getTitle() const                { return state.title.isNotEmpty() ? state.title : requestedTitle; }
    juce::String getErrorText() const;
    juce::String getWatchUrl() const             { return "https://www.youtube.com/watch?v=" + videoId; }

    double getLoopStart() const noexcept         { return loopStart; }
    double getLoopEnd() const noexcept           { return loopEnd; }
    bool hasLoop() const noexcept                { return loopEnd > loopStart; }
    bool isLoopEnabled() const noexcept          { return loopEnabled && hasLoop(); }

    void setCompact (bool);
    bool isCompact() const noexcept              { return compact; }

    std::function<void()> onExpand, onClose, onStateChanged;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    struct Browser;

    void handleMessage (const juce::var&);
    void run (const juce::String& script);
    void sendLoop();

    std::unique_ptr<Browser> browser;   // created on the first load (no web view process until a video is opened)
    juce::String videoId, requestedTitle;
    bool pageReady = false, compact = false;
    State state;
    double loopStart = 0.0, loopEnd = 0.0;
    bool loopEnabled = false;
    double pendingRate = 1.0, pendingVolume = 1.0;
    juce::uint32 lastRateSend = 0;

    juce::TextButton playButton { "Play" }, expandButton { "Expand" }, closeButton { "X" };
    juce::ComponentDragger dragger;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YouTubePlayer)
};

} // namespace pv
