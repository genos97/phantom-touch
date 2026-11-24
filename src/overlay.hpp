#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>

#include "aim_engine.hpp"
#include "config_ui.hpp"
#include "profile.hpp"
#include "protocol.hpp"

#include <string>
#include <vector>

class OverlayApp {
public:
    OverlayApp(ScrcpyBridge* bridge, AimEngine* engine,
               const std::string& profilesDir, const std::string& profilePath,
               const std::string& mirrorTitle, bool demoMode);
    ~OverlayApp();

    bool create(HINSTANCE hInst);
    int runMessageLoop();

    // Global hook callbacks (forward to singleton instance)
    LRESULT handleLowLevelMouse(int nCode, WPARAM wParam, LPARAM lParam);
    LRESULT handleLowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam);

private:
    enum DragTargetType {
        DRAG_NONE = 0,
        DRAG_JOY,
        DRAG_AIM,
        DRAG_FIRE,
        DRAG_LEFT_FIRE,
        DRAG_FLOAT_BALL,
        DRAG_BUTTON,
        DRAG_SWIPE_END
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void registerRawInput();
    void installHooks();
    void removeHooks();

    void setAimMode(bool enabled);
    void setEditMode(bool enabled);
    void toggleFireMode();
    void toggleHud();
    void triggerQuickReset();
    void saveCurrentProfile();
    void applyHudOpacity();
    void handleConfigUIResult(const ConfigUIResult& res);

    void syncToMirrorWindow();
    void updateClipCursor();
    bool isPointInMirror(POINT screenPt, float& outNormX, float& outNormY) const;
    bool isPointInFloatBallScreen(POINT screenPt) const;
    bool isMirrorOrOverlayForeground() const;

    void onPaint(HDC hdc);
    void paintDemoBackdrop(HDC hdc, int w, int h);
    void paintPlayStatusBar(HDC hdc, int w);
    void paintFloatingBall(HDC hdc, int w, int h, bool editMode);
    void paintKeymapNodes(HDC hdc, int w, int h, bool editMode);

    enum RebindTarget {
        REBIND_NONE = 0,
        REBIND_BUTTON,
        REBIND_FIRE_KEY,
        REBIND_TOGGLE_AIM
    };

    // Edit mode mouse/key interaction
    void onEditMouseDown(int x, int y, bool rightButton);
    void onEditDoubleClick(int x, int y);
    bool onEditMouseBind(int mouseVk);
    void onEditMouseMove(int x, int y);
    void onEditMouseUp();
    void onEditMouseWheel(int x, int y, int delta);
    bool onEditKeyDown(int vk);
    void paintRebindPrompt(HDC hdc, int w, int h);

    DragTargetType hitTestNode(int px, int py, int& outBtnIndex) const;
    void applyProfileToEngine();
    void showToast(const std::string& msg, DWORD durationMs = 2000);

    ScrcpyBridge* bridge_;
    AimEngine* engine_;
    std::string profilesDir_;
    std::string profilePath_;
    std::string mirrorTitle_;
    bool demoMode_ = false;

    HINSTANCE hInst_ = NULL;
    HWND hwnd_ = NULL;
    HWND mirrorHwnd_ = NULL;
    bool sawMirrorWindow_ = false;
    RECT mirrorClientScreenRect_; // Screen coordinates of target viewport

    HHOOK mouseHook_ = NULL;
    HHOOK keyHook_ = NULL;

    Profile profile_;
    ConfigPanelUI configUI_;

    bool aimMode_ = false;
    bool holdUnlockedAim_ = false;
    bool toggleAimKeyDown_ = false;
    bool hookKeyDown_[256];
    bool editMode_ = false;
    bool hadForeground_ = false;
    bool cursorHidden_ = false;
    bool cursorDirectDragging_ = false;

    // Floating config ball drag/click state in Cursor Mode
    bool floatBallHookPressed_ = false;
    bool floatBallHookMoved_ = false;
    POINT floatBallPressPt_;

    // Edit mode interactive state
    DragTargetType dragging_ = DRAG_NONE;
    int dragBtnIndex_ = -1;
    int selectedBtnIndex_ = -1;
    RebindTarget rebindTarget_ = REBIND_NONE;
    int addNodeStagger_ = 0;

    // Toast feedback banner
    std::string toastMsg_;
    DWORD toastExpireTick_ = 0;
    bool toastWasVisible_ = false;
    DWORD lastPlayPaintTick_ = 0;

    // Persistent cached offscreen backbuffer (eliminates 60Hz 8MB GDI allocations on the input hook thread!)
    HDC cacheDC_ = NULL;
    HBITMAP cacheBmp_ = NULL;
    HGDIOBJ cacheOldBmp_ = NULL;
    int cacheW_ = 0;
    int cacheH_ = 0;

    HFONT fontSmall_ = NULL;
    HFONT fontBold_ = NULL;
    HFONT fontTitle_ = NULL;
};
