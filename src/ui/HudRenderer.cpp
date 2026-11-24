#include "ui/HudRenderer.hpp"

#include "config/KeyCodes.hpp"

#include <cstdio>
#include <cstring>

void HudRenderer::paintDemoBackdrop(HDC hdc, int w, int h, const HudFonts& fonts) {
    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(26, 33, 46));
    HGDIOBJ oldPen = SelectObject(hdc, gridPen);
    for (int x = 64; x < w; x += 64) {
        MoveToEx(hdc, x, 0, NULL);
        LineTo(hdc, x, h);
    }
    for (int y = 64; y < h; y += 64) {
        MoveToEx(hdc, 0, y, NULL);
        LineTo(hdc, w, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);

    int cx = w / 2, cy = h / 2;
    HPEN chPen = CreatePen(PS_SOLID, 1, RGB(60, 82, 104));
    oldPen = SelectObject(hdc, chPen);
    MoveToEx(hdc, cx - 14, cy, NULL); LineTo(hdc, cx + 14, cy);
    MoveToEx(hdc, cx, cy - 14, NULL); LineTo(hdc, cx, cy + 14);
    SelectObject(hdc, oldPen);
    DeleteObject(chPen);

    SelectObject(hdc, fonts.smallFont);
    GdiDraw::centeredText(hdc, "OFFLINE KEYMAP STUDIO CANVAS  (Click 'CFG' ball or F2 to open Studio | ~ to test Aim)",
                          w / 2, h - 54, RGB(115, 138, 165));
}

void HudRenderer::paintFloatingBall(HDC hdc, int w, int h, const Profile& profile,
                                    bool editMode, const HudFonts& fonts) {
    int bx = static_cast<int>(profile.floatBtnX * static_cast<float>(w));
    int by = static_cast<int>(profile.floatBtnY * static_cast<float>(h));
    COLORREF fill = editMode ? RGB(0, 125, 170) : RGB(20, 28, 42);
    COLORREF ring = editMode ? RGB(255, 255, 255) : RGB(0, 225, 255);
    GdiDraw::circle(hdc, bx, by, FLOAT_BALL_RADIUS, fill, ring, 2, false);
    SelectObject(hdc, fonts.boldFont);
    GdiDraw::centeredText(hdc, "CFG", bx, by, RGB(235, 250, 255));
}

void HudRenderer::paintPlayStatusBar(HDC hdc, int w, const Profile& profile,
                                     const EngineTelemetry& tel, const HudFonts& fonts) {
    const char* fmode = (profile.fireMode == FIRE_MODE_DRAG_RIGHT) ? "DRAG-FIRE"
                      : (profile.fireMode == FIRE_MODE_CLAW_LEFT)  ? "CLAW-LEFT"
                                                                   : "DUAL-DRAG";
    const char* stateTag = tel.holdViewActive ? "FREELOOK"
                         : (tel.aimMode && tel.scopeActive) ? "AIM+ADS"
                         : tel.aimMode ? "AIM ON" : "CURSOR";
    char hzBuf[16];
    if (profile.pollHz <= 0) std::snprintf(hzBuf, sizeof(hzBuf), "MAX");
    else std::snprintf(hzBuf, sizeof(hzBuf), "%dHz", profile.pollHz);

    std::string aimKeyName = KeyCodes::toName(profile.toggleAimVk);
    char text[300];
    std::snprintf(text, sizeof(text),
                  " %s | %s | %s | X:%.2f Y:%.2f | FX:%.2f FY:%.2f | %s (%u/s) | %s:Aim  F2/CFG:Studio  F4:HUD ",
                  stateTag, fmode, profile.name.c_str(),
                  profile.sensX, profile.sensY,
                  profile.fireSensX, profile.fireSensY,
                  hzBuf, tel.touchRateHz, aimKeyName.c_str());

    SelectObject(hdc, fonts.boldFont);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text, static_cast<int>(std::strlen(text)), &sz);
    int bw = sz.cx + 16;
    int bh = 24;
    RECT bar = { (w - bw) / 2, 6, (w + bw) / 2, 6 + bh };
    COLORREF border = tel.holdViewActive ? RGB(0, 240, 255)
                    : (tel.aimMode && tel.scopeActive) ? RGB(170, 140, 255)
                    : tel.aimMode ? RGB(0, 230, 140) : RGB(255, 185, 40);
    GdiDraw::roundedBox(hdc, bar, 8, RGB(18, 22, 32), border, 2);
    GdiDraw::centeredText(hdc, text, w / 2, (bar.top + bar.bottom) / 2,
                          tel.aimMode ? RGB(185, 255, 220) : RGB(255, 225, 140));
}

