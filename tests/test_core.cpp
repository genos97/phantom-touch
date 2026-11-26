#include "bridge/QrCodeGen.hpp"
#include "bridge/SpsParser.hpp"
#include "bridge/TouchFingerPool.hpp"
#include "bridge/ScrcpyBridge.hpp"
#include "config/AppSettings.hpp"
#include "config/KeyCodes.hpp"
#include "config/Profile.hpp"
#include "config/ProfileStore.hpp"
#include "engine/ActionDispatcher.hpp"
#include "engine/FpsViewController.hpp"
#include "engine/GamepadController.hpp"
#include "engine/InputEngine.hpp"
#include "engine/InputHookThread.hpp"
#include "engine/JoystickController.hpp"
#include "engine/ThreadScheduling.hpp"
#include "ui/DrawerPanel.hpp"
#include "ui/GdiPlusDraw.hpp"
#include "video/D3D11Renderer.hpp"
#include "video/H264Decoder.hpp"
#include "video/LatencyTelemetry.hpp"

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
    swipeNode.resetView = true;
    swipeNode.reactivateJoy = true;
    orig.buttons.push_back(swipeNode);
    orig.rememberCursorPos = true;
    orig.cursorScrollStepPx = 110;

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
    assert(last.resetView == true && last.reactivateJoy == true);
    assert(loaded.rememberCursorPos == true && loaded.cursorScrollStepPx == 110);

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

    // Blank / cleared (0-button) profile MUST stay 0 buttons after save + reload!
    const std::string tmpBlankPath = "profiles\\_test_blank_roundtrip.cfg";
    assert(ProfileStore::save(tmpBlankPath, blank));
    Profile loadedBlank;
    assert(ProfileStore::load(tmpBlankPath, loadedBlank));
    assert(loadedBlank.name == "Custom Test");
    assert(loadedBlank.buttons.empty());
    std::remove(tmpBlankPath.c_str());

    // .active_profile atomic persistence round-trip in isolated temp folder
    const std::string tmpDir = "profiles\\_test_active_dir";
    CreateDirectoryA(tmpDir.c_str(), NULL);
    const std::string tmpCustom = tmpDir + "\\my_custom.cfg";
    assert(ProfileStore::save(tmpCustom, orig));
    ProfileStore::saveLastActive(tmpDir, tmpCustom);
    assert(ProfileStore::loadLastActive(tmpDir) == tmpCustom);

    // Creating a new config when my_custom.cfg already exists must auto-suffix instead of overwriting!
    DrawerPanel createUi;
    createUi.setProfilesDir(tmpDir, tmpCustom);
    createUi.setActiveTab(2); // PROFILES tab
    HDC scDC = GetDC(NULL);
    HDC mDC = CreateCompatibleDC(scDC);
    HBITMAP bm = CreateCompatibleBitmap(scDC, 400, 700);
    HGDIOBJ obm = SelectObject(mDC, bm);
    int dummySel = -1;
    Profile editCopy = orig;
    AppSettings dummySet;
    std::vector<std::string> noLogs;
    RECT drawerRc = { 0, 0, 356, 700 };
    {
        Gdiplus::Graphics g(mDC);
        createUi.paint(g, drawerRc, editCopy, dummySet, tmpCustom, dummySel, false, false, "", "", noLogs, -1, -1);
    }
    DrawerActionResult mkRes;
    // "+ BLANK CONFIG" button sits at y = 307..337 inside drawerRc (after Per-App Link card + Mode toggle)
    assert(createUi.onMouseDown(80, 322, editCopy, dummySet, dummySel, mkRes));
    assert(mkRes.requestSwitchProfile && mkRes.switchProfilePath == (tmpDir + "\\my_custom_2.cfg"));
    // Verify original my_custom.cfg was NOT overwritten and my_custom_2.cfg has 0 buttons!
    Profile verifyOrig, verifyBlank2;
    assert(ProfileStore::load(tmpCustom, verifyOrig) && verifyOrig.buttons.size() == orig.buttons.size());
    assert(ProfileStore::load(mkRes.switchProfilePath, verifyBlank2) && verifyBlank2.buttons.empty());
    SelectObject(mDC, obm); DeleteObject(bm); DeleteDC(mDC); ReleaseDC(NULL, scDC);

    std::remove((tmpDir + "\\my_custom_2.cfg").c_str());
    std::remove(tmpCustom.c_str());
    std::remove((tmpDir + "\\.active_profile").c_str());
    RemoveDirectoryA(tmpDir.c_str());

    // AppSettings round-trip & scrcpy args builder
    AppSettings as;
    as.winX = 120; as.winY = 80; as.winW = 1440; as.winH = 820;
    as.maximized = true; as.alwaysOnTop = true;
    as.mirrorFps = 120; as.mirrorBitrateMbps = 24; as.mirrorMaxSize = 1600;
    as.mirrorAudio = true; as.turnScreenOff = true; as.stayAwake = true;
    as.lastWirelessAddr = "192.168.1.77:5555";
    const std::string tmpSetPath = "profiles\\_test_shell_settings.cfg";
    assert(AppSettingsStore::save(tmpSetPath, as));
    AppSettings asLoaded;
    assert(AppSettingsStore::load(tmpSetPath, asLoaded));
    assert(asLoaded.winW == 1440 && asLoaded.maximized && asLoaded.alwaysOnTop);
    assert(asLoaded.mirrorFps == 120 && asLoaded.mirrorBitrateMbps == 24 && asLoaded.mirrorMaxSize == 1600);
    assert(asLoaded.mirrorAudio && asLoaded.turnScreenOff && asLoaded.lastWirelessAddr == "192.168.1.77:5555");
    std::string builtArgs = asLoaded.buildScrcpyArgs();
    assert(builtArgs.find("--max-fps=120") != std::string::npos);
    assert(builtArgs.find("--video-bit-rate=24M") != std::string::npos);
    assert(builtArgs.find("--max-size=1600") != std::string::npos);
    assert(builtArgs.find("--stay-awake") == std::string::npos);
    assert(builtArgs.find("--turn-screen-off") == std::string::npos);
    assert(builtArgs.find("--no-audio") == std::string::npos);
    std::remove(tmpSetPath.c_str());

    std::vector<std::string> files = ProfileStore::listProfiles("profiles");
    assert(!files.empty());
    for (size_t i = 0; i < files.size(); ++i) {
        assert(files[i].find("\\.") == std::string::npos);
        assert(files[i].find("\\_") == std::string::npos);
    }

    std::remove(tmpPath.c_str());
    std::printf("[Test] ProfileStore + AppSettings atomic save, blank reload & safe creation OK (%u nodes, %u profiles)\n",
                static_cast<unsigned>(loaded.buttons.size()), static_cast<unsigned>(files.size()));
}

