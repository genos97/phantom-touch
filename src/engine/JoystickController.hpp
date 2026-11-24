#pragma once

#include "bridge/TouchTypes.hpp"
#include "config/Profile.hpp"

#include <cstdint>

// Manages the virtual WASD touchscreen joystick (PointerId::JOYSTICK),
// including Sprint (LShift) / Walk (LCtrl) radius switching and Free Fire 3-stage stepped glide.
class JoystickController {
public:
    JoystickController();

    void setTouchSink(TouchEmitFn fn, void* ctx);

    // Returns true if vk was handled as a WASD / Sprint / Walk key
    bool handleKey(int vk, bool down, bool allowSprintWalk,
                   const Profile& profile, uint32_t screenW, uint32_t screenH,
                   double nowMs, uint32_t& outPacketsSent);

    // Steps smooth glide interpolation & delayed anti-stuck reactivation
    void tick(const Profile& profile, uint32_t screenW, uint32_t screenH,
              double nowMs, uint32_t& outPacketsSent);

    // Lifts and schedules clean non-blocking re-engagement (>=15ms gap)
    void reactivate(const Profile& profile, uint32_t screenW, uint32_t screenH,
                    double nowMs, uint32_t& outPacketsSent);

    // Lifts the virtual joystick finger and clears all WASD directional states
    void release(uint32_t& outPacketsSent);

    bool isDown() const { return joyDown_; }
    int32_t lastX() const { return joyLastX_; }
    int32_t lastY() const { return joyLastY_; }

private:
    void updateTarget(const Profile& profile, uint32_t screenW, uint32_t screenH,
                      double nowMs, uint32_t& outPacketsSent);
    void emitTouch(TouchAction action, int32_t px, int32_t py, uint32_t& outPacketsSent);

    TouchEmitFn emitFn_ = nullptr;
    void* emitCtx_ = nullptr;

    bool keyUp_ = false;
    bool keyDown_ = false;
    bool keyLeft_ = false;
    bool keyRight_ = false;
    bool keySprint_ = false;
    bool keyWalk_ = false;

    bool joyDown_ = false;
    double joyCurX_ = 0.0;
    double joyCurY_ = 0.0;
    int32_t joyLastX_ = -1;
    int32_t joyLastY_ = -1;

    bool glideActive_ = false;
    bool glideIsFreeFire_ = true;
    int glideStage_ = 0;
    double glideStartX_ = 0.0;
    double glideStartY_ = 0.0;
    double glideTargetX_ = 0.0;
    double glideTargetY_ = 0.0;
    double glideNextMs_ = 0.0;

    bool pendingReactivate_ = false;
    double reactivateAtMs_ = 0.0;
};
