#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <array>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// PulseWatch — Signal Quiet
// AppBar telemetria nativa. Cor = estado. Marca = glyph. Clique = histórico 60s.

constexpr UINT kBarQuiet = 32;
constexpr UINT kBarFocus = 40;
constexpr UINT_PTR kTimer = 1;
constexpr UINT_PTR kFocusTimer = 2;
constexpr UINT kFocusPollMs = 100;
constexpr UINT kAppBarMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kFocusCheckMessage = WM_APP + 3;
constexpr UINT kExitCommand = 1001;
constexpr UINT kAboutCommand = 1002;
constexpr UINT kThemeLightCommand = 1010;
constexpr UINT kThemeDarkCommand = 1011;
constexpr UINT kRefresh1Command = 1020;
constexpr UINT kRefresh2Command = 1021;
constexpr UINT kRefresh5Command = 1022;
constexpr UINT kToggleCpuCommand = 1030;
constexpr UINT kToggleRamCommand = 1031;
constexpr UINT kToggleGpuCommand = 1032;
constexpr UINT kToggleFanCommand = 1033;
constexpr UINT kAutostartCommand = 1040;
constexpr UINT kPresetQuietCommand = 1050;
constexpr UINT kPresetFocusCommand = 1051;
constexpr UINT kPresetGamingCommand = 1052;
constexpr UINT kMonitorFirstCommand = 1100;
constexpr int kExitHotkey = 1; // Ctrl+Alt+Shift+P
constexpr size_t kHistoryLen = 60;
constexpr float kIdleNetMBps = 0.5f;

namespace Tokens {
constexpr COLORREF SignalTeal = RGB(47, 158, 138);
constexpr COLORREF Amber = RGB(193, 125, 37);
constexpr COLORREF Coral = RGB(194, 65, 48);
constexpr COLORREF DarkBg = RGB(18, 22, 24);
constexpr COLORREF LightBg = RGB(244, 246, 245);
constexpr COLORREF DarkInk = RGB(245, 246, 247);
constexpr COLORREF LightInk = RGB(21, 25, 27);
constexpr COLORREF DarkMute = RGB(143, 163, 160);
constexpr COLORREF LightMute = RGB(110, 122, 120);
}

enum class DensityPreset { Quiet = 0, Focus = 1, Gaming = 2 };
enum class AlertLevel { Normal, Warning, Critical };
enum class MetricId { Down, Up, Cpu, Ram, Gpu, Fan, Count };

struct Settings {
    bool dark{}, showCpu{true}, showRam{true}, showGpu{true}, showFan{true};
    UINT refreshMs{1000};
    DensityPreset preset{DensityPreset::Quiet};
    std::wstring monitorDevice;
} g_settings;

struct MonitorChoice { std::wstring device, title; };
std::vector<MonitorChoice> g_monitors;

struct MetricHit {
    MetricId id{MetricId::Count};
    D2D1_RECT_F rect{};
    bool active{};
};

struct History {
    std::array<float, kHistoryLen> down{}, up{}, cpu{}, ram{}, gpu{}, fan{};
    size_t write{};
    size_t count{};
} g_history;

HWND g_window{};
HWND g_sparkWindow{};
APPBARDATA g_appbar{ sizeof(APPBARDATA) };
NOTIFYICONDATAW g_tray{};
HICON g_pulseIcon{};
ID2D1Factory* g_d2dFactory{};
IDWriteFactory* g_dwriteFactory{};
ID2D1HwndRenderTarget* g_renderTarget{};
ID2D1HwndRenderTarget* g_sparkTarget{};
IDWriteTextFormat* g_brandFormat{};
IDWriteTextFormat* g_metricFormat{};
IDWriteTextFormat* g_metricWarnFormat{};
IDWriteTextFormat* g_metricCritFormat{};
IDWriteTextFormat* g_sparkTitleFormat{};
IDWriteTextFormat* g_sparkValueFormat{};
bool g_appbarRegistered{};
bool g_yieldedForFocus{};
int g_fullscreenClearStreak{};
HWINEVENTHOOK g_focusHook{};
HWINEVENTHOOK g_moveHook{};
ULONGLONG g_rx{}, g_tx{}, g_idle{}, g_kernel{}, g_user{}, g_lastTick{};
double g_down{}, g_up{}, g_cpu{};
DWORD g_ram{};
int g_gpuFan{-1}, g_gpuTemp{-1}, g_gpuCountdown{};
std::array<MetricHit, 6> g_hits{};
MetricId g_sparkMetric{MetricId::Count};
std::wstring g_sparkTitle;
std::array<float, kHistoryLen> g_sparkSeries{};
size_t g_sparkSeriesCount{};

using NvmlReturn = int;
using NvmlDevice = void*;
using NvmlInitFn = NvmlReturn(__cdecl*)();
using NvmlShutdownFn = NvmlReturn(__cdecl*)();
using NvmlGetHandleFn = NvmlReturn(__cdecl*)(unsigned int, NvmlDevice*);
using NvmlGetFanFn = NvmlReturn(__cdecl*)(NvmlDevice, unsigned int*);
using NvmlGetFanV2Fn = NvmlReturn(__cdecl*)(NvmlDevice, unsigned int, unsigned int*);
using NvmlGetTemperatureFn = NvmlReturn(__cdecl*)(NvmlDevice, unsigned int, unsigned int*);

struct NvmlApi {
    HMODULE module{};
    NvmlDevice device{};
    NvmlInitFn init{};
    NvmlShutdownFn shutdown{};
    NvmlGetHandleFn getHandle{};
    NvmlGetFanFn getFan{};
    NvmlGetFanV2Fn getFanV2{};
    NvmlGetTemperatureFn getTemperature{};
    bool ready{};
} g_nvml;

UINT BarHeight() {
    return g_settings.preset == DensityPreset::Focus ? kBarFocus : kBarQuiet;
}

COLORREF InkColor() { return g_settings.dark ? Tokens::DarkInk : Tokens::LightInk; }
COLORREF MuteColor() { return g_settings.dark ? Tokens::DarkMute : Tokens::LightMute; }
COLORREF BgColor() { return g_settings.dark ? Tokens::DarkBg : Tokens::LightBg; }

AlertLevel LevelFor(double value, double warningAt, double criticalAt) {
    if (value >= criticalAt) return AlertLevel::Critical;
    if (value >= warningAt) return AlertLevel::Warning;
    return AlertLevel::Normal;
}

COLORREF ColorForLevel(AlertLevel level, COLORREF normal) {
    if (level == AlertLevel::Critical) return Tokens::Coral;
    if (level == AlertLevel::Warning) return Tokens::Amber;
    return normal;
}

std::wstring SettingsPath() {
    wchar_t localAppData[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData))) return L"PulseWatch.ini";
    std::wstring directory = std::wstring(localAppData) + L"\\PulseWatch";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\settings.ini";
}

std::wstring DiagnosticPath() {
    const auto settings = SettingsPath();
    return settings.substr(0, settings.find_last_of(L"\\/")) + L"\\pulsewatch.log";
}

void WriteDiagnostic(const std::wstring& event) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    std::wofstream log(DiagnosticPath(), std::ios::app);
    if (log) log << now.wYear << L'-' << std::setw(2) << std::setfill(L'0') << now.wMonth
                 << L'-' << std::setw(2) << now.wDay << L' ' << std::setw(2) << now.wHour
                 << L':' << std::setw(2) << now.wMinute << L':' << std::setw(2) << now.wSecond
                 << L" | " << event << L'\n';
}