void HudRenderer::paintKeymapNodes(HDC hdc, int w, int h, const Profile& profile,
                                   const EngineTelemetry& tel, bool editMode, bool demoMode,
                                   int selectedBtnIndex, bool waitingButtonRebind,
                                   const HudFonts& fonts) {
    // 1. WASD Virtual Joystick
    {
        int jx = static_cast<int>(profile.joyX * w);
        int jy = static_cast<int>(profile.joyY * h);
        int jr = static_cast<int>(profile.joyRadius * h);
        int jsr = static_cast<int>(profile.joySprintRadius * h);

        if (editMode) {
            HPEN dashPen = CreatePen(PS_DASH, 1, RGB(60, 200, 220));
            HGDIOBJ op = SelectObject(hdc, dashPen);
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Ellipse(hdc, jx - jsr, jy - jsr, jx + jsr, jy + jsr);
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(dashPen);
        }

        GdiDraw::circle(hdc, jx, jy, jr, RGB(20, 40, 32), RGB(60, 235, 145), editMode ? 2 : 1, !editMode);
        SelectObject(hdc, fonts.boldFont);
        GdiDraw::centeredText(hdc, "W", jx, jy - jr + 12, RGB(140, 255, 190));
        GdiDraw::centeredText(hdc, "S", jx, jy + jr - 12, RGB(140, 255, 190));
        GdiDraw::centeredText(hdc, "A", jx - jr + 12, jy, RGB(140, 255, 190));
        GdiDraw::centeredText(hdc, "D", jx + jr - 12, jy, RGB(140, 255, 190));

        if (tel.joyDown) {
            int tx = static_cast<int>(tel.joyFingerNormX * w);
            int ty = static_cast<int>(tel.joyFingerNormY * h);
            GdiDraw::circle(hdc, tx, ty, 8, RGB(60, 255, 150), RGB(255, 255, 255), 2);
        }
    }

    // 2. Aim Anchor & Safe Box
    {
        int ax = static_cast<int>(profile.aimX * w);
        int ay = static_cast<int>(profile.aimY * h);
        int hw = static_cast<int>(profile.aimHalfW * w);
        int hh = static_cast<int>(profile.aimHalfH * h);

        if ((editMode && !profile.unboundedAim) || demoMode) {
            RECT box = {
                (ax - hw < w / 3) ? (w / 3) : (ax - hw),
                (ay - hh < 30) ? 30 : (ay - hh),
                (ax + hw > w - 6) ? (w - 6) : (ax + hw),
                (ay + hh > h - 6) ? (h - 6) : (ay + hh)
            };
            GdiDraw::roundedBox(hdc, box, 8, RGB(0, 0, 0), RGB(255, 195, 45), editMode ? 2 : 1, true);
        }

        GdiDraw::circle(hdc, ax, ay, editMode ? 22 : 14, RGB(45, 36, 16), RGB(255, 200, 50), 2, !editMode);
        SelectObject(hdc, fonts.boldFont);
        GdiDraw::centeredText(hdc, "AIM", ax, ay, RGB(255, 220, 100));

        if (tel.aimFingerDown) {
            int fx = static_cast<int>(tel.aimFingerNormX * w);
            int fy = static_cast<int>(tel.aimFingerNormY * h);
            COLORREF c = tel.fireHeld ? RGB(255, 70, 70) : RGB(255, 215, 0);
            GdiDraw::circle(hdc, fx, fy, 7, c, RGB(255, 255, 255), 2);
        }
    }

    // 3. Fire Buttons (Right Fire & Left Fire)
    {
        bool usesRightFire = (profile.fireMode != FIRE_MODE_CLAW_LEFT);
        int fx = static_cast<int>(profile.fireX * w);
        int fy = static_cast<int>(profile.fireY * h);
        if (usesRightFire || editMode) {
            COLORREF border = usesRightFire ? RGB(255, 80, 60) : RGB(130, 75, 75);
            GdiDraw::circle(hdc, fx, fy, 24, RGB(58, 22, 22), border, 2, !editMode);
            SelectObject(hdc, fonts.boldFont);
            GdiDraw::centeredText(hdc, KeyCodes::toName(profile.fireVk), fx, fy - 5, RGB(255, 170, 150));
            SelectObject(hdc, fonts.smallFont);
            GdiDraw::centeredText(hdc, "FIRE", fx, fy + 9, RGB(255, 120, 100));
        }

        if (!usesRightFire || editMode) {
            int lfx = static_cast<int>(profile.leftFireX * w);
            int lfy = static_cast<int>(profile.leftFireY * h);
            COLORREF border = (!usesRightFire) ? RGB(255, 80, 60) : RGB(130, 75, 75);
            GdiDraw::circle(hdc, lfx, lfy, 20, RGB(52, 22, 28), border, 2, !editMode);
            SelectObject(hdc, fonts.smallFont);
            GdiDraw::centeredText(hdc, "L-FIRE", lfx, lfy, RGB(255, 175, 175));
        }
    }

    // 4. Action Nodes
    for (size_t i = 0; i < profile.buttons.size(); ++i) {
        const KeyNode& b = profile.buttons[i];
        int bx = static_cast<int>(b.x * w);
        int by = static_cast<int>(b.y * h);
        int br = static_cast<int>(b.radius * h);
        if (br < 18) br = 18;
        if (br > 28) br = 28;

        if (editMode && b.action == NODE_SWIPE) {
            int bx2 = static_cast<int>(b.x2 * w);
            int by2 = static_cast<int>(b.y2 * h);
            HPEN spPen = CreatePen(PS_SOLID, 2, RGB(255, 165, 60));
            HGDIOBJ op = SelectObject(hdc, spPen);
            MoveToEx(hdc, bx, by, NULL);
            LineTo(hdc, bx2, by2);
            SelectObject(hdc, op);
            DeleteObject(spPen);
            GdiDraw::circle(hdc, bx2, by2, 9, RGB(255, 165, 60), RGB(255, 255, 255), 2);
            SelectObject(hdc, fonts.smallFont);
            GdiDraw::centeredText(hdc, "END", bx2, by2 + 16, RGB(255, 195, 110));
        }

        bool selected = editMode && (static_cast<int>(i) == selectedBtnIndex);
        COLORREF typeColor = RGB(80, 190, 255);
        if (b.action == NODE_DOWN_UP) typeColor = RGB(80, 235, 160);
        else if (b.action == NODE_HOLD_VIEW) typeColor = RGB(0, 240, 240);
        else if (b.action == NODE_SWIPE) typeColor = RGB(255, 165, 60);
        else if (b.action == NODE_CURSOR_TOGGLE) typeColor = RGB(195, 120, 255);
        else if (b.action == NODE_RESET_VIEW || b.action == NODE_SWITCH_SENS || b.action == NODE_REACTIVATE_JOY)
            typeColor = RGB(255, 210, 80);

        COLORREF border = selected ? RGB(255, 255, 255) : typeColor;
        COLORREF fill   = selected ? RGB(30, 72, 96) : RGB(24, 30, 44);

        GdiDraw::circle(hdc, bx, by, br, fill, border, selected ? 3 : 2, !editMode);

        SelectObject(hdc, fonts.boldFont);
        std::string keyTxt;
        if (selected && waitingButtonRebind) {
            keyTxt = "?";
        } else if (b.comboVk == 0 && b.vk == VK_CUSTOM_MWHEEL_UP) {
            keyTxt = "SCRL+";
        } else if (b.comboVk == 0 && b.vk == VK_CUSTOM_MWHEEL_DOWN) {
            keyTxt = "SCRL-";
        } else {
            keyTxt = KeyCodes::comboToName(b.comboVk, b.vk);
        }
        GdiDraw::centeredText(hdc, keyTxt, bx, by - 5, RGB(245, 250, 255));
        SelectObject(hdc, fonts.smallFont);
        GdiDraw::centeredText(hdc, b.label, bx, by + 9, RGB(170, 210, 240));
        if (editMode && b.action != NODE_PRESS && b.action != NODE_CLICK) {
            GdiDraw::centeredText(hdc, ProfileDefaults::actionTypeToName(b.action), bx, by + br + 9, typeColor);
        }
    }
}

