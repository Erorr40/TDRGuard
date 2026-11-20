// GpuMonitor.cpp – GPU telemetry backend for TDRGuard
// Requires Windows 10+ (DXGI 1.4, PDH GPU Engine counter).

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "GpuMonitor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

GpuMonitor::GpuMonitor() = default;

GpuMonitor::~GpuMonitor()
{
    Shutdown();
}

// ---------------------------------------------------------------------------
// Public lifecycle
// ---------------------------------------------------------------------------

bool GpuMonitor::Initialize()
{
    if (!InitDxgi()) {
        return false;        // DXGI is mandatory; fail hard.
    }
    InitPdh();               // PDH failure is non-fatal; pdhValid_ stays false.
    return true;
}

void GpuMonitor::Shutdown()
{
    StopPolling();

    if (dxgiAdapter_) {
        dxgiAdapter_->Release();
        dxgiAdapter_ = nullptr;
    }
    if (dxgiFactory_) {
        dxgiFactory_->Release();
        dxgiFactory_ = nullptr;
    }

    ShutdownPdh();
}

// ---------------------------------------------------------------------------
// Polling thread control
// ---------------------------------------------------------------------------

void GpuMonitor::StartPolling()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;  // already running
    }
    pollThread_ = std::thread(&GpuMonitor::PollLoop, this);
}

void GpuMonitor::StopPolling()
{
    running_.store(false);
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
}

// ---------------------------------------------------------------------------
// Public accessors (thread-safe)
// ---------------------------------------------------------------------------

GpuTelemetry GpuMonitor::GetLatest() const
{
    std::lock_guard<std::mutex> lk(latestMtx_);
    return latest_;
}

size_t GpuMonitor::GetRingSnapshot(std::array<GpuTelemetry, kRingCapacity>& out) const
{
    return ring_.snapshot(out);
}

std::array<ProcessInfo, 5> GpuMonitor::GetTopProcesses() const
{
    std::lock_guard<std::mutex> lk(procMtx_);
    return topProcs_;
}

// ---------------------------------------------------------------------------
// DXGI initialisation
// ---------------------------------------------------------------------------

bool GpuMonitor::InitDxgi()
{
    // Obtain IDXGIFactory4 via CreateDXGIFactory1 + QueryInterface.
    IDXGIFactory1* factory1 = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory1));
    if (FAILED(hr) || !factory1) {
        return false;
    }

    hr = factory1->QueryInterface(__uuidof(IDXGIFactory4),
                                  reinterpret_cast<void**>(&dxgiFactory_));
    factory1->Release();

    if (FAILED(hr) || !dxgiFactory_) {
        return false;
    }

    // Enumerate adapters; pick the first hardware (non-software) adapter.
    IDXGIAdapter1* adapter1 = nullptr;
    for (UINT i = 0; dxgiFactory_->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter1->GetDesc1(&desc))) {
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                // Upgrade to IDXGIAdapter3 for QueryVideoMemoryInfo.
                IDXGIAdapter3* adapter3 = nullptr;
                if (SUCCEEDED(adapter1->QueryInterface(__uuidof(IDXGIAdapter3),
                                                       reinterpret_cast<void**>(&adapter3)))) {
                    dxgiAdapter_ = adapter3;
                    adapterName_ = desc.Description;
                    adapter1->Release();
                    return true;
                }
            }
        }
        adapter1->Release();
        adapter1 = nullptr;
    }

    // No suitable hardware adapter found.
    return false;
}

// ---------------------------------------------------------------------------
// PDH initialisation / teardown
// ---------------------------------------------------------------------------

bool GpuMonitor::InitPdh()
{
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &pdhQuery_);
    if (status != ERROR_SUCCESS) {
        pdhQuery_ = nullptr;
        pdhValid_ = false;
        return false;
    }

    // The "GPU Engine" counter path works on Windows 10 1903+.
    // On older builds the counter may not exist; degrade gracefully.
    status = PdhAddCounterW(pdhQuery_,
                            L"\\GPU Engine(*)\\Utilization Percentage",
                            0,
                            &pdhCounter_);
    if (status != ERROR_SUCCESS) {
        pdhCounter_ = nullptr;
        pdhValid_   = false;
        return false;
    }

    // Prime the counter with an initial collection (PDH needs two samples
    // before it can compute a meaningful rate value).
    PdhCollectQueryData(pdhQuery_);

    pdhValid_ = true;
    return true;
}

void GpuMonitor::ShutdownPdh()
{
    if (pdhQuery_) {
        PdhCloseQuery(pdhQuery_);
        pdhQuery_   = nullptr;
        pdhCounter_ = nullptr;
        pdhValid_   = false;
    }
}

// ---------------------------------------------------------------------------
// SampleOnce – capture one telemetry snapshot
// ---------------------------------------------------------------------------

