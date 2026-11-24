#include "engine/ActionDispatcher.hpp"

#include "config/KeyCodes.hpp"

#include <cmath>

ActionDispatcher::ActionDispatcher() {
}

void ActionDispatcher::setTouchSink(TouchEmitFn fn, void* ctx) {
    emitFn_ = fn;
    emitCtx_ = ctx;
}

void ActionDispatcher::resetForProfile(size_t buttonCount) {
    states_.assign(buttonCount, ButtonRuntime());
}

void ActionDispatcher::emitTouch(TouchAction action, uint64_t ptrId,
                                 int32_t px, int32_t py, uint32_t& outPacketsSent) {
    if (emitFn_) {
        emitFn_(emitCtx_, action, ptrId, px, py, true);
    }
    ++outPacketsSent;
}

void ActionDispatcher::sendTapWithJitter(TouchAction action, uint64_t ptrId, float nx, float ny,
                                         int jitterPx, uint32_t screenW, uint32_t screenH,
                                         int32_t& ioPx, int32_t& ioPy, uint32_t& outPacketsSent) {
    if (action == TOUCH_DOWN) {
        int32_t px = static_cast<int32_t>(nx * static_cast<float>(screenW) + 0.5f);
        int32_t py = static_cast<int32_t>(ny * static_cast<float>(screenH) + 0.5f);
        if (jitterPx > 0) {
            rngState_ = rngState_ * 1664525u + 1013904223u;
            int jx = static_cast<int>((rngState_ >> 16) % static_cast<uint32_t>(2 * jitterPx + 1)) - jitterPx;
            rngState_ = rngState_ * 1664525u + 1013904223u;
            int jy = static_cast<int>((rngState_ >> 16) % static_cast<uint32_t>(2 * jitterPx + 1)) - jitterPx;
            px += jx;
            py += jy;
        }
        ioPx = px;
        ioPy = py;
    }
    emitTouch(action, ptrId, ioPx, ioPy, outPacketsSent);
}

void ActionDispatcher::triggerStart(size_t idx, bool isAimMode, const Profile& profile,
                                    uint32_t screenW, uint32_t screenH, double nowMs,
                                    bool& ioScopeSensActive, ActionEffects& outEffects,
                                    uint32_t& outPacketsSent) {
    if (idx >= profile.buttons.size() || idx >= states_.size()) return;
    const KeyNode& b = profile.buttons[idx];
    ButtonRuntime& st = states_[idx];
    if (st.keyHeld) return;
    st.keyHeld = true;

    const uint64_t ptrId = PointerId::BTN_BASE + idx;

    switch (b.action) {
        case NODE_PRESS: {
            bool isWhl = (b.vk == VK_CUSTOM_MWHEEL_UP || b.vk == VK_CUSTOM_MWHEEL_DOWN);
            if (isWhl && st.touchDown) {
                emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
                st.touchDown = false;
            }
            if (!st.touchDown) {
                sendTapWithJitter(TOUCH_DOWN, ptrId, b.x, b.y, profile.tapJitterPx,
                                  screenW, screenH, st.lastPx, st.lastPy, outPacketsSent);
                st.touchDown = true;
                st.autoReleaseAtMs = isWhl ? (nowMs + 35.0) : 0.0;
            }
            if (b.switchScopeSens) ioScopeSensActive = !ioScopeSensActive;
            break;
        }
        case NODE_CLICK: {
            if (st.touchDown) {
                emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
            }
            sendTapWithJitter(TOUCH_DOWN, ptrId, b.x, b.y, profile.tapJitterPx,
                              screenW, screenH, st.lastPx, st.lastPy, outPacketsSent);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            if (b.switchScopeSens) ioScopeSensActive = !ioScopeSensActive;
            break;
        }
        case NODE_DOWN_UP: {
            if (st.touchDown) {
                emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
            }
            sendTapWithJitter(TOUCH_DOWN, ptrId, b.x, b.y, profile.tapJitterPx,
                              screenW, screenH, st.lastPx, st.lastPy, outPacketsSent);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            if (b.switchScopeSens) ioScopeSensActive = true;
            break;
        }
        case NODE_UP_TRIGGER: {
            break;
        }
        case NODE_HOLD_VIEW: {
            if (!isAimMode) break;
            outEffects.pushHoldView = true;
            outEffects.holdViewIdx = static_cast<int>(idx);
            break;
        }
        case NODE_SWIPE: {
            if (st.touchDown) {
                emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
            }
            st.swipeX1 = static_cast<double>(b.x) * static_cast<double>(screenW);
            st.swipeY1 = static_cast<double>(b.y) * static_cast<double>(screenH);
            st.swipeX2 = static_cast<double>(b.x2) * static_cast<double>(screenW);
            st.swipeY2 = static_cast<double>(b.y2) * static_cast<double>(screenH);
            st.swipeStartMs = nowMs;
            st.swipeDurMs = static_cast<double>(b.durationMs > 10 ? b.durationMs : 120);
            st.lastPx = static_cast<int32_t>(std::floor(st.swipeX1 + 0.5));
            st.lastPy = static_cast<int32_t>(std::floor(st.swipeY1 + 0.5));
            emitTouch(TOUCH_DOWN, ptrId, st.lastPx, st.lastPy, outPacketsSent);
            st.touchDown = true;
            st.swiping = true;
            break;
        }
        case NODE_CURSOR_TOGGLE: {
            sendTapWithJitter(TOUCH_DOWN, ptrId, b.x, b.y, profile.tapJitterPx,
                              screenW, screenH, st.lastPx, st.lastPy, outPacketsSent);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + 35.0;
            break;
        }
        case NODE_RESET_VIEW: {
            outEffects.resetMainView = true;
            ioScopeSensActive = false;
            break;
        }
        case NODE_SWITCH_SENS: {
            ioScopeSensActive = b.toggleMode ? !ioScopeSensActive : true;
            break;
        }
        case NODE_REACTIVATE_JOY: {
            outEffects.reactivateJoystick = true;
            break;
        }
        default:
            break;
    }

    // Optional per-node add-on effects (e.g. Scope + resetView, Vault/Jump + reactivateJoy)
    if (b.resetView && b.action != NODE_RESET_VIEW) {
        outEffects.resetMainView = true;
    }
    if (b.reactivateJoy && b.action != NODE_REACTIVATE_JOY) {
        outEffects.reactivateJoystick = true;
    }
}

