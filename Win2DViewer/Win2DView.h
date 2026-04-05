#pragma once

#include "ConsoleDebugMessages.h"
#include "MyScrollView.h"
#include "SvgDocument.h"
#include "WaitableRenderTimer.h"
#include "WinrtNsAliases.h"

#include <d2d1svg.h>
#include <winrt/Microsoft.Graphics.Canvas.Effects.h>
#include <winrt/Microsoft.Graphics.Canvas.SVG.h>
#include <winrt/Microsoft.Graphics.Canvas.Text.h>
#include <winrt/Microsoft.Graphics.Canvas.UI.Composition.h>
#include <winrt/Microsoft.Graphics.Canvas.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include "DeviceLostHelper.h"

class CWin2DView : public CWindowImpl<CWin2DView>,
                   public CMyScrollView
{
public:
    struct SvgTextOverlayItem
    {
        std::wstring text;
        float x = 0.0f;
        float y = 0.0f;
        float fontSize = 12.0f;
        bool bold = false;
        wna::wd::ui::Color color{ 255, 0, 0, 0 };
        std::wstring fontFamily = L"Segoe UI";
        wna::cv::txt::CanvasHorizontalAlignment textAlignment = wna::cv::txt::CanvasHorizontalAlignment::Left;
    };

    enum class RenderLayerMode
    {
        EffectsOverSvg = 0,
        SvgOverEffects = 1,
        SvgOnly = 2,
        EffectsOnly = 3
    };

    enum class RenderUpdatePolicy
    {
        StaticOptimized = 0,
        DynamicFullFrame = 1
    };

    DECLARE_WND_CLASS_EX(L"Win2DViewer.WTLView", CS_DBLCLKS, 0)

    CWin2DView() noexcept;
    ~CWin2DView();

    BOOL PreTranslateMessage(MSG* pMsg);

    void SetDocument(CSvgDocument* newDocument) noexcept;
    void RefreshDocument();
    void SetRenderLayerMode(RenderLayerMode mode);
    void SetConsoleDebugEnabled(bool enabled) noexcept;
    bool IsConsoleDebugEnabled() const noexcept { return consoleDebugEnabled; }
    RenderLayerMode GetRenderLayerMode() const noexcept { return renderLayerMode; }

    BEGIN_MSG_MAP(CWin2DView)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_HSCROLL, OnHScroll)
        MESSAGE_HANDLER(WM_VSCROLL, OnVScroll)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_GESTURE, OnGesture)
        MESSAGE_HANDLER(WM_APP + 1, OnRenderTick)
    END_MSG_MAP()

protected:
    void DrawClientRect(RECT& rect) override;
    void OnScrollPositionChanging(CPoint oldPos, CPoint newPos) override;

private:
    void OnDirect3DDeviceLost(DeviceLostHelper const*, DeviceLostEventArgs const&);
    wna::wd::uid::DesktopWindowTarget CreateDesktopWindowTarget(wna::wd::uic::Compositor const& compositor,
                                                                HWND window);
    void PrepareVisuals(wna::wd::uic::Compositor const& compositor);
    void SurfaceScroll(CPoint const& newPosition);
    void Zoom(int zDelta, CPoint screenPoint);
    bool ShouldAnimateEffects() const noexcept;
    RenderUpdatePolicy GetRenderUpdatePolicy() const noexcept;
    void StopRenderTimer() noexcept;
    void TryStartRenderTimer();
    void QueueRenderTick();
    void UpdateScrollBarVisibilityPolicy();
    void MarkSvgLayerDirty() noexcept;
    bool RenderSvgLayer(wna::cv::core::ICanvasResourceCreator const& resourceCreator,
                        float viewportWidth,
                        float viewportHeight,
                        wna::wd::num::float3x2 const& viewTransform);
    void DrawSvgTextOverlay(wna::cv::core::CanvasDrawingSession const& session,
                            wna::wd::num::float3x2 const& transform);

    bool Redraw(float cx, float cy, float wx, float wy, float width, float height, UINT dpi, CRect* clipRect = nullptr);
    void ScenarioWin2D(wna::wd::uic::Compositor const& compositor,
                       wna::wd::uic::ContainerVisual const& root,
                       UINT dpi,
                       int cx,
                       int cy);
    bool LoadSvg();
    void CreateFlameEffect();
    void SetupText(wna::cv::core::ICanvasResourceCreator resourceCreator);
    void ConfigureEffect();

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnHScroll(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnVScroll(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseWheel(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnGesture(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnRenderTick(UINT, WPARAM, LPARAM, BOOL&);

private:
    // Document and layer model.
    CSvgDocument* document = nullptr;
    RenderLayerMode renderLayerMode = RenderLayerMode::EffectsOverSvg;
    wna::cv::svg::CanvasSvgDocument svgDocument{ nullptr };
    wna::rt::hstring svgXml;
    float svgDocumentWidth = 0.0f;
    float svgDocumentHeight = 0.0f;
    std::vector<SvgTextOverlayItem> svgTextOverlays;

    // View transform and interaction state.
    int currentDpi = USER_DEFAULT_SCREEN_DPI;
    int width = 0;
    int height = 0;
    int ppmBitmapResolution = 72;
    wna::wd::num::float3x2 transformMatrix{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    bool translateDragging = false;
    CPoint currentMouse;
    CSize scrollDiff{ 0, 0 };
    clock_t scrollStartTime = 0;
    unsigned int scrollTimeDiff = 0;
    DWORD displayFrequency = 60;
    float angle = 0.0f;

    // Composition and rendering resources.
    wna::wd::uic::Compositor compositor;
    wna::wd::uid::DesktopWindowTarget target{ nullptr };
    wna::wd::uic::ContainerVisual root{ nullptr };
    wna::wd::uic::SpriteVisual contentVisual{ nullptr };
    wna::cv::core::CanvasDevice canvasDevice{ nullptr };
    DeviceLostHelper deviceLostHelper;
    bool inDeviceLost = false;
    wna::wd::uic::CompositionGraphicsDevice graphicsDevice{ nullptr };
    wna::wd::uic::CompositionDrawingSurface drawingSurface{ nullptr };
    wna::cv::core::CanvasRenderTarget svgLayerBitmap{ nullptr };
    wna::cv::core::CanvasRenderTarget sceneBitmap{ nullptr };
    wna::cv::core::CanvasRenderTarget trailBitmap{ nullptr };
    wna::cv::core::CanvasRenderTarget effectsTextBitmap{ nullptr };
    bool svgLayerDirty = true;
    wna::cv::eff::MorphologyEffect morphologyEffect{ nullptr };
    wna::cv::eff::CompositeEffect compositeEffect{ nullptr };
    wna::cv::eff::Transform2DEffect flameAnimation{ nullptr };
    wna::cv::eff::Transform2DEffect flamePosition{ nullptr };
    std::string pendingText;
    std::string displayText;
    float fontSize = 10.0f;

    // Timing and render scheduling.
    WaitableRenderTimer renderTimer;
    std::atomic_bool renderTickQueued{ false };
    std::chrono::nanoseconds activeRenderIntervalNs{ 0 };
    std::chrono::steady_clock::time_point lastRenderTickTime{};

    // Diagnostics and debug state.
    bool consoleDebugEnabled = false;
    int lastLoggedTextOverlayCount = -1;
    int lastLoggedTextOverlayFailures = -1;
};
