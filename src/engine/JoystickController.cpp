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
        if (joyDown_) {
            emitTouch(TOUCH_UP, joyLastX_, joyLastY_, outPacketsSent);
            joyDown_ = false;
            joyLastX_ = -1;
            joyLastY_ = -1;
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
        // Free Fire smooth 3-stage stepped glide:
        // Stage 0 (+8ms): center settle, Stage 1 (+15ms): 20%, Stage 2 (+15ms): 50%, Stage 3 (+15ms): 100%
        glideActive_ = true;
        glideStartX_ = joyCurX_;
        glideStartY_ = joyCurY_;
        glideTargetX_ = targetX;
        glideTargetY_ = targetY;
        glideStage_ = firstDown ? 0 : 1;
        glideNextMs_ = nowMs + (firstDown ? 8.0 : 10.0);
    } else {
        glideActive_ = false;
        joyCurX_ = targetX;
        joyCurY_ = targetY;
        int32_t tx = static_cast<int32_t>(std::floor(targetX + 0.5));
        int32_t ty = static_cast<int32_t>(std::floor(targetY + 0.5));
        if (tx != joyLastX_ || ty != joyLastY_) {
            emitTouch(TOUCH_MOVE, tx, ty, outPacketsSent);
            joyLastX_ = tx;
            joyLastY_ = ty;
        }
    }
}

void JoystickController::tick(double nowMs, uint32_t& outPacketsSent) {
    if (!glideActive_ || !joyDown_) return;
    if (nowMs < glideNextMs_) return;

    static const double kRatios[4] = { 0.0, 0.20, 0.50, 1.00 };
    double r = kRatios[glideStage_];
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
}

void JoystickController::reactivate(const Profile& profile, uint32_t screenW, uint32_t screenH,
                                    double nowMs, uint32_t& outPacketsSent) {
    glideActive_ = false;
    if (joyDown_) {
        emitTouch(TOUCH_UP, joyLastX_, joyLastY_, outPacketsSent);
        joyDown_ = false;
        joyLastX_ = -1;
        joyLastY_ = -1;
    }
    updateTarget(profile, screenW, screenH, nowMs, outPacketsSent);
}

void JoystickController::release(uint32_t& outPacketsSent) {
    glideActive_ = false;
    if (joyDown_) {
        emitTouch(TOUCH_UP, joyLastX_, joyLastY_, outPacketsSent);
        joyDown_ = false;
        joyLastX_ = -1;
        joyLastY_ = -1;
    }
    keyUp_ = keyDown_ = keyLeft_ = keyRight_ = keySprint_ = keyWalk_ = false;
}
