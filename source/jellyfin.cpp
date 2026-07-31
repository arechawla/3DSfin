#include "jellyfin.h"
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>

// ---------------------------------------------------------------------------
// Minimal JSON helpers — handles the specific shapes Jellyfin returns.
// Not a general parser: no unicode escapes, no deeply nested look-ahead.
// ---------------------------------------------------------------------------

// Returns the raw value string for a key (string unquoted, numbers as-is).
static std::string jStr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        pos++;
        std::string out;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\') { pos++; }
            if (pos < json.size()) out += json[pos++];
        }
        return out;
    }
    // number / bool / null
    size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != '\n')
        end++;
    std::string v = json.substr(pos, end - pos);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
    return v;
}

// Advance i past a quoted JSON string (i should point at the opening '"').
static void skipStr(const std::string& s, size_t& i) {
    i++; // skip opening quote
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') i++; // skip escaped character
        i++;
    }
    // i now points at closing '"' (or end)
}

// Returns the content of the first { } block after "key":
static std::string jObj(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find('{', pos + needle.size());
    if (pos == std::string::npos) return "";
    int depth = 0;
    for (size_t i = pos; i < json.size(); i++) {
        if      (json[i] == '"') skipStr(json, i);
        else if (json[i] == '{') depth++;
        else if (json[i] == '}') {
            if (--depth == 0) return json.substr(pos, i - pos + 1);
        }
    }
    return "";
}

// Returns the content of the first [ ] block after "key":
static std::string jArr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return "";
    int depth = 0;
    for (size_t i = pos; i < json.size(); i++) {
        if      (json[i] == '"') skipStr(json, i);
        else if (json[i] == '[') depth++;
        else if (json[i] == ']') {
            if (--depth == 0) return json.substr(pos, i - pos + 1);
        }
    }
    return "";
}

// Calls cb once per top-level { } object inside an array string.
static void jForEach(const std::string& arr,
                     const std::function<void(const std::string&)>& cb) {
    size_t pos = 0;
    while (pos < arr.size()) {
        size_t start = arr.find('{', pos);
        if (start == std::string::npos) break;
        int depth = 0;
        for (size_t i = start; i < arr.size(); i++) {
            if      (arr[i] == '"') skipStr(arr, i);
            else if (arr[i] == '{') depth++;
            else if (arr[i] == '}') {
                if (--depth == 0) {
                    cb(arr.substr(start, i - start + 1));
                    pos = i + 1;
                    break;
                }
            }
        }
        if (depth > 0) break;
    }
}

// ---------------------------------------------------------------------------

static std::string makeDeviceId() {
    char buf[32];
    // Deterministic-ish: mix svcGetSystemTick with a constant
    u64 tick = svcGetSystemTick();
    snprintf(buf, sizeof(buf), "%08X%08X",
             static_cast<unsigned>(tick >> 32),
             static_cast<unsigned>(tick & 0xFFFFFFFF));
    return std::string(buf);
}

JellyfinClient::JellyfinClient() : deviceId_(makeDeviceId()) {}

void JellyfinClient::applyAuthHeader() {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "MediaBrowser Client=\"3DSFin\", Device=\"Nintendo 3DS\","
             " DeviceId=\"%s\", Version=\"0.2.0\", Token=\"%s\"",
             deviceId_.c_str(), accessToken_.c_str());
    http_.setHeader("X-Emby-Authorization", buf);
}

bool JellyfinClient::connect(const std::string& serverUrl) {
    serverUrl_ = serverUrl;
    while (!serverUrl_.empty() && serverUrl_.back() == '/')
        serverUrl_.pop_back();

    http_.setBaseUrl(serverUrl_);

    // Probe /System/Info/Public — no auth required
    auto resp = http_.get("/System/Info/Public");
    return resp.ok();
}

