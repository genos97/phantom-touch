#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class InputEngine;

enum ShellAppMsg {
    MSG_TOGGLE_AIM         = WM_APP + 1,
    MSG_UNLOCK_AIM         = WM_APP + 2,
    MSG_TOGGLE_STUDIO      = WM_APP + 3,
    MSG_CYCLE_FIRE         = WM_APP + 4,
    MSG_TOGGLE_HUD         = WM_APP + 5,
    MSG_RESET_TOUCH        = WM_APP + 6,
    MSG_LOCK_AIM           = WM_APP + 7,
    MSG_TOGGLE_FULLSCREEN  = WM_APP + 8,
    MSG_CONNECT_DONE       = WM_APP + 9,
    MSG_SENS_X_DOWN        = WM_APP + 10,
    MSG_SENS_X_UP          = WM_APP + 11,
    MSG_SENS_Y_DOWN        = WM_APP + 12,
    MSG_SENS_Y_UP          = WM_APP + 13,
    MSG_WIRELESS_DONE      = WM_APP + 14,
    MSG_DEVICE_CLIPBOARD   = WM_APP + 15,
    MSG_FOREGROUND_APP     = WM_APP + 16,
    MSG_DROP_TOAST         = WM_APP + 17,
    MSG_ANDROID_BACK       = WM_APP + 18,
    MSG_QR_PAIR_UPDATE     = WM_APP + 20,
    MSG_DEVICE_LOST        = WM_APP + 21,
    MSG_VIDEO_FALLBACK     = WM_APP + 22,
    MSG_TOGGLE_DEBUG_HUD   = WM_APP + 23,
    MSG_RUN_LATENCY_PROBE  = WM_APP + 24,
    MSG_INJECT_VIDEO_STALL = WM_APP + 25,
    MSG_TOOLS_READY        = WM_APP + 26,
    MSG_DEVICES_REFRESHED  = WM_APP + 27,
    MSG_DISCONNECT_DONE    = WM_APP + 28,
    MSG_SCREENSHOT_DONE    = WM_APP + 29,
    MSG_PROBE_UPDATE       = WM_APP + 30,
    MSG_THERMAL_UPDATE     = WM_APP + 31,
    MSG_CONNECT_STEP       = WM_APP + 32,
    MSG_REPAIR_TOOLS_DONE  = WM_APP + 33,
    MSG_TOGGLE_CMD_PALETTE = WM_APP + 34,
    MSG_TOGGLE_CHEAT_SHEET = WM_APP + 35,
    MSG_PAD_NAV            = WM_APP + 36
};

// Lockless/microsecond snapshot pushed from ShellWindow to the dedicated input thread
// so hooks and WM_INPUT never touch Win32 UI state or block on UI locks (LATENCY_PLAN 1.2).
struct InputHookSnapshot {
    HWND shellHwnd = NULL;
    HWND hudHwnd = NULL;
    HWND viewportHwnd = NULL;
    HWND embeddedHwnd = NULL;
    RECT videoScreenRect = { 0, 0, 0, 0 };

    bool connected = false;
    bool demoMode = false;
    bool aimMode = false;
    bool editMode = false;
    bool nativeVideoActive = false;
    bool uiCapturingText = false;
    bool popupOpen = false;
    bool waitingSpecialRebind = false;
    bool keymapEnabled = false;
    bool rightClickBack = true;

    int toggleAimVk = 0x70;    // VK_F1
    int holdUnlockVk = 0xC0;   // '~'
    int resetTouchVk = 0x74;   // VK_F5
};

class InputHookThread {
public:
    InputHookThread();
    ~InputHookThread();

    bool start(HINSTANCE hInst, InputEngine* engine, const InputHookSnapshot& initialSnap);
    void stop();
    bool isRunning() const { return thread_ != NULL; }
    void updateSnapshot(const InputHookSnapshot& snap);

private:
    static DWORD WINAPI threadProc(LPVOID param);
    static LRESULT CALLBACK rawWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK lowLevelKeyProc(int nCode, WPARAM wParam, LPARAM lParam);

    void runLoop();
    void handleRawInput(WPARAM wParam, LPARAM lParam);
    LRESULT handleMouseHook(int nCode, WPARAM wParam, LPARAM lParam);
    LRESULT handleKeyHook(int nCode, WPARAM wParam, LPARAM lParam);
    void readSnapshot(InputHookSnapshot& out) const;

    mutable CRITICAL_SECTION snapLock_;
    InputHookSnapshot snap_;

    HINSTANCE hInst_ = NULL;
    InputEngine* engine_ = nullptr;
    HANDLE thread_ = NULL;
    HANDLE readyEvent_ = NULL;
    DWORD threadId_ = 0;

    HWND rawHwnd_ = NULL;
    HHOOK mouseHook_ = NULL;
    HHOOK keyHook_ = NULL;

    // Thread-local hook state (accessed ONLY on the dedicated input thread)
    bool hookKeyDown_[256];
    bool toggleAimKeyDown_ = false;
    bool holdUnlockedAim_ = false;
    bool cursorDirectDragging_ = false;
    bool rightClickBackSwallowUp_ = false;
    bool middlePanDragging_ = false;
    int wheelDeltaAccum_ = 0;
    DWORD lastWheelTick_ = 0;
};