bool IsAutostartEnabled() {
    HKEY key{}; wchar_t value[MAX_PATH]{}; DWORD type{}, bytes = sizeof(value);
    const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) return false;
    const bool enabled = RegQueryValueExW(key, L"PulseWatch", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key); return enabled;
}

void SetAutostart(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        const std::wstring command = L"\"" + std::wstring(executable) + L"\"";
        RegSetValueExW(key, L"PulseWatch", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        WriteDiagnostic(L"Autostart enabled");
    } else {
        RegDeleteValueW(key, L"PulseWatch");
        WriteDiagnostic(L"Autostart disabled");
    }
    RegCloseKey(key);
}

void LoadSettings() {
    const auto path = SettingsPath();
    g_settings.dark = GetPrivateProfileIntW(L"view", L"dark", 0, path.c_str()) != 0;
    g_settings.showCpu = GetPrivateProfileIntW(L"metrics", L"cpu", 1, path.c_str()) != 0;
    g_settings.showRam = GetPrivateProfileIntW(L"metrics", L"ram", 1, path.c_str()) != 0;
    g_settings.showGpu = GetPrivateProfileIntW(L"metrics", L"gpu", 1, path.c_str()) != 0;
    g_settings.showFan = GetPrivateProfileIntW(L"metrics", L"fan", 1, path.c_str()) != 0;
    const UINT refresh = GetPrivateProfileIntW(L"view", L"refreshMs", 1000, path.c_str());
    g_settings.refreshMs = refresh == 2000 || refresh == 5000 ? refresh : 1000;
    const int preset = GetPrivateProfileIntW(L"view", L"preset", 0, path.c_str());
    g_settings.preset = preset == 1 ? DensityPreset::Focus : preset == 2 ? DensityPreset::Gaming : DensityPreset::Quiet;
    wchar_t monitor[CCHDEVICENAME]{};
    GetPrivateProfileStringW(L"view", L"monitor", L"", monitor, ARRAYSIZE(monitor), path.c_str());
    g_settings.monitorDevice = monitor;
}

void SaveSettings() {
    const auto path = SettingsPath();
    auto writeNumber = [&](const wchar_t* section, const wchar_t* key, UINT value) {
        const auto text = std::to_wstring(value);
        WritePrivateProfileStringW(section, key, text.c_str(), path.c_str());
    };
    writeNumber(L"view", L"dark", g_settings.dark);
    writeNumber(L"view", L"refreshMs", g_settings.refreshMs);
    writeNumber(L"view", L"preset", static_cast<UINT>(g_settings.preset));
    WritePrivateProfileStringW(L"view", L"monitor", g_settings.monitorDevice.c_str(), path.c_str());
    writeNumber(L"metrics", L"cpu", g_settings.showCpu);
    writeNumber(L"metrics", L"ram", g_settings.showRam);
    writeNumber(L"metrics", L"gpu", g_settings.showGpu);
    writeNumber(L"metrics", L"fan", g_settings.showFan);
}

ULONGLONG FileTimeValue(const FILETIME& value) {
    return (static_cast<ULONGLONG>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

std::wstring Rate(double bytes) {
    std::wstringstream value;
    value << std::fixed << std::setprecision(bytes < 1024 ? 0 : bytes < 1024 * 1024 ? 1 : 2);
    if (bytes < 1024) value << bytes << L" B/s";
    else if (bytes < 1024 * 1024) value << bytes / 1024 << L" KB/s";
    else value << bytes / (1024 * 1024) << L" MB/s";
    return value.str();
}

void ReadNetwork(ULONGLONG& received, ULONGLONG& sent) {
    received = sent = 0;
    ULONG bytes = 0;
    if (GetIfTable(nullptr, &bytes, FALSE) != ERROR_INSUFFICIENT_BUFFER) return;
    std::vector<BYTE> buffer(bytes);
    const auto table = reinterpret_cast<PMIB_IFTABLE>(buffer.data());
    if (GetIfTable(table, &bytes, FALSE) != NO_ERROR) return;
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& row = table->table[index];
        if (row.dwType != MIB_IF_TYPE_LOOPBACK && row.dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL) {
            received += row.dwInOctets;
            sent += row.dwOutOctets;
        }
    }
}

void PushHistory() {
    const size_t i = g_history.write % kHistoryLen;
    g_history.down[i] = static_cast<float>(g_down);
    g_history.up[i] = static_cast<float>(g_up);
    g_history.cpu[i] = static_cast<float>(g_cpu);
    g_history.ram[i] = static_cast<float>(g_ram);
    g_history.gpu[i] = g_gpuTemp >= 0 ? static_cast<float>(g_gpuTemp) : 0.0f;
    g_history.fan[i] = g_gpuFan >= 0 ? static_cast<float>(g_gpuFan) : 0.0f;
    g_history.write = (g_history.write + 1) % kHistoryLen;
    if (g_history.count < kHistoryLen) ++g_history.count;
}

const std::array<float, kHistoryLen>& SeriesFor(MetricId id) {
    switch (id) {
    case MetricId::Down: return g_history.down;
    case MetricId::Up: return g_history.up;
    case MetricId::Cpu: return g_history.cpu;
    case MetricId::Ram: return g_history.ram;
    case MetricId::Gpu: return g_history.gpu;
    case MetricId::Fan: return g_history.fan;
    default: return g_history.cpu;
    }
}

void CopySeriesChronological(MetricId id, std::array<float, kHistoryLen>& out, size_t& count) {
    count = g_history.count;
    const auto& src = SeriesFor(id);
    for (size_t n = 0; n < count; ++n) {
        const size_t idx = count < kHistoryLen
            ? n
            : (g_history.write + n) % kHistoryLen;
        out[n] = src[idx];
    }
}

void Sample() {
    ULONGLONG received, sent;
    ReadNetwork(received, sent);
    const auto now = GetTickCount64();
    if (g_lastTick && now > g_lastTick) {
        const double seconds = (now - g_lastTick) / 1000.0;
        g_down = (received - g_rx) / seconds;
        g_up = (sent - g_tx) / seconds;
    }
    g_rx = received; g_tx = sent; g_lastTick = now;

    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto idleNow = FileTimeValue(idle), kernelNow = FileTimeValue(kernel), userNow = FileTimeValue(user);
        if (g_kernel) {
            const auto total = (kernelNow - g_kernel) + (userNow - g_user);
            g_cpu = total ? 100.0 * (total - (idleNow - g_idle)) / total : 0;
        }
        g_idle = idleNow; g_kernel = kernelNow; g_user = userNow;
    }
    MEMORYSTATUSEX memory{ sizeof(memory) };
    if (GlobalMemoryStatusEx(&memory)) g_ram = memory.dwMemoryLoad;
    PushHistory();
}

