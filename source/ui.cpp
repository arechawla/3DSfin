#include "ui.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---- Palette ---------------------------------------------------------------
static constexpr u32 COL_BG        = C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF);
static constexpr u32 COL_BG_BOT    = C2D_Color32(0x16, 0x16, 0x28, 0xFF);
static constexpr u32 COL_BAR       = C2D_Color32(0x0f, 0x3c, 0x78, 0xFF);
static constexpr u32 COL_SEL       = C2D_Color32(0x1a, 0x6b, 0xb5, 0xFF);
static constexpr u32 COL_ROW_ALT   = C2D_Color32(0x22, 0x22, 0x3a, 0xFF);
static constexpr u32 COL_WHITE     = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
static constexpr u32 COL_GREY      = C2D_Color32(0xaa, 0xaa, 0xaa, 0xFF);
static constexpr u32 COL_YELLOW    = C2D_Color32(0xFF, 0xd7, 0x00, 0xFF);
static constexpr u32 COL_RED       = C2D_Color32(0xFF, 0x44, 0x44, 0xFF);
static constexpr u32 COL_GREEN     = C2D_Color32(0x44, 0xFF, 0x88, 0xFF);

// ---- Helpers ---------------------------------------------------------------

std::string UI::formatDuration(long long ticks) {
    if (ticks <= 0) return "";
    long long secs  = ticks / 10000000LL;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return std::string(buf);
}

std::string UI::truncate(const std::string& str, size_t maxLen) {
    if (str.size() <= maxLen) return str;
    return str.substr(0, maxLen - 3) + "...";
}

// ---- Construction ----------------------------------------------------------

UI::UI(C3D_RenderTarget* top, C3D_RenderTarget* bot)
    : top_(top), bot_(bot) {
    font_    = C2D_FontLoadSystem(CFG_REGION_USA);
    textBuf_ = C2D_TextBufNew(4096);
}

UI::~UI() {
    C2D_TextBufDelete(textBuf_);
    C2D_FontFree(font_);
}

// ---- Low-level draw helpers ------------------------------------------------

void UI::drawText(const std::string& str, float x, float y, float scale, u32 color) {
    C2D_Text t;
    C2D_TextFontParse(&t, font_, textBuf_, str.c_str());
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

void UI::drawTextBuf(const std::string& str, float x, float y, float scale, u32 color,
                     float maxWidth) {
    // Simple character-level truncation based on approximate char width
    float charW   = scale * 11.0f; // rough estimate for system font
    int   maxChars = static_cast<int>(maxWidth / charW);
    drawText(truncate(str, maxChars < 4 ? 4 : (size_t)maxChars), x, y, scale, color);
}

void UI::drawRect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

void UI::drawTopBar(const std::string& title) {
    drawRect(0, 0, TOP_W, 22, COL_BAR);
    drawText("3DSFin", 6, 4, 0.50f, COL_YELLOW);
    drawText(title,    80, 4, 0.50f, COL_WHITE);
}

void UI::drawBottomHints(const std::string& hints) {
    drawRect(0, BOT_H - 20, BOT_W, 20, COL_BAR);
    drawText(hints, 6, BOT_H - 17, 0.42f, COL_GREY);
}

void UI::drawScrollList(const std::vector<std::string>& rows, int selected, int offset) {
    int startY = 26;
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int idx = offset + i;
        if (idx >= (int)rows.size()) break;
        float ry = static_cast<float>(startY + i * ROW_HEIGHT);
        u32 bg   = (idx == selected) ? COL_SEL
                 : (i % 2 == 0)      ? COL_BG
                                      : COL_ROW_ALT;
        drawRect(0, ry, TOP_W, ROW_HEIGHT, bg);
        drawTextBuf(rows[idx], 8, ry + 5, 0.50f, COL_WHITE, TOP_W - 16);
    }

    // Scroll indicator
    if ((int)rows.size() > VISIBLE_ROWS) {
        float barH    = TOP_H - 26;
        float thumbH  = barH * VISIBLE_ROWS / rows.size();
        float thumbY  = 26 + barH * offset / rows.size();
        drawRect(TOP_W - 4, 26,     4, barH,   COL_ROW_ALT);
        drawRect(TOP_W - 4, thumbY, 4, thumbH, COL_GREY);
    }
}

