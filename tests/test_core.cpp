#include "bridge/SpsParser.hpp"
#include "bridge/TouchFingerPool.hpp"
#include "bridge/ScrcpyBridge.hpp"
#include "config/AppSettings.hpp"
#include "config/KeyCodes.hpp"
#include "config/Profile.hpp"
#include "config/ProfileStore.hpp"
#include "engine/ActionDispatcher.hpp"
#include "engine/FpsViewController.hpp"
#include "engine/InputEngine.hpp"
#include "engine/JoystickController.hpp"
#include "ui/DrawerPanel.hpp"
#include "ui/GdiPlusDraw.hpp"

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
    game.appPackage = "com.supercell.clashofclans,com.supercell.brawlstars";
    const std::string tmpGame = "profiles\\_test_app_link_stage1.cfg";
    assert(ProfileStore::save(tmpGame, game));
    Profile gameLoaded;
    assert(ProfileStore::load(tmpGame, gameLoaded));
    assert(gameLoaded.keymapEnabled && ProfileDefaults::matchesPackage(gameLoaded.appPackage, "com.supercell.brawlstars"));
    std::remove(tmpGame.c_str());

    // 3. AppSettings Stage 1 flags round-trip
    AppSettings s;
    s.autoSwitchProfiles = false;
    s.rightClickBack = false;
    const std::string tmpSet = "profiles\\_test_stage1_settings.cfg";
    assert(AppSettingsStore::save(tmpSet, s));
    AppSettings sLoad;
    assert(AppSettingsStore::load(tmpSet, sLoad));
    assert(!sLoad.autoSwitchProfiles && !sLoad.rightClickBack);
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
    std::printf("ALL PRODUCTION EMULATOR SHELL & ENGINE TESTS PASSED!\n");
    return 0;
}
