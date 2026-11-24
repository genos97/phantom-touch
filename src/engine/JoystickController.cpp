#include "engine/JoystickController.hpp"

#include "config/KeyCodes.hpp"

#include <cmath>

JoystickController::JoystickController() {
}

void JoystickController::setTouchSink(TouchEmitFn fn, void* ctx) {
    emitFn_ = fn;
    emitCtx_ = ctx;
}

void JoystickController::emitTouch(TouchAction action, int32_t px, int32_t py, uint32_t& outPacketsSent) {
    if (emitFn_) {
        emitFn_(emitCtx_, action, PointerId::JOYSTICK, px, py, true);
    }
    ++outPacketsSent;
}

bool JoystickController::handleKey(int vk, bool down, bool allowSprintWalk,
                                   const Profile& profile, uint32_t screenW, uint32_t screenH,
                                   double nowMs, uint32_t& outPacketsSent) {
    bool changed = false;
    bool matched = false;

    if (KeyCodes::matches(vk, profile.vkUp)) {
        matched = true;
        if (keyUp_ != down) { keyUp_ = down; changed = true; }
    } else if (KeyCodes::matches(vk, profile.vkDown)) {
        matched = true;
        if (keyDown_ != down) { keyDown_ = down; changed = true; }
    } else if (KeyCodes::matches(vk, profile.vkLeft)) {
        matched = true;
        if (keyLeft_ != down) { keyLeft_ = down; changed = true; }
    } else if (KeyCodes::matches(vk, profile.vkRight)) {
        matched = true;
        if (keyRight_ != down) { keyRight_ = down; changed = true; }
    } else if (allowSprintWalk && KeyCodes::matches(vk, profile.vkSprint)) {
        matched = true;
        if (keySprint_ != down) { keySprint_ = down; changed = true; }
    } else if (allowSprintWalk && profile.vkWalk != 0 && KeyCodes::matches(vk, profile.vkWalk)) {
        matched = true;
        if (keyWalk_ != down) { keyWalk_ = down; changed = true; }
    }

    if (matched && changed) {
        updateTarget(profile, screenW, screenH, nowMs, outPacketsSent);
    }
    return matched;
}

void JoystickController::updateTarget(const Profile& profile, uint32_t screenW, uint32_t screenH,
                                      double nowMs, uint32_t& outPacketsSent) {
    int ix = (keyRight_ ? 1 : 0) - (keyLeft_ ? 1 : 0);
    int iy = (keyDown_  ? 1 : 0) - (keyUp_   ? 1 : 0);

    double cx = static_cast<double>(profile.joyX) * static_cast<double>(screenW);
    double cy = static_cast<double>(profile.joyY) * static_cast<double>(screenH);

    if (ix == 0 && iy == 0) {
        glideActive_ = false;
        pendingReactivate_ = false;
        if (joyDown_) {
            emitTouch(TOUCH_UP, joyLastX_, joyLastY_, outPacketsSent);
            joyDown_ = false;
            joyLastX_ = -1;
            joyLastY_ = -1;
        }
        return;
    }

    if (pendingReactivate_) {
        // Wait for the clean >=15ms lift gap before re-planting
        return;
    }

    double vx = static_cast<double>(ix);
    double vy = static_cast<double>(iy);
    double len = std::sqrt(vx * vx + vy * vy);
    if (len > 0.001) {
        vx /= len;
        vy /= len;
    }

    // Dynamic radius selection: Shift+W = Sprint, Ctrl = Slow Walk, default = Run
    bool sprinting = keySprint_ && keyUp_ && !keyDown_;
    double radNorm = static_cast<double>(profile.joyRadius);
    if (keyWalk_) {
        radNorm = static_cast<double>(profile.joyWalkRadius);
    } else if (sprinting) {
        radNorm = static_cast<double>(profile.joySprintRadius);
    }
    double radPx = radNorm * static_cast<double>(screenH);

    double targetX = cx + vx * radPx;
    double targetY = cy + vy * radPx;

    bool firstDown = !joyDown_;
    if (firstDown) {
        int32_t cpx = static_cast<int32_t>(std::floor(cx + 0.5));
        int32_t cpy = static_cast<int32_t>(std::floor(cy + 0.5));
        emitTouch(TOUCH_DOWN, cpx, cpy, outPacketsSent);
        joyDown_ = true;
        joyCurX_ = cx;
        joyCurY_ = cy;
        joyLastX_ = cpx;
        joyLastY_ = cpy;
    }

    if (profile.joySteppedGlide) {
        // Free Fire smooth 4-stage stepped glide:
        // Stage 0 (+8ms): center settle, Stage 1 (+15ms): 20%, Stage 2 (+15ms): 50%, Stage 3 (+15ms): 100%
        glideActive_ = true;
        glideIsFreeFire_ = true;
        glideStartX_ = joyCurX_;
        glideStartY_ = joyCurY_;
        glideTargetX_ = targetX;
        glideTargetY_ = targetY;
        glideStage_ = firstDown ? 0 : 1;
        glideNextMs_ = nowMs + (firstDown ? 8.0 : 10.0);
    } else {
        // Fast 3-step 8ms glide (24ms total) so non-FF games never drop zero-ms thumb teleports
        glideActive_ = true;
        glideIsFreeFire_ = false;
        glideStartX_ = joyCurX_;
        glideStartY_ = joyCurY_;
        glideTargetX_ = targetX;
        glideTargetY_ = targetY;
        glideStage_ = 0;
        glideNextMs_ = nowMs + 8.0;
    }
}

