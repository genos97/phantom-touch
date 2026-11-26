#include "ui/OverlayPopups.hpp"
#include "config/KeyCodes.hpp"
#include "ui/HudRenderer.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

int dp(int px) {
    return GdiPlusDraw::dp(px);
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

bool isWordBoundary(const std::string& s, size_t idx) {
    if (idx == 0) return true;
    char c = s[idx - 1];
    return (c == ' ' || c == '_' || c == '-' || c == '/' || c == '.' || c == ':');
}

RECT clampScreenPopupRect(int x, int y, int w, int h) {
    POINT pt = { x, y };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + w > mi.rcWork.right - 6) x = mi.rcWork.right - w - 6;
        if (y + h > mi.rcWork.bottom - 6) y = mi.rcWork.bottom - h - 6;
        if (x < mi.rcWork.left + 6) x = mi.rcWork.left + 6;
        if (y < mi.rcWork.top + 6) y = mi.rcWork.top + 6;
    }
    RECT rc = { x, y, x + w, y + h };
    return rc;
}

} // namespace

// ============================================================================
// CommandPaletteModel (X5)
// ============================================================================

int CommandPaletteModel::fuzzyScore(const std::string& text, const std::string& query) {
    std::string q = toLowerAscii(query);
    if (q.empty()) return 1;
    std::string t = toLowerAscii(text);
    if (t.empty()) return 0;

    if (t == q) return 1000;
    if (t.compare(0, q.size(), q) == 0) {
        int penalty = static_cast<int>(t.size() > 200 ? 200 : t.size());
        return 820 - penalty;
    }

    size_t subPos = t.find(q);
    if (subPos != std::string::npos) {
        int base = isWordBoundary(t, subPos) ? 680 : 520;
        int pen = static_cast<int>(subPos > 120 ? 120 : subPos);
        return base - pen;
    }

    // Ordered subsequence matching with consecutive + word-boundary bonuses
    size_t qi = 0;
    size_t prevMatch = 0;
    int bonus = 0;
    for (size_t ti = 0; ti < t.size() && qi < q.size(); ++ti) {
        if (t[ti] == q[qi]) {
            if (qi > 0 && ti == prevMatch + 1) bonus += 16;
            if (isWordBoundary(t, ti)) bonus += 24;
            prevMatch = ti;
            ++qi;
        }
    }
    if (qi < q.size()) return 0;
    int score = 180 + bonus - static_cast<int>(t.size() > 140 ? 140 : t.size());
    return (score < 40) ? 40 : score;
}

std::vector<int> CommandPaletteModel::filter(const std::string& query) const {
    struct ScoredIdx {
        int idx;
        int score;
    };
    std::vector<ScoredIdx> scored;
    scored.reserve(cmds_.size());
    for (size_t i = 0; i < cmds_.size(); ++i) {
        const CommandEntry& c = cmds_[i];
        int s = fuzzyScore(c.title, query);
        int sKw = fuzzyScore(c.keywords, query) / 2 + (fuzzyScore(c.keywords, query) > 0 ? 120 : 0);
        int sCat = fuzzyScore(c.category + " " + c.title, query);
        int sHk = (!c.hotkey.empty()) ? fuzzyScore(c.hotkey, query) / 2 : 0;
        int best = s;
        if (sKw > best) best = sKw;
        if (sCat > best) best = sCat;
        if (sHk > best) best = sHk;
        if (best > 0) {
            ScoredIdx si;
            si.idx = static_cast<int>(i);
            si.score = best;
            scored.push_back(si);
        }
    }
    if (!query.empty()) {
        std::stable_sort(scored.begin(), scored.end(), [](const ScoredIdx& a, const ScoredIdx& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.idx < b.idx;
        });
    }
    std::vector<int> out;
    out.reserve(scored.size());
    for (size_t i = 0; i < scored.size(); ++i) out.push_back(scored[i].idx);
    return out;
}

