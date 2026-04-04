#include "pch.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
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
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")

namespace DesktopInteropNs
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
    constexpr int kPanelButtonWinRtBackdrop = 2004;

#ifndef DWMWA_USE_HOSTBACKDROPBRUSH
    constexpr DWORD kDwmAttrUseHostBackdropBrush = 17;
#else
    constexpr DWORD kDwmAttrUseHostBackdropBrush = static_cast<DWORD>(DWMWA_USE_HOSTBACKDROPBRUSH);
#endif

    std::optional<DesktopInteropNs::wus::DispatcherQueueController> gDispatcherQueueController;
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
        return (static_cast<uint32_t>(0xFF) << 24) |
            (static_cast<uint32_t>(r) << 16) |
            (static_cast<uint32_t>(g) << 8) |
            static_cast<uint32_t>(b);
    }

    class DesktopHostWindow
    {
    public:
        explicit DesktopHostWindow(DesktopInterop::DesktopHostBackend backend)
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
            DesktopInterop::EnsureDispatcherQueueControllerForCurrentThread();

            compositor = DesktopInteropNs::wuc::Compositor();

            namespace abi = ABI::Windows::UI::Composition::Desktop;
            auto interop = compositor.as<abi::ICompositorDesktopInterop>();
            DesktopInteropNs::wr::check_hresult(interop->CreateDesktopWindowTarget(
                windowHandle,
                true,
                reinterpret_cast<abi::IDesktopWindowTarget**>(DesktopInteropNs::wr::put_abi(compositionTarget))));

            rootVisual = compositor.CreateContainerVisual();
            rootVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
            compositionTarget.Root(rootVisual);

            if (backend == DesktopInterop::DesktopHostBackend::WinRTHostBackdrop)
            {
                EnableHostBackdropForWindow();
                hostBackdropVisual = compositor.CreateSpriteVisual();
                hostBackdropVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
                hostBackdropVisual.Brush(compositor.CreateHostBackdropBrush());
                rootVisual.Children().InsertAtTop(hostBackdropVisual);

                hostTintVisual = compositor.CreateSpriteVisual();
                hostTintVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
                hostTintVisual.Brush(compositor.CreateColorBrush({ 0x7A, 0x0F, 0x1D, 0x2B }));
                rootVisual.Children().InsertAtTop(hostTintVisual);
            }

            canvasDevice = DesktopInteropNs::mgc::CanvasDevice::GetSharedDevice();
            compositionGraphicsDevice = DesktopInteropNs::mgcu::CanvasComposition::CreateCompositionGraphicsDevice(compositor, canvasDevice);

            ResizeWinRTSurface();

            surfaceBrush = compositor.CreateSurfaceBrush(drawingSurface);
            surfaceBrush.Stretch(DesktopInteropNs::wuc::CompositionStretch::Fill);

            surfaceVisual = compositor.CreateSpriteVisual();
            surfaceVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
            surfaceVisual.Brush(surfaceBrush);
            rootVisual.Children().InsertAtTop(surfaceVisual);
        }

        void EnableHostBackdropForWindow() const
        {
            const BOOL enabled = TRUE;
            (void)::DwmSetWindowAttribute(
                windowHandle,
                kDwmAttrUseHostBackdropBrush,
                &enabled,
                sizeof(enabled));
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
                    DesktopInteropNs::wf::Size(static_cast<float>(width), static_cast<float>(height)),
                    DesktopInteropNs::wgx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    DesktopInteropNs::wgx::DirectXAlphaMode::Premultiplied);
            }
            else
            {
                drawingSurface.Resize(DesktopInteropNs::wg::SizeInt32{ width, height });
            }

            surfacePixelWidth = width;
            surfacePixelHeight = height;
            ClampWinRTBallPosition();
            DrawWinRTSurface();
        }

        void DrawWinRTSurface()
        {
            if (drawingSurface == nullptr)
            {
                return;
            }

            auto drawingSession = DesktopInteropNs::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface);

            const DesktopInteropNs::wui::Color backgroundColor{ 0xFF, 0x08, 0x1A, 0x2E };
            const DesktopInteropNs::wui::Color frameColor{ 0xFF, 0x2F, 0x6E, 0xB8 };
            const DesktopInteropNs::wui::Color orbColor{ 0xFF, 0xFF, 0xA5, 0x00 };
            const DesktopInteropNs::wui::Color textColor{ 0xFF, 0xF6, 0xF7, 0xFB };
            const DesktopInteropNs::wui::Color panelColor{ 0x98, 0x12, 0x1B, 0x25 };
            const DesktopInteropNs::wui::Color panelStroke{ 0xC0, 0x78, 0xB6, 0xF0 };
            const DesktopInteropNs::wui::Color velocityColor{ 0xD0, 0xFF, 0xE0, 0x66 };

            if (backend == DesktopInterop::DesktopHostBackend::WinRTComposition)
            {
                drawingSession.Clear(backgroundColor);
            }
            else
            {
                drawingSession.Clear(DesktopInteropNs::wui::Color{ 0x00, 0x00, 0x00, 0x00 });
            }

            const float width = static_cast<float>(surfacePixelWidth);
            const float height = static_cast<float>(surfacePixelHeight);
            const float inset = 20.0f;
            drawingSession.DrawRectangle(inset, inset, width - (inset * 2.0f), height - (inset * 2.0f), frameColor, 3.0f);

            const float radius = ballRadius;
            drawingSession.FillCircle(ballPosition, radius, orbColor);

            const float velocityScale = 0.12f;
            const DesktopInteropNs::wfn::float2 velocityTip
            {
                ballPosition.x + ballVelocity.x * velocityScale,
                ballPosition.y + ballVelocity.y * velocityScale
            };
            drawingSession.DrawLine(ballPosition, velocityTip, velocityColor, 3.0f);
            drawingSession.DrawCircle(ballPosition, radius + 6.0f, { 0x80, 0xFF, 0xD7, 0x8C }, 2.0f);

            drawingSession.FillRoundedRectangle({ 24.0f, height - 110.0f, width - 48.0f, 76.0f }, 12.0f, 12.0f, panelColor);
            drawingSession.DrawRoundedRectangle({ 24.0f, height - 110.0f, width - 48.0f, 76.0f }, 12.0f, 12.0f, panelStroke, 1.5f);

            const std::wstring title = (backend == DesktopInterop::DesktopHostBackend::WinRTHostBackdrop)
                ? L"WinRT Host Backdrop Material - Physics Test"
                : L"WinRT Composition Surface - Physics Test";
            drawingSession.DrawText(title, { 30.0f, 30.0f }, textColor);

            wchar_t infoLine[256]{};
            _snwprintf_s(
                infoLine,
                _TRUNCATE,
                L"Ball(%.1f, %.1f)  Velocity(%.1f, %.1f)  Radius %.1f",
                ballPosition.x,
                ballPosition.y,
                ballVelocity.x,
                ballVelocity.y,
                ballRadius);
            drawingSession.DrawText(infoLine, { 36.0f, height - 88.0f }, textColor);
        }

        void UpdateWinRTPhysics(float deltaSeconds)
        {
            if (surfacePixelWidth <= 0 || surfacePixelHeight <= 0)
            {
                return;
            }

            ballPosition.x += ballVelocity.x * deltaSeconds;
            ballPosition.y += ballVelocity.y * deltaSeconds;

            const float left = 20.0f + ballRadius;
            const float right = static_cast<float>(surfacePixelWidth) - 20.0f - ballRadius;
            const float top = 20.0f + ballRadius;
            const float bottom = static_cast<float>(surfacePixelHeight) - 20.0f - ballRadius;

            if (ballPosition.x < left)
            {
                ballPosition.x = left + (left - ballPosition.x);
                ballVelocity.x = std::abs(ballVelocity.x) * 0.95f;
            }
            else if (ballPosition.x > right)
            {
                ballPosition.x = right - (ballPosition.x - right);
                ballVelocity.x = -std::abs(ballVelocity.x) * 0.95f;
            }

            if (ballPosition.y < top)
            {
                ballPosition.y = top + (top - ballPosition.y);
                ballVelocity.y = std::abs(ballVelocity.y) * 0.95f;
            }
            else if (ballPosition.y > bottom)
            {
                ballPosition.y = bottom - (ballPosition.y - bottom);
                ballVelocity.y = -std::abs(ballVelocity.y) * 0.95f;
            }
        }

        void ClampWinRTBallPosition()
        {
            const float minDimension = static_cast<float>((std::min)(surfacePixelWidth, surfacePixelHeight));
            ballRadius = (std::max)(26.0f, minDimension * 0.08f);

            const float left = 20.0f + ballRadius;
            const float right = static_cast<float>(surfacePixelWidth) - 20.0f - ballRadius;
            const float top = 20.0f + ballRadius;
            const float bottom = static_cast<float>(surfacePixelHeight) - 20.0f - ballRadius;

            if (!physicsInitialized)
            {
                ballPosition = { static_cast<float>(surfacePixelWidth) * 0.5f, static_cast<float>(surfacePixelHeight) * 0.5f };
                ballVelocity = { 300.0f, 220.0f };
                physicsInitialized = true;
                return;
            }

            ballPosition.x = (std::max)(left, (std::min)(right, ballPosition.x));
            ballPosition.y = (std::max)(top, (std::min)(bottom, ballPosition.y));
        }

        void InitializeDirectComposition()
        {
            CreateD3D11Device();

            auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
            DesktopInteropNs::wr::check_hresult(::DCompositionCreateDevice2(
                dxgiDevice.get(),
                __uuidof(IDCompositionDevice),
                dcompDevice.put_void()));

            DesktopInteropNs::wr::check_hresult(dcompDevice->CreateTargetForHwnd(windowHandle, TRUE, dcompTarget.put()));
            DesktopInteropNs::wr::check_hresult(dcompDevice->CreateVisual(dcompVisual.put()));
            DesktopInteropNs::wr::check_hresult(dcompDevice->CreateVisual(dcompBackgroundVisual.put()));
            DesktopInteropNs::wr::check_hresult(dcompDevice->CreateVisual(dcompOverlayVisual.put()));

            InitializeDirectCompositionPipeline();
            ResizeDirectCompositionSwapChain();
            DesktopInteropNs::wr::check_hresult(dcompBackgroundVisual->SetContent(dcompSwapChain.get()));
            DesktopInteropNs::wr::check_hresult(dcompOverlayVisual->SetContent(dcompOverlaySwapChain.get()));
            DesktopInteropNs::wr::check_hresult(dcompVisual->AddVisual(dcompBackgroundVisual.get(), FALSE, nullptr));
            DesktopInteropNs::wr::check_hresult(dcompVisual->AddVisual(dcompOverlayVisual.get(), TRUE, dcompBackgroundVisual.get()));
            DesktopInteropNs::wr::check_hresult(dcompTarget->SetRoot(dcompVisual.get()));
            DesktopInteropNs::wr::check_hresult(dcompDevice->Commit());
        }

        void CreateDCompCompositionSwapChain(DXGI_ALPHA_MODE alphaMode, UINT width, UINT height, IDXGISwapChain1** swapChain)
        {
            auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
            DesktopInteropNs::wr::com_ptr<IDXGIAdapter> adapter;
            DesktopInteropNs::wr::check_hresult(dxgiDevice->GetAdapter(adapter.put()));

            DesktopInteropNs::wr::com_ptr<IDXGIFactory2> dxgiFactory;
            DesktopInteropNs::wr::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), dxgiFactory.put_void()));

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
            swapChainDesc.AlphaMode = alphaMode;
            swapChainDesc.Flags = 0;

            DesktopInteropNs::wr::check_hresult(dxgiFactory->CreateSwapChainForComposition(
                d3dDevice.get(),
                &swapChainDesc,
                nullptr,
                swapChain));
        }

        struct DCompFlowConstants
        {
            float time = 0.0f;
            float resolution[2]{};
            float _padding0 = 0.0f;
            float colorDark[4]{};
            float colorGold[4]{};
            float params[4]{};
        };

        struct DCompOverlayConstants
        {
            float resolution[2]{};
            float mouse[2]{};
            float handle[2]{};
            float time = 0.0f;
            float _padding0 = 0.0f;
            float params[4]{};
        };

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

            DesktopInteropNs::wr::check_hresult(hr);
        }

        void InitializeDirectCompositionPipeline()
        {
            static constexpr char kVertexShaderSource[] = R"(
                struct VSOut
                {
                    float4 position : SV_Position;
                    float2 uv : TEXCOORD0;
                };

                VSOut main(uint vertexId : SV_VertexID)
                {
                    float2 pos;
                    if (vertexId == 0) { pos = float2(-1.0, -1.0); }
                    else if (vertexId == 1) { pos = float2(-1.0, 3.0); }
                    else { pos = float2(3.0, -1.0); }

                    VSOut output;
                    output.position = float4(pos, 0.0, 1.0);
                    output.uv = pos * 0.5 + 0.5;
                    return output;
                }
            )";

            static constexpr char kPixelShaderSource[] = R"(
                cbuffer FlowConstants : register(b0)
                {
                    float gTime;
                    float2 gResolution;
                    float gPadding0;
                    float4 gColorDark;
                    float4 gColorGold;
                    float4 gParams;
                };

                float Hash21(float2 p)
                {
                    p = frac(p * float2(123.34, 456.21));
                    p += dot(p, p + 45.32);
                    return frac(p.x * p.y);
                }

                float2 Hash22(float2 p)
                {
                    float n = Hash21(p);
                    return float2(n, Hash21(p + n + 19.19));
                }

                float Noise(float2 p)
                {
                    float2 i = floor(p);
                    float2 f = frac(p);
                    float2 u = f * f * (3.0 - 2.0 * f);

                    float a = Hash21(i);
                    float b = Hash21(i + float2(1.0, 0.0));
                    float c = Hash21(i + float2(0.0, 1.0));
                    float d = Hash21(i + float2(1.0, 1.0));

                    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
                }

                float Fbm(float2 p)
                {
                    float value = 0.0;
                    float amplitude = 0.55;
                    [unroll]
                    for (int i = 0; i < 4; ++i)
                    {
                        value += Noise(p) * amplitude;
                        p = mul(float2x2(1.62, -1.18, 1.18, 1.62), p) + 9.7;
                        amplitude *= 0.52;
                    }
                    return value;
                }

                float2 Rotate(float2 p, float angle)
                {
                    float s = sin(angle);
                    float c = cos(angle);
                    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
                }

                void PickPalette(float t, out float3 colorA, out float3 colorB)
                {
                    static const float3 paletteA[6] =
                    {
                        float3(0.66, 0.13, 0.16),
                        float3(0.70, 0.12, 0.24),
                        float3(0.62, 0.18, 0.14),
                        float3(0.74, 0.14, 0.20),
                        float3(0.68, 0.16, 0.12),
                        float3(0.72, 0.13, 0.28)
                    };
                    static const float3 paletteB[6] =
                    {
                        float3(0.99, 0.36, 0.30),
                        float3(1.00, 0.46, 0.50),
                        float3(1.00, 0.50, 0.34),
                        float3(0.99, 0.42, 0.38),
                        float3(0.98, 0.54, 0.40),
                        float3(1.00, 0.46, 0.58)
                    };

                    const float k = t * 0.11;
                    const float idx = floor(k);
                    const float fracK = smoothstep(0.0, 1.0, frac(k));
                    const int i0 = (int)fmod(idx, 6.0);
                    const int i1 = (i0 + 1) % 6;

                    colorA = lerp(paletteA[i0], paletteA[i1], fracK);
                    colorB = lerp(paletteB[i0], paletteB[i1], fracK);

                    colorA = lerp(colorA, gColorDark.rgb, 0.05);
                    colorB = lerp(colorB, gColorGold.rgb, 0.04);
                }

                void PickBackgroundPalette(float t, out float3 bgA, out float3 bgB)
                {
                    static const float3 paletteBgA[4] =
                    {
                        float3(0.80, 0.72, 0.60),
                        float3(0.82, 0.68, 0.61),
                        float3(0.78, 0.71, 0.64),
                        float3(0.84, 0.70, 0.60)
                    };
                    static const float3 paletteBgB[4] =
                    {
                        float3(0.97, 0.86, 0.76),
                        float3(0.98, 0.80, 0.74),
                        float3(0.95, 0.85, 0.80),
                        float3(0.99, 0.84, 0.73)
                    };

                    const float k = t * 0.035;
                    const float idx = floor(k);
                    const float fracK = smoothstep(0.0, 1.0, frac(k));
                    const int i0 = (int)fmod(idx, 4.0);
                    const int i1 = (i0 + 1) % 4;

                    bgA = lerp(paletteBgA[i0], paletteBgA[i1], fracK);
                    bgB = lerp(paletteBgB[i0], paletteBgB[i1], fracK);
                }
            )"
            R"(
                float MetaballContribution(float2 p, float2 center, float radius, float aspect, float angle)
                {
                    float2 d = Rotate(p - center, angle);
                    d /= float2(
                        max(radius * aspect, 0.001),
                        max(radius / max(aspect, 0.001), 0.001));

                    const float dist2 = dot(d, d);
                    return 1.0 / (1.0 + dist2 * 2.4);
                }

                float SegmentContribution(float2 p, float2 a, float2 b, float radius, float softness)
                {
                    const float2 ab = b - a;
                    const float abLen2 = max(dot(ab, ab), 0.0001);
                    const float t = saturate(dot(p - a, ab) / abLen2);
                    const float2 closest = lerp(a, b, t);
                    const float2 delta = p - closest;
                    const float dist = length(delta) / max(radius, 0.001);
                    return exp(-pow(dist, max(softness, 0.2)));
                }

                float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
                {
                    const float flowSpeed = max(gParams.x, 0.2);
                    const float paletteSpeed = max(gParams.y, 0.2);
                    const float particleGain = max(gParams.z, 0.0);
                    const float swirlGain = max(gParams.w, 0.2);

                    const float time = gTime * flowSpeed;
                    const float paletteTime = gTime * paletteSpeed;
                    const float bgPaletteTime = gTime * max(paletteSpeed * 0.58, 0.18);
                    float2 p = uv - 0.5;
                    p.x *= gResolution.x / max(1.0, gResolution.y);

                    float3 colorA;
                    float3 colorB;
                    PickPalette(paletteTime, colorA, colorB);

                    float3 bgA;
                    float3 bgB;
                    PickBackgroundPalette(bgPaletteTime, bgA, bgB);

                    const float bgFlowA = Fbm(p * 1.05 + float2(time * 0.06, -time * 0.04));
                    const float bgFlowB = Fbm(Rotate(p * 0.88, 0.24) + float2(-time * 0.05, time * 0.03));
                    const float bgAxis = saturate(0.50 + (bgFlowA - 0.5) * 0.34 + (bgFlowB - 0.5) * 0.24 + p.y * 0.11);
                    float3 color = lerp(bgA, bgB, bgAxis);
                    color = lerp(color, lerp(bgA, bgB, 0.62 + 0.12 * sin(p.x * 0.85 + paletteTime * 0.06)), 0.20);
                    const float warmWashA = exp(-pow(length((p - float2(0.38, -0.14)) * float2(0.92, 1.04)) / 1.20, 2.0));
                    const float warmWashB = exp(-pow(length((p - float2(-0.52, 0.24)) * float2(1.04, 0.96)) / 1.08, 2.0));
                    color = lerp(color, lerp(bgA, bgB, 0.76), warmWashA * 0.18);
                    color = lerp(color, lerp(bgA, bgB, 0.22), warmWashB * 0.14);

                    const float focusPulse = 0.5 + 0.5 * sin(time * 0.70 + sin(time * 0.23) * 1.10 + cos(time * 0.11) * 0.72);
                    const float gatherPulse = 0.5 + 0.5 * sin(time * 0.52 + sin(time * 0.17) * 0.92);
                    const float driftPulse = 0.5 + 0.5 * cos(time * 0.40 + sin(time * 0.15) * 0.80);

                    const float2 ingressDir = normalize(float2(
                        cos(time * 0.14 + sin(time * 0.05) * 0.52),
                        sin(time * 0.12 - cos(time * 0.06) * 0.48)));
                    const float2 ingressNrm = float2(-ingressDir.y, ingressDir.x);
                    const float2 sceneDrift = float2(
                        sin(time * 0.045 + sin(time * 0.016) * 1.1),
                        cos(time * 0.038 + cos(time * 0.018) * 0.9)) * float2(0.30, 0.22);
                    const float2 hub0 = float2(
                        1.18 * sin(time * 0.10 + 0.35) + 0.24 * sin(time * 0.24 + 2.10),
                        0.62 * cos(time * 0.08 + 1.10)) + sceneDrift + ingressDir * 0.18;
                    const float2 hub1 = float2(
                        -0.18 + 0.82 * sin(time * 0.09 + 0.80) + 0.26 * sin(time * 0.18 + 1.90),
                        -0.08 + 0.46 * cos(time * 0.07 + 1.30)) - sceneDrift * 0.36;
                    const float2 hub2 = float2(
                        1.02 * cos(time * 0.06 + 2.10) + 0.34 * sin(time * 0.14 + 0.50),
                        -0.26 + 0.52 * sin(time * 0.09 + 1.70)) + sceneDrift * 0.28;
                    const float2 hub3 = float2(
                        -0.74 * cos(time * 0.05 + 0.50) + 0.56 * sin(time * 0.12 + 2.30),
                        0.38 * sin(time * 0.08 + 2.40) + 0.28 * cos(time * 0.10 + 0.90)) - sceneDrift * 0.22;
                    const float2 hubOpp = hub1 - ingressDir * (1.14 + 0.34 * driftPulse) + ingressNrm * (0.28 * sin(time * 0.20 + 0.7));

                    const float2 flowWarp = float2(
                        Fbm(p * 0.86 + float2(time * 0.22, -time * 0.18) + float2(1.6, -2.1)) - 0.5,
                        Fbm(Rotate(p * 0.82, 0.42) + float2(-time * 0.20, time * 0.16) + float2(-1.8, 2.2)) - 0.5);
                    const float2 coarseWarp = float2(
                        Fbm(p * 0.42 + float2(time * 0.08, -time * 0.07) + float2(3.2, -1.4)) - 0.5,
                        Fbm(Rotate(p * 0.38, 0.30) + float2(-time * 0.06, time * 0.05) + float2(-2.8, 2.5)) - 0.5);
                    const float2 q = p + flowWarp * 0.06 + coarseWarp * 0.10;

                    const float irregular = 0.5 + 0.5 * sin(time * 0.94 + sin(time * 0.37) * 1.40 + cos(time * 0.11) * 0.90);
                    const float orbitMixA = 0.5 + 0.5 * sin(time * 1.02 + cos(time * 0.33) * 1.1);
                    const float orbitMixB = 0.5 + 0.5 * sin(time * 0.88 + 1.7 + sin(time * 0.27) * 0.9);
                    const float2 cA = hub1 + float2(-0.18, 0.05) * (0.30 + 0.74 * irregular) + float2(cos(time * 0.74), sin(time * 0.68)) * 0.05;
                    const float2 cB = hub1 + float2(0.16, -0.04) * (0.28 + 0.78 * (1.0 - irregular)) + float2(cos(time * 0.66 + 1.6), sin(time * 0.72 + 0.8)) * 0.05;
                    const float2 cC = hub1 + float2(0.04, 0.14) * (0.16 + 0.66 * focusPulse) + float2(cos(time * 0.82 + 2.2), sin(time * 0.76 + 1.1)) * 0.04;
                    const float2 cD = hub1 + float2(-0.03, -0.16) * (0.16 + 0.62 * driftPulse) + float2(cos(time * 0.78 + 0.5), sin(time * 0.84 + 2.7)) * 0.04;
                    const float2 dA = hub2 + float2(cos(time * 0.46 + 2.0), sin(time * 0.52 + 1.1)) * (0.16 + 0.10 * driftPulse);
                    const float2 dB = hub2 + float2(cos(time * 0.58 + 0.8), sin(time * 0.62 + 2.4)) * (0.12 + 0.08 * focusPulse);
                    const float2 dC = lerp(hub2, hub3, 0.46 + (orbitMixA - 0.5) * 0.18) + float2(cos(time * 0.40 + 0.7), sin(time * 0.44 + 1.5)) * 0.05;
                    const float2 eA = hub3 + float2(cos(time * 0.44 + 1.7), sin(time * 0.50 + 0.3)) * (0.14 + 0.10 * gatherPulse);
                    const float2 eB = hub3 + float2(cos(time * 0.54 + 2.6), sin(time * 0.58 + 1.6)) * (0.10 + 0.08 * focusPulse);
                    const float2 oA = hubOpp + float2(cos(time * 0.42 + 2.7), sin(time * 0.48 + 0.9)) * (0.10 + 0.06 * orbitMixB);
                    const float2 oB = hubOpp + float2(cos(time * 0.50 + 1.4), sin(time * 0.56 + 2.2)) * (0.08 + 0.05 * gatherPulse);

                    float field = 0.0;
                    field += MetaballContribution(q, hub0 + ingressDir * (-0.18 + gatherPulse * 0.08) + ingressNrm * 0.04, 0.24, 1.34, -0.14) * 0.22;
                    field += MetaballContribution(q, hub0 + ingressDir * (0.08 + driftPulse * 0.18) - ingressNrm * (0.05 + 0.04 * focusPulse), 0.20, 1.24, 0.08) * 0.18;
                    field += MetaballContribution(q, hub0 + float2(cos(time * 0.38 + 0.4), sin(time * 0.42 + 1.2)) * (0.10 + 0.05 * gatherPulse), 0.16, 1.18, 0.28) * 0.15;

                    field += MetaballContribution(q, cA, 0.25, 1.18, 0.36) * 0.34;
                    field += MetaballContribution(q, cB, 0.25, 1.20, -0.28) * 0.34;
                    field += MetaballContribution(q, cC, 0.22, 1.14, 0.10) * 0.30;
                    field += MetaballContribution(q, cD, 0.20, 1.08, -0.10) * 0.26;

                    field += MetaballContribution(q, dA, 0.22, 1.20, -0.12) * 0.28;
                    field += MetaballContribution(q, dB, 0.20, 1.16, 0.20) * 0.24;
                    field += MetaballContribution(q, dC, 0.18, 1.12, -0.04) * 0.20;
                    field += MetaballContribution(q, eA, 0.22, 1.18, 0.38) * 0.28;
                    field += MetaballContribution(q, eB, 0.19, 1.12, -0.28) * 0.22;

                    field += MetaballContribution(q, oA, 0.18, 1.18, -0.46) * 0.18;
                    field += MetaballContribution(q, oB, 0.15, 1.12, 0.18) * 0.14;

                    const float trailA = SegmentContribution(q, cA, cB, 0.48, 1.6) * 0.38;
                    const float trailB = SegmentContribution(q, cB, dA, 0.42, 1.5) * 0.30;
                    const float trailC = SegmentContribution(q, dB, eA, 0.46, 1.5) * 0.28;
                    const float trailD = SegmentContribution(q, eB, oA, 0.52, 1.7) * 0.22;
                    const float trailField = trailA + trailB + trailC + trailD;

                    const float flowAxis = dot(q, ingressDir);
                    const float crossAxis = dot(q, ingressNrm);
                    const float sheetWaveA = Fbm(float2(flowAxis * 0.42 - time * 0.12, crossAxis * 0.72 + time * 0.10) + float2(1.4, -2.6));
                    const float sheetWaveB = Fbm(float2(flowAxis * 0.58 + time * 0.14, crossAxis * 0.54 - time * 0.12) + float2(-2.8, 1.7));
                    const float sheetDrift = sin(flowAxis * 1.4 - time * 0.34 + sheetWaveA * 1.8) * 0.24
                        + cos(flowAxis * 0.9 + time * 0.28 + sheetWaveB * 1.5) * 0.18;
                    const float sheetEnvelope = exp(-pow((crossAxis + sheetDrift) / 1.18, 2.0));
                    const float sheetRidgeA = smoothstep(0.30, 0.82, sheetWaveA * 0.64 + sheetWaveB * 0.36);
                    const float sheetRidgeB = smoothstep(0.26, 0.80, sheetWaveB * 0.58 + sheetWaveA * 0.42);
                    const float sheetField = sheetEnvelope * (0.44 + sheetRidgeA * 0.34 + sheetRidgeB * 0.22);

                    const float mistNoiseA = Fbm((q + coarseWarp * 0.48) * 0.72 + float2(-time * 0.16, time * 0.12));
                    const float mistNoiseB = Fbm(Rotate(q * 0.68, -0.34) + float2(time * 0.14, -time * 0.10) + float2(2.8, -1.2));
                    const float mistField = sheetField * 0.58 + field * 0.16 + trailField * 0.18 + mistNoiseA * 0.18 + mistNoiseB * 0.14;

                    const float hazeMask = smoothstep(0.28, 0.86, mistField);
                    const float bodyMask = smoothstep(0.37, 0.92, sheetField * 0.58 + trailField * 0.18 + field * 0.14 + mistNoiseA * 0.12);
                    const float coreMask = smoothstep(0.53, 0.96, sheetField * 0.36 + field * 0.42 + trailField * 0.12);
            )"
            R"(
                    const float2 nkx = float2(0.024, 0.000);
                    const float2 nky = float2(0.000, 0.024);
                    const float n0 = Fbm(q * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n1 = Fbm((q + nkx) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n2 = Fbm((q - nkx) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n3 = Fbm((q + nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n4 = Fbm((q - nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n5 = Fbm((q + nkx + nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n6 = Fbm((q + nkx - nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n7 = Fbm((q - nkx + nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n8 = Fbm((q - nkx - nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float blurNoise = n0 * 0.30 + (n1 + n2 + n3 + n4) * 0.12 + (n5 + n6 + n7 + n8) * 0.055;
            )"
            R"(
                    const float swirlA = Fbm(Rotate(q * 0.92, 0.74) + float2(-time * 0.58, time * 0.48));
                    const float swirlB = Fbm(q * 1.06 + float2(time * 0.52, -time * 0.46) + float2(2.4, -1.3));
                    const float swirlC = Fbm((q + float2(swirlA - 0.5, swirlB - 0.5) * 0.24) * 1.18 + float2(-time * 0.44, time * 0.40));
                    const float stirField = smoothstep(0.18, 0.82, swirlA * 0.34 + swirlB * 0.30 + swirlC * 0.36);
                    const float2 stirOffset = float2(swirlA - swirlB, swirlC - swirlA) * (0.056 * max(hazeMask, bodyMask));

                    const float motionField = smoothstep(0.28, 0.74, blurNoise * 0.46 + stirField * 0.34 + mistField * 0.20);
                    const float warmShift = 0.5 + 0.5 * sin(time * 0.74 + motionField * 2.4 + q.x * 0.8 + q.y * 0.5);
                    const float3 accentA = lerp(colorA, colorB, 0.48 + 0.14 * warmShift);
                    const float3 accentB = lerp(colorA, colorB, 0.84 - 0.10 * warmShift);
                    const float3 hazeColor = lerp(lerp(bgA, bgB, 0.54), lerp(accentA, accentB, 0.44 + 0.10 * warmShift), 0.48);
                    const float3 edgeTint = lerp(hazeColor, lerp(bgA, bgB, 0.58), 0.18);
                    const float colorField = saturate(0.28 + bodyMask * 0.20 + coreMask * 0.22 + motionField * 0.18 + (stirField - 0.5) * 0.30);
                    float3 waveColor = lerp(edgeTint, lerp(accentA, accentB, colorField), smoothstep(0.20, 0.76, bodyMask + coreMask * 0.36));
                    const float3 warmFocus = lerp(float3(0.99, 0.32, 0.28), float3(1.00, 0.46, 0.40), warmShift);
                    waveColor = lerp(waveColor, warmFocus, 0.16 + coreMask * 0.12 + stirField * 0.06);

                    color = lerp(color, hazeColor, hazeMask * (0.32 + motionField * 0.05));
                    const float foregroundAlpha = saturate(hazeMask * 0.56 + bodyMask * 0.28 + coreMask * 0.10);
                    color = lerp(color, waveColor, foregroundAlpha * 0.88);
                    color += lerp(accentB, float3(1.0, 0.95, 0.92), 0.54) * coreMask * (0.018 + stirField * 0.020);

                    const float2 refractOffset = (flowWarp * 0.012 + stirOffset + ingressDir * 0.008 * sin(time * 0.96 + dot(p, ingressDir) * 2.3)) * foregroundAlpha;
                    const float2 refrP = p + refractOffset;
                    const float refrAxis = saturate(0.60 + refrP.y * 0.08 + refrP.x * 0.05 + (Fbm(refrP * 1.12 + float2(time * 0.24, -time * 0.18)) - 0.5) * 0.10);
                    const float3 refrColor = lerp(bgA, bgB, refrAxis);
                    color = lerp(color, refrColor, foregroundAlpha * 0.015);

                    const float mote = smoothstep(0.978, 1.0, Hash21((uv + flowWarp * 0.06 + stirOffset) * gResolution * 0.11 + time * 1.42));
                    color += lerp(waveColor, float3(1.0, 1.0, 1.0), 0.42) * mote * (0.028 * particleGain);

                    const float grain = Hash21(uv * gResolution * 0.26 + time * 6.0) - 0.5;
                    color += grain * 0.010;

                    const float vignette = 1.0 - smoothstep(1.02, 1.34, length(p));
                    color *= lerp(0.97, 1.0, vignette);

                    color = saturate(color);
                    return float4(color, 1.0);
                }
            )";

            static constexpr char kOverlayPixelShaderSource[] = R"(
                cbuffer OverlayConstants : register(b0)
                {
                    float2 gResolution;
                    float2 gMouse;
                    float2 gHandle;
                    float gTime;
                    float gPadding0;
                    float4 gParams;
                };

                float SdRoundedRect(float2 p, float2 center, float2 halfSize, float radius)
                {
                    float2 d = abs(p - center) - halfSize + radius;
                    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
                }

                float SdSegment(float2 p, float2 a, float2 b)
                {
                    float2 pa = p - a;
                    float2 ba = b - a;
                    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 0.0001));
                    return length(pa - ba * h);
                }

                float FillMask(float sdf, float feather)
                {
                    return 1.0 - smoothstep(0.0, max(feather, 0.0001), sdf);
                }

                void Composite(inout float4 dst, float3 srcColor, float srcAlpha)
                {
                    dst.rgb = srcColor * srcAlpha + dst.rgb * (1.0 - srcAlpha);
                    dst.a = srcAlpha + dst.a * (1.0 - srcAlpha);
                }

                float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
                {
                    const float2 px = position.xy;
                    const float margin = 28.0;
                    const float panelWidth = min(gResolution.x * 0.36, 372.0);
                    const float panelHeight = min(gResolution.y * 0.34, 206.0);
                    const float2 panelHalf = float2(panelWidth * 0.5, panelHeight * 0.5);
                    const float2 panelCenter = float2(gResolution.x - margin - panelHalf.x, margin + panelHalf.y);

                    const float panelSdf = SdRoundedRect(px, panelCenter, panelHalf, 24.0);
                    const float panelMask = FillMask(panelSdf, 2.0);

                    float4 output = float4(0.0, 0.0, 0.0, 0.0);
                    const float3 panelColor = lerp(float3(0.10, 0.11, 0.14), float3(0.15, 0.10, 0.11), 0.5 + 0.5 * sin(gTime * 0.26));
                    Composite(output, panelColor, panelMask * 0.78);

                    const float2 chipSize = float2(panelWidth * 0.28, 34.0);
                    const float chipGap = 14.0;
                    const float2 chip1Center = panelCenter + float2(-panelHalf.x + 28.0 + chipSize.x * 0.5, -panelHalf.y + 30.0);
                    const float2 chip2Center = chip1Center + float2(chipSize.x + chipGap, 0.0);
                    const float chip1Mask = FillMask(SdRoundedRect(px, chip1Center, chipSize * 0.5, 16.0), 1.5);
                    const float chip2Mask = FillMask(SdRoundedRect(px, chip2Center, chipSize * 0.5, 16.0), 1.5);

                    const float hoverPrimary = saturate(gParams.x);
                    const float hoverSecondary = saturate(gParams.y);
                    const float accentToggle = saturate(gParams.z);
                    const float dragging = saturate(gParams.w);

                    const float3 chip1Color = lerp(float3(0.83, 0.31, 0.28), float3(0.98, 0.49, 0.38), 0.45 + hoverPrimary * 0.35 + accentToggle * 0.12);
                    const float3 chip2Color = lerp(float3(0.93, 0.72, 0.58), float3(0.99, 0.82, 0.68), 0.28 + hoverSecondary * 0.34 + (1.0 - accentToggle) * 0.10);
                    Composite(output, chip1Color, chip1Mask * (0.78 + hoverPrimary * 0.12));
                    Composite(output, chip2Color, chip2Mask * (0.66 + hoverSecondary * 0.14));

                    const float2 fieldMin = panelCenter + float2(-panelHalf.x + 22.0, -8.0);
                    const float2 fieldMax = panelCenter + float2(panelHalf.x - 22.0, panelHalf.y - 20.0);
                    const float2 fieldCenter = (fieldMin + fieldMax) * 0.5;
                    const float2 fieldHalf = (fieldMax - fieldMin) * 0.5;
                    const float fieldMask = FillMask(SdRoundedRect(px, fieldCenter, fieldHalf, 18.0), 1.4);

                    const float2 nodeA = float2(fieldMin.x + 26.0, fieldMax.y - 28.0 + sin(gTime * 0.62) * 8.0);
                    const float2 nodeB = gHandle;
                    const float2 nodeC = float2(fieldMax.x - 24.0, fieldMin.y + 24.0 + cos(gTime * 0.74) * 10.0);
                    const float2 nodeD = lerp(nodeA, nodeC, 0.50) + float2(cos(gTime * 0.84), sin(gTime * 0.92)) * 22.0;

                    const float linkAB = FillMask(SdSegment(px, nodeA, nodeB) - 5.0, 1.8);
                    const float linkBC = FillMask(SdSegment(px, nodeB, nodeC) - 4.5, 1.8);
                    const float linkAD = FillMask(SdSegment(px, nodeA, nodeD) - 3.8, 1.6);
                    const float linkDC = FillMask(SdSegment(px, nodeD, nodeC) - 3.8, 1.6);
                    const float linkMix = saturate(linkAB * 0.44 + linkBC * 0.40 + linkAD * 0.28 + linkDC * 0.28) * fieldMask;

                    const float2 flowUv = (px - fieldMin) / max(fieldMax - fieldMin, float2(1.0, 1.0));
                    const float sweep = sin((flowUv.x * 2.8 - flowUv.y * 1.4) * 3.14159 + gTime * 1.4 + accentToggle * 0.9) * 0.5 + 0.5;
                    const float sweep2 = cos((flowUv.x * 1.2 + flowUv.y * 2.1) * 3.14159 - gTime * 1.1) * 0.5 + 0.5;
                    const float3 linkColor = lerp(float3(0.96, 0.42, 0.38), float3(0.99, 0.76, 0.60), sweep * 0.58 + sweep2 * 0.18);
                    Composite(output, linkColor, linkMix * (0.22 + dragging * 0.10));

                    const float nodeMaskA = FillMask(length(px - nodeA) - 9.0, 1.4) * fieldMask;
                    const float nodeMaskB = FillMask(length(px - nodeB) - 11.0, 1.6) * fieldMask;
                    const float nodeMaskC = FillMask(length(px - nodeC) - 8.5, 1.4) * fieldMask;
                    const float nodeMaskD = FillMask(length(px - nodeD) - 7.0, 1.3) * fieldMask;

                    Composite(output, float3(0.98, 0.82, 0.70), nodeMaskA * 0.60);
                    Composite(output, float3(1.00, 0.49, 0.40), nodeMaskB * (0.88 + dragging * 0.08));
                    Composite(output, float3(0.99, 0.70, 0.56), nodeMaskC * 0.56);
                    Composite(output, float3(0.98, 0.58, 0.48), nodeMaskD * 0.46);

                    const float handleHalo = FillMask(length(px - nodeB) - 22.0, 10.0) * fieldMask;
                    Composite(output, float3(1.00, 0.72, 0.62), handleHalo * 0.08);

                    const float mouseHalo = FillMask(length(px - gMouse) - 20.0, 16.0) * panelMask;
                    Composite(output, float3(1.0, 0.96, 0.92), mouseHalo * 0.12);

                    return saturate(output);
                }
            )";

            UINT shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
            shaderFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

            DesktopInteropNs::wr::com_ptr<ID3DBlob> vertexShaderBlob;
            DesktopInteropNs::wr::com_ptr<ID3DBlob> pixelShaderBlob;
            DesktopInteropNs::wr::com_ptr<ID3DBlob> errorBlob;

            HRESULT hr = ::D3DCompile(
                kVertexShaderSource,
                sizeof(kVertexShaderSource) - 1,
                nullptr,
                nullptr,
                nullptr,
                "main",
                "vs_5_0",
                shaderFlags,
                0,
                vertexShaderBlob.put(),
                errorBlob.put());
            DesktopInteropNs::wr::check_hresult(hr);

            errorBlob = nullptr;
            hr = ::D3DCompile(
                kPixelShaderSource,
                sizeof(kPixelShaderSource) - 1,
                nullptr,
                nullptr,
                nullptr,
                "main",
                "ps_5_0",
                shaderFlags,
                0,
                pixelShaderBlob.put(),
                errorBlob.put());
            DesktopInteropNs::wr::check_hresult(hr);

            DesktopInteropNs::wr::check_hresult(d3dDevice->CreateVertexShader(
                vertexShaderBlob->GetBufferPointer(),
                vertexShaderBlob->GetBufferSize(),
                nullptr,
                dcompVertexShader.put()));

            DesktopInteropNs::wr::check_hresult(d3dDevice->CreatePixelShader(
                pixelShaderBlob->GetBufferPointer(),
                pixelShaderBlob->GetBufferSize(),
                nullptr,
                dcompPixelShader.put()));

            errorBlob = nullptr;
            hr = ::D3DCompile(
                kOverlayPixelShaderSource,
                sizeof(kOverlayPixelShaderSource) - 1,
                nullptr,
                nullptr,
                nullptr,
                "main",
                "ps_5_0",
                shaderFlags,
                0,
                pixelShaderBlob.put(),
                errorBlob.put());
            DesktopInteropNs::wr::check_hresult(hr);

            DesktopInteropNs::wr::check_hresult(d3dDevice->CreatePixelShader(
                pixelShaderBlob->GetBufferPointer(),
                pixelShaderBlob->GetBufferSize(),
                nullptr,
                dcompOverlayPixelShader.put()));

            D3D11_BUFFER_DESC constantBufferDesc{};
            constantBufferDesc.ByteWidth = sizeof(DCompFlowConstants);
            constantBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            constantBufferDesc.MiscFlags = 0;
            constantBufferDesc.StructureByteStride = 0;
            DesktopInteropNs::wr::check_hresult(d3dDevice->CreateBuffer(&constantBufferDesc, nullptr, dcompConstantBuffer.put()));

            constantBufferDesc.ByteWidth = sizeof(DCompOverlayConstants);
            DesktopInteropNs::wr::check_hresult(d3dDevice->CreateBuffer(&constantBufferDesc, nullptr, dcompOverlayConstantBuffer.put()));
        }

        void ResizeDirectCompositionSwapChain()
        {
            RECT clientRect{};
            ::GetClientRect(windowHandle, &clientRect);

            const UINT width = static_cast<UINT>((std::max<LONG>)(1L, clientRect.right - clientRect.left));
            const UINT height = static_cast<UINT>((std::max<LONG>)(1L, clientRect.bottom - clientRect.top));

            if (dcompSwapChain == nullptr)
            {
                CreateDCompCompositionSwapChain(DXGI_ALPHA_MODE_IGNORE, width, height, dcompSwapChain.put());
                CreateDCompCompositionSwapChain(DXGI_ALPHA_MODE_PREMULTIPLIED, width, height, dcompOverlaySwapChain.put());
            }
            else
            {
                dcompRenderTargetView = nullptr;
                dcompOverlayRenderTargetView = nullptr;
                d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
                d3dContext->ClearState();
                d3dContext->Flush();
                DesktopInteropNs::wr::check_hresult(dcompSwapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
                DesktopInteropNs::wr::check_hresult(dcompOverlaySwapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
            }

            DesktopInteropNs::wr::com_ptr<ID3D11Texture2D> backBuffer;
            DesktopInteropNs::wr::check_hresult(dcompSwapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));
            DesktopInteropNs::wr::check_hresult(d3dDevice->CreateRenderTargetView(backBuffer.get(), nullptr, dcompRenderTargetView.put()));

            DesktopInteropNs::wr::com_ptr<ID3D11Texture2D> overlayBackBuffer;
            DesktopInteropNs::wr::check_hresult(dcompOverlaySwapChain->GetBuffer(0, IID_PPV_ARGS(overlayBackBuffer.put())));
            DesktopInteropNs::wr::check_hresult(d3dDevice->CreateRenderTargetView(overlayBackBuffer.get(), nullptr, dcompOverlayRenderTargetView.put()));

            dcompPixelWidth = width;
            dcompPixelHeight = height;
            ClampDCompOverlayHandle();
        }

        RECT GetDCompOverlayPanelRectPixels() const
        {
            const LONG panelWidth = static_cast<LONG>((std::min)(dcompPixelWidth * 36 / 100, 372u));
            const LONG panelHeight = static_cast<LONG>((std::min)(dcompPixelHeight * 34 / 100, 206u));
            RECT rect{};
            rect.left = static_cast<LONG>(dcompPixelWidth) - 28 - panelWidth;
            rect.top = 28;
            rect.right = rect.left + panelWidth;
            rect.bottom = rect.top + panelHeight;
            return rect;
        }

        RECT GetDCompPrimaryChipRectPixels() const
        {
            const RECT panel = GetDCompOverlayPanelRectPixels();
            const LONG chipWidth = (panel.right - panel.left) * 28 / 100;
            RECT rect{};
            rect.left = panel.left + 28;
            rect.top = panel.top + 13;
            rect.right = rect.left + chipWidth;
            rect.bottom = rect.top + 34;
            return rect;
        }

        RECT GetDCompSecondaryChipRectPixels() const
        {
            RECT rect = GetDCompPrimaryChipRectPixels();
            const LONG gap = 14;
            const LONG width = rect.right - rect.left;
            rect.left += width + gap;
            rect.right = rect.left + width;
            return rect;
        }

        RECT GetDCompFieldRectPixels() const
        {
            const RECT panel = GetDCompOverlayPanelRectPixels();
            RECT rect{};
            rect.left = panel.left + 22;
            rect.top = panel.top + 58;
            rect.right = panel.right - 22;
            rect.bottom = panel.bottom - 20;
            return rect;
        }

        bool IsPointInsideRect(const RECT& rect, LONG x, LONG y) const
        {
            return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
        }

        void ClampDCompOverlayHandle()
        {
            if (dcompPixelWidth == 0 || dcompPixelHeight == 0)
            {
                return;
            }

            const RECT fieldRect = GetDCompFieldRectPixels();
            if (!dcompOverlayHandleInitialized)
            {
                dcompOverlayHandleX = Lerp(static_cast<float>(fieldRect.left), static_cast<float>(fieldRect.right), 0.58f);
                dcompOverlayHandleY = Lerp(static_cast<float>(fieldRect.bottom), static_cast<float>(fieldRect.top), 0.46f);
                dcompOverlayHandleInitialized = true;
            }

            dcompOverlayHandleX = (std::max)(static_cast<float>(fieldRect.left + 12), (std::min)(static_cast<float>(fieldRect.right - 12), dcompOverlayHandleX));
            dcompOverlayHandleY = (std::max)(static_cast<float>(fieldRect.top + 12), (std::min)(static_cast<float>(fieldRect.bottom - 12), dcompOverlayHandleY));
        }

        void UpdateDCompOverlayHoverState(LONG x, LONG y)
        {
            dcompMouseX = static_cast<float>(x);
            dcompMouseY = static_cast<float>(y);
            dcompHoverPrimary = IsPointInsideRect(GetDCompPrimaryChipRectPixels(), x, y);
            dcompHoverSecondary = IsPointInsideRect(GetDCompSecondaryChipRectPixels(), x, y);

            const float dx = dcompOverlayHandleX - static_cast<float>(x);
            const float dy = dcompOverlayHandleY - static_cast<float>(y);
            dcompHoverHandle = ((dx * dx) + (dy * dy)) <= (18.0f * 18.0f);
        }

        void HandleDirectCompositionMouseMove(LONG x, LONG y)
        {
            if (!dcompMouseTracking)
            {
                TRACKMOUSEEVENT trackMouseEvent{};
                trackMouseEvent.cbSize = sizeof(trackMouseEvent);
                trackMouseEvent.dwFlags = TME_LEAVE;
                trackMouseEvent.hwndTrack = windowHandle;
                ::TrackMouseEvent(&trackMouseEvent);
                dcompMouseTracking = true;
            }

            dcompMouseInside = true;
            UpdateDCompOverlayHoverState(x, y);

            if (dcompOverlayDragging)
            {
                dcompOverlayHandleX = static_cast<float>(x);
                dcompOverlayHandleY = static_cast<float>(y);
                ClampDCompOverlayHandle();
            }
        }

        void HandleDirectCompositionMouseLeave()
        {
            dcompMouseTracking = false;
            dcompMouseInside = false;
            dcompHoverPrimary = false;
            dcompHoverSecondary = false;
            dcompHoverHandle = false;
        }

        void HandleDirectCompositionLButtonDown(LONG x, LONG y)
        {
            UpdateDCompOverlayHoverState(x, y);

            if (dcompHoverPrimary)
            {
                dcompOverlayAccentEnabled = !dcompOverlayAccentEnabled;
            }
            else if (dcompHoverSecondary)
            {
                dcompOverlayLinkBoost = !dcompOverlayLinkBoost;
            }
            else if (dcompHoverHandle || IsPointInsideRect(GetDCompFieldRectPixels(), x, y))
            {
                dcompOverlayDragging = true;
                dcompOverlayHandleX = static_cast<float>(x);
                dcompOverlayHandleY = static_cast<float>(y);
                ClampDCompOverlayHandle();
                ::SetCapture(windowHandle);
            }
        }

        void HandleDirectCompositionLButtonUp(LONG x, LONG y)
        {
            UpdateDCompOverlayHoverState(x, y);
            if (dcompOverlayDragging)
            {
                dcompOverlayDragging = false;
                ::ReleaseCapture();
            }
        }

        void RenderDirectCompositionOverlay()
        {
            if (dcompOverlaySwapChain == nullptr || dcompOverlayRenderTargetView == nullptr || dcompOverlayConstantBuffer == nullptr)
            {
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT mapHr = d3dContext->Map(dcompOverlayConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(mapHr))
            {
                return;
            }

            auto* constants = reinterpret_cast<DCompOverlayConstants*>(mapped.pData);
            constants->resolution[0] = static_cast<float>(dcompPixelWidth);
            constants->resolution[1] = static_cast<float>(dcompPixelHeight);
            constants->mouse[0] = dcompMouseX;
            constants->mouse[1] = dcompMouseY;
            constants->handle[0] = dcompOverlayHandleX;
            constants->handle[1] = dcompOverlayHandleY;
            constants->time = animationPhase;
            constants->params[0] = dcompHoverPrimary ? 1.0f : 0.0f;
            constants->params[1] = dcompHoverSecondary ? 1.0f : 0.0f;
            constants->params[2] = dcompOverlayAccentEnabled ? 1.0f : 0.0f;
            constants->params[3] = dcompOverlayDragging ? 1.0f : (dcompOverlayLinkBoost ? 0.55f : 0.0f);
            d3dContext->Unmap(dcompOverlayConstantBuffer.get(), 0);

            const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            d3dContext->ClearRenderTargetView(dcompOverlayRenderTargetView.get(), clearColor);

            ID3D11RenderTargetView* renderTargetViews[] = { dcompOverlayRenderTargetView.get() };
            d3dContext->OMSetRenderTargets(1, renderTargetViews, nullptr);
            d3dContext->IASetInputLayout(nullptr);
            d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            d3dContext->VSSetShader(dcompVertexShader.get(), nullptr, 0);
            d3dContext->PSSetShader(dcompOverlayPixelShader.get(), nullptr, 0);
            ID3D11Buffer* constantBuffers[] = { dcompOverlayConstantBuffer.get() };
            d3dContext->PSSetConstantBuffers(0, 1, constantBuffers);
            d3dContext->Draw(3, 0);

            (void)dcompOverlaySwapChain->Present(1, 0);
        }

        void RenderDirectCompositionFrame()
        {
            if (dcompSwapChain == nullptr || dcompRenderTargetView == nullptr || dcompConstantBuffer == nullptr)
            {
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT mapHr = d3dContext->Map(dcompConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(mapHr))
            {
                return;
            }

            auto* constants = reinterpret_cast<DCompFlowConstants*>(mapped.pData);
            constants->time = animationPhase;
            constants->resolution[0] = static_cast<float>(dcompPixelWidth);
            constants->resolution[1] = static_cast<float>(dcompPixelHeight);
            constants->colorDark[0] = 0.380f;
            constants->colorDark[1] = 0.090f;
            constants->colorDark[2] = 0.090f;
            constants->colorDark[3] = 1.0f;
            constants->colorGold[0] = 1.000f;
            constants->colorGold[1] = 0.420f;
            constants->colorGold[2] = 0.380f;
            constants->colorGold[3] = 1.0f;
            constants->params[0] = 1.78f;
            constants->params[1] = 1.18f;
            constants->params[2] = 0.46f;
            constants->params[3] = 0.72f;
            d3dContext->Unmap(dcompConstantBuffer.get(), 0);

            D3D11_VIEWPORT viewport{};
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width = static_cast<float>(dcompPixelWidth);
            viewport.Height = static_cast<float>(dcompPixelHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            d3dContext->RSSetViewports(1, &viewport);

            ID3D11RenderTargetView* renderTargetViews[] = { dcompRenderTargetView.get() };
            d3dContext->OMSetRenderTargets(1, renderTargetViews, nullptr);
            d3dContext->IASetInputLayout(nullptr);
            d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            d3dContext->VSSetShader(dcompVertexShader.get(), nullptr, 0);
            d3dContext->PSSetShader(dcompPixelShader.get(), nullptr, 0);
            ID3D11Buffer* constantBuffers[] = { dcompConstantBuffer.get() };
            d3dContext->PSSetConstantBuffers(0, 1, constantBuffers);
            d3dContext->Draw(3, 0);

            (void)dcompSwapChain->Present(1, 0);
            RenderDirectCompositionOverlay();
        }

        void HandleResize()
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

        void RenderFrame()
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

    private:
        DesktopInterop::DesktopHostBackend backend = DesktopInterop::DesktopHostBackend::WinRTComposition;
        HWND windowHandle = nullptr;
        bool rendererInitialized = false;
        float animationPhase = 0.0f;
        bool hasLastFrameTime = false;
        std::chrono::steady_clock::time_point lastFrameTime{};
        int surfacePixelWidth = 0;
        int surfacePixelHeight = 0;
        bool physicsInitialized = false;
        float ballRadius = 34.0f;
        DesktopInteropNs::wfn::float2 ballPosition{ 180.0f, 180.0f };
        DesktopInteropNs::wfn::float2 ballVelocity{ 300.0f, 220.0f };

        DesktopInteropNs::wuc::Compositor compositor{ nullptr };
        DesktopInteropNs::wud::DesktopWindowTarget compositionTarget{ nullptr };
        DesktopInteropNs::wuc::ContainerVisual rootVisual{ nullptr };
        DesktopInteropNs::wuc::SpriteVisual hostBackdropVisual{ nullptr };
        DesktopInteropNs::wuc::SpriteVisual hostTintVisual{ nullptr };
        DesktopInteropNs::wuc::SpriteVisual surfaceVisual{ nullptr };
        DesktopInteropNs::wuc::CompositionSurfaceBrush surfaceBrush{ nullptr };
        DesktopInteropNs::mgc::CanvasDevice canvasDevice{ nullptr };
        DesktopInteropNs::wuc::CompositionGraphicsDevice compositionGraphicsDevice{ nullptr };
        DesktopInteropNs::wuc::CompositionDrawingSurface drawingSurface{ nullptr };

        DesktopInteropNs::wr::com_ptr<ID3D11Device> d3dDevice;
        DesktopInteropNs::wr::com_ptr<ID3D11DeviceContext> d3dContext;
        DesktopInteropNs::wr::com_ptr<IDCompositionDevice> dcompDevice;
        DesktopInteropNs::wr::com_ptr<IDCompositionTarget> dcompTarget;
        DesktopInteropNs::wr::com_ptr<IDCompositionVisual> dcompVisual;
        DesktopInteropNs::wr::com_ptr<IDCompositionVisual> dcompBackgroundVisual;
        DesktopInteropNs::wr::com_ptr<IDCompositionVisual> dcompOverlayVisual;
        DesktopInteropNs::wr::com_ptr<IDXGISwapChain1> dcompSwapChain;
        DesktopInteropNs::wr::com_ptr<IDXGISwapChain1> dcompOverlaySwapChain;
        DesktopInteropNs::wr::com_ptr<ID3D11RenderTargetView> dcompRenderTargetView;
        DesktopInteropNs::wr::com_ptr<ID3D11RenderTargetView> dcompOverlayRenderTargetView;
        DesktopInteropNs::wr::com_ptr<ID3D11VertexShader> dcompVertexShader;
        DesktopInteropNs::wr::com_ptr<ID3D11PixelShader> dcompPixelShader;
        DesktopInteropNs::wr::com_ptr<ID3D11PixelShader> dcompOverlayPixelShader;
        DesktopInteropNs::wr::com_ptr<ID3D11Buffer> dcompConstantBuffer;
        DesktopInteropNs::wr::com_ptr<ID3D11Buffer> dcompOverlayConstantBuffer;
        UINT dcompPixelWidth = 0;
        UINT dcompPixelHeight = 0;
        bool dcompMouseTracking = false;
        bool dcompMouseInside = false;
        bool dcompHoverPrimary = false;
        bool dcompHoverSecondary = false;
        bool dcompHoverHandle = false;
        bool dcompOverlayDragging = false;
        bool dcompOverlayAccentEnabled = true;
        bool dcompOverlayLinkBoost = false;
        bool dcompOverlayHandleInitialized = false;
        float dcompMouseX = 0.0f;
        float dcompMouseY = 0.0f;
        float dcompOverlayHandleX = 0.0f;
        float dcompOverlayHandleY = 0.0f;
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
                L"Create WinRT Physics Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                12,
                60,
                250,
                32,
                windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonWinRt)),
                ::GetModuleHandleW(nullptr),
                nullptr);

            winRtBackdropButton = ::CreateWindowExW(
                0,
                WC_BUTTONW,
                L"Create WinRT Host Backdrop Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                272,
                60,
                250,
                32,
                windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonWinRtBackdrop)),
                ::GetModuleHandleW(nullptr),
                nullptr);

            dcompButton = ::CreateWindowExW(
                0,
                WC_BUTTONW,
                L"Create DirectComposition Flow Host",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                12,
                102,
                250,
                32,
                windowHandle,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelButtonDComp)),
                ::GetModuleHandleW(nullptr),
                nullptr);

            bothButton = ::CreateWindowExW(
                0,
                WC_BUTTONW,
                L"Create All Hosts",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                272,
                102,
                250,
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
                ::SendMessageW(winRtBackdropButton, WM_SETFONT, reinterpret_cast<WPARAM>(messageFont), TRUE);
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
                CreateHostFromPanel(DesktopInterop::DesktopHostBackend::WinRTComposition);
                return 0;
            case kPanelButtonWinRtBackdrop:
                CreateHostFromPanel(DesktopInterop::DesktopHostBackend::WinRTHostBackdrop);
                return 0;
            case kPanelButtonDComp:
                CreateHostFromPanel(DesktopInterop::DesktopHostBackend::DirectComposition);
                return 0;
            case kPanelButtonBoth:
                CreateHostFromPanel(DesktopInterop::DesktopHostBackend::WinRTComposition);
                CreateHostFromPanel(DesktopInterop::DesktopHostBackend::WinRTHostBackdrop);
                CreateHostFromPanel(DesktopInterop::DesktopHostBackend::DirectComposition);
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
                ::MoveWindow(winRtBackdropButton, rightButtonX, 60, rightButtonWidth, 32, TRUE);
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
                const std::wstring message = L"Failed to create desktop host window.\n" + errorMessage;
                ::MessageBoxW(windowHandle, message.c_str(), L"Desktop Host Test", MB_OK | MB_ICONERROR);
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
}

DesktopInteropNs::wus::DispatcherQueueController DesktopInterop::CreateDispatcherQueueControllerForCurrentThread()
{
    namespace abi = ABI::Windows::System;

    DispatcherQueueOptions options
    {
        sizeof(DispatcherQueueOptions),
        DQTYPE_THREAD_CURRENT,
        DQTAT_COM_STA
    };

    DesktopInteropNs::wus::DispatcherQueueController controller{ nullptr };
    DesktopInteropNs::wr::check_hresult(::CreateDispatcherQueueController(
        options,
        reinterpret_cast<abi::IDispatcherQueueController**>(DesktopInteropNs::wr::put_abi(controller))));
    return controller;
}

DesktopInteropNs::wus::DispatcherQueueController DesktopInterop::EnsureDispatcherQueueControllerForCurrentThread()
{
    if (DesktopInteropNs::wus::DispatcherQueue::GetForCurrentThread() != nullptr)
    {
        if (!gDispatcherQueueController.has_value())
        {
            return DesktopInteropNs::wus::DispatcherQueueController{ nullptr };
        }

        return *gDispatcherQueueController;
    }

    gDispatcherQueueController = CreateDispatcherQueueControllerForCurrentThread();
    return *gDispatcherQueueController;
}

bool DesktopInterop::CreateDesktopHostTestWindow(DesktopHostBackend backend, std::wstring* errorMessage)
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

void DesktopInterop::ShowDesktopHostTestPanel(HWND ownerWindow)
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
