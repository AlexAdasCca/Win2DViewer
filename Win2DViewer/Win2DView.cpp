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
    StopRenderTimer();
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

    StopRenderTimer();
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
        // Reset retained SVG state eagerly when the document is cleared.
        // Otherwise large XML and parsed text overlays can survive until the
        // next load and look like a leak in process memory snapshots.
        svgXml.clear();
        svgDocumentWidth = 0.0f;
        svgDocumentHeight = 0.0f;
        svgTextOverlays.clear();
        svgTextOverlays.shrink_to_fit();
        svgLayerBitmap = nullptr;
        svgLayerDirty = true;
        UpdateScrollBarVisibilityPolicy();

        ScenarioWin2D(compositor, root, currentDpi, clientWidth, clientHeight);
        SetScrollSizes(MM_TEXT, CSize(clientWidth, clientHeight));
        transformMatrix = Win2DViewInternal::IdentityTransform();
        ScrollToPosition(CPoint(0, 0));
        if (drawingSurface != nullptr)
        {
            // Force a full clear pass immediately so stale content is removed
            // even if the next WM_PAINT update region is partial.
            (void)Redraw(clientWidth / 4.0f,
                         clientHeight / 4.0f,
                         300.0f,
                         300.0f,
                         static_cast<float>(clientWidth),
                         static_cast<float>(clientHeight),
                         currentDpi);
        }
        Invalidate();
        return;
    }

    ScenarioWin2D(compositor, root, currentDpi, clientWidth, clientHeight);

    if (svgDocument != nullptr)
    {
        svgDocument.Close();
        svgDocument = nullptr;
    }
    svgLayerBitmap = nullptr;
    svgLayerDirty = true;

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
    UpdateScrollBarVisibilityPolicy();

    if (renderLayerMode == RenderLayerMode::SvgOnly)
    {
        // SvgOnly does not consume effect history. Release effect caches
        // immediately to avoid stale visuals and unnecessary memory retention.
        StopRenderTimer();
        sceneBitmap = nullptr;
        trailBitmap = nullptr;
        effectsTextBitmap = nullptr;
        lastRenderTickTime = std::chrono::steady_clock::time_point{};
    }

    if (IsWindow())
    {
        ScenarioWin2D(compositor, root, currentDpi, width, height);
        if (drawingSurface != nullptr && width > 0 && height > 0)
        {
            (void)Redraw(width / 4.0f,
                         height / 4.0f,
                         300.0f,
                         300.0f,
                         static_cast<float>(width),
                         static_cast<float>(height),
                         currentDpi);
        }
        Invalidate();
    }
}

void CWin2DView::UpdateScrollBarVisibilityPolicy()
{
    const bool hasLoadedSvg = (svgDocument != nullptr && svgDocumentWidth > 0.0f && svgDocumentHeight > 0.0f);
    const bool effectAndSvgVisible = hasLoadedSvg && (renderLayerMode == RenderLayerMode::EffectsOverSvg ||
                                                      renderLayerMode == RenderLayerMode::SvgOverEffects);

    SetKeepScrollBarsVisible(effectAndSvgVisible);
    if (IsWindow())
    {
        UpdateScrollMetrics();
    }
}

bool CWin2DView::ShouldAnimateEffects() const noexcept
{
    return renderLayerMode != RenderLayerMode::SvgOnly;
}

CWin2DView::RenderUpdatePolicy CWin2DView::GetRenderUpdatePolicy() const noexcept
{
    return ShouldAnimateEffects() ? RenderUpdatePolicy::DynamicFullFrame : RenderUpdatePolicy::StaticOptimized;
}