bool InitializeNvml() {
    g_nvml.module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_nvml.module) return false;
    g_nvml.init = reinterpret_cast<NvmlInitFn>(GetProcAddress(g_nvml.module, "nvmlInit_v2"));
    g_nvml.shutdown = reinterpret_cast<NvmlShutdownFn>(GetProcAddress(g_nvml.module, "nvmlShutdown"));
    g_nvml.getHandle = reinterpret_cast<NvmlGetHandleFn>(GetProcAddress(g_nvml.module, "nvmlDeviceGetHandleByIndex_v2"));
    g_nvml.getFan = reinterpret_cast<NvmlGetFanFn>(GetProcAddress(g_nvml.module, "nvmlDeviceGetFanSpeed"));
    g_nvml.getFanV2 = reinterpret_cast<NvmlGetFanV2Fn>(GetProcAddress(g_nvml.module, "nvmlDeviceGetFanSpeed_v2"));
    g_nvml.getTemperature = reinterpret_cast<NvmlGetTemperatureFn>(GetProcAddress(g_nvml.module, "nvmlDeviceGetTemperature"));
    if (!g_nvml.init || !g_nvml.shutdown || !g_nvml.getHandle || !g_nvml.getTemperature ||
        (!g_nvml.getFan && !g_nvml.getFanV2) || g_nvml.init() != 0 || g_nvml.getHandle(0, &g_nvml.device) != 0) {
        if (g_nvml.module) FreeLibrary(g_nvml.module);
        g_nvml = {};
        return false;
    }
    g_nvml.ready = true;
    return true;
}

void SampleGpu() {
    if (!g_nvml.ready) return;
    unsigned int temperature{}, fan{};
    if (g_nvml.getTemperature(g_nvml.device, 0, &temperature) == 0) g_gpuTemp = static_cast<int>(temperature);
    const NvmlReturn fanResult = g_nvml.getFanV2 ? g_nvml.getFanV2(g_nvml.device, 0, &fan) : g_nvml.getFan(g_nvml.device, &fan);
    if (fanResult == 0) g_gpuFan = static_cast<int>(fan);
}

void ShutdownNvml() {
    if (g_nvml.ready && g_nvml.shutdown) g_nvml.shutdown();
    if (g_nvml.module) FreeLibrary(g_nvml.module);
    g_nvml = {};
}

BOOL CALLBACK ListMonitors(HMONITOR monitor, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW info{ sizeof(info) };
    if (GetMonitorInfoW(monitor, &info)) {
        const bool primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        g_monitors.push_back({ info.szDevice, std::wstring(primary ? L"Principal - " : L"Monitor - ") + info.szDevice });
    }
    return TRUE;
}

BOOL CALLBACK FindConfiguredMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{ sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    const bool wanted = !g_settings.monitorDevice.empty() && g_settings.monitorDevice == info.szDevice;
    const bool fallback = g_settings.monitorDevice.empty() && (info.dwFlags & MONITORINFOF_PRIMARY);
    if (wanted || fallback) { *reinterpret_cast<RECT*>(data) = info.rcMonitor; return FALSE; }
    return TRUE;
}

bool GetBarMonitor(MONITORINFOEXW& info) {
    info = { sizeof(info) };
    RECT monitorRect{};
    EnumDisplayMonitors(nullptr, nullptr, FindConfiguredMonitor, reinterpret_cast<LPARAM>(&monitorRect));
    if (IsRectEmpty(&monitorRect)) {
        g_settings.monitorDevice.clear();
        EnumDisplayMonitors(nullptr, nullptr, FindConfiguredMonitor, reinterpret_cast<LPARAM>(&monitorRect));
    }
    if (IsRectEmpty(&monitorRect)) return false;
    const HMONITOR handle = MonitorFromRect(&monitorRect, MONITOR_DEFAULTTONEAREST);
    return GetMonitorInfoW(handle, &info) != FALSE;
}

bool IsShellOrDesktopWindow(HWND hwnd) {
    if (!hwnd) return true;
    wchar_t className[64]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    return _wcsicmp(className, L"Progman") == 0 ||
           _wcsicmp(className, L"WorkerW") == 0 ||
           _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0 ||
           _wcsicmp(className, L"PulseWatchTopBar") == 0 ||
           _wcsicmp(className, L"PulseWatchSpark") == 0;
}

bool RectCoversMonitor(const RECT& windowRect, const RECT& monitorRect) {
    constexpr LONG slack = 4;
    return windowRect.left <= monitorRect.left + slack &&
           windowRect.top <= monitorRect.top + slack &&
           windowRect.right >= monitorRect.right - slack &&
           windowRect.bottom >= monitorRect.bottom - slack;
}

bool IsFullscreenCandidate(HWND hwnd, const MONITORINFOEXW& barMonitor) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsShellOrDesktopWindow(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER)) return false;

    const HMONITOR windowMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!windowMonitor) return false;
    MONITORINFOEXW windowInfo{ sizeof(windowInfo) };
    if (!GetMonitorInfoW(windowMonitor, &windowInfo)) return false;
    if (wcscmp(windowInfo.szDevice, barMonitor.szDevice) != 0) return false;

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return false;
    if (!RectCoversMonitor(windowRect, barMonitor.rcMonitor)) return false;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    const bool bordered = (style & WS_CAPTION) != 0 || (style & WS_THICKFRAME) != 0;
    const bool toolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;
    if (toolWindow && bordered) return false;

    // Maximized desktop apps keep chrome; true focus surfaces are borderless / cover the monitor.
    if (bordered && (style & WS_MAXIMIZE)) return false;
    return !bordered || RectCoversMonitor(windowRect, barMonitor.rcMonitor);
}

bool ShouldYieldToFullscreen() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_PRESENTATION_MODE ||
            state == QUNS_BUSY) {
            return true;
        }
    }

    MONITORINFOEXW barMonitor{};
    if (!GetBarMonitor(barMonitor)) return false;

    HWND foreground = GetForegroundWindow();
    if (IsFullscreenCandidate(foreground, barMonitor)) return true;

    // Some browsers keep a child focused; climb to the root ownerless frame.
    if (foreground) {
        HWND root = GetAncestor(foreground, GA_ROOT);
        if (root && root != foreground && IsFullscreenCandidate(root, barMonitor)) return true;
    }
    return false;
}

void CloseSparkWindow();
void PositionAppBar();
void RemoveAppBar();

