#pragma once

#include "capture.h"
#include "settings.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

Bitmap LoadWallpaper(const std::wstring& path);
void PaintBitmap(HDC dc, RECT bounds, const Bitmap& bitmap, bool cover);

class ClockRenderer {
public:
    bool Draw(HDC dc, RECT bounds, const Settings& settings, float scale, bool alpha);
    static RECT Position(RECT screen, const Settings& settings, float scale);

private:
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> alphaTarget_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> opaqueTarget_;
};

class Presentation {
public:
    enum class Scene { Desktop, Freeze, Saver };
    Presentation() = default;
    ~Presentation();
    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    static bool Register(HINSTANCE instance);
    bool Prepare(RECT screen, Scene scene, const Settings& settings, const Bitmap* wallpaper,
                 Bitmap frozen = {});
    bool Configure(const Settings& settings, const Bitmap* wallpaper);
    void SetProgress(double progress);
    bool UpdateClock();
    void Clear();
    Scene CurrentScene() const { return scene_; }
    bool VisibleClock() const { return clock_ != nullptr; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    HWND MakeLayer();
    void Paint(HDC dc, HWND window);
    void DropClock();
    bool SendClock(BYTE alpha);

    HWND base_{}, dim_{}, clock_{};
    HDC clockDc_{};
    HBITMAP clockBitmap_{}, clockOldBitmap_{};
    RECT screen_{}, clockRect_{};
    Scene scene_{Scene::Desktop};
    Settings settings_;
    const Bitmap* wallpaper_{};
    Bitmap frozen_;
    ClockRenderer text_;
    double progress_{};
};
