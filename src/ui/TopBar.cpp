#include "ui/TopBar.hpp"
#include "ui/Strings.hpp"
#include "ui/Theme.hpp"

#include <cstdio>

namespace {

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

int dp(int px) {
    return GdiPlusDraw::dp(px);
}

} // namespace

void TopBar::layout(const RECT& bounds,
                    const std::string& deviceLabel, bool connected, bool demoMode,
                    const Profile& profile, const EngineTelemetry& tel,
                    const TelemetrySummary& videoTel,
                    TopBarConnState connState) {
    buttons_.clear();
    bounds_ = bounds;
    showLatPill_ = false;
    showTelPill_ = false;
    ZeroMemory(&rLat_, sizeof(rLat_));
    ZeroMemory(&rTel_, sizeof(rTel_));

    const int x0 = bounds.left;
    const int y0 = bounds.top;
    const int x1 = bounds.right;
    const int y1 = bounds.bottom;

    TopBarConnState st = connState;
    if (st == TB_CONN_IDLE) {
        if (connected) st = TB_CONN_CONNECTED;
        else if (demoMode) st = TB_CONN_DEMO;
    }

    // Device Status Pill
    int curX = x0 + dp(174);
    if (st == TB_CONN_CONNECTED)         dotCol_ = Theme::ACCENT_GREEN;
    else if (st == TB_CONN_RECONNECTING) dotCol_ = Theme::ACCENT_AMBER;
    else if (st == TB_CONN_LOST)         dotCol_ = Theme::ACCENT_RED;
    else if (st == TB_CONN_DEMO)         dotCol_ = Theme::ACCENT_CYAN;
    else                                 dotCol_ = Theme::TEXT_MUTED;
    devTxt_ = connected ? deviceLabel
                        : (demoMode ? Strings::tr("tb.offline_studio", "Offline Studio")
                        : (st == TB_CONN_RECONNECTING ? Strings::tr("tb.reconnecting", "Reconnecting...")
                        : (st == TB_CONN_LOST ? Strings::tr("tb.lost_link", "Lost Link")
                        : Strings::tr("tb.no_device", "No Device"))));
    if (devTxt_.size() > 26) devTxt_ = devTxt_.substr(0, 25) + "...";
    int devW = dp(26 + static_cast<int>(devTxt_.size()) * 7);
    if (devW < dp(110)) devW = dp(110);
    if (devW > dp(225)) devW = dp(225);
    rDev_ = { curX, y0 + dp(7), curX + devW, y1 - dp(7) };
    curX = rDev_.right + dp(8);

    // Clickable Active Profile Pill (opens searchable dropdown)
    std::string profName = profile.name.empty() ? "Profile" : profile.name;
    if (profName.size() > 20) profName = profName.substr(0, 19) + "...";
    profTxt_ = "CFG: " + profName + " v";
    int profW = dp(40 + static_cast<int>(profName.size()) * 7);
    if (profW < dp(118)) profW = dp(118);
    if (profW > dp(215)) profW = dp(215);
    rProf_ = { curX, y0 + dp(7), curX + profW, y1 - dp(7) };
    buttons_.push_back(Btn{ rProf_, TB_OPEN_PROFILES });
    curX = rProf_.right + dp(8);

    // Right Window Chrome Controls (Close, Max, Min, Pin, Help)
    const int winBtnW = dp(42);
    rClose_   = { x1 - winBtnW, y0, x1, y1 - 1 };
    maxBtnRc_ = { rClose_.left - winBtnW, y0, rClose_.left, y1 - 1 };
    rMin_     = { maxBtnRc_.left - winBtnW, y0, maxBtnRc_.left, y1 - 1 };
    rPin_     = { rMin_.left - dp(34), y0 + dp(6), rMin_.left - dp(4), y1 - dp(6) };
    rHelp_    = { rPin_.left - dp(30), y0 + dp(6), rPin_.left - dp(4), y1 - dp(6) };

    buttons_.push_back(Btn{ rHelp_, TB_OPEN_HELP });
    buttons_.push_back(Btn{ rPin_, TB_TOGGLE_PIN });
    buttons_.push_back(Btn{ rMin_, TB_MINIMIZE });
    buttons_.push_back(Btn{ maxBtnRc_, TB_MAXIMIZE });
    buttons_.push_back(Btn{ rClose_, TB_CLOSE });

    // Live Telemetry Pill + Video Pipeline Latency Pill (0.1 + U4)
    int availRight = rHelp_.left - dp(12);
    if (connected && videoTel.presentedCount > 0 && (availRight - curX) >= dp(400)) {
        char latBuf[80];
        std::snprintf(latBuf, sizeof(latBuf), "%.0ffps · p50 %.1f/%.1fms",
                      videoTel.measuredFps,
                      videoTel.rxToPresentP50Ms, videoTel.rxToPresentP95Ms);
        latTxt_ = latBuf;
        rLat_ = { curX, y0 + dp(7), curX + dp(184), y1 - dp(7) };
        showLatPill_ = true;
        curX = rLat_.right + dp(8);
    }
    if (availRight - curX >= dp(216)) {
        char telBuf[96];
        telBg_ = Theme::ACCENT_AMBER_BG;
        telBd_ = Theme::ACCENT_AMBER;
        telFg_ = Theme::ACCENT_AMBER;

        if (!profile.keymapEnabled) {
            std::string tmode = Strings::tr("tb.typing_mode", "TYPING  |  F1: Game");
            std::snprintf(telBuf, sizeof(telBuf), "%s", tmode.c_str());
            telBg_ = Theme::ACCENT_CYAN_BG;
            telBd_ = Theme::ACCENT_CYAN;
            telFg_ = Theme::ACCENT_CYAN;
        } else {
            const char* modeTxt = tel.holdViewActive ? "FREELOOK"
                                : (tel.aimMode && tel.scopeActive) ? "AIM+ADS (~)"
                                : tel.aimMode ? "AIM (~ / F1)" : "CURSOR (F1)";
            const char* fireTxt = (profile.fireMode == FIRE_MODE_DRAG_RIGHT) ? "DRAG-FIRE"
                                : (profile.fireMode == FIRE_MODE_CLAW_LEFT)  ? "CLAW-LEFT"
                                                                             : "DUAL-DRAG";
            if (tel.gamepadConnected) {
                std::snprintf(telBuf, sizeof(telBuf), "%s | PAD | %u/s", modeTxt, tel.touchRateHz);
            } else {
                std::snprintf(telBuf, sizeof(telBuf), "%s | %s | %u/s", modeTxt, fireTxt, tel.touchRateHz);
            }
            if (tel.aimMode) {
                telBg_ = Theme::ACCENT_GREEN_BG;
                telBd_ = Theme::ACCENT_GREEN;
                telFg_ = Theme::ACCENT_GREEN;
            }
        }
        telTxt_ = telBuf;
        int pillW = dp(224);
        if (availRight - curX < pillW) pillW = availRight - curX;
        rTel_ = { availRight - pillW, y0 + dp(7), availRight, y1 - dp(7) };
        showTelPill_ = true;
        buttons_.push_back(Btn{ rTel_, TB_TOGGLE_AIM });
    }
}