void UpdateTrayTip(bool yielded) {
    if (!g_tray.hWnd) return;
    if (yielded) {
        lstrcpynW(g_tray.szTip, L"PulseWatch · oculto (tela cheia)", ARRAYSIZE(g_tray.szTip));
    } else {
        lstrcpynW(g_tray.szTip, L"PulseWatch · Signal Quiet — Ctrl+Alt+Shift+P encerra", ARRAYSIZE(g_tray.szTip));
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void YieldBarForFocus() {
    if (g_yieldedForFocus) return;
    g_yieldedForFocus = true;
    CloseSparkWindow();
    ShowWindow(g_window, SW_HIDE);
    RemoveAppBar();
    UpdateTrayTip(true);
    WriteDiagnostic(L"Yielded bar for fullscreen focus");
}

void RestoreBarFromFocus() {
    if (!g_yieldedForFocus) return;
    g_yieldedForFocus = false;
    g_appbar = { sizeof(APPBARDATA) };
    g_appbar.hWnd = g_window;
    g_appbar.uCallbackMessage = kAppBarMessage;
    g_appbarRegistered = SHAppBarMessage(ABM_NEW, &g_appbar) != 0;
    if (!g_appbarRegistered) {
        WriteDiagnostic(L"AppBar re-registration failed after fullscreen");
        return;
    }
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    PositionAppBar();
    UpdateTrayTip(false);
    InvalidateRect(g_window, nullptr, TRUE);
    WriteDiagnostic(L"Restored bar after fullscreen focus");
}

void UpdateFocusYield() {
    const bool wantYield = ShouldYieldToFullscreen();
    if (wantYield) {
        g_fullscreenClearStreak = 0;
        YieldBarForFocus();
        return;
    }

    // Restore with a short clear streak so Alt+Tab blips do not flicker the bar.
    if (g_fullscreenClearStreak < 2) ++g_fullscreenClearStreak;
    if (g_fullscreenClearStreak >= 2) RestoreBarFromFocus();
}

void CALLBACK FocusWinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (g_window) PostMessageW(g_window, kFocusCheckMessage, 0, 0);
}

void InstallFocusHooks() {
    if (!g_focusHook) {
        g_focusHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, FocusWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    if (!g_moveHook) {
        g_moveHook = SetWinEventHook(
            EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND,
            nullptr, FocusWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
}

void RemoveFocusHooks() {
    if (g_focusHook) { UnhookWinEvent(g_focusHook); g_focusHook = nullptr; }
    if (g_moveHook) { UnhookWinEvent(g_moveHook); g_moveHook = nullptr; }
}

template <typename T> void ReleaseCom(T*& value) { if (value) { value->Release(); value = nullptr; } }

D2D1_COLOR_F D2DColor(COLORREF color, float alpha = 1.0f) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha);
}

IDWriteTextFormat* FormatForLevel(AlertLevel level) {
    if (level == AlertLevel::Critical) return g_metricCritFormat;
    if (level == AlertLevel::Warning) return g_metricWarnFormat;
    return g_metricFormat;
}

bool CreateMetricFormats() {
    ReleaseCom(g_metricFormat);
    ReleaseCom(g_metricWarnFormat);
    ReleaseCom(g_metricCritFormat);
    const wchar_t* faces[] = { L"Cascadia Mono", L"Consolas", L"Segoe UI Variable" };
    for (const auto* face : faces) {
        if (SUCCEEDED(g_dwriteFactory->CreateTextFormat(face, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"pt-BR", &g_metricFormat)) &&
            SUCCEEDED(g_dwriteFactory->CreateTextFormat(face, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"pt-BR", &g_metricWarnFormat)) &&
            SUCCEEDED(g_dwriteFactory->CreateTextFormat(face, nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"pt-BR", &g_metricCritFormat))) {
            g_metricFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_metricWarnFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_metricCritFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return true;
        }
        ReleaseCom(g_metricFormat);
        ReleaseCom(g_metricWarnFormat);
        ReleaseCom(g_metricCritFormat);
    }
    return false;
}

bool InitializeRenderer() {
    if (g_d2dFactory) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dwriteFactory)))) return false;
    if (FAILED(g_dwriteFactory->CreateTextFormat(L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"pt-BR", &g_brandFormat))) return false;
    if (!CreateMetricFormats()) return false;
    if (FAILED(g_dwriteFactory->CreateTextFormat(L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"pt-BR", &g_sparkTitleFormat)) ||
        FAILED(g_dwriteFactory->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"pt-BR", &g_sparkValueFormat))) {
        // Cascadia may be absent; retry Consolas for spark values.
        ReleaseCom(g_sparkValueFormat);
        if (FAILED(g_dwriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"pt-BR", &g_sparkValueFormat))) return false;
    }
    g_brandFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_sparkTitleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_sparkValueFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return true;
}

float WindowDpiScale() {
    const UINT dpi = GetDpiForWindow(g_window);
    return dpi ? dpi / 96.0f : 1.0f;
}

bool EnsureRenderTarget() {
    if (g_renderTarget) return true;
    RECT rect{}; GetClientRect(g_window, &rect);
    const UINT dpi = GetDpiForWindow(g_window);
    if (FAILED(g_d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE)),
        D2D1::HwndRenderTargetProperties(g_window, D2D1::SizeU(rect.right - rect.left, rect.bottom - rect.top)), &g_renderTarget))) return false;
    g_renderTarget->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    return true;
}

float MeasureText(const std::wstring& text, IDWriteTextFormat* format) {
    IDWriteTextLayout* layout{};
    if (FAILED(g_dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 500.0f, 40.0f, &layout))) return 0;
    DWRITE_TEXT_METRICS metrics{}; layout->GetMetrics(&metrics); layout->Release();
    return metrics.widthIncludingTrailingWhitespace;
}

void DrawVectorArrow(ID2D1Brush* brush, float x, float cy, bool up) {
    const float top = cy - 7.0f, bottom = cy + 7.0f;
    const float tip = up ? top : bottom, wing = up ? top + 4.0f : bottom - 4.0f;
    g_renderTarget->DrawLine(D2D1::Point2F(x + 6, top), D2D1::Point2F(x + 6, bottom), brush, 1.6f);
    g_renderTarget->DrawLine(D2D1::Point2F(x + 6, tip), D2D1::Point2F(x + 2, wing), brush, 1.6f);
    g_renderTarget->DrawLine(D2D1::Point2F(x + 6, tip), D2D1::Point2F(x + 10, wing), brush, 1.6f);
}

void DrawChipIcon(ID2D1Brush* brush, float x, float cy) {
    const D2D1_RECT_F body = D2D1::RectF(x + 3, cy - 6, x + 15, cy + 6);
    g_renderTarget->DrawRectangle(body, brush, 1.25f);
    for (int pin = 0; pin < 3; ++pin) {
        const float p = x + 5 + pin * 4;
        g_renderTarget->DrawLine(D2D1::Point2F(p, cy - 8), D2D1::Point2F(p, cy - 6), brush, 1.0f);
        g_renderTarget->DrawLine(D2D1::Point2F(p, cy + 6), D2D1::Point2F(p, cy + 8), brush, 1.0f);
    }
}

void DrawMemoryIcon(ID2D1Brush* brush, float x, float cy) {
    g_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 2, cy - 5, x + 16, cy + 5), 1.5f, 1.5f), brush, 1.25f);
    g_renderTarget->DrawLine(D2D1::Point2F(x + 5, cy - 2), D2D1::Point2F(x + 13, cy - 2), brush, 1.0f);
    g_renderTarget->DrawLine(D2D1::Point2F(x + 5, cy + 2), D2D1::Point2F(x + 10, cy + 2), brush, 1.0f);
}

void DrawThermometerIcon(ID2D1Brush* brush, float x, float cy) {
    g_renderTarget->DrawLine(D2D1::Point2F(x + 9, cy - 6), D2D1::Point2F(x + 9, cy + 4), brush, 1.6f);
    g_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 9, cy + 5), 2.6f, 2.6f), brush, 1.5f);
    g_renderTarget->DrawLine(D2D1::Point2F(x + 9, cy - 6), D2D1::Point2F(x + 12, cy - 6), brush, 1.3f);
}

void DrawFanIcon(ID2D1Brush* brush, float x, float cy) {
    const auto center = D2D1::Point2F(x + 9, cy);
    g_renderTarget->DrawEllipse(D2D1::Ellipse(center, 2.2f, 2.2f), brush, 1.3f);
    g_renderTarget->DrawLine(center, D2D1::Point2F(x + 14, cy - 4), brush, 1.5f);
    g_renderTarget->DrawLine(center, D2D1::Point2F(x + 13, cy + 5), brush, 1.5f);
    g_renderTarget->DrawLine(center, D2D1::Point2F(x + 4, cy + 2), brush, 1.5f);
}

