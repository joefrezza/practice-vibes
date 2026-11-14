#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Library.h"

namespace pv
{

/** The "Topics" tab: practice topics (title, notes, resources) stored in the user's library folder.

    The folder may sit in a cloud drive shared with other devices: the panel watches it and picks up
    changes made elsewhere every few seconds while the tab is showing.
*/
class LibraryPanel final : public juce::Component,
                           private juce::ListBoxModel,
                           private juce::Timer
{
public:
    LibraryPanel();
    ~LibraryPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    std::function<void (const juce::String&, std::unique_ptr<juce::Component>)> showOverlay;
    std::function<void()> hideOverlay;
    std::function<void (const juce::File&)> onOpenAudio;

    /** Writes any pending edit to disk (also called when the app quits). */
    void flush();

private:
    class TopicView;
    class InboxView;
    class ResourceRow;
    class ResourceEditor;
    class Viewer;
    friend class TopicView;
    friend class InboxView;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    void timerCallback() override;

    void openLibrary (const juce::File& folder);
    void chooseFolder();
    void showFolderMenu();
    void rebuildList (const juce::String& idToSelect);
    void showSelection();
    void refreshAfterRescan();

    void newTopic();
    void deleteTopic (const juce::String& topicId);
    void editedTopic();                        // a title/description keystroke: save soon
    void addFiles (const juce::String& topicId);
    void addUrls (const juce::String& topicId, const juce::Array<juce::URL>&);
    void showTagMenu();
    void updateTagButton();
    void editResource (const juce::String& topicId, const juce::String& resourceId, bool isNewLink);
    void openResource (const Topic&, const Resource&);
    void openFile (const juce::File&, const juce::String& title);
    void showResourceMenu (const juce::String& topicId, const juce::String& resourceId, juce::Component& anchor);
    void showInboxMenu (const juce::File&, juce::Component& anchor);
    void showError (const juce::String& message);

    std::unique_ptr<Library> library;
    juce::String signature;
    juce::String selectedId;                   // topic id, "inbox" or empty
    juce::String tagFilter;                    // show only topics with this tag (on the topic or a resource)
    std::vector<juce::String> rowIds;
    bool dirty = false, rebuilding = false;
    juce::uint32 lastEdit = 0, lastPoll = 0;

    // Shown until the user has picked a library folder.
    juce::Label setupLabel;
    juce::TextButton useDefaultButton, chooseFolderButton { "Choose a folder..." };

    juce::TextEditor searchBox;
    juce::TextButton newTopicButton { "+ New topic" }, folderButton { "Library folder..." }, tagsButton { "Tags" };
    juce::ListBox topicList { "Topics", this };
    juce::Label emptyLabel;

    std::unique_ptr<TopicView> topicView;
    std::unique_ptr<InboxView> inboxView;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryPanel)
};

} // namespace pv
