#include "ui/DrawerPanel.hpp"
#include "config/KeyCodes.hpp"
#include "config/ProfileStore.hpp"
#include "ui/EditorState.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

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
    DSL_ACCEL_POWER,
    DSL_ACCEL_REF,
    DSL_ACCEL_CAP,
    DSL_TDOWN_DELAY,
    DSL_TUP_DELAY,
    DSL_SCROLL_STEP,
    DSL_WALK_RADIUS,
    DSL_FIRE_BOX_W,
    DSL_NODE_DUR,
    DSL_NODE_RADIUS,
    DSL_PAD_DZ_L,
    DSL_PAD_DZ_R,
    DSL_PAD_LOOK_SENS
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

int dp(int px) {
    return GdiPlusDraw::dp(px);
}

bool containsCaseInsensitive(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            unsigned char a = static_cast<unsigned char>(hay[i + j]);
            unsigned char b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

} // namespace

DrawerPanel::DrawerPanel() {
    ZeroMemory(&bounds_, sizeof(bounds_));
    ZeroMemory(&bodyClip_, sizeof(bodyClip_));
    ZeroMemory(&activeSliderTrack_, sizeof(activeSliderTrack_));
    for (int i = 0; i < 5; ++i) scrollY_[i] = 0;
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
    if (tab > 4) tab = 4;
    if (tab != 4) selectedPadBtn_ = -1;
    activeTab_ = tab;
    clearFocus();
    if (tab == 2) refreshProfileList();
}

std::string DrawerPanel::specialRebindLabel() const {
    switch (specialRebind_) {
        case SRB_TOGGLE_AIM:  return "AIM LOCK / UNLOCK";
        case SRB_HOLD_UNLOCK: return "HOLD-TO-UNLOCK CURSOR";
        case SRB_RESET_TOUCH: return "RESET ALL TOUCHES";
        case SRB_JOY_UP:      return "JOYSTICK UP";
        case SRB_JOY_DOWN:    return "JOYSTICK DOWN";
        case SRB_JOY_LEFT:    return "JOYSTICK LEFT";
        case SRB_JOY_RIGHT:   return "JOYSTICK RIGHT";
        case SRB_JOY_SPRINT:  return "JOYSTICK SPRINT";
        case SRB_JOY_WALK:    return "JOYSTICK WALK";
        case SRB_CAST_CANCEL: return "SKILL CAST CANCEL KEY";
        default:              return "KEY";
    }
}

bool DrawerPanel::bindSelectedPadButtonToNode(Profile& profile, const std::string& label, std::string& outToast) {
    if (activeTab_ != 4 || selectedPadBtn_ < 0 || selectedPadBtn_ >= PAD_BUTTON_COUNT || label.empty()) {
        return false;
    }
    int bid = selectedPadBtn_;
    profile.padMap[bid] = label;
    selectedPadBtn_ = -1;
    outToast = std::string("Pad ") + ProfileDefaults::padButtonDisplayName(bid) + " -> " + label;
    return true;
}

bool DrawerPanel::containsPoint(int x, int y) const {
    return ptIn(bounds_, x, y);
}

bool DrawerPanel::hoverItemAt(int x, int y, int& outId, RECT& outRc) const {
    for (size_t i = 0; i < sliders_.size(); ++i) {
        if (ptIn(sliders_[i].hitRc, x, y)) {
            outId = 1000 + sliders_[i].id;
            outRc = sliders_[i].hitRc;
            return true;
        }
    }
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        if (ptIn(hotspots_[i].rc, x, y)) {
            outId = (hotspots_[i].id << 8) ^ (hotspots_[i].extra & 0xFF);
            outRc = hotspots_[i].rc;
            return true;
        }
    }
    return false;
}

bool DrawerPanel::rectForAction(int actionId, int extra, RECT& outRc) const {
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        if (hotspots_[i].id == actionId && (extra == -1 || hotspots_[i].extra == extra)) {
            outRc = hotspots_[i].rc;
            return true;
        }
    }
    return false;
}

void DrawerPanel::drawSliderRow(Gdiplus::Graphics& g, int x, int y, int w,
                                const char* label, float val, float minV, float maxV,
                                const char* valText, uint32_t accent, int sliderId,
                                int hoverX, int hoverY) {
    const int lblW = dp(104);
    const int valW = dp(48);
    const int trX = x + lblW;
    const int trW = w - lblW - valW - dp(6);
    const int cy = y + dp(13);

    GdiPlusDraw::textLeft(g, label, static_cast<float>(x), static_cast<float>(cy), 11.5f, false, Theme::TEXT_SECONDARY, static_cast<float>(lblW - dp(4)));

    RECT tr = { trX, cy - dp(3), trX + trW, cy + dp(3) };
    RECT hit = { trX - dp(8), y, trX + trW + dp(8), y + dp(26) };
    float norm = (maxV > minV) ? clampf((val - minV) / (maxV - minV), 0.0f, 1.0f) : 0.0f;
    bool hov = (activeSliderId_ == sliderId) || ptIn(hit, hoverX, hoverY);
    GdiPlusDraw::drawSlider(g, tr, norm, accent, hov);
    if (activeSliderId_ == sliderId) {
        // U4: Floating value bubble over active slider thumb
        float kx = static_cast<float>(tr.left) + static_cast<float>(tr.right - tr.left) * norm;
        int bw = dp(44);
        int bh = dp(18);
        int bx = static_cast<int>(kx) - bw / 2;
        int by = tr.top - bh - dp(4);
        if (bx < x) bx = x;
        if (bx + bw > x + w) bx = x + w - bw;
        RECT rBub = { bx, by, bx + bw, by + bh };
        GdiPlusDraw::roundedCard(g, rBub, GdiPlusDraw::dpf(5.0f), Theme::BG_SHELL, accent, 1.1f);
        GdiPlusDraw::textCenter(g, valText, static_cast<float>(rBub.left + rBub.right) * 0.5f,
                                static_cast<float>(rBub.top + rBub.bottom) * 0.5f, 9.5f, true, accent);
    }
    GdiPlusDraw::textRight(g, valText, static_cast<float>(x + w), static_cast<float>(cy), 11.5f, true, accent);

    if (hit.bottom > bodyClip_.top && hit.top < bodyClip_.bottom) {
        sliders_.push_back(SliderHit{ tr, hit, sliderId, minV, maxV });
    }
}

void DrawerPanel::drawToggleRow(Gdiplus::Graphics& g, int x, int y, int w,
                                const char* label, bool on, uint32_t accent, int actionId,
                                int hoverX, int hoverY) {
    RECT row = { x, y, x + w, y + dp(32) };
    bool hov = ptIn(row, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, row, GdiPlusDraw::dpf(7.0f), hov ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textLeft(g, label, static_cast<float>(x + dp(10)), static_cast<float>(y + dp(16)), 11.5f, false, Theme::TEXT_PRIMARY, static_cast<float>(w - dp(64)));
    RECT sw = { x + w - dp(48), y + dp(6), x + w - dp(8), y + dp(26) };
    GdiPlusDraw::drawToggleSwitch(g, sw, on, accent);
    if (row.bottom > bodyClip_.top && row.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ row, actionId, 0 });
    }
}

