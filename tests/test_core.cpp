#include "../src/sps_parser.hpp"
#include "../src/profile.hpp"
#include "../src/config_ui.hpp"
#include "../src/protocol.hpp"
#include "../src/aim_engine.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static void testSpsParser() {
    const uint8_t sps1920x1080[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x64, 0x00, 0x28,
        0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xF0, 0x3C, 0x60, 0xC6, 0x58
    };
    uint32_t w = 0, h = 0;
    bool ok = parseH264SpsResolution(sps1920x1080, sizeof(sps1920x1080), w, h);
    std::printf("[Test] SPS 1920x1080 -> ok=%d, %ux%u\n", ok ? 1 : 0, w, h);
    assert(ok && w == 1920 && h == 1080);

    const uint8_t sps1280x720[] = {
        0x00, 0x00, 0x00, 0x01, 0x67,
        0x42, 0xC0, 0x1F,
        0xD9, 0x00, 0x50, 0x05, 0xBB, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC0, 0xF1, 0x83, 0x19, 0x60
    };
    w = h = 0;
    ok = parseH264SpsResolution(sps1280x720, sizeof(sps1280x720), w, h);
    std::printf("[Test] SPS 1280x720  -> ok=%d, %ux%u\n", ok ? 1 : 0, w, h);
    assert(ok && w == 1280 && h == 720);

    const uint8_t junk[] = { 0x00, 0x00, 0x01, 0x67, 0xFF, 0xFF };
    assert(!parseH264SpsResolution(junk, sizeof(junk), w, h));
}

static void testProfileV3RoundTrip() {
    Profile orig = createDefaultFreeFireProfile();
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

    // Add a 2-key combo swipe node to verify combo + swipe round-trip
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
    assert(saveProfile(tmpPath, orig));

    Profile loaded;
    assert(loadProfile(tmpPath, loaded));
    assert(loaded.name == orig.name);
    assert(loaded.buttons.size() == orig.buttons.size());
    assert(loaded.fireMode == FIRE_MODE_CLAW_LEFT);
    assert(loaded.unboundedAim == true);
    assert(loaded.pollHz == 250);
    assert(loaded.joySteppedGlide == true);
    assert(std::fabs(loaded.ffCounterAccel - 8.5f) < 0.05f);
    assert(std::fabs(loaded.scopeSensX - 0.60f) < 0.01f);
    assert(std::fabs(loaded.scopeSensY - 0.70f) < 0.01f);

    const KeyNode& last = loaded.buttons.back();
    assert(last.label == "SWP");
    assert(last.comboVk == 0xA4 && last.vk == '1');
    assert(last.action == NODE_SWIPE);
    assert(std::fabs(last.x2 - 0.60f) < 0.01f);
    assert(std::fabs(last.y2 - 0.25f) < 0.01f);
    assert(last.durationMs == 140);

    int cVk = 0, pVk = 0;
    parseComboName("LALT+1", cVk, pVk);
    assert(cVk == 0xA4 && pVk == '1');
    assert(comboToName(cVk, pVk) == "LALT+1");
    assert(actionTypeToName(nameToActionType("DOWN_UP")) == "DOWN_UP");
    assert(actionTypeToName(nameToActionType("HOLD_VIEW")) == "HOLD_VIEW");
    assert(sanitizeProfileFilename("My Free Fire #1!") == "my_free_fire_1");

    Profile blank = createBlankProfile("Custom Test");
    assert(blank.buttons.empty());
    assert(blank.name == "Custom Test");
    assert(std::fabs(blank.sensX - 0.95f) < 0.01f); // Preserves working default sens!
    assert(std::fabs(blank.sensY - 1.15f) < 0.01f);

    std::vector<std::string> files = listProfileFiles("profiles");
    assert(!files.empty());

    std::remove(tmpPath.c_str());
    std::printf("[Test] Profile v3+Studio round-trip, blank creator, listing & defaults OK (%u nodes, %u profiles)\n",
                static_cast<unsigned>(loaded.buttons.size()), static_cast<unsigned>(files.size()));
}

