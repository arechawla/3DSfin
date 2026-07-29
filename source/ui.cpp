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

// Draw an image so it fills the whole x/y/w/h box without distortion ("cover"):
// scale uniformly to cover the box, then center-crop the overflow by trimming the
// subtexture's UV rect to the box's aspect ratio. (C2D_DrawImageAt with separate
// sx/sy would instead *stretch* the art, smearing portrait covers into the card.)
static void drawImageCover(const C2D_Image& im, float x, float y, float w, float h) {
    const float iw = (float)im.subtex->width;
    const float ih = (float)im.subtex->height;
    if (iw <= 0 || ih <= 0) return;

    // Work on a copy of the subtexture so we can shrink its UV window.
    Tex3DS_SubTexture st = *im.subtex;
    const float imgA  = iw / ih;
    const float boxA  = w / h;

    if (imgA > boxA) {
        // Source is wider than the box → keep full height, crop left/right.
        const float keep = boxA / imgA;                  // fraction of width shown
        const float trim = (st.right - st.left) * (1.0f - keep) * 0.5f;
        st.left  += trim;
        st.right -= trim;
        st.width  = (u16)(iw * keep);
    } else {
        // Source is taller than the box → keep full width, crop top/bottom.
        const float keep = imgA / boxA;                  // fraction of height shown
        const float trim = (st.top - st.bottom) * (1.0f - keep) * 0.5f;  // top > bottom
        st.top    -= trim;
        st.bottom += trim;
        st.height  = (u16)(ih * keep);
    }

    C2D_Image cropped = { im.tex, &st };
    C2D_DrawImageAt(cropped, x, y, 0.0f, nullptr, w / (float)st.width, h / (float)st.height);
}

// Bottom-screen "Continue Watching" poster strip for the library grid view.
void UI::drawResumeStrip(const std::vector<JellyfinItem>& resume,
                         const std::vector<C2D_Image>& covers,
                         int sel, int offset, bool focus) {
    drawText("Continue Watching", 8, 6, 0.50f, focus ? COL_WHITE : COL_GREY);

    if (resume.empty()) {
        drawText("Nothing in progress", 8, 100, 0.46f, COL_GREY);
        return;
    }

    const int   vis    = RESUME_VISIBLE;
    const float mx     = 8.0f;
    const float gap    = 8.0f;
    const float cardW  = (BOT_W - 2 * mx - (vis - 1) * gap) / vis;  // ~70
    const float postH  = 104.0f;
    const float top    = 28.0f;

    for (int i = 0; i < vis; i++) {
        int idx = offset + i;
        if (idx >= (int)resume.size()) break;

        float x   = mx + i * (cardW + gap);
        bool  hl  = focus && idx == sel;

        if (hl) drawRect(x - 3, top - 3, cardW + 6, postH + 6, COL_SEL);

        bool hasImg = (idx < (int)covers.size() && covers[idx].tex != nullptr);
        if (hasImg) drawImageCover(covers[idx], x, top, cardW, postH);
        else        drawRect(x, top, cardW, postH, COL_ROW_ALT);

        // Progress bar pinned to the bottom of the poster.
        float frac = 0.0f;
        if (resume[idx].runTimeTicks > 0)
            frac = (float)resume[idx].resumeTicks / (float)resume[idx].runTimeTicks;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        drawRect(x, top + postH - 5, cardW,        5, C2D_Color32(0, 0, 0, 0xC0));
        drawRect(x, top + postH - 5, cardW * frac, 5, COL_YELLOW);

        // Episodes read better as series name; movies use their own title.
        const std::string& label = !resume[idx].seriesName.empty()
                                  ? resume[idx].seriesName : resume[idx].name;
        drawTextBuf(label, x, top + postH + 4, 0.40f,
                    hl ? COL_WHITE : COL_GREY, cardW);
    }

    // Horizontal scroll indicator when more items exist than fit.
    if ((int)resume.size() > vis) {
        float barW   = BOT_W - 16;
        float thumbW = barW * vis / resume.size();
        float thumbX = 8 + barW * offset / resume.size();
        float y      = top + postH + 22;
        drawRect(8,      y, barW,   3, COL_ROW_ALT);
        drawRect(thumbX, y, thumbW, 3, COL_GREY);
    }
}

