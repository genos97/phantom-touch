#include "ui/StudioPanel.hpp"
#include "config/KeyCodes.hpp"
#include "config/ProfileStore.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

enum PanelActionId {
    PA_TOGGLE_COLLAPSE = 100,
    PA_CLOSE_EDIT,
    PA_TAB_SENS,
    PA_TAB_ADD,
    PA_TAB_PROFILES,
    PA_TAB_ADV,

    // Bottom footer actions
    PA_SAVE,
    PA_RESET_SENS,

    // Fire Mode segmented buttons
    PA_FIRE_CLAW,
    PA_FIRE_DRAG,
    PA_FIRE_DUAL,

    // Add Keys palette
    PA_ADD_NODE, // extra = KeyActionType
    PA_CLEAR_BUTTONS,

    // Profiles tab
    PA_FOCUS_NEW_NAME,
    PA_CREATE_BLANK,
    PA_CREATE_CLONE,
    PA_LOAD_PROFILE,   // extra = index in profileEntries_
    PA_DELETE_PROFILE, // extra = index in profileEntries_
    PA_RESET_FF_DEFAULTS,

    // Advanced tab
    PA_BOUNDS_UNBOUNDED,
    PA_BOUNDS_BOXED,
    PA_POLL_125,
    PA_POLL_250,
    PA_POLL_500,
    PA_POLL_MAX,
    PA_JOY_GLIDE,
    PA_JOY_INSTANT,
    PA_TOGGLE_INVERT_Y,
    PA_REBIND_TOGGLE_AIM,
    PA_SET_TOGGLE_MMB,
    PA_REBIND_HOLD_UNLOCK,
    PA_CLEAR_HOLD_UNLOCK,

    // Selected Node Inspector
    PA_INSP_FOCUS_LABEL,
    PA_INSP_REBIND_KEY,
    PA_INSP_SET_MOUSE_VK, // extra = VK_* mouse button / wheel
    PA_INSP_ACT_PREV,
    PA_INSP_ACT_NEXT,
    PA_INSP_TOGGLE_SCOPE,
    PA_INSP_DELETE
};

enum SliderId {
    SL_SENS_X = 1,
    SL_SENS_Y,
    SL_FIRE_X,
    SL_FIRE_Y,
    SL_SCOPE_X,
    SL_SCOPE_Y,
    SL_FF_CA,
    SL_SMOOTH,
    SL_HUD_OPACITY,
    SL_IDLE_RESET,
    SL_TAP_JITTER,
    SL_NODE_DUR
};


float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::string getFileBaseName(const std::string& fullPath) {
    size_t pos = fullPath.find_last_of("\\/");
    return (pos == std::string::npos) ? fullPath : fullPath.substr(pos + 1);
}


} // namespace

StudioPanel::StudioPanel() {
    ZeroMemory(&panelRect_, sizeof(panelRect_));
    ZeroMemory(&titleBarRect_, sizeof(titleBarRect_));
    ZeroMemory(&inspectorRect_, sizeof(inspectorRect_));
    ZeroMemory(&activeSliderTrack_, sizeof(activeSliderTrack_));
}

void StudioPanel::setProfilesDir(const std::string& dir, const std::string& activePath) {
    profilesDir_ = dir;
    activeProfilePath_ = activePath;
    refreshProfileList();
}

void StudioPanel::refreshProfileList() {
    profileEntries_.clear();
    if (profilesDir_.empty()) return;
    std::vector<std::string> files = ProfileStore::listProfiles(profilesDir_);
    for (size_t i = 0; i < files.size(); ++i) {
        ProfileEntry e;
        e.path = files[i];
        e.fileName = getFileBaseName(files[i]);
        Profile tmp;
        if (ProfileStore::load(files[i], tmp)) {
            e.displayName = tmp.name;
        } else {
            e.displayName = e.fileName;
        }
        profileEntries_.push_back(e);
    }
}

void StudioPanel::drawSlider(HDC hdc, int x, int y, int w,
                               const char* label, float val, float minV, float maxV,
                               const char* valText, COLORREF accent, int sliderId,
                               HFONT fontSmall, HFONT fontBold, int labelW) {
    const int valW   = 46;
    const int trackX = x + labelW;
    const int trackW = w - labelW - valW - 8;
    const int cy = y + 12;

    SelectObject(hdc, fontSmall);
    GdiDraw::leftText(hdc, label, x, cy, RGB(205, 218, 235));

    RECT tr = { trackX, cy - 3, trackX + trackW, cy + 4 };
    GdiDraw::roundedBox(hdc, tr, 4, RGB(34, 42, 58), RGB(58, 70, 92), 1);

    float norm = (maxV > minV) ? clampf((val - minV) / (maxV - minV), 0.0f, 1.0f) : 0.0f;
    int knobX = trackX + static_cast<int>(norm * static_cast<float>(trackW) + 0.5f);

    if (knobX > trackX + 2) {
        RECT fillR = { trackX, cy - 3, knobX, cy + 4 };
        GdiDraw::roundedBox(hdc, fillR, 4, accent, accent, 1);
    }

    // Crisp circular thumb knob
    HPEN kPen = CreatePen(PS_SOLID, 2, RGB(245, 250, 255));
    HBRUSH kBr = CreateSolidBrush(accent);
    HGDIOBJ op = SelectObject(hdc, kPen);
    HGDIOBJ ob = SelectObject(hdc, kBr);
    Ellipse(hdc, knobX - 7, cy - 7, knobX + 7, cy + 7);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(kPen);
    DeleteObject(kBr);

    SelectObject(hdc, fontBold);
    GdiDraw::centeredText(hdc, valText, x + w - valW / 2, cy, accent);

    SliderHit sh;
    sh.trackRc = tr;
    sh.hitRc = { trackX - 8, y, trackX + trackW + 8, y + 24 };
    sh.id = sliderId;
    sh.minVal = minV;
    sh.maxVal = maxV;
    sliders_.push_back(sh);
}

