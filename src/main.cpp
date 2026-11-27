#include "capture.h"
#include "presentation.h"
#include "settings.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wtsapi32.h>
#include <windowsx.h>
#include <initguid.h>
#include <oleacc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr auto WindowClass = L"Foco.Settings";
constexpr UINT TrayMessage = WM_APP + 2, OpenMessage = WM_APP + 3;
constexpr UINT_PTR TrackTimer = 1, AnimationTimer = 2, ClockTimer = 3, SaveTimer = 4, IdentifyTimer = 5,
                   PauseTimer = 6;
constexpr ULONGLONG CaptureBudget = 250, FadeIn = 180, FadeOut = 100;
constexpr UINT MillisecondsPerMinute = 60'000;
constexpr int DesignWidth = 900, DesignHeight = 643;
constexpr int AppIconResource = 101, HeaderIconSize = 39;

enum Control {
    Enable = 100, Target, Identify, Dim, DimSlider, Freeze, Saver, LeaveDelay, FadeLeave, FadeReturn, Startup,
    BackgroundType, Browse, Fit, BackgroundColor, Clock, TimeFormat, Seconds, Date, Weekday, ClockSize,
    ClockPosition, ClockColor, Hotkey, HotkeyApply, ControlEnd
};
enum TrayCommand { TraySettings = 400, TrayPause, TrayPause15Minutes, TrayPause1Hour, TrayExit };

struct Palette {
    COLORREF background, panel, panelLine, text, secondary, control, accent, accentText;
};

bool SystemDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

Palette Theme(bool dark) {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_BTNFACE),
                GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT)};
    }
    if (dark) return {RGB(23, 24, 27), RGB(31, 32, 36), RGB(54, 56, 62), RGB(245, 246, 249),
                      RGB(177, 182, 194), RGB(45, 47, 54), RGB(161, 187, 255), RGB(17, 29, 56)};
    return {RGB(244, 245, 248), RGB(255, 255, 255), RGB(222, 225, 233), RGB(27, 32, 43),
            RGB(82, 91, 111), RGB(236, 239, 246), RGB(42, 76, 168), RGB(255, 255, 255)};
}

void Fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Rounded(HDC dc, RECT rect, int radius, COLORREF color, COLORREF outline) {
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, outline);
    const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
}

void Text(HDC dc, RECT rect, std::wstring_view label, HFONT font, COLORREF color, UINT flags = DT_LEFT) {
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, label.data(), static_cast<int>(label.size()), &rect,
              flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, previous);
}

std::wstring HotkeyLabel(WORD hotkey) {
    const UINT key = LOBYTE(hotkey);
    if (!key) return L"未设置";
    const BYTE modifiers = HIBYTE(hotkey);
    std::wstring label;
    if (modifiers & HOTKEYF_CONTROL) label += L"Ctrl + ";
    if (modifiers & HOTKEYF_ALT) label += L"Alt + ";
    if (modifiers & HOTKEYF_SHIFT) label += L"Shift + ";

    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    const LONG keyInfo = static_cast<LONG>((scan << 16) | ((modifiers & HOTKEYF_EXT) ? (1u << 24) : 0));
    wchar_t name[64]{};
    if (GetKeyNameTextW(keyInfo, name, ARRAYSIZE(name))) label += name;
    else label += L"键 " + std::to_wstring(key);
    return label;
}

std::wstring PickImage(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    }
    const COMDLG_FILTERSPEC formats[]{{L"图片", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.tif;*.tiff;*.gif"},
                                     {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(ARRAYSIZE(formats), formats);
    dialog->SetTitle(L"选择副屏壁纸");
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path{};
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

bool PickColor(HWND owner, COLORREF& color) {
    static COLORREF recent[16]{};
    CHOOSECOLORW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.rgbResult = color;
    dialog.lpCustColors = recent;
    dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&dialog)) return false;
    color = dialog.rgbResult;
    return true;
}

const wchar_t* CaptureMessage(CaptureIssue status) {
    switch (status) {
    case CaptureIssue::Timeout: return L"冻结抓帧超时";
    case CaptureIssue::Unsupported: return L"当前显示环境无法冻结";
    case CaptureIssue::Protected: return L"受保护内容无法冻结";
    case CaptureIssue::Hdr: return L"冻结当前仅支持 SDR";
    default: return L"";
    }
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance), path_(SettingsPath()), settings_(LoadSettings(path_)) {}
    ~App();
    bool Create(bool trayOnly);
    HWND Window() const { return window_; }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

private:
    enum class Phase { Live, Capture, Showing, Returning };
    HWND ControlWindow(Control id) const { return controls_[static_cast<size_t>(id - Enable)]; }
    bool Checked(Control id) const { return SendMessageW(ControlWindow(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void Check(Control id, bool value);
    int ComboIndex(Control id) const;
    int SliderValue(Control id) const;
    int Px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    RECT Area(int left, int top, int right, int bottom) const;
    RECT PreviewArea() const { return Area(475, 162, 856, 315); }
    RECT SliderLabelArea(Control id) const {
        return id == DimSlider ? Area(348, 255, 410, 284) : Area(475, 493, 646, 515);
    }
    HWND Add(Control id, PCWSTR type, PCWSTR name, DWORD style);
    void Checkbox(Control id, PCWSTR label);
    void Button(Control id, PCWSTR label);
    void Combo(Control id, std::initializer_list<PCWSTR> choices);
    void Slider(Control id, int low, int high);
    static LRESULT CALLBACK CheckboxProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                          UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK ComboProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK HotkeyProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR data);
    void DrawCheckbox(HWND hwnd, HDC dc);
    void DrawButton(const DRAWITEMSTRUCT& draw);
    void DrawCombo(HWND hwnd, HDC dc);
    void DrawComboItem(const DRAWITEMSTRUCT& draw);
    void DrawHotkey(HWND hwnd, HDC dc);
    void SetupControls();
    void UpdateControls();
    void RefreshTheme();
    void RefreshFonts();
    void RefreshHeaderIcon();
    void Layout();
    void Scroll(bool horizontal, WPARAM wparam);
    void Draw(HDC dc);
    void DrawPreview(HDC dc);
    void HandleCommand(Control id, WORD notification);
    void HandleSlider(HWND slider);
    void UpdateWallpaper();
    void SaveSoon();
    void SaveNow();

    void ShowSettings();
    void AddTray();
    NOTIFYICONDATAW TrayData() const;
    void TrayMenu();
    void UpdateStatus();
    void TogglePause();
    void PauseFor(UINT durationMs);
    void ClearPause();
    void UpdateTimedPause();
    bool ApplyHotkey(WORD value, bool initial = false);
    void RefreshMonitors();
    const Monitor* TargetMonitor() const;
    void IdentifyMonitors();
    void ClearIdentifiers();
    void Arm();
    void Tick();
    bool CursorOnTarget() const;
    void StartEffect(const Monitor& monitor);
    bool ShowEffect(Presentation::Scene scene, Bitmap frozen = {});
    void CaptureReady();
    void CaptureFailed(CaptureIssue issue);
    void RestoreDesktop();
    void ResetEffect();
    void Animate(double target, ULONGLONG duration);
    void AnimationTick();
    void ClockTick();
    void ScheduleClock();
    void RenderingSettingsChanged();
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);

    HINSTANCE instance_;
    HWND window_{};
    HICON icon_{}, smallIcon_{}, headerIcon_{};
    UINT taskbarCreated_{};
    UINT dpi_{96};
    bool trayReady_{}, wtsReady_{}, paused_{}, locked_{}, disconnected_{}, asleep_{}, tracking_{}, closing_{};
    bool savePending_{}, saveFailed_{}, hotkeyProblem_{}, wallpaperProblem_{}, overlayProblem_{};
    bool startAtLogin_{}, startupProblem_{};
    bool bufferedPaint_{};
    int hotkeyId_{};
    Palette palette_{Theme(false)};
    HBRUSH panelBrush_{}, controlBrush_{};
    HFONT regular_{}, medium_{}, title_{}, heading_{}, small_{};
    std::array<HWND, ControlEnd - Enable> controls_{};
    int offsetX_{}, offsetY_{};
    std::wstring status_;
    std::filesystem::path path_;
    Settings settings_;
    std::vector<Monitor> monitors_;
    std::vector<HWND> identifiers_;
    Bitmap wallpaper_;
    Presentation presentation_;
    ClockRenderer previewClock_;
    std::unique_ptr<DesktopCapture> capture_;
    CaptureIssue captureIssue_{CaptureIssue::None};
    Phase phase_{Phase::Live};
    ULONGLONG outsideSince_{}, captureDeadline_{}, animationStart_{}, animationLength_{};
    ULONGLONG pauseUntil_{};
    unsigned captureRequest_{};
    double progress_{}, animationFrom_{}, animationTo_{};
};

App::~App() {
    capture_.reset();
    for (auto icon : {icon_, smallIcon_, headerIcon_}) if (icon) DestroyIcon(icon);
    for (auto font : {regular_, medium_, title_, heading_, small_}) if (font) DeleteObject(font);
    if (panelBrush_) DeleteObject(panelBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    if (bufferedPaint_) BufferedPaintUnInit();
}

bool App::Create(bool trayOnly) {
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES | ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&common)) return false;
    bufferedPaint_ = SUCCEEDED(BufferedPaintInit());
    LoadIconMetric(instance_, MAKEINTRESOURCEW(AppIconResource), LIM_LARGE, &icon_);
    LoadIconMetric(instance_, MAKEINTRESOURCEW(AppIconResource), LIM_SMALL, &smallIcon_);
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance_;
    cls.hIcon = icon_;
    cls.hIconSm = smallIcon_;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = WindowClass;
    cls.lpfnWndProc = WindowProc;
    if (!RegisterClassExW(&cls) || !Presentation::Register(instance_)) return false;

    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_HSCROLL | WS_VSCROLL;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    dpi_ = GetDpiForSystem();
    RECT desired{0, 0, Px(DesignWidth + 24), Px(DesignHeight + 24)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, 0, dpi_);
    const LONG width = std::min(work.right - work.left, desired.right - desired.left);
    const LONG height = std::min(work.bottom - work.top, desired.bottom - desired.top);
    if (!CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, L"Foco · 副屏减扰", style,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance_, this)) return false;
    if (!trayOnly || !trayReady_) ShowSettings();
    return true;
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* app = static_cast<App*>(create->lpCreateParams);
        app->window_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app ? app->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

