#include "pch.h"

#include "Win2DViewInternal.h"

namespace
{
    std::chrono::nanoseconds GetNormalRenderInterval()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double, std::milli>(1000.0 / 60.0));
    }
}

void CWin2DView::StopRenderTimer() noexcept
{
    renderTimer.Stop();
    renderTickQueued.store(false, std::memory_order_release);
    activeRenderIntervalNs = std::chrono::nanoseconds(0);
    lastRenderTickTime = std::chrono::steady_clock::time_point{};
}

void CWin2DView::QueueRenderTick()
{
    if (!::IsWindow(m_hWnd))
    {
        return;
    }

    if (!renderTickQueued.exchange(true, std::memory_order_acq_rel))
    {
        if (!::PostMessage(m_hWnd, Win2DViewInternal::kRenderTickMsg, 0, 0))
        {
            renderTickQueued.store(false, std::memory_order_release);
        }
    }
}

void CWin2DView::MarkSvgLayerDirty() noexcept
{
    svgLayerDirty = true;
}

bool CWin2DView::RenderSvgLayer(wna::cv::core::ICanvasResourceCreator const& resourceCreator,
                                float viewportWidth,
                                float viewportHeight,
                                wna::wd::num::float3x2 const& viewTransform)
{
    if (svgDocument == nullptr || viewportWidth <= 0.0f || viewportHeight <= 0.0f)
    {
        svgLayerBitmap = nullptr;
        svgLayerDirty = false;
        return false;
    }

    bool recreateBitmap = (svgLayerBitmap == nullptr);
    if (!recreateBitmap)
    {
        const auto size = svgLayerBitmap.Size();
        recreateBitmap =
            (std::fabs(size.Width - viewportWidth) > 0.5f || std::fabs(size.Height - viewportHeight) > 0.5f);
    }

    if (recreateBitmap)
    {
        svgLayerBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
            resourceCreator, viewportWidth, viewportHeight, Win2DViewInternal::kPixelsDpi);
    }

    auto session = svgLayerBitmap.CreateDrawingSession();
    auto transparent = Win2DViewNs::wui::Colors::Black();
    transparent.A = 0;
    session.Clear(transparent);

    session.Transform(viewTransform);
    const float svgWidth = (svgDocumentWidth > 0.0f) ? svgDocumentWidth : viewportWidth;
    const float svgHeight = (svgDocumentHeight > 0.0f) ? svgDocumentHeight : viewportHeight;
    session.DrawSvg(svgDocument, Win2DViewNs::wf::Size(svgWidth, svgHeight));
    DrawSvgTextOverlay(session, viewTransform);
    session.Close();

    svgLayerDirty = false;
    return true;
}

void CWin2DView::TryStartRenderTimer()
{
    if (!::IsWindow(m_hWnd) || !ShouldAnimateEffects() || drawingSurface == nullptr || width <= 0 || height <= 0)
    {
        StopRenderTimer();
        return;
    }

    const std::chrono::nanoseconds desiredInterval = GetNormalRenderInterval();
    const bool restartRequired = !renderTimer.IsRunning() || desiredInterval != activeRenderIntervalNs;
    if (restartRequired)
    {
        const bool timerStarted = renderTimer.Start(desiredInterval, [this]() {
            if (!::IsWindow(m_hWnd))
            {
                return false;
            }

            if (!renderTickQueued.exchange(true, std::memory_order_acq_rel))
            {
                if (!::PostMessage(m_hWnd, Win2DViewInternal::kRenderTickMsg, 0, 0))
                {
                    renderTickQueued.store(false, std::memory_order_release);
                    return false;
                }
            }
            return true;
        });
        if (!timerStarted)
        {
            StopRenderTimer();
            return;
        }

        activeRenderIntervalNs = desiredInterval;
    }

    QueueRenderTick();
}

