#include "ui/Animator.hpp"

#include <cstring>

namespace {

int g_reduceMotionTestOverride = -1; // -1 = query OS, 0 = off, 1 = on

bool rectsEqual(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

void Animator::setReduceMotionOverrideForTest(int mode) {
    g_reduceMotionTestOverride = mode;
}

bool Animator::isReduceMotionEnabled() {
    if (g_reduceMotionTestOverride == 1) return true;
    if (g_reduceMotionTestOverride == 0) return false;
#ifndef SPI_GETCLIENTAREAANIMATION
#define SPI_GETCLIENTAREAANIMATION 0x1042
#endif
    BOOL animEnabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animEnabled, 0)) {
        return animEnabled == FALSE;
    }
    return false;
}

double Animator::currentTimeMs() {
    static LARGE_INTEGER freq = {{0, 0}};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (freq.QuadPart > 0)
        ? (static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart))
        : static_cast<double>(GetTickCount());
}

float Animator::evaluateEase(float t, AnimEase ease) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (ease == EASE_OUT_CUBIC) {
        float inv = 1.0f - t;
        return 1.0f - inv * inv * inv;
    }
    if (ease == EASE_IN_CUBIC) {
        return t * t * t;
    }
    return t;
}

void Animator::start(uint32_t id, float from, float to, int durationMs, AnimEase ease,
                     const RECT* dirtyRc, double nowMs) {
    if (nowMs < 0.0) nowMs = currentTimeMs();
    bool reduce = isReduceMotionEnabled();

    Tween* slot = nullptr;
    for (size_t i = 0; i < tweens_.size(); ++i) {
        if (tweens_[i].id == id) {
            slot = &tweens_[i];
            break;
        }
    }
    if (!slot) {
        tweens_.push_back(Tween());
        slot = &tweens_.back();
    }

    slot->id = id;
    slot->fromVal = from;
    slot->toVal = to;
    slot->startMs = nowMs;
    slot->durationMs = durationMs;
    slot->ease = ease;
    if (dirtyRc && dirtyRc->right > dirtyRc->left && dirtyRc->bottom > dirtyRc->top) {
        slot->hasDirtyRect = true;
        slot->dirtyRc = *dirtyRc;
    } else {
        slot->hasDirtyRect = false;
        ZeroMemory(&slot->dirtyRc, sizeof(slot->dirtyRc));
    }

    if (reduce || durationMs <= 0) {
        slot->curVal = to;
        slot->active = false;
    } else {
        slot->curVal = from;
        slot->active = true;
    }
}

float Animator::value(uint32_t id, float fallback) const {
    for (size_t i = 0; i < tweens_.size(); ++i) {
        if (tweens_[i].id == id) {
            return tweens_[i].curVal;
        }
    }
    return fallback;
}

bool Animator::isActive(uint32_t id) const {
    for (size_t i = 0; i < tweens_.size(); ++i) {
        if (tweens_[i].id == id) {
            return tweens_[i].active;
        }
    }
    return false;
}

bool Animator::hasActive() const {
    for (size_t i = 0; i < tweens_.size(); ++i) {
        if (tweens_[i].active) return true;
    }
    return false;
}

bool Animator::tick(double nowMs, std::vector<RECT>& outDirtyRects) {
    outDirtyRects.clear();
    bool anyUpdated = false;
    for (size_t i = 0; i < tweens_.size(); ++i) {
        Tween& tw = tweens_[i];
        if (!tw.active) continue;
        anyUpdated = true;

        float rawT = (tw.durationMs > 0)
            ? static_cast<float>((nowMs - tw.startMs) / static_cast<double>(tw.durationMs))
            : 1.0f;
        if (rawT >= 1.0f) {
            tw.curVal = tw.toVal;
            tw.active = false;
        } else {
            float k = evaluateEase(rawT, tw.ease);
            tw.curVal = tw.fromVal + (tw.toVal - tw.fromVal) * k;
        }

        if (tw.hasDirtyRect) {
            bool dup = false;
            for (size_t di = 0; di < outDirtyRects.size(); ++di) {
                if (rectsEqual(outDirtyRects[di], tw.dirtyRc)) {
                    dup = true;
                    break;
                }
            }
            if (!dup) outDirtyRects.push_back(tw.dirtyRc);
        }
    }
    return anyUpdated;
}

void Animator::cancel(uint32_t id) {
    for (size_t i = 0; i < tweens_.size(); ++i) {
        if (tweens_[i].id == id) {
            tweens_[i].active = false;
        }
    }
}

void Animator::clear() {
    tweens_.clear();
}