RECT App::Area(int left, int top, int right, int bottom) const {
    return {Px(left) - offsetX_, Px(top) - offsetY_, Px(right) - offsetX_, Px(bottom) - offsetY_};
}

HWND App::Add(Control id, PCWSTR type, PCWSTR name, DWORD style) {
    HWND child = CreateWindowExW(0, type, name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    controls_[static_cast<size_t>(id - Enable)] = child;
    return child;
}

void App::Checkbox(Control id, PCWSTR label) {
    const auto hwnd = Add(id, WC_BUTTONW, label, BS_AUTOCHECKBOX);
    SetWindowSubclass(hwnd, CheckboxProc, 1, reinterpret_cast<DWORD_PTR>(this));
}

void App::Button(Control id, PCWSTR label) { Add(id, WC_BUTTONW, label, BS_OWNERDRAW); }

void App::Combo(Control id, std::initializer_list<PCWSTR> choices) {
    const auto hwnd = Add(id, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                          WS_VSCROLL | CBS_HASSTRINGS);
    SetWindowSubclass(hwnd, ComboProc, 1, reinterpret_cast<DWORD_PTR>(this));
    for (const auto option : choices) SendMessageW(hwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
}

void App::Slider(Control id, int low, int high) {
    const auto hwnd = Add(id, TRACKBAR_CLASSW, L"", TBS_NOTICKS | TBS_HORZ);
    SendMessageW(hwnd, TBM_SETRANGE, TRUE, MAKELPARAM(low, high));
    SendMessageW(hwnd, TBM_SETPAGESIZE, 0, 10);
}

void App::Check(Control id, bool value) {
    auto child = ControlWindow(id);
    SendMessageW(child, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(child, nullptr, FALSE);
}

int App::ComboIndex(Control id) const {
    return static_cast<int>(SendMessageW(ControlWindow(id), CB_GETCURSEL, 0, 0));
}

int App::SliderValue(Control id) const {
    return static_cast<int>(SendMessageW(ControlWindow(id), TBM_GETPOS, 0, 0));
}

void App::SetupControls() {
    Checkbox(Enable, L"启用 Foco");
    Combo(Target, {});
    Button(Identify, L"识别显示器");
    Checkbox(Dim, L"减少亮度");
    Slider(DimSlider, 0, 100);
    SetWindowTextW(ControlWindow(DimSlider), L"变暗程度");
    Checkbox(Freeze, L"冻结");
    Checkbox(Saver, L"屏保");
    Combo(LeaveDelay, {L"立即", L"150 毫秒", L"0.5 秒", L"1 秒", L"3 秒"});
    Checkbox(FadeLeave, L"移出时淡入");
    Checkbox(FadeReturn, L"移入时淡出");
    Checkbox(Startup, L"登录 Windows 后运行");
    Combo(BackgroundType, {L"纯色", L"图片"});
    Button(Browse, L"选择图片");
    Combo(Fit, {L"等比填满", L"完整显示"});
    Button(BackgroundColor, L"背景色");
    Checkbox(Clock, L"显示时钟");
    Combo(TimeFormat, {L"24 小时制", L"12 小时制"});
    Checkbox(Seconds, L"秒数");
    Checkbox(Date, L"日期");
    Checkbox(Weekday, L"星期");
    Slider(ClockSize, 32, 200);
    SetWindowTextW(ControlWindow(ClockSize), L"时钟字号");
    Combo(ClockPosition, {L"居中", L"左上", L"右上", L"左下", L"右下"});
    Button(ClockColor, L"文字色");
    const auto hotkey = Add(Hotkey, HOTKEY_CLASSW, L"", 0);
    SetWindowSubclass(hotkey, HotkeyProc, 1, reinterpret_cast<DWORD_PTR>(this));
    const auto extendedStyle = GetWindowLongPtrW(hotkey, GWL_EXSTYLE);
    SetWindowLongPtrW(hotkey, GWL_EXSTYLE, extendedStyle & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
    SetWindowPos(hotkey, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                                               SWP_NOZORDER | SWP_NOACTIVATE);
    Button(HotkeyApply, L"应用");

    ComPtr<IAccPropServices> accessibility;
    if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&accessibility)))) {
        const std::pair<Control, PCWSTR> names[]{{Target, L"目标副屏"}, {DimSlider, L"变暗程度"},
            {LeaveDelay, L"离开延时"},
            {BackgroundType, L"屏保背景类型"}, {Fit, L"壁纸缩放"}, {TimeFormat, L"时钟小时制"},
            {ClockSize, L"时钟字号"}, {ClockPosition, L"时钟位置"}, {Hotkey, L"暂停快捷键"}};
        for (const auto& [control, name] : names) {
            accessibility->SetHwndPropStr(ControlWindow(control), static_cast<DWORD>(OBJID_CLIENT),
                                          CHILDID_SELF, PROPID_ACC_NAME, name);
        }
    }

    Check(Enable, settings_.enabled);
    Check(Dim, settings_.dim);
    Check(Freeze, settings_.freeze);
    Check(Saver, settings_.saver);
    Check(FadeLeave, settings_.fadeOnLeave);
    Check(FadeReturn, settings_.fadeOnReturn);
    Check(Startup, startAtLogin_);
    Check(Clock, settings_.clock);
    Check(Seconds, settings_.seconds);
    Check(Date, settings_.date);
    Check(Weekday, settings_.weekday);
    SendMessageW(ControlWindow(DimSlider), TBM_SETPOS, TRUE, settings_.dimLevel);
    SendMessageW(ControlWindow(ClockSize), TBM_SETPOS, TRUE, settings_.clockSize);
    const auto delay = std::find(LeaveDelaysMs.begin(), LeaveDelaysMs.end(), settings_.leaveDelayMs);
    SendMessageW(ControlWindow(LeaveDelay), CB_SETCURSEL, static_cast<WPARAM>(delay - LeaveDelaysMs.begin()), 0);
    SendMessageW(ControlWindow(BackgroundType), CB_SETCURSEL, settings_.useImage, 0);
    SendMessageW(ControlWindow(Fit), CB_SETCURSEL, !settings_.fillImage, 0);
    SendMessageW(ControlWindow(TimeFormat), CB_SETCURSEL, settings_.twelveHour, 0);
    SendMessageW(ControlWindow(ClockPosition), CB_SETCURSEL, settings_.clockPosition, 0);
    SendMessageW(ControlWindow(Hotkey), HKM_SETHOTKEY, settings_.hotkey, 0);
    UpdateControls();
}

