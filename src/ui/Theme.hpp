#pragma once

#include <cstdint>

namespace Theme {

// Base layout metrics at 96 DPI (scale at runtime via Metrics::dp / GdiPlusDraw::dp)
static const int TOPBAR_H        = 40;
static const int RAIL_W          = 54;
static const int DRAWER_W        = 356;
static const int RESIZE_BORDER   = 6;
static const int MIN_WIN_W       = 960;
static const int MIN_WIN_H       = 600;

// Spacing tokens v2 (base 4 at 96 DPI)
static const int SP_1            = 4;
static const int SP_2            = 8;
static const int SP_3            = 12;
static const int SP_4            = 16;
static const int SP_6            = 24;
static const int SP_8            = 32;

// Corner radius tokens
static const float R_SM          = 6.0f;
static const float R_MD          = 10.0f;
static const float R_LG          = 14.0f;
static const float R_PILL        = 999.0f;

// Six-step type scale (px at 96 DPI)
static const float FONT_CAPTION  = 11.0f;
static const float FONT_BODY     = 13.0f;
static const float FONT_LABEL    = 13.0f;
static const float FONT_TITLE    = 15.0f;
static const float FONT_HEADLINE = 18.0f;
static const float FONT_DISPLAY  = 24.0f;

// ARGB Color Tokens (0xAARRGGBB for GDI+, contrast-fixed >= 4.5:1 text / >= 1.29:1 card surface)
static const uint32_t BG_CANVAS      = 0xFF0A0D13u; // Pure dark letterbox viewport matte
static const uint32_t BG_SHELL       = 0xFF10151Fu; // TopBar & SideRail obsidian chrome
static const uint32_t BG_DRAWER      = 0xFF131924u; // Docked Studio Drawer background
static const uint32_t BG_CARD        = 0xFF232E42u; // Elevated control cards (U1 contrast fix)
static const uint32_t BG_CARD_HOVER  = 0xFF2B3850u; // Hover state
static const uint32_t BG_INPUT       = 0xFF0C1018u; // Text input recessed background
static const uint32_t BG_TRACK       = 0xFF2E3B54u; // Slider & toggle track

static const uint32_t BORDER_SUBTLE  = 0xFF2E3B54u;
static const uint32_t BORDER_STRONG  = 0xFF3A4D6Fu;
static const uint32_t BORDER_CHROME  = 0xFF1C2638u;

static const uint32_t ACCENT_CYAN    = 0xFF00D4FFu; // Signature Phantom Touch electric cyan
static const uint32_t ACCENT_CYAN_BG = 0xFF0B3B52u;
static const uint32_t ACCENT_GREEN   = 0xFF2CE59Bu; // Active Aim / Connected / Saved
static const uint32_t ACCENT_GREEN_BG= 0xFF11382Bu;
static const uint32_t ACCENT_AMBER   = 0xFFFFB84Du; // Cursor mode / Swipe / Warning
static const uint32_t ACCENT_AMBER_BG= 0xFF3D2E14u;
static const uint32_t ACCENT_PURPLE  = 0xFFB88CFFu; // Scope / Cursor Toggle / Polling
static const uint32_t ACCENT_PURPLE_BG=0xFF2C1F47u;
static const uint32_t ACCENT_RED     = 0xFFFF5C6Cu; // Delete / Close / Danger
static const uint32_t ACCENT_RED_BG  = 0xFF421A22u;

// State layer overlays
static const uint32_t STATE_HOVER    = 0x0DFFFFFFu; // white 5%
static const uint32_t STATE_PRESSED  = 0x17FFFFFFu; // white 9%
static const uint32_t STATE_SELECTED = 0x2400D4FFu; // accent 14%

// Semantic aliases
static const uint32_t SEM_SUCCESS    = ACCENT_GREEN;
static const uint32_t SEM_WARNING    = ACCENT_AMBER;
static const uint32_t SEM_DANGER     = ACCENT_RED;
static const uint32_t SEM_INFO       = ACCENT_CYAN;

// HUD overlay tokens
static const uint32_t HUD_TAP        = ACCENT_CYAN;
static const uint32_t HUD_HOLD       = ACCENT_GREEN;
static const uint32_t HUD_SWIPE      = ACCENT_PURPLE;
static const uint32_t HUD_TOGGLE     = ACCENT_AMBER;
static const uint32_t HUD_FIRE       = 0xFFFF8A3Du; // Orange (distinct from danger red)
static const uint32_t HUD_CAST       = 0xFFE36BFFu;

static const uint32_t TEXT_PRIMARY   = 0xFFF0F5FFu;
static const uint32_t TEXT_SECONDARY = 0xFFA6B6CEu;
static const uint32_t TEXT_MUTED     = 0xFF8B9BB4u; // WCAG AA >= 4.7:1 on BG_CARD

struct Metrics {
    int dpi = 96;

    int dp(int px) const {
        return (dpi <= 96) ? px : ((px * dpi + 48) / 96);
    }
    float dpf(float px) const {
        return (dpi <= 96) ? px : (px * static_cast<float>(dpi) / 96.0f);
    }
    int topBarH() const      { return dp(TOPBAR_H); }
    int railW() const        { return dp(RAIL_W); }
    int drawerW() const      { return dp(DRAWER_W); }
    int resizeBorder() const { return dp(RESIZE_BORDER); }
    int minWinW() const      { return dp(MIN_WIN_W); }
    int minWinH() const      { return dp(MIN_WIN_H); }
};

} // namespace Theme