void DrawBrandGlyph(ID2D1Brush* brush, float x, float cy) {
    g_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 9, cy), 9.0f, 9.0f), brush);
    ID2D1SolidColorBrush* stroke{};
    g_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &stroke);
    const D2D1_POINT_2F points[] = {
        D2D1::Point2F(x + 2, cy + 1), D2D1::Point2F(x + 5, cy + 1), D2D1::Point2F(x + 7.5f, cy - 4),
        D2D1::Point2F(x + 10.5f, cy + 5), D2D1::Point2F(x + 13, cy), D2D1::Point2F(x + 16, cy)
    };
    for (int i = 0; i < 5; ++i)
        g_renderTarget->DrawLine(points[i], points[i + 1], stroke, 1.6f);
    ReleaseCom(stroke);
}

void DrawMiniSparkline(float x, float y, float width, float height, MetricId id, ID2D1Brush* brush) {
    std::array<float, kHistoryLen> series{};
    size_t count{};
    CopySeriesChronological(id, series, count);
    if (count < 2) return;
    float minV = series[0], maxV = series[0];
    for (size_t i = 1; i < count; ++i) { minV = (std::min)(minV, series[i]); maxV = (std::max)(maxV, series[i]); }
    if (maxV - minV < 0.001f) maxV = minV + 1.0f;
    D2D1_POINT_2F prev{};
    for (size_t i = 0; i < count; ++i) {
        const float px = x + (width * i / (count - 1));
        const float py = y + height - ((series[i] - minV) / (maxV - minV)) * height;
        const auto pt = D2D1::Point2F(px, py);
        if (i > 0) g_renderTarget->DrawLine(prev, pt, brush, 1.1f);
        prev = pt;
    }
}

bool ShowNetworkMetrics() {
    if (g_settings.preset != DensityPreset::Gaming) return true;
    const double mbDown = g_down / (1024.0 * 1024.0);
    const double mbUp = g_up / (1024.0 * 1024.0);
    return mbDown >= kIdleNetMBps || mbUp >= kIdleNetMBps;
}

bool ShowMetric(MetricId id) {
    const bool gaming = g_settings.preset == DensityPreset::Gaming;
    switch (id) {
    case MetricId::Down:
    case MetricId::Up: return ShowNetworkMetrics();
    case MetricId::Cpu: return gaming ? false : g_settings.showCpu;
    case MetricId::Ram: return g_settings.showRam;
    case MetricId::Gpu: return gaming ? true : g_settings.showGpu;
    case MetricId::Fan: return gaming ? true : g_settings.showFan;
    default: return false;
    }
}

AlertLevel LevelForMetric(MetricId id) {
    switch (id) {
    case MetricId::Down: return LevelFor(g_down / (1024.0 * 1024.0), 25.0, 100.0);
    case MetricId::Up: return LevelFor(g_up / (1024.0 * 1024.0), 25.0, 100.0);
    case MetricId::Cpu: return LevelFor(g_cpu, 75.0, 90.0);
    case MetricId::Ram: return LevelFor(g_ram, 80.0, 92.0);
    case MetricId::Gpu: return g_gpuTemp >= 0 ? LevelFor(g_gpuTemp, 72.0, 83.0) : AlertLevel::Normal;
    case MetricId::Fan: return g_gpuFan >= 0 ? LevelFor(g_gpuFan, 70.0, 88.0) : AlertLevel::Normal;
    default: return AlertLevel::Normal;
    }
}

std::wstring LabelFor(MetricId id) {
    switch (id) {
    case MetricId::Down: return Rate(g_down);
    case MetricId::Up: return Rate(g_up);
    case MetricId::Cpu: return L"CPU " + std::to_wstring(static_cast<int>(g_cpu)) + L"%";
    case MetricId::Ram: return L"RAM " + std::to_wstring(g_ram) + L"%";
    case MetricId::Gpu: return g_gpuTemp >= 0 ? L"GPU " + std::to_wstring(g_gpuTemp) + L"\u00B0C" : L"GPU --";
    case MetricId::Fan: return g_gpuFan >= 0 ? L"FAN " + std::to_wstring(g_gpuFan) + L"%" : L"FAN --";
    default: return L"";
    }
}

std::wstring TitleFor(MetricId id) {
    switch (id) {
    case MetricId::Down: return L"Download · 60s";
    case MetricId::Up: return L"Upload · 60s";
    case MetricId::Cpu: return L"CPU · 60s";
    case MetricId::Ram: return L"RAM · 60s";
    case MetricId::Gpu: return L"GPU temperatura · 60s";
    case MetricId::Fan: return L"Ventoinha GPU · 60s";
    default: return L"Metrica";
    }
}

void PaintDirect2D() {
    if (!InitializeRenderer() || !EnsureRenderTarget()) return;
    const auto size = g_renderTarget->GetSize();
    const float cy = size.height * 0.5f;
    const bool focus = g_settings.preset == DensityPreset::Focus;
    const float sparkH = focus ? 10.0f : 0.0f;
    const float sparkGap = focus ? 6.0f : 0.0f;

    ID2D1SolidColorBrush* background{};
    ID2D1SolidColorBrush* hairline{};
    ID2D1SolidColorBrush* brand{};
    g_renderTarget->CreateSolidColorBrush(D2DColor(BgColor()), &background);
    g_renderTarget->CreateSolidColorBrush(D2DColor(MuteColor(), 0.55f), &hairline);
    g_renderTarget->CreateSolidColorBrush(D2DColor(Tokens::SignalTeal), &brand);

    g_renderTarget->BeginDraw();
    g_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), background);
    g_renderTarget->DrawLine(D2D1::Point2F(0, size.height - 0.5f), D2D1::Point2F(size.width, size.height - 0.5f), hairline, 0.5f);

    DrawBrandGlyph(brand, 12.0f, cy);

    for (auto& hit : g_hits) hit = {};

    struct Item {
        MetricId id;
        int icon; // -1 arrow down, -2 arrow up, 0 chip, 1 mem, 2 thermo, 3 fan
    };
    const Item items[] = {
        { MetricId::Down, -1 }, { MetricId::Up, -2 },
        { MetricId::Cpu, 0 }, { MetricId::Ram, 1 }, { MetricId::Gpu, 2 }, { MetricId::Fan, 3 }
    };

    float total = 0;
    bool firstSystem = true;
    for (const auto& item : items) {
        if (!ShowMetric(item.id)) continue;
        if ((item.id == MetricId::Cpu || item.id == MetricId::Ram || item.id == MetricId::Gpu || item.id == MetricId::Fan) && firstSystem) {
            total += 14;
            firstSystem = false;
        }
        const auto text = LabelFor(item.id);
        const auto level = LevelForMetric(item.id);
        auto* format = FormatForLevel(level);
        total += 18 + MeasureText(text, format) + 14 + sparkGap + (focus ? 36.0f : 0.0f);
    }

    float x = size.width - total - 14.0f;
    if (x < 42.0f) x = 42.0f;

    firstSystem = true;
    size_t hitIndex = 0;
    for (const auto& item : items) {
        if (!ShowMetric(item.id)) continue;
        if ((item.id == MetricId::Cpu || item.id == MetricId::Ram || item.id == MetricId::Gpu || item.id == MetricId::Fan) && firstSystem) {
            g_renderTarget->DrawLine(D2D1::Point2F(x, cy - 7), D2D1::Point2F(x, cy + 7), hairline, 0.7f);
            x += 10;
            firstSystem = false;
        }

        const auto text = LabelFor(item.id);
        const auto level = LevelForMetric(item.id);
        const COLORREF color = ColorForLevel(level, MuteColor());
        ID2D1SolidColorBrush* brush{};
        g_renderTarget->CreateSolidColorBrush(D2DColor(color), &brush);
        auto* format = FormatForLevel(level);

        const float startX = x;
        if (item.icon == -1) DrawVectorArrow(brush, x, cy, false);
        else if (item.icon == -2) DrawVectorArrow(brush, x, cy, true);
        else if (item.icon == 0) DrawChipIcon(brush, x, cy);
        else if (item.icon == 1) DrawMemoryIcon(brush, x, cy);
        else if (item.icon == 2) DrawThermometerIcon(brush, x, cy);
        else DrawFanIcon(brush, x, cy);
        x += 18.0f;

        const float textW = MeasureText(text, format);
        g_renderTarget->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
            D2D1::RectF(x, 0, x + textW + 2, size.height), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        x += textW + 4.0f;

        if (focus) {
            x += sparkGap;
            DrawMiniSparkline(x, cy - sparkH * 0.5f, 36.0f, sparkH, item.id, brush);
            x += 36.0f;
        }

        if (hitIndex < g_hits.size()) {
            g_hits[hitIndex] = { item.id, D2D1::RectF(startX - 2, 0, x + 4, size.height), true };
            ++hitIndex;
        }
        x += 10.0f;
        ReleaseCom(brush);
    }

    const HRESULT result = g_renderTarget->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) ReleaseCom(g_renderTarget);
    ReleaseCom(brand); ReleaseCom(hairline); ReleaseCom(background);
}