void CommandPaletteModel::paint(Gdiplus::Graphics& g, int w, int h,
                                const std::string& query, const std::vector<int>& filtered,
                                int selectedRow, int hoverRow,
                                std::vector<RECT>& outRowRects) const {
    outRowRects.clear();
    RECT full = { 0, 0, w, h };
    GdiPlusDraw::roundedCard(g, full, GdiPlusDraw::dpf(Theme::R_LG), 0xF5111722u, Theme::ACCENT_CYAN, 1.3f);

    const int pad = dp(14);
    const int boxH = dp(36);
    RECT rSearch = { pad, pad, w - pad, pad + boxH };
    GdiPlusDraw::roundedCard(g, rSearch, GdiPlusDraw::dpf(Theme::R_MD), Theme::BG_INPUT, Theme::ACCENT_CYAN, 1.2f);
    std::string sTxt = query.empty() ? "Search commands, presets, profiles... _" : ("> " + query + "_");
    GdiPlusDraw::textLeft(g, sTxt, static_cast<float>(rSearch.left + dp(12)),
                          static_cast<float>(rSearch.top + rSearch.bottom) * 0.5f,
                          12.5f, !query.empty(),
                          query.empty() ? Theme::TEXT_MUTED : Theme::TEXT_PRIMARY,
                          static_cast<float>(w - pad * 2 - dp(76)));

    RECT rEsc = { rSearch.right - dp(60), rSearch.top + dp(7), rSearch.right - dp(8), rSearch.bottom - dp(7) };
    GdiPlusDraw::roundedCard(g, rEsc, GdiPlusDraw::dpf(5.0f), Theme::BG_CARD, Theme::BORDER_SUBTLE, 1.0f);
    GdiPlusDraw::textCenter(g, "ESC", static_cast<float>(rEsc.left + rEsc.right) * 0.5f,
                            static_cast<float>(rEsc.top + rEsc.bottom) * 0.5f, 9.5f, true, Theme::TEXT_MUTED);

    int y = rSearch.bottom + dp(10);
    if (filtered.empty()) {
        GdiPlusDraw::textCenter(g, "No commands match '" + query + "'",
                                static_cast<float>(w) * 0.5f, static_cast<float>(y + dp(34)),
                                12.0f, false, Theme::TEXT_MUTED);
        return;
    }

    const int rowH = dp(36);
    const int gap = dp(4);
    int maxRows = (h - y - dp(26)) / (rowH + gap);
    if (maxRows < 1) maxRows = 1;
    int shown = static_cast<int>(filtered.size());
    if (shown > maxRows) shown = maxRows;

    int startOff = 0;
    if (selectedRow >= shown) {
        startOff = selectedRow - shown + 1;
    }

    for (int r = 0; r < shown; ++r) {
        int listPos = startOff + r;
        if (listPos >= static_cast<int>(filtered.size())) break;
        const CommandEntry& c = cmds_[filtered[listPos]];
        RECT rc = { pad, y, w - pad, y + rowH };
        bool isSel = (listPos == selectedRow);
        bool isHov = (listPos == hoverRow);
        GdiPlusDraw::roundedCard(g, rc, GdiPlusDraw::dpf(8.0f),
                                 isSel ? Theme::ACCENT_CYAN_BG : (isHov ? Theme::BG_CARD_HOVER : Theme::BG_CARD),
                                 isSel ? Theme::ACCENT_CYAN : (isHov ? Theme::BORDER_STRONG : Theme::BORDER_SUBTLE),
                                 isSel ? 1.3f : 1.0f);

        int catW = dp(68);
        RECT rCat = { rc.left + dp(8), rc.top + dp(8), rc.left + dp(8) + catW, rc.bottom - dp(8) };
        GdiPlusDraw::roundedCard(g, rCat, GdiPlusDraw::dpf(5.0f), Theme::BG_SHELL, Theme::BORDER_SUBTLE, 0.8f);
        GdiPlusDraw::textCenter(g, c.category, static_cast<float>(rCat.left + rCat.right) * 0.5f,
                                static_cast<float>(rCat.top + rCat.bottom) * 0.5f, 9.5f, true, Theme::ACCENT_CYAN);

        int hkW = c.hotkey.empty() ? 0 : dp(72);
        GdiPlusDraw::textLeft(g, c.title, static_cast<float>(rCat.right + dp(10)),
                              static_cast<float>(rc.top + rc.bottom) * 0.5f, 12.0f, true,
                              isSel ? 0xFFFFFFFFu : Theme::TEXT_PRIMARY,
                              static_cast<float>(rc.right - rCat.right - hkW - dp(24)));

        if (!c.hotkey.empty()) {
            RECT rHk = { rc.right - hkW - dp(8), rc.top + dp(7), rc.right - dp(8), rc.bottom - dp(7) };
            GdiPlusDraw::roundedCard(g, rHk, GdiPlusDraw::dpf(5.0f), Theme::BG_INPUT, Theme::BORDER_STRONG, 1.0f);
            GdiPlusDraw::textCenter(g, c.hotkey, static_cast<float>(rHk.left + rHk.right) * 0.5f,
                                    static_cast<float>(rHk.top + rHk.bottom) * 0.5f, 9.5f, true, Theme::TEXT_SECONDARY);
        }

        outRowRects.push_back(rc);
        y += rowH + gap;
    }

    GdiPlusDraw::textLeft(g, "Up/Down: Select  |  Enter: Run  |  Ctrl+K: Close",
                          static_cast<float>(pad + 2), static_cast<float>(h - dp(12)), 9.5f, false, Theme::TEXT_MUTED);
}

