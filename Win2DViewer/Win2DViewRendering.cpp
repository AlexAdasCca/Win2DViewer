#include "pch.h"

#include "Win2DViewInternal.h"

Win2DViewNs::wud::DesktopWindowTarget CWin2DView::CreateDesktopWindowTarget(
    Win2DViewNs::wuc::Compositor const& compositor, HWND window)
{
    namespace abi = ABI::Windows::UI::Composition::Desktop;

    auto interop = compositor.as<abi::ICompositorDesktopInterop>();
    Win2DViewNs::wud::DesktopWindowTarget target{nullptr};
    Win2DViewNs::wr::check_hresult(interop->CreateDesktopWindowTarget(
        window, true,
        reinterpret_cast<abi::IDesktopWindowTarget**>(
            Win2DViewNs::wr::put_abi(target))));
    return target;
}

void CWin2DView::PrepareVisuals(
    Win2DViewNs::wuc::Compositor const& compositor)
{
    target = CreateDesktopWindowTarget(compositor, m_hWnd);

    root = compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({1.0f, 1.0f});

    contentVisual = compositor.CreateSpriteVisual();
    contentVisual.RelativeSizeAdjustment({1.0f, 1.0f});

    root.Children().InsertAtTop(contentVisual);

    target.Root(root);
}

void CWin2DView::OnDirect3DDeviceLost(DeviceLostHelper const*,
                                      DeviceLostEventArgs const&)
{
    inDeviceLost = true;
    renderTimer.stop();

    auto canvasDevice = Win2DViewNs::mgc::CanvasDevice::GetSharedDevice();
    Win2DViewNs::wr::com_ptr<
        ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>
        graphicsDeviceInterop{graphicsDevice.as<
            ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>()};

    Win2DViewNs::wr::com_ptr<
        ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>
        nativeDeviceWrapper = canvasDevice.as<
            ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>();
    Win2DViewNs::wr::com_ptr<ID2D1Device2> d2dDevice{nullptr};
    Win2DViewNs::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(
        nullptr, 0.0f, Win2DViewNs::wr::guid_of<ID2D1Device2>(),
        d2dDevice.put_void()));
    Win2DViewNs::wr::check_hresult(
        graphicsDeviceInterop->SetRenderingDevice(d2dDevice.get()));

    drawingSurface = nullptr;
    sceneBitmap = nullptr;
    trailBitmap = nullptr;
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
                               UINT dpi, int cx, int cy)
{
    if (inDeviceLost || cx <= 0 || cy <= 0)
    {
        return;
    }

    if (width != cx || height != cy || drawingSurface == nullptr)
    {
        renderTimer.stop();
        width = cx;
        height = cy;

        try
        {
            if (contentVisual == nullptr)
            {
                contentVisual = compositor.CreateSpriteVisual();
                contentVisual.RelativeSizeAdjustment({1.0f, 1.0f});
                root.Children().InsertAtTop(contentVisual);
            }

            if (drawingSurface != nullptr)
            {
                drawingSurface.Resize(Win2DViewNs::wg::SizeInt32{width, height});
                contentVisual.Brush(compositor.CreateSurfaceBrush(drawingSurface));
            }

            if (drawingSurface == nullptr)
            {
                canvasDevice = Win2DViewNs::mgc::CanvasDevice::GetSharedDevice();
                auto dxgiDevice = Win2DViewInternal::GetDXGIDevice(canvasDevice);
                deviceLostHelper.WatchDevice(dxgiDevice);
                deviceLostHelper.DeviceLost({this, &CWin2DView::OnDirect3DDeviceLost});

                if (graphicsDevice == nullptr)
                {
                    graphicsDevice = Win2DViewNs::mgcu::CanvasComposition::
                        CreateCompositionGraphicsDevice(compositor, canvasDevice);
                }

                drawingSurface = graphicsDevice.CreateDrawingSurface(
                    Win2DViewNs::wf::Size(static_cast<float>(width),
                                          static_cast<float>(height)),
                    Win2DViewNs::wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    Win2DViewNs::wgd::DirectXAlphaMode::Premultiplied);

                contentVisual.Brush(compositor.CreateSurfaceBrush(drawingSurface));
            }

            sceneBitmap = nullptr;
            trailBitmap = nullptr;
            CreateFlameEffect();
            displayText.clear();

            Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f,
                   static_cast<float>(width), static_cast<float>(height), dpi);
        }
        catch (Win2DViewNs::wr::hresult_error const&)
        {
            renderTimer.stop();
        }
    }

    if (ShouldAnimateEffects())
    {
        renderTimer.start(1000.0 / 60.0, [this, dpi]()
        {
            if (!::IsWindow(m_hWnd))
            {
                return false;
            }
            if (!renderTickQueued.exchange(true, std::memory_order_acq_rel))
            {
                ::PostMessage(m_hWnd, Win2DViewInternal::kRenderTickMsg,
                              static_cast<WPARAM>(dpi), 0);
            }
            return true;
        });
    }
    else
    {
        renderTimer.stop();
    }
}

