#include "bridge/SpsParser.hpp"
#include "bridge/ScrcpyBridge.hpp"
#include "config/KeyCodes.hpp"
#include "config/Profile.hpp"
#include "config/ProfileStore.hpp"
#include "engine/ActionDispatcher.hpp"
#include "engine/FpsViewController.hpp"
#include "engine/InputEngine.hpp"
#include "engine/JoystickController.hpp"
#include "ui/StudioPanel.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct RecordedTouch {
    TouchAction action;
    uint64_t ptrId;
    int32_t px;
    int32_t py;
    bool clamped;
};

static void recordTouchCb(void* ctx, TouchAction action, uint64_t ptrId,
                          int32_t px, int32_t py, bool clamped) {
    std::vector<RecordedTouch>* log = reinterpret_cast<std::vector<RecordedTouch>*>(ctx);
    RecordedTouch rt = { action, ptrId, px, py, clamped };
    log->push_back(rt);
}

static void testSpsParser() {
    const uint8_t sps1920x1080[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x64, 0x00, 0x28,
        0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xF0, 0x3C, 0x60, 0xC6, 0x58
    };
    uint32_t w = 0, h = 0;
    bool ok = SpsParser::parseH264Resolution(sps1920x1080, sizeof(sps1920x1080), w, h);
    std::printf("[Test] SPS 1920x1080 -> ok=%d, %ux%u\n", ok ? 1 : 0, w, h);
    assert(ok && w == 1920 && h == 1080);

    const uint8_t sps1280x720[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0xC0, 0x1F,
        0xD9, 0x00, 0x50, 0x05, 0xBB, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC0, 0xF1, 0x83, 0x19, 0x60
    };
    w = h = 0;
    ok = SpsParser::parseH264Resolution(sps1280x720, sizeof(sps1280x720), w, h);
    std::printf("[Test] SPS 1280x720  -> ok=%d, %ux%u\n", ok ? 1 : 0, w, h);
    assert(ok && w == 1280 && h == 720);

    const uint8_t junk[] = { 0x00, 0x00, 0x01, 0x67, 0xFF, 0xFF };
    assert(!SpsParser::parseH264Resolution(junk, sizeof(junk), w, h));
}

static void testProfileRoundTrip() {
    Profile orig = ProfileDefaults::createFreeFire();
    orig.sensX = 1.05f;
    orig.sensY = 1.30f;
    orig.fireSensX = 0.80f;
    orig.fireSensY = 1.40f;
    orig.scopeSensX = 0.60f;
    orig.scopeSensY = 0.70f;
    orig.ffCounterAccel = 8.5f;
    orig.unboundedAim = true;
    orig.pollHz = 250;
    orig.joySteppedGlide = true;
    orig.fireMode = FIRE_MODE_CLAW_LEFT;

    KeyNode swipeNode;
    swipeNode.label = "SWP";
    swipeNode.comboVk = 0xA4; // LALT
    swipeNode.vk = '1';
    swipeNode.action = NODE_SWIPE;
    swipeNode.x = 0.30f;
    swipeNode.y = 0.50f;
    swipeNode.x2 = 0.60f;
    swipeNode.y2 = 0.25f;
    swipeNode.durationMs = 140;
    orig.buttons.push_back(swipeNode);

    const std::string tmpPath = "profiles\\_test_v3_roundtrip.cfg";
    assert(ProfileStore::save(tmpPath, orig));

    Profile loaded;
    assert(ProfileStore::load(tmpPath, loaded));
    assert(loaded.name == orig.name);
    assert(loaded.buttons.size() == orig.buttons.size());
    assert(loaded.fireMode == FIRE_MODE_CLAW_LEFT);
    assert(loaded.unboundedAim == true);
    assert(loaded.pollHz == 250);
    assert(loaded.joySteppedGlide == true);
    assert(std::fabs(loaded.ffCounterAccel - 8.5f) < 0.05f);

    const KeyNode& last = loaded.buttons.back();
    assert(last.label == "SWP");
    assert(last.comboVk == 0xA4 && last.vk == '1');
    assert(last.action == NODE_SWIPE);
    assert(std::fabs(last.x2 - 0.60f) < 0.01f);
    assert(last.durationMs == 140);

    int cVk = 0, pVk = 0;
    KeyCodes::parseCombo("LALT+1", cVk, pVk);
    assert(cVk == 0xA4 && pVk == '1');
    assert(KeyCodes::comboToName(cVk, pVk) == "LALT+1");
    assert(ProfileDefaults::actionTypeToName(ProfileDefaults::nameToActionType("DOWN_UP")) == "DOWN_UP");
    assert(ProfileStore::sanitizeFilename("My Free Fire #1!") == "my_free_fire_1");

    Profile blank = ProfileDefaults::createBlank("Custom Test");
    assert(blank.buttons.empty());
    assert(std::fabs(blank.sensX - 0.95f) < 0.01f);
    assert(std::fabs(blank.sensY - 1.15f) < 0.01f);

    std::vector<std::string> files = ProfileStore::listProfiles("profiles");
    assert(!files.empty());

    std::remove(tmpPath.c_str());
    std::printf("[Test] ProfileStore round-trip, KeyCodes & defaults OK (%u nodes, %u profiles)\n",
                static_cast<unsigned>(loaded.buttons.size()), static_cast<unsigned>(files.size()));
}

