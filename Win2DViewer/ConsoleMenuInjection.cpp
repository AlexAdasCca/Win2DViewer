#include "pch.h"

#include <sstream>
#include <winternl.h>
#include <string>
#include <vector>

#include "ConsoleMenuInjection.h"
#include "ConsoleTextWriter.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#define WIN2DVIEWER_TEMP_DISABLE_PROCESS_CONSOLE_HOST_FASTPATH 0

namespace
{
    using NtGetNextProcessFn = NTSTATUS(NTAPI*)(HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
    using NtCreateThreadExFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    constexpr ACCESS_MASK kInjectionAccess =
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    constexpr ACCESS_MASK kEnumerationAccess = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
    // NtGetNextProcess Flags value:
    // PROCESS_GET_NEXT_FLAGS_PREVIOUS_PROCESS = 0x1
    // Reference (community): https://ntdoc.m417z.com/ntgetnextprocess
    constexpr ULONG kProcessGetNextFlagsPreviousProcess = 0x1;

    // NtQueryInformationProcess(ProcessInformationClass=49) for console host lookup.
    // This value is commonly named ProcessConsoleHostProcess in community headers.
    // References:
    // - https://ntdoc.m417z.com/processinfoclass
    // - https://stackoverflow.com/questions/53679384/ntqueryinformationprocess-processconsolehostprocess-returns-wrong-process-id
    constexpr PROCESSINFOCLASS kProcessConsoleHostProcessClass = static_cast<PROCESSINFOCLASS>(49);
    constexpr ULONG_PTR kProcessConsoleHostPidMask = ~static_cast<ULONG_PTR>(3);

    struct UnicodeString32
    {
        USHORT Length = 0;
        USHORT MaximumLength = 0;
        ULONG Buffer = 0;
    };

    struct UnicodeString64
    {
        USHORT Length = 0;
        USHORT MaximumLength = 0;
        ULONG Padding = 0;
        ULONGLONG Buffer = 0;
    };

    struct ProcessBasicInformationData
    {
        PVOID Reserved1 = nullptr;
        PPEB PebBaseAddress = nullptr;
        PVOID Reserved2[2]{};
        ULONG_PTR UniqueProcessId = 0;
        ULONG_PTR InheritedFromUniqueProcessId = 0;
    };

    struct PebPartial32
    {
        BYTE Reserved1[0x10]{};
        ULONG ProcessParameters = 0;
    };

    struct PebPartial64
    {
        BYTE Reserved1[0x20]{};
        ULONGLONG ProcessParameters = 0;
    };

    struct RtlUserProcessParametersPartial32
    {
        BYTE Reserved1[0x38]{};
        UnicodeString32 ImagePathName{};
        UnicodeString32 CommandLine{};
    };

    struct RtlUserProcessParametersPartial64
    {
        BYTE Reserved1[0x60]{};
        UnicodeString64 ImagePathName{};
        UnicodeString64 CommandLine{};
    };

    DWORD gInjectedConsoleHostProcessId = 0;
    bool gDiagnosticsEnabled = false;
    HANDLE gInjectionThread = nullptr;
    std::atomic_bool gInjectionInProgress = false;

    void LogLine(std::wstring const& line)
    {
        if (gDiagnosticsEnabled && ::GetConsoleWindow() != nullptr)
        {
            DiagnosticConsole::WriteLine(line);
            return;
        }

        DiagnosticConsole::WriteLine(line);
    }

    std::wstring FormatNtStatus(NTSTATUS status)
    {
        std::wstringstream ss;
        ss << L"0x" << std::hex << static_cast<unsigned long>(status);
        return ss.str();
    }

    std::wstring GetConsoleHookDllPath()
    {
        wchar_t modulePath[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
        std::wstring path(modulePath);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            path.erase(slash + 1);
        }
#if defined(_WIN64)
        path += L"ConsoleMenuHook64.dll";
#else
        path += L"ConsoleMenuHook32.dll";
#endif
        return path;
    }

    bool WaitForConsoleWindow()
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (::GetConsoleWindow() != nullptr)
            {
                LogLine(L"[ConsoleMenuInjection] Console window detected.");
                return true;
            }
            ::Sleep(25);
        }
        LogLine(L"[ConsoleMenuInjection] Console window wait timed out.");
        return false;
    }

    bool EndsWithInsensitive(std::wstring_view value, std::wstring_view suffix)
    {
        if (suffix.size() > value.size())
        {
            return false;
        }

        const size_t offset = value.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i)
        {
            if (towlower(value[offset + i]) != towlower(suffix[i]))
            {
                return false;
            }
        }
        return true;
    }

    template<typename AddressType>
    bool ReadRemoteUnicodeString(HANDLE processHandle, AddressType remoteAddress, USHORT byteLength, std::wstring& text)
    {
        if (remoteAddress == 0 || byteLength == 0)
        {
            text.clear();
            return true;
        }

        std::vector<wchar_t> buffer((byteLength / sizeof(wchar_t)) + 1, L'\0');
        SIZE_T bytesRead = 0;
        if (!::ReadProcessMemory(
            processHandle,
            reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(remoteAddress)),
            buffer.data(),
            byteLength,
            &bytesRead))
        {
            return false;
        }

        text.assign(buffer.data(), bytesRead / sizeof(wchar_t));
        return true;
    }

    bool ReadProcessPebStrings(
        HANDLE processHandle,
        NtQueryInformationProcessFn ntQueryInformationProcess,
        std::wstring& imagePath,
        std::wstring& commandLine)
    {
        imagePath.clear();
        commandLine.clear();

        ULONG_PTR wow64Peb = 0;
        if (NT_SUCCESS(ntQueryInformationProcess(
            processHandle,
            ProcessWow64Information,
            &wow64Peb,
            sizeof(wow64Peb),
            nullptr)) && wow64Peb != 0)
        {
            PebPartial32 peb32{};
            if (!::ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(wow64Peb), &peb32, sizeof(peb32), nullptr))
            {
                return false;
            }

            RtlUserProcessParametersPartial32 params32{};
            if (!::ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(peb32.ProcessParameters)), &params32, sizeof(params32), nullptr))
            {
                return false;
            }

            if (!ReadRemoteUnicodeString(processHandle, params32.ImagePathName.Buffer, params32.ImagePathName.Length, imagePath))
            {
                return false;
            }
            if (!ReadRemoteUnicodeString(processHandle, params32.CommandLine.Buffer, params32.CommandLine.Length, commandLine))
            {
                return false;
            }
            return true;
        }

        ProcessBasicInformationData info{};
        if (!NT_SUCCESS(ntQueryInformationProcess(processHandle, ProcessBasicInformation, &info, sizeof(info), nullptr)))
        {
            return false;
        }

        PebPartial64 peb64{};
        if (!::ReadProcessMemory(processHandle, info.PebBaseAddress, &peb64, sizeof(peb64), nullptr))
        {
            return false;
        }

        RtlUserProcessParametersPartial64 params64{};
        if (!::ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(peb64.ProcessParameters)), &params64, sizeof(params64), nullptr))
        {
            return false;
        }

        if (!ReadRemoteUnicodeString(processHandle, params64.ImagePathName.Buffer, params64.ImagePathName.Length, imagePath))
        {
            return false;
        }
        if (!ReadRemoteUnicodeString(processHandle, params64.CommandLine.Buffer, params64.CommandLine.Length, commandLine))
        {
            return false;
        }
        return true;
    }

    bool TryQueryBasicProcessInfo(HANDLE processHandle, NtQueryInformationProcessFn ntQueryInformationProcess, ProcessBasicInformationData& info)
    {
        info = {};
        const NTSTATUS queryStatus = ntQueryInformationProcess(processHandle, ProcessBasicInformation, &info, sizeof(info), nullptr);
        if (!NT_SUCCESS(queryStatus))
        {
            LogLine(L"[ConsoleMenuInjection] NtQueryInformationProcess(ProcessBasicInformation) failed: " + FormatNtStatus(queryStatus));
            return false;
        }
        return true;
    }

    bool IsConhostImagePath(HANDLE processHandle, std::wstring* imagePathOut = nullptr)
    {
        wchar_t queryImagePath[MAX_PATH]{};
        DWORD imagePathSize = _countof(queryImagePath);
        if (!::QueryFullProcessImageNameW(processHandle, 0, queryImagePath, &imagePathSize))
        {
            return false;
        }

        if (imagePathOut != nullptr)
        {
            *imagePathOut = queryImagePath;
        }
        return EndsWithInsensitive(queryImagePath, L"\\conhost.exe");
    }

    bool IsCurrentConsoleHostProcess(HANDLE processHandle, NtQueryInformationProcessFn ntQueryInformationProcess)
    {
        ProcessBasicInformationData info{};
        if (!TryQueryBasicProcessInfo(processHandle, ntQueryInformationProcess, info))
        {
            return false;
        }

        if (static_cast<DWORD>(info.InheritedFromUniqueProcessId) != ::GetCurrentProcessId())
        {
            return false;
        }

        std::wstring queryImagePath;
        if (!IsConhostImagePath(processHandle, &queryImagePath))
        {
            return false;
        }

        std::wstring imagePath;
        std::wstring commandLine;
        if (!ReadProcessPebStrings(processHandle, ntQueryInformationProcess, imagePath, commandLine))
        {
            LogLine(L"[ConsoleMenuInjection] Failed to read remote PEB strings.");
        }

        DiagnosticConsole::LineBuilder line;
        line << L"[ConsoleMenuInjection] Candidate pid=" << ::GetProcessId(processHandle)
             << L" parent=" << static_cast<DWORD>(info.InheritedFromUniqueProcessId)
             << L" image=" << (!imagePath.empty() ? imagePath : queryImagePath)
             << L" cmd=" << commandLine;
        LogLine(line.str());

        if (!commandLine.empty())
        {
            const bool commandLineMatches =
                commandLine.find(L"conhost.exe") != std::wstring::npos ||
                commandLine.find(L"Console Window Host") != std::wstring::npos;
            if (!commandLineMatches)
            {
                return false;
            }
        }

        return true;
    }

    HANDLE TryFindConsoleHostProcessViaProcessInfoClass(NtQueryInformationProcessFn ntQueryInformationProcess)
    {
        ULONG_PTR rawConsoleHost = 0;
        // NtQueryInformationProcess parameters:
        // - ProcessHandle: current process
        // - ProcessInformationClass: ProcessConsoleHostProcess(49)
        // - ProcessInformation: receives ULONG_PTR payload (PID + low-bit flags)
        // - ProcessInformationLength: sizeof(ULONG_PTR)
        const NTSTATUS status = ntQueryInformationProcess(
            ::GetCurrentProcess(),
            kProcessConsoleHostProcessClass,
            &rawConsoleHost,
            sizeof(rawConsoleHost),
            nullptr);
        if (!NT_SUCCESS(status) || rawConsoleHost == 0)
        {
            std::wstringstream ss;
            ss << L"[ConsoleMenuInjection] ProcessConsoleHostProcess query failed, status="
               << FormatNtStatus(status)
               << L" raw=0x" << std::hex << rawConsoleHost;
            LogLine(ss.str());
            return nullptr;
        }

        const DWORD hostPid = static_cast<DWORD>(rawConsoleHost & kProcessConsoleHostPidMask);
        std::wstringstream pidLine;
        pidLine << L"[ConsoleMenuInjection] ProcessConsoleHostProcess raw=0x"
                << std::hex << rawConsoleHost
                << L" maskedPid=" << std::dec << hostPid;
        LogLine(pidLine.str());
        if (hostPid == 0)
        {
            return nullptr;
        }

        HANDLE verifyHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, hostPid);
        if (verifyHandle == nullptr)
        {
            return nullptr;
        }

        ProcessBasicInformationData info{};
        const bool hasBasicInfo = TryQueryBasicProcessInfo(verifyHandle, ntQueryInformationProcess, info);
        const bool isChildProcess = hasBasicInfo && static_cast<DWORD>(info.InheritedFromUniqueProcessId) == ::GetCurrentProcessId();
        const bool isConhostImage = IsConhostImagePath(verifyHandle);
        ::CloseHandle(verifyHandle);

        if (!isChildProcess || !isConhostImage)
        {
            LogLine(L"[ConsoleMenuInjection] ProcessConsoleHostProcess candidate rejected by child/image validation.");
            return nullptr;
        }

        HANDLE injectHandle = ::OpenProcess(kInjectionAccess, FALSE, hostPid);
        if (injectHandle == nullptr)
        {
            std::wstringstream ss;
            ss << L"[ConsoleMenuInjection] OpenProcess(injection) failed for hostPid=" << hostPid
               << L" gle=" << ::GetLastError();
            LogLine(ss.str());
            return nullptr;
        }

        return injectHandle;
    }

    HANDLE FindCurrentConsoleHostProcess()
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }

        const auto ntGetNextProcess = reinterpret_cast<NtGetNextProcessFn>(::GetProcAddress(ntdll, "NtGetNextProcess"));
        const auto ntQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess"));
        if (ntGetNextProcess == nullptr || ntQueryInformationProcess == nullptr)
        {
            LogLine(L"[ConsoleMenuInjection] Failed to resolve NtGetNextProcess or NtQueryInformationProcess.");
            return nullptr;
        }

