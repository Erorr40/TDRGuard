#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <array>
#include <functional>
#include <atomic>
#include <thread>
#include <memory>

struct GpuTelemetry;
class GpuMonitor;
class DriverRecovery;
class EventWatcher;

// Callbacks to parent (main.cpp) for settings actions
struct DashboardCallbacks {
    std::function<void()>              onRestartDriver;
    std::function<void(bool)>          onSetAutoStart;    // bool = enabled
    std::function<bool()>              onGetAutoStart;
    std::function<void(float)>         onSetVramThreshold;
    std::function<float()>             onGetVramThreshold;
    std::function<void(std::wstring)>  onOpenFolder;
    std::function<void()>              onQuit;
};

class DashboardUI {
public:
    DashboardUI() = default;
    ~DashboardUI();

    // Initialize DirectX 11 device + ImGui context (called once on first open)
    bool Initialize(HINSTANCE hInst);

    // Show or hide the dashboard window
    void Show();
    void Hide();
    bool IsVisible() const;

    // Set data sources
    void SetMonitor(GpuMonitor* monitor)         { monitor_ = monitor; }
    void SetEventWatcher(EventWatcher* watcher)  { watcher_ = watcher; }
    void SetCallbacks(DashboardCallbacks cbs)    { callbacks_ = std::move(cbs); }

    // Run the UI message/render loop on this thread (blocking until window closed)
    // Call from a dedicated UI thread
    void RunLoop();

    // Thread-safe signal to close the window
    void RequestClose();

private:
    // Win32 window
    bool CreateDashboardWindow(HINSTANCE hInst);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    // DirectX 11
    bool CreateDeviceAndSwapChain();
    void CleanupDevice();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void HandleResize(UINT w, UINT h);

    // ImGui rendering
    void SetupImGuiStyle();
    void RenderFrame();
    void RenderMainMenuBar();
    void RenderOverviewPanel();
    void RenderGraphPanel();
    void RenderCrashPanel();
    void RenderSettingsPanel();

    // Win32
    HWND      hwnd_   = nullptr;
    HINSTANCE hInst_  = nullptr;
    bool      initialized_ = false;
    std::atomic<bool> visible_      {false};
    std::atomic<bool> closeRequest_ {false};

    // DX11
    ID3D11Device*            d3dDevice_            = nullptr;
    ID3D11DeviceContext*     d3dContext_            = nullptr;
    IDXGISwapChain*          swapChain_             = nullptr;
    ID3D11RenderTargetView*  mainRenderTargetView_  = nullptr;

    // Data sources (non-owning pointers)
    GpuMonitor*   monitor_  = nullptr;
    EventWatcher* watcher_  = nullptr;

    // Callbacks
    DashboardCallbacks callbacks_;

    // UI state
    float vramThresholdEdit_   = 90.0f;
    bool  showConfirmReset_    = false;
    bool  showSettings_        = false;
    char  hotkeyScratch_[64]   = "Ctrl+Shift+Alt+R";

    // Graph history (last 60 vram/gpu samples for the ImGui plots)
    static constexpr int kGraphLen = 60;
    float vramHistory_[kGraphLen]  = {};
    float gpuHistory_[kGraphLen]   = {};
    int   graphOffset_             = 0;
    float graphVramMax_            = 100.0f;
};