static void testEmulatorShellHelpersAndDrawer() {
    assert(KeyCodes::toName(0x10) == "LSHIFT");
    assert(KeyCodes::fromName("0x10") == 0xA0);
    assert(KeyCodes::fromName("MMB") == 0x04 && KeyCodes::toName(0x04) == "MMB");
    assert(KeyCodes::fromName("SCROLLUP") == VK_CUSTOM_MWHEEL_UP);
    assert(KeyCodes::fromName("SCROLLDOWN") == VK_CUSTOM_MWHEEL_DOWN);

    // 1. Aspect-ratio letterboxing math (landscape, portrait, ultrawide)
    ViewportRect land = GdiPlusDraw::computeAspectFit(1000, 700, 2340, 1080, 0);
    assert(land.w == 1000 && land.h > 455 && land.h < 470 && land.x == 0 && land.y > 100);

    ViewportRect port = GdiPlusDraw::computeAspectFit(1000, 700, 1080, 2340, 0);
    assert(port.h == 700 && port.w > 315 && port.w < 330 && port.y == 0 && port.x > 300);

    // 2. ADB Device Hub parser (USB + Wireless + unauthorized)
    const std::string adbOut =
        "List of devices attached\n"
        "RZCW401ABCD            device product:r9qxxx model:SM_G990E device:r9q transport_id:1\n"
        "192.168.1.42:5555      device product:pixel model:Pixel_8 transport_id:2\n"
        "ABC123456              unauthorized transport_id:3\n";
    std::vector<AdbDeviceInfo> devs = ScrcpyBridge::parseAdbDevicesOutput(adbOut);
    assert(devs.size() == 3);
    assert(devs[0].serial == "RZCW401ABCD" && devs[0].model == "SM G990E" && devs[0].state == "device" && !devs[0].wireless);
    assert(devs[1].serial == "192.168.1.42:5555" && devs[1].wireless && devs[1].model == "Pixel 8");
    assert(devs[2].state == "unauthorized");

    // 3. Docked Outside-Viewport DrawerPanel: Key Inspector (MMB, SCROLL UP, SCROLL DOWN) & SENS tab (LShift + MMB)
    Profile loaded = ProfileDefaults::createFreeFire();
    AppSettings settings;
    std::vector<std::string> logs;

    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, 356, 900);
    HGDIOBJ oldBmp = SelectObject(memDC, bmp);

    DrawerPanel drawer;
    drawer.setProfilesDir("profiles", "profiles\\freefire.cfg");
    drawer.setActiveTab(0); // KEYS tab (Inspector at top when sel=0)
    int sel = 0;
    RECT rc = { 0, 0, 356, 900 };
    {
        Gdiplus::Graphics g(memDC);
        drawer.paint(g, rc, loaded, settings, "profiles\\freefire.cfg", sel, false, true, "SM-G990E", "com.dts.freefiremax", logs, -1, -1);
    }

    // Inspector layout inside drawer (body starts at y=87, startY=97):
    // Card top=97, header+22=129, label+32=161, rebind+34=195, mouse row y=195..217, scroll row y=222..246
    DrawerActionResult rMmb;
    assert(drawer.onMouseDown(12 + 10 + 2 * 62 + 25, 206, loaded, settings, sel, rMmb));
    assert(loaded.buttons[0].vk == VK_MBUTTON && rMmb.profileModified);

    DrawerActionResult rScUp;
    assert(drawer.onMouseDown(80, 234, loaded, settings, sel, rScUp));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_UP);

    DrawerActionResult rScDn;
    assert(drawer.onMouseDown(260, 234, loaded, settings, sel, rScDn));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_DOWN);

    // Switch to SENS tab (tab 1) and verify LShift normalization on Lock/Unlock Key rebind
    drawer.setActiveTab(1);
    {
        Gdiplus::Graphics g(memDC);
        drawer.paint(g, rc, loaded, settings, "profiles\\freefire.cfg", sel, false, true, "SM-G990E", "com.dts.freefiremax", logs, -1, -1);
    }
    // Trigger Lock/Unlock rebind -> reject LMB -> bind VK_SHIFT -> normalized to VK_LSHIFT
    // Find by scrolling or direct hit (height 900 fits entire SENS tab!)
    DrawerActionResult rReb;
    assert(drawer.onMouseDown(120, 748, loaded, settings, sel, rReb));
    assert(drawer.isRebindingToggleAim());

    DrawerActionResult rLmb;
    assert(drawer.onMouseBind(VK_LBUTTON, loaded, rLmb));
    assert(drawer.isRebindingToggleAim());

    DrawerActionResult rShift;
    assert(drawer.onKeyDown(VK_SHIFT, loaded, settings, sel, rShift));
    assert(!drawer.isRebindingToggleAim() && loaded.toggleAimVk == VK_LSHIFT);

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    std::printf("[Test] Emulator Shell (AspectFit, ADB Device Hub parser, DrawerPanel Keys/Scroll/Sens) OK!\n");
}

