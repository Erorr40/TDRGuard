#include "EventWatcher.h"
#include "GpuMonitor.h"

#include <winevt.h>
#include <shlobj.h>
#include <psapi.h>
#include <objbase.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Escape a wide string for embedding in a JSON string value.
// Handles \, ", newlines, carriage returns, and tabs.
std::wstring JsonEscapeW(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 32);
    for (wchar_t ch : s) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        case L'\t': out += L"\\t";  break;
        default:
            if (ch < 0x20) {
                // Control characters — write as \uXXXX
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(ch));
                out += buf;
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

// Format a system_clock::time_point as "YYYY-MM-DD HH:MM:SS UTC" (narrow).
std::string FormatTimeUtc(const std::chrono::system_clock::time_point& tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    struct tm tmBuf{};
#if defined(_WIN32)
    gmtime_s(&tmBuf, &t);
#else
    gmtime_r(&t, &tmBuf);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmBuf);
    return buf;
}

// Format as "YYYYMMDD_HHMMSS" (local time) for use in filenames.
std::wstring FormatTimestampForFilename(const std::chrono::system_clock::time_point& tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    struct tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
        tmBuf.tm_year + 1900,
        tmBuf.tm_mon  + 1,
        tmBuf.tm_mday,
        tmBuf.tm_hour,
        tmBuf.tm_min,
        tmBuf.tm_sec);
    return buf;
}

// Write bytes to a file atomically via a .tmp intermediate.
// Returns true on success.
bool WriteFileAtomic(const std::wstring& finalPath,
                     const std::string&  utf8Content) {
    std::wstring tmpPath = finalPath + L".tmp";

    HANDLE h = CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    bool ok = (WriteFile(h,
                         utf8Content.data(),
                         static_cast<DWORD>(utf8Content.size()),
                         &written,
                         nullptr) == TRUE)
              && (written == static_cast<DWORD>(utf8Content.size()));
    CloseHandle(h);

    if (!ok) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    return MoveFileExW(tmpPath.c_str(),
                       finalPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// Narrow UTF-8 from wide string (best-effort, BMP only via WideCharToMultiByte).
std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0,
                                 w.c_str(), static_cast<int>(w.size()),
                                 nullptr, 0, nullptr, nullptr);
    if (sz <= 0) return {};
    std::string s(static_cast<size_t>(sz), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
                        w.c_str(), static_cast<int>(w.size()),
                        s.data(), sz, nullptr, nullptr);
    return s;
}

// Ensure a directory (and all parents) exists.  Ignores ERROR_ALREADY_EXISTS.
void EnsureDirectoryExists(const std::wstring& path) {
    // Try creating the full path first.
    if (CreateDirectoryW(path.c_str(), nullptr)) return;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return;

    // Walk and create each component.
    std::wstring current;
    current.reserve(path.size());
    size_t pos = 0;
    while (pos < path.size()) {
        size_t sep = path.find_first_of(L"\\/", pos);
        if (sep == std::wstring::npos) sep = path.size();
        current += path.substr(pos, sep - pos);
        if (!current.empty()) {
            CreateDirectoryW(current.c_str(), nullptr);
            // Ignore errors; the final CreateDirectoryW call below is authoritative.
        }
        if (sep < path.size()) current += path[sep];
        pos = sep + 1;
    }
    // One more attempt at the full path.
    CreateDirectoryW(path.c_str(), nullptr);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// EventWatcher ctor / dtor
// ---------------------------------------------------------------------------

EventWatcher::EventWatcher() = default;

EventWatcher::~EventWatcher() {
    StopWatching();
}

// ---------------------------------------------------------------------------
// GetCrashDir
// ---------------------------------------------------------------------------

/*static*/ std::wstring EventWatcher::GetCrashDir() {
    std::wstring base;

    // Preferred: SHGetKnownFolderPath (Vista+)
    PWSTR knownPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &knownPath))) {
        base = knownPath;
        CoTaskMemFree(knownPath);
    } else {
        // Fallback: environment variable
        wchar_t envBuf[MAX_PATH]{};
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", envBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            base = envBuf;
        }
    }

    if (base.empty()) {
        // Last resort: current directory
        wchar_t cwd[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        base = cwd;
    }

    std::wstring crashDir = base + L"\\TDRGuard\\Crashes";
    EnsureDirectoryExists(crashDir);
    return crashDir;
}