static void testStudioPanelMouseAndScrollRebind() {
    assert(KeyCodes::toName(0x10) == "LSHIFT");
    assert(KeyCodes::fromName("0x10") == 0xA0);
    assert(KeyCodes::fromName("MMB") == 0x04 && KeyCodes::toName(0x04) == "MMB");
    assert(KeyCodes::fromName("SCROLLUP") == VK_CUSTOM_MWHEEL_UP);
    assert(KeyCodes::fromName("SCROLLDOWN") == VK_CUSTOM_MWHEEL_DOWN);

    Profile loaded = ProfileDefaults::createFreeFire();

    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, 1280, 720);
    HGDIOBJ oldBmp = SelectObject(memDC, bmp);
    HFONT fS = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fB = CreateFontA(14, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fT = CreateFontA(15, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    StudioPanel ui;
    ui.setProfilesDir("profiles", "profiles\\freefire.cfg");
    int sel = 0;
    ui.paint(memDC, 1280, 720, loaded, "profiles\\freefire.cfg", sel, true, fS, fB, fT);

    int px = static_cast<int>(loaded.panelX * 1280.0f);
    int py = static_cast<int>(loaded.panelY * 720.0f);
    StudioUIResult res;
    assert(ui.onMouseDown(px + 330, py + 48, 1280, 720, loaded, sel, res));
    assert(res.cancelRebind == true);

    ui.paint(memDC, 1280, 720, loaded, "profiles\\freefire.cfg", sel, false, fS, fB, fT);
    StudioUIResult res2;
    assert(ui.onMouseDown(px + 180, py + 292, 1280, 720, loaded, sel, res2));
    assert(ui.isRebindingToggleAim() == true);

    StudioUIResult resLmb;
    assert(ui.onMouseBind(VK_LBUTTON, loaded, sel, resLmb));
    assert(ui.isRebindingToggleAim() == true);

    StudioUIResult resShift;
    assert(ui.onKeyDown(VK_SHIFT, loaded, sel, resShift));
    assert(!ui.isRebindingToggleAim() && loaded.toggleAimVk == VK_LSHIFT);

    StudioUIResult resMmbChip;
    assert(ui.onMouseDown(px + 375, py + 292, 1280, 720, loaded, sel, resMmbChip));
    assert(loaded.toggleAimVk == VK_MBUTTON);

    // Key Inspector Row 1 (MMB) & Row 2 (SCROLL UP / SCROLL DOWN)
    StudioUIResult resInspMmb;
    assert(ui.onMouseDown(px + 440 + 12 + 2 * 48 + 20, py + 109, 1280, 720, loaded, sel, resInspMmb));
    assert(loaded.buttons[0].vk == VK_MBUTTON && resInspMmb.cancelRebind);

    StudioUIResult resScUp;
    assert(ui.onMouseDown(px + 440 + 60, py + 137, 1280, 720, loaded, sel, resScUp));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_UP);

    StudioUIResult resScDn;
    assert(ui.onMouseDown(px + 440 + 190, py + 137, 1280, 720, loaded, sel, resScDn));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_DOWN);

    DeleteObject(fS); DeleteObject(fB); DeleteObject(fT);
    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    std::printf("[Test] StudioPanel LShift, MMB & Scroll Up/Down UI interactions OK!\n");
}