void DrawerPanel::paint(Gdiplus::Graphics& g, const RECT& rawBounds,
                        const Profile& profile, const AppSettings& settings,
                        const std::string& activeProfilePath,
                        int selectedBtnIndex, bool waitingNodeRebind,
                        bool connected, const std::string& deviceLabel,
                        const std::string& foregroundPkg,
                        const std::vector<std::string>& logs,
                        int hoverX, int hoverY,
                        int slideOffsetX) {
    RECT bounds = rawBounds;
    bounds.left += slideOffsetX;
    bounds.right += slideOffsetX;
    bounds_ = bounds;
    activeProfilePath_ = activeProfilePath;
    hotspots_.clear();
    sliders_.clear();
    focusables_.clear();

    GdiPlusDraw::fillRect(g, bounds, Theme::BG_DRAWER);
    GdiPlusDraw::vLine(g, bounds.left, bounds.top, bounds.bottom, Theme::BORDER_STRONG);

    const int x0 = bounds.left + dp(12);
    const int w = (bounds.right - bounds.left) - dp(24);
    const int hdrY = bounds.top + dp(10);

    // 1. Drawer Header + Close button
    static const char* kTitles[5] = {
        "KEYMAP STUDIO",
        "SENSITIVITY & PHYSICS",
        "PROFILES MANAGER",
        "DEVICE & STREAM",
        "GAMEPAD v2"
    };
    GdiPlusDraw::textLeft(g, kTitles[(activeTab_ >= 0 && activeTab_ < 5) ? activeTab_ : 0], static_cast<float>(x0), static_cast<float>(hdrY + dp(12)), 13.0f, true, Theme::ACCENT_CYAN, static_cast<float>(w - dp(32)));

    RECT rCls = { bounds.right - dp(34), hdrY, bounds.right - dp(12), hdrY + dp(24) };
    bool hovCls = ptIn(rCls, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rCls, GdiPlusDraw::dpf(6.0f), hovCls ? Theme::ACCENT_RED_BG : Theme::BG_CARD,
                             hovCls ? Theme::ACCENT_RED : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::drawIcon(g, ICON_CLOSE, static_cast<float>(rCls.left + rCls.right) * 0.5f,
                          static_cast<float>(rCls.top + rCls.bottom) * 0.5f, GdiPlusDraw::dpf(10.0f),
                          hovCls ? Theme::ACCENT_RED : Theme::TEXT_SECONDARY);
    hotspots_.push_back(Hotspot{ rCls, DA_CLOSE, 0 });

    // 2. 5 Navigation Tabs (KEYS | SENS | CFG | DEV | PAD)
    const char* tabNames[5] = { "KEYS", "SENS", "CFG", "DEV", "PAD" };
    int tabY = hdrY + dp(32);
    int tabW = (w - dp(16)) / 5;
    for (int i = 0; i < 5; ++i) {
        RECT tr = { x0 + i * (tabW + dp(4)), tabY, x0 + i * (tabW + dp(4)) + tabW, tabY + dp(26) };
        bool active = (activeTab_ == i);
        bool hov = ptIn(tr, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, tr, GdiPlusDraw::dpf(6.0f),
                                 active ? Theme::ACCENT_CYAN_BG : (hov ? Theme::BG_CARD_HOVER : Theme::BG_CARD),
                                 active ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE,
                                 active ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, tabNames[i],
                                static_cast<float>(tr.left + tr.right) * 0.5f,
                                static_cast<float>(tr.top + tr.bottom) * 0.5f,
                                9.8f, true, active ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        hotspots_.push_back(Hotspot{ tr, DA_TAB_0 + i, 0 });
    }
    const size_t chromeHdrHotspotsEnd = hotspots_.size();

    int sepTop = tabY + dp(34);
    int footerH = dp(46);
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

    int startY = bodyClip_.top + dp(10) - scrollY_[activeTab_];
    int endY = startY;
    if (activeTab_ == 0) {
        endY = paintTabKeys(g, x0, startY, w, profile, selectedBtnIndex, waitingNodeRebind, hoverX, hoverY);
    } else if (activeTab_ == 1) {
        endY = paintTabSens(g, x0, startY, w, profile, hoverX, hoverY);
    } else if (activeTab_ == 2) {
        endY = paintTabProfiles(g, x0, startY, w, profile, activeProfilePath, foregroundPkg, hoverX, hoverY);
    } else if (activeTab_ == 3) {
        endY = paintTabDevice(g, x0, startY, w, settings, connected, deviceLabel, logs, hoverX, hoverY);
    } else {
        endY = paintTabGamepad(g, x0, startY, w, profile, settings, hoverX, hoverY);
    }

    int contentH = (endY - startY) + dp(20);
    int viewH = bodyClip_.bottom - bodyClip_.top;
    maxScrollY_ = (contentH > viewH) ? (contentH - viewH) : 0;
    if (scrollY_[activeTab_] > maxScrollY_) scrollY_[activeTab_] = maxScrollY_;
    if (scrollY_[activeTab_] < 0) scrollY_[activeTab_] = 0;

    // Subtle scrollbar thumb when scrollable
    if (maxScrollY_ > 0 && viewH > dp(30)) {
        float thumbH = clampf(static_cast<float>(viewH * viewH) / static_cast<float>(contentH), GdiPlusDraw::dpf(28.0f), static_cast<float>(viewH - dp(8)));
        float ratio = static_cast<float>(scrollY_[activeTab_]) / static_cast<float>(maxScrollY_);
        float thumbY = static_cast<float>(bodyClip_.top + dp(4)) + ratio * (static_cast<float>(viewH - dp(8)) - thumbH);
        GdiPlusDraw::fillRoundedRect(g, static_cast<float>(bounds.right - dp(6)), thumbY, GdiPlusDraw::dpf(4.0f), thumbH, GdiPlusDraw::dpf(2.0f), Theme::BORDER_STRONG);
    }

    // Build focusable order: Tabs & Close -> Sorted Body Controls -> Footer Save & Done
    for (size_t hi = 0; hi < chromeHdrHotspotsEnd; ++hi) {
        FocusableItem fi;
        fi.rc = hotspots_[hi].rc;
        fi.isSlider = false;
        fi.inBody = false;
        fi.id = hotspots_[hi].id;
        fi.extra = hotspots_[hi].extra;
        focusables_.push_back(fi);
    }
    std::vector<FocusableItem> bodyFocus;
    for (size_t hi = chromeHdrHotspotsEnd; hi < hotspots_.size(); ++hi) {
        FocusableItem fi;
        fi.rc = hotspots_[hi].rc;
        fi.isSlider = false;
        fi.inBody = true;
        fi.id = hotspots_[hi].id;
        fi.extra = hotspots_[hi].extra;
        bodyFocus.push_back(fi);
    }
    for (size_t si = 0; si < sliders_.size(); ++si) {
        FocusableItem fi;
        fi.rc = sliders_[si].hitRc;
        fi.isSlider = true;
        fi.inBody = true;
        fi.id = sliders_[si].id;
        fi.extra = 0;
        fi.minVal = sliders_[si].minVal;
        fi.maxVal = sliders_[si].maxVal;
        bodyFocus.push_back(fi);
    }
    std::sort(bodyFocus.begin(), bodyFocus.end(), [](const FocusableItem& a, const FocusableItem& b) {
        int dy = (a.rc.top / 6) - (b.rc.top / 6);
        if (dy != 0) return dy < 0;
        return a.rc.left < b.rc.left;
    });
    focusables_.insert(focusables_.end(), bodyFocus.begin(), bodyFocus.end());

    if (focusedIdx_ >= 0 && focusedIdx_ < static_cast<int>(focusables_.size()) &&
        focusables_[focusedIdx_].inBody) {
        const RECT& fr = focusables_[focusedIdx_].rc;
        GdiPlusDraw::strokeRoundedRect(g,
            static_cast<float>(fr.left - 2), static_cast<float>(fr.top - 2),
            static_cast<float>(fr.right - fr.left + 4), static_cast<float>(fr.bottom - fr.top + 4),
            GdiPlusDraw::dpf(7.0f), Theme::ACCENT_CYAN, 2.0f);
    }

    g.Restore(savedState);

    // 4. Persistent Footer Bar (Auto-Save Status + Save Now + Done)
    int fy = sepBot + dp(9);
    GdiPlusDraw::textLeft(g, "Auto-Saved", static_cast<float>(x0), static_cast<float>(fy + dp(14)), 11.0f, true, Theme::ACCENT_GREEN, static_cast<float>(dp(90)));

    RECT rSave = { x0 + dp(96), fy, x0 + dp(214), fy + dp(28) };
    bool hovSave = ptIn(rSave, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rSave, GdiPlusDraw::dpf(7.0f), hovSave ? Theme::ACCENT_GREEN : Theme::ACCENT_GREEN_BG,
                             Theme::ACCENT_GREEN, 1.1f);
    GdiPlusDraw::textCenter(g, "SAVE (Ctrl+S)", static_cast<float>(rSave.left + rSave.right) * 0.5f,
                            static_cast<float>(fy + dp(14)), 11.0f, true, hovSave ? 0xFF07130Du : Theme::TEXT_PRIMARY);
    hotspots_.push_back(Hotspot{ rSave, DA_SAVE, 0 });
    {
        FocusableItem fi;
        fi.rc = rSave; fi.id = DA_SAVE;
        focusables_.push_back(fi);
    }

    RECT rDone = { rSave.right + dp(8), fy, bounds.right - dp(12), fy + dp(28) };
    bool hovDone = ptIn(rDone, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rDone, GdiPlusDraw::dpf(7.0f), hovDone ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG,
                             Theme::ACCENT_CYAN, 1.1f);
    GdiPlusDraw::textCenter(g, "DONE (F2)", static_cast<float>(rDone.left + rDone.right) * 0.5f,
                            static_cast<float>(fy + dp(14)), 11.0f, true, hovDone ? 0xFF07131Au : Theme::TEXT_PRIMARY);
    hotspots_.push_back(Hotspot{ rDone, DA_CLOSE, 0 });
    {
        FocusableItem fi;
        fi.rc = rDone; fi.id = DA_CLOSE;
        focusables_.push_back(fi);
    }

    if (focusedIdx_ >= static_cast<int>(focusables_.size())) {
        focusedIdx_ = focusables_.empty() ? -1 : (static_cast<int>(focusables_.size()) - 1);
    }
    if (focusedIdx_ >= 0 && focusedIdx_ < static_cast<int>(focusables_.size()) &&
        !focusables_[focusedIdx_].inBody) {
        const RECT& fr = focusables_[focusedIdx_].rc;
        GdiPlusDraw::strokeRoundedRect(g,
            static_cast<float>(fr.left - 2), static_cast<float>(fr.top - 2),
            static_cast<float>(fr.right - fr.left + 4), static_cast<float>(fr.bottom - fr.top + 4),
            GdiPlusDraw::dpf(7.0f), Theme::ACCENT_CYAN, 2.0f);
    }
}

int DrawerPanel::paintTabKeys(Gdiplus::Graphics& g, int x, int y, int w,
                              const Profile& p, int selectedBtnIndex, bool waitingRebind,
                              int hoverX, int hoverY) {
    // 0. Quick Edit <-> Live Test toggle (test keymap without closing KEYS panel!)
    {
        RECT rTest = { x, y, x + w, y + 28 };
        bool hovT = ptIn(rTest, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rTest, 7.0f,
                                 editTestLive_ ? (hovT ? Theme::ACCENT_AMBER : Theme::ACCENT_AMBER_BG)
                                               : (hovT ? Theme::ACCENT_GREEN : Theme::ACCENT_GREEN_BG),
                                 editTestLive_ ? Theme::ACCENT_AMBER : Theme::ACCENT_GREEN, 1.1f);
        GdiPlusDraw::textCenter(g,
            editTestLive_ ? "LIVE TEST ACTIVE (Click to resume Edit)" : "TEST LIVE (Pause Edit Mode)",
            static_cast<float>(x + w / 2), static_cast<float>(y + 14),
            10.5f, true,
            hovT ? 0xFF07130Du : (editTestLive_ ? Theme::ACCENT_AMBER : Theme::ACCENT_GREEN));
        if (rTest.bottom > bodyClip_.top && rTest.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rTest, DA_TOGGLE_EDIT_TEST, 0 });
        }
        y += 34;
    }

    // 1. Docked Selected Key Inspector (ALWAYS visible at top when a key is selected!)
    if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
        const KeyNode& node = p.buttons[selectedBtnIndex];
        std::vector<KeyConflict> conflicts = EditorGeometry::detectConflicts(p, true);
        std::string conflictMsg;
        for (size_t ci = 0; ci < conflicts.size(); ++ci) {
            if (conflicts[ci].buttonIndex == selectedBtnIndex) {
                conflictMsg = conflicts[ci].reason;
                break;
            }
        }
        int extraH = conflictMsg.empty() ? 0 : 22;
        if (node.action == NODE_SKILL_CAST) extraH += 94;
        RECT card = { x, y, x + w, y + 320 + extraH };
        GdiPlusDraw::roundedCard(g, card, 10.0f, Theme::BG_CARD,
                                 conflictMsg.empty() ? Theme::ACCENT_CYAN : Theme::ACCENT_AMBER, 1.4f);

        int ix = x + 10;
        int iw = w - 20;
        int cy = y + 10;
        GdiPlusDraw::textLeft(g, "SELECTED KEY INSPECTOR", static_cast<float>(ix), static_cast<float>(cy + 7), 10.5f, true, Theme::ACCENT_CYAN);
        cy += 22;

        if (!conflictMsg.empty()) {
            RECT rWarn = { ix, cy, ix + iw, cy + 18 };
            GdiPlusDraw::roundedCard(g, rWarn, 4.0f, Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, 0.9f);
            GdiPlusDraw::textLeft(g, "! " + conflictMsg, static_cast<float>(ix + 6), static_cast<float>(cy + 9),
                                  9.5f, true, Theme::ACCENT_AMBER, static_cast<float>(iw - 12));
            cy += 22;
        }

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
        GdiPlusDraw::textCenter(g, ProfileDefaults::actionTypeToHumanLabel(node.action), static_cast<float>(rAct.left + rAct.right) * 0.5f, static_cast<float>(cy + 13), 11.5f, true, Theme::ACCENT_GREEN);
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

        if (node.action == NODE_SKILL_CAST) {
            drawToggleRow(g, ix, cy, iw, "Quick Cast (Tap = Cast)", node.quickCast, 0xFF38D9A9u, DA_INSP_QUICK_CAST, hoverX, hoverY);
            cy += dp(36);

            char rbuf[16];
            std::snprintf(rbuf, sizeof(rbuf), "%.2f", node.radius);
            drawSliderRow(g, ix, cy, iw, "Cast Radius", node.radius, 0.04f, 0.30f, rbuf, 0xFF38D9A9u, DSL_NODE_RADIUS, hoverX, hoverY);
            cy += 30;

            bool clrAvail = (p.castCancelVk != 0);
            int cnlW = clrAvail ? (iw - 34) : iw;
            RECT rCnl = { ix, cy, ix + cnlW, cy + 24 };
            bool waitCnl = (specialRebind_ == SRB_CAST_CANCEL);
            std::string cnlTxt = waitCnl
                ? "Press Cancel Key..."
                : (std::string("Cast Cancel: ") + (clrAvail ? KeyCodes::toName(p.castCancelVk) : std::string("None")));
            GdiPlusDraw::roundedCard(g, rCnl, 5.0f,
                                     waitCnl ? Theme::ACCENT_AMBER_BG : (ptIn(rCnl, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_INPUT),
                                     waitCnl ? Theme::ACCENT_AMBER : Theme::BORDER_STRONG, 1.0f);
            GdiPlusDraw::textLeft(g, cnlTxt, static_cast<float>(ix + 8), static_cast<float>(cy + 12), 10.5f, true,
                                  waitCnl ? Theme::ACCENT_AMBER : Theme::TEXT_PRIMARY, static_cast<float>(cnlW - 14));
            if (rCnl.bottom > bodyClip_.top && rCnl.top < bodyClip_.bottom) {
                hotspots_.push_back(Hotspot{ rCnl, DA_REBIND_CAST_CANCEL, 0 });
            }
            if (clrAvail) {
                RECT rCnlClr = { rCnl.right + 4, cy, ix + iw, cy + 24 };
                GdiPlusDraw::roundedCard(g, rCnlClr, 5.0f, ptIn(rCnlClr, hoverX, hoverY) ? Theme::ACCENT_RED_BG : Theme::BG_INPUT,
                                         Theme::BORDER_STRONG, 1.0f);
                GdiPlusDraw::textCenter(g, "X", static_cast<float>(rCnlClr.left + rCnlClr.right) * 0.5f,
                                        static_cast<float>(cy + 12), 10.0f, true, Theme::ACCENT_RED);
                if (rCnlClr.bottom > bodyClip_.top && rCnlClr.top < bodyClip_.bottom) {
                    hotspots_.push_back(Hotspot{ rCnlClr, DA_CLEAR_CAST_CANCEL, 0 });
                }
            }
            cy += 30;
        }

        // Per-node add-on chips: ADS Sens | Toggle | Rst View | Fix Joy
        int tw = (iw - 12) / 4;
        RECT rScp = { ix, cy, ix + tw, cy + 24 };
        RECT rTog = { ix + tw + 4, cy, ix + 2 * tw + 4, cy + 24 };
        RECT rRst = { ix + 2 * tw + 8, cy, ix + 3 * tw + 8, cy + 24 };
        RECT rJoy = { ix + 3 * tw + 12, cy, ix + iw, cy + 24 };
        GdiPlusDraw::roundedCard(g, rScp, 5.0f, node.switchScopeSens ? Theme::ACCENT_PURPLE_BG : Theme::BG_INPUT, node.switchScopeSens ? Theme::ACCENT_PURPLE : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::roundedCard(g, rTog, 5.0f, node.toggleMode ? Theme::ACCENT_CYAN_BG : Theme::BG_INPUT, node.toggleMode ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::roundedCard(g, rRst, 5.0f, node.resetView ? Theme::ACCENT_AMBER_BG : Theme::BG_INPUT, node.resetView ? Theme::ACCENT_AMBER : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::roundedCard(g, rJoy, 5.0f, node.reactivateJoy ? Theme::ACCENT_GREEN_BG : Theme::BG_INPUT, node.reactivateJoy ? Theme::ACCENT_GREEN : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textCenter(g, "ADS Sens", static_cast<float>(rScp.left + rScp.right) * 0.5f, static_cast<float>(cy + 12), 9.5f, true, node.switchScopeSens ? Theme::ACCENT_PURPLE : Theme::TEXT_MUTED);
        GdiPlusDraw::textCenter(g, "Toggle",   static_cast<float>(rTog.left + rTog.right) * 0.5f, static_cast<float>(cy + 12), 9.5f, true, node.toggleMode ? Theme::ACCENT_CYAN : Theme::TEXT_MUTED);
        GdiPlusDraw::textCenter(g, "Rst View", static_cast<float>(rRst.left + rRst.right) * 0.5f, static_cast<float>(cy + 12), 9.5f, true, node.resetView ? Theme::ACCENT_AMBER : Theme::TEXT_MUTED);
        GdiPlusDraw::textCenter(g, "Fix Joy",  static_cast<float>(rJoy.left + rJoy.right) * 0.5f, static_cast<float>(cy + 12), 9.5f, true, node.reactivateJoy ? Theme::ACCENT_GREEN : Theme::TEXT_MUTED);
        if (rScp.bottom > bodyClip_.top && rScp.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rScp, DA_INSP_SCOPE, 0 });
            hotspots_.push_back(Hotspot{ rTog, DA_INSP_TOGGLE_MODE, 0 });
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

    // 2. Add New Controls Palette (10 action types + live search filter)
    GdiPlusDraw::textLeft(g, "ADD NEW CONTROL TO PHONE", static_cast<float>(x), static_cast<float>(y + 8), 10.5f, true, Theme::TEXT_MUTED);
    y += 22;

    RECT rPS = { x, y, x + w, y + 24 };
    GdiPlusDraw::roundedCard(g, rPS, 6.0f, Theme::BG_INPUT,
                             editingPaletteSearch_ ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE,
                             editingPaletteSearch_ ? 1.3f : 1.0f);
    std::string psTxt = paletteSearch_.empty()
        ? (editingPaletteSearch_ ? "Type to filter controls... _" : "Filter controls (Tap, Hold, Swipe, Scope)...")
        : ("Filter: " + paletteSearch_ + (editingPaletteSearch_ ? "_" : ""));
    GdiPlusDraw::textLeft(g, psTxt, static_cast<float>(x + 9), static_cast<float>(y + 12), 10.5f,
                          !paletteSearch_.empty(),
                          paletteSearch_.empty() ? Theme::TEXT_MUTED : Theme::TEXT_PRIMARY,
                          static_cast<float>(w - 18));
    if (rPS.bottom > bodyClip_.top && rPS.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rPS, DA_FOCUS_PALETTE_SEARCH, 0 });
    }
    y += 30;

    struct PaletteSpec {
        const char* title;
        const char* sub;
        KeyActionType type;
        uint32_t accent;
    };
    static const PaletteSpec items[11] = {
        { "+ Hold Key",      "Hold touch down",      NODE_PRESS,          Theme::ACCENT_CYAN },
        { "+ Tap",           "Fixed single tap",     NODE_CLICK,          Theme::ACCENT_CYAN },
        { "+ Tap Down & Up", "ADS / hold-to-scope",  NODE_DOWN_UP,        Theme::ACCENT_GREEN },
        { "+ Free Look",     "Temp drag-look",       NODE_HOLD_VIEW,      0xFF00F0F0u },
        { "+ Swipe",         "Drag A -> B",          NODE_SWIPE,          Theme::ACCENT_AMBER },
        { "+ Cursor Toggle", "Bag / Map unlock",     NODE_CURSOR_TOGGLE,  Theme::ACCENT_PURPLE },
        { "+ Reset Camera",  "Re-center view",       NODE_RESET_VIEW,     Theme::ACCENT_AMBER },
        { "+ Sensitivity",   "Scope / ADS switch",   NODE_SWITCH_SENS,    Theme::ACCENT_PURPLE },
        { "+ Reset Joy",     "Unstick WASD",         NODE_REACTIVATE_JOY, Theme::ACCENT_GREEN },
        { "+ Release Tap",   "Tap on key release",   NODE_UP_TRIGGER,     Theme::TEXT_SECONDARY },
        { "+ Skill Cast",    "MOBA cursor aim+cnl",  NODE_SKILL_CAST,     0xFF38D9A9u }
    };

    std::vector<int> matched;
    for (int i = 0; i < 11; ++i) {
        if (containsCaseInsensitive(items[i].title, paletteSearch_) ||
            containsCaseInsensitive(items[i].sub, paletteSearch_) ||
            containsCaseInsensitive(ProfileDefaults::actionTypeToHumanLabel(items[i].type), paletteSearch_)) {
            matched.push_back(i);
        }
    }

    int colW = (w - 8) / 2;
    int tileH = 44;
    if (matched.empty()) {
        RECT rEmpty = { x, y, x + w, y + 32 };
        GdiPlusDraw::roundedCard(g, rEmpty, 7.0f, Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textCenter(g, "No controls match '" + paletteSearch_ + "'",
                                static_cast<float>(x + w / 2), static_cast<float>(y + 16),
                                10.5f, false, Theme::TEXT_MUTED);
        y += 38;
    } else {
        for (size_t mi = 0; mi < matched.size(); ++mi) {
            int i = matched[mi];
            int col = static_cast<int>(mi) % 2;
            int row = static_cast<int>(mi) / 2;
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
        int rows = (static_cast<int>(matched.size()) + 1) / 2;
        y += rows * (tileH + 6) + 8;
    }

    // U4: Danger Zone card
    RECT rDZ = { x, y, x + w, y + 54 };
    GdiPlusDraw::roundedCard(g, rDZ, 8.0f, Theme::BG_CARD, Theme::SEM_DANGER, 1.0f);
    GdiPlusDraw::textLeft(g, "DANGER ZONE", static_cast<float>(x + 10), static_cast<float>(y + 11), 9.5f, true, Theme::SEM_DANGER);
    RECT rClr = { x + 8, y + 20, x + w - 8, y + 46 };
    bool hovClr = ptIn(rClr, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rClr, 6.0f, hovClr ? Theme::ACCENT_RED : Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
    GdiPlusDraw::textCenter(g, "CLEAR ALL EXTRA KEYS (Keep WASD / AIM / FIRE)",
                            static_cast<float>(x + w / 2), static_cast<float>(y + 33),
                            10.0f, true, hovClr ? 0xFFFFFFFFu : Theme::ACCENT_RED);
    if (rClr.bottom > bodyClip_.top && rClr.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rClr, DA_CLEAR_BUTTONS, 0 });
    return rDZ.bottom + 8;
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
    bool rbAim = (specialRebind_ == SRB_TOGGLE_AIM);
    GdiPlusDraw::roundedCard(g, rAimK, 6.0f, rbAim ? Theme::ACCENT_AMBER_BG : Theme::BG_CARD,
                             rbAim ? Theme::ACCENT_AMBER : Theme::BORDER_STRONG, 1.1f);
    std::string aimLbl = rbAim ? "Press Key / Mouse..." : ("Lock/Unlock: " + KeyCodes::toName(p.toggleAimVk));
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
    bool rbHold = (specialRebind_ == SRB_HOLD_UNLOCK);
    GdiPlusDraw::roundedCard(g, rHoldK, 6.0f, rbHold ? Theme::ACCENT_AMBER_BG : Theme::BG_CARD,
                             rbHold ? Theme::ACCENT_AMBER : Theme::BORDER_STRONG, 1.1f);
    std::string holdLbl = rbHold ? "Press Key / Mouse..." : ("Hold-Unlock: " + KeyCodes::toName(p.holdUnlockVk));
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
    y += 36;

    // F16: Collapsible Advanced Tuning & Keys section
    RECT rAdvHdr = { x, y, x + w, y + 30 };
    bool hovAdv = ptIn(rAdvHdr, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rAdvHdr, 7.0f, hovAdv ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             advancedExpanded_ ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, 1.1f);
    GdiPlusDraw::textLeft(g, advancedExpanded_ ? "v ADVANCED TUNING & KEYS" : "> ADVANCED TUNING & KEYS",
                          static_cast<float>(x + 12), static_cast<float>(y + 15), 11.0f, true,
                          advancedExpanded_ ? Theme::ACCENT_CYAN : Theme::TEXT_PRIMARY, static_cast<float>(w - 24));
    if (rAdvHdr.bottom > bodyClip_.top && rAdvHdr.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rAdvHdr, DA_TOGGLE_ADVANCED, 0 });
    }
    y += 36;

    if (advancedExpanded_) {
        std::snprintf(buf, sizeof(buf), "%.2f", p.accelPower);
        drawSliderRow(g, x, y, w, "Accel Power", p.accelPower, 1.00f, 2.50f, buf, Theme::ACCENT_PURPLE, DSL_ACCEL_POWER, hoverX, hoverY); y += step;
        std::snprintf(buf, sizeof(buf), "%.0f", p.accelRefSpeed);
        drawSliderRow(g, x, y, w, "Accel Ref Spd", p.accelRefSpeed, 2.0f, 80.0f, buf, Theme::ACCENT_PURPLE, DSL_ACCEL_REF, hoverX, hoverY); y += step;
        std::snprintf(buf, sizeof(buf), "%.2fx", p.accelCap);
        drawSliderRow(g, x, y, w, "Accel Cap", p.accelCap, 1.00f, 4.00f, buf, Theme::ACCENT_PURPLE, DSL_ACCEL_CAP, hoverX, hoverY); y += step;
        std::snprintf(buf, sizeof(buf), "%dms", p.touchDownDelayMs);
        drawSliderRow(g, x, y, w, "Touch Down Dly", static_cast<float>(p.touchDownDelayMs), 0.0f, 50.0f, buf, Theme::ACCENT_AMBER, DSL_TDOWN_DELAY, hoverX, hoverY); y += step;
        std::snprintf(buf, sizeof(buf), "%dms", p.touchUpDelayMs);
        drawSliderRow(g, x, y, w, "Touch Up Dly", static_cast<float>(p.touchUpDelayMs), 0.0f, 50.0f, buf, Theme::ACCENT_AMBER, DSL_TUP_DELAY, hoverX, hoverY); y += step;
        std::snprintf(buf, sizeof(buf), "%dpx", p.cursorScrollStepPx);
        drawSliderRow(g, x, y, w, "Scroll Step", static_cast<float>(p.cursorScrollStepPx), 20.0f, 250.0f, buf, Theme::ACCENT_CYAN, DSL_SCROLL_STEP, hoverX, hoverY); y += step;
        std::snprintf(buf, sizeof(buf), "%.3f", p.joyWalkRadius);
        drawSliderRow(g, x, y, w, "Walk Radius", p.joyWalkRadius, 0.015f, 0.120f, buf, Theme::ACCENT_GREEN, DSL_WALK_RADIUS, hoverX, hoverY); y += step;
        std::snprintf(buf, sizeof(buf), "%.2f", p.fireBoxHalfW);
        drawSliderRow(g, x, y, w, "Fire Box Width", p.fireBoxHalfW, 0.08f, 0.70f, buf, Theme::ACCENT_RED, DSL_FIRE_BOX_W, hoverX, hoverY); y += step + 6;

        GdiPlusDraw::textLeft(g, "MOVEMENT & RESET KEYS", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
        y += 20;

        auto drawKeyBtn = [&](RECT r, const char* prefix, int vkVal, SpecialRebindTarget rbTgt, int actId) {
            bool rb = (specialRebind_ == rbTgt);
            bool hovK = ptIn(r, hoverX, hoverY);
            GdiPlusDraw::roundedCard(g, r, 6.0f, rb ? Theme::ACCENT_AMBER_BG : (hovK ? Theme::BG_CARD_HOVER : Theme::BG_CARD),
                                     rb ? Theme::ACCENT_AMBER : Theme::BORDER_STRONG, 1.0f);
            std::string txt = rb ? "Press Key..." : (std::string(prefix) + ": " + KeyCodes::toName(vkVal));
            GdiPlusDraw::textCenter(g, txt, static_cast<float>(r.left + r.right) * 0.5f,
                                    static_cast<float>(r.top + r.bottom) * 0.5f, 10.5f, true, Theme::TEXT_PRIMARY);
            if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) {
                hotspots_.push_back(Hotspot{ r, actId, 0 });
            }
        };

        int khw = (w - 6) / 2;
        drawKeyBtn(RECT{ x, y, x + khw, y + 26 }, "Up", p.vkUp, SRB_JOY_UP, DA_REBIND_JOY_UP);
        drawKeyBtn(RECT{ x + khw + 6, y, x + w, y + 26 }, "Down", p.vkDown, SRB_JOY_DOWN, DA_REBIND_JOY_DOWN);
        y += 30;
        drawKeyBtn(RECT{ x, y, x + khw, y + 26 }, "Left", p.vkLeft, SRB_JOY_LEFT, DA_REBIND_JOY_LEFT);
        drawKeyBtn(RECT{ x + khw + 6, y, x + w, y + 26 }, "Right", p.vkRight, SRB_JOY_RIGHT, DA_REBIND_JOY_RIGHT);
        y += 30;
        drawKeyBtn(RECT{ x, y, x + khw, y + 26 }, "Sprint", p.vkSprint, SRB_JOY_SPRINT, DA_REBIND_JOY_SPRINT);
        drawKeyBtn(RECT{ x + khw + 6, y, x + w - 46, y + 26 }, "Walk", p.vkWalk, SRB_JOY_WALK, DA_REBIND_JOY_WALK);
        RECT rClrW = { x + w - 42, y, x + w, y + 26 };
        GdiPlusDraw::roundedCard(g, rClrW, 6.0f, Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
        GdiPlusDraw::textCenter(g, "CLR", static_cast<float>(rClrW.left + rClrW.right) * 0.5f, static_cast<float>(y + 13), 10.0f, true, Theme::ACCENT_RED);
        if (rClrW.bottom > bodyClip_.top && rClrW.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rClrW, DA_CLEAR_JOY_WALK, 0 });
        y += 30;
        drawKeyBtn(RECT{ x, y, x + w, y + 26 }, "Quick Reset Key", p.resetTouchVk, SRB_RESET_TOUCH, DA_REBIND_RESET_TOUCH);
        y += 34;
    }
    return y;
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

    // 2x3 Starter Templates Gallery (F4)
    GdiPlusDraw::textLeft(g, "STARTER TEMPLATES", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 20;
    const int tColW = (w - 8) / 2;
    const int tTileH = 26;
    for (int ti = 0; ti < 6; ++ti) {
        int col = ti % 2;
        int row = ti / 2;
        RECT rt = { x + col * (tColW + 8), y + row * (tTileH + 6), x + col * (tColW + 8) + tColW, y + row * (tTileH + 6) + tTileH };
        bool hovT = ptIn(rt, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rt, 6.0f, hovT ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                                 hovT ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textCenter(g, ProfileDefaults::templateName(static_cast<ProfileTemplateKind>(ti)),
                                static_cast<float>(rt.left + rt.right) * 0.5f,
                                static_cast<float>(rt.top + rt.bottom) * 0.5f,
                                10.0f, true, hovT ? Theme::ACCENT_CYAN : Theme::TEXT_PRIMARY);
        if (rt.bottom > bodyClip_.top && rt.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rt, DA_CREATE_TEMPLATE, ti });
        }
    }
    y += 3 * (tTileH + 6) + 8;

    // Import / Export / Copy Text (F4)
    int tw3 = (w - 12) / 3;
    RECT rImp = { x, y, x + tw3, y + 26 };
    RECT rExp = { rImp.right + 6, y, rImp.right + 6 + tw3, y + 26 };
    RECT rCpy = { rExp.right + 6, y, x + w, y + 26 };
    GdiPlusDraw::roundedCard(g, rImp, 6.0f, ptIn(rImp, hoverX, hoverY) ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, Theme::ACCENT_CYAN, 1.0f);
    GdiPlusDraw::roundedCard(g, rExp, 6.0f, ptIn(rExp, hoverX, hoverY) ? Theme::ACCENT_GREEN_BG : Theme::BG_CARD, Theme::ACCENT_GREEN, 1.0f);
    GdiPlusDraw::roundedCard(g, rCpy, 6.0f, ptIn(rCpy, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "Import .cfg", static_cast<float>(rImp.left + rImp.right) * 0.5f, static_cast<float>(y + 13), 10.0f, true, Theme::ACCENT_CYAN);
    GdiPlusDraw::textCenter(g, "Export .cfg", static_cast<float>(rExp.left + rExp.right) * 0.5f, static_cast<float>(y + 13), 10.0f, true, Theme::ACCENT_GREEN);
    GdiPlusDraw::textCenter(g, "Copy Text", static_cast<float>(rCpy.left + rCpy.right) * 0.5f, static_cast<float>(y + 13), 10.0f, true, Theme::TEXT_SECONDARY);
    if (rImp.bottom > bodyClip_.top && rImp.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rImp, DA_IMPORT_PROFILE, 0 });
        hotspots_.push_back(Hotspot{ rExp, DA_EXPORT_PROFILE, 0 });
        hotspots_.push_back(Hotspot{ rCpy, DA_COPY_PROFILE_TEXT, 0 });
    }
    y += 34;

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
    auto drawSectionHdr = [&](UiIcon icon, const char* title, uint32_t accent) {
        GdiPlusDraw::drawIcon(g, icon, static_cast<float>(x + 7), static_cast<float>(y + 8), 12.0f, accent);
        GdiPlusDraw::textLeft(g, title, static_cast<float>(x + 18), static_cast<float>(y + 8), 10.5f, true, Theme::TEXT_MUTED);
        y += 20;
    };

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

    // Connection Summary Card
    RECT rDev = { x, y, x + w, y + 66 };
    GdiPlusDraw::roundedCard(g, rDev, 9.0f, Theme::BG_CARD, connected ? Theme::ACCENT_GREEN : Theme::BORDER_STRONG, 1.2f);
    GdiPlusDraw::textLeft(g, connected ? "CONNECTED PHONE" : "DISCONNECTED", static_cast<float>(x + 12), static_cast<float>(y + 15), 10.0f, true,
                          connected ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER);
    GdiPlusDraw::textLeft(g, connected ? deviceLabel : "Open Device Hub or plug in USB", static_cast<float>(x + 12), static_cast<float>(y + 34), 12.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - 24));
    GdiPlusDraw::textLeft(g, "Stream: " + s.buildScrcpyArgs(), static_cast<float>(x + 12), static_cast<float>(y + 52), 10.0f, false, Theme::TEXT_MUTED, static_cast<float>(w - 24));
    y += 76;

    // F16 / N16 / X8: Pending stream-settings bar ("N changes need reconnect · Apply")
    if (connected && pendingStreamChangeCount_ > 0) {
        RECT rPend = { x, y, x + w, y + 36 };
        GdiPlusDraw::roundedCard(g, rPend, 8.0f, Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, 1.2f);
        char pbuf[64];
        if (pendingStreamChangeCount_ == 1) {
            std::snprintf(pbuf, sizeof(pbuf), "1 change needs reconnect");
        } else {
            std::snprintf(pbuf, sizeof(pbuf), "%d changes need reconnect", pendingStreamChangeCount_);
        }
        GdiPlusDraw::textLeft(g, pbuf, static_cast<float>(x + 10), static_cast<float>(y + 18), 11.0f, true, Theme::ACCENT_AMBER, static_cast<float>(w - 92));
        RECT rRecNow = { rPend.right - 74, y + 5, rPend.right - 8, y + 31 };
        bool hovRn = ptIn(rRecNow, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rRecNow, 6.0f, hovRn ? Theme::ACCENT_AMBER : Theme::BG_CARD, Theme::ACCENT_AMBER, 1.1f);
        GdiPlusDraw::textCenter(g, "Apply", static_cast<float>(rRecNow.left + rRecNow.right) * 0.5f,
                                static_cast<float>(y + 18), 10.5f, true, hovRn ? 0xFF081018u : Theme::TEXT_PRIMARY);
        if (rRecNow.bottom > bodyClip_.top && rRecNow.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rRecNow, DA_APPLY_RECONNECT, 0 });
        }
        y += 44;
    }

    // 1. DEVICE
    drawSectionHdr(ICON_QR, "DEVICE", Theme::ACCENT_CYAN);
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
    y += 34;
    drawToggleRow(g, x, y, w, "Turn Phone Screen Off", s.turnScreenOff, Theme::ACCENT_CYAN, DA_TOGGLE_SCREEN_OFF, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Keep Phone Awake", s.stayAwake, Theme::ACCENT_CYAN, DA_TOGGLE_STAY_AWAKE, hoverX, hoverY); y += 36;
    RECT rGuide = { x, y, x + w, y + 28 };
    bool hovG = ptIn(rGuide, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rGuide, 7.0f, hovG ? Theme::BG_CARD_HOVER : Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "OPEN SETUP GUIDE / ONBOARDING", static_cast<float>(x + w / 2),
                            static_cast<float>(y + 14), 10.5f, true, Theme::TEXT_SECONDARY);
    if (rGuide.bottom > bodyClip_.top && rGuide.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rGuide, DA_OPEN_ONBOARDING, 0 });
    }
    y += 38;

    // 2. STREAM
    drawSectionHdr(ICON_CAST, "STREAM", Theme::ACCENT_GREEN);
    const char* spLbls[4] = { "Comp", "Bal", "Qual", "WiFi" };
    const StreamPresetId spIds[4] = { PRESET_COMPETITIVE, PRESET_BALANCED, PRESET_QUALITY, PRESET_WIFI };
    StreamPresetId curPreset = s.detectStreamPreset();
    int spw = (w - 12) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT r = { x + i * (spw + 4), y, x + i * (spw + 4) + spw, y + 26 };
        bool on = (curPreset == spIds[i]);
        GdiPlusDraw::roundedCard(g, r, 6.0f, on ? Theme::ACCENT_GREEN_BG : Theme::BG_CARD, on ? Theme::ACCENT_GREEN : Theme::BORDER_SUBTLE, on ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, spLbls[i], static_cast<float>(r.left + r.right) * 0.5f, static_cast<float>(y + 13), 10.5f, true, on ? Theme::ACCENT_GREEN : Theme::TEXT_SECONDARY);
        if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ r, DA_STREAM_PRESET, static_cast<int>(spIds[i]) });
    }
    y += 34;

    GdiPlusDraw::textLeft(g, "MIRROR MAX FPS", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 18;
    const char* fLbls[4] = { "Uncapped", "60", "90", "120" };
    const int fVals[4] = { 0, 60, 90, 120 };
    int fw = (w - 12) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT r = { x + i * (fw + 4), y, x + i * (fw + 4) + fw, y + 26 };
        bool on = (s.mirrorFps == fVals[i]);
        GdiPlusDraw::roundedCard(g, r, 6.0f, on ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, on ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, on ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, fLbls[i], static_cast<float>(r.left + r.right) * 0.5f, static_cast<float>(y + 13), 10.5f, true, on ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ r, DA_STREAM_FPS, fVals[i] });
    }
    y += 34;

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

    GdiPlusDraw::textLeft(g, "VIDEO RESOLUTION (2.1)", static_cast<float>(x), static_cast<float>(y + 6), 10.5f, true, Theme::TEXT_MUTED);
    y += 18;
    const char* rLbls[4] = { "Native", "1920", "1600", "1280" };
    const int rVals[4] = { 0, 1920, 1600, 1280 };
    for (int i = 0; i < 4; ++i) {
        RECT r = { x + i * (bw + 4), y, x + i * (bw + 4) + bw, y + 26 };
        bool on = (s.mirrorMaxSize == rVals[i]);
        GdiPlusDraw::roundedCard(g, r, 6.0f, on ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, on ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, on ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, rLbls[i], static_cast<float>(r.left + r.right) * 0.5f, static_cast<float>(y + 13), 10.5f, true, on ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        if (r.bottom > bodyClip_.top && r.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ r, DA_STREAM_MAXSIZE, rVals[i] });
    }
    y += 34;

    drawToggleRow(g, x, y, w, "Forward Phone Audio (scrcpy 5.0)", s.mirrorAudio, Theme::ACCENT_GREEN, DA_TOGGLE_AUDIO, hoverX, hoverY); y += 36;
    if (s.mirrorAudio) {
        drawSeg3("AUDIO RELAY BUFFER (1.6)", "20ms", "25ms", "30ms", 20, 25, 30, s.audioBufferMs, DA_AUDIO_BUFFER);
        drawToggleRow(g, x, y, w, "Raw PCM Audio (USB Low-Latency)", s.audioRawCodec, Theme::ACCENT_GREEN, DA_TOGGLE_AUDIO_RAW, hoverX, hoverY); y += 36;
    }
    y += 4;

    // 3. INPUT
    drawSectionHdr(ICON_KEYMAP, "INPUT", Theme::ACCENT_AMBER);
    drawToggleRow(g, x, y, w, "Auto-Switch Profile by Open App", s.autoSwitchProfiles, Theme::ACCENT_GREEN, DA_TOGGLE_AUTO_SWITCH, hoverX, hoverY); y += 36;
    drawToggleRow(g, x, y, w, "Right-Click = Android Back (Cursor)", s.rightClickBack, Theme::ACCENT_CYAN, DA_TOGGLE_RCLICK_BACK, hoverX, hoverY); y += 40;

    // 4. GAMEPAD
    drawSectionHdr(ICON_GAMEPAD, "GAMEPAD", Theme::ACCENT_GREEN);
    drawToggleRow(g, x, y, w, "XInput Gamepad-to-Touch Controller", s.gamepadEnabled, Theme::ACCENT_GREEN, DA_TOGGLE_GAMEPAD, hoverX, hoverY); y += 40;

    // 5. PRIVACY & LOGS
    drawSectionHdr(ICON_SHIELD, "PRIVACY & LOGS", Theme::ACCENT_CYAN);
    RECT rLatCard = { x, y, x + w, y + 90 };
    GdiPlusDraw::roundedCard(g, rLatCard, 8.0f, Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
    char modeLine[112];
    std::snprintf(modeLine, sizeof(modeLine), "Present: %s | %.0ffps | rx->dec %.1f/%.1fms",
                  presentModeLabel_.empty() ? "Idle" : presentModeLabel_.c_str(),
                  vidTelemetry_.measuredFps,
                  vidTelemetry_.rxToDecodeP50Ms,
                  vidTelemetry_.rxToDecodeP95Ms);
    GdiPlusDraw::textLeft(g, modeLine, static_cast<float>(x + 10), static_cast<float>(y + 12), 10.0f, false, Theme::TEXT_SECONDARY, static_cast<float>(w - 20));
    if (thermalInfo_.valid) {
        char thBuf[112];
        bool warm = (thermalInfo_.batteryTempC >= 40.0f || thermalInfo_.thermalStatus >= 2);
        std::snprintf(thBuf, sizeof(thBuf), "Bat: %.1fC (%d%%) | Thermal: %s%s",
                      thermalInfo_.batteryTempC,
                      thermalInfo_.batteryLevel,
                      thermalInfo_.thermalLabel.c_str(),
                      warm ? " (Use Screen-Off!)" : "");
        GdiPlusDraw::textLeft(g, thBuf, static_cast<float>(x + 10), static_cast<float>(y + 27), 10.0f, warm,
                              warm ? Theme::ACCENT_AMBER : Theme::ACCENT_GREEN, static_cast<float>(w - 20));
    } else {
        GdiPlusDraw::textLeft(g, "Bat/Thermal: querying connected phone...", static_cast<float>(x + 10), static_cast<float>(y + 27), 10.0f, false, Theme::TEXT_MUTED, static_cast<float>(w - 20));
    }
    std::string probeTxt = probeResult_.running
        ? probeResult_.summaryLine
        : (probeResult_.valid ? probeResult_.summaryLine : "No round-trip probe run yet");
    GdiPlusDraw::textLeft(g, probeTxt, static_cast<float>(x + 10), static_cast<float>(y + 43), 10.5f, true,
                          probeResult_.valid ? Theme::ACCENT_GREEN : Theme::TEXT_MUTED, static_cast<float>(w - 20));
    RECT rProbeBtn = { x + 10, y + 58, x + w - 10, y + 82 };
    bool hovPr = ptIn(rProbeBtn, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rProbeBtn, 6.0f,
                             probeResult_.running ? Theme::ACCENT_AMBER_BG : (hovPr ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG),
                             probeResult_.running ? Theme::ACCENT_AMBER : Theme::ACCENT_CYAN, 1.0f);
    GdiPlusDraw::textCenter(g, probeResult_.running ? "PROBING ROUND-TRIP LATENCY..." : "LATENCY TEST (50-100x Round-Trip)",
                            static_cast<float>(rProbeBtn.left + rProbeBtn.right) * 0.5f, static_cast<float>(y + 70), 10.5f, true,
                            hovPr && !probeResult_.running ? 0xFF08121Au : Theme::TEXT_PRIMARY);
    if (rProbeBtn.bottom > bodyClip_.top && rProbeBtn.top < bodyClip_.bottom && !probeResult_.running) {
        hotspots_.push_back(Hotspot{ rProbeBtn, DA_RUN_LATENCY_PROBE, 0 });
    }
    y += 96;

    RECT rLog = { x, y, x + w, y + 114 };
    GdiPlusDraw::roundedCard(g, rLog, 8.0f, Theme::BG_INPUT, Theme::BORDER_SUBTLE, 1.0f);
    int ly = y + 12;
    size_t startIdx = (logs.size() > 6) ? (logs.size() - 6) : 0;
    for (size_t i = startIdx; i < logs.size(); ++i) {
        GdiPlusDraw::textLeft(g, logs[i], static_cast<float>(x + 8), static_cast<float>(ly), 10.0f, false, Theme::TEXT_SECONDARY, static_cast<float>(w - 16));
        ly += 17;
    }
    if (logs.empty()) {
        GdiPlusDraw::textLeft(g, "Ready. Connect phone or edit keymaps.", static_cast<float>(x + 8), static_cast<float>(ly), 10.0f, false, Theme::TEXT_MUTED, static_cast<float>(w - 16));
    }
    y = rLog.bottom + 10;

    // 6. ADVANCED (F16 - collapsible)
    RECT rAdv = { x, y, x + w, y + 28 };
    GdiPlusDraw::roundedCard(g, rAdv, 7.0f, ptIn(rAdv, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::drawIcon(g, ICON_APPS, static_cast<float>(x + 14), static_cast<float>(y + 14), 12.0f, Theme::ACCENT_PURPLE);
    GdiPlusDraw::textLeft(g, devAdvancedExpanded_ ? "ADVANCED (F16)  [-]" : "ADVANCED (F16)  [+]",
                          static_cast<float>(x + 28), static_cast<float>(y + 14), 10.5f, true, Theme::TEXT_SECONDARY);
    if (rAdv.bottom > bodyClip_.top && rAdv.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rAdv, DA_TOGGLE_DEV_ADVANCED, 0 });
    }
    y += 34;

    if (devAdvancedExpanded_) {
        int curOpts = 0;
        if (s.videoCodecOptions == "bitrate-mode=2,max-bframes=0") curOpts = 1;
        else if (s.videoCodecOptions == "intra-refresh-period=60,bitrate-mode=2,max-bframes=0") curOpts = 2;
        drawSeg3("ENCODER LOW-LATENCY KEYS (2.2)", "Default", "CBR+NoB", "WiFi Intra", 0, 1, 2, curOpts, DA_CODEC_OPTS);

        std::string encShown = s.encoderForDevice(activeDeviceSerial_);
        if (encShown.empty()) encShown = activeEncoder_;
        std::string encLabel = encShown.empty() ? "Auto (Hardware)" : encShown;
        RECT rEnc = { x, y, x + w, y + 30 };
        bool hovEnc = ptIn(rEnc, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rEnc, 7.0f, hovEnc ? Theme::BG_CARD_HOVER : Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textLeft(g, "Encoder (2.3): " + encLabel, static_cast<float>(x + 10), static_cast<float>(y + 15), 11.0f, false, Theme::TEXT_PRIMARY, static_cast<float>(w - 72));
        GdiPlusDraw::textRight(g, "Cycle", static_cast<float>(x + w - 10), static_cast<float>(y + 15), 10.5f, true, Theme::ACCENT_CYAN);
        if (rEnc.bottom > bodyClip_.top && rEnc.top < bodyClip_.bottom) hotspots_.push_back(Hotspot{ rEnc, DA_CYCLE_ENCODER, 0 });
        y += 36;

        drawToggleRow(g, x, y, w, "Native D3D11 Single-Stream Video", s.useNativeVideo, Theme::ACCENT_CYAN, DA_TOGGLE_NATIVE_VIDEO, hoverX, hoverY); y += 36;
        drawToggleRow(g, x, y, w, "VSync (Waitable, No Tearing)", s.vsyncEnabled, Theme::ACCENT_CYAN, DA_TOGGLE_VSYNC, hoverX, hoverY); y += 36;
        drawToggleRow(g, x, y, w, "Second-Phone Virtual Display", s.virtualDisplayMode, Theme::ACCENT_PURPLE, DA_TOGGLE_VIRTUAL_DISPLAY, hoverX, hoverY); y += 34;

        if (s.virtualDisplayMode) {
            int curVd = 0;
            if (s.virtualDisplaySpec == "1280x720/240") curVd = 1;
            else if (s.virtualDisplaySpec == "auto") curVd = 2;
            drawSeg3("VIRTUAL DISPLAY SIZE", "1080p PC", "720p Fast", "Auto", 0, 1, 2, curVd, DA_VD_PRESET);
            GdiPlusDraw::textLeft(g, "Note: Android virtual displays may cap games at 60 fps (2.6).", static_cast<float>(x + 4), static_cast<float>(y - 2), 9.5f, false, Theme::TEXT_MUTED, static_cast<float>(w - 8));
            y += 14;
        }
    }

    // U4: Danger Zone / Stream Session Controls
    RECT rDZ = { x, y, x + w, y + 58 };
    GdiPlusDraw::roundedCard(g, rDZ, 8.0f, Theme::BG_CARD, Theme::SEM_DANGER, 1.0f);
    GdiPlusDraw::textLeft(g, "STREAM SESSION & DANGER ZONE", static_cast<float>(x + 10), static_cast<float>(y + 11), 9.5f, true, Theme::SEM_DANGER);
    int hw = (w - 24) / 2;
    RECT rRec = { x + 8, y + 22, x + 8 + hw + 18, y + 50 };
    RECT rDis = { rRec.right + 8, y + 22, x + w - 8, y + 50 };
    GdiPlusDraw::roundedCard(g, rRec, 6.0f, ptIn(rRec, hoverX, hoverY) ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG, Theme::ACCENT_CYAN, 1.1f);
    GdiPlusDraw::textCenter(g, "APPLY & RECONNECT", static_cast<float>(rRec.left + rRec.right) * 0.5f, static_cast<float>(y + 36), 10.0f, true,
                            ptIn(rRec, hoverX, hoverY) ? 0xFF08121Au : Theme::TEXT_PRIMARY);
    GdiPlusDraw::roundedCard(g, rDis, 6.0f, ptIn(rDis, hoverX, hoverY) ? Theme::ACCENT_RED : Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
    GdiPlusDraw::textCenter(g, "DISCONNECT", static_cast<float>(rDis.left + rDis.right) * 0.5f, static_cast<float>(y + 36), 10.0f, true,
                            ptIn(rDis, hoverX, hoverY) ? 0xFFFFFFFFu : Theme::ACCENT_RED);
    if (rRec.bottom > bodyClip_.top && rRec.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rRec, DA_APPLY_RECONNECT, 0 });
        hotspots_.push_back(Hotspot{ rDis, DA_DISCONNECT, 0 });
    }
    return rDZ.bottom + 10;
}

