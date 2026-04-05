#include "pch.h"

#include "DesktopCompositionInteropInternal.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")

namespace DesktopInteropInternal
{
    std::optional<DesktopInteropNs::wus::DispatcherQueueController> gDispatcherQueueController;
    std::unordered_map<HWND, std::unique_ptr<DesktopHostWindow>> gDesktopHostWindows;

    std::wstring FormatLastErrorMessage(std::wstring_view context)
    {
        const DWORD errorCode = ::GetLastError();
        wil::unique_hlocal_string systemMessage;
        ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr,
                         errorCode,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         reinterpret_cast<LPWSTR>(systemMessage.addressof()),
                         0,
                         nullptr);

        std::wstring result{ context };
        result += L" failed. GetLastError=" + std::to_wstring(errorCode);
        if (systemMessage)
        {
            result += L" (";
            result += systemMessage.get();
            result += L')';
        }

        return result;
    }

    void SetErrorMessage(std::wstring* errorMessage, std::wstring message)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::move(message);
        }
    }

    float Clamp01(float value)
    {
        return (std::max)(0.0f, (std::min)(1.0f, value));
    }

    float Lerp(float a, float b, float t)
    {
        return a + ((b - a) * t);
    }

    uint8_t ToByte(float value)
    {
        return static_cast<uint8_t>(Clamp01(value) * 255.0f + 0.5f);
    }

    uint32_t PackBgra(uint8_t b, uint8_t g, uint8_t r)
    {
        return (static_cast<uint32_t>(0xFF) << 24) | (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }

    DesktopHostWindow::DesktopHostWindow(DesktopInterop::DesktopHostBackend backend) : backend(backend) {}

    bool DesktopHostWindow::CreateWindowInstance(std::wstring* errorMessage)
    {
        WNDCLASSW windowClass{};
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kDesktopHostWindowClassName;
        windowClass.lpfnWndProc = &DesktopHostWindow::WndProc;
        windowClass.hbrBackground = nullptr;
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        ::RegisterClassW(&windowClass);

        const wchar_t* windowTitle = L"Desktop Host - WinRT Composition Surface";
        if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
        {
            windowTitle = L"Desktop Host - DirectComposition Flow Material";
        }
        else if (backend == DesktopInterop::DesktopHostBackend::WinRTHostBackdrop)
        {
            windowTitle = L"Desktop Host - WinRT Host Backdrop Material";
        }

        const DWORD exStyle = WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP;
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        windowHandle = ::CreateWindowExW(exStyle,
                                         kDesktopHostWindowClassName,
                                         windowTitle,
                                         style,
                                         CW_USEDEFAULT,
                                         CW_USEDEFAULT,
                                         960,
                                         640,
                                         nullptr,
                                         nullptr,
                                         ::GetModuleHandleW(nullptr),
                                         this);

        if (windowHandle == nullptr)
        {
            SetErrorMessage(errorMessage, FormatLastErrorMessage(L"CreateWindowExW"));
            return false;
        }

        return true;
    }

    bool DesktopHostWindow::Initialize(std::wstring* errorMessage)
    {
        try
        {
            if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
            {
                InitializeDirectComposition();
            }
            else
            {
                InitializeWinRTComposition();
            }

            rendererInitialized = true;
            const UINT timerInterval = 16;
            ::SetTimer(windowHandle, kRenderTimerId, timerInterval, nullptr);
            ::ShowWindow(windowHandle, SW_SHOWNORMAL);
            ::UpdateWindow(windowHandle);
            return true;
        }
        catch (DesktopInteropNs::wr::hresult_error const& ex)
        {
            SetErrorMessage(errorMessage, std::wstring{ ex.message().c_str() });
            return false;
        }
    }

    HWND DesktopHostWindow::GetWindowHandle() const noexcept
    {
        return windowHandle;
    }

    LRESULT DesktopHostWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                if (rendererInitialized)
                {
                    HandleResize();
                }
                return 0;
            case WM_TIMER:
                if (wParam == kRenderTimerId && rendererInitialized)
                {
                    RenderFrame();
                }
                return 0;
            case WM_MOUSEMOVE:
                if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
                {
                    HandleDirectCompositionMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                    return 0;
                }
                break;
            case WM_MOUSELEAVE:
                if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
                {
                    HandleDirectCompositionMouseLeave();
                    return 0;
                }
                break;
            case WM_LBUTTONDOWN:
                if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
                {
                    HandleDirectCompositionLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                    return 0;
                }
                break;
            case WM_LBUTTONUP:
                if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
                {
                    HandleDirectCompositionLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                    return 0;
                }
                break;
            case WM_PAINT:
            {
                PAINTSTRUCT paintStruct{};
                ::BeginPaint(windowHandle, &paintStruct);
                ::EndPaint(windowHandle, &paintStruct);
                return 0;
            }
            case WM_DESTROY:
                if (dcompOverlayDragging && ::GetCapture() == windowHandle)
                {
                    ::ReleaseCapture();
                }
                ::KillTimer(windowHandle, kRenderTimerId);
                return 0;
            default:
                return ::DefWindowProcW(windowHandle, message, wParam, lParam);
        }

        return ::DefWindowProcW(windowHandle, message, wParam, lParam);
    }

    LRESULT CALLBACK DesktopHostWindow::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        DesktopHostWindow* that = nullptr;
        if (message == WM_NCCREATE)
        {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            that = static_cast<DesktopHostWindow*>(createStruct->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
            that->windowHandle = window;
        }
        else
        {
            that = reinterpret_cast<DesktopHostWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        LRESULT result = (that != nullptr) ? that->HandleMessage(message, wParam, lParam) :
                                             ::DefWindowProcW(window, message, wParam, lParam);

        if (message == WM_NCDESTROY && that != nullptr)
        {
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            gDesktopHostWindows.erase(window);
        }

        return result;
    }

    void DesktopHostWindow::HandleResize()
    {
        if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
        {
            ResizeDirectCompositionSwapChain();
            RenderDirectCompositionFrame();
        }
        else
        {
            ResizeWinRTSurface();
        }
    }

    void DesktopHostWindow::RenderFrame()
    {
        const auto now = std::chrono::steady_clock::now();
        float deltaSeconds = 1.0f / 60.0f;
        if (hasLastFrameTime)
        {
            deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
            deltaSeconds = (std::max)(0.001f, (std::min)(0.050f, deltaSeconds));
        }
        lastFrameTime = now;
        hasLastFrameTime = true;

        animationPhase += deltaSeconds;

        if (backend == DesktopInterop::DesktopHostBackend::DirectComposition)
        {
            RenderDirectCompositionFrame();
        }
        else
        {
            UpdateWinRTPhysics(deltaSeconds);
            DrawWinRTSurface();
        }
    }
} // namespace DesktopInteropInternal

