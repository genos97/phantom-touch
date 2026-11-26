#include "config/Profile.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>

namespace {

std::string trimUpper(const std::string& raw) {
    size_t s = 0;
    while (s < raw.size() && std::isspace(static_cast<unsigned char>(raw[s]))) ++s;
    size_t e = raw.size();
    while (e > s && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    std::string out = raw.substr(s, e - s);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

} // namespace

std::string ProfileDefaults::actionTypeToName(KeyActionType a) {
    switch (a) {
        case NODE_PRESS:          return "PRESS";
        case NODE_CLICK:          return "CLICK";
        case NODE_DOWN_UP:        return "DOWN_UP";
        case NODE_UP_TRIGGER:     return "UP_TRIGGER";
        case NODE_HOLD_VIEW:      return "HOLD_VIEW";
        case NODE_SWIPE:          return "SWIPE";
        case NODE_CURSOR_TOGGLE:  return "CURSOR_TOGGLE";
        case NODE_RESET_VIEW:     return "RESET_VIEW";
        case NODE_SWITCH_SENS:    return "SWITCH_SENS";
        case NODE_REACTIVATE_JOY: return "REACTIVATE_JOY";
        default:                  return "PRESS";
    }
}

KeyActionType ProfileDefaults::nameToActionType(const std::string& raw) {
    std::string s = trimUpper(raw);
    if (s == "CLICK" || s == "TAP")          return NODE_CLICK;
    if (s == "DOWN_UP" || s == "HOLD_ADS")   return NODE_DOWN_UP;
    if (s == "UP_TRIGGER" || s == "UP")      return NODE_UP_TRIGGER;
    if (s == "HOLD_VIEW" || s == "FREELOOK") return NODE_HOLD_VIEW;
    if (s == "SWIPE")                        return NODE_SWIPE;
    if (s == "CURSOR_TOGGLE" || s == "CURSOR" || s == "1" || s == "TRUE") return NODE_CURSOR_TOGGLE;
    if (s == "RESET_VIEW")                   return NODE_RESET_VIEW;
    if (s == "SWITCH_SENS")                  return NODE_SWITCH_SENS;
    if (s == "REACTIVATE_JOY")               return NODE_REACTIVATE_JOY;
    return NODE_PRESS;
}

Profile ProfileDefaults::createFreeFire() {
    Profile p;
    p.name = "Free Fire MAX (S21 FE)";
    p.appPackage = "com.dts.freefiremax,com.dts.freefireth,com.dts.freefireind";
    p.scrcpyArgs = "--max-fps=90 --video-bit-rate=16M";
    p.controlMaxSize = 0;
    p.pollHz = 250;

    p.toggleAimVk = 0xC0; // ~
    p.holdUnlockVk = 0;
    p.resetTouchVk = 0x74; // F5
    p.showHudInPlay = true;

    p.aimX = 0.620f;
    p.aimY = 0.700f;
    p.aimHalfW = 0.160f;
    p.aimHalfH = 0.640f;

    p.unboundedAim = true;
    p.invertY = false;
    p.ffCounterAccel = 7.0f;
    p.sensX = 0.95f;
    p.sensY = 1.15f;
    p.fireSensX = 0.80f;
    p.fireSensY = 1.25f;
    p.scopeSensX = 0.65f;
    p.scopeSensY = 0.75f;
    p.accelPower = 1.00f;
    p.accelRefSpeed = 14.0f;
    p.accelCap = 2.00f;
    p.smoothing = 0.10f;
    p.idleResetMs = 700;
    p.touchDownDelayMs = 15;
    p.touchUpDelayMs = 0;
    p.tapJitterPx = 0;

    p.fireMode = FIRE_MODE_CLAW_LEFT;
    p.fireVk = 0x01; // LMB
    p.fireX = 0.851f;
    p.fireY = 0.728f;
    p.fireBoxHalfW = 0.350f;
    p.fireBoxHalfH = 0.720f;
    p.leftFireX = 0.236f;
    p.leftFireY = 0.428f;

    p.joyX = 0.162f;
    p.joyY = 0.750f;
    p.joyRadius = 0.085f;
    p.joySprintRadius = 0.160f;
    p.joyWalkRadius = 0.042f;
    p.joySteppedGlide = true;
    p.vkUp = 'W';
    p.vkDown = 'S';
    p.vkLeft = 'A';
    p.vkRight = 'D';
    p.vkSprint = 0xA0; // LSHIFT
    p.vkWalk = 0xA2;   // LCTRL

    auto addNode = [&](const char* label, int comboVk, int vk, KeyActionType act,
                       float x, float y, bool scopeSens = false, int durMs = 35) {
        KeyNode n;
        n.label = label;
        n.comboVk = comboVk;
        n.vk = vk;
        n.action = act;
        n.x = x;
        n.y = y;
        n.x2 = x;
        n.y2 = y;
        n.durationMs = durMs;
        n.radius = 0.030f;
        n.switchScopeSens = scopeSens;
        p.buttons.push_back(n);
    };

    // Calibrated to Samsung S21 FE 2340x1080 Free Fire MAX HUD
    addNode("SCOPE",    0, 0x02, NODE_PRESS,         0.931f, 0.495f, true);
    addNode("JUMP",     0, 0x20, NODE_PRESS,         0.941f, 0.658f);
    addNode("CROUCH",   0, 'C',  NODE_PRESS,         0.883f, 0.892f);
    addNode("PRONE",    0, 'Z',  NODE_PRESS,         0.793f, 0.892f);
    addNode("RELOAD",   0, 'R',  NODE_CLICK,         0.880f, 0.085f);
    addNode("GLOO",     0, 'E',  NODE_PRESS,         0.270f, 0.855f);
    addNode("MEDKIT",   0, '4',  NODE_CLICK,         0.081f, 0.648f);
    addNode("WPN1",     0, '1',  NODE_CLICK,         0.880f, 0.085f);
    addNode("WPN2",     0, '2',  NODE_CLICK,         0.840f, 0.215f);
    addNode("MELEE",    0, '3',  NODE_CLICK,         0.932f, 0.215f);
    addNode("SWITCH",   0, 'Q',  NODE_CLICK,         0.770f, 0.085f);
    addNode("LOOT",     0, 'F',  NODE_CLICK,         0.720f, 0.340f);
    addNode("SPRINT",   0, 0x14, NODE_CLICK,         0.128f, 0.422f);
    addNode("USE",      0, 'G',  NODE_PRESS,         0.650f, 0.670f);
    addNode("FREELOOK", 0, 0xA4, NODE_HOLD_VIEW,     0.775f, 0.275f);
    addNode("BAG",      0, 0x09, NODE_CURSOR_TOGGLE, 0.081f, 0.542f);
    addNode("MAP",      0, 'M',  NODE_CURSOR_TOGGLE, 0.095f, 0.140f);
    addNode("RSTVIEW",  0, 'V',  NODE_RESET_VIEW,    0.500f, 0.150f);

    return p;
}

Profile ProfileDefaults::createBlank(const std::string& name) {
    Profile p = createFreeFire();
    p.name = name.empty() ? "Custom Keymap" : name;
    p.appPackage.clear();
    p.keymapEnabled = true;
    p.buttons.clear();
    return p;
}

Profile ProfileDefaults::createDesktop(const std::string& name) {
    Profile p = createBlank(name.empty() ? "Desktop (Typing)" : name);
    p.keymapEnabled = false;
    p.showHudInPlay = false;
    return p;
}

bool ProfileDefaults::matchesPackage(const std::string& packageList, const std::string& activePkg) {
    if (packageList.empty() || activePkg.empty()) return false;
    size_t pos = 0;
    while (pos < packageList.size()) {
        size_t comma = packageList.find(',', pos);
        std::string tok = (comma == std::string::npos)
            ? packageList.substr(pos)
            : packageList.substr(pos, comma - pos);
        size_t a = 0;
        while (a < tok.size() && std::isspace(static_cast<unsigned char>(tok[a]))) ++a;
        size_t b = tok.size();
        while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1]))) --b;
        std::string clean = tok.substr(a, b - a);
        if (!clean.empty() && clean == activePkg) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

