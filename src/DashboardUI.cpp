// DashboardUI.cpp — TDRGuard DirectX 11 + Dear ImGui overlay dashboard
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DashboardUI.h"
#include "GpuMonitor.h"
#include "EventWatcher.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wchar.h>
#include <cwchar>
#include <cstdio>
#include <chrono>
#include <ctime>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Forward declaration required by imgui_impl_win32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                               WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
DashboardUI::~DashboardUI() {
    if (initialized_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        initialized_ = false;
    }
    CleanupDevice();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialize — called once before the first Show()
// ---------------------------------------------------------------------------
bool DashboardUI::Initialize(HINSTANCE hInst) {
    hInst_ = hInst;

    if (!CreateDashboardWindow(hInst))
        return false;

    if (!CreateDeviceAndSwapChain())
        return false;

    SetupImGuiStyle();

    // Pull initial threshold from callbacks if available
    if (callbacks_.onGetVramThreshold)
        vramThresholdEdit_ = callbacks_.onGetVramThreshold();

    initialized_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Show / Hide / IsVisible
// ---------------------------------------------------------------------------
void DashboardUI::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    visible_.store(true, std::memory_order_relaxed);
}

void DashboardUI::Hide() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_.store(false, std::memory_order_relaxed);
}

bool DashboardUI::IsVisible() const {
    return visible_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// RequestClose — thread-safe signal to exit RunLoop
// ---------------------------------------------------------------------------
void DashboardUI::RequestClose() {
    closeRequest_.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CreateDashboardWindow
// ---------------------------------------------------------------------------
bool DashboardUI::CreateDashboardWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TDRGuardDashboard";
    wc.hIconSm       = LoadIconW(hInst, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
        return false;

    // Center on primary monitor
    const int scrW = GetSystemMetrics(SM_CXSCREEN);
    const int scrH = GetSystemMetrics(SM_CYSCREEN);
    constexpr int kW = 1100;
    constexpr int kH = 680;
    const int posX = (scrW - kW) / 2;
    const int posY = (scrH - kH) / 2;

    hwnd_ = CreateWindowExW(
        0,
        L"TDRGuardDashboard",
        L"TDRGuard \u2014 GPU Watchdog Dashboard",
        WS_OVERLAPPEDWINDOW,
        posX, posY, kW, kH,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd_)
        return false;

    // Store this pointer so WndProc can dispatch into HandleMessage
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

// ---------------------------------------------------------------------------
// WndProc (static) — dispatches to instance HandleMessage
// ---------------------------------------------------------------------------
LRESULT CALLBACK DashboardUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return TRUE;

    auto* ui = reinterpret_cast<DashboardUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (ui)
        return ui->HandleMessage(hwnd, msg, wParam, lParam);

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// HandleMessage — instance-level Win32 message handler
// ---------------------------------------------------------------------------
LRESULT DashboardUI::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            const UINT w = LOWORD(lParam);
            const UINT h = HIWORD(lParam);
            HandleResize(w, h);
        }
        return 0;

    case WM_CLOSE:
        // Hide rather than destroy so the system tray still works
        Hide();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// CreateDeviceAndSwapChain
// ---------------------------------------------------------------------------
bool DashboardUI::CreateDeviceAndSwapChain() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd_;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Try feature level 11.0, fall back to 10.0
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevelOut{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        static_cast<UINT>(ARRAYSIZE(featureLevels)),
        D3D11_SDK_VERSION,
        &sd,
        &swapChain_,
        &d3dDevice_,
        &featureLevelOut,
        &d3dContext_);

    if (FAILED(hr))
        return false;

    CreateRenderTarget();
    return true;
}

// ---------------------------------------------------------------------------
// CleanupDevice
// ---------------------------------------------------------------------------
void DashboardUI::CleanupDevice() {
    CleanupRenderTarget();
    if (swapChain_)   { swapChain_->Release();   swapChain_   = nullptr; }
    if (d3dContext_)  { d3dContext_->Release();   d3dContext_  = nullptr; }
    if (d3dDevice_)   { d3dDevice_->Release();    d3dDevice_   = nullptr; }
}

// ---------------------------------------------------------------------------
// CreateRenderTarget / CleanupRenderTarget
// ---------------------------------------------------------------------------
void DashboardUI::CreateRenderTarget() {
    ID3D11Texture2D* pBackBuf = nullptr;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&pBackBuf));
    if (pBackBuf) {
        d3dDevice_->CreateRenderTargetView(pBackBuf, nullptr, &mainRenderTargetView_);
        pBackBuf->Release();
    }
}

