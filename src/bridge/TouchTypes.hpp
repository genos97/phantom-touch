#pragma once

#include <cstdint>

enum TouchAction : uint8_t {
    TOUCH_DOWN = 0,
    TOUCH_UP   = 1,
    TOUCH_MOVE = 2
};

namespace PointerId {
    static const uint64_t JOYSTICK  = 1;
    static const uint64_t AIM_A     = 2;
    static const uint64_t FIRE      = 3;
    static const uint64_t AIM_B     = 4;
    static const uint64_t HOLD_VIEW = 5;
    static const uint64_t SCROLL    = 6;
    static const uint64_t CURSOR    = 9;
    static const uint64_t BTN_BASE  = 20;
}

// Generic touch output sink so controllers can run against ScrcpyBridge or offline unit tests
typedef void (*TouchEmitFn)(void* ctx, TouchAction action, uint64_t pointerId,
                            int32_t px, int32_t py, bool clampToScreen);
