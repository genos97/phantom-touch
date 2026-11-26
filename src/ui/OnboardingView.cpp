#include "ui/OnboardingView.hpp"
#include "ui/Theme.hpp"

#include <cstdio>

namespace {

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

int dp(int px) {
    return GdiPlusDraw::dp(px);
}

} // namespace

OnboardingView::OnboardingView()
    : step_(OB_STEP_TOOLS),
      profileChoice_(OB_PROF_DESKTOP),
      wirelessTab_(false),
      editingWifi_(false),
      cachedQrBmp_(nullptr),
      cachedQrSize_(0),
      cachedQrCellPx_(0),
      cachedQrHash_(0) {
}

OnboardingView::~OnboardingView() {
    delete cachedQrBmp_;
    cachedQrBmp_ = nullptr;
}

OnboardingStep OnboardingView::computeRecommendedStep(OnboardingStep current,
                                                      bool toolsReady,
                                                      bool connected,
                                                      bool hasAuthorizedDevice,
                                                      bool hasUnauthorizedDevice) {
    if (!toolsReady) {
        return OB_STEP_TOOLS;
    }
    if (current == OB_STEP_TOOLS) {
        if (connected) return OB_STEP_PROFILE;
        if (hasAuthorizedDevice || hasUnauthorizedDevice) return OB_STEP_CONNECT;
        return OB_STEP_PREPARE;
    }
    if (current <= OB_STEP_CONNECT && connected) {
        return OB_STEP_PROFILE;
    }
    return current;
}

void OnboardingView::setStep(OnboardingStep step) {
    if (step < OB_STEP_TOOLS) step = OB_STEP_TOOLS;
    if (step > OB_STEP_DONE) step = OB_STEP_DONE;
    step_ = step;
    editingWifi_ = false;
}

void OnboardingView::setWirelessTab(bool wireless) {
    wirelessTab_ = wireless;
    editingWifi_ = false;
}

