#include "ui/HudOverlay.hpp"
#include "config/KeyCodes.hpp"

#include <cmath>
#include <cstring>

namespace {

float clamp01(float v) {
    if (v < 0.01f) return 0.01f;
    if (v > 0.99f) return 0.99f;
    return v;
}

float clampRange(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

HudOverlay::HudOverlay(IHudOverlayHost* host) : host_(host) {
    ZeroMemory(&lastScreenRect_, sizeof(lastScreenRect_));
}

HudOverlay::~HudOverlay() {
    destroy();
}

void HudOverlay::freeSurface() {
    if (dibDC_) {
        if (dibOldBmp_) SelectObject(dibDC_, dibOldBmp_);
        if (dibBmp_) DeleteObject(dibBmp_);
        DeleteDC(dibDC_);
        dibDC_ = NULL;
        dibBmp_ = NULL;
        dibOldBmp_ = NULL;
        dibBits_ = NULL;
        dibW_ = 0;
        dibH_ = 0;
    }
}

bool HudOverlay::ensureSurface(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (dibDC_ && dibBits_ && w == dibW_ && h == dibH_) return true;

    freeSurface();
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;

    dibDC_ = CreateCompatibleDC(screenDC);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // Top-down DIB matches GDI+ Bitmap memory layout
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dibBmp_ = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &dibBits_, NULL, 0);
    ReleaseDC(NULL, screenDC);

    if (!dibDC_ || !dibBmp_ || !dibBits_) {
        freeSurface();
        return false;
    }
    dibOldBmp_ = SelectObject(dibDC_, dibBmp_);
    dibW_ = w;
    dibH_ = h;
    return true;
}

void HudOverlay::destroy() {
    freeSurface();
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = NULL;
}

bool HudOverlay::create(HWND ownerShellHwnd, HINSTANCE hInst) {
    ownerHwnd_ = ownerShellHwnd;
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = HudOverlay::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "PhantomTouchHudOverlay";
        if (!RegisterClassExA(&wc)) return false;
        reg = true;
    }

    // Owned WS_EX_LAYERED popup (NO WS_EX_TOPMOST!): composited per-pixel via UpdateLayeredWindow
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExA(exStyle, "PhantomTouchHudOverlay", "Phantom Touch HUD",
                            WS_POPUP, 0, 0, 100, 100,
                            ownerShellHwnd, NULL, hInst, this);
    return hwnd_ != NULL;
}

void HudOverlay::setEmbeddedD3DMode(bool embedded, HudBitmapReadyFn cb, void* ctx) {
    embeddedD3DMode_ = embedded;
    d3dBitmapCb_ = cb;
    d3dBitmapCtx_ = ctx;
    if (embeddedD3DMode_ && hwnd_ && IsWindow(hwnd_)) {
        // Never display the separate floating Win32 popup when D3D11 draws HUD in the same frame!
        ShowWindow(hwnd_, SW_HIDE);
    }
    if (visible_) {
        renderLayered();
    }
}

void HudOverlay::syncBounds(const RECT& videoScreenRect, bool visible) {
    int w = videoScreenRect.right - videoScreenRect.left;
    int h = videoScreenRect.bottom - videoScreenRect.top;
    if (!visible || w < 32 || h < 32) {
        if (visible_) {
            if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
        }
        return;
    }

    bool changed = (videoScreenRect.left != lastScreenRect_.left ||
                    videoScreenRect.top != lastScreenRect_.top ||
                    videoScreenRect.right != lastScreenRect_.right ||
                    videoScreenRect.bottom != lastScreenRect_.bottom);
    if (changed || !visible_) {
        lastScreenRect_ = videoScreenRect;
        if (!visible_) {
            if (!embeddedD3DMode_ && hwnd_) {
                ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            }
            visible_ = true;
        }
        renderLayered();
    }
}

