#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <vector>

enum AnimEase {
    EASE_OUT_CUBIC = 0,
    EASE_IN_CUBIC  = 1,
    EASE_LINEAR    = 2
};

enum AnimDurationMs {
    ANIM_FAST_MS       = 120,
    ANIM_BASE_MS       = 180,
    ANIM_SLOW_MS       = 260,
    ANIM_MODE_PULSE_MS = 200
};

enum AnimId : uint32_t {
    ANIM_DRAWER_SLIDE   = 1,
    ANIM_HOVER_FADE     = 2,
    ANIM_TOAST_ALPHA    = 3,
    ANIM_HUD_MODE_PULSE = 4,
    ANIM_ONBOARD_PAGE   = 5
};

// U6: Lightweight tween engine driven by a single ~8ms timer only while active tweens exist.
// Respects Windows "reduce animations" setting (SPI_GETCLIENTAREAANIMATION).
class Animator {
public:
    static bool isReduceMotionEnabled();
    static void setReduceMotionOverrideForTest(int mode); // -1 = OS query, 0 = force off, 1 = force on
    static double currentTimeMs();

    void start(uint32_t id, float from, float to, int durationMs, AnimEase ease,
               const RECT* dirtyRc = nullptr, double nowMs = -1.0);

    float value(uint32_t id, float fallback = 0.0f) const;
    bool isActive(uint32_t id) const;
    bool hasActive() const;

    bool tick(double nowMs, std::vector<RECT>& outDirtyRects);
    void cancel(uint32_t id);
    void clear();

private:
    struct Tween {
        uint32_t id;
        float fromVal;
        float toVal;
        float curVal;
        double startMs;
        int durationMs;
        AnimEase ease;
        bool active;
        bool hasDirtyRect;
        RECT dirtyRc;

        Tween()
            : id(0), fromVal(0.0f), toVal(0.0f), curVal(0.0f),
              startMs(0.0), durationMs(0), ease(EASE_OUT_CUBIC),
              active(false), hasDirtyRect(false) {
            ZeroMemory(&dirtyRc, sizeof(dirtyRc));
        }
    };

    static float evaluateEase(float t, AnimEase ease);
    std::vector<Tween> tweens_;
};