DesktopInteropNs::wus::DispatcherQueueController DesktopInterop::CreateDispatcherQueueControllerForCurrentThread()
{
    namespace abi = ABI::Windows::System;

    DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };

    DesktopInteropNs::wus::DispatcherQueueController controller{ nullptr };
    DesktopInteropNs::wr::check_hresult(::CreateDispatcherQueueController(
        options, reinterpret_cast<abi::IDispatcherQueueController**>(DesktopInteropNs::wr::put_abi(controller))));
    return controller;
}

DesktopInteropNs::wus::DispatcherQueueController DesktopInterop::EnsureDispatcherQueueControllerForCurrentThread()
{
    if (DesktopInteropNs::wus::DispatcherQueue::GetForCurrentThread() != nullptr)
    {
        if (!DesktopInteropInternal::gDispatcherQueueController.has_value())
        {
            return DesktopInteropNs::wus::DispatcherQueueController{ nullptr };
        }

        return *DesktopInteropInternal::gDispatcherQueueController;
    }

    DesktopInteropInternal::gDispatcherQueueController = CreateDispatcherQueueControllerForCurrentThread();
    return *DesktopInteropInternal::gDispatcherQueueController;
}

bool DesktopInterop::CreateDesktopHostTestWindow(DesktopHostBackend backend, std::wstring* errorMessage)
{
    auto hostWindow = std::make_unique<DesktopInteropInternal::DesktopHostWindow>(backend);
    if (!hostWindow->CreateWindowInstance(errorMessage))
    {
        return false;
    }

    HWND hostHandle = hostWindow->GetWindowHandle();
    DesktopInteropInternal::gDesktopHostWindows.emplace(hostHandle, std::move(hostWindow));
    DesktopInteropInternal::DesktopHostWindow* instance = DesktopInteropInternal::gDesktopHostWindows[hostHandle].get();

    if (!instance->Initialize(errorMessage))
    {
        ::DestroyWindow(hostHandle);
        return false;
    }

    return true;
}
