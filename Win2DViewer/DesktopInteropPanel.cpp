#include "pch.h"

#include "DesktopCompositionInteropInternal.h"

namespace DesktopInteropInternal
{
    class DesktopHostTestPanelWindow
    {
      public:
        bool Create(HWND ownerWindow)
        {
            WNDCLASSW windowClass{};
            windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.lpszClassName = kDesktopHostPanelClassName;
            windowClass.lpfnWndProc = &DesktopHostTestPanelWindow::WndProc;
            windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            windowClass.style = CS_HREDRAW | CS_VREDRAW;
            ::RegisterClassW(&windowClass);

            const DWORD exStyle = WS_EX_APPWINDOW;
            const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                WS_MINIMIZEBOX | WS_THICKFRAME;
            windowHandle = ::CreateWindowExW(
                exStyle, kDesktopHostPanelClassName, L"Desktop Host Test Panel", style,
                CW_USEDEFAULT, CW_USEDEFAULT, 560, 230, ownerWindow, nullptr,
                ::GetModuleHandleW(nullptr), this);

            if (windowHandle == nullptr)
            {
                return false;
            }

            return true;
        }

        bool IsWindow() const noexcept
        {
            return windowHandle != nullptr && ::IsWindow(windowHandle);
        }

        void ShowAndActivate() const
        {
            if (!IsWindow())
            {
                return;
            }

            ::ShowWindow(windowHandle, SW_SHOWNORMAL);
            ::SetForegroundWindow(windowHandle);
        }

        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
                case WM_CREATE:
                    return OnCreate();
                case WM_SIZE:
                    LayoutControls();
                    return 0;
                case WM_COMMAND:
                    return OnCommand(LOWORD(wParam));
                case WM_CLOSE:
                    ::ShowWindow(windowHandle, SW_HIDE);
                    return 0;
                default:
                    return ::DefWindowProcW(windowHandle, message, wParam, lParam);
            }
        }

        static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam)
        {
            DesktopHostTestPanelWindow* that = nullptr;
            if (message == WM_NCCREATE)
            {
                auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
                that = static_cast<DesktopHostTestPanelWindow*>(
                    createStruct->lpCreateParams);
                ::SetWindowLongPtrW(window, GWLP_USERDATA,
                                    reinterpret_cast<LONG_PTR>(that));
                that->windowHandle = window;
            }
            else
            {
                that = reinterpret_cast<DesktopHostTestPanelWindow*>(
                    ::GetWindowLongPtrW(window, GWLP_USERDATA));
            }

            if (that == nullptr)
            {
                return ::DefWindowProcW(window, message, wParam, lParam);
            }

            return that->HandleMessage(message, wParam, lParam);
        }

      private:
        LRESULT OnCreate()
        {
            instructionLabel = ::CreateWindowExW(
                0, WC_STATICW,
                L"Create isolated Win32 host windows for interop validation. Host "
                L"windows use WS_EX_NOREDIRECTIONBITMAP.",
                WS_CHILD | WS_VISIBLE, 12, 12, 520, 36, windowHandle, nullptr,
                ::GetModuleHandleW(nullptr), nullptr);

            winRtButton = ::CreateWindowExW(
                0, WC_BUTTONW, L"Create WinRT Physics Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12, 60, 250, 32, windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonWinRt)),
                ::GetModuleHandleW(nullptr), nullptr);

            winRtBackdropButton = ::CreateWindowExW(
                0, WC_BUTTONW, L"Create WinRT Host Backdrop Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 272, 60, 250, 32, windowHandle,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kPanelButtonWinRtBackdrop)),
                ::GetModuleHandleW(nullptr), nullptr);

            dcompButton = ::CreateWindowExW(
                0, WC_BUTTONW, L"Create DirectComposition Flow Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12, 102, 250, 32, windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonDComp)),
                ::GetModuleHandleW(nullptr), nullptr);

            bothButton = ::CreateWindowExW(
                0, WC_BUTTONW, L"Create All Hosts",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 272, 102, 250, 32, windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonBoth)),
                ::GetModuleHandleW(nullptr), nullptr);

            HFONT messageFont =
                reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            if (messageFont != nullptr)
            {
                ::SendMessageW(instructionLabel, WM_SETFONT,
                               reinterpret_cast<WPARAM>(messageFont), TRUE);
                ::SendMessageW(winRtButton, WM_SETFONT,
                               reinterpret_cast<WPARAM>(messageFont), TRUE);
                ::SendMessageW(winRtBackdropButton, WM_SETFONT,
                               reinterpret_cast<WPARAM>(messageFont), TRUE);
                ::SendMessageW(dcompButton, WM_SETFONT,
                               reinterpret_cast<WPARAM>(messageFont), TRUE);
                ::SendMessageW(bothButton, WM_SETFONT,
                               reinterpret_cast<WPARAM>(messageFont), TRUE);
            }

            return 0;
        }

        LRESULT OnCommand(UINT commandId)
        {
            switch (commandId)
            {
                case kPanelButtonWinRt:
                    CreateHostFromPanel(DesktopInterop::DesktopHostBackend::WinRTComposition);
                    return 0;
                case kPanelButtonWinRtBackdrop:
                    CreateHostFromPanel(
                        DesktopInterop::DesktopHostBackend::WinRTHostBackdrop);
                    return 0;
                case kPanelButtonDComp:
                    CreateHostFromPanel(
                        DesktopInterop::DesktopHostBackend::DirectComposition);
                    return 0;
                case kPanelButtonBoth:
                    CreateHostFromPanel(DesktopInterop::DesktopHostBackend::WinRTComposition);
                    CreateHostFromPanel(
                        DesktopInterop::DesktopHostBackend::WinRTHostBackdrop);
                    CreateHostFromPanel(
                        DesktopInterop::DesktopHostBackend::DirectComposition);
                    return 0;
                default:
                    return 0;
            }
        }

        void LayoutControls() const
        {
            RECT clientRect{};
            ::GetClientRect(windowHandle, &clientRect);

            const int width = clientRect.right - clientRect.left;
            const int buttonWidth = (std::max)(160, (width - 38) / 2);
            const int rightButtonX = 18 + buttonWidth;
            const int rightButtonWidth = width - rightButtonX - 18;

            if (instructionLabel != nullptr)
            {
                ::MoveWindow(instructionLabel, 12, 12, width - 24, 36, TRUE);
            }
            if (winRtButton != nullptr)
            {
                ::MoveWindow(winRtButton, 12, 60, buttonWidth, 32, TRUE);
            }
            if (winRtBackdropButton != nullptr)
            {
                ::MoveWindow(winRtBackdropButton, rightButtonX, 60, rightButtonWidth, 32,
                             TRUE);
            }
            if (dcompButton != nullptr)
            {
                ::MoveWindow(dcompButton, 12, 102, buttonWidth, 32, TRUE);
            }
            if (bothButton != nullptr)
            {
                ::MoveWindow(bothButton, rightButtonX, 102, rightButtonWidth, 32, TRUE);
            }
        }

        void CreateHostFromPanel(DesktopInterop::DesktopHostBackend backend) const
        {
            std::wstring errorMessage;
            if (!DesktopInterop::CreateDesktopHostTestWindow(backend, &errorMessage))
            {
                const std::wstring message =
                    L"Failed to create desktop host window.\n" + errorMessage;
                ::MessageBoxW(windowHandle, message.c_str(), L"Desktop Host Test",
                              MB_OK | MB_ICONERROR);
            }
        }

      private:
        HWND windowHandle = nullptr;
        HWND instructionLabel = nullptr;
        HWND winRtButton = nullptr;
        HWND winRtBackdropButton = nullptr;
        HWND dcompButton = nullptr;
        HWND bothButton = nullptr;
    };

    std::unique_ptr<DesktopHostTestPanelWindow> gDesktopHostTestPanel;
} // namespace DesktopInteropInternal

void DesktopInterop::ShowDesktopHostTestPanel(HWND ownerWindow)
{
    if (DesktopInteropInternal::gDesktopHostTestPanel != nullptr &&
        DesktopInteropInternal::gDesktopHostTestPanel->IsWindow())
    {
        DesktopInteropInternal::gDesktopHostTestPanel->ShowAndActivate();
        return;
    }

    auto panel =
        std::make_unique<DesktopInteropInternal::DesktopHostTestPanelWindow>();
    if (!panel->Create(ownerWindow))
    {
        const std::wstring error = DesktopInteropInternal::FormatLastErrorMessage(
            L"Create desktop host test panel");
        ::MessageBoxW(ownerWindow, error.c_str(), L"Desktop Host Test",
                      MB_OK | MB_ICONERROR);
        return;
    }

    panel->ShowAndActivate();
    DesktopInteropInternal::gDesktopHostTestPanel = std::move(panel);
}
