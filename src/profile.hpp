#pragma once

#include <string>
#include <vector>

enum FireMode : int {
    FIRE_MODE_DRAG_RIGHT = 0, // Clean 1-frame handoff onto Right Fire button + full-screen vertical drag
    FIRE_MODE_CLAW_LEFT  = 1, // Separate Left Fire button while Aim finger drags camera continuously
    FIRE_MODE_DUAL_DRAG  = 2  // Keeps Aim + Right Fire both moving together
};

enum KeyActionType : int {
    NODE_PRESS          = 0, // Hold touch down while key is held
    NODE_CLICK          = 1, // Fixed-duration single tap on press (durationMs)
    NODE_DOWN_UP        = 2, // Tap on key press + tap again on release (e.g. Hold-to-ADS Scope!)
    NODE_UP_TRIGGER     = 3, // Fixed-duration tap on key release
    NODE_HOLD_VIEW      = 4, // Pushes a temporary drag-look view at (x,y) while held (e.g. Free Look Eye)
    NODE_SWIPE          = 5, // Smooth directional swipe from (x,y) to (x2,y2) over durationMs
    NODE_CURSOR_TOGGLE  = 6, // Taps (x,y) and toggles Aim <-> Cursor mode (e.g. Bag/Map)
    NODE_RESET_VIEW     = 7, // Resets active camera finger cleanly without tapping
    NODE_SWITCH_SENS    = 8, // Switches to secondary Scope X/Y sensitivity (hold or toggle)
    NODE_REACTIVATE_JOY = 9  // Releases and cleanly re-engages WASD joystick
};

// Custom virtual key codes beyond Win32 VK_* (0x01..0xFE)
static const int VK_CUSTOM_MWHEEL_UP   = 0x101;
static const int VK_CUSTOM_MWHEEL_DOWN = 0x102;

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
    bool switchScopeSens = false;    // If true (e.g. SCOPE button), also activates scopeSensX/Y!
    bool toggleMode = false;         // For NODE_SWITCH_SENS: false = hold, true = toggle
};

struct Profile {
    std::string name = "Free Fire MAX (S21 FE)";
    std::string scrcpyArgs = "--max-fps=90 --video-bit-rate=16M";
    int controlMaxSize = 0;          // 0 = native phone resolution (1:1 pixel accuracy)
    int pollHz = 250;                // 125 / 250 / 500 Hz mouse coalescing (0 = raw 1000 Hz)

    int toggleAimVk = 0xC0;          // VK_OEM_3 (`~`); F1 also toggles as backup
    int holdUnlockVk = 0;            // Optional Hold-to-Unlock cursor key (0 = off, e.g. LALT)
    int resetTouchVk = 0x74;         // VK_F5 (quick view + touch reset)
    bool showHudInPlay = true;
    float hudOpacity = 0.75f;        // 0.15..1.00 HUD badge visibility
    float floatBtnX = 0.032f;        // Draggable floating config ball X (normalized)
    float floatBtnY = 0.280f;        // Draggable floating config ball Y (normalized)
    float panelX = 0.260f;           // Draggable config panel top-left X
    float panelY = 0.100f;           // Draggable config panel top-left Y

    // Aim view & Free Fire counter-acceleration physics
    float aimX = 0.620f;
    float aimY = 0.700f;
    float aimHalfW = 0.160f;
    float aimHalfH = 0.640f;

    bool unboundedAim = true;        // Infinite virtual move coords (zero mid-fight edge resets!)
    bool invertY = false;
    float ffCounterAccel = 7.0f;     // Free Fire horizontal counter-acceleration (0.0 = off, 7.0 = FF tuned)
    float sensX = 0.95f;             // Base horizontal sensitivity (pixels per raw mouse count)
    float sensY = 1.15f;             // Base vertical sensitivity
    float fireSensX = 0.80f;         // Multiplier on X while firing
    float fireSensY = 1.25f;         // Multiplier on Y while firing
    float scopeSensX = 0.65f;        // Multiplier on X while scoped (ADS)
    float scopeSensY = 0.75f;        // Multiplier on Y while scoped (ADS)
    float accelPower = 1.00f;        // 1.0 = pure linear, >1.0 = dynamic flick curve
    float accelRefSpeed = 14.0f;
    float accelCap = 2.00f;
    float smoothing = 0.10f;         // 0.0 = raw instant, 0.05..0.5 = anti-jitter
    int idleResetMs = 700;           // Periodic auto-release after idle (only used in boxed mode)
    int touchDownDelayMs = 15;       // Minimum clean gap (ms) before re-placing aim finger
    int touchUpDelayMs = 0;          // Optional delay before lifting aim finger on edge-release
    int tapJitterPx = 4;             // Subtle anti-overlap pixel jitter on button taps (0 = exact)

    // Fire button settings
    int fireMode = FIRE_MODE_CLAW_LEFT; // 0=DRAG-FIRE, 1=CLAW-LEFT, 2=DUAL-DRAG
    int fireVk = 0x01;                  // VK_LBUTTON
    float fireX = 0.851f;               // Right Fire button
    float fireY = 0.728f;
    float fireBoxHalfW = 0.350f;
    float fireBoxHalfH = 0.720f;
    float leftFireX = 0.236f;           // Left Fire button (CLAW-LEFT mode)
    float leftFireY = 0.428f;

    // WASD Virtual Joystick (with Free Fire stepped glide + sprint/walk radius switch)
    float joyX = 0.162f;
    float joyY = 0.750f;
    float joyRadius = 0.085f;
    float joySprintRadius = 0.160f;
    float joyWalkRadius = 0.042f;
    bool joySteppedGlide = true;        // 3-stage smooth glide (8ms -> 20% -> 50% -> 100%)
    int vkUp = 'W';
    int vkDown = 'S';
    int vkLeft = 'A';
    int vkRight = 'D';
    int vkSprint = 0xA0;                // VK_LSHIFT
    int vkWalk = 0xA2;                  // VK_LCONTROL

    std::vector<KeyNode> buttons;
};

Profile createDefaultFreeFireProfile();
Profile createBlankProfile(const std::string& name);
bool loadProfile(const std::string& path, Profile& outProfile);
bool saveProfile(const std::string& path, const Profile& profile);

// Scans profilesDir for *.cfg files (returns full paths sorted alphabetically)
std::vector<std::string> listProfileFiles(const std::string& profilesDir);
std::string sanitizeProfileFilename(const std::string& name);
std::string loadLastActiveProfilePath(const std::string& profilesDir);
void saveLastActiveProfilePath(const std::string& profilesDir, const std::string& fullPath);

std::string vkToName(int vk);
int nameToVk(const std::string& name);
std::string comboToName(int comboVk, int vk);
void parseComboName(const std::string& spec, int& outComboVk, int& outVk);
std::string actionTypeToName(KeyActionType a);
KeyActionType nameToActionType(const std::string& s);
