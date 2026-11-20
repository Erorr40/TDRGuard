// DriverRecovery.cpp — GPU soft-reset via Configuration Manager API
// Part of TDRGuard, a C++20 Windows GPU watchdog utility.

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shell32.lib")

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <devguid.h>
#include <objbase.h>   // StringFromGUID2

#include <atomic>
#include <string>
#include <vector>
#include <functional>

#include "DriverRecovery.h"

// ---------------------------------------------------------------------------
// File-static reset-in-progress flag
// ---------------------------------------------------------------------------
static std::atomic<bool> resetInProgress_{ false };

// ---------------------------------------------------------------------------
// Helper: map common CONFIGRET codes to a human-readable wstring
// ---------------------------------------------------------------------------
static std::wstring CrToString(CONFIGRET cr)
{
    switch (cr)
    {
    case CR_SUCCESS:                  return L"CR_SUCCESS";
    case CR_DEFAULT:                  return L"CR_DEFAULT";
    case CR_OUT_OF_MEMORY:            return L"CR_OUT_OF_MEMORY";
    case CR_INVALID_POINTER:          return L"CR_INVALID_POINTER";
    case CR_INVALID_FLAG:             return L"CR_INVALID_FLAG";
    case CR_INVALID_DEVNODE:          return L"CR_INVALID_DEVNODE";
    case CR_INVALID_RES_DES:          return L"CR_INVALID_RES_DES";
    case CR_INVALID_LOG_CONF:         return L"CR_INVALID_LOG_CONF";
    case CR_INVALID_ARBITRATOR:       return L"CR_INVALID_ARBITRATOR";
    case CR_INVALID_NODELIST:         return L"CR_INVALID_NODELIST";
    case CR_DEVNODE_HAS_REQS:         return L"CR_DEVNODE_HAS_REQS";
    case CR_INVALID_RESOURCEID:       return L"CR_INVALID_RESOURCEID";
    case CR_DLVXD_NOT_FOUND:          return L"CR_DLVXD_NOT_FOUND";
    case CR_NO_SUCH_DEVNODE:          return L"CR_NO_SUCH_DEVNODE";
    case CR_NO_MORE_LOG_CONF:         return L"CR_NO_MORE_LOG_CONF";
    case CR_NO_MORE_RES_DES:          return L"CR_NO_MORE_RES_DES";
    case CR_ALREADY_SUCH_DEVNODE:     return L"CR_ALREADY_SUCH_DEVNODE";
    case CR_INVALID_RANGE_LIST:       return L"CR_INVALID_RANGE_LIST";
    case CR_INVALID_RANGE:            return L"CR_INVALID_RANGE";
    case CR_FAILURE:                  return L"CR_FAILURE";
    case CR_NO_SUCH_LOGICAL_DEV:      return L"CR_NO_SUCH_LOGICAL_DEV";
    case CR_CREATE_BLOCKED:           return L"CR_CREATE_BLOCKED";
    case CR_NOT_SYSTEM_VM:            return L"CR_NOT_SYSTEM_VM";
    case CR_REMOVE_VETOED:            return L"CR_REMOVE_VETOED";
    case CR_APM_VETOED:               return L"CR_APM_VETOED";
    case CR_INVALID_LOAD_TYPE:        return L"CR_INVALID_LOAD_TYPE";
    case CR_BUFFER_SMALL:             return L"CR_BUFFER_SMALL";
    case CR_NO_ARBITRATOR:            return L"CR_NO_ARBITRATOR";
    case CR_NO_REGISTRY_HANDLE:       return L"CR_NO_REGISTRY_HANDLE";
    case CR_REGISTRY_ERROR:           return L"CR_REGISTRY_ERROR";
    case CR_INVALID_DEVICE_ID:        return L"CR_INVALID_DEVICE_ID";
    case CR_INVALID_DATA:             return L"CR_INVALID_DATA";
    case CR_INVALID_API:              return L"CR_INVALID_API";
    case CR_DEVLOADER_NOT_READY:      return L"CR_DEVLOADER_NOT_READY";
    case CR_NEED_RESTART:             return L"CR_NEED_RESTART";
    case CR_NO_MORE_HW_PROFILES:      return L"CR_NO_MORE_HW_PROFILES";
    case CR_DEVICE_NOT_THERE:         return L"CR_DEVICE_NOT_THERE";
    case CR_NO_SUCH_VALUE:            return L"CR_NO_SUCH_VALUE";
    case CR_WRONG_TYPE:               return L"CR_WRONG_TYPE";
    case CR_INVALID_PRIORITY:         return L"CR_INVALID_PRIORITY";
    case CR_NOT_DISABLEABLE:          return L"CR_NOT_DISABLEABLE";
    case CR_FREE_RESOURCES:           return L"CR_FREE_RESOURCES";
    case CR_QUERY_VETOED:             return L"CR_QUERY_VETOED";
    case CR_CANT_SHARE_IRQ:           return L"CR_CANT_SHARE_IRQ";
    case CR_NO_DEPENDENT:             return L"CR_NO_DEPENDENT";
    case CR_SAME_RESOURCES:           return L"CR_SAME_RESOURCES";
    case CR_NO_SUCH_REGISTRY_KEY:     return L"CR_NO_SUCH_REGISTRY_KEY";
    case CR_INVALID_MACHINENAME:      return L"CR_INVALID_MACHINENAME";
    case CR_REMOTE_COMM_FAILURE:      return L"CR_REMOTE_COMM_FAILURE";
    case CR_MACHINE_UNAVAILABLE:      return L"CR_MACHINE_UNAVAILABLE";
    case CR_NO_CM_SERVICES:           return L"CR_NO_CM_SERVICES";
    case CR_ACCESS_DENIED:            return L"CR_ACCESS_DENIED";
    case CR_CALL_NOT_IMPLEMENTED:     return L"CR_CALL_NOT_IMPLEMENTED";
    case CR_INVALID_PROPERTY:         return L"CR_INVALID_PROPERTY";
    case CR_DEVICE_INTERFACE_ACTIVE:  return L"CR_DEVICE_INTERFACE_ACTIVE";
    case CR_NO_SUCH_DEVICE_INTERFACE: return L"CR_NO_SUCH_DEVICE_INTERFACE";
    case CR_INVALID_REFERENCE_STRING: return L"CR_INVALID_REFERENCE_STRING";
    case CR_INVALID_CONFLICT_LIST:    return L"CR_INVALID_CONFLICT_LIST";
    case CR_INVALID_INDEX:            return L"CR_INVALID_INDEX";
    case CR_INVALID_STRUCTURE_SIZE:   return L"CR_INVALID_STRUCTURE_SIZE";
    default:
    {
        wchar_t buf[32];
        swprintf_s(buf, L"CR_UNKNOWN(0x%08X)", static_cast<unsigned>(cr));
        return buf;
    }
    }
}

