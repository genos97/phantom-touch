#include "ui/DrawerPanel.hpp"
#include "config/KeyCodes.hpp"
#include "config/ProfileStore.hpp"
#include "ui/Theme.hpp"

#include <cmath>
#include <cstdio>

namespace {

enum DrawerActionId {
    DA_TAB_0 = 200,
    DA_TAB_1,
    DA_TAB_2,
    DA_TAB_3,
    DA_CLOSE,
    DA_SAVE,

    // Keys tab
    DA_ADD_NODE,        // extra = KeyActionType
    DA_CLEAR_BUTTONS,
    DA_INSP_LABEL,
    DA_INSP_REBIND,
    DA_INSP_MOUSE_VK,   // extra = VK
    DA_INSP_ACT_PREV,
    DA_INSP_ACT_NEXT,
    DA_INSP_SCOPE,
    DA_INSP_RSTVIEW,
    DA_INSP_REJOY,
    DA_INSP_DELETE,

    // Sens & Physics tab
    DA_FIRE_CLAW,
    DA_FIRE_DRAG,
    DA_FIRE_DUAL,
    DA_BOUNDS_UNBOUNDED,
    DA_BOUNDS_BOXED,
    DA_POLL_125,
    DA_POLL_250,
    DA_POLL_500,
    DA_POLL_MAX,
    DA_JOY_GLIDE,
    DA_JOY_FAST,
    DA_TOGGLE_INVERT_Y,
    DA_TOGGLE_REMEMBER_CURSOR,
    DA_REBIND_TOGGLE_AIM,
    DA_SET_TOGGLE_MMB,
    DA_REBIND_HOLD_UNLOCK,
    DA_CLEAR_HOLD_UNLOCK,
    DA_RESET_SENS,

    // Profiles tab
    DA_FOCUS_NEW_NAME,
    DA_CREATE_BLANK,
    DA_CREATE_CLONE,
    DA_LOAD_PROFILE,    // extra = idx
    DA_DELETE_PROFILE,  // extra = idx
    DA_TOGGLE_KEYMAP_MODE,
    DA_LINK_CURRENT_APP,
    DA_UNLINK_APP,
    DA_TOGGLE_EDGE_ANCHOR,
    DA_CALIBRATE_ASPECT,

    // Device & Stream tab
    DA_STREAM_FPS,      // extra = 60/90/120
    DA_STREAM_BITRATE,  // extra = 8/12/16/24
    DA_STREAM_MAXSIZE,  // extra = 0/1600/1280
    DA_TOGGLE_NATIVE_VIDEO,
    DA_TOGGLE_VIRTUAL_DISPLAY,
    DA_VD_PRESET,       // extra = 0 (1080p), 1 (720p), 2 (auto)
    DA_TOGGLE_GAMEPAD,
    DA_TOGGLE_AUDIO,
    DA_TOGGLE_SCREEN_OFF,
    DA_TOGGLE_STAY_AWAKE,
    DA_TOGGLE_AUTO_SWITCH,
    DA_TOGGLE_RCLICK_BACK,
    DA_APPLY_RECONNECT,
    DA_DISCONNECT,
    DA_FOCUS_WIRELESS,
    DA_CONNECT_WIRELESS
};

enum DrawerSliderId {
    DSL_SENS_X = 1,
    DSL_SENS_Y,
    DSL_FIRE_X,
    DSL_FIRE_Y,
    DSL_SCOPE_X,
    DSL_SCOPE_Y,
    DSL_FF_CA,
    DSL_SMOOTH,
    DSL_HUD_OPACITY,
    DSL_IDLE_RESET,
    DSL_TAP_JITTER,
    DSL_NODE_DUR
};

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::string baseName(const std::string& p) {
    size_t pos = p.find_last_of("\\/");
    return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

} // namespace

DrawerPanel::DrawerPanel() {
    ZeroMemory(&bounds_, sizeof(bounds_));
    ZeroMemory(&bodyClip_, sizeof(bodyClip_));
    ZeroMemory(&activeSliderTrack_, sizeof(activeSliderTrack_));
    for (int i = 0; i < 4; ++i) scrollY_[i] = 0;
}

void DrawerPanel::setProfilesDir(const std::string& dir, const std::string& activePath) {
    profilesDir_ = dir;
    activeProfilePath_ = activePath;
    refreshProfileList();
}

void DrawerPanel::refreshProfileList() {
    profileEntries_.clear();
    if (profilesDir_.empty()) return;
    std::vector<std::string> files = ProfileStore::listProfiles(profilesDir_);
    for (size_t i = 0; i < files.size(); ++i) {
        ProfileEntry e;
        e.path = files[i];
        e.fileName = baseName(files[i]);
        Profile tmp;
        e.displayName = ProfileStore::load(files[i], tmp) ? tmp.name : e.fileName;
        profileEntries_.push_back(e);
    }
}

void DrawerPanel::setActiveTab(int tab) {
    if (tab < 0) tab = 0;
    if (tab > 3) tab = 3;
    activeTab_ = tab;
    clearFocus();
    if (tab == 2) refreshProfileList();
}

bool DrawerPanel::containsPoint(int x, int y) const {
    return ptIn(bounds_, x, y);
}

void DrawerPanel::drawSliderRow(Gdiplus::Graphics& g, int x, int y, int w,
                                const char* label, float val, float minV, float maxV,
                                const char* valText, uint32_t accent, int sliderId,
                                int hoverX, int hoverY) {
    const int lblW = 104;
    const int valW = 48;
    const int trX = x + lblW;
    const int trW = w - lblW - valW - 6;
    const int cy = y + 13;

    GdiPlusDraw::textLeft(g, label, static_cast<float>(x), static_cast<float>(cy), 11.5f, false, Theme::TEXT_SECONDARY, static_cast<float>(lblW - 4));

    RECT tr = { trX, cy - 3, trX + trW, cy + 3 };
    RECT hit = { trX - 8, y, trX + trW + 8, y + 26 };
    float norm = (maxV > minV) ? clampf((val - minV) / (maxV - minV), 0.0f, 1.0f) : 0.0f;
    bool hov = (activeSliderId_ == sliderId) || ptIn(hit, hoverX, hoverY);
    GdiPlusDraw::drawSlider(g, tr, norm, accent, hov);
    GdiPlusDraw::textRight(g, valText, static_cast<float>(x + w), static_cast<float>(cy), 11.5f, true, accent);

    if (hit.bottom > bodyClip_.top && hit.top < bodyClip_.bottom) {
        sliders_.push_back(SliderHit{ tr, hit, sliderId, minV, maxV });
    }
}

void DrawerPanel::drawToggleRow(Gdiplus::Graphics& g, int x, int y, int w,
                                const char* label, bool on, uint32_t accent, int actionId,
                                int hoverX, int hoverY) {
    RECT row = { x, y, x + w, y + 30 };
    bool hov = ptIn(row, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, row, 7.0f, hov ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textLeft(g, label, static_cast<float>(x + 10), static_cast<float>(y + 15), 11.5f, false, Theme::TEXT_PRIMARY, static_cast<float>(w - 64));
    RECT sw = { x + w - 48, y + 5, x + w - 8, y + 25 };
    GdiPlusDraw::drawToggleSwitch(g, sw, on, accent);
    if (row.bottom > bodyClip_.top && row.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ row, actionId, 0 });
    }
}

