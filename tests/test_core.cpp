#include "audio/AudioPlayer.hpp"
#include "bridge/ConnectionState.hpp"
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
#include "ui/Animator.hpp"
#include "ui/DesktopShortcut.hpp"
#include "ui/DrawerPanel.hpp"
#include "ui/EditorState.hpp"
#include "ui/GdiPlusDraw.hpp"
#include "ui/Strings.hpp"
#include "ui/HudRenderer.hpp"
#include "ui/OnboardingView.hpp"
#include "ui/OverlayPopups.hpp"
#include "ui/SessionTabBar.hpp"
#include "ui/SideRail.hpp"
#include "ui/Theme.hpp"
#include "ui/TopBar.hpp"
#include "ui/ViewportHost.hpp"
#include "video/D3D11Renderer.hpp"
#include "video/H264Decoder.hpp"
#include "video/LatencyTelemetry.hpp"
#include "video/Recorder.hpp"

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
    // C2: use rectForAction test hook instead of brittle hard-coded pixel coordinates
    RECT rBlankBtn = { 0, 0, 0, 0 };
    assert(createUi.rectForAction(DA_CREATE_BLANK, 0, rBlankBtn));
    assert(createUi.onMouseDown((rBlankBtn.left + rBlankBtn.right) / 2, (rBlankBtn.top + rBlankBtn.bottom) / 2,
                                editCopy, dummySet, dummySel, mkRes));
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

    // C2: click Key Inspector & SENS controls via DrawerPanel::rectForAction(...)
    RECT rMmb = { 0, 0, 0, 0 }, rUp = { 0, 0, 0, 0 }, rDn = { 0, 0, 0, 0 };
    assert(drawer.rectForAction(DA_INSP_MOUSE_VK, VK_MBUTTON, rMmb));
    DrawerActionResult rMmbRes;
    assert(drawer.onMouseDown((rMmb.left + rMmb.right) / 2, (rMmb.top + rMmb.bottom) / 2, loaded, settings, sel, rMmbRes));
    assert(loaded.buttons[0].vk == VK_MBUTTON && rMmbRes.profileModified);

    assert(drawer.rectForAction(DA_INSP_MOUSE_VK, VK_CUSTOM_MWHEEL_UP, rUp));
    DrawerActionResult rScUp;
    assert(drawer.onMouseDown((rUp.left + rUp.right) / 2, (rUp.top + rUp.bottom) / 2, loaded, settings, sel, rScUp));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_UP);

    assert(drawer.rectForAction(DA_INSP_MOUSE_VK, VK_CUSTOM_MWHEEL_DOWN, rDn));
    DrawerActionResult rScDn;
    assert(drawer.onMouseDown((rDn.left + rDn.right) / 2, (rDn.top + rDn.bottom) / 2, loaded, settings, sel, rScDn));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_DOWN);

    // Switch to SENS tab (tab 1) and verify LShift normalization on Lock/Unlock Key rebind
    drawer.setActiveTab(1);
    {
        Gdiplus::Graphics g(memDC);
        drawer.paint(g, rc, loaded, settings, "profiles\\freefire.cfg", sel, false, true, "SM-G990E", "com.dts.freefiremax", logs, -1, -1);
    }
    RECT rRebind = { 0, 0, 0, 0 };
    assert(drawer.rectForAction(DA_REBIND_TOGGLE_AIM, 0, rRebind));
    DrawerActionResult rReb;
    assert(drawer.onMouseDown((rRebind.left + rRebind.right) / 2, (rRebind.top + rRebind.bottom) / 2, loaded, settings, sel, rReb));
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

static void testSprint1bPhase2PresetsAndTelemetry() {
    // 1. 2.3: Parse real S21 FE --list-encoders output and prefer HW encoder (never SW!)
    const std::string encRaw =
        "[server] INFO: List of video encoders:\n"
        "    --video-codec=h264 --video-encoder=c2.exynos.h264.encoder         (hw) [vendor]\n"
        "    --video-codec=h264 --video-encoder=OMX.Exynos.AVC.Encoder         (hw) [vendor]\n"
        "    --video-codec=h264 --video-encoder=c2.android.avc.encoder         (sw)\n"
        "    --video-codec=h264 --video-encoder=OMX.google.h264.encoder        (sw)\n"
        "    --video-codec=h265 --video-encoder=c2.exynos.hevc.encoder         (hw) [vendor]\n";
    std::vector<VideoEncoderInfo> encs = ScrcpyBridge::parseH264Encoders(encRaw);
    assert(encs.size() == 4);
    assert(encs[0].name == "c2.exynos.h264.encoder" && encs[0].isHardware);
    assert(encs[1].name == "OMX.Exynos.AVC.Encoder" && encs[1].isHardware);
    assert(encs[2].name == "c2.android.avc.encoder" && !encs[2].isHardware);
    assert(ScrcpyBridge::pickBestHardwareH264Encoder(encs) == "c2.exynos.h264.encoder");

    std::vector<VideoEncoderInfo> swOnly(encs.begin() + 2, encs.end());
    assert(ScrcpyBridge::pickBestHardwareH264Encoder(swOnly).empty());

    // 2. 2.2: Option sanitization strips shell metacharacters
    assert(ScrcpyBridge::sanitizeServerOption("intra-refresh-period=60,bitrate-mode=2,max-bframes=0; rm -rf /") ==
           "intra-refresh-period=60,bitrate-mode=2,max-bframes=0rm-rf");

    // 3. 2.5: Thermal telemetry parser (Samsung S21 FE healthy fixture + hot throttling fixture)
    const std::string thOk =
        "  status: 5\n"
        "  level: 100\n"
        "  temperature: 280\n"
        "11-26 18:23:03 Sending ACTION_BATTERY_CHANGED: level:99, temperature:450\n"
        "---THERMAL---\n"
        "Thermal Status: 0\n";
    PhoneThermalInfo tiOk = ScrcpyBridge::parseThermalTelemetry(thOk);
    assert(tiOk.valid && tiOk.batteryLevel == 100);
    assert(std::fabs(tiOk.batteryTempC - 28.0f) < 0.05f);
    assert(tiOk.thermalStatus == 0 && tiOk.thermalLabel == "OK");

    const std::string thHot = "  level: 42\n  temperature: 435\n---THERMAL---\nThermal Status: 3\n";
    PhoneThermalInfo tiHot = ScrcpyBridge::parseThermalTelemetry(thHot);
    assert(tiHot.valid && tiHot.batteryLevel == 42);
    assert(std::fabs(tiHot.batteryTempC - 43.5f) < 0.05f);
    assert(tiHot.thermalStatus == 3 && tiHot.thermalLabel == "SEVERE");

    // 4. 2.1/2.2/2.3/2.4: Stream presets, detectStreamPreset, per-device encoder persistence, and legacy cfg compat
    AppSettings ps;
    ps.applyStreamPreset(PRESET_COMPETITIVE);
    assert(ps.mirrorMaxSize == 1280 && ps.mirrorFps == 0 && ps.mirrorBitrateMbps == 16);
    assert(ps.videoCodecOptions == "bitrate-mode=2,max-bframes=0" && ps.audioRawCodec);
    assert(ps.detectStreamPreset() == PRESET_COMPETITIVE);

    ps.applyStreamPreset(PRESET_WIFI);
    assert(ps.mirrorFps == 60 && ps.mirrorBitrateMbps == 8);
    assert(ps.videoCodecOptions == "intra-refresh-period=60,bitrate-mode=2,max-bframes=0");
    assert(ps.detectStreamPreset() == PRESET_WIFI);
    assert(ps.buildScrcpyArgs().find("--video-codec-options=intra-refresh-period=60,bitrate-mode=2,max-bframes=0") != std::string::npos);

    ps.setEncoderForDevice("R5CRC0RS88Y", "c2.exynos.h264.encoder");
    const std::string tmp1b = "profiles\\_test_sprint1b_settings.cfg";
    assert(AppSettingsStore::save(tmp1b, ps));
    AppSettings loaded1b;
    assert(AppSettingsStore::load(tmp1b, loaded1b));
    std::remove(tmp1b.c_str());
    assert(loaded1b.detectStreamPreset() == PRESET_WIFI);
    assert(loaded1b.encoderForDevice("R5CRC0RS88Y") == "c2.exynos.h264.encoder");

    // 5. Phase 2 telemetry: rxToDecodeP50Ms / rxToDecodeP95Ms and measuredFps
    LatencyTelemetry tel2;
    for (int i = 0; i < 61; ++i) {
        FrameTiming ft;
        ft.qpcPktRecv = 1000000LL + static_cast<int64_t>(i) * 100000LL;
        ft.qpcProcIn  = ft.qpcPktRecv + 1000LL;
        ft.qpcDecoded = ft.qpcPktRecv + 25000LL;
        ft.qpcPresentCall = 0;
        ft.qpcPresentRet  = 0;
        ft.present = true;
        tel2.recordPresented(ft);
    }
    TelemetrySummary sum2 = tel2.getSummary();
    assert(sum2.rxToDecodeP50Ms > 0.0 && sum2.rxToDecodeP95Ms >= sum2.rxToDecodeP50Ms);
    assert(sum2.measuredFps > 1.0);

    std::printf("[Test] Sprint 1b (2.1/2.2/2.3/2.4/2.5/2.6 presets, encoders & thermal) OK!\n");
}

static void testSprint2aUiFoundation() {
    // 1. U1 contrast/HUD tokens
    assert(Theme::BG_CARD == 0xFF232E42u);
    assert(Theme::TEXT_MUTED == 0xFF8B9BB4u);
    assert(Theme::HUD_FIRE == 0xFFFF8A3Du);

    // 2. U2 Theme::Metrics at 96 / 144 (150%) / 192 (200%) DPI
    Theme::Metrics m100, m150, m200;
    m100.dpi = 96;
    m150.dpi = 144;
    m200.dpi = 192;
    assert(m100.topBarH() == 40 && m150.topBarH() == 60 && m200.topBarH() == 80);
    assert(m100.railW() == 54 && m150.railW() == 81 && m200.railW() == 108);
    assert(m100.drawerW() == 356 && m150.drawerW() == 534 && m200.drawerW() == 712);
    assert(m100.minWinW() == 960 && m150.minWinW() == 1440 && m200.minWinW() == 1920);
    assert(m200.minWinH() == 1200);

    // 3. C1/C2/U2/U3/S2(a): TopBar maximizeButtonRect & hoverItemAt + SideRail + DrawerPanel at 100/150/200% DPI
    Profile p = ProfileDefaults::createFreeFire();
    EngineTelemetry tel;
    AppSettings s;
    std::vector<std::string> logs;
    HDC scDC = GetDC(NULL);
    HDC mDC = CreateCompatibleDC(scDC);
    HBITMAP bmp = CreateCompatibleBitmap(scDC, 1400, 1800);
    HGDIOBJ old = SelectObject(mDC, bmp);

    const int dpis[3] = { 96, 144, 192 };
    for (int i = 0; i < 3; ++i) {
        GdiPlusDraw::setActiveDpi(dpis[i]);
        Theme::Metrics mm;
        mm.dpi = dpis[i];

        TopBar tb;
        RECT tbRc = { 0, 0, mm.dp(1200), mm.topBarH() };
        {
            Gdiplus::Graphics g(mDC);
            tb.paint(g, tbRc, "SM-G990E", true, false, p, tel, false, false, -1, -1);
        }
        RECT maxRc = tb.maximizeButtonRect();
        assert(maxRc.right > maxRc.left && maxRc.bottom > maxRc.top);
        assert(tb.hitTest((maxRc.left + maxRc.right) / 2, (maxRc.top + maxRc.bottom) / 2) == TB_MAXIMIZE);
        int hid = -1;
        RECT hrc = { 0, 0, 0, 0 };
        assert(tb.hoverItemAt((maxRc.left + maxRc.right) / 2, (maxRc.top + maxRc.bottom) / 2, hid, hrc));
        assert(hid == TB_MAXIMIZE && hrc.left == maxRc.left);

        SideRail sr;
        SideRailState srSt;
        srSt.connected = true;
        RECT srRc = { mm.dp(1146), mm.topBarH(), mm.dp(1200), mm.dp(760) };
        {
            Gdiplus::Graphics g(mDC);
            sr.paint(g, srRc, srSt, -1, -1);
        }
        assert(sr.hoverItemAt((srRc.left + srRc.right) / 2, mm.topBarH() + mm.dp(16), hid, hrc));
        assert(hid == SR_AIM_TOGGLE);

        DrawerPanel dpnl;
        dpnl.setProfilesDir("profiles", "profiles\\freefire.cfg");
        dpnl.setActiveTab(0);
        RECT dRc = { 0, 0, mm.drawerW(), mm.dp(850) };
        {
            Gdiplus::Graphics g(mDC);
            dpnl.paint(g, dRc, p, s, "profiles\\freefire.cfg", 0, false, true, "SM-G990E", "com.dts.freefiremax", logs, -1, -1);
        }
        RECT rMmb = { 0, 0, 0, 0 };
        assert(dpnl.rectForAction(DA_INSP_MOUSE_VK, VK_MBUTTON, rMmb));
        assert(rMmb.bottom <= dRc.bottom && rMmb.right > rMmb.left);
    }
    GdiPlusDraw::setActiveDpi(96);

    SelectObject(mDC, old);
    DeleteObject(bmp);
    DeleteDC(mDC);
    ReleaseDC(NULL, scDC);
    std::printf("[Test] Sprint 2A (C1/C2/U1/U2/U3/S2/S4/N29/N32) OK!\n");
}

