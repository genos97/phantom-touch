#include "ui/SideRail.hpp"
#include "ui/Theme.hpp"

namespace {

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

} // namespace

void SideRail::paint(Gdiplus::Graphics& g, const RECT& bounds,
                     const SideRailState& st, int hoverX, int hoverY) {
    items_.clear();

    GdiPlusDraw::fillRect(g, bounds, Theme::BG_SHELL);
    GdiPlusDraw::vLine(g, bounds.left, bounds.top, bounds.bottom, Theme::BORDER_CHROME);

    struct RailSpec {
        SideRailAction act;
        UiIcon icon;
        const char* label;
        const char* tip;
        bool active;
        uint32_t activeCol;
        bool enabled;
    };

    RailSpec topItems[] = {
        { SR_AIM_TOGGLE,      ICON_AIM,        "AIM",  "Aim Lock (~ / F1)",              st.aimMode, Theme::ACCENT_GREEN, true },
        { SR_DRAWER_KEYS,     ICON_KEYMAP,     "KEYS", "Keymap Studio (F2)",             st.drawerOpen && st.activeDrawerTab == 0, Theme::ACCENT_CYAN, true },
        { SR_DRAWER_SENS,     ICON_SENS,       "SENS", "Sensitivity & Aim Physics",      st.drawerOpen && st.activeDrawerTab == 1, Theme::ACCENT_CYAN, true },
        { SR_DRAWER_PROFILES, ICON_PROFILES,   "CFG",  "Saved Profiles",                 st.drawerOpen && st.activeDrawerTab == 2, Theme::ACCENT_CYAN, true },
        { SR_HUD_TOGGLE,      ICON_HUD,        "HUD",  "Show / Hide Key Badges (F4)",    st.showHud, Theme::ACCENT_CYAN, true },
        { SR_FIRE_CYCLE,      ICON_FIRE,       "FIRE", "Cycle Fire Mode (F3)",           false, Theme::ACCENT_AMBER, true },
        { SR_RESET_TOUCH,     ICON_RESET,      "RST",  "Anti-Stuck Touch Reset (F5)",    false, Theme::ACCENT_AMBER, true },
        { SR_FULLSCREEN,      ICON_FULLSCREEN, "FULL", "Fullscreen Gaming (F11)",        st.fullscreen, Theme::ACCENT_CYAN, true }
    };

    RailSpec botItems[] = {
        { SR_ANDROID_BACK,    ICON_BACK,       "BACK", "Android Back",                   false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_HOME,    ICON_HOME,       "HOME", "Android Home",                   false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_RECENTS, ICON_RECENTS,    "APPS", "Android Recents",                false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_VOL_UP,  ICON_VOL_UP,     "VOL+", "Volume Up",                      false, Theme::ACCENT_CYAN, st.connected },
        { SR_ANDROID_VOL_DN,  ICON_VOL_DN,     "VOL-", "Volume Down",                    false, Theme::ACCENT_CYAN, st.connected },
        { SR_SCREENSHOT,      ICON_CAMERA,     "SHOT", "Capture Phone Screenshot",       false, Theme::ACCENT_GREEN, st.connected },
        { SR_DRAWER_DEVICE,   ICON_SETTINGS,   "DEV",  "Device, Wireless ADB & Stream",  st.drawerOpen && st.activeDrawerTab == 3, Theme::ACCENT_CYAN, true }
    };

    const int totalH = bounds.bottom - bounds.top;
    const int nTop = 8;
    const int nBot = 7;
    int btnH = (totalH >= 680) ? 40 : 34;
    int gap  = (totalH >= 680) ? 3 : 2;
    if ((nTop + nBot) * (btnH + gap) + 22 > totalH) {
        btnH = (totalH - 22) / (nTop + nBot) - 1;
        if (btnH < 26) btnH = 26;
        gap = 1;
    }

    auto drawBtn = [&](int y, const RailSpec& sp) {
        RECT r = { bounds.left + 5, y, bounds.right - 5, y + btnH };
        bool hov = sp.enabled && ptIn(r, hoverX, hoverY);
        uint32_t bg = sp.active
            ? ((sp.activeCol == Theme::ACCENT_GREEN) ? Theme::ACCENT_GREEN_BG : Theme::ACCENT_CYAN_BG)
            : (hov ? Theme::BG_CARD_HOVER : 0x00000000u);
        uint32_t bd = sp.active ? sp.activeCol : (hov ? Theme::BORDER_STRONG : 0x00000000u);
        if ((bg >> 24) != 0 || (bd >> 24) != 0) {
            GdiPlusDraw::roundedCard(g, r, 8.0f, bg, bd, sp.active ? 1.3f : 1.0f);
        }
        if (sp.active) {
            RECT pill = { bounds.left + 1, y + 7, bounds.left + 4, y + btnH - 7 };
            GdiPlusDraw::fillRect(g, pill, sp.activeCol);
        }

        uint32_t fg = !sp.enabled ? 0xFF3E4B61u
                    : sp.active   ? sp.activeCol
                    : hov         ? Theme::TEXT_PRIMARY
                                  : Theme::TEXT_SECONDARY;

        float cx = static_cast<float>(r.left + r.right) * 0.5f;
        if (btnH >= 34) {
            GdiPlusDraw::drawIcon(g, sp.icon, cx, static_cast<float>(r.top) + btnH * 0.36f, 15.0f, fg);
            GdiPlusDraw::textCenter(g, sp.label, cx, static_cast<float>(r.bottom) - 7.5f, 8.5f, true, fg);
        } else {
            GdiPlusDraw::drawIcon(g, sp.icon, cx, static_cast<float>(r.top + r.bottom) * 0.5f, 14.0f, fg);
        }
        if (sp.enabled) {
            items_.push_back(Item{ r, sp.act, sp.tip });
        }
    };

    int y = bounds.top + 6;
    for (int i = 0; i < nTop; ++i) {
        drawBtn(y, topItems[i]);
        y += btnH + gap;
    }

    int botBlockH = nBot * (btnH + gap) + 6;
    int botStartY = bounds.bottom - botBlockH;
    if (botStartY < y + 10) botStartY = y + 10;

    GdiPlusDraw::hLine(g, bounds.left + 10, bounds.right - 10, botStartY - 6, Theme::BORDER_SUBTLE);

    int by = botStartY;
    for (int i = 0; i < nBot; ++i) {
        if (by + btnH > bounds.bottom - 2) break;
        drawBtn(by, botItems[i]);
        by += btnH + gap;
    }
}

SideRailAction SideRail::hitTest(int x, int y) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (ptIn(items_[i].rc, x, y)) {
            return items_[i].action;
        }
    }
    return SR_NONE;
}

std::string SideRail::tooltipAt(int x, int y) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (ptIn(items_[i].rc, x, y)) {
            return items_[i].tip;
        }
    }
    return "";
}