void DrawerPanel::paint(Gdiplus::Graphics& g, const RECT& bounds,
                        const Profile& profile, const AppSettings& settings,
                        const std::string& activeProfilePath,
                        int selectedBtnIndex, bool waitingNodeRebind,
                        bool connected, const std::string& deviceLabel,
                        const std::string& foregroundPkg,
                        const std::vector<std::string>& logs,
                        int hoverX, int hoverY) {
    bounds_ = bounds;
    activeProfilePath_ = activeProfilePath;
    hotspots_.clear();
    sliders_.clear();

    GdiPlusDraw::fillRect(g, bounds, Theme::BG_DRAWER);
    GdiPlusDraw::vLine(g, bounds.left, bounds.top, bounds.bottom, Theme::BORDER_STRONG);

    const int x0 = bounds.left + 12;
    const int w = (bounds.right - bounds.left) - 24;
    const int hdrY = bounds.top + 10;

    // 1. Drawer Header + Close button
    static const char* kTitles[4] = {
        "KEYMAP STUDIO",
        "SENSITIVITY & PHYSICS",
        "PROFILES MANAGER",
        "DEVICE & STREAM"
    };
    GdiPlusDraw::textLeft(g, kTitles[activeTab_], static_cast<float>(x0), static_cast<float>(hdrY + 12), 13.0f, true, Theme::ACCENT_CYAN, static_cast<float>(w - 32));

    RECT rCls = { bounds.right - 34, hdrY, bounds.right - 12, hdrY + 24 };
    bool hovCls = ptIn(rCls, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rCls, 6.0f, hovCls ? Theme::ACCENT_RED_BG : Theme::BG_CARD,
                             hovCls ? Theme::ACCENT_RED : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::drawIcon(g, ICON_CLOSE, static_cast<float>(rCls.left + rCls.right) * 0.5f,
                          static_cast<float>(rCls.top + rCls.bottom) * 0.5f, 10.0f,
                          hovCls ? Theme::ACCENT_RED : Theme::TEXT_SECONDARY);
    hotspots_.push_back(Hotspot{ rCls, DA_CLOSE, 0 });

    // 2. 4 Navigation Tabs
    const char* tabNames[4] = { "KEYS", "SENS", "PROFILES", "DEVICE" };
    int tabY = hdrY + 32;
    int tabW = (w - 12) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT tr = { x0 + i * (tabW + 4), tabY, x0 + i * (tabW + 4) + tabW, tabY + 26 };
        bool active = (activeTab_ == i);
        bool hov = ptIn(tr, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, tr, 6.0f,
                                 active ? Theme::ACCENT_CYAN_BG : (hov ? Theme::BG_CARD_HOVER : Theme::BG_CARD),
                                 active ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE,
                                 active ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, tabNames[i],
                                static_cast<float>(tr.left + tr.right) * 0.5f,
                                static_cast<float>(tr.top + tr.bottom) * 0.5f,
                                10.5f, true, active ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        hotspots_.push_back(Hotspot{ tr, DA_TAB_0 + i, 0 });
    }

    int sepTop = tabY + 34;
    int footerH = 46;
    int sepBot = bounds.bottom - footerH;
    GdiPlusDraw::hLine(g, bounds.left + 1, bounds.right, sepTop, Theme::BORDER_CHROME);
    GdiPlusDraw::hLine(g, bounds.left + 1, bounds.right, sepBot, Theme::BORDER_CHROME);

    bodyClip_ = { bounds.left + 1, sepTop + 1, bounds.right - 1, sepBot - 1 };

    // 3. Scrollable Tab Body (clipped strictly to bodyClip_)
    Gdiplus::GraphicsState savedState = g.Save();
    g.SetClip(Gdiplus::Rect(static_cast<INT>(bodyClip_.left),
                            static_cast<INT>(bodyClip_.top),
                            static_cast<INT>(bodyClip_.right - bodyClip_.left),
                            static_cast<INT>(bodyClip_.bottom - bodyClip_.top)));

    int startY = bodyClip_.top + 10 - scrollY_[activeTab_];
    int endY = startY;
    if (activeTab_ == 0) {
        endY = paintTabKeys(g, x0, startY, w, profile, selectedBtnIndex, waitingNodeRebind, hoverX, hoverY);
    } else if (activeTab_ == 1) {
        endY = paintTabSens(g, x0, startY, w, profile, hoverX, hoverY);
    } else if (activeTab_ == 2) {
        endY = paintTabProfiles(g, x0, startY, w, profile, activeProfilePath, foregroundPkg, hoverX, hoverY);
    } else {
        endY = paintTabDevice(g, x0, startY, w, settings, connected, deviceLabel, logs, hoverX, hoverY);
    }

    int contentH = (endY - startY) + 20;
    int viewH = bodyClip_.bottom - bodyClip_.top;
    maxScrollY_ = (contentH > viewH) ? (contentH - viewH) : 0;
    if (scrollY_[activeTab_] > maxScrollY_) scrollY_[activeTab_] = maxScrollY_;
    if (scrollY_[activeTab_] < 0) scrollY_[activeTab_] = 0;

    // Subtle scrollbar thumb when scrollable
    if (maxScrollY_ > 0 && viewH > 30) {
        float thumbH = clampf(static_cast<float>(viewH * viewH) / static_cast<float>(contentH), 28.0f, static_cast<float>(viewH - 8));
        float ratio = static_cast<float>(scrollY_[activeTab_]) / static_cast<float>(maxScrollY_);
        float thumbY = static_cast<float>(bodyClip_.top + 4) + ratio * (static_cast<float>(viewH - 8) - thumbH);
        GdiPlusDraw::fillRoundedRect(g, static_cast<float>(bounds.right - 6), thumbY, 4.0f, thumbH, 2.0f, Theme::BORDER_STRONG);
    }

    g.Restore(savedState);

    // 4. Persistent Footer Bar (Auto-Save Status + Save Now + Done)
    int fy = sepBot + 9;
    GdiPlusDraw::textLeft(g, "Auto-Saved", static_cast<float>(x0), static_cast<float>(fy + 14), 11.0f, true, Theme::ACCENT_GREEN, 90.0f);

    RECT rSave = { x0 + 96, fy, x0 + 214, fy + 28 };
    bool hovSave = ptIn(rSave, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rSave, 7.0f, hovSave ? Theme::ACCENT_GREEN : Theme::ACCENT_GREEN_BG,
                             Theme::ACCENT_GREEN, 1.1f);
    GdiPlusDraw::textCenter(g, "SAVE (Ctrl+S)", static_cast<float>(rSave.left + rSave.right) * 0.5f,
                            static_cast<float>(fy + 14), 11.0f, true, hovSave ? 0xFF07130Du : Theme::TEXT_PRIMARY);
    hotspots_.push_back(Hotspot{ rSave, DA_SAVE, 0 });

    RECT rDone = { rSave.right + 8, fy, bounds.right - 12, fy + 28 };
    bool hovDone = ptIn(rDone, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rDone, 7.0f, hovDone ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG,
                             Theme::ACCENT_CYAN, 1.1f);
    GdiPlusDraw::textCenter(g, "DONE (F2)", static_cast<float>(rDone.left + rDone.right) * 0.5f,
                            static_cast<float>(fy + 14), 11.0f, true, hovDone ? 0xFF07131Au : Theme::TEXT_PRIMARY);
    hotspots_.push_back(Hotspot{ rDone, DA_CLOSE, 0 });
}

int DrawerPanel::paintTabKeys(Gdiplus::Graphics& g, int x, int y, int w,
                              const Profile& p, int selectedBtnIndex, bool waitingRebind,
                              int hoverX, int hoverY) {
    // 1. Docked Selected Key Inspector (ALWAYS visible at top when a key is selected!)
    if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
        const KeyNode& node = p.buttons[selectedBtnIndex];
        RECT card = { x, y, x + w, y + 320 };
        GdiPlusDraw::roundedCard(g, card, 10.0f, Theme::BG_CARD, Theme::ACCENT_CYAN, 1.4f);

        int ix = x + 10;
        int iw = w - 20;
        int cy = y + 10;
        GdiPlusDraw::textLeft(g, "SELECTED KEY INSPECTOR", static_cast<float>(ix), static_cast<float>(cy + 7), 10.5f, true, Theme::ACCENT_CYAN);
        cy += 22;

        // Label field
        RECT rLbl = { ix, cy, ix + iw, cy + 26 };
        GdiPlusDraw::roundedCard(g, rLbl, 6.0f, Theme::BG_INPUT,
                                 editingNodeLabel_ ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG,
                                 editingNodeLabel_ ? 1.5f : 1.0f);
        GdiPlusDraw::textLeft(g, "Label: " + node.label + (editingNodeLabel_ ? "_" : ""),
                              static_cast<float>(ix + 9), static_cast<float>(cy + 13), 12.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(iw - 16));
        if (rLbl.bottom > bodyClip_.top && rLbl.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rLbl, DA_INSP_LABEL, 0 });
        cy += 32;

        // Rebind Key Button
        RECT rBind = { ix, cy, ix + iw, cy + 28 };
        bool hovBind = ptIn(rBind, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rBind, 6.0f,
                                 waitingRebind ? Theme::ACCENT_AMBER_BG : (hovBind ? Theme::BG_CARD_HOVER : Theme::ACCENT_CYAN_BG),
                                 waitingRebind ? Theme::ACCENT_AMBER : Theme::ACCENT_CYAN, 1.2f);
        std::string keyDisplay = (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_UP)   ? "SCROLL UP"
                               : (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_DOWN) ? "SCROLL DOWN"
                                                                                         : KeyCodes::comboToName(node.comboVk, node.vk);
        std::string ktxt = waitingRebind ? "Press Key / Mouse / Scroll..." : ("Key: " + keyDisplay + "  [Rebind]");
        GdiPlusDraw::textCenter(g, ktxt, static_cast<float>(ix + iw / 2), static_cast<float>(cy + 14), 12.0f, true, Theme::TEXT_PRIMARY);
        if (rBind.bottom > bodyClip_.top && rBind.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rBind, DA_INSP_REBIND, 0 });
        cy += 34;

        // Row 1: Mouse buttons (LMB / RMB / MMB / X1 / X2)
        static const char* mLbls[5] = { "LMB", "RMB", "MMB", "X1", "X2" };
        static const int   mVks[5]  = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        int cw = (iw - 16) / 5;
        for (int i = 0; i < 5; ++i) {
            RECT rcM = { ix + i * (cw + 4), cy, ix + i * (cw + 4) + cw, cy + 22 };
            bool on = (node.comboVk == 0 && node.vk == mVks[i]);
            bool hov = ptIn(rcM, hoverX, hoverY);
            GdiPlusDraw::roundedCard(g, rcM, 5.0f,
                                     on ? Theme::ACCENT_CYAN_BG : (hov ? Theme::BG_CARD_HOVER : Theme::BG_INPUT),
                                     on ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
            GdiPlusDraw::textCenter(g, mLbls[i], static_cast<float>(rcM.left + rcM.right) * 0.5f,
                                    static_cast<float>(cy + 11), 10.5f, true, on ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
            if (rcM.bottom > bodyClip_.top && rcM.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rcM, DA_INSP_MOUSE_VK, mVks[i] });
        }
        cy += 27;

        // Row 2: Explicit SCROLL UP / SCROLL DOWN buttons
        int hw = (iw - 6) / 2;
        RECT rUp = { ix, cy, ix + hw, cy + 24 };
        RECT rDn = { ix + hw + 6, cy, ix + iw, cy + 24 };
        bool upOn = (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_UP);
        bool dnOn = (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_DOWN);
        GdiPlusDraw::roundedCard(g, rUp, 5.0f, upOn ? Theme::ACCENT_CYAN_BG : Theme::BG_INPUT, upOn ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, 1.1f);
        GdiPlusDraw::roundedCard(g, rDn, 5.0f, dnOn ? Theme::ACCENT_CYAN_BG : Theme::BG_INPUT, dnOn ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, 1.1f);
        GdiPlusDraw::textCenter(g, "SCROLL UP", static_cast<float>(rUp.left + rUp.right) * 0.5f, static_cast<float>(cy + 12), 10.5f, true, upOn ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        GdiPlusDraw::textCenter(g, "SCROLL DOWN", static_cast<float>(rDn.left + rDn.right) * 0.5f, static_cast<float>(cy + 12), 10.5f, true, dnOn ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        if (rUp.bottom > bodyClip_.top && rUp.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rUp, DA_INSP_MOUSE_VK, VK_CUSTOM_MWHEEL_UP });
            hotspots_.push_back(Hotspot{ rDn, DA_INSP_MOUSE_VK, VK_CUSTOM_MWHEEL_DOWN });
        }
        cy += 30;

        // Action Type stepper
        RECT rPrev = { ix, cy, ix + 28, cy + 26 };
        RECT rAct  = { ix + 32, cy, ix + iw - 32, cy + 26 };
        RECT rNext = { ix + iw - 28, cy, ix + iw, cy + 26 };
        GdiPlusDraw::roundedCard(g, rPrev, 5.0f, ptIn(rPrev, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_INPUT, Theme::BORDER_STRONG, 1.0f);
        GdiPlusDraw::roundedCard(g, rAct,  5.0f, Theme::BG_INPUT, Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::roundedCard(g, rNext, 5.0f, ptIn(rNext, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_INPUT, Theme::BORDER_STRONG, 1.0f);
        GdiPlusDraw::textCenter(g, "<", static_cast<float>(rPrev.left + rPrev.right) * 0.5f, static_cast<float>(cy + 13), 12.0f, true, Theme::TEXT_PRIMARY);
        GdiPlusDraw::textCenter(g, ProfileDefaults::actionTypeToName(node.action), static_cast<float>(rAct.left + rAct.right) * 0.5f, static_cast<float>(cy + 13), 11.5f, true, Theme::ACCENT_GREEN);
        GdiPlusDraw::textCenter(g, ">", static_cast<float>(rNext.left + rNext.right) * 0.5f, static_cast<float>(cy + 13), 12.0f, true, Theme::TEXT_PRIMARY);
        if (rPrev.bottom > bodyClip_.top && rPrev.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rPrev, DA_INSP_ACT_PREV, 0 });
            hotspots_.push_back(Hotspot{ rNext, DA_INSP_ACT_NEXT, 0 });
        }
        cy += 32;

        // Duration slider
        char dbuf[16];
        std::snprintf(dbuf, sizeof(dbuf), "%dms", node.durationMs);
        drawSliderRow(g, ix, cy, iw, "Tap/Swipe Dur", static_cast<float>(node.durationMs), 15.0f, 300.0f, dbuf, Theme::ACCENT_AMBER, DSL_NODE_DUR, hoverX, hoverY);
        cy += 30;

        // Per-node add-on chips: Scope Sens | Reset View | Fix Joy
        int tw = (iw - 8) / 3;
        RECT rScp = { ix, cy, ix + tw, cy + 24 };
        RECT rRst = { ix + tw + 4, cy, ix + 2 * tw + 4, cy + 24 };
        RECT rJoy = { ix + 2 * tw + 8, cy, ix + iw, cy + 24 };
        GdiPlusDraw::roundedCard(g, rScp, 5.0f, node.switchScopeSens ? Theme::ACCENT_PURPLE_BG : Theme::BG_INPUT, node.switchScopeSens ? Theme::ACCENT_PURPLE : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::roundedCard(g, rRst, 5.0f, node.resetView ? Theme::ACCENT_AMBER_BG : Theme::BG_INPUT, node.resetView ? Theme::ACCENT_AMBER : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::roundedCard(g, rJoy, 5.0f, node.reactivateJoy ? Theme::ACCENT_GREEN_BG : Theme::BG_INPUT, node.reactivateJoy ? Theme::ACCENT_GREEN : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textCenter(g, "ADS Sens", static_cast<float>(rScp.left + rScp.right) * 0.5f, static_cast<float>(cy + 12), 10.0f, true, node.switchScopeSens ? Theme::ACCENT_PURPLE : Theme::TEXT_MUTED);
        GdiPlusDraw::textCenter(g, "Rst View", static_cast<float>(rRst.left + rRst.right) * 0.5f, static_cast<float>(cy + 12), 10.0f, true, node.resetView ? Theme::ACCENT_AMBER : Theme::TEXT_MUTED);
        GdiPlusDraw::textCenter(g, "Fix Joy",  static_cast<float>(rJoy.left + rJoy.right) * 0.5f, static_cast<float>(cy + 12), 10.0f, true, node.reactivateJoy ? Theme::ACCENT_GREEN : Theme::TEXT_MUTED);
        if (rScp.bottom > bodyClip_.top && rScp.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rScp, DA_INSP_SCOPE, 0 });
            hotspots_.push_back(Hotspot{ rRst, DA_INSP_RSTVIEW, 0 });
            hotspots_.push_back(Hotspot{ rJoy, DA_INSP_REJOY, 0 });
        }
        cy += 32;

        // Delete button
        RECT rDel = { ix, cy, ix + iw, cy + 26 };
        bool hovDel = ptIn(rDel, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rDel, 6.0f, hovDel ? Theme::ACCENT_RED : Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
        GdiPlusDraw::textCenter(g, "DELETE SELECTED KEY", static_cast<float>(ix + iw / 2), static_cast<float>(cy + 13), 11.0f, true, hovDel ? 0xFFFFFFFFu : Theme::ACCENT_RED);
        if (rDel.bottom > bodyClip_.top && rDel.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rDel, DA_INSP_DELETE, 0 });

        y = card.bottom + 14;
    } else {
        RECT hint = { x, y, x + w, y + 58 };
        GdiPlusDraw::roundedCard(g, hint, 9.0f, Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textLeft(g, "Click any key on the phone to inspect or edit it", static_cast<float>(x + 12), static_cast<float>(y + 20), 11.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - 24));
        GdiPlusDraw::textLeft(g, "Drag to move | Right-Click phone to place | Dbl-Click to replace", static_cast<float>(x + 12), static_cast<float>(y + 39), 10.5f, false, Theme::TEXT_MUTED, static_cast<float>(w - 24));
        y = hint.bottom + 12;
    }

    // 2. Add New Controls Palette (10 action types)
    GdiPlusDraw::textLeft(g, "ADD NEW CONTROL TO PHONE", static_cast<float>(x), static_cast<float>(y + 8), 10.5f, true, Theme::TEXT_MUTED);
    y += 22;

    struct PaletteSpec {
        const char* title;
        const char* sub;
        KeyActionType type;
        uint32_t accent;
    };
    static const PaletteSpec items[10] = {
        { "+ Press Key",     "Hold touch down",     NODE_PRESS,          Theme::ACCENT_CYAN },
        { "+ Tap / Click",   "Fixed single tap",    NODE_CLICK,          Theme::ACCENT_CYAN },
        { "+ Hold-to-ADS",   "Tap on down & up",    NODE_DOWN_UP,        Theme::ACCENT_GREEN },
        { "+ Free Look",     "Temp drag-look",      NODE_HOLD_VIEW,      0xFF00F0F0u },
        { "+ Swipe",         "Drag A -> B",         NODE_SWIPE,          Theme::ACCENT_AMBER },
        { "+ Cursor Toggle", "Bag / Map unlock",    NODE_CURSOR_TOGGLE,  Theme::ACCENT_PURPLE },
        { "+ View Reset",    "Re-center camera",    NODE_RESET_VIEW,     Theme::ACCENT_AMBER },
        { "+ Scope Sens",    "ADS sensitivity",     NODE_SWITCH_SENS,    Theme::ACCENT_PURPLE },
        { "+ Fix Joystick",  "Unstick WASD",        NODE_REACTIVATE_JOY, Theme::ACCENT_GREEN },
        { "+ Up Trigger",    "Tap on release",      NODE_UP_TRIGGER,     Theme::TEXT_SECONDARY }
    };

    int colW = (w - 8) / 2;
    int tileH = 44;
    for (int i = 0; i < 10; ++i) {
        int col = i % 2;
        int row = i / 2;
        int tx = x + col * (colW + 8);
        int ty = y + row * (tileH + 6);
        RECT r = { tx, ty, tx + colW, ty + tileH };
        bool hov = ptIn(r, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, r, 8.0f, hov ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                                 hov ? items[i].accent : Theme::BORDER_SUBTLE, hov ? 1.3f : 1.0f);
        GdiPlusDraw::textLeft(g, items[i].title, static_cast<float>(tx + 10), static_cast<float>(ty + 15), 11.5f, true, items[i].accent, static_cast<float>(colW - 14));
        GdiPlusDraw::textLeft(g, items[i].sub, static_cast<float>(tx + 10), static_cast<float>(ty + 31), 10.0f, false, Theme::TEXT_MUTED, static_cast<float>(colW - 14));
        if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ r, DA_ADD_NODE, static_cast<int>(items[i].type) });
        }
    }
    y += 5 * (tileH + 6) + 8;

    RECT rClr = { x, y, x + w, y + 28 };
    bool hovClr = ptIn(rClr, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rClr, 7.0f, hovClr ? Theme::ACCENT_RED : Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
    GdiPlusDraw::textCenter(g, "CLEAR ALL EXTRA KEYS (Keep WASD / AIM / FIRE)",
                            static_cast<float>(x + w / 2), static_cast<float>(y + 14),
                            10.5f, true, hovClr ? 0xFFFFFFFFu : Theme::ACCENT_RED);
    if (rClr.bottom > bodyClip_.top && rClr.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rClr, DA_CLEAR_BUTTONS, 0 });
    return y + 34;
}