void StudioPanel::paint(HDC hdc, int canvasW, int canvasH,
                          const Profile& profile, const std::string& activeProfilePath,
                          int selectedBtnIndex, bool waitingForNodeRebind,
                          HFONT fontSmall, HFONT fontBold, HFONT fontTitle) {
    hotspots_.clear();
    sliders_.clear();
    inspectorVisible_ = false;
    activeProfilePath_ = activeProfilePath;

    const int panelW = 430;
    const int panelH = collapsed_ ? 36 : 426;

    int px = static_cast<int>(profile.panelX * static_cast<float>(canvasW));
    int py = static_cast<int>(profile.panelY * static_cast<float>(canvasH));
    if (px < 8) px = 8;
    if (py < 8) py = 8;
    if (px + panelW > canvasW - 8) px = canvasW - panelW - 8;
    if (py + panelH > canvasH - 8) py = canvasH - panelH - 8;
    if (px < 4) px = 4;
    if (py < 4) py = 4;

    panelRect_ = { px, py, px + panelW, py + panelH };
    titleBarRect_ = { px, py, px + panelW - 60, py + 34 };

    // Main panel background
    GdiDraw::roundedBox(hdc, panelRect_, 12, RGB(17, 22, 33), RGB(0, 195, 255), 2);

    // Title bar strip
    RECT hdrBg = { px + 1, py + 1, px + panelW - 1, py + 34 };
    GdiDraw::roundedBox(hdc, hdrBg, 10, RGB(24, 32, 48), RGB(38, 52, 74), 1);

    SelectObject(hdc, fontTitle);
    std::string title = "KEYMAP STUDIO  |  " + profile.name;
    if (title.size() > 38) title = title.substr(0, 37) + "...";
    GdiDraw::leftText(hdc, title, px + 14, py + 18, RGB(0, 225, 255));

    // Collapse & Close buttons in top-right of title bar
    RECT rMin = { px + panelW - 56, py + 6, px + panelW - 32, py + 28 };
    GdiDraw::roundedBox(hdc, rMin, 5, RGB(38, 48, 68), RGB(95, 130, 175), 1);
    SelectObject(hdc, fontBold);
    GdiDraw::centeredText(hdc, collapsed_ ? "+" : "-", (rMin.left + rMin.right) / 2, (rMin.top + rMin.bottom) / 2, RGB(230, 240, 255));
    hotspots_.push_back(Hotspot{ rMin, PA_TOGGLE_COLLAPSE, 0 });

    RECT rCls = { px + panelW - 28, py + 6, px + panelW - 6, py + 28 };
    GdiDraw::roundedBox(hdc, rCls, 5, RGB(95, 32, 38), RGB(255, 95, 95), 1);
    GdiDraw::centeredText(hdc, "X", (rCls.left + rCls.right) / 2, (rCls.top + rCls.bottom) / 2, RGB(255, 230, 230));
    hotspots_.push_back(Hotspot{ rCls, PA_CLOSE_EDIT, 0 });

    if (!collapsed_) {
        // 4 Clean Navigation Tabs
        const char* tabNames[4] = { "SENSITIVITY", "ADD KEYS", "PROFILES", "ADVANCED" };
        const int tabActs[4]    = { PA_TAB_SENS, PA_TAB_ADD, PA_TAB_PROFILES, PA_TAB_ADV };
        int tabW = (panelW - 24) / 4;
        int tabY = py + 42;
        for (int i = 0; i < 4; ++i) {
            RECT tr = { px + 12 + i * tabW, tabY, px + 12 + (i + 1) * tabW - 4, tabY + 26 };
            bool active = (activeTab_ == i);
            GdiDraw::roundedBox(hdc, tr, 6,
                    active ? RGB(0, 120, 165) : RGB(28, 36, 52),
                    active ? RGB(0, 230, 255) : RGB(56, 70, 96),
                    active ? 2 : 1);
            SelectObject(hdc, fontBold);
            GdiDraw::centeredText(hdc, tabNames[i], (tr.left + tr.right) / 2, (tr.top + tr.bottom) / 2,
                           active ? RGB(255, 255, 255) : RGB(170, 190, 215));
            hotspots_.push_back(Hotspot{ tr, tabActs[i], 0 });
        }

        // Tab Body
        int bodyX = px + 14;
        int bodyY = py + 76;
        int bodyW = panelW - 28;

        if (activeTab_ == 0) {
            paintTabSensitivity(hdc, bodyX, bodyY, bodyW, profile, fontSmall, fontBold);
        } else if (activeTab_ == 1) {
            paintTabAddKeys(hdc, bodyX, bodyY, bodyW, fontSmall, fontBold);
        } else if (activeTab_ == 2) {
            paintTabProfiles(hdc, bodyX, bodyY, bodyW, activeProfilePath, fontSmall, fontBold);
        } else {
            paintTabAdvanced(hdc, bodyX, bodyY, bodyW, profile, fontSmall, fontBold);
        }

        // Persistent Footer Bar (Save / Done)
        int footY = py + panelH - 38;
        RECT rSave = { px + 14, footY, px + 170, footY + 28 };
        GdiDraw::roundedBox(hdc, rSave, 6, RGB(22, 95, 56), RGB(60, 235, 135), 1);
        SelectObject(hdc, fontBold);
        GdiDraw::centeredText(hdc, "SAVE CONFIG (Ctrl+S)", (rSave.left + rSave.right) / 2, (rSave.top + rSave.bottom) / 2, RGB(240, 255, 245));
        hotspots_.push_back(Hotspot{ rSave, PA_SAVE, 0 });

        RECT rRst = { px + 178, footY, px + 312, footY + 28 };
        GdiDraw::roundedBox(hdc, rRst, 6, RGB(50, 44, 30), RGB(220, 175, 65), 1);
        GdiDraw::centeredText(hdc, "RESET DEFAULTS", (rRst.left + rRst.right) / 2, (rRst.top + rRst.bottom) / 2, RGB(255, 230, 150));
        hotspots_.push_back(Hotspot{ rRst, PA_RESET_SENS, 0 });

        RECT rDone = { px + 320, footY, px + panelW - 14, footY + 28 };
        GdiDraw::roundedBox(hdc, rDone, 6, RGB(34, 62, 112), RGB(95, 185, 255), 1);
        GdiDraw::centeredText(hdc, "DONE (F2)", (rDone.left + rDone.right) / 2, (rDone.top + rDone.bottom) / 2, RGB(240, 248, 255));
        hotspots_.push_back(Hotspot{ rDone, PA_CLOSE_EDIT, 0 });
    }

    // Selected Node Inspector Card (pops cleanly beside the panel when any key node is clicked!)
    if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
        const int inspW = 264;
        const int inspH = 276;
        int ix = px + panelW + 10;
        if (ix + inspW > canvasW - 8) ix = px - inspW - 10;
        if (ix < 8) ix = 8;
        int iy = py;
        if (iy + inspH > canvasH - 8) iy = canvasH - inspH - 8;
        if (iy < 8) iy = 8;
        inspectorRect_ = { ix, iy, ix + inspW, iy + inspH };
        inspectorVisible_ = true;
        paintNodeInspector(hdc, ix, iy, inspW, profile.buttons[selectedBtnIndex],
                           waitingForNodeRebind, fontSmall, fontBold);
    }
}