void ShutdownRenderer() {
    ReleaseCom(g_sparkTarget);
    ReleaseCom(g_renderTarget);
    ReleaseCom(g_brandFormat);
    ReleaseCom(g_metricFormat);
    ReleaseCom(g_metricWarnFormat);
    ReleaseCom(g_metricCritFormat);
    ReleaseCom(g_sparkTitleFormat);
    ReleaseCom(g_sparkValueFormat);
    ReleaseCom(g_dwriteFactory);
    ReleaseCom(g_d2dFactory);
}

void PaintSparkWindow() {
    if (!g_sparkWindow || !InitializeRenderer()) return;
    RECT rect{}; GetClientRect(g_sparkWindow, &rect);
    if (!g_sparkTarget) {
        const UINT dpi = GetDpiForWindow(g_sparkWindow);
        if (FAILED(g_d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE)),
            D2D1::HwndRenderTargetProperties(g_sparkWindow, D2D1::SizeU(rect.right - rect.left, rect.bottom - rect.top)), &g_sparkTarget))) return;
        g_sparkTarget->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    }

    ID2D1SolidColorBrush* background{};
    ID2D1SolidColorBrush* ink{};
    ID2D1SolidColorBrush* mute{};
    ID2D1SolidColorBrush* line{};
    g_sparkTarget->CreateSolidColorBrush(D2DColor(BgColor()), &background);
    g_sparkTarget->CreateSolidColorBrush(D2DColor(InkColor()), &ink);
    g_sparkTarget->CreateSolidColorBrush(D2DColor(MuteColor()), &mute);
    g_sparkTarget->CreateSolidColorBrush(D2DColor(Tokens::SignalTeal), &line);

    const float width = static_cast<float>(rect.right);
    const float height = static_cast<float>(rect.bottom);
    g_sparkTarget->BeginDraw();
    g_sparkTarget->FillRectangle(D2D1::RectF(0, 0, width, height), background);
    g_sparkTarget->DrawRectangle(D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f), mute, 1.0f);
    g_sparkTarget->DrawTextW(g_sparkTitle.c_str(), static_cast<UINT32>(g_sparkTitle.size()), g_sparkTitleFormat,
        D2D1::RectF(12, 10, width - 12, 28), ink);

    const auto value = LabelFor(g_sparkMetric);
    g_sparkTarget->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), g_sparkValueFormat,
        D2D1::RectF(12, 30, width - 12, 48), mute);

    const float chartLeft = 12.0f, chartTop = 56.0f, chartRight = width - 12.0f, chartBottom = height - 14.0f;
    g_sparkTarget->DrawLine(D2D1::Point2F(chartLeft, chartBottom), D2D1::Point2F(chartRight, chartBottom), mute, 0.7f);

    if (g_sparkSeriesCount >= 2) {
        float minV = g_sparkSeries[0], maxV = g_sparkSeries[0];
        for (size_t i = 1; i < g_sparkSeriesCount; ++i) {
            minV = (std::min)(minV, g_sparkSeries[i]);
            maxV = (std::max)(maxV, g_sparkSeries[i]);
        }
        if (maxV - minV < 0.001f) maxV = minV + 1.0f;
        D2D1_POINT_2F prev{};
        for (size_t i = 0; i < g_sparkSeriesCount; ++i) {
            const float px = chartLeft + ((chartRight - chartLeft) * i / (g_sparkSeriesCount - 1));
            const float py = chartBottom - ((g_sparkSeries[i] - minV) / (maxV - minV)) * (chartBottom - chartTop);
            const auto pt = D2D1::Point2F(px, py);
            if (i > 0) g_sparkTarget->DrawLine(prev, pt, line, 1.5f);
            prev = pt;
        }
    } else {
        const wchar_t* wait = L"Coletando amostras...";
        g_sparkTarget->DrawTextW(wait, 22, g_sparkValueFormat, D2D1::RectF(12, 80, width - 12, 100), mute);
    }

    if (g_sparkTarget->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseCom(g_sparkTarget);
    ReleaseCom(line); ReleaseCom(mute); ReleaseCom(ink); ReleaseCom(background);
}

void CloseSparkWindow() {
    if (g_sparkWindow) {
        DestroyWindow(g_sparkWindow);
        g_sparkWindow = nullptr;
    }
    ReleaseCom(g_sparkTarget);
    g_sparkMetric = MetricId::Count;
}

LRESULT CALLBACK SparkProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        PaintSparkWindow();
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        CloseSparkWindow();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) CloseSparkWindow();
        return 0;
    case WM_DESTROY:
        g_sparkWindow = nullptr;
        ReleaseCom(g_sparkTarget);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void OpenSparkFor(MetricId id, POINT screenPoint) {
    if (id == MetricId::Count) return;
    CloseSparkWindow();
    g_sparkMetric = id;
    g_sparkTitle = TitleFor(id);
    CopySeriesChronological(id, g_sparkSeries, g_sparkSeriesCount);

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseWatchSpark";
        wc.lpfnWndProc = SparkProc;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    constexpr int width = 280, height = 170;
    int x = screenPoint.x - width / 2;
    int y = screenPoint.y + 8;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (x + width > work.right) x = work.right - width - 8;
    if (x < work.left) x = work.left + 8;
    if (y + height > work.bottom) y = screenPoint.y - height - 12;

    g_sparkWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"PulseWatchSpark", L"PulseWatch", WS_POPUP,
        x, y, width, height, g_window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_sparkWindow) {
        WriteDiagnostic(L"Spark window create failed");
        return;
    }
    ShowWindow(g_sparkWindow, SW_SHOWNOACTIVATE);
    SetWindowPos(g_sparkWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(g_sparkWindow, nullptr, FALSE);
    UpdateWindow(g_sparkWindow);
}

