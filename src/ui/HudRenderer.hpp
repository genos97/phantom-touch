#pragma once

#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "ui/EditorState.hpp"
#include "ui/GdiPlusDraw.hpp"

#include <string>
#include <vector>

struct HudEditExtras {
    const std::vector<int>* multiSelected;
    const std::vector<AlignGuide>* alignGuides;
    const std::vector<KeyConflict>* conflicts;
    bool marqueeActive;
    RECT marqueeRc;
    bool skipDynamicDots;
    float modePulseAlpha;
    bool modePulseAim;

    HudEditExtras()
        : multiSelected(nullptr),
          alignGuides(nullptr),
          conflicts(nullptr),
          marqueeActive(false),
          skipDynamicDots(false),
          modePulseAlpha(0.0f),
          modePulseAim(false) {
        ZeroMemory(&marqueeRc, sizeof(marqueeRc));
    }
};

enum HudToastKind {
    TOAST_INFO    = 0,
    TOAST_SUCCESS = 1,
    TOAST_WARNING = 2,
    TOAST_ERROR   = 3
};

// Clean, minimalist emulator-style GDI+ HUD renderer (BlueStacks 5 / MuMu style).
// Renders directly into a 32-bit premultiplied ARGB surface for silky per-pixel glass transparency.
class HudRenderer {
public:
    // Formats a key binding into a compact, clean emulator keycap string (e.g. LSHIFT -> SHIFT, MWHEELUP -> SCR+)
    static std::string shortKeyLabel(int comboVk, int vk);

    // Computes the exact on-canvas bounding box of an action node pill (shared by paint + hit-testing)
    static void computeNodePillSize(const std::string& keyText, bool editMode, float& outW, float& outH);

    // Computes the center (outX, outY) and radius of the quick-delete 'x' chip on a selected node in Edit Mode
    static void computeDeleteChipPos(float nodeCx, float nodeCy, float pillW, float pillH,
                                     float& outChipX, float& outChipY, float& outChipR);

    static void paintKeymapNodes(Gdiplus::Graphics& g, int w, int h,
                                 const Profile& profile, const EngineTelemetry& tel,
                                 bool editMode, bool demoMode,
                                 int selectedBtnIndex, int hoveredBtnIndex,
                                 bool hoverDeleteChip, bool waitingButtonRebind,
                                 float playOpacity,
                                 const HudEditExtras* extras = nullptr);

    static void paintCrosshairDot(Gdiplus::Graphics& g, int w, int h);

    static void paintFullscreenStatus(Gdiplus::Graphics& g, int w,
                                      const Profile& profile, const EngineTelemetry& tel);

    static void paintRebindPrompt(Gdiplus::Graphics& g, int w, int h,
                                  const std::string& targetName, bool specialAimKey);

    static void paintToast(Gdiplus::Graphics& g, int w, int h, const std::string& msg);
    static void paintToastWithAction(Gdiplus::Graphics& g, int w, int h,
                                     const std::string& msg, const std::string& actionLabel,
                                     RECT* outActionRc = nullptr, int toastKind = TOAST_INFO);
};