// ---- Frame -----------------------------------------------------------------

void UI::beginFrame() {
    C2D_TextBufClear(textBuf_);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_, COL_BG);
    C2D_TargetClear(bot_, COL_BG_BOT);
}

void UI::endFrame() {
    C3D_FrameEnd(0);
}

// ---- Screens ---------------------------------------------------------------

void UI::drawSetupScreen(const std::string& currentUrl) {
    C2D_SceneBegin(top_);
    drawTopBar("Server Setup");
    drawText("Press A to enter your Jellyfin server URL", 8, 60, 0.50f, COL_WHITE);
    drawText("Example:  http://192.168.1.x:8096",         8, 90, 0.48f, COL_GREY);
    if (!currentUrl.empty()) {
        drawText("Current:  " + currentUrl, 8, 120, 0.48f, COL_GREEN);
    }

    C2D_SceneBegin(bot_);
    drawBottomHints("A: Enter URL   START: Quit");
}

void UI::drawLoginScreen(const std::string& serverUrl, const std::string& username) {
    C2D_SceneBegin(top_);
    drawTopBar("Login");
    drawText("Server:",   8, 50,  0.50f, COL_GREY);
    drawTextBuf(serverUrl, 80, 50, 0.48f, COL_WHITE, TOP_W - 88);
    drawText("Press A to log in", 8, 100, 0.52f, COL_WHITE);
    if (!username.empty())
        drawText("User: " + username, 8, 130, 0.48f, COL_GREEN);

    C2D_SceneBegin(bot_);
    drawBottomHints("A: Enter credentials   B: Change server");
}

void UI::drawLoadingScreen(const std::string& msg) {
    C2D_SceneBegin(top_);
    drawTopBar("Loading");
    drawText(msg, 8, 110, 0.55f, COL_WHITE);

    C2D_SceneBegin(bot_);
}

void UI::drawErrorScreen(const std::string& msg) {
    C2D_SceneBegin(top_);
    drawTopBar("Error");
    drawRect(0, 26, TOP_W, TOP_H - 26, COL_BG);

    // Multi-line: split on \n
    float y = 60;
    std::string line;
    for (char c : msg) {
        if (c == '\n') {
            drawText(line, 8, y, 0.52f, COL_RED);
            line.clear();
            y += 26;
        } else {
            line += c;
        }
    }
    if (!line.empty()) drawText(line, 8, y, 0.52f, COL_RED);

    C2D_SceneBegin(bot_);
    drawBottomHints("A/B: Back to login");
}

// Placeholder card color, keyed off the library's collection type.
static u32 placeholderColor(const std::string& type) {
    if (type == "movies")   return C2D_Color32(0x2e, 0x4b, 0x8c, 0xFF); // blue
    if (type == "tvshows")  return C2D_Color32(0x6b, 0x2e, 0x8c, 0xFF); // purple
    if (type == "music")    return C2D_Color32(0x2e, 0x8c, 0x5a, 0xFF); // green
    if (type == "books")    return C2D_Color32(0x8c, 0x6b, 0x2e, 0xFF); // amber
    if (type == "homevideos" || type == "photos")
                            return C2D_Color32(0x8c, 0x2e, 0x4b, 0xFF); // rose
    return C2D_Color32(0x3a, 0x3a, 0x52, 0xFF);                         // neutral
}

