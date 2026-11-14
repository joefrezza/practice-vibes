#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

namespace pv
{

/** What a resource is, decided from its file extension or URL. Drives the icon and what "Open" does. */
enum class ResourceKind
{
    pdf, image, text, document, spreadsheet, presentation, audio, video,
    youtube, googleDoc, web, other
};

/** One item attached to a topic: a file inside the topic's folder, or a web link. */
struct Resource
{
    juce::String id;            // file resources: the file name (same on every device); links: a UUID
    juce::String title;
    juce::String description;
    juce::String fileName;      // file inside the topic folder; empty for links
    juce::String url;           // links only
    juce::StringArray tags;
    juce::Time added;

    bool isLink() const noexcept    { return fileName.isEmpty(); }
};

ResourceKind classifyFileName (const juce::String& fileName);
ResourceKind classifyUrl (const juce::String& url);
ResourceKind kindOf (const Resource&);

/** A YouTube video reference taken from any usual link form. */
struct YouTubeLink
{
    juce::String videoId;
    double startSeconds = 0.0;
};

/** watch?v=, youtu.be/, shorts/, embed/, live/, m. and music. hosts; start from t= / start= / #t= ("90", "1m30s"). */
std::optional<YouTubeLink> parseYouTubeUrl (const juce::String& url);

/** Splits "jazz, #technique , Jazz" into { "jazz", "technique" }: trimmed, no '#', no empties, no duplicates (ignoring case). */
juce::StringArray parseTags (const juce::String& text);
juce::String joinTags (const juce::StringArray& tags);
bool hasTag (const juce::StringArray& tags, const juce::String& tag);

/** Short text for the resource's badge, e.g. "PDF", "MP3", "YT", "WEB". */
juce::String badgeText (const Resource&);

/** A practice topic: a folder in the library holding topic.xml and the topic's files. */
struct Topic
{
    juce::String id;            // UUID, or "dir:<folder name>" for folders made outside the app
    juce::Time created;
    juce::String title;
    juce::String description;
    juce::StringArray tags;
    std::vector<Resource> resources;

    juce::File folder;
    juce::StringArray conflictFiles;   // copies of topic.xml left by a cloud sync conflict

    juce::File getFile (const Resource& r) const    { return r.isLink() ? juce::File() : folder.getChildFile (r.fileName); }
    bool isMissing (const Resource& r) const        { return ! r.isLink() && ! getFile (r).existsAsFile(); }
    Resource* findResource (const juce::String& resourceId);
    bool matches (const juce::String& searchText) const;

    /** True if the topic or any of its resources carries the tag (empty tag = always). */
    bool matchesTag (const juce::String& tag) const;
};

/** The user's library folder: one sub-folder per topic, plus loose files (the Inbox).

    Everything lives in plain files so the folder can sit in a cloud drive and be shared between devices:
    - <topic folder>/topic.xml holds the title, description and the resource list;
    - files in a topic folder that topic.xml doesn't list (e.g. dropped in from another device)
      still show up as resources, titled with their file name;
    - files at the top level are the Inbox, waiting to be moved into a topic;
    - folders whose name starts with "." or "_" are ignored (reserved for the app's own data).
*/
class Library
{
public:
    explicit Library (const juce::File& rootFolder);

    const juce::File& getRoot() const noexcept           { return root; }

    /** Re-reads every topic and the Inbox from disk. */
    void rescan();

    /** Cheap fingerprint of the folder (names, sizes, modification times): changes when anything on disk does. */
    juce::String computeSignature() const;

    const std::vector<Topic>& getTopics() const noexcept { return topics; }   // newest first
    Topic* findTopic (const juce::String& topicId);
    const juce::Array<juce::File>& getInbox() const noexcept { return inbox; }

    /** Every tag used by a topic or resource, sorted, with the first spelling found. */
    juce::StringArray getAllTags() const;

    Topic* createTopic (const juce::String& title);
    bool saveTopic (const Topic&);
    bool deleteTopic (const juce::String& topicId);

    /** Copies a file (or reads a URL, e.g. from the iOS document picker) into the topic's folder. */
    Resource* addFile (Topic&, const juce::URL& source);

    /** Moves a file from the Inbox into the topic. */
    Resource* moveInboxFile (Topic&, const juce::File& inboxFile);

    Resource* addLink (Topic&, const juce::String& title, const juce::String& url);

    /** Removes a resource; a file resource's file goes to the bin (or is deleted where there's no bin). */
    bool removeResource (Topic&, const juce::String& resourceId);

    bool moveResource (Topic&, const juce::String& resourceId, int delta);

    static bool isIgnoredFile (const juce::File&);
    static bool isConflictCopy (const juce::String& fileName);
    static juce::String titleFromUrl (const juce::String& url);

    static constexpr const char* topicFileName = "topic.xml";

private:
    static bool loadTopic (const juce::File& folder, Topic&);
    void sortTopics();
    juce::File uniqueChild (const juce::File& folder, const juce::String& fileName) const;

    juce::File root;
    std::vector<Topic> topics;
    juce::Array<juce::File> inbox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Library)
};

/** Where the library lives on this device (a per-device setting, kept outside the library). */
struct LibraryLocation
{
    static juce::File getDefaultFolder();
    static juce::File load();                 // empty File if the user hasn't chosen yet
    static void save (const juce::File& folder);
};

} // namespace pv
