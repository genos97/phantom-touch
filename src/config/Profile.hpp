#pragma once

#include "config/KeyCodes.hpp"

#include <cstdint>
#include <string>
#include <vector>

enum FireMode : int {
    FIRE_MODE_DRAG_RIGHT = 0, // Clean 1-frame handoff onto Right Fire button + vertical drag
    FIRE_MODE_CLAW_LEFT  = 1, // Separate Left Fire button while Aim finger drags camera continuously
    FIRE_MODE_DUAL_DRAG  = 2  // Keeps Aim + Right Fire both moving together
};

enum KeyActionType : int {
    NODE_PRESS          = 0, // Hold touch down while key is held
    NODE_CLICK          = 1, // Fixed-duration single tap on press (durationMs)
    NODE_DOWN_UP        = 2, // Tap on key press + tap again on release (Hold-to-ADS Scope)
    NODE_UP_TRIGGER     = 3, // Fixed-duration tap on key release
    NODE_HOLD_VIEW      = 4, // Pushes a temporary drag-look view at (x,y) while held (Free Look Eye)
    NODE_SWIPE          = 5, // Smooth directional swipe from (x,y) to (x2,y2) over durationMs
    NODE_CURSOR_TOGGLE  = 6, // Taps (x,y) and toggles Aim <-> Cursor mode (Bag/Map)
    NODE_RESET_VIEW     = 7, // Resets active camera finger cleanly without tapping
    NODE_SWITCH_SENS    = 8, // Switches to secondary Scope X/Y sensitivity (hold or toggle)
    NODE_REACTIVATE_JOY = 9  // Releases and cleanly re-engages WASD joystick
};

enum ProfileTemplateKind : int {
    TMPL_DESKTOP          = 0,
    TMPL_CONTROLLER_BASIC = 1,
    TMPL_MOBA             = 2,
    TMPL_STRATEGY_RTS     = 3,
    TMPL_RACING           = 4,
    TMPL_PLATFORMER       = 5
};

struct KeyNode {
    std::string label;
    int comboVk = 0;                 // Optional 2-key prefix (0 = single key, e.g. LALT for LALT+1)
    int vk = 0;                      // Primary trigger key
    KeyActionType action = NODE_PRESS;
    float x = 0.5f;                  // Normalized [0.0, 1.0]
    float y = 0.5f;
    float x2 = 0.5f;                 // End coordinate for NODE_SWIPE
    float y2 = 0.5f;
    int durationMs = 35;             // Tap or swipe duration
    float radius = 0.030f;           // Visual badge radius
    bool switchScopeSens = false;    // Also toggles/activates scopeSensX/Y
    bool toggleMode = false;         // For NODE_SWITCH_SENS: false = hold, true = toggle
    bool resetView = false;          // Optional add-on: also cleanly resets main camera finger
    bool reactivateJoy = false;      // Optional add-on: cleanly lifts & re-engages WASD joystick
};

struct Profile {
    std::string name = "Free Fire MAX (S21 FE)";
    std::string appPackage = "com.dts.freefiremax,com.dts.freefireth"; // Optional comma-separated Android package(s) for auto-switch
    bool keymapEnabled = true;       // false = Desktop Typing Mode (all PC keys type directly into Android!)
    bool edgeAnchored = false;       // Stage 3: anchor left/right HUD nodes relative to screen edges (20:9 <-> 16:9 survival)
    float refAspect = 2.1667f;       // Reference screen aspect ratio (W/H) where profile was calibrated (2340/1080)
    std::string scrcpyArgs = "--max-fps=90 --video-bit-rate=16M";
    int controlMaxSize = 0;          // 0 = native phone resolution
    int pollHz = 250;                // 125 / 250 / 500 Hz coalescing (0 = MAX 1000 Hz)

    int toggleAimVk = 0xC0;          // VK_OEM_3 (`~`); F1 also toggles as backup
    int holdUnlockVk = 0;            // Optional Hold-to-Unlock cursor key (0 = off)
    int resetTouchVk = 0x74;         // VK_F5 (quick view + touch reset)
    bool showHudInPlay = true;
    float hudOpacity = 0.75f;
    float floatBtnX = 0.032f;
    float floatBtnY = 0.280f;
    float panelX = 0.260f;
    float panelY = 0.100f;
    bool rememberCursorPos = false;  // false = spawn cursor at center on unlock; true = restore last position
    int cursorScrollStepPx = 95;     // Vertical touch-drag distance per wheel notch in Cursor Mode

    // Aim view & Free Fire counter-acceleration physics
    float aimX = 0.620f;
    float aimY = 0.700f;
    float aimHalfW = 0.160f;
    float aimHalfH = 0.640f;

    bool unboundedAim = true;
    bool invertY = false;
    float ffCounterAccel = 7.0f;
    float sensX = 0.95f;
    float sensY = 1.15f;
    float fireSensX = 0.80f;
    float fireSensY = 1.25f;
    float scopeSensX = 0.65f;
    float scopeSensY = 0.75f;
    float accelPower = 1.00f;
    float accelRefSpeed = 14.0f;
    float accelCap = 2.00f;
    float smoothing = 0.10f;
    int idleResetMs = 700;
    int touchDownDelayMs = 15;
    int touchUpDelayMs = 0;
    int tapJitterPx = 0;

    // Fire button settings
    int fireMode = FIRE_MODE_CLAW_LEFT;
    int fireVk = 0x01;               // VK_LBUTTON
    float fireX = 0.851f;
    float fireY = 0.728f;
    float fireBoxHalfW = 0.350f;
    float fireBoxHalfH = 0.720f;
    float leftFireX = 0.236f;
    float leftFireY = 0.428f;

    // WASD Virtual Joystick
    float joyX = 0.162f;
    float joyY = 0.750f;
    float joyRadius = 0.085f;
    float joySprintRadius = 0.160f;
    float joyWalkRadius = 0.042f;
    bool joySteppedGlide = true;
    int vkUp = 'W';
    int vkDown = 'S';
    int vkLeft = 'A';
    int vkRight = 'D';
    int vkSprint = 0xA0;             // VK_LSHIFT
    int vkWalk = 0xA2;               // VK_LCONTROL

    std::vector<KeyNode> buttons;
};

class ProfileDefaults {
public:
    static Profile createFreeFire();
    static Profile createBlank(const std::string& name);
    static Profile createDesktop(const std::string& name = "Desktop (Typing)");
    static Profile createTemplate(ProfileTemplateKind kind);
    static const char* templateName(ProfileTemplateKind kind);
    static bool matchesPackage(const std::string& packageList, const std::string& activePkg);
    static void mapEdgeAnchored(float nx, float ny, bool edgeAnchored,
                                float refAspect, float liveAspect,
                                float& outX, float& outY);
    static void unmapEdgeAnchored(float liveX, float liveY, bool edgeAnchored,
                                  float refAspect, float liveAspect,
                                  float& outX, float& outY);
    static Profile resolveLayout(const Profile& src, uint32_t screenW, uint32_t screenH);
    static std::string actionTypeToName(KeyActionType action);
    static std::string actionTypeToHumanLabel(KeyActionType action);
    static KeyActionType nameToActionType(const std::string& raw);
};