Win2DViewNs::wud::DesktopWindowTarget CWin2DView::CreateDesktopWindowTarget(
    Win2DViewNs::wuc::Compositor const& compositor,
    HWND window)
{
    namespace abi = ABI::Windows::UI::Composition::Desktop;

    auto interop = compositor.as<abi::ICompositorDesktopInterop>();
    Win2DViewNs::wud::DesktopWindowTarget target{ nullptr };
    Win2DViewNs::wr::check_hresult(interop->CreateDesktopWindowTarget(
        window, true, reinterpret_cast<abi::IDesktopWindowTarget**>(Win2DViewNs::wr::put_abi(target))));
    return target;
}

void CWin2DView::PrepareVisuals(Win2DViewNs::wuc::Compositor const& compositor)
{
    target = CreateDesktopWindowTarget(compositor, m_hWnd);

    root = compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({ 1.0f, 1.0f });

    contentVisual = compositor.CreateSpriteVisual();
    contentVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });

    root.Children().InsertAtTop(contentVisual);

    target.Root(root);
}

void CWin2DView::OnDirect3DDeviceLost(DeviceLostHelper const*, DeviceLostEventArgs const&)
{
    inDeviceLost = true;
    StopRenderTimer();

    auto canvasDevice = Win2DViewNs::mgc::CanvasDevice::GetSharedDevice();
    Win2DViewNs::wr::com_ptr<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop> graphicsDeviceInterop{
        graphicsDevice.as<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>()
    };

    Win2DViewNs::wr::com_ptr<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative> nativeDeviceWrapper =
        canvasDevice.as<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>();
    Win2DViewNs::wr::com_ptr<ID2D1Device2> d2dDevice{ nullptr };
    Win2DViewNs::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(
        nullptr, 0.0f, Win2DViewNs::wr::guid_of<ID2D1Device2>(), d2dDevice.put_void()));
    Win2DViewNs::wr::check_hresult(graphicsDeviceInterop->SetRenderingDevice(d2dDevice.get()));

    drawingSurface = nullptr;
    svgLayerBitmap = nullptr;
    sceneBitmap = nullptr;
    trailBitmap = nullptr;
    effectsTextBitmap = nullptr;
    svgLayerDirty = true;
    if (svgDocument != nullptr)
    {
        svgDocument.Close();
        svgDocument = nullptr;
    }

    inDeviceLost = false;
    ScenarioWin2D(compositor, root, currentDpi, width, height);
}

void CWin2DView::ScenarioWin2D(Win2DViewNs::wuc::Compositor const& compositor,
                               Win2DViewNs::wuc::ContainerVisual const& root,
                               UINT dpi,
                               int cx,
                               int cy)
{
    if (inDeviceLost || cx <= 0 || cy <= 0)
    {
        StopRenderTimer();
        return;
    }

    if (width != cx || height != cy || drawingSurface == nullptr)
    {
        StopRenderTimer();
        width = cx;
        height = cy;

        try
        {
            if (contentVisual == nullptr)
            {
                contentVisual = compositor.CreateSpriteVisual();
                contentVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
                root.Children().InsertAtTop(contentVisual);
            }

            if (drawingSurface != nullptr)
            {
                drawingSurface.Resize(Win2DViewNs::wg::SizeInt32{ width, height });
                contentVisual.Brush(compositor.CreateSurfaceBrush(drawingSurface));
            }

            if (drawingSurface == nullptr)
            {
                canvasDevice = Win2DViewNs::mgc::CanvasDevice::GetSharedDevice();
                auto dxgiDevice = Win2DViewInternal::GetDXGIDevice(canvasDevice);
                deviceLostHelper.WatchDevice(dxgiDevice);
                deviceLostHelper.DeviceLost({ this, &CWin2DView::OnDirect3DDeviceLost });

                if (graphicsDevice == nullptr)
                {
                    graphicsDevice =
                        Win2DViewNs::mgcu::CanvasComposition::CreateCompositionGraphicsDevice(compositor, canvasDevice);
                }

                drawingSurface = graphicsDevice.CreateDrawingSurface(
                    Win2DViewNs::wf::Size(static_cast<float>(width), static_cast<float>(height)),
                    Win2DViewNs::wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    Win2DViewNs::wgd::DirectXAlphaMode::Premultiplied);

                contentVisual.Brush(compositor.CreateSurfaceBrush(drawingSurface));
            }

            sceneBitmap = nullptr;
            trailBitmap = nullptr;
            effectsTextBitmap = nullptr;
            svgLayerBitmap = nullptr;
            svgLayerDirty = true;
            CreateFlameEffect();
            displayText.clear();

            Redraw(width / 4.0f,
                   height / 4.0f,
                   300.0f,
                   300.0f,
                   static_cast<float>(width),
                   static_cast<float>(height),
                   dpi);
        }
        catch (Win2DViewNs::wr::hresult_error const&)
        {
            StopRenderTimer();
        }
    }
    (void)dpi;
    TryStartRenderTimer();
}

