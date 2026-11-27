#include "ui/SessionTabBar.hpp"

#include <cstdio>

namespace {

bool ptInRect(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

} // namespace

void SessionTabBar::layout(const RECT& bounds, const std::vector<SessionTabInfo>& tabs, bool allowAdd) {
    bounds_ = bounds;
    items_.clear();
    addBtnRc_ = { 0, 0, 0, 0 };
    hasAddBtn_ = false;

    const int bw = bounds.right - bounds.left;
    const int bh = bounds.bottom - bounds.top;
    if (bw <= 0 || bh <= 0 || tabs.empty()) return;

    const int padX = GdiPlusDraw::dp(8);
    const int gap = GdiPlusDraw::dp(6);
    const int tabY = bounds.top + GdiPlusDraw::dp(3);
    const int tabH = bh - GdiPlusDraw::dp(6);
    const int addW = (allowAdd && static_cast<int>(tabs.size()) < kMaxTabs) ? GdiPlusDraw::dp(28) : 0;

    int count = static_cast<int>(tabs.size());
    if (count > kMaxTabs) count = kMaxTabs;

    int availW = bw - padX * 2 - (addW > 0 ? (addW + gap) : 0) - gap * (count - 1);
    if (availW < count * GdiPlusDraw::dp(80)) availW = count * GdiPlusDraw::dp(80);
    int tabW = availW / count;
    const int maxTabW = GdiPlusDraw::dp(220);
    if (tabW > maxTabW) tabW = maxTabW;

    int curX = bounds.left + padX;
    for (int i = 0; i < count; ++i) {
        const SessionTabInfo& t = tabs[static_cast<size_t>(i)];
        ItemLayout item;
        item.index = t.index;
        item.tabRc = { curX, tabY, curX + tabW, tabY + tabH };
        if (t.canClose) {
            const int cSz = GdiPlusDraw::dp(16);
            const int cRight = item.tabRc.right - GdiPlusDraw::dp(5);
            const int cTop = tabY + (tabH - cSz) / 2;
            item.closeRc = { cRight - cSz, cTop, cRight, cTop + cSz };
        }
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%d ", i + 1);
        item.label = std::string(prefix) + (!t.title.empty() ? t.title : "Phone");
        if (!t.subtitle.empty()) {
            item.tooltip = item.label + " (" + t.subtitle + ")";
        } else {
            item.tooltip = item.label;
        }
        item.active = t.active;
        item.connected = t.connected;
        item.connecting = t.connecting;
        item.reconnecting = t.reconnecting;
        item.canClose = t.canClose;
        items_.push_back(item);
        curX += tabW + gap;
    }

    if (addW > 0 && curX + addW <= bounds.right - padX + GdiPlusDraw::dp(4)) {
        addBtnRc_ = { curX, tabY, curX + addW, tabY + tabH };
        hasAddBtn_ = true;
    }
}

void SessionTabBar::paint(Gdiplus::Graphics& g, const RECT& bounds,
                          const std::vector<SessionTabInfo>& tabs, bool allowAdd,
                          int hoverX, int hoverY) {
    layout(bounds, tabs, allowAdd);
    if (items_.empty()) return;

    GdiPlusDraw::fillRect(g, bounds_, Theme::BG_SHELL);
    GdiPlusDraw::hLine(g, bounds_.left, bounds_.right, bounds_.bottom - 1, Theme::BORDER_CHROME);

    for (size_t i = 0; i < items_.size(); ++i) {
        const ItemLayout& item = items_[i];
        bool hovTab = ptInRect(item.tabRc, hoverX, hoverY);
        bool hovClose = item.canClose && ptInRect(item.closeRc, hoverX, hoverY);

        uint32_t bg = item.active
            ? Theme::BG_CARD_HOVER
            : (hovTab ? Theme::BG_CARD : Theme::BG_DRAWER);
        uint32_t bd = item.active
            ? Theme::ACCENT_CYAN
            : (hovTab ? Theme::BORDER_STRONG : Theme::BORDER_CHROME);
        GdiPlusDraw::roundedCard(g, item.tabRc, GdiPlusDraw::dpf(6.0f), bg, bd, item.active ? 1.3f : 1.0f);

        // Status dot
        uint32_t dotCol = Theme::TEXT_MUTED;
        if (item.connected) dotCol = Theme::ACCENT_GREEN;
        else if (item.connecting || item.reconnecting) dotCol = Theme::ACCENT_AMBER;

        const float dotR = GdiPlusDraw::dpf(3.2f);
        const float dotCx = static_cast<float>(item.tabRc.left + GdiPlusDraw::dp(10));
        const float dotCy = static_cast<float>(item.tabRc.top + item.tabRc.bottom) * 0.5f;
        Gdiplus::SolidBrush dotBr(GdiPlusDraw::argb(dotCol));
        g.FillEllipse(&dotBr, dotCx - dotR, dotCy - dotR, dotR * 2.0f, dotR * 2.0f);

        const float textLeftX = dotCx + dotR + GdiPlusDraw::dpf(6.0f);
        const float textRightX = item.canClose
            ? static_cast<float>(item.closeRc.left - GdiPlusDraw::dp(4))
            : static_cast<float>(item.tabRc.right - GdiPlusDraw::dp(8));
        const float maxTextW = textRightX - textLeftX;
        if (maxTextW > 12.0f) {
            GdiPlusDraw::textLeft(g, item.label, textLeftX, dotCy, 11.0f, item.active,
                                  item.active ? Theme::TEXT_PRIMARY : Theme::TEXT_SECONDARY, maxTextW);
        }

        if (item.canClose) {
            if (hovClose) {
                GdiPlusDraw::roundedCard(g, item.closeRc, GdiPlusDraw::dpf(4.0f),
                                         Theme::ACCENT_RED_BG, Theme::ACCENT_RED, 1.0f);
            }
            const float cx = static_cast<float>(item.closeRc.left + item.closeRc.right) * 0.5f;
            const float cy = static_cast<float>(item.closeRc.top + item.closeRc.bottom) * 0.5f;
            const float d = GdiPlusDraw::dpf(3.2f);
            Gdiplus::Pen xPen(GdiPlusDraw::argb(hovClose ? Theme::ACCENT_RED : Theme::TEXT_MUTED),
                              GdiPlusDraw::dpf(1.4f));
            xPen.SetStartCap(Gdiplus::LineCapRound);
            xPen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&xPen, cx - d, cy - d, cx + d, cy + d);
            g.DrawLine(&xPen, cx + d, cy - d, cx - d, cy + d);
        }
    }