static void testSprint2bConnectionOnboardingAndSettings() {
    // 1. F3 exponential backoff & X2/F3 connection state machine
    assert(ConnectionStateMachine::backoffMs(0) == 1000);
    assert(ConnectionStateMachine::backoffMs(1) == 2000);
    assert(ConnectionStateMachine::backoffMs(2) == 4000);
    assert(ConnectionStateMachine::backoffMs(3) == 8000);
    assert(ConnectionStateMachine::backoffMs(4) == 10000);
    assert(ConnectionStateMachine::backoffMs(5) == 10000);

    ConnStateInput ci;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_NO_TOOLS);
    ci.toolsReady = true;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_NO_DEVICE);
    ci.unauthorizedDeviceCount = 1;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_UNAUTHORIZED);
    ci.readyDeviceCount = 1;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_READY);
    ci.reconnecting = true;
    ci.reconnectAttempt = 2;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_RECONNECTING);
    ci.reconnectAttempt = 6;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_LOST);
    ci.connecting = true;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_CONNECTING);
    ci.streaming = true;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_STREAMING);
    ci = ConnStateInput();
    ci.toolsReady = true;
    ci.hasError = true;
    assert(ConnectionStateMachine::evaluate(ci) == CONN_ERROR);

    // 2. X2 FLAG_SECURE detection parser
    assert(!ScrcpyBridge::parseWindowSecureFlag("mCurrentFocus=Window{123 u0 com.dts.freefiremax}"));
    assert(ScrcpyBridge::parseWindowSecureFlag("mCurrentFocus=Window{456 u0 com.bank.app} flags=FLAG_SECURE"));
    assert(ScrcpyBridge::parseWindowSecureFlag("WindowState{abc isSecure=true}"));

    // 3. F1 OnboardingView state machine & AppSettings::onboardingDone + hasPendingStreamChanges
    assert(OnboardingView::computeRecommendedStep(OB_STEP_TOOLS, false, false, false, false) == OB_STEP_TOOLS);
    assert(OnboardingView::computeRecommendedStep(OB_STEP_TOOLS, true, false, false, false) == OB_STEP_PREPARE);
    assert(OnboardingView::computeRecommendedStep(OB_STEP_TOOLS, true, false, true, false) == OB_STEP_CONNECT);
    assert(OnboardingView::computeRecommendedStep(OB_STEP_TOOLS, true, false, false, true) == OB_STEP_CONNECT);
    assert(OnboardingView::computeRecommendedStep(OB_STEP_CONNECT, true, true, true, false) == OB_STEP_PROFILE);
    assert(OnboardingView::computeRecommendedStep(OB_STEP_TOUR, true, true, true, false) == OB_STEP_TOUR);

    AppSettings baseSet, liveSet;
    assert(!baseSet.onboardingDone);
    liveSet.vsyncEnabled = true;
    liveSet.turnScreenOff = true;
    liveSet.gamepadEnabled = false;
    liveSet.onboardingDone = true;
    assert(!AppSettings::hasPendingStreamChanges(liveSet, baseSet, "SER1"));
    liveSet.mirrorBitrateMbps = 24;
    assert(AppSettings::hasPendingStreamChanges(liveSet, baseSet, "SER1"));
    liveSet.mirrorBitrateMbps = baseSet.mirrorBitrateMbps;
    liveSet.setEncoderForDevice("SER1", "c2.exynos.h264.encoder");
    assert(AppSettings::hasPendingStreamChanges(liveSet, baseSet, "SER1"));

    std::string tmpCfg = "profiles\\_test_s2b_settings.cfg";
    assert(AppSettingsStore::save(tmpCfg, liveSet));
    AppSettings reloaded;
    assert(AppSettingsStore::load(tmpCfg, reloaded));
    assert(reloaded.onboardingDone == true);
    DeleteFileA(tmpCfg.c_str());

    // 4. F16 DrawerPanel Advanced section + tuningOnlyModified + Device pending bar
    HDC scDC = GetDC(NULL);
    HDC mDC = CreateCompatibleDC(scDC);
    HBITMAP bmp = CreateCompatibleBitmap(scDC, 1200, 1600);
    HGDIOBJ old = SelectObject(mDC, bmp);
    GdiPlusDraw::setActiveDpi(96);

    DrawerPanel dpnl;
    dpnl.setProfilesDir("profiles", "profiles\\freefire.cfg");
    dpnl.setActiveTab(1); // SENS
    Profile prof = ProfileDefaults::createFreeFire();
    AppSettings set;
    std::vector<std::string> logs;
    RECT dRc = { 0, 0, 356, 1500 };
    {
        Gdiplus::Graphics g(mDC);
        dpnl.paint(g, dRc, prof, set, "profiles\\freefire.cfg", -1, false, true, "SM-G990E", "", logs, -1, -1);
    }
    RECT rAdvBtn = { 0, 0, 0, 0 };
    assert(dpnl.rectForAction(DA_TOGGLE_ADVANCED, -1, rAdvBtn));
    int sel = -1;
    DrawerActionResult res;
    assert(dpnl.onMouseDown((rAdvBtn.left + rAdvBtn.right) / 2, (rAdvBtn.top + rAdvBtn.bottom) / 2, prof, set, sel, res));
    assert(dpnl.isAdvancedExpanded());
    {
        Gdiplus::Graphics g(mDC);
        dpnl.paint(g, dRc, prof, set, "profiles\\freefire.cfg", -1, false, true, "SM-G990E", "", logs, -1, -1);
    }
    RECT rResetKey = { 0, 0, 0, 0 };
    assert(dpnl.rectForAction(DA_REBIND_RESET_TOUCH, -1, rResetKey));
    res = DrawerActionResult();
    assert(dpnl.onMouseDown((rResetKey.left + rResetKey.right) / 2, (rResetKey.top + rResetKey.bottom) / 2, prof, set, sel, res));
    assert(dpnl.isWaitingSpecialRebind());
    res = DrawerActionResult();
    assert(dpnl.onKeyDown(VK_F6, prof, set, sel, res));
    assert(prof.resetTouchVk == VK_F6 && res.profileModified);

    RECT rFireDual = { 0, 0, 0, 0 };
    assert(dpnl.rectForAction(DA_FIRE_DUAL, -1, rFireDual));
    res = DrawerActionResult();
    assert(dpnl.onMouseDown((rFireDual.left + rFireDual.right) / 2, (rFireDual.top + rFireDual.bottom) / 2, prof, set, sel, res));
    assert(res.tuningOnlyModified && !res.profileModified);

    // 5. F3 ViewportHost scrollable Hub renders all 6 devices (no 3-device limit)
    std::vector<AdbDeviceInfo> sixDevs;
    for (int i = 0; i < 6; ++i) {
        AdbDeviceInfo d;
        char sbuf[32], mbuf[32];
        std::snprintf(sbuf, sizeof(sbuf), "DEV-%d", i);
        std::snprintf(mbuf, sizeof(mbuf), "Phone_%d", i);
        d.serial = sbuf;
        d.model = mbuf;
        d.state = "device";
        sixDevs.push_back(d);
    }
    ViewportHost vh;
    HubViewState hvs;
    hvs.connState = CONN_READY;
    hvs.devices = &sixDevs;
    RECT vpRc = { 0, 0, 860, 660 };
    {
        Gdiplus::Graphics g(mDC);
        vh.paint(g, vpRc, false, false, hvs, -1, -1);
    }
    assert(vh.onHubMouseWheel(430, 300, -120 * 3));
    {
        Gdiplus::Graphics g(mDC);
        vh.paint(g, vpRc, false, false, hvs, -1, -1);
    }
    RECT rDev5 = { 0, 0, 0, 0 };
    assert(vh.rectForHubAction(HUB_CONNECT_DEVICE, 5, rDev5));
    HubClickResult hcr = vh.hitTestHub((rDev5.left + rDev5.right) / 2, (rDev5.top + rDev5.bottom) / 2);
    assert(hcr.action == HUB_CONNECT_DEVICE && hcr.index == 5);

    // 6. F1 OnboardingView paint + click verification
    OnboardingView ob;
    ob.setStep(OB_STEP_CONNECT);
    QrMatrix qrm;
    {
        Gdiplus::Graphics g(mDC);
        ob.paint(g, vpRc, true, false, false, false, "", "", sixDevs, "192.168.1.50:5555", false, &qrm, "", -1, -1);
    }
    OnboardingClickResult obRes;
    RECT rSkip = { 0, 0, 0, 0 };
    assert(ob.rectForStepButton(1, rSkip)); // BTN_SKIP
    assert(ob.onMouseDown((rSkip.left + rSkip.right) / 2, (rSkip.top + rSkip.bottom) / 2, obRes));
    assert(obRes.action == OB_ACT_SKIP);

    SelectObject(mDC, old);
    DeleteObject(bmp);
    DeleteDC(mDC);
    ReleaseDC(NULL, scDC);
    std::printf("[Test] Sprint 2B (X2/F1/F2/F3/F16) OK!\n");
}

static void removeDirRecursiveA(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            removeDirRecursiveA(full);
            RemoveDirectoryA(full.c_str());
        } else {
            DeleteFileA(full.c_str());
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    RemoveDirectoryA(dir.c_str());
}

