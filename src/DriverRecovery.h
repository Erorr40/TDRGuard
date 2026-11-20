#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <string>
#include <vector>
#include <functional>

// Result of a driver recovery operation
struct RecoveryResult {
    bool         success;
    std::wstring message;    // human-readable outcome
    DWORD        errorCode;  // GetLastError() or CM error code, 0 on success
};

class DriverRecovery {
public:
    DriverRecovery()  = default;
    ~DriverRecovery() = default;

    // Check if running elevated (IsUserAnAdmin / TokenElevation)
    static bool IsElevated();

    // Relaunch self with ShellExecuteEx "runas" verb for elevation.
    // Returns true if relaunch was initiated (caller should exit).
    static bool RelaunchElevated(const std::wstring& args = L"--restart-gpu");

    // Find all active display adapter device instance IDs.
    // Uses CM_Get_Device_ID_List with GUID_DEVCLASS_DISPLAY filter.
    static std::vector<std::wstring> EnumerateDisplayAdapters();

    // Perform a soft GPU driver reset:
    //   1. CM_Locate_DevNode for the device instance
    //   2. CM_Disable_DevNode (CM_DISABLE_UI_NOT_OK to suppress UI)
    //   3. Sleep 1500 ms
    //   4. CM_Enable_DevNode
    //   5. Sleep 500 ms
    // Requires elevation. Returns detailed RecoveryResult.
    static RecoveryResult ResetDisplayAdapter(const std::wstring& deviceInstanceId);

    // High-level entry point: enumerate adapters, pick first, reset it.
    // If not elevated: prompts for elevation via RelaunchElevated().
    // progressCb: called with status strings during the operation.
    static RecoveryResult PerformGpuReset(
        std::function<void(const std::wstring&)> progressCb = nullptr);

    // Check if a reset is already in progress (atomic flag).
    static bool IsResetInProgress();
};
