#include "engine/GamepadController.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstring>

namespace {

// Standard XInput wire layout (identical across xinput1_4.dll, xinput1_3.dll, xinput9_1_0.dll)
struct RawXInputGamepad {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct RawXInputState {
    DWORD dwPacketNumber;
    RawXInputGamepad Gamepad;
};

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, RawXInputState* pState);

// Standard XINPUT_GAMEPAD_* bitmasks
const uint16_t kBtnDpadUp    = 0x0001;
const uint16_t kBtnDpadDown  = 0x0002;
const uint16_t kBtnDpadLeft  = 0x0004;
const uint16_t kBtnDpadRight = 0x0008;
const uint16_t kBtnStart     = 0x0010;
const uint16_t kBtnBack      = 0x0020;
const uint16_t kBtnLThumb    = 0x0040;
const uint16_t kBtnRThumb    = 0x0080;
const uint16_t kBtnLB        = 0x0100;
const uint16_t kBtnRB        = 0x0200;
const uint16_t kBtnA         = 0x1000;
const uint16_t kBtnB         = 0x2000;
const uint16_t kBtnX         = 0x4000;
const uint16_t kBtnY         = 0x8000;

struct BtnVkMap {
    uint16_t mask;
    int vk;
};

const BtnVkMap kButtonTable[] = {
    { kBtnA,         VK_SPACE },   // Jump
    { kBtnB,         'Q'      },   // Crouch / Switch
    { kBtnX,         'R'      },   // Reload
    { kBtnY,         '1'      },   // Weapon 1
    { kBtnLB,        'E'      },   // Gloo / Utility
    { kBtnRB,        'F'      },   // Loot / Interact
    { kBtnDpadUp,    '4'      },   // Medkit
    { kBtnDpadDown,  'Z'      },   // Prone
    { kBtnDpadLeft,  '2'      },   // Weapon 2
    { kBtnDpadRight, '3'      },   // Melee
    { kBtnStart,     'M'      },   // Map
    { kBtnBack,      0xA0     },   // LSHIFT (Bag / Sprint)
    { kBtnLThumb,    'G'      },   // Use
    { kBtnRThumb,    'C'      }    // Crouch alt
};

int32_t clampi32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

GamepadController::GamepadController() {}

GamepadController::~GamepadController() {
    if (xinputDll_) {
        FreeLibrary(reinterpret_cast<HMODULE>(xinputDll_));
        xinputDll_ = nullptr;
        xinputGetStateFn_ = nullptr;
    }
}

void GamepadController::setTouchSink(TouchEmitFn fn, void* ctx) {
    sink_ = fn;
    sinkCtx_ = ctx;
}

void GamepadController::ensureXInputLoaded() {
    if (loadAttempted_) return;
    loadAttempted_ = true;

    const char* dlls[3] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3; ++i) {
        HMODULE mod = LoadLibraryA(dlls[i]);
        if (mod) {
            void* fn = reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState"));
            if (fn) {
                xinputDll_ = mod;
                xinputGetStateFn_ = fn;
                return;
            }
            FreeLibrary(mod);
        }
    }
}

void GamepadController::normalizeStick(int16_t rawX, int16_t rawY, float deadzone, float& outX, float& outY) {
    float fx = static_cast<float>(rawX) / 32767.0f;
    float fy = static_cast<float>(rawY) / 32767.0f;
    if (fx < -1.0f) fx = -1.0f;
    if (fx >  1.0f) fx =  1.0f;
    if (fy < -1.0f) fy = -1.0f;
    if (fy >  1.0f) fy =  1.0f;

    float mag = std::sqrt(fx * fx + fy * fy);
    if (mag <= deadzone) {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }
    float scaled = (mag - deadzone) / (1.0f - deadzone);
    if (scaled > 1.0f) scaled = 1.0f;
    outX = (fx / mag) * scaled;
    outY = (fy / mag) * scaled;
}