bool CWin2DView::Redraw(float cx, float cy, float wx, float wy, float width, float height, UINT dpi, CRect* clipRect)
{
    (void)cx;
    (void)cy;
    (void)wx;
    (void)wy;
    (void)dpi;

    if (drawingSurface == nullptr)
    {
        return false;
    }

    try
    {
        auto clearColor = Win2DViewNs::wui::Colors::Black();
        clearColor.A = 0;

        auto renderEffectsFrame =
            [&](Win2DViewNs::mgc::ICanvasResourceCreator const& resourceCreator, float sceneWidth, float sceneHeight) {
            bool newBitmap = false;
            bool recreateBitmap = sceneBitmap == nullptr;
            if (!recreateBitmap)
            {
                const auto size = sceneBitmap.Size();
                if (std::fabs(size.Width - sceneWidth) > 0.5f || std::fabs(size.Height - sceneHeight) > 0.5f)
                {
                    recreateBitmap = true;
                    sceneBitmap = nullptr;
                }
            }
            if (recreateBitmap)
            {
                sceneBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight, Win2DViewInternal::kPixelsDpi);
                trailBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight, Win2DViewInternal::kPixelsDpi);
                // Reuse a persistent text render target instead of creating a
                // full-size temporary bitmap every frame during animation.
                effectsTextBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight, Win2DViewInternal::kPixelsDpi);
                newBitmap = true;
            }
            else if (trailBitmap == nullptr)
            {
                trailBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight, Win2DViewInternal::kPixelsDpi);
                newBitmap = true;
            }
            else if (effectsTextBitmap == nullptr)
            {
                effectsTextBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight, Win2DViewInternal::kPixelsDpi);
            }

            auto drawingSession = sceneBitmap.CreateDrawingSession();
            drawingSession.Clear(clearColor);
            if (!newBitmap && trailBitmap != nullptr)
            {
                Win2DViewNs::mgce::OpacityEffect fadedTrail;
                fadedTrail.Source(trailBitmap);
                fadedTrail.Opacity(0.90f);
                drawingSession.DrawImage(fadedTrail);
            }

            const float blockW = std::min(300.0f, sceneWidth * 0.35f);
            const float blockH = std::min(300.0f, sceneHeight * 0.35f);
            const float blockX = sceneWidth * 0.18f;
            const float blockY = sceneHeight * 0.20f;

            Win2DViewNs::wfn::float2 center(sceneWidth * 0.5f, sceneHeight * 0.5f);
            drawingSession.Transform(Win2DViewNs::wfn::make_float3x2_rotation(
                                         static_cast<float>(angle * Win2DViewInternal::kPi / 180.0), center) *
                                     drawingSession.Transform());

            drawingSession.FillRectangle(Win2DViewNs::wf::Rect{ blockX, blockY, blockW, blockH },
                                         Win2DViewNs::wui::Colors::Red());
            drawingSession.FillRectangle(Win2DViewNs::wf::Rect{ blockX + blockW, blockY + blockH, blockW, blockH },
                                         Win2DViewNs::wui::Colors::Green());

            Win2DViewNs::mgct::CanvasTextFormat textFormat;
            textFormat.FontSize(angle / 2 + 1);

            const Win2DViewNs::wr::hstring message{ L"Hello Win2D in WTL!" };
            const Win2DViewNs::wf::Rect textRect{ 0, 0, sceneWidth, sceneHeight };
            auto textSession = effectsTextBitmap.CreateDrawingSession();
            auto textClear = Win2DViewNs::wui::Colors::Black();
            textClear.A = 0;
            textSession.Clear(textClear);
            textSession.DrawText(message, textRect, Win2DViewNs::wui::Colors::Blue(), textFormat);
            textSession.Close();

            Win2DViewNs::mgce::GaussianBlurEffect blur;
            blur.BlurAmount(5);
            blur.Source(effectsTextBitmap);
            drawingSession.DrawImage(blur);

            const auto newFontSize = Win2DViewInternal::GetFontSize(sceneWidth);
            if (pendingText != displayText || newFontSize != fontSize)
            {
                displayText = pendingText;
                fontSize = newFontSize;
                SetupText(resourceCreator);
            }

            ConfigureEffect();
            drawingSession.DrawImage(compositeEffect, Win2DViewNs::wfn::float2(sceneWidth / 2.0f, sceneHeight / 2.0f));
            drawingSession.Close();

            if (trailBitmap != nullptr)
            {
                auto trailSession = trailBitmap.CreateDrawingSession();
                trailSession.Clear(clearColor);
                trailSession.DrawImage(sceneBitmap);
                trailSession.Close();
            }
        };

        const bool hasSvg = (svgDocument != nullptr);
        const bool drawSvg = hasSvg && renderLayerMode != RenderLayerMode::EffectsOnly;
        const bool drawEffects = renderLayerMode != RenderLayerMode::SvgOnly;
        const float viewportWidth = std::max(1.0f, width);
        const float viewportHeight = std::max(1.0f, height);

        auto viewTransform = Win2DViewInternal::IdentityTransform();
        if (hasSvg)
        {
            viewTransform = transformMatrix;
        }

        // Partial update rectangles are fragile when a full-screen effect layer
        // is present. Keep partial rendering only for static SVG-only frames.
        const bool usePartialUpdate =
            (clipRect != nullptr && GetRenderUpdatePolicy() == RenderUpdatePolicy::StaticOptimized && !drawEffects);

        // The composition surface is allocated with integral pixel dimensions from
        // CWin2DView::width/height. Build update rectangles from those exact
        // dimensions to avoid float-to-int drift between caller parameters and
        // actual surface size.
        const int surfacePixelWidth = std::max(1, this->width);
        const int surfacePixelHeight = std::max(1, this->height);

        CRect effectiveClipRect(0, 0, surfacePixelWidth, surfacePixelHeight);
        if (usePartialUpdate)
        {
            CRect requestedClip(*clipRect);
            requestedClip.NormalizeRect();
            CRect surfaceRect(0, 0, surfacePixelWidth, surfacePixelHeight);
            // CanvasComposition rejects invalid update rectangles with
            // E_INVALIDARG. Normalize and clamp to surface bounds before
            // calling CreateDrawingSessionWithUpdateRect.
            if (!effectiveClipRect.IntersectRect(requestedClip, surfaceRect) || effectiveClipRect.IsRectEmpty())
            {
                // Empty or out-of-bounds update rectangles can trigger E_INVALIDARG in
                // CanvasComposition::CreateDrawingSessionWithUpdateRect.
                return true;
            }
        }

        Win2DViewNs::mgc::CanvasDrawingSession session{ nullptr };
        if (usePartialUpdate)
        {
            Win2DViewNs::wf::Rect updateRect(static_cast<float>(effectiveClipRect.left),
                                             static_cast<float>(effectiveClipRect.top),
                                             static_cast<float>(effectiveClipRect.Width()),
                                             static_cast<float>(effectiveClipRect.Height()));
            session = Win2DViewNs::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface, updateRect);
        }
        else
        {
            // Full-frame rendering should not go through update-rect sessions.
            // In practice, Win2D projection may surface first-chance E_INVALIDARG
            // from CreateDrawingSessionWithUpdateRect during transition timing.
            // Using the full-surface overload removes that failure mode.
            session = Win2DViewNs::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface);
        }

        session.Antialiasing(Win2DViewNs::mgc::CanvasAntialiasing::Antialiased);

        Win2DViewNs::wr::com_ptr<ID2D1RenderTarget> target{ nullptr };
        if (usePartialUpdate)
        {
            Win2DViewNs::wr::com_ptr<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>
                nativeDeviceWrapper = session.as<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>();
            Win2DViewNs::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(
                nullptr, 0.0f, Win2DViewNs::wr::guid_of<ID2D1RenderTarget>(), target.put_void()));

            D2D1_RECT_F clip{};
            clip.left = 0;
            clip.top = 0;
            clip.right = static_cast<float>(effectiveClipRect.Width());
            clip.bottom = static_cast<float>(effectiveClipRect.Height());
            target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        }

        auto transparent = Win2DViewNs::wui::Colors::Black();
        transparent.A = 0;
        session.Clear(transparent);

        auto resourceCreator = session.as<Win2DViewNs::mgc::ICanvasResourceCreator>();

        if (drawEffects)
        {
            renderEffectsFrame(resourceCreator, viewportWidth, viewportHeight);
        }
        else
        {
            sceneBitmap = nullptr;
            trailBitmap = nullptr;
            effectsTextBitmap = nullptr;
        }

        if (drawSvg)
        {
            bool recreateSvgLayer = (svgLayerBitmap == nullptr);
            if (!recreateSvgLayer)
            {
                const auto size = svgLayerBitmap.Size();
                recreateSvgLayer =
                    (std::fabs(size.Width - viewportWidth) > 0.5f || std::fabs(size.Height - viewportHeight) > 0.5f);
            }

            if (recreateSvgLayer)
            {
                svgLayerDirty = true;
            }

            if (svgLayerDirty)
            {
                if (!RenderSvgLayer(resourceCreator, viewportWidth, viewportHeight, viewTransform))
                {
                    return false;
                }
            }
        }
        else
        {
            svgLayerBitmap = nullptr;
            svgLayerDirty = false;
        }

        auto outputTransform = Win2DViewInternal::IdentityTransform();
        if (usePartialUpdate)
        {
            // Update-rect drawing sessions use a local origin at the update
            // rectangle. Shift full-surface images so the clipped region maps
            // to the correct global location on the composition surface.
            outputTransform.m31 = -static_cast<float>(effectiveClipRect.left);
            outputTransform.m32 = -static_cast<float>(effectiveClipRect.top);
        }
        session.Transform(outputTransform);

        if (drawEffects && renderLayerMode != RenderLayerMode::EffectsOverSvg)
        {
            session.DrawImage(sceneBitmap);
        }

        if (drawSvg && svgLayerBitmap != nullptr)
        {
            session.DrawImage(svgLayerBitmap);
        }

        if (drawEffects && renderLayerMode == RenderLayerMode::EffectsOverSvg)
        {
            session.DrawImage(sceneBitmap);
        }

        if (usePartialUpdate)
        {
            target->PopAxisAlignedClip();
        }

        session.Close();
    }
    catch (Win2DViewNs::wr::hresult_error const&)
    {
        return false;
    }

    return true;
}