void StudioPanel::paintTabSensitivity(HDC hdc, int x, int y, int w,
                                        const Profile& p, HFONT fS, HFONT fB) {
    char buf[24];
    int cy = y;
    const int step = 28;

    std::snprintf(buf, sizeof(buf), "%.2f", p.sensX);
    drawSlider(hdc, x, cy, w, "X Sensitivity", p.sensX, 0.10f, 4.00f, buf, RGB(70, 215, 255), SL_SENS_X, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%.2f", p.sensY);
    drawSlider(hdc, x, cy, w, "Y Sensitivity", p.sensY, 0.10f, 4.00f, buf, RGB(255, 200, 65), SL_SENS_Y, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%.2f", p.fireSensX);
    drawSlider(hdc, x, cy, w, "Fire X Mult", p.fireSensX, 0.20f, 3.00f, buf, RGB(255, 135, 90), SL_FIRE_X, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%.2f", p.fireSensY);
    drawSlider(hdc, x, cy, w, "Fire Y Mult", p.fireSensY, 0.20f, 3.00f, buf, RGB(255, 90, 90), SL_FIRE_Y, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%.2f", p.scopeSensX);
    drawSlider(hdc, x, cy, w, "Scope X Mult", p.scopeSensX, 0.20f, 2.50f, buf, RGB(175, 135, 255), SL_SCOPE_X, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%.2f", p.scopeSensY);
    drawSlider(hdc, x, cy, w, "Scope Y Mult", p.scopeSensY, 0.20f, 2.50f, buf, RGB(215, 150, 255), SL_SCOPE_Y, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%.1f", p.ffCounterAccel);
    drawSlider(hdc, x, cy, w, "FF Counter-Accel", p.ffCounterAccel, 0.0f, 15.0f, buf, RGB(0, 235, 195), SL_FF_CA, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%.2f", p.smoothing);
    drawSlider(hdc, x, cy, w, "Smoothing", p.smoothing, 0.0f, 0.60f, buf, RGB(120, 235, 155), SL_SMOOTH, fS, fB);
    cy += step;

    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.hudOpacity * 100.0f + 0.5f));
    drawSlider(hdc, x, cy, w, "HUD Opacity", p.hudOpacity, 0.15f, 1.00f, buf, RGB(165, 200, 240), SL_HUD_OPACITY, fS, fB);
    cy += step + 4;

    // Fire Mode 3-way selector
    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Fire Mode:", x, cy + 13, RGB(200, 215, 235));
    const char* fLbls[3] = { "CLAW-LEFT", "DRAG-FIRE", "DUAL-DRAG" };
    const int fActs[3]   = { PA_FIRE_CLAW, PA_FIRE_DRAG, PA_FIRE_DUAL };
    const int fVals[3]   = { FIRE_MODE_CLAW_LEFT, FIRE_MODE_DRAG_RIGHT, FIRE_MODE_DUAL_DRAG };
    int bx = x + 82;
    int bw = (w - 82) / 3;
    for (int i = 0; i < 3; ++i) {
        RECT r = { bx + i * bw, cy, bx + (i + 1) * bw - 4, cy + 26 };
        bool on = (p.fireMode == fVals[i]);
        GdiDraw::roundedBox(hdc, r, 5, on ? RGB(155, 50, 50) : RGB(32, 38, 52),
                on ? RGB(255, 110, 90) : RGB(65, 76, 98), on ? 2 : 1);
        SelectObject(hdc, fB);
        GdiDraw::centeredText(hdc, fLbls[i], (r.left + r.right) / 2, (r.top + r.bottom) / 2,
                       on ? RGB(255, 245, 240) : RGB(170, 185, 205));
        hotspots_.push_back(Hotspot{ r, fActs[i], 0 });
    }
}

void StudioPanel::paintTabAddKeys(HDC hdc, int x, int y, int w, HFONT fS, HFONT fB) {
    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Click control -> bind Key, Mouse (LMB/RMB/MMB/X1/X2) or Scroll:", x, y + 8, RGB(175, 200, 225));

    struct PaletteItem {
        const char* title;
        const char* desc;
        KeyActionType type;
        COLORREF accent;
    };
    static const PaletteItem items[] = {
        { "+ Press Key",     "Hold touch while held",      NODE_PRESS,          RGB(80, 195, 255) },
        { "+ Tap / Click",   "Fixed 35ms single tap",      NODE_CLICK,          RGB(95, 210, 255) },
        { "+ Hold-to-ADS",   "Tap on down & release",      NODE_DOWN_UP,        RGB(80, 235, 160) },
        { "+ Free Look",     "Temp drag-look on hold",     NODE_HOLD_VIEW,      RGB(0, 240, 240)  },
        { "+ Swipe Gesture", "Draggable start -> end",     NODE_SWIPE,          RGB(255, 165, 60) },
        { "+ Cursor Toggle", "Tap + free mouse (Bag/Map)", NODE_CURSOR_TOGGLE,  RGB(195, 120, 255)},
        { "+ View Reset",    "Clean camera re-center",     NODE_RESET_VIEW,     RGB(255, 210, 80) },
        { "+ Scope Sens",    "Hold/toggle ADS X/Y",        NODE_SWITCH_SENS,    RGB(215, 155, 255)},
        { "+ Fix Joystick",  "Unstick WASD movement",      NODE_REACTIVATE_JOY, RGB(120, 235, 150)},
        { "+ Up Trigger",    "Tap when key released",      NODE_UP_TRIGGER,     RGB(140, 195, 230)}
    };

    int colW = (w - 8) / 2;
    int tileH = 44;
    int startY = y + 24;
    for (int i = 0; i < 10; ++i) {
        int col = i % 2;
        int row = i / 2;
        int tx = x + col * (colW + 8);
        int ty = startY + row * (tileH + 6);
        RECT r = { tx, ty, tx + colW, ty + tileH };
        GdiDraw::roundedBox(hdc, r, 7, RGB(26, 33, 48), items[i].accent, 1);
        SelectObject(hdc, fB);
        GdiDraw::leftText(hdc, items[i].title, tx + 10, ty + 14, items[i].accent);
        SelectObject(hdc, fS);
        GdiDraw::leftText(hdc, items[i].desc, tx + 10, ty + 31, RGB(175, 192, 212));
        hotspots_.push_back(Hotspot{ r, PA_ADD_NODE, static_cast<int>(items[i].type) });
    }

    int botY = startY + 5 * (tileH + 6) + 4;
    RECT rClr = { x, botY, x + w, botY + 26 };
    GdiDraw::roundedBox(hdc, rClr, 6, RGB(65, 28, 32), RGB(220, 90, 90), 1);
    SelectObject(hdc, fB);
    GdiDraw::centeredText(hdc, "CLEAR ALL EXTRA KEYS (Keep WASD / AIM / FIRE)", (rClr.left + rClr.right) / 2, (rClr.top + rClr.bottom) / 2, RGB(255, 185, 185));
    hotspots_.push_back(Hotspot{ rClr, PA_CLEAR_BUTTONS, 0 });
}

void StudioPanel::paintTabProfiles(HDC hdc, int x, int y, int w,
                                     const std::string& activePath, HFONT fS, HFONT fB) {
    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Create a new saved config or click any saved config below to load:", x, y + 8, RGB(175, 200, 225));

    // Inline text box for New Config Name
    RECT rName = { x, y + 22, x + w, y + 48 };
    GdiDraw::roundedBox(hdc, rName, 6, RGB(12, 16, 25),
            editingNewProfileName_ ? RGB(0, 235, 255) : RGB(75, 92, 120),
            editingNewProfileName_ ? 2 : 1);
    SelectObject(hdc, fB);
    std::string shownName = "Name: " + newProfileName_ + (editingNewProfileName_ ? "_" : "");
    GdiDraw::leftText(hdc, shownName, x + 10, y + 35, RGB(240, 248, 255));
    hotspots_.push_back(Hotspot{ rName, PA_FOCUS_NEW_NAME, 0 });

    // Create Blank / Clone Current buttons
    int halfW = (w - 8) / 2;
    RECT rBlank = { x, y + 54, x + halfW, y + 82 };
    GdiDraw::roundedBox(hdc, rBlank, 6, RGB(24, 72, 105), RGB(75, 205, 255), 1);
    GdiDraw::centeredText(hdc, "+ CREATE BLANK CONFIG", (rBlank.left + rBlank.right) / 2, (rBlank.top + rBlank.bottom) / 2, RGB(230, 248, 255));
    hotspots_.push_back(Hotspot{ rBlank, PA_CREATE_BLANK, 0 });

    RECT rClone = { x + halfW + 8, y + 54, x + w, y + 82 };
    GdiDraw::roundedBox(hdc, rClone, 6, RGB(28, 80, 62), RGB(80, 235, 160), 1);
    GdiDraw::centeredText(hdc, "+ CLONE CURRENT", (rClone.left + rClone.right) / 2, (rClone.top + rClone.bottom) / 2, RGB(230, 255, 240));
    hotspots_.push_back(Hotspot{ rClone, PA_CREATE_CLONE, 0 });

    // Saved profiles list
    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Saved Configs (profiles\\*.cfg):", x, y + 98, RGB(165, 185, 210));

    int listY = y + 110;
    int maxShow = 6;
    for (size_t i = 0; i < profileEntries_.size() && static_cast<int>(i) < maxShow; ++i) {
        const ProfileEntry& e = profileEntries_[i];
        bool isActive = (lstrcmpiA(e.path.c_str(), activePath.c_str()) == 0);

        RECT rRow = { x, listY, x + w - 58, listY + 28 };
        GdiDraw::roundedBox(hdc, rRow, 6,
                isActive ? RGB(20, 68, 52) : RGB(26, 33, 48),
                isActive ? RGB(60, 240, 145) : RGB(62, 76, 102),
                isActive ? 2 : 1);
        SelectObject(hdc, fB);
        std::string rowText = (isActive ? "* " : "  ") + e.displayName + "  (" + e.fileName + ")";
        if (rowText.size() > 42) rowText = rowText.substr(0, 41) + "...";
        GdiDraw::leftText(hdc, rowText, x + 8, listY + 14, isActive ? RGB(170, 255, 210) : RGB(220, 232, 245));
        hotspots_.push_back(Hotspot{ rRow, PA_LOAD_PROFILE, static_cast<int>(i) });

        RECT rDel = { x + w - 52, listY, x + w, listY + 28 };
        GdiDraw::roundedBox(hdc, rDel, 6, RGB(70, 26, 32), RGB(215, 85, 85), 1);
        SelectObject(hdc, fS);
        GdiDraw::centeredText(hdc, "DEL", (rDel.left + rDel.right) / 2, (rDel.top + rDel.bottom) / 2, RGB(255, 180, 180));
        hotspots_.push_back(Hotspot{ rDel, PA_DELETE_PROFILE, static_cast<int>(i) });

        listY += 32;
    }
}

void StudioPanel::paintTabAdvanced(HDC hdc, int x, int y, int w,
                                     const Profile& p, HFONT fS, HFONT fB) {
    int cy = y + 4;
    auto drawSeg2 = [&](const char* lbl, const char* a, const char* b, bool firstOn, int actA, int actB) {
        SelectObject(hdc, fS);
        GdiDraw::leftText(hdc, lbl, x, cy + 13, RGB(200, 215, 235));
        int bx = x + 124;
        int bw = (w - 124) / 2;
        RECT rA = { bx, cy, bx + bw - 4, cy + 26 };
        RECT rB = { bx + bw, cy, x + w, cy + 26 };
        GdiDraw::roundedBox(hdc, rA, 5, firstOn ? RGB(0, 115, 155) : RGB(30, 38, 54), firstOn ? RGB(0, 230, 255) : RGB(62, 76, 100), firstOn ? 2 : 1);
        GdiDraw::roundedBox(hdc, rB, 5, !firstOn ? RGB(0, 115, 155) : RGB(30, 38, 54), !firstOn ? RGB(0, 230, 255) : RGB(62, 76, 100), !firstOn ? 2 : 1);
        SelectObject(hdc, fB);
        GdiDraw::centeredText(hdc, a, (rA.left + rA.right) / 2, (rA.top + rA.bottom) / 2, firstOn ? RGB(255, 255, 255) : RGB(170, 185, 205));
        GdiDraw::centeredText(hdc, b, (rB.left + rB.right) / 2, (rB.top + rB.bottom) / 2, !firstOn ? RGB(255, 255, 255) : RGB(170, 185, 205));
        hotspots_.push_back(Hotspot{ rA, actA, 0 });
        hotspots_.push_back(Hotspot{ rB, actB, 0 });
        cy += 34;
    };

    drawSeg2("Aim Bounds:", "UNBOUNDED (FF)", "BOXED (15ms)", p.unboundedAim, PA_BOUNDS_UNBOUNDED, PA_BOUNDS_BOXED);

    // Polling rate 4-way selector
    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Mouse Polling:", x, cy + 13, RGB(200, 215, 235));
    const char* pLbls[4] = { "125Hz", "250Hz", "500Hz", "MAX" };
    const int pActs[4]   = { PA_POLL_125, PA_POLL_250, PA_POLL_500, PA_POLL_MAX };
    const int pVals[4]   = { 125, 250, 500, 0 };
    int bx = x + 124;
    int bw4 = (w - 124) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT r = { bx + i * bw4, cy, bx + (i + 1) * bw4 - 3, cy + 26 };
        bool on = (p.pollHz == pVals[i]);
        GdiDraw::roundedBox(hdc, r, 5, on ? RGB(78, 52, 140) : RGB(30, 38, 54),
                on ? RGB(180, 145, 255) : RGB(62, 76, 100), on ? 2 : 1);
        SelectObject(hdc, fB);
        GdiDraw::centeredText(hdc, pLbls[i], (r.left + r.right) / 2, (r.top + r.bottom) / 2,
                       on ? RGB(255, 255, 255) : RGB(170, 185, 205));
        hotspots_.push_back(Hotspot{ r, pActs[i], 0 });
    }
    cy += 34;

    drawSeg2("WASD Joystick:", "FF 3-Step Glide", "Fast 3-Step (24ms)", p.joySteppedGlide, PA_JOY_GLIDE, PA_JOY_INSTANT);

    // Invert Y toggle
    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Invert Y Axis:", x, cy + 13, RGB(200, 215, 235));
    RECT rInv = { x + 124, cy, x + 215, cy + 26 };
    GdiDraw::roundedBox(hdc, rInv, 5, p.invertY ? RGB(130, 60, 40) : RGB(30, 38, 54),
            p.invertY ? RGB(255, 130, 90) : RGB(62, 76, 100), 1);
    SelectObject(hdc, fB);
    GdiDraw::centeredText(hdc, p.invertY ? "ON" : "OFF", (rInv.left + rInv.right) / 2, (rInv.top + rInv.bottom) / 2, RGB(240, 248, 255));
    hotspots_.push_back(Hotspot{ rInv, PA_TOGGLE_INVERT_Y, 0 });
    cy += 36;

    // Sliders for Idle Reset & Tap Jitter
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%dms", p.idleResetMs);
    drawSlider(hdc, x, cy, w, "Idle Reset", static_cast<float>(p.idleResetMs), 0.0f, 2000.0f, buf, RGB(95, 200, 255), SL_IDLE_RESET, fS, fB);
    cy += 30;

    std::snprintf(buf, sizeof(buf), "%dpx", p.tapJitterPx);
    drawSlider(hdc, x, cy, w, "Tap Jitter", static_cast<float>(p.tapJitterPx), 0.0f, 12.0f, buf, RGB(140, 225, 165), SL_TAP_JITTER, fS, fB);
    cy += 34;

    // Toggle Aim Key & Hold-to-Unlock Key rebinders
    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Lock/Unlock Key:", x, cy + 13, RGB(200, 215, 235));
    RECT rAimK = { x + 124, cy, x + w - 90, cy + 26 };
    GdiDraw::roundedBox(hdc, rAimK, 5, rebindingToggleAim_ ? RGB(115, 85, 20) : RGB(30, 38, 54),
            rebindingToggleAim_ ? RGB(255, 215, 60) : RGB(80, 180, 255), 1);
    SelectObject(hdc, fB);
    std::string aimKLbl = rebindingToggleAim_ ? "Press Key/Mouse..." : KeyCodes::toName(p.toggleAimVk);
    GdiDraw::centeredText(hdc, aimKLbl, (rAimK.left + rAimK.right) / 2, (rAimK.top + rAimK.bottom) / 2, RGB(245, 250, 255));
    hotspots_.push_back(Hotspot{ rAimK, PA_REBIND_TOGGLE_AIM, 0 });

    RECT rMmbAim = { x + w - 82, cy, x + w, cy + 26 };
    bool isMmbAim = (p.toggleAimVk == VK_MBUTTON);
    GdiDraw::roundedBox(hdc, rMmbAim, 5, isMmbAim ? RGB(0, 115, 155) : RGB(34, 44, 62),
            isMmbAim ? RGB(0, 230, 255) : RGB(90, 140, 195), 1);
    SelectObject(hdc, fS);
    GdiDraw::centeredText(hdc, "MMB", (rMmbAim.left + rMmbAim.right) / 2, (rMmbAim.top + rMmbAim.bottom) / 2, RGB(225, 242, 255));
    hotspots_.push_back(Hotspot{ rMmbAim, PA_SET_TOGGLE_MMB, 0 });
    cy += 34;

    SelectObject(hdc, fS);
    GdiDraw::leftText(hdc, "Hold-Unlock Key:", x, cy + 13, RGB(200, 215, 235));
    RECT rHoldK = { x + 124, cy, x + w - 90, cy + 26 };
    GdiDraw::roundedBox(hdc, rHoldK, 5, rebindingHoldUnlock_ ? RGB(115, 85, 20) : RGB(30, 38, 54),
            rebindingHoldUnlock_ ? RGB(255, 215, 60) : RGB(80, 180, 255), 1);
    SelectObject(hdc, fB);
    std::string holdKLbl = rebindingHoldUnlock_ ? "Press Key/Mouse..." : KeyCodes::toName(p.holdUnlockVk);
    GdiDraw::centeredText(hdc, holdKLbl, (rHoldK.left + rHoldK.right) / 2, (rHoldK.top + rHoldK.bottom) / 2, RGB(245, 250, 255));
    hotspots_.push_back(Hotspot{ rHoldK, PA_REBIND_HOLD_UNLOCK, 0 });

    RECT rClrH = { x + w - 82, cy, x + w, cy + 26 };
    GdiDraw::roundedBox(hdc, rClrH, 5, RGB(58, 32, 36), RGB(200, 95, 95), 1);
    SelectObject(hdc, fS);
    GdiDraw::centeredText(hdc, "CLEAR", (rClrH.left + rClrH.right) / 2, (rClrH.top + rClrH.bottom) / 2, RGB(255, 195, 195));
    hotspots_.push_back(Hotspot{ rClrH, PA_CLEAR_HOLD_UNLOCK, 0 });
}

void StudioPanel::paintNodeInspector(HDC hdc, int x, int y, int w, const KeyNode& node,
                                       bool waitingRebind, HFONT fS, HFONT fB) {
    RECT rc = { x, y, x + w, y + 276 };
    GdiDraw::roundedBox(hdc, rc, 10, RGB(19, 25, 38), RGB(0, 230, 255), 2);

    SelectObject(hdc, fB);
    GdiDraw::leftText(hdc, "KEY INSPECTOR  (Dbl-Click to Rebind)", x + 12, y + 16, RGB(0, 225, 255));

    int cy = y + 32;
    // Label field
    RECT rLbl = { x + 12, cy, x + w - 12, cy + 26 };
    GdiDraw::roundedBox(hdc, rLbl, 5, RGB(12, 16, 24),
            editingNodeLabel_ ? RGB(0, 235, 255) : RGB(65, 82, 110),
            editingNodeLabel_ ? 2 : 1);
    SelectObject(hdc, fB);
    std::string ltxt = "Label: " + node.label + (editingNodeLabel_ ? "_" : "");
    GdiDraw::leftText(hdc, ltxt, x + 20, cy + 13, RGB(240, 248, 255));
    hotspots_.push_back(Hotspot{ rLbl, PA_INSP_FOCUS_LABEL, 0 });
    cy += 34;

    // Bind Key button
    RECT rBind = { x + 12, cy, x + w - 12, cy + 26 };
    GdiDraw::roundedBox(hdc, rBind, 5, waitingRebind ? RGB(120, 88, 22) : RGB(32, 46, 72),
            waitingRebind ? RGB(255, 215, 65) : RGB(90, 190, 255), 1);
    std::string keyDisplay = (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_UP)   ? "SCROLL UP"
                           : (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_DOWN) ? "SCROLL DOWN"
                                                                                     : KeyCodes::comboToName(node.comboVk, node.vk);
    std::string ktxt = waitingRebind ? "Press Key / Mouse / Scroll..." : ("Key: " + keyDisplay + "  [Rebind]");
    GdiDraw::centeredText(hdc, ktxt, x + w / 2, cy + 13, RGB(245, 250, 255));
    hotspots_.push_back(Hotspot{ rBind, PA_INSP_REBIND_KEY, 0 });
    cy += 32;

    // Row 1: Quick Mouse Button Chips (LMB / RMB / MMB / X1 / X2)
    static const char* mLbls[5] = { "LMB", "RMB", "MMB", "X1", "X2" };
    static const int   mVks[5]  = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    int chipW = (w - 24 - 12) / 5;
    for (int i = 0; i < 5; ++i) {
        int cx = x + 12 + i * (chipW + 3);
        RECT rcM = { cx, cy, cx + chipW, cy + 22 };
        bool on = (node.comboVk == 0 && node.vk == mVks[i]);
        GdiDraw::roundedBox(hdc, rcM, 4, on ? RGB(0, 120, 165) : RGB(28, 36, 52),
                on ? RGB(0, 235, 255) : RGB(65, 80, 108), 1);
        SelectObject(hdc, fS);
        GdiDraw::centeredText(hdc, mLbls[i], (rcM.left + rcM.right) / 2, cy + 11,
                       on ? RGB(255, 255, 255) : RGB(180, 200, 225));
        hotspots_.push_back(Hotspot{ rcM, PA_INSP_SET_MOUSE_VK, mVks[i] });
    }
    cy += 27;

    // Row 2: Explicit Mouse Wheel Scroll Up / Scroll Down Buttons
    int halfScrollW = (w - 24 - 6) / 2;
    RECT rcScUp = { x + 12, cy, x + 12 + halfScrollW, cy + 24 };
    RECT rcScDn = { x + 12 + halfScrollW + 6, cy, x + w - 12, cy + 24 };
    bool upOn = (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_UP);
    bool dnOn = (node.comboVk == 0 && node.vk == VK_CUSTOM_MWHEEL_DOWN);
    GdiDraw::roundedBox(hdc, rcScUp, 4, upOn ? RGB(0, 120, 165) : RGB(28, 36, 52),
            upOn ? RGB(0, 235, 255) : RGB(75, 115, 158), upOn ? 2 : 1);
    GdiDraw::roundedBox(hdc, rcScDn, 4, dnOn ? RGB(0, 120, 165) : RGB(28, 36, 52),
            dnOn ? RGB(0, 235, 255) : RGB(75, 115, 158), dnOn ? 2 : 1);
    SelectObject(hdc, fB);
    GdiDraw::centeredText(hdc, "SCROLL UP", (rcScUp.left + rcScUp.right) / 2, cy + 12,
                   upOn ? RGB(255, 255, 255) : RGB(175, 220, 255));
    GdiDraw::centeredText(hdc, "SCROLL DOWN", (rcScDn.left + rcScDn.right) / 2, cy + 12,
                   dnOn ? RGB(255, 255, 255) : RGB(175, 220, 255));
    hotspots_.push_back(Hotspot{ rcScUp, PA_INSP_SET_MOUSE_VK, VK_CUSTOM_MWHEEL_UP });
    hotspots_.push_back(Hotspot{ rcScDn, PA_INSP_SET_MOUSE_VK, VK_CUSTOM_MWHEEL_DOWN });
    cy += 31;

    // Action type stepper < ACTION >
    RECT rPrev = { x + 12, cy, x + 38, cy + 26 };
    RECT rAct  = { x + 42, cy, x + w - 42, cy + 26 };
    RECT rNext = { x + w - 38, cy, x + w - 12, cy + 26 };
    GdiDraw::roundedBox(hdc, rPrev, 5, RGB(34, 42, 60), RGB(95, 165, 230), 1);
    GdiDraw::roundedBox(hdc, rAct,  5, RGB(14, 20, 32), RGB(70, 95, 130), 1);
    GdiDraw::roundedBox(hdc, rNext, 5, RGB(34, 42, 60), RGB(95, 165, 230), 1);
    SelectObject(hdc, fB);
    GdiDraw::centeredText(hdc, "<", (rPrev.left + rPrev.right) / 2, cy + 13, RGB(255, 255, 255));
    GdiDraw::centeredText(hdc, ProfileDefaults::actionTypeToName(node.action), (rAct.left + rAct.right) / 2, cy + 13, RGB(120, 235, 195));
    GdiDraw::centeredText(hdc, ">", (rNext.left + rNext.right) / 2, cy + 13, RGB(255, 255, 255));
    hotspots_.push_back(Hotspot{ rPrev, PA_INSP_ACT_PREV, 0 });
    hotspots_.push_back(Hotspot{ rNext, PA_INSP_ACT_NEXT, 0 });
    cy += 34;

    // Tap/Swipe duration slider
    char dbuf[16];
    std::snprintf(dbuf, sizeof(dbuf), "%dms", node.durationMs);
    drawSlider(hdc, x + 12, cy, w - 24, "Duration", static_cast<float>(node.durationMs), 15.0f, 300.0f, dbuf, RGB(255, 185, 80), SL_NODE_DUR, fS, fB, 62);
    cy += 32;

    // Scope Sens Link toggle + Delete button
    RECT rScp = { x + 12, cy, x + 156, cy + 28 };
    GdiDraw::roundedBox(hdc, rScp, 5, node.switchScopeSens ? RGB(75, 48, 125) : RGB(30, 38, 52),
            node.switchScopeSens ? RGB(195, 145, 255) : RGB(70, 84, 108), 1);
    SelectObject(hdc, fS);
    GdiDraw::centeredText(hdc, node.switchScopeSens ? "Scope Sens: ON" : "Scope Sens: OFF",
                   (rScp.left + rScp.right) / 2, cy + 14, RGB(235, 240, 255));
    hotspots_.push_back(Hotspot{ rScp, PA_INSP_TOGGLE_SCOPE, 0 });

    RECT rDel = { x + 164, cy, x + w - 12, cy + 28 };
    GdiDraw::roundedBox(hdc, rDel, 5, RGB(95, 28, 34), RGB(255, 90, 90), 1);
    SelectObject(hdc, fB);
    GdiDraw::centeredText(hdc, "DELETE", (rDel.left + rDel.right) / 2, cy + 14, RGB(255, 210, 210));
    hotspots_.push_back(Hotspot{ rDel, PA_INSP_DELETE, 0 });
}

bool StudioPanel::containsPoint(int x, int y) const {
    if (GdiDraw::ptInRect(panelRect_, x, y)) return true;
    if (inspectorVisible_ && GdiDraw::ptInRect(inspectorRect_, x, y)) return true;
    return false;
}

void StudioPanel::applySliderValue(int sliderId, float normT, Profile& p, int selectedBtnIndex) {
    normT = clampf(normT, 0.0f, 1.0f);
    for (size_t i = 0; i < sliders_.size(); ++i) {
        if (sliders_[i].id != sliderId) continue;
        float v = sliders_[i].minVal + normT * (sliders_[i].maxVal - sliders_[i].minVal);
        switch (sliderId) {
            case SL_SENS_X:      p.sensX = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case SL_SENS_Y:      p.sensY = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case SL_FIRE_X:      p.fireSensX = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case SL_FIRE_Y:      p.fireSensY = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case SL_SCOPE_X:     p.scopeSensX = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case SL_SCOPE_Y:     p.scopeSensY = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case SL_FF_CA:       p.ffCounterAccel = std::floor(v * 2.0f + 0.5f) / 2.0f; break;
            case SL_SMOOTH:      p.smoothing = std::floor(v * 50.0f + 0.5f) / 50.0f; break;
            case SL_HUD_OPACITY: p.hudOpacity = std::floor(v * 20.0f + 0.5f) / 20.0f; break;
            case SL_IDLE_RESET:  p.idleResetMs = static_cast<int>(std::floor(v / 25.0f + 0.5f)) * 25; break;
            case SL_TAP_JITTER:  p.tapJitterPx = static_cast<int>(std::floor(v + 0.5f)); break;
            case SL_NODE_DUR:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(p.buttons.size())) {
                    p.buttons[selectedBtnIndex].durationMs = static_cast<int>(std::floor(v / 5.0f + 0.5f)) * 5;
                }
                break;
            default: break;
        }
        return;
    }
}

bool StudioPanel::onMouseDown(int x, int y, int /*canvasW*/, int /*canvasH*/,
                                Profile& profile, int& selectedBtnIndex, StudioUIResult& out) {
    if (!containsPoint(x, y)) {
        editingNewProfileName_ = false;
        editingNodeLabel_ = false;
        return false;
    }

    // Check title bar drag first
    if (GdiDraw::ptInRect(titleBarRect_, x, y)) {
        draggingPanel_ = true;
        dragOffX_ = x - panelRect_.left;
        dragOffY_ = y - panelRect_.top;
        editingNewProfileName_ = false;
        editingNodeLabel_ = false;
        return true;
    }

    // Check interactive sliders
    for (size_t i = 0; i < sliders_.size(); ++i) {
        if (GdiDraw::ptInRect(sliders_[i].hitRc, x, y)) {
            activeSliderId_ = sliders_[i].id;
            activeSliderTrack_ = sliders_[i].trackRc;
            float w = static_cast<float>(activeSliderTrack_.right - activeSliderTrack_.left);
            float t = (w > 1.0f) ? (static_cast<float>(x - activeSliderTrack_.left) / w) : 0.0f;
            applySliderValue(activeSliderId_, t, profile, selectedBtnIndex);
            out.profileModified = true;
            editingNewProfileName_ = false;
            editingNodeLabel_ = false;
            return true;
        }
    }

    // Check clickable UI hotspots
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        const Hotspot& hs = hotspots_[i];
        if (!GdiDraw::ptInRect(hs.rc, x, y)) continue;

        editingNewProfileName_ = false;
        editingNodeLabel_ = false;
        rebindingToggleAim_ = false;
        rebindingHoldUnlock_ = false;
        if (hs.id != PA_INSP_REBIND_KEY && hs.id != PA_ADD_NODE) {
            out.cancelRebind = true;
        }

        switch (hs.id) {
            case PA_TOGGLE_COLLAPSE: collapsed_ = !collapsed_; return true;
            case PA_CLOSE_EDIT:      out.requestCloseEdit = true; return true;
            case PA_TAB_SENS:        activeTab_ = 0; return true;
            case PA_TAB_ADD:         activeTab_ = 1; return true;
            case PA_TAB_PROFILES:    activeTab_ = 2; refreshProfileList(); return true;
            case PA_TAB_ADV:         activeTab_ = 3; return true;
            case PA_SAVE:            out.requestSave = true; return true;

            case PA_RESET_SENS: {
                Profile def = ProfileDefaults::createFreeFire();
                profile.sensX = def.sensX;
                profile.sensY = def.sensY;
                profile.fireSensX = def.fireSensX;
                profile.fireSensY = def.fireSensY;
                profile.scopeSensX = def.scopeSensX;
                profile.scopeSensY = def.scopeSensY;
                profile.ffCounterAccel = def.ffCounterAccel;
                profile.smoothing = def.smoothing;
                profile.hudOpacity = def.hudOpacity;
                out.profileModified = true;
                out.toastMessage = "Sensitivities reset to default Free Fire values!";
                return true;
            }

            case PA_FIRE_CLAW: profile.fireMode = FIRE_MODE_CLAW_LEFT; out.profileModified = true; out.toastMessage = "Fire Mode: CLAW-LEFT"; return true;
            case PA_FIRE_DRAG: profile.fireMode = FIRE_MODE_DRAG_RIGHT; out.profileModified = true; out.toastMessage = "Fire Mode: DRAG-FIRE"; return true;
            case PA_FIRE_DUAL: profile.fireMode = FIRE_MODE_DUAL_DRAG; out.profileModified = true; out.toastMessage = "Fire Mode: DUAL-DRAG"; return true;

            case PA_ADD_NODE:
                out.requestAddNode = true;
                out.addNodeType = static_cast<KeyActionType>(hs.extra);
                out.addNodeLabel = ProfileDefaults::actionTypeToName(out.addNodeType);
                return true;

            case PA_CLEAR_BUTTONS:
                profile.buttons.clear();
                selectedBtnIndex = -1;
                out.profileModified = true;
                out.toastMessage = "Cleared extra keys â€” add your own from the palette!";
                return true;

            case PA_FOCUS_NEW_NAME:
                editingNewProfileName_ = true;
                return true;

            case PA_CREATE_BLANK:
            case PA_CREATE_CLONE: {
                std::string baseFname = ProfileStore::sanitizeFilename(newProfileName_);
                std::string fname = baseFname;
                std::string fullPath = profilesDir_ + "\\" + fname + ".cfg";
                // Never silently overwrite an existing saved config (auto-suffix _2, _3, ...)
                int suffix = 2;
                while (GetFileAttributesA(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 100) {
                    char sbuf[32];
                    std::snprintf(sbuf, sizeof(sbuf), "_%d", suffix++);
                    fname = baseFname + sbuf;
                    fullPath = profilesDir_ + "\\" + fname + ".cfg";
                }
                Profile np = (hs.id == PA_CREATE_BLANK)
                    ? ProfileDefaults::createBlank(fname)
                    : profile;
                np.name = (fname == baseFname && !newProfileName_.empty()) ? newProfileName_ : fname;
                if (ProfileStore::save(fullPath, np)) {
                    refreshProfileList();
                    out.requestSwitchProfile = true;
                    out.switchProfilePath = fullPath;
                    out.toastMessage = "Created & loaded config: " + fname + ".cfg";
                } else {
                    out.toastMessage = "ERROR: Could not create " + fullPath;
                }
                return true;
            }

            case PA_LOAD_PROFILE:
                if (hs.extra >= 0 && hs.extra < static_cast<int>(profileEntries_.size())) {
                    out.requestSwitchProfile = true;
                    out.switchProfilePath = profileEntries_[hs.extra].path;
                }
                return true;

            case PA_DELETE_PROFILE:
                if (hs.extra >= 0 && hs.extra < static_cast<int>(profileEntries_.size())) {
                    if (profileEntries_.size() <= 1) {
                        out.toastMessage = "Cannot delete the last remaining config!";
                    } else if (lstrcmpiA(profileEntries_[hs.extra].path.c_str(), activeProfilePath_.c_str()) == 0) {
                        out.toastMessage = "Switch to another config before deleting the active one.";
                    } else {
                        std::remove(profileEntries_[hs.extra].path.c_str());
                        out.toastMessage = "Deleted " + profileEntries_[hs.extra].fileName;
                        refreshProfileList();
                    }
                }
                return true;

            case PA_BOUNDS_UNBOUNDED: profile.unboundedAim = true;  out.profileModified = true; out.toastMessage = "Aim: UNBOUNDED (Free Fire)"; return true;
            case PA_BOUNDS_BOXED:     profile.unboundedAim = false; out.profileModified = true; out.toastMessage = "Aim: BOXED (15ms Edge Reset)"; return true;
            case PA_POLL_125:         profile.pollHz = 125; out.profileModified = true; out.toastMessage = "Polling: 125 Hz"; return true;
            case PA_POLL_250:         profile.pollHz = 250; out.profileModified = true; out.toastMessage = "Polling: 250 Hz"; return true;
            case PA_POLL_500:         profile.pollHz = 500; out.profileModified = true; out.toastMessage = "Polling: 500 Hz"; return true;
            case PA_POLL_MAX:         profile.pollHz = 0;   out.profileModified = true; out.toastMessage = "Polling: MAX (1000 Hz)"; return true;
            case PA_JOY_GLIDE:        profile.joySteppedGlide = true;  out.profileModified = true; out.toastMessage = "Joystick: FF 3-Stage Glide"; return true;
            case PA_JOY_INSTANT:      profile.joySteppedGlide = false; out.profileModified = true; out.toastMessage = "Joystick: Instant Jump"; return true;
            case PA_TOGGLE_INVERT_Y:  profile.invertY = !profile.invertY; out.profileModified = true; return true;
            case PA_REBIND_TOGGLE_AIM:  rebindingToggleAim_ = true;  out.toastMessage = "Press ANY keyboard key or Mouse button (e.g. MMB, LShift) for Aim Lock..."; return true;
            case PA_SET_TOGGLE_MMB:     profile.toggleAimVk = VK_MBUTTON; out.profileModified = true; out.toastMessage = "Aim Lock Toggle set to MMB (Middle Mouse)!"; return true;
            case PA_REBIND_HOLD_UNLOCK: rebindingHoldUnlock_ = true; out.toastMessage = "Press ANY key or Mouse button for Hold-to-Unlock Cursor..."; return true;
            case PA_CLEAR_HOLD_UNLOCK:  profile.holdUnlockVk = 0; out.profileModified = true; out.toastMessage = "Hold-to-Unlock cleared"; return true;

            case PA_INSP_FOCUS_LABEL: editingNodeLabel_ = true; return true;
            case PA_INSP_REBIND_KEY:  out.requestStartRebindNode = true; return true;
            case PA_INSP_SET_MOUSE_VK:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].comboVk = 0;
                    profile.buttons[selectedBtnIndex].vk = hs.extra;
                    out.profileModified = true;
                    out.toastMessage = "Bound '" + profile.buttons[selectedBtnIndex].label + "' -> " + KeyCodes::toName(hs.extra);
                }
                return true;
            case PA_INSP_ACT_PREV:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    KeyNode& b = profile.buttons[selectedBtnIndex];
                    b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 9) % 10);
                    if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) { b.x2 = clampf(b.x + 0.12f, 0.02f, 0.98f); b.durationMs = 120; }
                    out.profileModified = true;
                }
                return true;
            case PA_INSP_ACT_NEXT:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    KeyNode& b = profile.buttons[selectedBtnIndex];
                    b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 1) % 10);
                    if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) { b.x2 = clampf(b.x + 0.12f, 0.02f, 0.98f); b.durationMs = 120; }
                    out.profileModified = true;
                }
                return true;
            case PA_INSP_TOGGLE_SCOPE:
                if (selectedBtnIndex >= 0 && selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
                    profile.buttons[selectedBtnIndex].switchScopeSens = !profile.buttons[selectedBtnIndex].switchScopeSens;
                    out.profileModified = true;
                }
                return true;
            case PA_INSP_DELETE:
                out.requestDeleteSelectedNode = true;
                return true;
            default:
                break;
        }
    }

    return true; // Click landed inside panel background; swallow so we don't accidentally drag canvas nodes behind panel
}

