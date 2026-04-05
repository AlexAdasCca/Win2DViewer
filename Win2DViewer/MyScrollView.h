#pragma once

using UpdatePanningFeedbackFn = BOOL(WINAPI*)(HWND, LONG, LONG, BOOL);

class CMyScrollView
{
  public:
    CMyScrollView();
    virtual ~CMyScrollView() = default;

    void AttachScrollWindow(HWND hwnd) noexcept
    {
        scrollHwnd = hwnd;
    }

    CPoint GetScrollPosition() const noexcept
    {
        return currentScrollPos;
    }
    CSize GetTotalSize() const noexcept
    {
        return totalSize;
    }

    void SetScrollSizes(int mapMode, SIZE sizeTotal);
    void ScrollToPosition(POINT pt);
    BOOL OnScrollBy(CSize sizeScroll, BOOL bDoScroll = TRUE);
    BOOL HandleHorizontalScroll(UINT scrollCode, UINT scrollPos, BOOL bDoScroll = TRUE);
    BOOL HandleVerticalScroll(UINT scrollCode, UINT scrollPos, BOOL bDoScroll = TRUE);

    int GetScrollLimit(int bar) const;
    void CheckScrollBars(BOOL& hasHorizontal, BOOL& hasVertical) const;
    void UpdateScrollMetrics();

  protected:
    virtual void DrawClientRect(RECT& rect)
    {
    }
    virtual void OnScrollPositionChanging(CPoint oldPos, CPoint newPos)
    {
    }

    HWND ScrollHwnd() const noexcept
    {
        return scrollHwnd;
    }
    CPoint ClampScrollPosition(CPoint pt) const;

  private:
    BOOL HandleScroll(UINT bar, UINT scrollCode, UINT scrollPos, BOOL bDoScroll);
    void ScrollToDevicePosition(POINT pt);
    void DoNoScrollUpdate(int dx, int dy);
    void UpdateScrollBar(UINT bar);

  private:
    HWND scrollHwnd = nullptr;
    CPoint currentScrollPos{0, 0};
    CSize totalSize{0, 0};
    CSize pageDev{0, 0};
    CSize lineDev{1, 1};
    wil::unique_hmodule themeModule;
    UpdatePanningFeedbackFn updatePanningFeedback = nullptr;
};
