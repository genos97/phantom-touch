#include "engine/GamepadController.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cctype>
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

// Standard XINPUT_GAMEPAD_* bitmasks (including Guide bit 0x0400 via XInputGetStateEx ordinal 100)
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
const uint16_t kBtnGuide     = 0x0400;
const uint16_t kBtnA         = 0x1000;
const uint16_t kBtnB         = 0x2000;
const uint16_t kBtnX         = 0x4000;
const uint16_t kBtnY         = 0x8000;

struct PadMaskEntry {
    int padId;
    uint16_t mask; // 0 for LT/RT triggers
    int defaultVk;
};

const PadMaskEntry kPadTable[14] = {
    { PAD_A,          kBtnA,         VK_SPACE },
    { PAD_B,          kBtnB,         'Q'      },
    { PAD_X,          kBtnX,         'R'      },
    { PAD_Y,          kBtnY,         '1'      },
    { PAD_LB,         kBtnLB,        'E'      },
    { PAD_RB,         kBtnRB,        'F'      },
    { PAD_BACK,       kBtnBack,      0xA0     },
    { PAD_START,      kBtnStart,     'M'      },
    { PAD_LS,         kBtnLThumb,    'G'      },
    { PAD_RS,         kBtnRThumb,    'C'      },
    { PAD_DPAD_UP,    kBtnDpadUp,    '4'      },
    { PAD_DPAD_DOWN,  kBtnDpadDown,  'Z'      },
    { PAD_DPAD_LEFT,  kBtnDpadLeft,  '2'      },
    { PAD_DPAD_RIGHT, kBtnDpadRight, '3'      }
};

// HID Gamepad Report Descriptor ported from Genymobile/scrcpy app/src/hid/hid_gamepad.c (Apache-2.0)
const uint8_t kScHidGamepadReportDesc[] = {
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x09, 0x05,                   // Usage (Gamepad)
    0xA1, 0x01,                   // Collection (Application)
    0xA1, 0x00,                   // Collection (Physical)
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x09, 0x30,                   // Usage (X)
    0x09, 0x31,                   // Usage (Y)
    0x09, 0x33,                   // Usage (Rx)
    0x09, 0x34,                   // Usage (Ry)
    0x15, 0x00,                   // Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical Maximum (65535)
    0x75, 0x10,                   // Report Size (16)
    0x95, 0x04,                   // Report Count (4)
    0x81, 0x02,                   // Input (Data, Variable, Absolute)
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x09, 0x32,                   // Usage (Z)
    0x09, 0x35,                   // Usage (Rz)
    0x15, 0x00,                   // Logical Minimum (0)
    0x26, 0xFF, 0x7F,             // Logical Maximum (32767)
    0x75, 0x10,                   // Report Size (16)
    0x95, 0x02,                   // Report Count (2)
    0x81, 0x02,                   // Input (Data, Variable, Absolute)
    0x05, 0x09,                   // Usage Page (Buttons)
    0x19, 0x01,                   // Usage Minimum (1)
    0x29, 0x10,                   // Usage Maximum (16)
    0x15, 0x00,                   // Logical Minimum (0)
    0x25, 0x01,                   // Logical Maximum (1)
    0x95, 0x10,                   // Report Count (16)
    0x75, 0x01,                   // Report Size (1)
    0x81, 0x02,                   // Input (Data, Variable, Absolute)
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x09, 0x39,                   // Usage (Hat switch)
    0x15, 0x01,                   // Logical Minimum (1)
    0x25, 0x08,                   // Logical Maximum (8)
    0x75, 0x04,                   // Report Size (4)
    0x95, 0x01,                   // Report Count (1)
    0x81, 0x42,                   // Input (Data, Variable, Null State)
    0xC0,                         // End Collection
    0xC0                          // End Collection
};

void writeLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