// ---------------------------------------------------------------------------
// StartWatching / StopWatching
// ---------------------------------------------------------------------------

bool EventWatcher::StartWatching() {
    // System channel: TDR recovery (4101) and LiveKernelEvent (161)
    const wchar_t* sysQuery =
        L"*[System[(EventID=4101 or EventID=161)]]";

    subscriptionSystem_ = EvtSubscribe(
        nullptr,                        // local machine session
        nullptr,                        // no signal event
        L"System",                      // channel
        sysQuery,                       // XPath query
        nullptr,                        // bookmark
        this,                           // user context
        EventCallbackStatic,            // callback
        EvtSubscribeToFutureEvents);    // future events only

    // Application channel: WHEA-Logger (EventID 1)
    const wchar_t* appQuery =
        L"*[System[Provider[@Name='WHEA-Logger'] and EventID=1]]";

    subscriptionApp_ = EvtSubscribe(
        nullptr,
        nullptr,
        L"Application",
        appQuery,
        nullptr,
        this,
        EventCallbackStatic,
        EvtSubscribeToFutureEvents);

    // Application failure is non-fatal; return true if System subscription succeeded.
    return subscriptionSystem_ != nullptr;
}

void EventWatcher::StopWatching() {
    if (subscriptionSystem_) {
        EvtClose(subscriptionSystem_);
        subscriptionSystem_ = nullptr;
    }
    if (subscriptionApp_) {
        EvtClose(subscriptionApp_);
        subscriptionApp_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// EventCallbackStatic
// ---------------------------------------------------------------------------

/*static*/ DWORD WINAPI EventWatcher::EventCallbackStatic(
    EVT_SUBSCRIBE_NOTIFY_ACTION action,
    PVOID                       userContext,
    EVT_HANDLE                  event)
{
    if (action == EvtSubscribeActionDeliver && userContext != nullptr) {
        auto* self = static_cast<EventWatcher*>(userContext);
        self->HandleEvent(event);
    }
    return ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// HandleEvent
// ---------------------------------------------------------------------------

void EventWatcher::HandleEvent(EVT_HANDLE event) {
    DWORD evtId = ExtractEventId(event);

    // Filter: only handle events we care about
    if (evtId != kEventId_TdrRecovery &&
        evtId != kEventId_LiveKernelEvent &&
        evtId != kEventId_WheatError) {
        return;
    }

    std::wstring xmlStr = RenderEventXml(event);

    // Build TdrEvent
    TdrEvent tdr{};
    tdr.detectedAt = std::chrono::system_clock::now();
    tdr.eventId    = evtId;
    tdr.source     = L"System";
    tdr.message    = xmlStr.substr(0, 2000);

    // Capture telemetry snapshot if available
    std::vector<GpuTelemetry> history;
    if (ringSnapshotFn_) {
        history = ringSnapshotFn_();
    }

    // Determine adapter name from first telemetry entry (best effort)
    std::wstring adapterName = L"Unknown GPU";

    // Export crash report
    tdr.reportPath = ExportCrashReport(tdr, history, adapterName);

    // Store event (exclusive lock)
    AcquireSRWLockExclusive(&eventsLock_);
    recentEvents_.push_back(tdr);
    ReleaseSRWLockExclusive(&eventsLock_);

    // Fire user callback
    if (tdrCb_) {
        tdrCb_(tdr);
    }
}

// ---------------------------------------------------------------------------
// RenderEventXml
// ---------------------------------------------------------------------------

std::wstring EventWatcher::RenderEventXml(EVT_HANDLE event) {
    DWORD bufferUsed  = 0;
    DWORD propCount   = 0;

    // First call: get required buffer size
    EvtRender(nullptr, event, EvtRenderEventXml,
              0, nullptr, &bufferUsed, &propCount);

    if (bufferUsed == 0) {
        return {};
    }

    std::vector<WCHAR> buf(bufferUsed / sizeof(WCHAR) + 1, L'\0');

    if (!EvtRender(nullptr, event, EvtRenderEventXml,
                   bufferUsed, buf.data(), &bufferUsed, &propCount)) {
        return {};
    }

    std::wstring xml(buf.data());
    if (xml.size() > 2000) {
        xml.resize(2000);
    }
    return xml;
}

// ---------------------------------------------------------------------------
// ExtractEventId
// ---------------------------------------------------------------------------

DWORD EventWatcher::ExtractEventId(EVT_HANDLE event) {
    // Create a system render context to extract well-known system values.
    EVT_HANDLE renderCtx = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
    if (renderCtx == nullptr) {
        // Fallback: parse from XML
        std::wstring xml = RenderEventXml(event);
        const wchar_t* tag = L"<EventID>";
        size_t pos = xml.find(tag);
        if (pos != std::wstring::npos) {
            pos += wcslen(tag);
            return static_cast<DWORD>(_wtoi(xml.c_str() + pos));
        }
        return 0;
    }

    DWORD bufferUsed = 0;
    DWORD propCount  = 0;

    // First call to determine required size
    EvtRender(renderCtx, event, EvtRenderEventValues,
              0, nullptr, &bufferUsed, &propCount);

    std::vector<BYTE> valueBuf(bufferUsed, 0);

    BOOL ok = EvtRender(renderCtx, event, EvtRenderEventValues,
                        bufferUsed,
                        valueBuf.data(),
                        &bufferUsed,
                        &propCount);

    EvtClose(renderCtx);

    if (!ok || propCount == 0) {
        // Fallback: parse XML
        std::wstring xml = RenderEventXml(event);
        const wchar_t* tag = L"<EventID>";
        size_t pos = xml.find(tag);
        if (pos != std::wstring::npos) {
            pos += wcslen(tag);
            return static_cast<DWORD>(_wtoi(xml.c_str() + pos));
        }
        return 0;
    }

    // EvtSystemEventID is index 1 in the system property array (0-based).
    // The buffer is an array of EVT_VARIANT structures.
    auto* variants = reinterpret_cast<EVT_VARIANT*>(valueBuf.data());

    // EvtSystemEventID == 1 (0-based index in EvtRenderContextSystem values)
    // Guard against under-sized result
    if (propCount > EvtSystemEventID) {
        const EVT_VARIANT& v = variants[EvtSystemEventID];
        if (v.Type == EvtVarTypeUInt16) {
            return static_cast<DWORD>(v.UInt16Val);
        }
        if (v.Type == EvtVarTypeUInt32) {
            return v.UInt32Val;
        }
    }

    // Fallback: parse XML
    std::wstring xml = RenderEventXml(event);
    const wchar_t* tag = L"<EventID>";
    size_t pos = xml.find(tag);
    if (pos != std::wstring::npos) {
        pos += wcslen(tag);
        return static_cast<DWORD>(_wtoi(xml.c_str() + pos));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ExportCrashReport
// ---------------------------------------------------------------------------

std::wstring EventWatcher::ExportCrashReport(
    const TdrEvent&              evt,
    const std::vector<GpuTelemetry>& telemetryHistory,
    const std::wstring&          adapterName)
{
    std::wstring crashDir = GetCrashDir();
    std::wstring stem     = L"TDR_" + FormatTimestampForFilename(evt.detectedAt);
    std::wstring jsonPath = crashDir + L"\\" + stem + L".json";
    std::wstring txtPath  = crashDir + L"\\" + stem + L".txt";

    // ----------------------------------------------------------------
    // Build JSON (manually — no external library)
    // ----------------------------------------------------------------
    std::string evtTimeStr  = FormatTimeUtc(evt.detectedAt);
    std::wstring escapedXml = JsonEscapeW(evt.message);
    std::wstring escapedSrc = JsonEscapeW(evt.source);
    std::wstring escapedAda = JsonEscapeW(adapterName);

    std::string json;
    json.reserve(4096 + telemetryHistory.size() * 128);

    json += "{\n";

    // event block
    json += "  \"event\": {\n";
    json += "    \"id\": ";
    json += std::to_string(evt.eventId);
    json += ",\n";
    json += "    \"time\": \"";
    json += evtTimeStr;
    json += "\",\n";
    json += "    \"source\": \"";
    json += WToUtf8(escapedSrc);
    json += "\",\n";
    json += "    \"xml\": \"";
    json += WToUtf8(escapedXml);
    json += "\"\n";
    json += "  },\n";

    // adapter
    json += "  \"adapter\": \"";
    json += WToUtf8(escapedAda);
    json += "\",\n";

    // telemetry count
    json += "  \"telemetry_count\": ";
    json += std::to_string(telemetryHistory.size());
    json += ",\n";

    // telemetry array
    json += "  \"telemetry\": [\n";
    for (size_t i = 0; i < telemetryHistory.size(); ++i) {
        const GpuTelemetry& t = telemetryHistory[i];
        std::string tsStr = FormatTimeUtc(t.timestamp);
        double vramUsedMb   = static_cast<double>(t.dedicatedVramUsed)   / (1024.0 * 1024.0);
        double vramBudgetMb = static_cast<double>(t.dedicatedVramBudget) / (1024.0 * 1024.0);

        char entry[512];
        snprintf(entry, sizeof(entry),
            "    { \"ts\": \"%s\", \"vram_used_mb\": %.2f, \"vram_budget_mb\": %.2f, "
            "\"vram_pct\": %.2f, \"gpu_pct\": %.2f }",
            tsStr.c_str(),
            vramUsedMb,
            vramBudgetMb,
            static_cast<double>(t.vramUsagePercent),
            static_cast<double>(t.gpuUsagePercent));

        json += entry;
        if (i + 1 < telemetryHistory.size()) json += ',';
        json += '\n';
    }
    json += "  ]\n";
    json += "}\n";

    WriteFileAtomic(jsonPath, json);

    // ----------------------------------------------------------------
    // Build human-readable .txt companion
    // ----------------------------------------------------------------
    std::string txt;
    txt.reserve(1024);
    txt += "=== TDRGuard Crash Report ===\n";
    txt += "Event Time : " + evtTimeStr + "\n";
    txt += "Event ID   : " + std::to_string(evt.eventId) + "\n";
    txt += "Source     : " + WToUtf8(evt.source) + "\n";
    txt += "Adapter    : " + WToUtf8(adapterName) + "\n";
    txt += "Report     : " + WToUtf8(jsonPath) + "\n";
    txt += "\n--- Telemetry Summary (" + std::to_string(telemetryHistory.size()) + " samples) ---\n";

    if (!telemetryHistory.empty()) {
        const GpuTelemetry& last = telemetryHistory.back();
        char summary[256];
        snprintf(summary, sizeof(summary),
            "Last sample  : VRAM %.1f / %.1f MB (%.1f%%), GPU %.1f%%\n",
            static_cast<double>(last.dedicatedVramUsed)   / (1024.0 * 1024.0),
            static_cast<double>(last.dedicatedVramBudget) / (1024.0 * 1024.0),
            static_cast<double>(last.vramUsagePercent),
            static_cast<double>(last.gpuUsagePercent));
        txt += summary;
    }

    txt += "\n--- Event XML Excerpt ---\n";
    txt += WToUtf8(evt.message);
    txt += "\n";

    WriteFileAtomic(txtPath, txt);

    return jsonPath;
}

// ---------------------------------------------------------------------------
// GetCrashReportPaths
// ---------------------------------------------------------------------------

std::vector<std::wstring> EventWatcher::GetCrashReportPaths() const {
    std::wstring crashDir = GetCrashDir();
    std::wstring pattern  = crashDir + L"\\*.json";

    std::vector<std::wstring> paths;

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return paths;
    }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            paths.push_back(crashDir + L"\\" + fd.cFileName);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // Sort descending by filename (filenames encode ISO-like timestamp, so
    // lexicographic descending == newest first).
    std::sort(paths.begin(), paths.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return a > b;
              });

    return paths;
}
