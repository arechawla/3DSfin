#include "jellyfin.h"
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
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
             " DeviceId=\"%s\", Version=\"0.1.0\", Token=\"%s\"",
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
             " DeviceId=\"%s\", Version=\"0.1.0\"",
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

std::vector<JellyfinItem> JellyfinClient::getItems(const std::string& parentId,
                                                    int startIndex,
                                                    int limit) {
    std::vector<JellyfinItem> result;

    char path[512];
    snprintf(path, sizeof(path),
             "/Users/%s/Items"
             "?ParentId=%s"
             "&IncludeItemTypes=Movie,Episode,Series"
             "&Recursive=true"
             "&Fields=RunTimeTicks,ProductionYear"
             "&SortBy=SortName&SortOrder=Ascending"
             "&StartIndex=%d&Limit=%d",
             userId_.c_str(), parentId.c_str(), startIndex, limit);

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

        std::string ticks = jStr(obj, "RunTimeTicks");
        if (!ticks.empty()) item.runTimeTicks = std::stoll(ticks);

        std::string year = jStr(obj, "ProductionYear");
        if (!year.empty()) item.productionYear = std::stoi(year);

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

std::string JellyfinClient::getStreamUrl(const std::string& itemId) const {
    // Ask Jellyfin to transcode to H.264/AAC at 3DS-friendly resolution.
    // Phase 2: feed this URL to the MVD hardware decoder.
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
             "&VideoBitrate=1500000"
             "&MaxWidth=400"
             "&MaxHeight=240"
             "&SubtitleMethod=None"
             "&IsPlayback=true"
             "&StartTimeTicks=0",
             serverUrl_.c_str(), itemId.c_str(),
             accessToken_.c_str(), deviceId_.c_str());
    return std::string(url);
}