void HudRenderer::paintRebindPrompt(HDC hdc, int w, int h, const std::string& targetName,
                                    bool specialAimKey, const HudFonts& fonts) {
    const int bw = 576;
    const int bh = 70;
    int topY = h - bh - 56;
    if (topY < 14) topY = 14;
    RECT rc = { (w - bw) / 2, topY, (w + bw) / 2, topY + bh };
    GdiDraw::roundedBox(hdc, rc, 12, RGB(18, 26, 42), RGB(255, 210, 60), 3);

    SelectObject(hdc, fonts.titleFont);
    GdiDraw::centeredText(hdc, "REPLACE BUTTON: " + targetName, w / 2, rc.top + 22, RGB(255, 220, 80));

    SelectObject(hdc, fonts.smallFont);
    const char* hint = specialAimKey
        ? "Press ANY Keyboard Key (e.g. LShift, ~) or Mouse Button (MMB / RMB / X1 / X2 / Wheel)  |  ESC = Cancel"
        : "Press ANY Keyboard Key, Mouse Button (LMB / RMB / MMB / X1 / X2), Wheel, or Combo  |  ESC = Cancel";
    GdiDraw::centeredText(hdc, hint, w / 2, rc.top + 48, RGB(215, 232, 248));
}

void HudRenderer::paintToast(HDC hdc, int w, int h, const std::string& msg, const HudFonts& fonts) {
    SelectObject(hdc, fonts.boldFont);
    SIZE sz;
    GetTextExtentPoint32A(hdc, msg.c_str(), static_cast<int>(msg.size()), &sz);
    int bw = sz.cx + 28;
    int bh = 30;
    RECT trc = { (w - bw) / 2, h - bh - 16, (w + bw) / 2, h - 16 };
    GdiDraw::roundedBox(hdc, trc, 10, RGB(18, 24, 36), RGB(0, 215, 255), 2);
    GdiDraw::centeredText(hdc, msg, w / 2, (trc.top + trc.bottom) / 2, RGB(240, 248, 255));
}
