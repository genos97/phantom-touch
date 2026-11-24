#pragma once

#include "bridge/TouchTypes.hpp"

#include <cstdint>

// ============================================================================
// Phantom Touch — Bounded Virtual Finger ID Pool (Android slots 0..9)
// ============================================================================
// Translates arbitrary logical touch channels (Joystick, Aim A/B, Fire,
// Hold-View, Cursor, Scroll, and N mapped buttons) into strict Android
// multi-touch finger IDs [0..9] before dispatching to scrcpy-server.
//
// Key properties:
//   - Hard cap of 10 simultaneous fingers (matches Android & scrcpy PointersState).
//   - Round-robin (FIFO) free-slot rotation so rapid lift + re-plant strokes
//     (e.g. 360° camera edge resets) receive a fresh slot instead of reusing
//     the just-released slot immediately.
//   - Duplicate-DOWN protection, redundant-MOVE suppression, and orphan-UP guard.
//   - releaseAll() guarantees every active finger emits TOUCH_UP at its last
//     emitted coordinates (zero stuck touches on Alt-Tab, Edit Mode, or profile swap).
// ============================================================================
class TouchFingerPool {
public:
    static const int MAX_FINGERS = 10;

    TouchFingerPool();

    void setDownstreamSink(TouchEmitFn fn, void* ctx);

    // Maps a logical channel touch event to a bounded 0..9 finger ID and forwards downstream.
    // Returns true if a downstream packet was emitted.
    bool emit(TouchAction action, uint64_t logicalId,
              int32_t px, int32_t py, bool clampToScreen);

    // Releases one logical channel if currently active (returns true if TOUCH_UP was emitted).
    bool releaseLogical(uint64_t logicalId);

    // Cleanly emits TOUCH_UP for every active slot in 0..9. Returns number of fingers lifted.
    int releaseAll();

    // Inspection helpers for telemetry and unit testing
    int activeCount() const;
    int findSlot(uint64_t logicalId) const;
    bool isLogicalActive(uint64_t logicalId) const { return findSlot(logicalId) >= 0; }

private:
    struct SlotState {
        bool active = false;
        uint64_t logicalId = 0;
        int32_t lastPx = 0;
        int32_t lastPy = 0;
        bool lastClamped = true;
    };

    int allocateSlot();
    void freeSlot(int slot);
    void forward(TouchAction action, int slot, int32_t px, int32_t py, bool clampToScreen);

    TouchEmitFn sinkFn_ = nullptr;
    void* sinkCtx_ = nullptr;

    SlotState slots_[MAX_FINGERS];
    int freeQueue_[MAX_FINGERS];
    int freeHead_ = 0;
    int freeTail_ = 0;
    int freeCount_ = 0;
};