// ============================================================================
// CheatSheetModel (X6)
// ============================================================================

CheatSheetData CheatSheetModel::build(const Profile& profile) {
    CheatSheetData d;
    d.profileName = profile.name.empty() ? "Unnamed Profile" : profile.name;
    d.keymapEnabled = profile.keymapEnabled;

    d.profileBindings.push_back(CheatRow(KeyCodes::toName(profile.toggleAimVk) + " / F1", "Toggle Aim Lock", Theme::ACCENT_GREEN));
    if (profile.holdUnlockVk != 0) {
        d.profileBindings.push_back(CheatRow(KeyCodes::toName(profile.holdUnlockVk), "Hold to Unlock Cursor", Theme::ACCENT_CYAN));
    }

    if (!profile.keymapEnabled) {
        d.profileBindings.push_back(CheatRow("Keyboard", "Desktop PC Typing Mode (direct text)", Theme::ACCENT_CYAN));
        d.profileBindings.push_back(CheatRow("Left Drag", "Touch / Scroll on Phone", Theme::TEXT_SECONDARY));
    } else {
        std::string wasd = KeyCodes::toName(profile.vkUp) + "/" + KeyCodes::toName(profile.vkLeft) + "/" +
                           KeyCodes::toName(profile.vkDown) + "/" + KeyCodes::toName(profile.vkRight);
        d.profileBindings.push_back(CheatRow(wasd, "Movement Virtual Joystick", Theme::ACCENT_GREEN));
        const char* fireModeLbl = (profile.fireMode == FIRE_MODE_CLAW_LEFT) ? "Fire (Claw Left)"
                                : (profile.fireMode == FIRE_MODE_DUAL_DRAG) ? "Fire (Dual Drag)"
                                                                            : "Fire (Drag Fire)";
        d.profileBindings.push_back(CheatRow(HudRenderer::shortKeyLabel(0, profile.fireVk), fireModeLbl, Theme::HUD_FIRE));
        if (profile.resetTouchVk != 0) {
            d.profileBindings.push_back(CheatRow(KeyCodes::toName(profile.resetTouchVk), "Quick Touch Reset", Theme::ACCENT_AMBER));
        }

        for (size_t i = 0; i < profile.buttons.size(); ++i) {
            const KeyNode& b = profile.buttons[i];
            std::string keyStr = HudRenderer::shortKeyLabel(b.comboVk, b.vk);
            std::string actStr = ProfileDefaults::actionTypeToHumanLabel(b.action);
            std::string desc = b.label.empty() ? actStr : (b.label + " (" + actStr + ")");
            uint32_t col = (b.action == NODE_CURSOR_TOGGLE || b.action == NODE_SWITCH_SENS || b.action == NODE_RESET_VIEW)
                ? Theme::ACCENT_PURPLE : Theme::ACCENT_CYAN;
            d.profileBindings.push_back(CheatRow(keyStr, desc, col));
        }
    }

    d.globalHotkeys.push_back(CheatRow("Ctrl + K", "Command Palette", Theme::ACCENT_CYAN));
    d.globalHotkeys.push_back(CheatRow("Ctrl + /", "Toggle This Cheat Sheet", Theme::ACCENT_CYAN));
    d.globalHotkeys.push_back(CheatRow("F1", "Aim Lock <-> Cursor Mode", Theme::ACCENT_GREEN));
    d.globalHotkeys.push_back(CheatRow("F2", "Keymap Studio Drawer", Theme::ACCENT_CYAN));
    d.globalHotkeys.push_back(CheatRow("F3 / F4", "Cycle Fire Mode / Toggle HUD", Theme::ACCENT_AMBER));
    d.globalHotkeys.push_back(CheatRow("F5 / F11", "Reset Touches / Fullscreen", Theme::ACCENT_AMBER));
    d.globalHotkeys.push_back(CheatRow("[  /  ]", "Sens X -0.05 / +0.05", Theme::TEXT_SECONDARY));
    d.globalHotkeys.push_back(CheatRow("-  /  =", "Sens Y -0.05 / +0.05", Theme::TEXT_SECONDARY));
    d.globalHotkeys.push_back(CheatRow("Ctrl+Shift+D", "Latency & UI Debug Strip", Theme::TEXT_MUTED));
    d.globalHotkeys.push_back(CheatRow("Ctrl+Shift+L", "Run Round-Trip Latency Probe", Theme::TEXT_MUTED));
    return d;
}

