#include "presentation.h"

#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr auto LayerClass = L"Foco.DisplayLayer";

BITMAPINFO BitmapInfo(unsigned width, unsigned height) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

void Fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

D2D1_COLOR_F Color(COLORREF color, float alpha = 1.0f) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                        GetBValue(color) / 255.0f, alpha);
}

std::wstring CalendarText(const SYSTEMTIME& now, const Settings& settings) {
    std::wstring text;
    if (settings.date) {
        wchar_t date[32]{};
        swprintf_s(date, L"%u年%u月%u日", static_cast<unsigned>(now.wYear),
                   static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay));
        text = date;
    }
    if (settings.weekday) {
        constexpr PCWSTR weekdays[]{L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六"};
        if (settings.date) text += L"  ";
        text += weekdays[now.wDayOfWeek];
    }
    return text;
}
}

Bitmap LoadWallpaper(const std::wstring& path) {
    if (path.empty()) return {};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))) || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame)))
        return {};
    UINT width{}, height{};
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height) return {};
    ComPtr<IWICBitmapSource> source = frame;
    constexpr UINT maximum = 4096;
    if (std::max(width, height) > maximum) {
        const double ratio = static_cast<double>(maximum) / std::max(width, height);
        width = std::max(1u, static_cast<UINT>(width * ratio));
        height = std::max(1u, static_cast<UINT>(height * ratio));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) return {};
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGR,
               WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return {};
    Bitmap bitmap{width, height, {}};
    try {
        bitmap.pixels.resize(static_cast<size_t>(width) * height);
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (FAILED(converter->CopyPixels(nullptr, width * 4, width * height * 4,
                                    reinterpret_cast<BYTE*>(bitmap.pixels.data())))) return {};
    return bitmap;
}