int DrawerPanel::paintTabGamepad(Gdiplus::Graphics& g, int x, int y, int w,
                                 const Profile& p, const AppSettings& s, int hoverX, int hoverY) {
    GdiPlusDraw::textLeft(g, "GAMEPAD v2 (F6)", static_cast<float>(x), static_cast<float>(y + 8), 10.5f, true, Theme::TEXT_MUTED);
    y += 22;

    drawToggleRow(g, x, y, w, "Gamepad Enabled (XInput)", s.gamepadEnabled, Theme::ACCENT_GREEN, DA_TOGGLE_GAMEPAD, hoverX, hoverY);
    y += dp(38);

    // Touch Map vs Native UHID Mode
    GdiPlusDraw::textLeft(g, "CONTROLLER MODE", static_cast<float>(x), static_cast<float>(y + 7), 10.0f, true, Theme::TEXT_MUTED);
    y += 18;
    int mw = (w - 6) / 2;
    RECT rTch = { x, y, x + mw, y + 30 };
    RECT rNat = { x + mw + 6, y, x + w, y + 30 };
    bool onTch = (p.padMode != PAD_MODE_NATIVE);
    bool onNat = (p.padMode == PAD_MODE_NATIVE);
    GdiPlusDraw::roundedCard(g, rTch, 6.0f, onTch ? Theme::ACCENT_CYAN_BG : (ptIn(rTch, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_INPUT),
                             onTch ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.2f);
    GdiPlusDraw::roundedCard(g, rNat, 6.0f, onNat ? Theme::ACCENT_GREEN_BG : (ptIn(rNat, hoverX, hoverY) ? Theme::BG_CARD_HOVER : Theme::BG_INPUT),
                             onNat ? Theme::ACCENT_GREEN : Theme::BORDER_SUBTLE, 1.2f);
    GdiPlusDraw::textCenter(g, "TOUCH MAP", static_cast<float>(rTch.left + rTch.right) * 0.5f, static_cast<float>(y + 15),
                            11.0f, true, onTch ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
    GdiPlusDraw::textCenter(g, "NATIVE UHID", static_cast<float>(rNat.left + rNat.right) * 0.5f, static_cast<float>(y + 15),
                            11.0f, true, onNat ? Theme::ACCENT_GREEN : Theme::TEXT_SECONDARY);
    if (rTch.bottom > bodyClip_.top && rTch.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rTch, DA_PAD_MODE, 0 });
        hotspots_.push_back(Hotspot{ rNat, DA_PAD_MODE, 1 });
    }
    y += 34;

    GdiPlusDraw::textLeft(g, "PS DualSense / DS4: enable Steam Input or DS4Windows (XInput).",
                          static_cast<float>(x), static_cast<float>(y + 7), 9.2f, false, Theme::TEXT_MUTED, static_cast<float>(w));
    y += 19;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", p.padDeadzoneL);
    drawSliderRow(g, x, y, w, "Left Deadzone", p.padDeadzoneL, 0.05f, 0.50f, buf, Theme::ACCENT_CYAN, DSL_PAD_DZ_L, hoverX, hoverY);
    y += 29;
    std::snprintf(buf, sizeof(buf), "%.2f", p.padDeadzoneR);
    drawSliderRow(g, x, y, w, "Right Deadzone", p.padDeadzoneR, 0.05f, 0.50f, buf, Theme::ACCENT_CYAN, DSL_PAD_DZ_R, hoverX, hoverY);
    y += 29;
    std::snprintf(buf, sizeof(buf), "%.2fx", p.padLookSens);
    drawSliderRow(g, x, y, w, "R-Stick Look", p.padLookSens, 0.25f, 3.00f, buf, Theme::ACCENT_AMBER, DSL_PAD_LOOK_SENS, hoverX, hoverY);
    y += 32;

    // Interactive Controller Diagram Card (all 16 XInput controls clickable)
    const int diagH = 216;
    RECT rDiag = { x, y, x + w, y + diagH };
    GdiPlusDraw::roundedCard(g, rDiag, 9.0f, Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textLeft(g, onNat ? "CONTROLLER DIAGRAM (NATIVE HID ACTIVE)" : "CONTROLLER DIAGRAM (CLICK TO MAP)",
                          static_cast<float>(x + 10), static_cast<float>(y + 12), 9.5f, true, Theme::TEXT_MUTED, static_cast<float>(w - 20));

    auto drawPadBtn = [&](int bx, int by, int bw, int bh, int padId, const char* shortNm) {
        RECT rb = { bx, by, bx + bw, by + bh };
        bool sel = (selectedPadBtn_ == padId);
        bool hov = ptIn(rb, hoverX, hoverY);
        bool mapped = (padId >= 0 && padId < PAD_BUTTON_COUNT && !p.padMap[padId].empty());
        uint32_t bg = sel ? Theme::ACCENT_CYAN_BG : (mapped ? Theme::ACCENT_GREEN_BG : (hov ? Theme::BG_CARD_HOVER : Theme::BG_INPUT));
        uint32_t bd = sel ? Theme::ACCENT_CYAN : (mapped ? Theme::ACCENT_GREEN : Theme::BORDER_STRONG);
        GdiPlusDraw::roundedCard(g, rb, 5.0f, bg, bd, sel ? 1.5f : 1.0f);
        std::string txt = shortNm;
        if (mapped) {
            std::string t = p.padMap[padId];
            if (t.size() > 4) t = t.substr(0, 4);
            txt += ":" + t;
        }
        GdiPlusDraw::textCenter(g, txt, static_cast<float>(rb.left + rb.right) * 0.5f,
                                static_cast<float>(rb.top + rb.bottom) * 0.5f, 8.6f, true,
                                sel ? Theme::ACCENT_CYAN : (mapped ? Theme::ACCENT_GREEN : Theme::TEXT_PRIMARY));
        if (rb.bottom > bodyClip_.top && rb.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rb, DA_PAD_BIND_BTN, padId });
        }
    };

    int innerX = x + 10;
    int innerW = w - 20;
    int row1Y = y + 26;
    int bwTop = (innerW - 18) / 4;
    drawPadBtn(innerX, row1Y, bwTop, 22, PAD_LT, "LT");
    drawPadBtn(innerX + bwTop + 6, row1Y, bwTop, 22, PAD_LB, "LB");
    drawPadBtn(innerX + 2 * (bwTop + 6), row1Y, bwTop, 22, PAD_RB, "RB");
    drawPadBtn(innerX + 3 * (bwTop + 6), row1Y, bwTop, 22, PAD_RT, "RT");

    // Left D-Pad + LS, Center Back/Start, Right Face Diamond + RS
    int dpadCx = innerX + 46;
    int dpadCy = y + 92;
    drawPadBtn(dpadCx - 17, dpadCy - 30, 34, 20, PAD_DPAD_UP, "UP");
    drawPadBtn(dpadCx - 43, dpadCy - 10, 30, 20, PAD_DPAD_LEFT, "LT");
    drawPadBtn(dpadCx + 13, dpadCy - 10, 30, 20, PAD_DPAD_RIGHT, "RT");
    drawPadBtn(dpadCx - 17, dpadCy + 10, 34, 20, PAD_DPAD_DOWN, "DN");

    int midCx = innerX + innerW / 2;
    drawPadBtn(midCx - 46, y + 66, 42, 22, PAD_BACK, "BACK");
    drawPadBtn(midCx + 4,  y + 66, 42, 22, PAD_START, "STRT");
    drawPadBtn(dpadCx - 22, y + 146, 44, 24, PAD_LS, "LS");
    int faceCx = innerX + innerW - 46;
    int faceCy = y + 92;
    drawPadBtn(faceCx - 17, faceCy - 30, 34, 20, PAD_Y, "Y");
    drawPadBtn(faceCx - 43, faceCy - 10, 30, 20, PAD_X, "X");
    drawPadBtn(faceCx + 13, faceCy - 10, 30, 20, PAD_B, "B");
    drawPadBtn(faceCx - 17, faceCy + 10, 34, 20, PAD_A, "A");
    drawPadBtn(faceCx - 22, y + 146, 44, 24, PAD_RS, "RS");

    GdiPlusDraw::textCenter(g, "Tip: click a button, then click any node on phone",
                            static_cast<float>(x + w / 2), static_cast<float>(y + 194),
                            9.2f, false, Theme::TEXT_MUTED);
    y += diagH + 10;

    // Picker card when a gamepad button is selected
    if (selectedPadBtn_ >= 0 && selectedPadBtn_ < PAD_BUTTON_COUNT) {
        std::string hdr = std::string("MAP PAD '") + ProfileDefaults::padButtonDisplayName(selectedPadBtn_) + "' TO NODE:";
        GdiPlusDraw::textLeft(g, hdr, static_cast<float>(x), static_cast<float>(y + 7), 10.0f, true, Theme::ACCENT_CYAN);
        y += 20;

        int cw3 = (w - 8) / 3;
        RECT rClr = { x, y, x + cw3, y + 24 };
        RECT rFir = { x + cw3 + 4, y, x + 2 * cw3 + 4, y + 24 };
        RECT rScp = { x + 2 * cw3 + 8, y, x + w, y + 24 };
        GdiPlusDraw::roundedCard(g, rClr, 5.0f, Theme::BG_INPUT, Theme::BORDER_STRONG, 1.0f);
        GdiPlusDraw::roundedCard(g, rFir, 5.0f, Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
        GdiPlusDraw::roundedCard(g, rScp, 5.0f, Theme::ACCENT_PURPLE_BG, Theme::ACCENT_PURPLE, 1.0f);
        GdiPlusDraw::textCenter(g, "Auto", static_cast<float>(rClr.left + rClr.right) * 0.5f, static_cast<float>(y + 12), 10.0f, true, Theme::TEXT_SECONDARY);
        GdiPlusDraw::textCenter(g, "FIRE", static_cast<float>(rFir.left + rFir.right) * 0.5f, static_cast<float>(y + 12), 10.0f, true, Theme::ACCENT_RED);
        GdiPlusDraw::textCenter(g, "SCOPE", static_cast<float>(rScp.left + rScp.right) * 0.5f, static_cast<float>(y + 12), 10.0f, true, Theme::ACCENT_PURPLE);
        if (rClr.bottom > bodyClip_.top && rClr.top < bodyClip_.bottom) {
            hotspots_.push_back(Hotspot{ rClr, DA_PAD_ASSIGN_TARGET, -1 });
            hotspots_.push_back(Hotspot{ rFir, DA_PAD_ASSIGN_TARGET, -2 });
            hotspots_.push_back(Hotspot{ rScp, DA_PAD_ASSIGN_TARGET, -3 });
        }
        y += 28;

        int btnColW = (w - 8) / 3;
        int shown = static_cast<int>(p.buttons.size());
        if (shown > 12) shown = 12;
        for (int bi = 0; bi < shown; ++bi) {
            int col = bi % 3;
            int row = bi / 3;
            RECT rN = { x + col * (btnColW + 4), y + row * 26, x + col * (btnColW + 4) + btnColW, y + row * 26 + 22 };
            bool cur = (!p.padMap[selectedPadBtn_].empty() &&
                        lstrcmpiA(p.padMap[selectedPadBtn_].c_str(), p.buttons[bi].label.c_str()) == 0);
            GdiPlusDraw::roundedCard(g, rN, 5.0f, cur ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD,
                                     cur ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
            GdiPlusDraw::textCenter(g, p.buttons[bi].label.empty() ? KeyCodes::toName(p.buttons[bi].vk) : p.buttons[bi].label,
                                    static_cast<float>(rN.left + rN.right) * 0.5f,
                                    static_cast<float>(rN.top + rN.bottom) * 0.5f, 9.5f, true,
                                    cur ? Theme::ACCENT_CYAN : Theme::TEXT_PRIMARY);
            if (rN.bottom > bodyClip_.top && rN.top < bodyClip_.bottom) {
                hotspots_.push_back(Hotspot{ rN, DA_PAD_ASSIGN_TARGET, bi });
            }
        }
        if (shown > 0) y += ((shown + 2) / 3) * 26 + 4;
    }

    RECT rRstPad = { x, y, x + w, y + 28 };
    bool hovRst = ptIn(rRstPad, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rRstPad, 7.0f, hovRst ? Theme::ACCENT_RED_BG : Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "Reset Default Pad Mapping", static_cast<float>(x + w / 2), static_cast<float>(y + 14),
                            10.5f, true, Theme::TEXT_SECONDARY);
    if (rRstPad.bottom > bodyClip_.top && rRstPad.top < bodyClip_.bottom) {
        hotspots_.push_back(Hotspot{ rRstPad, DA_PAD_RESET_MAP, 0 });
    }
    return rRstPad.bottom + 12;
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
            case DSL_ACCEL_POWER: p.accelPower = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_ACCEL_REF:   p.accelRefSpeed = std::floor(v + 0.5f); break;
            case DSL_ACCEL_CAP:   p.accelCap = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case DSL_TDOWN_DELAY: p.touchDownDelayMs = static_cast<int>(std::floor(v + 0.5f)); break;
            case DSL_TUP_DELAY:   p.touchUpDelayMs = static_cast<int>(std::floor(v + 0.5f)); break;
            case DSL_SCROLL_STEP: p.cursorScrollStepPx = static_cast<int>(std::floor(v / 5.0f + 0.5f)) * 5; break;
            case DSL_WALK_RADIUS: p.joyWalkRadius = std::floor(v * 200.0f + 0.5f) / 200.0f; break;
            case DSL_FIRE_BOX_W:  p.fireBoxHalfW = std::floor(v * 50.0f + 0.5f) / 50.0f; break;
            case DSL_NODE_DUR:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
                    p.buttons[selectedBtnIndex].durationMs = static_cast<int>(std::floor(v / 5.0f + 0.5f)) * 5;
                }
                break;
            case DSL_NODE_RADIUS:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
                    p.buttons[selectedBtnIndex].radius = std::floor(v * 100.0f + 0.5f) / 100.0f;
                }
                break;
            case DSL_PAD_DZ_L:      p.padDeadzoneL = std::floor(v * 100.0f + 0.5f) / 100.0f; break;
            case DSL_PAD_DZ_R:      p.padDeadzoneR = std::floor(v * 100.0f + 0.5f) / 100.0f; break;
            case DSL_PAD_LOOK_SENS: p.padLookSens = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            default: break;
        }
        return;
    }
}