void ActionDispatcher::triggerStop(size_t idx, const Profile& profile,
                                   uint32_t screenW, uint32_t screenH, double nowMs,
                                   bool& ioScopeSensActive, ActionEffects& outEffects,
                                   uint32_t& outPacketsSent) {
    if (idx >= profile.buttons.size() || idx >= states_.size()) return;
    const KeyNode& b = profile.buttons[idx];
    ButtonRuntime& st = states_[idx];
    if (!st.keyHeld) return;
    st.keyHeld = false;

    const uint64_t ptrId = PointerId::BTN_BASE + idx;

    switch (b.action) {
        case NODE_PRESS: {
            if (st.touchDown && st.autoReleaseAtMs == 0.0) {
                emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
                st.touchDown = false;
            }
            break;
        }
        case NODE_DOWN_UP: {
            if (st.touchDown) {
                emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
            }
            sendTapWithJitter(TOUCH_DOWN, ptrId, b.x, b.y, profile.tapJitterPx,
                              screenW, screenH, st.lastPx, st.lastPy, outPacketsSent);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            if (b.switchScopeSens) ioScopeSensActive = false;
            if (b.resetView) outEffects.resetMainView = true;
            break;
        }
        case NODE_UP_TRIGGER: {
            if (st.touchDown) {
                emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
            }
            sendTapWithJitter(TOUCH_DOWN, ptrId, b.x, b.y, profile.tapJitterPx,
                              screenW, screenH, st.lastPx, st.lastPy, outPacketsSent);
            st.touchDown = true;
            st.autoReleaseAtMs = nowMs + static_cast<double>(b.durationMs > 0 ? b.durationMs : 35);
            if (b.resetView) outEffects.resetMainView = true;
            if (b.reactivateJoy) outEffects.reactivateJoystick = true;
            break;
        }
        case NODE_HOLD_VIEW: {
            outEffects.popHoldView = true;
            outEffects.popHoldViewIdx = static_cast<int>(idx);
            break;
        }
        case NODE_SWITCH_SENS: {
            if (!b.toggleMode) {
                ioScopeSensActive = false;
            }
            break;
        }
        default:
            break;
    }
}

