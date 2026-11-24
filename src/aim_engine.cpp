#include "aim_engine.hpp"

#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {

static const uint64_t PTR_JOYSTICK  = 1;
static const uint64_t PTR_AIM_A     = 2;
static const uint64_t PTR_FIRE      = 3;
static const uint64_t PTR_AIM_B     = 4;
static const uint64_t PTR_HOLD_VIEW = 5;
static const uint64_t PTR_CURSOR    = 9;
static const uint64_t PTR_BTN_BASE  = 20;

double getTimeMs() {
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (static_cast<double>(now.QuadPart) * 1000.0) / static_cast<double>(freq.QuadPart);
}

double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool vkMatch(int evVk, int targetVk) {
    if (targetVk == 0) return false;
    if (evVk == targetVk) return true;
    if (targetVk == VK_SHIFT   && (evVk == VK_LSHIFT   || evVk == VK_RSHIFT))   return true;
    if (targetVk == VK_CONTROL && (evVk == VK_LCONTROL || evVk == VK_RCONTROL)) return true;
    if (targetVk == VK_MENU    && (evVk == VK_LMENU    || evVk == VK_RMENU))    return true;
    return false;
}

bool isMouseVk(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
           vk == VK_XBUTTON1 || vk == VK_XBUTTON2 ||
           vk == VK_CUSTOM_MWHEEL_UP || vk == VK_CUSTOM_MWHEEL_DOWN;
}

} // namespace

void AimEngine::fpsViewEmitBridge(void* ctx, TouchAction action, uint64_t pointerId,
                                  int32_t px, int32_t py, bool clampToScreen) {
    ScrcpyBridge* br = reinterpret_cast<ScrcpyBridge*>(ctx);
    if (br) {
        br->sendTouchPixels(action, pointerId, px, py, clampToScreen);
    }
}

void AimEngine::syncFpsViewConfigLocked(uint32_t w, uint32_t h) {
    if (w == 0) w = 1920;
    if (h == 0) h = 1080;
    double dw = static_cast<double>(w);
    double dh = static_cast<double>(h);

    FpsViewConfig cfg;
    cfg.screenW = dw;
    cfg.screenH = dh;
    cfg.centerX = static_cast<double>(profile_.aimX) * dw;
    cfg.centerY = static_cast<double>(profile_.aimY) * dh;
    cfg.sensX = static_cast<double>(profile_.sensX);
    cfg.sensY = static_cast<double>(profile_.sensY);
    cfg.ffCounterAccel = static_cast<double>(profile_.ffCounterAccel);
    cfg.invertY = profile_.invertY;
    cfg.unbounded = profile_.unboundedAim;
    cfg.clampInsteadOfRelease = false;
    cfg.touchDownDelayMs = (profile_.touchDownDelayMs >= 15) ? profile_.touchDownDelayMs : 15;
    cfg.touchUpDelayMs = profile_.touchUpDelayMs;
    cfg.idleReleaseMs = profile_.idleResetMs;
    cfg.edgeReleaseOnly = (profile_.idleResetMs <= 0);

    if (profile_.unboundedAim) {
        cfg.rectLeft   = -99999999.0;
        cfg.rectTop    = -99999999.0;
        cfg.rectRight  =  99999999.0;
        cfg.rectBottom =  99999999.0;
        cfg.touchDownRandomRangePx = 0; // Exact center landing in Free Fire
    } else {
        // Bounded 360° box safely inset by 50px from physical screen edges (with ±50px landing jitter)
        double margin = 50.0;
        double minX = clampd(cfg.centerX - static_cast<double>(profile_.aimHalfW) * dw, margin, dw - margin);
        double maxX = clampd(cfg.centerX + static_cast<double>(profile_.aimHalfW) * dw, margin, dw - margin);
        double minY = clampd(cfg.centerY - static_cast<double>(profile_.aimHalfH) * dh, margin, dh - margin);
        double maxY = clampd(cfg.centerY + static_cast<double>(profile_.aimHalfH) * dh, margin, dh - margin);
        if (maxX - minX < 120.0) { minX = margin; maxX = dw - margin; }
        if (maxY - minY < 120.0) { minY = margin; maxY = dh - margin; }
        cfg.rectLeft   = minX;
        cfg.rectTop    = minY;
        cfg.rectRight  = maxX;
        cfg.rectBottom = maxY;
        cfg.touchDownRandomRangePx = 50;
    }

    viewStack_.configureMain(cfg);
    lastConfiguredW_ = w;
    lastConfiguredH_ = h;
}

AimEngine::AimEngine(ScrcpyBridge* bridge)
    : bridge_(bridge),
      viewStack_(PTR_AIM_A, PTR_AIM_B, PTR_HOLD_VIEW) {
    InitializeCriticalSection(&lock_);
    InitializeCriticalSection(&inputLock_);
    wakeEvent_ = CreateEventA(NULL, FALSE, FALSE, NULL);
    std::memset(heldKeys_, 0, sizeof(heldKeys_));
    std::memset(hookHeldKeys_, 0, sizeof(hookHeldKeys_));
    profile_ = createDefaultFreeFireProfile();
    btnState_.resize(profile_.buttons.size());
    viewStack_.setTouchSink(fpsViewEmitBridge, bridge_);
    uint32_t w = 1920, h = 1080;
    if (bridge_) bridge_->getScreenSize(w, h);
    syncFpsViewConfigLocked(w, h);
    EnterCriticalSection(&inputLock_);
    rebuildKeyRouteCacheLocked();
    LeaveCriticalSection(&inputLock_);
}

AimEngine::~AimEngine() {
    stop();
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = NULL;
    }
    DeleteCriticalSection(&inputLock_);
    DeleteCriticalSection(&lock_);
}

void AimEngine::rebuildKeyRouteCacheLocked() {
    routeVkUp_     = profile_.vkUp;
    routeVkDown_   = profile_.vkDown;
    routeVkLeft_   = profile_.vkLeft;
    routeVkRight_  = profile_.vkRight;
    routeVkSprint_ = profile_.vkSprint;
    routeVkWalk_   = profile_.vkWalk;
    routeFireVk_   = profile_.fireVk;
    routeButtons_.clear();
    routeButtons_.reserve(profile_.buttons.size());
    for (size_t i = 0; i < profile_.buttons.size(); ++i) {
        KeyRouteEntry e;
        e.comboVk = profile_.buttons[i].comboVk;
        e.vk      = profile_.buttons[i].vk;
        e.action  = profile_.buttons[i].action;
        routeButtons_.push_back(e);
    }
}

void AimEngine::start() {
    if (thread_ != NULL) return;
    InterlockedExchange(&stopFlag_, 0);
    thread_ = CreateThread(NULL, 0, tickThreadProc, this, 0, NULL);
    if (thread_) {
        SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST);
    }
}