int32_t clampi32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float clampRangef(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::string trimStr(const std::string& raw) {
    size_t s = 0;
    while (s < raw.size() && std::isspace(static_cast<unsigned char>(raw[s]))) ++s;
    size_t e = raw.size();
    while (e > s && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    return raw.substr(s, e - s);
}

} // namespace

GamepadController::GamepadController() {
    std::memset(lastUsedVk_, 0, sizeof(lastUsedVk_));
    std::memset(lastHidReport_, 0, sizeof(lastHidReport_));
}

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

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
    const wchar_t* dlls[3] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < 3; ++i) {
        HMODULE mod = LoadLibraryExW(dlls[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (mod) {
            void* fn = reinterpret_cast<void*>(GetProcAddress(mod, MAKEINTRESOURCEA(100))); // XInputGetStateEx
            if (!fn) fn = reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState"));
            if (fn) {
                xinputDll_ = mod;
                xinputGetStateFn_ = fn;
                return;
            }
            FreeLibrary(mod);
        }
    }
}

const uint8_t* GamepadController::hidReportDescriptor(uint16_t& outSize) {
    outSize = static_cast<uint16_t>(sizeof(kScHidGamepadReportDesc));
    return kScHidGamepadReportDesc;
}

void GamepadController::packHidReport(int16_t rawLx, int16_t rawLy, int16_t rawRx, int16_t rawRy,
                                      uint8_t rawLt, uint8_t rawRt, uint16_t xinputButtons,
                                      bool guidePressed, uint8_t outReport15[15]) {
    auto axisPlus = [](int16_t v) -> uint16_t {
        return static_cast<uint16_t>(static_cast<int32_t>(v) + 0x8000);
    };
    auto axisInvY = [](int16_t v) -> uint16_t {
        int32_t inv = -static_cast<int32_t>(v);
        if (inv > 32767) inv = 32767;
        if (inv < -32768) inv = -32768;
        return static_cast<uint16_t>(inv + 0x8000);
    };

    writeLe16(outReport15 + 0,  axisPlus(rawLx));
    writeLe16(outReport15 + 2,  axisInvY(rawLy));
    writeLe16(outReport15 + 4,  axisPlus(rawRx));
    writeLe16(outReport15 + 6,  axisInvY(rawRy));
    writeLe16(outReport15 + 8,  static_cast<uint16_t>((static_cast<uint32_t>(rawLt) * 32767u) / 255u));
    writeLe16(outReport15 + 10, static_cast<uint16_t>((static_cast<uint32_t>(rawRt) * 32767u) / 255u));

    uint16_t hidBtns = 0;
    if (xinputButtons & kBtnA)      hidBtns |= 0x0001u;
    if (xinputButtons & kBtnB)      hidBtns |= 0x0002u;
    if (xinputButtons & kBtnX)      hidBtns |= 0x0008u;
    if (xinputButtons & kBtnY)      hidBtns |= 0x0010u;
    if (xinputButtons & kBtnLB)     hidBtns |= 0x0040u;
    if (xinputButtons & kBtnRB)     hidBtns |= 0x0080u;
    if (xinputButtons & kBtnBack)   hidBtns |= 0x0400u;
    if (xinputButtons & kBtnStart)  hidBtns |= 0x0800u;
    if (guidePressed || (xinputButtons & kBtnGuide)) hidBtns |= 0x1000u;
    if (xinputButtons & kBtnLThumb) hidBtns |= 0x2000u;
    if (xinputButtons & kBtnRThumb) hidBtns |= 0x4000u;
    writeLe16(outReport15 + 12, hidBtns);

    uint8_t dpad = 0;
    if (xinputButtons & kBtnDpadUp) {
        if (xinputButtons & kBtnDpadLeft) dpad = 8;
        else if (xinputButtons & kBtnDpadRight) dpad = 2;
        else dpad = 1;
    } else if (xinputButtons & kBtnDpadDown) {
        if (xinputButtons & kBtnDpadLeft) dpad = 6;
        else if (xinputButtons & kBtnDpadRight) dpad = 4;
        else dpad = 5;
    } else if (xinputButtons & kBtnDpadLeft) {
        dpad = 7;
    } else if (xinputButtons & kBtnDpadRight) {
        dpad = 3;
    }
    outReport15[14] = dpad;
}

int GamepadController::resolvePadButtonVk(int padBtnId, const Profile& profile) {
    if (padBtnId >= 0 && padBtnId < PAD_BUTTON_COUNT) {
        std::string spec = trimStr(profile.padMap[padBtnId]);
        if (!spec.empty()) {
            if (lstrcmpiA(spec.c_str(), "NONE") == 0 || lstrcmpiA(spec.c_str(), "UNBOUND") == 0) return 0;
            if (lstrcmpiA(spec.c_str(), "FIRE") == 0)  return profile.fireVk;
            if (lstrcmpiA(spec.c_str(), "SCOPE") == 0) return VK_RBUTTON;
            for (size_t i = 0; i < profile.buttons.size(); ++i) {
                if (!profile.buttons[i].label.empty() &&
                    lstrcmpiA(profile.buttons[i].label.c_str(), spec.c_str()) == 0) {
                    return profile.buttons[i].vk;
                }
            }
            int vk = KeyCodes::fromName(spec);
            if (vk != 0) return vk;
        }
    }
    if (padBtnId == PAD_RT) return profile.fireVk;
    if (padBtnId == PAD_LT) return VK_RBUTTON;
    for (size_t i = 0; i < sizeof(kPadTable) / sizeof(kPadTable[0]); ++i) {
        if (kPadTable[i].padId == padBtnId) return kPadTable[i].defaultVk;
    }
    return 0;
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

void GamepadController::releaseJoystickOnly(uint32_t& packetsCounter) {
    if (joyDown_) {
        joyDown_ = false;
        emit(TOUCH_UP, joyLastX_, joyLastY_, packetsCounter);
    }
}

void GamepadController::suspendForFocusLoss(uint32_t& packetsCounter, GamepadFrameOutput& out) {
    out = GamepadFrameOutput();
    out.connected = connected_;
    releaseJoystickOnly(packetsCounter);
    for (size_t i = 0; i < sizeof(kPadTable) / sizeof(kPadTable[0]); ++i) {
        if ((prevButtons_ & kPadTable[i].mask) && out.keyDeltaCount < 16) {
            int vk = lastUsedVk_[kPadTable[i].padId];
            if (vk != 0) out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ vk, false };
        }
    }
    if (prevRt_ && out.keyDeltaCount < 16 && lastUsedVk_[PAD_RT] != 0) {
        out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ lastUsedVk_[PAD_RT], false };
    }
    if (prevLt_ && out.keyDeltaCount < 16 && lastUsedVk_[PAD_LT] != 0) {
        out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ lastUsedVk_[PAD_LT], false };
    }
    suppressedButtons_ |= prevButtons_;
    if (prevLt_) suppressedLt_ = true;
    if (prevRt_) suppressedRt_ = true;
    prevButtons_ = 0;
    prevLt_ = false;
    prevRt_ = false;
    prevNavButtons_ = 0;
    prevNavGuide_ = false;

    if (uhidCreated_) {
        uint8_t neutral[15];
        packHidReport(0, 0, 0, 0, 0, 0, 0, false, neutral);
        if (!hasLastHidReport_ || std::memcmp(lastHidReport_, neutral, 15) != 0) {
            std::memcpy(lastHidReport_, neutral, 15);
            std::memcpy(out.uhidReport, neutral, 15);
            hasLastHidReport_ = true;
            out.uhidReportChanged = true;
        }
        out.uhidActive = true;
    }
}