void HudOverlay::setInteractiveEdit(bool editMode, float hudOpacity) {
    (void)hudOpacity;
    interactive_ = editMode;
    dragging_ = DRAG_NONE;
    dragPending_ = false;
    dragActive_ = false;
    marqueeActive_ = false;
    activeGuides_.clear();
    hoveredBtnIndex_ = -1;
    hoverDeleteChip_ = false;
    if (!editMode) {
        if (pendingNewNode_ && rebindTarget_ != HUD_REBIND_NONE && host_) {
            cancelRebind();
        }
        rebindTarget_ = HUD_REBIND_NONE;
        pendingNewNode_ = false;
    }

    if (hwnd_ && !embeddedD3DMode_) {
        LONG ex = GetWindowLongA(hwnd_, GWL_EXSTYLE);
        if (editMode) {
            ex &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        } else {
            ex |= (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        }
        SetWindowLongA(hwnd_, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd_, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    renderLayered();
}

void HudOverlay::applyOpacity(bool editMode, float hudOpacity) {
    (void)editMode;
    (void)hudOpacity;
    renderLayered();
}

void HudOverlay::invalidate() {
    if (visible_ && (embeddedD3DMode_ || hwnd_)) {
        renderLayered();
    }
}

void HudOverlay::dispatchMouseDown(int x, int y, bool rightBtn) {
    if (interactive_) onMouseDown(x, y, rightBtn);
}

void HudOverlay::dispatchDoubleClick(int x, int y) {
    if (interactive_) onDoubleClick(x, y);
}

void HudOverlay::dispatchMouseMove(int x, int y) {
    if (interactive_) onMouseMove(x, y);
}

void HudOverlay::dispatchMouseLeave() {
    if (interactive_ && (hoveredBtnIndex_ != -1 || hoverDeleteChip_)) {
        hoveredBtnIndex_ = -1;
        hoverDeleteChip_ = false;
        renderLayered();
    }
}

void HudOverlay::dispatchMouseUp() {
    if (interactive_) onMouseUp();
}

void HudOverlay::dispatchMouseWheel(int x, int y, int delta) {
    if (!interactive_) return;
    int wvk = (delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN;
    if (handleMouseBind(wvk)) return;
    onMouseWheel(x, y, delta);
}

void HudOverlay::setSelectedBtnIndex(int idx) {
    selectedBtnIndex_ = idx;
    selectedIndices_.clear();
    if (idx >= 0) selectedIndices_.push_back(idx);
    renderLayered();
}

void HudOverlay::startRebind(HudRebindTarget target) {
    pendingNewNode_ = false;
    rebindTarget_ = target;
    renderLayered();
}

void HudOverlay::startNewNodeCapture(int newIdx, const Profile& beforeAdd) {
    pendingNewNode_ = true;
    preAddProfile_ = beforeAdd;
    selectedBtnIndex_ = newIdx;
    selectedIndices_.clear();
    if (newIdx >= 0) selectedIndices_.push_back(newIdx);
    rebindTarget_ = HUD_REBIND_BUTTON;
    renderLayered();
}

void HudOverlay::cancelRebind() {
    if (rebindTarget_ == HUD_REBIND_NONE && !pendingNewNode_) return;
    bool wasNew = pendingNewNode_;
    pendingNewNode_ = false;
    rebindTarget_ = HUD_REBIND_NONE;
    if (wasNew && host_) {
        host_->hudProfile() = preAddProfile_;
        selectedBtnIndex_ = -1;
        selectedIndices_.clear();
        host_->hudOnSelectionChanged(-1);
        host_->hudOnProfileChanged(true);
    }
    renderLayered();
}

void HudOverlay::pushUndoSnapshot(const Profile& beforeState, const std::string& desc) {
    undoStack_.push(beforeState, desc);
}

bool HudOverlay::undo() {
    if (!host_) return false;
    std::string desc;
    if (!undoStack_.undo(host_->hudProfile(), &desc)) return false;
    selectedBtnIndex_ = -1;
    selectedIndices_.clear();
    hoveredBtnIndex_ = -1;
    pendingNewNode_ = false;
    rebindTarget_ = HUD_REBIND_NONE;
    host_->hudOnSelectionChanged(-1);
    host_->hudOnProfileChanged(true);
    host_->hudOnToast("Undid: " + (desc.empty() ? std::string("edit") : desc), 1600);
    renderLayered();
    return true;
}

bool HudOverlay::redo() {
    if (!host_) return false;
    std::string desc;
    if (!undoStack_.redo(host_->hudProfile(), &desc)) return false;
    selectedBtnIndex_ = -1;
    selectedIndices_.clear();
    hoveredBtnIndex_ = -1;
    host_->hudOnSelectionChanged(-1);
    host_->hudOnProfileChanged(true);
    host_->hudOnToast("Redid: " + (desc.empty() ? std::string("edit") : desc), 1600);
    renderLayered();
    return true;
}

void HudOverlay::clearUndo() {
    undoStack_.clear();
}

void HudOverlay::showToast(const std::string& msg, DWORD durationMs) {
    toastMsg_ = msg;
    toastActionLabel_.clear();
    ZeroMemory(&toastActionRc_, sizeof(toastActionRc_));
    toastExpireTick_ = GetTickCount() + durationMs;
    toastWasVisible_ = !msg.empty();
    renderLayered();
}

void HudOverlay::showSnackbar(const std::string& msg, const std::string& actionLabel, DWORD durationMs) {
    toastMsg_ = msg;
    toastActionLabel_ = actionLabel;
    ZeroMemory(&toastActionRc_, sizeof(toastActionRc_));
    toastExpireTick_ = GetTickCount() + durationMs;
    toastWasVisible_ = !msg.empty();
    renderLayered();
}

bool HudOverlay::hitTestToastAction(int vx, int vy) const {
    if (toastActionLabel_.empty() || !toastWasVisible_) return false;
    if (static_cast<LONG>(GetTickCount() - toastExpireTick_) >= 0) return false;
    return (vx >= toastActionRc_.left && vx < toastActionRc_.right &&
            vy >= toastActionRc_.top && vy < toastActionRc_.bottom);
}

bool HudOverlay::checkToastExpired() {
    bool active = (!toastMsg_.empty() && static_cast<LONG>(GetTickCount() - toastExpireTick_) < 0);
    bool expired = (toastWasVisible_ && !active);
    toastWasVisible_ = active;
    if (!active) {
        toastActionLabel_.clear();
        ZeroMemory(&toastActionRc_, sizeof(toastActionRc_));
    }
    if (expired) renderLayered();
    return expired;
}

void HudOverlay::renderLayered() {
    if (!visible_ || !host_) return;
    if (!embeddedD3DMode_ && !hwnd_) return;
    int w = lastScreenRect_.right - lastScreenRect_.left;
    int h = lastScreenRect_.bottom - lastScreenRect_.top;
    if (w < 32 || h < 32) return;
    if (!ensureSurface(w, h)) return;

    const Profile& p = host_->hudProfile();
    EngineTelemetry tel = host_->hudTelemetry();
    bool editMode = host_->hudIsEditMode();
    bool demoMode = host_->hudIsDemoMode();
    bool aimMode  = host_->hudIsAimMode();

    {
        // Wrap our 32-bit top-down DIBSection directly as PixelFormat32bppPARGB for perfect alpha compositing!
        Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(dibBits_));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        if (editMode) {
            // Subtle ~28% dark glass wash: keeps live game HUD bright while ensuring alpha > 0
            // on every pixel so 100% of clicks, drags, right-clicks & double-clicks hit cleanly!
            g.Clear(Gdiplus::Color(72, 8, 12, 20));
            GdiPlusDraw::strokeRoundedRect(g, 1.0f, 1.0f, static_cast<float>(w - 2), static_cast<float>(h - 2),
                                           6.0f, 0x9900D4FFu, 1.5f);

            std::vector<KeyConflict> conflicts = EditorGeometry::detectConflicts(p, aimMode);
            HudEditExtras ex;
            ex.multiSelected = &selectedIndices_;
            ex.alignGuides = &activeGuides_;
            ex.conflicts = &conflicts;
            ex.marqueeActive = marqueeActive_;
            ex.marqueeRc = marqueeRc_;
            ex.skipDynamicDots = false;

            HudRenderer::paintKeymapNodes(g, w, h, p, tel, true, demoMode,
                                          selectedBtnIndex_, hoveredBtnIndex_, hoverDeleteChip_,
                                          (rebindTarget_ == HUD_REBIND_BUTTON), p.hudOpacity, &ex);

            if (rebindTarget_ != HUD_REBIND_NONE || host_->hudIsSpecialDrawerRebind()) {
                std::string targetName = "KEY";
                bool specialAim = false;
                if (host_->hudIsSpecialDrawerRebind()) {
                    targetName = host_->hudSpecialRebindLabel();
                    specialAim = host_->hudIsSpecialAimRebind();
                } else if (rebindTarget_ == HUD_REBIND_TOGGLE_AIM) {
                    targetName = "AIM LOCK / UNLOCK";
                    specialAim = true;
                } else if (rebindTarget_ == HUD_REBIND_FIRE_KEY) {
                    targetName = "FIRE";
                } else if (rebindTarget_ == HUD_REBIND_BUTTON &&
                           selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
                    targetName = p.buttons[selectedBtnIndex_].label;
                }
                HudRenderer::paintRebindPrompt(g, w, h, targetName, specialAim);
            }
        } else {
            // Play Mode: 100% transparent outside the frosted keycaps!
            g.Clear(Gdiplus::Color(0, 0, 0, 0));

            std::vector<KeyConflict> playConflicts = EditorGeometry::detectConflicts(p, aimMode);
            HudEditExtras ex;
            ex.conflicts = &playConflicts;
            ex.skipDynamicDots = embeddedD3DMode_;

            if (p.keymapEnabled && (p.showHudInPlay || demoMode)) {
                HudRenderer::paintKeymapNodes(g, w, h, p, tel, false, demoMode,
                                              -1, -1, false, false, p.hudOpacity, &ex);
            }
            if (p.keymapEnabled && aimMode && !demoMode && !embeddedD3DMode_) {
                HudRenderer::paintCrosshairDot(g, w, h);
            }
            if (host_->hudIsFullscreen() && !aimMode) {
                HudRenderer::paintFullscreenStatus(g, w, p, tel);
            }
        }

        if (!toastMsg_.empty() && static_cast<LONG>(GetTickCount() - toastExpireTick_) < 0) {
            HudRenderer::paintToastWithAction(g, w, h, toastMsg_, toastActionLabel_, &toastActionRc_);
        } else {
            ZeroMemory(&toastActionRc_, sizeof(toastActionRc_));
        }
    }

    if (embeddedD3DMode_) {
        if (d3dBitmapCb_) {
            d3dBitmapCb_(d3dBitmapCtx_, dibBits_, w, h);
        }
        return;
    }

    POINT ptDst = { lastScreenRect_.left, lastScreenRect_.top };
    SIZE  sz    = { w, h };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, NULL, &ptDst, &sz, dibDC_, &ptSrc, 0, &bf, ULW_ALPHA);
}

LRESULT CALLBACK HudOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HudOverlay* self = NULL;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = reinterpret_cast<HudOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HudOverlay*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT HudOverlay::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd, NULL);
            return 0;
        case WM_LBUTTONDOWN:
            if (interactive_) {
                SetCapture(hwnd);
                onMouseDown(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), false);
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (interactive_) {
                onDoubleClick(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                return 0;
            }
            break;
        case WM_RBUTTONDOWN:
            if (interactive_) {
                onMouseDown(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), true);
                return 0;
            }
            break;
        case WM_MBUTTONDOWN:
            if (interactive_ && handleMouseBind(VK_MBUTTON)) return 0;
            break;
        case WM_XBUTTONDOWN:
            if (interactive_) {
                int xvk = (HIWORD(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                if (handleMouseBind(xvk)) return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (interactive_) {
                TRACKMOUSEEVENT tme;
                ZeroMemory(&tme, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                onMouseMove(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                return 0;
            }
            break;
        case WM_MOUSELEAVE:
            if (interactive_ && (hoveredBtnIndex_ != -1 || hoverDeleteChip_)) {
                hoveredBtnIndex_ = -1;
                hoverDeleteChip_ = false;
                renderLayered();
            }
            return 0;
        case WM_LBUTTONUP:
            if (interactive_) {
                if (GetCapture() == hwnd) ReleaseCapture();
                onMouseUp();
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (interactive_) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int wvk = (delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN;
                if (handleMouseBind(wvk)) return 0;
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                ScreenToClient(hwnd, &pt);
                onMouseWheel(pt.x, pt.y, delta);
                return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (interactive_) {
                if (handleKeyDown(static_cast<int>(wParam))) return 0;
            } else if (msg == WM_SYSKEYDOWN) {
                return 0;
            }
            break;
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            if (!interactive_) return 0;
            break;
        case WM_SYSCOMMAND:
            if (!interactive_ && (wParam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_SETCURSOR:
            if (host_ && host_->hudIsAimMode()) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

bool HudOverlay::hitTestDeleteChip(int px, int py) const {
    if (!host_ || !interactive_) return false;
    int w = lastScreenRect_.right - lastScreenRect_.left;
    int h = lastScreenRect_.bottom - lastScreenRect_.top;
    if (w <= 0 || h <= 0) return false;
    Profile p = ProfileDefaults::resolveLayout(host_->hudProfile(), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    if (selectedBtnIndex_ < 0 || selectedBtnIndex_ >= static_cast<int>(p.buttons.size())) return false;

    const KeyNode& b = p.buttons[selectedBtnIndex_];
    float bx = b.x * static_cast<float>(w);
    float by = b.y * static_cast<float>(h);
    std::string keyTxt = HudRenderer::shortKeyLabel(b.comboVk, b.vk);
    float pw = 0.0f, ph = 0.0f;
    HudRenderer::computeNodePillSize(keyTxt, true, pw, ph);

    float cx = 0.0f, cy = 0.0f, cr = 0.0f;
    HudRenderer::computeDeleteChipPos(bx, by, pw, ph, cx, cy, cr);
    float dx = static_cast<float>(px) - cx;
    float dy = static_cast<float>(py) - cy;
    float hitR = cr + 3.0f;
    return (dx * dx + dy * dy) <= (hitR * hitR);
}

HudOverlay::DragTarget HudOverlay::hitTestNode(int px, int py, int& outBtnIdx) const {
    outBtnIdx = -1;
    if (!host_) return DRAG_NONE;
    int w = lastScreenRect_.right - lastScreenRect_.left;
    int h = lastScreenRect_.bottom - lastScreenRect_.top;
    if (w <= 0 || h <= 0) return DRAG_NONE;

    float fpx = static_cast<float>(px);
    float fpy = static_cast<float>(py);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);

    Profile p = ProfileDefaults::resolveLayout(host_->hudProfile(), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    for (int i = static_cast<int>(p.buttons.size()) - 1; i >= 0; --i) {
        if (p.buttons[i].action != NODE_SWIPE) continue;
        float ex = p.buttons[i].x2 * fw;
        float ey = p.buttons[i].y2 * fh;
        float dx = fpx - ex, dy = fpy - ey;
        if (dx * dx + dy * dy <= 14.0f * 14.0f) { outBtnIdx = i; return DRAG_SWIPE_END; }
    }
    for (int i = static_cast<int>(p.buttons.size()) - 1; i >= 0; --i) {
        const KeyNode& b = p.buttons[i];
        float bx = b.x * fw;
        float by = b.y * fh;
        std::string keyTxt = HudRenderer::shortKeyLabel(b.comboVk, b.vk);
        float pw = 0.0f, ph = 0.0f;
        HudRenderer::computeNodePillSize(keyTxt, true, pw, ph);
        float halfW = pw * 0.5f + 4.0f;
        float halfH = ph * 0.5f + 4.0f;
        if (std::fabs(fpx - bx) <= halfW && std::fabs(fpy - by) <= halfH) {
            outBtnIdx = i;
            return DRAG_BUTTON;
        }
    }
    {
        float fx = p.fireX * fw, fy = p.fireY * fh;
        if ((fpx - fx) * (fpx - fx) + (fpy - fy) * (fpy - fy) <= 24.0f * 24.0f) return DRAG_FIRE;
    }
    {
        float lfx = p.leftFireX * fw, lfy = p.leftFireY * fh;
        if ((fpx - lfx) * (fpx - lfx) + (fpy - lfy) * (fpy - lfy) <= 24.0f * 24.0f) return DRAG_LEFT_FIRE;
    }
    {
        float ax = p.aimX * fw, ay = p.aimY * fh;
        if ((fpx - ax) * (fpx - ax) + (fpy - ay) * (fpy - ay) <= 24.0f * 24.0f) return DRAG_AIM;
    }
    {
        float jx = p.joyX * fw, jy = p.joyY * fh;
        float jr = p.joyRadius * fh;
        if (jr < 28.0f) jr = 28.0f;
        if ((fpx - jx) * (fpx - jx) + (fpy - jy) * (fpy - jy) <= jr * jr) return DRAG_JOY;
    }
    return DRAG_NONE;
}

bool HudOverlay::handleMouseBind(int mouseVk) {
    if (!host_) return false;
    if (host_->hudIsSpecialDrawerRebind()) {
        host_->hudOnMouseBindToDrawer(mouseVk);
        return true;
    }
    if (rebindTarget_ == HUD_REBIND_NONE) return false;

    Profile& p = host_->hudProfile();
    if (rebindTarget_ == HUD_REBIND_BUTTON &&
        selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        Profile before = pendingNewNode_ ? preAddProfile_ : p;
        p.buttons[selectedBtnIndex_].comboVk = 0;
        p.buttons[selectedBtnIndex_].vk = mouseVk;
        if (p.buttons[selectedBtnIndex_].label == "KEY") {
            p.buttons[selectedBtnIndex_].label = KeyCodes::toName(mouseVk);
        }
        undoStack_.push(before, pendingNewNode_ ? "Add key" : "Rebind key");
        pendingNewNode_ = false;
        rebindTarget_ = HUD_REBIND_NONE;
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Bound '" + p.buttons[selectedBtnIndex_].label + "' -> " + KeyCodes::toName(mouseVk), 1800);
        renderLayered();
        return true;
    }
    if (rebindTarget_ == HUD_REBIND_FIRE_KEY) {
        undoStack_.push(p, "Rebind Fire");
        p.fireVk = mouseVk;
        rebindTarget_ = HUD_REBIND_NONE;
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Fire key -> " + KeyCodes::toName(mouseVk), 1800);
        renderLayered();
        return true;
    }
    if (rebindTarget_ == HUD_REBIND_TOGGLE_AIM) {
        if (mouseVk == VK_LBUTTON) {
            host_->hudOnToast("Use Keyboard, MMB, RMB, X1, X2, or Wheel for Aim Lock", 2400);
            return true;
        }
        undoStack_.push(p, "Rebind Aim Lock");
        p.toggleAimVk = mouseVk;
        rebindTarget_ = HUD_REBIND_NONE;
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Aim Lock/Unlock key -> " + KeyCodes::toName(mouseVk), 1800);
        renderLayered();
        return true;
    }
    return false;
}

void HudOverlay::onDoubleClick(int x, int y) {
    if (!host_) return;
    Profile& p = host_->hudProfile();
    int btnIdx = -1;
    DragTarget t = hitTestNode(x, y, btnIdx);
    if (t == DRAG_BUTTON && btnIdx >= 0 && btnIdx < static_cast<int>(p.buttons.size())) {
        selectedBtnIndex_ = btnIdx;
        dragging_ = DRAG_NONE;
        rebindTarget_ = HUD_REBIND_BUTTON;
        host_->hudOnSelectionChanged(btnIdx);
        host_->hudOnToast("Replace '" + p.buttons[btnIdx].label + "': Press ANY key, Mouse button, or Scroll", 4000);
        renderLayered();
    } else if (t == DRAG_FIRE || t == DRAG_LEFT_FIRE) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = HUD_REBIND_FIRE_KEY;
        host_->hudOnToast("Replace FIRE key: Press ANY key or Mouse button", 4000);
        renderLayered();
    } else if (t == DRAG_AIM) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = HUD_REBIND_TOGGLE_AIM;
        host_->hudOnToast("Replace AIM LOCK key: Press ANY key (LShift, ~) or MMB", 4000);
        renderLayered();
    }
}

void HudOverlay::onMouseDown(int x, int y, bool rightBtn) {
    if (!host_) return;
    if (rebindTarget_ != HUD_REBIND_NONE || host_->hudIsSpecialDrawerRebind()) {
        if (handleMouseBind(rightBtn ? VK_RBUTTON : VK_LBUTTON)) return;
    }

    int w = lastScreenRect_.right - lastScreenRect_.left;
    int h = lastScreenRect_.bottom - lastScreenRect_.top;
    if (w <= 0 || h <= 0) return;

    Profile& p = host_->hudProfile();

    // X9: Clickable Undo / Retry snackbar action over HUD overlay
    if (!rightBtn && hitTestToastAction(x, y)) {
        if (undo()) return;
    }

    // 1-click quick-delete 'x' chip on the currently selected keycap (with Undo!)
    if (!rightBtn && hitTestDeleteChip(x, y) &&
        selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        undoStack_.push(p, "Delete key");
        p.buttons.erase(p.buttons.begin() + selectedBtnIndex_);
        selectedBtnIndex_ = -1;
        selectedIndices_.clear();
        hoveredBtnIndex_ = -1;
        hoverDeleteChip_ = false;
        dragging_ = DRAG_NONE;
        dragPending_ = false;
        dragActive_ = false;
        host_->hudOnSelectionChanged(-1);
        host_->hudOnProfileChanged(true);
        showSnackbar("Key deleted", "Undo", 6000);
        renderLayered();
        return;
    }

    if (rightBtn) {
        Profile beforeAdd = p;
        KeyNode n;
        n.label = "KEY";
        n.vk = 'K';
        n.action = NODE_PRESS;
        float liveNx = clamp01(static_cast<float>(x) / static_cast<float>(w));
        float liveNy = clamp01(static_cast<float>(y) / static_cast<float>(h));
        ProfileDefaults::unmapEdgeAnchored(liveNx, liveNy, p.edgeAnchored, p.refAspect,
                                           static_cast<float>(w) / static_cast<float>(h), n.x, n.y);
        n.x2 = n.x;
        n.y2 = n.y;
        n.radius = 0.030f;
        p.buttons.push_back(n);
        int newIdx = static_cast<int>(p.buttons.size()) - 1;
        startNewNodeCapture(newIdx, beforeAdd);
        host_->hudOnSelectionChanged(newIdx);
        host_->hudOnToast("New Key placed! Press key, Mouse button, or Scroll (Esc cancels)", 3500);
        return;
    }

    int btnIdx = -1;
    dragging_ = hitTestNode(x, y, btnIdx);
    dragBtnIndex_ = btnIdx;
    dragPending_ = false;
    dragActive_ = false;
    marqueeActive_ = false;
    activeGuides_.clear();

    bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (dragging_ == DRAG_BUTTON) {
        if (ctrlHeld) {
            bool found = false;
            for (size_t i = 0; i < selectedIndices_.size(); ++i) {
                if (selectedIndices_[i] == btnIdx) {
                    selectedIndices_.erase(selectedIndices_.begin() + i);
                    found = true;
                    break;
                }
            }
            if (!found) selectedIndices_.push_back(btnIdx);
            selectedBtnIndex_ = selectedIndices_.empty() ? -1 : selectedIndices_.back();
        } else {
            bool alreadyInGroup = false;
            for (size_t i = 0; i < selectedIndices_.size(); ++i) {
                if (selectedIndices_[i] == btnIdx) { alreadyInGroup = true; break; }
            }
            if (!alreadyInGroup) {
                selectedIndices_.clear();
                selectedIndices_.push_back(btnIdx);
            }
            selectedBtnIndex_ = btnIdx;
        }
    } else if (dragging_ == DRAG_SWIPE_END) {
        selectedBtnIndex_ = btnIdx;
        selectedIndices_.assign(1, btnIdx);
    } else if (dragging_ == DRAG_NONE) {
        if (!ctrlHeld) {
            selectedBtnIndex_ = -1;
            selectedIndices_.clear();
        }
        marqueeActive_ = true;
        marqueeStartPx_ = x;
        marqueeStartPy_ = y;
        marqueeRc_ = { x, y, x, y };
    }

    if (dragging_ != DRAG_NONE) {
        preDragProfile_ = p;
        dragPending_ = true;
        dragStartPx_ = x;
        dragStartPy_ = y;
        float liveNx = clamp01(static_cast<float>(x) / static_cast<float>(w));
        float liveNy = clamp01(static_cast<float>(y) / static_cast<float>(h));
        float curNx = liveNx, curNy = liveNy;
        ProfileDefaults::unmapEdgeAnchored(liveNx, liveNy, p.edgeAnchored, p.refAspect,
                                           static_cast<float>(w) / static_cast<float>(h), curNx, curNy);
        float nodeX = curNx, nodeY = curNy;
        switch (dragging_) {
            case DRAG_JOY:       nodeX = p.joyX; nodeY = p.joyY; break;
            case DRAG_AIM:       nodeX = p.aimX; nodeY = p.aimY; break;
            case DRAG_FIRE:      nodeX = p.fireX; nodeY = p.fireY; break;
            case DRAG_LEFT_FIRE: nodeX = p.leftFireX; nodeY = p.leftFireY; break;
            case DRAG_BUTTON:
                if (btnIdx >= 0 && btnIdx < static_cast<int>(p.buttons.size())) {
                    nodeX = p.buttons[btnIdx].x;
                    nodeY = p.buttons[btnIdx].y;
                }
                break;
            case DRAG_SWIPE_END:
                if (btnIdx >= 0 && btnIdx < static_cast<int>(p.buttons.size())) {
                    nodeX = p.buttons[btnIdx].x2;
                    nodeY = p.buttons[btnIdx].y2;
                }
                break;
            default: break;
        }
        // N33: preserve grab offset so node never snaps center onto cursor on click!
        dragOffsetX_ = nodeX - curNx;
        dragOffsetY_ = nodeY - curNy;
    }

    host_->hudOnSelectionChanged(selectedBtnIndex_);
    renderLayered();
}

void HudOverlay::onMouseMove(int x, int y) {
    if (!host_) return;
    int w = lastScreenRect_.right - lastScreenRect_.left;
    int h = lastScreenRect_.bottom - lastScreenRect_.top;
    if (w <= 0 || h <= 0) return;

    if (marqueeActive_) {
        marqueeRc_.left   = (marqueeStartPx_ < x) ? marqueeStartPx_ : x;
        marqueeRc_.right  = (marqueeStartPx_ > x) ? marqueeStartPx_ : x;
        marqueeRc_.top    = (marqueeStartPy_ < y) ? marqueeStartPy_ : y;
        marqueeRc_.bottom = (marqueeStartPy_ > y) ? marqueeStartPy_ : y;
        renderLayered();
        return;
    }

    if (dragging_ == DRAG_NONE) {
        bool newDelHov = hitTestDeleteChip(x, y);
        int hovIdx = -1;
        if (!newDelHov) {
            hitTestNode(x, y, hovIdx);
        }
        if (hovIdx != hoveredBtnIndex_ || newDelHov != hoverDeleteChip_) {
            hoveredBtnIndex_ = hovIdx;
            hoverDeleteChip_ = newDelHov;
            renderLayered();
        }
        return;
    }

    // N33: require 4 dp drag threshold before moving node
    if (dragPending_ && !dragActive_) {
        int thresh = GdiPlusDraw::dp(4);
        int dxPx = x - dragStartPx_;
        int dyPx = y - dragStartPy_;
        if (dxPx < 0) dxPx = -dxPx;
        if (dyPx < 0) dyPx = -dyPx;
        if (dxPx < thresh && dyPx < thresh) {
            return;
        }
        dragActive_ = true;
    }

    float liveNx = clamp01(static_cast<float>(x) / static_cast<float>(w));
    float liveNy = clamp01(static_cast<float>(y) / static_cast<float>(h));
    Profile& p = host_->hudProfile();
    float curNx = liveNx, curNy = liveNy;
    ProfileDefaults::unmapEdgeAnchored(liveNx, liveNy, p.edgeAnchored, p.refAspect,
                                       static_cast<float>(w) / static_cast<float>(h), curNx, curNy);
    float nx = clamp01(curNx + dragOffsetX_);
    float ny = clamp01(curNy + dragOffsetY_);

    bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    activeGuides_.clear();

    switch (dragging_) {
        case DRAG_JOY:
            EditorGeometry::snapAndCollectGuides(p, -1, nx, ny, shiftHeld, activeGuides_);
            p.joyX = nx; p.joyY = ny;
            break;
        case DRAG_AIM:
            EditorGeometry::snapAndCollectGuides(p, -1, nx, ny, shiftHeld, activeGuides_);
            p.aimX = nx; p.aimY = ny;
            break;
        case DRAG_FIRE:
            EditorGeometry::snapAndCollectGuides(p, -1, nx, ny, shiftHeld, activeGuides_);
            p.fireX = nx; p.fireY = ny;
            break;
        case DRAG_LEFT_FIRE:
            EditorGeometry::snapAndCollectGuides(p, -1, nx, ny, shiftHeld, activeGuides_);
            p.leftFireX = nx; p.leftFireY = ny;
            break;
        case DRAG_BUTTON:
            if (dragBtnIndex_ >= 0 && dragBtnIndex_ < static_cast<int>(p.buttons.size())) {
                EditorGeometry::snapAndCollectGuides(p, dragBtnIndex_, nx, ny, shiftHeld, activeGuides_);
                KeyNode& b = p.buttons[dragBtnIndex_];
                float dx = nx - b.x, dy = ny - b.y;
                b.x = nx; b.y = ny;
                if (b.action == NODE_SWIPE) {
                    b.x2 = clamp01(b.x2 + dx);
                    b.y2 = clamp01(b.y2 + dy);
                }
                for (size_t mi = 0; mi < selectedIndices_.size(); ++mi) {
                    int idx = selectedIndices_[mi];
                    if (idx == dragBtnIndex_ || idx < 0 || idx >= static_cast<int>(p.buttons.size())) continue;
                    KeyNode& mb = p.buttons[idx];
                    mb.x = clamp01(mb.x + dx);
                    mb.y = clamp01(mb.y + dy);
                    if (mb.action == NODE_SWIPE) {
                        mb.x2 = clamp01(mb.x2 + dx);
                        mb.y2 = clamp01(mb.y2 + dy);
                    }
                }
            }
            break;
        case DRAG_SWIPE_END:
            if (dragBtnIndex_ >= 0 && dragBtnIndex_ < static_cast<int>(p.buttons.size())) {
                p.buttons[dragBtnIndex_].x2 = nx;
                p.buttons[dragBtnIndex_].y2 = ny;
            }
            break;
        default: break;
    }
    // N33: HUD-only redraw during drag — engine & debounced worker save run on onMouseUp!
    renderLayered();
}

void HudOverlay::onMouseUp() {
    activeGuides_.clear();
    if (marqueeActive_) {
        marqueeActive_ = false;
        if (host_) {
            int w = lastScreenRect_.right - lastScreenRect_.left;
            int h = lastScreenRect_.bottom - lastScreenRect_.top;
            if (w > 0 && h > 0 &&
                (marqueeRc_.right - marqueeRc_.left >= 4 || marqueeRc_.bottom - marqueeRc_.top >= 4)) {
                Profile resP = ProfileDefaults::resolveLayout(host_->hudProfile(), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                for (size_t i = 0; i < resP.buttons.size(); ++i) {
                    int px = static_cast<int>(resP.buttons[i].x * static_cast<float>(w));
                    int py = static_cast<int>(resP.buttons[i].y * static_cast<float>(h));
                    if (px >= marqueeRc_.left && px <= marqueeRc_.right &&
                        py >= marqueeRc_.top && py <= marqueeRc_.bottom) {
                        bool dup = false;
                        for (size_t j = 0; j < selectedIndices_.size(); ++j) {
                            if (selectedIndices_[j] == static_cast<int>(i)) { dup = true; break; }
                        }
                        if (!dup) selectedIndices_.push_back(static_cast<int>(i));
                    }
                }
                selectedBtnIndex_ = selectedIndices_.empty() ? -1 : selectedIndices_.back();
                host_->hudOnSelectionChanged(selectedBtnIndex_);
            }
        }
        renderLayered();
        return;
    }
    if (dragging_ != DRAG_NONE) {
        bool didMove = dragActive_;
        dragging_ = DRAG_NONE;
        dragPending_ = false;
        dragActive_ = false;
        if (didMove && host_) {
            undoStack_.push(preDragProfile_, "Move node");
            host_->hudOnProfileChanged(true);
        }
        renderLayered();
    }
}

void HudOverlay::onMouseWheel(int x, int y, int delta) {
    if (!host_) return;
    int btnIdx = -1;
    DragTarget t = hitTestNode(x, y, btnIdx);
    float step = (delta > 0) ? 0.010f : -0.010f;
    bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    Profile& p = host_->hudProfile();

    if (t == DRAG_JOY) {
        if (shiftHeld) p.joySprintRadius = clampRange(p.joySprintRadius + step, 0.04f, 0.35f);
        else p.joyRadius = clampRange(p.joyRadius + step, 0.03f, 0.25f);
    } else if (t == DRAG_AIM) {
        if (shiftHeld) p.aimHalfH = clampRange(p.aimHalfH + step, 0.06f, 0.80f);
        else {
            p.aimHalfW = clampRange(p.aimHalfW + step, 0.06f, 0.45f);
            p.aimHalfH = clampRange(p.aimHalfH + step, 0.06f, 0.80f);
        }
    } else if (t == DRAG_FIRE) {
        p.fireBoxHalfH = clampRange(p.fireBoxHalfH + step, 0.10f, 0.90f);
    } else {
        return;
    }
    host_->hudOnProfileChanged(true);
    renderLayered();
}

void HudOverlay::duplicateSelected() {
    if (!host_ || selectedIndices_.empty()) return;
    Profile& p = host_->hudProfile();
    undoStack_.push(p, "Duplicate node");
    std::vector<int> nextSel;
    for (size_t i = 0; i < selectedIndices_.size(); ++i) {
        int idx = selectedIndices_[i];
        if (idx < 0 || idx >= static_cast<int>(p.buttons.size())) continue;
        KeyNode copy = p.buttons[idx];
        copy.x = clamp01(copy.x + 0.025f);
        copy.y = clamp01(copy.y + 0.025f);
        if (copy.action == NODE_SWIPE) {
            copy.x2 = clamp01(copy.x2 + 0.025f);
            copy.y2 = clamp01(copy.y2 + 0.025f);
        }
        p.buttons.push_back(copy);
        nextSel.push_back(static_cast<int>(p.buttons.size()) - 1);
    }
    if (!nextSel.empty()) {
        selectedIndices_ = nextSel;
        selectedBtnIndex_ = nextSel.back();
        host_->hudOnSelectionChanged(selectedBtnIndex_);
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Duplicated key(s)", 1400);
        renderLayered();
    }
}

void HudOverlay::copySelected() {
    if (!host_ || selectedIndices_.empty()) return;
    const Profile& p = host_->hudProfile();
    copiedNodes_.clear();
    for (size_t i = 0; i < selectedIndices_.size(); ++i) {
        int idx = selectedIndices_[i];
        if (idx >= 0 && idx < static_cast<int>(p.buttons.size())) {
            copiedNodes_.push_back(p.buttons[idx]);
        }
    }
    if (!copiedNodes_.empty()) {
        host_->hudOnToast("Copied node(s) — Ctrl+V to paste in any profile", 1800);
    }
}

void HudOverlay::pasteCopied() {
    if (!host_ || copiedNodes_.empty()) return;
    Profile& p = host_->hudProfile();
    undoStack_.push(p, "Paste node");
    selectedIndices_.clear();
    for (size_t i = 0; i < copiedNodes_.size(); ++i) {
        KeyNode n = copiedNodes_[i];
        n.x = clamp01(n.x + 0.02f);
        n.y = clamp01(n.y + 0.02f);
        if (n.action == NODE_SWIPE) {
            n.x2 = clamp01(n.x2 + 0.02f);
            n.y2 = clamp01(n.y2 + 0.02f);
        }
        p.buttons.push_back(n);
        selectedIndices_.push_back(static_cast<int>(p.buttons.size()) - 1);
    }
    selectedBtnIndex_ = selectedIndices_.empty() ? -1 : selectedIndices_.back();
    host_->hudOnSelectionChanged(selectedBtnIndex_);
    host_->hudOnProfileChanged(true);
    host_->hudOnToast("Pasted node(s)", 1400);
    renderLayered();
}

void HudOverlay::nudgeSelected(float dx, float dy) {
    if (!host_ || selectedIndices_.empty()) return;
    Profile& p = host_->hudProfile();
    undoStack_.push(p, "Nudge node");
    for (size_t i = 0; i < selectedIndices_.size(); ++i) {
        int idx = selectedIndices_[i];
        if (idx < 0 || idx >= static_cast<int>(p.buttons.size())) continue;
        p.buttons[idx].x = clamp01(p.buttons[idx].x + dx);
        p.buttons[idx].y = clamp01(p.buttons[idx].y + dy);
        if (p.buttons[idx].action == NODE_SWIPE) {
            p.buttons[idx].x2 = clamp01(p.buttons[idx].x2 + dx);
            p.buttons[idx].y2 = clamp01(p.buttons[idx].y2 + dy);
        }
    }
    host_->hudOnProfileChanged(true);
    renderLayered();
}

void HudOverlay::deleteSelected() {
    if (!host_ || selectedIndices_.empty()) return;
    Profile& p = host_->hudProfile();
    undoStack_.push(p, "Delete key");
    std::vector<KeyNode> kept;
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        bool rm = false;
        for (size_t j = 0; j < selectedIndices_.size(); ++j) {
            if (selectedIndices_[j] == static_cast<int>(i)) { rm = true; break; }
        }
        if (!rm) kept.push_back(p.buttons[i]);
    }
    p.buttons.swap(kept);
    selectedBtnIndex_ = -1;
    selectedIndices_.clear();
    hoveredBtnIndex_ = -1;
    hoverDeleteChip_ = false;
    host_->hudOnSelectionChanged(-1);
    host_->hudOnProfileChanged(true);
    showSnackbar("Key deleted", "Undo", 6000);
    renderLayered();
}

bool HudOverlay::handleKeyDown(int vk) {
    if (!host_) return false;
    Profile& p = host_->hudProfile();

    if (rebindTarget_ != HUD_REBIND_NONE) {
        if (vk == VK_ESCAPE) {
            if (pendingNewNode_) {
                cancelRebind();
                host_->hudOnToast("Add key cancelled", 1200);
            } else {
                rebindTarget_ = HUD_REBIND_NONE;
                host_->hudOnToast("Rebind cancelled", 1000);
                renderLayered();
            }
            return true;
        }
        vk = KeyCodes::normalizeModifier(vk);
        if (rebindTarget_ == HUD_REBIND_FIRE_KEY) {
            undoStack_.push(p, "Rebind Fire");
            p.fireVk = vk;
            rebindTarget_ = HUD_REBIND_NONE;
            host_->hudOnProfileChanged(true);
            host_->hudOnToast("Fire key -> " + KeyCodes::toName(vk), 1800);
            renderLayered();
            return true;
        }
        if (rebindTarget_ == HUD_REBIND_TOGGLE_AIM) {
            undoStack_.push(p, "Rebind Aim Lock");
            p.toggleAimVk = vk;
            rebindTarget_ = HUD_REBIND_NONE;
            host_->hudOnProfileChanged(true);
            host_->hudOnToast("Aim Lock/Unlock key -> " + KeyCodes::toName(vk), 1800);
            renderLayered();
            return true;
        }
        if (rebindTarget_ == HUD_REBIND_BUTTON &&
            selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
            Profile before = pendingNewNode_ ? preAddProfile_ : p;
            int comboPrefix = 0;
            if (vk != VK_LMENU && vk != VK_RMENU && (GetAsyncKeyState(VK_LMENU) & 0x8000)) comboPrefix = VK_LMENU;
            else if (vk != VK_LCONTROL && vk != VK_RCONTROL && (GetAsyncKeyState(VK_LCONTROL) & 0x8000)) comboPrefix = VK_LCONTROL;
            else if (vk != VK_LSHIFT && vk != VK_RSHIFT && (GetAsyncKeyState(VK_LSHIFT) & 0x8000)) comboPrefix = VK_LSHIFT;

            p.buttons[selectedBtnIndex_].comboVk = comboPrefix;
            p.buttons[selectedBtnIndex_].vk = vk;
            if (p.buttons[selectedBtnIndex_].label == "KEY") {
                p.buttons[selectedBtnIndex_].label = KeyCodes::comboToName(comboPrefix, vk);
            }
            undoStack_.push(before, pendingNewNode_ ? "Add key" : "Rebind key");
            pendingNewNode_ = false;
            rebindTarget_ = HUD_REBIND_NONE;
            host_->hudOnProfileChanged(true);
            host_->hudOnToast("Bound '" + p.buttons[selectedBtnIndex_].label + "' -> " + KeyCodes::comboToName(comboPrefix, vk), 1800);
            renderLayered();
        }
        return true;
    }

    bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrlHeld && vk == 'S') {
        host_->hudOnRequestSave();
        return true;
    }
    if (ctrlHeld && vk == 'Z') return undo();
    if (ctrlHeld && vk == 'Y') return redo();
    if (ctrlHeld && vk == 'D') { duplicateSelected(); return true; }
    if (ctrlHeld && vk == 'C') { copySelected(); return true; }
    if (ctrlHeld && vk == 'V') { pasteCopied(); return true; }

    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
        if (!selectedIndices_.empty()) {
            float step = shiftHeld ? 0.010f : 0.002f;
            float dx = (vk == VK_LEFT) ? -step : (vk == VK_RIGHT ? step : 0.0f);
            float dy = (vk == VK_UP)   ? -step : (vk == VK_DOWN  ? step : 0.0f);
            nudgeSelected(dx, dy);
            return true;
        }
    }

    if (vk == VK_F2 || vk == VK_ESCAPE) {
        host_->hudOnRequestCloseEdit();
        return true;
    }
    if (vk == 'T' && selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        undoStack_.push(p, "Change action");
        KeyNode& b = p.buttons[selectedBtnIndex_];
        b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 1) % 10);
        if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) {
            b.x2 = clamp01(b.x + 0.12f);
            b.durationMs = 120;
        }
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Node '" + b.label + "' -> " + ProfileDefaults::actionTypeToHumanLabel(b.action), 1600);
        renderLayered();
        return true;
    }
    if (vk == VK_RETURN && selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        rebindTarget_ = HUD_REBIND_BUTTON;
        pendingNewNode_ = false;
        host_->hudOnToast("Replace '" + p.buttons[selectedBtnIndex_].label + "': Press ANY key, Mouse button, or Scroll", 4000);
        renderLayered();
        return true;
    }
    if (vk == VK_DELETE && !selectedIndices_.empty()) {
        deleteSelected();
        return true;
    }
    return false;
}