int DrawerPanel::paintTabSens(Gdiplus::Graphics& g, int x, int y, int w,
                              const Profile& p, int hoverX, int hoverY) {
    char buf[24];
    const int step = 28;

    GdiPlusDraw::textLeft(g, "LOOK & AIM SENSITIVITY", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 20;

    std::snprintf(buf, sizeof(buf), "%.2f", p.sensX);
    drawSliderRow(g, x, y, w, "X Sensitivity", p.sensX, 0.10f, 4.00f, buf, Theme::ACCENT_CYAN, DSL_SENS_X, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%.2f", p.sensY);
    drawSliderRow(g, x, y, w, "Y Sensitivity", p.sensY, 0.10f, 4.00f, buf, Theme::ACCENT_CYAN, DSL_SENS_Y, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%.2f", p.fireSensX);
    drawSliderRow(g, x, y, w, "Fire X Mult", p.fireSensX, 0.20f, 2.50f, buf, Theme::ACCENT_RED, DSL_FIRE_X, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%.2f", p.fireSensY);
    drawSliderRow(g, x, y, w, "Fire Y Drag", p.fireSensY, 0.20f, 3.00f, buf, Theme::ACCENT_RED, DSL_FIRE_Y, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%.2f", p.scopeSensX);
    drawSliderRow(g, x, y, w, "Scope X Mult", p.scopeSensX, 0.15f, 2.00f, buf, Theme::ACCENT_PURPLE, DSL_SCOPE_X, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%.2f", p.scopeSensY);
    drawSliderRow(g, x, y, w, "Scope Y Mult", p.scopeSensY, 0.15f, 2.00f, buf, Theme::ACCENT_PURPLE, DSL_SCOPE_Y, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%.1f", p.ffCounterAccel);
    drawSliderRow(g, x, y, w, "FF Anti-Accel", p.ffCounterAccel, 0.0f, 20.0f, buf, Theme::ACCENT_AMBER, DSL_FF_CA, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%.2f", p.smoothing);
    drawSliderRow(g, x, y, w, "Smoothing", p.smoothing, 0.0f, 0.60f, buf, Theme::ACCENT_GREEN, DSL_SMOOTH, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.hudOpacity * 100.0f + 0.5f));
    drawSliderRow(g, x, y, w, "HUD Opacity", p.hudOpacity, 0.20f, 1.00f, buf, Theme::TEXT_SECONDARY, DSL_HUD_OPACITY, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%dms", p.idleResetMs);
    drawSliderRow(g, x, y, w, "Idle Reset", static_cast<float>(p.idleResetMs), 0.0f, 2000.0f, buf, Theme::ACCENT_CYAN, DSL_IDLE_RESET, hoverX, hoverY); y += step;
    std::snprintf(buf, sizeof(buf), "%dpx", p.tapJitterPx);
    drawSliderRow(g, x, y, w, "Tap Jitter", static_cast<float>(p.tapJitterPx), 0.0f, 12.0f, buf, Theme::ACCENT_GREEN, DSL_TAP_JITTER, hoverX, hoverY); y += step + 8;

    // Fire Mode segmented
    GdiPlusDraw::textLeft(g, "FIRE MODE", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 18;
    const char* fLbls[3] = { "CLAW-LEFT", "DRAG-FIRE", "DUAL-DRAG" };
    const int fActs[3] = { DA_FIRE_CLAW, DA_FIRE_DRAG, DA_FIRE_DUAL };
    const int fModes[3] = { FIRE_MODE_CLAW_LEFT, FIRE_MODE_DRAG_RIGHT, FIRE_MODE_DUAL_DRAG };
    int fw = (w - 8) / 3;
    for (int i = 0; i < 3; ++i) {
        RECT r = { x + i * (fw + 4), y, x + i * (fw + 4) + fw, y + 26 };
        bool on = (p.fireMode == fModes[i]);
        GdiPlusDraw::roundedCard(g, r, 6.0f, on ? Theme::ACCENT_RED_BG : Theme::BG_CARD, on ? Theme::ACCENT_RED : Theme::BORDER_SUBTLE, on ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, fLbls[i], static_cast<float>(r.left + r.right) * 0.5f, static_cast<float>(y + 13), 10.5f, true, on ? Theme::ACCENT_RED : Theme::TEXT_SECONDARY);
        if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ r, fActs[i], 0 });
    }
    y += 34;

    // Aim Bounds
    auto drawSeg2 = [&](const char* title, const char* a, const char* b, bool firstOn, int actA, int actB) {
        GdiPlusDraw::textLeft(g, title, static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
        y += 18;
        int hw = (w - 6) / 2;
        RECT rA = { x, y, x + hw, y + 26 };
        RECT rB = { x + hw + 6, y, x + w, y + 26 };
        GdiPlusDraw::roundedCard(g, rA, 6.0f, firstOn ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, firstOn ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, firstOn ? 1.4f : 1.0f);
        GdiPlusDraw::roundedCard(g, rB, 6.0f, !firstOn ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, !firstOn ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, !firstOn ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, a, static_cast<float>(rA.left + rA.right) * 0.5f, static_cast<float>(y + 13), 10.5f, true, firstOn ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        GdiPlusDraw::textCenter(g, b, static_cast<float>(rB.left + rB.right) * 0.5f, static_cast<float>(y + 13), 10.5f, true, !firstOn ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        if (rA.bottom > bodyClip_.top && rA.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rA, actA, 0 });
            hotspots_.push_back(Hotspot{ rB, actB, 0 });
        }
        y += 34;
    };

    drawSeg2("AIM BOUNDS", "UNBOUNDED (FF)", "BOXED (15ms)", p.unboundedAim, DA_BOUNDS_UNBOUNDED, DA_BOUNDS_BOXED);

    // Polling rate 4-way
    GdiPlusDraw::textLeft(g, "MOUSE POLLING RATE", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 18;
    const char* pLbls[4] = { "125Hz", "250Hz", "500Hz", "MAX" };
    const int pActs[4] = { DA_POLL_125, DA_POLL_250, DA_POLL_500, DA_POLL_MAX };
    const int pVals[4] = { 125, 250, 500, 0 };
    int pw = (w - 12) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT r = { x + i * (pw + 4), y, x + i * (pw + 4) + pw, y + 26 };
        bool on = (p.pollHz == pVals[i]);
        GdiPlusDraw::roundedCard(g, r, 6.0f, on ? Theme::ACCENT_PURPLE_BG : Theme::BG_CARD, on ? Theme::ACCENT_PURPLE : Theme::BORDER_SUBTLE, on ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, pLbls[i], static_cast<float>(r.left + r.right) * 0.5f, static_cast<float>(y + 13), 10.5f, true, on ? Theme::ACCENT_PURPLE : Theme::TEXT_SECONDARY);
        if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ r, pActs[i], 0 });
    }
    y += 34;

    drawSeg2("WASD JOYSTICK GLIDE", "FF 4-Step Glide", "Fast 3-Step (24ms)", p.joySteppedGlide, DA_JOY_GLIDE, DA_JOY_FAST);

    drawToggleRow(g, x, y, w, "Invert Y Axis", p.invertY, Theme::ACCENT_AMBER, DA_TOGGLE_INVERT_Y, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Remember Cursor Position on Unlock", p.rememberCursorPos, Theme::ACCENT_CYAN, DA_TOGGLE_REMEMBER_CURSOR, hoverX, hoverY); y += 40;

    // Lock / Unlock & Hold-Unlock hotkeys
    GdiPlusDraw::textLeft(g, "AIM LOCK HOTKEYS", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 20;

    RECT rAimK = { x, y, x + w - 68, y + 28 };
    GdiPlusDraw::roundedCard(g, rAimK, 6.0f, rebindingToggleAim_ ? Theme::ACCENT_AMBER_BG : Theme::BG_CARD,
                             rebindingToggleAim_ ? Theme::ACCENT_AMBER : Theme::BORDER_STRONG, 1.1f);
    std::string aimLbl = rebindingToggleAim_ ? "Press Key / Mouse..." : ("Lock/Unlock: " + KeyCodes::toName(p.toggleAimVk));
    GdiPlusDraw::textCenter(g, aimLbl, static_cast<float>(rAimK.left + rAimK.right) * 0.5f, static_cast<float>(y + 14), 11.5f, true, Theme::TEXT_PRIMARY);
    RECT rMmb = { x + w - 62, y, x + w, y + 28 };
    bool isMmb = (p.toggleAimVk == VK_MBUTTON);
    GdiPlusDraw::roundedCard(g, rMmb, 6.0f, isMmb ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, isMmb ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.1f);
    GdiPlusDraw::textCenter(g, "MMB", static_cast<float>(rMmb.left + rMmb.right) * 0.5f, static_cast<float>(y + 14), 11.0f, true, isMmb ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
    if (rAimK.bottom > bodyClip_.top && rAimK.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rAimK, DA_REBIND_TOGGLE_AIM, 0 });
        hotspots_.push_back(Hotspot{ rMmb, DA_SET_TOGGLE_MMB, 0 });
    }
    y += 34;

    RECT rHoldK = { x, y, x + w - 68, y + 28 };
    GdiPlusDraw::roundedCard(g, rHoldK, 6.0f, rebindingHoldUnlock_ ? Theme::ACCENT_AMBER_BG : Theme::BG_CARD,
                             rebindingHoldUnlock_ ? Theme::ACCENT_AMBER : Theme::BORDER_STRONG, 1.1f);
    std::string holdLbl = rebindingHoldUnlock_ ? "Press Key / Mouse..." : ("Hold-Unlock: " + KeyCodes::toName(p.holdUnlockVk));
    GdiPlusDraw::textCenter(g, holdLbl, static_cast<float>(rHoldK.left + rHoldK.right) * 0.5f, static_cast<float>(y + 14), 11.5f, true, Theme::TEXT_PRIMARY);
    RECT rClrH = { x + w - 62, y, x + w, y + 28 };
    GdiPlusDraw::roundedCard(g, rClrH, 6.0f, Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
    GdiPlusDraw::textCenter(g, "CLEAR", static_cast<float>(rClrH.left + rClrH.right) * 0.5f, static_cast<float>(y + 14), 10.5f, true, Theme::ACCENT_RED);
    if (rHoldK.bottom > bodyClip_.top && rHoldK.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rHoldK, DA_REBIND_HOLD_UNLOCK, 0 });
        hotspots_.push_back(Hotspot{ rClrH, DA_CLEAR_HOLD_UNLOCK, 0 });
    }
    y += 38;

    RECT rRstS = { x, y, x + w, y + 28 };
    GdiPlusDraw::roundedCard(g, rRstS, 7.0f, ptIn(rRstS, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, 1.0f);
    GdiPlusDraw::textCenter(g, "RESET SENSITIVITY DEFAULTS", static_cast<float>(x + w / 2), static_cast<float>(y + 14), 11.0f, true, Theme::ACCENT_AMBER);
    if (rRstS.bottom > bodyClip_.top && rRstS.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rRstS, DA_RESET_SENS, 0 });
    return y + 36;
}

int DrawerPanel::paintTabProfiles(Gdiplus::Graphics& g, int x, int y, int w,
                                  const Profile& p, const std::string& activePath,
                                  const std::string& foregroundPkg, int hoverX, int hoverY) {
    // 1. Active Profile App-Link & Input Mode Card
    RECT rLink = { x, y, x + w, y + 104 };
    GdiPlusDraw::roundedCard(g, rLink, 9.0f, Theme::BG_CARD, Theme::BORDER_STRONG, 1.1f);
    int lx = x + 10, lw = w - 20, ly = y + 10;
    GdiPlusDraw::textLeft(g, "PER-APP AUTO-SWITCH LINK", static_cast<float>(lx), static_cast<float>(ly + 6), 10.0f, true, Theme::ACCENT_CYAN);
    ly += 18;
    std::string liveTxt = "Open App: " + (foregroundPkg.empty() ? std::string("(none detected)") : foregroundPkg);
    GdiPlusDraw::textLeft(g, liveTxt, static_cast<float>(lx), static_cast<float>(ly + 6), 11.0f, false, Theme::TEXT_SECONDARY, static_cast<float>(lw));
    ly += 16;
    std::string linkedTxt = "Linked To: " + (p.appPackage.empty() ? std::string("Manual (not linked)") : p.appPackage);
    GdiPlusDraw::textLeft(g, linkedTxt, static_cast<float>(lx), static_cast<float>(ly + 6), 11.0f, true,
                          p.appPackage.empty() ? Theme::TEXT_MUTED : Theme::ACCENT_GREEN, static_cast<float>(lw));
    ly += 20;

    int lhw = (lw - 6) / 2;
    RECT rBindApp = { lx, ly, lx + lhw + 22, ly + 26 };
    RECT rUnlink  = { rBindApp.right + 6, ly, lx + lw, ly + 26 };
    bool canLink = !foregroundPkg.empty();
    GdiPlusDraw::roundedCard(g, rBindApp, 6.0f,
                             canLink ? (ptIn(rBindApp, hoverX, hoverY) ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG) : Theme::BG_INPUT,
                             canLink ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textCenter(g, "LINK CURRENT APP", static_cast<float>(rBindApp.left + rBindApp.right) * 0.5f,
                            static_cast<float>(ly + 13), 10.5f, true,
                            canLink ? (ptIn(rBindApp, hoverX, hoverY) ? 0xFF07131Au : Theme::TEXT_PRIMARY) : Theme::TEXT_MUTED);
    GdiPlusDraw::roundedCard(g, rUnlink, 6.0f, ptIn(rUnlink, hoverX, hoverY) ? Theme::ACCENT_RED_BG : Theme::BG_INPUT,
                             Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "UNLINK", static_cast<float>(rUnlink.left + rUnlink.right) * 0.5f,
                            static_cast<float>(ly + 13), 10.5f, true, Theme::TEXT_SECONDARY);
    if (rBindApp.bottom > bodyClip_.top && rBindApp.top < bodyClip_.bottom) {
        if (canLink) hotspots_.push_back(Hotspot{ rBindApp, DA_LINK_CURRENT_APP, 0 });
        hotspots_.push_back(Hotspot{ rUnlink, DA_UNLINK_APP, 0 });
    }
    y = rLink.bottom + 8;

    drawToggleRow(g, x, y, w, p.keymapEnabled ? "Mode: Game Keymap (OFF = PC Typing)" : "Mode: Desktop PC Typing (No Keymap)",
                  p.keymapEnabled, Theme::ACCENT_GREEN, DA_TOGGLE_KEYMAP_MODE, hoverX, hoverY);
    y += 40;

    GdiPlusDraw::textLeft(g, "CREATE NEW CONFIG", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 20;

    RECT rName = { x, y, x + w, y + 30 };
    GdiPlusDraw::roundedCard(g, rName, 7.0f, Theme::BG_INPUT,
                             editingNewProfileName_ ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG,
                             editingNewProfileName_ ? 1.5f : 1.0f);
    GdiPlusDraw::textLeft(g, "Name: " + newProfileName_ + (editingNewProfileName_ ? "_" : ""),
                          static_cast<float>(x + 10), static_cast<float>(y + 15), 12.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - 20));
    if (rName.bottom > bodyClip_.top && rName.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rName, DA_FOCUS_NEW_NAME, 0 });
    y += 38;

    int hw = (w - 8) / 2;
    RECT rBlank = { x, y, x + hw, y + 30 };
    RECT rClone = { x + hw + 8, y, x + w, y + 30 };
    GdiPlusDraw::roundedCard(g, rBlank, 7.0f, ptIn(rBlank, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::ACCENT_CYAN_BG, Theme::ACCENT_CYAN, 1.1f);
    GdiPlusDraw::roundedCard(g, rClone, 7.0f, ptIn(rClone, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::ACCENT_GREEN_BG, Theme::ACCENT_GREEN, 1.1f);
    GdiPlusDraw::textCenter(g, "+ BLANK CONFIG", static_cast<float>(rBlank.left + rBlank.right) * 0.5f, static_cast<float>(y + 15), 11.0f, true, Theme::ACCENT_CYAN);
    GdiPlusDraw::textCenter(g, "+ CLONE CURRENT", static_cast<float>(rClone.left + rClone.right) * 0.5f, static_cast<float>(y + 15), 11.0f, true, Theme::ACCENT_GREEN);
    if (rBlank.bottom > bodyClip_.top && rBlank.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rBlank, DA_CREATE_BLANK, 0 });
        hotspots_.push_back(Hotspot{ rClone, DA_CREATE_CLONE, 0 });
    }
    y += 40;

    // Shareable Edge-Anchored Aspect Scaling (survives 20:9 <-> 16:9 screens!)
    char aspLbl[80];
    std::snprintf(aspLbl, sizeof(aspLbl), "Edge-Anchored Layout (Ref %.2f:1)", p.refAspect);
    drawToggleRow(g, x, y, w, aspLbl, p.edgeAnchored, Theme::ACCENT_CYAN, DA_TOGGLE_EDGE_ANCHOR, hoverX, hoverY);
    y += 34;
    RECT rCal = { x, y, x + w, y + 26 };
    bool hovCal = ptIn(rCal, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rCal, 6.0f, hovCal ? Theme::BG_CARD_HOVER : Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "CALIBRATE REF ASPECT TO CURRENT SCREEN", static_cast<float>(x + w / 2),
                            static_cast<float>(y + 13), 10.0f, true, Theme::TEXT_SECONDARY);
    if (rCal.bottom > bodyClip_.top && rCal.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rCal, DA_CALIBRATE_ASPECT, 0 });
    }
    y += 36;

    GdiPlusDraw::textLeft(g, "SAVED CONFIGS (profiles\\*.cfg)", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 22;

    for (size_t i = 0; i < profileEntries_.size(); ++i) {
        const ProfileEntry& e = profileEntries_[i];
        bool isAct = (lstrcmpiA(e.path.c_str(), activePath.c_str()) == 0);
        RECT rRow = { x, y, x + w - 54, y + 36 };
        bool hov = ptIn(rRow, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rRow, 8.0f,
                                 isAct ? Theme::ACCENT_GREEN_BG : (hov ? Theme::BG_CARD_HOVER : Theme::BG_CARD),
                                 isAct ? Theme::ACCENT_GREEN : Theme::BORDER_SUBTLE, isAct ? 1.4f : 1.0f);
        GdiPlusDraw::textLeft(g, (isAct ? "* " : "") + e.displayName, static_cast<float>(x + 10), static_cast<float>(y + 12), 12.0f, true,
                              isAct ? Theme::ACCENT_GREEN : Theme::TEXT_PRIMARY, static_cast<float>(w - 76));
        GdiPlusDraw::textLeft(g, e.fileName, static_cast<float>(x + 10), static_cast<float>(y + 26), 10.0f, false, Theme::TEXT_MUTED, static_cast<float>(w - 76));

        RECT rDel = { x + w - 48, y, x + w, y + 36 };
        bool hovD = ptIn(rDel, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rDel, 8.0f, hovD ? Theme::ACCENT_RED : Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
        GdiPlusDraw::textCenter(g, "DEL", static_cast<float>(rDel.left + rDel.right) * 0.5f, static_cast<float>(y + 18), 10.5f, true, hovD ? 0xFFFFFFFFu : Theme::ACCENT_RED);

        if (rRow.bottom > bodyClip_.top && rRow.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rRow, DA_LOAD_PROFILE, static_cast<int>(i) });
            hotspots_.push_back(Hotspot{ rDel, DA_DELETE_PROFILE, static_cast<int>(i) });
        }
        y += 42;
    }
    return y + 8;
}

int DrawerPanel::paintTabDevice(Gdiplus::Graphics& g, int x, int y, int w,
                                const AppSettings& s, bool connected, const std::string& deviceLabel,
                                const std::vector<std::string>& logs, int hoverX, int hoverY) {
    // 1. Connection Card
    RECT rDev = { x, y, x + w, y + 66 };
    GdiPlusDraw::roundedCard(g, rDev, 9.0f, Theme::BG_CARD, connected ? Theme::ACCENT_GREEN : Theme::BORDER_STRONG, 1.2f);
    GdiPlusDraw::textLeft(g, connected ? "CONNECTED PHONE" : "DISCONNECTED", static_cast<float>(x + 12), static_cast<float>(y + 15), 10.0f, true,
                          connected ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER);
    GdiPlusDraw::textLeft(g, connected ? deviceLabel : "Open Device Hub or plug in USB", static_cast<float>(x + 12), static_cast<float>(y + 34), 12.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - 24));
    GdiPlusDraw::textLeft(g, "Stream: " + s.buildScrcpyArgs(), static_cast<float>(x + 12), static_cast<float>(y + 52), 10.0f, false, Theme::TEXT_MUTED, static_cast<float>(w - 24));
    y += 76;

    // 2. Stream Quality Controls
    auto drawSeg3 = [&](const char* title, const char* l0, const char* l1, const char* l2,
                        int v0, int v1, int v2, int curV, int actId) {
        GdiPlusDraw::textLeft(g, title, static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
        y += 18;
        const char* ls[3] = { l0, l1, l2 };
        const int vs[3] = { v0, v1, v2 };
        int cw = (w - 8) / 3;
        for (int i = 0; i < 3; ++i) {
            RECT r = { x + i * (cw + 4), y, x + i * (cw + 4) + cw, y + 26 };
            bool on = (curV == vs[i]);
            GdiPlusDraw::roundedCard(g, r, 6.0f, on ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, on ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, on ? 1.4f : 1.0f);
            GdiPlusDraw::textCenter(g, ls[i], static_cast<float>(r.left + r.right) * 0.5f, static_cast<float>(y + 13), 11.0f, true, on ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
            if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ r, actId, vs[i] });
        }
        y += 34;
    };

    drawSeg3("MIRROR MAX FPS", "60 FPS", "90 FPS", "120 FPS", 60, 90, 120, s.mirrorFps, DA_STREAM_FPS);

    // Bitrate 4-way
    GdiPlusDraw::textLeft(g, "VIDEO BITRATE", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 18;
    const char* bLbls[4] = { "8M", "12M", "16M", "24M" };
    const int bVals[4] = { 8, 12, 16, 24 };
    int bw = (w - 12) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT r = { x + i * (bw + 4), y, x + i * (bw + 4) + bw, y + 26 };
        bool on = (s.mirrorBitrateMbps == bVals[i]);
        GdiPlusDraw::roundedCard(g, r, 6.0f, on ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, on ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, on ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, bLbls[i], static_cast<float>(r.left + r.right) * 0.5f, static_cast<float>(y + 13), 11.0f, true, on ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ r, DA_STREAM_BITRATE, bVals[i] });
    }
    y += 34;

    drawSeg3("VIDEO RESOLUTION", "Native", "1600px", "1280px", 0, 1600, 1280, s.mirrorMaxSize, DA_STREAM_MAXSIZE);

    // Stage 2 & Stage 3 Renderer + Second-Phone Virtual Display controls
    drawToggleRow(g, x, y, w, "Native D3D11 Single-Stream Video", s.useNativeVideo, Theme::ACCENT_CYAN, DA_TOGGLE_NATIVE_VIDEO, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Second-Phone Virtual Display", s.virtualDisplayMode, Theme::ACCENT_PURPLE, DA_TOGGLE_VIRTUAL_DISPLAY, hoverX, hoverY); y += 34;

    if (s.virtualDisplayMode) {
        int curVd = 0;
        if (s.virtualDisplaySpec == "1280x720/240") curVd = 1;
        else if (s.virtualDisplaySpec == "auto") curVd = 2;
        drawSeg3("VIRTUAL DISPLAY SIZE", "1080p PC", "720p Fast", "Auto", 0, 1, 2, curVd, DA_VD_PRESET);
    }

    drawToggleRow(g, x, y, w, "XInput Gamepad-to-Touch Controller", s.gamepadEnabled, Theme::ACCENT_GREEN, DA_TOGGLE_GAMEPAD, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Forward Phone Audio (scrcpy 5.0)", s.mirrorAudio, Theme::ACCENT_GREEN, DA_TOGGLE_AUDIO, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Turn Phone Screen Off", s.turnScreenOff, Theme::ACCENT_CYAN, DA_TOGGLE_SCREEN_OFF, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Keep Phone Awake", s.stayAwake, Theme::ACCENT_CYAN, DA_TOGGLE_STAY_AWAKE, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Auto-Switch Profile by Open App", s.autoSwitchProfiles, Theme::ACCENT_GREEN, DA_TOGGLE_AUTO_SWITCH, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Right-Click = Android Back (Cursor)", s.rightClickBack, Theme::ACCENT_CYAN, DA_TOGGLE_RCLICK_BACK, hoverX, hoverY); y += 40;

    // Apply & Restart Stream / Disconnect
    int hw = (w - 8) / 2;
    RECT rRec = { x, y, x + hw + 28, y + 30 };
    RECT rDis = { rRec.right + 8, y, x + w, y + 30 };
    GdiPlusDraw::roundedCard(g, rRec, 7.0f, ptIn(rRec, hoverX, hoverY) ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG, Theme::ACCENT_CYAN, 1.2f);
    GdiPlusDraw::textCenter(g, "APPLY & RECONNECT", static_cast<float>(rRec.left + rRec.right) * 0.5f, static_cast<float>(y + 15), 11.0f, true,
                            ptIn(rRec, hoverX, hoverY) ? 0xFF08121Au : Theme::TEXT_PRIMARY);
    GdiPlusDraw::roundedCard(g, rDis, 7.0f, ptIn(rDis, hoverX, hoverY) ? Theme::ACCENT_RED : Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
    GdiPlusDraw::textCenter(g, "DISCONNECT", static_cast<float>(rDis.left + rDis.right) * 0.5f, static_cast<float>(y + 15), 10.5f, true,
                            ptIn(rDis, hoverX, hoverY) ? 0xFFFFFFFFu : Theme::ACCENT_RED);
    if (rRec.bottom > bodyClip_.top && rRec.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rRec, DA_APPLY_RECONNECT, 0 });
        hotspots_.push_back(Hotspot{ rDis, DA_DISCONNECT, 0 });
    }
    y += 42;

    // Wireless ADB quick connect
    GdiPlusDraw::textLeft(g, "WIRELESS ADB", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 20;
    RECT rWInp = { x, y, x + w - 82, y + 28 };
    GdiPlusDraw::roundedCard(g, rWInp, 6.0f, Theme::BG_INPUT, editingWireless_ ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, editingWireless_ ? 1.5f : 1.0f);
    GdiPlusDraw::textLeft(g, s.lastWirelessAddr + (editingWireless_ ? "_" : ""), static_cast<float>(x + 8), static_cast<float>(y + 14), 11.5f, false, Theme::TEXT_PRIMARY, static_cast<float>(w - 94));
    RECT rWGo = { rWInp.right + 6, y, x + w, y + 28 };
    GdiPlusDraw::roundedCard(g, rWGo, 6.0f, ptIn(rWGo, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_CARD, Theme::ACCENT_CYAN, 1.0f);
    GdiPlusDraw::textCenter(g, "CONNECT", static_cast<float>(rWGo.left + rWGo.right) * 0.5f, static_cast<float>(y + 14), 10.5f, true, Theme::ACCENT_CYAN);
    if (rWInp.bottom > bodyClip_.top && rWInp.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rWInp, DA_FOCUS_WIRELESS, 0 });
        hotspots_.push_back(Hotspot{ rWGo, DA_CONNECT_WIRELESS, 0 });
    }
    y += 40;

    // Activity Log Card
    GdiPlusDraw::textLeft(g, "ACTIVITY LOG", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 20;
    RECT rLog = { x, y, x + w, y + 118 };
    GdiPlusDraw::roundedCard(g, rLog, 8.0f, Theme::BG_INPUT, Theme::BORDER_SUBTLE, 1.0f);
    int ly = y + 12;
    size_t startIdx = (logs.size() > 6) ? (logs.size() - 6) : 0;
    for (size_t i = startIdx; i < logs.size(); ++i) {
        GdiPlusDraw::textLeft(g, logs[i], static_cast<float>(x + 8), static_cast<float>(ly), 10.0f, false, Theme::TEXT_SECONDARY, static_cast<float>(w - 16));
        ly += 17;
    }
    if (logs.empty()) {
        GdiPlusDraw::textLeft(g, "Ready. Connect phone or edit keymaps.", static_cast<float>(x + 8), static_cast<float>(ly), 10.5f, false, Theme::TEXT_MUTED, static_cast<float>(w - 16));
    }
    return rLog.bottom + 10;
}

void DrawerPanel::applySliderValue(int sliderId, float normT, Profile& p, int selectedBtnIndex) {
    normT = clampf(normT, 0.0f, 1.0f);
    for (size_t i = 0; i < sliders_.size(); ++i) {
        if (sliders_[i].id != sliderId) continue;
        float v = sliders_[i].minVal + normT * (sliders_[i].maxVal - sliders_[i].minVal);
        switch (sliderId) {
            case DSL_SENS_X:      p.sensX = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_SENS_Y:      p.sensY = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_FIRE_X:      p.fireSensX = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_FIRE_Y:      p.fireSensY = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_SCOPE_X:     p.scopeSensX = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_SCOPE_Y:     p.scopeSensY = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_FF_CA:       p.ffCounterAccel = std::floor(v * 2.0f + 0.5f) / 2.0f; break;
            case DSL_SMOOTH:      p.smoothing = std::floor(v * 50.0f + 0.5f) / 50.0f; break;
            case DSL_HUD_OPACITY: p.hudOpacity = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_IDLE_RESET:  p.idleResetMs = static_cast<int>(std::floor(v / 25.0f + 0.5f)) * 25; break;
            case DSL_TAP_JITTER:  p.tapJitterPx = static_cast<int>(std::floor(v + 0.5f)); break;
            case DSL_NODE_DUR:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
                    p.buttons[selectedBtnIndex].durationMs = static_cast<int>(std::floor(v / 5.0f + 0.5f)) * 5;
                }
                break;
            default: break;
        }
        return;
    }
}

bool DrawerPanel::onMouseDown(int x, int y, Profile& profile, AppSettings& settings,
                              int& selectedBtnIndex, DrawerActionResult& out) {
    if (!containsPoint(x, y)) {
        clearFocus();
        return false;
    }

    // Sliders first
    for (size_t i = 0; i < sliders_.size(); ++i) {
        if (ptIn(sliders_[i].hitRc, x, y)) {
            activeSliderId_ = sliders_[i].id;
            activeSliderTrack_ = sliders_[i].trackRc;
            float w = static_cast<float>(activeSliderTrack_.right - activeSliderTrack_.left);
            float t = (w > 1.0f) ? (static_cast<float>(x - activeSliderTrack_.left) / w) : 0.0f;
            applySliderValue(activeSliderId_, t, profile, selectedBtnIndex);
            out.profileModified = true;
            clearFocus();
            return true;
        }
    }

    for (size_t i = 0; i < hotspots_.size(); ++i) {
        const Hotspot& hs = hotspots_[i];
        if (!ptIn(hs.rc, x, y)) continue;

        clearFocus();
        if (hs.id != DA_INSP_REBIND && hs.id != DA_ADD_NODE) {
            out.cancelRebind = true;
        }

        switch (hs.id) {
            case DA_TAB_0: case DA_TAB_1: case DA_TAB_2: case DA_TAB_3:
                setActiveTab(hs.id - DA_TAB_0);
                return true;
            case DA_CLOSE: out.requestCloseDrawer = true; return true;
            case DA_SAVE:  out.requestSave = true; return true;

            case DA_ADD_NODE:
                out.requestAddNode = true;
                out.addNodeType = static_cast<KeyActionType>(hs.extra);
                out.addNodeLabel = ProfileDefaults::actionTypeToName(out.addNodeType);
                return true;
            case DA_CLEAR_BUTTONS:
                profile.buttons.clear();
                selectedBtnIndex = -1;
                out.profileModified = true;
                out.toastMessage = "Cleared extra keys (WASD / AIM / FIRE kept)";
                return true;
            case DA_INSP_LABEL: editingNodeLabel_ = true; return true;
            case DA_INSP_REBIND: out.requestStartRebindNode = true; return true;
            case DA_INSP_MOUSE_VK:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].comboVk = 0;
                    profile.buttons[selectedBtnIndex].vk = hs.extra;
                    out.profileModified = true;
                    out.toastMessage = "Bound '" + profile.buttons[selectedBtnIndex].label + "' -> " + KeyCodes::toName(hs.extra);
                }
                return true;
            case DA_INSP_ACT_PREV:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    KeyNode& b = profile.buttons[selectedBtnIndex];
                    b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 9) % 10);
                    if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) { b.x2 = clampf(b.x + 0.12f, 0.02f, 0.98f); b.durationMs = 120; }
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_ACT_NEXT:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    KeyNode& b = profile.buttons[selectedBtnIndex];
                    b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 1) % 10);
                    if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) { b.x2 = clampf(b.x + 0.12f, 0.02f, 0.98f); b.durationMs = 120; }
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_SCOPE:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].switchScopeSens = !profile.buttons[selectedBtnIndex].switchScopeSens;
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_RSTVIEW:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].resetView = !profile.buttons[selectedBtnIndex].resetView;
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_REJOY:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].reactivateJoy = !profile.buttons[selectedBtnIndex].reactivateJoy;
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_DELETE: out.requestDeleteSelectedNode = true; return true;

            case DA_FIRE_CLAW: profile.fireMode = FIRE_MODE_CLAW_LEFT; out.profileModified = true; out.toastMessage = "Fire Mode: CLAW-LEFT"; return true;
            case DA_FIRE_DRAG: profile.fireMode = FIRE_MODE_DRAG_RIGHT; out.profileModified = true; out.toastMessage = "Fire Mode: DRAG-FIRE"; return true;
            case DA_FIRE_DUAL: profile.fireMode = FIRE_MODE_DUAL_DRAG; out.profileModified = true; out.toastMessage = "Fire Mode: DUAL-DRAG"; return true;
            case DA_BOUNDS_UNBOUNDED: profile.unboundedAim = true; out.profileModified = true; out.toastMessage = "Aim Bounds: UNBOUNDED (Free Fire)"; return true;
            case DA_BOUNDS_BOXED:     profile.unboundedAim = false; out.profileModified = true; out.toastMessage = "Aim Bounds: BOXED (15ms Edge Reset)"; return true;
            case DA_POLL_125: profile.pollHz = 125; out.profileModified = true; out.toastMessage = "Polling: 125 Hz"; return true;
            case DA_POLL_250: profile.pollHz = 250; out.profileModified = true; out.toastMessage = "Polling: 250 Hz"; return true;
            case DA_POLL_500: profile.pollHz = 500; out.profileModified = true; out.toastMessage = "Polling: 500 Hz"; return true;
            case DA_POLL_MAX: profile.pollHz = 0;   out.profileModified = true; out.toastMessage = "Polling: MAX (1000 Hz)"; return true;
            case DA_JOY_GLIDE: profile.joySteppedGlide = true; out.profileModified = true; out.toastMessage = "Joystick: FF 4-Step Glide"; return true;
            case DA_JOY_FAST:  profile.joySteppedGlide = false; out.profileModified = true; out.toastMessage = "Joystick: Fast 3-Step (24ms)"; return true;
            case DA_TOGGLE_INVERT_Y: profile.invertY = !profile.invertY; out.profileModified = true; return true;
            case DA_TOGGLE_REMEMBER_CURSOR: profile.rememberCursorPos = !profile.rememberCursorPos; out.profileModified = true; return true;
            case DA_REBIND_TOGGLE_AIM: rebindingToggleAim_ = true; out.toastMessage = "Press ANY key or Mouse button (MMB, LShift, ~) for Aim Lock..."; return true;
            case DA_SET_TOGGLE_MMB: profile.toggleAimVk = VK_MBUTTON; out.profileModified = true; out.toastMessage = "Aim Lock Toggle set to MMB"; return true;
            case DA_REBIND_HOLD_UNLOCK: rebindingHoldUnlock_ = true; out.toastMessage = "Press ANY key or Mouse button for Hold-to-Unlock..."; return true;
            case DA_CLEAR_HOLD_UNLOCK: profile.holdUnlockVk = 0; out.profileModified = true; out.toastMessage = "Hold-to-Unlock cleared"; return true;
            case DA_RESET_SENS: {
                Profile def = ProfileDefaults::createFreeFire();
                profile.sensX = def.sensX; profile.sensY = def.sensY;
                profile.fireSensX = def.fireSensX; profile.fireSensY = def.fireSensY;
                profile.scopeSensX = def.scopeSensX; profile.scopeSensY = def.scopeSensY;
                profile.ffCounterAccel = def.ffCounterAccel;
                profile.smoothing = def.smoothing;
                profile.hudOpacity = def.hudOpacity;
                out.profileModified = true;
                out.toastMessage = "Sensitivities reset to default Free Fire values";
                return true;
            }

            case DA_FOCUS_NEW_NAME: editingNewProfileName_ = true; return true;
            case DA_CREATE_BLANK:
            case DA_CREATE_CLONE: {
                std::string baseFname = ProfileStore::sanitizeFilename(newProfileName_);
                std::string fname = baseFname;
                std::string fullPath = profilesDir_ + "\\" + fname + ".cfg";
                int suffix = 2;
                while (GetFileAttributesA(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 100) {
                    char sbuf[32];
                    std::snprintf(sbuf, sizeof(sbuf), "_%d", suffix++);
                    fname = baseFname + sbuf;
                    fullPath = profilesDir_ + "\\" + fname + ".cfg";
                }
                Profile np = (hs.id == DA_CREATE_BLANK) ? ProfileDefaults::createBlank(fname) : profile;
                np.name = (fname == baseFname && !newProfileName_.empty()) ? newProfileName_ : fname;
                if (ProfileStore::save(fullPath, np)) {
                    refreshProfileList();
                    out.requestSwitchProfile = true;
                    out.switchProfilePath = fullPath;
                    out.toastMessage = "Created & loaded " + fname + ".cfg";
                } else {
                    out.toastMessage = "ERROR: Could not create " + fullPath;
                }
                return true;
            }
            case DA_LOAD_PROFILE:
                if (hs.extra >= 0 && hs.extra < static_cast<int>(profileEntries_.size())) {
                    out.requestSwitchProfile = true;
                    out.switchProfilePath = profileEntries_[hs.extra].path;
                }
                return true;
            case DA_DELETE_PROFILE:
                if (hs.extra >= 0 && hs.extra < static_cast<int>(profileEntries_.size())) {
                    if (profileEntries_.size() <= 1) {
                        out.toastMessage = "Cannot delete the last remaining config";
                    } else if (lstrcmpiA(profileEntries_[hs.extra].path.c_str(), activeProfilePath_.c_str()) == 0) {
                        out.toastMessage = "Switch to another config before deleting the active one";
                    } else {
                        std::remove(profileEntries_[hs.extra].path.c_str());
                        out.toastMessage = "Deleted " + profileEntries_[hs.extra].fileName;
                        refreshProfileList();
                    }
                }
                return true;

            case DA_TOGGLE_KEYMAP_MODE:
                profile.keymapEnabled = !profile.keymapEnabled;
                out.profileModified = true;
                out.toastMessage = profile.keymapEnabled ? "Game Keymap Mode ON" : "Desktop Typing Mode ON (full PC keyboard)";
                return true;
            case DA_LINK_CURRENT_APP:
                out.requestLinkCurrentApp = true;
                return true;
            case DA_UNLINK_APP:
                out.requestUnlinkApp = true;
                return true;
            case DA_TOGGLE_EDGE_ANCHOR:
                profile.edgeAnchored = !profile.edgeAnchored;
                out.profileModified = true;
                out.toastMessage = profile.edgeAnchored
                    ? "Edge-Anchored Shareable Layout ON (20:9 <-> 16:9)"
                    : "Edge-Anchored Layout OFF (raw normalized)";
                return true;
            case DA_CALIBRATE_ASPECT:
                out.requestCalibrateAspect = true;
                return true;

            case DA_STREAM_FPS:     settings.mirrorFps = hs.extra; out.settingsModified = true; return true;
            case DA_STREAM_BITRATE: settings.mirrorBitrateMbps = hs.extra; out.settingsModified = true; return true;
            case DA_STREAM_MAXSIZE: settings.mirrorMaxSize = hs.extra; out.settingsModified = true; return true;
            case DA_TOGGLE_NATIVE_VIDEO:
                settings.useNativeVideo = !settings.useNativeVideo;
                if (!settings.useNativeVideo) settings.virtualDisplayMode = false;
                out.settingsModified = true;
                out.toastMessage = settings.useNativeVideo
                    ? "Native D3D11 Single-Stream ON (click Apply & Reconnect)"
                    : "Legacy scrcpy.exe Fallback ON (click Apply & Reconnect)";
                return true;
            case DA_TOGGLE_VIRTUAL_DISPLAY:
                settings.virtualDisplayMode = !settings.virtualDisplayMode;
                if (settings.virtualDisplayMode) settings.useNativeVideo = true;
                out.settingsModified = true;
                out.toastMessage = settings.virtualDisplayMode
                    ? "Second-Phone Virtual Display ON (click Apply & Reconnect)"
                    : "Second-Phone Virtual Display OFF (mirror main screen)";
                return true;
            case DA_VD_PRESET:
                settings.virtualDisplaySpec = (hs.extra == 1) ? "1280x720/240"
                                            : (hs.extra == 2) ? "auto" : "1920x1080/320";
                out.settingsModified = true;
                return true;
            case DA_TOGGLE_GAMEPAD:
                settings.gamepadEnabled = !settings.gamepadEnabled;
                out.settingsModified = true;
                out.toastMessage = settings.gamepadEnabled ? "XInput Gamepad: ON" : "XInput Gamepad: OFF";
                return true;
            case DA_TOGGLE_AUDIO:      settings.mirrorAudio = !settings.mirrorAudio; out.settingsModified = true; return true;
            case DA_TOGGLE_SCREEN_OFF: settings.turnScreenOff = !settings.turnScreenOff; out.settingsModified = true; return true;
            case DA_TOGGLE_STAY_AWAKE: settings.stayAwake = !settings.stayAwake; out.settingsModified = true; return true;
            case DA_TOGGLE_AUTO_SWITCH:
                settings.autoSwitchProfiles = !settings.autoSwitchProfiles;
                out.settingsModified = true;
                out.toastMessage = settings.autoSwitchProfiles ? "Per-App Auto-Switch: ON" : "Per-App Auto-Switch: OFF";
                return true;
            case DA_TOGGLE_RCLICK_BACK:
                settings.rightClickBack = !settings.rightClickBack;
                out.settingsModified = true;
                out.toastMessage = settings.rightClickBack ? "Cursor Right-Click = Back: ON" : "Cursor Right-Click = Back: OFF";
                return true;
            case DA_APPLY_RECONNECT:   out.requestReconnectStream = true; return true;
            case DA_DISCONNECT:        out.requestDisconnectDevice = true; return true;
            case DA_FOCUS_WIRELESS:    editingWireless_ = true; return true;
            case DA_CONNECT_WIRELESS:
                out.requestConnectWireless = true;
                out.wirelessAddr = settings.lastWirelessAddr;
                return true;
            default:
                break;
        }
    }
    return true;
}