void ActionDispatcher::dispatchKey(int vk, bool down, bool isAimMode,
                                   const Profile& profile, const bool heldKeys[512],
                                   uint32_t screenW, uint32_t screenH, double nowMs,
                                   bool& ioScopeSensActive, ActionEffects& outEffects,
                                   uint32_t& outPacketsSent, bool toggleOnly) {
    bool isWheel = (vk == VK_CUSTOM_MWHEEL_UP || vk == VK_CUSTOM_MWHEEL_DOWN);

    if (down) {
        bool comboMatched = false;
        for (size_t i = 0; i < profile.buttons.size() && i < states_.size(); ++i) {
            const KeyNode& b = profile.buttons[i];
            if (toggleOnly && b.action != NODE_CURSOR_TOGGLE) continue;
            if (!isAimMode && b.action == NODE_HOLD_VIEW) continue;
            if (b.comboVk != 0 && KeyCodes::matches(vk, b.vk) && KeyCodes::isKeyHeld(heldKeys, b.comboVk)) {
                triggerStart(i, isAimMode, profile, screenW, screenH, nowMs,
                             ioScopeSensActive, outEffects, outPacketsSent);
                if (isWheel || b.action == NODE_CURSOR_TOGGLE) states_[i].keyHeld = false;
                comboMatched = true;
            }
        }
        if (!comboMatched) {
            for (size_t i = 0; i < profile.buttons.size() && i < states_.size(); ++i) {
                const KeyNode& b = profile.buttons[i];
                if (toggleOnly && b.action != NODE_CURSOR_TOGGLE) continue;
                if (!isAimMode && b.action == NODE_HOLD_VIEW) continue;
                if (b.comboVk == 0 && KeyCodes::matches(vk, b.vk)) {
                    triggerStart(i, isAimMode, profile, screenW, screenH, nowMs,
                                 ioScopeSensActive, outEffects, outPacketsSent);
                    if (isWheel || b.action == NODE_CURSOR_TOGGLE) states_[i].keyHeld = false;
                }
            }
        }
    } else {
        for (size_t i = 0; i < profile.buttons.size() && i < states_.size(); ++i) {
            const KeyNode& b = profile.buttons[i];
            if ((KeyCodes::matches(vk, b.vk) || (b.comboVk != 0 && KeyCodes::matches(vk, b.comboVk))) &&
                states_[i].keyHeld) {
                triggerStop(i, profile, screenW, screenH, nowMs,
                            ioScopeSensActive, outEffects, outPacketsSent);
            }
        }
    }
}

void ActionDispatcher::tick(const Profile& profile, double nowMs, uint32_t& outPacketsSent) {
    for (size_t i = 0; i < states_.size() && i < profile.buttons.size(); ++i) {
        ButtonRuntime& st = states_[i];
        const uint64_t ptrId = PointerId::BTN_BASE + i;

        if (st.swiping && st.touchDown) {
            double t = (nowMs - st.swipeStartMs) / st.swipeDurMs;
            if (t >= 1.0) {
                int32_t ex = static_cast<int32_t>(std::floor(st.swipeX2 + 0.5));
                int32_t ey = static_cast<int32_t>(std::floor(st.swipeY2 + 0.5));
                emitTouch(TOUCH_MOVE, ptrId, ex, ey, outPacketsSent);
                emitTouch(TOUCH_UP, ptrId, ex, ey, outPacketsSent);
                st.lastPx = ex;
                st.lastPy = ey;
                st.swiping = false;
                st.touchDown = false;
            } else {
                int32_t cx = static_cast<int32_t>(std::floor(st.swipeX1 + (st.swipeX2 - st.swipeX1) * t + 0.5));
                int32_t cy = static_cast<int32_t>(std::floor(st.swipeY1 + (st.swipeY2 - st.swipeY1) * t + 0.5));
                if (cx != st.lastPx || cy != st.lastPy) {
                    emitTouch(TOUCH_MOVE, ptrId, cx, cy, outPacketsSent);
                    st.lastPx = cx;
                    st.lastPy = cy;
                }
            }
        } else if (st.touchDown && st.autoReleaseAtMs > 0.0 && nowMs >= st.autoReleaseAtMs) {
            emitTouch(TOUCH_UP, ptrId, st.lastPx, st.lastPy, outPacketsSent);
            st.touchDown = false;
            st.autoReleaseAtMs = 0.0;
        }
    }
}

void ActionDispatcher::releaseButtons(const Profile& profile, bool mouseOnly, uint32_t& outPacketsSent) {
    for (size_t i = 0; i < states_.size() && i < profile.buttons.size(); ++i) {
        if (mouseOnly && !KeyCodes::isMouseKey(profile.buttons[i].vk)) continue;
        states_[i].keyHeld = false;
        if (!mouseOnly) states_[i].swiping = false;
        if (states_[i].touchDown && states_[i].autoReleaseAtMs == 0.0) {
            emitTouch(TOUCH_UP, PointerId::BTN_BASE + i, states_[i].lastPx, states_[i].lastPy, outPacketsSent);
            states_[i].touchDown = false;
        }
    }
}
