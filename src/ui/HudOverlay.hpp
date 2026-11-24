#pragma once

#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "ui/HudRenderer.hpp"

#include <string>

enum HudRebindTarget {
    HUD_REBIND_NONE = 0,
    HUD_REBIND_BUTTON,
    HUD_REBIND_FIRE_KEY,
    HUD_REBIND_TOGGLE_AIM
};

// Callback interface implemented by ShellWindow to stay synchronized with HudOverlay
struct IHudOverlayHost {
    virtual ~IHudOverlayHost() {}
    virtual Profile& hudProfile() = 0;
    virtual EngineTelemetry hudTelemetry() const = 0;
    virtual bool hudIsEditMode() const = 0;
    virtual bool hudIsDemoMode() const = 0;
    virtual bool hudIsAimMode() const = 0;
    virtual bool hudIsFullscreen() const = 0;
    virtual bool hudIsSpecialDrawerRebind() const = 0;
    virtual std::string hudSpecialRebindLabel() const = 0;
    virtual bool hudIsSpecialAimRebind() const = 0;

    virtual void hudOnSelectionChanged(int newSelectedIdx) = 0;
    virtual void hudOnProfileChanged(bool commitSaveNow) = 0;
    virtual void hudOnRequestSave() = 0;
    virtual void hudOnRequestCloseEdit() = 0;
    virtual void hudOnMouseBindToDrawer(int mouseVk) = 0;
    virtual void hudOnToast(const std::string& msg, DWORD durationMs) = 0;
};

// Lightweight owned layered overlay pinned STRICTLY to the embedded phone video rect.
// Renders ONLY key badges, crosshair dot, toasts, and Edit Mode draggable key nodes
// (zero overlapping config panels and zero floating CFG ball!).
class HudOverlay {
public:
    explicit HudOverlay(IHudOverlayHost* host);
    ~HudOverlay();

    bool create(HWND ownerShellHwnd, HINSTANCE hInst);
    void destroy();

    HWND hwnd() const { return hwnd_; }
    void syncBounds(const RECT& videoScreenRect, bool visible);
    void setInteractiveEdit(bool editMode, float hudOpacity);
    void applyOpacity(bool editMode, float hudOpacity);
    void invalidate();

    int selectedBtnIndex() const { return selectedBtnIndex_; }
    void setSelectedBtnIndex(int idx);

    HudRebindTarget rebindTarget() const { return rebindTarget_; }
    void startRebind(HudRebindTarget target);
    void cancelRebind();

    void showToast(const std::string& msg, DWORD durationMs = 2000);
    bool checkToastExpired();

    // Forwarded from ShellWindow if user presses a key/mouse while ShellWindow has focus
    bool handleKeyDown(int vk);
    bool handleMouseBind(int mouseVk);

private:
    enum DragTarget {
        DRAG_NONE = 0,
        DRAG_JOY,
        DRAG_AIM,
        DRAG_FIRE,
        DRAG_LEFT_FIRE,
        DRAG_BUTTON,
        DRAG_SWIPE_END
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    DragTarget hitTestNode(int px, int py, int& outBtnIdx) const;
    void onMouseDown(int x, int y, bool rightBtn);
    void onDoubleClick(int x, int y);
    void onMouseMove(int x, int y);
    void onMouseUp();
    void onMouseWheel(int x, int y, int delta);
    void onPaint(HDC hdc);

    IHudOverlayHost* host_;
    HWND hwnd_ = NULL;
    HWND ownerHwnd_ = NULL;
    RECT lastScreenRect_;
    bool visible_ = false;
    bool interactive_ = false;

    DragTarget dragging_ = DRAG_NONE;
    int dragBtnIndex_ = -1;
    int selectedBtnIndex_ = -1;
    HudRebindTarget rebindTarget_ = HUD_REBIND_NONE;

    std::string toastMsg_;
    DWORD toastExpireTick_ = 0;
    bool toastWasVisible_ = false;

    HDC cacheDC_ = NULL;
    HBITMAP cacheBmp_ = NULL;
    HGDIOBJ cacheOldBmp_ = NULL;
    int cacheW_ = 0;
    int cacheH_ = 0;
    HudFonts fonts_;
};