static void testMouseAndLShiftRebind() {
    // 1. Generic VK_SHIFT (0x10) / VK_CONTROL (0x11) / VK_MENU (0x12) heal cleanly to LSHIFT/LCTRL/LALT
    assert(vkToName(0x10) == "LSHIFT");
    assert(vkToName(0xA0) == "LSHIFT");
    assert(nameToVk("0x10") == 0xA0);
    assert(nameToVk("0x11") == 0xA2);
    assert(nameToVk("0x12") == 0xA4);
    assert(nameToVk("LSHIFT") == 0xA0);
    assert(nameToVk("MMB") == 0x04);
    assert(vkToName(0x04) == "MMB");
    assert(nameToVk("X1") == 0x05 && vkToName(0x05) == "X1");
    assert(nameToVk("X2") == 0x06 && vkToName(0x06) == "X2");
    assert(nameToVk("MWHEELUP") == VK_CUSTOM_MWHEEL_UP);
    assert(nameToVk("MWHEELDOWN") == VK_CUSTOM_MWHEEL_DOWN);

    // 2. Round-trip LShift Aim Lock + MMB action button + Wheel button
    Profile p = createDefaultFreeFireProfile();
    p.toggleAimVk = VK_LSHIFT;
    p.holdUnlockVk = VK_MBUTTON;

    KeyNode mmbNode;
    mmbNode.label = "GLOO";
    mmbNode.vk = VK_MBUTTON;
    mmbNode.action = NODE_PRESS;
    mmbNode.x = 0.25f;
    mmbNode.y = 0.75f;
    p.buttons.push_back(mmbNode);

    KeyNode whlNode;
    whlNode.label = "WEAP";
    whlNode.vk = VK_CUSTOM_MWHEEL_UP;
    whlNode.action = NODE_CLICK;
    whlNode.x = 0.80f;
    whlNode.y = 0.20f;
    p.buttons.push_back(whlNode);

    const std::string tmpPath = "profiles\\_test_lshift_mmb.cfg";
    assert(saveProfile(tmpPath, p));
    Profile loaded;
    assert(loadProfile(tmpPath, loaded));
    assert(loaded.toggleAimVk == VK_LSHIFT);
    assert(loaded.holdUnlockVk == VK_MBUTTON);
    assert(loaded.buttons.size() == p.buttons.size());
    assert(loaded.buttons[loaded.buttons.size() - 2].vk == VK_MBUTTON);
    assert(loaded.buttons[loaded.buttons.size() - 1].vk == VK_CUSTOM_MWHEEL_UP);
    std::remove(tmpPath.c_str());

    // 3. ConfigPanelUI: paint Advanced tab, click Aim Lock Key, bind VK_SHIFT -> normalizes to VK_LSHIFT!
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, 1280, 720);
    HGDIOBJ oldBmp = SelectObject(memDC, bmp);
    HFONT fS = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fB = CreateFontA(14, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fT = CreateFontA(15, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    ConfigPanelUI ui;
    ui.setProfilesDir("profiles", "profiles\\freefire.cfg");
    int sel = 0;
    ui.paint(memDC, 1280, 720, loaded, "profiles\\freefire.cfg", sel, true, fS, fB, fT);

    // Click ADVANCED tab (tab 3): panel starts around x = 0.26*1280 = 332, y = 0.10*720 = 72
    int px = static_cast<int>(loaded.panelX * 1280.0f);
    int py = static_cast<int>(loaded.panelY * 720.0f);
    ConfigUIResult res;
    assert(ui.onMouseDown(px + 330, py + 48, 1280, 720, loaded, sel, res));
    assert(res.cancelRebind == true);

    // Repaint Advanced tab and click Aim Lock Key button (around y + 324)
    ui.paint(memDC, 1280, 720, loaded, "profiles\\freefire.cfg", sel, false, fS, fB, fT);
    // Find Aim Lock Key row: cy starts at contentY(py+74)+4=py+78, then +34,+34,+34,+36,+30,+34 = py+280
    ConfigUIResult res2;
    assert(ui.onMouseDown(px + 180, py + 292, 1280, 720, loaded, sel, res2));
    assert(ui.isRebindingToggleAim() == true);

    // Reject LMB for Aim Lock
    ConfigUIResult resLmb;
    assert(ui.onMouseBind(VK_LBUTTON, loaded, sel, resLmb));
    assert(ui.isRebindingToggleAim() == true); // Still waiting!

    // Accept generic VK_SHIFT -> normalizes to VK_LSHIFT (0xA0)
    ConfigUIResult resShift;
    assert(ui.onKeyDown(VK_SHIFT, loaded, sel, resShift));
    assert(!ui.isRebindingToggleAim());
    assert(loaded.toggleAimVk == VK_LSHIFT);

    // Click MMB quick chip beside Aim Lock Key (x = px+14+(402-82)..(402) => px+375, y = py+292)
    ConfigUIResult resMmbChip;
    assert(ui.onMouseDown(px + 375, py + 292, 1280, 720, loaded, sel, resMmbChip));
    assert(loaded.toggleAimVk == VK_MBUTTON);

    // Click MMB chip in Key Inspector (chip index 2 out of 5): chipW=45, step=48, inspector at px+440, Row 1 at py+98..120
    ConfigUIResult resInspMmb;
    assert(ui.onMouseDown(px + 440 + 12 + 2 * 48 + 20, py + 109, 1280, 720, loaded, sel, resInspMmb));
    assert(loaded.buttons[0].vk == VK_MBUTTON);
    assert(resInspMmb.cancelRebind == true);

    // Click SCROLL UP and SCROLL DOWN chips in Row 2 (y = py + 125..149)
    ConfigUIResult resScUp;
    assert(ui.onMouseDown(px + 440 + 60, py + 137, 1280, 720, loaded, sel, resScUp));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_UP);
    ConfigUIResult resScDn;
    assert(ui.onMouseDown(px + 440 + 190, py + 137, 1280, 720, loaded, sel, resScDn));
    assert(loaded.buttons[0].vk == VK_CUSTOM_MWHEEL_DOWN);

    DeleteObject(fS); DeleteObject(fB); DeleteObject(fT);
    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    std::printf("[Test] LShift normalization, MMB/Mouse bindings & ConfigPanelUI interactions OK!\n");
}