void CheatSheetModel::paint(Gdiplus::Graphics& g, int w, int h, const CheatSheetData& data) {
    RECT full = { 0, 0, w, h };
    GdiPlusDraw::roundedCard(g, full, GdiPlusDraw::dpf(Theme::R_LG), 0xF5111722u, Theme::ACCENT_CYAN, 1.3f);

    const int pad = dp(18);
    GdiPlusDraw::textLeft(g, "LIVE KEYBINDING CHEAT SHEET  ·  " + data.profileName,
                          static_cast<float>(pad), static_cast<float>(pad + dp(8)),
                          14.0f, true, Theme::ACCENT_CYAN, static_cast<float>(w - pad * 2 - dp(120)));
    GdiPlusDraw::textRight(g, "Esc or Ctrl+/ to close",
                           static_cast<float>(w - pad), static_cast<float>(pad + dp(8)),
                           10.5f, false, Theme::TEXT_MUTED);
    GdiPlusDraw::hLine(g, pad, w - pad, pad + dp(24), Theme::BORDER_CHROME);

    const int colGap = dp(18);
    const int colW = (w - pad * 2 - colGap) / 2;
    int lx = pad;
    int rx = pad + colW + colGap;
    int topY = pad + dp(34);

    auto drawColumn = [&](int x, int cw, const char* hdr, const std::vector<CheatRow>& rows) {
        GdiPlusDraw::textLeft(g, hdr, static_cast<float>(x), static_cast<float>(topY + dp(8)), 10.5f, true, Theme::TEXT_MUTED);
        int y = topY + dp(22);
        const int rowH = dp(24);
        int maxBottom = h - dp(16);
        for (size_t i = 0; i < rows.size(); ++i) {
            if (y + rowH > maxBottom) break;
            RECT rKey = { x, y + dp(2), x + dp(96), y + rowH - dp(2) };
            GdiPlusDraw::roundedCard(g, rKey, GdiPlusDraw::dpf(5.0f), Theme::BG_INPUT, rows[i].accent, 0.9f);
            GdiPlusDraw::textCenter(g, rows[i].key, static_cast<float>(rKey.left + rKey.right) * 0.5f,
                                    static_cast<float>(rKey.top + rKey.bottom) * 0.5f, 9.5f, true, rows[i].accent);
            GdiPlusDraw::textLeft(g, rows[i].desc, static_cast<float>(rKey.right + dp(8)),
                                  static_cast<float>(y + rowH / 2), 11.0f, false, Theme::TEXT_PRIMARY,
                                  static_cast<float>(cw - dp(104)));
            y += rowH + dp(2);
        }
    };

    drawColumn(lx, colW, "ACTIVE PROFILE BINDINGS", data.profileBindings);
    drawColumn(rx, colW, "GLOBAL SHORTCUTS", data.globalHotkeys);
}

