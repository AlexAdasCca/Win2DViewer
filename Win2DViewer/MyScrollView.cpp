#include "pch.h"

#include "MyScrollView.h"

namespace
{
    constexpr int kMinimumLineScroll = 16;
}

CMyScrollView::CMyScrollView()
    : themeModule(::LoadLibraryW(L"uxtheme.dll"))
{
    if (themeModule)
    {
        updatePanningFeedback = reinterpret_cast<UpdatePanningFeedbackFn>(
            ::GetProcAddress(themeModule.get(), "UpdatePanningFeedback"));
    }
}

void CMyScrollView::SetScrollSizes(int /*mapMode*/, SIZE sizeTotal)
{
    totalSize = sizeTotal;
    UpdateScrollMetrics();
    ScrollToPosition(currentScrollPos);
}

void CMyScrollView::UpdateScrollMetrics()
{
    if (scrollHwnd == nullptr)
    {
        return;
    }

    RECT rect{};
    ::GetClientRect(scrollHwnd, &rect);

    pageDev.cx = std::max<int>(0, rect.right - rect.left);
    pageDev.cy = std::max<int>(0, rect.bottom - rect.top);
    lineDev.cx = std::max<int>(kMinimumLineScroll, pageDev.cx / 10);
    lineDev.cy = std::max<int>(kMinimumLineScroll, pageDev.cy / 10);

    UpdateScrollBar(SB_HORZ);
    UpdateScrollBar(SB_VERT);
}

void CMyScrollView::UpdateScrollBar(UINT bar)
{
    if (scrollHwnd == nullptr)
    {
        return;
    }

    const bool horizontal = bar == SB_HORZ;
    const int total = horizontal ? totalSize.cx : totalSize.cy;
    const int page = horizontal ? pageDev.cx : pageDev.cy;
    const int current = horizontal ? currentScrollPos.x : currentScrollPos.y;

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
    si.nMin = 0;
    si.nMax = std::max<int>(0, total - 1);
    si.nPage = static_cast<UINT>(std::max<int>(0, page));
    si.nPos = std::max<int>(0, current);
    ::SetScrollInfo(scrollHwnd, bar, &si, TRUE);
    ::ShowScrollBar(scrollHwnd, bar, total > page);
}

CPoint CMyScrollView::ClampScrollPosition(CPoint pt) const
{
    pt.x = std::clamp<int>(pt.x, 0, GetScrollLimit(SB_HORZ));
    pt.y = std::clamp<int>(pt.y, 0, GetScrollLimit(SB_VERT));
    return pt;
}

void CMyScrollView::ScrollToPosition(POINT pt)
{
    CPoint target = ClampScrollPosition(pt);
    const int xOverPan = pt.x - target.x;
    const int yOverPan = pt.y - target.y;

    if (updatePanningFeedback != nullptr && (xOverPan != 0 || yOverPan != 0))
    {
        updatePanningFeedback(scrollHwnd, xOverPan, yOverPan, FALSE);
    }

    ScrollToDevicePosition(target);
}

void CMyScrollView::ScrollToDevicePosition(POINT pt)
{
    if (scrollHwnd == nullptr)
    {
        return;
    }

    CPoint target = ClampScrollPosition(pt);
    if (target == currentScrollPos)
    {
        return;
    }

    const CPoint oldPos = currentScrollPos;
    OnScrollPositionChanging(oldPos, target);

    ::SetScrollPos(scrollHwnd, SB_HORZ, target.x, TRUE);
    ::SetScrollPos(scrollHwnd, SB_VERT, target.y, TRUE);
    DoNoScrollUpdate(oldPos.x - target.x, oldPos.y - target.y);
    currentScrollPos = target;
}

BOOL CMyScrollView::HandleHorizontalScroll(UINT scrollCode, UINT scrollPos, BOOL bDoScroll)
{
    return HandleScroll(SB_HORZ, scrollCode, scrollPos, bDoScroll);
}

BOOL CMyScrollView::HandleVerticalScroll(UINT scrollCode, UINT scrollPos, BOOL bDoScroll)
{
    return HandleScroll(SB_VERT, scrollCode, scrollPos, bDoScroll);
}