void CWin2DView::CreateFlameEffect()
{
    morphologyEffect = Win2DViewNs::mgce::MorphologyEffect();
    morphologyEffect.Mode(Win2DViewNs::mgce::MorphologyEffectMode::Dilate);
    morphologyEffect.Width(7);
    morphologyEffect.Height(1);

    auto blur = Win2DViewNs::mgce::GaussianBlurEffect();
    blur.Source(morphologyEffect);
    blur.BlurAmount(3.0f);

    Win2DViewNs::mgce::Matrix5x4 colorMatrix{};
    colorMatrix.M42 = 1.0f;
    colorMatrix.M44 = 1.0f;
    colorMatrix.M51 = 1.0f;
    colorMatrix.M52 = -0.5f;

    auto colorize = Win2DViewNs::mgce::ColorMatrixEffect();
    colorize.Source(blur);
    colorize.ColorMatrix(colorMatrix);

    Win2DViewNs::mgce::TurbulenceEffect turbulence;
    turbulence.Frequency(Win2DViewNs::wfn::float2(0.109f, 0.109f));
    turbulence.Size(Win2DViewNs::wfn::float2(500.0f, 80.0f));

    Win2DViewNs::mgce::BorderEffect border;
    border.Source(turbulence);
    border.ExtendX(Win2DViewNs::mgc::CanvasEdgeBehavior::Mirror);
    border.ExtendY(Win2DViewNs::mgc::CanvasEdgeBehavior::Mirror);

    flameAnimation = Win2DViewNs::mgce::Transform2DEffect();
    flameAnimation.Source(border);

    Win2DViewNs::mgce::DisplacementMapEffect displacement;
    displacement.Source(colorize);
    displacement.Displacement(flameAnimation);
    displacement.Amount(40.0f);

    flamePosition = Win2DViewNs::mgce::Transform2DEffect();
    flamePosition.Source(displacement);

    compositeEffect = Win2DViewNs::mgce::CompositeEffect();
    compositeEffect.Sources().Append(flamePosition);
    compositeEffect.Sources().Append(nullptr);
}

