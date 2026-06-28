#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <cstdio>
#include <sys/stat.h>
#include "jellyfin.h"
#include "ui.h"
#include "player.h"
#include "image.h"

// ---- App state -------------------------------------------------------------

enum AppState {
    STATE_SERVER_SETUP,
    STATE_LOGIN,
    STATE_LOADING,
    STATE_LIBRARIES,
    STATE_ITEMS,
    STATE_PLAYER,
    STATE_ERROR,
};

enum PendingLoad {
    LOAD_NONE,
    LOAD_CONNECT_AND_AUTH,
    LOAD_LIBRARIES,
    LOAD_ITEMS,
};

// ---- Globals ---------------------------------------------------------------

static JellyfinClient client;
static AppState  state       = STATE_SERVER_SETUP;
static PendingLoad pending   = LOAD_NONE;
static std::string loadMsg;
static std::string errorMsg;

static std::vector<JellyfinLibrary> libraries;
static std::vector<C2D_Image>       libCovers;     // GPU textures, parallel to libraries
static std::vector<std::string>     libCoverData;  // raw JPEG bytes cache (survives C3D_Fini)
static std::vector<JellyfinItem>    items;
static int selLib  = 0, libOffset  = 0;

// "Continue Watching" strip (bottom screen of the grid view). resumeFocus flips
// the d-pad between the top-screen grid and this strip.
static std::vector<JellyfinItem> resumeItems;
static std::vector<C2D_Image>    resumeCovers;
static std::vector<std::string>  resumeCoverData;
static int  selResume = 0, resumeOffset = 0;
static bool resumeFocus = false;

static void freeLibCovers() {
    for (auto& im : libCovers) Image_free(&im);
    libCovers.clear();
}

static void freeResumeCovers() {
    for (auto& im : resumeCovers) Image_free(&im);
    resumeCovers.clear();
}

// (Re)build cover textures from the cached JPEG bytes — no network. Called after
// playback tears down and re-creates the GPU context.
static void buildCoverTextures() {
    freeLibCovers();
    libCovers.assign(libraries.size(), C2D_Image{});
    for (size_t i = 0; i < libCoverData.size() && i < libCovers.size(); i++)
        if (!libCoverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(libCoverData[i].data()),
                libCoverData[i].size(), &libCovers[i]);
}

// Fetch each library's cover art over the network into the cache, then build textures.
static void fetchLibCovers() {
    libCoverData.assign(libraries.size(), std::string());
    for (size_t i = 0; i < libraries.size(); i++)
        libCoverData[i] = client.getPrimaryImage(libraries[i].id, 256);
    buildCoverTextures();
}

// (Re)build resume poster textures from cached JPEG bytes — no network.
static void buildResumeTextures() {
    freeResumeCovers();
    resumeCovers.assign(resumeItems.size(), C2D_Image{});
    for (size_t i = 0; i < resumeCoverData.size() && i < resumeCovers.size(); i++)
        if (!resumeCoverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(resumeCoverData[i].data()),
                resumeCoverData[i].size(), &resumeCovers[i]);
}

// Fetch the "Continue Watching" list and its poster art, then build textures.
static void fetchResume() {
    resumeItems  = client.getResumeItems();
    selResume    = 0;
    resumeOffset = 0;
    resumeFocus  = false;
    resumeCoverData.assign(resumeItems.size(), std::string());
    for (size_t i = 0; i < resumeItems.size(); i++)
        resumeCoverData[i] = client.getPrimaryImage(resumeItems[i].id, 200);
    buildResumeTextures();
}
static int selItem = 0, itemOffset = 0;
static std::string playerUrl;
// Set just before entering STATE_PLAYER. playItem can come from either the item
// list or the Continue Watching strip, so the player path is source-agnostic.
static JellyfinItem playItem;
static double       playStartSec = 0.0;
static AppState     playReturn   = STATE_ITEMS;
static std::string pendingUsername;
static std::string pendingPassword;

// ---- Config ----------------------------------------------------------------

static const char* CFG_PATH = "/3ds/3dsfin/config.ini";
static std::string cfgServer;
static std::string cfgUsername;

static void saveConfig() {
    FILE* f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "server=%s\n",   cfgServer.c_str());
    fprintf(f, "username=%s\n", cfgUsername.c_str());
    fclose(f);
}

static void loadConfig() {
    FILE* f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.substr(0, 7)  == "server=")   cfgServer   = s.substr(7);
        if (s.substr(0, 9)  == "username=") cfgUsername = s.substr(9);
    }
    fclose(f);
}

// ---- Software keyboard helpers ---------------------------------------------

