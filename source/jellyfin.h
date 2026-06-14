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
    std::string name;
    std::string type;           // "Movie", "Episode", "Series"
    long long   runTimeTicks;   // 10,000,000 ticks per second
    int         productionYear;
};

class JellyfinClient {
public:
    JellyfinClient();

    // Returns false if the server is unreachable
    bool connect(const std::string& serverUrl);

    // Returns false on bad credentials
    bool authenticate(const std::string& username, const std::string& password);

    std::vector<JellyfinLibrary> getLibraries();

    // parentId = library Id; limit = items per page
    std::vector<JellyfinItem> getItems(const std::string& parentId,
                                       int startIndex = 0,
                                       int limit      = 50);

    // Returns a direct-stream URL pre-configured for 3DS capabilities
    std::string getStreamUrl(const std::string& itemId) const;

    bool        isAuthenticated() const { return !accessToken_.empty(); }
    std::string serverUrl()       const { return serverUrl_; }
    int         lastStatus()      const { return lastStatus_; }

private:
    int         lastStatus_ = 0;
    std::string serverUrl_;
    std::string userId_;
    std::string accessToken_;
    std::string deviceId_;
    HttpClient  http_;

    void applyAuthHeader();
};