// ============================================================================
// ProfileDropdownModel (X7)
// ============================================================================

std::vector<int> ProfileDropdownModel::filter(const std::vector<ProfileMenuItem>& items, const std::string& query) {
    struct Scored { int idx; int score; };
    std::vector<Scored> list;
    for (size_t i = 0; i < items.size(); ++i) {
        int s1 = CommandPaletteModel::fuzzyScore(items[i].displayName, query);
        int s2 = items[i].appPackage.empty() ? 0 : CommandPaletteModel::fuzzyScore(items[i].appPackage, query);
        int best = (s1 > s2) ? s1 : s2;
        if (best > 0) {
            if (query.empty()) {
                if (items[i].active) best += 300;
                if (items[i].recent) best += 150;
            }
            Scored s;
            s.idx = static_cast<int>(i);
            s.score = best;
            list.push_back(s);
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.idx < b.idx;
    });
    std::vector<int> out;
    out.reserve(list.size());
    for (size_t i = 0; i < list.size(); ++i) out.push_back(list[i].idx);
    return out;
}

void ProfileDropdownModel::paint(Gdiplus::Graphics& g, int w, int h,
                                 const std::string& query,
                                 const std::vector<ProfileMenuItem>& items,
                                 const std::vector<int>& filtered,
                                 int selectedRow, int hoverRow,
                                 std::vector<RECT>& outItemRects,
                                 RECT& outManageRc) {
    outItemRects.clear();
    ZeroMemory(&outManageRc, sizeof(outManageRc));

    RECT full = { 0, 0, w, h };
    GdiPlusDraw::roundedCard(g, full, GdiPlusDraw::dpf(Theme::R_MD), 0xF5111722u, Theme::ACCENT_CYAN, 1.2f);

    const int pad = dp(10);
    RECT rSearch = { pad, pad, w - pad, pad + dp(28) };
    GdiPlusDraw::roundedCard(g, rSearch, GdiPlusDraw::dpf(6.0f), Theme::BG_INPUT, Theme::ACCENT_CYAN, 1.0f);
    std::string qTxt = query.empty() ? "Filter profiles or linked app... _" : (query + "_");
    GdiPlusDraw::textLeft(g, qTxt, static_cast<float>(rSearch.left + dp(8)),
                          static_cast<float>(rSearch.top + rSearch.bottom) * 0.5f,
                          11.0f, !query.empty(),
                          query.empty() ? Theme::TEXT_MUTED : Theme::TEXT_PRIMARY,
                          static_cast<float>(w - pad * 2 - dp(16)));

    int y = rSearch.bottom + dp(8);
    int manageH = dp(30);
    int maxListBottom = h - manageH - pad - dp(6);
    const int rowH = dp(36);
    const int gap = dp(4);

    if (filtered.empty()) {
        GdiPlusDraw::textCenter(g, "No matching profiles", static_cast<float>(w) * 0.5f,
                                static_cast<float>(y + dp(24)), 11.0f, false, Theme::TEXT_MUTED);
    } else {
        for (size_t ri = 0; ri < filtered.size(); ++ri) {
            if (y + rowH > maxListBottom) break;
            const ProfileMenuItem& it = items[filtered[ri]];
            RECT rc = { pad, y, w - pad, y + rowH };
            bool isSel = (static_cast<int>(ri) == selectedRow);
            bool isHov = (static_cast<int>(ri) == hoverRow);
            uint32_t bg = it.active ? Theme::ACCENT_GREEN_BG : (isSel ? Theme::ACCENT_CYAN_BG : (isHov ? Theme::BG_CARD_HOVER : Theme::BG_CARD));
            uint32_t bd = it.active ? Theme::ACCENT_GREEN : (isSel ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE);
            GdiPlusDraw::roundedCard(g, rc, GdiPlusDraw::dpf(7.0f), bg, bd, (it.active || isSel) ? 1.2f : 1.0f);

            std::string prefix = it.active ? "* " : "";
            GdiPlusDraw::textLeft(g, prefix + it.displayName, static_cast<float>(rc.left + dp(9)),
                                  static_cast<float>(rc.top + dp(12)), 11.5f, true,
                                  it.active ? Theme::ACCENT_GREEN : Theme::TEXT_PRIMARY,
                                  static_cast<float>(w - pad * 2 - dp(76)));

            std::string sub = it.appPackage.empty()
                ? (it.recent ? "Recent" : "Manual")
                : ("for " + it.appPackage);
            uint32_t subCol = it.appPackage.empty() ? Theme::TEXT_MUTED : Theme::ACCENT_CYAN;
            GdiPlusDraw::textLeft(g, sub, static_cast<float>(rc.left + dp(9)),
                                  static_cast<float>(rc.top + dp(26)), 9.5f, !it.appPackage.empty(), subCol,
                                  static_cast<float>(w - pad * 2 - dp(76)));

            if (it.recent && !it.active) {
                RECT rRec = { rc.right - dp(54), rc.top + dp(9), rc.right - dp(6), rc.top + dp(27) };
                GdiPlusDraw::roundedCard(g, rRec, GdiPlusDraw::dpf(5.0f), Theme::ACCENT_PURPLE_BG, Theme::ACCENT_PURPLE, 0.8f);
                GdiPlusDraw::textCenter(g, "Recent", static_cast<float>(rRec.left + rRec.right) * 0.5f,
                                        static_cast<float>(rRec.top + rRec.bottom) * 0.5f, 8.5f, true, Theme::ACCENT_PURPLE);
            }
            outItemRects.push_back(rc);
            y += rowH + gap;
        }
    }

    outManageRc = { pad, h - manageH - pad + 2, w - pad, h - pad + 2 };
    GdiPlusDraw::roundedCard(g, outManageRc, GdiPlusDraw::dpf(7.0f), Theme::BG_SHELL, Theme::BORDER_STRONG, 1.0f);
    GdiPlusDraw::textCenter(g, "Manage Profiles...", static_cast<float>(w) * 0.5f,
                            static_cast<float>(outManageRc.top + outManageRc.bottom) * 0.5f,
                            11.0f, true, Theme::ACCENT_CYAN);
}