void GamepadController::emit(TouchAction action, int32_t px, int32_t py, uint32_t& packetsCounter) {
    if (!sink_) return;
    sink_(sinkCtx_, action, PointerId::JOYSTICK, px, py, true);
    ++packetsCounter;
}

void GamepadController::releaseAll(uint32_t& packetsCounter) {
    if (joyDown_) {
        joyDown_ = false;
        emit(TOUCH_UP, joyLastX_, joyLastY_, packetsCounter);
    }
    prevButtons_ = 0;
    prevLt_ = false;
    prevRt_ = false;
}

void GamepadController::pollAndProcess(const Profile& profile, uint32_t screenW, uint32_t screenH,
                                       double dtMs, uint32_t& packetsCounter, GamepadFrameOutput& out) {
    out = GamepadFrameOutput();
    if (!enabled_) {
        if (connected_ || joyDown_) {
            connected_ = false;
            releaseAll(packetsCounter);
        }
        return;
    }

    ensureXInputLoaded();
    if (!xinputGetStateFn_) return;

    elapsedMs_ += dtMs;
    PFN_XInputGetState fnGet = reinterpret_cast<PFN_XInputGetState>(xinputGetStateFn_);

    RawXInputState st;
    ZeroMemory(&st, sizeof(st));
    bool found = false;

    if (connected_) {
        if (fnGet(static_cast<DWORD>(activeSlot_), &st) == ERROR_SUCCESS) {
            found = true;
        }
    }
    // Avoid spamming disconnected slots: scan slots 0..3 every ~1200 ms when idle
    if (!found && (connected_ || elapsedMs_ >= nextScanAtMs_)) {
        nextScanAtMs_ = elapsedMs_ + 1200.0;
        for (int slot = 0; slot < 4; ++slot) {
            if (fnGet(static_cast<DWORD>(slot), &st) == ERROR_SUCCESS) {
                activeSlot_ = slot;
                found = true;
                break;
            }
        }
    }

    GamepadSnapshot snap;
    snap.connected = found;
    if (found) {
        normalizeStick(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, 0.18f, snap.lx, snap.ly);
        normalizeStick(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, 0.16f, snap.rx, snap.ry);
        snap.lt = static_cast<float>(st.Gamepad.bLeftTrigger) / 255.0f;
        snap.rt = static_cast<float>(st.Gamepad.bRightTrigger) / 255.0f;
        snap.buttons = st.Gamepad.wButtons;
    }

    processSnapshot(snap, profile, screenW, screenH, dtMs, packetsCounter, out);
}

