#pragma once

#include <DispatcherQueue.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Graphics.h>

#include "DesktopCompositionInterop.h"

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
} // namespace DesktopInteropNs

namespace DesktopInteropInternal
{
    inline constexpr wchar_t kDesktopHostWindowClassName[] = L"Win2DViewer.DesktopHostWindow";
    inline constexpr wchar_t kDesktopHostPanelClassName[] = L"Win2DViewer.DesktopHostPanel";
    inline constexpr UINT_PTR kRenderTimerId = 1;

    inline constexpr int kPanelButtonWinRt = 2001;
    inline constexpr int kPanelButtonDComp = 2002;
    inline constexpr int kPanelButtonBoth = 2003;
    inline constexpr int kPanelButtonWinRtBackdrop = 2004;

#ifndef DWMWA_USE_HOSTBACKDROPBRUSH
    inline constexpr DWORD kDwmAttrUseHostBackdropBrush = 17;
#else
    inline constexpr DWORD kDwmAttrUseHostBackdropBrush = static_cast<DWORD>(DWMWA_USE_HOSTBACKDROPBRUSH);
#endif

    class DesktopHostWindow;

    extern std::optional<DesktopInteropNs::wus::DispatcherQueueController> gDispatcherQueueController;
    extern std::unordered_map<HWND, std::unique_ptr<DesktopHostWindow>> gDesktopHostWindows;

    std::wstring FormatLastErrorMessage(std::wstring_view context);
    void SetErrorMessage(std::wstring* errorMessage, std::wstring message);
    float Clamp01(float value);
    float Lerp(float a, float b, float t);
    uint8_t ToByte(float value);
    uint32_t PackBgra(uint8_t b, uint8_t g, uint8_t r);

    class DesktopHostWindow
    {
    public:
        explicit DesktopHostWindow(DesktopInterop::DesktopHostBackend backend);

        bool CreateWindowInstance(std::wstring* errorMessage);
        bool Initialize(std::wstring* errorMessage);
        HWND GetWindowHandle() const noexcept;
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    private:
        void InitializeWinRTComposition();
        void EnableHostBackdropForWindow() const;
        void ResizeWinRTSurface();
        void DrawWinRTSurface();
        void UpdateWinRTPhysics(float deltaSeconds);
        void ClampWinRTBallPosition();

        void InitializeDirectComposition();
        void CreateDCompCompositionSwapChain(DXGI_ALPHA_MODE alphaMode,
                                             UINT width,
                                             UINT height,
                                             IDXGISwapChain1** swapChain);
        void CreateD3D11Device();
        void InitializeDirectCompositionPipeline();
        void ResizeDirectCompositionSwapChain();
        RECT GetDCompOverlayPanelRectPixels() const;
        RECT GetDCompPrimaryChipRectPixels() const;
        RECT GetDCompSecondaryChipRectPixels() const;
        RECT GetDCompFieldRectPixels() const;
        bool IsPointInsideRect(RECT const& rect, LONG x, LONG y) const;
        void ClampDCompOverlayHandle();
        void UpdateDCompOverlayHoverState(LONG x, LONG y);
        void HandleDirectCompositionMouseMove(LONG x, LONG y);
        void HandleDirectCompositionMouseLeave();
        void HandleDirectCompositionLButtonDown(LONG x, LONG y);
        void HandleDirectCompositionLButtonUp(LONG x, LONG y);
        void RenderDirectCompositionOverlay();
        void RenderDirectCompositionFrame();

        void HandleResize();
        void RenderFrame();

    private:
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
} // namespace DesktopInteropInternal