void CWin2DView::SetupText(Win2DViewNs::mgc::ICanvasResourceCreator resourceCreator)
{
    Win2DViewNs::mgc::CanvasCommandList textCommandList(resourceCreator);
    auto drawingSession = textCommandList.CreateDrawingSession();
    drawingSession.Clear(Win2DViewNs::wui::Color{ 0, 0, 0, 0 });

    Win2DViewNs::mgct::CanvasTextFormat textFormat;
    textFormat.FontFamily(L"Segoe UI");
    textFormat.FontSize(fontSize);
    textFormat.HorizontalAlignment(Win2DViewNs::mgct::CanvasHorizontalAlignment::Center);
    textFormat.VerticalAlignment(Win2DViewNs::mgct::CanvasVerticalAlignment::Top);

    drawingSession.DrawText(
        Win2DViewNs::wr::to_hstring(displayText), 0, 0, Win2DViewNs::wui::Colors::White(), textFormat);
    drawingSession.Close();

    morphologyEffect.Source(textCommandList);
    compositeEffect.Sources().SetAt(1, textCommandList);
}

void CWin2DView::ConfigureEffect()
{
    flameAnimation.TransformMatrix(
        Win2DViewNs::wfn::make_float3x2_translation(0, -((60.0f * static_cast<float>(::clock())) / CLOCKS_PER_SEC)));
    const float verticalOffset = fontSize * 1.4f;
    flamePosition.TransformMatrix(
        Win2DViewNs::wfn::make_float3x2_scale(1, 2, Win2DViewNs::wfn::float2(0, verticalOffset)));
}