void App::UpdateControls() {
    EnableWindow(ControlWindow(DimSlider), settings_.dim);
    EnableWindow(ControlWindow(Browse), settings_.saver && settings_.useImage);
    EnableWindow(ControlWindow(Fit), settings_.saver && settings_.useImage);
    for (const auto id : {BackgroundType, BackgroundColor, Clock}) EnableWindow(ControlWindow(id), settings_.saver);
    for (const auto id : {TimeFormat, Seconds, Date, Weekday, ClockSize, ClockPosition, ClockColor})
        EnableWindow(ControlWindow(id), settings_.saver && settings_.clock);
    for (auto child : controls_) InvalidateRect(child, nullptr, FALSE);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::RefreshTheme() {
    const BOOL dark = SystemDark();
    palette_ = Theme(dark != FALSE);
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (panelBrush_) DeleteObject(panelBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    panelBrush_ = CreateSolidBrush(palette_.panel);
    controlBrush_ = CreateSolidBrush(palette_.control);
    UpdateControls();
}

void App::RefreshFonts() {
    const auto font = [this](int size, int weight) {
        return CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    };
    const auto previous = regular_;
    regular_ = font(15, FW_NORMAL);
    for (auto child : controls_) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(regular_), TRUE);
    }
    for (auto id : {Target, LeaveDelay, BackgroundType, Fit, TimeFormat, ClockPosition}) {
        SendMessageW(ControlWindow(id), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(24));
        SendMessageW(ControlWindow(id), CB_SETITEMHEIGHT, 0, Px(27));
    }
    if (previous) DeleteObject(previous);
    if (medium_) DeleteObject(medium_);
    if (title_) DeleteObject(title_);
    if (heading_) DeleteObject(heading_);
    if (small_) DeleteObject(small_);
    medium_ = font(16, FW_SEMIBOLD);
    title_ = font(33, FW_SEMIBOLD);
    heading_ = font(18, FW_SEMIBOLD);
    small_ = font(13, FW_NORMAL);
}

void App::RefreshHeaderIcon() {
    const auto size = Px(HeaderIconSize);
    HICON icon{};
    if (SUCCEEDED(LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(AppIconResource), size, size, &icon))) {
        if (const auto previous = std::exchange(headerIcon_, icon)) DestroyIcon(previous);
    }
}