bool CWin2DView::Redraw(float cx, float cy, float wx, float wy, float width,
                        float height, UINT dpi, CRect* clipRect)
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
            [&](Win2DViewNs::mgc::ICanvasResourceCreator const& resourceCreator,
                float sceneWidth, float sceneHeight)
        {
            bool newBitmap = false;
            bool recreateBitmap = sceneBitmap == nullptr;
            if (!recreateBitmap)
            {
                const auto size = sceneBitmap.Size();
                if (std::fabs(size.Width - sceneWidth) > 0.5f ||
                    std::fabs(size.Height - sceneHeight) > 0.5f)
                {
                    recreateBitmap = true;
                    sceneBitmap = nullptr;
                }
            }
            if (recreateBitmap)
            {
                sceneBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight,
                    Win2DViewInternal::kPixelsDpi);
                trailBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight,
                    Win2DViewInternal::kPixelsDpi);
                newBitmap = true;
            }
            else if (trailBitmap == nullptr)
            {
                trailBitmap = Win2DViewNs::mgc::CanvasRenderTarget(
                    resourceCreator, sceneWidth, sceneHeight,
                    Win2DViewInternal::kPixelsDpi);
                newBitmap = true;
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

            Win2DViewNs::wfn::float2 center(sceneWidth * 0.5f,
                                            sceneHeight * 0.5f);
            drawingSession.Transform(
                Win2DViewNs::wfn::make_float3x2_rotation(
                    static_cast<float>(angle * Win2DViewInternal::kPi / 180.0),
                    center) *
                drawingSession.Transform());

            drawingSession.FillRectangle(
                Win2DViewNs::wf::Rect{blockX, blockY, blockW, blockH},
                Win2DViewNs::wui::Colors::Red());
            drawingSession.FillRectangle(Win2DViewNs::wf::Rect{blockX + blockW,
                                                               blockY + blockH,
                                                               blockW, blockH},
                                         Win2DViewNs::wui::Colors::Green());

            Win2DViewNs::mgct::CanvasTextFormat textFormat;
            textFormat.FontSize(angle / 2 + 1);

            const Win2DViewNs::wr::hstring message{L"Hello Win2D in WTL!"};
            const Win2DViewNs::wf::Rect textRect{0, 0, sceneWidth, sceneHeight};
            Win2DViewNs::mgc::CanvasRenderTarget textBitmap(
                resourceCreator, sceneWidth, sceneHeight,
                Win2DViewInternal::kPixelsDpi);
            auto textSession = textBitmap.CreateDrawingSession();
            auto textClear = Win2DViewNs::wui::Colors::Black();
            textClear.A = 0;
            textSession.Clear(textClear);
            textSession.DrawText(message, textRect,
                                 Win2DViewNs::wui::Colors::Blue(), textFormat);
            textSession.Close();

            Win2DViewNs::mgce::GaussianBlurEffect blur;
            blur.BlurAmount(5);
            blur.Source(textBitmap);
            drawingSession.DrawImage(blur);

            const auto newFontSize = Win2DViewInternal::GetFontSize(sceneWidth);
            if (pendingText != displayText || newFontSize != fontSize)
            {
                displayText = pendingText;
                fontSize = newFontSize;
                SetupText(resourceCreator);
            }

            ConfigureEffect();
            drawingSession.DrawImage(
                compositeEffect,
                Win2DViewNs::wfn::float2(sceneWidth / 2.0f, sceneHeight / 2.0f));
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
        const bool renderEffectsOnly =
            renderLayerMode == RenderLayerMode::EffectsOnly;
        if (document == nullptr || document->Empty() || renderEffectsOnly)
        {
            Win2DViewNs::mgc::CanvasDrawingSession rootSession{nullptr};
            auto outputTransform = Win2DViewInternal::IdentityTransform();
            if (clipRect == nullptr)
            {
                Win2DViewNs::wf::Rect updateRect(0.0f, 0.0f, static_cast<float>(width),
                                                 static_cast<float>(height));
                rootSession =
                    Win2DViewNs::mgcu::CanvasComposition::CreateDrawingSession(
                        drawingSurface, updateRect);
                rootSession.Clear(clearColor);
            }
            else
            {
                Win2DViewNs::wf::Rect updateRect(
                    static_cast<float>(clipRect->left),
                    static_cast<float>(clipRect->top),
                    static_cast<float>(clipRect->Width()),
                    static_cast<float>(clipRect->Height()));
                rootSession =
                    Win2DViewNs::mgcu::CanvasComposition::CreateDrawingSession(
                        drawingSurface, updateRect);
                if (!(hasSvg && renderEffectsOnly))
                {
                    outputTransform.m31 -= static_cast<float>(clipRect->left);
                    outputTransform.m32 -= static_cast<float>(clipRect->top);
                }
            }

            float sceneWidth = std::max(1.0f, width);
            float sceneHeight = std::max(1.0f, height);
            if (!(hasSvg && renderEffectsOnly))
            {
                outputTransform.m31 += transformMatrix.m31;
                outputTransform.m32 += transformMatrix.m32;

                const auto total = GetTotalSize();
                sceneWidth = std::max<float>(static_cast<float>(total.cx), width);
                sceneHeight = std::max<float>(static_cast<float>(total.cy), height);
            }

            auto resourceCreator =
                rootSession.as<Win2DViewNs::mgc::ICanvasResourceCreator>();
            renderEffectsFrame(resourceCreator, sceneWidth, sceneHeight);

            rootSession.Transform(outputTransform);
            rootSession.DrawImage(sceneBitmap);
            rootSession.Close();
            return true;
        }

        if (svgDocument != nullptr)
        {
            Win2DViewNs::mgc::CanvasDrawingSession session = nullptr;
            auto transform = transformMatrix;

            if (clipRect == nullptr)
            {
                Win2DViewNs::wf::Rect updateRect(0.0f, 0.0f, static_cast<float>(width),
                                                 static_cast<float>(height));
                session = Win2DViewNs::mgcu::CanvasComposition::CreateDrawingSession(
                    drawingSurface, updateRect);
            }
            else
            {
                Win2DViewNs::wf::Rect updateRect(
                    static_cast<float>(clipRect->left),
                    static_cast<float>(clipRect->top),
                    static_cast<float>(clipRect->Width()),
                    static_cast<float>(clipRect->Height()));
                session = Win2DViewNs::mgcu::CanvasComposition::CreateDrawingSession(
                    drawingSurface, updateRect);
                transform.m31 -= static_cast<float>(clipRect->left);
                transform.m32 -= static_cast<float>(clipRect->top);
            }

            session.Antialiasing(Win2DViewNs::mgc::CanvasAntialiasing::Antialiased);

            Win2DViewNs::wr::com_ptr<ID2D1RenderTarget> target{nullptr};
            if (clipRect != nullptr)
            {
                Win2DViewNs::wr::com_ptr<
                    ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>
                    nativeDeviceWrapper =
                        session.as<ABI::Microsoft::Graphics::Canvas::
                                       ICanvasResourceWrapperNative>();
                Win2DViewNs::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(
                    nullptr, 0.0f, Win2DViewNs::wr::guid_of<ID2D1RenderTarget>(),
                    target.put_void()));

                D2D1_RECT_F clip{};
                clip.left = 0;
                clip.top = 0;
                clip.right = static_cast<float>(clipRect->Width());
                clip.bottom = static_cast<float>(clipRect->Height());
                target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            }

            auto transparent = Win2DViewNs::wui::Colors::Black();
            transparent.A = 0;
            session.Clear(transparent);

            const bool drawSvg = renderLayerMode != RenderLayerMode::EffectsOnly;
            const bool drawEffects = renderLayerMode != RenderLayerMode::SvgOnly;
            const float effectWidth = std::max(1.0f, width);
            const float effectHeight = std::max(1.0f, height);

            if (drawEffects)
            {
                auto resourceCreator =
                    session.as<Win2DViewNs::mgc::ICanvasResourceCreator>();
                renderEffectsFrame(resourceCreator, effectWidth, effectHeight);
            }

            if (renderLayerMode == RenderLayerMode::SvgOverEffects && drawEffects)
            {
                session.Transform(Win2DViewInternal::IdentityTransform());
                session.DrawImage(sceneBitmap);
            }

            if (drawSvg)
            {
                session.Transform(transform);
                session.DrawSvg(svgDocument,
                                Win2DViewNs::wf::Size(static_cast<float>(width),
                                                      static_cast<float>(height)));
                DrawSvgTextOverlay(session, transform);
            }

            if (renderLayerMode == RenderLayerMode::EffectsOverSvg && drawEffects)
            {
                session.Transform(Win2DViewInternal::IdentityTransform());
                session.DrawImage(sceneBitmap);
            }

            if (clipRect != nullptr)
            {
                target->PopAxisAlignedClip();
            }

            session.Close();
        }
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

