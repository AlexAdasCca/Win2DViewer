#include "pch.h"

#include "SvgDocument.h"

namespace
{
    std::wstring FormatWin32ErrorMessage(DWORD errorCode)
    {
        LPWSTR buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD size = ::FormatMessageW(
            flags,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        std::wstring message;
        if (size != 0 && buffer != nullptr)
        {
            message.assign(buffer, size);
            ::LocalFree(buffer);
        }
        else
        {
            message = L"Unknown error.";
        }

        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
        {
            message.pop_back();
        }

        return message;
    }
}

bool CSvgDocument::LoadFromFile(std::wstring_view path, std::wstring* errorMessage)
{
    Clear();

    wil::unique_hfile file{
        ::CreateFileW(
            path.data(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr)
    };

    if (!file)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Failed to open file:\n";
            *errorMessage += FormatWin32ErrorMessage(::GetLastError());
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Failed to query file size:\n";
            *errorMessage += FormatWin32ErrorMessage(::GetLastError());
        }
        return false;
    }

    if (size.QuadPart < 0 || size.QuadPart > MAXDWORD)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"File is too large to load into memory.";
        }
        return false;
    }

    const auto byteCount = static_cast<size_t>(size.QuadPart);
    svgXmlBytes.assign(byteCount + 2, '\0');

    DWORD bytesRead = 0;
    if (byteCount != 0 &&
        !::ReadFile(file.get(), svgXmlBytes.data(), static_cast<DWORD>(byteCount), &bytesRead, nullptr))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Failed to read file:\n";
            *errorMessage += FormatWin32ErrorMessage(::GetLastError());
        }
        Clear();
        return false;
    }

    if (bytesRead != byteCount)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Failed to read the complete file.";
        }
        Clear();
        return false;
    }

    documentPath.assign(path);
    return true;
}

void CSvgDocument::Clear() noexcept
{
    svgXmlBytes.clear();
    documentPath.clear();
}