void AimEngine::stop() {
    if (thread_ == NULL) return;
    InterlockedExchange(&stopFlag_, 1);
    if (wakeEvent_) SetEvent(wakeEvent_);
    WaitForSingleObject(thread_, 1500);
    CloseHandle(thread_);
    thread_ = NULL;
    releaseAllTouches();
}

void AimEngine::setProfile(const Profile& profile) {
    EnterCriticalSection(&lock_);
    releaseAllLocked();
    profile_ = profile;
    btnState_.assign(profile_.buttons.size(), ButtonRuntime());
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    syncFpsViewConfigLocked(w, h);
    EnterCriticalSection(&inputLock_);
    pendingKeys_.clear();
    pendingCursor_.clear();
    std::memset(hookHeldKeys_, 0, sizeof(hookHeldKeys_));
    rebuildKeyRouteCacheLocked();
    LeaveCriticalSection(&inputLock_);
    LeaveCriticalSection(&lock_);
}

Profile AimEngine::getProfileCopy() const {
    EnterCriticalSection(&lock_);
    Profile copy = profile_;
    LeaveCriticalSection(&lock_);
    return copy;
}

void AimEngine::setAimMode(bool enabled) {
    bool wasEnabled = (aimMode_ != 0);
    if (wasEnabled == enabled) return;

    double nowMs = getTimeMs();
    EnterCriticalSection(&lock_);
    // Only release mouse/aim/fire/cursor fingers — KEEP WASD and held keyboard keys alive!
    // That way unlocking the mouse to loot/click NEVER interrupts running or disables the keyboard!
    releaseMouseTouchesLocked();
    InterlockedExchange(&aimMode_, enabled ? 1 : 0);
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    syncFpsViewConfigLocked(w, h);
    if (enabled) {
        viewStack_.onFocusGain(nowMs);
    } else {
        viewStack_.onFocusLose(nowMs);
    }
    EnterCriticalSection(&inputLock_);
    accumRawDx_ = 0.0;
    accumRawDy_ = 0.0;
    pendingCursor_.clear();
    LeaveCriticalSection(&inputLock_);
    smoothDx_ = 0.0;
    smoothDy_ = 0.0;
    scopeSensActive_ = false;
    LeaveCriticalSection(&lock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void AimEngine::onRawMouseDelta(int dx, int dy) {
    if (!aimMode_) return;
    EnterCriticalSection(&inputLock_);
    accumRawDx_ += static_cast<double>(dx);
    accumRawDy_ += static_cast<double>(dy);
    LeaveCriticalSection(&inputLock_);
}

void AimEngine::onDirectCursorTouch(TouchAction action, float normX, float normY) {
    if (aimMode_) return;
    if (thread_ == NULL) {
        EnterCriticalSection(&lock_);
        processDirectCursorLocked(action, normX, normY);
        LeaveCriticalSection(&lock_);
        return;
    }
    EnterCriticalSection(&inputLock_);
    // Coalesce consecutive TOUCH_MOVE cursor events to keep cursor dragging ultra-fresh
    if (action == TOUCH_MOVE && !pendingCursor_.empty() && pendingCursor_.back().action == TOUCH_MOVE) {
        pendingCursor_.back().normX = normX;
        pendingCursor_.back().normY = normY;
    } else {
        QueuedCursorTouch q = { action, normX, normY };
        pendingCursor_.push_back(q);
    }
    LeaveCriticalSection(&inputLock_);
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void AimEngine::processDirectCursorLocked(TouchAction action, float normX, float normY) {
    if (action == TOUCH_DOWN) {
        directCursorDown_ = true;
        bridge_->sendTouchNorm(TOUCH_DOWN, PTR_CURSOR, normX, normY);
        ++packetsThisSec_;
    } else if (action == TOUCH_MOVE) {
        if (directCursorDown_) {
            bridge_->sendTouchNorm(TOUCH_MOVE, PTR_CURSOR, normX, normY);
            ++packetsThisSec_;
        }
    } else if (action == TOUCH_UP) {
        if (directCursorDown_) {
            directCursorDown_ = false;
            bridge_->sendTouchNorm(TOUCH_UP, PTR_CURSOR, normX, normY);
            ++packetsThisSec_;
        }
    }
}

void AimEngine::sendTapWithJitterLocked(TouchAction action, uint64_t ptrId, float nx, float ny,
                                        int32_t& ioPx, int32_t& ioPy) {
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    if (action == TOUCH_DOWN) {
        int32_t px = static_cast<int32_t>(nx * static_cast<float>(w) + 0.5f);
        int32_t py = static_cast<int32_t>(ny * static_cast<float>(h) + 0.5f);
        int j = profile_.tapJitterPx;
        if (j > 0) {
            rngState_ = rngState_ * 1664525u + 1013904223u;
            int jx = static_cast<int>((rngState_ >> 16) % static_cast<uint32_t>(2 * j + 1)) - j;
            rngState_ = rngState_ * 1664525u + 1013904223u;
            int jy = static_cast<int>((rngState_ >> 16) % static_cast<uint32_t>(2 * j + 1)) - j;
            px += jx;
            py += jy;
        }
        ioPx = px;
        ioPy = py;
    }
    bridge_->sendTouchPixels(action, ptrId, ioPx, ioPy, true);
    ++packetsThisSec_;
}

void AimEngine::triggerButtonStartLocked(size_t idx, double nowMs, bool& outRequestToggleAim) {
    const KeyNode& b = profile_.buttons[idx];
    ButtonRuntime& st = btnState_[idx];
    if (st.keyHeld) return;
    st.keyHeld = true;

    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);

    switch (b.action) {
        case NODE_PRESS: {
            bool isWhl = (b.vk == VK_CUSTOM_MWHEEL_UP || b.vk == VK_CUSTOM_MWHEEL_DOWN);
            if (isWhl && st.touchDown) {
                // Fast multi-notch wheel scroll: lift previous tap cleanly and re-tap immediately!
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
                st.touchDown = false;
                ++packetsThisSec_;
            }
            if (!st.touchDown) {
                sendTapWithJitterLocked(TOUCH_DOWN, PTR_BTN_BASE + idx, b.x, b.y, st.lastPx, st.lastPy);
                st.touchDown = true;
                st.autoReleaseAtMs = isWhl ? (nowMs + 35.0) : 0.0;
            }
            if (b.switchScopeSens) scopeSensActive_ = !scopeSensActive_;
            break;
        }
        case NODE_CLICK: {
            if (st.touchDown) {
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
                ++packetsThisSec_;
            }
            sendTapWithJitterLocked(TOUCH_DOWN, PTR_BTN_BASE + idx, b.x, b.y, st.lastPx, st.lastPy);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            if (b.switchScopeSens) scopeSensActive_ = !scopeSensActive_;
            break;
        }
        case NODE_DOWN_UP: {
            // Tap on press (e.g. Hold-to-ADS opens scope) + tap again on release!
            if (st.touchDown) {
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
                ++packetsThisSec_;
            }
            sendTapWithJitterLocked(TOUCH_DOWN, PTR_BTN_BASE + idx, b.x, b.y, st.lastPx, st.lastPy);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            if (b.switchScopeSens) scopeSensActive_ = true;
            break;
        }
        case NODE_UP_TRIGGER: {
            // Fires only when released
            break;
        }
        case NODE_HOLD_VIEW: {
            if (!aimMode_) break; // Free-look drag requires mouse lock
            // Push temporary mouse-linked drag view (e.g. Free Look Eye) onto FpsViewStack!
            FpsViewConfig ovCfg;
            ovCfg.screenW = static_cast<double>(w);
            ovCfg.screenH = static_cast<double>(h);
            ovCfg.centerX = static_cast<double>(b.x) * ovCfg.screenW;
            ovCfg.centerY = static_cast<double>(b.y) * ovCfg.screenH;
            ovCfg.rectLeft = 4.0;
            ovCfg.rectTop = 4.0;
            ovCfg.rectRight = ovCfg.screenW - 5.0;
            ovCfg.rectBottom = ovCfg.screenH - 5.0;
            ovCfg.sensX = static_cast<double>(profile_.sensX);
            ovCfg.sensY = static_cast<double>(profile_.sensY);
            ovCfg.ffCounterAccel = 0.0;
            ovCfg.invertY = profile_.invertY;
            ovCfg.unbounded = profile_.unboundedAim;
            ovCfg.clampInsteadOfRelease = !profile_.unboundedAim;
            ovCfg.edgeReleaseOnly = true;
            ovCfg.touchDownRandomRangePx = 0;
            viewStack_.pushOverlay(static_cast<int>(idx), ovCfg, nowMs, packetsThisSec_);
            break;
        }
        case NODE_SWIPE: {
            if (st.touchDown) {
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
                ++packetsThisSec_;
            }
            st.swipeX1 = static_cast<double>(b.x) * static_cast<double>(w);
            st.swipeY1 = static_cast<double>(b.y) * static_cast<double>(h);
            st.swipeX2 = static_cast<double>(b.x2) * static_cast<double>(w);
            st.swipeY2 = static_cast<double>(b.y2) * static_cast<double>(h);
            st.swipeStartMs = nowMs;
            st.swipeDurMs = static_cast<double>(b.durationMs > 10 ? b.durationMs : 120);
            st.lastPx = static_cast<int32_t>(std::floor(st.swipeX1 + 0.5));
            st.lastPy = static_cast<int32_t>(std::floor(st.swipeY1 + 0.5));
            bridge_->sendTouchPixels(TOUCH_DOWN, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
            st.touchDown = true;
            st.swiping = true;
            ++packetsThisSec_;
            break;
        }
        case NODE_CURSOR_TOGGLE: {
            sendTapWithJitterLocked(TOUCH_DOWN, PTR_BTN_BASE + idx, b.x, b.y, st.lastPx, st.lastPy);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + 35.0;
            outRequestToggleAim = true;
            break;
        }
        case NODE_RESET_VIEW: {
            viewStack_.resetMainView(nowMs);
            scopeSensActive_ = false;
            aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
            break;
        }
        case NODE_SWITCH_SENS: {
            if (b.toggleMode) {
                scopeSensActive_ = !scopeSensActive_;
            } else {
                scopeSensActive_ = true;
            }
            break;
        }
        case NODE_REACTIVATE_JOY: {
            reactivateJoystickLocked(nowMs);
            break;
        }
        default:
            break;
    }
}

void AimEngine::triggerButtonStopLocked(size_t idx, double nowMs) {
    const KeyNode& b = profile_.buttons[idx];
    ButtonRuntime& st = btnState_[idx];
    if (!st.keyHeld) return;
    st.keyHeld = false;

    switch (b.action) {
        case NODE_PRESS: {
            if (st.touchDown && st.autoReleaseAtMs == 0.0) {
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
                st.touchDown = false;
                ++packetsThisSec_;
            }
            break;
        }
        case NODE_DOWN_UP: {
            // Second tap on release (closes Hold-to-ADS scope cleanly!)
            if (st.touchDown) {
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
                ++packetsThisSec_;
            }
            sendTapWithJitterLocked(TOUCH_DOWN, PTR_BTN_BASE + idx, b.x, b.y, st.lastPx, st.lastPy);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            if (b.switchScopeSens) scopeSensActive_ = false;
            break;
        }
        case NODE_UP_TRIGGER: {
            if (st.touchDown) {
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + idx, st.lastPx, st.lastPy, true);
                ++packetsThisSec_;
            }
            sendTapWithJitterLocked(TOUCH_DOWN, PTR_BTN_BASE + idx, b.x, b.y, st.lastPx, st.lastPy);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            break;
        }
        case NODE_HOLD_VIEW: {
            if (viewStack_.isOverlayActive() && viewStack_.overlayOwnerId() == static_cast<int>(idx)) {
                viewStack_.popOverlay(static_cast<int>(idx), nowMs, packetsThisSec_);
                aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
            }
            break;
        }
        case NODE_SWITCH_SENS: {
            if (!b.toggleMode) {
                scopeSensActive_ = false;
            }
            break;
        }
        default:
            break;
    }
}

bool AimEngine::classifyKeyInputLocked(int vk, bool down, bool isAim, bool& outRequestToggleAim) {
    outRequestToggleAim = false;
    bool isWheel = (vk == VK_CUSTOM_MWHEEL_UP || vk == VK_CUSTOM_MWHEEL_DOWN);
    bool mouseBtn = isMouseVk(vk);

    if (!isWheel && vk >= 0 && vk < 512) {
        hookHeldKeys_[vk] = down;
    }

    // In Cursor Mode (!isAim): mouse buttons stay free for direct cursor clicks EXCEPT NODE_CURSOR_TOGGLE,
    // while ALL keyboard keys (WASD, Sprint, Walk, Jump, Crouch, Gloo, 1/2/3, etc.) STILL WORK!
    if (!isAim && mouseBtn) {
        if (down) {
            for (size_t i = 0; i < routeButtons_.size(); ++i) {
                if (routeButtons_[i].action == NODE_CURSOR_TOGGLE &&
                    routeButtons_[i].comboVk == 0 && vkMatch(vk, routeButtons_[i].vk)) {
                    outRequestToggleAim = true;
                    return true;
                }
            }
        }
        return false;
    }

    // Check if an explicit button uses this key (so BAG=LSHIFT / CROUCH=LCTRL wins over Sprint/Walk)
    bool hasCustomBtn = false;
    bool hasToggleBtn = false;
    for (size_t i = 0; i < routeButtons_.size(); ++i) {
        const KeyRouteEntry& e = routeButtons_[i];
        bool comboActive = (e.comboVk != 0 && vkMatch(vk, e.vk) && e.comboVk < 512 && hookHeldKeys_[e.comboVk]);
        bool singleMatch = (e.comboVk == 0 && vkMatch(vk, e.vk));
        bool prefixRelease = (!down && e.comboVk != 0 && vkMatch(vk, e.comboVk));
        if (comboActive || singleMatch || prefixRelease) {
            if (!isAim && e.action == NODE_HOLD_VIEW) continue;
            hasCustomBtn = true;
            if (down && e.action == NODE_CURSOR_TOGGLE && (comboActive || singleMatch)) {
                hasToggleBtn = true;
            }
        }
    }

    if (hasToggleBtn) {
        outRequestToggleAim = true;
        return true;
    }

    if (vkMatch(vk, routeVkUp_) || vkMatch(vk, routeVkDown_) ||
        vkMatch(vk, routeVkLeft_) || vkMatch(vk, routeVkRight_)) {
        return true;
    }
    if (!hasCustomBtn && (vkMatch(vk, routeVkSprint_) ||
                          (routeVkWalk_ != 0 && vkMatch(vk, routeVkWalk_)))) {
        return true;
    }
    if ((isAim || !isMouseVk(routeFireVk_)) && vkMatch(vk, routeFireVk_)) {
        return true;
    }
    return hasCustomBtn;
}

bool AimEngine::isKeyMapped(int vk, bool aimModeActive) const {
    EnterCriticalSection(&inputLock_);
    bool mouseBtn = isMouseVk(vk);
    if (!aimModeActive && mouseBtn) {
        for (size_t i = 0; i < routeButtons_.size(); ++i) {
            if (routeButtons_[i].action == NODE_CURSOR_TOGGLE &&
                routeButtons_[i].comboVk == 0 && vkMatch(vk, routeButtons_[i].vk)) {
                LeaveCriticalSection(&inputLock_);
                return true;
            }
        }
        LeaveCriticalSection(&inputLock_);
        return false;
    }
    if (vkMatch(vk, routeVkUp_) || vkMatch(vk, routeVkDown_) ||
        vkMatch(vk, routeVkLeft_) || vkMatch(vk, routeVkRight_) ||
        vkMatch(vk, routeVkSprint_) || (routeVkWalk_ != 0 && vkMatch(vk, routeVkWalk_))) {
        LeaveCriticalSection(&inputLock_);
        return true;
    }
    if ((aimModeActive || !isMouseVk(routeFireVk_)) && vkMatch(vk, routeFireVk_)) {
        LeaveCriticalSection(&inputLock_);
        return true;
    }
    for (size_t i = 0; i < routeButtons_.size(); ++i) {
        if (vkMatch(vk, routeButtons_[i].vk)) {
            if (!aimModeActive && routeButtons_[i].action == NODE_HOLD_VIEW) continue;
            LeaveCriticalSection(&inputLock_);
            return true;
        }
    }
    LeaveCriticalSection(&inputLock_);
    return false;
}

bool AimEngine::onKeyEvent(int vk, bool down, bool& outRequestToggleAim) {
    outRequestToggleAim = false;
    double nowMs = getTimeMs();
    bool isAim = (aimMode_ != 0);

    // NON-BLOCKING (<5us): classify & enqueue under inputLock_ — NEVER calls TCP send() on Windows hook thread!
    EnterCriticalSection(&inputLock_);
    bool consumed = classifyKeyInputLocked(vk, down, isAim, outRequestToggleAim);
    bool shouldQueue = consumed || !down; // Always enqueue key-ups too so held combos release cleanly
    if (shouldQueue && thread_ != NULL) {
        QueuedKeyEvent ev = { vk, down, nowMs };
        pendingKeys_.push_back(ev);
    }
    LeaveCriticalSection(&inputLock_);

    if (thread_ == NULL) {
        // Offline / synchronous unit-test path when background thread is not running
        EnterCriticalSection(&lock_);
        processKeyEventLocked(vk, down, nowMs);
        LeaveCriticalSection(&lock_);
    } else if (shouldQueue && wakeEvent_) {
        SetEvent(wakeEvent_);
    }
    return consumed;
}

void AimEngine::processKeyEventLocked(int vk, bool down, double nowMs) {
    bool dummyToggle = false;
    bool isAim = (aimMode_ != 0);
    bool isWheel = (vk == VK_CUSTOM_MWHEEL_UP || vk == VK_CUSTOM_MWHEEL_DOWN);
    bool mouseBtn = isMouseVk(vk);

    if (!isWheel && vk >= 0 && vk < 512) {
        heldKeys_[vk] = down;
    }

    // In Cursor Mode (!isAim): mouse buttons ONLY fire NODE_CURSOR_TOGGLE (leaving LMB/RMB free for cursor clicks),
    // while ALL keyboard keys (WASD, Sprint, Walk, Jump, Crouch, Gloo, 1/2/3, etc.) continue working seamlessly!
    if (!isAim && mouseBtn) {
        if (down) {
            for (size_t i = 0; i < profile_.buttons.size(); ++i) {
                const KeyNode& b = profile_.buttons[i];
                if (b.action == NODE_CURSOR_TOGGLE && b.comboVk == 0 && vkMatch(vk, b.vk)) {
                    triggerButtonStartLocked(i, nowMs, dummyToggle);
                    btnState_[i].keyHeld = false;
                    return;
                }
            }
        }
        return;
    }

    // 1. Check WASD + Sprint + Walk joystick keys (works in BOTH Aim Mode AND Cursor Mode!)
    //    Explicit user-mapped Action Buttons (e.g. BAG=LSHIFT, CROUCH=LCTRL) take priority over Sprint/Walk modifiers!
    bool hasCustomButtonOnVk = false;
    for (size_t i = 0; i < profile_.buttons.size(); ++i) {
        if (profile_.buttons[i].comboVk == 0 && vkMatch(vk, profile_.buttons[i].vk)) {
            if (!isAim && profile_.buttons[i].action == NODE_HOLD_VIEW) continue;
            hasCustomButtonOnVk = true;
            break;
        }
    }

    bool joyChanged = false;
    bool isJoyKey = false;
    if (vkMatch(vk, profile_.vkUp))         { isJoyKey = true; if (keyUp_ != down)    { keyUp_ = down; joyChanged = true; } }
    else if (vkMatch(vk, profile_.vkDown))  { isJoyKey = true; if (keyDown_ != down)  { keyDown_ = down; joyChanged = true; } }
    else if (vkMatch(vk, profile_.vkLeft))  { isJoyKey = true; if (keyLeft_ != down)  { keyLeft_ = down; joyChanged = true; } }
    else if (vkMatch(vk, profile_.vkRight)) { isJoyKey = true; if (keyRight_ != down) { keyRight_ = down; joyChanged = true; } }
    else if (!hasCustomButtonOnVk && vkMatch(vk, profile_.vkSprint)) {
        isJoyKey = true;
        if (keySprint_ != down) { keySprint_ = down; joyChanged = true; }
    } else if (!hasCustomButtonOnVk && profile_.vkWalk != 0 && vkMatch(vk, profile_.vkWalk)) {
        isJoyKey = true;
        if (keyWalk_ != down) { keyWalk_ = down; joyChanged = true; }
    }

    if (isJoyKey) {
        if (joyChanged) {
            updateJoystickLocked(nowMs);
        }
        return;
    }

    // 2. Check Fire button (LMB by default; keyboard-bound fire also works when mouse unlocked)
    if ((isAim || !isMouseVk(profile_.fireVk)) && vkMatch(vk, profile_.fireVk)) {
        if (isWheel) {
            // Wheel-bound Fire: crisp auto-released tap
            if (down) {
                pressFireLocked(nowMs);
                quickTapFireReleaseAtMs_ = nowMs + 40.0;
            }
            return;
        }
        if (fireHeld_ != down) {
            fireHeld_ = down;

            if (profile_.fireMode == FIRE_MODE_DRAG_RIGHT) {
                if (down) {
                    quickTapFireReleaseAtMs_ = 0.0;
                    if (viewStack_.mainView().isFingerPlaced()) {
                        viewStack_.resetMainView(nowMs);
                        pendingFireDown_ = true;
                        pendingFireAtMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs > 0 ? profile_.touchDownDelayMs : 12);
                    } else {
                        pendingFireDown_ = false;
                        pressFireLocked(nowMs);
                    }
                } else {
                    if (pendingFireDown_) {
                        quickTapFireReleaseAtMs_ = pendingFireAtMs_ + 25.0;
                    } else {
                        liftFireFingerLocked();
                        aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
                    }
                }
            } else if (profile_.fireMode == FIRE_MODE_CLAW_LEFT) {
                if (down) {
                    if (isAim && !viewStack_.isOverlayActive()) {
                        viewStack_.ensureMainFingerDown(nowMs, packetsThisSec_);
                    }
                    pressFireLocked(nowMs);
                } else {
                    liftFireFingerLocked();
                }
            } else {
                // FIRE_MODE_DUAL_DRAG
                if (down) {
                    if (isAim && !viewStack_.isOverlayActive()) {
                        viewStack_.ensureMainFingerDown(nowMs, packetsThisSec_);
                    }
                    pressFireLocked(nowMs);
                } else {
                    liftFireFingerLocked();
                }
            }
        }
        return;
    }

    // 3. 2-Level Hotkey Lookup for Action Buttons (2-key combos take priority over single keys)
    if (down) {
        bool comboMatched = false;
        for (size_t i = 0; i < profile_.buttons.size(); ++i) {
            const KeyNode& b = profile_.buttons[i];
            if (!isAim && b.action == NODE_HOLD_VIEW) continue;
            if (b.comboVk != 0 && vkMatch(vk, b.vk) && b.comboVk < 512 && heldKeys_[b.comboVk]) {
                triggerButtonStartLocked(i, nowMs, dummyToggle);
                if (isWheel || b.action == NODE_CURSOR_TOGGLE) btnState_[i].keyHeld = false;
                comboMatched = true;
            }
        }
        if (!comboMatched) {
            for (size_t i = 0; i < profile_.buttons.size(); ++i) {
                const KeyNode& b = profile_.buttons[i];
                if (!isAim && b.action == NODE_HOLD_VIEW) continue;
                if (b.comboVk == 0 && vkMatch(vk, b.vk)) {
                    triggerButtonStartLocked(i, nowMs, dummyToggle);
                    if (isWheel || b.action == NODE_CURSOR_TOGGLE) btnState_[i].keyHeld = false;
                }
            }
        }
    } else {
        // On release, stop any active button triggered by this key OR whose combo prefix was released
        for (size_t i = 0; i < profile_.buttons.size(); ++i) {
            const KeyNode& b = profile_.buttons[i];
            if ((vkMatch(vk, b.vk) || (b.comboVk != 0 && vkMatch(vk, b.comboVk))) && btnState_[i].keyHeld) {
                triggerButtonStopLocked(i, nowMs);
            }
        }
    }
}

void AimEngine::updateJoystickLocked(double nowMs) {
    int ix = (keyRight_ ? 1 : 0) - (keyLeft_ ? 1 : 0);
    int iy = (keyDown_  ? 1 : 0) - (keyUp_   ? 1 : 0);

    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    double cx = static_cast<double>(profile_.joyX) * static_cast<double>(w);
    double cy = static_cast<double>(profile_.joyY) * static_cast<double>(h);

    if (ix == 0 && iy == 0) {
        joyGlideActive_ = false;
        if (joyDown_) {
            bridge_->sendTouchPixels(TOUCH_UP, PTR_JOYSTICK, joyLastX_, joyLastY_, true);
            joyDown_ = false;
            joyLastX_ = -1;
            joyLastY_ = -1;
            ++packetsThisSec_;
        }
        return;
    }

    double vx = static_cast<double>(ix);
    double vy = static_cast<double>(iy);
    double len = std::sqrt(vx * vx + vy * vy);
    if (len > 0.001) {
        vx /= len;
        vy /= len;
    }

    // Dynamic radius selection: Shift+W = Sprint, Ctrl = Slow Walk, default = normal Run
    bool sprinting = keySprint_ && keyUp_ && !keyDown_;
    double radNorm = static_cast<double>(profile_.joyRadius);
    if (keyWalk_) {
        radNorm = static_cast<double>(profile_.joyWalkRadius);
    } else if (sprinting) {
        radNorm = static_cast<double>(profile_.joySprintRadius);
    }
    double radPx = radNorm * static_cast<double>(h);

    double targetX = cx + vx * radPx;
    double targetY = cy + vy * radPx;

    bool firstDown = !joyDown_;
    if (firstDown) {
        int32_t cpx = static_cast<int32_t>(std::floor(cx + 0.5));
        int32_t cpy = static_cast<int32_t>(std::floor(cy + 0.5));
        bridge_->sendTouchPixels(TOUCH_DOWN, PTR_JOYSTICK, cpx, cpy, true);
        joyDown_ = true;
        joyCurX_ = cx;
        joyCurY_ = cy;
        joyLastX_ = cpx;
        joyLastY_ = cpy;
        ++packetsThisSec_;
    }

    if (profile_.joySteppedGlide) {
        // Free Fire smooth 3-stage stepped glide:
        // Stage 0 (+8ms): center settle, Stage 1 (+15ms): 20%, Stage 2 (+15ms): 50%, Stage 3 (+15ms): 100%
        joyGlideActive_ = true;
        joyGlideStartX_ = joyCurX_;
        joyGlideStartY_ = joyCurY_;
        joyGlideTargetX_ = targetX;
        joyGlideTargetY_ = targetY;
        joyGlideStage_ = firstDown ? 0 : 1;
        joyGlideNextMs_ = nowMs + (firstDown ? 8.0 : 10.0);
    } else {
        joyGlideActive_ = false;
        joyCurX_ = targetX;
        joyCurY_ = targetY;
        int32_t tx = static_cast<int32_t>(std::floor(targetX + 0.5));
        int32_t ty = static_cast<int32_t>(std::floor(targetY + 0.5));
        if (tx != joyLastX_ || ty != joyLastY_) {
            bridge_->sendTouchPixels(TOUCH_MOVE, PTR_JOYSTICK, tx, ty, true);
            joyLastX_ = tx;
            joyLastY_ = ty;
            ++packetsThisSec_;
        }
    }
}

void AimEngine::stepJoystickGlideLocked(double nowMs) {
    if (!joyGlideActive_ || !joyDown_) return;
    if (nowMs < joyGlideNextMs_) return;

    // Cumulative progress ratios: 0% (settle), 20%, 50%, 100%
    static const double kRatios[4] = { 0.0, 0.20, 0.50, 1.00 };
    double r = kRatios[joyGlideStage_];
    joyCurX_ = joyGlideStartX_ + (joyGlideTargetX_ - joyGlideStartX_) * r;
    joyCurY_ = joyGlideStartY_ + (joyGlideTargetY_ - joyGlideStartY_) * r;

    int32_t px = static_cast<int32_t>(std::floor(joyCurX_ + 0.5));
    int32_t py = static_cast<int32_t>(std::floor(joyCurY_ + 0.5));
    bridge_->sendTouchPixels(TOUCH_MOVE, PTR_JOYSTICK, px, py, true);
    joyLastX_ = px;
    joyLastY_ = py;
    ++packetsThisSec_;

    ++joyGlideStage_;
    if (joyGlideStage_ >= 4) {
        joyGlideActive_ = false;
    } else {
        joyGlideNextMs_ = nowMs + 15.0;
    }
}

void AimEngine::reactivateJoystickLocked(double nowMs) {
    joyGlideActive_ = false;
    if (joyDown_) {
        bridge_->sendTouchPixels(TOUCH_UP, PTR_JOYSTICK, joyLastX_, joyLastY_, true);
        joyDown_ = false;
        joyLastX_ = -1;
        joyLastY_ = -1;
        ++packetsThisSec_;
    }
    updateJoystickLocked(nowMs);
}

void AimEngine::pressFireLocked(double /*nowMs*/) {
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    double dw = static_cast<double>(w);
    double dh = static_cast<double>(h);

    if (profile_.fireMode == FIRE_MODE_CLAW_LEFT) {
        fireCurX_ = static_cast<double>(profile_.leftFireX) * dw;
        fireCurY_ = static_cast<double>(profile_.leftFireY) * dh;
    } else {
        fireCurX_ = static_cast<double>(profile_.fireX) * dw;
        fireCurY_ = static_cast<double>(profile_.fireY) * dh;
    }

    int32_t px = static_cast<int32_t>(std::floor(fireCurX_ + 0.5));
    int32_t py = static_cast<int32_t>(std::floor(fireCurY_ + 0.5));
    bridge_->sendTouchPixels(TOUCH_DOWN, PTR_FIRE, px, py, true);
    fireDown_ = true;
    fireStartX_ = px;
    fireStartY_ = py;
    fireLastSentX_ = px;
    fireLastSentY_ = py;
    ++packetsThisSec_;
}

void AimEngine::liftFireFingerLocked() {
    if (fireDown_) {
        int32_t dx = fireLastSentX_ - fireStartX_;
        int32_t dy = fireLastSentY_ - fireStartY_;
        bool unclamped = (profile_.fireMode != FIRE_MODE_CLAW_LEFT && profile_.unboundedAim);
        bridge_->sendTouchPixels(TOUCH_UP, PTR_FIRE, fireLastSentX_, fireLastSentY_, !unclamped);
        fireDown_ = false;
        fireLastSentX_ = -1;
        fireLastSentY_ = -1;
        ++packetsThisSec_;
        const char* mname = (profile_.fireMode == FIRE_MODE_DRAG_RIGHT) ? "DRAG-FIRE"
                          : (profile_.fireMode == FIRE_MODE_CLAW_LEFT)  ? "CLAW-LEFT"
                                                                        : "DUAL-DRAG";
        std::printf("[Fire] %s release | start=(%d,%d) drag=(%+d, %+d px)\n",
                    mname, fireStartX_, fireStartY_, dx, dy);
    }
    pendingFireDown_ = false;
    quickTapFireReleaseAtMs_ = 0.0;
}

void AimEngine::releaseMouseTouchesLocked() {
    double nowMs = getTimeMs();
    viewStack_.onFocusLose(nowMs);
    liftFireFingerLocked();
    fireHeld_ = false;
    scopeSensActive_ = false;
    pendingFireDown_ = false;
    quickTapFireReleaseAtMs_ = 0.0;
    aimCooldownUntilMs_ = 0.0;

    // Release only mouse-bound buttons (keep WASD joystick & keyboard buttons alive!)
    for (size_t i = 0; i < btnState_.size() && i < profile_.buttons.size(); ++i) {
        if (isMouseVk(profile_.buttons[i].vk)) {
            btnState_[i].keyHeld = false;
            if (btnState_[i].touchDown && btnState_[i].autoReleaseAtMs == 0.0) {
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + i, btnState_[i].lastPx, btnState_[i].lastPy, true);
                btnState_[i].touchDown = false;
            }
        }
    }

    if (directCursorDown_) {
        bridge_->sendTouchNorm(TOUCH_UP, PTR_CURSOR, 0.5f, 0.5f);
        directCursorDown_ = false;
    }
}