float DrawerPanel::currentSliderValue(int sliderId, const Profile& p, int selectedBtnIndex) const {
    switch (sliderId) {
        case DSL_SENS_X:      return p.sensX;
        case DSL_SENS_Y:      return p.sensY;
        case DSL_FIRE_X:      return p.fireSensX;
        case DSL_FIRE_Y:      return p.fireSensY;
        case DSL_SCOPE_X:     return p.scopeSensX;
        case DSL_SCOPE_Y:     return p.scopeSensY;
        case DSL_FF_CA:       return p.ffCounterAccel;
        case DSL_SMOOTH:      return p.smoothing;
        case DSL_HUD_OPACITY: return p.hudOpacity;
        case DSL_IDLE_RESET:  return static_cast<float>(p.idleResetMs);
        case DSL_TAP_JITTER:  return static_cast<float>(p.tapJitterPx);
        case DSL_ACCEL_POWER: return p.accelPower;
        case DSL_ACCEL_REF:   return p.accelRefSpeed;
        case DSL_ACCEL_CAP:   return p.accelCap;
        case DSL_TDOWN_DELAY: return static_cast<float>(p.touchDownDelayMs);
        case DSL_TUP_DELAY:   return static_cast<float>(p.touchUpDelayMs);
        case DSL_SCROLL_STEP: return static_cast<float>(p.cursorScrollStepPx);
        case DSL_WALK_RADIUS: return p.joyWalkRadius;
        case DSL_FIRE_BOX_W:  return p.fireBoxHalfW;
        case DSL_NODE_DUR:
            if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
                return static_cast<float>(p.buttons[selectedBtnIndex].durationMs);
            }
            return 35.0f;
        case DSL_NODE_RADIUS:
            if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
                return p.buttons[selectedBtnIndex].radius;
            }
            return 0.11f;
        case DSL_PAD_DZ_L:      return p.padDeadzoneL;
        case DSL_PAD_DZ_R:      return p.padDeadzoneR;
        case DSL_PAD_LOOK_SENS: return p.padLookSens;
        default: return 0.0f;
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
            if (activeSliderId_ == DSL_NODE_DUR || activeSliderId_ == DSL_NODE_RADIUS) out.profileModified = true;
            else                                                                       out.tuningOnlyModified = true;
            clearFocus();
            return true;
        }
    }

    for (size_t i = 0; i < hotspots_.size(); ++i) {
        const Hotspot& hs = hotspots_[i];
        if (!ptIn(hs.rc, x, y)) continue;
        clearFocus();
        return activateHotspot(hs, profile, settings, selectedBtnIndex, out);
    }
    return true;
}