MetricId HitTestMetric(int clientX, int clientY) {
    const float scale = WindowDpiScale();
    const float x = clientX / scale;
    const float y = clientY / scale;
    for (const auto& hit : g_hits) {
        if (!hit.active) continue;
        if (x >= hit.rect.left && x <= hit.rect.right &&
            y >= hit.rect.top && y <= hit.rect.bottom) return hit.id;
    }
    return MetricId::Count;
}

HICON CreatePulseIcon() {
    constexpr int size = 32;
    HDC screen = GetDC(nullptr);
    HDC colorDc = CreateCompatibleDC(screen);
    HDC maskDc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);
    const auto oldColor = SelectObject(colorDc, color);
    const auto oldMask = SelectObject(maskDc, mask);

    HBRUSH transparent = CreateSolidBrush(RGB(0, 0, 0));
    RECT iconRect{ 0, 0, size, size };
    FillRect(colorDc, &iconRect, transparent);
    DeleteObject(transparent);
    PatBlt(maskDc, 0, 0, size, size, WHITENESS);

    HPEN noPen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
    HBRUSH teal = CreateSolidBrush(Tokens::SignalTeal);
    const auto oldColorPen = SelectObject(colorDc, noPen);
    const auto oldColorBrush = SelectObject(colorDc, teal);
    Ellipse(colorDc, 2, 2, 30, 30);
    SelectObject(colorDc, oldColorBrush); SelectObject(colorDc, oldColorPen);
    DeleteObject(teal);

    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    const auto oldMaskBrush = SelectObject(maskDc, black);
    const auto oldMaskPen = SelectObject(maskDc, noPen);
    Ellipse(maskDc, 2, 2, 30, 30);
    SelectObject(maskDc, oldMaskPen); SelectObject(maskDc, oldMaskBrush);
    DeleteObject(black); DeleteObject(noPen);

    HPEN pulse = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    const auto oldPulse = SelectObject(colorDc, pulse);
    const POINT points[]{{ 7, 17 }, { 11, 17 }, { 14, 11 }, { 17, 22 }, { 20, 15 }, { 25, 15 }};
    Polyline(colorDc, points, ARRAYSIZE(points));
    SelectObject(colorDc, oldPulse); DeleteObject(pulse);

    SelectObject(colorDc, oldColor); SelectObject(maskDc, oldMask);
    DeleteDC(colorDc); DeleteDC(maskDc);
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE; iconInfo.hbmColor = color; iconInfo.hbmMask = mask;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(color); DeleteObject(mask);
    return icon;
}

void AddTrayIcon(HWND window) {
    g_tray = { sizeof(g_tray) };
    g_tray.hWnd = window; g_tray.uID = 1; g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_pulseIcon = CreatePulseIcon();
    g_tray.uCallbackMessage = kTrayMessage; g_tray.hIcon = g_pulseIcon;
    lstrcpynW(g_tray.szTip, L"PulseWatch · Signal Quiet — Ctrl+Alt+Shift+P encerra", ARRAYSIZE(g_tray.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void ShowAbout() {
    MessageBoxW(g_window,
        L"PulseWatch — Signal Quiet\n\n"
        L"Telemetria nativa no topo do desktop.\n"
        L"Clique em uma metrica para o historico de 60s.\n"
        L"Clique direito na bandeja para preferencias.\n"
        L"Em tela cheia (jogo, YouTube, apresentacao) a barra se oculta sozinha.\n\n"
        L"Atalho de emergencia: Ctrl+Alt+Shift+P\n"
        L"Presets: Quiet · Focus · Gaming\n\n"
        L"Cor = estado (amber / coral). Marca = glyph teal.",
        L"Sobre o PulseWatch",
        MB_OK | MB_ICONINFORMATION);
}

void ShowTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING | (g_settings.dark ? MF_UNCHECKED : MF_CHECKED), kThemeLightCommand, L"Claro");
    AppendMenuW(theme, MF_STRING | (g_settings.dark ? MF_CHECKED : MF_UNCHECKED), kThemeDarkCommand, L"Escuro");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"Tema");

    HMENU preset = CreatePopupMenu();
    AppendMenuW(preset, MF_STRING | (g_settings.preset == DensityPreset::Quiet ? MF_CHECKED : MF_UNCHECKED), kPresetQuietCommand, L"Quiet — so valores");
    AppendMenuW(preset, MF_STRING | (g_settings.preset == DensityPreset::Focus ? MF_CHECKED : MF_UNCHECKED), kPresetFocusCommand, L"Focus — micro-historico");
    AppendMenuW(preset, MF_STRING | (g_settings.preset == DensityPreset::Gaming ? MF_CHECKED : MF_UNCHECKED), kPresetGamingCommand, L"Gaming — GPU/FAN");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(preset), L"Densidade");

    g_monitors.clear(); EnumDisplayMonitors(nullptr, nullptr, ListMonitors, 0);
    HMENU monitors = CreatePopupMenu();
    for (size_t i = 0; i < g_monitors.size(); ++i) {
        const bool selected = g_settings.monitorDevice == g_monitors[i].device ||
            (g_settings.monitorDevice.empty() && i == 0);
        AppendMenuW(monitors, MF_STRING | (selected ? MF_CHECKED : MF_UNCHECKED), kMonitorFirstCommand + static_cast<UINT>(i), g_monitors[i].title.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(monitors), L"Monitor");

    HMENU refresh = CreatePopupMenu();
    AppendMenuW(refresh, MF_STRING | (g_settings.refreshMs == 1000 ? MF_CHECKED : MF_UNCHECKED), kRefresh1Command, L"Atualizar a cada 1 segundo");
    AppendMenuW(refresh, MF_STRING | (g_settings.refreshMs == 2000 ? MF_CHECKED : MF_UNCHECKED), kRefresh2Command, L"Atualizar a cada 2 segundos");
    AppendMenuW(refresh, MF_STRING | (g_settings.refreshMs == 5000 ? MF_CHECKED : MF_UNCHECKED), kRefresh5Command, L"Atualizar a cada 5 segundos");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(refresh), L"Atualizacao");

    HMENU metrics = CreatePopupMenu();
    AppendMenuW(metrics, MF_STRING | (g_settings.showCpu ? MF_CHECKED : MF_UNCHECKED), kToggleCpuCommand, L"CPU");
    AppendMenuW(metrics, MF_STRING | (g_settings.showRam ? MF_CHECKED : MF_UNCHECKED), kToggleRamCommand, L"RAM");
    AppendMenuW(metrics, MF_STRING | (g_settings.showGpu ? MF_CHECKED : MF_UNCHECKED), kToggleGpuCommand, L"GPU");
    AppendMenuW(metrics, MF_STRING | (g_settings.showFan ? MF_CHECKED : MF_UNCHECKED), kToggleFanCommand, L"Ventoinha GPU");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(metrics), L"Metricas");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED), kAutostartCommand, L"Iniciar com o Windows");
    AppendMenuW(menu, MF_STRING, kAboutCommand, L"Sobre · Signal Quiet");
    AppendMenuW(menu, MF_STRING, kExitCommand, L"Encerrar PulseWatch");
    SetForegroundWindow(window);
    POINT point{}; GetCursorPos(&point);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
}