bool StudioPanel::onMouseMove(int x, int y, int canvasW, int canvasH,
                                Profile& profile, int selectedBtnIndex, StudioUIResult& out) {
    if (draggingPanel_ && canvasW > 0 && canvasH > 0) {
        int nx = x - dragOffX_;
        int ny = y - dragOffY_;
        profile.panelX = clampf(static_cast<float>(nx) / static_cast<float>(canvasW), 0.005f, 0.85f);
        profile.panelY = clampf(static_cast<float>(ny) / static_cast<float>(canvasH), 0.005f, 0.85f);
        out.profileModified = true;
        return true;
    }

    if (activeSliderId_ != 0) {
        float w = static_cast<float>(activeSliderTrack_.right - activeSliderTrack_.left);
        float t = (w > 1.0f) ? (static_cast<float>(x - activeSliderTrack_.left) / w) : 0.0f;
        applySliderValue(activeSliderId_, t, profile, selectedBtnIndex);
        out.profileModified = true;
        return true;
    }

    return false;
}

bool StudioPanel::onMouseUp() {
    bool wasDragging = draggingPanel_ || (activeSliderId_ != 0);
    draggingPanel_ = false;
    activeSliderId_ = 0;
    return wasDragging;
}

bool StudioPanel::onKeyDown(int vk, Profile& profile, int selectedBtnIndex, StudioUIResult& out) {
    if (rebindingToggleAim_) {
        if (vk != VK_ESCAPE) {
            vk = KeyCodes::normalizeModifier(vk); // Fix: VK_SHIFT (0x10) -> VK_LSHIFT (0xA0) to match WH_KEYBOARD_LL!
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
        if (vk == VK_RETURN || vk == VK_ESCAPE) {
            editingNewProfileName_ = false;
            return true;
        }
        if (vk == VK_BACK && !newProfileName_.empty()) {
            newProfileName_.erase(newProfileName_.size() - 1);
            return true;
        }
        return true;
    }
    if (editingNodeLabel_) {
        if (vk == VK_RETURN || vk == VK_ESCAPE) {
            editingNodeLabel_ = false;
            return true;
        }
        if (vk == VK_BACK && selectedBtnIndex >= 0 &&
            selectedBtnIndex < static_cast<int>(profile.buttons.size())) {
            std::string& lbl = profile.buttons[selectedBtnIndex].label;
            if (!lbl.empty()) {
                lbl.erase(lbl.size() - 1);
                out.profileModified = true;
            }
            return true;
        }
        return true;
    }
    return false;
}

bool StudioPanel::onMouseBind(int mouseVk, Profile& profile, int /*selectedBtnIndex*/, StudioUIResult& out) {
    if (rebindingToggleAim_) {
        if (mouseVk == VK_LBUTTON) {
            out.toastMessage = "Use Keyboard, MMB, RMB, X1, X2, or Wheel for Aim Lock (LMB is reserved)";
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
            out.toastMessage = "Use Keyboard, MMB, RMB, X1, X2, or Wheel for Hold-Unlock (LMB is reserved)";
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

bool StudioPanel::onChar(char ch, Profile& profile, int selectedBtnIndex, StudioUIResult& out) {
    if (ch < 32 || ch > 126) return false;
    if (editingNewProfileName_) {
        if (newProfileName_.size() < 24) {
            newProfileName_.push_back(ch);
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
