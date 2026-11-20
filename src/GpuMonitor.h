#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <string>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <chrono>

// Telemetry snapshot for a single point in time
struct GpuTelemetry {
    std::chrono::system_clock::time_point timestamp;
    uint64_t dedicatedVramUsed;   // bytes
    uint64_t dedicatedVramBudget; // bytes
    uint64_t sharedMemUsed;       // bytes
    float    gpuUsagePercent;     // 0.0–100.0 (PDH engine utilization)
    float    vramUsagePercent;    // dedicatedVramUsed / dedicatedVramBudget * 100
    bool     vramThresholdExceeded;
};

// Top process info (by GPU mem usage)
struct ProcessInfo {
    DWORD   pid;
    wchar_t name[MAX_PATH];
    uint64_t workingSetBytes;
};

// Lock-free ring buffer (capacity = 60 slots → 60 seconds at 1Hz poll)
template<typename T, size_t N>
class RingBuffer {
public:
    RingBuffer() : head_(0), count_(0) {}

    void push(const T& val) {
        size_t idx = head_.fetch_add(1, std::memory_order_relaxed) % N;
        data_[idx] = val;
        size_t c = count_.load(std::memory_order_relaxed);
        if (c < N) count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Returns up to N most recent items (oldest→newest)
    size_t snapshot(std::array<T, N>& out) const {
        size_t cnt  = count_.load(std::memory_order_acquire);
        size_t head = head_.load(std::memory_order_acquire);
        size_t start = (head >= cnt) ? (head - cnt) : 0;
        for (size_t i = 0; i < cnt; ++i) {
            out[i] = data_[(start + i) % N];
        }
        return cnt;
    }

    size_t size() const { return count_.load(std::memory_order_relaxed); }

private:
    std::array<T, N>    data_{};
    std::atomic<size_t> head_;
    std::atomic<size_t> count_;
};

class GpuMonitor {
public:
    static constexpr size_t kRingCapacity         = 60;
    static constexpr float  kDefaultVramThresholdPct = 90.0f;

    GpuMonitor();
    ~GpuMonitor();

    // Enumerate DXGI adapter, init PDH. Returns false only if DXGI fails.
    bool Initialize();
    void Shutdown();

    // Start/stop background polling thread (1 Hz)
    void StartPolling();
    void StopPolling();

    // Current snapshot (thread-safe copy)
    GpuTelemetry GetLatest() const;

    // Ring buffer snapshot for crash export (returns count filled)
    size_t GetRingSnapshot(std::array<GpuTelemetry, kRingCapacity>& out) const;

    // Top 5 processes by working set (approximate GPU process detection)
    std::array<ProcessInfo, 5> GetTopProcesses() const;

    // Adapter description string
    std::wstring GetAdapterName() const { return adapterName_; }

    // VRAM threshold (percent, 0–100)
    void  SetVramThreshold(float pct) { vramThresholdPct_.store(pct); }
    float GetVramThreshold() const    { return vramThresholdPct_.load(); }

    // Status flags (set by polling thread)
    bool IsVramWarning()   const { return vramWarning_.load(); }
    bool IsHangSuspected() const { return hangSuspected_.load(); }

private:
    void         PollLoop();
    GpuTelemetry SampleOnce();
    void         ScanProcesses();
    bool         InitDxgi();
    bool         InitPdh();
    void         ShutdownPdh();

    // DXGI
    IDXGIFactory4* dxgiFactory_ = nullptr;
    IDXGIAdapter3* dxgiAdapter_ = nullptr;
    std::wstring   adapterName_;

    // PDH – GPU Engine utilization counter
    PDH_HQUERY   pdhQuery_   = nullptr;
    PDH_HCOUNTER pdhCounter_ = nullptr;
    bool         pdhValid_   = false;

    // Ring buffer
    RingBuffer<GpuTelemetry, kRingCapacity> ring_;

    // Latest snapshot (guarded)
    mutable std::mutex latestMtx_;
    GpuTelemetry       latest_{};

    // Top processes (guarded)
    mutable std::mutex         procMtx_;
    std::array<ProcessInfo, 5> topProcs_{};

    // Background thread
    std::thread       pollThread_;
    std::atomic<bool> running_{ false };

    // Thresholds / status flags
    std::atomic<float> vramThresholdPct_{ kDefaultVramThresholdPct };
    std::atomic<bool>  vramWarning_{ false };
    std::atomic<bool>  hangSuspected_{ false };

    // Hang detection: consecutive polls where GPU was at 100 %
    int consecutiveHighGpu_ = 0;
};
