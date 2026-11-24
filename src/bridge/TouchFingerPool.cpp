#include "bridge/TouchFingerPool.hpp"

TouchFingerPool::TouchFingerPool() {
    freeHead_ = 0;
    freeTail_ = 0;
    freeCount_ = MAX_FINGERS;
    for (int i = 0; i < MAX_FINGERS; ++i) {
        slots_[i] = SlotState();
        freeQueue_[i] = i;
    }
}

void TouchFingerPool::setDownstreamSink(TouchEmitFn fn, void* ctx) {
    sinkFn_ = fn;
    sinkCtx_ = ctx;
}

int TouchFingerPool::findSlot(uint64_t logicalId) const {
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (slots_[i].active && slots_[i].logicalId == logicalId) {
            return i;
        }
    }
    return -1;
}

int TouchFingerPool::activeCount() const {
    return MAX_FINGERS - freeCount_;
}

int TouchFingerPool::allocateSlot() {
    if (freeCount_ <= 0) return -1;
    int slot = freeQueue_[freeHead_];
    freeHead_ = (freeHead_ + 1) % MAX_FINGERS;
    --freeCount_;
    return slot;
}

void TouchFingerPool::freeSlot(int slot) {
    if (slot < 0 || slot >= MAX_FINGERS) return;
    if (!slots_[slot].active) return;
    slots_[slot].active = false;
    slots_[slot].logicalId = 0;
    if (freeCount_ < MAX_FINGERS) {
        freeQueue_[freeTail_] = slot;
        freeTail_ = (freeTail_ + 1) % MAX_FINGERS;
        ++freeCount_;
    }
}

void TouchFingerPool::forward(TouchAction action, int slot, int32_t px, int32_t py, bool clampToScreen) {
    if (sinkFn_) {
        sinkFn_(sinkCtx_, action, static_cast<uint64_t>(slot), px, py, clampToScreen);
    }
}

bool TouchFingerPool::emit(TouchAction action, uint64_t logicalId,
                           int32_t px, int32_t py, bool clampToScreen) {
    int existing = findSlot(logicalId);

    if (action == TOUCH_DOWN) {
        // If this logical channel was somehow still active, cleanly lift its old stroke first
        if (existing >= 0) {
            forward(TOUCH_UP, existing, slots_[existing].lastPx, slots_[existing].lastPy, slots_[existing].lastClamped);
            freeSlot(existing);
        }
        int slot = allocateSlot();
        if (slot < 0) {
            // All 10 Android touch slots are occupied; drop cleanly rather than corrupting PointersState
            return false;
        }
        slots_[slot].active = true;
        slots_[slot].logicalId = logicalId;
        slots_[slot].lastPx = px;
        slots_[slot].lastPy = py;
        slots_[slot].lastClamped = clampToScreen;
        forward(TOUCH_DOWN, slot, px, py, clampToScreen);
        return true;
    }

    if (action == TOUCH_MOVE) {
        if (existing < 0) return false;
        if (slots_[existing].lastPx == px && slots_[existing].lastPy == py) {
            return false;
        }
        slots_[existing].lastPx = px;
        slots_[existing].lastPy = py;
        slots_[existing].lastClamped = clampToScreen;
        forward(TOUCH_MOVE, existing, px, py, clampToScreen);
        return true;
    }

    // TOUCH_UP
    if (existing < 0) return false;
    forward(TOUCH_UP, existing, px, py, clampToScreen);
    freeSlot(existing);
    return true;
}

bool TouchFingerPool::releaseLogical(uint64_t logicalId) {
    int slot = findSlot(logicalId);
    if (slot < 0) return false;
    int32_t lx = slots_[slot].lastPx;
    int32_t ly = slots_[slot].lastPy;
    bool lc = slots_[slot].lastClamped;
    forward(TOUCH_UP, slot, lx, ly, lc);
    freeSlot(slot);
    return true;
}

int TouchFingerPool::releaseAll() {
    int released = 0;
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (slots_[i].active) {
            int32_t lx = slots_[i].lastPx;
            int32_t ly = slots_[i].lastPy;
            bool lc = slots_[i].lastClamped;
            forward(TOUCH_UP, i, lx, ly, lc);
            freeSlot(i);
            ++released;
        }
    }
    return released;
}