// ============================================================================
// LayeredTooltipWindow (U4 / N14)
// ============================================================================

LayeredTooltipWindow::LayeredTooltipWindow() {}

LayeredTooltipWindow::~LayeredTooltipWindow() {
    destroy();
}

void LayeredTooltipWindow::freeSurface() {
    if (dibDC_) {
        if (dibOldBmp_) SelectObject(dibDC_, dibOldBmp_);
        if (dibBmp_) DeleteObject(dibBmp_);
        DeleteDC(dibDC_);
        dibDC_ = NULL;
        dibBmp_ = NULL;
        dibOldBmp_ = NULL;
        dibBits_ = NULL;
        dibW_ = 0;
        dibH_ = 0;
    }
}

bool LayeredTooltipWindow::ensureSurface(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (dibDC_ && dibBits_ && w == dibW_ && h == dibH_) return true;
    freeSurface();
    HDC sc = GetDC(NULL);
    if (!sc) return false;
    dibDC_ = CreateCompatibleDC(sc);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dibBmp_ = CreateDIBSection(sc, &bmi, DIB_RGB_COLORS, &dibBits_, NULL, 0);
    ReleaseDC(NULL, sc);
    if (!dibDC_ || !dibBmp_ || !dibBits_) {
        freeSurface();
        return false;
    }
    dibOldBmp_ = SelectObject(dibDC_, dibBmp_);
    dibW_ = w;
    dibH_ = h;
    return true;
}

