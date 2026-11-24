#include "fps_view.hpp"

#include <cmath>

namespace {

double clampVal(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

FpsViewFinger::FpsViewFinger(uint64_t primaryPtrId, uint64_t altPtrId)
    : primaryPtrId_(primaryPtrId),
      altPtrId_(altPtrId),
      activePtrId_(primaryPtrId) {
}

void FpsViewFinger::setTouchSink(FpsTouchEmitFn fn, void* ctx) {
    emitFn_ = fn;
    emitCtx_ = ctx;
}

void FpsViewFinger::configure(const FpsViewConfig& cfg) {
    cfg_ = cfg;
    // Enforce >=15ms touchDownDelay floor to prevent touch fusion during edge-recenter
    if (cfg_.touchDownDelayMs < 15) {
        cfg_.touchDownDelayMs = 15;
    }
    if (cfg_.touchUpDelayMs < 0) {
        cfg_.touchUpDelayMs = 0;
    }
    if (cfg_.touchDownRandomRangePx < 0) {
        cfg_.touchDownRandomRangePx = 0;
    }
    if (!fingerPlaced_) {
        originX_ = cfg_.centerX;
        originY_ = cfg_.centerY;
        curX_ = cfg_.centerX;
        curY_ = cfg_.centerY;
    }
}

int FpsViewFinger::nextRandRange(int range) {
    if (range <= 0) return 0;
    rngState_ = rngState_ * 1664525u + 1013904223u;
    uint32_t span = static_cast<uint32_t>(range * 2);
    return static_cast<int>((rngState_ >> 16) % span) - range;
}

void FpsViewFinger::emitTouch(TouchAction action, uint64_t ptrId, int32_t px, int32_t py,
                              bool clampToScreen, uint32_t& outPacketsSent) {
    if (emitFn_) {
        emitFn_(emitCtx_, action, ptrId, px, py, clampToScreen);
    }
    ++outPacketsSent;
}

void FpsViewFinger::onFocusGain(double nowMs) {
    focused_ = true;
    accX_ = 0.0;
    accY_ = 0.0;
    pendingUp_ = false;
    lastMoveMs_ = nowMs;
}

void FpsViewFinger::onFocusLose(double nowMs) {
    focused_ = false;
    accX_ = 0.0;
    accY_ = 0.0;
    pendingUp_ = false;
    releaseFinger(nowMs);
}

bool FpsViewFinger::tryPlaceFinger(double nowMs, uint32_t& outPacketsSent) {
    if (fingerPlaced_) return true;

    // Non-blocking >=15ms separation after previous release so touches never fuse
    double minDelay = static_cast<double>(cfg_.touchDownDelayMs < 15 ? 15 : cfg_.touchDownDelayMs);
    if ((nowMs - lastReleaseMs_) < minDelay) {
        return false;
    }

    double landX = cfg_.centerX;
    double landY = cfg_.centerY;
    if (!cfg_.unbounded && !cfg_.clampInsteadOfRelease && cfg_.touchDownRandomRangePx > 0) {
        landX += static_cast<double>(nextRandRange(cfg_.touchDownRandomRangePx));
        landY += static_cast<double>(nextRandRange(cfg_.touchDownRandomRangePx));
        landX = clampVal(landX, cfg_.rectLeft + 2.0, cfg_.rectRight - 2.0);
        landY = clampVal(landY, cfg_.rectTop + 2.0, cfg_.rectBottom - 2.0);
    }

    originX_ = landX;
    originY_ = landY;
    curX_ = landX;
    curY_ = landY;

    int32_t px = static_cast<int32_t>(std::floor(curX_ + 0.5));
    int32_t py = static_cast<int32_t>(std::floor(curY_ + 0.5));
    emitTouch(TOUCH_DOWN, activePtrId_, px, py, true, outPacketsSent);

    fingerPlaced_ = true;
    lastSentX_ = px;
    lastSentY_ = py;
    lastMoveMs_ = nowMs;
    pendingUp_ = false;
    return true;
}

bool FpsViewFinger::forcePlaceAtCenter(double nowMs) {
    if (fingerPlaced_) return true;
    uint32_t dummy = 0;
    // Allow immediate placement at exact center (used for Free-Look push or CLAW-LEFT fire)
    originX_ = cfg_.centerX;
    originY_ = cfg_.centerY;
    curX_ = cfg_.centerX;
    curY_ = cfg_.centerY;

    int32_t px = static_cast<int32_t>(std::floor(curX_ + 0.5));
    int32_t py = static_cast<int32_t>(std::floor(curY_ + 0.5));
    emitTouch(TOUCH_DOWN, activePtrId_, px, py, true, dummy);

    fingerPlaced_ = true;
    lastSentX_ = px;
    lastSentY_ = py;
    lastMoveMs_ = nowMs;
    pendingUp_ = false;
    return true;
}

bool FpsViewFinger::releaseFinger(double nowMs) {
    pendingUp_ = false;
    if (!fingerPlaced_) return false;

    uint32_t dummy = 0;
    int32_t upX = lastSentX_;
    int32_t upY = lastSentY_;
    emitTouch(TOUCH_UP, activePtrId_, upX, upY, !cfg_.unbounded, dummy);

    fingerPlaced_ = false;
    lastSentX_ = -1;
    lastSentY_ = -1;
    lastReleaseMs_ = nowMs;
    accX_ = 0.0;
    accY_ = 0.0;

    // Alternate pointer ID on each re-plant so Android never blends two strokes
    if (altPtrId_ != primaryPtrId_) {
        activePtrId_ = (activePtrId_ == primaryPtrId_) ? altPtrId_ : primaryPtrId_;
    }

    curX_ = cfg_.centerX;
    curY_ = cfg_.centerY;
    return true;
}

void FpsViewFinger::onMouseDelta(double rawDx, double rawDy, double multX, double multY,
                                 double nowMs, uint32_t& outPacketsSent) {
    if (!focused_) return;

    // Complete any scheduled delayed edge-up before accepting new deltas
    if (pendingUp_) {
        if (nowMs >= pendingUpAtMs_) {
            releaseFinger(nowMs);
            ++outPacketsSent;
        }
        return;
    }

    if (std::fabs(rawDx) <= 0.0001 && std::fabs(rawDy) <= 0.0001) {
        return;
    }

    double dx = rawDx * cfg_.sensX * multX;
    double dy = rawDy * cfg_.sensY * multY;

    // Nonlinear horizontal counter-acceleration (matches Free Fire swipe curve)
    if (cfg_.ffCounterAccel > 0.01 && cfg_.screenW > 1.0 && !cfg_.clampInsteadOfRelease) {
        double r = std::fabs(curX_ - originX_) / cfg_.screenW;
        dx *= (0.98 / (cfg_.ffCounterAccel * r * r + 1.0)) + 0.02;
    }

    if (cfg_.invertY) {
        dy = -dy;
    }

    // Sub-pixel accumulation: preserve exact fractional remainder across ticks
    accX_ += dx;
    accY_ += dy;

    if (std::fabs(accX_) < 1.0 && std::fabs(accY_) < 1.0) {
        lastMoveMs_ = nowMs;
        return;
    }

    int32_t intDx = static_cast<int32_t>(accX_);
    int32_t intDy = static_cast<int32_t>(accY_);
    accX_ -= static_cast<double>(intDx);
    accY_ -= static_cast<double>(intDy);

    // Lazy touch-down: first whole-pixel delta places the virtual finger cleanly
    if (!fingerPlaced_) {
        tryPlaceFinger(nowMs, outPacketsSent);
        return;
    }

    // Guard single-tick extreme spikes while preserving fast 360° flicks
    intDx = static_cast<int32_t>(clampVal(static_cast<double>(intDx), -240.0, 240.0));
    intDy = static_cast<int32_t>(clampVal(static_cast<double>(intDy), -240.0, 240.0));

    double nextX = curX_ + static_cast<double>(intDx);
    double nextY = curY_ + static_cast<double>(intDy);

    if (cfg_.clampInsteadOfRelease) {
        // Linked mouse-move mode (e.g. Free Look Eye): stay held down and clamp to screen bounds
        nextX = clampVal(nextX, cfg_.rectLeft, cfg_.rectRight);
        nextY = clampVal(nextY, cfg_.rectTop, cfg_.rectBottom);
    } else if (!cfg_.unbounded) {
        // Boxed 360° mode: exiting moveRect triggers clean edge-release without out-of-bounds MOVE
        bool outOfBounds = (nextX < cfg_.rectLeft || nextX > cfg_.rectRight ||
                            nextY < cfg_.rectTop  || nextY > cfg_.rectBottom);
        if (outOfBounds) {
            if (cfg_.touchUpDelayMs > 0) {
                pendingUp_ = true;
                pendingUpAtMs_ = nowMs + static_cast<double>(cfg_.touchUpDelayMs);
            } else {
                releaseFinger(nowMs);
                ++outPacketsSent;
            }
            return;
        }
    }

    curX_ = nextX;
    curY_ = nextY;
    int32_t px = static_cast<int32_t>(std::floor(curX_ + 0.5));
    int32_t py = static_cast<int32_t>(std::floor(curY_ + 0.5));

    if (px != lastSentX_ || py != lastSentY_) {
        emitTouch(TOUCH_MOVE, activePtrId_, px, py, !cfg_.unbounded, outPacketsSent);
        lastSentX_ = px;
        lastSentY_ = py;
    }
    lastMoveMs_ = nowMs;
}

void FpsViewFinger::tick(double nowMs, bool suppressIdleRelease, uint32_t& outPacketsSent) {
    if (!focused_) return;

    if (pendingUp_ && nowMs >= pendingUpAtMs_) {
        if (releaseFinger(nowMs)) {
            ++outPacketsSent;
        }
        return;
    }

    // Idle auto-release timer: pre-emptively lifts finger between fights so next flick starts at center
    if (fingerPlaced_ && !suppressIdleRelease && !cfg_.clampInsteadOfRelease &&
        !cfg_.edgeReleaseOnly && cfg_.idleReleaseMs > 0) {
        if ((nowMs - lastMoveMs_) >= static_cast<double>(cfg_.idleReleaseMs)) {
            bool displaced = (std::fabs(curX_ - originX_) > cfg_.screenW * 0.20 ||
                              std::fabs(curY_ - originY_) > cfg_.screenH * 0.20);
            if (!cfg_.unbounded || displaced) {
                if (releaseFinger(nowMs)) {
                    ++outPacketsSent;
                }
            }
        }
    }
}

// ============================================================================
// FpsViewStack Implementation
// ============================================================================

FpsViewStack::FpsViewStack(uint64_t mainPtrA, uint64_t mainPtrB, uint64_t overlayPtr)
    : mainView_(mainPtrA, mainPtrB),
      overlayView_(overlayPtr, overlayPtr) {
}

void FpsViewStack::setTouchSink(FpsTouchEmitFn fn, void* ctx) {
    mainView_.setTouchSink(fn, ctx);
    overlayView_.setTouchSink(fn, ctx);
}

void FpsViewStack::configureMain(const FpsViewConfig& cfg) {
    mainView_.configure(cfg);
}

void FpsViewStack::onFocusGain(double nowMs) {
    overlayActive_ = false;
    overlayOwnerId_ = -1;
    overlayView_.onFocusLose(nowMs);
    mainView_.onFocusGain(nowMs);
}

void FpsViewStack::onFocusLose(double nowMs) {
    overlayActive_ = false;
    overlayOwnerId_ = -1;
    overlayView_.onFocusLose(nowMs);
    mainView_.onFocusLose(nowMs);
}

void FpsViewStack::pushOverlay(int ownerId, const FpsViewConfig& overlayCfg,
                               double nowMs, uint32_t& outPacketsSent) {
    // Lift main camera finger cleanly so Free Look Eye / secondary drag has exclusive camera control
    if (mainView_.releaseFinger(nowMs)) {
        ++outPacketsSent;
    }
    if (overlayActive_) {
        if (overlayView_.releaseFinger(nowMs)) {
            ++outPacketsSent;
        }
    }
    overlayView_.configure(overlayCfg);
    overlayView_.onFocusGain(nowMs);
    if (overlayView_.forcePlaceAtCenter(nowMs)) {
        ++outPacketsSent;
    }
    overlayActive_ = true;
    overlayOwnerId_ = ownerId;
}

void FpsViewStack::popOverlay(int ownerId, double nowMs, uint32_t& outPacketsSent) {
    if (!overlayActive_) return;
    if (ownerId >= 0 && overlayOwnerId_ != ownerId) return;

    if (overlayView_.releaseFinger(nowMs)) {
        ++outPacketsSent;
    }
    overlayView_.onFocusLose(nowMs);
    overlayActive_ = false;
    overlayOwnerId_ = -1;
    mainView_.onFocusGain(nowMs);
}

void FpsViewStack::ensureMainFingerDown(double nowMs, uint32_t& outPacketsSent) {
    if (overlayActive_) return;
    if (!mainView_.isFocused()) {
        mainView_.onFocusGain(nowMs);
    }
    if (!mainView_.isFingerPlaced()) {
        if (mainView_.forcePlaceAtCenter(nowMs)) {
            ++outPacketsSent;
        }
    }
}

void FpsViewStack::resetMainView(double nowMs) {
    if (overlayActive_) {
        overlayView_.onFocusLose(nowMs);
        overlayActive_ = false;
        overlayOwnerId_ = -1;
    }
    mainView_.releaseFinger(nowMs);
    mainView_.onFocusGain(nowMs);
}

void FpsViewStack::onMouseDelta(double rawDx, double rawDy, double multX, double multY,
                                double nowMs, uint32_t& outPacketsSent) {
    if (overlayActive_) {
        overlayView_.onMouseDelta(rawDx, rawDy, 1.0, 1.0, nowMs, outPacketsSent);
    } else {
        mainView_.onMouseDelta(rawDx, rawDy, multX, multY, nowMs, outPacketsSent);
    }
}

void FpsViewStack::tick(double nowMs, bool suppressIdleRelease, uint32_t& outPacketsSent) {
    if (overlayActive_) {
        overlayView_.tick(nowMs, true, outPacketsSent);
    } else {
        mainView_.tick(nowMs, suppressIdleRelease, outPacketsSent);
    }
}
