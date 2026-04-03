#include "pch.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

#include <winrt/Windows.Graphics.h>

#include "DesktopCompositionInterop.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

namespace ns
{
    namespace wr = wna::rt;
    namespace wus = wna::wd::sys;
    namespace wf = wna::wd::fnd;
    namespace wfn = wna::wd::num;
    namespace wui = wna::wd::ui;
    namespace wuc = wna::wd::uic;
    namespace wud = wna::wd::uid;
    namespace wg = wna::wd::gfx;
    namespace wgx = wna::wd::gdx;
    namespace mgc = wna::cv::core;
    namespace mgcu = wna::cv::uic;
}

namespace
{
    constexpr wchar_t kDesktopHostWindowClassName[] = L"Win2DViewer.DesktopHostWindow";
    constexpr wchar_t kDesktopHostPanelClassName[] = L"Win2DViewer.DesktopHostPanel";
    constexpr UINT_PTR kRenderTimerId = 1;

    constexpr int kPanelButtonWinRt = 2001;
    constexpr int kPanelButtonDComp = 2002;
    constexpr int kPanelButtonBoth = 2003;

    std::optional<ns::wus::DispatcherQueueController> gDispatcherQueueController;
    class DesktopHostWindow;
    std::unordered_map<HWND, std::unique_ptr<DesktopHostWindow>> gDesktopHostWindows;

