#include "ui/TopBar.hpp"
#include "ui/Theme.hpp"

#include <cstdio>

namespace {

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

} // namespace

void TopBar::paint(Gdiplus::Graphics& g, const RECT& bounds,
                   const std::string& deviceLabel, bool connected, bool demoMode,
                   const Profile& profile, const EngineTelemetry& tel,
                   bool alwaysOnTop, bool maximized, int hoverX, int hoverY,
                   const TelemetrySummary& videoTel) {
    buttons_.clear();

    const int x0 = bounds.left;
    const int y0 = bounds.top;
    const int x1 = bounds.right;
    const int y1 = bounds.bottom;
    const float cy = static_cast<float>(y0 + y1) * 0.5f;

    GdiPlusDraw::fillRect(g, bounds, Theme::BG_SHELL);
    GdiPlusDraw::hLine(g, x0, x1, y1 - 1, Theme::BORDER_CHROME);

    // 1. Brand Logo + Title
    GdiPlusDraw::drawIcon(g, ICON_LOGO, static_cast<float>(x0 + 22), cy, 16.0f, Theme::ACCENT_CYAN);
    GdiPlusDraw::textLeft(g, "PHANTOM TOUCH", static_cast<float>(x0 + 38), cy, 13.0f, true, Theme::TEXT_PRIMARY, 130.0f);

    // 2. Device Status Pill
    int curX = x0 + 174;
    uint32_t dotCol = connected ? Theme::ACCENT_GREEN : (demoMode ? Theme::ACCENT_CYAN : Theme::ACCENT_AMBER);
    std::string devTxt = connected ? deviceLabel : (demoMode ? "Offline Studio" : "No Device");
    if (devTxt.size() > 26) devTxt = devTxt.substr(0, 25) + "...";
    int devW = 26 + static_cast<int>(devTxt.size()) * 7;
    if (devW < 110) devW = 110;
    if (devW > 225) devW = 225;
    RECT rDev = { curX, y0 + 7, curX + devW, y1 - 7 };
    GdiPlusDraw::roundedCard(g, rDev, 13.0f, Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
    {
        Gdiplus::SolidBrush dbr(GdiPlusDraw::argb(dotCol));
        g.FillEllipse(&dbr, static_cast<float>(rDev.left + 10), cy - 3.5f, 7.0f, 7.0f);
    }
    GdiPlusDraw::textLeft(g, devTxt, static_cast<float>(rDev.left + 23), cy, 11.5f, false, Theme::TEXT_SECONDARY, static_cast<float>(devW - 28));
    curX = rDev.right + 8;

    // 3. Clickable Active Profile Pill
    std::string profName = profile.name.empty() ? "Profile" : profile.name;
    if (profName.size() > 22) profName = profName.substr(0, 21) + "...";
    int profW = 34 + static_cast<int>(profName.size()) * 7;
    if (profW < 112) profW = 112;
    if (profW > 205) profW = 205;
    RECT rProf = { curX, y0 + 7, curX + profW, y1 - 7 };
    bool hovProf = ptIn(rProf, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rProf, 13.0f, hovProf ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovProf ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textLeft(g, "CFG: " + profName, static_cast<float>(rProf.left + 11), cy, 11.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(profW - 20));
    buttons_.push_back(Btn{ rProf, TB_OPEN_PROFILES });
    curX = rProf.right + 8;

    // 4. Right Window Chrome Controls (Close, Max, Min, Pin)
    const int winBtnW = 42;
    RECT rClose = { x1 - winBtnW, y0, x1, y1 - 1 };
    RECT rMax   = { rClose.left - winBtnW, y0, rClose.left, y1 - 1 };
    RECT rMin   = { rMax.left - winBtnW, y0, rMax.left, y1 - 1 };
    RECT rPin   = { rMin.left - 34, y0 + 6, rMin.left - 4, y1 - 6 };

    if (ptIn(rClose, hoverX, hoverY)) GdiPlusDraw::fillRect(g, rClose, Theme::ACCENT_RED);
    if (ptIn(rMax, hoverX, hoverY))   GdiPlusDraw::fillRect(g, rMax, Theme::BG_CARD_HOVER);
    if (ptIn(rMin, hoverX, hoverY))   GdiPlusDraw::fillRect(g, rMin, Theme::BG_CARD_HOVER);
    bool hovPin = ptIn(rPin, hoverX, hoverY);
    if (alwaysOnTop || hovPin) {
        GdiPlusDraw::roundedCard(g, rPin, 6.0f, alwaysOnTop ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD_HOVER,
                                 alwaysOnTop ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    }

    GdiPlusDraw::drawIcon(g, ICON_PIN,      static_cast<float>(rPin.left + rPin.right) * 0.5f,   cy, 12.0f, alwaysOnTop ? Theme::ACCENT_CYAN : Theme::TEXT_MUTED);
    GdiPlusDraw::drawIcon(g, ICON_MINIMIZE, static_cast<float>(rMin.left + rMin.right) * 0.5f,   cy, 11.0f, Theme::TEXT_SECONDARY);
    GdiPlusDraw::drawIcon(g, ICON_MAXIMIZE, static_cast<float>(rMax.left + rMax.right) * 0.5f,   cy, maximized ? 10.0f : 11.0f, Theme::TEXT_SECONDARY);
    GdiPlusDraw::drawIcon(g, ICON_CLOSE,    static_cast<float>(rClose.left + rClose.right) * 0.5f, cy, 11.0f,
                          ptIn(rClose, hoverX, hoverY) ? 0xFFFFFFFFu : Theme::TEXT_SECONDARY);

    buttons_.push_back(Btn{ rPin, TB_TOGGLE_PIN });
    buttons_.push_back(Btn{ rMin, TB_MINIMIZE });
    buttons_.push_back(Btn{ rMax, TB_MAXIMIZE });
    buttons_.push_back(Btn{ rClose, TB_CLOSE });

    // 5. Live Telemetry Pill + Video Pipeline Latency Pill (0.1)
    int availRight = rPin.left - 12;
    if (connected && videoTel.presentedCount > 0 && (availRight - curX) >= 400) {
        char latBuf[80];
        std::snprintf(latBuf, sizeof(latBuf), "p50 %.1f / p95 %.1fms | %llu/%llu/%llu",
                      videoTel.rxToPresentP50Ms, videoTel.rxToPresentP95Ms,
                      static_cast<unsigned long long>(videoTel.decodedCount),
                      static_cast<unsigned long long>(videoTel.presentedCount),
                      static_cast<unsigned long long>(videoTel.skippedCount));
        RECT rLat = { curX, y0 + 7, curX + 196, y1 - 7 };
        GdiPlusDraw::roundedCard(g, rLat, 12.0f, Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textCenter(g, latBuf, static_cast<float>(rLat.left + rLat.right) * 0.5f, cy,
                                10.0f, false, Theme::TEXT_SECONDARY);
        curX = rLat.right + 8;
    }
    if (availRight - curX >= 216) {
        char telBuf[96];
        uint32_t bg = Theme::ACCENT_AMBER_BG;
        uint32_t bd = Theme::ACCENT_AMBER;
        uint32_t fg = Theme::ACCENT_AMBER;

        if (!profile.keymapEnabled) {
            std::snprintf(telBuf, sizeof(telBuf), "TYPING MODE  |  F1: Game");
            bg = Theme::ACCENT_CYAN_BG;
            bd = Theme::ACCENT_CYAN;
            fg = Theme::ACCENT_CYAN;
        } else {
            const char* modeTxt = tel.holdViewActive ? "FREELOOK"
                                : (tel.aimMode && tel.scopeActive) ? "AIM + ADS"
                                : tel.aimMode ? "AIM LOCKED" : "CURSOR MODE";
            const char* fireTxt = (profile.fireMode == FIRE_MODE_DRAG_RIGHT) ? "DRAG-FIRE"
                                : (profile.fireMode == FIRE_MODE_CLAW_LEFT)  ? "CLAW-LEFT"
                                                                             : "DUAL-DRAG";
            if (tel.gamepadConnected) {
                std::snprintf(telBuf, sizeof(telBuf), "%s | PAD | %u/s", modeTxt, tel.touchRateHz);
            } else {
                std::snprintf(telBuf, sizeof(telBuf), "%s  |  %s  |  %u/s", modeTxt, fireTxt, tel.touchRateHz);
            }
            if (tel.aimMode) {
                bg = Theme::ACCENT_GREEN_BG;
                bd = Theme::ACCENT_GREEN;
                fg = Theme::ACCENT_GREEN;
            }
        }
        int pillW = 216;
        if (availRight - curX < pillW) pillW = availRight - curX;
        RECT rTel = { availRight - pillW, y0 + 7, availRight, y1 - 7 };
        bool hovTel = ptIn(rTel, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rTel, 13.0f, hovTel ? Theme::BG_CARD_HOVER : bg, bd, 1.2f);
        GdiPlusDraw::textCenter(g, telBuf, static_cast<float>(rTel.left + rTel.right) * 0.5f, cy,
                                11.0f, true, fg);
        buttons_.push_back(Btn{ rTel, TB_TOGGLE_AIM });
    }
}

TopBarAction TopBar::hitTest(int x, int y) const {
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (ptIn(buttons_[i].rc, x, y)) {
            return buttons_[i].action;
        }
    }
    return TB_NONE;
}
