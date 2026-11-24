#include "ui/HudRenderer.hpp"
#include "config/KeyCodes.hpp"
#include "ui/Theme.hpp"

#include <cmath>
#include <cstdio>

namespace {

uint32_t withAlpha(uint32_t rgbHex, float alpha01) {
    if (alpha01 < 0.0f) alpha01 = 0.0f;
    if (alpha01 > 1.0f) alpha01 = 1.0f;
    uint32_t a = static_cast<uint32_t>(alpha01 * 255.0f + 0.5f) & 0xFFu;
    return (a << 24) | (rgbHex & 0x00FFFFFFu);
}

uint32_t nodeAccentRgb(KeyActionType act) {
    switch (act) {
        case NODE_DOWN_UP:        return 0x2CE59Bu; // Emerald
        case NODE_HOLD_VIEW:      return 0x00D4FFu; // Cyan
        case NODE_SWIPE:          return 0xFFB84Du; // Amber
        case NODE_CURSOR_TOGGLE:  return 0xC49BFFu; // Soft lavender
        case NODE_RESET_VIEW:
        case NODE_SWITCH_SENS:
        case NODE_REACTIVATE_JOY: return 0xFFD066u; // Soft gold
        default:                  return 0xDCE8FAu; // Crisp frosted white-slate
    }
}

void drawFrostedCap(Gdiplus::Graphics& g, float cx, float cy, float w, float h,
                    float radius, uint32_t fillArgb, uint32_t strokeArgb, float strokeW) {
    float x = cx - w * 0.5f;
    float y = cy - h * 0.5f;
    GdiPlusDraw::fillRoundedRect(g, x, y, w, h, radius, fillArgb);
    if (strokeW > 0.0f && (strokeArgb >> 24) > 0) {
        GdiPlusDraw::strokeRoundedRect(g, x, y, w, h, radius, strokeArgb, strokeW);
    }
}

void drawSubtleText(Gdiplus::Graphics& g, const std::string& txt,
                    float cx, float cy, float sizePx, bool bold, uint32_t colorArgb) {
    // Soft subtle drop shadow so thin translucent labels stay legible over bright sky/snow in games
    uint32_t a = (colorArgb >> 24) & 0xFFu;
    uint32_t shadowA = (a * 65u) / 100u;
    GdiPlusDraw::textCenter(g, txt, cx, cy + 0.9f, sizePx, bold, (shadowA << 24) | 0x0005080Du);
    GdiPlusDraw::textCenter(g, txt, cx, cy, sizePx, bold, colorArgb);
}

} // namespace

std::string HudRenderer::shortKeyLabel(int comboVk, int vk) {
    if (comboVk == 0) {
        if (vk == VK_CUSTOM_MWHEEL_UP)   return "SCR+";
        if (vk == VK_CUSTOM_MWHEEL_DOWN) return "SCR-";
        if (vk == 0x20) return "SPC";
        if (vk == 0xA0 || vk == 0x10) return "SHFT";
        if (vk == 0xA1) return "RSHF";
        if (vk == 0xA2 || vk == 0x11) return "CTRL";
        if (vk == 0xA3) return "RCTL";
        if (vk == 0xA4 || vk == 0x12) return "ALT";
        if (vk == 0xA5) return "RALT";
        if (vk == 0x14) return "CAPS";
        if (vk == 0xC0) return "~";
        return KeyCodes::toName(vk);
    }
    return KeyCodes::comboToName(comboVk, vk);
}

void HudRenderer::computeNodePillSize(const std::string& keyText, bool editMode, float& outW, float& outH) {
    float base = editMode ? 34.0f : 26.0f;
    outH = base;
    if (keyText.size() <= 1) {
        outW = base;
    } else if (keyText.size() == 2) {
        outW = base + 4.0f;
    } else {
        float perChar = editMode ? 7.6f : 6.4f;
        outW = 14.0f + static_cast<float>(keyText.size()) * perChar;
        if (outW < base + 6.0f) outW = base + 6.0f;
        if (outW > 78.0f) outW = 78.0f;
    }
}

void HudRenderer::computeDeleteChipPos(float nodeCx, float nodeCy, float pillW, float pillH,
                                       float& outChipX, float& outChipY, float& outChipR) {
    outChipX = nodeCx + pillW * 0.5f + 1.0f;
    outChipY = nodeCy - pillH * 0.5f - 1.0f;
    outChipR = 8.0f;
}

