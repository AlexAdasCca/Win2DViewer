#include "pch.h"

#include "Win2DViewInternal.h"

void CWin2DView::SurfaceScroll(CPoint const& newPosition)
{
    const int dx = newPosition.x - static_cast<int>(-transformMatrix.m31);
    const int dy = newPosition.y - static_cast<int>(-transformMatrix.m32);
    if ((dx != 0 || dy != 0) && drawingSurface != nullptr)
    {
        drawingSurface.Scroll(Win2DViewNs::wg::PointInt32{ -dx, -dy });
    }
}

void CWin2DView::Zoom(int zDelta, CPoint screenPoint)
{
    KillTimer(Win2DViewInternal::kInertiaTimerId);

    const int dpi25 = std::max(1, (25 * currentDpi) / 96);
    ppmBitmapResolution -= zDelta / dpi25;
    ppmBitmapResolution = std::clamp(ppmBitmapResolution,
                                     std::max(36, (36 * currentDpi) / 96),
                                     std::max(72, (Win2DViewInternal::kMaxBitmapResolution * currentDpi) / 96));

    if (svgDocument == nullptr || svgDocumentWidth <= 0.0f || svgDocumentHeight <= 0.0f)
    {
        return;
    }

    const float scale = ppmBitmapResolution / 72.0f;
    transformMatrix.m11 = scale;
    transformMatrix.m22 = scale;

    POINT clientPoint = screenPoint;
    ::ScreenToClient(m_hWnd, &clientPoint);

    RECT clientRect{};
    GetClientRect(&clientRect);

    CPoint scrollPosition = GetScrollPosition();
    CPoint anchor = CPoint(clientPoint) + scrollPosition;
    CSize total = GetTotalSize();
    if (total.cx <= 0 || total.cy <= 0)
    {
        return;
    }

    const float fracX = static_cast<float>(anchor.x) / total.cx;
    const float fracY = static_cast<float>(anchor.y) / total.cy;

    CSize scaledTotal(std::clamp(static_cast<int>(svgDocumentWidth * scale), 0, 100000000),
                      std::clamp(static_cast<int>(svgDocumentHeight * scale), 0, 100000000));

    CPoint scaledAnchor(Win2DViewInternal::RoundToInt(fracX * scaledTotal.cx),
                        Win2DViewInternal::RoundToInt(fracY * scaledTotal.cy));

    scrollPosition += (scaledAnchor - anchor);
    if (scaledTotal.cx <= (clientRect.right - clientRect.left))
    {
        scrollPosition.x = 0;
    }
    if (scaledTotal.cy <= (clientRect.bottom - clientRect.top))
    {
        scrollPosition.y = 0;
    }

    SetScrollSizes(MM_TEXT, scaledTotal);
    scrollPosition = ClampScrollPosition(scrollPosition);
    transformMatrix.m31 = static_cast<float>(-scrollPosition.x);
    transformMatrix.m32 = static_cast<float>(-scrollPosition.y);
    ScrollToPosition(scrollPosition);
    Invalidate();
}

LRESULT CWin2DView::OnHScroll(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    HandleHorizontalScroll(LOWORD(wParam), HIWORD(wParam));
    return 0;
}

LRESULT CWin2DView::OnVScroll(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    HandleVerticalScroll(LOWORD(wParam), HIWORD(wParam));
    return 0;
}

LRESULT CWin2DView::OnMouseWheel(UINT, WPARAM wParam, LPARAM lParam, BOOL&)
{
    Zoom((GET_WHEEL_DELTA_WPARAM(wParam) < 0 ? 1 : -1) * ppmBitmapResolution * 25 / 4,
         CPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
    return 0;
}

LRESULT CWin2DView::OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL&)
{
    scrollDiff = CSize(0, 0);
    SetFocus();

    if (GetCapture() != m_hWnd)
    {
        SetCapture();
        translateDragging = true;
        currentMouse = CPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        scrollStartTime = ::clock();
    }

    return 0;
}

