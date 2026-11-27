#pragma once

#include "ui/GdiPlusDraw.hpp"
#include "ui/Theme.hpp"

#include <string>
#include <vector>

struct SessionTabInfo {
    int index = 0;
    std::string title;
    std::string subtitle;
    bool active = false;
    bool connected = false;
    bool connecting = false;
    bool reconnecting = false;
    bool canClose = true;
};

enum SessionTabAction {
    STAB_NONE = 0,
    STAB_SELECT = 1,
    STAB_CLOSE = 2,
    STAB_ADD = 3
};

struct SessionTabHit {
    SessionTabAction action = STAB_NONE;
    int tabIndex = -1;
    SessionTabHit() = default;
    SessionTabHit(SessionTabAction a, int idx) : action(a), tabIndex(idx) {}
};

// Multi-phone F15 tab strip rendered directly under TopBar when >= 2 tabs are open (up to 4 phones).
class SessionTabBar {
public:
    static const int kMaxTabs = 4;
    static const int kBarHeight = 30;

    void layout(const RECT& bounds, const std::vector<SessionTabInfo>& tabs, bool allowAdd = true);
    void paint(Gdiplus::Graphics& g, const RECT& bounds,
               const std::vector<SessionTabInfo>& tabs, bool allowAdd,
               int hoverX, int hoverY);

    SessionTabHit hitTest(int x, int y) const;
    bool hoverItemAt(int x, int y, int& outId, RECT& outRc) const;
    std::string tooltipAt(int x, int y) const;
    bool isInteractivePoint(int x, int y) const { return hitTest(x, y).action != STAB_NONE; }

    int tabCount() const { return static_cast<int>(items_.size()); }
    RECT tabRect(int idx) const;
    RECT addButtonRect() const { return addBtnRc_; }

private:
    struct ItemLayout {
        int index = 0;
        RECT tabRc = { 0, 0, 0, 0 };
        RECT closeRc = { 0, 0, 0, 0 };
        std::string label;
        std::string tooltip;
        bool active = false;
        bool connected = false;
        bool connecting = false;
        bool reconnecting = false;
        bool canClose = true;
    };

    RECT bounds_ = { 0, 0, 0, 0 };
    std::vector<ItemLayout> items_;
    RECT addBtnRc_ = { 0, 0, 0, 0 };
    bool hasAddBtn_ = false;
};