    std::wstring FormatLastErrorMessage(std::wstring_view context)
    {
        const DWORD errorCode = ::GetLastError();
        wchar_t* systemMessage = nullptr;
        ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&systemMessage),
            0,
            nullptr);

        std::wstring result{ context };
        result += L" failed. GetLastError=" + std::to_wstring(errorCode);
        if (systemMessage != nullptr)
        {
            result += L" (";
            result += systemMessage;
            result += L')';
            ::LocalFree(systemMessage);
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

    class DesktopHostWindow
    {
    public:
        explicit DesktopHostWindow(desktopinterop::DesktopHostBackend backend)
            : backend(backend)
        {
        }

        bool CreateWindowInstance(std::wstring* errorMessage)
        {
            WNDCLASSW windowClass{};
            windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.lpszClassName = kDesktopHostWindowClassName;
            windowClass.lpfnWndProc = &DesktopHostWindow::WndProc;
            windowClass.hbrBackground = nullptr;
            windowClass.style = CS_HREDRAW | CS_VREDRAW;
            ::RegisterClassW(&windowClass);

            const wchar_t* windowTitle = (backend == desktopinterop::DesktopHostBackend::WinRTComposition)
                ? L"Desktop Host - WinRT Composition Surface"
                : L"Desktop Host - DirectComposition Surface";

            const DWORD exStyle = WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP;
            const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
            windowHandle = ::CreateWindowExW(
                exStyle,
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

        bool Initialize(std::wstring* errorMessage)
        {
            try
            {
                if (backend == desktopinterop::DesktopHostBackend::WinRTComposition)
                {
                    InitializeWinRTComposition();
                }
                else
                {
                    InitializeDirectComposition();
                }

                rendererInitialized = true;
                ::SetTimer(windowHandle, kRenderTimerId, 16, nullptr);
                ::ShowWindow(windowHandle, SW_SHOWNORMAL);
                ::UpdateWindow(windowHandle);
                return true;
            }
            catch (ns::wr::hresult_error const& ex)
            {
                SetErrorMessage(errorMessage, std::wstring{ ex.message().c_str() });
                return false;
            }
        }

        HWND GetWindowHandle() const noexcept
        {
            return windowHandle;
        }

        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
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
            case WM_PAINT:
            {
                PAINTSTRUCT paintStruct{};
                ::BeginPaint(windowHandle, &paintStruct);
                ::EndPaint(windowHandle, &paintStruct);
                return 0;
            }
            case WM_DESTROY:
                ::KillTimer(windowHandle, kRenderTimerId);
                return 0;
            default:
                return ::DefWindowProcW(windowHandle, message, wParam, lParam);
            }
        }

        static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
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

            LRESULT result = (that != nullptr)
                ? that->HandleMessage(message, wParam, lParam)
                : ::DefWindowProcW(window, message, wParam, lParam);

            if (message == WM_NCDESTROY && that != nullptr)
            {
                ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                gDesktopHostWindows.erase(window);
            }

            return result;
        }

    private:
        void InitializeWinRTComposition()
        {
            desktopinterop::EnsureDispatcherQueueControllerForCurrentThread();

            compositor = ns::wuc::Compositor();

            namespace abi = ABI::Windows::UI::Composition::Desktop;
            auto interop = compositor.as<abi::ICompositorDesktopInterop>();
            ns::wr::check_hresult(interop->CreateDesktopWindowTarget(
                windowHandle,
                true,
                reinterpret_cast<abi::IDesktopWindowTarget**>(ns::wr::put_abi(compositionTarget))));

            rootVisual = compositor.CreateContainerVisual();
            rootVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
            compositionTarget.Root(rootVisual);

            canvasDevice = ns::mgc::CanvasDevice::GetSharedDevice();
            compositionGraphicsDevice = ns::mgcu::CanvasComposition::CreateCompositionGraphicsDevice(compositor, canvasDevice);

            ResizeWinRTSurface();

            surfaceBrush = compositor.CreateSurfaceBrush(drawingSurface);
            surfaceBrush.Stretch(ns::wuc::CompositionStretch::Fill);

            surfaceVisual = compositor.CreateSpriteVisual();
            surfaceVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
            surfaceVisual.Brush(surfaceBrush);
            rootVisual.Children().InsertAtTop(surfaceVisual);
        }

        void ResizeWinRTSurface()
        {
            RECT clientRect{};
            ::GetClientRect(windowHandle, &clientRect);

            const int width = static_cast<int>((std::max<LONG>)(1L, clientRect.right - clientRect.left));
            const int height = static_cast<int>((std::max<LONG>)(1L, clientRect.bottom - clientRect.top));

            if (drawingSurface == nullptr)
            {
                drawingSurface = compositionGraphicsDevice.CreateDrawingSurface(
                    ns::wf::Size(static_cast<float>(width), static_cast<float>(height)),
                    ns::wgx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    ns::wgx::DirectXAlphaMode::Premultiplied);
            }
            else
            {
                drawingSurface.Resize(ns::wg::SizeInt32{ width, height });
            }

            surfacePixelWidth = width;
            surfacePixelHeight = height;
            DrawWinRTSurface();
        }

        void DrawWinRTSurface()
        {
            if (drawingSurface == nullptr)
            {
                return;
            }

            auto drawingSession = ns::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface);

            const ns::wui::Color backgroundColor{ 0xFF, 0x08, 0x1A, 0x2E };
            const ns::wui::Color frameColor{ 0xFF, 0x2F, 0x6E, 0xB8 };
            const ns::wui::Color orbColor{ 0xFF, 0xFF, 0xA5, 0x00 };
            const ns::wui::Color textColor{ 0xFF, 0xF6, 0xF7, 0xFB };

            drawingSession.Clear(backgroundColor);

            const float width = static_cast<float>(surfacePixelWidth);
            const float height = static_cast<float>(surfacePixelHeight);
            const float inset = 20.0f;
            drawingSession.DrawRectangle(inset, inset, width - (inset * 2.0f), height - (inset * 2.0f), frameColor, 3.0f);

            const float radius = (std::max)(28.0f, (std::min)(width, height) * 0.10f);
            const float orbitX = (std::max)(20.0f, (width * 0.5f) - radius - 32.0f);
            const float orbitY = (std::max)(20.0f, (height * 0.5f) - radius - 32.0f);
            const float centerX = (width * 0.5f) + std::cos(animationPhase) * orbitX;
            const float centerY = (height * 0.5f) + std::sin(animationPhase * 1.3f) * orbitY;
            drawingSession.FillCircle({ centerX, centerY }, radius, orbColor);

            drawingSession.DrawText(L"WinRT Composition + Win2D Surface", { 30.0f, 30.0f }, textColor);
        }

        void InitializeDirectComposition()
        {
            CreateD3D11Device();

            auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
            ns::wr::check_hresult(::DCompositionCreateDevice2(
                dxgiDevice.get(),
                __uuidof(IDCompositionDevice),
                dcompDevice.put_void()));

            ns::wr::check_hresult(dcompDevice->CreateTargetForHwnd(windowHandle, TRUE, dcompTarget.put()));
            ns::wr::check_hresult(dcompDevice->CreateVisual(dcompVisual.put()));

            ResizeDirectCompositionSwapChain();
            ns::wr::check_hresult(dcompVisual->SetContent(dcompSwapChain.get()));
            ns::wr::check_hresult(dcompTarget->SetRoot(dcompVisual.get()));
            ns::wr::check_hresult(dcompDevice->Commit());
        }

        void CreateD3D11Device()
        {
            UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
            creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

            constexpr D3D_FEATURE_LEVEL featureLevels[] =
            {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0
            };

            D3D_FEATURE_LEVEL selectedFeatureLevel{};
            HRESULT hr = ::D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                creationFlags,
                featureLevels,
                _countof(featureLevels),
                D3D11_SDK_VERSION,
                d3dDevice.put(),
                &selectedFeatureLevel,
                d3dContext.put());