void App::Layout() {
    if (!ControlWindow(Enable)) return;
    RECT client{};
    GetClientRect(window_, &client);
    for (const bool horizontal : {true, false}) {
        const int bar = horizontal ? SB_HORZ : SB_VERT;
        int& offset = horizontal ? offsetX_ : offsetY_;
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0,
                        Px(horizontal ? DesignWidth : DesignHeight) - 1,
                        static_cast<UINT>(horizontal ? client.right : client.bottom), offset};
        SetScrollInfo(window_, bar, &info, TRUE);
        offset = GetScrollPos(window_, bar);
    }
    const auto place = [&](Control id, int x, int y, int width, int height) {
        SetWindowPos(ControlWindow(id), nullptr, Px(x) - offsetX_, Px(y) - offsetY_, Px(width), Px(height),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(Enable, 733, 38, 129, 32);
    place(Target, 48, 163, 259, 220);
    place(Identify, 314, 162, 100, 30);
    place(Dim, 48, 221, 244, 28);
    place(DimSlider, 49, 257, 297, 28);
    place(Freeze, 48, 304, 351, 30);
    place(Saver, 48, 348, 351, 30);
    place(LeaveDelay, 300, 411, 114, 178);
    place(FadeLeave, 48, 444, 173, 28);
    place(FadeReturn, 224, 444, 169, 28);
    place(Startup, 48, 514, 348, 28);
    place(BackgroundType, 475, 359, 88, 150);
    place(Browse, 569, 357, 92, 29);
    place(Fit, 667, 359, 108, 150);
    place(BackgroundColor, 782, 357, 74, 29);
    place(Clock, 475, 418, 170, 28);
    place(TimeFormat, 475, 455, 124, 150);
    place(Seconds, 612, 452, 74, 30);
    place(Date, 696, 452, 74, 30);
    place(Weekday, 780, 452, 76, 30);
    place(ClockSize, 475, 518, 151, 28);
    place(ClockPosition, 658, 521, 106, 174);
    place(ClockColor, 782, 519, 74, 29);
    place(Hotkey, 666, 596, 120, 25);
    place(HotkeyApply, 795, 593, 62, 31);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::Scroll(bool horizontal, WPARAM request) {
    const int bar = horizontal ? SB_HORZ : SB_VERT;
    SCROLLINFO info{sizeof(info), SIF_ALL};
    if (!GetScrollInfo(window_, bar, &info)) return;
    switch (LOWORD(request)) {
    case SB_LINEUP: info.nPos -= Px(24); break;
    case SB_LINEDOWN: info.nPos += Px(24); break;
    case SB_PAGEUP: info.nPos -= static_cast<int>(info.nPage); break;
    case SB_PAGEDOWN: info.nPos += static_cast<int>(info.nPage); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: info.nPos = info.nTrackPos; break;
    default: return;
    }
    info.fMask = SIF_POS;
    SetScrollInfo(window_, bar, &info, TRUE);
    (horizontal ? offsetX_ : offsetY_) = GetScrollPos(window_, bar);
    Layout();
}

LRESULT CALLBACK App::CheckboxProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        app->DrawCheckbox(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK App::ComboProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        app->DrawCombo(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        app->DrawCombo(hwnd, reinterpret_cast<HDC>(wparam));
        return 0;
    }
    const auto result = DefSubclassProc(hwnd, message, wparam, lparam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SETCURSEL ||
        message == WM_ENABLE || message == WM_LBUTTONUP || message == WM_KEYUP) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return result;
}

LRESULT CALLBACK App::HotkeyProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                UINT_PTR, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        app->DrawHotkey(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        app->DrawHotkey(hwnd, reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_INPUTLANGCHANGE:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case HKM_SETHOTKEY: {
        const auto result = DefSubclassProc(hwnd, message, wparam, lparam);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    }
    default: return DefSubclassProc(hwnd, message, wparam, lparam);
    }
}

void App::DrawCheckbox(HWND hwnd, HDC dc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool master = hwnd == ControlWindow(Enable);
    const auto fill = master ? palette_.background : palette_.panel;
    Fill(dc, rect, fill);
    const LONG top = (rect.bottom - Px(18)) / 2;
    const RECT mark{Px(1), top, Px(19), top + Px(18)};
    const auto border = checked ? palette_.accent : palette_.secondary;
    Rounded(dc, mark, Px(5), checked ? palette_.accent : fill, border);
    if (checked) {
        const auto pen = CreatePen(PS_SOLID, Px(2), palette_.accentText);
        const auto old = SelectObject(dc, pen);
        MoveToEx(dc, Px(5), top + Px(9), nullptr);
        LineTo(dc, Px(8), top + Px(12));
        LineTo(dc, Px(15), top + Px(5));
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    wchar_t label[100]{};
    GetWindowTextW(hwnd, label, ARRAYSIZE(label));
    rect.left += Px(27);
    Text(dc, rect, label, master ? medium_ : regular_, enabled ? palette_.text : palette_.secondary);
    if (GetFocus() == hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        InflateRect(&rect, -1, -1);
        DrawFocusRect(dc, &rect);
    }
}

void App::DrawButton(const DRAWITEMSTRUCT& draw) {
    wchar_t label[80]{};
    GetWindowTextW(draw.hwndItem, label, ARRAYSIZE(label));
    const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const auto fill = pressed ? palette_.panelLine : palette_.control;
    Fill(draw.hDC, draw.rcItem, draw.CtlID == HotkeyApply ? palette_.background : palette_.panel);
    Rounded(draw.hDC, draw.rcItem, Px(8), fill, palette_.panelLine);
    Text(draw.hDC, draw.rcItem, label, small_, enabled ? palette_.text : palette_.secondary, DT_CENTER);
    if ((draw.itemState & ODS_FOCUS) && !(draw.itemState & ODS_NOFOCUSRECT)) {
        RECT inset = draw.rcItem;
        InflateRect(&inset, -Px(3), -Px(3));
        DrawFocusRect(draw.hDC, &inset);
    }
}

void App::DrawCombo(HWND hwnd, HDC dc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    Fill(dc, rect, palette_.panel);
    Rounded(dc, rect, Px(8), enabled ? palette_.control : palette_.panel,
            GetFocus() == hwnd ? palette_.accent : palette_.panelLine);
    wchar_t label[256]{};
    GetWindowTextW(hwnd, label, ARRAYSIZE(label));
    const RECT text{Px(8), 0, rect.right - Px(24), rect.bottom};
    Text(dc, text, label, small_, enabled ? palette_.text : palette_.secondary);
    const auto pen = CreatePen(PS_SOLID, Px(1), enabled ? palette_.text : palette_.secondary);
    const auto old = SelectObject(dc, pen);
    const LONG x = rect.right - Px(13), y = rect.bottom / 2;
    MoveToEx(dc, x - Px(3), y - Px(1), nullptr);
    LineTo(dc, x, y + Px(2));
    LineTo(dc, x + Px(4), y - Px(2));
    SelectObject(dc, old);
    DeleteObject(pen);
}

void App::DrawComboItem(const DRAWITEMSTRUCT& draw) {
    if (draw.itemState & ODS_COMBOBOXEDIT) return;
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    Fill(draw.hDC, draw.rcItem, selected ? palette_.accent : palette_.control);
    if (draw.itemID == static_cast<UINT>(-1)) return;
    const auto length = SendMessageW(draw.hwndItem, CB_GETLBTEXTLEN, draw.itemID, 0);
    if (length == CB_ERR) return;
    std::wstring label(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(draw.hwndItem, CB_GETLBTEXT, draw.itemID, reinterpret_cast<LPARAM>(label.data()));
    label.resize(static_cast<size_t>(length));
    RECT inset = draw.rcItem;
    inset.left += Px(8);
    Text(draw.hDC, inset, label, small_, selected ? palette_.accentText : palette_.text);
}

void App::DrawHotkey(HWND hwnd, HDC dc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const bool focused = GetFocus() == hwnd;
    Fill(dc, rect, palette_.background);
    Rounded(dc, rect, Px(8), palette_.control, focused ? palette_.accent : palette_.panelLine);
    InflateRect(&rect, -Px(8), 0);
    const auto hotkey = static_cast<WORD>(SendMessageW(hwnd, HKM_GETHOTKEY, 0, 0));
    const auto label = HotkeyLabel(hotkey);
    Text(dc, rect, label, small_, LOBYTE(hotkey) && IsWindowEnabled(hwnd) ? palette_.text : palette_.secondary);
    if (focused) {
        const auto previous = SelectObject(dc, small_);
        SIZE size{};
        TEXTMETRICW metrics{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        SetCaretPos(std::min(rect.left + size.cx + Px(2), rect.right),
                    std::max(0L, (rect.bottom - metrics.tmHeight) / 2));
    }
}

void App::DrawPreview(HDC dc) {
    const RECT preview = PreviewArea();
    if (!RectVisible(dc, &preview)) return;
    const int saved = SaveDC(dc);
    POINT corners[]{{preview.left, preview.top}, {preview.right + 1, preview.bottom + 1}};
    // The clip region uses device coordinates, including the buffered paint viewport offset.
    LPtoDP(dc, corners, ARRAYSIZE(corners));
    const auto clip = CreateRoundRectRgn(corners[0].x, corners[0].y, corners[1].x, corners[1].y,
                                         Px(12), Px(12));
    ExtSelectClipRgn(dc, clip, RGN_AND);
    DeleteObject(clip);
    Fill(dc, preview, settings_.saver ? settings_.background : RGB(42, 49, 66));
    if (settings_.saver) {
        if (settings_.useImage && wallpaper_) PaintBitmap(dc, preview, wallpaper_, settings_.fillImage);
        if (settings_.clock) {
            const float scale = static_cast<float>(Px(1)) * 0.46f;
            previewClock_.Draw(dc, ClockRenderer::Position(preview, settings_, scale), settings_, scale, false);
        }
    } else {
        Text(dc, preview, settings_.freeze ? L"冻结画面" : L"实时桌面", medium_, RGB(237, 240, 249), DT_CENTER);
    }
    if (settings_.dim && settings_.dimLevel > 0) {
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(settings_.dimLevel * 2.55)), 0};
        const auto sample = CreateCompatibleDC(dc);
        const auto surface = CreateCompatibleBitmap(dc, 1, 1);
        if (sample && surface) {
            const auto previous = SelectObject(sample, surface);
            SetPixelV(sample, 0, 0, RGB(0, 0, 0));
            AlphaBlend(dc, preview.left, preview.top, preview.right - preview.left, preview.bottom - preview.top,
                       sample, 0, 0, 1, 1, blend);
            SelectObject(sample, previous);
        }
        if (surface) DeleteObject(surface);
        if (sample) DeleteDC(sample);
    }
    RestoreDC(dc, saved);
}

void App::Draw(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    Fill(dc, client, palette_.background);
    if (const auto icon = headerIcon_ ? headerIcon_ : icon_) {
        const auto size = Px(HeaderIconSize);
        DrawIconEx(dc, Px(31) - offsetX_, Px(25) - offsetY_, icon, size, size, 0, nullptr, DI_NORMAL);
    }
    Text(dc, Area(82, 21, 441, 68), L"Foco", title_, palette_.text);
    Text(dc, Area(34, 72, 650, 96), L"鼠标移出副屏时，自动应用选中的效果。", regular_, palette_.secondary);
    Rounded(dc, Area(28, 113, 434, 574), Px(17), palette_.panel, palette_.panelLine);
    Rounded(dc, Area(452, 113, 874, 574), Px(17), palette_.panel, palette_.panelLine);
    Text(dc, Area(48, 126, 409, 158), L"副屏", heading_, palette_.text);
    Text(dc, Area(475, 126, 855, 157), L"效果预览", heading_, palette_.text);
    Text(dc, SliderLabelArea(DimSlider), std::to_wstring(settings_.dimLevel) + L"%", medium_, palette_.text, DT_RIGHT);
    Fill(dc, Area(48, 396, 414, 397), palette_.panelLine);
    Text(dc, Area(48, 409, 220, 439), L"切换设置", heading_, palette_.text);
    Text(dc, Area(230, 409, 292, 439), L"离开延时", small_, palette_.secondary, DT_RIGHT);
    Text(dc, Area(48, 475, 410, 497), L"关闭对应开关后，画面直接切换。", small_, palette_.secondary);
    DrawPreview(dc);
    Text(dc, Area(475, 322, 855, 351), L"屏保背景", heading_, palette_.text);
    if (settings_.useImage && !settings_.wallpaper.empty()) {
        const auto filename = std::filesystem::path(settings_.wallpaper).filename().wstring();
        Text(dc, Area(475, 390, 855, 414), filename, small_, palette_.secondary);
    }
    Text(dc, SliderLabelArea(ClockSize), L"字号  " + std::to_wstring(settings_.clockSize), small_, palette_.secondary);
    Text(dc, Area(658, 493, 760, 515), L"时钟位置", small_, palette_.secondary);
    Text(dc, Area(33, 591, 580, 625), status_, regular_, palette_.secondary);
    Text(dc, Area(568, 592, 660, 625), L"暂停快捷键", small_, palette_.secondary);
}

void App::HandleCommand(Control id, WORD notification) {
    if (notification == CBN_SELCHANGE) {
        const int chosen = ComboIndex(id);
        if (chosen == CB_ERR) return;
        switch (id) {
        case Target: {
            if (chosen > 0 && static_cast<size_t>(chosen) <= monitors_.size()) {
                const auto& monitor = monitors_[static_cast<size_t>(chosen - 1)];
                if (monitor.primary || monitor.id.empty()) {
                    RefreshMonitors();
                    return;
                }
                settings_.monitorId = monitor.id;
            } else {
                settings_.monitorId.clear();
            }
            ResetEffect();
            Arm();
            break;
        }
        case LeaveDelay:
            if (chosen < 0 || static_cast<size_t>(chosen) >= LeaveDelaysMs.size()) return;
            settings_.leaveDelayMs = LeaveDelaysMs[static_cast<size_t>(chosen)];
            if (tracking_) Tick();
            break;
        case BackgroundType:
            settings_.useImage = chosen == 1;
            UpdateWallpaper();
            RenderingSettingsChanged();
            break;
        case Fit: settings_.fillImage = chosen == 0; RenderingSettingsChanged(); break;
        case TimeFormat: settings_.twelveHour = chosen == 1; RenderingSettingsChanged(); break;
        case ClockPosition: settings_.clockPosition = chosen; RenderingSettingsChanged(); break;
        default: return;
        }
        UpdateControls();
        SaveSoon();
        return;
    }
    if (notification != BN_CLICKED) return;
    switch (id) {
    case Enable:
        settings_.enabled = Checked(Enable);
        ClearPause();
        Arm();
        break;
    case Identify: IdentifyMonitors(); return;
    case Dim: settings_.dim = Checked(Dim); RenderingSettingsChanged(); break;
    case Freeze:
    case Saver:
        settings_.freeze = Checked(Freeze);
        settings_.saver = Checked(Saver);
        if (id == Saver) UpdateWallpaper();
        ResetEffect();
        Arm();
        break;
    case FadeLeave:
        settings_.fadeOnLeave = Checked(FadeLeave);
        if (!settings_.fadeOnLeave && phase_ == Phase::Showing) Animate(1.0, 0);
        break;
    case FadeReturn:
        settings_.fadeOnReturn = Checked(FadeReturn);
        if (!settings_.fadeOnReturn && phase_ == Phase::Returning) Animate(0.0, 0);
        break;
    case Startup:
        startupProblem_ = !SetStartAtLogin(Checked(Startup));
        startAtLogin_ = StartAtLogin();
        Check(Startup, startAtLogin_);
        UpdateStatus();
        return;
    case Browse: {
        const auto path = PickImage(window_);
        if (path.empty()) return;
        settings_.wallpaper = path;
        UpdateWallpaper();
        RenderingSettingsChanged();
        break;
    }
    case BackgroundColor:
        if (!PickColor(window_, settings_.background)) return;
        RenderingSettingsChanged();
        break;
    case Clock: settings_.clock = Checked(Clock); RenderingSettingsChanged(); break;
    case Seconds: settings_.seconds = Checked(Seconds); RenderingSettingsChanged(); break;
    case Date: settings_.date = Checked(Date); RenderingSettingsChanged(); break;
    case Weekday: settings_.weekday = Checked(Weekday); RenderingSettingsChanged(); break;
    case ClockColor:
        if (!PickColor(window_, settings_.clockColor)) return;
        RenderingSettingsChanged();
        break;
    case HotkeyApply: {
        const auto key = static_cast<WORD>(SendMessageW(ControlWindow(Hotkey), HKM_GETHOTKEY, 0, 0));
        if (!ApplyHotkey(key)) return;
        break;
    }
    default: return;
    }
    UpdateControls();
    SaveSoon();
}

void App::HandleSlider(HWND slider) {
    const auto id = static_cast<Control>(GetDlgCtrlID(slider));
    if (id != DimSlider && id != ClockSize) return;
    int& value = id == DimSlider ? settings_.dimLevel : settings_.clockSize;
    const int next = SliderValue(id);
    if (value == next) return;
    value = next;
    RenderingSettingsChanged();
    SaveSoon();
    const auto label = SliderLabelArea(id), preview = PreviewArea();
    InvalidateRect(window_, &label, FALSE);
    InvalidateRect(window_, &preview, FALSE);
}

void App::UpdateWallpaper() {
    if (!settings_.saver || !settings_.useImage || settings_.wallpaper.empty()) {
        wallpaper_ = {};
        wallpaperProblem_ = false;
        return;
    }
    wallpaper_ = LoadWallpaper(settings_.wallpaper);
    wallpaperProblem_ = !wallpaper_;
}

void App::SaveSoon() {
    savePending_ = true;
    if (!SetTimer(window_, SaveTimer, 350, nullptr)) SaveNow();
}

void App::SaveNow() {
    KillTimer(window_, SaveTimer);
    if (!savePending_) return;
    saveFailed_ = !SaveSettings(path_, settings_);
    savePending_ = saveFailed_;
    if (!closing_) UpdateStatus();
}

void App::ShowSettings() {
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
    ScheduleClock();
}

NOTIFYICONDATAW App::TrayData() const {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = window_;
    icon.uID = 1;
    return icon;
}

void App::AddTray() {
    auto icon = TrayData();
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = TrayMessage;
    icon.hIcon = smallIcon_ ? smallIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, L"Foco");
    trayReady_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    if (trayReady_) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
    }
    UpdateStatus();
}

void App::TrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, TraySettings, L"打开设置");
    const UINT pauseFlags = MF_STRING | (settings_.enabled ? 0 : MF_GRAYED);
    AppendMenuW(menu, pauseFlags, TrayPause, paused_ ? L"继续运行" : L"暂停效果");
    AppendMenuW(menu, pauseFlags, TrayPause15Minutes, L"暂停 15 分钟");
    AppendMenuW(menu, pauseFlags, TrayPause1Hour, L"暂停 1 小时");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayExit, L"退出 Foco");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const auto action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                       cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
    switch (action) {
    case TraySettings: ShowSettings(); break;
    case TrayPause: TogglePause(); break;
    case TrayPause15Minutes: PauseFor(15 * MillisecondsPerMinute); break;
    case TrayPause1Hour: PauseFor(60 * MillisecondsPerMinute); break;
    case TrayExit: DestroyWindow(window_); break;
    }
}

