#include "pch.h"

#include "HookState.h"

#include <string_view>

#include <winnt.h>
#include <winternl.h>

#include "..\Win2DViewer\ConsoleHookIpc.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace ConsoleMenuHook
{
    namespace
    {
        using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

        struct ProcessBasicInformationData
        {
            PVOID Reserved1 = nullptr;
            PPEB PebBaseAddress = nullptr;
            PVOID Reserved2[2]{};
            ULONG_PTR UniqueProcessId = 0;
            ULONG_PTR InheritedFromUniqueProcessId = 0;
        };

        RuntimeState gRuntimeState{};
    }

    RuntimeState& GetRuntimeState()
    {
        return gRuntimeState;
    }

    void LogLine(std::wstring const& line)
    {
        diagnosticconsole::ConfigureUnicodeConsole();
        diagnosticconsole::WriteLine(line);
    }

    DWORD QueryParentProcessId()
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return 0;
        }

        const auto ntQueryInformationProcess =
            reinterpret_cast<NtQueryInformationProcessFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess"));
        if (ntQueryInformationProcess == nullptr)
        {
            return 0;
        }

        // NtQueryInformationProcess(ProcessBasicInformation) returns the caller's
        // PROCESS_BASIC_INFORMATION, including InheritedFromUniqueProcessId.
        //
        // API reference (official):
        // https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntqueryinformationprocess
        //
        // Structure layout reference (community):
        // https://ntdoc.m417z.com/process_basic_information
        ProcessBasicInformationData info{};
        const NTSTATUS status = ntQueryInformationProcess(
            ::GetCurrentProcess(),
            ProcessBasicInformation,
            &info,
            static_cast<ULONG>(sizeof(info)),
            nullptr);
        if (!NT_SUCCESS(status))
        {
            return 0;
        }

        return static_cast<DWORD>(info.InheritedFromUniqueProcessId);
    }

    void NotifyOwnerConsoleClose(std::wstring const& source)
    {
        bool expected = false;
        if (!gRuntimeState.ConsoleCloseNotified.compare_exchange_strong(expected, true))
        {
            return;
        }

        const DWORD ownerPid = QueryParentProcessId();
        if (ownerPid == 0)
        {
            LogLine(L"[ConsoleMenuHook] Console close notify failed: parent pid query failed.");
            return;
        }

        const std::wstring eventName = consolehookipc::BuildConsoleCloseNotifyEventName(ownerPid);
        HANDLE notifyEvent = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
        if (notifyEvent == nullptr)
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Console close notify open failed. source="
                 << source
                 << L" ownerPid=" << ownerPid
                 << L" gle=" << ::GetLastError();
            LogLine(line.str());
            return;
        }

        if (!::SetEvent(notifyEvent))
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Console close notify SetEvent failed. source="
                 << source
                 << L" ownerPid=" << ownerPid
                 << L" gle=" << ::GetLastError();
            LogLine(line.str());
            ::CloseHandle(notifyEvent);
            return;
        }

        ::CloseHandle(notifyEvent);
        diagnosticconsole::LineBuilder line;
        line << L"[ConsoleMenuHook] Console close notify sent. source="
             << source
             << L" ownerPid=" << ownerPid;
        LogLine(line.str());
    }

    void CaptureConsoleWindow(HWND windowHandle)
    {
        if (windowHandle == nullptr || !::IsWindow(windowHandle))
        {
            return;
        }

        wchar_t className[64]{};
        if (::GetClassNameW(windowHandle, className, _countof(className)) == 0)
        {
            return;
        }

        if (std::wstring_view(className) != L"ConsoleWindowClass")
        {
            return;
        }

        HWND expected = nullptr;
        if (gRuntimeState.ConsoleWindow.compare_exchange_strong(expected, windowHandle))
        {
            gRuntimeState.ConsoleCloseNotified.store(false);
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Captured console window hwnd=0x"
                 << std::hex << reinterpret_cast<ULONG_PTR>(windowHandle);
            LogLine(line.str());
        }
    }

    void ResetWindowIntegrationStateAfterDestroy()
    {
        gRuntimeState.ConsoleWindowOriginalWndProc.store(nullptr);
        gRuntimeState.ConsoleWindow.store(nullptr);
        gRuntimeState.ConsoleMenuInstalled.store(false);
        gRuntimeState.ConsoleWindowSubclassed.store(false);
        gRuntimeState.ConsoleIntegrateInProgress.store(false);
        gRuntimeState.ConsoleCloseNotified.store(false);
    }
}