static void testJoystickAndActionModules() {
    std::vector<RecordedTouch> log;
    uint32_t pkts = 0;
    Profile p = ProfileDefaults::createFreeFire();

    // 1. Standalone JoystickController 3-stage stepped glide
    JoystickController joy;
    joy.setTouchSink(recordTouchCb, &log);
    assert(joy.handleKey('W', true, true, p, 2000, 1000, 100.0, pkts));
    assert(joy.isDown() && log.size() == 1 && log[0].action == TOUCH_DOWN);
    joy.tick(109.0, pkts); // stage 0
    joy.tick(125.0, pkts); // stage 1 (20%)
    joy.tick(141.0, pkts); // stage 2 (50%)
    joy.tick(157.0, pkts); // stage 3 (100%)
    assert(log.back().action == TOUCH_MOVE && log.back().py < log[0].py);
    assert(joy.handleKey('W', false, true, p, 2000, 1000, 170.0, pkts));
    assert(!joy.isDown() && log.back().action == TOUCH_UP);

    // 2. Standalone ActionDispatcher: combo + multi-notch wheel press
    log.clear();
    ActionDispatcher disp;
    disp.setTouchSink(recordTouchCb, &log);
    KeyNode whl;
    whl.label = "WPN";
    whl.vk = VK_CUSTOM_MWHEEL_UP;
    whl.action = NODE_PRESS;
    whl.x = 0.8f; whl.y = 0.2f;
    p.buttons.push_back(whl);
    disp.resetForProfile(p.buttons.size());

    bool held[512] = {false};
    bool scopeSens = false;
    ActionEffects eff;
    disp.dispatchKey(VK_CUSTOM_MWHEEL_UP, true, true, p, held, 2000, 1000, 200.0, scopeSens, eff, pkts);
    assert(log.size() == 1 && log[0].action == TOUCH_DOWN);
    // Second fast scroll notch lifts & re-taps cleanly
    disp.dispatchKey(VK_CUSTOM_MWHEEL_UP, true, true, p, held, 2000, 1000, 210.0, scopeSens, eff, pkts);
    assert(log.size() == 3 && log[1].action == TOUCH_UP && log[2].action == TOUCH_DOWN);
    disp.tick(p, 250.0, pkts);
    assert(log.back().action == TOUCH_UP);

    std::printf("[Test] Standalone JoystickController & ActionDispatcher OK!\n");
}

