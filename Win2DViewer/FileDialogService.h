#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace FileDialogService
{
    std::optional<std::wstring> BrowseForSvgFile(HWND owner);
}
