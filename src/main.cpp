// main.cpp — TDRGuard entry point, tray icon, hotkey, lifecycle orchestrator
// C++20 / Win32 / Windows 10+

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wchar.h>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <array>
#include "GpuMonitor.h"
#include "DriverRecovery.h"
#include "EventWatcher.h"
#include "DashboardUI.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr UINT WM_TRAYICON     = WM_APP + 1;
static constexpr UINT IDI_TRAY        = 1;
static constexpr UINT IDM_DASHBOARD   = 101;
static constexpr UINT IDM_RESTART_GPU = 102;
static constexpr UINT IDM_AUTOSTART   = 103;
static constexpr UINT IDM_QUIT        = 104;
static constexpr UINT HOTKEY_ID_RESET = 1;
static constexpr UINT TIMER_ID_STATUS = 1;

// ---------------------------------------------------------------------------
// File-static helper: create a solid-color 16x16 HICON
// ---------------------------------------------------------------------------
static HICON CreateSolidColorIcon(COLORREF color)
{
    constexpr int kSize = 16;

    // Build a 16x16 32-bpp DIB section for the colour bitmap
    BITMAPV5HEADER bmi{};
    bmi.bV5Size        = sizeof(bmi);
    bmi.bV5Width       = kSize;
    bmi.bV5Height      = -kSize; // top-down
    bmi.bV5Planes      = 1;
    bmi.bV5BitCount    = 32;
    bmi.bV5Compression = BI_RGB;

    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hColor = CreateDIBSection(
        hdc,
        reinterpret_cast<BITMAPINFO*>(&bmi),
        DIB_RGB_COLORS,
        &bits,
        nullptr, 0);
    ReleaseDC(nullptr, hdc);

    if (!hColor)
        return nullptr;

    // Fill every pixel with the requested colour (BGRA layout in DIB)
    BYTE b = GetBValue(color);
    BYTE g = GetGValue(color);
    BYTE r = GetRValue(color);
    auto* px = static_cast<DWORD*>(bits);
    for (int i = 0; i < kSize * kSize; ++i)
    {
        px[i] = (static_cast<DWORD>(0xFF) << 24) | // alpha = opaque
                (static_cast<DWORD>(r)    << 16) |
                (static_cast<DWORD>(g)    <<  8) |
                (static_cast<DWORD>(b));
    }

    // All-black 1-bpp mask (required by CreateIconIndirect)
    HBITMAP hMask = CreateBitmap(kSize, kSize, 1, 1, nullptr);

    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask  = hMask;
    HICON icon  = CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);
    return icon;
}

// ---------------------------------------------------------------------------
// TdrGuardApp
// ---------------------------------------------------------------------------
class TdrGuardApp {
public:
    static TdrGuardApp& Get()
    {
        static TdrGuardApp inst;
        return inst;
    }

    int  Run(HINSTANCE hInst, int nCmdShow);
    void Quit();

private:
    TdrGuardApp()  = default;
    ~TdrGuardApp();

    bool Init(HINSTANCE hInst);
    void Shutdown();

    // Single-instance mutex
    bool AcquireSingleInstanceMutex();
    void ReleaseSingleInstanceMutex();

    // Tray icon
    bool CreateTrayIcon(HWND hwnd);
    void UpdateTrayIcon(int status);   // 0=green, 1=yellow, 2=red
    void RemoveTrayIcon();
    void ShowTrayContextMenu(HWND hwnd);
    void ShowTrayBalloon(const wchar_t* title, const wchar_t* msg, DWORD infoFlags = NIIF_INFO);

    // Hidden message window
    HWND CreateMessageWindow(HINSTANCE hInst);
    static LRESULT CALLBACK MsgWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    // Hotkey
    void RegisterGlobalHotkey(HWND hwnd);
    void UnregisterGlobalHotkey(HWND hwnd);

    // Auto-start registry
    bool GetAutoStart() const;
    void SetAutoStart(bool enable);

    // Status update timer callback
    void OnStatusTimer();

    // GPU reset (runs on worker thread to avoid blocking the message loop)
    void TriggerGpuReset();

    // Dashboard
    void OpenDashboard();

    // Subsystems
    std::unique_ptr<GpuMonitor>   gpuMonitor_;
    std::unique_ptr<EventWatcher> eventWatcher_;
    std::unique_ptr<DashboardUI>  dashboard_;

    // Dashboard thread
    std::thread       dashThread_;
    std::atomic<bool> dashRunning_{ false };