void HudRenderer::paintKeymapNodes(Gdiplus::Graphics& g, int w, int h,
                                   const Profile& profile, const EngineTelemetry& tel,
                                   bool editMode, bool demoMode,
                                   int selectedBtnIndex, int hoveredBtnIndex,
                                   bool hoverDeleteChip, bool waitingButtonRebind,
                                   float playOpacity) {
    // In Play Mode, obey user's HUD Opacity smoothly per-pixel; in Edit Mode keep crisp full clarity
    float op = editMode ? 1.0f : ((playOpacity < 0.15f) ? 0.15f : (playOpacity > 1.0f ? 1.0f : playOpacity));
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);

    // =========================================================================
    // 1. WASD Virtual Joystick — ultra-clean translucent emulator ring
    // =========================================================================
    {
        float jx = profile.joyX * fw;
        float jy = profile.joyY * fh;
        float jr = profile.joyRadius * fh;
        if (jr < 28.0f) jr = 28.0f;
        float jsr = profile.joySprintRadius * fh;

        // Subtle sprint ring (Edit Mode only)
        if (editMode && jsr > jr + 6.0f) {
            Gdiplus::Pen dashPen(GdiPlusDraw::argb(withAlpha(0x00D4FFu, 0.35f)), 1.2f);
            dashPen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawEllipse(&dashPen, jx - jsr, jy - jsr, jsr * 2.0f, jsr * 2.0f);
        }

        // Frosted circular pad
        Gdiplus::SolidBrush padBr(GdiPlusDraw::argb(withAlpha(0x0C111Au, editMode ? 0.44f : 0.24f * op)));
        Gdiplus::Pen padPen(GdiPlusDraw::argb(withAlpha(0xE6F0FFu, editMode ? 0.55f : 0.34f * op)), editMode ? 1.5f : 1.15f);
        g.FillEllipse(&padBr, jx - jr, jy - jr, jr * 2.0f, jr * 2.0f);
        g.DrawEllipse(&padPen, jx - jr, jy - jr, jr * 2.0f, jr * 2.0f);

        // Subtle inner thumb guide ring
        float innerR = jr * 0.28f;
        Gdiplus::Pen inPen(GdiPlusDraw::argb(withAlpha(0xFFFFFFu, editMode ? 0.24f : 0.14f * op)), 1.0f);
        g.DrawEllipse(&inPen, jx - innerR, jy - innerR, innerR * 2.0f, innerR * 2.0f);

        // Minimalist W / S / A / D glyphs cleanly inset inside ring
        float off = jr * 0.68f;
        float fontSz = editMode ? 11.5f : 10.5f;
        uint32_t txtCol = withAlpha(0xF0F6FFu, editMode ? 0.92f : 0.78f * op);
        drawSubtleText(g, KeyCodes::toName(profile.vkUp),    jx, jy - off, fontSz, true, txtCol);
        drawSubtleText(g, KeyCodes::toName(profile.vkDown),  jx, jy + off, fontSz, true, txtCol);
        drawSubtleText(g, KeyCodes::toName(profile.vkLeft),  jx - off, jy, fontSz, true, txtCol);
        drawSubtleText(g, KeyCodes::toName(profile.vkRight), jx + off, jy, fontSz, true, txtCol);

        // Live active virtual thumb indicator
        if (tel.joyDown) {
            float tx = tel.joyFingerNormX * fw;
            float ty = tel.joyFingerNormY * fh;
            Gdiplus::SolidBrush dotBr(GdiPlusDraw::argb(withAlpha(0x2CE59Bu, 0.78f * op)));
            Gdiplus::Pen dotPen(GdiPlusDraw::argb(withAlpha(0xFFFFFFu, 0.85f * op)), 1.4f);
            g.FillEllipse(&dotBr, tx - 6.0f, ty - 6.0f, 12.0f, 12.0f);
            g.DrawEllipse(&dotPen, tx - 6.0f, ty - 6.0f, 12.0f, 12.0f);
        }
    }

    // =========================================================================
    // 2. Aim / 360° Camera Anchor — clean minimalist crosshair emblem
    // =========================================================================
    {
        float ax = profile.aimX * fw;
        float ay = profile.aimY * fh;

        if ((editMode && !profile.unboundedAim) || demoMode) {
            float hw = profile.aimHalfW * fw;
            float hh = profile.aimHalfH * fh;
            float bx1 = (ax - hw < fw * 0.33f) ? (fw * 0.33f) : (ax - hw);
            float by1 = (ay - hh < 24.0f) ? 24.0f : (ay - hh);
            float bx2 = (ax + hw > fw - 8.0f) ? (fw - 8.0f) : (ax + hw);
            float by2 = (ay + hh > fh - 8.0f) ? (fh - 8.0f) : (ay + hh);
            GdiPlusDraw::strokeRoundedRect(g, bx1, by1, bx2 - bx1, by2 - by1, 10.0f,
                                           withAlpha(0xFFB84Du, editMode ? 0.50f : 0.25f * op), 1.3f);
        }

        float r = editMode ? 20.0f : 13.0f;
        drawFrostedCap(g, ax, ay, r * 2.0f, r * 2.0f, r,
                       withAlpha(0x0E131Du, editMode ? 0.72f : 0.34f * op),
                       withAlpha(0x00D4FFu, editMode ? 0.85f : 0.45f * op),
                       editMode ? 1.6f : 1.1f);
        GdiPlusDraw::drawIcon(g, ICON_AIM, ax, ay, editMode ? 17.0f : 12.0f,
                              withAlpha(0x00D4FFu, editMode ? 0.96f : 0.65f * op));
        if (editMode) {
            drawSubtleText(g, "AIM (" + shortKeyLabel(0, profile.toggleAimVk) + ")",
                           ax, ay + r + 10.0f, 9.5f, true, withAlpha(0xC8DCFAu, 0.88f));
        }

        if (tel.aimFingerDown && !editMode) {
            float fx = tel.aimFingerNormX * fw;
            float fy = tel.aimFingerNormY * fh;
            uint32_t col = tel.fireHeld ? 0xFF5C6Cu : 0x00D4FFu;
            Gdiplus::SolidBrush fbr(GdiPlusDraw::argb(withAlpha(col, 0.70f * op)));
            g.FillEllipse(&fbr, fx - 4.0f, fy - 4.0f, 8.0f, 8.0f);
        }
    }

    // =========================================================================
    // 3. Fire Buttons (Right Fire & Left Fire) — subtle frosted circular badges
    // =========================================================================
    {
        bool usesRightFire = (profile.fireMode != FIRE_MODE_CLAW_LEFT);
        std::string fireTxt = shortKeyLabel(0, profile.fireVk);

        auto drawFireCap = [&](float fx, float fy, bool active, const char* sub) {
            float r = editMode ? 20.0f : 14.5f;
            float aScale = active ? 1.0f : 0.48f;
            drawFrostedCap(g, fx, fy, r * 2.0f, r * 2.0f, r,
                           withAlpha(0x141923u, (editMode ? 0.72f : 0.40f * op) * aScale),
                           withAlpha(active ? 0xFF6B7Au : 0x8A9BB5u, (editMode ? 0.85f : 0.52f * op) * aScale),
                           editMode ? 1.5f : 1.15f);
            drawSubtleText(g, fireTxt, fx, fy, editMode ? 11.0f : 9.5f, true,
                           withAlpha(0xFFF0F2u, (editMode ? 0.95f : 0.82f * op) * aScale));
            if (editMode) {
                drawSubtleText(g, sub, fx, fy + r + 9.5f, 9.0f, true, withAlpha(0xFFB3BAu, 0.82f * aScale));
            }
        };

        if (usesRightFire || editMode) {
            drawFireCap(profile.fireX * fw, profile.fireY * fh, usesRightFire, "FIRE");
        }
        if (!usesRightFire || editMode) {
            drawFireCap(profile.leftFireX * fw, profile.leftFireY * fh, !usesRightFire, "L-FIRE");
        }
    }

    // =========================================================================
    // 4. Action Nodes — minimalist translucent rounded keycaps!
    // =========================================================================
    for (size_t i = 0; i < profile.buttons.size(); ++i) {
        const KeyNode& b = profile.buttons[i];
        float bx = b.x * fw;
        float by = b.y * fh;

        bool selected = editMode && (static_cast<int>(i) == selectedBtnIndex);
        bool hovered  = editMode && (static_cast<int>(i) == hoveredBtnIndex);
        uint32_t accentRgb = nodeAccentRgb(b.action);

        // Smooth vector swipe trajectory (Edit Mode only)
        if (editMode && b.action == NODE_SWIPE) {
            float bx2 = b.x2 * fw;
            float by2 = b.y2 * fh;
            Gdiplus::Pen spPen(GdiPlusDraw::argb(withAlpha(0xFFB84Du, 0.78f)), 1.8f);
            spPen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawLine(&spPen, bx, by, bx2, by2);
            drawFrostedCap(g, bx2, by2, 16.0f, 16.0f, 8.0f,
                           withAlpha(0x2A1E0Cu, 0.88f), withAlpha(0xFFB84Du, 0.95f), 1.5f);
            drawSubtleText(g, "END", bx2, by2 + 14.0f, 8.5f, true, withAlpha(0xFFD488u, 0.9f));
        }

        std::string keyTxt = (selected && waitingButtonRebind)
            ? "?"
            : shortKeyLabel(b.comboVk, b.vk);

        float pw = 0.0f, ph = 0.0f;
        computeNodePillSize(keyTxt, editMode, pw, ph);
        float rad = ph * 0.5f;

        // Soft outer selection halo (Edit Mode selected node)
        if (selected) {
            GdiPlusDraw::strokeRoundedRect(g, bx - pw * 0.5f - 3.0f, by - ph * 0.5f - 3.0f,
                                           pw + 6.0f, ph + 6.0f, rad + 3.0f,
                                           withAlpha(0x00D4FFu, 0.40f), 2.5f);
        }

        // Translucent frosted keycap fill + hairline border
        uint32_t fillRgb = selected ? 0x0E2A3Cu : (hovered ? 0x1A2434u : 0x0E131Cu);
        float fillA      = editMode ? (selected ? 0.84f : 0.70f) : (0.42f * op);
        uint32_t rimRgb  = selected ? 0x00D4FFu : (hovered ? 0xFFFFFFu : accentRgb);
        float rimA       = editMode ? (selected ? 0.98f : (hovered ? 0.88f : 0.58f)) : (0.48f * op);
        float rimW       = selected ? 1.8f : (editMode ? 1.3f : 1.0f);

        drawFrostedCap(g, bx, by, pw, ph, rad,
                       withAlpha(fillRgb, fillA),
                       withAlpha(rimRgb, rimA), rimW);

        // Single clean crisp key label centered inside the keycap (NEVER overflows!)
        float fontSz = (keyTxt.size() <= 2) ? (editMode ? 12.5f : 11.0f)
                                            : (editMode ? 10.5f : 9.2f);
        uint32_t txtCol = withAlpha(selected ? 0xFFFFFFu : 0xF2F7FFu, editMode ? 0.98f : 0.88f * op);
        drawSubtleText(g, keyTxt, bx, by, fontSz, true, txtCol);

        // Tiny subtle mode indicator pip on bottom rim for non-PRESS special nodes
        if (b.action != NODE_PRESS && b.action != NODE_CLICK) {
            Gdiplus::SolidBrush pipBr(GdiPlusDraw::argb(withAlpha(accentRgb, editMode ? 0.95f : 0.65f * op)));
            g.FillEllipse(&pipBr, bx - 2.2f, by + ph * 0.5f - 4.2f, 4.4f, 4.4f);
        }

        // Edit Mode ONLY: subtle floating micro-label underneath + 1-click 'x' delete badge on selected node
        if (editMode) {
            if (!b.label.empty() && b.label != keyTxt) {
                drawSubtleText(g, b.label, bx, by + ph * 0.5f + 8.5f, 8.8f, false, withAlpha(0xD4E2F5u, 0.82f));
            }
            if (selected) {
                float cx = 0.0f, cy = 0.0f, cr = 0.0f;
                computeDeleteChipPos(bx, by, pw, ph, cx, cy, cr);
                Gdiplus::SolidBrush dbr(GdiPlusDraw::argb(withAlpha(hoverDeleteChip ? 0xFF4757u : 0x2A1820u, 0.95f)));
                Gdiplus::Pen dpen(GdiPlusDraw::argb(withAlpha(0xFF6B7Au, 0.95f)), 1.2f);
                g.FillEllipse(&dbr, cx - cr, cy - cr, cr * 2.0f, cr * 2.0f);
                g.DrawEllipse(&dpen, cx - cr, cy - cr, cr * 2.0f, cr * 2.0f);
                GdiPlusDraw::drawIcon(g, ICON_CLOSE, cx, cy, 7.0f, 0xFFFFFFFFu);
            }
        }
    }
}