bool JellyfinClient::authenticate(const std::string& username,
                                   const std::string& password) {
    // Auth header without token for the login request
    char authHdr[256];
    snprintf(authHdr, sizeof(authHdr),
             "MediaBrowser Client=\"3DSFin\", Device=\"Nintendo 3DS\","
             " DeviceId=\"%s\", Version=\"0.2.0\"",
             deviceId_.c_str());
    http_.setHeader("X-Emby-Authorization", authHdr);

    char body[512];
    snprintf(body, sizeof(body),
             "{\"Username\":\"%s\",\"Pw\":\"%s\"}",
             username.c_str(), password.c_str());

    auto resp = http_.post("/Users/AuthenticateByName", body);
    lastStatus_ = resp.status;
    if (!resp.ok()) return false;

    accessToken_ = jStr(resp.body, "AccessToken");
    std::string userObj = jObj(resp.body, "User");
    userId_ = jStr(userObj, "Id");

    if (accessToken_.empty() || userId_.empty()) {
        lastStatus_ = -3; // parse failure
        // Dump response body to SD card for debugging
        FILE* f = fopen("/3ds/3dsfin/auth_debug.txt", "w");
        if (f) {
            fprintf(f, "status: %d\nbody:\n%s\n", resp.status, resp.body.c_str());
            fclose(f);
        }
        return false;
    }

    applyAuthHeader();
    return true;
}

std::vector<JellyfinLibrary> JellyfinClient::getLibraries() {
    std::vector<JellyfinLibrary> result;

    char path[128];
    snprintf(path, sizeof(path), "/Users/%s/Views", userId_.c_str());
    auto resp = http_.get(path);
    {
        FILE* f = fopen("/3ds/3dsfin/lib_debug.txt", "w");
        if (f) { fprintf(f, "status: %d\nbody:\n%s\n", resp.status, resp.body.c_str()); fclose(f); }
    }
    if (!resp.ok()) return result;

    std::string arr = jArr(resp.body, "Items");
    jForEach(arr, [&](const std::string& obj) {
        JellyfinLibrary lib;
        lib.id             = jStr(obj, "Id");
        lib.name           = jStr(obj, "Name");
        lib.collectionType = jStr(obj, "CollectionType");
        if (!lib.id.empty()) result.push_back(lib);
    });
    return result;
}

std::vector<JellyfinItem> JellyfinClient::getChildren(const std::string& parentId,
                                                      ChildKind kind,
                                                      int       limit) {
    std::vector<JellyfinItem> result;

    char path[512];
    switch (kind) {
    case ChildKind::Seasons:
        // A series' seasons, ordered by season number. No Recursive so episodes
        // stay one level down.
        snprintf(path, sizeof(path),
                 "/Users/%s/Items"
                 "?ParentId=%s"
                 "&IncludeItemTypes=Season"
                 "&Fields=RunTimeTicks,ProductionYear"
                 "&SortBy=IndexNumber,SortName&SortOrder=Ascending"
                 "&Limit=%d",
                 userId_.c_str(), parentId.c_str(), limit);
        break;

    case ChildKind::Episodes:
        // A single season's episodes, ordered by episode number.
        snprintf(path, sizeof(path),
                 "/Users/%s/Items"
                 "?ParentId=%s"
                 "&IncludeItemTypes=Episode"
                 "&Fields=RunTimeTicks,ProductionYear"
                 "&SortBy=ParentIndexNumber,IndexNumber&SortOrder=Ascending"
                 "&Limit=%d",
                 userId_.c_str(), parentId.c_str(), limit);
        break;

    case ChildKind::EpisodesRecursive:
        // Every episode beneath a series, flattened across seasons and ordered
        // by season (ParentIndexNumber) then episode (IndexNumber).
        snprintf(path, sizeof(path),
                 "/Users/%s/Items"
                 "?ParentId=%s"
                 "&IncludeItemTypes=Episode"
                 "&Recursive=true"
                 "&Fields=RunTimeTicks,ProductionYear"
                 "&SortBy=ParentIndexNumber,IndexNumber&SortOrder=Ascending"
                 "&Limit=%d",
                 userId_.c_str(), parentId.c_str(), limit);
        break;

    case ChildKind::Direct:
    default:
        // Direct children only: a Movies library yields Movies, a Shows library
        // yields Series. No Recursive so episodes don't bubble up to this level.
        snprintf(path, sizeof(path),
                 "/Users/%s/Items"
                 "?ParentId=%s"
                 "&Fields=RunTimeTicks,ProductionYear"
                 "&SortBy=SortName&SortOrder=Ascending"
                 "&Limit=%d",
                 userId_.c_str(), parentId.c_str(), limit);
        break;
    }

    auto resp = http_.get(path);
    if (!resp.ok()) return result;

    std::string arr = jArr(resp.body, "Items");
    jForEach(arr, [&](const std::string& obj) {
        JellyfinItem item;
        item.id             = jStr(obj, "Id");
        item.name           = jStr(obj, "Name");
        item.type           = jStr(obj, "Type");
        item.seriesName     = jStr(obj, "SeriesName");   // present for episodes
        item.productionYear = 0;
        item.runTimeTicks   = 0;
        item.resumeTicks    = 0;

        std::string ticks = jStr(obj, "RunTimeTicks");
        if (!ticks.empty()) item.runTimeTicks = std::stoll(ticks);

        std::string year = jStr(obj, "ProductionYear");
        if (!year.empty()) item.productionYear = std::stoi(year);

        // Resume a partially-watched item when launched straight from the grid.
        std::string pos = jStr(obj, "PlaybackPositionTicks");
        if (!pos.empty()) item.resumeTicks = std::stoll(pos);

        if (!item.id.empty()) result.push_back(item);
    });
    return result;
}