LRESULT CWin2DView::OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&)
{
    if (translateDragging)
    {
        ReleaseCapture();
        translateDragging = false;

        if (scrollTimeDiff > 6 && scrollDiff != CSize(0, 0))
        {
            const int milliseconds = static_cast<int>(scrollTimeDiff * 1000 / CLOCKS_PER_SEC);
            const int interval = 1000 / std::max<DWORD>(1, displayFrequency);
            const int factor = std::max<int>(1, static_cast<int>(displayFrequency * milliseconds));

            scrollDiff.cx = (scrollDiff.cx * 1000) / factor;
            scrollDiff.cy = (scrollDiff.cy * 1000) / factor;
            Win2DViewInternal::gDisplaySyncHelper.WaitForVSync();
            SetTimer(Win2DViewInternal::kInertiaTimerId, interval);
        }
    }

    return 0;
}

LRESULT CWin2DView::OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL&)
{
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hWnd, 0 };
    ::TrackMouseEvent(&tme);

    if (!translateDragging)
    {
        return 0;
    }

    const clock_t now = ::clock();
    scrollTimeDiff = static_cast<unsigned int>(now - scrollStartTime);
    scrollStartTime = now;

    const CPoint oldPos = GetScrollPosition();
    CPoint newPos = oldPos;
    const CPoint point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    const CSize delta = point - currentMouse;
    newPos -= delta;

    currentMouse = point;

    BOOL hasHorizontal = FALSE;
    BOOL hasVertical = FALSE;
    CheckScrollBars(hasHorizontal, hasVertical);
    if (!hasHorizontal)
    {
        newPos.x = 0;
    }
    if (!hasVertical)
    {
        newPos.y = 0;
    }

    newPos = ClampScrollPosition(newPos);
    scrollDiff = newPos - oldPos;
    if (scrollDiff != CSize(0, 0))
    {
        ScrollToPosition(newPos);
    }

    return 0;
}

LRESULT CWin2DView::OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&)
{
    if (translateDragging)
    {
        ReleaseCapture();
        translateDragging = false;
    }

    return 0;
}

LRESULT CWin2DView::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    if (wParam != Win2DViewInternal::kInertiaTimerId)
    {
        return 0;
    }

    CPoint oldPos = GetScrollPosition();
    CPoint newPos = oldPos + scrollDiff;
    newPos = ClampScrollPosition(newPos);

    if (Win2DViewInternal::gDampScrolling)
    {
        float dx = static_cast<float>(scrollDiff.cx);
        float dy = static_cast<float>(scrollDiff.cy);

        if (dx != 0.0f)
        {
            dx -= (dx > 0.0f) ? 1.0f : -1.0f;
        }
        if (dy != 0.0f)
        {
            dy -= (dy > 0.0f) ? 1.0f : -1.0f;
        }

        scrollDiff = CSize(static_cast<int>(dx), static_cast<int>(dy));
    }

    Win2DViewInternal::gDisplaySyncHelper.WaitForVSync();
    ScrollToPosition(newPos);

    if (GetScrollPosition() == oldPos || scrollDiff == CSize(0, 0))
    {
        scrollDiff = CSize(0, 0);
        KillTimer(Win2DViewInternal::kInertiaTimerId);
    }

    return 0;
}

LRESULT CWin2DView::OnGesture(UINT, WPARAM wParam, LPARAM lParam, BOOL&)
{
    GESTUREINFO gestureInfo{};
    gestureInfo.cbSize = sizeof(gestureInfo);

    static int currentDistance = 0;
    static bool gestureStart = false;

    BOOL handled = FALSE;
    if (::GetGestureInfo(reinterpret_cast<HGESTUREINFO>(lParam), &gestureInfo))
    {
        switch (gestureInfo.dwID)
        {
            case GID_BEGIN:
                gestureStart = true;
                break;
            case GID_ZOOM:
                if (!gestureStart)
                {
                    const int delta = static_cast<int>(gestureInfo.ullArguments) - currentDistance;
                    if (delta != 0)
                    {
                        const double scaledDelta = (delta < 0 ? 1.0 : -1.0) * static_cast<double>(ppmBitmapResolution) *
                                                   25.0 * (std::abs(delta) / 120.0) / 4.0;
                        Zoom(static_cast<int>(scaledDelta),
                             CPoint(gestureInfo.ptsLocation.x, gestureInfo.ptsLocation.y));
                    }
                }

                gestureStart = false;
                currentDistance = static_cast<int>(gestureInfo.ullArguments);
                handled = TRUE;
                break;
            default:
                break;
        }
    }

    if (handled)
    {
        ::CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(lParam));
        return 1;
    }

    return DefWindowProc(WM_GESTURE, wParam, lParam);
}
