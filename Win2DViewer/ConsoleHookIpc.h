#pragma once

#include <string>

namespace consolehookipc
{
    inline std::wstring BuildConsoleCloseNotifyEventName(DWORD ownerProcessId)
    {
        wchar_t name[96]{};
        swprintf_s(name, L"Local\\Win2DViewer.ConsoleCloseNotify.%lu", static_cast<unsigned long>(ownerProcessId));
        return name;
    }
}