void DashboardUI::CleanupRenderTarget() {
    if (mainRenderTargetView_) {
        mainRenderTargetView_->Release();
        mainRenderTargetView_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// HandleResize
// ---------------------------------------------------------------------------
void DashboardUI::HandleResize(UINT w, UINT h) {
    CleanupRenderTarget();
    swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

// ---------------------------------------------------------------------------
// SetupImGuiStyle — ImGui context, backends, custom dark theme
// ---------------------------------------------------------------------------
void DashboardUI::SetupImGuiStyle() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr; // Don't save layout to imgui.ini

    // Base dark theme then customise
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.FramePadding      = ImVec2(8.0f, 4.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]              = ImVec4(0.90f, 0.90f, 0.95f, 1.00f);
    colors[ImGuiCol_WindowBg]          = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg]           = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_PopupBg]           = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_FrameBg]           = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]    = ImVec4(0.20f, 0.20f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgActive]     = ImVec4(0.24f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_TitleBg]           = ImVec4(0.10f, 0.10f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgActive]     = ImVec4(0.15f, 0.15f, 0.55f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]       = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrab]        = ImVec4(0.30f, 0.30f, 0.80f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]  = ImVec4(0.40f, 0.40f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]            = ImVec4(0.20f, 0.20f, 0.60f, 1.00f);
    colors[ImGuiCol_ButtonHovered]     = ImVec4(0.30f, 0.30f, 0.80f, 1.00f);
    colors[ImGuiCol_ButtonActive]      = ImVec4(0.35f, 0.35f, 0.90f, 1.00f);
    colors[ImGuiCol_Header]            = ImVec4(0.20f, 0.20f, 0.55f, 1.00f);
    colors[ImGuiCol_HeaderHovered]     = ImVec4(0.28f, 0.28f, 0.70f, 1.00f);
    colors[ImGuiCol_HeaderActive]      = ImVec4(0.35f, 0.35f, 0.80f, 1.00f);
    colors[ImGuiCol_PlotLines]         = ImVec4(0.00f, 0.85f, 0.30f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]  = ImVec4(0.20f, 1.00f, 0.40f, 1.00f);
    colors[ImGuiCol_PlotHistogram]     = ImVec4(0.00f, 0.85f, 0.30f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.20f, 1.00f, 0.40f, 1.00f);
    colors[ImGuiCol_Separator]         = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_CheckMark]         = ImVec4(0.30f, 0.80f, 1.00f, 1.00f);

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(d3dDevice_, d3dContext_);

    // Use default embedded font at 15px
    io.Fonts->AddFontDefault();
    ImFontConfig cfg{};
    cfg.SizePixels = 15.0f;
    // Font is already added above; just rebuild so the size is applied
    io.Fonts->Build();
}

// ---------------------------------------------------------------------------
// RunLoop — blocking message + render loop (call from dedicated UI thread)
// ---------------------------------------------------------------------------
void DashboardUI::RunLoop() {
    MSG msg{};
    while (true) {
        // Drain the Win32 message queue
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                return;
        }

        // Handle external close request
        if (closeRequest_.load(std::memory_order_relaxed)) {
            if (hwnd_) {
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
            }
            break;
        }

        RenderFrame();
    }
}

// ---------------------------------------------------------------------------
// RenderFrame — one ImGui frame
// ---------------------------------------------------------------------------
void DashboardUI::RenderFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Update graph history from the monitor (if available)
    if (monitor_) {
        GpuTelemetry t = monitor_->GetLatest();
        vramHistory_[graphOffset_] = t.vramUsagePercent;
        gpuHistory_[graphOffset_]  = t.gpuUsagePercent;
        graphOffset_ = (graphOffset_ + 1) % kGraphLen;
    }

    // Full-screen dockspace background window
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGuiWindowFlags bgFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav        | ImGuiWindowFlags_NoMove   |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##background", nullptr, bgFlags);
        ImGui::End();
    }

    RenderOverviewPanel();
    RenderGraphPanel();
    RenderCrashPanel();
    RenderSettingsPanel();

    ImGui::Render();

    float clearColor[4] = { 0.07f, 0.07f, 0.09f, 1.0f };
    d3dContext_->OMSetRenderTargets(1, &mainRenderTargetView_, nullptr);
    d3dContext_->ClearRenderTargetView(mainRenderTargetView_, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}

