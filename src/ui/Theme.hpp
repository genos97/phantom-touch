#pragma once

#include <cstdint>

namespace Theme {

// Layout metrics (in pixels)
static const int TOPBAR_H        = 40;
static const int RAIL_W          = 54;
static const int DRAWER_W        = 356;
static const int RESIZE_BORDER   = 6;
static const int MIN_WIN_W       = 960;
static const int MIN_WIN_H       = 600;

// ARGB Color Tokens (0xAARRGGBB for GDI+)
static const uint32_t BG_CANVAS      = 0xFF0A0D13u; // Pure dark letterbox viewport matte
static const uint32_t BG_SHELL       = 0xFF10151Fu; // TopBar & SideRail obsidian chrome
static const uint32_t BG_DRAWER      = 0xFF131924u; // Docked Studio Drawer background
static const uint32_t BG_CARD        = 0xFF1A2232u; // Elevated control cards
static const uint32_t BG_CARD_HOVER  = 0xFF222C40u; // Hover state
static const uint32_t BG_INPUT       = 0xFF0C1018u; // Text input recessed background
static const uint32_t BG_TRACK       = 0xFF232E42u; // Slider & toggle track

static const uint32_t BORDER_SUBTLE  = 0xFF232E42u;
static const uint32_t BORDER_STRONG  = 0xFF344563u;
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

static const uint32_t TEXT_PRIMARY   = 0xFFF0F5FFu;
static const uint32_t TEXT_SECONDARY = 0xFFA6B6CEu;
static const uint32_t TEXT_MUTED     = 0xFF6B7D99u;

} // namespace Theme