static void testTouchFingerPool() {
    std::vector<RecordedTouch> log;
    TouchFingerPool pool;
    pool.setDownstreamSink(recordTouchCb, &log);

    // 1. Allocate 10 distinct logical IDs (including high button IDs >= 20) -> strict 0..9 slots
    for (int i = 0; i < 10; ++i) {
        bool ok = pool.emit(TOUCH_DOWN, PointerId::BTN_BASE + i, 100 + i * 10, 200, true);
        assert(ok);
        assert(log.back().ptrId < 10);
    }
    assert(pool.activeCount() == 10);

    // 2. 11th simultaneous finger is cleanly dropped instead of overflowing Android's 10-slot limit
    assert(!pool.emit(TOUCH_DOWN, PointerId::BTN_BASE + 99, 500, 500, true));
    assert(pool.activeCount() == 10);

    // 3. Redundant MOVE to exact same coordinate is suppressed; new coordinate emits MOVE
    size_t beforeMove = log.size();
    assert(!pool.emit(TOUCH_MOVE, PointerId::BTN_BASE + 0, 100, 200, true));
    assert(log.size() == beforeMove);
    assert(pool.emit(TOUCH_MOVE, PointerId::BTN_BASE + 0, 120, 210, true));
    assert(log.size() == beforeMove + 1 && log.back().action == TOUCH_MOVE);

    // 4. Orphan UP / MOVE on inactive channel ignored
    assert(!pool.emit(TOUCH_UP, 9999, 0, 0, true));
    assert(!pool.emit(TOUCH_MOVE, 9999, 10, 10, true));

    // 5. releaseAll() drains every active finger back to 0 cleanly
    int lifted = pool.releaseAll();
    assert(lifted == 10 && pool.activeCount() == 0);

    // 6. FIFO round-robin rotation: releasing & immediately re-planting rotates to a fresh slot
    assert(pool.emit(TOUCH_DOWN, PointerId::AIM_A, 1200, 600, true));
    uint64_t firstSlot = log.back().ptrId;
    assert(pool.emit(TOUCH_UP, PointerId::AIM_A, 1250, 600, true));
    assert(pool.emit(TOUCH_DOWN, PointerId::AIM_A, 1200, 600, true));
    uint64_t secondSlot = log.back().ptrId;
    assert(firstSlot != secondSlot && secondSlot < 10);
    assert(pool.releaseAll() == 1 && pool.activeCount() == 0);

    std::printf("[Test] TouchFingerPool (0..9 bounds, FIFO rotation, overflow/orphan/drain guards) OK!\n");
}

static void testJoystickAndActionModules() {
    std::vector<RecordedTouch> log;
    uint32_t pkts = 0;
    Profile p = ProfileDefaults::createFreeFire();

    // 1. Standalone JoystickController Free Fire 4-stage stepped glide
    JoystickController joy;
    joy.setTouchSink(recordTouchCb, &log);
    assert(joy.handleKey('W', true, true, p, 2000, 1000, 100.0, pkts));
    assert(joy.isDown() && log.size() == 1 && log[0].action == TOUCH_DOWN);
    joy.tick(p, 2000, 1000, 109.0, pkts); // stage 0
    joy.tick(p, 2000, 1000, 125.0, pkts); // stage 1 (20%)
    joy.tick(p, 2000, 1000, 141.0, pkts); // stage 2 (50%)
    joy.tick(p, 2000, 1000, 157.0, pkts); // stage 3 (100%)
    assert(log.back().action == TOUCH_MOVE && log.back().py < log[0].py);

    // Non-blocking >=15ms reactivate() gap (no 0ms UP+DOWN fusion!)
    joy.reactivate(p, 2000, 1000, 160.0, pkts);
    assert(!joy.isDown() && log.back().action == TOUCH_UP);
    joy.tick(p, 2000, 1000, 168.0, pkts); // +8ms < 15ms: still lifted
    assert(!joy.isDown());
    joy.tick(p, 2000, 1000, 176.0, pkts); // +16ms >= 15ms: cleanly re-planted
    assert(joy.isDown() && log.back().action == TOUCH_DOWN);
    assert(joy.handleKey('W', false, true, p, 2000, 1000, 190.0, pkts));
    assert(!joy.isDown() && log.back().action == TOUCH_UP);

    // Non-FF fast 3-step 24ms glide (no 0ms teleport!)
    log.clear();
    p.joySteppedGlide = false;
    assert(joy.handleKey('D', true, true, p, 2000, 1000, 200.0, pkts));
    assert(log.size() == 1 && log[0].action == TOUCH_DOWN);
    joy.tick(p, 2000, 1000, 209.0, pkts);
    joy.tick(p, 2000, 1000, 218.0, pkts);
    joy.tick(p, 2000, 1000, 227.0, pkts);
    assert(log.size() == 4 && log.back().action == TOUCH_MOVE && log.back().px > log[0].px);
    joy.release(pkts);
    p.joySteppedGlide = true;

    // 2. Standalone ActionDispatcher: modifier-normalized 2-key combo, toggleOnly isolation, add-on flags, multi-notch wheel
    log.clear();
    ActionDispatcher disp;
    disp.setTouchSink(recordTouchCb, &log);
    KeyNode whl;
    whl.label = "WPN";
    whl.vk = VK_CUSTOM_MWHEEL_UP;
    whl.action = NODE_PRESS;
    whl.x = 0.8f; whl.y = 0.2f;
    p.buttons.push_back(whl);

    KeyNode comboNode;
    comboNode.label = "CMB";
    comboNode.comboVk = VK_SHIFT; // Generic VK_SHIFT must match held VK_LSHIFT!
    comboNode.vk = '1';
    comboNode.action = NODE_CLICK;
    comboNode.resetView = true;
    comboNode.reactivateJoy = true;
    comboNode.x = 0.5f; comboNode.y = 0.5f;
    p.buttons.push_back(comboNode);

    disp.resetForProfile(p.buttons.size());

    bool held[512] = {false};
    bool scopeSens = false;
    ActionEffects eff;

    // Cursor Mode mouse isolation: toggleOnly=true must NOT trigger RMB SCOPE (NODE_PRESS)
    disp.dispatchKey(VK_RBUTTON, true, false, p, held, 2000, 1000, 195.0, scopeSens, eff, pkts, true);
    assert(log.empty());

    // Modifier-normalized 2-key combo: hold VK_LSHIFT, press '1' -> fires combo + resetView + reactivateJoy
    held[VK_LSHIFT] = true;
    disp.dispatchKey('1', true, true, p, held, 2000, 1000, 198.0, scopeSens, eff, pkts);
    assert(log.size() == 1 && log[0].action == TOUCH_DOWN);
    assert(eff.resetMainView && eff.reactivateJoystick);
    held[VK_LSHIFT] = false;
    disp.tick(p, 240.0, pkts);
    log.clear();

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

    // Live worker thread: unlocked WASD/KB, cursor scroll-drag, bounded 0..9 pool & concurrent mouse/KB/scroll
    std::vector<RecordedTouch> engineLog;
    engine.setTouchSinkForTest(recordTouchCb, &engineLog);
    engine.start();
    engine.setAimMode(false);
    assert(engine.onKeyEvent('W', true, reqToggle) && !reqToggle);
    assert(engine.onKeyEvent(VK_SPACE, true, reqToggle) && !reqToggle);
    assert(!engine.onKeyEvent(VK_LBUTTON, true, reqToggle));
    // Unlocked cursor scroll-wheel drag (downward scroll drags finger upward & auto-releases after 80ms)
    engine.onDirectCursorScroll(0.50f, 0.50f, -1);
    Sleep(15);
    assert(engine.getTelemetry().joyDown == true);
    Sleep(95); // allow 80ms cursor scroll auto-release to fire

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

    // Every emitted touch packet across joystick, scroll-drag, buttons, and FPS aim MUST stay strictly in 0..9!
    assert(!engineLog.empty());
    bool sawScrollDragMove = false;
    for (size_t i = 0; i < engineLog.size(); ++i) {
        assert(engineLog[i].ptrId < 10);
        if (engineLog[i].action == TOUCH_MOVE && engineLog[i].px == 960 && engineLog[i].py < 540) {
            sawScrollDragMove = true;
        }
    }
    assert(sawScrollDragMove);
    std::printf("[Test] InputEngine integration (0..9 pool, cursor scroll-drag, LShift toggle, unlocked KB/WASD) OK!\n");
}

