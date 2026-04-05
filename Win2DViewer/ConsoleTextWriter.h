#pragma once

#include <windows.h>
#include <wil/resource.h>

#include <sstream>
#include <string>
#include <string_view>

namespace DiagnosticConsole
{
    class LineBuilder
    {
    public:
        template<typename T>
        LineBuilder& operator<<(T const& value)
        {
            stream_ << value;
            return *this;
        }

        std::wstring str() const { return stream_.str(); }

    private:
        std::wostringstream stream_;
    };

    inline void ConfigureUnicodeConsole()
    {
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);
    }

    inline void WriteLine(std::wstring_view line, bool mirrorToDebugger = true)
    {
        if (mirrorToDebugger)
        {
            std::wstring debugLine(line);
            debugLine.push_back(L'\n');
            ::OutputDebugStringW(debugLine.c_str());
        }

        HANDLE consoleHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        wil::unique_hfile fallbackConsoleHandle;
        if (consoleHandle == nullptr || consoleHandle == INVALID_HANDLE_VALUE)
        {
            fallbackConsoleHandle.reset(
                ::CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            if (!fallbackConsoleHandle)
            {
                return;
            }
            consoleHandle = fallbackConsoleHandle.get();
        }

        DWORD mode = 0;
        if (!::GetConsoleMode(consoleHandle, &mode))
        {
            return;
        }

        DWORD written = 0;
        (void)::WriteConsoleW(consoleHandle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        (void)::WriteConsoleW(consoleHandle, L"\r\n", 2, &written, nullptr);
    }

    inline void WriteLine(LineBuilder const& builder, bool mirrorToDebugger = true)
    {
        WriteLine(builder.str(), mirrorToDebugger);
    }
} // namespace DiagnosticConsole
