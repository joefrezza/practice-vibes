#include "Library.h"
#include "AppPaths.h"

namespace pv
{

//==============================================================================
ResourceKind classifyFileName (const juce::String& fileName)
{
    const auto ext = fileName.fromLastOccurrenceOf (".", false, false).toLowerCase();

    if (! fileName.containsChar ('.'))                                           return ResourceKind::other;
    if (ext == "pdf")                                                            return ResourceKind::pdf;
    if (juce::StringArray { "jpg", "jpeg", "png", "gif", "heic", "heif", "webp", "bmp", "tif", "tiff" }.contains (ext))
        return ResourceKind::image;
    if (juce::StringArray { "txt", "md", "rtf", "html", "htm", "gp", "gp5", "gpx", "tab", "chopro", "cho" }.contains (ext))
        return ResourceKind::text;
    if (juce::StringArray { "doc", "docx", "odt", "pages" }.contains (ext))     return ResourceKind::document;
    if (juce::StringArray { "xls", "xlsx", "ods", "numbers", "csv" }.contains (ext))
        return ResourceKind::spreadsheet;
    if (juce::StringArray { "ppt", "pptx", "odp", "key" }.contains (ext))       return ResourceKind::presentation;
    if (juce::StringArray { "wav", "aif", "aiff", "flac", "mp3", "m4a", "ogg", "aac" }.contains (ext))
        return ResourceKind::audio;
    if (juce::StringArray { "mp4", "mov", "m4v", "webm", "mkv", "avi" }.contains (ext))
        return ResourceKind::video;
    if (juce::StringArray { "gdoc", "gsheet", "gslides" }.contains (ext))       return ResourceKind::googleDoc;
    if (juce::StringArray { "url", "webloc" }.contains (ext))                   return ResourceKind::web;

    return ResourceKind::other;
}

ResourceKind classifyUrl (const juce::String& url)
{
    const auto lower = url.trim().toLowerCase();
    const auto host = juce::URL (lower).getDomain();

    if (host.endsWith ("youtube.com") || host.endsWith ("youtu.be") || host.endsWith ("youtube-nocookie.com"))
        return ResourceKind::youtube;

    if (host == "docs.google.com" || host == "drive.google.com")
        return ResourceKind::googleDoc;

    return ResourceKind::web;
}

ResourceKind kindOf (const Resource& r)
{
    return r.isLink() ? classifyUrl (r.url) : classifyFileName (r.fileName);
}

juce::StringArray parseTags (const juce::String& text)
{
    juce::StringArray result;

    for (auto tag : juce::StringArray::fromTokens (text, ",;", "\""))
    {
        tag = tag.trim();

        while (tag.startsWithChar ('#'))
            tag = tag.substring (1).trim();

        if (tag.isNotEmpty() && ! hasTag (result, tag))
            result.add (tag);
    }

    return result;
}

juce::String joinTags (const juce::StringArray& tags)
{
    return tags.joinIntoString (", ");
}

bool hasTag (const juce::StringArray& tags, const juce::String& tag)
{
    return tags.contains (tag.trim(), true);
}

juce::String badgeText (const Resource& r)
{
    switch (kindOf (r))
    {
        case ResourceKind::youtube:    return "YT";
        case ResourceKind::googleDoc:  return r.isLink() ? juce::String ("GDOC")
                                                         : r.fileName.fromLastOccurrenceOf (".", false, false).toUpperCase();
        case ResourceKind::web:        return "WEB";
        case ResourceKind::other:
            if (! r.fileName.containsChar ('.'))
                return "FILE";
            break;
        case ResourceKind::pdf: case ResourceKind::image: case ResourceKind::text: case ResourceKind::document:
        case ResourceKind::spreadsheet: case ResourceKind::presentation: case ResourceKind::audio: case ResourceKind::video:
            break;
    }

    auto ext = r.fileName.fromLastOccurrenceOf (".", false, false).toUpperCase();

    if (ext == "JPEG") ext = "JPG";
    if (ext == "AIFF") ext = "AIF";

    return ext.substring (0, 4);
}

//==============================================================================
Resource* Topic::findResource (const juce::String& resourceId)
{
    for (auto& r : resources)
        if (r.id == resourceId)
            return &r;

    return nullptr;
}

bool Topic::matches (const juce::String& searchText) const
{
    const auto text = searchText.trim();

    if (text.isEmpty())
        return true;

    auto tagsMatch = [&] (const juce::StringArray& list)
    {
        for (auto& t : list)
            if (t.containsIgnoreCase (text))
                return true;

        return false;
    };

    if (title.containsIgnoreCase (text) || description.containsIgnoreCase (text) || tagsMatch (tags))
        return true;

    for (auto& r : resources)
        if (r.title.containsIgnoreCase (text) || r.description.containsIgnoreCase (text) || tagsMatch (r.tags))
            return true;

    return false;
}

bool Topic::matchesTag (const juce::String& tag) const
{
    if (tag.isEmpty() || hasTag (tags, tag))
        return true;

    for (auto& r : resources)
        if (hasTag (r.tags, tag))
            return true;

    return false;
}

//==============================================================================
Library::Library (const juce::File& rootFolder) : root (rootFolder)
{
    root.createDirectory();
    rescan();
}

bool Library::isIgnoredFile (const juce::File& f)
{
    const auto name = f.getFileName();

    return name.startsWithChar ('.') || name.startsWithChar ('_') || name.startsWith ("~$")
        || name.equalsIgnoreCase ("desktop.ini") || name.equalsIgnoreCase ("Thumbs.db")
        || name.endsWithIgnoreCase (".tmp") || name.endsWithIgnoreCase (".partial")
        || name.endsWithIgnoreCase (".crdownload") || name.endsWithIgnoreCase (".icloud")
        || f.isHidden();
}

bool Library::isConflictCopy (const juce::String& fileName)
{
    // OneDrive: "topic-PCNAME.xml"; Dropbox: "topic (x's conflicted copy).xml"; Google Drive: "topic (1).xml".
    return fileName.startsWithIgnoreCase ("topic") && fileName.endsWithIgnoreCase (".xml")
        && ! fileName.equalsIgnoreCase (topicFileName);
}

juce::String Library::titleFromUrl (const juce::String& url)
{
    switch (classifyUrl (url))
    {
        case ResourceKind::youtube:    return "YouTube video";
        case ResourceKind::googleDoc:  return "Google document";
        case ResourceKind::pdf: case ResourceKind::image: case ResourceKind::text: case ResourceKind::document:
        case ResourceKind::spreadsheet: case ResourceKind::presentation: case ResourceKind::audio: case ResourceKind::video:
        case ResourceKind::web: case ResourceKind::other:
            break;
    }

    auto host = juce::URL (url.trim()).getDomain();

    if (host.startsWithIgnoreCase ("www."))
        host = host.substring (4);

    return host.isNotEmpty() ? host : url.trim();
}

//==============================================================================
bool Library::loadTopic (const juce::File& folder, Topic& topic)
{
    topic = {};
    topic.folder = folder;

    if (auto xml = juce::parseXMLIfTagMatches (folder.getChildFile (topicFileName), "TOPIC"))
    {
        topic.id = xml->getStringAttribute ("id");
        topic.title = xml->getStringAttribute ("title");
        topic.created = juce::Time::fromISO8601 (xml->getStringAttribute ("created"));
        topic.tags = parseTags (xml->getStringAttribute ("tags"));

        if (auto* d = xml->getChildByName ("DESCRIPTION"))
            topic.description = d->getAllSubText();

        for (auto* e : xml->getChildWithTagNameIterator ("RESOURCE"))
        {
            Resource r;
            r.fileName = e->getStringAttribute ("file");
            r.url = e->getStringAttribute ("url");
            r.id = r.fileName.isNotEmpty() ? r.fileName : e->getStringAttribute ("id");
            r.title = e->getStringAttribute ("title");
            r.added = juce::Time::fromISO8601 (e->getStringAttribute ("added"));
            r.tags = parseTags (e->getStringAttribute ("tags"));

            if (auto* d = e->getChildByName ("DESCRIPTION"))
                r.description = d->getAllSubText();

            if (r.id.isEmpty() || (r.fileName.isEmpty() && r.url.isEmpty()) || topic.findResource (r.id) != nullptr)
                continue;

            if (r.fileName.isNotEmpty() && (r.fileName.containsAnyOf ("/\\") || r.fileName == ".."))
                continue;   // never let topic.xml point outside its folder

            topic.resources.push_back (std::move (r));
        }
    }

    if (topic.id.isEmpty())
        topic.id = "dir:" + folder.getFileName();

    if (topic.title.isEmpty())
        topic.title = folder.getFileName();

    if (topic.created == juce::Time())
        topic.created = folder.getCreationTime();

    // Files that topic.xml doesn't list yet (added from another device, by hand, or still syncing).
    auto files = folder.findChildFiles (juce::File::findFiles, false);
    files.sort();

    for (auto& f : files)
    {
        const auto name = f.getFileName();

        if (name.equalsIgnoreCase (topicFileName) || isIgnoredFile (f))
            continue;

        if (isConflictCopy (name))
        {
            topic.conflictFiles.add (name);
            continue;
        }

        if (topic.findResource (name) == nullptr)
        {
            Resource r;
            r.id = r.fileName = name;
            r.title = f.getFileNameWithoutExtension();
            r.added = f.getCreationTime();
            topic.resources.push_back (std::move (r));
        }
    }

    return true;
}

void Library::rescan()
{
    topics.clear();
    inbox.clear();

    if (! root.isDirectory())
        return;

    for (auto& entry : juce::RangedDirectoryIterator (root, false, "*", juce::File::findFilesAndDirectories))
    {
        const auto& f = entry.getFile();

        if (isIgnoredFile (f))
            continue;

        if (entry.isDirectory())
        {
            Topic t;
            loadTopic (f, t);
            topics.push_back (std::move (t));
        }
        else
        {
            inbox.add (f);
        }
    }

    inbox.sort();
    sortTopics();
}

void Library::sortTopics()
{
    std::stable_sort (topics.begin(), topics.end(), [] (const Topic& a, const Topic& b)
    {
        if (a.created != b.created)
            return a.created > b.created;

        return a.title.compareNatural (b.title) < 0;
    });
}

juce::String Library::computeSignature() const
{
    juce::String sig;

    if (! root.isDirectory())
        return sig;

    for (auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFilesAndDirectories))
        sig << entry.getFile().getRelativePathFrom (root) << '|' << entry.getFileSize() << '|'
            << entry.getModificationTime().toMilliseconds() << '\n';

