#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <windows.h>
#include <winerror.h>
#include <ole2.h>
#include <gdiplus.h>

#include <cstdint>
#include <string>

// Vector icons rendered cleanly via anti-aliased GDI+ paths (zero missing font glyphs!)
enum UiIcon {
    ICON_LOGO = 0,
    ICON_AIM,
    ICON_KEYMAP,
    ICON_SENS,
    ICON_PROFILES,
    ICON_HUD,
    ICON_FIRE,
    ICON_RESET,
    ICON_FULLSCREEN,
    ICON_BACK,
    ICON_HOME,
    ICON_RECENTS,
    ICON_VOL_UP,
    ICON_VOL_DN,
    ICON_CAMERA,
    ICON_SETTINGS,
    ICON_PIN,
    ICON_MINIMIZE,
    ICON_MAXIMIZE,
    ICON_CLOSE,
    ICON_USB,
    ICON_WIFI,
    ICON_PLAY,
    ICON_REFRESH
};

// Pure geometry struct for aspect-ratio viewport layout (unit-testable without HWND)
struct ViewportRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// RAII GDI+ lifetime guard (create once in Application::run)
class GdiPlusSession {
public:
    GdiPlusSession();
    ~GdiPlusSession();
    bool ok() const { return started_; }
private:
    ULONG_PTR token_ = 0;
    bool started_ = false;
};

class GdiPlusDraw {
public:
    static Gdiplus::Color argb(uint32_t c);
    static std::wstring toWide(const std::string& utf8OrAscii);

    static void addRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r);

    static void fillRoundedRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, uint32_t fillArgb);
    static void strokeRoundedRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, uint32_t strokeArgb, float width = 1.0f);
    static void roundedCard(Gdiplus::Graphics& g, const RECT& rc, float r,
                            uint32_t fillArgb, uint32_t borderArgb, float borderWidth = 1.0f);
    static void fillRect(Gdiplus::Graphics& g, const RECT& rc, uint32_t fillArgb);
    static void hLine(Gdiplus::Graphics& g, int x1, int x2, int y, uint32_t argbColor);
    static void vLine(Gdiplus::Graphics& g, int x, int y1, int y2, uint32_t argbColor);

    static void textLeft(Gdiplus::Graphics& g, const std::string& text,
                         float x, float cy, float sizePx, bool bold, uint32_t colorArgb, float maxW = 0.0f);
    static void textCenter(Gdiplus::Graphics& g, const std::string& text,
                           float cx, float cy, float sizePx, bool bold, uint32_t colorArgb);
    static void textRight(Gdiplus::Graphics& g, const std::string& text,
                          float rightX, float cy, float sizePx, bool bold, uint32_t colorArgb);

    static void drawToggleSwitch(Gdiplus::Graphics& g, const RECT& rc, bool on, uint32_t accentArgb);
    static void drawSlider(Gdiplus::Graphics& g, const RECT& trackRc, float norm01, uint32_t accentArgb, bool hovered);
    static void drawIcon(Gdiplus::Graphics& g, UiIcon icon, float cx, float cy, float sz, uint32_t colorArgb);

    // Pure math helper: fits a phone screen (phoneW x phoneH) into a viewport host box preserving exact aspect ratio
    static ViewportRect computeAspectFit(int hostW, int hostH, uint32_t phoneW, uint32_t phoneH, int padding = 0);

    // Enable Windows 11 dark titlebar / native rounded corners dynamically via dwmapi.dll
    static void enableDarkDwmFrame(HWND hwnd);
};