void JoystickController::tick(const Profile& profile, uint32_t screenW, uint32_t screenH,
                              double nowMs, uint32_t& outPacketsSent) {
    if (pendingReactivate_) {
        if (nowMs >= reactivateAtMs_) {
            pendingReactivate_ = false;
            updateTarget(profile, screenW, screenH, nowMs, outPacketsSent);
        }
        return;
    }

    if (!glideActive_ || !joyDown_) return;
    if (nowMs < glideNextMs_) return;

    if (glideIsFreeFire_) {
        static const double kFfRatios[4] = { 0.0, 0.20, 0.50, 1.00 };
        double r = kFfRatios[glideStage_];
        joyCurX_ = glideStartX_ + (glideTargetX_ - glideStartX_) * r;
        joyCurY_ = glideStartY_ + (glideTargetY_ - glideStartY_) * r;

        int32_t px = static_cast<int32_t>(std::floor(joyCurX_ + 0.5));
        int32_t py = static_cast<int32_t>(std::floor(joyCurY_ + 0.5));
        emitTouch(TOUCH_MOVE, px, py, outPacketsSent);
        joyLastX_ = px;
        joyLastY_ = py;

        ++glideStage_;
        if (glideStage_ >= 4) {
            glideActive_ = false;
        } else {
            glideNextMs_ = nowMs + 15.0;
        }
    } else {
        static const double kFastRatios[3] = { 0.3333, 0.6667, 1.00 };
        double r = kFastRatios[glideStage_];
        joyCurX_ = glideStartX_ + (glideTargetX_ - glideStartX_) * r;
        joyCurY_ = glideStartY_ + (glideTargetY_ - glideStartY_) * r;

        int32_t px = static_cast<int32_t>(std::floor(joyCurX_ + 0.5));
        int32_t py = static_cast<int32_t>(std::floor(joyCurY_ + 0.5));
        emitTouch(TOUCH_MOVE, px, py, outPacketsSent);
        joyLastX_ = px;
        joyLastY_ = py;

        ++glideStage_;
        if (glideStage_ >= 3) {
            glideActive_ = false;
        } else {
            glideNextMs_ = nowMs + 8.0;
        }
    }
}

void JoystickController::reactivate(const Profile& profile, uint32_t screenW, uint32_t screenH,
                                    double nowMs, uint32_t& outPacketsSent) {
    glideActive_ = false;
    bool wasDown = joyDown_;
    if (joyDown_) {
        emitTouch(TOUCH_UP, joyLastX_, joyLastY_, outPacketsSent);
        joyDown_ = false;
        joyLastX_ = -1;
        joyLastY_ = -1;
    }
    bool hasDir = (keyUp_ != keyDown_) || (keyLeft_ != keyRight_);
    if (!hasDir) {
        pendingReactivate_ = false;
        return;
    }
    if (wasDown) {
        // Non-blocking >=15ms lift gap so the game registers thumb lift -> clean re-engage
        double gapMs = static_cast<double>(profile.touchDownDelayMs >= 15 ? profile.touchDownDelayMs : 15);
        pendingReactivate_ = true;
        reactivateAtMs_ = nowMs + gapMs;
    } else {
        pendingReactivate_ = false;
        updateTarget(profile, screenW, screenH, nowMs, outPacketsSent);
    }
}

void JoystickController::release(uint32_t& outPacketsSent) {
    glideActive_ = false;
    pendingReactivate_ = false;
    if (joyDown_) {
        emitTouch(TOUCH_UP, joyLastX_, joyLastY_, outPacketsSent);
        joyDown_ = false;
        joyLastX_ = -1;
        joyLastY_ = -1;
    }
    keyUp_ = keyDown_ = keyLeft_ = keyRight_ = keySprint_ = keyWalk_ = false;
}