    return juce::String::toHexString (sig.hashCode64()) + ":" + juce::String (sig.length());
}

juce::StringArray Library::getAllTags() const
{
    juce::StringArray all;

    for (auto& t : topics)
    {
        for (auto& tag : t.tags)
            if (! hasTag (all, tag))
                all.add (tag);

        for (auto& r : t.resources)
            for (auto& tag : r.tags)
                if (! hasTag (all, tag))
                    all.add (tag);
    }

    all.sortNatural();
    return all;
}

Topic* Library::findTopic (const juce::String& topicId)
{
    for (auto& t : topics)
        if (t.id == topicId)
            return &t;

    return nullptr;
}

juce::File Library::uniqueChild (const juce::File& folder, const juce::String& fileName) const
{
    auto name = juce::File::createLegalFileName (fileName).trim();

    if (name.isEmpty() || name.startsWithChar ('.') || name.startsWithChar ('_'))
        name = "File" + name;

    auto file = folder.getChildFile (name);
    const auto stem = file.getFileNameWithoutExtension(), ext = file.getFileExtension();

    // "name (2).pdf", "name (3).pdf"... like Explorer / Finder.
    for (int n = 2; file.exists() && n < 10000; ++n)
        file = folder.getChildFile (stem + " (" + juce::String (n) + ")" + ext);

    return file;
}