static void testFpsViewController() {
    std::vector<RecordedTouch> log;
    uint32_t pkts = 0;

    FpsViewFinger boxed(PointerId::AIM_A, PointerId::AIM_B);
    boxed.setTouchSink(recordTouchCb, &log);
    FpsViewConfig bcfg;
    bcfg.screenW = 2000.0; bcfg.screenH = 1000.0;
    bcfg.centerX = 1200.0; bcfg.centerY = 500.0;
    bcfg.rectLeft = 50.0;  bcfg.rectTop = 50.0;
    bcfg.rectRight = 1950.0; bcfg.rectBottom = 950.0;
    bcfg.sensX = 1.0; bcfg.sensY = 1.0;
    bcfg.ffCounterAccel = 0.0;
    bcfg.unbounded = false;
    bcfg.touchDownRandomRangePx = 50;
    bcfg.touchDownDelayMs = 15;
    bcfg.idleReleaseMs = 700;
    boxed.configure(bcfg);

    boxed.onFocusGain(100.0);
    assert(!boxed.isFingerPlaced() && log.empty());
    boxed.onMouseDelta(0.4, 0.0, 1.0, 1.0, 102.0, pkts);
    assert(!boxed.isFingerPlaced());
    boxed.onMouseDelta(0.7, 0.0, 1.0, 1.0, 104.0, pkts);
    assert(boxed.isFingerPlaced() && log.size() == 1 && log[0].action == TOUCH_DOWN && log[0].ptrId == PointerId::AIM_A);
    assert(std::abs(log[0].px - 1200) <= 50 && std::abs(log[0].py - 500) <= 50);

    int32_t startPx = log[0].px;
    for (int i = 0; i < 10; ++i) {
        boxed.onMouseDelta(0.3, 0.0, 1.0, 1.0, 110.0 + i * 2.0, pkts);
    }
    assert(log.back().action == TOUCH_MOVE && log.back().px == startPx + 3);

    double t = 200.0;
    while (boxed.isFingerPlaced()) {
        boxed.onMouseDelta(120.0, 0.0, 1.0, 1.0, t, pkts);
        t += 4.0;
        assert(t < 400.0);
    }
    assert(log.back().action == TOUCH_UP && log.back().ptrId == PointerId::AIM_A);
    assert(boxed.activePointerId() == PointerId::AIM_B);
    double releaseT = t - 4.0;

    boxed.onMouseDelta(20.0, 0.0, 1.0, 1.0, releaseT + 8.0, pkts);
    assert(!boxed.isFingerPlaced());
    boxed.onMouseDelta(20.0, 0.0, 1.0, 1.0, releaseT + 16.0, pkts);
    assert(boxed.isFingerPlaced() && log.back().action == TOUCH_DOWN && log.back().ptrId == PointerId::AIM_B);

    boxed.tick(releaseT + 800.0, false, pkts);
    assert(!boxed.isFingerPlaced() && log.back().action == TOUCH_UP);

    // Free Fire Unbounded + FpsViewStack Overlay push/pop
    log.clear();
    FpsViewStack stack(PointerId::AIM_A, PointerId::AIM_B, PointerId::HOLD_VIEW);
    stack.setTouchSink(recordTouchCb, &log);
    FpsViewConfig ffCfg = bcfg;
    ffCfg.unbounded = true;
    ffCfg.rectLeft = -99999999.0; ffCfg.rectTop = -99999999.0;
    ffCfg.rectRight = 99999999.0; ffCfg.rectBottom = 99999999.0;
    ffCfg.touchDownRandomRangePx = 0;
    ffCfg.ffCounterAccel = 7.0;
    stack.configureMain(ffCfg);
    stack.onFocusGain(1000.0);

    stack.onMouseDelta(5.0, 0.0, 1.0, 1.0, 1004.0, pkts);
    assert(log.size() == 1 && log[0].action == TOUCH_DOWN && log[0].px == 1200 && log[0].py == 500);
    for (int i = 0; i < 6; ++i) {
        stack.onMouseDelta(0.0, -150.0, 1.0, 1.0, 1010.0 + i * 4.0, pkts);
    }
    assert(stack.mainView().isFingerPlaced() && log.back().py < 0 && !log.back().clamped);

    FpsViewConfig eyeCfg = bcfg;
    eyeCfg.centerX = 1600.0; eyeCfg.centerY = 300.0;
    eyeCfg.clampInsteadOfRelease = true;
    eyeCfg.unbounded = false;
    stack.pushOverlay(7, eyeCfg, 1100.0, pkts);
    assert(stack.isOverlayActive() && !stack.mainView().isFingerPlaced());
    assert(log.back().action == TOUCH_DOWN && log.back().ptrId == PointerId::HOLD_VIEW);
    stack.onMouseDelta(50.0, -20.0, 1.0, 1.0, 1104.0, pkts);
    assert(log.back().action == TOUCH_MOVE && log.back().px == 1650 && log.back().py == 280);
    stack.popOverlay(7, 1120.0, pkts);
    assert(!stack.isOverlayActive() && log.back().action == TOUCH_UP && log.back().ptrId == PointerId::HOLD_VIEW);

    std::printf("[Test] Standalone FpsViewFinger & FpsViewStack OK!\n");
}