void App::UpdateStatus() {
    std::wstring next;
    if (!settings_.enabled) next = L"尚未启用";
    else if (paused_) {
        next = L"已暂停";
        if (const auto now = GetTickCount64(); pauseUntil_ > now) {
            const auto minutes = (pauseUntil_ - now + MillisecondsPerMinute - 1) / MillisecondsPerMinute;
            next += L" · 约 " + std::to_wstring(minutes) + L" 分钟后恢复";
        }
    } else if (locked_ || asleep_ || disconnected_) next = L"系统会话暂不可用";
    else if (!TargetMonitor()) next = settings_.monitorId.empty() ? L"请选择副屏" : L"已选副屏未连接或已设为主屏";
    else if (!settings_.AnyEffect()) next = L"选择至少一种有效的效果";
    else if (phase_ == Phase::Capture) next = L"正在准备冻结画面";
    else if (const auto issue = CaptureMessage(captureIssue_); *issue) next = issue;
    else if (overlayProblem_) next = L"当前显示环境无法应用所选效果";
    else if (phase_ == Phase::Showing) next = L"副屏效果已生效";
    else next = L"正在监测副屏";
    if (wallpaperProblem_) next += L" · 壁纸不可用，使用背景色";
    if (hotkeyProblem_) next += L" · 快捷键无法注册";
    if (saveFailed_) next += L" · 设置保存失败";
    if (startupProblem_) next += L" · 登录启动设置失败";
    if (next == status_) return;
    status_ = std::move(next);
    const auto statusArea = Area(30, 589, 565, 628);
    InvalidateRect(window_, &statusArea, FALSE);
    if (trayReady_) {
        auto icon = TrayData();
        icon.uFlags = NIF_TIP | NIF_SHOWTIP;
        const std::wstring tip = L"Foco · " + status_;
        wcsncpy_s(icon.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }
}

void App::TogglePause() {
    if (!settings_.enabled) return;
    if (paused_) ClearPause();
    else paused_ = true;
    Arm();
}

void App::PauseFor(UINT durationMs) {
    if (!settings_.enabled || !SetTimer(window_, PauseTimer, std::min(durationMs, MillisecondsPerMinute), nullptr))
        return;
    paused_ = true;
    pauseUntil_ = GetTickCount64() + durationMs;
    Arm();
}

void App::ClearPause() {
    if (pauseUntil_) KillTimer(window_, PauseTimer);
    pauseUntil_ = 0;
    paused_ = false;
}

void App::UpdateTimedPause() {
    if (!pauseUntil_) return;
    const auto now = GetTickCount64();
    if (now < pauseUntil_) {
        const auto remaining = pauseUntil_ - now;
        const auto nextMinute = static_cast<UINT>((remaining - 1) % MillisecondsPerMinute + 1);
        if (SetTimer(window_, PauseTimer, nextMinute, nullptr)) {
            UpdateStatus();
            return;
        }
    }
    ClearPause();
    Arm();
}

bool App::ApplyHotkey(WORD value, bool initial) {
    if (!initial && value == settings_.hotkey && !hotkeyProblem_) return true;
    if (!LOBYTE(value)) {
        if (hotkeyId_) UnregisterHotKey(window_, std::exchange(hotkeyId_, 0));
        settings_.hotkey = 0;
        hotkeyProblem_ = false;
        UpdateStatus();
        return true;
    }
    const WORD keys = HIBYTE(value);
    const int nextId = hotkeyId_ == 1000 ? 1001 : 1000;
    UINT modifiers = MOD_NOREPEAT;
    if (keys & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
    if (keys & HOTKEYF_ALT) modifiers |= MOD_ALT;
    if (keys & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;
    hotkeyProblem_ = (modifiers == MOD_NOREPEAT) || !RegisterHotKey(window_, nextId, modifiers, LOBYTE(value));
    if (!hotkeyProblem_) {
        if (hotkeyId_) UnregisterHotKey(window_, hotkeyId_);
        hotkeyId_ = nextId;
        settings_.hotkey = value;
    }
    UpdateStatus();
    return !hotkeyProblem_;
}

const Monitor* App::TargetMonitor() const {
    if (settings_.monitorId.empty()) return nullptr;
    const Monitor* result{};
    for (const auto& monitor : monitors_) {
        if (monitor.id != settings_.monitorId) continue;
        if (result || monitor.primary) return nullptr;
        result = &monitor;
    }
    return result;
}

void App::RefreshMonitors() {
    ClearIdentifiers();
    ResetEffect();
    monitors_ = EnumerateMonitors();
    const HWND picker = ControlWindow(Target);
    SendMessageW(picker, CB_RESETCONTENT, 0, 0);
    const bool hasSecondary = std::any_of(monitors_.begin(), monitors_.end(), [](const Monitor& screen) {
        return !screen.primary && !screen.id.empty();
    });
    SendMessageW(picker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
        hasSecondary ? L"请选择副屏" : L"等待可用的扩展显示器"));
    const auto* target = TargetMonitor();
    int selected = 0;
    for (size_t i = 0; i < monitors_.size(); ++i) {
        const auto& screen = monitors_[i];
        SendMessageW(picker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(screen.label.c_str()));
        if (target == &screen) selected = static_cast<int>(i + 1);
    }
    SendMessageW(picker, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    Arm();
}

void App::IdentifyMonitors() {
    ClearIdentifiers();
    ResetEffect();
    for (const auto& monitor : monitors_) {
        const auto& r = monitor.rect;
        const LONG width = std::min(420L, r.right - r.left), height = std::min(80L, r.bottom - r.top);
        HWND label = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST, WC_STATICW, monitor.label.c_str(), WS_POPUP | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
            r.left + (r.right - r.left - width) / 2, r.top + (r.bottom - r.top - height) / 2,
            width, height, nullptr, nullptr, instance_, nullptr);
        if (!label) continue;
        identifiers_.push_back(label);
        SetLayeredWindowAttributes(label, 0, 248, LWA_ALPHA);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), TRUE);
        ShowWindow(label, SW_SHOWNOACTIVATE);
    }
    if (!identifiers_.empty() && !SetTimer(window_, IdentifyTimer, 1350, nullptr)) ClearIdentifiers();
    Arm();
}