bool DrawerPanel::onMouseMove(int x, int y, Profile& profile, int selectedBtnIndex, DrawerActionResult& out) {
    (void)y;
    if (activeSliderId_ != 0) {
        float w = static_cast<float>(activeSliderTrack_.right - activeSliderTrack_.left);
        float t = (w > 1.0f) ? (static_cast<float>(x - activeSliderTrack_.left) / w) : 0.0f;
        applySliderValue(activeSliderId_, t, profile, selectedBtnIndex);
        out.profileModified = true;
        return true;
    }
    return false;
}

bool DrawerPanel::onMouseUp() {
    bool was = (activeSliderId_ != 0);
    activeSliderId_ = 0;
    return was;
}

bool DrawerPanel::onMouseWheel(int x, int y, int delta) {
    if (!containsPoint(x, y)) return false;
    if (maxScrollY_ <= 0) return true;
    int step = (delta > 0) ? -42 : 42;
    scrollY_[activeTab_] += step;
    if (scrollY_[activeTab_] < 0) scrollY_[activeTab_] = 0;
    if (scrollY_[activeTab_] > maxScrollY_) scrollY_[activeTab_] = maxScrollY_;
    return true;
}

bool DrawerPanel::onKeyDown(int vk, Profile& profile, AppSettings& settings,
                            int selectedBtnIndex, DrawerActionResult& out) {
    if (rebindingToggleAim_) {
        if (vk != VK_ESCAPE) {
            vk = KeyCodes::normalizeModifier(vk);
            profile.toggleAimVk = vk;
            out.profileModified = true;
            out.toastMessage = "Aim Lock Toggle bound to " + KeyCodes::toName(vk);
        }
        rebindingToggleAim_ = false;
        return true;
    }
    if (rebindingHoldUnlock_) {
        if (vk != VK_ESCAPE) {
            vk = KeyCodes::normalizeModifier(vk);
            profile.holdUnlockVk = vk;
            out.profileModified = true;
            out.toastMessage = "Hold-to-Unlock bound to " + KeyCodes::toName(vk);
        }
        rebindingHoldUnlock_ = false;
        return true;
    }
    if (editingNewProfileName_) {
        if (vk == VK_RETURN || vk == VK_ESCAPE) { editingNewProfileName_ = false; return true; }
        if (vk == VK_BACK && !newProfileName_.empty()) newProfileName_.pop_back();
        return true;
    }
    if (editingWireless_) {
        if (vk == VK_RETURN) {
            editingWireless_ = false;
            out.requestConnectWireless = true;
            out.wirelessAddr = settings.lastWirelessAddr;
            return true;
        }
        if (vk == VK_ESCAPE) { editingWireless_ = false; return true; }
        if (vk == VK_BACK && !settings.lastWirelessAddr.empty()) {
            settings.lastWirelessAddr.pop_back();
            out.settingsModified = true;
        }
        return true;
    }
    if (editingNodeLabel_) {
        if (vk == VK_RETURN || vk == VK_ESCAPE) { editingNodeLabel_ = false; return true; }
        if (vk == VK_BACK && selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
            std::string& lbl = profile.buttons[selectedBtnIndex].label;
            if (!lbl.empty()) {
                lbl.pop_back();
                out.profileModified = true;
            }
        }
        return true;
    }
    return false;
}