GpuTelemetry GpuMonitor::SampleOnce()
{
    GpuTelemetry t{};
    t.timestamp = std::chrono::system_clock::now();

    // ---- VRAM via DXGI ------------------------------------------------
    if (dxgiAdapter_) {
        DXGI_QUERY_VIDEO_MEMORY_INFO localInfo{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo{};

        if (SUCCEEDED(dxgiAdapter_->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo))) {
            t.dedicatedVramUsed   = localInfo.CurrentUsage;
            t.dedicatedVramBudget = localInfo.Budget;
        }

        if (SUCCEEDED(dxgiAdapter_->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo))) {
            t.sharedMemUsed = nonLocalInfo.CurrentUsage;
        }

        if (t.dedicatedVramBudget > 0) {
            t.vramUsagePercent = static_cast<float>(t.dedicatedVramUsed) /
                                 static_cast<float>(t.dedicatedVramBudget) * 100.0f;
        }
    }

    // ---- GPU utilization via PDH --------------------------------------
    float gpuUsage = 0.0f;

    if (pdhValid_ && pdhQuery_ && pdhCounter_) {
        PDH_STATUS collectStatus = PdhCollectQueryData(pdhQuery_);

        if (collectStatus == ERROR_SUCCESS) {
            // Query the array of formatted counter values (one per GPU engine instance).
            DWORD   bufSize    = 0;
            DWORD   itemCount  = 0;

            // First call: determine required buffer size.
            PDH_STATUS arraySt = PdhGetFormattedCounterArrayW(
                pdhCounter_,
                PDH_FMT_DOUBLE,
                &bufSize,
                &itemCount,
                nullptr);

            if ((arraySt == PDH_MORE_DATA || arraySt == ERROR_SUCCESS) && bufSize > 0) {
                std::vector<BYTE> buf(bufSize, 0);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());

                arraySt = PdhGetFormattedCounterArrayW(
                    pdhCounter_,
                    PDH_FMT_DOUBLE,
                    &bufSize,
                    &itemCount,
                    items);

                if (arraySt == ERROR_SUCCESS && itemCount > 0) {
                    double sum = 0.0;
                    DWORD  valid = 0;
                    for (DWORD i = 0; i < itemCount; ++i) {
                        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                            items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
                            sum += items[i].FmtValue.doubleValue;
                            ++valid;
                        }
                    }
                    if (valid > 0) {
                        gpuUsage = static_cast<float>(sum / static_cast<double>(valid));
                    }
                }
            }
        }
    }

    // Clamp to [0, 100]
    gpuUsage = std::max(0.0f, std::min(100.0f, gpuUsage));
    t.gpuUsagePercent = gpuUsage;

    // ---- Threshold / hang detection -----------------------------------
    float threshold = vramThresholdPct_.load(std::memory_order_relaxed);

    bool vramOver = (t.vramUsagePercent > threshold);
    t.vramThresholdExceeded = vramOver;
    vramWarning_.store(vramOver, std::memory_order_relaxed);

    // Hang detection: 5+ consecutive polls at exactly 100 % GPU utilization.
    if (gpuUsage >= 99.9f) {
        ++consecutiveHighGpu_;
    } else {
        consecutiveHighGpu_ = 0;
    }
    hangSuspected_.store(consecutiveHighGpu_ >= 5, std::memory_order_relaxed);

    return t;
}

// ---------------------------------------------------------------------------
// ScanProcesses – enumerate top 5 by working set
// ---------------------------------------------------------------------------

void GpuMonitor::ScanProcesses()
{
    struct ProcEntry {
        DWORD    pid;
        wchar_t  name[MAX_PATH];
        uint64_t workingSet;
    };

    std::vector<ProcEntry> entries;
    entries.reserve(256);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE proc = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE,
                pe.th32ProcessID);

            if (proc) {
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(proc,
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                         sizeof(pmc))) {
                    ProcEntry entry{};
                    entry.pid        = pe.th32ProcessID;
                    entry.workingSet = static_cast<uint64_t>(pmc.WorkingSetSize);
                    // Safely copy process name
                    wcsncpy_s(entry.name, MAX_PATH, pe.szExeFile, _TRUNCATE);
                    entries.push_back(entry);
                }
                CloseHandle(proc);
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);

    // Sort descending by working set.
    std::sort(entries.begin(), entries.end(),
              [](const ProcEntry& a, const ProcEntry& b) {
                  return a.workingSet > b.workingSet;
              });

    // Store top 5 under lock.
    std::array<ProcessInfo, 5> top{};
    size_t count = std::min<size_t>(5, entries.size());
    for (size_t i = 0; i < count; ++i) {
        top[i].pid             = entries[i].pid;
        top[i].workingSetBytes = entries[i].workingSet;
        wcsncpy_s(top[i].name, MAX_PATH, entries[i].name, _TRUNCATE);
    }

    std::lock_guard<std::mutex> lk(procMtx_);
    topProcs_ = top;
}

// ---------------------------------------------------------------------------
// PollLoop – background 1 Hz polling thread
// ---------------------------------------------------------------------------

void GpuMonitor::PollLoop()
{
    int pollCount = 0;

    while (running_.load(std::memory_order_relaxed)) {
        GpuTelemetry sample = SampleOnce();

        ring_.push(sample);

        {
            std::lock_guard<std::mutex> lk(latestMtx_);
            latest_ = sample;
        }

        ++pollCount;
        // ScanProcesses every 5 polls to reduce overhead.
        if (pollCount % 5 == 0) {
            ScanProcesses();
        }

        // Sleep approximately 1 second, checking running_ flag every 100 ms
        // to allow responsive shutdown.
        for (int slice = 0; slice < 10 && running_.load(std::memory_order_relaxed); ++slice) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
