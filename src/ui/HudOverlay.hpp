#pragma once

#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "ui/EditorState.hpp"
#include "ui/HudRenderer.hpp"

#include <string>
#include <vector>

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

typedef void (*HudBitmapReadyFn)(void* ctx, const void* bgraPremulPixels, int width, int height);

// Per-pixel premultiplied-alpha (GDI+ PARGB) HUD compositor:
//   - In Stage 2 Native D3D11 Mode: renders directly into an in-memory PARGB surface and hands
//     it to D3D11Renderer to draw in the EXACT SAME frame as the decoded phone video (zero floating window!).
//   - In Offline Studio / Legacy scrcpy.exe Mode: presents via UpdateLayeredWindow owned popup.
class HudOverlay {
public:
    explicit HudOverlay(IHudOverlayHost* host);
    ~HudOverlay();

    bool create(HWND ownerShellHwnd, HINSTANCE hInst);
    void destroy();

    // Enables/disables Stage 2 same-frame D3D11 compositing (hides the separate Win32 popup HWND!)
    void setEmbeddedD3DMode(bool embedded, HudBitmapReadyFn cb = nullptr, void* ctx = nullptr);
    bool isEmbeddedD3DMode() const { return embeddedD3DMode_; }

    HWND hwnd() const { return hwnd_; }
    void syncBounds(const RECT& videoScreenRect, bool visible);
    void setInteractiveEdit(bool editMode, float hudOpacity);
    void applyOpacity(bool editMode, float hudOpacity);
    void invalidate();

    int selectedBtnIndex() const { return selectedBtnIndex_; }
    void setSelectedBtnIndex(int idx);
    const std::vector<int>& selectedIndices() const { return selectedIndices_; }

    HudRebindTarget rebindTarget() const { return rebindTarget_; }
    void startRebind(HudRebindTarget target);
    void startNewNodeCapture(int newIdx, const Profile& beforeAdd);
    void cancelRebind();

    // X4 Undo / Redo / Multi-Select / Clipboard
    void pushUndoSnapshot(const Profile& beforeState, const std::string& desc);
    bool undo();
    bool redo();
    void clearUndo();
    bool canUndo() const { return undoStack_.canUndo(); }
    bool canRedo() const { return undoStack_.canRedo(); }

    void showToast(const std::string& msg, DWORD durationMs = 2000);
    void showSnackbar(const std::string& msg, const std::string& actionLabel, DWORD durationMs = 6000);
    bool checkToastExpired();
    bool hitTestToastAction(int vx, int vy) const;

    // Forwarded from ShellWindow / ViewportHost (coords relative to video rect top-left)
    bool handleKeyDown(int vk);
    bool handleMouseBind(int mouseVk);
    void dispatchMouseDown(int x, int y, bool rightBtn);
    void dispatchDoubleClick(int x, int y);
    void dispatchMouseMove(int x, int y);
    void dispatchMouseLeave();
    void dispatchMouseUp();
    void dispatchMouseWheel(int x, int y, int delta);

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

    bool ensureSurface(int w, int h);
    void freeSurface();
    void renderLayered();

    bool hitTestDeleteChip(int px, int py) const;
    DragTarget hitTestNode(int px, int py, int& outBtnIdx) const;
    void onMouseDown(int x, int y, bool rightBtn);
    void onDoubleClick(int x, int y);
    void onMouseMove(int x, int y);
    void onMouseUp();
    void onMouseWheel(int x, int y, int delta);
    void duplicateSelected();
    void copySelected();
    void pasteCopied();
    void nudgeSelected(float dx, float dy);
    void deleteSelected();

    IHudOverlayHost* host_;
    HWND hwnd_ = NULL;
    HWND ownerHwnd_ = NULL;
    RECT lastScreenRect_;
    bool visible_ = false;
    bool interactive_ = false;

    DragTarget dragging_ = DRAG_NONE;
    int dragBtnIndex_ = -1;
    bool dragPending_ = false;
    bool dragActive_ = false;
    int dragStartPx_ = 0;
    int dragStartPy_ = 0;
    float dragOffsetX_ = 0.0f;
    float dragOffsetY_ = 0.0f;
    Profile preDragProfile_;

    bool marqueeActive_ = false;
    int marqueeStartPx_ = 0;
    int marqueeStartPy_ = 0;
    RECT marqueeRc_ = { 0, 0, 0, 0 };

    int selectedBtnIndex_ = -1;
    std::vector<int> selectedIndices_;
    std::vector<KeyNode> copiedNodes_;
    std::vector<AlignGuide> activeGuides_;
    ProfileUndoStack undoStack_;

    int hoveredBtnIndex_ = -1;
    bool hoverDeleteChip_ = false;
    HudRebindTarget rebindTarget_ = HUD_REBIND_NONE;
    bool pendingNewNode_ = false;
    Profile preAddProfile_;

    std::string toastMsg_;
    std::string toastActionLabel_;
    RECT toastActionRc_ = { 0, 0, 0, 0 };
    DWORD toastExpireTick_ = 0;
    bool toastWasVisible_ = false;

    // 32-bit top-down premultiplied ARGB DIBSection backing store for UpdateLayeredWindow
    HDC dibDC_ = NULL;
    HBITMAP dibBmp_ = NULL;
    HGDIOBJ dibOldBmp_ = NULL;
    void* dibBits_ = NULL;
    int dibW_ = 0;
    int dibH_ = 0;

    bool embeddedD3DMode_ = false;
    HudBitmapReadyFn d3dBitmapCb_ = nullptr;
    void* d3dBitmapCtx_ = nullptr;
};
