#pragma once

#include "ui/GdiPlusDraw.hpp"

#include <string>
#include <vector>

enum SideRailAction {
    SR_NONE = 0,
    SR_AIM_TOGGLE,
    SR_DRAWER_KEYS,
    SR_DRAWER_SENS,
    SR_DRAWER_PROFILES,
    SR_HUD_TOGGLE,
    SR_FIRE_CYCLE,
    SR_RESET_TOUCH,
    SR_FULLSCREEN,
    SR_ANDROID_BACK,
    SR_ANDROID_HOME,
    SR_ANDROID_RECENTS,
    SR_ANDROID_VOL_UP,
    SR_ANDROID_VOL_DN,
    SR_SCREENSHOT,
    SR_DRAWER_DEVICE
};

struct SideRailState {
    bool aimMode = false;
    bool drawerOpen = false;
    int activeDrawerTab = 0; // 0=Keys, 1=Sens, 2=Profiles, 3=Device
    bool showHud = true;
    bool fullscreen = false;
    bool connected = false;
};

class SideRail {
public:
    void paint(Gdiplus::Graphics& g, const RECT& bounds,
               const SideRailState& state, int hoverX, int hoverY);

    SideRailAction hitTest(int x, int y) const;
    std::string tooltipAt(int x, int y) const;

private:
    struct Item {
        RECT rc;
        SideRailAction action;
        std::string tip;
    };
    std::vector<Item> items_;
};