BOOL CMyScrollView::HandleScroll(UINT bar, UINT scrollCode, UINT scrollPos, BOOL bDoScroll)
{
    const bool horizontal = bar == SB_HORZ;
    int current = horizontal ? currentScrollPos.x : currentScrollPos.y;
    int target = current;

    if (scrollCode == SB_THUMBTRACK || scrollCode == SB_THUMBPOSITION)
    {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        if (::GetScrollInfo(scrollHwnd, bar, &si))
        {
            target = si.nTrackPos;
        }
        else
        {
            target = static_cast<int>(scrollPos);
        }
    }
    else if (horizontal)
    {
        switch (scrollCode)
        {
            case SB_LINELEFT:
                target -= lineDev.cx;
                break;
            case SB_LINERIGHT:
                target += lineDev.cx;
                break;
            case SB_PAGELEFT:
                target -= pageDev.cx;
                break;
            case SB_PAGERIGHT:
                target += pageDev.cx;
                break;
            case SB_LEFT:
                target = 0;
                break;
            case SB_RIGHT:
                target = GetScrollLimit(bar);
                break;
            default:
                return FALSE;
        }
    }
    else
    {
        switch (scrollCode)
        {
            case SB_LINEUP:
                target -= lineDev.cy;
                break;
            case SB_LINEDOWN:
                target += lineDev.cy;
                break;
            case SB_PAGEUP:
                target -= pageDev.cy;
                break;
            case SB_PAGEDOWN:
                target += pageDev.cy;
                break;
            case SB_TOP:
                target = 0;
                break;
            case SB_BOTTOM:
                target = GetScrollLimit(bar);
                break;
            default:
                return FALSE;
        }
    }

    CSize delta{0, 0};
    if (horizontal)
    {
        delta.cx = target - current;
    }
    else
    {
        delta.cy = target - current;
    }

    return OnScrollBy(delta, bDoScroll);
}

BOOL CMyScrollView::OnScrollBy(CSize sizeScroll, BOOL bDoScroll)
{
    if (scrollHwnd == nullptr)
    {
        return FALSE;
    }

    CPoint target = ClampScrollPosition(CPoint{
        currentScrollPos.x + sizeScroll.cx,
        currentScrollPos.y + sizeScroll.cy});

    if (target == currentScrollPos)
    {
        return FALSE;
    }

    if (bDoScroll)
    {
        const CPoint oldPos = currentScrollPos;
        OnScrollPositionChanging(oldPos, target);

        ::SetScrollPos(scrollHwnd, SB_HORZ, target.x, TRUE);
        ::SetScrollPos(scrollHwnd, SB_VERT, target.y, TRUE);
        DoNoScrollUpdate(oldPos.x - target.x, oldPos.y - target.y);
        currentScrollPos = target;
    }

    return TRUE;
}

int CMyScrollView::GetScrollLimit(int bar) const
{
    if (scrollHwnd == nullptr)
    {
        return 0;
    }

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    if (!::GetScrollInfo(scrollHwnd, bar, &si))
    {
        return 0;
    }

    return std::max<int>(0, si.nMax - static_cast<int>(std::max<UINT>(1U, si.nPage)) + 1);
}

void CMyScrollView::CheckScrollBars(BOOL& hasHorizontal, BOOL& hasVertical) const
{
    hasHorizontal = GetScrollLimit(SB_HORZ) > 0;
    hasVertical = GetScrollLimit(SB_VERT) > 0;
}

void CMyScrollView::DoNoScrollUpdate(int dx, int dy)
{
    if (scrollHwnd == nullptr)
    {
        return;
    }

    RECT clientRect{};
    ::GetClientRect(scrollHwnd, &clientRect);

    if (dx != 0)
    {
        RECT rect = clientRect;
        if (dx < 0)
        {
            rect.left = rect.right + dx;
        }
        else
        {
            rect.right = rect.left + dx;
        }

        DrawClientRect(rect);
    }

    if (dy != 0)
    {
        RECT rect = clientRect;
        if (dy < 0)
        {
            rect.top = rect.bottom + dy;
        }
        else
        {
            rect.bottom = rect.top + dy;
        }

        DrawClientRect(rect);
    }
}