static void testSprint3aProfileLibraryEditorAndDynamicHud() {
    std::printf("[Test] Sprint 3A (F4 Templates/Import/Trash, X4 Editor, X9 Undo, S5 Dynamic HUD)...\n");

    // 1. All 6 Profile Templates & Human Action Labels
    for (int k = 0; k <= static_cast<int>(TMPL_PLATFORMER); ++k) {
        ProfileTemplateKind kind = static_cast<ProfileTemplateKind>(k);
        Profile tp = ProfileDefaults::createTemplate(kind);
        assert(!tp.name.empty());
        assert(std::strlen(ProfileDefaults::templateName(kind)) > 0);
        if (kind == TMPL_DESKTOP || kind == TMPL_CONTROLLER_BASIC) {
            assert(!tp.keymapEnabled);
        } else {
            assert(tp.keymapEnabled);
            assert(!tp.buttons.empty());
        }
    }
    assert(ProfileDefaults::actionTypeToHumanLabel(NODE_PRESS) == "Hold");
    assert(ProfileDefaults::actionTypeToHumanLabel(NODE_CLICK) == "Tap");
    assert(ProfileDefaults::actionTypeToHumanLabel(NODE_DOWN_UP) == "Tap Down & Up");
    assert(ProfileDefaults::actionTypeToHumanLabel(NODE_HOLD_VIEW) == "Free Look");
    assert(ProfileDefaults::actionTypeToHumanLabel(NODE_CURSOR_TOGGLE) == "Cursor Toggle");

    // 2. ProfileStore serialize/parse v1 + headerless v0 + escaping + error cases
    Profile br = ProfileDefaults::createTemplate(TMPL_MOBA);
    br.name = "My , Pro #1 \\MOBA";
    std::string cfgText = ProfileStore::serializeToString(br);
    assert(cfgText.find("# phantom-touch-profile v1") != std::string::npos);

    Profile parsed;
    std::string parseErr;
    assert(ProfileStore::parseFromString(cfgText, parsed, &parseErr));
    assert(parsed.name == br.name);
    assert(parsed.buttons.size() == br.buttons.size());

    // Headerless v0 still parses cleanly
    size_t firstNl = cfgText.find('\n');
    std::string v0Text = (firstNl != std::string::npos) ? cfgText.substr(firstNl + 1) : cfgText;
    Profile parsedV0;
    assert(ProfileStore::parseFromString(v0Text, parsedV0, &parseErr));
    assert(parsedV0.buttons.size() == br.buttons.size());

    // Garbage text rejected cleanly
    Profile badProf;
    std::string badErr;
    assert(!ProfileStore::parseFromString("not_a_profile_at_all\n###\n", badProf, &badErr));
    assert(!badErr.empty());

    // 3. Import clash deduplication + soft-delete Trash & Restore
    std::string tempDir = "tests\\_tmp_s3a_profiles";
    removeDirRecursiveA(tempDir);
    CreateDirectoryA(tempDir.c_str(), NULL);
    std::string srcCfg = tempDir + "\\incoming_source.txt";
    assert(ProfileStore::save(srcCfg, br));

    std::string imp1, imp2, impErr;
    assert(ProfileStore::importProfileFile(srcCfg, tempDir, imp1, impErr));
    assert(ProfileStore::importProfileFile(srcCfg, tempDir, imp2, impErr));
    assert(imp1 != imp2);
    assert(ProfileStore::listProfiles(tempDir).size() == 2);

    std::string trashPath;
    assert(ProfileStore::moveToTrash(imp2, tempDir, trashPath));
    assert(ProfileStore::listProfiles(tempDir).size() == 1);
    assert(GetFileAttributesA(trashPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    assert(ProfileStore::restoreFromTrash(trashPath, imp2));
    assert(ProfileStore::listProfiles(tempDir).size() == 2);

    // 4. N27 Last Phone Dimensions Persistence
    AppSettings sDef;
    sDef.lastPhoneW = 2340;
    sDef.lastPhoneH = 1080;
    std::string setTmp = tempDir + "\\settings.cfg";
    assert(AppSettingsStore::save(setTmp, sDef));
    AppSettings sLoad;
    assert(AppSettingsStore::load(setTmp, sLoad));
    assert(sLoad.lastPhoneW == 2340 && sLoad.lastPhoneH == 1080);

    removeDirRecursiveA(tempDir);

    // 5. ProfileUndoStack bounded history & Redo
    ProfileUndoStack undoStack;
    Profile cur = ProfileDefaults::createBlank("undo_test");
    for (int i = 0; i < 60; ++i) {
        Profile before = cur;
        KeyNode n;
        n.vk = 'A' + (i % 26);
        n.label = "K";
        cur.buttons.push_back(n);
        undoStack.push(before, "add");
    }
    assert(undoStack.canUndo());
    int undone = 0;
    while (undoStack.canUndo()) {
        assert(undoStack.undo(cur));
        ++undone;
    }
    assert(undone == static_cast<int>(ProfileUndoStack::kMaxDepth)); // bounded at 50!
    assert(cur.buttons.size() == 10);
    assert(undoStack.canRedo());
    assert(undoStack.redo(cur));
    assert(cur.buttons.size() == 11);

    // 6. EditorGeometry snapping, alignment guides, and conflict detection
    Profile ed;
    KeyNode a; a.vk = 'G'; a.label = "G1"; a.x = 0.500f; a.y = 0.400f;
    KeyNode b; b.vk = 'G'; b.label = "G2"; b.x = 0.504f; b.y = 0.700f; // near-aligned x & duplicate key
    KeyNode m; m.vk = VK_LBUTTON; m.label = "LMB"; m.x = 0.300f; m.y = 0.300f;
    ed.buttons.push_back(a);
    ed.buttons.push_back(b);
    ed.buttons.push_back(m);

    float snapX = 0.504f, snapY = 0.650f;
    std::vector<AlignGuide> guides;
    EditorGeometry::snapAndCollectGuides(ed, 1, snapX, snapY, false, guides);
    assert(std::fabs(snapX - 0.500f) < 0.001f);
    assert(!guides.empty());

    float gridX = 0.413f, gridY = 0.627f;
    EditorGeometry::snapAndCollectGuides(ed, 1, gridX, gridY, true, guides);
    assert(std::fabs(gridX - 0.41f) < 0.001f);
    assert(std::fabs(gridY - 0.63f) < 0.001f);

    std::vector<KeyConflict> conflicts = EditorGeometry::detectConflicts(ed, false /* cursor mode */);
    bool sawDup = false, sawCursorMouse = false;
    for (size_t ci = 0; ci < conflicts.size(); ++ci) {
        if (conflicts[ci].buttonIndex == 0 || conflicts[ci].buttonIndex == 1) sawDup = true;
        if (conflicts[ci].buttonIndex == 2 && conflicts[ci].cannotFireInCursor) sawCursorMouse = true;
    }
    assert(sawDup && sawCursorMouse);

    // 7. HudRenderer clean glyph labels, snackbar button rect, and extras
    assert(HudRenderer::shortKeyLabel(0, VK_SPACE) == "Spc");
    assert(HudRenderer::shortKeyLabel(0, VK_CUSTOM_MWHEEL_UP) == "Scr+");
    assert(HudRenderer::shortKeyLabel(0, VK_OEM_3) == "~");

    HDC scDC = GetDC(NULL);
    HDC mDC = CreateCompatibleDC(scDC);
    HBITMAP bmp = CreateCompatibleBitmap(scDC, 600, 900);
    HGDIOBJ old = SelectObject(mDC, bmp);
    {
        Gdiplus::Graphics g(mDC);
        std::vector<int> multi; multi.push_back(0); multi.push_back(1);
        HudEditExtras ex;
        ex.multiSelected = &multi;
        ex.alignGuides = &guides;
        ex.conflicts = &conflicts;
        ex.marqueeActive = true;
        ex.marqueeRc = RECT{ 20, 20, 180, 140 };
        ex.skipDynamicDots = true;
        EngineTelemetry tel;
        HudRenderer::paintKeymapNodes(g, 600, 900, ed, tel, true, false, 0, 1, false, false, 1.0f, &ex);
        RECT undoRc = { 0, 0, 0, 0 };
        HudRenderer::paintToastWithAction(g, 600, 900, "Cleared extra keys", "Undo", &undoRc);
        assert(undoRc.right > undoRc.left && undoRc.bottom > undoRc.top);

        // 8. DrawerPanel palette search + Profiles templates/import/export + Live Test button + Undo clear
        DrawerPanel dp;
        RECT dr = { 0, 0, 356, 860 };
        AppSettings appS;
        dp.setActiveTab(0);
        dp.paint(g, dr, ed, appS, "", 0, false, true, "Phone", "com.test", std::vector<std::string>(), -1, -1);

        RECT rPS = { 0, 0, 0, 0 };
        assert(dp.rectForAction(DA_FOCUS_PALETTE_SEARCH, -1, rPS));
        DrawerActionResult res;
        int sel = 0;
        assert(dp.onMouseDown((rPS.left + rPS.right) / 2, (rPS.top + rPS.bottom) / 2, ed, appS, sel, res));
        for (const char* s = "Swipe"; *s; ++s) assert(dp.onChar(*s, ed, appS, sel, res));
        dp.paint(g, dr, ed, appS, "", 0, false, true, "Phone", "com.test", std::vector<std::string>(), -1, -1);
        RECT rSwipeAdd = { 0, 0, 0, 0 }, rPressAdd = { 0, 0, 0, 0 };
        assert(dp.rectForAction(DA_ADD_NODE, static_cast<int>(NODE_SWIPE), rSwipeAdd));
        assert(!dp.rectForAction(DA_ADD_NODE, static_cast<int>(NODE_PRESS), rPressAdd));

        RECT rTestLive = { 0, 0, 0, 0 }, rClrAll = { 0, 0, 0, 0 };
        assert(dp.rectForAction(DA_TOGGLE_EDIT_TEST, -1, rTestLive));
        assert(dp.rectForAction(DA_CLEAR_BUTTONS, -1, rClrAll));
        DrawerActionResult resClr;
        assert(dp.onMouseDown((rClrAll.left + rClrAll.right) / 2, (rClrAll.top + rClrAll.bottom) / 2, ed, appS, sel, resClr));
        assert(resClr.requestClearButtonsUndo);

        // Profiles tab: 6 starter templates + Import/Export/Copy Text hotspots
        dp.setActiveTab(2);
        dp.paint(g, dr, ed, appS, "", -1, false, true, "Phone", "com.test", std::vector<std::string>(), -1, -1);
        for (int ti = 0; ti < 6; ++ti) {
            RECT rt = { 0, 0, 0, 0 };
            assert(dp.rectForAction(DA_CREATE_TEMPLATE, ti, rt));
        }
        RECT rImp = { 0, 0, 0, 0 }, rExp = { 0, 0, 0, 0 }, rCpy = { 0, 0, 0, 0 };
        assert(dp.rectForAction(DA_IMPORT_PROFILE, -1, rImp));
        assert(dp.rectForAction(DA_EXPORT_PROFILE, -1, rExp));
        assert(dp.rectForAction(DA_COPY_PROFILE_TEXT, -1, rCpy));
    }
    SelectObject(mDC, old);
    DeleteObject(bmp);
    DeleteDC(mDC);
    ReleaseDC(NULL, scDC);

    // 9. D3D11Renderer::setDynamicHudMarkers thread-safe pre-init call (S5)
    D3D11Renderer renderer;
    DynamicHudMarkers dm;
    dm.active = true;
    dm.aimFingerDown = true;
    dm.joyDown = true;
    renderer.setDynamicHudMarkers(dm);

    std::printf("[Test] Sprint 3A (F4/X4/X9/S5) OK!\n");
}

static void testSprint3bPolishCommandPaletteMotionAndA11y() {
    std::printf("[Test] Sprint 3B (U5/U6/X5/X6/X7/X8/U4/U7)...\n");

    // 1. U6: Animator easing, dirty-rect tracking, completion, and Reduce Motion override
    Animator::setReduceMotionOverrideForTest(0);
    Animator anim;
    RECT animRc = { 10, 20, 310, 620 };
    anim.start(ANIM_DRAWER_SLIDE, 300.0f, 0.0f, ANIM_BASE_MS, EASE_OUT_CUBIC, &animRc, 1000.0);
    assert(anim.isActive(ANIM_DRAWER_SLIDE));
    assert(anim.hasActive());
    std::vector<RECT> dirty;
    assert(anim.tick(1090.0, dirty));
    assert(dirty.size() == 1);
    float mid = anim.value(ANIM_DRAWER_SLIDE, -1.0f);
    assert(mid > 0.0f && mid < 150.0f); // ease-out cubic reaches >50% progress by t=0.5
    dirty.clear();
    assert(anim.tick(1250.0, dirty));
    assert(!anim.isActive(ANIM_DRAWER_SLIDE));
    assert(!anim.hasActive());
    assert(std::fabs(anim.value(ANIM_DRAWER_SLIDE, -1.0f)) < 0.01f);

    Animator::setReduceMotionOverrideForTest(1); // Reduce Motion snaps immediately
    anim.start(ANIM_HOVER_FADE, 0.0f, 1.0f, ANIM_FAST_MS, EASE_LINEAR, &animRc, 2000.0);
    assert(!anim.isActive(ANIM_HOVER_FADE));
    assert(std::fabs(anim.value(ANIM_HOVER_FADE, 0.0f) - 1.0f) < 0.01f);
    Animator::setReduceMotionOverrideForTest(-1);

    // 2. X5: CommandPaletteModel fuzzy matching + paint rows
    CommandPaletteModel pal;
    std::vector<CommandEntry> cmds;
    {
        CommandEntry e1; e1.id = "mode.aim"; e1.title = "Toggle Keymap Mode"; e1.keywords = "aim typing f12"; e1.hotkey = "F12"; e1.category = "MODE";
        CommandEntry e2; e2.id = "app.screenshot"; e2.title = "Take Screenshot"; e2.keywords = "capture snap"; e2.hotkey = ""; e2.category = "VIEW";
        CommandEntry e3; e3.id = "tab.device"; e3.title = "Open Device Settings"; e3.keywords = "adb usb wifi"; e3.hotkey = ""; e3.category = "DEVICE";
        cmds.push_back(e1); cmds.push_back(e2); cmds.push_back(e3);
    }
    pal.setCommands(cmds);
    assert(CommandPaletteModel::fuzzyScore("Take Screenshot", "shot") > 0);
    std::vector<int> fAll = pal.filter("");
    assert(fAll.size() == 3);
    std::vector<int> fShot = pal.filter("shot");
    assert(fShot.size() == 1 && cmds[fShot[0]].id == "app.screenshot");

    // 3. X6: CheatSheetModel dynamic build from current profile
    Profile prof;
    prof.name = "Free Fire Test";
    prof.keymapEnabled = true;
    KeyNode rNode;
    rNode.vk = 'R';
    rNode.label = "Reload";
    rNode.x = 0.75f;
    rNode.y = 0.80f;
    prof.buttons.push_back(rNode);
    CheatSheetData sheet = CheatSheetModel::build(prof);
    assert(sheet.profileName == "Free Fire Test");
    assert(sheet.keymapEnabled == true);
    assert(!sheet.profileBindings.empty());
    assert(!sheet.globalHotkeys.empty());

    // 4. X7: ProfileDropdownModel filter + X8: countPendingStreamChanges
    std::vector<ProfileMenuItem> menuItems(2);
    menuItems[0].path = "p1.json"; menuItems[0].displayName = "Free Fire"; menuItems[0].appPackage = "com.dts.freefireth"; menuItems[0].active = true;
    menuItems[1].path = "p2.json"; menuItems[1].displayName = "BGMI"; menuItems[1].appPackage = "com.pubg.imobile"; menuItems[1].recent = true;
    std::vector<int> pf = ProfileDropdownModel::filter(menuItems, "bgmi");
    assert(pf.size() == 1 && pf[0] == 1);

    AppSettings sLive;
    AppSettings sApplied = sLive;
    assert(AppSettings::countPendingStreamChanges(sLive, sApplied, "SER1") == 0);
    sLive.mirrorFps = 120;
    sLive.mirrorBitrateMbps = 24;
    sLive.virtualDisplayMode = true;
    assert(AppSettings::countPendingStreamChanges(sLive, sApplied, "SER1") == 3);

    // 5. U5/U4/U7/X5/X6/X7 Paint + Focus + Telemetry glow smoke on offscreen GDI+ DC
    HDC scDC = GetDC(NULL);
    HDC mDC = CreateCompatibleDC(scDC);
    HBITMAP bmp = CreateCompatibleBitmap(scDC, 1100, 760);
    HGDIOBJ old = SelectObject(mDC, bmp);
    {
        Gdiplus::Graphics g(mDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        UiIcon newIcons[] = {
            ICON_GAMEPAD, ICON_CAST, ICON_RECORD, ICON_FOLDER,
            ICON_LANGUAGE, ICON_SHIELD, ICON_QR, ICON_APPS
        };
        for (int i = 0; i < 8; ++i) {
            GdiPlusDraw::drawIcon(g, newIcons[i], 30.0f + i * 24.0f, 30.0f, 16.0f, Theme::ACCENT_CYAN);
        }

        std::vector<RECT> palRows;
        pal.paint(g, 560, 400, "shot", fShot, 0, -1, palRows);
        assert(palRows.size() == 1 && palRows[0].bottom > palRows[0].top);
        CheatSheetModel::paint(g, 680, 480, sheet);
        std::vector<RECT> profRows;
        RECT manageRc = { 0, 0, 0, 0 };
        ProfileDropdownModel::paint(g, 320, 320, "", menuItems, pf, 0, -1, profRows, manageRc);
        assert(profRows.size() == 1 && manageRc.bottom > manageRc.top);

        TopBar topBar;
        EngineTelemetry tel;
        tel.activeButtonMask = 1ULL;
        TelemetrySummary vTel;
        vTel.measuredFps = 92.0;
        RECT tbRc = { 0, 0, 1080, 46 };
        topBar.paint(g, tbRc, "SM-G990E", true, false, prof, tel, false, false, 10, 10, vTel, TB_CONN_CONNECTED);
        RECT pr = topBar.profilePillRect();
        assert(pr.right > pr.left);
        assert(!topBar.tooltipAt((pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2).empty());

        SideRail rail;
        SideRailState srs;
        srs.connected = true;
        RECT srRc = { 1016, 46, 1080, 740 };
        rail.paint(g, srRc, srs, 1048, 80);
        RECT srItem = { 0, 0, 0, 0 };
        int hitY = -1;
        for (int y = srRc.top; y < srRc.bottom; y += 4) {
            if (rail.itemRectAt(1048, y, srItem)) {
                hitY = y;
                break;
            }
        }
        assert(hitY >= 0);
        assert(!rail.tooltipAt(1048, hitY).empty());

        HudRenderer::paintKeymapNodes(g, 590, 700, prof, tel, false, false, -1, -1, false, false, 0.85f);
        RECT toastAct = { 0, 0, 0, 0 };
        HudRenderer::paintToastWithAction(g, 590, 700, "Action failed", "Retry", &toastAct, TOAST_ERROR);
        assert(toastAct.right > toastAct.left);

        DrawerPanel dp;
        dp.setActiveTab(3);
        dp.setPendingStreamChangeCount(3);
        RECT dRc = { 680, 46, 1016, 740 };
        dp.paint(g, dRc, prof, sLive, "p1.json", -1, false, true, "SM-G990E", "com.dts.freefireth",
                 std::vector<std::string>(), 0, 0, 0);
        assert(!dp.hasKeyboardFocus());
        DrawerActionResult kRes;
        assert(dp.onKeyDown(VK_TAB, prof, sLive, -1, kRes));
        assert(dp.hasKeyboardFocus());
        assert(dp.onKeyDown(VK_ESCAPE, prof, sLive, -1, kRes));
        assert(!dp.hasKeyboardFocus());

        RECT advRc = { 0, 0, 0, 0 };
        for (int step = 0; step < 40 && !dp.rectForAction(DA_TOGGLE_DEV_ADVANCED, -1, advRc); ++step) {
            dp.onMouseWheel(800, 400, -360);
            dp.paint(g, dRc, prof, sLive, "p1.json", -1, false, true, "SM-G990E", "com.dts.freefireth",
                     std::vector<std::string>(), 0, 0, 0);
        }
        assert(dp.rectForAction(DA_TOGGLE_DEV_ADVANCED, -1, advRc));
    }
    SelectObject(mDC, old);
    DeleteObject(bmp);
    DeleteDC(mDC);
    ReleaseDC(NULL, scDC);

    // 6. U6: D3D11Renderer mode pulse alpha pre-init call
    D3D11Renderer renderer;
    DynamicHudMarkers dm;
    dm.active = true;
    dm.modePulseAlpha = 0.75f;
    dm.modePulseAim = true;
    renderer.setDynamicHudMarkers(dm);

    std::printf("[Test] Sprint 3B (U5/U6/X5/X6/X7/X8/U4/U7) OK!\n");
}

static void testSprint4aMobaAndGamepadV2() {
    std::printf("[Test] Sprint 4A (F5 MOBA/E12 + F6 Gamepad v2 UHID)...\n");

    // 1. F5 + F6 Profile defaults, serialization round-trip & legacy backward-compatibility
    Profile moba = ProfileDefaults::createTemplate(TMPL_MOBA);
    assert(moba.castCancelVk == VK_SPACE);
    assert(moba.buttons.size() >= 5);
    assert(moba.buttons[1].action == NODE_SKILL_CAST);
    assert(std::fabs(moba.buttons[1].radius - 0.110f) < 0.001f);
    assert(std::fabs(moba.buttons[1].x2 - 0.880f) < 0.001f);
    assert(std::fabs(moba.buttons[1].y2 - 0.180f) < 0.001f);
    moba.buttons[1].quickCast = true;
    moba.padMode = PAD_MODE_NATIVE;
    moba.padDeadzoneL = 0.21f;
    moba.padDeadzoneR = 0.29f;
    moba.padLookSens = 1.35f;
    moba.padMap[PAD_A] = "SKILL1";
    moba.padMap[PAD_RT] = "FIRE";

    std::string ser = ProfileStore::serializeToString(moba);
    Profile reloaded;
    assert(ProfileStore::parseFromString(ser, reloaded, nullptr));
    assert(reloaded.castCancelVk == VK_SPACE);
    assert(reloaded.buttons[1].action == NODE_SKILL_CAST);
    assert(reloaded.buttons[1].quickCast == true);
    assert(std::fabs(reloaded.buttons[1].radius - 0.110f) < 0.001f);
    assert(reloaded.padMode == PAD_MODE_NATIVE);
    assert(std::fabs(reloaded.padDeadzoneL - 0.21f) < 0.002f);
    assert(std::fabs(reloaded.padDeadzoneR - 0.29f) < 0.002f);
    assert(std::fabs(reloaded.padLookSens - 1.35f) < 0.002f);
    assert(reloaded.padMap[PAD_A] == "SKILL1");
    assert(reloaded.padMap[PAD_RT] == "FIRE");

    Profile legacy;
    assert(ProfileStore::parseFromString("name = LegacyV0\nkeymap_enabled = 1\n", legacy, nullptr));
    assert(legacy.padMode == PAD_MODE_TOUCH);
    assert(legacy.castCancelVk == 0);

    // 2. EditorGeometry: skill cast cancel key clash flagged
    {
        Profile clashP = moba;
        KeyNode bad;
        bad.label = "BAD";
        bad.vk = VK_SPACE;
        bad.action = NODE_CLICK;
        bad.x = 0.40f; bad.y = 0.80f;
        clashP.buttons.push_back(bad);
        std::vector<KeyConflict> conf = EditorGeometry::detectConflicts(clashP, true);
        bool sawCancelConflict = false;
        for (size_t i = 0; i < conf.size(); ++i) {
            if (conf[i].reason.find("Cancel") != std::string::npos) sawCancelConflict = true;
        }
        assert(sawCancelConflict);
    }

    // 3. F5 ActionDispatcher MOBA skill-cast aim + tick track + cancel + quick-cast
    {
        std::vector<RecordedTouch> log;
        ActionDispatcher ad;
        ad.setTouchSink(recordTouchCb, &log);
        Profile mp = ProfileDefaults::createTemplate(TMPL_MOBA);
        ad.resetForProfile(mp.buttons.size());
        bool held[512];
        std::memset(held, 0, sizeof(held));
        bool scp = false;
        ActionEffects eff;
        uint32_t pkt = 0;

        // Cursor due right from castCenter (0.5, 0.5)
        ad.setCursorNorm(0.80f, 0.50f);
        held['Q'] = true;
        ad.dispatchKey('Q', true, true, mp, held, 1920, 1080, 10.0, scp, eff, pkt);
        assert(log.size() == 2);
        assert(log[0].action == TOUCH_DOWN);
        assert(log[1].action == TOUCH_MOVE);
        int32_t skillX = static_cast<int32_t>(std::floor(mp.buttons[1].x * 1920.0f + 0.5f));
        int32_t skillY = static_cast<int32_t>(std::floor(mp.buttons[1].y * 1080.0f + 0.5f));
        assert(log[1].px > skillX);
        assert(std::abs(log[1].py - skillY) <= 2);

        // Move cursor straight up from center -> tick updates touch upward
        ad.setCursorNorm(0.50f, 0.15f);
        ad.tick(mp, 20.0, pkt);
        assert(log.size() == 3 && log[2].action == TOUCH_MOVE);
        assert(log[2].py < skillY);

        // Release Q casts skill
        held['Q'] = false;
        ad.dispatchKey('Q', false, true, mp, held, 1920, 1080, 30.0, scp, eff, pkt);
        assert(log.size() == 4 && log[3].action == TOUCH_UP);

        // Cancel test: hold Q, cancel to (x2, y2), release Q emits no extra UP
        ad.setCursorNorm(0.80f, 0.50f);
        held['Q'] = true;
        ad.dispatchKey('Q', true, true, mp, held, 1920, 1080, 40.0, scp, eff, pkt);
        assert(ad.hasActiveSkillCast());
        size_t beforeCancel = log.size();
        assert(ad.cancelActiveSkillCasts(mp, 1920, 1080, pkt));
        assert(!ad.hasActiveSkillCast());
        assert(log.size() == beforeCancel + 2);
        assert(log[beforeCancel].action == TOUCH_MOVE);
        assert(std::abs(log[beforeCancel].px - static_cast<int32_t>(0.880f * 1920.0f)) <= 2);
        assert(std::abs(log[beforeCancel].py - static_cast<int32_t>(0.180f * 1080.0f)) <= 2);
        assert(log[beforeCancel + 1].action == TOUCH_UP);
        size_t afterCancel = log.size();
        held['Q'] = false;
        ad.dispatchKey('Q', false, true, mp, held, 1920, 1080, 50.0, scp, eff, pkt);
        assert(log.size() == afterCancel);

        // Quick-cast auto-releases on tick without key release
        mp.buttons[1].quickCast = true;
        held['Q'] = true;
        ad.dispatchKey('Q', true, true, mp, held, 1920, 1080, 100.0, scp, eff, pkt);
        size_t beforeTick = log.size();
        ad.tick(mp, 135.0, pkt);
        assert(log.size() == beforeTick + 1 && log.back().action == TOUCH_UP);
    }

    // 4. E12 Pinch release + F5 Two-Finger Pan in InputEngine (offline headless sync)
    {
        std::vector<RecordedTouch> log;
        ScrcpyBridge br;
        InputEngine eng(&br);
        eng.setTouchSinkForTest(recordTouchCb, &log);
        eng.onDirectCursorPinch(0.5f, 0.5f, 1);
        size_t nPinch = log.size();
        assert(nPinch >= 2);
        log.clear();
        eng.onDirectCursorTouch(TOUCH_DOWN, 0.30f, 0.30f);
        assert(log.size() == 3);
        assert(log[0].action == TOUCH_UP && log[1].action == TOUCH_UP && log[2].action == TOUCH_DOWN);
        assert(std::abs(log[2].px - static_cast<int32_t>(0.30f * 1920.0f)) <= 2);
        assert(std::abs(log[2].py - static_cast<int32_t>(0.30f * 1080.0f)) <= 2);
        eng.onDirectCursorTouch(TOUCH_UP, 0.30f, 0.30f);

        log.clear();
        eng.onDirectCursorTwoFingerPan(TOUCH_DOWN, 0.50f, 0.50f);
        eng.onDirectCursorTwoFingerPan(TOUCH_MOVE, 0.60f, 0.50f);
        eng.onDirectCursorTwoFingerPan(TOUCH_UP,   0.60f, 0.50f);
        assert(log.size() == 6);
        assert(log[0].action == TOUCH_DOWN && log[1].action == TOUCH_DOWN);
        assert(log[2].action == TOUCH_MOVE && log[3].action == TOUCH_MOVE);
        assert(log[4].action == TOUCH_UP   && log[5].action == TOUCH_UP);
    }

    // 5. F6 ScrcpyBridge UHID byte-exact serializers
    {
        uint16_t descSz = 0;
        const uint8_t* desc = GamepadController::hidReportDescriptor(descSz);
        assert(desc != nullptr && descSz == 80);
        assert(desc[0] == 0x05 && desc[descSz - 1] == 0xC0);

        uint8_t buf[256];
        size_t cLen = ScrcpyBridge::serializeUhidCreate(buf, sizeof(buf), 3, 0x045E, 0x028E, "PTPad", desc, descSz);
        assert(cLen == 10u + 5u + descSz);
        assert(buf[0] == 12);
        assert(buf[1] == 0x00 && buf[2] == 0x03);
        assert(buf[3] == 0x04 && buf[4] == 0x5E);
        assert(buf[5] == 0x02 && buf[6] == 0x8E);
        assert(buf[7] == 5 && std::memcmp(buf + 8, "PTPad", 5) == 0);
        assert(buf[13] == 0 && buf[14] == descSz);

        uint8_t rep[15];
        GamepadController::packHidReport(0, 32767, 0, 0, 0, 255, 0x1001, true, rep);
        assert(rep[0] == 0x00 && rep[1] == 0x80); // LX center 0x8000 LE
        assert(rep[2] == 0x01 && rep[3] == 0x00); // LY up (inverted +32767 -> 1)
        assert(rep[10] == 0xFF && rep[11] == 0x7F); // RT full 32767
        assert(rep[12] == 0x01 && rep[13] == 0x10); // A (0x0001) + Guide (0x1000)
        assert(rep[14] == 1); // D-pad Up hat = 1

        size_t iLen = ScrcpyBridge::serializeUhidInput(buf, sizeof(buf), 3, rep, 15);
        assert(iLen == 20 && buf[0] == 13 && buf[3] == 0 && buf[4] == 15);
        size_t dLen = ScrcpyBridge::serializeUhidDestroy(buf, sizeof(buf), 3);
        assert(dLen == 3 && buf[0] == 14 && buf[1] == 0 && buf[2] == 3);
    }

    // 6. F6 GamepadController per-profile map, E11 disable key-up, UHID lifecycle & shell nav
    {
        Profile pNat = moba;
        assert(GamepadController::resolvePadButtonVk(PAD_A, pNat) == 'Q');
        assert(GamepadController::resolvePadButtonVk(PAD_RT, pNat) == pNat.fireVk);

        GamepadController gp;
        std::vector<RecordedTouch> log;
        gp.setTouchSink(recordTouchCb, &log);
        uint32_t pkt = 0;

        GamepadSnapshot snap;
        snap.connected = true;
        snap.buttons = 0x1001; // A + DpadUp
        snap.guidePressed = true;
        snap.hasRawAxes = true;
        snap.rawLy = 10000;

        GamepadFrameOutput out;
        gp.processSnapshot(snap, pNat, 1920, 1080, 4.0, pkt, out, false, true /*shellMenuOpen*/);
        assert(out.uhidActive && out.uhidNeedCreate && out.uhidReportChanged);
        assert((out.shellNavMask & PAD_NAV_GUIDE) != 0);
        assert((out.shellNavMask & PAD_NAV_UP) != 0);

        gp.suspendForFocusLoss(pkt, out);
        assert(out.uhidReportChanged);
        assert(out.uhidReport[2] == 0x00 && out.uhidReport[3] == 0x80); // neutral center

        // Touch mode E11 test: hold A -> disable -> emits key-up
        Profile pTch = pNat;
        pTch.padMode = PAD_MODE_TOUCH;
        gp.processSnapshot(snap, pTch, 1920, 1080, 4.0, pkt, out, false, false);
        assert(out.uhidNeedDestroy);
        assert(out.keyDeltaCount >= 1 && out.keyDeltas[0].vk == 'Q' && out.keyDeltas[0].down == true);
        gp.setEnabled(false);
        gp.pollAndProcess(pTch, 1920, 1080, 4.0, pkt, out, false, false);
        bool sawUpQ = false;
        for (int i = 0; i < out.keyDeltaCount; ++i) {
            if (out.keyDeltas[i].vk == 'Q' && !out.keyDeltas[i].down) sawUpQ = true;
        }
        assert(sawUpQ);
    }

    // 7. DrawerPanel Tab 4 (PAD) + HudRenderer NODE_SKILL_CAST paint smoke
    {
        HDC scDC = GetDC(NULL);
        HDC mDC = CreateCompatibleDC(scDC);
        HBITMAP bmp = CreateCompatibleBitmap(scDC, 1100, 760);
        HGDIOBJ old = SelectObject(mDC, bmp);
        {
            Gdiplus::Graphics g(mDC);
            DrawerPanel dp;
            dp.setActiveTab(4);
            RECT dRc = { 680, 46, 1016, 740 };
            AppSettings as;
            dp.paint(g, dRc, moba, as, "moba.cfg", -1, false, true, "Phone", "com.moba",
                     std::vector<std::string>(), 0, 0, 0);
            RECT rMode = { 0, 0, 0, 0 }, rPadA = { 0, 0, 0, 0 };
            assert(dp.rectForAction(DA_PAD_MODE, 1, rMode));
            assert(dp.rectForAction(DA_PAD_BIND_BTN, PAD_A, rPadA));

            EngineTelemetry tel;
            HudRenderer::paintKeymapNodes(g, 590, 700, moba, tel, true, false, 1, -1, false, false, 1.0f);
        }
        SelectObject(mDC, old);
        DeleteObject(bmp);
        DeleteDC(mDC);
        ReleaseDC(NULL, scDC);
    }

    std::printf("[Test] Sprint 4A (F5 MOBA/E12 + F6 Gamepad v2 UHID) OK!\n");
}

static void testSprint4bInAppAudio() {
    std::printf("[Test] Sprint 4B (F7 In-App WASAPI Audio + MF AAC)...\n");

    // 1. ScrcpyBridge audio codec auto-selection (USB -> raw, Wi-Fi -> aac) & 4cc parsing
    assert(ScrcpyBridge::selectAudioCodec("R5CRC0RS88Y") == "raw");
    assert(ScrcpyBridge::selectAudioCodec("") == "raw");
    assert(ScrcpyBridge::selectAudioCodec("192.168.1.50:5555") == "aac");

    assert(ScrcpyBridge::parseAudioCodecId(0x00726177u) == AUDIO_CODEC_RAW);
    assert(ScrcpyBridge::parseAudioCodecId(0x00616163u) == AUDIO_CODEC_AAC);
    assert(ScrcpyBridge::parseAudioCodecId(0u) == AUDIO_CODEC_NONE);
    assert(ScrcpyBridge::parseAudioCodecId(1u) == AUDIO_CODEC_NONE);
    assert(ScrcpyBridge::parseAudioCodecId(0x6f707573u) == AUDIO_CODEC_NONE);

    // 2. ScrcpyBridge 12-byte audio packet header parser (v4/v5 and v2/v3)
    auto writeHdr = [](uint8_t out[12], uint64_t f, uint32_t sz) {
        for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>((f >> (56 - i * 8)) & 0xFFu);
        out[8]  = static_cast<uint8_t>((sz >> 24) & 0xFFu);
        out[9]  = static_cast<uint8_t>((sz >> 16) & 0xFFu);
        out[10] = static_cast<uint8_t>((sz >> 8) & 0xFFu);
        out[11] = static_cast<uint8_t>(sz & 0xFFu);
    };
    {
        uint8_t hdr[12];
        uint64_t pts = 99;
        uint32_t sz = 0;
        bool cfg = false;
        writeHdr(hdr, (1ULL << 62), 2u);
        assert(ScrcpyBridge::parseAudioPacketHeader(hdr, true, pts, sz, cfg));
        assert(cfg && pts == 0u && sz == 2u);

        writeHdr(hdr, (1ULL << 61) | 1234567ULL, 4096u);
        assert(ScrcpyBridge::parseAudioPacketHeader(hdr, true, pts, sz, cfg));
        assert(!cfg && pts == 1234567ULL && sz == 4096u);

        writeHdr(hdr, (1ULL << 63), 2u);
        assert(ScrcpyBridge::parseAudioPacketHeader(hdr, false, pts, sz, cfg));
        assert(cfg && pts == 0u && sz == 2u);

        writeHdr(hdr, (1ULL << 62) | 987654ULL, 512u);
        assert(ScrcpyBridge::parseAudioPacketHeader(hdr, false, pts, sz, cfg));
        assert(!cfg && pts == 987654ULL && sz == 512u);

        writeHdr(hdr, 0ULL, 0u);
        assert(!ScrcpyBridge::parseAudioPacketHeader(hdr, true, pts, sz, cfg));
        writeHdr(hdr, 0ULL, 2u * 1024u * 1024u);
        assert(!ScrcpyBridge::parseAudioPacketHeader(hdr, true, pts, sz, cfg));
    }

    // 3. AudioDriftRegulator: pre-roll, S16LE conversion, steady-state, +/- drift compensation, overflow & discontinuity
    {
        AudioDriftRegulator reg;
        reg.configure(48000u, 30u);
        assert(reg.targetFrames() == 1440u);

        std::vector<int16_t> s16In(1024u * 2u, 16384);
        std::vector<float> outBuf(480u * 2u, -1.0f);

        reg.pushInterleavedS16(s16In.data(), 1024u, 0ULL);
        assert(!reg.isPrimed());
        assert(!reg.pullInterleavedFloat(outBuf.data(), 480u));
        assert(reg.underflowFrames() == 0u);
        assert(std::fabs(outBuf[0]) < 1e-6f);

        reg.pushInterleavedS16(s16In.data(), 1024u, 21333ULL);
        assert(reg.bufferedFrames() >= 1440u);
        assert(reg.pullInterleavedFloat(outBuf.data(), 480u));
        assert(reg.isPrimed());
        assert(reg.underflowFrames() == 0u);
        // After 16-frame fade-in, sample reaches 0.5f (= 16384 / 32768)
        assert(std::fabs(outBuf[400u * 2u] - 0.5f) < 0.01f);

        // Steady-state streaming over 200 pulls of 480 frames
        reg.configure(48000u, 30u);
        uint64_t produced = 0, consumed = 0, pts = 0;
        for (int k = 0; k < 2; ++k) {
            reg.pushInterleavedS16(s16In.data(), 1024u, pts);
            produced += 1024u;
            pts += 21333ULL;
        }
        for (int cyc = 0; cyc < 200; ++cyc) {
            while (produced < consumed + 1440u) {
                reg.pushInterleavedS16(s16In.data(), 1024u, pts);
                produced += 1024u;
                pts += 21333ULL;
            }
            assert(reg.pullInterleavedFloat(outBuf.data(), 480u));
            consumed += 480u;
        }
        assert(reg.underflowFrames() == 0u);
        assert(reg.droppedFrames() == 0u);

        // Positive clock drift (+1% faster producer): driftRate > 1.0, zero drops
        reg.configure(48000u, 30u);
        produced = 0; consumed = 0; pts = 0;
        for (int k = 0; k < 2; ++k) {
            reg.pushInterleavedS16(s16In.data(), 1024u, pts);
            produced += 1024u;
            pts += 21333ULL;
        }
        for (int cyc = 0; cyc < 300; ++cyc) {
            const uint64_t wantProd = static_cast<uint64_t>(static_cast<double>(consumed) * 1.01) + 1440ULL;
            while (produced < wantProd) {
                reg.pushInterleavedS16(s16In.data(), 1024u, pts);
                produced += 1024u;
                pts += 21333ULL;
            }
            reg.pullInterleavedFloat(outBuf.data(), 480u);
            consumed += 480u;
        }
        assert(reg.currentRate() > 1.0);
        assert(reg.droppedFrames() == 0u);

        // Negative clock drift (-0.8% slower producer): driftRate < 1.0, zero underflow
        reg.configure(48000u, 40u);
        produced = 0; consumed = 0; pts = 0;
        for (int k = 0; k < 2; ++k) {
            reg.pushInterleavedS16(s16In.data(), 1024u, pts);
            produced += 1024u;
            pts += 21333ULL;
        }
        for (int cyc = 0; cyc < 250; ++cyc) {
            const uint64_t wantProd = static_cast<uint64_t>(static_cast<double>(consumed) * 0.992) + 1920ULL;
            while (produced < wantProd) {
                reg.pushInterleavedS16(s16In.data(), 1024u, pts);
                produced += 1024u;
                pts += 21333ULL;
            }
            reg.pullInterleavedFloat(outBuf.data(), 480u);
            consumed += 480u;
        }
        assert(reg.currentRate() < 1.0);
        assert(reg.underflowFrames() == 0u);

        // Hard overflow protection when producer runs unchecked
        reg.configure(48000u, 25u);
        reg.pushInterleavedS16(s16In.data(), 1024u, 0ULL);
        reg.pushInterleavedS16(s16In.data(), 1024u, 21333ULL);
        reg.pullInterleavedFloat(outBuf.data(), 480u); // prime
        for (int k = 0; k < 20; ++k) {
            reg.pushInterleavedS16(s16In.data(), 1024u, static_cast<uint64_t>(k + 2) * 21333ULL);
        }
        assert(reg.droppedFrames() > 0u);
        assert(reg.bufferedFrames() <= 4080u);

        // PTS discontinuity > 100ms inserts target silence and resets drift
        reg.configure(48000u, 30u);
        reg.pushInterleavedS16(s16In.data(), 480u, 0ULL);
        reg.pushInterleavedS16(s16In.data(), 480u, 500000ULL);
        assert(reg.bufferedFrames() >= 1440u);
        assert(reg.currentRate() == 1.0);
    }

    // 4. AAC AudioSpecificConfig parser, MF HEAACWAVEINFO blob & live CMSAACDecMFT probe
    {
        const uint8_t asc48[2] = { 0x11, 0x90 };
        uint32_t sr = 0, ch = 0;
        assert(AudioPlayer::parseAacAudioSpecificConfig(asc48, sizeof(asc48), sr, ch));
        assert(sr == 48000u && ch == 2u);

        const uint8_t asc44[2] = { 0x12, 0x10 };
        assert(AudioPlayer::parseAacAudioSpecificConfig(asc44, sizeof(asc44), sr, ch));
        assert(sr == 44100u && ch == 2u);

        std::vector<uint8_t> ud = AudioPlayer::buildMfAacUserData(asc48, sizeof(asc48));
        assert(ud.size() == 14u);
        assert(ud[0] == 0u && ud[1] == 0u && ud[2] == 0xFEu && ud[3] == 0u);
        assert(ud[12] == 0x11u && ud[13] == 0x90u);

        std::string aacErr;
        assert(AudioPlayer::probeMfAacDecoderInit(asc48, sizeof(asc48), aacErr));
    }

    // 5. ThreadScheduling::enableMmcssAudio + AudioPlayer WASAPI lifecycle smoke test
    {
        DWORD taskIdx = 0;
        HANDLE avrt = ThreadScheduling::enableMmcssAudio(taskIdx);
        ThreadScheduling::disableMmcss(avrt);

        AudioPlayer ap;
        ap.prepareForSession();
        std::string apErr;
        ap.start(AUDIO_CODEC_RAW, 25, apErr);
        std::vector<int16_t> pcm(1024u * 2u, 100);
        ap.onAudioPacket(AUDIO_CODEC_RAW, reinterpret_cast<const uint8_t*>(pcm.data()),
                         pcm.size() * sizeof(int16_t), 0ULL, false);
        AudioStats st = ap.getStats();
        assert(st.packetsReceived == 1u && st.framesPushed == 1024u);
        ap.requestStop();
        ap.stop();
        assert(!ap.isRunning());
    }

    std::printf("[Test] Sprint 4B (F7 In-App WASAPI Audio + MF AAC) OK!\n");
}

static void testSprint5aLanguageAndClipboard() {
    std::printf("[Test] Sprint 5A (F10 IME/Unicode/i18n + F11 Clipboard v2)...\n");

    // 1. ClipboardSyncGuard: loop suppression (E14), Win seq dedup, and B3 non-ASCII clipboard restore
    {
        ClipboardSyncGuard cg;
        assert(!cg.shouldSendPcToPhone("", 1u));
        assert(cg.shouldSendPcToPhone("pc copy 1", 10u));
        assert(!cg.shouldSendPcToPhone("pc copy 1", 10u));  // same Windows seq
        assert(!cg.shouldSendPcToPhone("pc copy 1", 11u));  // same text hash
        assert(!cg.shouldApplyPhoneToPc("pc copy 1", 1000ULL)); // Android echoes PC copy back

        assert(cg.shouldApplyPhoneToPc("phone otp 918273", 1100ULL));
        cg.noteLocalClipboardWrite(12u, "phone otp 918273");
        assert(!cg.shouldSendPcToPhone("phone otp 918273", 12u)); // WM_CLIPBOARDUPDATE same seq
        assert(!cg.shouldSendPcToPhone("phone otp 918273", 13u)); // same hash

        // B3 non-ASCII burst typing preserves initial clipboard & restores after 250ms
        cg.beginNonAsciiTyping("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "keep_this", 2000ULL);
        assert(cg.hasPendingRestore());
        cg.beginNonAsciiTyping("za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87", "should_not_overwrite", 2100ULL);
        assert(!cg.shouldApplyPhoneToPc("za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87", 2150ULL)); // echo suppressed
        std::string restored;
        assert(!cg.pollClipboardRestore(2250ULL, restored));
        assert(cg.pollClipboardRestore(2360ULL, restored));
        assert(restored == "keep_this");
        assert(!cg.hasPendingRestore());
        assert(!cg.shouldApplyPhoneToPc("keep_this", 2400ULL)); // restored clipboard echo suppressed
    }

    // 2. UTF-8 pure-ASCII & multi-script chunk splitting (Polish, Russian, Hindi, Chinese, Emoji)
    {
        assert(ScrcpyBridge::isPureAscii("Hello 123!"));
        const std::string pl = "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87";
        const std::string ru = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
        const std::string hi = "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87";
        const std::string zh = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";
        const std::string em = "\xF0\x9F\x94\xA5\xF0\x9F\x8E\xAE";
        assert(!ScrcpyBridge::isPureAscii(pl));
        assert(!ScrcpyBridge::isPureAscii(ru));
        assert(!ScrcpyBridge::isPureAscii(hi));
        assert(!ScrcpyBridge::isPureAscii(zh));
        assert(!ScrcpyBridge::isPureAscii(em));
        std::vector<std::string> chunks = ScrcpyBridge::splitUtf8Chunks(hi + zh + em, 5);
        std::string rejoined;
        for (size_t i = 0; i < chunks.size(); ++i) rejoined += chunks[i];
        assert(rejoined == hi + zh + em);
    }

    // 3. Strings catalog parser & all 6 live lang/*.txt files (no rebuild switching)
    {
        std::map<std::string, std::string> pm;
        const std::string bomSample = "\xEF\xBB\xBF# Comment line\n  tab.keys = CLEF  \r\nsec.input=ENTR\xC3\x89" "E\n";
        Strings::parseCatalogText(bomSample, pm);
        assert(pm["tab.keys"] == "CLEF");
        assert(pm["sec.input"] == "ENTR\xC3\x89" "E");

        Strings::init("lang", "en");
        const std::vector<LanguageInfo>& langs = Strings::languages();
        assert(langs.size() == 6u);
        for (size_t i = 0; i < langs.size(); ++i) {
            assert(Strings::setLanguage(langs[i].code));
            assert(Strings::currentLanguage() == langs[i].code);
            assert(!Strings::tr("tab.keys", "KEYS").empty());
            assert(Strings::tr("nonexistent.key", "FallbackEn") == "FallbackEn");
        }
        assert(Strings::setLanguage("hi"));
        assert(Strings::tr("sec.language", "LANGUAGE") == "\xE0\xA4\xAD\xE0\xA4\xBE\xE0\xA4\xB7\xE0\xA4\xBE");
        assert(Strings::setLanguage("ru"));
        assert(Strings::tr("sec.language", "LANGUAGE") == "\xD0\xAF\xD0\x97\xD0\xAB\xD0\x9A");
        assert(Strings::setLanguage("en"));
        assert(Strings::tr("sec.language", "LANGUAGE") == "LANGUAGE");
    }

    // 4. Script font fallback detection + offscreen GDI+ draw smoke across all 4 scripts
    {
        assert(std::string(GdiPlusDraw::detectFontFamilyForText("Phantom Touch")) == "Segoe UI");
        assert(std::string(GdiPlusDraw::detectFontFamilyForText("za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87")) == "Segoe UI");
        assert(std::string(GdiPlusDraw::detectFontFamilyForText("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82")) == "Segoe UI");
        assert(std::string(GdiPlusDraw::detectFontFamilyForText("\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87")) == "Nirmala UI");
        assert(std::string(GdiPlusDraw::detectFontFamilyForText("\xE4\xBD\xA0\xE5\xA5\xBD")) == "Microsoft YaHei UI");

        HDC scDC = GetDC(NULL);
        HDC mDC = CreateCompatibleDC(scDC);
        HBITMAP bmp = CreateCompatibleBitmap(scDC, 320, 120);
        HGDIOBJ old = SelectObject(mDC, bmp);
        {
            Gdiplus::Graphics g(mDC);
            GdiPlusDraw::textLeft(g, "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87", 10.0f, 20.0f, 12.0f, false, 0xFFFFFFFFu);
            GdiPlusDraw::textLeft(g, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", 10.0f, 45.0f, 12.0f, false, 0xFFFFFFFFu);
            GdiPlusDraw::textLeft(g, "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87", 10.0f, 70.0f, 12.0f, false, 0xFFFFFFFFu);
            GdiPlusDraw::textLeft(g, "\xE4\xBD\xA0\xE5\xA5\xBD", 10.0f, 95.0f, 12.0f, false, 0xFFFFFFFFu);
        }
        SelectObject(mDC, old);
        DeleteObject(bmp);
        DeleteDC(mDC);
        ReleaseDC(NULL, scDC);
    }

    // 5. AppSettingsStore round-trip + pending change detection for clipboardSync & uiLanguage
    {
        AppSettings a1;
        a1.clipboardSync = false;
        a1.clipboardNoticeShown = true;
        a1.uiLanguage = "pt-BR";
        const std::string tmpCfg = "profiles\\_test_sprint5a_settings.cfg";
        assert(AppSettingsStore::save(tmpCfg, a1));
        AppSettings a2;
        assert(AppSettingsStore::load(tmpCfg, a2));
        std::remove(tmpCfg.c_str());
        assert(a2.clipboardSync == false);
        assert(a2.clipboardNoticeShown == true);
        assert(a2.uiLanguage == "pt-BR");

        AppSettings base;
        assert(AppSettings::countPendingStreamChanges(a2, base, "") >= 1);
    }

    // 6. DrawerPanel DEV tab localization render + DA_TOGGLE_CLIPBOARD_SYNC & DA_SET_LANGUAGE actions
    {
        DrawerPanel dp;
        dp.setActiveTab(3); // DEV
        Profile p = ProfileDefaults::createDesktop("Desktop");
        AppSettings s;
        RECT bounds = { 0, 0, 356, 1600 };
        std::vector<std::string> logs;

        HDC scDC = GetDC(NULL);
        HDC mDC = CreateCompatibleDC(scDC);
        HBITMAP bmp = CreateCompatibleBitmap(scDC, 356, 1600);
        HGDIOBJ old = SelectObject(mDC, bmp);
        {
            Gdiplus::Graphics g(mDC);
            Strings::setLanguage("hi");
            dp.paint(g, bounds, p, s, "", -1, false, true, "SM-G990E", "", logs, -1, -1, 0);
            Strings::setLanguage("ru");
            dp.paint(g, bounds, p, s, "", -1, false, true, "SM-G990E", "", logs, -1, -1, 0);
        }

        RECT rcClip = {0, 0, 0, 0};
        assert(dp.rectForAction(DA_TOGGLE_CLIPBOARD_SYNC, -1, rcClip));
        int sel = -1;
        DrawerActionResult res;
        assert(dp.onMouseDown((rcClip.left + rcClip.right) / 2, (rcClip.top + rcClip.bottom) / 2, p, s, sel, res));
        assert(s.clipboardSync == false && res.settingsModified);

        RECT rcHi = {0, 0, 0, 0};
        assert(dp.rectForAction(DA_SET_LANGUAGE, 4, rcHi));
        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcHi.left + rcHi.right) / 2, (rcHi.top + rcHi.bottom) / 2, p, s, sel, res));
        assert(s.uiLanguage == "hi" && res.languageChanged && Strings::currentLanguage() == "hi");

        Strings::setLanguage("en");
        SelectObject(mDC, old);
        DeleteObject(bmp);
        DeleteDC(mDC);
        ReleaseDC(NULL, scDC);
    }

    {
        D3D11Renderer r;
        assert(r.lastPresentTick() == 0);
        r.markStreamActivated();
        assert(r.lastPresentTick() != 0);
        ViewportRect vr;
        vr.x = 0; vr.y = 0; vr.w = 64; vr.h = 64;
        r.tryRender(vr, false);
    }

    std::printf("[Test] Sprint 5A (F10 IME/Unicode/i18n + F11 Clipboard v2) OK!\n");
}

void testSprint5bAppsFilesAndShortcuts() {
    std::printf("[Test] Sprint 5B (F8 Apps/2ndPhone + F9 Files/CF_DIB)...\n");

    // 1. ScrcpyBridge::parseAppListOutput (system '*', user '-', wrapped >30-char app name)
    {
        const std::string fixture =
            "[server] INFO: Device: [samsung] samsung SM-G990E (Android 16)\n"
            "[server] INFO: Processing Android apps... (this may take some time)\n"
            "[server] INFO: List of apps:\n"
            " * Calendar                       com.samsung.android.calendar\n"
            " * Live Transcribe & Sound Notifications\n"
            "                                  com.google.audio.hearing.visualization.accessibility.scribe\n"
            " * Maps                           com.google.android.apps.maps\n"
            " - Free Fire MAX                  com.dts.freefiremax\n"
            " - WhatsApp                       com.whatsapp\n";
        std::vector<DeviceAppInfo> apps = ScrcpyBridge::parseAppListOutput(fixture);
        assert(apps.size() == 5);
        assert(apps[0].isSystem && apps[0].name == "Calendar" && apps[0].packageName == "com.samsung.android.calendar");
        assert(apps[1].isSystem && apps[1].name == "Live Transcribe & Sound Notifications");
        assert(apps[1].packageName == "com.google.audio.hearing.visualization.accessibility.scribe");
        assert(apps[2].isSystem && apps[2].name == "Maps" && apps[2].packageName == "com.google.android.apps.maps");
        assert(!apps[3].isSystem && apps[3].name == "Free Fire MAX" && apps[3].packageName == "com.dts.freefiremax");
        assert(!apps[4].isSystem && apps[4].name == "WhatsApp" && apps[4].packageName == "com.whatsapp");
    }

    // 2. LaunchOptions CLI --app=<package> and --app <package> parsing
    {
        const char* argv1[] = { "PhantomTouch.exe", "--app=com.whatsapp", "--probe-on-connect", "--probe-samples=12" };
        LaunchOptions o1 = parseLaunchOptions(4, argv1);
        assert(o1.launchAppPackage == "com.whatsapp");
        assert(o1.probeOnConnect && o1.probeSamples == 12);

        const char* argv2[] = { "PhantomTouch.exe", "--app", "com.dts.freefiremax;rm -rf /", "--console" };
        LaunchOptions o2 = parseLaunchOptions(4, argv2);
        assert(o2.launchAppPackage == "com.dts.freefiremaxrm-rf");
        assert(o2.showConsole);
    }

    // 3. AppSettings pinned_apps persistence + togglePinApp
    {
        AppSettings a1;
        a1.togglePinApp("com.whatsapp");
        a1.togglePinApp("com.dts.freefiremax");
        assert(a1.isAppPinned("com.whatsapp") && a1.isAppPinned("com.dts.freefiremax"));
        const std::string tmpCfg = "profiles\\_test_sprint5b_settings.cfg";
        assert(AppSettingsStore::save(tmpCfg, a1));
        AppSettings a2;
        assert(AppSettingsStore::load(tmpCfg, a2));
        std::remove(tmpCfg.c_str());
        assert(a2.pinnedApps.size() == 2);
        assert(a2.isAppPinned("com.whatsapp") && a2.isAppPinned("com.dts.freefiremax"));
        a2.togglePinApp("com.whatsapp");
        assert(!a2.isAppPinned("com.whatsapp") && a2.pinnedApps.size() == 1);
    }

    // 4. DesktopShortcut::createAppShortcutFile writes a real Windows .lnk in %TEMP%
    {
        assert(DesktopShortcut::sanitizeTitle("App: <Test>/1?", "com.example.app") == "App   Test  1");
        wchar_t tempDir[MAX_PATH] = {0};
        DWORD n = GetTempPathW(MAX_PATH, tempDir);
        assert(n > 0);
        int u8n = WideCharToMultiByte(CP_UTF8, 0, tempDir, -1, NULL, 0, NULL, NULL);
        std::string tempUtf8(static_cast<size_t>(u8n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, tempDir, -1, &tempUtf8[0], u8n - 1, NULL, NULL);
        const std::string lnkPath = tempUtf8 + "pt_test_5b_app.lnk";
        std::string err;
        assert(DesktopShortcut::createAppShortcutFile(
            lnkPath, "C:\\Windows\\System32\\notepad.exe", "C:\\Windows\\System32",
            "WhatsApp", "com.whatsapp", err));
        WIN32_FILE_ATTRIBUTE_DATA fad;
        assert(GetFileAttributesExA(lnkPath.c_str(), GetFileExInfoStandard, &fad));
        assert(fad.nFileSizeLow > 64);
        DeleteFileA(lnkPath.c_str());
    }

    // 5. ScrcpyBridge::serializeScanFile (type 22 + 4B BE length + UTF-8 path, <= 256 bytes)
    {
        uint8_t buf[300];
        const std::string path = "/sdcard/Download/photo_caf\xC3\xA9.jpg";
        size_t n = ScrcpyBridge::serializeScanFile(buf, sizeof(buf), path);
        assert(n == 5 + path.size());
        assert(buf[0] == 22);
        uint32_t beLen = (static_cast<uint32_t>(buf[1]) << 24) |
                         (static_cast<uint32_t>(buf[2]) << 16) |
                         (static_cast<uint32_t>(buf[3]) << 8) |
                         static_cast<uint32_t>(buf[4]);
        assert(beLen == path.size());
        assert(std::memcmp(buf + 5, path.data(), path.size()) == 0);

        std::string longUtf8(255, 'a');
        longUtf8 += "\xC3\xA9"; // 2-byte char crossing 256-byte cap -> truncates before \xC3
        size_t nCap = ScrcpyBridge::serializeScanFile(buf, sizeof(buf), longUtf8);
        assert(nCap == 5 + 255);
    }

    // 6. ScrcpyBridge::parseLsLaOutput + shellSingleQuote (UTF-8, spaces, symlinks, dirs-first sorting)
    {
        assert(ScrcpyBridge::shellSingleQuote("/sdcard/a'b dir") == "'/sdcard/a'\\''b dir'");
        const std::string lsRaw =
            "total 42\n"
            "drwxrws--- 2 u0_a291 media_rw 3452 2026-02-22 21:14 .\n"
            "drwxrws--- 2 u0_a291 media_rw 3452 2026-02-22 21:14 ..\n"
            "-rwxrwx--- 1 u0_a291 media_rw 14694693 2026-06-15 13:37 uptodown file (1).apk\n"
            "-rw-rw---- 1 u0_a291 media_rw 128 2026-11-27 05:57 pt_test_caf\xC3\xA9_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt\n"
            "drwxrws--- 5 u0_a291 media_rw 3452 2026-06-14 17:56 DCIM\n"
            "lrwxrwxrwx 1 root root 11 2026-01-01 00:00 LinkFolder -> /sdcard/Download\n";
        std::vector<RemoteFileEntry> entries = ScrcpyBridge::parseLsLaOutput(lsRaw, "/sdcard");
        assert(entries.size() == 4);
        assert(entries[0].isDir && entries[0].name == "DCIM" && entries[0].fullPath == "/sdcard/DCIM");
        assert(entries[1].isSymlink && entries[1].name == "LinkFolder" && entries[1].fullPath == "/sdcard/LinkFolder");
        assert(!entries[2].isDir && entries[2].name == "pt_test_caf\xC3\xA9_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt" && entries[2].sizeBytes == 128);
        assert(!entries[3].isDir && entries[3].name == "uptodown file (1).apk" && entries[3].sizeBytes == 14694693u);
    }

    // 7. D3D11Renderer::buildDibFromBgra CF_DIB bottom-up header & row flip
    {
        uint8_t bgra[2 * 2 * 4] = {
            10, 20, 30, 0,   40, 50, 60, 0,   // Top row
            70, 80, 90, 0,   11, 22, 33, 128  // Bottom row
        };
        std::vector<uint8_t> dib;
        assert(D3D11Renderer::buildDibFromBgra(bgra, 2, 2, dib));
        assert(dib.size() == sizeof(BITMAPINFOHEADER) + 16);
        const BITMAPINFOHEADER* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
        assert(bih->biSize == sizeof(BITMAPINFOHEADER) && bih->biWidth == 2 && bih->biHeight == 2 && bih->biBitCount == 32);
        const uint8_t* px = dib.data() + sizeof(BITMAPINFOHEADER);
        assert(px[0] == 70 && px[1] == 80 && px[2] == 90 && px[3] == 255); // Bottom row first in DIB
        assert(px[8] == 10 && px[9] == 20 && px[10] == 30 && px[11] == 255);
    }

    // 8. DrawerPanel Tab 5 (APPS) & Tab 6 (FILES) render and actions
    {
        DrawerPanel dp;
        Profile p = ProfileDefaults::createDesktop("Desktop");
        AppSettings s;
        RECT bounds = { 0, 0, 356, 780 };
        std::vector<std::string> logs;

        std::vector<DeviceAppInfo> apps;
        DeviceAppInfo a1; a1.name = "WhatsApp"; a1.packageName = "com.whatsapp"; a1.isSystem = false;
        apps.push_back(a1);
        dp.setInstalledApps(apps, false, "1 app loaded");

        std::vector<RemoteFileEntry> files;
        RemoteFileEntry f1; f1.name = "photo.jpg"; f1.fullPath = "/sdcard/Download/photo.jpg"; f1.sizeBytes = 2048; f1.modified = "2026-11-27 06:00";
        files.push_back(f1);
        dp.setRemoteFiles("/sdcard/Download", files, false, "1 item");

        HDC scDC = GetDC(NULL);
        HDC mDC = CreateCompatibleDC(scDC);
        HBITMAP bmp = CreateCompatibleBitmap(scDC, 356, 780);
        HGDIOBJ old = SelectObject(mDC, bmp);
        {
            Gdiplus::Graphics g(mDC);
            dp.setActiveTab(5);
            dp.paint(g, bounds, p, s, "", -1, false, true, "SM-G990E", "", logs, -1, -1, 0);
        }

        RECT rcPin = {0,0,0,0}, rcSec = {0,0,0,0}, rcLnk = {0,0,0,0};
        assert(dp.rectForAction(DA_APP_TOGGLE_PIN, 0, rcPin));
        assert(dp.rectForAction(DA_APP_OPEN_SECOND_PHONE, 0, rcSec));
        assert(dp.rectForAction(DA_APP_CREATE_SHORTCUT, 0, rcLnk));

        int sel = -1;
        DrawerActionResult res;
        assert(dp.onMouseDown((rcPin.left + rcPin.right) / 2, (rcPin.top + rcPin.bottom) / 2, p, s, sel, res));
        assert(s.isAppPinned("com.whatsapp") && res.settingsModified);

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcSec.left + rcSec.right) / 2, (rcSec.top + rcSec.bottom) / 2, p, s, sel, res));
        assert(res.requestOpenAppSecondPhone && res.appPackage == "com.whatsapp");

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcLnk.left + rcLnk.right) / 2, (rcLnk.top + rcLnk.bottom) / 2, p, s, sel, res));
        assert(res.requestCreateAppShortcut && res.appPackage == "com.whatsapp");

        {
            Gdiplus::Graphics g(mDC);
            dp.setActiveTab(6);
            dp.paint(g, bounds, p, s, "", -1, false, true, "SM-G990E", "", logs, -1, -1, 0);
        }
        RECT rcPush = {0,0,0,0}, rcPull = {0,0,0,0};
        assert(dp.rectForAction(DA_FILES_PUSH_PICKER, -1, rcPush));
        assert(dp.rectForAction(DA_FILE_PULL, 0, rcPull));

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcPush.left + rcPush.right) / 2, (rcPush.top + rcPush.bottom) / 2, p, s, sel, res));
        assert(res.requestPushFilePicker && res.pushTargetDir == "/sdcard/Download");

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcPull.left + rcPull.right) / 2, (rcPull.top + rcPull.bottom) / 2, p, s, sel, res));
        assert(res.requestPullRemoteFile && res.pullRemotePath == "/sdcard/Download/photo.jpg");

        SelectObject(mDC, old);
        DeleteObject(bmp);
        DeleteDC(mDC);
        ReleaseDC(NULL, scDC);
    }

    std::printf("[Test] Sprint 5B (F8 Apps/2ndPhone + F9 Files/CF_DIB) OK!\n");
}