static void testAimEngineLShiftCursorToggle() {
    // Reproduces exact bug from my_custom.cfg: joy_sprint = LSHIFT + button BAG = LSHIFT (CURSOR_TOGGLE)
    ScrcpyBridge bridge;
    AimEngine engine(&bridge);

    Profile p = createDefaultFreeFireProfile();
    p.vkSprint = VK_LSHIFT;
    bool foundBag = false;
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        if (p.buttons[i].action == NODE_CURSOR_TOGGLE) {
            p.buttons[i].vk = VK_LSHIFT;
            foundBag = true;
            break;
        }
    }
    assert(foundBag);
    engine.setProfile(p);

    // 1. In Cursor Mode (aimMode == false): LSHIFT triggers BAG CURSOR_TOGGLE -> reqToggle == true (locks mouse)
    engine.setAimMode(false);
    bool reqToggle = false;
    bool consumed = engine.onKeyEvent(VK_LSHIFT, true, reqToggle);
    assert(consumed && reqToggle);
    engine.onKeyEvent(VK_LSHIFT, false, reqToggle);

    // 2. In Aim Mode (aimMode == true): LSHIFT must STILL trigger BAG CURSOR_TOGGLE -> reqToggle == true (unlocks mouse!)
    //    Previously joy_sprint = LSHIFT swallowed VK_LSHIFT and returned reqToggle == false!
    engine.setAimMode(true);
    reqToggle = false;
    consumed = engine.onKeyEvent(VK_LSHIFT, true, reqToggle);
    assert(consumed && reqToggle);
    engine.onKeyEvent(VK_LSHIFT, false, reqToggle);

    // 3. Also verify MMB action button triggers cleanly in Aim Mode
    KeyNode mmbBtn;
    mmbBtn.label = "MMBTEST";
    mmbBtn.vk = VK_MBUTTON;
    mmbBtn.action = NODE_PRESS;
    mmbBtn.x = 0.5f;
    mmbBtn.y = 0.5f;
    p.buttons.push_back(mmbBtn);
    engine.setProfile(p);
    engine.setAimMode(true);
    reqToggle = false;
    consumed = engine.onKeyEvent(VK_MBUTTON, true, reqToggle);
    assert(consumed && !reqToggle);
    consumed = engine.onKeyEvent(VK_MBUTTON, false, reqToggle);
    assert(consumed);

    std::printf("[Test] AimEngine LShift CURSOR_TOGGLE unlock (despite joy_sprint=LSHIFT) & MMB press OK!\n");
}

