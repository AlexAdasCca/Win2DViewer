#include "pch.h"

#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wil/com.h>

#include "FileDialogService.h"

namespace
{
    constexpr COMDLG_FILTERSPEC kSvgFileFilters[] = { { L"SVG Files (*.svg)", L"*.svg" },
                                                      { L"All Files (*.*)", L"*.*" } };

    // Use a dedicated dialog client state bucket for this application.
    constexpr GUID kSvgDialogClientGuid = { 0x95f8e8f1,
                                            0x4f26,
                                            0x4ad7,
                                            { 0xa6, 0xa6, 0x0f, 0x57, 0x3d, 0x88, 0x65, 0x12 } };
}

namespace FileDialogService
{
    std::optional<std::wstring> BrowseForSvgFile(HWND owner)
    {
        // Shell dialogs can emit many first chance exceptions and internal
        // diagnostic logs in the debugger. These are mostly shell probe noise.
        // End users do not see these messages. Application side mitigation can
        // only reduce frequency, not guarantee full elimination.
        auto fileDialog = wil::CoCreateInstanceNoThrow<IFileOpenDialog>(CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER);
        if (!fileDialog)
        {
            return std::nullopt;
        }

        DWORD options = 0;
        if (FAILED(fileDialog->GetOptions(&options)))
        {
            return std::nullopt;
        }

        options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
                   FOS_STRICTFILETYPES;
        if (FAILED(fileDialog->SetOptions(options)))
        {
            return std::nullopt;
        }

        (void)fileDialog->SetClientGuid(kSvgDialogClientGuid);
        (void)fileDialog->ClearClientData();

        if (FAILED(fileDialog->SetFileTypes(static_cast<UINT>(std::size(kSvgFileFilters)), kSvgFileFilters)))
        {
            return std::nullopt;
        }

        (void)fileDialog->SetFileTypeIndex(1);
        (void)fileDialog->SetDefaultExtension(L"svg");

        wil::unique_cotaskmem_string defaultFolderPath;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, defaultFolderPath.put())))
        {
            wil::com_ptr_nothrow<IShellItem> defaultFolder;
            if (SUCCEEDED(::SHCreateItemFromParsingName(
                    defaultFolderPath.get(), nullptr, __uuidof(IShellItem), defaultFolder.put_void())))
            {
                // Force a local filesystem start point for each open operation.
                (void)fileDialog->SetFolder(defaultFolder.get());
                (void)fileDialog->SetDefaultFolder(defaultFolder.get());
            }
        }

        const HRESULT showResult = fileDialog->Show(owner);
        if (showResult == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return std::nullopt;
        }
        if (FAILED(showResult))
        {
            return std::nullopt;
        }

        wil::com_ptr_nothrow<IShellItem> selectedItem;
        if (FAILED(fileDialog->GetResult(selectedItem.put())))
        {
            return std::nullopt;
        }

        wil::unique_cotaskmem_string filePath;
        if (FAILED(selectedItem->GetDisplayName(SIGDN_FILESYSPATH, filePath.put())))
        {
            return std::nullopt;
        }

        return std::wstring(filePath.get());
    }
}
