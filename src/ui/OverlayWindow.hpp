#pragma once

#include "bridge/ScrcpyBridge.hpp"
#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "ui/HudRenderer.hpp"
#include "ui/StudioPanel.hpp"

#include <string>

// Transparent Win32 topmost overlay pinned to the mirror window:
//   - Captures Raw Mouse Input (WM_INPUT) + global WH_MOUSE_LL / WH_KEYBOARD_LL hooks
//   - Coordinates Play Mode (Aim Lock / Cursor Mode) and F2 Edit Mode (Keymap Studio)
//   - Delegates HUD drawing to HudRenderer and panel controls to StudioPanel
class OverlayWindow {
public:
    OverlayWindow(ScrcpyBridge* bridge, InputEngine* engine,
                  const std::string& profilesDir, const std::string& profilePath,
                  const std::string& mirrorTitle, bool demoMode);
    ~OverlayWindow();

    bool create(HINSTANCE hInst);
    int runMessageLoop();

    LRESULT handleLowLevelMouse(int nCode, WPARAM wParam, LPARAM lParam);
    LRESULT handleLowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam);

private:
    enum DragTargetType {
        DRAG_NONE = 0,
        DRAG_JOY,
        DRAG_AIM,
        DRAG_FIRE,
        DRAG_LEFT_FIRE,
        DRAG_BUTTON,
        DRAG_SWIPE_END,
        DRAG_FLOAT_BALL
    };

    enum RebindTarget {
        REBIND_NONE = 0,
        REBIND_BUTTON,
        REBIND_FIRE_KEY,
        REBIND_TOGGLE_AIM
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void registerRawInput();
    void installHooks();
    void removeHooks();
    void syncToMirrorWindow();
    void updateClipCursor();
    void applyHudOpacity();

    void setAimMode(bool enabled);
    void setEditMode(bool enabled);
    void toggleFireMode();
    void toggleHud();
    void triggerQuickReset();
    void saveCurrentProfile();
    void handleStudioResult(const StudioUIResult& res);

    bool isMirrorOrOverlayForeground() const;
    bool isPointInMirror(POINT screenPt, float& outNormX, float& outNormY) const;
    bool isPointInFloatBallScreen(POINT screenPt) const;

    void onPaint(HDC hdc);
    void onEditMouseDown(int x, int y, bool rightButton);
    void onEditDoubleClick(int x, int y);
    bool onEditMouseBind(int mouseVk);
    void onEditMouseMove(int x, int y);
    void onEditMouseUp();
    void onEditMouseWheel(int x, int y, int delta);
    bool onEditKeyDown(int vk);

    DragTargetType hitTestNode(int px, int py, int& outBtnIndex) const;
    void applyProfileToEngine();
    void showToast(const std::string& msg, DWORD durationMs = 2000);

    ScrcpyBridge* bridge_;
    InputEngine* engine_;
    std::string profilesDir_;
    std::string profilePath_;
    std::string mirrorTitle_;
    bool demoMode_ = false;

    HINSTANCE hInst_ = NULL;
    HWND hwnd_ = NULL;
    HWND mirrorHwnd_ = NULL;
    bool sawMirrorWindow_ = false;
    RECT mirrorClientScreenRect_;

    HHOOK mouseHook_ = NULL;
    HHOOK keyHook_ = NULL;

    Profile profile_;
    StudioPanel studioPanel_;

    bool aimMode_ = false;
    bool holdUnlockedAim_ = false;
    bool toggleAimKeyDown_ = false;
    bool hookKeyDown_[256];
    bool editMode_ = false;
    bool hadForeground_ = false;
    bool cursorHidden_ = false;
    bool cursorDirectDragging_ = false;
    POINT lastUnlockedCursorPt_;
    bool hasLastUnlockedCursorPt_ = false;

    bool floatBallHookPressed_ = false;
    bool floatBallHookMoved_ = false;
    POINT floatBallPressPt_;

    DragTargetType dragging_ = DRAG_NONE;
    int dragBtnIndex_ = -1;
    int selectedBtnIndex_ = -1;
    RebindTarget rebindTarget_ = REBIND_NONE;
    int addNodeStagger_ = 0;

    std::string toastMsg_;
    DWORD toastExpireTick_ = 0;
    bool toastWasVisible_ = false;
    DWORD lastPlayPaintTick_ = 0;

    HDC cacheDC_ = NULL;
    HBITMAP cacheBmp_ = NULL;
    HGDIOBJ cacheOldBmp_ = NULL;
    int cacheW_ = 0;
    int cacheH_ = 0;

    HudFonts fonts_;
};