void PaintBitmap(HDC dc, RECT bounds, const Bitmap& bitmap, bool cover) {
    if (!bitmap || bounds.left >= bounds.right || bounds.top >= bounds.bottom) return;
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    const double horizontal = static_cast<double>(width) / bitmap.width;
    const double vertical = static_cast<double>(height) / bitmap.height;
    const double scale = cover ? std::max(horizontal, vertical) : std::min(horizontal, vertical);
    const int targetWidth = static_cast<int>(std::round(bitmap.width * scale));
    const int targetHeight = static_cast<int>(std::round(bitmap.height * scale));
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    const auto info = BitmapInfo(bitmap.width, bitmap.height);
    StretchDIBits(dc, bounds.left + (width - targetWidth) / 2,
                  bounds.top + (height - targetHeight) / 2, targetWidth, targetHeight,
                  0, 0, static_cast<int>(bitmap.width), static_cast<int>(bitmap.height),
                  bitmap.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    RestoreDC(dc, saved);
}

RECT ClockRenderer::Position(RECT screen, const Settings& settings, float scale) {
    const LONG width = screen.right - screen.left;
    const LONG height = screen.bottom - screen.top;
    const float size = static_cast<float>(settings.clockSize) * scale;
    const LONG margin = static_cast<LONG>(std::min(width, height) * 0.045);
    const LONG maximumWidth = std::max(1L, width - 2 * margin);
    const LONG maximumHeight = std::max(1L, height - 2 * margin);
    const LONG boxWidth = std::min(maximumWidth, static_cast<LONG>(std::ceil(size * 9 + 12)));
    const bool showCalendar = settings.date || settings.weekday;
    const LONG boxHeight = std::min(maximumHeight,
                                    static_cast<LONG>(std::ceil(size * (showCalendar ? 1.85f : 1.35f) + 6)));
    const auto position = settings.clockPosition;
    const LONG x = position == 0 ? screen.left + (width - boxWidth) / 2 :
                   (position == 1 || position == 3 ? screen.left + margin : screen.right - margin - boxWidth);
    const LONG y = position == 0 ? screen.top + (height - boxHeight) / 2 :
                   (position <= 2 ? screen.top + margin : screen.bottom - margin - boxHeight);
    return {x, y, x + boxWidth, y + boxHeight};
}

bool ClockRenderer::Draw(HDC dc, RECT bounds, const Settings& settings, float scale, bool alpha) {
    if (!d2d_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf())))
        return false;
    if (!write_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(write_.ReleaseAndGetAddressOf())))) return false;
    auto& target = alpha ? alphaTarget_ : opaqueTarget_;
    if (!target) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            alpha ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(d2d_->CreateDCRenderTarget(&properties, &target))) return false;
    }
    if (FAILED(target->BindDC(dc, &bounds))) return false;
    const float width = static_cast<float>(bounds.right - bounds.left);
    const float height = static_cast<float>(bounds.bottom - bounds.top);
    const float desired = static_cast<float>(settings.clockSize) * scale;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const auto calendar = CalendarText(now, settings);
    const float size = std::max(1.0f, std::min({desired, width / (settings.seconds ? 6.8f : 4.8f),
                                               height / (calendar.empty() ? 1.16f : 1.78f)}));
    ComPtr<IDWriteTextFormat> timeFormat, calendarFormat;
    if (FAILED(write_->CreateTextFormat(L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_SEMI_LIGHT,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &timeFormat))) return false;
    if (!calendar.empty() && FAILED(write_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * 0.32f, L"zh-CN", &calendarFormat))) return false;
    const auto position = settings.clockPosition;
    const auto alignment = position == 0 ? DWRITE_TEXT_ALIGNMENT_CENTER :
        (position == 1 || position == 3 ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_TRAILING);
    for (auto* format : {timeFormat.Get(), calendarFormat.Get()}) {
        if (!format) continue;
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    const unsigned hour = settings.twelveHour ? (now.wHour + 11u) % 12u + 1u : now.wHour;
    wchar_t time[40]{};
    if (settings.seconds) swprintf_s(time, L"%02u:%02u:%02u", hour, static_cast<unsigned>(now.wMinute),
                                     static_cast<unsigned>(now.wSecond));
    else swprintf_s(time, L"%02u:%02u", hour, static_cast<unsigned>(now.wMinute));
    if (settings.twelveHour) wcscat_s(time, now.wHour < 12 ? L" AM" : L" PM");

    ComPtr<ID2D1SolidColorBrush> ink, shadow;
    if (FAILED(target->CreateSolidColorBrush(Color(settings.clockColor), &ink)) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &shadow))) return false;
    target->BeginDraw();
    if (alpha) target->Clear(D2D1::ColorF(0, 0, 0, 0));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const auto draw = [&](const wchar_t* label, IDWriteTextFormat* format, float top, float bottom) {
        const auto length = static_cast<UINT32>(wcslen(label));
        target->DrawText(label, length, format, D2D1::RectF(4, top + 1, width - 2, bottom + 1), shadow.Get());
        target->DrawText(label, length, format, D2D1::RectF(3, top, width - 3, bottom), ink.Get());
    };
    const float timeBottom = calendarFormat ? height * 0.72f : height;
    draw(time, timeFormat.Get(), 0, timeBottom);
    if (calendarFormat) draw(calendar.c_str(), calendarFormat.Get(), timeBottom - 2, height);
    const auto status = target->EndDraw();
    if (status == D2DERR_RECREATE_TARGET) target.Reset();
    return SUCCEEDED(status);
}

Presentation::~Presentation() { Clear(); }

bool Presentation::Register(HINSTANCE instance) {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpszClassName = LayerClass;
    cls.lpfnWndProc = WindowProc;
    return RegisterClassW(&cls) != 0;
}

HWND Presentation::MakeLayer() {
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
        WS_EX_TOPMOST, LayerClass, L"", WS_POPUP, screen_.left, screen_.top,
        screen_.right - screen_.left, screen_.bottom - screen_.top, nullptr, nullptr,
        GetModuleHandleW(nullptr), this);
}

bool Presentation::Prepare(RECT screen, Scene scene, const Settings& settings,
                           const Bitmap* wallpaper, Bitmap frozen) {
    Clear();
    screen_ = screen;
    scene_ = scene;
    frozen_ = std::move(frozen);
    if (scene_ != Scene::Desktop && (!(base_ = MakeLayer()) ||
        !SetLayeredWindowAttributes(base_, 0, 0, LWA_ALPHA))) {
        Clear();
        return false;
    }
    if (Configure(settings, wallpaper)) return true;
    Clear();
    return false;
}