void OnboardingView::paint(Gdiplus::Graphics& g, const RECT& viewportRc,
                           bool toolsReady, bool connected, bool connecting, bool repairingTools,
                           const std::string& toolsErr, const std::string& statusMsg,
                           const std::vector<AdbDeviceInfo>& devices,
                           const std::string& wirelessAddr,
                           bool showingQrPair, const QrMatrix* qrMatrix, const std::string& qrStatus,
                           int hoverX, int hoverY) {
    hotspots_.clear();
    GdiPlusDraw::fillRect(g, viewportRc, Theme::BG_CANVAS);

    int cardW = dp(600);
    int cardH = dp(476);
    int availW = (viewportRc.right - viewportRc.left) - dp(28);
    int availH = (viewportRc.bottom - viewportRc.top) - dp(28);
    if (cardW > availW) cardW = availW;
    if (cardH > availH) cardH = availH;
    if (cardW < dp(360)) cardW = dp(360);
    if (cardH < dp(340)) cardH = dp(340);

    int cx = (viewportRc.left + viewportRc.right) / 2;
    int cy = (viewportRc.top + viewportRc.bottom) / 2;
    RECT card = { cx - cardW / 2, cy - cardH / 2, cx + cardW / 2, cy + cardH / 2 };

    GdiPlusDraw::roundedCard(g, card, GdiPlusDraw::dpf(14.0f), Theme::BG_DRAWER, Theme::BORDER_STRONG, 1.5f);

    int padX = dp(24);
    int x = card.left + padX;
    int w = cardW - padX * 2;
    int y = card.top + dp(20);

    // Header + Skip link
    GdiPlusDraw::drawIcon(g, ICON_LOGO, static_cast<float>(x + dp(12)), static_cast<float>(y + dp(11)),
                          GdiPlusDraw::dpf(20.0f), Theme::ACCENT_CYAN);
    GdiPlusDraw::textLeft(g, "Welcome to Phantom Touch", static_cast<float>(x + dp(34)), static_cast<float>(y + dp(11)),
                          17.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - dp(120)));

    RECT rSkip = { card.right - padX - dp(66), y, card.right - padX, y + dp(26) };
    bool hovSkip = ptIn(rSkip, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rSkip, GdiPlusDraw::dpf(7.0f), hovSkip ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             hovSkip ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textCenter(g, "Skip", static_cast<float>(rSkip.left + rSkip.right) * 0.5f,
                            static_cast<float>(rSkip.top + rSkip.bottom) * 0.5f, 11.0f, true, Theme::TEXT_SECONDARY);
    hotspots_.push_back(Hotspot{ rSkip, BTN_SKIP, 0 });
    y += dp(36);

    // 5-Step Progress Pills
    static const char* kStepLabels[5] = { "1. Tools", "2. Phone", "3. Connect", "4. Starter", "5. Hotkeys" };
    int stepW = (w - dp(16)) / 5;
    for (int i = 0; i < 5; ++i) {
        RECT rs = { x + i * (stepW + dp(4)), y, x + i * (stepW + dp(4)) + stepW, y + dp(26) };
        bool isCur = (static_cast<int>(step_) == i);
        bool isDone = (static_cast<int>(step_) > i);
        uint32_t bg = isCur ? Theme::ACCENT_CYAN_BG : (isDone ? Theme::ACCENT_GREEN_BG : Theme::BG_CARD);
        uint32_t bd = isCur ? Theme::ACCENT_CYAN : (isDone ? Theme::ACCENT_GREEN : Theme::BORDER_SUBTLE);
        uint32_t fg = isCur ? Theme::ACCENT_CYAN : (isDone ? Theme::ACCENT_GREEN : Theme::TEXT_MUTED);
        GdiPlusDraw::roundedCard(g, rs, GdiPlusDraw::dpf(6.0f), bg, bd, isCur ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, kStepLabels[i], static_cast<float>(rs.left + rs.right) * 0.5f,
                                static_cast<float>(rs.top + rs.bottom) * 0.5f, 10.5f, true, fg);
        if (i <= static_cast<int>(step_) || toolsReady) {
            hotspots_.push_back(Hotspot{ rs, BTN_STEP_BASE + i, i });
        }
    }
    y += dp(40);

    // Footer buttons reserved at bottom
    const int footerY = card.bottom - dp(54);
    const int contentBottom = footerY - dp(12);
    const char* priLabel = "Continue";
    const char* secLabel = "Back";
    bool priEnabled = true;

    if (step_ == OB_STEP_TOOLS) {
        GdiPlusDraw::textLeft(g, "Step 1 of 5 · Phone Tools Check", static_cast<float>(x), static_cast<float>(y),
                              15.0f, true, Theme::TEXT_PRIMARY);
        y += dp(26);
        RECT rBox = { x, y, x + w, y + dp(128) };
        uint32_t bd = toolsReady ? Theme::ACCENT_GREEN : (repairingTools ? Theme::ACCENT_AMBER : Theme::ACCENT_RED);
        GdiPlusDraw::roundedCard(g, rBox, GdiPlusDraw::dpf(10.0f), Theme::BG_CARD, bd, 1.3f);
        GdiPlusDraw::drawIcon(g, toolsReady ? ICON_PLAY : ICON_USB,
                              static_cast<float>(x + dp(28)), static_cast<float>(y + dp(40)),
                              GdiPlusDraw::dpf(24.0f), bd);
        GdiPlusDraw::textLeft(g, toolsReady ? "Bundled phone tools ready (scrcpy 5.0.1 + adb)"
                                            : (repairingTools ? "Repairing phone tools..." : "Phone tools missing or incomplete"),
                              static_cast<float>(x + dp(56)), static_cast<float>(y + dp(32)),
                              13.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - dp(72)));
        std::string sub = toolsReady
            ? "All required USB, Wireless ADB, and low-latency streaming components are verified."
            : (toolsErr.empty() ? "Click Repair Tools to install scrcpy + adb automatically." : toolsErr);
        GdiPlusDraw::textLeft(g, sub, static_cast<float>(x + dp(56)), static_cast<float>(y + dp(58)),
                              11.0f, false, Theme::TEXT_SECONDARY, static_cast<float>(w - dp(72)));
        GdiPlusDraw::textLeft(g, "No external setup script needed when packaged tools are present.",
                              static_cast<float>(x + dp(56)), static_cast<float>(y + dp(88)),
                              10.5f, false, Theme::TEXT_MUTED, static_cast<float>(w - dp(72)));

        priLabel = toolsReady ? "Continue" : (repairingTools ? "Repairing..." : "Repair Tools");
        priEnabled = !repairingTools;
        secLabel = "Re-check";
    } else if (step_ == OB_STEP_PREPARE) {
        GdiPlusDraw::textLeft(g, "Step 2 of 5 · Prepare Your Android Phone", static_cast<float>(x), static_cast<float>(y),
                              15.0f, true, Theme::TEXT_PRIMARY);
        y += dp(26);
        static const char* kGuide[4][2] = {
            { "1. Open Settings -> About phone", "Scroll down to Software information (on Samsung / MIUI / Pixel)." },
            { "2. Tap 'Build number' 7 times", "Enter your PIN if asked until 'You are now a developer!' appears." },
            { "3. Developer options -> USB debugging", "Turn ON USB debugging (or Wireless debugging for Android 11+ Wi-Fi)." },
            { "4. Plug in USB cable & tap Allow", "Choose 'Always allow from this computer' on your phone screen." }
        };
        int rowH = (contentBottom - y - dp(18)) / 4;
        if (rowH > dp(58)) rowH = dp(58);
        for (int i = 0; i < 4; ++i) {
            RECT row = { x, y, x + w, y + rowH - dp(6) };
            GdiPlusDraw::roundedCard(g, row, GdiPlusDraw::dpf(9.0f), Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
            GdiPlusDraw::drawIcon(g, (i < 2) ? ICON_SETTINGS : ICON_USB,
                                  static_cast<float>(row.left + dp(20)),
                                  static_cast<float>(row.top + (rowH - dp(6)) / 2),
                                  GdiPlusDraw::dpf(16.0f), Theme::ACCENT_CYAN);
            GdiPlusDraw::textLeft(g, kGuide[i][0], static_cast<float>(row.left + dp(42)),
                                  static_cast<float>(row.top + dp(16)), 12.0f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - dp(54)));
            GdiPlusDraw::textLeft(g, kGuide[i][1], static_cast<float>(row.left + dp(42)),
                                  static_cast<float>(row.top + dp(34)), 10.5f, false, Theme::TEXT_MUTED, static_cast<float>(w - dp(54)));
            y += rowH;
        }
        priLabel = "I enabled USB debugging";
        secLabel = "Back";
    } else if (step_ == OB_STEP_CONNECT) {
        GdiPlusDraw::textLeft(g, "Step 3 of 5 · Connect Your Phone", static_cast<float>(x), static_cast<float>(y),
                              15.0f, true, Theme::TEXT_PRIMARY);
        y += dp(26);

        // USB / Wireless sub-tabs
        int tw = (w - dp(8)) / 2;
        RECT rUsb = { x, y, x + tw, y + dp(30) };
        RECT rWifi = { x + tw + dp(8), y, x + w, y + dp(30) };
        GdiPlusDraw::roundedCard(g, rUsb, GdiPlusDraw::dpf(8.0f), !wirelessTab_ ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD,
                                 !wirelessTab_ ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, !wirelessTab_ ? 1.4f : 1.0f);
        GdiPlusDraw::roundedCard(g, rWifi, GdiPlusDraw::dpf(8.0f), wirelessTab_ ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD,
                                 wirelessTab_ ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, wirelessTab_ ? 1.4f : 1.0f);
        GdiPlusDraw::textCenter(g, "USB Cable (Auto-Detect)", static_cast<float>(rUsb.left + rUsb.right) * 0.5f,
                                static_cast<float>(rUsb.top + rUsb.bottom) * 0.5f, 11.5f, true,
                                !wirelessTab_ ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        GdiPlusDraw::textCenter(g, "Wireless ADB / QR Pair", static_cast<float>(rWifi.left + rWifi.right) * 0.5f,
                                static_cast<float>(rWifi.top + rWifi.bottom) * 0.5f, 11.5f, true,
                                wirelessTab_ ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
        hotspots_.push_back(Hotspot{ rUsb, BTN_TAB_USB, 0 });
        hotspots_.push_back(Hotspot{ rWifi, BTN_TAB_WIFI, 0 });
        y += dp(38);

        if (!wirelessTab_) {
            bool anyUnauth = false;
            for (size_t i = 0; i < devices.size(); ++i) {
                if (devices[i].state == "unauthorized") anyUnauth = true;
            }
            if (anyUnauth) {
                RECT rWarn = { x, y, x + w, y + dp(34) };
                GdiPlusDraw::roundedCard(g, rWarn, GdiPlusDraw::dpf(8.0f), Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, 1.2f);
                GdiPlusDraw::textLeft(g, "Check your phone and tap Allow on the USB debugging prompt.",
                                      static_cast<float>(x + dp(12)), static_cast<float>(y + dp(17)),
                                      11.5f, true, Theme::ACCENT_AMBER, static_cast<float>(w - dp(24)));
                y += dp(42);
            } else if (!statusMsg.empty()) {
                RECT rMsg = { x, y, x + w, y + dp(30) };
                GdiPlusDraw::roundedCard(g, rMsg, GdiPlusDraw::dpf(8.0f),
                                         connecting ? Theme::ACCENT_CYAN_BG : Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
                GdiPlusDraw::textLeft(g, statusMsg, static_cast<float>(x + dp(12)), static_cast<float>(y + dp(15)),
                                      11.0f, false, connecting ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY,
                                      static_cast<float>(w - dp(24)));
                y += dp(38);
            }

            if (devices.empty()) {
                RECT rEmpty = { x, y, x + w, y + dp(90) };
                GdiPlusDraw::roundedCard(g, rEmpty, GdiPlusDraw::dpf(9.0f), Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
                GdiPlusDraw::textCenter(g, "Waiting for USB device (auto-checking every 2s)...",
                                        static_cast<float>(x + w / 2), static_cast<float>(y + dp(34)),
                                        12.5f, true, Theme::TEXT_PRIMARY);
                GdiPlusDraw::textCenter(g, "Plug in your cable now, or switch to Wireless ADB above.",
                                        static_cast<float>(x + w / 2), static_cast<float>(y + dp(56)),
                                        11.0f, false, Theme::TEXT_MUTED);
            } else {
                for (size_t i = 0; i < devices.size() && (y + dp(48)) <= contentBottom; ++i) {
                    const AdbDeviceInfo& d = devices[i];
                    bool ready = (d.state == "device");
                    RECT rDev = { x, y, x + w, y + dp(44) };
                    GdiPlusDraw::roundedCard(g, rDev, GdiPlusDraw::dpf(9.0f), Theme::BG_CARD,
                                             ready ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER, 1.1f);
                    GdiPlusDraw::drawIcon(g, d.wireless ? ICON_WIFI : ICON_USB,
                                          static_cast<float>(x + dp(20)), static_cast<float>(y + dp(22)),
                                          GdiPlusDraw::dpf(15.0f), ready ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER);
                    GdiPlusDraw::textLeft(g, d.model.empty() ? d.serial : d.model,
                                          static_cast<float>(x + dp(40)), static_cast<float>(y + dp(15)),
                                          12.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(w - dp(150)));
                    GdiPlusDraw::textLeft(g, ready ? "Authorized — Ready to connect" : "Unauthorized — Tap Allow on phone",
                                          static_cast<float>(x + dp(40)), static_cast<float>(y + dp(31)),
                                          10.5f, false, ready ? Theme::ACCENT_GREEN : Theme::ACCENT_AMBER,
                                          static_cast<float>(w - dp(150)));
                    if (ready) {
                        RECT rConn = { rDev.right - dp(96), y + dp(7), rDev.right - dp(8), y + dp(37) };
                        bool hovC = ptIn(rConn, hoverX, hoverY);
                        GdiPlusDraw::roundedCard(g, rConn, GdiPlusDraw::dpf(7.0f),
                                                 hovC ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG, Theme::ACCENT_CYAN, 1.2f);
                        GdiPlusDraw::textCenter(g, "Connect", static_cast<float>(rConn.left + rConn.right) * 0.5f,
                                                static_cast<float>(rConn.top + rConn.bottom) * 0.5f,
                                                11.5f, true, hovC ? 0xFF081018u : Theme::TEXT_PRIMARY);
                        hotspots_.push_back(Hotspot{ rConn, BTN_DEV_BASE + static_cast<int>(i), static_cast<int>(i) });
                    }
                    y += dp(50);
                }
            }
            priLabel = connected ? "Continue" : "Skip Connect for Now";
            secLabel = "Retry USB";
        } else {
            // Wireless ADB / QR Pair tab
            if (showingQrPair && qrMatrix && qrMatrix->size > 0) {
                const int qrSz = dp(150);
                RECT rQr = { cx - qrSz / 2, y, cx + qrSz / 2, y + qrSz };
                GdiPlusDraw::roundedCard(g, rQr, GdiPlusDraw::dpf(10.0f), 0xFFFFFFFFu, Theme::ACCENT_CYAN, 2.0f);
                const int cellPx = (qrSz - dp(12)) / (qrMatrix->size + 6);
                if (cellPx >= 2) {
                    uint32_t qhash = static_cast<uint32_t>(qrMatrix->size);
                    for (int r = 0; r < qrMatrix->size; ++r) {
                        for (int c = 0; c < qrMatrix->size; ++c) {
                            qhash = (qhash * 131u) + (qrMatrix->at(c, r) ? 1u : 0u);
                        }
                    }
                    const int drawSz = cellPx * qrMatrix->size;
                    if (!cachedQrBmp_ || cachedQrSize_ != qrMatrix->size ||
                        cachedQrCellPx_ != cellPx || cachedQrHash_ != qhash) {
                        delete cachedQrBmp_;
                        cachedQrBmp_ = new Gdiplus::Bitmap(drawSz, drawSz, PixelFormat32bppPARGB);
                        Gdiplus::Graphics qg(cachedQrBmp_);
                        qg.Clear(Gdiplus::Color(0, 0, 0, 0));
                        Gdiplus::SolidBrush darkBr(Gdiplus::Color(255, 10, 14, 22));
                        for (int r = 0; r < qrMatrix->size; ++r) {
                            for (int c = 0; c < qrMatrix->size; ++c) {
                                if (qrMatrix->at(c, r)) {
                                    qg.FillRectangle(&darkBr, static_cast<INT>(c * cellPx), static_cast<INT>(r * cellPx),
                                                     static_cast<INT>(cellPx), static_cast<INT>(cellPx));
                                }
                            }
                        }
                        cachedQrSize_ = qrMatrix->size;
                        cachedQrCellPx_ = cellPx;
                        cachedQrHash_ = qhash;
                    }
                    g.DrawImage(cachedQrBmp_, static_cast<INT>(cx - drawSz / 2), static_cast<INT>(y + (qrSz - drawSz) / 2),
                                static_cast<INT>(drawSz), static_cast<INT>(drawSz));
                }
                GdiPlusDraw::textCenter(g, qrStatus.empty() ? "Scan in Developer Options -> Wireless debugging" : qrStatus,
                                        static_cast<float>(cx), static_cast<float>(y + qrSz + dp(18)),
                                        11.0f, true, Theme::ACCENT_CYAN);
                RECT rBackQr = { cx - dp(90), y + qrSz + dp(32), cx + dp(90), y + qrSz + dp(60) };
                GdiPlusDraw::roundedCard(g, rBackQr, GdiPlusDraw::dpf(7.0f), Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
                GdiPlusDraw::textCenter(g, "Stop QR Pair", static_cast<float>(cx),
                                        static_cast<float>(rBackQr.top + rBackQr.bottom) * 0.5f, 11.0f, true, Theme::TEXT_PRIMARY);
                hotspots_.push_back(Hotspot{ rBackQr, BTN_WIFI_QR, 0 });
            } else {
                GdiPlusDraw::textLeft(g, "Enter phone IP:port (example 192.168.1.42:5555) or pair via QR:",
                                      static_cast<float>(x), static_cast<float>(y + dp(8)), 11.5f, false, Theme::TEXT_SECONDARY);
                y += dp(26);
                RECT rInp = { x, y, x + w - dp(196), y + dp(34) };
                GdiPlusDraw::roundedCard(g, rInp, GdiPlusDraw::dpf(8.0f), Theme::BG_INPUT,
                                         editingWifi_ ? Theme::ACCENT_CYAN : Theme::BORDER_STRONG, editingWifi_ ? 1.5f : 1.0f);
                GdiPlusDraw::textLeft(g, wirelessAddr + (editingWifi_ ? "_" : ""),
                                      static_cast<float>(rInp.left + dp(12)), static_cast<float>(y + dp(17)),
                                      12.0f, false, Theme::TEXT_PRIMARY, static_cast<float>(rInp.right - rInp.left - dp(20)));
                hotspots_.push_back(Hotspot{ rInp, BTN_WIFI_INPUT, 0 });

                RECT rCon = { rInp.right + dp(6), y, rInp.right + dp(98), y + dp(34) };
                GdiPlusDraw::roundedCard(g, rCon, GdiPlusDraw::dpf(8.0f), Theme::BG_CARD, Theme::ACCENT_CYAN, 1.0f);
                GdiPlusDraw::textCenter(g, "Connect IP", static_cast<float>(rCon.left + rCon.right) * 0.5f,
                                        static_cast<float>(y + dp(17)), 11.0f, true, Theme::ACCENT_CYAN);
                hotspots_.push_back(Hotspot{ rCon, BTN_WIFI_CONNECT, 0 });

                RECT rQr = { rCon.right + dp(6), y, x + w, y + dp(34) };
                GdiPlusDraw::roundedCard(g, rQr, GdiPlusDraw::dpf(8.0f), Theme::ACCENT_PURPLE_BG, Theme::ACCENT_PURPLE, 1.0f);
                GdiPlusDraw::textCenter(g, "QR Pair", static_cast<float>(rQr.left + rQr.right) * 0.5f,
                                        static_cast<float>(y + dp(17)), 11.0f, true, Theme::ACCENT_PURPLE);
                hotspots_.push_back(Hotspot{ rQr, BTN_WIFI_QR, 0 });
            }
            priLabel = "Skip Connect for Now";
            secLabel = "Back";
        }
    } else if (step_ == OB_STEP_PROFILE) {
        GdiPlusDraw::textLeft(g, "Step 4 of 5 · Pick Your Starting Point", static_cast<float>(x), static_cast<float>(y),
                              15.0f, true, Theme::TEXT_PRIMARY);
        y += dp(28);
        struct CardSpec {
            const char* title;
            const char* desc;
            OnboardingProfileChoice ch;
            int id;
        };
        static const CardSpec kCards[3] = {
            { "Desktop (Typing)", "Full PC keyboard, clipboard paste, scroll wheel, and right-click Back.", OB_PROF_DESKTOP, BTN_PROF_DESKTOP },
            { "Controller / Gamepad", "Enable XInput controller-to-touch with desktop typing support.", OB_PROF_CONTROLLER, BTN_PROF_CONTROLLER },
            { "Game Keymap Starter", "WASD + mouse aim lock + key badges ready to customize per game.", OB_PROF_GAMING, BTN_PROF_GAMING }
        };
        int ch = dp(68);
        for (int i = 0; i < 3; ++i) {
            RECT rc = { x, y, x + w, y + ch };
            bool sel = (profileChoice_ == kCards[i].ch);
            bool hov = ptIn(rc, hoverX, hoverY);
            GdiPlusDraw::roundedCard(g, rc, GdiPlusDraw::dpf(10.0f),
                                     sel ? Theme::ACCENT_CYAN_BG : (hov ? Theme::BG_CARD_HOVER : Theme::BG_CARD),
                                     sel ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, sel ? 1.5f : 1.0f);
            GdiPlusDraw::textLeft(g, kCards[i].title, static_cast<float>(x + dp(16)), static_cast<float>(y + dp(24)),
                                  13.5f, true, sel ? Theme::ACCENT_CYAN : Theme::TEXT_PRIMARY, static_cast<float>(w - dp(32)));
            GdiPlusDraw::textLeft(g, kCards[i].desc, static_cast<float>(x + dp(16)), static_cast<float>(y + dp(45)),
                                  11.0f, false, Theme::TEXT_SECONDARY, static_cast<float>(w - dp(32)));
            hotspots_.push_back(Hotspot{ rc, kCards[i].id, 0 });
            y += ch + dp(10);
        }
        priLabel = "Use Selected Starter";
        secLabel = "Back";
    } else if (step_ == OB_STEP_TOUR) {
        GdiPlusDraw::textLeft(g, "Step 5 of 5 · Quick Hotkey Tour", static_cast<float>(x), static_cast<float>(y),
                              15.0f, true, Theme::TEXT_PRIMARY);
        y += dp(28);
        static const char* kKeys[3][2] = {
            { "~ or F1   ·   Toggle Aim Lock", "Lock mouse for camera look in games, or unlock to use cursor anytime (Esc always unlocks)." },
            { "F2        ·   Keymap Studio", "Open the side drawer to drag buttons, add controls, and tune sensitivity live." },
            { "F11       ·   Borderless Fullscreen", "Stretch your phone stream edge-to-edge with Hardware Independent Flip." }
        };
        int cw = (w - dp(16)) / 3;
        int ch = dp(136);
        for (int i = 0; i < 3; ++i) {
            RECT rc = { x + i * (cw + dp(8)), y, x + i * (cw + dp(8)) + cw, y + ch };
            GdiPlusDraw::roundedCard(g, rc, GdiPlusDraw::dpf(10.0f), Theme::BG_CARD, Theme::BORDER_STRONG, 1.0f);
            GdiPlusDraw::textLeft(g, kKeys[i][0], static_cast<float>(rc.left + dp(12)), static_cast<float>(rc.top + dp(26)),
                                  12.0f, true, Theme::ACCENT_CYAN, static_cast<float>(cw - dp(24)));
            GdiPlusDraw::textLeft(g, kKeys[i][1], static_cast<float>(rc.left + dp(12)), static_cast<float>(rc.top + dp(56)),
                                  10.5f, false, Theme::TEXT_SECONDARY, static_cast<float>(cw - dp(24)));
        }
        y += ch + dp(16);
        RECT rNote = { x, y, x + w, y + dp(36) };
        GdiPlusDraw::roundedCard(g, rNote, GdiPlusDraw::dpf(8.0f), Theme::ACCENT_CYAN_BG, Theme::ACCENT_CYAN, 1.0f);
        GdiPlusDraw::textCenter(g, "Tip: Press Ctrl+K anytime for commands, or click ? in the top bar to reopen this guide.",
                                static_cast<float>(x + w / 2), static_cast<float>(y + dp(18)),
                                11.0f, true, Theme::TEXT_PRIMARY);
        priLabel = "Start Using Phantom Touch";
        secLabel = "Back";
    }

    // Footer Navigation Buttons
    int btnW = dp(196);
    RECT rPri = { x + w - btnW, footerY, x + w, footerY + dp(36) };
    bool hovPri = priEnabled && ptIn(rPri, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rPri, GdiPlusDraw::dpf(8.0f),
                             priEnabled ? (hovPri ? Theme::ACCENT_CYAN : Theme::ACCENT_CYAN_BG) : Theme::BG_CARD,
                             priEnabled ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.3f);
    GdiPlusDraw::textCenter(g, priLabel, static_cast<float>(rPri.left + rPri.right) * 0.5f,
                            static_cast<float>(rPri.top + rPri.bottom) * 0.5f,
                            12.0f, true, hovPri ? 0xFF081018u : (priEnabled ? Theme::TEXT_PRIMARY : Theme::TEXT_MUTED));
    if (priEnabled) {
        hotspots_.push_back(Hotspot{ rPri, BTN_PRIMARY, 0 });
    }

    RECT rSec = { x, footerY, x + dp(128), footerY + dp(36) };
    bool hovSec = ptIn(rSec, hoverX, hoverY);
    GdiPlusDraw::roundedCard(g, rSec, GdiPlusDraw::dpf(8.0f), hovSec ? Theme::BG_CARD_HOVER : Theme::BG_CARD,
                             Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, secLabel, static_cast<float>(rSec.left + rSec.right) * 0.5f,
                            static_cast<float>(rSec.top + rSec.bottom) * 0.5f,
                            11.5f, true, Theme::TEXT_SECONDARY);
    hotspots_.push_back(Hotspot{ rSec, BTN_SECONDARY, 0 });
}

bool OnboardingView::onMouseDown(int x, int y, OnboardingClickResult& out) {
    out = OnboardingClickResult();
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        const Hotspot& hs = hotspots_[i];
        if (!ptIn(hs.rc, x, y)) continue;
        editingWifi_ = false;
        if (hs.id >= BTN_DEV_BASE) {
            out.action = OB_ACT_CONNECT_DEVICE;
            out.index = hs.extra;
            out.redraw = true;
            return true;
        }
        if (hs.id >= BTN_STEP_BASE && hs.id < BTN_STEP_BASE + 5) {
            setStep(static_cast<OnboardingStep>(hs.extra));
            out.redraw = true;
            return true;
        }
        switch (hs.id) {
            case BTN_SKIP:
                out.action = OB_ACT_SKIP;
                out.redraw = true;
                return true;
            case BTN_TAB_USB:
                wirelessTab_ = false;
                out.redraw = true;
                return true;
            case BTN_TAB_WIFI:
                wirelessTab_ = true;
                out.redraw = true;
                return true;
            case BTN_WIFI_INPUT:
                editingWifi_ = true;
                out.redraw = true;
                return true;
            case BTN_WIFI_CONNECT:
                out.action = OB_ACT_CONNECT_WIFI;
                out.redraw = true;
                return true;
            case BTN_WIFI_QR:
                out.action = OB_ACT_TOGGLE_QR;
                out.redraw = true;
                return true;
            case BTN_PROF_DESKTOP:
                profileChoice_ = OB_PROF_DESKTOP;
                out.redraw = true;
                return true;
            case BTN_PROF_CONTROLLER:
                profileChoice_ = OB_PROF_CONTROLLER;
                out.redraw = true;
                return true;
            case BTN_PROF_GAMING:
                profileChoice_ = OB_PROF_GAMING;
                out.redraw = true;
                return true;
            case BTN_SECONDARY:
                if (step_ == OB_STEP_TOOLS) {
                    out.action = OB_ACT_RECHECK_TOOLS;
                } else if (step_ == OB_STEP_CONNECT && !wirelessTab_) {
                    out.action = OB_ACT_RECHECK_TOOLS;
                } else if (step_ > OB_STEP_TOOLS) {
                    setStep(static_cast<OnboardingStep>(static_cast<int>(step_) - 1));
                }
                out.redraw = true;
                return true;
            case BTN_PRIMARY:
                if (step_ == OB_STEP_TOOLS) {
                    out.action = OB_ACT_REPAIR_TOOLS;
                } else if (step_ == OB_STEP_PREPARE) {
                    setStep(OB_STEP_CONNECT);
                } else if (step_ == OB_STEP_CONNECT) {
                    setStep(OB_STEP_PROFILE);
                } else if (step_ == OB_STEP_PROFILE) {
                    out.action = OB_ACT_APPLY_PROFILE_CHOICE;
                    out.profileChoice = profileChoice_;
                    setStep(OB_STEP_TOUR);
                } else if (step_ == OB_STEP_TOUR) {
                    out.action = OB_ACT_FINISH;
                }
                out.redraw = true;
                return true;
            default:
                break;
        }
    }
    if (editingWifi_) {
        editingWifi_ = false;
        out.redraw = true;
    }
    return false;
}

bool OnboardingView::onKeyDown(int vk, std::string& wirelessAddr, OnboardingClickResult& out) {
    out = OnboardingClickResult();
    if (!editingWifi_) return false;
    if (vk == VK_RETURN) {
        editingWifi_ = false;
        out.action = OB_ACT_CONNECT_WIFI;
        out.redraw = true;
        return true;
    }
    if (vk == VK_ESCAPE) {
        editingWifi_ = false;
        out.redraw = true;
        return true;
    }
    if (vk == VK_BACK && !wirelessAddr.empty()) {
        wirelessAddr.pop_back();
        out.redraw = true;
        return true;
    }
    return true;
}

bool OnboardingView::onChar(char ch, std::string& wirelessAddr) {
    if (!editingWifi_ || ch < 32 || ch > 126) return false;
    if (wirelessAddr.size() < 28) {
        wirelessAddr.push_back(ch);
        return true;
    }
    return false;
}

bool OnboardingView::hoverItemAt(int x, int y, int& outId, RECT& outRc) const {
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        if (ptIn(hotspots_[i].rc, x, y)) {
            outId = hotspots_[i].id;
            outRc = hotspots_[i].rc;
            return true;
        }
    }
    return false;
}

bool OnboardingView::rectForStepButton(int buttonId, RECT& outRc) const {
    for (size_t i = 0; i < hotspots_.size(); ++i) {
        if (hotspots_[i].id == buttonId) {
            outRc = hotspots_[i].rc;
            return true;
        }
    }
    return false;
}