static void testLowLatencyAimDefaultsAnd360Consistency() {
    Profile def = createDefaultFreeFireProfile();
    assert(def.pollHz == 250);
    assert(std::fabs(def.ffCounterAccel - 7.0f) < 0.01f);
    assert(std::fabs(def.smoothing - 0.10f) < 0.001f);
    assert(def.scrcpyArgs.find("--max-fps=90") != std::string::npos);

    // Verify AimEngine event-driven thread handles concurrent keyboard + mouse + wheel cleanly,
    // AND keeps WASD & keyboard active when mouse is unlocked (Cursor Mode)!
    ScrcpyBridge bridge;
    AimEngine engine(&bridge);
    KeyNode scrlBtn;
    scrlBtn.label = "WPNUP";
    scrlBtn.vk = VK_CUSTOM_MWHEEL_UP;
    scrlBtn.action = NODE_PRESS;
    scrlBtn.x = 0.85f;
    scrlBtn.y = 0.15f;
    def.buttons.push_back(scrlBtn);
    engine.setProfile(def);
    engine.start();

    // 1. Mouse UNLOCKED (Cursor Mode): W and Space MUST work, while LMB stays free for cursor clicks!
    engine.setAimMode(false);
    bool reqToggle = false;
    assert(engine.onKeyEvent('W', true, reqToggle) == true && !reqToggle);
    assert(engine.onKeyEvent(VK_SPACE, true, reqToggle) == true && !reqToggle);
    assert(engine.onKeyEvent(VK_LBUTTON, true, reqToggle) == false);
    Sleep(15);
    EngineTelemetry telUnlocked = engine.getTelemetry();
    assert(telUnlocked.aimMode == false);
    assert(telUnlocked.joyDown == true); // Running with W works while mouse is unlocked!

    // 2. Lock mouse while STILL holding W -> WASD stays alive, concurrent mouse + keys + scroll work smoothly!
    engine.setAimMode(true);
    for (int i = 0; i < 20; ++i) {
        engine.onRawMouseDelta(40, -10);
        if (i == 5)  engine.onKeyEvent('D', true, reqToggle);
        if (i == 10) engine.onKeyEvent(VK_CUSTOM_MWHEEL_UP, true, reqToggle);
        if (i == 12) engine.onKeyEvent(VK_CUSTOM_MWHEEL_UP, true, reqToggle);
        if (i == 15) engine.onKeyEvent('D', false, reqToggle);
        Sleep(2);
    }
    EngineTelemetry tel = engine.getTelemetry();
    assert(tel.aimMode == true);
    assert(tel.aimFingerDown == true);
    assert(tel.joyDown == true); // W stayed alive through Aim Mode lock!

    // 3. Unlock mouse again while STILL holding W -> W stays alive without interrupting movement!
    engine.setAimMode(false);
    Sleep(8);
    assert(engine.getTelemetry().joyDown == true);
    engine.onKeyEvent('W', false, reqToggle);
    engine.onKeyEvent(VK_SPACE, false, reqToggle);
    engine.stop();
    std::printf("[Test] Non-blocking KB+Mouse concurrency, unlocked keyboard/WASD & Scroll Up/Down OK!\n");
}

int main() {
    testSpsParser();
    testProfileV3RoundTrip();
    testMouseAndLShiftRebind();
    testAimEngineLShiftCursorToggle();
    testLowLatencyAimDefaultsAnd360Consistency();
    std::printf("ALL V3 CORE TESTS PASSED!\n");
    return 0;
}
