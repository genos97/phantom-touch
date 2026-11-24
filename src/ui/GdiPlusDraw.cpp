#include "ui/GdiPlusDraw.hpp"
#include "ui/Theme.hpp"

#include <cmath>

GdiPlusSession::GdiPlusSession() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token_, &input, NULL) == Gdiplus::Ok) {
        started_ = true;
    }
}

GdiPlusSession::~GdiPlusSession() {
    if (started_) {
        Gdiplus::GdiplusShutdown(token_);
        started_ = false;
    }
}

Gdiplus::Color GdiPlusDraw::argb(uint32_t c) {
    return Gdiplus::Color(
        static_cast<BYTE>((c >> 24) & 0xFF),
        static_cast<BYTE>((c >> 16) & 0xFF),
        static_cast<BYTE>((c >> 8)  & 0xFF),
        static_cast<BYTE>(c & 0xFF));
}

std::wstring GdiPlusDraw::toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), NULL, 0);
    if (len <= 0) {
        std::wstring fallback;
        fallback.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            fallback.push_back(static_cast<wchar_t>(static_cast<unsigned char>(s[i])));
        }
        return fallback;
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

void GdiPlusDraw::addRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    if (w <= 0.0f || h <= 0.0f) return;
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d < 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x,         y,         d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y,         d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d,   0.0f, 90.0f);
    path.AddArc(x,         y + h - d, d, d,  90.0f, 90.0f);
    path.CloseFigure();
}

void GdiPlusDraw::fillRoundedRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, uint32_t fillArgb) {
    Gdiplus::GraphicsPath path;
    addRoundedRectPath(path, x, y, w, h, r);
    Gdiplus::SolidBrush br(argb(fillArgb));
    g.FillPath(&br, &path);
}

void GdiPlusDraw::strokeRoundedRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, uint32_t strokeArgb, float width) {
    Gdiplus::GraphicsPath path;
    addRoundedRectPath(path, x, y, w, h, r);
    Gdiplus::Pen pen(argb(strokeArgb), width);
    g.DrawPath(&pen, &path);
}

void GdiPlusDraw::roundedCard(Gdiplus::Graphics& g, const RECT& rc, float r,
                              uint32_t fillArgb, uint32_t borderArgb, float borderWidth) {
    float x = static_cast<float>(rc.left);
    float y = static_cast<float>(rc.top);
    float w = static_cast<float>(rc.right - rc.left);
    float h = static_cast<float>(rc.bottom - rc.top);
    if (w <= 0.0f || h <= 0.0f) return;

    Gdiplus::GraphicsPath path;
    addRoundedRectPath(path, x, y, w, h, r);
    if ((fillArgb >> 24) != 0) {
        Gdiplus::SolidBrush br(argb(fillArgb));
        g.FillPath(&br, &path);
    }
    if (borderWidth > 0.0f && (borderArgb >> 24) != 0) {
        Gdiplus::Pen pen(argb(borderArgb), borderWidth);
        g.DrawPath(&pen, &path);
    }
}

void GdiPlusDraw::fillRect(Gdiplus::Graphics& g, const RECT& rc, uint32_t fillArgb) {
    Gdiplus::SolidBrush br(argb(fillArgb));
    g.FillRectangle(&br,
                    static_cast<INT>(rc.left),
                    static_cast<INT>(rc.top),
                    static_cast<INT>(rc.right - rc.left),
                    static_cast<INT>(rc.bottom - rc.top));
}

void GdiPlusDraw::hLine(Gdiplus::Graphics& g, int x1, int x2, int y, uint32_t argbColor) {
    Gdiplus::Pen pen(argb(argbColor), 1.0f);
    g.DrawLine(&pen, static_cast<float>(x1), static_cast<float>(y),
               static_cast<float>(x2), static_cast<float>(y));
}

void GdiPlusDraw::vLine(Gdiplus::Graphics& g, int x, int y1, int y2, uint32_t argbColor) {
    Gdiplus::Pen pen(argb(argbColor), 1.0f);
    g.DrawLine(&pen, static_cast<float>(x), static_cast<float>(y1),
               static_cast<float>(x), static_cast<float>(y2));
}