void UI::drawLibraryGrid(const std::vector<JellyfinLibrary>& libs,
                         const std::vector<C2D_Image>& covers,
                         int selected, int offset,
                         const std::vector<JellyfinItem>& resume,
                         const std::vector<C2D_Image>& resumeCovers,
                         int resumeSel, int resumeOffset, bool resumeFocus) {
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
        // Only the focused screen shows a selection: once the d-pad moves down
        // into the resume strip, the grid drops its outline (mirrors the way
        // drawResumeStrip gates its own highlight on focus).
        bool  sel = !resumeFocus && idx == selected;

        if (sel) drawRect(x - 3, y - 3, cardW + 6, cardH + 6, COL_SEL);

        bool hasImg = (idx < (int)covers.size() && covers[idx].tex != nullptr);
        if (hasImg) {
            drawImageCover(covers[idx], x, y, cardW, cardH);
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
    drawResumeStrip(resume, resumeCovers, resumeSel, resumeOffset, resumeFocus);

    if (resumeFocus)
        drawBottomHints("A: Resume   LEFT/RIGHT: Select   UP: Libraries");
    else if (!resume.empty())
        drawBottomHints("A: Open   D-Pad: Move   DOWN: Continue Watching");
    else
        drawBottomHints("A: Open   D-Pad: Move   START: Quit");
}

void UI::drawItemGrid(const std::vector<JellyfinItem>& items,
                      const std::vector<C2D_Image>& covers,
                      int selected, int offset,
                      const std::string& title) {
    C2D_SceneBegin(top_);
    drawTopBar(title);

    const int   cols   = ITEM_GRID_COLS;
    const int   rowsV  = ITEM_GRID_ROWS_VISIBLE;
    const float mx     = 12.0f;
    const float my     = 28.0f;
    const float gap    = 10.0f;
    const float cardW  = (TOP_W - 2 * mx - (cols - 1) * gap) / cols;  // ~86
    const float cardH  = 96.0f;
    const float pitchY = cardH + 12.0f;

    if (items.empty())
        drawText("(no items found)", 8, 110, 0.50f, COL_GREY);

    for (int i = 0; i < cols * rowsV; i++) {
        int idx = offset * cols + i;
        if (idx >= (int)items.size()) break;

        int   c = i % cols, r = i / cols;
        float x = mx + c * (cardW + gap);
        float y = my + r * pitchY;
        bool  sel = (idx == selected);

        if (sel) drawRect(x - 3, y - 3, cardW + 6, cardH + 6, COL_SEL);

        bool hasImg = (idx < (int)covers.size() && covers[idx].tex != nullptr);
        if (hasImg) drawImageCover(covers[idx], x, y, cardW, cardH);
        else        drawRect(x, y, cardW, cardH, COL_ROW_ALT);

        // Resume progress for partially-watched items.
        if (items[idx].runTimeTicks > 0 && items[idx].resumeTicks > 0) {
            float frac = (float)items[idx].resumeTicks / (float)items[idx].runTimeTicks;
            if (frac > 1.0f) frac = 1.0f;
            drawRect(x, y + cardH - 4, cardW,        4, C2D_Color32(0, 0, 0, 0xC0));
            drawRect(x, y + cardH - 4, cardW * frac, 4, COL_YELLOW);
        }

        // Title strip across the bottom of the card.
        drawRect(x, y + cardH - 20, cardW, 16, C2D_Color32(0, 0, 0, 0xB0));
        drawTextBuf(items[idx].name, x + 4, y + cardH - 18, 0.38f, COL_WHITE, cardW - 8);
    }

    // Scrollbar when there are more rows than fit.
    int totalRows = ((int)items.size() + cols - 1) / cols;
    if (totalRows > rowsV) {
        float barH   = TOP_H - 26;
        float thumbH = barH * rowsV / totalRows;
        float thumbY = 26 + barH * offset / totalRows;
        drawRect(TOP_W - 4, 26,     4, barH,   COL_ROW_ALT);
        drawRect(TOP_W - 4, thumbY, 4, thumbH, COL_GREY);
    }

    // Bottom screen: metadata for the selected item.
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

    // Series drill in, everything else plays.
    bool drillable = !items.empty() && selected < (int)items.size()
                  && items[selected].type == "Series";
    drawBottomHints(drillable ? "A: Open   B: Back   D-Pad: Move"
                              : "A: Play   B: Back   D-Pad: Move");
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