void App::ClearIdentifiers() {
    KillTimer(window_, IdentifyTimer);
    for (const auto label : identifiers_) DestroyWindow(label);
    identifiers_.clear();
}

void App::Arm() {
    const bool wanted = !closing_ && !paused_ && !asleep_ && !locked_ && !disconnected_ && settings_.enabled &&
                        settings_.AnyEffect() && TargetMonitor() && identifiers_.empty();
    if (wanted && !tracking_) tracking_ = SetTimer(window_, TrackTimer, 25, nullptr) != 0;
    if (!wanted) {
        KillTimer(window_, TrackTimer);
        tracking_ = false;
        ResetEffect();
    }
    if (tracking_) Tick();
    UpdateStatus();
}

bool App::CursorOnTarget() const {
    POINT p{};
    const auto* monitor = TargetMonitor();
    return !monitor || !GetCursorPos(&p) || MonitorFromPoint(p, MONITOR_DEFAULTTONULL) == monitor->handle;
}

void App::Tick() {
    const auto* monitor = TargetMonitor();
    if (!tracking_ || !monitor) return;
    POINT point{};
    if (!GetCursorPos(&point)) {
        if (phase_ != Phase::Live || outsideSince_) ResetEffect();
        return;
    }
    if (MonitorFromPoint(point, MONITOR_DEFAULTTONULL) == monitor->handle) {
        outsideSince_ = 0;
        if (phase_ == Phase::Capture || phase_ == Phase::Showing) RestoreDesktop();
        return;
    }
    const auto now = GetTickCount64();
    if (!outsideSince_) outsideSince_ = now;
    if (phase_ == Phase::Capture && now >= captureDeadline_) {
        CaptureReady();
        if (phase_ == Phase::Capture) CaptureFailed(CaptureIssue::Timeout);
    }
    if ((phase_ == Phase::Live || phase_ == Phase::Returning) &&
        now - outsideSince_ >= static_cast<ULONGLONG>(settings_.leaveDelayMs)) {
        StartEffect(*monitor);
    }
}