void CWin2DView::SetupText(
    Win2DViewNs::mgc::ICanvasResourceCreator resourceCreator)
{
    Win2DViewNs::mgc::CanvasCommandList textCommandList(resourceCreator);
    auto drawingSession = textCommandList.CreateDrawingSession();
    drawingSession.Clear(Win2DViewNs::wui::Color{0, 0, 0, 0});

    Win2DViewNs::mgct::CanvasTextFormat textFormat;
    textFormat.FontFamily(L"Segoe UI");
    textFormat.FontSize(fontSize);
    textFormat.HorizontalAlignment(
        Win2DViewNs::mgct::CanvasHorizontalAlignment::Center);
    textFormat.VerticalAlignment(Win2DViewNs::mgct::CanvasVerticalAlignment::Top);

    drawingSession.DrawText(Win2DViewNs::wr::to_hstring(displayText), 0, 0,
                            Win2DViewNs::wui::Colors::White(), textFormat);
    drawingSession.Close();

    morphologyEffect.Source(textCommandList);
    compositeEffect.Sources().SetAt(1, textCommandList);
}

void CWin2DView::ConfigureEffect()
{
    flameAnimation.TransformMatrix(Win2DViewNs::wfn::make_float3x2_translation(
        0, -((60.0f * static_cast<float>(::clock())) / CLOCKS_PER_SEC)));
    const float verticalOffset = fontSize * 1.4f;
    flamePosition.TransformMatrix(Win2DViewNs::wfn::make_float3x2_scale(
        1, 2, Win2DViewNs::wfn::float2(0, verticalOffset)));
}

