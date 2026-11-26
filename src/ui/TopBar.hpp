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
    void paint(Gdiplus::Graphics& g, const RECT& bounds,
               const std::string& deviceLabel, bool connected, bool demoMode,
               const Profile& profile, const EngineTelemetry& tel,
               bool alwaysOnTop, bool maximized, int hoverX, int hoverY,
               const TelemetrySummary& videoTel = TelemetrySummary());

    TopBarAction hitTest(int x, int y) const;
    bool isInteractivePoint(int x, int y) const { return hitTest(x, y) != TB_NONE; }

private:
    struct Btn {
        RECT rc;
        TopBarAction action;
    };
    std::vector<Btn> buttons_;
};
