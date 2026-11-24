#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>

#include <string>

class GdiDraw {
public:
    static bool ptInRect(const RECT& r, int x, int y);
    static void roundedBox(HDC hdc, const RECT& rc, int radius,
                           COLORREF fill, COLORREF border, int borderW = 1, bool hollow = false);
    static void circle(HDC hdc, int cx, int cy, int r,
                       COLORREF fill, COLORREF border, int borderW = 2, bool hollow = false);
    static void centeredText(HDC hdc, const std::string& text, int cx, int cy, COLORREF color);
    static void leftText(HDC hdc, const std::string& text, int x, int cy, COLORREF color);
};
