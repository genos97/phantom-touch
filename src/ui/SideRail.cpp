#include "ui/SideRail.hpp"
#include "ui/Theme.hpp"

namespace {

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

int dp(int px) {
    return GdiPlusDraw::dp(px);
}

} // namespace

void SideRail::layout(const RECT& bounds, const SideRailState& st) {
    items_.clear();

    struct RailSpec {
        SideRailAction act;
        UiIcon icon;
        const char* label;
        const char* tip;
        bool active;
        uint32_t activeCol;
        bool enabled;
    };

    RailSpec playItems[] = {
        { SR_AIM_TOGGLE,      ICON_AIM,        "AIM",  "Aim Lock (~ / F1)",              st.aimMode, Theme::ACCENT_GREEN, true },
        { SR_HUD_TOGGLE,      ICON_HUD,        "HUD",  "Show / Hide Key Badges (F4)",    st.showHud, Theme::ACCENT_CYAN, true },
        { SR_FIRE_CYCLE,      ICON_FIRE,       "FIRE", "Cycle Fire Mode (F3)",           false, Theme::ACCENT_AMBER, true },
        { SR_RESET_TOUCH,     ICON_RESET,      "RST",  "Anti-Stuck Touch Reset (F5)",    false, Theme::ACCENT_AMBER, true },
        { SR_FULLSCREEN,      ICON_FULLSCREEN, "FULL", "Fullscreen Gaming (F11)",        st.fullscreen, Theme::ACCENT_CYAN, true }
    };

    RailSpec studioItems[] = {
        { SR_DRAWER_KEYS,     ICON_KEYMAP,     "KEYS", "Keymap Studio (F2)",             st.drawerOpen && st.activeDrawerTab == 0, Theme::ACCENT_CYAN, true },
        { SR_DRAWER_SENS,     ICON_SENS,       "SENS", "Sensitivity & Aim Physics",      st.drawerOpen && st.activeDrawerTab == 1, Theme::ACCENT_CYAN, true },
        { SR_DRAWER_PROFILES, ICON_PROFILES,   "CFG",  "Saved Profiles (Ctrl+K)",        st.drawerOpen && st.activeDrawerTab == 2, Theme::ACCENT_CYAN, true }
    };

    RailSpec devItems[] = {
        { SR_ANDROID_BACK,    ICON_BACK,       "BACK", "Android Back",                   false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_HOME,    ICON_HOME,       "HOME", "Android Home",                   false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_RECENTS, ICON_RECENTS,    "APPS", "Android Recents",                false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_VOL_UP,  ICON_VOL_UP,     "VOL+", "Volume Up",                      false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_VOL_DN,  ICON_VOL_DN,     "VOL-", "Volume Down",                    false, Theme::ACCENT_CYAN, st.connected },
        { SR_SCREENSHOT,      ICON_CAMERA,     "SHOT", "Capture Phone Screenshot",       false, Theme::ACCENT_GREEN, st.connected },
        { SR_DRAWER_DEVICE,   ICON_SETTINGS,   "DEV",  "Device, Wireless ADB & Stream",  st.drawerOpen && st.activeDrawerTab == 3, Theme::ACCENT_CYAN, true }
    };

    const int totalH = bounds.bottom - bounds.top;
    const int nPlay = 5;
    const int nStudio = 3;
    const int nDev = 7;
    const int totalItems = nPlay + nStudio + nDev;
    int btnH = (totalH >= dp(700)) ? dp(39) : dp(33);
    int gap  = (totalH >= dp(700)) ? dp(3)  : dp(2);
    if (totalItems * (btnH + gap) + dp(32) > totalH) {
        btnH = (totalH - dp(32)) / totalItems - 1;
        if (btnH < dp(24)) btnH = dp(24);
        gap = 1;
    }

    int y = bounds.top + dp(6);
    for (int i = 0; i < nPlay; ++i) {
        RECT r = { bounds.left + dp(5), y, bounds.right - dp(5), y + btnH };
        items_.push_back(Item{ r, playItems[i].act, playItems[i].icon, playItems[i].label,
                               playItems[i].tip, playItems[i].active, playItems[i].activeCol, playItems[i].enabled });
        y += btnH + gap;
    }

    sepPlayY_ = y + dp(4);
    y += dp(9);
    for (int i = 0; i < nStudio; ++i) {
        RECT r = { bounds.left + dp(5), y, bounds.right - dp(5), y + btnH };
        items_.push_back(Item{ r, studioItems[i].act, studioItems[i].icon, studioItems[i].label,
                               studioItems[i].tip, studioItems[i].active, studioItems[i].activeCol, studioItems[i].enabled });
        y += btnH + gap;
    }

    int botBlockH = nDev * (btnH + gap) + dp(6);
    int botStartY = bounds.bottom - botBlockH;
    if (botStartY < y + dp(10)) botStartY = y + dp(10);
    sepDevY_ = botStartY - dp(5);

    int by = botStartY;
    for (int i = 0; i < nDev; ++i) {
        if (by + btnH > bounds.bottom - 2) break;
        RECT r = { bounds.left + dp(5), by, bounds.right - dp(5), by + btnH };
        items_.push_back(Item{ r, devItems[i].act, devItems[i].icon, devItems[i].label,
                               devItems[i].tip, devItems[i].active, devItems[i].activeCol, devItems[i].enabled });
        by += btnH + gap;
    }
}

