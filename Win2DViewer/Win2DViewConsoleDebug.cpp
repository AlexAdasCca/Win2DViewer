#include "pch.h"

#include "Win2DViewInternal.h"

void CWin2DView::SetConsoleDebugEnabled(bool enabled) noexcept
{
    consoleDebugEnabled = enabled;
    if (::IsWindow(m_hWnd))
    {
        ConsoleDebugLifecycle::SetStateSyncTargetWindow(
            ::GetAncestor(m_hWnd, GA_ROOT));
    }
    if (consoleDebugEnabled)
    {
        Win2DViewInternal::EnsureDebugConsole();
        std::wstringstream ss;
        ss << L"[ConsoleDebug] enabled=1 overlays=" << svgTextOverlays.size()
           << L" svgDoc=" << (svgDocument != nullptr ? 1 : 0) << L" dpi="
           << currentDpi << L" viewSize=" << width << L"x" << height;
        Win2DViewInternal::DebugPrintLine(ss.str());
    }
    else
    {
        Win2DViewInternal::DebugPrintLine(L"[ConsoleDebug] enabled=0");
        Win2DViewInternal::ReleaseDebugConsole();
        ConsoleDebugLifecycle::SetStateSyncTargetWindow(nullptr);
    }
}
