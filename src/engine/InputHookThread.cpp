#include "engine/InputHookThread.hpp"

#include "config/KeyCodes.hpp"
#include "engine/InputEngine.hpp"
#include "engine/ThreadScheduling.hpp"

#include <cstring>

namespace {

InputHookThread* g_hookThreadInstance = nullptr;

float clamp01(float v) {
    if (v < 0.01f) return 0.01f;
    if (v > 0.99f) return 0.99f;
    return v;
}

bool isAppForegroundSnap(const InputHookSnapshot& s) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == s.shellHwnd) return true;
    if (s.hudHwnd && fg == s.hudHwnd) return true;
    if (s.viewportHwnd && fg == s.viewportHwnd) return true;
    if (s.embeddedHwnd && fg == s.embeddedHwnd) return true;
    return false;
}

bool pointInVideoSnap(const InputHookSnapshot& s, POINT pt, float& outNormX, float& outNormY) {
    const RECT& rc = s.videoScreenRect;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    if (pt.x < rc.left || pt.x >= rc.right || pt.y < rc.top || pt.y >= rc.bottom) {
        return false;
    }
    outNormX = static_cast<float>(pt.x - rc.left) / static_cast<float>(w);
    outNormY = static_cast<float>(pt.y - rc.top)  / static_cast<float>(h);
    return true;
}

} // namespace

InputHookThread::InputHookThread() {
    InitializeCriticalSection(&snapLock_);
    std::memset(hookKeyDown_, 0, sizeof(hookKeyDown_));
}

InputHookThread::~InputHookThread() {
    stop();
    DeleteCriticalSection(&snapLock_);
}

bool InputHookThread::start(HINSTANCE hInst, InputEngine* engine, const InputHookSnapshot& initialSnap) {
    if (thread_ != NULL) return true;
    hInst_ = hInst;
    engine_ = engine;
    updateSnapshot(initialSnap);
    std::memset(hookKeyDown_, 0, sizeof(hookKeyDown_));
    toggleAimKeyDown_ = false;
    holdUnlockedAim_ = false;
    cursorDirectDragging_ = false;
    rightClickBackSwallowUp_ = false;

    readyEvent_ = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread_ = CreateThread(NULL, 0, threadProc, this, 0, &threadId_);
    if (!thread_) {
        if (readyEvent_) {
            CloseHandle(readyEvent_);
            readyEvent_ = NULL;
        }
        return false;
    }
    SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST);
    if (readyEvent_) {
        WaitForSingleObject(readyEvent_, 1000);
        CloseHandle(readyEvent_);
        readyEvent_ = NULL;
    }
    return true;
}

void InputHookThread::stop() {
    if (!thread_) return;
    if (threadId_ != 0) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    }
    WaitForSingleObject(thread_, 1200);
    CloseHandle(thread_);
    thread_ = NULL;
    threadId_ = 0;
}

void InputHookThread::updateSnapshot(const InputHookSnapshot& snap) {
    EnterCriticalSection(&snapLock_);
    snap_ = snap;
    LeaveCriticalSection(&snapLock_);
}

void InputHookThread::readSnapshot(InputHookSnapshot& out) const {
    EnterCriticalSection(&snapLock_);
    out = snap_;
    LeaveCriticalSection(&snapLock_);
}

DWORD WINAPI InputHookThread::threadProc(LPVOID param) {
    static_cast<InputHookThread*>(param)->runLoop();
    return 0;
}