std::vector<JellyfinItem> JellyfinClient::getResumeItems(int limit) {
    std::vector<JellyfinItem> result;

    char path[512];
    snprintf(path, sizeof(path),
             "/Users/%s/Items/Resume"
             "?MediaTypes=Video"
             "&Recursive=true"
             "&Fields=RunTimeTicks,ProductionYear"
             "&Limit=%d",
             userId_.c_str(), limit);

    auto resp = http_.get(path);
    if (!resp.ok()) return result;

    std::string arr = jArr(resp.body, "Items");
    jForEach(arr, [&](const std::string& obj) {
        JellyfinItem item;
        item.id             = jStr(obj, "Id");
        item.name           = jStr(obj, "Name");
        item.type           = jStr(obj, "Type");
        item.seriesName     = jStr(obj, "SeriesName");   // present for episodes
        item.productionYear = 0;
        item.runTimeTicks   = 0;
        item.resumeTicks    = 0;

        std::string ticks = jStr(obj, "RunTimeTicks");
        if (!ticks.empty()) item.runTimeTicks = std::stoll(ticks);

        std::string year = jStr(obj, "ProductionYear");
        if (!year.empty()) item.productionYear = std::stoi(year);

        // PlaybackPositionTicks lives inside the nested "UserData" object; jStr's
        // first-occurrence search finds it without isolating the sub-object.
        std::string pos = jStr(obj, "PlaybackPositionTicks");
        if (!pos.empty()) item.resumeTicks = std::stoll(pos);

        if (!item.id.empty()) result.push_back(item);
    });
    return result;
}

std::string JellyfinClient::getPrimaryImage(const std::string& itemId, int fillWidth) {
    char path[256];
    snprintf(path, sizeof(path),
             "/Items/%s/Images/Primary?fillWidth=%d&format=Jpg&quality=85",
             itemId.c_str(), fillWidth);
    auto resp = http_.get(path);
    if (!resp.ok()) return "";   // 404 = library has no Primary image
    return resp.body;            // raw JPEG bytes (binary-safe)
}