void CWin2DView::DrawClientRect(RECT& rect)
{
    if (svgDocument == nullptr)
    {
        return;
    }

    CRect clip(rect);
    if (!clip.IsRectEmpty() && width > 0 && height > 0)
    {
        Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f,
               static_cast<float>(width), static_cast<float>(height), currentDpi,
               &clip);
    }
}

void CWin2DView::OnScrollPositionChanging(CPoint /*oldPos*/, CPoint newPos)
{
    if (svgDocument != nullptr)
    {
        SurfaceScroll(newPos);
    }

    transformMatrix.m31 = static_cast<float>(-newPos.x);
    transformMatrix.m32 = static_cast<float>(-newPos.y);
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
    renderTimer.stop();
    renderTickQueued.store(false, std::memory_order_release);
    KillTimer(Win2DViewInternal::kInertiaTimerId);
    drawingSurface = nullptr;
    sceneBitmap = nullptr;
    trailBitmap = nullptr;
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
        renderTimer.stop();
        renderTickQueued.store(false, std::memory_order_release);
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

    if (svgDocument != nullptr && width > 0 && height > 0 &&
        drawingSurface != nullptr)
    {
        CRect clip(ps.rcPaint);
        Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f,
               static_cast<float>(width), static_cast<float>(height), currentDpi,
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

    if (!ShouldAnimateEffects() || drawingSurface == nullptr || width <= 0 ||
        height <= 0)
    {
        return 0;
    }

    const UINT dpi =
        wParam != 0 ? static_cast<UINT>(wParam) : static_cast<UINT>(currentDpi);
    if (Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f,
               static_cast<float>(width), static_cast<float>(height), dpi))
    {
        angle += 1.0f;
    }

    return 0;
}
