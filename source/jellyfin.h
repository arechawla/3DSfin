#pragma once
#include "http.h"
#include <string>
#include <vector>

struct JellyfinLibrary {
    std::string id;
    std::string name;
    std::string collectionType; // "movies", "tvshows", "music", etc.
};

struct JellyfinItem {
    std::string id;
    std::string name;           // movie title / episode name / season name
    std::string type;           // "Movie", "Episode", "Series", "Season"
    std::string seriesName;     // parent series (episodes only)
    long long   runTimeTicks;   // 10,000,000 ticks per second
    int         productionYear;
    long long   resumeTicks;    // saved playback position (0 = start), from UserData
};

// Which children to enumerate beneath a parent when browsing.
enum class ChildKind {
    Direct,             // a library's direct children: Movies or Series, by name
    Seasons,            // a series' seasons, ordered by season number
    Episodes,           // a season's episodes, ordered by episode number
    EpisodesRecursive,  // every episode beneath a series, flattened (seasonless fallback)
};

class JellyfinClient {
public:
    JellyfinClient();

    // Returns false if the server is unreachable
    bool connect(const std::string& serverUrl);

    // Returns false on bad credentials
    bool authenticate(const std::string& username, const std::string& password);

    std::vector<JellyfinLibrary> getLibraries();

    // Lists a parent's children according to kind: a library's movies/series,
    // a series' seasons, a season's episodes, or (fallback) every episode beneath
    // a series flattened in season/episode order. limit caps the page size.
    std::vector<JellyfinItem> getChildren(const std::string& parentId,
                                          ChildKind kind  = ChildKind::Direct,
                                          int       limit = 200);

    // In-progress items across all libraries ("Continue Watching"), newest first.
    // Each item's resumeTicks holds the saved playback position.
    std::vector<JellyfinItem> getResumeItems(int limit = 12);

    // Returns a direct-stream URL pre-configured for 3DS capabilities.
    // startTicks seeks the transcode to a resume position (0 = from the start).
    // Each call embeds a fresh PlaySessionId: without one, Jellyfin matches the
    // request to the still-running transcode of the previous stream and ignores
    // the new StartTimeTicks — which made seeking a no-op.
    std::string getStreamUrl(const std::string& itemId,
                             long long startTicks = 0);

    // Tells the server to kill the transcode job of the last getStreamUrl()
    // stream. Call between seeks (and after playback) so orphaned ffmpeg jobs
    // don't pile up server-side. Safe to call when nothing is active.
    void stopTranscode();

    // Fetches the raw Primary-image bytes (JPEG) for an item/library, scaled to
    // fillWidth px. Returns an empty string if the item has no image or on error.
    std::string getPrimaryImage(const std::string& itemId, int fillWidth);

    bool        isAuthenticated() const { return !accessToken_.empty(); }
    std::string serverUrl()       const { return serverUrl_; }
    int         lastStatus()      const { return lastStatus_; }

private:
    int         lastStatus_ = 0;
    std::string serverUrl_;
    std::string userId_;
    std::string accessToken_;
    std::string deviceId_;
    std::string lastPlaySessionId_;   // session of the last getStreamUrl() call
    HttpClient  http_;

    void applyAuthHeader();
};