LRESULT CALLBACK InputHookThread::rawWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    InputHookThread* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<InputHookThread*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<InputHookThread*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self && msg == WM_INPUT) {
        self->handleRawInput(wParam, lParam);
        if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK InputHookThread::lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (g_hookThreadInstance) {
        return g_hookThreadInstance->handleMouseHook(nCode, wParam, lParam);
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK InputHookThread::lowLevelKeyProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (g_hookThreadInstance) {
        return g_hookThreadInstance->handleKeyHook(nCode, wParam, lParam);
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void InputHookThread::runLoop() {
    DWORD taskIdx = 0;
    HANDLE avrt = ThreadScheduling::enableMmcssGames(taskIdx);
    g_hookThreadInstance = this;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InputHookThread::rawWndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = L"PhantomTouchRawInputClass";
    RegisterClassExW(&wc);

    rawHwnd_ = CreateWindowExW(0, L"PhantomTouchRawInputClass", L"", 0,
                               0, 0, 0, 0, HWND_MESSAGE, NULL, hInst_, this);

    if (rawHwnd_) {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x02;
        rid.dwFlags     = RIDEV_INPUTSINK;
        rid.hwndTarget  = rawHwnd_;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, lowLevelMouseProc, hInst_, 0);
    keyHook_   = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyProc, hInst_, 0);

    // Ensure message queue exists before signaling caller
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    if (readyEvent_) SetEvent(readyEvent_);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mouseHook_) { UnhookWindowsHookEx(mouseHook_); mouseHook_ = NULL; }
    if (keyHook_)   { UnhookWindowsHookEx(keyHook_);   keyHook_ = NULL; }
    if (rawHwnd_) {
        DestroyWindow(rawHwnd_);
        rawHwnd_ = NULL;
    }
    if (g_hookThreadInstance == this) g_hookThreadInstance = nullptr;
    ThreadScheduling::disableMmcss(avrt);
}

void InputHookThread::handleRawInput(WPARAM /*wParam*/, LPARAM lParam) {
    InputHookSnapshot snap;
    readSnapshot(snap);
    if (!snap.aimMode || !engine_) return;

    LONG sumDx = 0;
    LONG sumDy = 0;

    UINT dwSize = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
    if (dwSize > 0 && dwSize <= 256) {
        BYTE rawBuf[256];
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, rawBuf, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
            const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(rawBuf);
            if (raw->header.dwType == RIM_TYPEMOUSE &&
                (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                sumDx += raw->data.mouse.lLastX;
                sumDy += raw->data.mouse.lLastY;
            }
        }
    }

    // Batch any high-rate (1-8 kHz) buffered mouse packets sitting in the thread queue!
    for (int pass = 0; pass < 4; ++pass) {
        alignas(8) BYTE buf[2048];
        UINT cbSize = sizeof(buf);
        UINT n = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(buf), &cbSize, sizeof(RAWINPUTHEADER));
        if (n == 0 || n == static_cast<UINT>(-1)) break;

        PRAWINPUT cur = reinterpret_cast<PRAWINPUT>(buf);
        const uintptr_t bufEnd = reinterpret_cast<uintptr_t>(buf + sizeof(buf));
        for (UINT i = 0; i < n; ++i) {
            if (reinterpret_cast<uintptr_t>(cur) + sizeof(RAWINPUTHEADER) > bufEnd) break;
            if (cur->header.dwType == RIM_TYPEMOUSE &&
                (cur->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                sumDx += cur->data.mouse.lLastX;
                sumDy += cur->data.mouse.lLastY;
            }
            if (cur->header.dwSize < sizeof(RAWINPUTHEADER)) break;
            uintptr_t nextAddr = (reinterpret_cast<uintptr_t>(cur) + cur->header.dwSize + 7u) &
                                 ~static_cast<uintptr_t>(7u);
            if (nextAddr >= bufEnd) break;
            cur = reinterpret_cast<PRAWINPUT>(nextAddr);
        }
    }

    if (sumDx != 0 || sumDy != 0) {
        engine_->onRawMouseDelta(static_cast<int>(sumDx), static_cast<int>(sumDy));
    }
}

LRESULT InputHookThread::handleMouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !lParam) {
        return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
    }

    InputHookSnapshot s;
    readSnapshot(s);

    const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (s.editMode || s.waitingSpecialRebind) {
        return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
    }

    int mouseVk = 0;
    bool mouseDown = false;
    bool mouseUp = false;
    switch (wParam) {
        case WM_LBUTTONDOWN: mouseVk = VK_LBUTTON; mouseDown = true; break;
        case WM_LBUTTONUP:   mouseVk = VK_LBUTTON; mouseUp = true; break;
        case WM_RBUTTONDOWN: mouseVk = VK_RBUTTON; mouseDown = true; break;
        case WM_RBUTTONUP:   mouseVk = VK_RBUTTON; mouseUp = true; break;
        case WM_MBUTTONDOWN: mouseVk = VK_MBUTTON; mouseDown = true; break;
        case WM_MBUTTONUP:   mouseVk = VK_MBUTTON; mouseUp = true; break;
        case WM_XBUTTONDOWN: mouseVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2; mouseDown = true; break;
        case WM_XBUTTONUP:   mouseVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2; mouseUp = true; break;
        case WM_MOUSEWHEEL: {
            short delta = static_cast<short>(HIWORD(ms->mouseData));
            mouseVk = (delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN;
            mouseDown = true;
            break;
        }
        default: break;
    }

    const bool fg = isAppForegroundSnap(s);
    if (mouseVk != 0 && (s.aimMode || holdUnlockedAim_ || fg)) {
        if (s.holdUnlockVk != 0 && mouseVk == s.holdUnlockVk) {
            if (mouseDown && s.aimMode && !holdUnlockedAim_) {
                holdUnlockedAim_ = true;
                if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_UNLOCK_AIM, 0, 0);
                return 1;
            }
            if (mouseUp && holdUnlockedAim_) {
                holdUnlockedAim_ = false;
                if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_LOCK_AIM, 0, 0);
                return 1;
            }
        }
        if (mouseVk == s.toggleAimVk && (s.connected || s.demoMode)) {
            if (mouseDown) {
                if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_TOGGLE_AIM, 0, 0);
                return 1;
            }
            if (mouseUp) return 1;
        }
    }

    if (s.aimMode) {
        if (!fg) return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
        if (mouseVk != 0 && engine_) {
            bool reqToggle = false;
            engine_->onKeyEvent(mouseVk, mouseDown, reqToggle);
            if (reqToggle && s.shellHwnd) PostMessageW(s.shellHwnd, MSG_TOGGLE_AIM, 0, 0);
            return 1;
        }
    } else if (s.connected && engine_) {
        float nx = 0.0f, ny = 0.0f;
        bool inVideo = pointInVideoSnap(s, ms->pt, nx, ny);

        // Mouse-bound CURSOR_TOGGLE only fires inside video rect (never blocks sidebar/drawer clicks)
        if (inVideo && mouseVk != 0 && mouseVk != VK_LBUTTON && mouseDown && fg) {
            bool reqToggle = false;
            bool consumed = engine_->onKeyEvent(mouseVk, true, reqToggle);
            if (reqToggle && s.shellHwnd) PostMessageW(s.shellHwnd, MSG_TOGGLE_AIM, 0, 0);
            if (consumed) return 1;
        }

        if (!s.nativeVideoActive) {
            if (wParam == WM_MOUSEMOVE && inVideo && fg) {
                engine_->onCursorHoverNorm(nx, ny);
            }
            if (wParam == WM_LBUTTONDOWN && inVideo && fg) {
                cursorDirectDragging_ = true;
                engine_->onDirectCursorTouch(TOUCH_DOWN, nx, ny);
                return 1;
            } else if (wParam == WM_MOUSEMOVE && cursorDirectDragging_) {
                if (inVideo) engine_->onDirectCursorTouch(TOUCH_MOVE, nx, ny);
            } else if (wParam == WM_LBUTTONUP && cursorDirectDragging_) {
                cursorDirectDragging_ = false;
                engine_->onDirectCursorTouch(TOUCH_UP, clamp01(nx), clamp01(ny));
                return 1;
            } else if (wParam == WM_MBUTTONDOWN && inVideo && fg) {
                middlePanDragging_ = true;
                engine_->onDirectCursorTwoFingerPan(TOUCH_DOWN, nx, ny);
                return 1;
            } else if (wParam == WM_MOUSEMOVE && middlePanDragging_) {
                if (inVideo) engine_->onDirectCursorTwoFingerPan(TOUCH_MOVE, nx, ny);
            } else if (wParam == WM_MBUTTONUP && middlePanDragging_) {
                middlePanDragging_ = false;
                engine_->onDirectCursorTwoFingerPan(TOUCH_UP, clamp01(nx), clamp01(ny));
                return 1;
            } else if (wParam == WM_RBUTTONDOWN && s.rightClickBack && inVideo && fg) {
                rightClickBackSwallowUp_ = true;
                if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_ANDROID_BACK, 0, 0);
                return 1;
            } else if (wParam == WM_RBUTTONUP && rightClickBackSwallowUp_) {
                rightClickBackSwallowUp_ = false;
                return 1;
            } else if (wParam == WM_MOUSEWHEEL && inVideo && fg) {
                short rawDelta = static_cast<short>(HIWORD(ms->mouseData));
                DWORD nowT = GetTickCount();
                if ((rawDelta > 0 && wheelDeltaAccum_ < 0) ||
                    (rawDelta < 0 && wheelDeltaAccum_ > 0) ||
                    (nowT - lastWheelTick_) > 350u) {
                    wheelDeltaAccum_ = 0;
                }
                lastWheelTick_ = nowT;
                wheelDeltaAccum_ += static_cast<int>(rawDelta);
                int notches = wheelDeltaAccum_ / WHEEL_DELTA;
                wheelDeltaAccum_ -= notches * WHEEL_DELTA;
                if (notches != 0) {
                    bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (ctrlHeld) {
                        engine_->onDirectCursorPinch(nx, ny, notches);
                    } else {
                        engine_->onDirectCursorScroll(nx, ny, notches);
                    }
                }
                return 1;
            }
        }
    }

    return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
}