void SideRail::paint(Gdiplus::Graphics& g, const RECT& bounds,
                     const SideRailState& st, int hoverX, int hoverY) {
    layout(bounds, st);

    GdiPlusDraw::fillRect(g, bounds, Theme::BG_SHELL);
    GdiPlusDraw::vLine(g, bounds.left, bounds.top, bounds.bottom, Theme::BORDER_CHROME);
    GdiPlusDraw::hLine(g, bounds.left + dp(10), bounds.right - dp(10), sepPlayY_, Theme::BORDER_SUBTLE);
    GdiPlusDraw::hLine(g, bounds.left + dp(10), bounds.right - dp(10), sepDevY_, Theme::BORDER_SUBTLE);

    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& sp = items_[i];
        const RECT& r = sp.rc;
        const int btnH = r.bottom - r.top;
        bool hov = sp.enabled && ptIn(r, hoverX, hoverY);
        uint32_t bg = sp.active
            ? ((sp.activeCol == Theme::ACCENT_GREEN) ? Theme::ACCENT_GREEN_BG : Theme::ACCENT_CYAN_BG)
            : (hov ? Theme::BG_CARD_HOVER : 0x00000000u);
        uint32_t bd = sp.active ? sp.activeCol : (hov ? Theme::BORDER_STRONG : 0x00000000u);
        if ((bg >> 24) != 0 || (bd >> 24) != 0) {
            GdiPlusDraw::roundedCard(g, r, GdiPlusDraw::dpf(8.0f), bg, bd, sp.active ? 1.3f : 1.0f);
        }
        if (sp.active) {
            RECT pill = { bounds.left + 1, r.top + dp(7), bounds.left + dp(4), r.bottom - dp(7) };
            GdiPlusDraw::fillRect(g, pill, sp.activeCol);
        }

        uint32_t fg = !sp.enabled ? 0xFF3E4B61u
                    : sp.active   ? sp.activeCol
                    : hov         ? Theme::TEXT_PRIMARY
                                  : Theme::TEXT_SECONDARY;

        float cx = static_cast<float>(r.left + r.right) * 0.5f;
        if (btnH >= dp(34)) {
            GdiPlusDraw::drawIcon(g, sp.icon, cx, static_cast<float>(r.top) + btnH * 0.36f, GdiPlusDraw::dpf(15.0f), fg);
            GdiPlusDraw::textCenter(g, sp.label, cx, static_cast<float>(r.bottom) - GdiPlusDraw::dpf(7.5f), 8.5f, true, fg);
        } else {
            GdiPlusDraw::drawIcon(g, sp.icon, cx, static_cast<float>(r.top + r.bottom) * 0.5f, GdiPlusDraw::dpf(14.0f), fg);
        }
    }
}

SideRailAction SideRail::hitTest(int x, int y) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].enabled && ptIn(items_[i].rc, x, y)) {
            return items_[i].action;
        }
    }
    return SR_NONE;
}

bool SideRail::hoverItemAt(int x, int y, int& outId, RECT& outRc) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].enabled && ptIn(items_[i].rc, x, y)) {
            outId = static_cast<int>(items_[i].action);
            outRc = items_[i].rc;
            return true;
        }
    }
    return false;
}

bool SideRail::itemRectAt(int x, int y, RECT& outRc) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].enabled && ptIn(items_[i].rc, x, y)) {
            outRc = items_[i].rc;
            return true;
        }
    }
    return false;
}

std::string SideRail::tooltipAt(int x, int y) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].enabled && ptIn(items_[i].rc, x, y)) {
            return items_[i].tip;
        }
    }
    return "";
}