void HudRenderer::paintCrosshairDot(Gdiplus::Graphics& g, int w, int h) {
    float cx = static_cast<float>(w) * 0.5f;
    float cy = static_cast<float>(h) * 0.5f;
    Gdiplus::SolidBrush ring(GdiPlusDraw::argb(0x88000000u));
    Gdiplus::SolidBrush dot(GdiPlusDraw::argb(0xE02CE59Bu));
    g.FillEllipse(&ring, cx - 2.5f, cy - 2.5f, 5.0f, 5.0f);
    g.FillEllipse(&dot,  cx - 1.5f, cy - 1.5f, 3.0f, 3.0f);
}

void HudRenderer::paintFullscreenStatus(Gdiplus::Graphics& g, int w,
                                        const Profile& profile, const EngineTelemetry& tel) {
    const char* modeTxt = tel.aimMode ? "AIM LOCKED" : "CURSOR MODE";
    char txt[160];
    std::snprintf(txt, sizeof(txt), "%s  ·  %s  ·  F11 Exit Fullscreen", modeTxt, profile.name.c_str());
    float pw = 290.0f;
    float ph = 24.0f;
    float cx = static_cast<float>(w) * 0.5f;
    float cy = 20.0f;
    drawFrostedCap(g, cx, cy, pw, ph, 12.0f,
                   0xAA0E131Cu,
                   tel.aimMode ? 0x992CE59Bu : 0x99FFB84Du, 1.1f);
    GdiPlusDraw::textCenter(g, txt, cx, cy, 10.5f, true, Theme::TEXT_PRIMARY);
}