static std::string swkbdRead(const char* hint, const char* initial = nullptr,
                              bool hidden = false) {
    SwkbdState swkbd;
    char buf[512] = {};
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, hint);
    if (initial && *initial) swkbdSetInitialText(&swkbd, initial);
    if (hidden) swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdInputText(&swkbd, buf, sizeof(buf));
    return std::string(buf);
}

// ---- Main ------------------------------------------------------------------

int main() {
    gfxInitDefault();
    httpcInit(0x1000);
    acInit();

    // Run at the New 3DS clock (804 MHz) + L2 cache when available. Without this a
    // New 3DS runs at the Old 3DS 268 MHz, throttling the CPU blit, demux, and the
    // per-call HTTP copy — i.e. both decode smoothness and download throughput.
    // No-op on an Old 3DS, so it's always safe to call.
    osSetSpeedupEnable(true);

    // Ensure config directory exists
    mkdir("/3ds",       0777);
    mkdir("/3ds/3dsfin", 0777);
    loadConfig();

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget* topScreen =
        C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* botScreen =
        C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    UI ui(topScreen, botScreen);

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) break;

        // ------------------------------------------------------------------
        // Phase 1: execute any pending blocking network operation.
        // The previous frame already rendered the loading screen.
        // ------------------------------------------------------------------
        if (pending != LOAD_NONE) {
            switch (pending) {
                case LOAD_CONNECT_AND_AUTH:
                    if (!client.connect(cfgServer)) {
                        errorMsg = "Cannot reach server:\n" + cfgServer +
                                   "\n\nCheck the URL and your WiFi.";
                        state = STATE_ERROR;
                    } else if (!client.authenticate(pendingUsername, pendingPassword)) {
                        char statusBuf[64];
                        int s = client.lastStatus();
                        if (s == -3)
                            snprintf(statusBuf, sizeof(statusBuf), "Response parse error");
                        else if (s < 0)
                            snprintf(statusBuf, sizeof(statusBuf), "Network error (%d)", s);
                        else
                            snprintf(statusBuf, sizeof(statusBuf), "HTTP %d", s);
                        errorMsg = std::string("Login failed: ") + statusBuf +
                                   "\n\nIf HTTP 401: wrong credentials\n"
                                   "If HTTP 400: server rejected request\n"
                                   "If -1/-2: network/SSL error";
                        state = STATE_ERROR;
                    } else {
                        cfgUsername = pendingUsername;
                        saveConfig();
                        loadMsg = "Loading libraries...";
                        pending = LOAD_LIBRARIES;
                        // Render one loading frame before the next blocking call
                        ui.beginFrame();
                        ui.drawLoadingScreen(loadMsg);
                        ui.endFrame();
                        continue;
                    }
                    break;

                case LOAD_LIBRARIES:
                    libraries  = client.getLibraries();
                    selLib     = 0;
                    libOffset  = 0;
                    fetchLibCovers();   // network: cache JPEGs + build textures
                    fetchResume();      // Continue Watching list + poster art
                    state      = STATE_LIBRARIES;
                    break;

                case LOAD_ITEMS:
                    items      = client.getItems(libraries[selLib].id);
                    selItem    = 0;
                    itemOffset = 0;
                    state      = STATE_ITEMS;
                    break;

                default: break;
            }
            pending = LOAD_NONE;
        }

        // ------------------------------------------------------------------
        // Phase 2: handle input for the current state.
        // ------------------------------------------------------------------
        switch (state) {
            case STATE_SERVER_SETUP:
                if (kDown & KEY_A) {
                    std::string url = swkbdRead(
                        "Jellyfin server URL",
                        cfgServer.empty() ? "http://192.168.1.x:8096" : cfgServer.c_str());
                    if (!url.empty()) {
                        cfgServer = url;
                        state = STATE_LOGIN;
                    }
                }
                break;

            case STATE_LOGIN:
                if (kDown & KEY_B) { state = STATE_SERVER_SETUP; break; }
                if (kDown & KEY_A) {
                    pendingUsername = swkbdRead("Username",
                        cfgUsername.empty() ? nullptr : cfgUsername.c_str());
                    if (pendingUsername.empty()) break;
                    pendingPassword = swkbdRead("Password", nullptr, true);
                    if (pendingPassword.empty()) break;
                    loadMsg = "Connecting to " + cfgServer + "...";
                    pending = LOAD_CONNECT_AND_AUTH;
                    state   = STATE_LOADING;
                }
                break;

            case STATE_LIBRARIES: {
                int n    = (int)libraries.size();
                int cols = UI::GRID_COLS;

                // Focus lives in the Continue Watching strip on the bottom screen.
                if (resumeFocus) {
                    int rn = (int)resumeItems.size();
                    if (kDown & KEY_UP) { resumeFocus = false; break; }
                    if (kDown & KEY_RIGHT && selResume < rn - 1) selResume++;
                    if (kDown & KEY_LEFT  && selResume > 0)      selResume--;
                    // Keep the selected poster within the visible window.
                    if (selResume < resumeOffset) resumeOffset = selResume;
                    if (selResume >= resumeOffset + UI::RESUME_VISIBLE)
                        resumeOffset = selResume - UI::RESUME_VISIBLE + 1;
                    if (kDown & KEY_A && rn > 0) {
                        playItem     = resumeItems[selResume];
                        playStartSec = playItem.resumeTicks / 10000000.0;
                        playerUrl    = client.getStreamUrl(playItem.id, playItem.resumeTicks);
                        playReturn   = STATE_LIBRARIES;
                        state        = STATE_PLAYER;
                    }
                    break;
                }

                if (kDown & KEY_RIGHT && selLib < n - 1 && (selLib % cols) != cols - 1) selLib++;
                if (kDown & KEY_LEFT  && (selLib % cols) != 0)                          selLib--;
                if (kDown & KEY_DOWN  && selLib + cols < n)                             selLib += cols;
                if (kDown & KEY_UP    && selLib - cols >= 0)                            selLib -= cols;
                // DOWN with no grid row below drops focus into the Continue Watching strip.
                if (kDown & KEY_DOWN && selLib + cols >= n && !resumeItems.empty())
                    resumeFocus = true;
                // Scroll so the selected row stays visible (libOffset is in rows).
                int selRow = selLib / cols;
                if (selRow < libOffset) libOffset = selRow;
                if (selRow >= libOffset + UI::GRID_ROWS_VISIBLE)
                    libOffset = selRow - UI::GRID_ROWS_VISIBLE + 1;

                if (kDown & KEY_A && !libraries.empty()) {
                    loadMsg = "Loading \"" + libraries[selLib].name + "\"...";
                    pending = LOAD_ITEMS;
                    state   = STATE_LOADING;
                }
                break;
            }

            case STATE_ITEMS:
                if (kDown & KEY_B) { state = STATE_LIBRARIES; break; }
                if (kDown & KEY_DOWN && selItem < (int)items.size() - 1) {
                    selItem++;
                    if (selItem >= itemOffset + UI::VISIBLE_ROWS) itemOffset++;
                }
                if (kDown & KEY_UP && selItem > 0) {
                    selItem--;
                    if (selItem < itemOffset) itemOffset--;
                }
                if (kDown & KEY_A && !items.empty()) {
                    playItem     = items[selItem];
                    playStartSec = 0.0;
                    playerUrl    = client.getStreamUrl(playItem.id);
                    playReturn   = STATE_ITEMS;
                    state        = STATE_PLAYER;
                }
                break;

            case STATE_PLAYER: {
                freeLibCovers();      // textures live in VRAM that playback tears down
                freeResumeCovers();
                C2D_Fini();
                C3D_Fini();

                playerPlay(playerUrl, playItem.runTimeTicks,
                           playItem.seriesName,
                           playItem.name,
                           playItem.productionYear,
                           playStartSec);

                C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
                C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
                C2D_Prepare();
                topScreen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
                botScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
                ui.~UI();
                new (&ui) UI(topScreen, botScreen);
                buildCoverTextures();    // rebuild from cached JPEG bytes (no re-fetch)
                buildResumeTextures();
                state = playReturn;
                break;
            }

            case STATE_ERROR:
                if (kDown & KEY_A || kDown & KEY_B) {
                    errorMsg.clear();
                    state = STATE_LOGIN;
                }
                break;

            case STATE_LOADING:
                break;
        }

        // ------------------------------------------------------------------
        // Phase 3: render the current state.
        // ------------------------------------------------------------------
        ui.beginFrame();

        switch (state) {
            case STATE_SERVER_SETUP:
                ui.drawSetupScreen(cfgServer);
                break;
            case STATE_LOGIN:
                ui.drawLoginScreen(cfgServer, cfgUsername);
                break;
            case STATE_LOADING:
                ui.drawLoadingScreen(loadMsg);
                break;
            case STATE_LIBRARIES:
                ui.drawLibraryGrid(libraries, libCovers, selLib, libOffset,
                                   resumeItems, resumeCovers,
                                   selResume, resumeOffset, resumeFocus);
                break;
            case STATE_ITEMS:
                ui.drawItemList(items, selItem, itemOffset,
                                libraries[selLib].name);
                break;
            case STATE_PLAYER:
                ui.drawPlayerScreen(playItem, playerUrl);
                break;
            case STATE_ERROR:
                ui.drawErrorScreen(errorMsg);
                break;
        }

        ui.endFrame();
    }

    freeLibCovers();
    freeResumeCovers();
    C2D_Fini();
    C3D_Fini();
    acExit();
    httpcExit();
    gfxExit();
    return 0;
}