void App::StartEffect(const Monitor& monitor) {
    KillTimer(window_, AnimationTimer);
    animationLength_ = 0;
    presentation_.Clear();
    progress_ = 0;
    overlayProblem_ = false;
    captureIssue_ = CaptureIssue::None;
    if (settings_.saver && ShowEffect(Presentation::Scene::Saver)) return;
    if (settings_.freeze) {
        if (!capture_) capture_ = std::make_unique<DesktopCapture>(window_);
        captureDeadline_ = GetTickCount64() + CaptureBudget;
        captureRequest_ = capture_->Start(monitor.handle, captureDeadline_);
        phase_ = Phase::Capture;
        UpdateStatus();
        return;
    }
    ShowEffect(Presentation::Scene::Desktop);
}

bool App::ShowEffect(Presentation::Scene scene, Bitmap frozen) {
    const auto* monitor = TargetMonitor();
    const bool prepared = monitor && presentation_.Prepare(monitor->rect, scene, settings_, &wallpaper_,
                                                            std::move(frozen));
    phase_ = Phase::Showing;
    progress_ = 0;
    if (prepared) Animate(1.0, settings_.fadeOnLeave ? FadeIn : 0);
    else overlayProblem_ = true;
    ScheduleClock();
    UpdateStatus();
    return prepared;
}

void App::CaptureReady() {
    if (!capture_) return;
    auto result = capture_->TakeResult();
    if (!result || phase_ != Phase::Capture || result->request != captureRequest_) return;
    if (CursorOnTarget()) {
        RestoreDesktop();
        return;
    }
    if (result->issue != CaptureIssue::None) {
        CaptureFailed(result->issue);
        return;
    }
    const auto* monitor = TargetMonitor();
    if (!monitor || result->bitmap.width != static_cast<unsigned>(monitor->rect.right - monitor->rect.left) ||
        result->bitmap.height != static_cast<unsigned>(monitor->rect.bottom - monitor->rect.top)) {
        CaptureFailed(CaptureIssue::Unsupported);
        return;
    }
    if (!ShowEffect(Presentation::Scene::Freeze, std::move(result->bitmap))) CaptureFailed(CaptureIssue::Unsupported);
}

void App::CaptureFailed(CaptureIssue issue) {
    if (capture_) capture_->Cancel();
    captureIssue_ = issue;
    ShowEffect(Presentation::Scene::Desktop);
}

void App::RestoreDesktop() {
    outsideSince_ = 0;
    if (phase_ == Phase::Capture) {
        ResetEffect();
    } else {
        phase_ = Phase::Returning;
        Animate(0.0, settings_.fadeOnReturn ? FadeOut : 0);
    }
    UpdateStatus();
}

void App::ResetEffect() {
    KillTimer(window_, AnimationTimer);
    if (capture_) capture_->Cancel();
    presentation_.Clear();
    phase_ = Phase::Live;
    outsideSince_ = 0;
    animationLength_ = 0;
    progress_ = 0;
    overlayProblem_ = false;
    captureIssue_ = CaptureIssue::None;
    ScheduleClock();
}

void App::Animate(double target, ULONGLONG duration) {
    if (duration && std::abs(target - progress_) > 0.001 &&
        SetTimer(window_, AnimationTimer, 15, nullptr)) {
        animationStart_ = GetTickCount64();
        animationLength_ = std::max<ULONGLONG>(1, static_cast<ULONGLONG>(
            std::lround(static_cast<double>(duration) * std::abs(target - progress_))));
        animationFrom_ = progress_;
        animationTo_ = target;
        return;
    }
    KillTimer(window_, AnimationTimer);
    animationLength_ = 0;
    progress_ = target;
    presentation_.SetProgress(target);
    if (target == 0.0) {
        presentation_.Clear();
        phase_ = Phase::Live;
        ScheduleClock();
    }
}