#if !WIN2DVIEWER_TEMP_DISABLE_PROCESS_CONSOLE_HOST_FASTPATH
        if (HANDLE directHostProcess = TryFindConsoleHostProcessViaProcessInfoClass(ntQueryInformationProcess))
        {
            std::wstringstream ss;
            ss << L"[ConsoleMenuInjection] Matched conhost via ProcessConsoleHostProcess, pid="
               << ::GetProcessId(directHostProcess);
            LogLine(ss.str());
            return directHostProcess;
        }
#else
        LogLine(L"[ConsoleMenuInjection] ProcessConsoleHostProcess fast path is temporarily disabled by macro.");
#endif

        HANDLE currentHandle = nullptr;
        while (true)
        {
            HANDLE nextHandle = nullptr;
            // NtGetNextProcess parameters:
            // - ProcessHandle: iterator cursor (NULL for first item)
            // - DesiredAccess: query-only in enumeration phase
            // - HandleAttributes: 0
            // - Flags: PREVIOUS_PROCESS for reverse traversal
            // - NewProcessHandle: receives next cursor
            const NTSTATUS nextStatus = ntGetNextProcess(
                currentHandle,
                kEnumerationAccess,
                0,
                kProcessGetNextFlagsPreviousProcess,
                &nextHandle);

            if (currentHandle != nullptr)
            {
                ::CloseHandle(currentHandle);
                currentHandle = nullptr;
            }

            if (!NT_SUCCESS(nextStatus))
            {
                break;
            }

            currentHandle = nextHandle;
            if (IsCurrentConsoleHostProcess(currentHandle, ntQueryInformationProcess))
            {
                const DWORD matchedPid = ::GetProcessId(currentHandle);
                HANDLE injectHandle = ::OpenProcess(kInjectionAccess, FALSE, matchedPid);
                if (injectHandle != nullptr)
                {
                    std::wstringstream ss;
                    ss << L"[ConsoleMenuInjection] Matched conhost via fallback enumeration pid=" << matchedPid;
                    LogLine(ss.str());
                    ::CloseHandle(currentHandle);
                    return injectHandle;
                }
            }
        }

        if (currentHandle != nullptr)
        {
            ::CloseHandle(currentHandle);
        }

        LogLine(L"[ConsoleMenuInjection] No matching conhost process was found.");
        return nullptr;
    }

    bool InjectDllIntoProcess(HANDLE processHandle, std::wstring const& dllPath)
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return false;
        }

        const auto ntCreateThreadEx = reinterpret_cast<NtCreateThreadExFn>(::GetProcAddress(ntdll, "NtCreateThreadEx"));
        if (ntCreateThreadEx == nullptr)
        {
            LogLine(L"[ConsoleMenuInjection] Failed to resolve NtCreateThreadEx.");
            return false;
        }

        const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remoteBuffer = ::VirtualAllocEx(processHandle, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remoteBuffer == nullptr)
        {
            std::wstringstream ss;
            ss << L"[ConsoleMenuInjection] VirtualAllocEx failed. gle=" << ::GetLastError();
            LogLine(ss.str());
            return false;
        }

        bool success = false;
        HANDLE remoteThread = nullptr;
        do
        {
            if (!::WriteProcessMemory(processHandle, remoteBuffer, dllPath.c_str(), bytes, nullptr))
            {
                std::wstringstream ss;
                ss << L"[ConsoleMenuInjection] WriteProcessMemory failed. gle=" << ::GetLastError();
                LogLine(ss.str());
                break;
            }

            const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
            if (kernel32 == nullptr)
            {
                LogLine(L"[ConsoleMenuInjection] GetModuleHandleW(kernel32.dll) failed.");
                break;
            }

            FARPROC loadLibraryW = ::GetProcAddress(kernel32, "LoadLibraryW");
            if (loadLibraryW == nullptr)
            {
                LogLine(L"[ConsoleMenuInjection] GetProcAddress(LoadLibraryW) failed.");
                break;
            }

            const NTSTATUS createStatus = ntCreateThreadEx(
                &remoteThread,
                THREAD_ALL_ACCESS,
                nullptr,
                processHandle,
                reinterpret_cast<PTHREAD_START_ROUTINE>(loadLibraryW),
                remoteBuffer,
                0,
                0,
                0,
                0,
                nullptr);
            if (!NT_SUCCESS(createStatus))
            {
                LogLine(L"[ConsoleMenuInjection] NtCreateThreadEx failed: " + FormatNtStatus(createStatus));
                break;
            }

            if (::WaitForSingleObject(remoteThread, 5000) != WAIT_OBJECT_0)
            {
                LogLine(L"[ConsoleMenuInjection] Waiting for remote LoadLibraryW timed out.");
                break;
            }

            DWORD exitCode = 0;
            if (!::GetExitCodeThread(remoteThread, &exitCode) || exitCode == 0)
            {
                std::wstringstream ss;
                ss << L"[ConsoleMenuInjection] Remote LoadLibraryW exitCode=" << exitCode
                   << L" gle=" << ::GetLastError();
                LogLine(ss.str());
                break;
            }

            std::wstringstream ss;
            ss << L"[ConsoleMenuInjection] Remote LoadLibraryW succeeded. module=0x"
               << std::hex << exitCode;
            LogLine(ss.str());
            success = true;
        } while (false);

        if (remoteThread != nullptr)
        {
            ::CloseHandle(remoteThread);
        }
        ::VirtualFreeEx(processHandle, remoteBuffer, 0, MEM_RELEASE);
        return success;
    }
}

