#pragma once
#include <citro2d.h>
#include <string>
#include <vector>
#include "jellyfin.h"

class UI {
public:
    static constexpr int VISIBLE_ROWS  = 9;   // rows that fit on top screen
    static constexpr int ROW_HEIGHT    = 24;
    static constexpr int TOP_W         = 400;
    static constexpr int TOP_H         = 240;
    static constexpr int BOT_W         = 320;
    static constexpr int BOT_H         = 240;

    // Library home-screen grid (2-wide landscape cards)
    static constexpr int GRID_COLS         = 2;
    static constexpr int GRID_ROWS_VISIBLE = 2;

    // "Continue Watching" poster strip on the bottom screen of the grid view.
    static constexpr int RESUME_VISIBLE    = 4;   // poster cards shown at once

    // Item cover grid (movies / series / episodes inside a library)
    static constexpr int ITEM_GRID_COLS         = 4;
    static constexpr int ITEM_GRID_ROWS_VISIBLE = 2;

    UI(C3D_RenderTarget* top, C3D_RenderTarget* bot);
    ~UI();

    void beginFrame();
    void endFrame();

    void drawSetupScreen(const std::string& currentUrl);
    void drawLoginScreen(const std::string& serverUrl, const std::string& username);
    void drawLoadingScreen(const std::string& msg);
    void drawErrorScreen(const std::string& msg);

    // Grid of library cover-art cards. covers is parallel to libs; a card whose
    // covers[i].tex is null falls back to a colored placeholder. The bottom screen
    // shows a "Continue Watching" poster strip built from resume/resumeCovers;
    // resumeFocus selects whether the d-pad is currently driving the grid or strip.
    void drawLibraryGrid(const std::vector<JellyfinLibrary>& libs,
                         const std::vector<C2D_Image>& covers,
                         int selected, int offset,
                         const std::vector<JellyfinItem>& resume,
                         const std::vector<C2D_Image>& resumeCovers,
                         int resumeSel, int resumeOffset, bool resumeFocus);

    // Cover grid of the items inside a library level (movies, series, or
    // episodes). covers is parallel to items; a null tex falls back to a colored
    // placeholder. title is shown in the top bar (library or series name).
    void drawItemGrid(const std::vector<JellyfinItem>& items,
                      const std::vector<C2D_Image>& covers,
                      int selected, int offset,
                      const std::string& title);

    void drawPlayerScreen(const JellyfinItem& item, const std::string& streamUrl);

private:
    C3D_RenderTarget* top_;
    C3D_RenderTarget* bot_;
    C2D_Font          font_;
    C2D_TextBuf       textBuf_;

    void drawText(const std::string& str, float x, float y, float scale, u32 color);
    void drawTextBuf(const std::string& str, float x, float y, float scale, u32 color,
                     float maxWidth);
    void drawRect(float x, float y, float w, float h, u32 color);
    void drawTopBar(const std::string& title);
    void drawBottomHints(const std::string& hints);
    void drawScrollList(const std::vector<std::string>& rows,
                        int selected, int offset);
    void drawResumeStrip(const std::vector<JellyfinItem>& resume,
                         const std::vector<C2D_Image>& covers,
                         int sel, int offset, bool focus);

    static std::string formatDuration(long long ticks);
    static std::string truncate(const std::string& s, size_t maxLen);
};