void ProfileDefaults::mapEdgeAnchored(float nx, float ny, bool edgeAnchored,
                                      float refAspect, float liveAspect,
                                      float& outX, float& outY) {
    outX = nx;
    outY = ny;
    if (!edgeAnchored || refAspect < 0.30f || liveAspect < 0.30f) return;
    const bool refLand = (refAspect >= 1.0f);
    const bool liveLand = (liveAspect >= 1.0f);
    if (refLand != liveLand) return;
    if (std::fabs(refAspect - liveAspect) < 0.02f) return;

    float ratio = refAspect / liveAspect;
    if (ratio < 0.60f) ratio = 0.60f;
    if (ratio > 1.35f) ratio = 1.35f;

    float mappedX = nx;
    if (nx <= 0.35f) {
        mappedX = nx * ratio;
    } else if (nx >= 0.65f) {
        mappedX = 1.0f - (1.0f - nx) * ratio;
    } else {
        float t = (nx - 0.35f) / 0.30f;
        float leftEdge = 0.35f * ratio;
        float rightEdge = 1.0f - 0.35f * ratio;
        mappedX = leftEdge + t * (rightEdge - leftEdge);
    }
    if (mappedX < 0.01f) mappedX = 0.01f;
    if (mappedX > 0.99f) mappedX = 0.99f;
    outX = mappedX;
}

