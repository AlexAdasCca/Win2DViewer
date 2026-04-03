#include "pch.h"

#include <windows.ui.composition.interop.h>
#include <ShellScalingAPI.h>

#include "DesktopCompositionInterop.h"
#include "Win2DViewer.h"
#include "MainFrm.h"
#include "WinrtNsAliases.h"

CAppModule appModule;

namespace ns
{
    namespace wr = wna::rt;
    namespace wus = wna::wd::sys;
}

ns::wus::DispatcherQueueController CreateDispatcherQueueController()
{
    return desktopinterop::CreateDispatcherQueueControllerForCurrentThread();
}

std::wstring LoadAppString(UINT stringId)
{
    CStringW value;
    if (!value.LoadString(stringId))
    {
        return L"Win2DViewer";
    }

    return std::wstring{ value.GetString() };
}

namespace
{
    std::optional<std::wstring> GetInitialFilePath()
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (argv == nullptr)
        {
            return std::nullopt;
        }

        std::optional<std::wstring> result;
        if (argc > 1 && argv[1] != nullptr && argv[1][0] != L'\0')
        {
            result = argv[1];
        }

        ::LocalFree(argv);
        return result;
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    try
    {
        ns::wr::init_apartment(ns::wr::apartment_type::single_threaded);
        auto dispatcherController = CreateDispatcherQueueController();
        AtlInitCommonControls(ICC_WIN95_CLASSES);

        HRESULT hr = appModule.Init(nullptr, hInstance);
        if (FAILED(hr))
        {
            return hr;
        }

        int exitCode = 0;
        {
            CMessageLoop messageLoop;
            appModule.AddMessageLoop(&messageLoop);

            CMainFrame mainFrame;
            HWND frameHwnd = mainFrame.CreateEx(
                nullptr,
                CWindow::rcDefault,
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP);
            if (frameHwnd == nullptr)
            {
                appModule.RemoveMessageLoop();
                appModule.Term();
                const DWORD lastError = ::GetLastError();
                wchar_t buffer[256]{};
                _snwprintf_s(buffer, _TRUNCATE, L"Main frame CreateEx failed. GetLastError=%lu", lastError);
                ::MessageBoxW(nullptr, buffer, L"Win2DViewer", MB_ICONERROR | MB_OK);
                return 0;
            }

            mainFrame.ShowWindow(nCmdShow);
            mainFrame.UpdateWindow();

            if (auto initialFile = GetInitialFilePath(); initialFile.has_value())
            {
                mainFrame.OpenDocument(*initialFile);
            }

            exitCode = messageLoop.Run();
            appModule.RemoveMessageLoop();
        }

        appModule.Term();
        return exitCode;
    }
    catch (ns::wr::hresult_error const& ex)
    {
        std::wstring message = L"Application startup failed:\n";
        message += ex.message();
        ::MessageBoxW(nullptr, message.c_str(), L"Win2DViewer", MB_ICONERROR | MB_OK);
        return static_cast<int>(ex.code().value);
    }
}

