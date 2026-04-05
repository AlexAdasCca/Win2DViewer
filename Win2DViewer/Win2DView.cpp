#include "pch.h"

#include "Win2DViewInternal.h"

CWin2DView::CWin2DView() noexcept
{
    ppmBitmapResolution = 72;
    transformMatrix = Win2DViewInternal::IdentityTransform();
    Win2DViewInternal::gDisplaySyncHelper.Initialize();
    displayFrequency = Win2DViewInternal::gDisplaySyncHelper.GetFrequency();
}

CWin2DView::~CWin2DView()
{
    renderTimer.stop();
}

BOOL CWin2DView::PreTranslateMessage(MSG* /*pMsg*/)
{
    return FALSE;
}

void CWin2DView::SetDocument(CSvgDocument* newDocument) noexcept
{
    document = newDocument;
}

void CWin2DView::RefreshDocument()
{
    if (!IsWindow())
    {
        return;
    }

    renderTimer.stop();
    renderTickQueued.store(false, std::memory_order_release);
    KillTimer(Win2DViewInternal::kInertiaTimerId);

    RECT clientRect{};
    GetClientRect(&clientRect);
    const int clientWidth = std::max(1, static_cast<int>(clientRect.right - clientRect.left));
    const int clientHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));

    if (document == nullptr || document->Empty())
    {
        if (svgDocument != nullptr)
        {
            svgDocument.Close();
            svgDocument = nullptr;
        }

        ScenarioWin2D(compositor, root, currentDpi, clientWidth, clientHeight);
        SetScrollSizes(MM_TEXT, CSize(clientWidth, clientHeight));
        transformMatrix = Win2DViewInternal::IdentityTransform();
        ScrollToPosition(CPoint(0, 0));
        Invalidate();
        return;
    }

    ScenarioWin2D(compositor, root, currentDpi, clientWidth, clientHeight);

    if (svgDocument != nullptr)
    {
        svgDocument.Close();
        svgDocument = nullptr;
    }

    if (LoadSvg())
    {
        Redraw(clientWidth / 4.0f,
               clientHeight / 4.0f,
               300.0f,
               300.0f,
               static_cast<float>(clientWidth),
               static_cast<float>(clientHeight),
               currentDpi);
    }
}

void CWin2DView::SetRenderLayerMode(RenderLayerMode mode)
{
    if (renderLayerMode == mode)
    {
        return;
    }

    renderLayerMode = mode;
    if (IsWindow())
    {
        ScenarioWin2D(compositor, root, currentDpi, width, height);
        Invalidate();
    }
}

bool CWin2DView::ShouldAnimateEffects() const noexcept
{
    return renderLayerMode != RenderLayerMode::SvgOnly;
}