static void testSprint6aCreatorAndDeviceControls() {
    std::printf("[Test] Sprint 6A (F12 Recorder/Replay + F13 CleanFeed/Streamer + F14 Controls)...\n");

    // 1. F14 R-msg wire byte verification & B1 show_touches safety
    {
        uint8_t b[8] = {0};
        assert(ScrcpyBridge::serializeBackOrScreenOn(b, sizeof(b), 0) == 2 && b[0] == 4 && b[1] == 0);
        assert(ScrcpyBridge::serializeBackOrScreenOn(b, sizeof(b), 1) == 2 && b[0] == 4 && b[1] == 1);
        assert(ScrcpyBridge::serializeBackOrScreenOn(b, 1, 0) == 0);
        assert(ScrcpyBridge::serializeExpandNotificationPanel(b, sizeof(b)) == 1 && b[0] == 5);
        assert(ScrcpyBridge::serializeExpandSettingsPanel(b, sizeof(b)) == 1 && b[0] == 6);
        assert(ScrcpyBridge::serializeCollapsePanels(b, sizeof(b)) == 1 && b[0] == 7);
        assert(ScrcpyBridge::serializeRotateDevice(b, sizeof(b)) == 1 && b[0] == 11);
        assert(ScrcpyBridge::serializeResetVideo(b, sizeof(b)) == 1 && b[0] == 17);

        assert(ScrcpyBridge::buildShowTouchesServerArg(false, true).empty());
        assert(ScrcpyBridge::buildShowTouchesServerArg(true, false).empty());
        assert(ScrcpyBridge::buildShowTouchesServerArg(true, true) == " show_touches=true");
    }

    // 2. F12 ReplayRingBuffer multi-GOP windowing & keyframe alignment
    const uint8_t kSpsPps1280x720[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F,
        0xD9, 0x00, 0x50, 0x05, 0xBB, 0x01, 0x10, 0x00,
        0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03,
        0xC0, 0xF1, 0x83, 0x19, 0x60,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80
    };
    const uint8_t kIdrSlice[] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xA0, 0x11, 0x22, 0x33 };
    const uint8_t kPSlice[]   = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x21, 0x00, 0x44, 0x55, 0x66 };
    {
        ReplayRingBuffer ring;
        ring.configure(5, 16u * 1024u * 1024u);
        ring.pushVideo(kSpsPps1280x720, sizeof(kSpsPps1280x720), 0, true, false, 1280, 720);

        // Push 3 GOPs across t=0s..12s (keyframes at 0s, 4s, 8s)
        for (int g = 0; g < 3; ++g) {
            uint64_t gPts = static_cast<uint64_t>(g) * 4000000ULL;
            ring.pushVideo(kIdrSlice, sizeof(kIdrSlice), gPts, false, true, 1280, 720);
            ring.pushVideo(kPSlice, sizeof(kPSlice), gPts + 500000ULL, false, false, 1280, 720);
            std::vector<uint8_t> pcm(3840, 0); // 20ms @ 48kHz stereo S16
            ring.pushAudio(AUDIO_CODEC_RAW, pcm.data(), pcm.size(), gPts + 20000ULL, false);
        }
        ring.pushVideo(kPSlice, sizeof(kPSlice), 11500000ULL, false, false, 1280, 720);

        ReplaySnapshot snap;
        assert(ring.buildSnapshot(snap));
        assert(snap.width == 1280 && snap.height == 720);
        assert(!snap.videoConfig.empty());
        assert(snap.packets.size() >= 3);
        assert(snap.packets.front().isVideo && snap.packets.front().isKeyFrame);
        assert(snap.packets.front().ptsUs >= 4000000ULL);
    }

    // 3. F12 Media Foundation SinkWriter MP4 passthrough + PCM->AAC mux + SourceReader verify
    {
        ReplaySnapshot snap;
        snap.width = 1280;
        snap.height = 720;
        snap.videoConfig.assign(kSpsPps1280x720, kSpsPps1280x720 + sizeof(kSpsPps1280x720));
        snap.audioCodec = AUDIO_CODEC_RAW;

        for (int f = 0; f < 10; ++f) {
            RecordedPacket vp;
            vp.isVideo = true;
            vp.ptsUs = static_cast<uint64_t>(f) * 33333ULL;
            vp.isKeyFrame = (f == 0 || f == 5);
            const uint8_t* src = vp.isKeyFrame ? kIdrSlice : kPSlice;
            size_t slen = vp.isKeyFrame ? sizeof(kIdrSlice) : sizeof(kPSlice);
            vp.data.assign(src, src + slen);
            snap.packets.push_back(std::move(vp));

            RecordedPacket ap;
            ap.isVideo = false;
            ap.audioCodec = AUDIO_CODEC_RAW;
            ap.ptsUs = static_cast<uint64_t>(f) * 33333ULL;
            ap.data.assign(6400, 0);
            snap.packets.push_back(std::move(ap));
        }

        char tmpBuf[MAX_PATH] = {0};
        DWORD tlen = GetTempPathA(MAX_PATH, tmpBuf);
        assert(tlen > 0 && tlen < MAX_PATH);
        std::string testMp4 = std::string(tmpBuf) + "pt_sprint6a_test.mp4";
        DeleteFileA(testMp4.c_str());

        RecordingResult rr = Recorder::writeSnapshotToFile(snap, testMp4);
        std::printf("[Test] Sprint 6A MP4 mux: ok=%d fb=%d hasAud=%d vf=%u ap=%u err='%s'\n",
                    rr.ok ? 1 : 0, rr.usedH264Fallback ? 1 : 0, rr.hasAudio ? 1 : 0,
                    rr.videoFramesWritten, rr.audioPacketsWritten, rr.error.c_str());
        assert(rr.ok && !rr.usedH264Fallback);
        assert(rr.videoFramesWritten >= 8);
        assert(!rr.outputPath.empty());
        assert(GetFileAttributesA(rr.outputPath.c_str()) != INVALID_FILE_ATTRIBUTES);

        bool hasVid = false, hasAud = false;
        uint64_t dur100 = 0;
        uint32_t vidSamples = 0;
        assert(Recorder::verifyMp4Playable(rr.outputPath, hasVid, hasAud, dur100, &vidSamples));
        assert(hasVid && hasAud && vidSamples >= 5);
        DeleteFileA(rr.outputPath.c_str());
        DeleteFileA(testMp4.c_str());
    }

    // 4. AppSettings F12/F13 persistence + pending change detection
    {
        char tmpBuf[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmpBuf);
        std::string cfgPath = std::string(tmpBuf) + "pt_sprint6a_settings.cfg";
        DeleteFileA(cfgPath.c_str());

        AppSettings s;
        s.replayBufferEnabled = false;
        s.replayBufferSec = 60;
        s.streamerMode = true;
        s.streamerShowTouches = true;
        s.streamerKeypressOverlay = true;
        assert(AppSettingsStore::save(cfgPath, s));

        AppSettings loaded;
        assert(AppSettingsStore::load(cfgPath, loaded));
        assert(!loaded.replayBufferEnabled);
        assert(loaded.replayBufferSec == 60);
        assert(loaded.streamerMode);
        assert(loaded.streamerShowTouches);
        assert(loaded.streamerKeypressOverlay);

        AppSettings applied = loaded;
        applied.streamerShowTouches = false;
        assert(AppSettings::countPendingStreamChanges(loaded, applied, "SER1") == 1);
        DeleteFileA(cfgPath.c_str());
    }

    // 5. DrawerPanel DEV tab F12/F13/F14 hit-test & action dispatch
    {
        DrawerPanel dp;
        dp.setCreatorState(false, 0, false, 12.0);
        Profile p = ProfileDefaults::createFreeFire();
        AppSettings s;
        std::vector<std::string> logs;
        RECT bounds = { 0, 0, 356, 1800 };

        HDC scDC = GetDC(NULL);
        HDC mDC = CreateCompatibleDC(scDC);
        HBITMAP bmp = CreateCompatibleBitmap(scDC, 356, 1800);
        HGDIOBJ old = SelectObject(mDC, bmp);
        {
            Gdiplus::Graphics g(mDC);
            dp.setActiveTab(3);
            dp.paint(g, bounds, p, s, "", -1, false, true, "SM-G990E", "", logs, -1, -1, 0);
        }
        RECT rcRec = {0,0,0,0}, rcRep = {0,0,0,0}, rcCf = {0,0,0,0}, rcRot = {0,0,0,0}, rcNotif = {0,0,0,0}, rcPow = {0,0,0,0};
        assert(dp.rectForAction(DA_TOGGLE_RECORD, -1, rcRec));
        assert(dp.rectForAction(DA_SAVE_REPLAY, -1, rcRep));
        assert(dp.rectForAction(DA_TOGGLE_CLEAN_FEED, -1, rcCf));
        assert(dp.rectForAction(DA_DEV_ROTATE, -1, rcRot));
        assert(dp.rectForAction(DA_DEV_NOTIFS, -1, rcNotif));
        assert(dp.rectForAction(DA_DEV_POWER, -1, rcPow));

        int sel = -1;
        DrawerActionResult res;
        assert(dp.onMouseDown((rcRec.left + rcRec.right) / 2, (rcRec.top + rcRec.bottom) / 2, p, s, sel, res));
        assert(res.requestToggleRecord);

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcRep.left + rcRep.right) / 2, (rcRep.top + rcRep.bottom) / 2, p, s, sel, res));
        assert(res.requestSaveReplay);

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcCf.left + rcCf.right) / 2, (rcCf.top + rcCf.bottom) / 2, p, s, sel, res));
        assert(res.requestToggleCleanFeed);

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcRot.left + rcRot.right) / 2, (rcRot.top + rcRot.bottom) / 2, p, s, sel, res));
        assert(res.requestDeviceControl == 1);

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcNotif.left + rcNotif.right) / 2, (rcNotif.top + rcNotif.bottom) / 2, p, s, sel, res));
        assert(res.requestDeviceControl == 2);

        res = DrawerActionResult{};
        assert(dp.onMouseDown((rcPow.left + rcPow.right) / 2, (rcPow.top + rcPow.bottom) / 2, p, s, sel, res));
        assert(res.requestDeviceControl == 6);

        SelectObject(mDC, old);
        DeleteObject(bmp);
        DeleteDC(mDC);
        ReleaseDC(NULL, scDC);
    }

    std::printf("[Test] Sprint 6A (F12/F13/F14) OK!\n");
}