void AimEngine::releaseAllLocked() {
    releaseMouseTouchesLocked();

    joyGlideActive_ = false;
    if (joyDown_) {
        bridge_->sendTouchPixels(TOUCH_UP, PTR_JOYSTICK, joyLastX_, joyLastY_, true);
        joyDown_ = false;
        joyLastX_ = -1;
        joyLastY_ = -1;
    }
    keyUp_ = keyDown_ = keyLeft_ = keyRight_ = keySprint_ = keyWalk_ = false;
    std::memset(heldKeys_, 0, sizeof(heldKeys_));

    for (size_t i = 0; i < btnState_.size() && i < profile_.buttons.size(); ++i) {
        btnState_[i].keyHeld = false;
        btnState_[i].swiping = false;
        if (btnState_[i].touchDown && btnState_[i].autoReleaseAtMs == 0.0) {
            bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + i, btnState_[i].lastPx, btnState_[i].lastPy, true);
            btnState_[i].touchDown = false;
        }
    }
}

void AimEngine::releaseAllTouches() {
    EnterCriticalSection(&lock_);
    releaseAllLocked();
    EnterCriticalSection(&inputLock_);
    pendingKeys_.clear();
    pendingCursor_.clear();
    accumRawDx_ = 0.0;
    accumRawDy_ = 0.0;
    std::memset(hookHeldKeys_, 0, sizeof(hookHeldKeys_));
    LeaveCriticalSection(&inputLock_);
    LeaveCriticalSection(&lock_);
}