void App::AnimationTick() {
    if (!animationLength_) return;
    const auto elapsed = GetTickCount64() - animationStart_;
    if (elapsed >= animationLength_) {
        Animate(animationTo_, 0);
        return;
    }
    const double t = static_cast<double>(elapsed) / static_cast<double>(animationLength_);
    progress_ = std::lerp(animationFrom_, animationTo_, t * t * (3.0 - 2.0 * t));
    presentation_.SetProgress(progress_);
}

void App::ClockTick() {
    if (presentation_.VisibleClock()) presentation_.UpdateClock();
    if (IsWindowVisible(window_) && !IsIconic(window_) && settings_.saver && settings_.clock) {
        const auto preview = PreviewArea();
        InvalidateRect(window_, &preview, FALSE);
    }
    ScheduleClock();
}

void App::ScheduleClock() {
    KillTimer(window_, ClockTimer);
    if (closing_ || locked_ || asleep_ || disconnected_ || (!presentation_.VisibleClock() &&
        !(IsWindowVisible(window_) && !IsIconic(window_) && settings_.saver && settings_.clock))) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const UINT until = settings_.seconds ? 1000u - now.wMilliseconds :
                       (60u - now.wSecond) * 1000u - now.wMilliseconds;
    SetTimer(window_, ClockTimer, until, nullptr);
}

void App::RenderingSettingsChanged() {
    if ((phase_ == Phase::Showing || phase_ == Phase::Returning) &&
        !presentation_.Configure(settings_, &wallpaper_)) {
        ResetEffect();
    }
    Arm();
    ScheduleClock();
}

LRESULT App::Handle(UINT message, WPARAM wparam, LPARAM lparam) {
    if (taskbarCreated_ && message == taskbarCreated_) {
        trayReady_ = false;
        status_.clear();
        AddTray();
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(window_);
        RefreshHeaderIcon();
        startAtLogin_ = StartAtLogin();
        SetupControls();
        RefreshFonts();
        RefreshTheme();
        Layout();
        UpdateWallpaper();
        wtsReady_ = WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) != FALSE;
        RefreshMonitors();
        ApplyHotkey(settings_.hotkey, true);
        AddTray();
        return 0;
    case WM_CLOSE:
        if (trayReady_) {
            ShowWindow(window_, SW_HIDE);
            SaveNow();
            ScheduleClock();
        } else DestroyWindow(window_);
        return 0;
    case WM_QUERYENDSESSION:
        SaveNow();
        asleep_ = true;
        Arm();
        return TRUE;
    case WM_ENDSESSION:
        if (wparam) DestroyWindow(window_);
        else { asleep_ = false; Arm(); }
        return 0;
    case WM_DESTROY:
        closing_ = true;
        ClearPause();
        ClearIdentifiers();
        Arm();
        SaveNow();
        if (hotkeyId_) UnregisterHotKey(window_, hotkeyId_);
        if (wtsReady_) WTSUnRegisterSessionNotification(window_);
        if (trayReady_) {
            auto icon = TrayData();
            Shell_NotifyIconW(NIM_DELETE, &icon);
        }
        PostQuitMessage(0);
        return 0;
    case WM_SIZE: Layout(); ScheduleClock(); return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        dpi_ = HIWORD(wparam);
        RefreshHeaderIcon();
        ClearIdentifiers();
        RefreshFonts();
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        Arm();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE: RefreshTheme(); break;
    case WM_DISPLAYCHANGE: RefreshMonitors(); return 0;
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVNODES_CHANGED) RefreshMonitors();
        break;
    case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_LOCK || wparam == WTS_CONSOLE_DISCONNECT || wparam == WTS_REMOTE_DISCONNECT) {
            if (wparam == WTS_SESSION_LOCK) locked_ = true;
            else disconnected_ = true;
            ClearIdentifiers();
            Arm();
        } else if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_CONSOLE_CONNECT || wparam == WTS_REMOTE_CONNECT) {
            if (wparam == WTS_SESSION_UNLOCK) locked_ = false;
            else disconnected_ = false;
            RefreshMonitors();
            UpdateTimedPause();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) { asleep_ = true; ClearIdentifiers(); Arm(); }
        else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
            asleep_ = false;
            RefreshMonitors();
            UpdateTimedPause();
        }
        return TRUE;
    case WM_TIMECHANGE: ClockTick(); return 0;
    case WM_HOTKEY: if (wparam == static_cast<WPARAM>(hotkeyId_)) TogglePause(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        HDC buffer{};
        const auto handle = bufferedPaint_ && !IsRectEmpty(&paint.rcPaint) ?
            BeginBufferedPaint(dc, &paint.rcPaint, BPBF_COMPATIBLEBITMAP, nullptr, &buffer) : nullptr;
        Draw(handle ? buffer : dc);
        if (handle) EndBufferedPaint(handle, TRUE);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure->CtlType != ODT_COMBOBOX) break;
        measure->itemHeight = static_cast<UINT>(Px(27));
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw.CtlType == ODT_COMBOBOX) DrawComboItem(draw);
        else if (draw.CtlType == ODT_BUTTON) DrawButton(draw);
        else break;
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, palette_.text);
        SetBkColor(dc, palette_.control);
        return controlBrush_ ? reinterpret_cast<LRESULT>(controlBrush_) :
                               DefWindowProcW(window_, message, wparam, lparam);
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, palette_.secondary);
        SetBkColor(dc, palette_.panel);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_COMMAND: HandleCommand(static_cast<Control>(LOWORD(wparam)), HIWORD(wparam)); return 0;
    case WM_HSCROLL:
        if (lparam) HandleSlider(reinterpret_cast<HWND>(lparam));
        else Scroll(true, wparam);
        return 0;
    case WM_VSCROLL: if (!lparam) Scroll(false, wparam); return 0;
    case WM_MOUSEWHEEL:
        for (int amount = std::abs(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA; amount > 0; --amount)
            Scroll(false, GET_WHEEL_DELTA_WPARAM(wparam) < 0 ? SB_LINEDOWN : SB_LINEUP);
        return 0;
    case WM_TIMER:
        switch (wparam) {
        case TrackTimer: Tick(); break;
        case AnimationTimer: AnimationTick(); break;
        case ClockTimer: ClockTick(); break;
        case SaveTimer: SaveNow(); break;
        case IdentifyTimer: ClearIdentifiers(); Arm(); break;
        case PauseTimer: UpdateTimedPause(); break;
        }
        return 0;
    case CaptureReadyMessage: CaptureReady(); return 0;
    case OpenMessage: ShowSettings(); return 0;
    case TrayMessage:
        switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK: ShowSettings(); break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: TrayMenu(); break;
        }
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    const bool trayOnly = std::wstring_view(commandLine) == L"--tray";
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Foco.Desktop.Singleton");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!trayOnly) if (auto existing = FindWindowW(WindowClass, nullptr)) PostMessageW(existing, OpenMessage, 0, 0);
        CloseHandle(mutex);
        return 0;
    }
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        CloseHandle(mutex);
        return 1;
    }
    int status = 1;
    {
        App app(instance);
        if (app.Create(trayOnly)) {
            MSG message{};
            BOOL received{};
            while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                const auto settings = app.Window();
                if (settings && IsWindowVisible(settings) && IsDialogMessageW(settings, &message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            status = received == 0 ? static_cast<int>(message.wParam) : 1;
        }
    }
    CoUninitialize();
    CloseHandle(mutex);
    return status;
}