bool LayeredTooltipWindow::create(HWND ownerHwnd, HINSTANCE hInst) {
    ownerHwnd_ = ownerHwnd;
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.lpszClassName = L"PhantomTouchTooltipPopup";
        if (!RegisterClassW(&wc)) return false;
        reg = true;
    }
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(exStyle, L"PhantomTouchTooltipPopup", L"", WS_POPUP,
                            0, 0, 100, 28, ownerHwnd, NULL, hInst, NULL);
    return hwnd_ != NULL;
}

void LayeredTooltipWindow::destroy() {
    hide();
    freeSurface();
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = NULL;
}

void LayeredTooltipWindow::showAt(int screenX, int screenY, const std::string& text, bool alignRightOfAnchor) {
    if (!hwnd_ || text.empty()) {
        hide();
        return;
    }
    if (visible_ && text_ == text && lastX_ == screenX && lastY_ == screenY &&
        lastAlignRight_ == alignRightOfAnchor) {
        return;
    }
    text_ = text;
    lastX_ = screenX;
    lastY_ = screenY;
    lastAlignRight_ = alignRightOfAnchor;

    int w = dp(24) + static_cast<int>(GdiPlusDraw::dpf(static_cast<float>(text.size()) * 6.8f));
    if (w < dp(90)) w = dp(90);
    if (w > dp(280)) w = dp(280);
    int h = dp(28);

    int px = alignRightOfAnchor ? (screenX - w - dp(8)) : (screenX - w / 2);
    int py = alignRightOfAnchor ? (screenY - h / 2) : screenY;
    RECT rc = clampScreenPopupRect(px, py, w, h);

    if (!ensureSurface(w, h)) return;
    std::memset(dibBits_, 0, static_cast<size_t>(w) * h * 4);
    {
        Gdiplus::Graphics g(dibDC_);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        RECT lr = { 0, 0, w, h };
        GdiPlusDraw::roundedCard(g, lr, GdiPlusDraw::dpf(7.0f), 0xEB10151Fu, Theme::ACCENT_CYAN, 1.1f);
        GdiPlusDraw::textCenter(g, text, static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f,
                                10.5f, true, Theme::TEXT_PRIMARY);
    }

    POINT dstPt = { rc.left, rc.top };
    SIZE sz = { w, h };
    POINT srcPt = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    SetWindowPos(hwnd_, HWND_TOP, rc.left, rc.top, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateLayeredWindow(hwnd_, NULL, &dstPt, &sz, dibDC_, &srcPt, 0, &bf, ULW_ALPHA);
    visible_ = true;
}

void LayeredTooltipWindow::hide() {
    text_.clear();
    if (!visible_) return;
    visible_ = false;
    if (hwnd_ && IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

// ============================================================================
// LayeredModalPopup (X5 / X6 / X7)
// ============================================================================

LayeredModalPopup::LayeredModalPopup() {
    ZeroMemory(&screenRc_, sizeof(screenRc_));
}

LayeredModalPopup::~LayeredModalPopup() {
    destroy();
}

void LayeredModalPopup::freeSurface() {
    if (dibDC_) {
        if (dibOldBmp_) SelectObject(dibDC_, dibOldBmp_);
        if (dibBmp_) DeleteObject(dibBmp_);
        DeleteDC(dibDC_);
        dibDC_ = NULL;
        dibBmp_ = NULL;
        dibOldBmp_ = NULL;
        dibBits_ = NULL;
        dibW_ = 0;
        dibH_ = 0;
    }
}

bool LayeredModalPopup::ensureSurface(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (dibDC_ && dibBits_ && w == dibW_ && h == dibH_) return true;
    freeSurface();
    HDC sc = GetDC(NULL);
    if (!sc) return false;
    dibDC_ = CreateCompatibleDC(sc);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dibBmp_ = CreateDIBSection(sc, &bmi, DIB_RGB_COLORS, &dibBits_, NULL, 0);
    ReleaseDC(NULL, sc);
    if (!dibDC_ || !dibBmp_ || !dibBits_) {
        freeSurface();
        return false;
    }
    dibOldBmp_ = SelectObject(dibDC_, dibBmp_);
    dibW_ = w;
    dibH_ = h;
    return true;
}

bool LayeredModalPopup::create(HWND ownerHwnd, HINSTANCE hInst, IOverlayPopupHost* host) {
    ownerHwnd_ = ownerHwnd;
    host_ = host;
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = LayeredModalPopup::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"PhantomTouchModalPopup";
        if (!RegisterClassW(&wc)) return false;
        reg = true;
    }
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(exStyle, L"PhantomTouchModalPopup", L"", WS_POPUP,
                            0, 0, 320, 240, ownerHwnd, NULL, hInst, this);
    return hwnd_ != NULL;
}

void LayeredModalPopup::destroy() {
    hide();
    freeSurface();
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = NULL;
}

void LayeredModalPopup::showBounds(const RECT& screenRc) {
    if (!hwnd_) return;
    int w = screenRc.right - screenRc.left;
    int h = screenRc.bottom - screenRc.top;
    if (w < 64 || h < 64) {
        hide();
        return;
    }
    screenRc_ = clampScreenPopupRect(screenRc.left, screenRc.top, w, h);
    SetWindowPos(hwnd_, HWND_TOP, screenRc_.left, screenRc_.top, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    repaint();
}

void LayeredModalPopup::repaint() {
    if (!hwnd_ || !visible_ || !host_) return;
    int w = screenRc_.right - screenRc_.left;
    int h = screenRc_.bottom - screenRc_.top;
    if (w < 64 || h < 64) return;
    if (!ensureSurface(w, h)) return;

    std::memset(dibBits_, 0, static_cast<size_t>(w) * h * 4);
    {
        Gdiplus::Graphics g(dibDC_);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        host_->onPopupPaint(g, w, h);
    }
    POINT dstPt = { screenRc_.left, screenRc_.top };
    SIZE sz = { w, h };
    POINT srcPt = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd_, NULL, &dstPt, &sz, dibDC_, &srcPt, 0, &bf, ULW_ALPHA);
}

void LayeredModalPopup::hide() {
    if (!visible_) return;
    visible_ = false;
    if (hwnd_ && IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

LRESULT CALLBACK LayeredModalPopup::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LayeredModalPopup* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<LayeredModalPopup*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<LayeredModalPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT LayeredModalPopup::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (host_) host_->onPopupHover(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_LBUTTONDOWN:
            if (host_) host_->onPopupClick(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_MOUSEWHEEL:
            if (host_) host_->onPopupWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