static void testSprint6bMultiDeviceSessions() {
    // 1. Unique per-tab ADB forward port allocator (27199..27202) & setter
    {
        std::vector<int> used;
        assert(ScrcpyBridge::allocateUniqueForwardPort(used) == 27199);
        used.push_back(27199);
        assert(ScrcpyBridge::allocateUniqueForwardPort(used) == 27200);
        used.push_back(27201);
        // Reuses lowest free gap (27200)
        assert(ScrcpyBridge::allocateUniqueForwardPort(used) == 27200);
        used.push_back(27200);
        assert(ScrcpyBridge::allocateUniqueForwardPort(used) == 27202);

        ScrcpyBridge b0, b1;
        b0.setForwardPort(27199);
        b1.setForwardPort(27200);
        assert(b0.forwardPort() == 27199);
        assert(b1.forwardPort() == 27200);
    }

    // 2. Strict P7 single-focus InputEngine::setBridge(): releases active touches & never broadcasts
    {
        ScrcpyBridge b0, b1;
        b0.setForwardPort(27199);
        b1.setForwardPort(27200);
        InputEngine eng(&b0);
        assert(eng.activeBridge() == &b0);

        std::vector<RecordedTouch> touchLog;
        eng.setTouchSinkForTest(recordTouchCb, &touchLog);
        eng.setProfile(ProfileDefaults::createFreeFire());
        eng.start();
        eng.setAimMode(false);
        eng.onDirectCursorTouch(TOUCH_DOWN, 0.50f, 0.50f);
        Sleep(20);
        const size_t beforeSwitchCount = touchLog.size();
        assert(beforeSwitchCount >= 1);
        assert(touchLog.back().action == TOUCH_DOWN);

        // Switching focus to phone 2 synchronously releases phone 1 touches & routes only to b1
        eng.setBridge(&b1);
        assert(eng.activeBridge() == &b1);
        assert(touchLog.size() > beforeSwitchCount);
        bool sawRelease = false;
        for (size_t i = beforeSwitchCount; i < touchLog.size(); ++i) {
            if (touchLog[i].action == TOUCH_UP) {
                sawRelease = true;
                break;
            }
        }
        assert(sawRelease);
        eng.stop();
    }

    // 3. SessionTabBar 2-tab layout/hit-test & 4-tab hard cap (F15)
    {
        HDC scDC = GetDC(NULL);
        HDC mDC = CreateCompatibleDC(scDC);
        HBITMAP bmp = CreateCompatibleBitmap(scDC, 1280, 720);
        HGDIOBJ old = SelectObject(mDC, bmp);
        Gdiplus::Graphics g(mDC);

        SessionTabBar bar;
        std::vector<SessionTabInfo> tabs2;
        SessionTabInfo t0;
        t0.index = 0; t0.title = "Galaxy S21 FE"; t0.subtitle = "R5CRC0RS88Y:27199";
        t0.active = true; t0.connected = true; t0.canClose = true;
        SessionTabInfo t1;
        t1.index = 1; t1.title = "Pixel 8 Pro [2ND]"; t1.subtitle = "USB/WiFi:27200";
        t1.active = false; t1.connecting = true; t1.canClose = true;
        tabs2.push_back(t0);
        tabs2.push_back(t1);

        RECT barRc = { 0, 40, 1226, 70 };
        bar.paint(g, barRc, tabs2, true, -1, -1);
        assert(bar.tabCount() == 2);

        RECT rTab0 = bar.tabRect(0);
        RECT rTab1 = bar.tabRect(1);
        RECT rAdd = bar.addButtonRect();
        assert(rTab0.right > rTab0.left && rTab1.right > rTab1.left && rAdd.right > rAdd.left);

        SessionTabHit h0 = bar.hitTest(rTab0.left + 14, (rTab0.top + rTab0.bottom) / 2);
        assert(h0.action == STAB_SELECT && h0.tabIndex == 0);
        SessionTabHit h1 = bar.hitTest(rTab1.left + 14, (rTab1.top + rTab1.bottom) / 2);
        assert(h1.action == STAB_SELECT && h1.tabIndex == 1);
        SessionTabHit hc = bar.hitTest(rTab1.right - 10, (rTab1.top + rTab1.bottom) / 2);
        assert(hc.action == STAB_CLOSE && hc.tabIndex == 1);
        SessionTabHit ha = bar.hitTest((rAdd.left + rAdd.right) / 2, (rAdd.top + rAdd.bottom) / 2);
        assert(ha.action == STAB_ADD);
        assert(!bar.tooltipAt((rAdd.left + rAdd.right) / 2, (rAdd.top + rAdd.bottom) / 2).empty());

        // 4-tab cap hides '+' button
        std::vector<SessionTabInfo> tabs4 = tabs2;
        SessionTabInfo t2 = t0; t2.index = 2; t2.active = false;
        SessionTabInfo t3 = t1; t3.index = 3; t3.active = false;
        tabs4.push_back(t2);
        tabs4.push_back(t3);
        bar.paint(g, barRc, tabs4, false, -1, -1);
        assert(bar.tabCount() == 4);
        assert(bar.addButtonRect().right <= bar.addButtonRect().left);

        // 4. DrawerPanel DEV tab ADD PHONE TAB button + LaunchOptions --multi-tab-test
        DrawerPanel dp;
        dp.setActiveTab(3);
        Profile p = ProfileDefaults::createDesktop();
        AppSettings s;
        std::vector<std::string> logs;
        RECT drc = { 0, 0, 356, 1200 };
        dp.paint(g, drc, p, s, "", -1, false, true, "TestPhone", "", logs, -1, -1, 0);
        RECT rcAddTab = { 0, 0, 0, 0 };
        assert(dp.rectForAction(DA_ADD_PHONE_TAB, -1, rcAddTab));
        int sel = -1;
        DrawerActionResult res;
        assert(dp.onMouseDown((rcAddTab.left + rcAddTab.right) / 2,
                              (rcAddTab.top + rcAddTab.bottom) / 2, p, s, sel, res));
        assert(res.requestAddPhoneTab);

        const char* argvMt[] = { "PhantomTouch.exe", "--multi-tab-test" };
        LaunchOptions lo = parseLaunchOptions(2, argvMt);
        assert(lo.multiTabTest);

        SelectObject(mDC, old);
        DeleteObject(bmp);
        DeleteDC(mDC);
        ReleaseDC(NULL, scDC);
    }

    std::printf("[Test] Sprint 6B (F15 Multi-Phone Tabs & Strict Focus) OK!\n");
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
    testSprint1bPhase2PresetsAndTelemetry();
    testSprint2aUiFoundation();
    testSprint2bConnectionOnboardingAndSettings();
    testSprint3aProfileLibraryEditorAndDynamicHud();
    testSprint3bPolishCommandPaletteMotionAndA11y();
    std::fflush(stdout);
    testSprint4aMobaAndGamepadV2();
    std::fflush(stdout);
    testSprint4bInAppAudio();
    std::fflush(stdout);
    testSprint5aLanguageAndClipboard();
    std::fflush(stdout);
    testSprint5bAppsFilesAndShortcuts();
    std::fflush(stdout);
    testSprint6aCreatorAndDeviceControls();
    std::fflush(stdout);
    testSprint6bMultiDeviceSessions();
    std::printf("ALL SPRINT 6B + SPRINT 6A + SPRINT 5B + SPRINT 5A + SPRINT 4B + SPRINT 4A + SPRINT 3B + SPRINT 3A + SPRINT 2B + SPRINT 2A + SPRINT 1b + SPRINT 1 + SPRINT 0 + STAGE 1-3 TESTS PASSED!\n");
    return 0;
}