void GamepadController::processSnapshot(const GamepadSnapshot& snap, const Profile& profile,
                                        uint32_t screenW, uint32_t screenH, double dtMs,
                                        uint32_t& packetsCounter, GamepadFrameOutput& out) {
    out = GamepadFrameOutput();
    out.connected = snap.connected;

    if (!snap.connected || !profile.keymapEnabled) {
        if (connected_ && !snap.connected) {
            connected_ = false;
        }
        if (joyDown_) {
            joyDown_ = false;
            emit(TOUCH_UP, joyLastX_, joyLastY_, packetsCounter);
        }
        // Release any held gamepad buttons cleanly if controller unplugged mid-game
        if (!snap.connected) {
            for (size_t i = 0; i < sizeof(kButtonTable) / sizeof(kButtonTable[0]); ++i) {
                if ((prevButtons_ & kButtonTable[i].mask) && out.keyDeltaCount < 16) {
                    out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ kButtonTable[i].vk, false };
                }
            }
            if (prevRt_ && out.keyDeltaCount < 16) out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ profile.fireVk, false };
            if (prevLt_ && out.keyDeltaCount < 16) out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ VK_RBUTTON, false };
            prevButtons_ = 0;
            prevLt_ = false;
            prevRt_ = false;
        }
        return;
    }

    connected_ = true;
    const int32_t w = static_cast<int32_t>(screenW > 64 ? screenW : 1920);
    const int32_t h = static_cast<int32_t>(screenH > 64 ? screenH : 1080);

    // 1. Left Thumbstick -> smooth 360° analog virtual joystick (walk -> run -> sprint radius!)
    const float lMag = std::sqrt(snap.lx * snap.lx + snap.ly * snap.ly);
    if (lMag > 0.02f) {
        out.analogJoyActive = true;
        const double cx = static_cast<double>(profile.joyX) * static_cast<double>(w);
        const double cy = static_cast<double>(profile.joyY) * static_cast<double>(h);
        const double maxRad = (lMag > 0.90f && profile.joySprintRadius > profile.joyRadius)
            ? (static_cast<double>(profile.joySprintRadius) * static_cast<double>(h))
            : (static_cast<double>(profile.joyRadius) * static_cast<double>(h));
        const double rad = maxRad * static_cast<double>(lMag > 1.0f ? 1.0f : lMag);

        // XInput +Y is UP, Android touch +Y is DOWN -> negate snap.ly
        int32_t tx = clampi32(static_cast<int32_t>(std::floor(cx + (snap.lx / lMag) * rad + 0.5)), 2, w - 3);
        int32_t ty = clampi32(static_cast<int32_t>(std::floor(cy - (snap.ly / lMag) * rad + 0.5)), 2, h - 3);

        if (!joyDown_) {
            int32_t sx = clampi32(static_cast<int32_t>(std::floor(cx + 0.5)), 2, w - 3);
            int32_t sy = clampi32(static_cast<int32_t>(std::floor(cy + 0.5)), 2, h - 3);
            emit(TOUCH_DOWN, sx, sy, packetsCounter);
            joyDown_ = true;
            joyLastX_ = sx;
            joyLastY_ = sy;
        }
        if (std::abs(tx - joyLastX_) >= 2 || std::abs(ty - joyLastY_) >= 2) {
            emit(TOUCH_MOVE, tx, ty, packetsCounter);
            joyLastX_ = tx;
            joyLastY_ = ty;
        }
    } else if (joyDown_) {
        joyDown_ = false;
        emit(TOUCH_UP, joyLastX_, joyLastY_, packetsCounter);
    }

    // 2. Right Thumbstick -> quadratic 360° camera look velocity scaled by dtMs
    const float rMag = std::sqrt(snap.rx * snap.rx + snap.ry * snap.ry);
    if (rMag > 0.01f && dtMs > 0.0) {
        const double curve = static_cast<double>(rMag * (0.45f + 0.55f * rMag));
        const double baseSpeedPxPerSec = 980.0;
        const double step = baseSpeedPxPerSec * (dtMs / 1000.0) * curve;
        out.lookDx = (static_cast<double>(snap.rx) / static_cast<double>(rMag)) * step;
        out.lookDy = (-static_cast<double>(snap.ry) / static_cast<double>(rMag)) * step;
    }

    // 3. Triggers (RT -> Fire, LT -> Scope)
    const bool rtNow = (snap.rt >= 0.22f);
    const bool ltNow = (snap.lt >= 0.22f);
    if (rtNow != prevRt_ && out.keyDeltaCount < 16) {
        prevRt_ = rtNow;
        out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ profile.fireVk, rtNow };
    }
    if (ltNow != prevLt_ && out.keyDeltaCount < 16) {
        prevLt_ = ltNow;
        out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ VK_RBUTTON, ltNow };
    }

    // 4. Face, Shoulder & D-Pad buttons -> edge-triggered profile button events
    const uint16_t changed = static_cast<uint16_t>(snap.buttons ^ prevButtons_);
    if (changed != 0) {
        for (size_t i = 0; i < sizeof(kButtonTable) / sizeof(kButtonTable[0]); ++i) {
            if ((changed & kButtonTable[i].mask) && out.keyDeltaCount < 16) {
                bool pressed = (snap.buttons & kButtonTable[i].mask) != 0;
                out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ kButtonTable[i].vk, pressed };
            }
        }
        prevButtons_ = snap.buttons;
    }
}