bool DrawerPanel::onMouseBind(int mouseVk, Profile& profile, DrawerActionResult& out) {
    if (rebindingToggleAim_) {
        if (mouseVk == VK_LBUTTON) {
            out.toastMessage = "Use Keyboard, MMB, RMB, X1, X2, or Wheel for Aim Lock";
            return true;
        }
        profile.toggleAimVk = mouseVk;
        rebindingToggleAim_ = false;
        out.profileModified = true;
        out.toastMessage = "Aim Lock Toggle bound to " + KeyCodes::toName(mouseVk);
        return true;
    }
    if (rebindingHoldUnlock_) {
        if (mouseVk == VK_LBUTTON) {
            out.toastMessage = "Use Keyboard, MMB, RMB, X1, X2, or Wheel for Hold-Unlock";
            return true;
        }
        profile.holdUnlockVk = mouseVk;
        rebindingHoldUnlock_ = false;
        out.profileModified = true;
        out.toastMessage = "Hold-to-Unlock bound to " + KeyCodes::toName(mouseVk);
        return true;
    }
    return false;
}

bool DrawerPanel::onChar(char ch, Profile& profile, AppSettings& settings,
                         int selectedBtnIndex, DrawerActionResult& out) {
    if (ch < 32 || ch > 126) return false;
    if (editingNewProfileName_) {
        if (newProfileName_.size() < 24) newProfileName_.push_back(ch);
        return true;
    }
    if (editingWireless_) {
        if (settings.lastWirelessAddr.size() < 28) {
            settings.lastWirelessAddr.push_back(ch);
            out.settingsModified = true;
        }
        return true;
    }
    if (editingNodeLabel_ && selectedBtnIndex >= 0 &&
        selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
        std::string& lbl = profile.buttons[selectedBtnIndex].label;
        if (lbl.size() < 12) {
            lbl.push_back(ch);
            out.profileModified = true;
        }
        return true;
    }
    return false;
}