static void testStage1EmulatorFeatures() {
    // 1. Foreground Android package parser (Android 10-16 dumpsys window & activity)
    const std::string dump1 =
        "  mCurrentFocus=Window{a1b2c3 u0 com.dts.freefiremax/com.dts.freefireth.FFMainActivity}\n";
    assert(ScrcpyBridge::parseForegroundPackage(dump1) == "com.dts.freefiremax");

    const std::string dumpOverlay =
        "  mCurrentFocus=Window{111 u0 NotificationShade}\n"
        "  mFocusedApp=ActivityRecord{222 u0 com.whatsapp/.HomeActivity t42}\n";
    assert(ScrcpyBridge::parseForegroundPackage(dumpOverlay) == "com.whatsapp");

    const std::string dumpResumed =
        "  mCurrentFocus=Window{333 u0 com.android.systemui/com.android.systemui.shade}\n"
        "  topResumedActivity=ActivityRecord{444 u0 com.android.chrome/org.chromium.chrome.browser.ChromeTabbedActivity t9}\n";
    assert(ScrcpyBridge::parseForegroundPackage(dumpResumed) == "com.android.chrome");

    const std::string dumpIme =
        "  mCurrentFocus=Window{555 u0 InputMethod}\n"
        "  mResumedActivity: ActivityRecord{666 u0 com.spotify.music/.MainActivity t12}\n";
    assert(ScrcpyBridge::parseForegroundPackage(dumpIme) == "com.spotify.music");

    // 2. Per-app matching + Desktop profile defaults + ProfileStore round-trip
    assert(ProfileDefaults::matchesPackage("com.dts.freefiremax,com.dts.freefireth", "com.dts.freefiremax"));
    assert(ProfileDefaults::matchesPackage("com.dts.freefiremax, com.dts.freefireth", "com.dts.freefireth"));
    assert(!ProfileDefaults::matchesPackage("com.dts.freefiremax", "com.whatsapp"));
    assert(!ProfileDefaults::matchesPackage("", "com.whatsapp"));

    Profile desk = ProfileDefaults::createDesktop("Desktop Typing");
    assert(!desk.keymapEnabled && desk.buttons.empty() && desk.appPackage.empty());
    const std::string tmpDesk = "profiles\\_test_desktop_stage1.cfg";
    assert(ProfileStore::save(tmpDesk, desk));
    Profile deskLoaded;
    assert(ProfileStore::load(tmpDesk, deskLoaded));
    assert(!deskLoaded.keymapEnabled && deskLoaded.buttons.empty());
    std::remove(tmpDesk.c_str());

    Profile game = ProfileDefaults::createFreeFire();
    assert(game.keymapEnabled && ProfileDefaults::matchesPackage(game.appPackage, "com.dts.freefiremax"));
    assert(ProfileDefaults::matchesPackage(game.appPackage, "com.dts.freefireind"));
    game.appPackage = "com.supercell.clashofclans,com.supercell.brawlstars";
    const std::string tmpGame = "profiles\\_test_app_link_stage1.cfg";
    assert(ProfileStore::save(tmpGame, game));
    Profile gameLoaded;
    assert(ProfileStore::load(tmpGame, gameLoaded));
    assert(gameLoaded.keymapEnabled && ProfileDefaults::matchesPackage(gameLoaded.appPackage, "com.supercell.brawlstars"));
    std::remove(tmpGame.c_str());

    // 3. AppSettings Stage 1 flags & lastGameProfile round-trip
    AppSettings s;
    s.autoSwitchProfiles = false;
    s.rightClickBack = false;
    s.lastGameProfile = "profiles\\my_custom.cfg";
    const std::string tmpSet = "profiles\\_test_stage1_settings.cfg";
    assert(AppSettingsStore::save(tmpSet, s));
    AppSettings sLoad;
    assert(AppSettingsStore::load(tmpSet, sLoad));
    assert(!sLoad.autoSwitchProfiles && !sLoad.rightClickBack);
    assert(sLoad.lastGameProfile == "profiles\\my_custom.cfg");
    std::remove(tmpSet.c_str());

    // 4. KeyCodes Win32 VK -> Android keycode mapping
    assert(KeyCodes::vkToAndroidKeycode(VK_RETURN) == 66);
    assert(KeyCodes::vkToAndroidKeycode(VK_BACK) == 67);
    assert(KeyCodes::vkToAndroidKeycode(VK_ESCAPE) == 111);
    assert(KeyCodes::vkToAndroidKeycode('A') == 29);
    assert(KeyCodes::vkToAndroidKeycode('V') == 50);
    assert(KeyCodes::vkToAndroidKeycode('Z') == 54);
    assert(KeyCodes::vkToAndroidKeycode(VK_UP) == 19);
    assert(KeyCodes::vkToAndroidKeycode(VK_LEFT) == 21);

    // 5. InputEngine Desktop Typing Mode passthrough + Ctrl+Wheel 2-finger pinch-zoom
    ScrcpyBridge bridge;
    InputEngine engine(&bridge);
    std::vector<RecordedTouch> log;
    engine.setTouchSinkForTest(recordTouchCb, &log);
    engine.setProfile(desk);
    engine.start();

    bool reqToggle = false;
    assert(!engine.onKeyEvent('W', true, reqToggle));
    assert(!engine.onKeyEvent('Q', true, reqToggle));
    assert(!engine.onKeyEvent(VK_SPACE, true, reqToggle));
    assert(!engine.isKeyMapped('W', false));

    // Direct off-center cursor tap must emit TOUCH_DOWN and TOUCH_UP at the exact same coordinates
    engine.onDirectCursorTouch(TOUCH_DOWN, 0.25f, 0.75f);
    engine.onDirectCursorTouch(TOUCH_UP, 0.25f, 0.75f);
    Sleep(20);
    assert(log.size() >= 2);
    assert(log[0].action == TOUCH_DOWN && log[1].action == TOUCH_UP);
    assert(log[0].px == log[1].px && log[0].py == log[1].py);
    assert(log[0].px == 480 && log[0].py == 810); // 1920x1080 * (0.25, 0.75)
    log.clear();

    // Ctrl+Wheel pinch zoom: must plant 2 distinct fingers strictly in 0..9, expand them, and auto-release after 140ms
    engine.onDirectCursorPinch(0.50f, 0.50f, 1);
    Sleep(20);
    engine.onDirectCursorPinch(0.50f, 0.50f, 1);
    Sleep(180); // > 140ms auto-release
    engine.stop();

    int downCount = 0, moveCount = 0, upCount = 0;
    for (size_t i = 0; i < log.size(); ++i) {
        assert(log[i].ptrId < 10);
        if (log[i].action == TOUCH_DOWN) ++downCount;
        else if (log[i].action == TOUCH_MOVE) ++moveCount;
        else if (log[i].action == TOUCH_UP) ++upCount;
    }
    assert(downCount == 2 && moveCount >= 2 && upCount == 2);
    std::printf("[Test] Stage 1 (Auto-Switch parser, Desktop Typing, Android KeyCodes, Ctrl+Wheel Pinch-Zoom) OK!\n");
}