bool Presentation::Configure(const Settings& settings, const Bitmap* wallpaper) {
    settings_ = settings;
    wallpaper_ = wallpaper;
    if (scene_ == Scene::Saver && settings_.clock) {
        if (!clock_) clock_ = MakeLayer();
        if (!clock_ || !UpdateClock()) return false;
    } else {
        DropClock();
    }
    if (settings_.dim && settings_.dimLevel > 0) {
        if (!dim_) {
            dim_ = MakeLayer();
            if (!dim_ || !SetLayeredWindowAttributes(dim_, 0, 0, LWA_ALPHA)) return false;
        }
    } else if (dim_) {
        DestroyWindow(std::exchange(dim_, nullptr));
    }
    if (base_ && scene_ == Scene::Saver) InvalidateRect(base_, nullptr, FALSE);
    // A newly created clock must stay below an existing dim layer.
    if (clock_) SetWindowPos(clock_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (dim_) SetWindowPos(dim_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetProgress(progress_);
    return true;
}

void Presentation::SetProgress(double progress) {
    const auto previousAlpha = static_cast<BYTE>(std::lround(progress_ * 255));
    progress_ = std::clamp(progress, 0.0, 1.0);
    const auto alpha = static_cast<BYTE>(std::lround(progress_ * 255));
    const auto dimAlpha = static_cast<BYTE>(std::lround(progress_ * settings_.dimLevel * 2.55));
    if (base_) SetLayeredWindowAttributes(base_, 0, alpha, LWA_ALPHA);
    if (clock_ && previousAlpha != alpha) SendClock(alpha);
    if (dim_) SetLayeredWindowAttributes(dim_, 0, dimAlpha, LWA_ALPHA);
    for (HWND layer : {base_, clock_, dim_}) {
        if (layer && !IsWindowVisible(layer) && alpha) ShowWindow(layer, SW_SHOWNOACTIVATE);
    }
}

bool Presentation::UpdateClock() {
    if (!clock_) return false;
    const auto position = ClockRenderer::Position(screen_, settings_, 1.0f);
    const LONG width = position.right - position.left;
    const LONG height = position.bottom - position.top;
    if (!clockDc_ || width != clockRect_.right - clockRect_.left || height != clockRect_.bottom - clockRect_.top) {
        if (clockDc_) {
            SelectObject(clockDc_, clockOldBitmap_);
            DeleteObject(clockBitmap_);
            DeleteDC(clockDc_);
        }
        clockDc_ = CreateCompatibleDC(nullptr);
        const auto info = BitmapInfo(static_cast<unsigned>(width), static_cast<unsigned>(height));
        void* pixels{};
        clockBitmap_ = clockDc_ ? CreateDIBSection(clockDc_, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
        if (!clockBitmap_) {
            if (clockDc_) DeleteDC(std::exchange(clockDc_, nullptr));
            return false;
        }
        clockOldBitmap_ = static_cast<HBITMAP>(SelectObject(clockDc_, clockBitmap_));
    }
    clockRect_ = position;
    return text_.Draw(clockDc_, {0, 0, width, height}, settings_, 1.0f, true) &&
           SendClock(static_cast<BYTE>(std::lround(progress_ * 255)));
}

bool Presentation::SendClock(BYTE alpha) {
    if (!clock_ || !clockDc_) return false;
    POINT destination{clockRect_.left, clockRect_.top}, source{};
    SIZE size{clockRect_.right - clockRect_.left, clockRect_.bottom - clockRect_.top};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    return UpdateLayeredWindow(clock_, nullptr, &destination, &size, clockDc_, &source, 0, &blend, ULW_ALPHA);
}

void Presentation::DropClock() {
    if (clock_) DestroyWindow(std::exchange(clock_, nullptr));
    if (clockDc_) {
        SelectObject(clockDc_, clockOldBitmap_);
        DeleteObject(clockBitmap_);
        DeleteDC(std::exchange(clockDc_, nullptr));
    }
    clockBitmap_ = nullptr;
    clockOldBitmap_ = nullptr;
    clockRect_ = {};
}

void Presentation::Clear() {
    if (dim_) DestroyWindow(std::exchange(dim_, nullptr));
    DropClock();
    if (base_) DestroyWindow(std::exchange(base_, nullptr));
    frozen_ = {};
    wallpaper_ = nullptr;
    scene_ = Scene::Desktop;
    progress_ = 0;
}

void Presentation::Paint(HDC dc, HWND window) {
    RECT area{};
    GetClientRect(window, &area);
    if (window == dim_) {
        Fill(dc, area, RGB(0, 0, 0));
    } else if (window == base_) {
        if (scene_ == Scene::Freeze) PaintBitmap(dc, area, frozen_, true);
        if (scene_ == Scene::Saver) {
            Fill(dc, area, settings_.background);
            if (settings_.useImage && wallpaper_) PaintBitmap(dc, area, *wallpaper_, settings_.fillImage);
        }
    }
}

LRESULT CALLBACK Presentation::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const auto self = reinterpret_cast<Presentation*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        if (self) self->Paint(dc, hwnd);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default: return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