Topic* Library::createTopic (const juce::String& title)
{
    auto cleanTitle = title.trim();

    if (cleanTitle.isEmpty())
        cleanTitle = "New topic";

    auto folderName = juce::File::createLegalFileName (cleanTitle).trim().substring (0, 60).trim();

    if (folderName.isEmpty() || folderName.startsWithChar ('.') || folderName.startsWithChar ('_'))
        folderName = "Topic " + folderName;

    auto folder = uniqueChild (root, folderName);

    if (! folder.createDirectory())
        return nullptr;

    Topic t;
    t.id = juce::Uuid().toDashedString();
    t.title = cleanTitle;
    t.created = juce::Time::getCurrentTime();
    t.folder = folder;

    if (! saveTopic (t))
        return nullptr;

    const auto id = t.id;
    topics.push_back (std::move (t));
    sortTopics();
    return findTopic (id);
}

bool Library::saveTopic (const Topic& t)
{
    juce::XmlElement xml ("TOPIC");
    xml.setAttribute ("id", t.id);
    xml.setAttribute ("title", t.title);
    xml.setAttribute ("created", t.created.toISO8601 (true));

    if (! t.tags.isEmpty())
        xml.setAttribute ("tags", joinTags (t.tags));

    xml.createNewChildElement ("DESCRIPTION")->addTextElement (t.description);

    for (auto& r : t.resources)
    {
        auto* e = xml.createNewChildElement ("RESOURCE");

        if (r.isLink())
        {
            e->setAttribute ("id", r.id);
            e->setAttribute ("url", r.url);
        }
        else
        {
            e->setAttribute ("file", r.fileName);
        }

        e->setAttribute ("title", r.title);
        e->setAttribute ("added", r.added.toISO8601 (true));

        if (! r.tags.isEmpty())
            e->setAttribute ("tags", joinTags (r.tags));

        if (r.description.isNotEmpty())
            e->createNewChildElement ("DESCRIPTION")->addTextElement (r.description);
    }

    t.folder.createDirectory();
    return xml.writeTo (t.folder.getChildFile (topicFileName));
}

static bool removeToBin (const juce::File& f)
{
    if (! f.exists())
        return true;

   #if ! JUCE_IOS
    if (f.moveToTrash())
        return true;
   #endif

    return f.isDirectory() ? f.deleteRecursively() : f.deleteFile();
}