void TopBar::paint(Gdiplus::Graphics& g, const RECT& bounds,
                   const std::string& deviceLabel, bool connected, bool demoMode,
                   const Profile& profile, const EngineTelemetry& tel,
                   bool alwaysOnTop, bool maximized, int hoverX, int hoverY,
                   const TelemetrySummary& videoTel,
                   TopBarConnState connState) {
    layout(bounds, deviceLabel, connected, demoMode, profile, tel, videoTel, connState);

    const int x0 = bounds.left;
    const int x1 = bounds.right;
    const int y1 = bounds.bottom;
    const float cy = static_cast<float>(bounds.top + y1) * 0.5f;

    GdiPlusDraw::fillRect(g, bounds, Theme::BG_SHELL);
    GdiPlusDraw::hLine(g, x0, x1, y1 - 1, Theme::BORDER_CHROME);

    // 1. Brand Logo + Title
    GdiPlusDraw::drawIcon(g, ICON_LOGO, static_cast<float>(x0 + dp(22)), cy, GdiPlusDraw::dpf(16.0f), Theme::ACCENT_CYAN);
    GdiPlusDraw::textLeft(g, "PHANTOM TOUCH", static_cast<float>(x0 + dp(38)), cy, 13.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(dp(130)));

    // 2. Device Status Pill
    GdiPlusDraw::roundedCard(g, rDev_, GdiPlusDraw::dpf(13.0f), Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
    {
        float dotR = GdiPlusDraw::dpf(3.5f);
        Gdiplus::SolidBrush dbr(GdiPlusDraw::argb(dotCol_));
        g.FillEllipse(&dbr, static_cast<float>(rDev_.left + dp(10)), cy - dotR, dotR * 2.0f, dotR * 2.0f);
    }
    GdiPlusDraw::textLeft(g, devTxt_, static_cast<float>(rDev_.left + dp(23)), cy, 11.5f, false,
                          Theme::TEXT_SECONDARY, static_cast<float>((rDev_.right - rDev_.left) - dp(28)));

    // 3. Clickable Active Profile Pill
    bool hovProf = ptIn(rProf_, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rProf_, GdiPlusDraw::dpf(13.0f), hovProf ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovProf ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textLeft(g, profTxt_, static_cast<float>(rProf_.left + dp(11)), cy, 11.5f, true,
                          Theme::TEXT_PRIMARY, static_cast<float>((rProf_.right - rProf_.left) - dp(20)));

    // 4. Right Window Chrome Controls
    if (ptIn(rClose_, hoverX, hoverY))   GdiPlusDraw::fillRect(g, rClose_, Theme::ACCENT_RED);
    if (ptIn(maxBtnRc_, hoverX, hoverY)) GdiPlusDraw::fillRect(g, maxBtnRc_, Theme::BG_CARD_HOVER);
    if (ptIn(rMin_, hoverX, hoverY))     GdiPlusDraw::fillRect(g, rMin_, Theme::BG_CARD_HOVER);
    bool hovHelp = ptIn(rHelp_, hoverX, hoverY);
    if (hovHelp) {
        GdiPlusDraw::roundedCard(g, rHelp_, GdiPlusDraw::dpf(6.0f), Theme::BG_CARD_HOVER, Theme::ACCENT_CYAN, 1.0f);
    }
    bool hovPin = ptIn(rPin_, hoverX, hoverY);
    if (alwaysOnTop || hovPin) {
        GdiPlusDraw::roundedCard(g, rPin_, GdiPlusDraw::dpf(6.0f), alwaysOnTop ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD_HOVER,
                                 alwaysOnTop ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    }

    GdiPlusDraw::textCenter(g, "?", static_cast<float>(rHelp_.left + rHelp_.right) * 0.5f, cy,
                            12.5f, true, hovHelp ? Theme::ACCENT_CYAN : Theme::TEXT_MUTED);
    GdiPlusDraw::drawIcon(g, ICON_PIN,      static_cast<float>(rPin_.left + rPin_.right) * 0.5f,       cy, GdiPlusDraw::dpf(12.0f), alwaysOnTop ? Theme::ACCENT_CYAN : Theme::TEXT_MUTED);
    GdiPlusDraw::drawIcon(g, ICON_MINIMIZE, static_cast<float>(rMin_.left + rMin_.right) * 0.5f,       cy, GdiPlusDraw::dpf(11.0f), Theme::TEXT_SECONDARY);
    GdiPlusDraw::drawIcon(g, ICON_MAXIMIZE, static_cast<float>(maxBtnRc_.left + maxBtnRc_.right) * 0.5f, cy, GdiPlusDraw::dpf(maximized ? 10.0f : 11.0f), Theme::TEXT_SECONDARY);
    GdiPlusDraw::drawIcon(g, ICON_CLOSE,    static_cast<float>(rClose_.left + rClose_.right) * 0.5f,   cy, GdiPlusDraw::dpf(11.0f),
                          ptIn(rClose_, hoverX, hoverY) ? 0xFFFFFFFFu : Theme::TEXT_SECONDARY);

    // 5. Telemetry Pills
    if (showLatPill_) {
        GdiPlusDraw::roundedCard(g, rLat_, GdiPlusDraw::dpf(12.0f), Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::textCenter(g, latTxt_, static_cast<float>(rLat_.left + rLat_.right) * 0.5f, cy,
                                10.0f, false, Theme::TEXT_SECONDARY);
    }
    if (showTelPill_) {
        bool hovTel = ptIn(rTel_, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rTel_, GdiPlusDraw::dpf(13.0f), hovTel ? Theme::BG_CARD_HOVER : telBg_, telBd_, 1.2f);
        GdiPlusDraw::textCenter(g, telTxt_, static_cast<float>(rTel_.left + rTel_.right) * 0.5f, cy,
                                11.0f, true, telFg_);
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

bool TopBar::hoverItemAt(int x, int y, int& outId, RECT& outRc) const {
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (ptIn(buttons_[i].rc, x, y)) {
            outId = static_cast<int>(buttons_[i].action);
            outRc = buttons_[i].rc;
            return true;
        }
    }
    return false;
}

std::string TopBar::tooltipAt(int x, int y) const {
    if (showLatPill_ && ptIn(rLat_, x, y)) {
        return "Live video FPS and rx->present latency (Ctrl+Shift+D)";
    }
    TopBarAction a = hitTest(x, y);
    switch (a) {
        case TB_OPEN_PROFILES: return "Switch or search profiles (Ctrl+K)";
        case TB_TOGGLE_AIM:    return "Toggle Aim Lock <-> Cursor Mode (F1)";
        case TB_OPEN_HELP:     return "Setup guide & Cheat Sheet (Ctrl+/)";
        case TB_TOGGLE_PIN:    return "Pin window Always on Top";
        case TB_MINIMIZE:      return "Minimize";
        case TB_MAXIMIZE:      return "Maximize / Restore (Win11 Snap)";
        case TB_CLOSE:         return "Close Phantom Touch (Alt+F4)";
        default:               return "";
    }
}