void GamepadController::releaseAll(uint32_t& packetsCounter) {
    releaseJoystickOnly(packetsCounter);
    suppressedButtons_ |= prevButtons_;
    if (prevLt_) suppressedLt_ = true;
    if (prevRt_) suppressedRt_ = true;
    prevButtons_ = 0;
    prevLt_ = false;
    prevRt_ = false;
    prevNavButtons_ = 0;
    prevNavGuide_ = false;
}

void GamepadController::pollAndProcess(const Profile& profile, uint32_t screenW, uint32_t screenH,
                                       double dtMs, uint32_t& packetsCounter, GamepadFrameOutput& out,
                                       bool suppressLeftStick, bool shellMenuOpen) {
    out = GamepadFrameOutput();
    if (!enabled_) {
        if (connected_ || joyDown_ || prevButtons_ != 0 || prevLt_ || prevRt_ || uhidCreated_) {
            suspendForFocusLoss(packetsCounter, out);
            connected_ = false;
            suppressedButtons_ = 0;
            suppressedLt_ = false;
            suppressedRt_ = false;
            if (uhidCreated_) {
                out.uhidNeedDestroy = true;
                out.uhidActive = false;
                uhidCreated_ = false;
                hasLastHidReport_ = false;
            }
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

    float dzL = clampRangef(profile.padDeadzoneL, 0.05f, 0.55f);
    float dzR = clampRangef(profile.padDeadzoneR, 0.05f, 0.55f);
    GamepadSnapshot snap;
    snap.connected = found;
    if (found) {
        normalizeStick(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, dzL, snap.lx, snap.ly);
        normalizeStick(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, dzR, snap.rx, snap.ry);
        snap.lt = static_cast<float>(st.Gamepad.bLeftTrigger) / 255.0f;
        snap.rt = static_cast<float>(st.Gamepad.bRightTrigger) / 255.0f;
        snap.buttons = static_cast<uint16_t>(st.Gamepad.wButtons & ~kBtnGuide);
        snap.rawLx = st.Gamepad.sThumbLX;
        snap.rawLy = st.Gamepad.sThumbLY;
        snap.rawRx = st.Gamepad.sThumbRX;
        snap.rawRy = st.Gamepad.sThumbRY;
        snap.rawLt = st.Gamepad.bLeftTrigger;
        snap.rawRt = st.Gamepad.bRightTrigger;
        snap.guidePressed = (st.Gamepad.wButtons & kBtnGuide) != 0;
        snap.hasRawAxes = true;
    }

    processSnapshot(snap, profile, screenW, screenH, dtMs, packetsCounter, out, suppressLeftStick, shellMenuOpen);
}

void GamepadController::processSnapshot(const GamepadSnapshot& snap, const Profile& profile,
                                        uint32_t screenW, uint32_t screenH, double dtMs,
                                        uint32_t& packetsCounter, GamepadFrameOutput& out,
                                        bool suppressLeftStick, bool shellMenuOpen) {
    out = GamepadFrameOutput();
    out.connected = snap.connected;

    // E4: Clear neutral-button latches as soon as each button/trigger physically releases
    if (!snap.connected) {
        suppressedButtons_ = 0;
        suppressedLt_ = false;
        suppressedRt_ = false;
    } else {
        suppressedButtons_ = static_cast<uint16_t>(suppressedButtons_ & snap.buttons);
        if (snap.lt < 0.15f) suppressedLt_ = false;
        if (snap.rt < 0.15f) suppressedRt_ = false;
    }

    // F6.5: Shell menu / popup / picker navigation rising edges
    if (snap.connected) {
        if (snap.guidePressed && !prevNavGuide_) out.shellNavMask |= PAD_NAV_GUIDE;
        prevNavGuide_ = snap.guidePressed;
        if (shellMenuOpen) {
            if ((snap.buttons & kBtnDpadUp)    && !(prevNavButtons_ & kBtnDpadUp))    out.shellNavMask |= PAD_NAV_UP;
            if ((snap.buttons & kBtnDpadDown)  && !(prevNavButtons_ & kBtnDpadDown))  out.shellNavMask |= PAD_NAV_DOWN;
            if ((snap.buttons & kBtnDpadLeft)  && !(prevNavButtons_ & kBtnDpadLeft))  out.shellNavMask |= PAD_NAV_LEFT;
            if ((snap.buttons & kBtnDpadRight) && !(prevNavButtons_ & kBtnDpadRight)) out.shellNavMask |= PAD_NAV_RIGHT;
            if ((snap.buttons & kBtnA)         && !(prevNavButtons_ & kBtnA))         out.shellNavMask |= PAD_NAV_SELECT;
            if ((snap.buttons & kBtnB)         && !(prevNavButtons_ & kBtnB))         out.shellNavMask |= PAD_NAV_BACK;
            prevNavButtons_ = snap.buttons;
        } else {
            prevNavButtons_ = 0;
        }
    } else {
        prevNavButtons_ = 0;
        prevNavGuide_ = false;
    }

    auto flushTouchDeltas = [&]() {
        releaseJoystickOnly(packetsCounter);
        for (size_t i = 0; i < sizeof(kPadTable) / sizeof(kPadTable[0]); ++i) {
            if ((prevButtons_ & kPadTable[i].mask) && out.keyDeltaCount < 16) {
                int vk = lastUsedVk_[kPadTable[i].padId];
                if (vk != 0) out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ vk, false };
            }
        }
        if (prevRt_ && out.keyDeltaCount < 16 && lastUsedVk_[PAD_RT] != 0) {
            out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ lastUsedVk_[PAD_RT], false };
        }
        if (prevLt_ && out.keyDeltaCount < 16 && lastUsedVk_[PAD_LT] != 0) {
            out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ lastUsedVk_[PAD_LT], false };
        }
        prevButtons_ = 0;
        prevLt_ = false;
        prevRt_ = false;
    };

    // F6.3: Native UHID Gamepad Passthrough
    if (profile.padMode == PAD_MODE_NATIVE) {
        flushTouchDeltas();
        if (!snap.connected) {
            if (connected_) connected_ = false;
            if (uhidCreated_) {
                out.uhidNeedDestroy = true;
                uhidCreated_ = false;
                hasLastHidReport_ = false;
            }
            return;
        }
        connected_ = true;
        out.uhidActive = true;
        if (!uhidCreated_) {
            out.uhidNeedCreate = true;
            uhidCreated_ = true;
            hasLastHidReport_ = false;
        }
        int16_t rxLx = snap.hasRawAxes ? snap.rawLx : static_cast<int16_t>(clampRangef(snap.lx, -1.0f, 1.0f) * 32767.0f);
        int16_t rxLy = snap.hasRawAxes ? snap.rawLy : static_cast<int16_t>(clampRangef(snap.ly, -1.0f, 1.0f) * 32767.0f);
        int16_t rxRx = snap.hasRawAxes ? snap.rawRx : static_cast<int16_t>(clampRangef(snap.rx, -1.0f, 1.0f) * 32767.0f);
        int16_t rxRy = snap.hasRawAxes ? snap.rawRy : static_cast<int16_t>(clampRangef(snap.ry, -1.0f, 1.0f) * 32767.0f);
        uint8_t rLt  = snap.hasRawAxes ? snap.rawLt : static_cast<uint8_t>(clampRangef(snap.lt, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t rRt  = snap.hasRawAxes ? snap.rawRt : static_cast<uint8_t>(clampRangef(snap.rt, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint16_t fwdBtns = static_cast<uint16_t>(snap.buttons & ~suppressedButtons_);
        if (shellMenuOpen) {
            fwdBtns = static_cast<uint16_t>(fwdBtns & ~(kBtnDpadUp | kBtnDpadDown | kBtnDpadLeft | kBtnDpadRight | kBtnA | kBtnB));
        }
        uint8_t rep[15];
        packHidReport(rxLx, rxLy, rxRx, rxRy, rLt, rRt, fwdBtns, snap.guidePressed, rep);
        if (!hasLastHidReport_ || out.uhidNeedCreate || std::memcmp(lastHidReport_, rep, 15) != 0) {
            std::memcpy(lastHidReport_, rep, 15);
            std::memcpy(out.uhidReport, rep, 15);
            hasLastHidReport_ = true;
            out.uhidReportChanged = true;
        }
        return;
    }

    if (uhidCreated_) {
        out.uhidNeedDestroy = true;
        uhidCreated_ = false;
        hasLastHidReport_ = false;
    }

    if (!snap.connected || !profile.keymapEnabled) {
        if (connected_ && !snap.connected) {
            connected_ = false;
        }
        flushTouchDeltas();
        return;
    }

    connected_ = true;
    const int32_t w = static_cast<int32_t>(screenW > 64 ? screenW : 1920);
    const int32_t h = static_cast<int32_t>(screenH > 64 ? screenH : 1080);

    // 1. Left Thumbstick -> smooth 360° analog virtual joystick (walk -> run -> sprint radius!)
    const float lMag = std::sqrt(snap.lx * snap.lx + snap.ly * snap.ly);
    out.stickDeflected = (lMag > 0.02f);
    if (out.stickDeflected && !suppressLeftStick) {
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
    } else {
        releaseJoystickOnly(packetsCounter);
    }

    // 2. Right Thumbstick -> quadratic 360° camera look velocity scaled by dtMs and padLookSens
    const float rMag = std::sqrt(snap.rx * snap.rx + snap.ry * snap.ry);
    if (rMag > 0.01f && dtMs > 0.0) {
        const double curve = static_cast<double>(rMag * (0.45f + 0.55f * rMag));
        const double lookSens = static_cast<double>(clampRangef(profile.padLookSens, 0.10f, 4.00f));
        const double baseSpeedPxPerSec = 980.0 * lookSens;
        const double step = baseSpeedPxPerSec * (dtMs / 1000.0) * curve;
        out.lookDx = (static_cast<double>(snap.rx) / static_cast<double>(rMag)) * step;
        out.lookDy = (-static_cast<double>(snap.ry) / static_cast<double>(rMag)) * step;
    }

    // 3. Triggers (RT / LT via per-profile padMap, respecting E4 neutral-return latches)
    const bool rtNow = (snap.rt >= 0.22f) && !suppressedRt_;
    const bool ltNow = (snap.lt >= 0.22f) && !suppressedLt_;
    if (rtNow != prevRt_ && out.keyDeltaCount < 16) {
        prevRt_ = rtNow;
        int vk = rtNow ? resolvePadButtonVk(PAD_RT, profile) : lastUsedVk_[PAD_RT];
        if (rtNow) lastUsedVk_[PAD_RT] = vk;
        if (vk != 0) out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ vk, rtNow };
    }
    if (ltNow != prevLt_ && out.keyDeltaCount < 16) {
        prevLt_ = ltNow;
        int vk = ltNow ? resolvePadButtonVk(PAD_LT, profile) : lastUsedVk_[PAD_LT];
        if (ltNow) lastUsedVk_[PAD_LT] = vk;
        if (vk != 0) out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ vk, ltNow };
    }

    // 4. Face, Shoulder & D-Pad buttons -> per-profile mapped edge events (suppressed during shell menu nav)
    uint16_t effButtons = static_cast<uint16_t>(snap.buttons & ~suppressedButtons_);
    if (shellMenuOpen) {
        effButtons = static_cast<uint16_t>(effButtons & ~(kBtnDpadUp | kBtnDpadDown | kBtnDpadLeft | kBtnDpadRight | kBtnA | kBtnB));
    }
    const uint16_t changed = static_cast<uint16_t>(effButtons ^ prevButtons_);
    if (changed != 0) {
        for (size_t i = 0; i < sizeof(kPadTable) / sizeof(kPadTable[0]); ++i) {
            if ((changed & kPadTable[i].mask) && out.keyDeltaCount < 16) {
                bool pressed = (effButtons & kPadTable[i].mask) != 0;
                int pid = kPadTable[i].padId;
                int vk = pressed ? resolvePadButtonVk(pid, profile) : lastUsedVk_[pid];
                if (pressed) lastUsedVk_[pid] = vk;
                if (vk != 0) {
                    out.keyDeltas[out.keyDeltaCount++] = GamepadKeyDelta{ vk, pressed };
                }
            }
        }
        prevButtons_ = effButtons;
    }
}
