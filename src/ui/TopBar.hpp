#pragma once

#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "ui/GdiPlusDraw.hpp"
#include "video/LatencyTelemetry.hpp"

#include <string>
#include <vector>

enum TopBarAction {
    TB_NONE = 0,
    TB_OPEN_PROFILES,
    TB_TOGGLE_AIM,
    TB_TOGGLE_PIN,
    TB_MINIMIZE,
    TB_MAXIMIZE,
    TB_CLOSE
};

class TopBar {
public:
    void layout(const RECT& bounds,
                const std::string& deviceLabel, bool connected, bool demoMode,
                const Profile& profile, const EngineTelemetry& tel,
                const TelemetrySummary& videoTel = TelemetrySummary());

    void paint(Gdiplus::Graphics& g, const RECT& bounds,
               const std::string& deviceLabel, bool connected, bool demoMode,
               const Profile& profile, const EngineTelemetry& tel,
               bool alwaysOnTop, bool maximized, int hoverX, int hoverY,
               const TelemetrySummary& videoTel = TelemetrySummary());

    TopBarAction hitTest(int x, int y) const;
    bool hoverItemAt(int x, int y, int& outId, RECT& outRc) const;
    RECT maximizeButtonRect() const { return maxBtnRc_; }
    bool isInteractivePoint(int x, int y) const { return hitTest(x, y) != TB_NONE; }

private:
    struct Btn {
        RECT rc;
        TopBarAction action;
    };
    std::vector<Btn> buttons_;
    RECT bounds_ = { 0, 0, 0, 0 };
    RECT rDev_ = { 0, 0, 0, 0 };
    RECT rProf_ = { 0, 0, 0, 0 };
    RECT rPin_ = { 0, 0, 0, 0 };
    RECT rMin_ = { 0, 0, 0, 0 };
    RECT maxBtnRc_ = { 0, 0, 0, 0 };
    RECT rClose_ = { 0, 0, 0, 0 };
    RECT rLat_ = { 0, 0, 0, 0 };
    RECT rTel_ = { 0, 0, 0, 0 };
    std::string devTxt_;
    std::string profTxt_;
    std::string latTxt_;
    std::string telTxt_;
    uint32_t dotCol_ = 0;
    uint32_t telBg_ = 0;
    uint32_t telBd_ = 0;
    uint32_t telFg_ = 0;
    bool showLatPill_ = false;
    bool showTelPill_ = false;
};