bool DrawerPanel::activateHotspot(const Hotspot& hs, Profile& profile, AppSettings& settings,
                                  int& selectedBtnIndex, DrawerActionResult& out) {
    if (hs.id != DA_INSP_REBIND && hs.id != DA_ADD_NODE) {
        out.cancelRebind = true;
    }

    switch (hs.id) {
            case DA_TAB_0: case DA_TAB_1: case DA_TAB_2: case DA_TAB_3: case DA_TAB_4:
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
                out.requestClearButtonsUndo = true;
                return true;
            case DA_FOCUS_PALETTE_SEARCH:
                editingPaletteSearch_ = true;
                return true;
            case DA_TOGGLE_EDIT_TEST:
                out.requestToggleEditTest = true;
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
                    b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 10) % 11);
                    if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) { b.x2 = clampf(b.x + 0.12f, 0.02f, 0.98f); b.durationMs = 120; }
                    if (b.action == NODE_SKILL_CAST) {
                        if (b.x2 == b.x && b.y2 == b.y) { b.x2 = 0.880f; b.y2 = 0.180f; }
                        if (b.radius < 0.04f) b.radius = 0.110f;
                    }
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_ACT_NEXT:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    KeyNode& b = profile.buttons[selectedBtnIndex];
                    b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 1) % 11);
                    if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) { b.x2 = clampf(b.x + 0.12f, 0.02f, 0.98f); b.durationMs = 120; }
                    if (b.action == NODE_SKILL_CAST) {
                        if (b.x2 == b.x && b.y2 == b.y) { b.x2 = 0.880f; b.y2 = 0.180f; }
                        if (b.radius < 0.04f) b.radius = 0.110f;
                    }
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_QUICK_CAST:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].quickCast = !profile.buttons[selectedBtnIndex].quickCast;
                    out.profileModified = true;
                    out.toastMessage = profile.buttons[selectedBtnIndex].quickCast ? "Quick Cast: ON" : "Quick Cast: OFF (Aim while held)";
                }
                return true;
            case DA_REBIND_CAST_CANCEL:
                specialRebind_ = SRB_CAST_CANCEL;
                out.toastMessage = "Press key to cancel held skill casts (e.g. Space)...";
                return true;
            case DA_CLEAR_CAST_CANCEL:
                profile.castCancelVk = 0;
                out.profileModified = true;
                out.toastMessage = "Skill Cast Cancel key cleared";
                return true;
                return true;
            case DA_INSP_SCOPE:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].switchScopeSens = !profile.buttons[selectedBtnIndex].switchScopeSens;
                    out.profileModified = true;
                }
                return true;
            case DA_INSP_TOGGLE_MODE:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].toggleMode = !profile.buttons[selectedBtnIndex].toggleMode;
                    out.profileModified = true;
                    out.toastMessage = profile.buttons[selectedBtnIndex].toggleMode ? "Switch/Action Mode: TOGGLE" : "Switch/Action Mode: HOLD";
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

            case DA_FIRE_CLAW: profile.fireMode = FIRE_MODE_CLAW_LEFT; out.tuningOnlyModified = true; out.toastMessage = "Fire Mode: CLAW-LEFT"; return true;
            case DA_FIRE_DRAG: profile.fireMode = FIRE_MODE_DRAG_RIGHT; out.tuningOnlyModified = true; out.toastMessage = "Fire Mode: DRAG-FIRE"; return true;
            case DA_FIRE_DUAL: profile.fireMode = FIRE_MODE_DUAL_DRAG; out.tuningOnlyModified = true; out.toastMessage = "Fire Mode: DUAL-DRAG"; return true;
            case DA_BOUNDS_UNBOUNDED: profile.unboundedAim = true; out.tuningOnlyModified = true; out.toastMessage = "Aim Bounds: UNBOUNDED (Free Fire)"; return true;
            case DA_BOUNDS_BOXED:     profile.unboundedAim = false; out.tuningOnlyModified = true; out.toastMessage = "Aim Bounds: BOXED (15ms Edge Reset)"; return true;
            case DA_POLL_125: profile.pollHz = 125; out.tuningOnlyModified = true; out.toastMessage = "Polling: 125 Hz"; return true;
            case DA_POLL_250: profile.pollHz = 250; out.tuningOnlyModified = true; out.toastMessage = "Polling: 250 Hz"; return true;
            case DA_POLL_500: profile.pollHz = 500; out.tuningOnlyModified = true; out.toastMessage = "Polling: 500 Hz"; return true;
            case DA_POLL_MAX: profile.pollHz = 0;   out.tuningOnlyModified = true; out.toastMessage = "Polling: MAX (1000 Hz)"; return true;
            case DA_JOY_GLIDE: profile.joySteppedGlide = true; out.tuningOnlyModified = true; out.toastMessage = "Joystick: FF 4-Step Glide"; return true;
            case DA_JOY_FAST:  profile.joySteppedGlide = false; out.tuningOnlyModified = true; out.toastMessage = "Joystick: Fast 3-Step (24ms)"; return true;
            case DA_TOGGLE_INVERT_Y: profile.invertY = !profile.invertY; out.tuningOnlyModified = true; return true;
            case DA_TOGGLE_REMEMBER_CURSOR: profile.rememberCursorPos = !profile.rememberCursorPos; out.tuningOnlyModified = true; return true;
            case DA_REBIND_TOGGLE_AIM: specialRebind_ = SRB_TOGGLE_AIM; out.toastMessage = "Press ANY key or Mouse button (MMB, LShift, ~) for Aim Lock..."; return true;
            case DA_SET_TOGGLE_MMB: profile.toggleAimVk = VK_MBUTTON; out.profileModified = true; out.toastMessage = "Aim Lock Toggle set to MMB"; return true;
            case DA_REBIND_HOLD_UNLOCK: specialRebind_ = SRB_HOLD_UNLOCK; out.toastMessage = "Press ANY key or Mouse button for Hold-to-Unlock..."; return true;
            case DA_CLEAR_HOLD_UNLOCK: profile.holdUnlockVk = 0; out.profileModified = true; out.toastMessage = "Hold-to-Unlock cleared"; return true;
            case DA_TOGGLE_ADVANCED:
                advancedExpanded_ = !advancedExpanded_;
                return true;
            case DA_REBIND_RESET_TOUCH: specialRebind_ = SRB_RESET_TOUCH; out.toastMessage = "Press key for Quick Touch Reset..."; return true;
            case DA_REBIND_JOY_UP:      specialRebind_ = SRB_JOY_UP;      out.toastMessage = "Press key for Joystick Up..."; return true;
            case DA_REBIND_JOY_DOWN:    specialRebind_ = SRB_JOY_DOWN;    out.toastMessage = "Press key for Joystick Down..."; return true;
            case DA_REBIND_JOY_LEFT:    specialRebind_ = SRB_JOY_LEFT;    out.toastMessage = "Press key for Joystick Left..."; return true;
            case DA_REBIND_JOY_RIGHT:   specialRebind_ = SRB_JOY_RIGHT;   out.toastMessage = "Press key for Joystick Right..."; return true;
            case DA_REBIND_JOY_SPRINT:  specialRebind_ = SRB_JOY_SPRINT;  out.toastMessage = "Press key for Joystick Sprint..."; return true;
            case DA_REBIND_JOY_WALK:    specialRebind_ = SRB_JOY_WALK;    out.toastMessage = "Press key for Joystick Walk..."; return true;
            case DA_CLEAR_JOY_WALK:
                profile.vkWalk = 0;
                out.profileModified = true;
                out.toastMessage = "Joystick Walk key cleared";
                return true;
            case DA_RESET_SENS: {
                Profile def = ProfileDefaults::createFreeFire();
                profile.sensX = def.sensX; profile.sensY = def.sensY;
                profile.fireSensX = def.fireSensX; profile.fireSensY = def.fireSensY;
                profile.scopeSensX = def.scopeSensX; profile.scopeSensY = def.scopeSensY;
                profile.ffCounterAccel = def.ffCounterAccel;
                profile.smoothing = def.smoothing;
                profile.hudOpacity = def.hudOpacity;
                out.tuningOnlyModified = true;
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
                        out.requestDeleteProfileTrash = true;
                        out.deleteProfilePath = profileEntries_[hs.extra].path;
                        out.deleteProfileName = profileEntries_[hs.extra].fileName;
                    }
                }
                return true;
            case DA_CREATE_TEMPLATE:
                out.requestCreateTemplate = true;
                out.templateKind = hs.extra;
                return true;
            case DA_IMPORT_PROFILE:
                out.requestImportProfile = true;
                return true;
            case DA_EXPORT_PROFILE:
                out.requestExportProfile = true;
                return true;
            case DA_COPY_PROFILE_TEXT:
                out.requestCopyProfileText = true;
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

            case DA_STREAM_PRESET: {
                StreamPresetId pid = static_cast<StreamPresetId>(hs.extra);
                settings.applyStreamPreset(pid);
                out.settingsModified = true;
                out.vsyncToggled = true;
                out.toastMessage = std::string("Preset: ") + AppSettings::streamPresetName(pid) + " (click Apply & Reconnect)";
                return true;
            }
            case DA_STREAM_FPS:     settings.mirrorFps = hs.extra; out.settingsModified = true; return true;
            case DA_STREAM_BITRATE: settings.mirrorBitrateMbps = hs.extra; out.settingsModified = true; return true;
            case DA_STREAM_MAXSIZE: settings.mirrorMaxSize = hs.extra; out.settingsModified = true; return true;
            case DA_CODEC_OPTS:
                settings.videoCodecOptions = (hs.extra == 1) ? "bitrate-mode=2,max-bframes=0"
                                           : (hs.extra == 2) ? "intra-refresh-period=60,bitrate-mode=2,max-bframes=0"
                                                             : "";
                out.settingsModified = true;
                out.toastMessage = "Encoder options updated (click Apply & Reconnect)";
                return true;
            case DA_CYCLE_ENCODER: {
                if (!activeDeviceSerial_.empty()) {
                    std::vector<std::string> opts;
                    opts.push_back(""); // Auto (HW)
                    for (size_t ei = 0; ei < deviceEncoders_.size(); ++ei) {
                        if (deviceEncoders_[ei].isHardware) opts.push_back(deviceEncoders_[ei].name);
                    }
                    std::string cur = settings.encoderForDevice(activeDeviceSerial_);
                    size_t nextIdx = 0;
                    for (size_t oi = 0; oi < opts.size(); ++oi) {
                        if (opts[oi] == cur) { nextIdx = (oi + 1) % opts.size(); break; }
                    }
                    settings.setEncoderForDevice(activeDeviceSerial_, opts[nextIdx]);
                    out.settingsModified = true;
                    out.toastMessage = "Encoder: " + (opts[nextIdx].empty() ? std::string("Auto (HW)") : opts[nextIdx]) + " (Apply & Reconnect)";
                }
                return true;
            }
            case DA_TOGGLE_NATIVE_VIDEO:
                settings.useNativeVideo = !settings.useNativeVideo;
                if (!settings.useNativeVideo) settings.virtualDisplayMode = false;
                out.settingsModified = true;
                out.toastMessage = settings.useNativeVideo
                    ? "Native D3D11 Single-Stream ON (click Apply & Reconnect)"
                    : "Legacy scrcpy.exe Fallback ON (click Apply & Reconnect)";
                return true;
            case DA_TOGGLE_VSYNC:
                settings.vsyncEnabled = !settings.vsyncEnabled;
                out.settingsModified = true;
                out.vsyncToggled = true;
                out.toastMessage = settings.vsyncEnabled
                    ? "VSync ON (Waitable 1-frame, No Tearing)"
                    : "VSync OFF (Flip + Immediate Tearing)";
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
            case DA_AUDIO_BUFFER:      settings.audioBufferMs = hs.extra; out.settingsModified = true; return true;
            case DA_TOGGLE_AUDIO_RAW:  settings.audioRawCodec = !settings.audioRawCodec; out.settingsModified = true; return true;
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
            case DA_TOGGLE_DEV_ADVANCED:
                devAdvancedExpanded_ = !devAdvancedExpanded_;
                return true;
            case DA_APPLY_RECONNECT:   out.requestReconnectStream = true; return true;
            case DA_DISCONNECT:        out.requestDisconnectDevice = true; return true;
            case DA_RUN_LATENCY_PROBE: out.requestRunLatencyProbe = true; return true;
            case DA_FOCUS_WIRELESS:    editingWireless_ = true; return true;
            case DA_CONNECT_WIRELESS:
                out.requestConnectWireless = true;
                out.wirelessAddr = settings.lastWirelessAddr;
                return true;
            case DA_OPEN_ONBOARDING:
                out.requestOpenOnboarding = true;
                return true;
            case DA_PAD_MODE:
                profile.padMode = (hs.extra == 1) ? PAD_MODE_NATIVE : PAD_MODE_TOUCH;
                out.profileModified = true;
                out.toastMessage = (profile.padMode == PAD_MODE_NATIVE)
                    ? "Controller: NATIVE UHID passthrough"
                    : "Controller: Per-Profile TOUCH MAP";
                return true;
            case DA_PAD_BIND_BTN:
                selectedPadBtn_ = (selectedPadBtn_ == hs.extra) ? -1 : hs.extra;
                if (selectedPadBtn_ >= 0 && selectedPadBtn_ < PAD_BUTTON_COUNT) {
                    out.toastMessage = std::string("Click a node on phone or pick target for Pad ") +
                                       ProfileDefaults::padButtonDisplayName(selectedPadBtn_);
                }
                return true;
            case DA_PAD_ASSIGN_TARGET:
                if (selectedPadBtn_ >= 0 && selectedPadBtn_ < PAD_BUTTON_COUNT) {
                    int bid = selectedPadBtn_;
                    if (hs.extra == -1) {
                        profile.padMap[bid].clear();
                        out.toastMessage = std::string("Pad ") + ProfileDefaults::padButtonDisplayName(bid) + " -> Auto";
                    } else if (hs.extra == -2) {
                        profile.padMap[bid] = "FIRE";
                        out.toastMessage = std::string("Pad ") + ProfileDefaults::padButtonDisplayName(bid) + " -> FIRE";
                    } else if (hs.extra == -3) {
                        profile.padMap[bid] = "SCOPE";
                        out.toastMessage = std::string("Pad ") + ProfileDefaults::padButtonDisplayName(bid) + " -> SCOPE";
                    } else if (hs.extra >= 0 && hs.extra < static_cast<int>(profile.buttons.size())) {
                        std::string tgt = profile.buttons[hs.extra].label;
                        if (tgt.empty()) tgt = KeyCodes::toName(profile.buttons[hs.extra].vk);
                        profile.padMap[bid] = tgt;
                        out.toastMessage = std::string("Pad ") + ProfileDefaults::padButtonDisplayName(bid) + " -> " + tgt;
                    }
                    selectedPadBtn_ = -1;
                    out.profileModified = true;
                }
                return true;
            case DA_PAD_RESET_MAP:
                for (int pi = 0; pi < PAD_BUTTON_COUNT; ++pi) profile.padMap[pi].clear();
                profile.padDeadzoneL = 0.24f;
                profile.padDeadzoneR = 0.27f;
                profile.padLookSens = 1.00f;
                selectedPadBtn_ = -1;
                out.profileModified = true;
                out.toastMessage = "Reset controller mapping to defaults";
                return true;
            default:
                break;
    }
    return true;
}