bool ConsoleMenu::EnsureConsoleHookInjected()
{
    const std::wstring dllPath = GetConsoleHookDllPath();
    if (::GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LogLine(L"[ConsoleMenuInjection] Hook dll was not found: " + dllPath);
        return false;
    }

    if (!WaitForConsoleWindow())
    {
        return false;
    }

    for (int attempt = 0; attempt < 120; ++attempt)
    {
        HANDLE processHandle = FindCurrentConsoleHostProcess();
        if (processHandle != nullptr)
        {
            const DWORD processId = ::GetProcessId(processHandle);
            if (gInjectedConsoleHostProcessId == processId)
            {
                LogLine(L"[ConsoleMenuInjection] Hook already injected into current conhost.");
                ::CloseHandle(processHandle);
                return true;
            }

            const bool injected = InjectDllIntoProcess(processHandle, dllPath);
            ::CloseHandle(processHandle);
            if (injected)
            {
                gInjectedConsoleHostProcessId = processId;
                return true;
            }
        }

        ::Sleep(50);
    }

    LogLine(L"[ConsoleMenuInjection] Hook injection failed after retries.");
    return false;
}

namespace
{
    DWORD WINAPI ConsoleHookInjectionThreadProc(LPVOID)
    {
        const bool injected = ConsoleMenu::EnsureConsoleHookInjected();
        if (!injected)
        {
            LogLine(L"[ConsoleMenuInjection] Async injection thread finished without success.");
        }
        gInjectionInProgress.store(false);
        return injected ? 0 : 1;
    }
}

void ConsoleMenu::BeginConsoleHookInjectionAsync()
{
    bool expected = false;
    if (!gInjectionInProgress.compare_exchange_strong(expected, true))
    {
        return;
    }

    if (gInjectionThread != nullptr)
    {
        ::CloseHandle(gInjectionThread);
        gInjectionThread = nullptr;
    }

    gInjectionThread = ::CreateThread(nullptr, 0, &ConsoleHookInjectionThreadProc, nullptr, 0, nullptr);
    if (gInjectionThread == nullptr)
    {
        gInjectionInProgress.store(false);
        std::wstringstream ss;
        ss << L"[ConsoleMenuInjection] Failed to create async injection thread. gle=" << ::GetLastError();
        LogLine(ss.str());
    }
}

void ConsoleMenu::SetInjectionDiagnosticsEnabled(bool enabled) noexcept
{
    gDiagnosticsEnabled = enabled;
}

void ConsoleMenu::ResetConsoleHookState() noexcept
{
    gInjectedConsoleHostProcessId = 0;
    if (gInjectionThread != nullptr)
    {
        ::CloseHandle(gInjectionThread);
        gInjectionThread = nullptr;
    }
    gInjectionInProgress.store(false);
}