    // State
    HWND      msgWnd_       = nullptr;
    HINSTANCE hInst_        = nullptr;
    HANDLE    singleMutex_  = nullptr;
    bool      running_      = true;
    int       currentStatus_ = 0;    // 0=green, 1=yellow, 2=red

    // Tray icon data
    NOTIFYICONDATAW nid_{};

    // Drawn icons for tray
    HICON iconGreen_  = nullptr;
    HICON iconYellow_ = nullptr;
    HICON iconRed_    = nullptr;

    // Track last event count so OnStatusTimer can detect new TDR events
    size_t lastEventCount_ = 0;
};

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
TdrGuardApp::~TdrGuardApp()
{
    if (singleMutex_)
        ReleaseSingleInstanceMutex();
}

// ---------------------------------------------------------------------------
// AcquireSingleInstanceMutex
// ---------------------------------------------------------------------------
bool TdrGuardApp::AcquireSingleInstanceMutex()
{
    singleMutex_ = CreateMutexW(nullptr, TRUE, L"Global\\TDRGuard_SingleInstance");
    if (!singleMutex_)
        return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(singleMutex_);
        singleMutex_ = nullptr;
        MessageBoxW(
            nullptr,
            L"TDRGuard is already running.\n\nCheck the system tray for its icon.",
            L"TDRGuard",
            MB_OK | MB_ICONINFORMATION);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ReleaseSingleInstanceMutex
// ---------------------------------------------------------------------------
void TdrGuardApp::ReleaseSingleInstanceMutex()
{
    if (singleMutex_)
    {
        ReleaseMutex(singleMutex_);
        CloseHandle(singleMutex_);
        singleMutex_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// CreateTrayIcon
// ---------------------------------------------------------------------------
bool TdrGuardApp::CreateTrayIcon(HWND hwnd)
{
    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize           = sizeof(NOTIFYICONDATAW);
    nid_.hWnd             = hwnd;
    nid_.uID              = IDI_TRAY;
    nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon            = iconGreen_;
    wcscpy_s(nid_.szTip, L"TDRGuard \u2014 GPU Watchdog: Healthy");

    if (!Shell_NotifyIconW(NIM_ADD, &nid_))
        return false;

    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    return true;
}

// ---------------------------------------------------------------------------
// UpdateTrayIcon
// ---------------------------------------------------------------------------
void TdrGuardApp::UpdateTrayIcon(int status)
{
    currentStatus_ = status;
    nid_.uFlags    = NIF_ICON | NIF_TIP;

    switch (status)
    {
    case 1:
        nid_.hIcon = iconYellow_;
        wcscpy_s(nid_.szTip, L"TDRGuard \u2014 GPU Watchdog: VRAM Warning");
        break;
    case 2:
        nid_.hIcon = iconRed_;
        wcscpy_s(nid_.szTip, L"TDRGuard \u2014 GPU Watchdog: Hang Detected");
        break;
    default:
        nid_.hIcon = iconGreen_;
        wcscpy_s(nid_.szTip, L"TDRGuard \u2014 GPU Watchdog: Healthy");
        break;
    }

    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

// ---------------------------------------------------------------------------
// RemoveTrayIcon
// ---------------------------------------------------------------------------
void TdrGuardApp::RemoveTrayIcon()
{
    nid_.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
}

// ---------------------------------------------------------------------------
// ShowTrayContextMenu
// ---------------------------------------------------------------------------
void TdrGuardApp::ShowTrayContextMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    // "Open Dashboard"
    MENUITEMINFOW mii{};
    mii.cbSize     = sizeof(mii);
    mii.fMask      = MIIM_ID | MIIM_TYPE;
    mii.fType      = MFT_STRING;
    mii.wID        = IDM_DASHBOARD;
    mii.dwTypeData = const_cast<LPWSTR>(L"Open Dashboard");
    InsertMenuItemW(hMenu, 0, TRUE, &mii);

    // "Restart GPU Driver"
    mii.wID        = IDM_RESTART_GPU;
    mii.dwTypeData = const_cast<LPWSTR>(L"Restart GPU Driver");
    InsertMenuItemW(hMenu, 1, TRUE, &mii);

    // Separator
    mii.fMask = MIIM_TYPE;
    mii.fType = MFT_SEPARATOR;
    InsertMenuItemW(hMenu, 2, TRUE, &mii);

    // "Start with Windows" (with checkmark if enabled)
    mii.fMask      = MIIM_ID | MIIM_TYPE | MIIM_STATE;
    mii.fType      = MFT_STRING;
    mii.wID        = IDM_AUTOSTART;
    mii.dwTypeData = const_cast<LPWSTR>(L"Start with Windows");
    mii.fState     = GetAutoStart() ? MFS_CHECKED : MFS_UNCHECKED;
    InsertMenuItemW(hMenu, 3, TRUE, &mii);

    // Separator
    mii.fMask = MIIM_TYPE;
    mii.fType = MFT_SEPARATOR;
    InsertMenuItemW(hMenu, 4, TRUE, &mii);

    // "Quit"
    mii.fMask      = MIIM_ID | MIIM_TYPE;
    mii.fType      = MFT_STRING;
    mii.wID        = IDM_QUIT;
    mii.dwTypeData = const_cast<LPWSTR>(L"Quit");
    InsertMenuItemW(hMenu, 5, TRUE, &mii);

    // SetForegroundWindow is required so clicking outside dismisses the menu
    SetForegroundWindow(hwnd);

    POINT pt{};
    GetCursorPos(&pt);

    TrackPopupMenu(
        hMenu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x, pt.y,
        0, hwnd, nullptr);

    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// ---------------------------------------------------------------------------
// ShowTrayBalloon
// ---------------------------------------------------------------------------
void TdrGuardApp::ShowTrayBalloon(const wchar_t* title, const wchar_t* msg, DWORD infoFlags)
{
    nid_.uFlags |= NIF_INFO;
    wcscpy_s(nid_.szInfoTitle, title);
    wcscpy_s(nid_.szInfo, msg);
    nid_.dwInfoFlags = infoFlags;
    nid_.uTimeout    = 5000;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags &= ~NIF_INFO;
}

// ---------------------------------------------------------------------------
// RegisterGlobalHotkey
// ---------------------------------------------------------------------------
void TdrGuardApp::RegisterGlobalHotkey(HWND hwnd)
{
    if (!RegisterHotKey(hwnd, HOTKEY_ID_RESET,
                        MOD_CONTROL | MOD_SHIFT | MOD_ALT, 'R'))
    {
        ShowTrayBalloon(
            L"TDRGuard \u2014 Hotkey Conflict",
            L"Could not register Ctrl+Shift+Alt+R. "
            L"Another application may be using this combination.",
            NIIF_WARNING);
    }
}

// ---------------------------------------------------------------------------
// UnregisterGlobalHotkey
// ---------------------------------------------------------------------------
void TdrGuardApp::UnregisterGlobalHotkey(HWND hwnd)
{
    UnregisterHotKey(hwnd, HOTKEY_ID_RESET);
}

// ---------------------------------------------------------------------------
// GetAutoStart / SetAutoStart
// ---------------------------------------------------------------------------
bool TdrGuardApp::GetAutoStart() const
{
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS)
        return false;

    DWORD type  = 0;
    DWORD bytes = 0;
    result = RegQueryValueExW(hKey, L"TDRGuard", nullptr, &type, nullptr, &bytes);
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

void TdrGuardApp::SetAutoStart(bool enable)
{
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS)
        return;

    if (enable)
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        std::wstring quoted = L"\"";
        quoted += exePath;
        quoted += L"\"";

        RegSetValueExW(
            hKey, L"TDRGuard", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(quoted.c_str()),
            static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(hKey, L"TDRGuard");
    }

    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// OnStatusTimer — called every 2 seconds
// ---------------------------------------------------------------------------
void TdrGuardApp::OnStatusTimer()
{
    if (!gpuMonitor_)
        return;

    GpuTelemetry tel = gpuMonitor_->GetLatest();

    // Map telemetry flags to status level
    int newStatus = 0;
    if (gpuMonitor_->IsHangSuspected())
        newStatus = 2;
    else if (gpuMonitor_->IsVramWarning())
        newStatus = 1;

    if (newStatus != currentStatus_)
        UpdateTrayIcon(newStatus);

    // Check for new TDR events from the event watcher
    if (eventWatcher_)
    {
        const auto& events = eventWatcher_->GetRecentEvents();
        if (events.size() > lastEventCount_)
        {
            lastEventCount_ = events.size();
            const TdrEvent& ev = events.back();

            UpdateTrayIcon(2);

            std::wstring balloon = L"Event ID ";
            balloon += std::to_wstring(ev.eventId);
            balloon += L" \u2014 ";
            balloon += ev.source;
            if (!ev.reportPath.empty())
            {
                balloon += L"\nReport: ";
                balloon += ev.reportPath;
            }
            ShowTrayBalloon(L"TDRGuard \u2014 TDR Detected", balloon.c_str(), NIIF_ERROR);
        }
    }
}

// ---------------------------------------------------------------------------
// TriggerGpuReset — dispatched on a worker thread
// ---------------------------------------------------------------------------
void TdrGuardApp::TriggerGpuReset()
{
    if (DriverRecovery::IsResetInProgress())
    {
        ShowTrayBalloon(
            L"TDRGuard",
            L"A GPU driver reset is already in progress.",
            NIIF_INFO);
        return;
    }

    std::thread([this]()
    {
        ShowTrayBalloon(
            L"TDRGuard \u2014 Reset",
            L"Restarting GPU driver\u2026 please wait.",
            NIIF_INFO);

        RecoveryResult res = DriverRecovery::PerformGpuReset(
            [this](const std::wstring& status)
            {
                nid_.uFlags = NIF_TIP;
                std::wstring tip = L"TDRGuard \u2014 ";
                tip += status;
                if (tip.size() >= 128)
                    tip.resize(127);
                wcscpy_s(nid_.szTip, tip.c_str());
                Shell_NotifyIconW(NIM_MODIFY, &nid_);
            });

        if (res.success)
        {
            ShowTrayBalloon(
                L"TDRGuard \u2014 Reset Complete",
                L"GPU driver restarted successfully.",
                NIIF_INFO);
            UpdateTrayIcon(0);
        }
        else
        {
            std::wstring msg = L"GPU driver reset failed: ";
            msg += res.message;
            ShowTrayBalloon(L"TDRGuard \u2014 Reset Failed", msg.c_str(), NIIF_ERROR);
            UpdateTrayIcon(currentStatus_);
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// OpenDashboard
// ---------------------------------------------------------------------------
void TdrGuardApp::OpenDashboard()
{
    if (dashRunning_.load())
    {
        if (dashboard_)
        {
            dashboard_->Show();
        }
        return;
    }

    dashboard_ = std::make_unique<DashboardUI>();
    dashboard_->SetMonitor(gpuMonitor_.get());
    dashboard_->SetEventWatcher(eventWatcher_.get());

    DashboardCallbacks cbs;
    cbs.onRestartDriver = [this]() { TriggerGpuReset(); };
    cbs.onSetAutoStart  = [this](bool en) { SetAutoStart(en); };
    cbs.onGetAutoStart  = [this]() -> bool { return GetAutoStart(); };
    cbs.onSetVramThreshold = [this](float pct)
    {
        if (gpuMonitor_)
            gpuMonitor_->SetVramThreshold(pct);
    };
    cbs.onGetVramThreshold = [this]() -> float
    {
        return gpuMonitor_ ? gpuMonitor_->GetVramThreshold() : 90.0f;
    };
    cbs.onOpenFolder = [](std::wstring path)
    {
        ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    cbs.onQuit = [this]() { Quit(); };

    dashboard_->SetCallbacks(std::move(cbs));

    dashRunning_.store(true);

    dashThread_ = std::thread([this]()
    {
        dashboard_->Initialize(hInst_);
        dashboard_->RunLoop();
        dashRunning_.store(false);
    });
    dashThread_.detach();
}

// ---------------------------------------------------------------------------
// Quit
// ---------------------------------------------------------------------------
void TdrGuardApp::Quit()
{
    running_ = false;
    PostMessageW(msgWnd_, WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// CreateMessageWindow
// ---------------------------------------------------------------------------
HWND TdrGuardApp::CreateMessageWindow(HINSTANCE hInst)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MsgWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"TDRGuardMsg";

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"TDRGuardMsg",
        L"TDRGuard Message Window",
        0,
        0, 0, 0, 0,
        nullptr, nullptr, hInst, nullptr);

    return hwnd;
}

// ---------------------------------------------------------------------------
// MsgWndProc (static)
// ---------------------------------------------------------------------------
LRESULT CALLBACK TdrGuardApp::MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return TdrGuardApp::Get().HandleMessage(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// HandleMessage
// ---------------------------------------------------------------------------
LRESULT TdrGuardApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TRAYICON:
    {
        UINT notification = LOWORD(lp);
        switch (notification)
        {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayContextMenu(hwnd);
            break;
        case WM_LBUTTONDBLCLK:
            OpenDashboard();
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wp))
        {
        case IDM_DASHBOARD:
            OpenDashboard();
            break;
        case IDM_RESTART_GPU:
            TriggerGpuReset();
            break;
        case IDM_AUTOSTART:
            SetAutoStart(!GetAutoStart());
            break;
        case IDM_QUIT:
            Quit();
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_HOTKEY:
        if (static_cast<UINT>(wp) == HOTKEY_ID_RESET)
            TriggerGpuReset();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ID_STATUS)
            OnStatusTimer();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool TdrGuardApp::Init(HINSTANCE hInst)
{
    hInst_ = hInst;

    if (!AcquireSingleInstanceMutex())
        return false;

    // Common controls
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    // COM — apartment-threaded for shell calls
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Create status indicator icons (#00C853 green, #FFD600 yellow, #FF1744 red)
    iconGreen_  = CreateSolidColorIcon(RGB(0x00, 0xC8, 0x53));
    iconYellow_ = CreateSolidColorIcon(RGB(0xFF, 0xD6, 0x00));
    iconRed_    = CreateSolidColorIcon(RGB(0xFF, 0x17, 0x44));

    // GPU monitor
    gpuMonitor_ = std::make_unique<GpuMonitor>();
    if (!gpuMonitor_->Initialize())
    {
        // Non-fatal: continue without DXGI telemetry
    }
    gpuMonitor_->StartPolling();

    // Event watcher
    eventWatcher_ = std::make_unique<EventWatcher>();

    eventWatcher_->SetTdrCallback([this](const TdrEvent& ev)
    {
        // Background callback: update tray icon immediately
        UpdateTrayIcon(2);

        std::wstring title = L"TDRGuard \u2014 TDR / Hang Detected";
        std::wstring msg   = L"Source: ";
        msg += ev.source;
        msg += L"  ID: ";
        msg += std::to_wstring(ev.eventId);
        ShowTrayBalloon(title.c_str(), msg.c_str(), NIIF_ERROR);

        // Schedule crash export asynchronously
        std::thread([this, ev]()
        {
            if (!eventWatcher_)
                return;

            std::array<GpuTelemetry, GpuMonitor::kRingCapacity> snap{};
            size_t cnt = gpuMonitor_ ? gpuMonitor_->GetRingSnapshot(snap) : 0;

            std::vector<GpuTelemetry> history(snap.begin(), snap.begin() + cnt);

            std::wstring adapterName = gpuMonitor_
                ? gpuMonitor_->GetAdapterName()
                : L"Unknown";

            eventWatcher_->ExportCrashReport(ev, history, adapterName);
        }).detach();
    });

    eventWatcher_->SetRingSnapshotFn([this]() -> std::vector<GpuTelemetry>
    {
        if (!gpuMonitor_)
            return {};
        std::array<GpuTelemetry, GpuMonitor::kRingCapacity> snap{};
        size_t cnt = gpuMonitor_->GetRingSnapshot(snap);
        return std::vector<GpuTelemetry>(snap.begin(), snap.begin() + cnt);
    });

    eventWatcher_->StartWatching();

    // Message-only window
    msgWnd_ = CreateMessageWindow(hInst);
    if (!msgWnd_)
        return false;

    if (!CreateTrayIcon(msgWnd_))
        return false;

    RegisterGlobalHotkey(msgWnd_);

    // Poll status every 2 seconds
    SetTimer(msgWnd_, TIMER_ID_STATUS, 2000, nullptr);

    // Elevated relaunch path: perform an immediate GPU reset
    LPWSTR cmdLine = GetCommandLineW();
    if (cmdLine && wcsstr(cmdLine, L"--restart-gpu"))
        TriggerGpuReset();

    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void TdrGuardApp::Shutdown()
{
    if (msgWnd_)
    {
        KillTimer(msgWnd_, TIMER_ID_STATUS);
        UnregisterGlobalHotkey(msgWnd_);
    }

    RemoveTrayIcon();

    if (dashboard_)
        dashboard_->RequestClose();

    if (eventWatcher_)
        eventWatcher_->StopWatching();

    if (gpuMonitor_)
    {
        gpuMonitor_->StopPolling();
        gpuMonitor_->Shutdown();
    }

    CoUninitialize();
    ReleaseSingleInstanceMutex();

    if (iconGreen_)  { DestroyIcon(iconGreen_);  iconGreen_  = nullptr; }
    if (iconYellow_) { DestroyIcon(iconYellow_); iconYellow_ = nullptr; }
    if (iconRed_)    { DestroyIcon(iconRed_);    iconRed_    = nullptr; }
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
int TdrGuardApp::Run(HINSTANCE hInst, int /*nCmdShow*/)
{
    if (!Init(hInst))
        return 1;

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shutdown();
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE /*hPrevInst*/,
                    LPWSTR /*lpCmdLine*/, int nCmdShow)
{
    return TdrGuardApp::Get().Run(hInst, nCmdShow);
}