void GdiPlusDraw::textLeft(Gdiplus::Graphics& g, const std::string& text,
                           float x, float cy, float sizePx, bool bold, uint32_t colorArgb, float maxW) {
    std::wstring wtxt = toWide(text);
    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::Font font(&fam, sizePx, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush br(argb(colorArgb));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    float width = (maxW > 1.0f) ? maxW : 1600.0f;
    Gdiplus::RectF layout(x, cy - sizePx * 1.1f, width, sizePx * 2.2f);
    g.DrawString(wtxt.c_str(), -1, &font, layout, &fmt, &br);
}

void GdiPlusDraw::textCenter(Gdiplus::Graphics& g, const std::string& text,
                             float cx, float cy, float sizePx, bool bold, uint32_t colorArgb) {
    std::wstring wtxt = toWide(text);
    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::Font font(&fam, sizePx, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush br(argb(colorArgb));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::PointF pt(cx, cy);
    g.DrawString(wtxt.c_str(), -1, &font, pt, &fmt, &br);
}

void GdiPlusDraw::textRight(Gdiplus::Graphics& g, const std::string& text,
                            float rightX, float cy, float sizePx, bool bold, uint32_t colorArgb) {
    std::wstring wtxt = toWide(text);
    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::Font font(&fam, sizePx, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush br(argb(colorArgb));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentFar);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::PointF pt(rightX, cy);
    g.DrawString(wtxt.c_str(), -1, &font, pt, &fmt, &br);
}

void GdiPlusDraw::drawToggleSwitch(Gdiplus::Graphics& g, const RECT& rc, bool on, uint32_t accentArgb) {
    float x = static_cast<float>(rc.left);
    float y = static_cast<float>(rc.top);
    float w = static_cast<float>(rc.right - rc.left);
    float h = static_cast<float>(rc.bottom - rc.top);
    float r = h * 0.5f;
    fillRoundedRect(g, x, y, w, h, r, on ? accentArgb : Theme::BG_TRACK);
    strokeRoundedRect(g, x, y, w, h, r, on ? accentArgb : Theme::BORDER_STRONG, 1.0f);
    float pad = 3.0f;
    float knobD = h - pad * 2.0f;
    float kx = on ? (x + w - pad - knobD) : (x + pad);
    Gdiplus::SolidBrush kbr(argb(0xFFFFFFFFu));
    g.FillEllipse(&kbr, kx, y + pad, knobD, knobD);
}

void GdiPlusDraw::drawSlider(Gdiplus::Graphics& g, const RECT& trackRc, float norm01, uint32_t accentArgb, bool hovered) {
    if (norm01 < 0.0f) norm01 = 0.0f;
    if (norm01 > 1.0f) norm01 = 1.0f;
    float x = static_cast<float>(trackRc.left);
    float y = static_cast<float>(trackRc.top);
    float w = static_cast<float>(trackRc.right - trackRc.left);
    float h = static_cast<float>(trackRc.bottom - trackRc.top);
    float cy = y + h * 0.5f;

    fillRoundedRect(g, x, y, w, h, h * 0.5f, Theme::BG_TRACK);
    float fillW = w * norm01;
    if (fillW > 2.0f) {
        fillRoundedRect(g, x, y, fillW, h, h * 0.5f, accentArgb);
    }

    float kx = x + fillW;
    float kr = hovered ? 7.5f : 6.5f;
    if (hovered) {
        uint32_t glow = (accentArgb & 0x00FFFFFFu) | 0x44000000u;
        Gdiplus::SolidBrush gbr(argb(glow));
        g.FillEllipse(&gbr, kx - kr - 3.5f, cy - kr - 3.5f, (kr + 3.5f) * 2.0f, (kr + 3.5f) * 2.0f);
    }
    Gdiplus::SolidBrush kbr(argb(accentArgb));
    Gdiplus::Pen kpen(argb(0xFFFFFFFFu), 1.8f);
    g.FillEllipse(&kbr, kx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
    g.DrawEllipse(&kpen, kx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
}

void GdiPlusDraw::drawIcon(Gdiplus::Graphics& g, UiIcon icon, float cx, float cy, float sz, uint32_t colorArgb) {
    float r = sz * 0.5f;
    Gdiplus::Pen pen(argb(colorArgb), 1.75f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush br(argb(colorArgb));

    switch (icon) {
        case ICON_LOGO: {
            // Futuristic diamond + crosshair spark
            Gdiplus::PointF pts[4] = {
                Gdiplus::PointF(cx, cy - r),
                Gdiplus::PointF(cx + r, cy),
                Gdiplus::PointF(cx, cy + r),
                Gdiplus::PointF(cx - r, cy)
            };
            g.DrawPolygon(&pen, pts, 4);
            g.FillEllipse(&br, cx - 2.5f, cy - 2.5f, 5.0f, 5.0f);
            break;
        }
        case ICON_AIM: {
            float cr = r * 0.68f;
            g.DrawEllipse(&pen, cx - cr, cy - cr, cr * 2.0f, cr * 2.0f);
            g.DrawLine(&pen, cx - r, cy, cx - cr * 0.55f, cy);
            g.DrawLine(&pen, cx + cr * 0.55f, cy, cx + r, cy);
            g.DrawLine(&pen, cx, cy - r, cx, cy - cr * 0.55f);
            g.DrawLine(&pen, cx, cy + cr * 0.55f, cx, cy + r);
            g.FillEllipse(&br, cx - 1.5f, cy - 1.5f, 3.0f, 3.0f);
            break;
        }
        case ICON_KEYMAP: {
            // Keyboard outline with keycaps
            strokeRoundedRect(g, cx - r, cy - r * 0.65f, sz, sz * 0.65f, 3.0f, colorArgb, 1.6f);
            for (int i = -1; i <= 1; ++i) {
                g.FillRectangle(&br, cx + i * r * 0.5f - 1.5f, cy - r * 0.30f, 3.0f, 2.5f);
            }
            g.DrawLine(&pen, cx - r * 0.45f, cy + r * 0.25f, cx + r * 0.45f, cy + r * 0.25f);
            break;
        }
        case ICON_SENS: {
            // 3 horizontal equalizer sliders
            float ys[3] = { cy - r * 0.55f, cy, cy + r * 0.55f };
            float ks[3] = { cx - r * 0.3f, cx + r * 0.35f, cx - r * 0.1f };
            for (int i = 0; i < 3; ++i) {
                g.DrawLine(&pen, cx - r * 0.9f, ys[i], cx + r * 0.9f, ys[i]);
                g.FillEllipse(&br, ks[i] - 2.5f, ys[i] - 2.5f, 5.0f, 5.0f);
            }
            break;
        }
        case ICON_PROFILES: {
            // Layered cards
            strokeRoundedRect(g, cx - r * 0.85f, cy - r * 0.85f, sz * 0.72f, sz * 0.72f, 2.5f, colorArgb, 1.5f);
            g.DrawLine(&pen, cx - r * 0.35f, cy + r * 0.85f, cx + r * 0.85f, cy + r * 0.85f);
            g.DrawLine(&pen, cx + r * 0.85f, cy - r * 0.35f, cx + r * 0.85f, cy + r * 0.85f);
            break;
        }
        case ICON_HUD: {
            // Eye icon
            Gdiplus::GraphicsPath p;
            p.AddArc(cx - r, cy - r * 0.75f, sz, sz * 0.75f, 200.0f, 140.0f);
            g.DrawPath(&pen, &p);
            Gdiplus::GraphicsPath p2;
            p2.AddArc(cx - r, cy - r * 0.0f, sz, sz * 0.75f, 20.0f, 140.0f);
            g.DrawPath(&pen, &p2);
            g.DrawEllipse(&pen, cx - r * 0.32f, cy - r * 0.32f, r * 0.64f, r * 0.64f);
            break;
        }
        case ICON_FIRE: {
            // Stylized lightning/trigger bolt
            Gdiplus::PointF pts[5] = {
                Gdiplus::PointF(cx + r * 0.15f, cy - r),
                Gdiplus::PointF(cx - r * 0.55f, cy + r * 0.1f),
                Gdiplus::PointF(cx + r * 0.05f, cy + r * 0.1f),
                Gdiplus::PointF(cx - r * 0.15f, cy + r),
                Gdiplus::PointF(cx + r * 0.55f, cy - r * 0.1f)
            };
            g.DrawLines(&pen, pts, 5);
            break;
        }
        case ICON_RESET:
        case ICON_REFRESH: {
            Gdiplus::GraphicsPath arc;
            arc.AddArc(cx - r * 0.8f, cy - r * 0.8f, r * 1.6f, r * 1.6f, 40.0f, 285.0f);
            g.DrawPath(&pen, &arc);
            // Arrow head near top-right
            g.DrawLine(&pen, cx + r * 0.55f, cy - r * 0.78f, cx + r * 0.75f, cy - r * 0.32f);
            g.DrawLine(&pen, cx + r * 0.25f, cy - r * 0.38f, cx + r * 0.75f, cy - r * 0.32f);
            break;
        }
        case ICON_FULLSCREEN: {
            float d = r * 0.45f;
            // 4 corner brackets
            g.DrawLine(&pen, cx - r, cy - r, cx - r + d, cy - r);
            g.DrawLine(&pen, cx - r, cy - r, cx - r, cy - r + d);
            g.DrawLine(&pen, cx + r, cy - r, cx + r - d, cy - r);
            g.DrawLine(&pen, cx + r, cy - r, cx + r, cy - r + d);
            g.DrawLine(&pen, cx - r, cy + r, cx - r + d, cy + r);
            g.DrawLine(&pen, cx - r, cy + r, cx - r, cy + r - d);
            g.DrawLine(&pen, cx + r, cy + r, cx + r - d, cy + r);
            g.DrawLine(&pen, cx + r, cy + r, cx + r, cy + r - d);
            break;
        }
        case ICON_BACK: {
            // Left triangle / chevron
            Gdiplus::PointF pts[3] = {
                Gdiplus::PointF(cx + r * 0.35f, cy - r * 0.75f),
                Gdiplus::PointF(cx - r * 0.50f, cy),
                Gdiplus::PointF(cx + r * 0.35f, cy + r * 0.75f)
            };
            g.DrawLines(&pen, pts, 3);
            break;
        }
        case ICON_HOME: {
            float cr = r * 0.65f;
            g.DrawEllipse(&pen, cx - cr, cy - cr, cr * 2.0f, cr * 2.0f);
            break;
        }
        case ICON_RECENTS: {
            strokeRoundedRect(g, cx - r * 0.62f, cy - r * 0.62f, r * 1.24f, r * 1.24f, 2.5f, colorArgb, 1.7f);
            break;
        }
        case ICON_VOL_UP:
        case ICON_VOL_DN: {
            // Speaker cone on left + plus/minus on right
            Gdiplus::PointF spk[4] = {
                Gdiplus::PointF(cx - r * 0.85f, cy - r * 0.3f),
                Gdiplus::PointF(cx - r * 0.45f, cy - r * 0.3f),
                Gdiplus::PointF(cx - r * 0.05f, cy - r * 0.72f),
                Gdiplus::PointF(cx - r * 0.05f, cy + r * 0.72f)
            };
            g.DrawLines(&pen, spk, 4);
            g.DrawLine(&pen, cx - r * 0.05f, cy + r * 0.72f, cx - r * 0.45f, cy + r * 0.3f);
            g.DrawLine(&pen, cx - r * 0.45f, cy + r * 0.3f, cx - r * 0.85f, cy + r * 0.3f);
            g.DrawLine(&pen, cx - r * 0.85f, cy - r * 0.3f, cx - r * 0.85f, cy + r * 0.3f);
            // +/- symbol
            g.DrawLine(&pen, cx + r * 0.25f, cy, cx + r * 0.88f, cy);
            if (icon == ICON_VOL_UP) {
                g.DrawLine(&pen, cx + r * 0.56f, cy - r * 0.32f, cx + r * 0.56f, cy + r * 0.32f);
            }
            break;
        }
        case ICON_CAMERA: {
            strokeRoundedRect(g, cx - r * 0.9f, cy - r * 0.55f, r * 1.8f, r * 1.3f, 2.5f, colorArgb, 1.6f);
            g.DrawLine(&pen, cx - r * 0.35f, cy - r * 0.55f, cx - r * 0.18f, cy - r * 0.85f);
            g.DrawLine(&pen, cx - r * 0.18f, cy - r * 0.85f, cx + r * 0.18f, cy - r * 0.85f);
            g.DrawLine(&pen, cx + r * 0.18f, cy - r * 0.85f, cx + r * 0.35f, cy - r * 0.55f);
            g.DrawEllipse(&pen, cx - r * 0.34f, cy - r * 0.24f, r * 0.68f, r * 0.68f);
            break;
        }
        case ICON_SETTINGS: {
            float cr = r * 0.48f;
            g.DrawEllipse(&pen, cx - cr, cy - cr, cr * 2.0f, cr * 2.0f);
            for (int i = 0; i < 6; ++i) {
                float ang = static_cast<float>(i) * 3.14159265f / 3.0f;
                float c = std::cos(ang), s = std::sin(ang);
                g.DrawLine(&pen, cx + c * r * 0.60f, cy + s * r * 0.60f,
                           cx + c * r * 0.95f, cy + s * r * 0.95f);
            }
            break;
        }
        case ICON_PIN: {
            g.DrawLine(&pen, cx - r * 0.5f, cy - r * 0.7f, cx + r * 0.5f, cy - r * 0.7f);
            g.DrawLine(&pen, cx, cy - r * 0.7f, cx, cy + r * 0.85f);
            g.DrawLine(&pen, cx - r * 0.6f, cy + r * 0.05f, cx + r * 0.6f, cy + r * 0.05f);
            break;
        }
        case ICON_MINIMIZE: {
            g.DrawLine(&pen, cx - r * 0.65f, cy + r * 0.15f, cx + r * 0.65f, cy + r * 0.15f);
            break;
        }
        case ICON_MAXIMIZE: {
            strokeRoundedRect(g, cx - r * 0.62f, cy - r * 0.62f, r * 1.24f, r * 1.24f, 1.5f, colorArgb, 1.5f);
            break;
        }
        case ICON_CLOSE: {
            float d = r * 0.62f;
            g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d);
            g.DrawLine(&pen, cx + d, cy - d, cx - d, cy + d);
            break;
        }
        case ICON_USB: {
            g.DrawLine(&pen, cx, cy - r * 0.85f, cx, cy + r * 0.85f);
            g.DrawLine(&pen, cx, cy - r * 0.85f, cx - r * 0.35f, cy - r * 0.45f);
            g.DrawLine(&pen, cx, cy - r * 0.85f, cx + r * 0.35f, cy - r * 0.45f);
            g.DrawLine(&pen, cx, cy + r * 0.2f, cx - r * 0.55f, cy - r * 0.15f);
            g.DrawLine(&pen, cx, cy + r * 0.35f, cx + r * 0.55f, cy);
            g.FillEllipse(&br, cx - 2.5f, cy + r * 0.62f, 5.0f, 5.0f);
            break;
        }
        case ICON_WIFI: {
            for (int i = 1; i <= 3; ++i) {
                float rad = r * 0.32f * static_cast<float>(i);
                Gdiplus::GraphicsPath p;
                p.AddArc(cx - rad, cy + r * 0.35f - rad, rad * 2.0f, rad * 2.0f, 225.0f, 90.0f);
                g.DrawPath(&pen, &p);
            }
            g.FillEllipse(&br, cx - 2.0f, cy + r * 0.45f, 4.0f, 4.0f);
            break;
        }
        case ICON_PLAY: {
            Gdiplus::PointF tri[3] = {
                Gdiplus::PointF(cx - r * 0.45f, cy - r * 0.72f),
                Gdiplus::PointF(cx + r * 0.72f, cy),
                Gdiplus::PointF(cx - r * 0.45f, cy + r * 0.72f)
            };
            g.FillPolygon(&br, tri, 3);
            break;
        }
        default:
            break;
    }
}

ViewportRect GdiPlusDraw::computeAspectFit(int hostW, int hostH, uint32_t phoneW, uint32_t phoneH, int padding) {
    ViewportRect out;
    int availW = hostW - padding * 2;
    int availH = hostH - padding * 2;
    if (availW <= 0 || availH <= 0) return out;
    if (phoneW == 0 || phoneH == 0) {
        phoneW = 1920;
        phoneH = 1080;
    }

    double phoneAspect = static_cast<double>(phoneW) / static_cast<double>(phoneH);
    double hostAspect  = static_cast<double>(availW) / static_cast<double>(availH);

    int fitW = availW;
    int fitH = availH;
    if (hostAspect > phoneAspect) {
        // Host is wider than phone -> pillarbox left & right
        fitH = availH;
        fitW = static_cast<int>(std::floor(static_cast<double>(fitH) * phoneAspect + 0.5));
    } else {
        // Host is taller than phone -> letterbox top & bottom
        fitW = availW;
        fitH = static_cast<int>(std::floor(static_cast<double>(fitW) / phoneAspect + 0.5));
    }
    if (fitW < 64) fitW = 64;
    if (fitH < 64) fitH = 64;
    if (fitW > availW) fitW = availW;
    if (fitH > availH) fitH = availH;

    out.w = fitW;
    out.h = fitH;
    out.x = (hostW - fitW) / 2;
    out.y = (hostH - fitH) / 2;
    return out;
}

void GdiPlusDraw::enableDarkDwmFrame(HWND hwnd) {
    if (!hwnd) return;
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (!hDwm) return;
    typedef HRESULT (WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    DwmSetWindowAttributeFn fn = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(hDwm, "DwmSetWindowAttribute"));
    if (fn) {
        BOOL dark = TRUE;
        fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        DWORD cornerPref = 2 /* DWMWCP_ROUND */;
        fn(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
        COLORREF borderCol = RGB(35, 46, 66);
        fn(hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderCol, sizeof(borderCol));
    }
    FreeLibrary(hDwm);
}