// ---------------------------------------------------------------------------
// RenderOverviewPanel
// ---------------------------------------------------------------------------
void DashboardUI::RenderOverviewPanel() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 110.0f), ImGuiCond_Always);
    ImGui::Begin("Overview",
                 nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse);

    if (monitor_) {
        GpuTelemetry t  = monitor_->GetLatest();
        std::wstring adapterW = monitor_->GetAdapterName();

        // Convert adapter name to narrow string for ImGui
        char adapterA[256] = "<Unknown>";
        WideCharToMultiByte(CP_UTF8, 0, adapterW.c_str(), -1,
                            adapterA, sizeof(adapterA), nullptr, nullptr);

        const float vramUsedMB   = static_cast<float>(t.dedicatedVramUsed)   / (1024.0f * 1024.0f);
        const float vramBudgetMB = static_cast<float>(t.dedicatedVramBudget) / (1024.0f * 1024.0f);
        const float threshold    = monitor_->GetVramThreshold();
        const bool  hangDetected = monitor_->IsHangSuspected();
        const bool  vramWarn     = monitor_->IsVramWarning();

        ImGui::Text("Adapter: %s", adapterA);
        ImGui::SameLine(0, 40);
        ImGui::Text("VRAM: %.0f MB / %.0f MB", vramUsedMB, vramBudgetMB);
        ImGui::SameLine(0, 40);
        ImGui::Text("GPU: %.1f%%", t.gpuUsagePercent);

        ImGui::Spacing();

        // Status indicator
        if (hangDetected) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
            ImGui::Text("  HANG DETECTED");
            ImGui::PopStyleColor();
        } else if (vramWarn || t.vramUsagePercent > 80.0f) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.0f, 1.0f));
            ImGui::Text("  VRAM WARNING  (%.1f%%)", t.vramUsagePercent);
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 0.3f, 1.0f));
            ImGui::Text("  HEALTHY");
            ImGui::PopStyleColor();
        }

        // VRAM progress bar
        ImGui::SameLine(0, 30);
        char vramLabel[64];
        std::snprintf(vramLabel, sizeof(vramLabel), "%.1f%%", t.vramUsagePercent);
        ImVec4 barColor = (t.vramUsagePercent > threshold)
            ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f)
            : (t.vramUsagePercent > 80.0f ? ImVec4(1.0f, 0.85f, 0.0f, 1.0f)
                                          : ImVec4(0.0f, 0.75f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
        ImGui::ProgressBar(t.vramUsagePercent / 100.0f, ImVec2(220.0f, 18.0f), vramLabel);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("No GPU monitor attached.");
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// RenderGraphPanel
// ---------------------------------------------------------------------------
void DashboardUI::RenderGraphPanel() {
    ImGuiIO& io = ImGui::GetIO();
    // Positioned below the overview panel
    ImGui::SetNextWindowPos(ImVec2(0, 110.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.65f, 220.0f), ImGuiCond_Always);
    ImGui::Begin("VRAM & GPU History (60s)",
                 nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse);

    char vramOverlay[32], gpuOverlay[32];
    std::snprintf(vramOverlay, sizeof(vramOverlay), "VRAM: %.1f%%",
                  vramHistory_[(graphOffset_ + kGraphLen - 1) % kGraphLen]);
    std::snprintf(gpuOverlay, sizeof(gpuOverlay),  "GPU:  %.1f%%",
                  gpuHistory_ [(graphOffset_ + kGraphLen - 1) % kGraphLen]);

    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 0.85f, 0.3f, 1.0f));
    ImGui::PlotLines("VRAM %", vramHistory_, kGraphLen, graphOffset_,
                     vramOverlay, 0.0f, 100.0f, ImVec2(-1, 80));
    ImGui::Spacing();
    ImGui::PlotLines("GPU %",  gpuHistory_,  kGraphLen, graphOffset_,
                     gpuOverlay,  0.0f, 100.0f, ImVec2(-1, 80));
    ImGui::PopStyleColor();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// RenderCrashPanel
// ---------------------------------------------------------------------------
void DashboardUI::RenderCrashPanel() {
    ImGuiIO& io = ImGui::GetIO();
    const float panelX = io.DisplaySize.x * 0.65f;
    ImGui::SetNextWindowPos(ImVec2(panelX, 110.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x - panelX, io.DisplaySize.y - 110.0f - 220.0f),
        ImGuiCond_Always);
    ImGui::Begin("TDR / Crash Reports",
                 nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse);

    if (watcher_) {
        const auto& events = watcher_->GetRecentEvents();
        if (events.empty()) {
            ImGui::TextDisabled("No TDR events recorded this session.");
        } else {
            for (const auto& ev : events) {
                // Convert timestamp to human-readable string
                std::time_t tt = std::chrono::system_clock::to_time_t(ev.detectedAt);
                char timeBuf[64];
                std::tm tmBuf{};
#if defined(_MSC_VER)
                localtime_s(&tmBuf, &tt);
#else
                tmBuf = *std::localtime(&tt);
#endif
                std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);

                // Convert wstring source to narrow
                char srcA[128] = "Display";
                WideCharToMultiByte(CP_UTF8, 0, ev.source.c_str(), -1,
                                    srcA, sizeof(srcA), nullptr, nullptr);

                ImGui::Separator();
                ImGui::Text("Event %lu — %s — %s", static_cast<unsigned long>(ev.eventId),
                            srcA, timeBuf);

                if (!ev.reportPath.empty()) {
                    char btnLabel[128];
                    std::snprintf(btnLabel, sizeof(btnLabel), "Open Report##%lu",
                                  static_cast<unsigned long>(ev.eventId));
                    if (ImGui::SmallButton(btnLabel) && callbacks_.onOpenFolder) {
                        // Open the directory containing the report
                        std::wstring dir = ev.reportPath;
                        const auto slash = dir.find_last_of(L"\\/");
                        if (slash != std::wstring::npos)
                            dir.resize(slash);
                        callbacks_.onOpenFolder(dir);
                    }
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Open Reports Folder") && callbacks_.onOpenFolder)
            callbacks_.onOpenFolder(EventWatcher::GetCrashDir());
    } else {
        ImGui::TextDisabled("No event watcher attached.");
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// RenderSettingsPanel
// ---------------------------------------------------------------------------
void DashboardUI::RenderSettingsPanel() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 330.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.65f, io.DisplaySize.y - 330.0f),
                             ImGuiCond_Always);
    ImGui::Begin("Settings",
                 nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse);

    // --- Start with Windows ---
    bool autoStart = callbacks_.onGetAutoStart ? callbacks_.onGetAutoStart() : false;
    if (ImGui::Checkbox("Start with Windows", &autoStart)) {
        if (callbacks_.onSetAutoStart)
            callbacks_.onSetAutoStart(autoStart);
    }

    ImGui::Spacing();

    // --- VRAM Alert Threshold ---
    if (ImGui::SliderFloat("VRAM Alert Threshold %", &vramThresholdEdit_, 50.0f, 99.0f, "%.0f%%")) {
        if (callbacks_.onSetVramThreshold)
            callbacks_.onSetVramThreshold(vramThresholdEdit_);
        if (monitor_)
            monitor_->SetVramThreshold(vramThresholdEdit_);
    }

    ImGui::Spacing();

    // --- Global Hotkey (display only) ---
    ImGui::InputText("Global Hotkey", hotkeyScratch_, sizeof(hotkeyScratch_),
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hotkey configuration will be available in a future release.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Restart GPU Driver Now (danger button) ---
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button("Restart GPU Driver Now", ImVec2(220.0f, 32.0f)))
        showConfirmReset_ = true;
    ImGui::PopStyleColor(3);

    // Confirmation modal
    if (showConfirmReset_)
        ImGui::OpenPopup("Confirm Reset");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Reset", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoMove)) {
        ImGui::Text("This will force-restart the GPU driver.\n"
                    "All GPU workloads will be interrupted.\n\n"
                    "Proceed?");
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.10f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Yes, Restart Driver", ImVec2(160.0f, 0.0f))) {
            if (callbacks_.onRestartDriver)
                callbacks_.onRestartDriver();
            showConfirmReset_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f))) {
            showConfirmReset_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Quit ---
    if (ImGui::Button("Quit TDRGuard", ImVec2(140.0f, 28.0f))) {
        if (callbacks_.onQuit)
            callbacks_.onQuit();
    }

    ImGui::End();
}
