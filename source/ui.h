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

    UI(C3D_RenderTarget* top, C3D_RenderTarget* bot);
    ~UI();

    void beginFrame();
    void endFrame();

    void drawSetupScreen(const std::string& currentUrl);
    void drawLoginScreen(const std::string& serverUrl, const std::string& username);
    void drawLoadingScreen(const std::string& msg);
    void drawErrorScreen(const std::string& msg);

    void drawLibraryList(const std::vector<JellyfinLibrary>& libs,
                         int selected, int offset);

    void drawItemList(const std::vector<JellyfinItem>& items,
                      int selected, int offset,
                      const std::string& libraryName);

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

    static std::string formatDuration(long long ticks);
    static std::string truncate(const std::string& s, size_t maxLen);
};