void AimEngine::resetAimView() {
    EnterCriticalSection(&lock_);
    double nowMs = getTimeMs();
    viewStack_.resetMainView(nowMs);
    scopeSensActive_ = false;
    aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
    LeaveCriticalSection(&lock_);
}

void AimEngine::processTick(double nowMs) {
    // 0. Drain non-blocking hook queues in <2 microseconds (NEVER blocks Windows hooks during TCP send!)
    std::vector<QueuedKeyEvent> localKeys;
    std::vector<QueuedCursorTouch> localCursor;
    EnterCriticalSection(&inputLock_);
    if (!pendingKeys_.empty()) localKeys.swap(pendingKeys_);
    if (!pendingCursor_.empty()) localCursor.swap(pendingCursor_);
    LeaveCriticalSection(&inputLock_);

    EnterCriticalSection(&lock_);

    // 1. Update telemetry PPS counter every second
    if (nowMs - lastStatMs_ >= 1000.0) {
        lastPacketsPerSec_ = packetsThisSec_;
        packetsThisSec_ = 0;
        lastStatMs_ = nowMs;
    }

    // 1b. Dispatch all queued keyboard / mouse-button / wheel events IMMEDIATELY (0ms coalescing delay!)
    for (size_t i = 0; i < localKeys.size(); ++i) {
        processKeyEventLocked(localKeys[i].vk, localKeys[i].down, nowMs);
    }

    // 1c. Dispatch queued direct cursor touches when unlocked
    for (size_t i = 0; i < localCursor.size(); ++i) {
        if (!aimMode_) {
            processDirectCursorLocked(localCursor[i].action, localCursor[i].normX, localCursor[i].normY);
        }
    }

    // 2. Step scheduled pulse taps, directional swipes & fast 8ms WASD joystick ramp (works in BOTH Aim & Cursor Mode!)
    for (size_t i = 0; i < btnState_.size() && i < profile_.buttons.size(); ++i) {
        ButtonRuntime& st = btnState_[i];
        if (st.swiping && st.touchDown) {
            double t = (nowMs - st.swipeStartMs) / st.swipeDurMs;
            if (t >= 1.0) {
                int32_t ex = static_cast<int32_t>(std::floor(st.swipeX2 + 0.5));
                int32_t ey = static_cast<int32_t>(std::floor(st.swipeY2 + 0.5));
                bridge_->sendTouchPixels(TOUCH_MOVE, PTR_BTN_BASE + i, ex, ey, true);
                bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + i, ex, ey, true);
                st.lastPx = ex;
                st.lastPy = ey;
                st.swiping = false;
                st.touchDown = false;
                packetsThisSec_ += 2;
            } else {
                int32_t cx = static_cast<int32_t>(std::floor(st.swipeX1 + (st.swipeX2 - st.swipeX1) * t + 0.5));
                int32_t cy = static_cast<int32_t>(std::floor(st.swipeY1 + (st.swipeY2 - st.swipeY1) * t + 0.5));
                if (cx != st.lastPx || cy != st.lastPy) {
                    bridge_->sendTouchPixels(TOUCH_MOVE, PTR_BTN_BASE + i, cx, cy, true);
                    st.lastPx = cx;
                    st.lastPy = cy;
                    ++packetsThisSec_;
                }
            }
        } else if (st.touchDown && st.autoReleaseAtMs > 0.0 && nowMs >= st.autoReleaseAtMs) {
            bridge_->sendTouchPixels(TOUCH_UP, PTR_BTN_BASE + i, st.lastPx, st.lastPy, true);
            st.touchDown = false;
            st.autoReleaseAtMs = 0.0;
            ++packetsThisSec_;
        }
    }

    stepJoystickGlideLocked(nowMs);

    // 2c. Scheduled fire handoff & quick-tap auto-release
    if (pendingFireDown_ && nowMs >= pendingFireAtMs_) {
        pendingFireDown_ = false;
        pressFireLocked(nowMs);
    }
    if (fireDown_ && quickTapFireReleaseAtMs_ > 0.0 && nowMs >= quickTapFireReleaseAtMs_) {
        liftFireFingerLocked();
        aimCooldownUntilMs_ = nowMs + static_cast<double>(profile_.touchDownDelayMs);
    }

    if (!aimMode_ || pendingFireDown_) {
        LeaveCriticalSection(&lock_);
        return;
    }

    // 3. Drain accumulated raw mouse deltas (original smooth fixed-cadence pollHz drain!)
    double rawDx = 0.0, rawDy = 0.0;
    EnterCriticalSection(&inputLock_);
    rawDx = accumRawDx_;
    rawDy = accumRawDy_;
    accumRawDx_ = 0.0;
    accumRawDy_ = 0.0;
    LeaveCriticalSection(&inputLock_);

    // Keep FpsViewStack screen geometry synced on rotation
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    if (w != lastConfiguredW_ || h != lastConfiguredH_) {
        syncFpsViewConfigLocked(w, h);
    }
    double dw = static_cast<double>(w);
    double dh = static_cast<double>(h);

    // 4. Compute velocity curve + Scope/Fire multipliers + optional smoothing
    bool firingActive = fireHeld_ || fireDown_;
    double speed = std::sqrt(rawDx * rawDx + rawDy * rawDy);
    double accelGain = 1.0;
    if (profile_.accelPower > 1.001f && speed > static_cast<double>(profile_.accelRefSpeed)) {
        double ratio = speed / static_cast<double>(profile_.accelRefSpeed);
        accelGain = std::pow(ratio, static_cast<double>(profile_.accelPower) - 1.0);
        if (accelGain > static_cast<double>(profile_.accelCap)) {
            accelGain = static_cast<double>(profile_.accelCap);
        }
    }

    double multX = accelGain;
    double multY = accelGain;
    if (scopeSensActive_) {
        multX *= static_cast<double>(profile_.scopeSensX);
        multY *= static_cast<double>(profile_.scopeSensY);
    }
    if (firingActive) {
        multX *= static_cast<double>(profile_.fireSensX);
        multY *= static_cast<double>(profile_.fireSensY);
    }

    double feedDx = rawDx;
    double feedDy = rawDy;
    if (profile_.smoothing > 0.001f) {
        double alpha = clampd(static_cast<double>(profile_.smoothing), 0.0, 0.85);
        smoothDx_ = smoothDx_ * alpha + rawDx * (1.0 - alpha);
        smoothDy_ = smoothDy_ * alpha + rawDy * (1.0 - alpha);
        if (std::fabs(smoothDx_) < 0.02) smoothDx_ = 0.0;
        if (std::fabs(smoothDy_) < 0.02) smoothDy_ = 0.0;
        feedDx = smoothDx_;
        feedDy = smoothDy_;
    }

    bool hasFeed = (std::fabs(feedDx) > 0.0001 || std::fabs(feedDy) > 0.0001);

    // 5a. If a temporary HOLD_VIEW (e.g. Free Look Eye) is pushed onto FpsViewStack, it owns mouse movement!
    if (viewStack_.isOverlayActive()) {
        if (hasFeed) {
            viewStack_.onMouseDelta(feedDx, feedDy, 1.0, 1.0, nowMs, packetsThisSec_);
        }
        viewStack_.tick(nowMs, true, packetsThisSec_);
        LeaveCriticalSection(&lock_);
        return;
    }

    auto moveFireFinger = [&]() {
        if (!hasFeed || !fireDown_) return;
        double dx = feedDx * static_cast<double>(profile_.sensX) * multX;
        double dy = feedDy * static_cast<double>(profile_.sensY) * multY;
        if (profile_.invertY) dy = -dy;
        if (profile_.ffCounterAccel > 0.01f && dw > 1.0) {
            double r = std::fabs(fireCurX_ - static_cast<double>(fireStartX_)) / dw;
            double k = static_cast<double>(profile_.ffCounterAccel);
            dx *= ((0.98 / (k * r * r + 1.0)) + 0.02);
        }

        if (profile_.unboundedAim) {
            fireCurX_ += clampd(dx, -160.0, 160.0);
            fireCurY_ += clampd(dy, -160.0, 160.0);
        } else {
            double anchorX = static_cast<double>(profile_.fireX) * dw;
            double anchorY = static_cast<double>(profile_.fireY) * dh;
            double minX = clampd(anchorX - static_cast<double>(profile_.fireBoxHalfW) * dw, dw * 0.20, dw - 4.0);
            double maxX = clampd(anchorX + static_cast<double>(profile_.fireBoxHalfW) * dw, dw * 0.20, dw - 4.0);
            double minY = clampd(anchorY - static_cast<double>(profile_.fireBoxHalfH) * dh, dh * 0.02, dh - 4.0);
            double maxY = clampd(anchorY + static_cast<double>(profile_.fireBoxHalfH) * dh, dh * 0.02, dh * 0.98);
            fireCurX_ = clampd(fireCurX_ + clampd(dx, -160.0, 160.0), minX, maxX);
            fireCurY_ = clampd(fireCurY_ + clampd(dy, -160.0, 160.0), minY, maxY);
        }

        int32_t px = static_cast<int32_t>(std::floor(fireCurX_ + 0.5));
        int32_t py = static_cast<int32_t>(std::floor(fireCurY_ + 0.5));
        if (px != fireLastSentX_ || py != fireLastSentY_) {
            bridge_->sendTouchPixels(TOUCH_MOVE, PTR_FIRE, px, py, !profile_.unboundedAim);
            fireLastSentX_ = px;
            fireLastSentY_ = py;
            ++packetsThisSec_;
        }
    };

    auto moveMainCamera = [&]() {
        if (nowMs < aimCooldownUntilMs_) return;
        if (hasFeed) {
            viewStack_.onMouseDelta(feedDx, feedDy, multX, multY, nowMs, packetsThisSec_);
        }
        viewStack_.tick(nowMs, firingActive, packetsThisSec_);
    };

    // 5b. Route movement according to selected FireMode
    if (profile_.fireMode == FIRE_MODE_DRAG_RIGHT && fireDown_) {
        moveFireFinger();
    } else if (profile_.fireMode == FIRE_MODE_DUAL_DRAG && fireDown_) {
        moveMainCamera();
        moveFireFinger();
    } else {
        moveMainCamera();
    }

    LeaveCriticalSection(&lock_);
}