#ifdef _DEBUG
            if (FAILED(hr) && (creationFlags & D3D11_CREATE_DEVICE_DEBUG) != 0)
            {
                creationFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
                hr = ::D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_HARDWARE,
                    nullptr,
                    creationFlags,
                    featureLevels,
                    _countof(featureLevels),
                    D3D11_SDK_VERSION,
                    d3dDevice.put(),
                    &selectedFeatureLevel,
                    d3dContext.put());
            }
#endif

            if (FAILED(hr))
            {
                hr = ::D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_WARP,
                    nullptr,
                    creationFlags & ~D3D11_CREATE_DEVICE_DEBUG,
                    featureLevels,
                    _countof(featureLevels),
                    D3D11_SDK_VERSION,
                    d3dDevice.put(),
                    &selectedFeatureLevel,
                    d3dContext.put());
            }

            ns::wr::check_hresult(hr);
        }

        void ResizeDirectCompositionSwapChain()
        {
            RECT clientRect{};
            ::GetClientRect(windowHandle, &clientRect);

            const UINT width = static_cast<UINT>((std::max<LONG>)(1L, clientRect.right - clientRect.left));
            const UINT height = static_cast<UINT>((std::max<LONG>)(1L, clientRect.bottom - clientRect.top));

            if (dcompSwapChain == nullptr)
            {
                auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
                ns::wr::com_ptr<IDXGIAdapter> adapter;
                ns::wr::check_hresult(dxgiDevice->GetAdapter(adapter.put()));

                ns::wr::com_ptr<IDXGIFactory2> dxgiFactory;
                ns::wr::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), dxgiFactory.put_void()));

                DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
                swapChainDesc.Width = width;
                swapChainDesc.Height = height;
                swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                swapChainDesc.Stereo = FALSE;
                swapChainDesc.SampleDesc.Count = 1;
                swapChainDesc.SampleDesc.Quality = 0;
                swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swapChainDesc.BufferCount = 2;
                swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
                swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                swapChainDesc.Flags = 0;

                ns::wr::check_hresult(dxgiFactory->CreateSwapChainForComposition(
                    d3dDevice.get(),
                    &swapChainDesc,
                    nullptr,
                    dcompSwapChain.put()));
            }
            else
            {
                d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
                dcompRenderTargetView = nullptr;
                ns::wr::check_hresult(dcompSwapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
            }

            ns::wr::com_ptr<ID3D11Texture2D> backBuffer;
            ns::wr::check_hresult(dcompSwapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));
            ns::wr::check_hresult(d3dDevice->CreateRenderTargetView(backBuffer.get(), nullptr, dcompRenderTargetView.put()));
        }

        void RenderDirectCompositionFrame()
        {
            if (dcompRenderTargetView == nullptr || dcompSwapChain == nullptr)
            {
                return;
            }

            const float red = 0.14f + 0.30f * (0.5f + 0.5f * std::sin(animationPhase * 0.90f));
            const float green = 0.18f + 0.25f * (0.5f + 0.5f * std::sin(animationPhase * 1.35f + 1.1f));
            const float blue = 0.24f + 0.35f * (0.5f + 0.5f * std::sin(animationPhase * 0.70f + 2.0f));
            const float clearColor[4] = { red, green, blue, 1.0f };

            ID3D11RenderTargetView* renderTargetViews[] = { dcompRenderTargetView.get() };
            d3dContext->OMSetRenderTargets(1, renderTargetViews, nullptr);
            d3dContext->ClearRenderTargetView(dcompRenderTargetView.get(), clearColor);

            (void)dcompSwapChain->Present(1, 0);
        }

        void HandleResize()
        {
            if (backend == desktopinterop::DesktopHostBackend::WinRTComposition)
            {
                ResizeWinRTSurface();
            }
            else
            {
                ResizeDirectCompositionSwapChain();
                RenderDirectCompositionFrame();
            }
        }

        void RenderFrame()
        {
            animationPhase += 0.03f;

            if (backend == desktopinterop::DesktopHostBackend::WinRTComposition)
            {
                DrawWinRTSurface();
            }
            else
            {
                RenderDirectCompositionFrame();
            }
        }

    private:
        desktopinterop::DesktopHostBackend backend = desktopinterop::DesktopHostBackend::WinRTComposition;
        HWND windowHandle = nullptr;
        bool rendererInitialized = false;
        float animationPhase = 0.0f;
        int surfacePixelWidth = 0;
        int surfacePixelHeight = 0;

        ns::wuc::Compositor compositor{ nullptr };
        ns::wud::DesktopWindowTarget compositionTarget{ nullptr };
        ns::wuc::ContainerVisual rootVisual{ nullptr };
        ns::wuc::SpriteVisual surfaceVisual{ nullptr };
        ns::wuc::CompositionSurfaceBrush surfaceBrush{ nullptr };
        ns::mgc::CanvasDevice canvasDevice{ nullptr };
        ns::wuc::CompositionGraphicsDevice compositionGraphicsDevice{ nullptr };
        ns::wuc::CompositionDrawingSurface drawingSurface{ nullptr };

        ns::wr::com_ptr<ID3D11Device> d3dDevice;
        ns::wr::com_ptr<ID3D11DeviceContext> d3dContext;
        ns::wr::com_ptr<IDCompositionDevice> dcompDevice;
        ns::wr::com_ptr<IDCompositionTarget> dcompTarget;
        ns::wr::com_ptr<IDCompositionVisual> dcompVisual;
        ns::wr::com_ptr<IDXGISwapChain1> dcompSwapChain;
        ns::wr::com_ptr<ID3D11RenderTargetView> dcompRenderTargetView;
    };

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
            const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
            windowHandle = ::CreateWindowExW(
                exStyle,
                kDesktopHostPanelClassName,
                L"Desktop Host Test Panel",
                style,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                560,
                230,
                ownerWindow,
                nullptr,
                ::GetModuleHandleW(nullptr),
                this);

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

        static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            DesktopHostTestPanelWindow* that = nullptr;
            if (message == WM_NCCREATE)
            {
                auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
                that = static_cast<DesktopHostTestPanelWindow*>(createStruct->lpCreateParams);
                ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
                that->windowHandle = window;
            }
            else
            {
                that = reinterpret_cast<DesktopHostTestPanelWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
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
                0,
                WC_STATICW,
                L"Create isolated Win32 host windows for interop validation. Host windows use WS_EX_NOREDIRECTIONBITMAP.",
                WS_CHILD | WS_VISIBLE,
                12,
                12,
                520,
                36,
                windowHandle,
                nullptr,
                ::GetModuleHandleW(nullptr),
                nullptr);

            winRtButton = ::CreateWindowExW(
                0,
                WC_BUTTONW,
                L"Create WinRT Composition Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                12,
                60,
                250,
                32,
                windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonWinRt)),
                ::GetModuleHandleW(nullptr),
                nullptr);

            dcompButton = ::CreateWindowExW(
                0,
                WC_BUTTONW,
                L"Create DirectComposition Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                272,
                60,
                250,
                32,
                windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonDComp)),
                ::GetModuleHandleW(nullptr),
                nullptr);

            bothButton = ::CreateWindowExW(
                0,
                WC_BUTTONW,
                L"Create Both Hosts",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                12,
                104,
                510,
                32,
                windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonBoth)),
                ::GetModuleHandleW(nullptr),
                nullptr);

            HFONT messageFont = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            if (messageFont != nullptr)
            {
                ::SendMessageW(instructionLabel, WM_SETFONT, reinterpret_cast<WPARAM>(messageFont), TRUE);
                ::SendMessageW(winRtButton, WM_SETFONT, reinterpret_cast<WPARAM>(messageFont), TRUE);
                ::SendMessageW(dcompButton, WM_SETFONT, reinterpret_cast<WPARAM>(messageFont), TRUE);
                ::SendMessageW(bothButton, WM_SETFONT, reinterpret_cast<WPARAM>(messageFont), TRUE);
            }

            return 0;
        }

        LRESULT OnCommand(UINT commandId)
        {
            switch (commandId)
            {
            case kPanelButtonWinRt:
                CreateHostFromPanel(desktopinterop::DesktopHostBackend::WinRTComposition);
                return 0;
            case kPanelButtonDComp:
                CreateHostFromPanel(desktopinterop::DesktopHostBackend::DirectComposition);
                return 0;
            case kPanelButtonBoth:
                CreateHostFromPanel(desktopinterop::DesktopHostBackend::WinRTComposition);
                CreateHostFromPanel(desktopinterop::DesktopHostBackend::DirectComposition);
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
            const int buttonWidth = (std::max)(160, (width - 36) / 2);
            const int rightButtonX = 18 + buttonWidth;
            const int rightButtonWidth = width - rightButtonX - 18;
            const int combinedButtonWidth = width - 24 - 12;

            if (instructionLabel != nullptr)
            {
                ::MoveWindow(instructionLabel, 12, 12, width - 24, 36, TRUE);
            }
            if (winRtButton != nullptr)
            {
                ::MoveWindow(winRtButton, 12, 60, buttonWidth, 32, TRUE);
            }
            if (dcompButton != nullptr)
            {
                ::MoveWindow(dcompButton, rightButtonX, 60, rightButtonWidth, 32, TRUE);
            }
            if (bothButton != nullptr)
            {
                ::MoveWindow(bothButton, 12, 104, combinedButtonWidth, 32, TRUE);
            }
        }

        void CreateHostFromPanel(desktopinterop::DesktopHostBackend backend) const
        {
            std::wstring errorMessage;
            if (!desktopinterop::CreateDesktopHostTestWindow(backend, &errorMessage))
            {
                const std::wstring message = L"Failed to create desktop host window.\n" + errorMessage;
                ::MessageBoxW(windowHandle, message.c_str(), L"Desktop Host Test", MB_OK | MB_ICONERROR);
            }
        }

    private:
        HWND windowHandle = nullptr;
        HWND instructionLabel = nullptr;
        HWND winRtButton = nullptr;
        HWND dcompButton = nullptr;
        HWND bothButton = nullptr;
    };

    std::unique_ptr<DesktopHostTestPanelWindow> gDesktopHostTestPanel;
}