static void testInputEngineIntegration() {
    ScrcpyBridge bridge;
    InputEngine engine(&bridge);

    Profile p = ProfileDefaults::createFreeFire();
    assert(p.pollHz == 250);
    assert(std::fabs(p.ffCounterAccel - 7.0f) < 0.01f);
    assert(std::fabs(p.smoothing - 0.10f) < 0.001f);

    // BAG = LSHIFT with joy_sprint = LSHIFT must lock AND unlock cleanly!
    p.vkSprint = VK_LSHIFT;
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        if (p.buttons[i].action == NODE_CURSOR_TOGGLE) {
            p.buttons[i].vk = VK_LSHIFT;
            break;
        }
    }
    KeyNode scrlBtn;
    scrlBtn.label = "WPNUP";
    scrlBtn.vk = VK_CUSTOM_MWHEEL_UP;
    scrlBtn.action = NODE_PRESS;
    scrlBtn.x = 0.85f; scrlBtn.y = 0.15f;
    p.buttons.push_back(scrlBtn);

    engine.setProfile(p);
    bool reqToggle = false;
    engine.setAimMode(false);
    assert(engine.onKeyEvent(VK_LSHIFT, true, reqToggle) && reqToggle);
    engine.onKeyEvent(VK_LSHIFT, false, reqToggle);
    engine.setAimMode(true);
    reqToggle = false;
    assert(engine.onKeyEvent(VK_LSHIFT, true, reqToggle) && reqToggle);
    engine.onKeyEvent(VK_LSHIFT, false, reqToggle);

    // Live worker thread: unlocked WASD/KB + concurrent mouse/KB/scroll
    engine.start();
    engine.setAimMode(false);
    assert(engine.onKeyEvent('W', true, reqToggle) && !reqToggle);
    assert(engine.onKeyEvent(VK_SPACE, true, reqToggle) && !reqToggle);
    assert(!engine.onKeyEvent(VK_LBUTTON, true, reqToggle));
    Sleep(15);
    assert(engine.getTelemetry().joyDown == true);

    engine.setAimMode(true);
    for (int i = 0; i < 20; ++i) {
        engine.onRawMouseDelta(40, -10);
        if (i == 5)  engine.onKeyEvent('D', true, reqToggle);
        if (i == 10) engine.onKeyEvent(VK_CUSTOM_MWHEEL_UP, true, reqToggle);
        if (i == 15) engine.onKeyEvent('D', false, reqToggle);
        Sleep(2);
    }
    EngineTelemetry tel = engine.getTelemetry();
    assert(tel.aimMode && tel.aimFingerDown && tel.joyDown);

    engine.setAimMode(false);
    Sleep(8);
    assert(engine.getTelemetry().joyDown == true);
    engine.onKeyEvent('W', false, reqToggle);
    engine.onKeyEvent(VK_SPACE, false, reqToggle);
    engine.stop();
    std::printf("[Test] InputEngine integration (LShift toggle, unlocked KB/WASD, concurrent KB+mouse+scroll) OK!\n");
}

int main() {
    testSpsParser();
    testProfileRoundTrip();
    testStudioPanelMouseAndScrollRebind();
    testJoystickAndActionModules();
    testFpsViewController();
    testInputEngineIntegration();
    std::printf("ALL MODULAR ARCHITECTURE TESTS PASSED!\n");
    return 0;
}