DWORD WINAPI AimEngine::tickThreadProc(LPVOID param) {
    static_cast<AimEngine*>(param)->runLoop();
    return 0;
}

void AimEngine::runLoop() {
    timeBeginPeriod(1);
    double nextTickMs = getTimeMs();

    while (!stopFlag_) {
        int hz = 250;
        EnterCriticalSection(&lock_);
        hz = profile_.pollHz;
        LeaveCriticalSection(&lock_);
        if (hz <= 0) hz = 1000;
        if (hz < 60) hz = 60;
        if (hz > 1000) hz = 1000;
        double periodMs = 1000.0 / static_cast<double>(hz);

        double nowMs = getTimeMs();
        if (nowMs >= nextTickMs) {
            processTick(nowMs);
            nextTickMs += periodMs;
            if (nextTickMs < nowMs - periodMs * 2.0) {
                nextTickMs = nowMs + periodMs;
            }
        } else {
            double waitMs = nextTickMs - nowMs;
            if (waitMs > 1.5) {
                Sleep(1);
            } else {
                Sleep(0);
            }
        }
    }

    timeEndPeriod(1);
}

EngineTelemetry AimEngine::getTelemetry() const {
    EngineTelemetry t;
    EnterCriticalSection(&lock_);
    uint32_t w = 1920, h = 1080;
    bridge_->getScreenSize(w, h);
    t.screenW = w;
    t.screenH = h;
    t.aimMode = (aimMode_ != 0);
    t.scopeActive = scopeSensActive_;
    t.holdViewActive = viewStack_.isOverlayActive();
    bool mainDown = viewStack_.mainView().isFingerPlaced();
    bool ovDown = viewStack_.isOverlayActive() && viewStack_.overlayView().isFingerPlaced();
    t.aimFingerDown = mainDown || ovDown || (profile_.fireMode != FIRE_MODE_CLAW_LEFT && fireDown_);
    t.fireHeld = fireHeld_;
    t.joyDown = joyDown_;
    if (ovDown) {
        t.aimFingerNormX = static_cast<float>(clampd(viewStack_.overlayView().curX() / static_cast<double>(w), 0.0, 1.0));
        t.aimFingerNormY = static_cast<float>(clampd(viewStack_.overlayView().curY() / static_cast<double>(h), 0.0, 1.0));
    } else if (profile_.fireMode != FIRE_MODE_CLAW_LEFT && fireDown_) {
        t.aimFingerNormX = static_cast<float>(clampd(fireCurX_ / static_cast<double>(w), 0.0, 1.0));
        t.aimFingerNormY = static_cast<float>(clampd(fireCurY_ / static_cast<double>(h), 0.0, 1.0));
    } else if (mainDown) {
        t.aimFingerNormX = static_cast<float>(clampd(viewStack_.mainView().curX() / static_cast<double>(w), 0.0, 1.0));
        t.aimFingerNormY = static_cast<float>(clampd(viewStack_.mainView().curY() / static_cast<double>(h), 0.0, 1.0));
    } else {
        t.aimFingerNormX = profile_.aimX;
        t.aimFingerNormY = profile_.aimY;
    }
    if (joyDown_) {
        t.joyFingerNormX = static_cast<float>(joyLastX_) / static_cast<float>(w);
        t.joyFingerNormY = static_cast<float>(joyLastY_) / static_cast<float>(h);
    } else {
        t.joyFingerNormX = profile_.joyX;
        t.joyFingerNormY = profile_.joyY;
    }
    t.touchRateHz = lastPacketsPerSec_;
    LeaveCriticalSection(&lock_);
    return t;
}
