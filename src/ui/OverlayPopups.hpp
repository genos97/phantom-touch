#pragma once

#include "config/Profile.hpp"
#include "ui/GdiPlusDraw.hpp"

#include <cstdint>
#include <string>
#include <vector>

// X5: Command Palette entry & fuzzy matching model
struct CommandEntry {
    std::string id;
    std::string title;
    std::string keywords;
    std::string hotkey;
    std::string category;
};

class CommandPaletteModel {
public:
    void setCommands(const std::vector<CommandEntry>& cmds) { cmds_ = cmds; }
    const std::vector<CommandEntry>& commands() const { return cmds_; }

    static int fuzzyScore(const std::string& text, const std::string& query);
    std::vector<int> filter(const std::string& query) const;

    void paint(Gdiplus::Graphics& g, int w, int h,
               const std::string& query, const std::vector<int>& filtered,
               int selectedRow, int hoverRow,
               std::vector<RECT>& outRowRects) const;

private:
    std::vector<CommandEntry> cmds_;
};

// X6: Live Keybinding Cheat Sheet model (generated dynamically from current Profile + hotkeys)
struct CheatRow {
    std::string key;
    std::string desc;
    uint32_t accent;

    CheatRow() : accent(0xFF00D4FFu) {}
    CheatRow(const std::string& k, const std::string& d, uint32_t c)
        : key(k), desc(d), accent(c) {}
};

struct CheatSheetData {
    std::string profileName;
    bool keymapEnabled;
    std::vector<CheatRow> profileBindings;
    std::vector<CheatRow> globalHotkeys;

    CheatSheetData() : keymapEnabled(false) {}
};

class CheatSheetModel {
public:
    static CheatSheetData build(const Profile& profile);
    static void paint(Gdiplus::Graphics& g, int w, int h, const CheatSheetData& data);
};

// X7: TopBar searchable profile switcher dropdown model
struct ProfileMenuItem {
    std::string path;
    std::string displayName;
    std::string appPackage;
    bool active;
    bool recent;

    ProfileMenuItem() : active(false), recent(false) {}
};

class ProfileDropdownModel {
public:
    static std::vector<int> filter(const std::vector<ProfileMenuItem>& items, const std::string& query);
    static void paint(Gdiplus::Graphics& g, int w, int h,
                      const std::string& query,
                      const std::vector<ProfileMenuItem>& items,
                      const std::vector<int>& filtered,
                      int selectedRow, int hoverRow,
                      std::vector<RECT>& outItemRects,
                      RECT& outManageRc);
};

// U4 / N14: Owned top-level layered tooltip popup (always renders above native D3D11 video!)
class LayeredTooltipWindow {
public:
    LayeredTooltipWindow();
    ~LayeredTooltipWindow();

    bool create(HWND ownerHwnd, HINSTANCE hInst);
    void destroy();

    void showAt(int screenX, int screenY, const std::string& text, bool alignRightOfAnchor = false);
    void hide();
    bool isVisible() const { return visible_; }
    const std::string& text() const { return text_; }

private:
    void freeSurface();
    bool ensureSurface(int w, int h);

    HWND ownerHwnd_ = NULL;
    HWND hwnd_ = NULL;
    HDC dibDC_ = NULL;
    HBITMAP dibBmp_ = NULL;
    HGDIOBJ dibOldBmp_ = NULL;
    void* dibBits_ = NULL;
    int dibW_ = 0;
    int dibH_ = 0;
    bool visible_ = false;
    std::string text_;
    int lastX_ = -99999;
    int lastY_ = -99999;
    bool lastAlignRight_ = false;
};

// X5 / X6 / X7: Lightweight non-activating owned layered modal/popup surface above D3D11 video
class IOverlayPopupHost {
public:
    virtual ~IOverlayPopupHost() {}
    virtual void onPopupPaint(Gdiplus::Graphics& g, int w, int h) = 0;
    virtual void onPopupClick(int x, int y) = 0;
    virtual void onPopupHover(int x, int y) = 0;
    virtual void onPopupWheel(int delta) = 0;
};

class LayeredModalPopup {
public:
    LayeredModalPopup();
    ~LayeredModalPopup();

    bool create(HWND ownerHwnd, HINSTANCE hInst, IOverlayPopupHost* host);
    void destroy();

    void showBounds(const RECT& screenRc);
    void repaint();
    void hide();

    bool isVisible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }
    int width() const { return dibW_; }
    int height() const { return dibH_; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void freeSurface();
    bool ensureSurface(int w, int h);

    HWND ownerHwnd_ = NULL;
    HWND hwnd_ = NULL;
    IOverlayPopupHost* host_ = nullptr;
    HDC dibDC_ = NULL;
    HBITMAP dibBmp_ = NULL;
    HGDIOBJ dibOldBmp_ = NULL;
    void* dibBits_ = NULL;
    int dibW_ = 0;
    int dibH_ = 0;
    RECT screenRc_;
    bool visible_ = false;
};