void CWin2DView::DrawClientRect(RECT& rect)
{
    if (GetRenderUpdatePolicy() == RenderUpdatePolicy::DynamicFullFrame)
    {
        QueueRenderTick();
        return;
    }

    CRect clip(rect);
    if (!clip.IsRectEmpty() && width > 0 && height > 0)
    {
        // Static SVG redraw prefers full-frame update to avoid edge artifacts
        // from incremental stripe repaint during scroll and zoom operations.
        Redraw(width / 4.0f,
               height / 4.0f,
               300.0f,
               300.0f,
               static_cast<float>(width),
               static_cast<float>(height),
               currentDpi);
    }
}

void CWin2DView::OnScrollPositionChanging(CPoint /*oldPos*/, CPoint newPos)
{
    transformMatrix.m31 = static_cast<float>(-newPos.x);
    transformMatrix.m32 = static_cast<float>(-newPos.y);
    if (svgDocument != nullptr)
    {
        MarkSvgLayerDirty();
    }

    if (GetRenderUpdatePolicy() == RenderUpdatePolicy::DynamicFullFrame)
    {
        QueueRenderTick();
    }
}

LRESULT CWin2DView::OnCreate(UINT, WPARAM, LPARAM, BOOL&)
{
    AttachScrollWindow(m_hWnd);
    PrepareVisuals(compositor);

    currentDpi = GetDpiForWindow(m_hWnd);
    ppmBitmapResolution = (72 * currentDpi) / 96;
    pendingText = "Win2D in win32 desktop C++ with WTL";

    CreateFlameEffect();
    return 0;
}