    if (hasAddBtn_) {
        bool hovAdd = ptInRect(addBtnRc_, hoverX, hoverY);
        GdiPlusDraw::roundedCard(g, addBtnRc_, GdiPlusDraw::dpf(6.0f),
                                 hovAdd ? Theme::ACCENT_CYAN_BG : Theme::BG_DRAWER,
                                 hovAdd ? Theme::ACCENT_CYAN : Theme::BORDER_SUBTLE, 1.0f);
        const float cx = static_cast<float>(addBtnRc_.left + addBtnRc_.right) * 0.5f;
        const float cy = static_cast<float>(addBtnRc_.top + addBtnRc_.bottom) * 0.5f;
        GdiPlusDraw::textCenter(g, "+", cx, cy, 13.5f, true,
                                hovAdd ? Theme::ACCENT_CYAN : Theme::TEXT_SECONDARY);
    }
}

SessionTabHit SessionTabBar::hitTest(int x, int y) const {
    if (!ptInRect(bounds_, x, y)) return SessionTabHit{};
    if (hasAddBtn_ && ptInRect(addBtnRc_, x, y)) {
        return SessionTabHit{ STAB_ADD, -1 };
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        const ItemLayout& item = items_[i];
        if (item.canClose && ptInRect(item.closeRc, x, y)) {
            return SessionTabHit{ STAB_CLOSE, item.index };
        }
        if (ptInRect(item.tabRc, x, y)) {
            return SessionTabHit{ STAB_SELECT, item.index };
        }
    }
    return SessionTabHit{};
}

bool SessionTabBar::hoverItemAt(int x, int y, int& outId, RECT& outRc) const {
    if (!ptInRect(bounds_, x, y)) return false;
    if (hasAddBtn_ && ptInRect(addBtnRc_, x, y)) {
        outId = 900;
        outRc = addBtnRc_;
        return true;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        const ItemLayout& item = items_[i];
        if (item.canClose && ptInRect(item.closeRc, x, y)) {
            outId = 950 + item.index;
            outRc = item.closeRc;
            return true;
        }
        if (ptInRect(item.tabRc, x, y)) {
            outId = 910 + item.index;
            outRc = item.tabRc;
            return true;
        }
    }
    return false;
}

std::string SessionTabBar::tooltipAt(int x, int y) const {
    if (!ptInRect(bounds_, x, y)) return std::string();
    if (hasAddBtn_ && ptInRect(addBtnRc_, x, y)) {
        return "Open New Phone Tab (Ctrl+Shift+N)  |  Cycle Tabs (Ctrl+Tab)";
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        const ItemLayout& item = items_[i];
        if (item.canClose && ptInRect(item.closeRc, x, y)) {
            return "Close Phone Tab";
        }
        if (ptInRect(item.tabRc, x, y)) {
            return item.tooltip + "  |  Strict single-focus routing (Ctrl+Tab to switch)";
        }
    }
    return std::string();
}

RECT SessionTabBar::tabRect(int idx) const {
    if (idx < 0 || idx >= static_cast<int>(items_.size())) return RECT{ 0, 0, 0, 0 };
    return items_[static_cast<size_t>(idx)].tabRc;
}
