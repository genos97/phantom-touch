#pragma once

#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "ui/GdiDraw.hpp"

#include <string>

struct HudFonts {
    HFONT smallFont = NULL;
    HFONT boldFont  = NULL;
    HFONT titleFont = NULL;
};

// Stateless renderer for all in-game and edit-mode overlay graphics
class HudRenderer {
public:
    static const int FLOAT_BALL_RADIUS = 19;

    static void paintDemoBackdrop(HDC hdc, int w, int h, const HudFonts& fonts);
    static void paintFloatingBall(HDC hdc, int w, int h, const Profile& profile,
                                  bool editMode, const HudFonts& fonts);
    static void paintPlayStatusBar(HDC hdc, int w, const Profile& profile,
                                   const EngineTelemetry& tel, const HudFonts& fonts);
    static void paintKeymapNodes(HDC hdc, int w, int h, const Profile& profile,
                                 const EngineTelemetry& tel, bool editMode, bool demoMode,
                                 int selectedBtnIndex, bool waitingButtonRebind,
                                 const HudFonts& fonts);
    static void paintRebindPrompt(HDC hdc, int w, int h, const std::string& targetName,
                                  bool specialAimKey, const HudFonts& fonts);
    static void paintToast(HDC hdc, int w, int h, const std::string& msg, const HudFonts& fonts);
};