std::vector<JellyfinAudioTrack> JellyfinClient::getAudioTracks(const std::string& itemId) {
    std::vector<JellyfinAudioTrack> result;

    // Ids= returns the full item including MediaSources; MediaStreams lives inside
    // the first source. Same /Users/{id}/Items shape the browse calls use.
    char path[512];
    snprintf(path, sizeof(path),
             "/Users/%s/Items?Ids=%s&Fields=MediaSources&Limit=1",
             userId_.c_str(), itemId.c_str());

    auto resp = http_.get(path);
    if (!resp.ok()) return result;

    // First MediaStreams array = the first media source's streams (all we play).
    std::string streams = jArr(resp.body, "MediaStreams");
    if (streams.empty()) return result;

    jForEach(streams, [&](const std::string& obj) {
        if (jStr(obj, "Type") != "Audio") return;

        JellyfinAudioTrack t;
        std::string idx = jStr(obj, "Index");
        if (idx.empty()) return;                  // no index = nothing to request
        t.index     = std::stoi(idx);
        t.title     = jStr(obj, "DisplayTitle");
        t.language  = jStr(obj, "Language");
        t.isDefault = jStr(obj, "IsDefault") == "true";

        // DisplayTitle is normally populated; fall back to whatever identifies it.
        if (t.title.empty()) {
            t.title = t.language.empty() ? jStr(obj, "Codec") : t.language;
            if (t.title.empty()) t.title = "Track " + std::to_string(t.index);
        }
        result.push_back(t);
    });
    return result;
}

std::string JellyfinClient::getStreamUrl(const std::string& itemId,
                                         long long startTicks,
                                         int       audioStreamIndex) {
    // Ask Jellyfin to transcode to H.264/AAC at 3DS-friendly resolution.
    // Phase 2: feed this URL to the MVD hardware decoder.

    // Fresh session id per stream: this is what makes the server actually start
    // a new transcode at startTicks instead of resuming the previous job.
    static unsigned seq = 0;
    char psid[64];
    snprintf(psid, sizeof(psid), "3dsfin-%llu-%u",
             (unsigned long long)time(nullptr), ++seq);
    lastPlaySessionId_ = psid;

    char url[1024];
    snprintf(url, sizeof(url),
             "%s/Videos/%s/stream.ts"
             "?Container=ts"
             "&api_key=%s"
             "&DeviceId=%s"
             "&VideoCodec=h264"
             // Force Baseline profile: no B-frames (so decode order == display
             // order — we blit frames as they decode, with no reorder buffer) and
             // no CABAC. Both make the stream much friendlier to the MVD decoder.
             "&Profile=baseline"
             "&AudioCodec=aac"
             // Cap + downmix audio. A 5.1 source otherwise transcodes to ~384 kbit/s
             // AAC — which nearly DOUBLES the stream and, since we don't play audio
             // yet, is pure wasted RF budget. Stereo @ 96 kbit/s frees it for video.
             "&AudioBitrate=96000"
             "&MaxAudioChannels=2"
             // Bitrate must stay UNDER sustained 3DS Wi-Fi throughput or the ring
             // buffer drains faster than it fills and playback buffers every few
             // seconds. 0.4 Mbps for headroom on a marginal link; still fine for
             // 400x224. Raise/lower to taste.
             "&VideoBitrate=400000"
             // Cap fps at 24. With VideoBitrate capped this doesn't cut bandwidth,
             // but at a low bitrate it gives each frame ~25% more bits (cleaner
             // image) and trims blit CPU on core 0 (more time for the downloader).
             "&MaxFramerate=24"
             "&MaxWidth=400"
             "&MaxHeight=240"
             "&SubtitleMethod=None"
             "&IsPlayback=true"
             // Seek the transcode to the resume position (0 = from the start).
             "&StartTimeTicks=%lld"
             "&PlaySessionId=%s",
             serverUrl_.c_str(), itemId.c_str(),
             accessToken_.c_str(), deviceId_.c_str(), startTicks, psid);

    std::string out(url);
    // Absolute stream index from getAudioTracks(). Omitted entirely when -1 so the
    // server applies its own default-track rules, exactly as before.
    if (audioStreamIndex >= 0)
        out += "&AudioStreamIndex=" + std::to_string(audioStreamIndex);
    return out;
}

void JellyfinClient::stopTranscode() {
    if (lastPlaySessionId_.empty()) return;
    char path[256];
    snprintf(path, sizeof(path),
             "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s",
             deviceId_.c_str(), lastPlaySessionId_.c_str());
    http_.del(path);
    lastPlaySessionId_.clear();
}