void PositionAppBar() {
    if (g_yieldedForFocus || !g_appbarRegistered) return;
    RECT primary{};
    EnumDisplayMonitors(nullptr, nullptr, FindConfiguredMonitor, reinterpret_cast<LPARAM>(&primary));
    if (IsRectEmpty(&primary)) { g_settings.monitorDevice.clear(); EnumDisplayMonitors(nullptr, nullptr, FindConfiguredMonitor, reinterpret_cast<LPARAM>(&primary)); }
    const UINT height = BarHeight();
    g_appbar.uEdge = ABE_TOP;
    g_appbar.rc = primary;
    g_appbar.rc.bottom = g_appbar.rc.top + static_cast<LONG>(height);
    SHAppBarMessage(ABM_QUERYPOS, &g_appbar);
    g_appbar.rc.bottom = g_appbar.rc.top + static_cast<LONG>(height);
    SHAppBarMessage(ABM_SETPOS, &g_appbar);
    SetWindowPos(g_window, HWND_TOPMOST, g_appbar.rc.left, g_appbar.rc.top,
                 g_appbar.rc.right - g_appbar.rc.left, g_appbar.rc.bottom - g_appbar.rc.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void ApplyPreferences() {
    KillTimer(g_window, kTimer);
    KillTimer(g_window, kFocusTimer);
    SetTimer(g_window, kTimer, g_settings.refreshMs, nullptr);
    SetTimer(g_window, kFocusTimer, kFocusPollMs, nullptr);
    SetLayeredWindowAttributes(g_window, 0, g_settings.dark ? 230 : 220, LWA_ALPHA);
    SaveSettings();
    if (!g_yieldedForFocus) PositionAppBar();
    if (g_renderTarget) {
        RECT rect{}; GetClientRect(g_window, &rect);
        g_renderTarget->Resize(D2D1::SizeU(rect.right - rect.left, rect.bottom - rect.top));
    }
    if (!g_yieldedForFocus) InvalidateRect(g_window, nullptr, TRUE);
    if (g_sparkWindow) InvalidateRect(g_sparkWindow, nullptr, FALSE);
}

void RemoveAppBar() {
    if (g_appbarRegistered) { SHAppBarMessage(ABM_REMOVE, &g_appbar); g_appbarRegistered = false; }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window; g_appbar.hWnd = window; g_appbar.uCallbackMessage = kAppBarMessage;
        AddTrayIcon(window); RegisterHotKey(window, kExitHotkey, MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'P');
        Sample();
        const bool nvmlReady = InitializeNvml();
        SampleGpu();
        WriteDiagnostic(nvmlReady ? L"Started Signal Quiet; NVIDIA telemetry available" : L"Started Signal Quiet; NVIDIA telemetry unavailable");
        g_appbarRegistered = SHAppBarMessage(ABM_NEW, &g_appbar) != 0;
        if (!g_appbarRegistered) { WriteDiagnostic(L"AppBar registration failed"); return -1; }
        PositionAppBar();
        SetTimer(window, kTimer, g_settings.refreshMs, nullptr);
        SetTimer(window, kFocusTimer, kFocusPollMs, nullptr);
        InstallFocusHooks();
        SaveSettings();
        return 0;
    }
    case kFocusCheckMessage:
        UpdateFocusYield();
        return 0;
    case WM_TIMER:
        if (wParam == kFocusTimer) {
            UpdateFocusYield();
            return 0;
        }
        Sample();
        if (++g_gpuCountdown >= 2) { g_gpuCountdown = 0; SampleGpu(); }
        if (g_yieldedForFocus) return 0;
        if (g_sparkWindow && g_sparkMetric != MetricId::Count) {
            CopySeriesChronological(g_sparkMetric, g_sparkSeries, g_sparkSeriesCount);
            InvalidateRect(g_sparkWindow, nullptr, FALSE);
        }
        InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_HOTKEY: if (wParam == kExitHotkey) DestroyWindow(window); return 0;
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const auto metric = HitTestMetric(x, y);
        if (metric != MetricId::Count) {
            POINT screen{ x, y };
            ClientToScreen(window, &screen);
            OpenSparkFor(metric, screen);
        } else {
            CloseSparkWindow();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        ShowTrayMenu(window);
        return 0;
    }
    case WM_COMMAND: {
        const UINT command = LOWORD(wParam);
        if (command == kExitCommand) { DestroyWindow(window); return 0; }
        if (command == kAboutCommand) { ShowAbout(); return 0; }
        if (command == kThemeLightCommand) g_settings.dark = false;
        else if (command == kThemeDarkCommand) g_settings.dark = true;
        else if (command == kRefresh1Command) g_settings.refreshMs = 1000;
        else if (command == kRefresh2Command) g_settings.refreshMs = 2000;
        else if (command == kRefresh5Command) g_settings.refreshMs = 5000;
        else if (command == kToggleCpuCommand) g_settings.showCpu = !g_settings.showCpu;
        else if (command == kToggleRamCommand) g_settings.showRam = !g_settings.showRam;
        else if (command == kToggleGpuCommand) g_settings.showGpu = !g_settings.showGpu;
        else if (command == kToggleFanCommand) g_settings.showFan = !g_settings.showFan;
        else if (command == kPresetQuietCommand) g_settings.preset = DensityPreset::Quiet;
        else if (command == kPresetFocusCommand) g_settings.preset = DensityPreset::Focus;
        else if (command == kPresetGamingCommand) g_settings.preset = DensityPreset::Gaming;
        else if (command == kAutostartCommand) { SetAutostart(!IsAutostartEnabled()); return 0; }
        else if (command >= kMonitorFirstCommand && command < kMonitorFirstCommand + g_monitors.size())
            g_settings.monitorDevice = g_monitors[command - kMonitorFirstCommand].device;
        else return 0;
        ApplyPreferences(); return 0;
    }
    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu(window);
        if (lParam == WM_LBUTTONDBLCLK) DestroyWindow(window);
        if (lParam == WM_LBUTTONUP) ShowAbout();
        return 0;
    case kAppBarMessage: if (!g_yieldedForFocus && lParam == ABN_POSCHANGED) PositionAppBar(); return 0;
    case WM_DISPLAYCHANGE: if (!g_yieldedForFocus) PositionAppBar(); return 0;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(window, &paint); PaintDirect2D(); EndPaint(window, &paint); return 0; }
    case WM_SIZE:
        if (g_renderTarget) g_renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt{}; GetCursorPos(&pt); ScreenToClient(window, &pt);
            SetCursor(LoadCursor(nullptr, HitTestMetric(pt.x, pt.y) != MetricId::Count ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wParam) RemoveAppBar(); return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        CloseSparkWindow();
        RemoveFocusHooks();
        KillTimer(window, kTimer); KillTimer(window, kFocusTimer); UnregisterHotKey(window, kExitHotkey);
        Shell_NotifyIconW(NIM_DELETE, &g_tray); if (g_pulseIcon) DestroyIcon(g_pulseIcon);
        RemoveAppBar(); ShutdownNvml(); ShutdownRenderer(); WriteDiagnostic(L"Stopped"); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LoadSettings();
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance; windowClass.lpszClassName = L"PulseWatchTopBar";
    windowClass.lpfnWndProc = WindowProc; windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&windowClass)) return 1;
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        windowClass.lpszClassName, L"PulseWatch", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    SetLayeredWindowAttributes(window, 0, g_settings.dark ? 230 : 220, LWA_ALPHA);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