void UI::drawLibraryGrid(const std::vector<JellyfinLibrary>& libs,
                         const std::vector<C2D_Image>& covers,
                         int selected, int offset) {
    C2D_SceneBegin(top_);
    drawTopBar("Libraries");

    const int   cols   = GRID_COLS;
    const int   rowsV  = GRID_ROWS_VISIBLE;
    const float mx     = 10.0f;   // left/right margin
    const float my     = 30.0f;   // top of grid (below the bar)
    const float gap    = 10.0f;   // gap between cards
    const float cardW  = (TOP_W - 2 * mx - (cols - 1) * gap) / cols;  // ~185
    const float cardH  = 92.0f;
    const float pitchY = cardH + 12.0f;

    if (libs.empty()) {
        drawText("(no libraries found)", 8, 110, 0.50f, COL_GREY);
    }

    for (int i = 0; i < cols * rowsV; i++) {
        int idx = offset * cols + i;
        if (idx >= (int)libs.size()) break;

        int   c = i % cols, r = i / cols;
        float x = mx + c * (cardW + gap);
        float y = my + r * pitchY;
        bool  sel = (idx == selected);

        if (sel) drawRect(x - 3, y - 3, cardW + 6, cardH + 6, COL_SEL);

        bool hasImg = (idx < (int)covers.size() && covers[idx].tex != nullptr);
        if (hasImg) {
            const C2D_Image& im = covers[idx];
            float sx = cardW / (float)im.subtex->width;
            float sy = cardH / (float)im.subtex->height;
            C2D_DrawImageAt(im, x, y, 0.0f, nullptr, sx, sy);
        } else {
            drawRect(x, y, cardW, cardH, placeholderColor(libs[idx].collectionType));
        }

        // Name strip across the bottom of the card.
        drawRect(x, y + cardH - 22, cardW, 22, C2D_Color32(0, 0, 0, 0xB0));
        drawTextBuf(libs[idx].name, x + 6, y + cardH - 19, 0.46f, COL_WHITE, cardW - 12);
    }

    // Scrollbar when there are more rows than fit.
    int totalRows = ((int)libs.size() + cols - 1) / cols;
    if (totalRows > rowsV) {
        float barH   = TOP_H - 26;
        float thumbH = barH * rowsV / totalRows;
        float thumbY = 26 + barH * offset / totalRows;
        drawRect(TOP_W - 4, 26,     4, barH,   COL_ROW_ALT);
        drawRect(TOP_W - 4, thumbY, 4, thumbH, COL_GREY);
    }

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);
    if (selected >= 0 && selected < (int)libs.size()) {
        drawText(truncate(libs[selected].name, 28), 8, 30, 0.58f, COL_WHITE);
        if (!libs[selected].collectionType.empty())
            drawText(libs[selected].collectionType, 8, 60, 0.46f, COL_GREY);
    }
    drawBottomHints("A: Open   D-Pad: Move   START: Quit");
}

void UI::drawItemList(const std::vector<JellyfinItem>& items,
                      int selected, int offset,
                      const std::string& libraryName) {
    std::vector<std::string> rows;
    for (auto& item : items) {
        std::string label = item.name;
        if (item.productionYear > 0) {
            char yr[8];
            snprintf(yr, sizeof(yr), " (%d)", item.productionYear);
            label += yr;
        }
        rows.push_back(label);
    }
    if (rows.empty()) rows.push_back("(no items found)");

    C2D_SceneBegin(top_);
    drawTopBar(libraryName);
    drawScrollList(rows, selected, offset);

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);

    if (!items.empty() && selected < (int)items.size()) {
        auto& it = items[selected];
        drawText(truncate(it.name, 30), 8, 20, 0.52f, COL_WHITE);
        if (it.productionYear > 0) {
            char yr[32];
            snprintf(yr, sizeof(yr), "Year: %d", it.productionYear);
            drawText(yr, 8, 48, 0.48f, COL_GREY);
        }
        std::string dur = formatDuration(it.runTimeTicks);
        if (!dur.empty()) drawText("Duration: " + dur, 8, 70, 0.48f, COL_GREY);
        drawText("Type: " + it.type, 8, 92, 0.48f, COL_GREY);
    }

    drawBottomHints("A: Play   B: Libraries   UP/DOWN: Navigate");
}

void UI::drawPlayerScreen(const JellyfinItem& item, const std::string& streamUrl) {
    C2D_SceneBegin(top_);
    drawTopBar("Now Playing");
    drawText(truncate(item.name, 40), 8, 40, 0.52f, COL_WHITE);

    std::string dur = formatDuration(item.runTimeTicks);
    if (!dur.empty()) drawText("Duration: " + dur, 8, 70, 0.48f, COL_GREY);
    drawText("Type: " + item.type, 8, 90, 0.48f, COL_GREY);

    drawText("Stream URL ready.", 8, 120, 0.48f, COL_GREEN);
    drawText("(Video playback: Phase 2)", 8, 142, 0.44f, COL_GREY);

    // Show beginning of URL for debug
    drawTextBuf(streamUrl, 8, 168, 0.38f, COL_GREY, TOP_W - 16);

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);
    drawText("Stream URL copied to log.", 8, 80, 0.48f, COL_GREY);
    drawBottomHints("B: Back to list");
}