void HudRenderer::paintRebindPrompt(Gdiplus::Graphics& g, int w, int h,
                                    const std::string& targetName, bool specialAimKey) {
    float pw = 510.0f;
    if (pw > static_cast<float>(w - 24)) pw = static_cast<float>(w - 24);
    float ph = 60.0f;
    float cx = static_cast<float>(w) * 0.5f;
    float cy = static_cast<float>(h) - ph * 0.5f - 28.0f;
    if (cy < ph * 0.5f + 12.0f) cy = ph * 0.5f + 12.0f;

    drawFrostedCap(g, cx, cy, pw, ph, 12.0f, 0xE2101622u, Theme::ACCENT_AMBER, 1.6f);
    GdiPlusDraw::textCenter(g, "REBIND: " + targetName, cx, cy - 11.0f, 13.0f, true, Theme::ACCENT_AMBER);
    const char* hint = specialAimKey
        ? "Press ANY Keyboard Key (LShift, ~) or Mouse Button (MMB / RMB / X1 / X2 / Scroll)  |  ESC = Cancel"
        : "Press ANY Keyboard Key, Mouse Button (LMB / RMB / MMB / X1 / X2), Scroll, or Combo  |  ESC = Cancel";
    GdiPlusDraw::textCenter(g, hint, cx, cy + 12.0f, 10.5f, false, Theme::TEXT_SECONDARY);
}

void HudRenderer::paintToast(Gdiplus::Graphics& g, int w, int h, const std::string& msg) {
    float pw = 34.0f + static_cast<float>(msg.size()) * 6.8f;
    if (pw < 140.0f) pw = 140.0f;
    if (pw > static_cast<float>(w - 28)) pw = static_cast<float>(w - 28);
    float ph = 30.0f;
    float cx = static_cast<float>(w) * 0.5f;
    float cy = static_cast<float>(h) - ph * 0.5f - 18.0f;

    drawFrostedCap(g, cx, cy, pw, ph, 15.0f, 0xDC101622u, 0xAA00D4FFu, 1.2f);
    GdiPlusDraw::textCenter(g, msg, cx, cy, 11.5f, true, Theme::TEXT_PRIMARY);
}
