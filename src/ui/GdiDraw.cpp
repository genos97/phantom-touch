#include "ui/GdiDraw.hpp"

bool GdiDraw::ptInRect(const RECT& r, int x, int y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

void GdiDraw::roundedBox(HDC hdc, const RECT& rc, int radius,
                         COLORREF fill, COLORREF border, int borderW, bool hollow) {
    HPEN pen = CreatePen(PS_SOLID, borderW, border);
    HBRUSH brush = hollow ? static_cast<HBRUSH>(GetStockObject(NULL_BRUSH)) : CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    if (!hollow) DeleteObject(brush);
}

void GdiDraw::circle(HDC hdc, int cx, int cy, int r,
                     COLORREF fill, COLORREF border, int borderW, bool hollow) {
    HPEN pen = CreatePen(PS_SOLID, borderW, border);
    HBRUSH brush = hollow ? static_cast<HBRUSH>(GetStockObject(NULL_BRUSH)) : CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    if (!hollow) DeleteObject(brush);
}

void GdiDraw::centeredText(HDC hdc, const std::string& text, int cx, int cy, COLORREF color) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
    TextOutA(hdc, cx - sz.cx / 2, cy - sz.cy / 2, text.c_str(), static_cast<int>(text.size()));
}

void GdiDraw::leftText(HDC hdc, const std::string& text, int x, int cy, COLORREF color) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
    TextOutA(hdc, x, cy - sz.cy / 2, text.c_str(), static_cast<int>(text.size()));
}