bool DrawerPanel::onMouseMove(int x, int y, Profile& profile, int selectedBtnIndex, DrawerActionResult& out) {
    (void)y;
    if (activeSliderId_ != 0) {
        float w = static_cast<float>(activeSliderTrack_.right - activeSliderTrack_.left);
        float t = (w > 1.0f) ? (static_cast<float>(x - activeSliderTrack_.left) / w) : 0.0f;
        applySliderValue(activeSliderId_, t, profile, selectedBtnIndex);
        if (activeSliderId_ == DSL_NODE_DUR || activeSliderId_ == DSL_NODE_RADIUS) out.profileModified = true;
        else                                                                       out.tuningOnlyModified = true;
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
    if (specialRebind_ != SRB_NONE) {
        if (vk != VK_ESCAPE) {
            vk = KeyCodes::normalizeModifier(vk);
            std::string lbl = specialRebindLabel();
            switch (specialRebind_) {
                case SRB_TOGGLE_AIM:  profile.toggleAimVk = vk; break;
                case SRB_HOLD_UNLOCK: profile.holdUnlockVk = vk; break;
                case SRB_RESET_TOUCH: profile.resetTouchVk = vk; break;
                case SRB_JOY_UP:      profile.vkUp = vk; break;
                case SRB_JOY_DOWN:    profile.vkDown = vk; break;
                case SRB_JOY_LEFT:    profile.vkLeft = vk; break;
                case SRB_JOY_RIGHT:   profile.vkRight = vk; break;
                case SRB_JOY_SPRINT:  profile.vkSprint = vk; break;
                case SRB_JOY_WALK:    profile.vkWalk = vk; break;
                case SRB_CAST_CANCEL: profile.castCancelVk = vk; break;
                default: break;
            }
            out.profileModified = true;
            out.toastMessage = lbl + " bound to " + KeyCodes::toName(vk);
        }
        specialRebind_ = SRB_NONE;
        return true;
    }
    if (editingPaletteSearch_) {
        if (vk == VK_RETURN || vk == VK_ESCAPE) { editingPaletteSearch_ = false; return true; }
        if (vk == VK_BACK && !paletteSearch_.empty()) paletteSearch_.pop_back();
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
    // U7: Keyboard accessibility (Tab / Shift+Tab focus cycle, Left/Right adjust, Space/Enter activate)
    if (vk == VK_TAB && !focusables_.empty()) {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int n = static_cast<int>(focusables_.size());
        if (focusedIdx_ < 0 || focusedIdx_ >= n) {
            focusedIdx_ = shift ? (n - 1) : 0;
        } else {
            focusedIdx_ = (focusedIdx_ + (shift ? (n - 1) : 1)) % n;
        }
        return true;
    }
    if (focusedIdx_ >= 0 && focusedIdx_ < static_cast<int>(focusables_.size())) {
        const FocusableItem& fi = focusables_[focusedIdx_];
        if (vk == VK_ESCAPE) {
            focusedIdx_ = -1;
            return true;
        }
        if ((vk == VK_SPACE || vk == VK_RETURN) && !fi.isSlider) {
            Hotspot hs = { fi.rc, fi.id, fi.extra };
            int sel = selectedBtnIndex;
            return activateHotspot(hs, profile, settings, sel, out);
        }
        if (vk == VK_LEFT || vk == VK_RIGHT) {
            int dir = (vk == VK_RIGHT) ? 1 : -1;
            if (fi.isSlider) {
                float span = fi.maxVal - fi.minVal;
                float cur = currentSliderValue(fi.id, profile, selectedBtnIndex);
                float norm = (span > 0.0f) ? clampf((cur - fi.minVal) / span, 0.0f, 1.0f) : 0.0f;
                applySliderValue(fi.id, clampf(norm + dir * 0.05f, 0.0f, 1.0f), profile, selectedBtnIndex);
                if (fi.id == DSL_NODE_DUR || fi.id == DSL_NODE_RADIUS) out.profileModified = true;
                else                                                   out.tuningOnlyModified = true;
                return true;
            }
            auto segGroup = [](int id) -> int {
                if (id >= DA_TAB_0 && id <= DA_TAB_4) return 1;
                if (id == DA_FIRE_CLAW || id == DA_FIRE_DRAG || id == DA_FIRE_DUAL) return 2;
                if (id == DA_BOUNDS_UNBOUNDED || id == DA_BOUNDS_BOXED) return 3;
                if (id >= DA_POLL_125 && id <= DA_POLL_MAX) return 4;
                if (id == DA_JOY_GLIDE || id == DA_JOY_FAST) return 5;
                if (id == DA_PAD_MODE) return 6;
                if (id == DA_STREAM_PRESET || id == DA_STREAM_FPS || id == DA_STREAM_BITRATE ||
                    id == DA_STREAM_MAXSIZE || id == DA_CODEC_OPTS || id == DA_VD_PRESET ||
                    id == DA_AUDIO_BUFFER || id == DA_INSP_MOUSE_VK) {
                    return 100 + id;
                }
                return 0;
            };
            int grp = segGroup(fi.id);
            if (grp != 0) {
                int nxt = focusedIdx_ + dir;
                if (nxt >= 0 && nxt < static_cast<int>(focusables_.size()) &&
                    !focusables_[nxt].isSlider && segGroup(focusables_[nxt].id) == grp) {
                    focusedIdx_ = nxt;
                    Hotspot hs = { focusables_[nxt].rc, focusables_[nxt].id, focusables_[nxt].extra };
                    int sel = selectedBtnIndex;
                    return activateHotspot(hs, profile, settings, sel, out);
                }
            }
        }
    }
    return false;
}

bool DrawerPanel::onMouseBind(int mouseVk, Profile& profile, DrawerActionResult& out) {
    if (specialRebind_ == SRB_NONE) return false;
    if (mouseVk == VK_LBUTTON) {
        out.toastMessage = "Use Keyboard, MMB, RMB, X1, X2, or Wheel for " + specialRebindLabel();
        return true;
    }
    if (specialRebind_ != SRB_TOGGLE_AIM && specialRebind_ != SRB_HOLD_UNLOCK) {
        out.toastMessage = "Please press a keyboard key for " + specialRebindLabel();
        return true;
    }
    if (specialRebind_ == SRB_TOGGLE_AIM) {
        profile.toggleAimVk = mouseVk;
        out.toastMessage = "Aim Lock Toggle bound to " + KeyCodes::toName(mouseVk);
    } else {
        profile.holdUnlockVk = mouseVk;
        out.toastMessage = "Hold-to-Unlock bound to " + KeyCodes::toName(mouseVk);
    }
    specialRebind_ = SRB_NONE;
    out.profileModified = true;
    return true;
}

bool DrawerPanel::onChar(char ch, Profile& profile, AppSettings& settings,
                         int selectedBtnIndex, DrawerActionResult& out) {
    if (ch < 32 || ch > 126) return false;
    if (editingPaletteSearch_) {
        if (paletteSearch_.size() < 24) paletteSearch_.push_back(ch);
        return true;
    }
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