ns::wus::DispatcherQueueController desktopinterop::CreateDispatcherQueueControllerForCurrentThread()
{
    namespace abi = ABI::Windows::System;

    DispatcherQueueOptions options
    {
        sizeof(DispatcherQueueOptions),
        DQTYPE_THREAD_CURRENT,
        DQTAT_COM_STA
    };

    ns::wus::DispatcherQueueController controller{ nullptr };
    ns::wr::check_hresult(::CreateDispatcherQueueController(
        options,
        reinterpret_cast<abi::IDispatcherQueueController**>(ns::wr::put_abi(controller))));
    return controller;
}

ns::wus::DispatcherQueueController desktopinterop::EnsureDispatcherQueueControllerForCurrentThread()
{
    if (ns::wus::DispatcherQueue::GetForCurrentThread() != nullptr)
    {
        if (!gDispatcherQueueController.has_value())
        {
            return ns::wus::DispatcherQueueController{ nullptr };
        }

        return *gDispatcherQueueController;
    }

    gDispatcherQueueController = CreateDispatcherQueueControllerForCurrentThread();
    return *gDispatcherQueueController;
}

bool desktopinterop::CreateDesktopHostTestWindow(DesktopHostBackend backend, std::wstring* errorMessage)
{
    auto hostWindow = std::make_unique<DesktopHostWindow>(backend);
    if (!hostWindow->CreateWindowInstance(errorMessage))
    {
        return false;
    }

    HWND hostHandle = hostWindow->GetWindowHandle();
    gDesktopHostWindows.emplace(hostHandle, std::move(hostWindow));
    DesktopHostWindow* instance = gDesktopHostWindows[hostHandle].get();

    if (!instance->Initialize(errorMessage))
    {
        ::DestroyWindow(hostHandle);
        return false;
    }

    return true;
}

void desktopinterop::ShowDesktopHostTestPanel(HWND ownerWindow)
{
    if (gDesktopHostTestPanel != nullptr && gDesktopHostTestPanel->IsWindow())
    {
        gDesktopHostTestPanel->ShowAndActivate();
        return;
    }

    auto panel = std::make_unique<DesktopHostTestPanelWindow>();
    if (!panel->Create(ownerWindow))
    {
        const std::wstring error = FormatLastErrorMessage(L"Create desktop host test panel");
        ::MessageBoxW(ownerWindow, error.c_str(), L"Desktop Host Test", MB_OK | MB_ICONERROR);
        return;
    }

    panel->ShowAndActivate();
    gDesktopHostTestPanel = std::move(panel);
}
