#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <cstdio>
#include <sys/stat.h>
#include "jellyfin.h"
#include "ui.h"
#include "player.h"

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
static std::vector<JellyfinItem>    items;
static int selLib  = 0, libOffset  = 0;
static int selItem = 0, itemOffset = 0;
static std::string playerUrl;
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

            case STATE_LIBRARIES:
                if (kDown & KEY_DOWN && selLib < (int)libraries.size() - 1) {
                    selLib++;
                    if (selLib >= libOffset + UI::VISIBLE_ROWS) libOffset++;
                }
                if (kDown & KEY_UP && selLib > 0) {
                    selLib--;
                    if (selLib < libOffset) libOffset--;
                }
                if (kDown & KEY_A && !libraries.empty()) {
                    loadMsg = "Loading \"" + libraries[selLib].name + "\"...";
                    pending = LOAD_ITEMS;
                    state   = STATE_LOADING;
                }
                break;

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
                    playerUrl = client.getStreamUrl(items[selItem].id);
                    state     = STATE_PLAYER;
                }
                break;

            case STATE_PLAYER: {
                C2D_Fini();
                C3D_Fini();

                playerPlay(playerUrl);

                C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
                C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
                C2D_Prepare();
                topScreen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
                botScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
                ui.~UI();
                new (&ui) UI(topScreen, botScreen);
                state = STATE_ITEMS;
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
                ui.drawLibraryList(libraries, selLib, libOffset);
                break;
            case STATE_ITEMS:
                ui.drawItemList(items, selItem, itemOffset,
                                libraries[selLib].name);
                break;
            case STATE_PLAYER:
                ui.drawPlayerScreen(items[selItem], playerUrl);
                break;
            case STATE_ERROR:
                ui.drawErrorScreen(errorMsg);
                break;
        }

        ui.endFrame();
    }

    C2D_Fini();
    C3D_Fini();
    acExit();
    httpcExit();
    gfxExit();
    return 0;
}