void ProfileDefaults::unmapEdgeAnchored(float liveX, float liveY, bool edgeAnchored,
                                        float refAspect, float liveAspect,
                                        float& outX, float& outY) {
    outX = liveX;
    outY = liveY;
    if (!edgeAnchored || refAspect < 0.30f || liveAspect < 0.30f) return;
    const bool refLand = (refAspect >= 1.0f);
    const bool liveLand = (liveAspect >= 1.0f);
    if (refLand != liveLand) return;
    if (std::fabs(refAspect - liveAspect) < 0.02f) return;

    float ratio = refAspect / liveAspect;
    if (ratio < 0.60f) ratio = 0.60f;
    if (ratio > 1.35f) ratio = 1.35f;

    float leftEdge = 0.35f * ratio;
    float rightEdge = 1.0f - 0.35f * ratio;
    float origX = liveX;
    if (liveX <= leftEdge) {
        origX = liveX / ratio;
    } else if (liveX >= rightEdge) {
        origX = 1.0f - (1.0f - liveX) / ratio;
    } else if (rightEdge > leftEdge + 0.005f) {
        float t = (liveX - leftEdge) / (rightEdge - leftEdge);
        origX = 0.35f + t * 0.30f;
    }
    if (origX < 0.01f) origX = 0.01f;
    if (origX > 0.99f) origX = 0.99f;
    outX = origX;
}

Profile ProfileDefaults::resolveLayout(const Profile& src, uint32_t screenW, uint32_t screenH) {
    if (!src.edgeAnchored || screenW < 64 || screenH < 64) {
        return src;
    }
    const float liveAspect = static_cast<float>(screenW) / static_cast<float>(screenH);
    if (std::fabs(src.refAspect - liveAspect) < 0.02f) {
        return src;
    }

    Profile out = src;
    mapEdgeAnchored(src.aimX, src.aimY, true, src.refAspect, liveAspect, out.aimX, out.aimY);
    mapEdgeAnchored(src.fireX, src.fireY, true, src.refAspect, liveAspect, out.fireX, out.fireY);
    mapEdgeAnchored(src.leftFireX, src.leftFireY, true, src.refAspect, liveAspect, out.leftFireX, out.leftFireY);
    mapEdgeAnchored(src.joyX, src.joyY, true, src.refAspect, liveAspect, out.joyX, out.joyY);
    for (size_t i = 0; i < out.buttons.size(); ++i) {
        mapEdgeAnchored(src.buttons[i].x, src.buttons[i].y, true, src.refAspect, liveAspect,
                        out.buttons[i].x, out.buttons[i].y);
        mapEdgeAnchored(src.buttons[i].x2, src.buttons[i].y2, true, src.refAspect, liveAspect,
                        out.buttons[i].x2, out.buttons[i].y2);
    }
    return out;
}
