#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winevt.h>
#include <string>
#include <functional>
#include <vector>
#include <chrono>
#include <filesystem>
#include <array>

// Forward declaration
struct GpuTelemetry;
template<typename T, size_t N> class RingBuffer;

// Represents a captured crash/TDR event
struct TdrEvent {
    std::chrono::system_clock::time_point detectedAt;
    DWORD   eventId;        // e.g. 4101
    std::wstring source;    // e.g. L"Display"
    std::wstring message;   // XML excerpt or description
    std::wstring reportPath;// path to written .json report
};

// Callback signatures
using TdrDetectedCallback = std::function<void(const TdrEvent&)>;

class EventWatcher {
public:
    static constexpr DWORD kEventId_TdrRecovery     = 4101; // Display driver stopped & recovered
    static constexpr DWORD kEventId_LiveKernelEvent = 161;  // LiveKernelEvent (hang)
    static constexpr DWORD kEventId_WheatError      = 1;    // WHEA-Logger hardware error

    EventWatcher();
    ~EventWatcher();

    // Set callback — called on background thread when TDR detected
    void SetTdrCallback(TdrDetectedCallback cb) { tdrCb_ = std::move(cb); }

    // Start watching Windows Event Log (System + Application channels)
    // ringSnapshot: pointer to GpuMonitor's snapshot function (called at TDR time)
    bool StartWatching();
    void StopWatching();

    // Export ring buffer snapshot to crash report files
    // Returns path to the written .json file
    std::wstring ExportCrashReport(
        const TdrEvent& evt,
        const std::vector<GpuTelemetry>& telemetryHistory,
        const std::wstring& adapterName);

    // Get list of all past crash reports in %LocalAppData%\TDRGuard\Crashes
    std::vector<std::wstring> GetCrashReportPaths() const;

    // Get the crash reports directory path
    static std::wstring GetCrashDir();

    // Recent TDR events captured this session
    const std::vector<TdrEvent>& GetRecentEvents() const { return recentEvents_; }

private:
    static DWORD WINAPI EventCallbackStatic(
        EVT_SUBSCRIBE_NOTIFY_ACTION action,
        PVOID userContext,
        EVT_HANDLE event);

    void HandleEvent(EVT_HANDLE event);
    std::wstring RenderEventXml(EVT_HANDLE event);
    DWORD ExtractEventId(EVT_HANDLE event);

    EVT_HANDLE   subscriptionSystem_ = nullptr;
    EVT_HANDLE   subscriptionApp_    = nullptr;
    TdrDetectedCallback tdrCb_;
    std::vector<TdrEvent> recentEvents_;
    mutable SRWLOCK eventsLock_ = SRWLOCK_INIT;

    // Pointer to ring buffer snapshot — set via SetRingSnapshotFn
    std::function<std::vector<GpuTelemetry>()> ringSnapshotFn_;
public:
    void SetRingSnapshotFn(std::function<std::vector<GpuTelemetry>()> fn) {
        ringSnapshotFn_ = std::move(fn);
    }
};
