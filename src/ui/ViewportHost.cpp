#include "ui/ViewportHost.hpp"
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
    ZeroMemory(&hostBounds_, sizeof(hostBounds_));
}

ViewportHost::~ViewportHost() {
    detachMirror();
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = NULL;
    }
}

LRESULT CALLBACK ViewportHost::hostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(RGB(10, 13, 19));
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            HWND parent = GetParent(hwnd);
            if (parent) {
                SetFocus(parent);
                // Forward viewport clicks to parent ShellWindow if needed (e.g. Device Hub)
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                MapWindowPoints(hwnd, parent, &pt, 1);
                PostMessageA(parent, msg, wParam, MAKELPARAM(pt.x, pt.y));
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

bool ViewportHost::create(HWND parentHwnd, HINSTANCE hInst) {
    parentHwnd_ = parentHwnd;
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ViewportHost::hostWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "PhantomTouchViewportHost";
        if (!RegisterClassExA(&wc)) return false;
        classRegistered = true;
    }

    hwnd_ = CreateWindowExA(0, "PhantomTouchViewportHost", "",
                            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, 100, 100,
                            parentHwnd, NULL, hInst, NULL);
    return hwnd_ != NULL;
}

void ViewportHost::setBounds(const RECT& boundsInParent) {
    hostBounds_ = boundsInParent;
    int w = boundsInParent.right - boundsInParent.left;
    int h = boundsInParent.bottom - boundsInParent.top;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (hwnd_) {
        if (isMirrorEmbedded()) {
            ShowWindow(hwnd_, SW_SHOW);
            SetWindowPos(hwnd_, HWND_BOTTOM, boundsInParent.left, boundsInParent.top, w, h,
                         SWP_NOACTIVATE);
        } else {
            // Keep child host hidden when Device Hub or Offline Studio is drawn directly on ShellWindow
            ShowWindow(hwnd_, SW_HIDE);
        }
    }
    updateVideoLayout(lastPhoneW_, lastPhoneH_);
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
    int pad = isMirrorEmbedded() ? 4 : 16;
    videoRect_ = GdiPlusDraw::computeAspectFit(hostW, hostH, lastPhoneW_, lastPhoneH_, pad);

    if (isMirrorEmbedded()) {
        SetWindowPos(embeddedHwnd_, HWND_TOP,
                     videoRect_.x, videoRect_.y, videoRect_.w, videoRect_.h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void ViewportHost::detachMirror() {
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
                         bool connected, bool demoMode, bool connecting,
                         const std::string& statusMsg,
                         const std::vector<AdbDeviceInfo>& devices,
                         const std::string& wirelessAddr, bool editingWirelessAddr,
                         int hoverX, int hoverY) {
    hotspots_.clear();
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
        if (!isMirrorEmbedded()) {
            float cx = static_cast<float>(clientRc.left + clientRc.right) * 0.5f;
            float cy = static_cast<float>(clientRc.top + clientRc.bottom) * 0.5f;
            GdiPlusDraw::textCenter(g, "Starting embedded phone stream...", cx, cy, 14.0f, true, Theme::ACCENT_CYAN);
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
        float cy = static_cast<float>(vid.top + vid.bottom) * 0.5f;
        GdiPlusDraw::textCenter(g, "OFFLINE KEYMAP STUDIO CANVAS", cx, static_cast<float>(vid.top + 24),
                                12.0f, true, Theme::TEXT_MUTED);
        GdiPlusDraw::textCenter(g, "Drag keys directly here  |  Right-Click to add  |  Double-Click to replace  |  Use Right Drawer for settings",
                                cx, static_cast<float>(vid.bottom - 20), 11.5f, false, Theme::TEXT_MUTED);
        (void)cy;
        return;
    }

    // Disconnected State: Production Device Hub Card centered inside ViewportHost
    int cardW = 540;
    int cardH = 420;
    int availW = clientRc.right - clientRc.left - 32;
    int availH = clientRc.bottom - clientRc.top - 32;
    if (cardW > availW) cardW = availW;
    if (cardH > availH) cardH = availH;
    if (cardW < 340) cardW = 340;
    if (cardH < 320) cardH = 320;

    int cx = (clientRc.left + clientRc.right) / 2;
    int cy = (clientRc.top + clientRc.bottom) / 2;
    RECT card = { cx - cardW / 2, cy - cardH / 2, cx + cardW / 2, cy + cardH / 2 };

    GdiPlusDraw::roundedCard(g, card, 14.0f, Theme::BG_DRAWER, Theme::BORDER_STRONG, 1.5f);

    int x = card.left + 24;
    int w = cardW - 48;
    int y = card.top + 26;

    // Header
    GdiPlusDraw::drawIcon(g, ICON_LOGO, static_cast<float>(x + 12), static_cast<float>(y + 10), 22.0f, Theme::ACCENT_CYAN);
    GdiPlusDraw::textLeft(g, "Phantom Touch Device Hub", static_cast<float>(x + 34), static_cast<float>(y + 8), 18.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - 130));
    GdiPlusDraw::textLeft(g, "Connect an Android phone via USB debugging or Wireless ADB", static_cast<float>(x + 34), static_cast<float>(y + 28), 11.5f, false, Theme::TEXT_MUTED, static_cast<float>(w - 130));

    RECT rRef = { card.right - 24 - 92, y + 2, card.right - 24, y + 32 };
    bool hovRef = ptIn(rRef, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rRef, 8.0f, hovRef ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovRef ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::drawIcon(g, ICON_REFRESH, static_cast<float>(rRef.left + 16), static_cast<float>(rRef.top + 15), 12.0f, Theme::ACCENT_CYAN);
    GdiPlusDraw::textLeft(g, "Refresh", static_cast<float>(rRef.left + 30), static_cast<float>(rRef.top + 15), 12.0f, true, Theme::TEXT_PRIMARY, 58.0f);
    hotspots_.push_back(HubHotspot{ rRef, HUB_REFRESH_DEVICES, 0 });
    y += 50;

    if (!statusMsg.empty()) {
        RECT rSt = { x, y, x + w, y + 32 };
        uint32_t bg = connecting ? Theme::ACCENT_CYAN_BG : Theme::ACCENT_AMBER_BG;
        uint32_t bd = connecting ? Theme::ACCENT_CYAN : Theme::ACCENT_AMBER;
        GdiPlusDraw::roundedCard(g, rSt, 8.0f, bg, bd, 1.0f);
        GdiPlusDraw::textLeft(g, statusMsg, static_cast<float>(x + 12), static_cast<float>(y + 16), 11.5f, true,
                              connecting ? Theme::ACCENT_CYAN : Theme::ACCENT_AMBER, static_cast<float>(w - 24));
        y += 42;
    }

    // Section 1: Detected USB / ADB Devices
    GdiPlusDraw::textLeft(g, "DETECTED DEVICES", static_cast<float>(x), static_cast<float>(y + 8), 10.5f, true, Theme::TEXT_MUTED);
    y += 22;

    if (devices.empty()) {
        RECT rEmpty = { x, y, x + w, y + 66 };
        GdiPlusDraw::roundedCard(g, rEmpty, 10.0f, Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
        GdiPlusDraw::drawIcon(g, ICON_USB, static_cast<float>(x + 26), static_cast<float>(y + 33), 20.0f, Theme::TEXT_MUTED);
        GdiPlusDraw::textLeft(g, "No Android devices detected yet", static_cast<float>(x + 50), static_cast<float>(y + 24), 13.0f, true, Theme::TEXT_PRIMARY);
        GdiPlusDraw::textLeft(g, "Enable USB Debugging on your phone, plug in cable, and click Refresh.", static_cast<float>(x + 50), static_cast<float>(y + 44), 11.0f, false, Theme::TEXT_MUTED, static_cast<float>(w - 60));
        y += 78;
    } else {
        int shown = 0;
        for (size_t i = 0; i < devices.size() && shown < 3; ++i, ++shown) {
            const AdbDeviceInfo& d = devices[i];
            bool ready = (d.state == "device");
            RECT rDev = { x, y, x + w, y + 48 };
            GdiPlusDraw::roundedCard(g, rDev, 10.0f, Theme::BG_CARD, ready ? Theme::BORDER_STRONG : Theme::ACCENT_AMBER, 1.0f);
            GdiPlusDraw::drawIcon(g, d.wireless ? ICON_WIFI : ICON_USB, static_cast<float>(x + 22), static_cast<float>(y + 24), 16.0f,
                                  ready ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER);
            GdiPlusDraw::textLeft(g, d.model, static_cast<float>(x + 42), static_cast<float>(y + 17), 13.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - 155));
            std::string sub = d.serial + "  ·  " + (ready ? "Ready" : d.state);
            GdiPlusDraw::textLeft(g, sub, static_cast<float>(x + 42), static_cast<float>(y + 34), 11.0f, false,
                                  ready ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER, static_cast<float>(w - 155));

            RECT rBtn = { rDev.right - 102, y + 9, rDev.right - 10, y + 39 };
            bool hovB = ready && ptIn(rBtn, hoverX, hoverY);
            GdiPlusDraw::roundedCard(g, rBtn, 8.0f,
                                     ready ? (hovB ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG) : Theme::BG_TRACK,
                                     ready ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.2f);
            GdiPlusDraw::textCenter(g, ready ? "LAUNCH" : "ALLOW USB",
                                    static_cast<float>(rBtn.left + rBtn.right) * 0.5f,
                                    static_cast<float>(rBtn.top + rBtn.bottom) * 0.5f,
                                    11.5f, true, hovB ? 0xFF081018u : (ready ? Theme::TEXT_PRIMARY : Theme::TEXT_MUTED));
            if (ready) {
                hotspots_.push_back(HubHotspot{ rBtn, HUB_CONNECT_DEVICE, static_cast<int>(i) });
            }
            y += 54;
        }
    }

    // Section 2: Wireless ADB Quick Connect
    y += 6;
    GdiPlusDraw::textLeft(g, "WIRELESS ADB (TCP/IP)", static_cast<float>(x), static_cast<float>(y + 8), 10.5f, true, Theme::TEXT_MUTED);
    y += 22;

    RECT rInp = { x, y, x + w - 110, y + 34 };
    GdiPlusDraw::roundedCard(g, rInp, 8.0f, Theme::BG_INPUT,
                             editingWirelessAddr ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG,
                             editingWirelessAddr ? 1.6f : 1.0f);
    GdiPlusDraw::drawIcon(g, ICON_WIFI, static_cast<float>(rInp.left + 16), static_cast<float>(y + 17), 13.0f, Theme::TEXT_MUTED);
    std::string dispAddr = wirelessAddr + (editingWirelessAddr ? "_" : "");
    GdiPlusDraw::textLeft(g, dispAddr, static_cast<float>(rInp.left + 32), static_cast<float>(y + 17), 12.5f, false, Theme::TEXT_PRIMARY, static_cast<float>(w - 150));
    hotspots_.push_back(HubHotspot{ rInp, HUB_FOCUS_WIFI_ADDR, 0 });

    RECT rWBtn = { rInp.right + 8, y, x + w, y + 34 };
    bool hovW = ptIn(rWBtn, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rWBtn, 8.0f, hovW ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovW ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "CONNECT", static_cast<float>(rWBtn.left + rWBtn.right) * 0.5f, static_cast<float>(y + 17), 12.0f, true, Theme::ACCENT_CYAN);
    hotspots_.push_back(HubHotspot{ rWBtn, HUB_CONNECT_WIFI, 0 });

    // Bottom Offline Keymap Studio Button
    RECT rOff = { x, card.bottom - 52, x + w, card.bottom - 18 };
    bool hovOff = ptIn(rOff, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rOff, 8.0f, hovOff ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovOff ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textCenter(g, "Open Offline Keymap Studio (Edit Configs Without Phone)",
                            static_cast<float>(rOff.left + rOff.right) * 0.5f,
                            static_cast<float>(rOff.top + rOff.bottom) * 0.5f,
                            12.0f, true, Theme::TEXT_SECONDARY);
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