LRESULT CWin2DView::OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
{
    StopRenderTimer();
    KillTimer(Win2DViewInternal::kInertiaTimerId);
    drawingSurface = nullptr;
    svgLayerBitmap = nullptr;
    sceneBitmap = nullptr;
    trailBitmap = nullptr;
    effectsTextBitmap = nullptr;
    svgLayerDirty = true;
    if (svgDocument != nullptr)
    {
        svgDocument.Close();
        svgDocument = nullptr;
    }
    consoleDebugEnabled = false;
    Win2DViewInternal::ReleaseDebugConsole();
    return 0;
}

LRESULT CWin2DView::OnSize(UINT, WPARAM wParam, LPARAM lParam, BOOL&)
{
    const UINT sizeType = static_cast<UINT>(wParam);
    if (sizeType == SIZE_MINIMIZED)
    {
        StopRenderTimer();
        return 0;
    }

    currentDpi = GetDpiForWindow(m_hWnd);

    const int cx = std::max(1, GET_X_LPARAM(lParam));
    const int cy = std::max(1, GET_Y_LPARAM(lParam));
    UpdateScrollMetrics();

    if (svgDocument == nullptr)
    {
        SetScrollSizes(MM_TEXT, CSize(cx, cy));
    }

    if (cx > 0 && cy > 0)
    {
        ScenarioWin2D(compositor, root, currentDpi, cx, cy);
    }

    return 0;
}

LRESULT CWin2DView::OnPaint(UINT, WPARAM, LPARAM, BOOL&)
{
    PAINTSTRUCT ps{};
    BeginPaint(&ps);
    EndPaint(&ps);

    // WM_PAINT may carry an empty region. Skip redraw to avoid constructing
    // a zero-area update rect for Win2D composition APIs.
    if (ps.rcPaint.right <= ps.rcPaint.left || ps.rcPaint.bottom <= ps.rcPaint.top)
    {
        return 0;
    }

    if (width > 0 && height > 0 && drawingSurface != nullptr)
    {
        CRect clip(ps.rcPaint);
        Redraw(width / 4.0f,
               height / 4.0f,
               300.0f,
               300.0f,
               static_cast<float>(width),
               static_cast<float>(height),
               currentDpi,
               &clip);
    }

    return 0;
}

LRESULT CWin2DView::OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&)
{
    return 1;
}

LRESULT CWin2DView::OnRenderTick(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    renderTickQueued.store(false, std::memory_order_release);

    const HWND rootWindow = ::GetAncestor(m_hWnd, GA_ROOT);
    if (rootWindow != nullptr && ::IsIconic(rootWindow))
    {
        return 0;
    }

    if (!ShouldAnimateEffects() || drawingSurface == nullptr || width <= 0 || height <= 0)
    {
        lastRenderTickTime = std::chrono::steady_clock::time_point{};
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    double deltaSeconds = 1.0 / 60.0;
    if (lastRenderTickTime.time_since_epoch().count() != 0)
    {
        deltaSeconds = std::chrono::duration<double>(now - lastRenderTickTime).count();
        deltaSeconds = std::clamp(deltaSeconds, 0.0, 0.05);
    }
    lastRenderTickTime = now;

    const UINT dpi = wParam != 0 ? static_cast<UINT>(wParam) : static_cast<UINT>(currentDpi);
    if (Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f, static_cast<float>(width), static_cast<float>(height), dpi))
    {
        angle += static_cast<float>(60.0 * deltaSeconds);
    }

    return 0;
}
