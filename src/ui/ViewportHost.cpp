#include "ui/ViewportHost.hpp"
#include "ui/Strings.hpp"
#include "ui/Theme.hpp"

#include <cstdio>

namespace {

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

struct EnumFindCtx {
    DWORD pid;
    const char* title;
    HWND found;
};

BOOL CALLBACK enumFindMirrorProc(HWND hwnd, LPARAM lParam) {
    EnumFindCtx* ctx = reinterpret_cast<EnumFindCtx*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (ctx->pid != 0 && wpid != ctx->pid) return TRUE;

    char buf[256] = {0};
    GetWindowTextA(hwnd, buf, sizeof(buf) - 1);
    if (ctx->title && lstrcmpA(buf, ctx->title) == 0) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

} // namespace

ViewportHost::ViewportHost() {
    InitializeCriticalSection(&rectLock_);
    ZeroMemory(&hostBounds_, sizeof(hostBounds_));
}

ViewportHost::~ViewportHost() {
    delete cachedQrBmp_;
    cachedQrBmp_ = nullptr;
    detachMirror();
    d3d_.shutdown();
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = NULL;
    }
    DeleteCriticalSection(&rectLock_);
}

LRESULT CALLBACK ViewportHost::hostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ViewportHost* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<ViewportHost*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ViewportHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleHostMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ViewportHost::handleHostMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (isNativeVideoActive()) {
                if (GetTickCount() - lastPresentTick() >= 45) {
                    renderNativeFrame(true);
                }
            } else {
                RECT rc;
                GetClientRect(hwnd, &rc);
                HBRUSH br = CreateSolidBrush(RGB(10, 13, 19));
                FillRect(hdc, &rc, br);
                DeleteObject(br);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            if (parentHwnd_) {
                SetFocus(parentHwnd_);
            }
            return MA_ACTIVATE;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            if (parentHwnd_) {
                return SendMessageW(parentHwnd_, msg, wParam, lParam);
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_MOUSEMOVE: {
            if (msg == WM_MOUSEMOVE) {
                TRACKMOUSEEVENT tme;
                ZeroMemory(&tme, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            if (parentHwnd_) {
                if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
                    SetFocus(parentHwnd_);
                }
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                MapWindowPoints(hwnd, parentHwnd_, &pt, 1);
                SendMessageW(parentHwnd_, msg, wParam, MAKELPARAM(pt.x, pt.y));
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (parentHwnd_) PostMessageW(parentHwnd_, WM_MOUSELEAVE, 0, 0);
            return 0;
        case WM_MOUSEWHEEL:
            if (parentHwnd_) {
                SendMessageW(parentHwnd_, msg, wParam, lParam);
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ViewportHost::create(HWND parentHwnd, HINSTANCE hInst) {
    parentHwnd_ = parentHwnd;
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = ViewportHost::hostWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"PhantomTouchViewportHost";
        if (!RegisterClassExW(&wc)) return false;
        classRegistered = true;
    }

    hwnd_ = CreateWindowExW(0, L"PhantomTouchViewportHost", L"",
                            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, 100, 100,
                            parentHwnd, NULL, hInst, this);
    return hwnd_ != NULL;
}

bool ViewportHost::rebuildNativeDevice(std::string& outError) {
    if (!d3d_.rebuildDevice(outError)) {
        return false;
    }
    updateVideoLayout(lastPhoneW_, lastPhoneH_);
    renderNativeFrame(true);
    return true;
}

bool ViewportHost::ensureNativeDeviceInitialized(std::string& outError, bool vsyncEnabled) {
    if (!hwnd_) {
        outError = "ViewportHost HWND not created.";
        return false;
    }
    int w = hostBounds_.right - hostBounds_.left;
    int h = hostBounds_.bottom - hostBounds_.top;
    if (w < 64) w = 640;
    if (h < 64) h = 360;

    if (!d3d_.isInitialized()) {
        if (!d3d_.init(hwnd_, w, h, outError, vsyncEnabled)) {
            return false;
        }
    } else {
        d3d_.setVsync(vsyncEnabled);
        d3d_.resize(w, h);
    }
    return true;
}

bool ViewportHost::enableNativeVideo(uint32_t phoneW, uint32_t phoneH, std::string& outError, bool vsyncEnabled) {
    if (!ensureNativeDeviceInitialized(outError, vsyncEnabled)) {
        InterlockedExchange(&nativeVideoActive_, 0);
        return false;
    }
    int w = hostBounds_.right - hostBounds_.left;
    int h = hostBounds_.bottom - hostBounds_.top;
    if (w < 64) w = 640;
    if (h < 64) h = 360;

    updateVideoLayout(phoneW, phoneH);
    if (!fullscreenTarget_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetWindowPos(hwnd_, HWND_BOTTOM, hostBounds_.left, hostBounds_.top, w, h, SWP_NOACTIVATE);
    }
    d3d_.clearOccluded();
    d3d_.markStreamActivated();
    InterlockedExchange(&nativeVideoActive_, 1);
    return true;
}

bool ViewportHost::setFullscreenTarget(HWND topLevelHwnd, bool enterFullscreen, int monW, int monH) {
    if (!d3d_.isInitialized()) {
        fullscreenTarget_ = false;
        return false;
    }
    if (enterFullscreen) {
        if (!topLevelHwnd || !IsWindow(topLevelHwnd)) return false;
        fullscreenTarget_ = true;
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
        bool ok = d3d_.rebindSwapChainTarget(topLevelHwnd, monW, monH);
        updateVideoLayout(lastPhoneW_, lastPhoneH_);
        return ok;
    } else {
        fullscreenTarget_ = false;
        int w = hostBounds_.right - hostBounds_.left;
        int h = hostBounds_.bottom - hostBounds_.top;
        if (w < 8) w = 640;
        if (h < 8) h = 360;
        bool ok = (hwnd_ && IsWindow(hwnd_)) ? d3d_.rebindSwapChainTarget(hwnd_, w, h) : false;
        if (hwnd_ && isNativeVideoActive()) {
            ShowWindow(hwnd_, SW_SHOW);
            SetWindowPos(hwnd_, HWND_BOTTOM, hostBounds_.left, hostBounds_.top, w, h, SWP_NOACTIVATE);
        }
        updateVideoLayout(lastPhoneW_, lastPhoneH_);
        return ok;
    }
}

void ViewportHost::disableNativeVideo() {
    fullscreenTarget_ = false;
    InterlockedExchange(&nativeVideoActive_, 0);
    d3d_.clearVideoFrame();
    d3d_.clearHudBitmap();
    if (!isMirrorEmbedded() && hwnd_ && IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

bool ViewportHost::presentGpuNV12Frame(ID3D11Texture2D* srcTex, uint32_t subresourceIdx,
                                       uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                       bool drawHudOverlay, FrameTiming* ioTiming) {
    if (!isNativeVideoActive()) return false;
    ViewportRect vr;
    EnterCriticalSection(&rectLock_);
    vr = videoRect_;
    LeaveCriticalSection(&rectLock_);
    return d3d_.submitAndRenderGpuNV12(srcTex, subresourceIdx, cropX, cropY, outW, outH, vr, drawHudOverlay, ioTiming);
}

void ViewportHost::presentCpuNV12Frame(const uint8_t* yPlane, const uint8_t* uvPlane, uint32_t stride,
                                       uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                       bool drawHudOverlay, FrameTiming* ioTiming) {
    if (!isNativeVideoActive()) return;
    ViewportRect vr;
    EnterCriticalSection(&rectLock_);
    vr = videoRect_;
    LeaveCriticalSection(&rectLock_);
    d3d_.submitAndRenderCpuNV12(yPlane, uvPlane, stride, cropX, cropY, outW, outH, vr, drawHudOverlay, ioTiming);
}

void ViewportHost::submitDecodedFrame(const DecodedNV12Frame& frame) {
    d3d_.submitNV12Frame(frame);
}

void ViewportHost::updateEmbeddedHud(const void* bgraPremulPixels, int width, int height) {
    d3d_.updateHudBitmap(bgraPremulPixels, width, height);
}

void ViewportHost::renderNativeFrame(bool drawHudOverlay, FrameTiming* ioTiming) {
    if (isNativeVideoActive()) {
        ViewportRect vr;
        EnterCriticalSection(&rectLock_);
        vr = videoRect_;
        LeaveCriticalSection(&rectLock_);
        d3d_.tryRender(vr, drawHudOverlay, ioTiming);
    }
}

bool ViewportHost::captureNativeScreenshot(const std::string& savePath, std::string& outError) const {
    if (!isNativeVideoActive()) {
        outError = "Native D3D11 video not active.";
        return false;
    }
    return d3d_.saveLatestFramePng(savePath, outError);
}

void ViewportHost::setBounds(const RECT& boundsInParent) {
    int prevLeft = hostBounds_.left;
    int prevTop  = hostBounds_.top;
    int prevW = hostBounds_.right - hostBounds_.left;
    int prevH = hostBounds_.bottom - hostBounds_.top;
    hostBounds_ = boundsInParent;
    int w = boundsInParent.right - boundsInParent.left;
    int h = boundsInParent.bottom - boundsInParent.top;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    bool sizeChanged = (w != prevW || h != prevH);
    bool posChanged  = (boundsInParent.left != prevLeft || boundsInParent.top != prevTop);

    if (hwnd_) {
        if (fullscreenTarget_ && isNativeVideoActive()) {
            if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
            if (sizeChanged) d3d_.resize(w, h);
        } else if (isMirrorEmbedded() || isNativeVideoActive()) {
            bool wasVisible = (IsWindowVisible(hwnd_) != FALSE);
            if (!wasVisible) ShowWindow(hwnd_, SW_SHOW);
            if (!wasVisible || sizeChanged || posChanged) {
                SetWindowPos(hwnd_, HWND_BOTTOM, boundsInParent.left, boundsInParent.top, w, h,
                             SWP_NOACTIVATE);
            }
            if (isNativeVideoActive() && sizeChanged) {
                d3d_.resize(w, h);
            }
        } else {
            // Keep child host hidden when Device Hub or Offline Studio is drawn directly on ShellWindow
            if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
        }
    }
    updateVideoLayout(lastPhoneW_, lastPhoneH_);
    // S4 + 0.6 rule 3: never present on UI thread for pure moves or while video thread is actively presenting
    if (isNativeVideoActive() && (sizeChanged || (GetTickCount() - lastPresentTick() >= 45))) {
        renderNativeFrame(true);
    }
}

bool ViewportHost::tryEmbedMirror(const std::string& windowTitle, DWORD expectedPid,
                                  uint32_t phoneW, uint32_t phoneH) {
    if (isMirrorEmbedded()) {
        updateVideoLayout(phoneW, phoneH);
        return true;
    }
    if (!hwnd_) return false;

    EnumFindCtx ctx = { expectedPid, windowTitle.c_str(), NULL };
    EnumWindows(enumFindMirrorProc, reinterpret_cast<LPARAM>(&ctx));
    HWND target = ctx.found;
    if (!target) {
        target = FindWindowA(NULL, windowTitle.c_str());
    }
    if (!target || !IsWindow(target)) return false;

    // Strip top-level decorations and attach cleanly as a non-focus-stealing WS_CHILD
    LONG style = GetWindowLongA(target, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER);
    style |= (WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    SetWindowLongA(target, GWL_STYLE, style);

    LONG exStyle = GetWindowLongA(target, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    exStyle |= WS_EX_NOACTIVATE;
    SetWindowLongA(target, GWL_EXSTYLE, exStyle);

    // Disable direct Win32 input on the --no-control scrcpy child so focus NEVER deadlocks across processes!
    EnableWindow(target, FALSE);
    SetParent(target, hwnd_);

    embeddedHwnd_ = target;
    ShowWindow(hwnd_, SW_SHOW);
    SetWindowPos(hwnd_, HWND_BOTTOM,
                 hostBounds_.left, hostBounds_.top,
                 hostBounds_.right - hostBounds_.left,
                 hostBounds_.bottom - hostBounds_.top,
                 SWP_NOACTIVATE);

    updateVideoLayout(phoneW, phoneH);
    ShowWindow(embeddedHwnd_, SW_SHOWNOACTIVATE);
    return true;
}

void ViewportHost::updateVideoLayout(uint32_t phoneW, uint32_t phoneH) {
    if (phoneW >= 64 && phoneH >= 64) {
        lastPhoneW_ = phoneW;
        lastPhoneH_ = phoneH;
    }
    int hostW = hostBounds_.right - hostBounds_.left;
    int hostH = hostBounds_.bottom - hostBounds_.top;
    int pad = fullscreenTarget_ ? 0 : ((isMirrorEmbedded() || isNativeVideoActive()) ? 4 : 16);
    ViewportRect vr = GdiPlusDraw::computeAspectFit(hostW, hostH, lastPhoneW_, lastPhoneH_, pad);
    EnterCriticalSection(&rectLock_);
    videoRect_ = vr;
    LeaveCriticalSection(&rectLock_);

    if (isMirrorEmbedded()) {
        SetWindowPos(embeddedHwnd_, HWND_TOP,
                     vr.x, vr.y, vr.w, vr.h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void ViewportHost::detachMirror() {
    InterlockedExchange(&nativeVideoActive_, 0);
    d3d_.clearVideoFrame();
    d3d_.clearHudBitmap();
    if (embeddedHwnd_ && IsWindow(embeddedHwnd_)) {
        ShowWindow(embeddedHwnd_, SW_HIDE);
        SetParent(embeddedHwnd_, NULL);
        EnableWindow(embeddedHwnd_, TRUE);
    }
    embeddedHwnd_ = NULL;
    if (hwnd_ && IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

RECT ViewportHost::activeVideoScreenRect() const {
    RECT out = { 0, 0, 0, 0 };
    if (!parentHwnd_ || !IsWindow(parentHwnd_)) return out;
    POINT tl = { hostBounds_.left + videoRect_.x, hostBounds_.top + videoRect_.y };
    POINT br = { tl.x + videoRect_.w, tl.y + videoRect_.h };
    ClientToScreen(parentHwnd_, &tl);
    ClientToScreen(parentHwnd_, &br);
    out.left = tl.x;
    out.top = tl.y;
    out.right = br.x;
    out.bottom = br.y;
    return out;
}

void ViewportHost::paint(Gdiplus::Graphics& g, const RECT& clientRc,
                         bool connected, bool demoMode,
                         const HubViewState& hub,
                         int hoverX, int hoverY) {
    hotspots_.clear();
    ZeroMemory(&hubDeviceListRc_, sizeof(hubDeviceListRc_));
    hubDeviceMaxScrollY_ = 0;
    GdiPlusDraw::fillRect(g, clientRc, Theme::BG_CANVAS);

    if (connected) {
        // Subtle bezel frame around the embedded phone screen
        RECT bezel = {
            clientRc.left + videoRect_.x - 1,
            clientRc.top  + videoRect_.y - 1,
            clientRc.left + videoRect_.x + videoRect_.w + 1,
            clientRc.top  + videoRect_.y + videoRect_.h + 1
        };
        GdiPlusDraw::roundedCard(g, bezel, 6.0f, 0x00000000u, Theme::BORDER_SUBTLE, 1.0f);
        if (!isMirrorEmbedded() && !isNativeVideoActive()) {
            float cx = static_cast<float>(clientRc.left + clientRc.right) * 0.5f;
            float cy = static_cast<float>(clientRc.top + clientRc.bottom) * 0.5f;
            GdiPlusDraw::textCenter(g, ConnectionStateMachine::stepLabel(hub.connectStep), cx, cy,
                                    14.0f, true, Theme::ACCENT_CYAN);
        }
        return;
    }

    if (demoMode) {
        // Sleek 16:9 Offline Keymap Studio canvas bezel
        RECT vid = {
            clientRc.left + videoRect_.x,
            clientRc.top  + videoRect_.y,
            clientRc.left + videoRect_.x + videoRect_.w,
            clientRc.top  + videoRect_.y + videoRect_.h
        };
        GdiPlusDraw::roundedCard(g, vid, 10.0f, 0xFF0E131Du, Theme::BORDER_STRONG, 1.5f);
        Gdiplus::Pen gridPen(GdiPlusDraw::argb(0xFF172030u), 1.0f);
        for (int gx = vid.left + 64; gx < vid.right; gx += 64) {
            g.DrawLine(&gridPen, static_cast<float>(gx), static_cast<float>(vid.top + 2),
                       static_cast<float>(gx), static_cast<float>(vid.bottom - 2));
        }
        for (int gy = vid.top + 64; gy < vid.bottom; gy += 64) {
            g.DrawLine(&gridPen, static_cast<float>(vid.left + 2), static_cast<float>(gy),
                       static_cast<float>(vid.right - 2), static_cast<float>(gy));
        }
        float cx = static_cast<float>(vid.left + vid.right) * 0.5f;
        GdiPlusDraw::textCenter(g, "OFFLINE KEYMAP STUDIO CANVAS", cx, static_cast<float>(vid.top + 24),
                                12.0f, true, Theme::TEXT_MUTED);
        GdiPlusDraw::textCenter(g, "Drag keys directly here  |  Right-Click to add  |  Double-Click to replace  |  Use Right Drawer for settings",
                                cx, static_cast<float>(vid.bottom - 20), 11.5f, false, Theme::TEXT_MUTED);
        return;
    }

    // Disconnected / X2 / F3 Connection Hub Card centered inside ViewportHost
    int cardW = GdiPlusDraw::dp(560);
    int cardH = GdiPlusDraw::dp(468);
    int availW = clientRc.right - clientRc.left - GdiPlusDraw::dp(28);
    int availH = clientRc.bottom - clientRc.top - GdiPlusDraw::dp(28);
    if (cardW > availW) cardW = availW;
    if (cardH > availH) cardH = availH;
    if (cardW < GdiPlusDraw::dp(340)) cardW = GdiPlusDraw::dp(340);
    if (cardH < GdiPlusDraw::dp(320)) cardH = GdiPlusDraw::dp(320);

    int cx = (clientRc.left + clientRc.right) / 2;
    int cy = (clientRc.top + clientRc.bottom) / 2;
    RECT card = { cx - cardW / 2, cy - cardH / 2, cx + cardW / 2, cy + cardH / 2 };

    GdiPlusDraw::roundedCard(g, card, GdiPlusDraw::dpf(14.0f), Theme::BG_DRAWER, Theme::BORDER_STRONG, 1.5f);

    int padX = GdiPlusDraw::dp(22);
    int x = card.left + padX;
    int w = cardW - padX * 2;
    int y = card.top + GdiPlusDraw::dp(22);

    // Header
    GdiPlusDraw::drawIcon(g, ICON_LOGO, static_cast<float>(x + GdiPlusDraw::dp(12)),
                          static_cast<float>(y + GdiPlusDraw::dp(11)), GdiPlusDraw::dpf(22.0f), Theme::ACCENT_CYAN);
    GdiPlusDraw::textLeft(g, hub.showingQrPair ? "Wireless QR Pairing (Android 11+)"
                                               : Strings::tr("hub.title", "Phantom Touch Device Hub"),
                          static_cast<float>(x + GdiPlusDraw::dp(34)), static_cast<float>(y + GdiPlusDraw::dp(8)),
                          17.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - GdiPlusDraw::dp(135)));
    GdiPlusDraw::textLeft(g, hub.showingQrPair ? "Developer options -> Wireless debugging -> Pair device with QR code"
                                               : Strings::tr("hub.subtitle", "Auto-detecting USB debugging & Wireless ADB devices"),
                          static_cast<float>(x + GdiPlusDraw::dp(34)), static_cast<float>(y + GdiPlusDraw::dp(27)),
                          11.0f, false, Theme::TEXT_MUTED, static_cast<float>(w - GdiPlusDraw::dp(135)));

    RECT rRef = { card.right - padX - GdiPlusDraw::dp(92), y + GdiPlusDraw::dp(2), card.right - padX, y + GdiPlusDraw::dp(32) };
    bool hovRef = ptIn(rRef, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rRef, GdiPlusDraw::dpf(8.0f), hovRef ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovRef ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    if (hub.showingQrPair) {
        GdiPlusDraw::textCenter(g, Strings::tr("btn.back", "BACK"), static_cast<float>(rRef.left + rRef.right) * 0.5f,
                                static_cast<float>(rRef.top + rRef.bottom) * 0.5f, 12.0f, true, Theme::ACCENT_CYAN);
        hotspots_.push_back(HubHotspot{ rRef, HUB_TOGGLE_QR_PAIR, 0 });
    } else {
        GdiPlusDraw::drawIcon(g, ICON_REFRESH, static_cast<float>(rRef.left + GdiPlusDraw::dp(16)),
                              static_cast<float>(rRef.top + GdiPlusDraw::dp(15)), GdiPlusDraw::dpf(12.0f), Theme::ACCENT_CYAN);
        GdiPlusDraw::textLeft(g, Strings::tr("btn.refresh", "Refresh"), static_cast<float>(rRef.left + GdiPlusDraw::dp(30)),
                              static_cast<float>(rRef.top + GdiPlusDraw::dp(15)), 11.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(GdiPlusDraw::dp(58)));
        hotspots_.push_back(HubHotspot{ rRef, HUB_REFRESH_DEVICES, 0 });
    }
    y += GdiPlusDraw::dp(48);

    if (hub.showingQrPair) {
        const int qrBoxSz = GdiPlusDraw::dp(196);
        int qx = cx - qrBoxSz / 2;
        int qy = y + GdiPlusDraw::dp(4);
        RECT rQrBg = { qx, qy, qx + qrBoxSz, qy + qrBoxSz };
        GdiPlusDraw::roundedCard(g, rQrBg, GdiPlusDraw::dpf(10.0f), 0xFFFFFFFFu, Theme::ACCENT_CYAN, 2.0f);

        if (hub.qrMatrix && hub.qrMatrix->size > 0) {
            const int quiet = 3;
            const int totalMod = hub.qrMatrix->size + quiet * 2;
            const int cellPx = (qrBoxSz - GdiPlusDraw::dp(12)) / totalMod;
            if (cellPx >= 2) {
                uint32_t qhash = static_cast<uint32_t>(hub.qrMatrix->size);
                for (int r = 0; r < hub.qrMatrix->size; ++r) {
                    for (int c = 0; c < hub.qrMatrix->size; ++c) {
                        qhash = (qhash * 131u) + (hub.qrMatrix->at(c, r) ? 1u : 0u);
                    }
                }
                const int drawSz = cellPx * hub.qrMatrix->size;
                if (!cachedQrBmp_ || cachedQrSize_ != hub.qrMatrix->size ||
                    cachedQrCellPx_ != cellPx || cachedQrHash_ != qhash) {
                    delete cachedQrBmp_;
                    cachedQrBmp_ = new Gdiplus::Bitmap(drawSz, drawSz, PixelFormat32bppPARGB);
                    Gdiplus::Graphics qg(cachedQrBmp_);
                    qg.Clear(Gdiplus::Color(0, 0, 0, 0));
                    Gdiplus::SolidBrush darkBr(Gdiplus::Color(255, 10, 14, 22));
                    for (int r = 0; r < hub.qrMatrix->size; ++r) {
                        for (int c = 0; c < hub.qrMatrix->size; ++c) {
                            if (hub.qrMatrix->at(c, r)) {
                                qg.FillRectangle(&darkBr, static_cast<INT>(c * cellPx),
                                                 static_cast<INT>(r * cellPx),
                                                 static_cast<INT>(cellPx), static_cast<INT>(cellPx));
                            }
                        }
                    }
                    cachedQrSize_ = hub.qrMatrix->size;
                    cachedQrCellPx_ = cellPx;
                    cachedQrHash_ = qhash;
                }
                const int ox = qx + (qrBoxSz - drawSz) / 2;
                const int oy = qy + (qrBoxSz - drawSz) / 2;
                g.DrawImage(cachedQrBmp_, static_cast<INT>(ox), static_cast<INT>(oy),
                            static_cast<INT>(drawSz), static_cast<INT>(drawSz));
            }
        }

        int sy = qy + qrBoxSz + GdiPlusDraw::dp(18);
        GdiPlusDraw::textCenter(g, hub.qrStatus.empty() ? "Waiting for phone to scan QR code over Wi-Fi..." : hub.qrStatus,
                                static_cast<float>(cx), static_cast<float>(sy), 12.5f, true, Theme::ACCENT_CYAN);
        GdiPlusDraw::textCenter(g, "Both PC and phone must be on the same Wi-Fi network (mDNS auto-pair + connect).",
                                static_cast<float>(cx), static_cast<float>(sy + GdiPlusDraw::dp(20)), 11.0f, false, Theme::TEXT_MUTED);

        RECT rBack = { x, card.bottom - GdiPlusDraw::dp(50), x + w, card.bottom - GdiPlusDraw::dp(16) };
        bool hovB = ptIn(rBack, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, rBack, GdiPlusDraw::dpf(8.0f), hovB ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                                 hovB ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, 1.0f);
        GdiPlusDraw::textCenter(g, "Return to USB / Wireless Device List",
                                static_cast<float>(rBack.left + rBack.right) * 0.5f,
                                static_cast<float>(rBack.top + rBack.bottom) * 0.5f, 12.0f, true, Theme::TEXT_PRIMARY);
        hotspots_.push_back(HubHotspot{ rBack, HUB_TOGGLE_QR_PAIR, 0 });
        return;
    }

    // X2 State Card (clear message + single primary action)
    auto drawStateCard = [&](uint32_t bg, uint32_t bd, uint32_t titleCol,
                             const std::string& headline, const std::string& sub,
                             const char* priLabel, HubActionType priAct,
                             const char* secLabel, HubActionType secAct) {
        const int cardBoxH = GdiPlusDraw::dp(62);
        RECT rSt = { x, y, x + w, y + cardBoxH };
        GdiPlusDraw::roundedCard(g, rSt, GdiPlusDraw::dpf(9.0f), bg, bd, 1.2f);
        int textMaxW = w - GdiPlusDraw::dp(secLabel ? 210 : (priLabel ? 132 : 24));
        GdiPlusDraw::textLeft(g, headline, static_cast<float>(x + GdiPlusDraw::dp(12)),
                              static_cast<float>(y + GdiPlusDraw::dp(21)), 12.5f, true, titleCol, static_cast<float>(textMaxW));
        GdiPlusDraw::textLeft(g, sub, static_cast<float>(x + GdiPlusDraw::dp(12)),
                              static_cast<float>(y + GdiPlusDraw::dp(41)), 10.5f, false, Theme::TEXT_SECONDARY, static_cast<float>(textMaxW));

        int rightCursor = rSt.right - GdiPlusDraw::dp(10);
        if (priLabel && priAct != HUB_NONE) {
            int pw = GdiPlusDraw::dp(106);
            RECT rPri = { rightCursor - pw, y + GdiPlusDraw::dp(16), rightCursor, y + GdiPlusDraw::dp(46) };
            bool hovP = ptIn(rPri, hoverX, hoverY);
            GdiPlusDraw::roundedCard(g, rPri, GdiPlusDraw::dpf(7.0f), hovP ? bd : Theme::BG_CARD, bd, 1.2f);
            GdiPlusDraw::textCenter(g, priLabel, static_cast<float>(rPri.left + rPri.right) * 0.5f,
                                    static_cast<float>(rPri.top + rPri.bottom) * 0.5f,
                                    11.0f, true, hovP ? 0xFF081018u : Theme::TEXT_PRIMARY);
            hotspots_.push_back(HubHotspot{ rPri, priAct, 0 });
            rightCursor = rPri.left - GdiPlusDraw::dp(6);
        }
        if (secLabel && secAct != HUB_NONE) {
            int sw = GdiPlusDraw::dp(80);
            RECT rSec = { rightCursor - sw, y + GdiPlusDraw::dp(16), rightCursor, y + GdiPlusDraw::dp(46) };
            bool hovS = ptIn(rSec, hoverX, hoverY);
            GdiPlusDraw::roundedCard(g, rSec, GdiPlusDraw::dpf(7.0f), hovS ? Theme::BG_CARD_HOVER : Theme::BG_INPUT,
                                     Theme::BORDER_STRONG, 1.0f);
            GdiPlusDraw::textCenter(g, secLabel, static_cast<float>(rSec.left + rSec.right) * 0.5f,
                                    static_cast<float>(rSec.top + rSec.bottom) * 0.5f,
                                    10.5f, true, Theme::TEXT_SECONDARY);
            hotspots_.push_back(HubHotspot{ rSec, secAct, 0 });
        }
        y += cardBoxH + GdiPlusDraw::dp(10);
    };

    if (hub.connState == CONN_NO_TOOLS) {
        drawStateCard(Theme::ACCENT_RED_BG, Theme::ACCENT_RED, Theme::ACCENT_RED,
                      "Phantom Touch needs its phone tools.",
                      "adb.exe, scrcpy-server, and scrcpy.exe were not found in .\\scrcpy\\.",
                      "Repair", HUB_REPAIR_TOOLS, "Re-check", HUB_REFRESH_DEVICES);
    } else if (hub.connState == CONN_CONNECTING) {
        const int progH = GdiPlusDraw::dp(76);
        RECT rPr = { x, y, x + w, y + progH };
        GdiPlusDraw::roundedCard(g, rPr, GdiPlusDraw::dpf(9.0f), Theme::ACCENT_CYAN_BG, Theme::ACCENT_CYAN, 1.2f);
        static const char* kStepNames[3] = { "1. Push server", "2. Start", "3. Video" };
        int segW = (w - GdiPlusDraw::dp(32)) / 3;
        int curStepIdx = static_cast<int>(hub.connectStep);
        for (int s = 0; s < 3; ++s) {
            RECT rs = { x + GdiPlusDraw::dp(10) + s * (segW + GdiPlusDraw::dp(6)),
                        y + GdiPlusDraw::dp(10),
                        x + GdiPlusDraw::dp(10) + s * (segW + GdiPlusDraw::dp(6)) + segW,
                        y + GdiPlusDraw::dp(36) };
            bool done = (s < curStepIdx);
            bool active = (s == curStepIdx);
            uint32_t sBg = done ? Theme::ACCENT_GREEN_BG : (active ? Theme::ACCENT_CYAN_BG : Theme::BG_INPUT);
            uint32_t sBd = done ? Theme::ACCENT_GREEN : (active ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE);
            uint32_t sFg = done ? Theme::ACCENT_GREEN : (active ? Theme::ACCENT_CYAN : Theme::TEXT_MUTED);
            GdiPlusDraw::roundedCard(g, rs, GdiPlusDraw::dpf(6.0f), sBg, sBd, active ? 1.4f : 1.0f);
            GdiPlusDraw::textCenter(g, kStepNames[s], static_cast<float>(rs.left + rs.right) * 0.5f,
                                    static_cast<float>(rs.top + rs.bottom) * 0.5f, 10.5f, true, sFg);
        }
        GdiPlusDraw::textLeft(g, hub.statusMsg.empty() ? ConnectionStateMachine::stepLabel(hub.connectStep) : hub.statusMsg,
                              static_cast<float>(x + GdiPlusDraw::dp(12)),
                              static_cast<float>(y + GdiPlusDraw::dp(55)),
                              11.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - GdiPlusDraw::dp(24)));
        y += progH + GdiPlusDraw::dp(10);
    } else if (hub.connState == CONN_RECONNECTING) {
        char head[96];
        std::snprintf(head, sizeof(head), "Reconnecting in %ds (attempt %d/%d)...",
                      hub.reconnectCountdownSec > 0 ? hub.reconnectCountdownSec : 1,
                      hub.reconnectAttempt > 0 ? hub.reconnectAttempt : 1,
                      hub.maxReconnectAttempts > 0 ? hub.maxReconnectAttempts : 5);
        drawStateCard(Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, Theme::ACCENT_AMBER,
                      head, "Plug your USB cable back in or keep Wi-Fi active — profile will resume automatically.",
                      "Cancel", HUB_CANCEL_RECONNECT, "Retry Now", HUB_RETRY_RECONNECT);
    } else if (hub.connState == CONN_LOST) {
        drawStateCard(Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, Theme::ACCENT_AMBER,
                      "Connection lost.",
                      hub.statusMsg.empty() ? "Auto-reconnect timed out. Reconnect or pick another device below." : hub.statusMsg,
                      "Reconnect", HUB_RETRY_RECONNECT, "Devices", HUB_CANCEL_RECONNECT);
    } else if (hub.connState == CONN_ERROR) {
        drawStateCard(Theme::ACCENT_RED_BG, Theme::ACCENT_RED, Theme::ACCENT_RED,
                      hub.statusMsg.empty() ? "Could not start phone mirror session." : hub.statusMsg,
                      "Verify USB debugging is enabled, or copy details for the log.",
                      "Copy details", HUB_COPY_ERROR, "Retry", HUB_REFRESH_DEVICES);
    } else if (hub.connState == CONN_UNAUTHORIZED) {
        drawStateCard(Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, Theme::ACCENT_AMBER,
                      "Tap Allow on your phone.",
                      "Unlock your phone and accept the 'Allow USB debugging' prompt (auto-retries every 2s).",
                      "Retry", HUB_REFRESH_DEVICES, nullptr, HUB_NONE);
    } else if (hub.connState == CONN_NO_DEVICE) {
        drawStateCard(Theme::BG_CARD, Theme::BORDER_STRONG, Theme::TEXT_PRIMARY,
                      "Plug in your phone with USB debugging on.",
                      "Connect via USB cable or Wireless ADB below.",
                      "How to enable", HUB_OPEN_ONBOARDING, nullptr, HUB_NONE);
    }

    // Reserve fixed footer heights: Wireless ADB (64dp) + Offline Studio (48dp)
    int footerH = GdiPlusDraw::dp(116);
    int listAvailH = (card.bottom - footerH) - (y + GdiPlusDraw::dp(20));
    if (listAvailH < GdiPlusDraw::dp(58)) listAvailH = GdiPlusDraw::dp(58);

    // Section 1: Detected USB / ADB Devices (F3: scrollable, NO 3-device limit!)
    std::vector<AdbDeviceInfo> emptyDevs;
    const std::vector<AdbDeviceInfo>& devices = hub.devices ? *hub.devices : emptyDevs;
    char devHdr[64];
    std::snprintf(devHdr, sizeof(devHdr), "DETECTED DEVICES (%zu)", devices.size());
    GdiPlusDraw::textLeft(g, devHdr, static_cast<float>(x), static_cast<float>(y + GdiPlusDraw::dp(7)), 10.5f, true, Theme::TEXT_MUTED);
    y += GdiPlusDraw::dp(20);

    hubDeviceListRc_ = { x, y, x + w, y + listAvailH };

    if (devices.empty()) {
        RECT rEmpty = { x, y, x + w, y + (listAvailH < GdiPlusDraw::dp(62) ? listAvailH : GdiPlusDraw::dp(62)) };
        GdiPlusDraw::roundedCard(g, rEmpty, GdiPlusDraw::dpf(9.0f), Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
        int midY = (rEmpty.top + rEmpty.bottom) / 2;
        GdiPlusDraw::drawIcon(g, ICON_USB, static_cast<float>(x + GdiPlusDraw::dp(24)), static_cast<float>(midY),
                              GdiPlusDraw::dpf(18.0f), Theme::TEXT_MUTED);
        GdiPlusDraw::textLeft(g, "Waiting for USB or Wireless ADB devices...",
                              static_cast<float>(x + GdiPlusDraw::dp(46)), static_cast<float>(midY - GdiPlusDraw::dp(8)),
                              12.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - GdiPlusDraw::dp(56)));
        GdiPlusDraw::textLeft(g, "New devices appear automatically when plugged in.",
                              static_cast<float>(x + GdiPlusDraw::dp(46)), static_cast<float>(midY + GdiPlusDraw::dp(9)),
                              10.5f, false, Theme::TEXT_MUTED, static_cast<float>(w - GdiPlusDraw::dp(56)));
    } else {
        const int rowH = GdiPlusDraw::dp(46);
        const int rowGap = GdiPlusDraw::dp(6);
        const int totalH = static_cast<int>(devices.size()) * (rowH + rowGap) - rowGap;
        hubDeviceMaxScrollY_ = (totalH > listAvailH) ? (totalH - listAvailH) : 0;
        if (hubDeviceScrollY_ < 0) hubDeviceScrollY_ = 0;
        if (hubDeviceScrollY_ > hubDeviceMaxScrollY_) hubDeviceScrollY_ = hubDeviceMaxScrollY_;

        const int sbW = (hubDeviceMaxScrollY_ > 0) ? GdiPlusDraw::dp(8) : 0;
        Gdiplus::GraphicsState gSt = g.Save();
        g.SetClip(Gdiplus::Rect(hubDeviceListRc_.left, hubDeviceListRc_.top,
                                hubDeviceListRc_.right - hubDeviceListRc_.left,
                                hubDeviceListRc_.bottom - hubDeviceListRc_.top));

        for (size_t i = 0; i < devices.size(); ++i) {
            const AdbDeviceInfo& d = devices[i];
            int ry = y + static_cast<int>(i) * (rowH + rowGap) - hubDeviceScrollY_;
            RECT rDev = { x, ry, x + w - sbW - (sbW > 0 ? GdiPlusDraw::dp(4) : 0), ry + rowH };
            if (rDev.bottom <= hubDeviceListRc_.top || rDev.top >= hubDeviceListRc_.bottom) continue;

            bool ready = (d.state == "device");
            GdiPlusDraw::roundedCard(g, rDev, GdiPlusDraw::dpf(9.0f), Theme::BG_CARD,
                                     ready ? Theme::BORDER_STRONG : Theme::ACCENT_AMBER, 1.0f);
            GdiPlusDraw::drawIcon(g, d.wireless ? ICON_WIFI : ICON_USB,
                                  static_cast<float>(rDev.left + GdiPlusDraw::dp(20)),
                                  static_cast<float>(ry + rowH / 2),
                                  GdiPlusDraw::dpf(15.0f), ready ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER);

            std::string title = (d.model.empty() ? std::string("Android Device") : d.model) +
                                (d.wireless ? "  [Wi-Fi]" : "  [USB]");
            GdiPlusDraw::textLeft(g, title,
                                  static_cast<float>(rDev.left + GdiPlusDraw::dp(38)),
                                  static_cast<float>(ry + GdiPlusDraw::dp(15)),
                                  12.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - GdiPlusDraw::dp(155)));
            std::string sub = d.serial + "  ·  " + (ready ? "Ready" : d.state);
            GdiPlusDraw::textLeft(g, sub,
                                  static_cast<float>(rDev.left + GdiPlusDraw::dp(38)),
                                  static_cast<float>(ry + GdiPlusDraw::dp(32)),
                                  10.5f, false, ready ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER,
                                  static_cast<float>(w - GdiPlusDraw::dp(155)));

            RECT rBtn = { rDev.right - GdiPlusDraw::dp(96), ry + GdiPlusDraw::dp(8), rDev.right - GdiPlusDraw::dp(8), ry + rowH - GdiPlusDraw::dp(8) };
            bool hovB = ready && ptIn(rBtn, hoverX, hoverY) && ptIn(hubDeviceListRc_, hoverX, hoverY);
            GdiPlusDraw::roundedCard(g, rBtn, GdiPlusDraw::dpf(7.0f),
                                     ready ? (hovB ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG) : Theme::BG_TRACK,
                                     ready ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.2f);
            GdiPlusDraw::textCenter(g, ready ? "LAUNCH" : "ALLOW USB",
                                    static_cast<float>(rBtn.left + rBtn.right) * 0.5f,
                                    static_cast<float>(rBtn.top + rBtn.bottom) * 0.5f,
                                    11.0f, true, hovB ? 0xFF081018u : (ready ? Theme::TEXT_PRIMARY : Theme::TEXT_MUTED));
            if (ready) {
                RECT hit = rBtn;
                if (hit.top < hubDeviceListRc_.top) hit.top = hubDeviceListRc_.top;
                if (hit.bottom > hubDeviceListRc_.bottom) hit.bottom = hubDeviceListRc_.bottom;
                if (hit.bottom > hit.top) {
                    hotspots_.push_back(HubHotspot{ hit, HUB_CONNECT_DEVICE, static_cast<int>(i) });
                }
            }
        }
        g.Restore(gSt);

        if (hubDeviceMaxScrollY_ > 0) {
            RECT rTrack = { x + w - GdiPlusDraw::dp(6), hubDeviceListRc_.top, x + w, hubDeviceListRc_.bottom };
            GdiPlusDraw::roundedCard(g, rTrack, GdiPlusDraw::dpf(3.0f), Theme::BG_TRACK, Theme::BG_TRACK, 0.0f);
            int th = (listAvailH * listAvailH) / totalH;
            if (th < GdiPlusDraw::dp(20)) th = GdiPlusDraw::dp(20);
            int ty = hubDeviceListRc_.top + (hubDeviceScrollY_ * (listAvailH - th)) / hubDeviceMaxScrollY_;
            RECT rThumb = { rTrack.left, ty, rTrack.right, ty + th };
            GdiPlusDraw::roundedCard(g, rThumb, GdiPlusDraw::dpf(3.0f), Theme::BORDER_STRONG, Theme::BORDER_STRONG, 0.0f);
        }
    }

    // Section 2: Wireless ADB Quick Connect + Android 11+ QR Code Pairing
    y = hubDeviceListRc_.bottom + GdiPlusDraw::dp(12);
    GdiPlusDraw::textLeft(g, "WIRELESS ADB (TCP/IP & ANDROID 11+ QR PAIRING)", static_cast<float>(x), static_cast<float>(y + GdiPlusDraw::dp(7)), 10.5f, true, Theme::TEXT_MUTED);
    y += GdiPlusDraw::dp(20);

    int btnConnW = GdiPlusDraw::dp(88);
    int btnQrW = GdiPlusDraw::dp(84);
    RECT rInp = { x, y, x + w - btnConnW - btnQrW - GdiPlusDraw::dp(12), y + GdiPlusDraw::dp(34) };
    GdiPlusDraw::roundedCard(g, rInp, GdiPlusDraw::dpf(8.0f), Theme::BG_INPUT,
                             hub.editingWirelessAddr ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG,
                             hub.editingWirelessAddr ? 1.6f : 1.0f);
    GdiPlusDraw::drawIcon(g, ICON_WIFI, static_cast<float>(rInp.left + GdiPlusDraw::dp(16)),
                          static_cast<float>(y + GdiPlusDraw::dp(17)), GdiPlusDraw::dpf(13.0f), Theme::TEXT_MUTED);
    std::string dispAddr = hub.wirelessAddr + (hub.editingWirelessAddr ? "_" : "");
    GdiPlusDraw::textLeft(g, dispAddr, static_cast<float>(rInp.left + GdiPlusDraw::dp(32)),
                          static_cast<float>(y + GdiPlusDraw::dp(17)), 12.0f, false, Theme::TEXT_PRIMARY,
                          static_cast<float>(rInp.right - rInp.left - GdiPlusDraw::dp(36)));
    hotspots_.push_back(HubHotspot{ rInp, HUB_FOCUS_WIFI_ADDR, 0 });

    RECT rWBtn = { rInp.right + GdiPlusDraw::dp(6), y, rInp.right + GdiPlusDraw::dp(6) + btnConnW, y + GdiPlusDraw::dp(34) };
    bool hovW = ptIn(rWBtn, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rWBtn, GdiPlusDraw::dpf(8.0f), hovW ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovW ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "CONNECT", static_cast<float>(rWBtn.left + rWBtn.right) * 0.5f,
                            static_cast<float>(y + GdiPlusDraw::dp(17)), 11.0f, true, Theme::ACCENT_CYAN);
    hotspots_.push_back(HubHotspot{ rWBtn, HUB_CONNECT_WIFI, 0 });

    RECT rQrBtn = { rWBtn.right + GdiPlusDraw::dp(6), y, x + w, y + GdiPlusDraw::dp(34) };
    bool hovQr = ptIn(rQrBtn, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rQrBtn, GdiPlusDraw::dpf(8.0f), hovQr ? Theme::ACCENT_PURPLE : Theme::ACCENT_PURPLE_BG,
                             Theme::ACCENT_PURPLE, 1.1f);
    GdiPlusDraw::textCenter(g, "QR PAIR", static_cast<float>(rQrBtn.left + rQrBtn.right) * 0.5f,
                            static_cast<float>(y + GdiPlusDraw::dp(17)), 11.0f, true,
                            hovQr ? 0xFFFFFFFFu : Theme::ACCENT_PURPLE);
    hotspots_.push_back(HubHotspot{ rQrBtn, HUB_TOGGLE_QR_PAIR, 0 });

    // Bottom Offline Keymap Studio Button
    RECT rOff = { x, card.bottom - GdiPlusDraw::dp(48), x + w, card.bottom - GdiPlusDraw::dp(14) };
    bool hovOff = ptIn(rOff, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rOff, GdiPlusDraw::dpf(8.0f), hovOff ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovOff ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textCenter(g, "Open Offline Keymap Studio (Edit Configs Without Phone)",
                            static_cast<float>(rOff.left + rOff.right) * 0.5f,
                            static_cast<float>(rOff.top + rOff.bottom) * 0.5f,
                            11.5f, true, Theme::TEXT_SECONDARY);
    hotspots_.push_back(HubHotspot{ rOff, HUB_START_OFFLINE_STUDIO, 0 });
}

HubClickResult ViewportHost::hitTestHub(int x, int y) const {
    HubClickResult res;
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        if (ptIn(hotspots_[i].rc, x, y)) {
            res.action = hotspots_[i].act;
            res.index = hotspots_[i].extra;
            return res;
        }
    }
    return res;
}

bool ViewportHost::onHubMouseWheel(int x, int y, int delta) {
    if (hubDeviceMaxScrollY_ <= 0 || !ptIn(hubDeviceListRc_, x, y)) return false;
    int step = (delta > 0) ? -GdiPlusDraw::dp(36) : GdiPlusDraw::dp(36);
    hubDeviceScrollY_ += step;
    if (hubDeviceScrollY_ < 0) hubDeviceScrollY_ = 0;
    if (hubDeviceScrollY_ > hubDeviceMaxScrollY_) hubDeviceScrollY_ = hubDeviceMaxScrollY_;
    return true;
}

bool ViewportHost::hoverHubAt(int x, int y, int& outId, RECT& outRc) const {
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        if (ptIn(hotspots_[i].rc, x, y)) {
            outId = (static_cast<int>(hotspots_[i].act) << 8) ^ (hotspots_[i].extra & 0xFF);
            outRc = hotspots_[i].rc;
            return true;
        }
    }
    return false;
}

bool ViewportHost::rectForHubAction(HubActionType act, int extra, RECT& outRc) const {
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        if (hotspots_[i].act == act && (extra == -1 || hotspots_[i].extra == extra)) {
            outRc = hotspots_[i].rc;
            return true;
        }
    }
    return false;
}
