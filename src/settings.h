#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

struct Settings {
    std::wstring monitorId;
    std::wstring wallpaper;
    bool enabled = false;
    bool dim = false;
    bool freeze = false;
    bool saver = false;
    bool fadeOnLeave = true;
    bool fadeOnReturn = true;
    int dimLevel = 50;
    bool useImage = false;
    bool fillImage = true;
    COLORREF background = RGB(31, 32, 36);
    bool clock = false;
    bool twelveHour = false;
    bool seconds = false;
    bool date = false;
    int clockSize = 80;
    int clockPosition = 0;
    COLORREF clockColor = RGB(245, 245, 245);
    WORD hotkey = MAKEWORD('F', 0x06); // Ctrl+Alt, as encoded by the native hotkey control.
    bool AnyEffect() const { return (dim && dimLevel > 0) || freeze || saver; }
};

struct Monitor {
    HMONITOR handle{};
    RECT rect{};
    std::wstring device;
    std::wstring id;
    std::wstring label;
    bool primary = false;
};

std::vector<Monitor> EnumerateMonitors();
std::filesystem::path SettingsPath();
Settings LoadSettings(const std::filesystem::path& path);
bool SaveSettings(const std::filesystem::path& path, const Settings& settings);
bool StartAtLogin();
bool SetStartAtLogin(bool enabled);