static void testStage2VideoPipeline() {
    // 1. Windows Media Foundation built-in H.264 Decoder MFT initialization (NV12)
    H264Decoder dec;
    std::string decErr;
    bool decOk = dec.init(1280, 720, decErr);
    assert(decOk && dec.isInitialized());
    dec.shutdown();

    // 2. NV12 -> BGRA BT.709 color conversion test (white Y=235, UV=128 -> near 255,255,255)
    DecodedNV12Frame frame;
    frame.width = 64;
    frame.height = 64;
    frame.stride = 64;
    frame.surfaceH = 64;
    frame.data.resize(64 * 64 * 3 / 2);
    std::memset(frame.data.data(), 235, 64 * 64);          // Y = 235 (white)
    std::memset(frame.data.data() + 64 * 64, 128, 64 * 32); // U=128, V=128 (neutral)

    std::vector<uint8_t> bgra;
    assert(D3D11Renderer::convertNV12ToBGRA(frame, bgra));
    assert(bgra.size() == 64 * 64 * 4);
    assert(bgra[0] >= 250 && bgra[1] >= 250 && bgra[2] >= 250 && bgra[3] == 255);

    // 3. Real Direct3D 11 Renderer + HLSL shader compilation + same-frame HUD overlay + instant PNG screenshot
    HWND tmpHwnd = CreateWindowExA(0, "STATIC", "D3D11Test", WS_POPUP, 0, 0, 320, 180, NULL, NULL, GetModuleHandleA(NULL), NULL);
    assert(tmpHwnd != NULL);
    D3D11Renderer d3d;
    std::string d3dErr;
    assert(d3d.init(tmpHwnd, 320, 180, d3dErr));
    assert(d3d.isInitialized());

    d3d.submitNV12Frame(frame);
    assert(d3d.hasVideoFrame());

    std::vector<uint8_t> hudPix(64 * 64 * 4, 128);
    d3d.updateHudBitmap(hudPix.data(), 64, 64);
    ViewportRect vr;
    vr.x = 0; vr.y = 0; vr.w = 320; vr.h = 180;
    d3d.render(vr, true);

    const std::string pngPath = "profiles\\_test_d3d11_snap.png";
    std::string snapErr;
    assert(d3d.saveLatestFramePng(pngPath, snapErr));
    FILE* f = std::fopen(pngPath.c_str(), "rb");
    assert(f != NULL);
    unsigned char sig[8] = {0};
    assert(std::fread(sig, 1, 8, f) == 8);
    std::fclose(f);
    assert(sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
    std::remove(pngPath.c_str());

    d3d.shutdown();
    DestroyWindow(tmpHwnd);
    std::printf("[Test] Stage 2 (Media Foundation H.264 MFT, Direct3D 11 NV12+HUD shader, instant PNG capture) OK!\n");
}

static void testStage3EmulatorFeatures() {
    // 1. Edge-Anchored Shareable Profiles (20:9 <-> 16:9 survival + exact inverse round-trip!)
    float mx = 0.0f, my = 0.0f;
    // Left-edge joystick at 0.14 on 20:9 (2.222) -> 16:9 (1.778): ratio=1.25 -> 0.175
    ProfileDefaults::mapEdgeAnchored(0.14f, 0.75f, true, 20.0f / 9.0f, 16.0f / 9.0f, mx, my);
    assert(std::fabs(mx - 0.175f) < 0.002f && std::fabs(my - 0.75f) < 0.001f);
    float ux = 0.0f, uy = 0.0f;
    ProfileDefaults::unmapEdgeAnchored(mx, my, true, 20.0f / 9.0f, 16.0f / 9.0f, ux, uy);
    assert(std::fabs(ux - 0.14f) < 0.002f);

    // Right-edge Jump button at 0.94 on 20:9 -> 16:9: 1 - 0.06*1.25 = 0.925
    ProfileDefaults::mapEdgeAnchored(0.94f, 0.65f, true, 20.0f / 9.0f, 16.0f / 9.0f, mx, my);
    assert(std::fabs(mx - 0.925f) < 0.002f);
    ProfileDefaults::unmapEdgeAnchored(mx, my, true, 20.0f / 9.0f, 16.0f / 9.0f, ux, uy);
    assert(std::fabs(ux - 0.94f) < 0.002f);

    // Center stays dead-center (0.50 -> 0.50)
    ProfileDefaults::mapEdgeAnchored(0.50f, 0.50f, true, 20.0f / 9.0f, 16.0f / 9.0f, mx, my);
    assert(std::fabs(mx - 0.50f) < 0.002f);

    Profile edgeProf = ProfileDefaults::createFreeFire();
    edgeProf.edgeAnchored = true;
    edgeProf.refAspect = 20.0f / 9.0f;
    const std::string tmpEdge = "profiles\\_test_edge_anchor.cfg";
    assert(ProfileStore::save(tmpEdge, edgeProf));
    Profile edgeLoaded;
    assert(ProfileStore::load(tmpEdge, edgeLoaded));
    assert(edgeLoaded.edgeAnchored && std::fabs(edgeLoaded.refAspect - (20.0f / 9.0f)) < 0.01f);
    std::remove(tmpEdge.c_str());

    Profile resolved169 = ProfileDefaults::resolveLayout(edgeLoaded, 1920, 1080);
    assert(resolved169.joyX > edgeLoaded.joyX + 0.02f); // Left joystick shifts inward on narrower 16:9!
    assert(resolved169.fireX < edgeLoaded.fireX - 0.01f); // Right fire button shifts inward on narrower 16:9!

    // 2. AppSettings Stage 2 & 3 Virtual Display + Native Video + Gamepad round-trip
    AppSettings as;
    as.useNativeVideo = true;
    as.virtualDisplayMode = true;
    as.virtualDisplaySpec = "1280x720/240";
    as.gamepadEnabled = false;
    const std::string tmpS3 = "profiles\\_test_stage3_settings.cfg";
    assert(AppSettingsStore::save(tmpS3, as));
    AppSettings asL;
    assert(AppSettingsStore::load(tmpS3, asL));
    assert(asL.useNativeVideo && asL.virtualDisplayMode && asL.virtualDisplaySpec == "1280x720/240" && !asL.gamepadEnabled);
    std::remove(tmpS3.c_str());

    // 3. XInput Gamepad-to-Touch Controller (deadzone, 360° analog joystick, right-stick look, triggers & buttons)
    float sx = 1.0f, sy = 1.0f;
    GamepadController::normalizeStick(2000, -1500, 0.18f, sx, sy); // inside deadzone -> 0
    assert(sx == 0.0f && sy == 0.0f);
    GamepadController::normalizeStick(32767, 0, 0.18f, sx, sy);
    assert(std::fabs(sx - 1.0f) < 0.01f && std::fabs(sy) < 0.01f);

    std::vector<RecordedTouch> gpLog;
    uint32_t gpPkts = 0;
    GamepadController gp;
    gp.setTouchSink(recordTouchCb, &gpLog);
    Profile gpProf = ProfileDefaults::createFreeFire();

    GamepadSnapshot snap;
    snap.connected = true;
    snap.lx = 0.0f; snap.ly = 0.95f; // Push left stick full UP -> sprint joystick up
    snap.rx = 0.80f; snap.ry = 0.20f; // Right stick up-right -> lookDx > 0, lookDy < 0
    snap.rt = 0.85f;                  // RT -> Fire (LMB)
    snap.buttons = 0x1000;            // A button -> VK_SPACE (Jump)
    GamepadFrameOutput gpOut;
    gp.processSnapshot(snap, gpProf, 1920, 1080, 10.0, gpPkts, gpOut);
    assert(gpOut.connected && gpOut.analogJoyActive && gp.isJoyDown());
    assert(gpLog.size() == 2 && gpLog[0].action == TOUCH_DOWN && gpLog[1].action == TOUCH_MOVE && gpLog[1].py < gpLog[0].py);
    assert(gpOut.lookDx > 1.0 && gpOut.lookDy < -0.2);
    assert(gpOut.keyDeltaCount == 2); // Fire + Jump pressed

    // Release sticks & buttons -> joystick lifts cleanly & buttons emit UP
    GamepadSnapshot idle;
    idle.connected = true;
    gp.processSnapshot(idle, gpProf, 1920, 1080, 10.0, gpPkts, gpOut);
    assert(!gp.isJoyDown() && gpLog.back().action == TOUCH_UP);
    assert(gpOut.keyDeltaCount == 2 && !gpOut.keyDeltas[0].down && !gpOut.keyDeltas[1].down);

    // 4. ISO/IEC 18004 QR Code Generator (Android 11+ Wireless ADB Pairing) + mDNS Parser
    std::string qrPayload = QrCodeGen::buildAndroidAdbPairPayload("studio-PT4821", "938271");
    assert(qrPayload == "WIFI:T:ADB;S:studio-PT4821;P:938271;;");
    QrMatrix qm;
    assert(QrCodeGen::encode(qrPayload, qm));
    assert(qm.size == 29 || qm.size == 33);
    // Verify top-left, top-right, bottom-left 7x7 finder outer ring & 3x3 dark core
    assert(qm.at(0, 0) && qm.at(6, 0) && qm.at(0, 6) && qm.at(3, 3) && !qm.at(1, 1));
    assert(qm.at(qm.size - 1, 0) && qm.at(qm.size - 7, 0) && qm.at(qm.size - 4, 3));
    assert(qm.at(0, qm.size - 1) && qm.at(0, qm.size - 7) && qm.at(3, qm.size - 4));
    // Verify timing patterns & mandatory dark module (8, size-8)
    assert(qm.at(8, 6) && !qm.at(9, 6) && qm.at(10, 6));
    assert(qm.at(8, qm.size - 8));

    const std::string mdnsRaw =
        "List of discovered mdns services\n"
        "studio-PT4821\t_adb-tls-pairing._tcp\t192.168.1.42:37891\n"
        "adb-RZCW401ABCD-xyz\t_adb-tls-connect._tcp\t192.168.1.42:41235\n";
    std::vector<MdnsServiceInfo> svcs = ScrcpyBridge::parseMdnsServicesOutput(mdnsRaw);
    assert(svcs.size() == 2);
    assert(svcs[0].name == "studio-PT4821" && svcs[0].type == "_adb-tls-pairing._tcp" && svcs[0].addr == "192.168.1.42:37891");
    assert(svcs[1].type == "_adb-tls-connect._tcp" && svcs[1].addr == "192.168.1.42:41235");

    std::printf("[Test] Stage 3 (Edge-Anchored 20:9<->16:9, Virtual Display cfg, XInput Gamepad, ISO QR + mDNS) OK!\n");
}

static void testSprint0Fixes() {
    // 1. V4: Session header parser (server rotation vs client resize vs video packet)
    const uint8_t hdrServerRot[12] = { 0x80, 0, 0, 0, 0, 0, 0x04, 0x38, 0, 0, 0x09, 0x24 }; // 1080x2340
    uint32_t outW = 0, outH = 0;
    bool clientResized = true;
    assert(ScrcpyBridge::parseV4SessionHeader(hdrServerRot, outW, outH, clientResized));
    assert(outW == 1080 && outH == 2340 && !clientResized);

    const uint8_t hdrClientResize[12] = { 0x80, 0, 0, 0x01, 0, 0, 0x07, 0x80, 0, 0, 0x04, 0x38 }; // 1920x1080
    assert(ScrcpyBridge::parseV4SessionHeader(hdrClientResize, outW, outH, clientResized));
    assert(outW == 1920 && outH == 1080 && clientResized);

    const uint8_t hdrVideoPkt[12] = { 0x40, 0, 0, 0x01, 0, 0, 0, 0x64, 0, 0, 0x01, 0x00 };
    assert(!ScrcpyBridge::parseV4SessionHeader(hdrVideoPkt, outW, outH, clientResized));

    // 2. V5: serializeResizeDisplay byte-for-byte (type 21, even-rounded width/height)
    uint8_t rbuf[5] = {0};
    assert(ScrcpyBridge::serializeResizeDisplay(rbuf, 1921, 1081) == 5);
    assert(rbuf[0] == 21 && rbuf[1] == 0x07 && rbuf[2] == 0x80 && rbuf[3] == 0x04 && rbuf[4] == 0x38);

    // 3. V10: Default mirrorFps is 0 (Uncapped) and buildScrcpyArgs omits --max-fps
    AppSettings defSet;
    assert(defSet.mirrorFps == 0);
    assert(defSet.buildScrcpyArgs().find("--max-fps") == std::string::npos);

    // 4. V12 / V7: Version major parser handles >=10 cleanly
    assert(ScrcpyBridge::parseMajorVersion("5.0.1") == 5);
    assert(ScrcpyBridge::parseMajorVersion("10.2.0") == 10);

    // 5. B3: Pure ASCII check & UTF-8 code-point safe chunking
    assert(ScrcpyBridge::isPureAscii("Hello, World 123!"));
    assert(!ScrcpyBridge::isPureAscii("Zażółć gęślą jaźń 写真"));
    const std::string utf8Multi = "abc\xE2\x82\xACxyz\xF0\x9F\x98\x80"; // "abc€xyz😀"
    std::vector<std::string> chunks = ScrcpyBridge::splitUtf8Chunks(utf8Multi, 5);
    std::string rejoined;
    for (size_t i = 0; i < chunks.size(); ++i) {
        assert(!chunks[i].empty());
        assert((static_cast<unsigned char>(chunks[i][0]) & 0xC0u) != 0x80u); // Never starts on continuation byte!
        rejoined += chunks[i];
    }
    assert(rejoined == utf8Multi);

    // 6. B6: BCryptGenRandom pairing code (>=10 alphanumeric chars, distinct across calls)
    std::string c1 = QrCodeGen::generateSecurePairingCode(12);
    std::string c2 = QrCodeGen::generateSecurePairingCode(12);
    assert(c1.size() == 12 && c2.size() == 12 && c1 != c2);
    for (size_t i = 0; i < c1.size(); ++i) {
        char ch = c1[i];
        bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        assert(alnum);
    }

    // 7. E1: AltGr (Ctrl+Alt) is NOT treated as Ctrl shortcut
    assert(KeyCodes::isPureCtrlShortcut(true, false));
    assert(!KeyCodes::isPureCtrlShortcut(true, true));
    assert(!KeyCodes::isPureCtrlShortcut(false, false));

    // 8. E2, E3, E4: Gamepad trigger in cursor mode + owner suppression + focus-loss neutral latch
    std::vector<RecordedTouch> gpLog;
    uint32_t gpPkts = 0;
    GamepadController gp;
    gp.setTouchSink(recordTouchCb, &gpLog);
    Profile gpProf = ProfileDefaults::createFreeFire();

    GamepadSnapshot sStick;
    sStick.connected = true;
    sStick.lx = 0.0f; sStick.ly = 0.90f;
    sStick.rt = 0.85f; // E2: trigger held
    GamepadFrameOutput gpOut;
    gp.processSnapshot(sStick, gpProf, 1920, 1080, 10.0, gpPkts, gpOut, true /* suppressLeftStick for E3 */);
    assert(gpOut.stickDeflected && !gpOut.analogJoyActive && !gp.isJoyDown() && gpLog.empty());
    assert(gpOut.keyDeltaCount == 1 && gpOut.keyDeltas[0].vk == gpProf.fireVk && gpOut.keyDeltas[0].down);

    // E4: Focus loss lifts held trigger and latches until released to neutral
    gp.suspendForFocusLoss(gpPkts, gpOut);
    assert(gpOut.keyDeltaCount == 1 && gpOut.keyDeltas[0].vk == gpProf.fireVk && !gpOut.keyDeltas[0].down);
    // Holding RT still depressed after focus returns -> must NOT re-trigger!
    gp.processSnapshot(sStick, gpProf, 1920, 1080, 10.0, gpPkts, gpOut, false);
    assert(gpOut.keyDeltaCount == 0);
    // Return RT to neutral, then press again -> fires cleanly!
    GamepadSnapshot sNeutral;
    sNeutral.connected = true;
    gp.processSnapshot(sNeutral, gpProf, 1920, 1080, 10.0, gpPkts, gpOut, false);
    gp.processSnapshot(sStick, gpProf, 1920, 1080, 10.0, gpPkts, gpOut, false);
    assert(gpOut.keyDeltaCount == 1 && gpOut.keyDeltas[0].vk == gpProf.fireVk && gpOut.keyDeltas[0].down);

    // 9. P2 & N12: zero default jitter + comma/hash/backslash escaping in ProfileStore
    assert(Profile().tapJitterPx == 0 && ProfileDefaults::createFreeFire().tapJitterPx == 0);
    Profile escProf = ProfileDefaults::createFreeFire();
    escProf.name = "My #1, Pro\\Cfg";
    escProf.buttons[0].label = "A,B#1\\2";
    const std::string tmpEsc = "profiles\\_test_escape.cfg";
    assert(ProfileStore::save(tmpEsc, escProf));
    Profile escLoaded;
    assert(ProfileStore::load(tmpEsc, escLoaded));
    std::remove(tmpEsc.c_str());
    assert(escLoaded.name == "My #1, Pro\\Cfg");
    assert(!escLoaded.buttons.empty() && escLoaded.buttons[0].label == "A,B#1\\2");

    // 10. N28: InputEngine::updateTuning must NOT release active touches!
    ScrcpyBridge tBridge;
    InputEngine tEng(&tBridge);
    tEng.setGamepadEnabled(false);
    Profile tProf = ProfileDefaults::createFreeFire();
    tEng.setProfile(tProf);
    std::vector<RecordedTouch> n28Log;
    tEng.setTouchSinkForTest(recordTouchCb, &n28Log);
    bool dummyToggle = false;
    tEng.onKeyEvent('W', true, dummyToggle);
    assert(tEng.getTelemetry().joyDown);
    size_t upCountBefore = 0;
    for (size_t i = 0; i < n28Log.size(); ++i) {
        if (n28Log[i].action == TOUCH_UP) ++upCountBefore;
    }
    tProf.sensX = 1.75f;
    tProf.fireMode = FIRE_MODE_DRAG_RIGHT;
    tProf.showHudInPlay = false;
    tEng.updateTuning(tProf);
    size_t upCountAfter = 0;
    for (size_t i = 0; i < n28Log.size(); ++i) {
        if (n28Log[i].action == TOUCH_UP) ++upCountAfter;
    }
    assert(upCountAfter == upCountBefore && tEng.getTelemetry().joyDown);
    assert(std::fabs(tEng.getProfileCopy().sensX - 1.75f) < 0.01f);
    assert(tEng.getProfileCopy().fireMode == FIRE_MODE_DRAG_RIGHT && !tEng.getProfileCopy().showHudInPlay);
    tEng.releaseAllTouches();

    std::printf("[Test] Sprint 0 (V4/V5/V10/V12/B3/B6/E1/E2/E3/E4/P2/N12/N28) OK!\n");
}

static void testSprint1LatencyAndAsync() {
    // 1. 0.1 & 1.1: LatencyTelemetry ring buffer, percentiles, and skipped frame accounting
    LatencyTelemetry tel;
    for (int i = 0; i < 650; ++i) {
        FrameTiming ft;
        ft.qpcPktRecv = 1000;
        ft.qpcProcIn = 1200;
        ft.qpcDecoded = 1200 + 10000;
        ft.qpcPresentCall = 1200 + 10500;
        ft.qpcPresentRet = 1200 + 11000 + (i % 20) * 500;
        ft.present = true;
        tel.recordPresented(ft);
    }
    FrameTiming skp;
    skp.present = false;
    tel.recordSkipped(skp);
    tel.recordSkipped(skp);
    TelemetrySummary sum = tel.getSummary();
    assert(sum.decodedCount == 652);
    assert(sum.presentedCount == 650);
    assert(sum.skippedCount == 2);
    assert(sum.sampleCount == LatencyTelemetry::kRingCapacity);
    assert(sum.rxToPresentP95Ms >= sum.rxToPresentP50Ms && sum.rxToPresentP50Ms > 0.0);

    std::vector<double> pvals = { 10.0, 2.0, 6.0, 4.0, 8.0 };
    assert(std::fabs(LatencyTelemetry::computePercentile(pvals, 0.50) - 6.0) < 0.001);
    assert(std::fabs(LatencyTelemetry::computePercentile(pvals, 0.0) - 2.0) < 0.001);
    assert(std::fabs(LatencyTelemetry::computePercentile(pvals, 1.0) - 10.0) < 0.001);

    // 2. 1.7: ControlPacketQueue always drains touch/key ahead of clipboard, and caps UTF-8 safely
    ControlPacketQueue cq;
    std::vector<uint8_t> clipPkt = { 9, 1, 2, 3 };
    std::vector<uint8_t> touchPkt = { 2, 0, 1, 2 };
    std::vector<uint8_t> keyPkt = { 0, 0, 1, 2 };
    cq.push(clipPkt, false);
    cq.push(touchPkt, true);
    cq.push(keyPkt, true);
    assert(cq.highSize() == 2 && cq.lowSize() == 1);
    std::vector<uint8_t> out;
    assert(cq.popNext(out) && out[0] == 2);
    assert(cq.popNext(out) && out[0] == 0);
    assert(cq.popNext(out) && out[0] == 9);
    assert(!cq.popNext(out));

    const std::string euro = "abc\xE2\x82\xACxyz"; // "abc€xyz"
    std::string capped = ControlPacketQueue::truncateUtf8Cap(euro, 5);
    assert(capped == "abc"); // Must not split 3-byte € sequence
    assert(ControlPacketQueue::kMaxClipboardBytes == 32768);

    // 3. 1.4 & 1.6: AppSettings round-trip for VSync & audio relay tuning
    AppSettings as;
    as.vsyncEnabled = true;
    as.audioBufferMs = 20;
    as.audioRawCodec = true;
    const std::string tmpSet = "profiles\\_test_sprint1_settings.cfg";
    assert(AppSettingsStore::save(tmpSet, as));
    AppSettings loadedAs;
    assert(AppSettingsStore::load(tmpSet, loadedAs));
    std::remove(tmpSet.c_str());
    assert(loadedAs.vsyncEnabled == true);
    assert(loadedAs.audioBufferMs == 20);
    assert(loadedAs.audioRawCodec == true);

    // 4. 1.3: ThreadScheduling EcoQoS & MMCSS smoke test
    ThreadScheduling::disableProcessEcoQoS();
    DWORD taskIdx = 0;
    HANDLE avrt = ThreadScheduling::enableMmcssGames(taskIdx);
    ThreadScheduling::disableMmcss(avrt);

    // 5. 1.2: InputHookThread start, snapshot update, and clean shutdown
    {
        ScrcpyBridge b;
        InputEngine eng(&b);
        InputHookThread ht;
        InputHookSnapshot snap;
        assert(ht.start(GetModuleHandleW(NULL), &eng, snap));
        assert(ht.isRunning());
        snap.aimMode = true;
        ht.updateSnapshot(snap);
        ht.stop();
        assert(!ht.isRunning());
    }

    std::printf("[Test] Sprint 1 (0.1/1.1/1.2/1.3/1.4/1.5/1.6/1.7) OK!\n");
}

int main() {
    GdiPlusSession gdiplus;
    testSpsParser();
    testTouchFingerPool();
    testProfileRoundTrip();
    testEmulatorShellHelpersAndDrawer();
    testJoystickAndActionModules();
    testFpsViewController();
    testInputEngineIntegration();
    testStage1EmulatorFeatures();
    testStage2VideoPipeline();
    testStage3EmulatorFeatures();
    testSprint0Fixes();
    testSprint1LatencyAndAsync();
    std::printf("ALL SPRINT 1 + SPRINT 0 + STAGE 1-3 TESTS PASSED!\n");
    return 0;
}