bool Library::deleteTopic (const juce::String& topicId)
{
    auto it = std::find_if (topics.begin(), topics.end(), [&] (const Topic& t) { return t.id == topicId; });

    if (it == topics.end() || ! removeToBin (it->folder))
        return false;

    topics.erase (it);
    return true;
}

Resource* Library::addFile (Topic& topic, const juce::URL& source)
{
    const auto sourceName = source.isLocalFile() ? source.getLocalFile().getFileName()
                                                 : juce::URL::removeEscapeChars (source.getFileName());
    auto target = uniqueChild (topic.folder, sourceName);
    topic.folder.createDirectory();

    bool copied = false;

    if (source.isLocalFile())
    {
        copied = source.getLocalFile().copyFileTo (target);
    }
    else if (auto in = source.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)))
    {
        juce::FileOutputStream out (target);
        copied = out.openedOk() && out.writeFromInputStream (*in, -1) >= 0;
        out.flush();
        copied = copied && out.getStatus().wasOk();
    }

    if (! copied)
    {
        target.deleteFile();
        return nullptr;
    }

    Resource r;
    r.id = r.fileName = target.getFileName();
    r.title = target.getFileNameWithoutExtension();
    r.added = juce::Time::getCurrentTime();
    topic.resources.push_back (r);
    saveTopic (topic);
    return topic.findResource (r.id);
}

Resource* Library::moveInboxFile (Topic& topic, const juce::File& inboxFile)
{
    if (! inbox.contains (inboxFile) || ! inboxFile.existsAsFile())
        return nullptr;

    auto target = uniqueChild (topic.folder, inboxFile.getFileName());
    topic.folder.createDirectory();

    if (! inboxFile.moveFileTo (target))
        return nullptr;

    inbox.removeFirstMatchingValue (inboxFile);

    Resource r;
    r.id = r.fileName = target.getFileName();
    r.title = target.getFileNameWithoutExtension();
    r.added = juce::Time::getCurrentTime();
    topic.resources.push_back (r);
    saveTopic (topic);
    return topic.findResource (r.id);
}

Resource* Library::addLink (Topic& topic, const juce::String& title, const juce::String& url)
{
    auto cleanUrl = url.trim();

    if (cleanUrl.isEmpty())
        return nullptr;

    if (! cleanUrl.containsChar (':'))
        cleanUrl = "https://" + cleanUrl;

    Resource r;
    r.id = juce::Uuid().toDashedString();
    r.url = cleanUrl;
    r.title = title.trim().isNotEmpty() ? title.trim() : titleFromUrl (cleanUrl);
    r.added = juce::Time::getCurrentTime();
    topic.resources.push_back (r);
    saveTopic (topic);
    return topic.findResource (r.id);
}

bool Library::removeResource (Topic& topic, const juce::String& resourceId)
{
    auto it = std::find_if (topic.resources.begin(), topic.resources.end(),
                            [&] (const Resource& r) { return r.id == resourceId; });

    if (it == topic.resources.end())
        return false;

    if (! it->isLink() && ! removeToBin (topic.getFile (*it)))
        return false;

    topic.resources.erase (it);
    return saveTopic (topic);
}

bool Library::moveResource (Topic& topic, const juce::String& resourceId, int delta)
{
    auto it = std::find_if (topic.resources.begin(), topic.resources.end(),
                            [&] (const Resource& r) { return r.id == resourceId; });

    if (it == topic.resources.end())
        return false;

    const auto from = (int) std::distance (topic.resources.begin(), it);
    const auto to = juce::jlimit (0, (int) topic.resources.size() - 1, from + delta);

    if (from == to)
        return false;

    auto moved = std::move (topic.resources[(size_t) from]);
    topic.resources.erase (topic.resources.begin() + from);
    topic.resources.insert (topic.resources.begin() + to, std::move (moved));
    return saveTopic (topic);
}

//==============================================================================
juce::File LibraryLocation::getDefaultFolder()
{
   #if JUCE_IOS
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Library");
   #else
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Practice Vibes").getChildFile ("Library");
   #endif
}

juce::File LibraryLocation::load()
{
    if (auto xml = juce::parseXMLIfTagMatches (getLibrarySettingsFile(), "LIBRARY"))
    {
        // The default folder is stored by name, not path: the iOS app container moves between installs.
        if (xml->getBoolAttribute ("default"))
            return getDefaultFolder();

        const auto path = xml->getStringAttribute ("folder");

        if (juce::File::isAbsolutePath (path))
            return juce::File (path);
    }

    return {};
}

void LibraryLocation::save (const juce::File& folder)
{
    juce::XmlElement xml ("LIBRARY");

    if (folder == getDefaultFolder())
        xml.setAttribute ("default", true);
    else
        xml.setAttribute ("folder", folder.getFullPathName());

    xml.writeTo (getLibrarySettingsFile());
}

} // namespace pv