LRESULT InputHookThread::handleKeyHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !lParam) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    InputHookSnapshot s;
    readSnapshot(s);

    const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    int vk = static_cast<int>(kb->vkCode);
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!down && !up) return CallNextHookEx(keyHook_, nCode, wParam, lParam);

    bool isRepeat = false;
    if (vk >= 0 && vk < 256) {
        if (down) { isRepeat = hookKeyDown_[vk]; hookKeyDown_[vk] = true; }
        else if (up) { hookKeyDown_[vk] = false; }
    }

    const bool fg = isAppForegroundSnap(s);

    if (up && (KeyCodes::matches(vk, s.toggleAimVk) || vk == VK_F1)) {
        bool wasDown = toggleAimKeyDown_;
        toggleAimKeyDown_ = false;
        if (wasDown && !s.editMode && (s.aimMode || vk == VK_F1 || (s.keymapEnabled && fg))) return 1;
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }
    if (up && holdUnlockedAim_ && KeyCodes::matches(vk, s.holdUnlockVk)) {
        holdUnlockedAim_ = false;
        if (!s.editMode && fg) {
            if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_LOCK_AIM, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }
    if (down && (vk == VK_LWIN || vk == VK_RWIN)) {
        if (s.aimMode && s.shellHwnd) PostMessageW(s.shellHwnd, MSG_UNLOCK_AIM, 0, 0);
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (!s.aimMode && !fg) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    bool altDown = ((kb->flags & LLKHF_ALTDOWN) != 0) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    // X5 / X6: Global Command Palette (Ctrl+K) & Binding Cheat Sheet (Ctrl+/)
    if (fg && down && ctrlDown && !shiftDown && !altDown && !s.waitingSpecialRebind && s.shellHwnd) {
        if (vk == 'K') {
            if (!isRepeat) PostMessageW(s.shellHwnd, MSG_TOGGLE_CMD_PALETTE, 0, 0);
            return 1;
        }
        if (vk == VK_OEM_2 && (!s.uiCapturingText || s.popupOpen)) {
            if (!isRepeat) PostMessageW(s.shellHwnd, MSG_TOGGLE_CHEAT_SHEET, 0, 0);
            return 1;
        }
    }

    if (s.editMode || s.uiCapturingText) {
        if (down && !isRepeat && vk == VK_F2 && !s.uiCapturingText && s.shellHwnd) {
            PostMessageW(s.shellHwnd, MSG_TOGGLE_STUDIO, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    // Sprint 1 diagnostic hotkeys (Ctrl+Shift+D/L/T) always available in foreground
    if (down && !isRepeat && ctrlDown && shiftDown && !altDown && s.shellHwnd) {
        if (vk == 'D') { PostMessageW(s.shellHwnd, MSG_TOGGLE_DEBUG_HUD, 0, 0); return 1; }
        if (vk == 'L') { PostMessageW(s.shellHwnd, MSG_RUN_LATENCY_PROBE, 0, 0); return 1; }
        if (vk == 'T') { PostMessageW(s.shellHwnd, MSG_INJECT_VIDEO_STALL, 0, 0); return 1; }
    }

    if (!s.connected && !s.demoMode) {
        if (down && !isRepeat && !altDown && vk == VK_F11 && s.shellHwnd) {
            PostMessageW(s.shellHwnd, MSG_TOGGLE_FULLSCREEN, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (s.keymapEnabled || s.aimMode) {
        if (KeyCodes::matches(vk, s.holdUnlockVk)) {
            if (down) {
                if (!isRepeat && s.aimMode && !holdUnlockedAim_) {
                    holdUnlockedAim_ = true;
                    if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_UNLOCK_AIM, 0, 0);
                }
                if (holdUnlockedAim_) return 1;
            }
        }

        if (!altDown && (KeyCodes::matches(vk, s.toggleAimVk) || vk == VK_F1)) {
            if (down) {
                if (!isRepeat && !toggleAimKeyDown_) {
                    toggleAimKeyDown_ = true;
                    if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_TOGGLE_AIM, 0, 0);
                }
                return 1;
            }
        }
    } else if (!altDown && vk == VK_F1 && down) {
        if (!isRepeat && !toggleAimKeyDown_) {
            toggleAimKeyDown_ = true;
            if (s.shellHwnd) PostMessageW(s.shellHwnd, MSG_TOGGLE_AIM, 0, 0);
        }
        return 1;
    }

    if (down && isRepeat) {
        if (!altDown && (s.aimMode || (engine_ && engine_->isKeyMapped(vk, false)))) return 1;
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    // E1: In unlocked Cursor Mode, allow pure Ctrl+C/V/X/A/Z (never match AltGr as Ctrl!)
    if (!s.aimMode && KeyCodes::isPureCtrlShortcut(ctrlDown, altDown)) {
        if (vk == 'C' || vk == 'V' || vk == 'X' || vk == 'A' || vk == 'Z') {
            return CallNextHookEx(keyHook_, nCode, wParam, lParam);
        }
    }

    // N31: Do NOT intercept fixed hotkeys while Alt is held (so Alt+F4 closes cleanly!)
    if (down && !altDown && s.shellHwnd) {
        if (vk == VK_F2)  { PostMessageW(s.shellHwnd, MSG_TOGGLE_STUDIO, 0, 0); return 1; }
        if (vk == VK_F3)  { PostMessageW(s.shellHwnd, MSG_CYCLE_FIRE, 0, 0); return 1; }
        if (vk == VK_F4)  { PostMessageW(s.shellHwnd, MSG_TOGGLE_HUD, 0, 0); return 1; }
        if (vk == VK_F11) { PostMessageW(s.shellHwnd, MSG_TOGGLE_FULLSCREEN, 0, 0); return 1; }
        if (vk == s.resetTouchVk) { PostMessageW(s.shellHwnd, MSG_RESET_TOUCH, 0, 0); return 1; }
        if (s.keymapEnabled) {
            if (vk == VK_OEM_4)     { PostMessageW(s.shellHwnd, MSG_SENS_X_DOWN, 0, 0); return 1; }
            if (vk == VK_OEM_6)     { PostMessageW(s.shellHwnd, MSG_SENS_X_UP, 0, 0); return 1; }
            if (vk == VK_OEM_MINUS) { PostMessageW(s.shellHwnd, MSG_SENS_Y_DOWN, 0, 0); return 1; }
            if (vk == VK_OEM_PLUS)  { PostMessageW(s.shellHwnd, MSG_SENS_Y_UP, 0, 0); return 1; }
        }
    }

    if (altDown && !s.aimMode) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (engine_) {
        bool reqToggle = false;
        bool consumed = engine_->onKeyEvent(vk, down, reqToggle);
        if (reqToggle && s.shellHwnd) PostMessageW(s.shellHwnd, MSG_TOGGLE_AIM, 0, 0);
        if (consumed) return 1;
    }

    return CallNextHookEx(keyHook_, nCode, wParam, lParam);
}