// ---------------------------------------------------------------------------
// IsElevated
// ---------------------------------------------------------------------------
bool DriverRecovery::IsElevated()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;

    TOKEN_ELEVATION elevation{};
    DWORD cbSize = sizeof(elevation);
    BOOL ok = GetTokenInformation(hToken, TokenElevation, &elevation, cbSize, &cbSize);
    CloseHandle(hToken);

    if (!ok)
        return false;

    return elevation.TokenIsElevated != 0;
}

// ---------------------------------------------------------------------------
// RelaunchElevated
// ---------------------------------------------------------------------------
bool DriverRecovery::RelaunchElevated(const std::wstring& args)
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return false;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd         = nullptr;
    sei.lpVerb       = L"runas";
    sei.lpFile       = exePath;
    sei.lpParameters = args.c_str();
    sei.lpDirectory  = nullptr;
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei))
        return false;

    if (sei.hProcess)
    {
        CloseHandle(sei.hProcess);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// EnumerateDisplayAdapters
// ---------------------------------------------------------------------------
std::vector<std::wstring> DriverRecovery::EnumerateDisplayAdapters()
{
    std::vector<std::wstring> adapters;

    // Convert GUID_DEVCLASS_DISPLAY to a wide string filter
    // Format: {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    wchar_t guidStr[64] = {};
    if (StringFromGUID2(GUID_DEVCLASS_DISPLAY, guidStr, static_cast<int>(std::size(guidStr))) == 0)
        return adapters;

    // Query required buffer size
    ULONG bufferLen = 0;
    CONFIGRET cr = CM_Get_Device_ID_List_SizeW(
        &bufferLen,
        guidStr,
        CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT);

    if (cr != CR_SUCCESS || bufferLen == 0)
        return adapters;

    // Allocate and fill the multi-string buffer
    std::vector<wchar_t> buffer(bufferLen, L'\0');
    cr = CM_Get_Device_ID_ListW(
        guidStr,
        buffer.data(),
        bufferLen,
        CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT);

    if (cr != CR_SUCCESS)
        return adapters;

    // Parse the double-null–terminated multi-string
    const wchar_t* p = buffer.data();
    while (p && *p != L'\0')
    {
        adapters.emplace_back(p);
        p += adapters.back().size() + 1;  // advance past the string and its null terminator
    }

    return adapters;
}

// ---------------------------------------------------------------------------
// ResetDisplayAdapter
// ---------------------------------------------------------------------------
RecoveryResult DriverRecovery::ResetDisplayAdapter(const std::wstring& deviceInstanceId)
{
    if (deviceInstanceId.empty())
    {
        return RecoveryResult{
            false,
            L"ResetDisplayAdapter: device instance ID must not be empty.",
            ERROR_INVALID_PARAMETER
        };
    }

    // Locate the devnode
    DEVINST devInst = 0;
    CONFIGRET cr = CM_Locate_DevNodeW(
        &devInst,
        const_cast<DEVINSTID_W>(deviceInstanceId.c_str()),
        CM_LOCATE_DEVNODE_NORMAL);

    if (cr != CR_SUCCESS)
    {
        return RecoveryResult{
            false,
            L"CM_Locate_DevNodeW failed for '" + deviceInstanceId +
                L"': " + CrToString(cr),
            static_cast<DWORD>(cr)
        };
    }

    // Disable the device node (suppress any driver-generated UI)
    cr = CM_Disable_DevNode(devInst, CM_DISABLE_UI_NOT_OK);
    if (cr != CR_SUCCESS)
    {
        return RecoveryResult{
            false,
            L"CM_Disable_DevNode failed: " + CrToString(cr),
            static_cast<DWORD>(cr)
        };
    }

    // Allow the device to fully settle in the disabled state
    Sleep(1500);

    // Re-enable the device node
    cr = CM_Enable_DevNode(devInst, 0);
    if (cr != CR_SUCCESS)
    {
        return RecoveryResult{
            false,
            L"CM_Enable_DevNode failed: " + CrToString(cr),
            static_cast<DWORD>(cr)
        };
    }

    // Allow the driver to finish re-initialization
    Sleep(500);

    return RecoveryResult{
        true,
        L"GPU driver reset successful for: " + deviceInstanceId,
        0
    };
}

// ---------------------------------------------------------------------------
// PerformGpuReset
// ---------------------------------------------------------------------------
RecoveryResult DriverRecovery::PerformGpuReset(
    std::function<void(const std::wstring&)> progressCb)
{
    // Guard against concurrent resets
    bool expected = false;
    if (!resetInProgress_.compare_exchange_strong(expected, true))
    {
        return RecoveryResult{
            false,
            L"A GPU reset is already in progress.",
            ERROR_BUSY
        };
    }

    // RAII guard: always clear the flag when this scope exits
    struct ResetGuard
    {
        ~ResetGuard() { resetInProgress_.store(false); }
    } guard;

    auto notify = [&progressCb](const std::wstring& msg)
    {
        if (progressCb)
            progressCb(msg);
    };

    notify(L"Checking elevation...");

    if (!IsElevated())
    {
        notify(L"Requesting elevation...");
        RelaunchElevated();
        return RecoveryResult{
            false,
            L"Elevation requested - relaunching elevated",
            0
        };
    }

    notify(L"Enumerating display adapters...");

    std::vector<std::wstring> adapters = EnumerateDisplayAdapters();
    if (adapters.empty())
    {
        return RecoveryResult{
            false,
            L"No display adapters found.",
            ERROR_NOT_FOUND
        };
    }

    notify(L"Resetting: " + adapters[0]);

    return ResetDisplayAdapter(adapters[0]);
}

// ---------------------------------------------------------------------------
// IsResetInProgress
// ---------------------------------------------------------------------------
bool DriverRecovery::IsResetInProgress()
{
    return resetInProgress_.load();
}
