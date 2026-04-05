#include "pch.h"

#include "DesktopCompositionInteropInternal.h"

void DesktopInteropInternal::DesktopHostWindow::InitializeWinRTComposition()
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
    compositionGraphicsDevice =
        DesktopInteropNs::mgcu::CanvasComposition::CreateCompositionGraphicsDevice(compositor, canvasDevice);

    ResizeWinRTSurface();

    surfaceBrush = compositor.CreateSurfaceBrush(drawingSurface);
    surfaceBrush.Stretch(DesktopInteropNs::wuc::CompositionStretch::Fill);

    surfaceVisual = compositor.CreateSpriteVisual();
    surfaceVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
    surfaceVisual.Brush(surfaceBrush);
    rootVisual.Children().InsertAtTop(surfaceVisual);
}

void DesktopInteropInternal::DesktopHostWindow::EnableHostBackdropForWindow() const
{
    const BOOL enabled = TRUE;
    (void)::DwmSetWindowAttribute(windowHandle, kDwmAttrUseHostBackdropBrush, &enabled, sizeof(enabled));
}

void DesktopInteropInternal::DesktopHostWindow::ResizeWinRTSurface()
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

void DesktopInteropInternal::DesktopHostWindow::DrawWinRTSurface()
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
    const DesktopInteropNs::wfn::float2 velocityTip{ ballPosition.x + ballVelocity.x * velocityScale,
                                                     ballPosition.y + ballVelocity.y * velocityScale };
    drawingSession.DrawLine(ballPosition, velocityTip, velocityColor, 3.0f);
    drawingSession.DrawCircle(ballPosition, radius + 6.0f, { 0x80, 0xFF, 0xD7, 0x8C }, 2.0f);

    drawingSession.FillRoundedRectangle({ 24.0f, height - 110.0f, width - 48.0f, 76.0f }, 12.0f, 12.0f, panelColor);
    drawingSession.DrawRoundedRectangle(
        { 24.0f, height - 110.0f, width - 48.0f, 76.0f }, 12.0f, 12.0f, panelStroke, 1.5f);

    const std::wstring title = (backend == DesktopInterop::DesktopHostBackend::WinRTHostBackdrop) ?
                                   L"WinRT Host Backdrop Material - Physics Test" :
                                   L"WinRT Composition Surface - Physics Test";
    drawingSession.DrawText(title, { 30.0f, 30.0f }, textColor);

    wchar_t infoLine[256]{};
    _snwprintf_s(infoLine,
                 _TRUNCATE,
                 L"Ball(%.1f, %.1f)  Velocity(%.1f, %.1f)  Radius %.1f",
                 ballPosition.x,
                 ballPosition.y,
                 ballVelocity.x,
                 ballVelocity.y,
                 ballRadius);
    drawingSession.DrawText(infoLine, { 36.0f, height - 88.0f }, textColor);
}

void DesktopInteropInternal::DesktopHostWindow::UpdateWinRTPhysics(float deltaSeconds)
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

void DesktopInteropInternal::DesktopHostWindow::ClampWinRTBallPosition()
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
