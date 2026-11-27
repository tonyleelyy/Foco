#include "settings.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <shlobj.h>

namespace {
struct DisplayIdentity {
    std::wstring id;
    std::wstring name;
};

std::map<std::wstring, DisplayIdentity> DisplayIdentities() {
    std::map<std::wstring, DisplayIdentity> result;
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount{}, modeCount{};
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return result;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                               &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status != ERROR_SUCCESS) return result;
        for (UINT32 i = 0; i < pathCount; ++i) {
            const auto& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                             path.sourceInfo.adapterId, path.sourceInfo.id};
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                             path.targetInfo.adapterId, path.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
            const auto [entry, inserted] = result.try_emplace(source.viewGdiDeviceName,
                DisplayIdentity{target.monitorDevicePath, target.monitorFriendlyDeviceName});
            // Mirrored physical outputs share a source, so there is no safe unique binding.
            if (!inserted) entry->second.id.clear();
        }
        return result;
    }
    return result;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    std::string result(WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0,
                                           nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), length, result.data(),
                        static_cast<int>(result.size()), nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    std::wstring result(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length,
                                           nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(),
                        static_cast<int>(result.size()));
    return result;
}

constexpr auto RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
}

std::vector<Monitor> EnumerateMonitors() {
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(handle, &info)) {
            auto& list = *reinterpret_cast<std::vector<Monitor>*>(data);
            list.push_back({handle, info.rcMonitor, info.szDevice, {}, {},
                            (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    const auto identities = DisplayIdentities();
    for (auto& monitor : monitors) {
        const auto known = identities.find(monitor.device);
        std::wstring friendly = L"显示器";
        if (known != identities.end()) {
            monitor.id = known->second.id;
            if (!known->second.name.empty()) friendly = known->second.name;
        }
        const auto number = monitor.device.find(L"DISPLAY");
        const auto suffix = number == std::wstring::npos ? L"" : monitor.device.substr(number + 7);
        monitor.label = suffix.empty() ? friendly : suffix + L"  ·  " + friendly;
        monitor.label += L"  (" + std::to_wstring(monitor.rect.right - monitor.rect.left) +
                         L"×" + std::to_wstring(monitor.rect.bottom - monitor.rect.top) + L")";
        if (monitor.primary) monitor.label += L"  主屏";
    }
    return monitors;
}

std::filesystem::path SettingsPath() {
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return {};
    const std::filesystem::path path = std::filesystem::path(local) / L"Foco" / L"settings.txt";
    CoTaskMemFree(local);
    return path;
}

Settings LoadSettings(const std::filesystem::path& path) {
    Settings settings;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (key == "monitor" || key == "wallpaper") {
            std::string decoded;
            std::istringstream field(value);
            if (field >> std::quoted(decoded)) {
                (key == "monitor" ? settings.monitorId : settings.wallpaper) = FromUtf8(decoded);
            }
            continue;
        }
        int number{};
        std::istringstream field(value);
        if (!(field >> number)) continue;
        if (key == "enabled") settings.enabled = number != 0;
        else if (key == "dim") settings.dim = number != 0;
        else if (key == "freeze") settings.freeze = number != 0;
        else if (key == "saver") settings.saver = number != 0;
        else if (key == "fadeOnLeave") settings.fadeOnLeave = number != 0;
        else if (key == "fadeOnReturn") settings.fadeOnReturn = number != 0;
        else if (key == "leaveDelayMs" && std::find(LeaveDelaysMs.begin(), LeaveDelaysMs.end(), number) !=
                 LeaveDelaysMs.end()) settings.leaveDelayMs = number;
        else if (key == "dimLevel") settings.dimLevel = std::clamp(number, 0, 100);
        else if (key == "useImage") settings.useImage = number != 0;
        else if (key == "fillImage") settings.fillImage = number != 0;
        else if (key == "background") settings.background = static_cast<COLORREF>(number) & 0xffffff;
        else if (key == "clock") settings.clock = number != 0;
        else if (key == "twelveHour") settings.twelveHour = number != 0;
        else if (key == "seconds") settings.seconds = number != 0;
        else if (key == "date") settings.date = number != 0;
        else if (key == "weekday") settings.weekday = number != 0;
        else if (key == "clockSize") settings.clockSize = std::clamp(number, 32, 200);
        else if (key == "clockPosition") settings.clockPosition = std::clamp(number, 0, 4);
        else if (key == "clockColor") settings.clockColor = static_cast<COLORREF>(number) & 0xffffff;
        else if (key == "hotkey") settings.hotkey = static_cast<WORD>(std::clamp(number, 0, 65535));
    }
    return settings;
}

bool SaveSettings(const std::filesystem::path& path, const Settings& s) {
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << "monitor=" << std::quoted(ToUtf8(s.monitorId)) << '\n'
        << "wallpaper=" << std::quoted(ToUtf8(s.wallpaper)) << '\n'
        << "enabled=" << s.enabled << '\n' << "dim=" << s.dim << '\n'
        << "freeze=" << s.freeze << '\n' << "saver=" << s.saver << '\n'
        << "fadeOnLeave=" << s.fadeOnLeave << '\n' << "fadeOnReturn=" << s.fadeOnReturn << '\n'
        << "leaveDelayMs=" << s.leaveDelayMs << '\n'
        << "dimLevel=" << s.dimLevel << '\n' << "useImage=" << s.useImage << '\n'
        << "fillImage=" << s.fillImage << '\n' << "background=" << s.background << '\n'
        << "clock=" << s.clock << '\n' << "twelveHour=" << s.twelveHour << '\n'
        << "seconds=" << s.seconds << '\n' << "date=" << s.date << '\n' << "weekday=" << s.weekday << '\n'
        << "clockSize=" << s.clockSize << '\n' << "clockPosition=" << s.clockPosition << '\n'
        << "clockColor=" << s.clockColor << '\n' << "hotkey=" << s.hotkey << '\n';
    out.close();
    return out && MoveFileExW(temporary.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool StartAtLogin() {
    DWORD size{};
    return RegGetValueW(HKEY_CURRENT_USER, RunKey, L"Foco", RRF_RT_REG_SZ,
                        nullptr, nullptr, &size) == ERROR_SUCCESS;
}

bool SetStartAtLogin(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG status{};
    if (enabled) {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        if (length && length < ARRAYSIZE(executable)) {
            const std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --tray";
            status = RegSetValueExW(key, L"Foco", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        } else {
            status = ERROR_INVALID_NAME;
        }
    } else {
        status = RegDeleteValueW(key, L"Foco");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}
