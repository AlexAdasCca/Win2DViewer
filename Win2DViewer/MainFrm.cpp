#include "pch.h"

#include <dwmapi.h>

#include "DesktopCompositionInterop.h"
#include "MainFrm.h"
#include "SystemMenuExtensions.h"
#include "Win2DViewer.h"

#pragma comment(lib, "dwmapi.lib")

namespace
{
    constexpr wchar_t kSvgFilter[] = L"SVG Files (*.svg)\0*.svg\0All Files (*.*)\0*.*\0";

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
    constexpr DWORD kDwmAttrSystemBackdropType = 38;
#else
    constexpr DWORD kDwmAttrSystemBackdropType = static_cast<DWORD>(DWMWA_SYSTEMBACKDROP_TYPE);
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
    constexpr DWORD kDwmAttrImmersiveDarkMode = 20;
#else
    constexpr DWORD kDwmAttrImmersiveDarkMode = static_cast<DWORD>(DWMWA_USE_IMMERSIVE_DARK_MODE);
#endif
#ifndef DWMWA_USE_HOSTBACKDROPBRUSH
    constexpr DWORD kDwmAttrUseHostBackdropBrush = 17;
#else
    constexpr DWORD kDwmAttrUseHostBackdropBrush = static_cast<DWORD>(DWMWA_USE_HOSTBACKDROPBRUSH);
#endif
#ifndef DWMSBT_MAINWINDOW
    constexpr DWORD kDwmBackdropMainWindow = 2;
#else
    constexpr DWORD kDwmBackdropMainWindow = static_cast<DWORD>(DWMSBT_MAINWINDOW);
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
    constexpr DWORD kDwmBackdropTransientWindow = 3;
#else
    constexpr DWORD kDwmBackdropTransientWindow = static_cast<DWORD>(DWMSBT_TRANSIENTWINDOW);
#endif

    std::optional<std::wstring> BrowseForSvgFile(HWND owner)
    {
        wchar_t filePath[MAX_PATH] = {};

        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = kSvgFilter;
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = _countof(filePath);
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
        ofn.lpstrDefExt = L"svg";

        if (!::GetOpenFileNameW(&ofn))
        {
            return std::nullopt;
        }

        return std::wstring{filePath};
    }

    UINT LayerModeToCommandId(CWin2DView::RenderLayerMode mode)
    {
        switch (mode)
        {
            case CWin2DView::RenderLayerMode::EffectsOverSvg:
                return ID_VIEW_LAYER_EFFECT_OVER_SVG;
            case CWin2DView::RenderLayerMode::SvgOverEffects:
                return ID_VIEW_LAYER_SVG_OVER_EFFECT;
            case CWin2DView::RenderLayerMode::SvgOnly:
                return ID_VIEW_LAYER_SVG_ONLY;
            case CWin2DView::RenderLayerMode::EffectsOnly:
                return ID_VIEW_LAYER_EFFECT_ONLY;
            default:
                return ID_VIEW_LAYER_EFFECT_OVER_SVG;
        }
    }

    void ShowAboutDialog(HWND ownerWindow)
    {
        const std::wstring message = LoadAppString(IDS_APP_TITLE) + L"\nWTL host for Win2D SVG rendering.";
        AtlMessageBox(ownerWindow, message.c_str(), IDS_APP_TITLE, MB_OK | MB_ICONINFORMATION);
    }
} // namespace

BOOL CMainFrame::PreTranslateMessage(MSG* pMsg)
{
    if (systemMenuHost.HandleShortcut(m_hWnd, pMsg->message, pMsg->wParam))
    {
        return TRUE;
    }

    if (CFrameWindowImpl<CMainFrame>::PreTranslateMessage(pMsg))
    {
        return TRUE;
    }

    return view.IsWindow() ? view.PreTranslateMessage(pMsg) : FALSE;
}

LRESULT CMainFrame::OnCreate(UINT, WPARAM, LPARAM, BOOL&)
{
    RECT rcClient{};
    GetClientRect(&rcClient);

    m_hWndClient = view.Create(
        m_hWnd,
        rcClient,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_HSCROLL | WS_VSCROLL,
        WS_EX_NOREDIRECTIONBITMAP);

    if (m_hWndClient == nullptr)
    {
        return -1;
    }

    EnableWindowBackdrop();
    InitializeToolBar();
    InitializeSystemMenu();
    UpdateLayout();

    view.SetDocument(&document);
    view.RefreshDocument();
    UpdateLayerMenuState();

    DragAcceptFiles(TRUE);

    if (auto* messageLoop = appModule.GetMessageLoop(); messageLoop != nullptr)
    {
        messageLoop->AddMessageFilter(this);
    }

    UpdateWindowTitle();
    return 0;
}

LRESULT CMainFrame::OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
{
    if (auto* messageLoop = appModule.GetMessageLoop(); messageLoop != nullptr)
    {
        messageLoop->RemoveMessageFilter(this);
    }

    ::PostQuitMessage(0);
    return 0;
}

LRESULT CMainFrame::OnEraseBkgnd(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    HDC hdc = reinterpret_cast<HDC>(wParam);
    RECT rc{};
    GetClientRect(&rc);
    ::FillRect(hdc, &rc, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
    return 1;
}

LRESULT CMainFrame::OnDropFiles(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    HDROP dropHandle = reinterpret_cast<HDROP>(wParam);
    wchar_t path[MAX_PATH]{};
    if (::DragQueryFileW(dropHandle, 0, path, _countof(path)) != 0)
    {
        OpenDocument(path);
    }

    ::DragFinish(dropHandle);
    return 0;
}

LRESULT CMainFrame::OnInitMenuPopup(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    HMENU menuHandle = reinterpret_cast<HMENU>(wParam);
    if (menuHandle != nullptr)
    {
        UpdateLayerMenuState();
        UpdateSystemMenuState(menuHandle);
    }

    return 0;
}

LRESULT CMainFrame::OnSysCommand(UINT, WPARAM wParam, LPARAM, BOOL& bHandled)
{
    const UINT_PTR commandId = (wParam & 0xFFF0);
    if (systemMenuHost.HandleCommand(m_hWnd, commandId))
    {
        return 0;
    }

    bHandled = FALSE;
    return 0;
}

LRESULT CMainFrame::OnConsoleDebugStateSync(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    const bool enabled = wParam != 0;
    if (view.IsConsoleDebugEnabled() != enabled)
    {
        view.SetConsoleDebugEnabled(enabled);
    }

    consoleDebugEnabled = view.IsConsoleDebugEnabled();
    UpdateLayerMenuState();
    ::DrawMenuBar(m_hWnd);
    return 0;
}

LRESULT CMainFrame::OnFileNew(WORD, WORD, HWND, BOOL&)
{
    document.Clear();
    ApplyDocument();
    return 0;
}

LRESULT CMainFrame::OnFileOpen(WORD, WORD, HWND, BOOL&)
{
    if (auto path = BrowseForSvgFile(m_hWnd); path.has_value())
    {
        OpenDocument(*path);
    }

    return 0;
}

LRESULT CMainFrame::OnFileExit(WORD, WORD, HWND, BOOL&)
{
    PostMessage(WM_CLOSE);
    return 0;
}

LRESULT CMainFrame::OnAppAbout(WORD, WORD, HWND, BOOL&)
{
    ShowAboutDialog(m_hWnd);
    return 0;
}

LRESULT CMainFrame::OnLayerEffectsOverSvg(WORD, WORD, HWND, BOOL&)
{
    view.SetRenderLayerMode(CWin2DView::RenderLayerMode::EffectsOverSvg);
    UpdateLayerMenuState();
    return 0;
}

LRESULT CMainFrame::OnLayerSvgOverEffects(WORD, WORD, HWND, BOOL&)
{
    view.SetRenderLayerMode(CWin2DView::RenderLayerMode::SvgOverEffects);
    UpdateLayerMenuState();
    return 0;
}

LRESULT CMainFrame::OnLayerSvgOnly(WORD, WORD, HWND, BOOL&)
{
    view.SetRenderLayerMode(CWin2DView::RenderLayerMode::SvgOnly);
    UpdateLayerMenuState();
    return 0;
}

LRESULT CMainFrame::OnLayerEffectsOnly(WORD, WORD, HWND, BOOL&)
{
    view.SetRenderLayerMode(CWin2DView::RenderLayerMode::EffectsOnly);
    UpdateLayerMenuState();
    return 0;
}

LRESULT CMainFrame::OnViewConsoleDebug(WORD, WORD, HWND, BOOL&)
{
    consoleDebugEnabled = !consoleDebugEnabled;
    view.SetConsoleDebugEnabled(consoleDebugEnabled);
    UpdateLayerMenuState();
    return 0;
}

LRESULT CMainFrame::OnTestCreateWinRtHost(WORD, WORD, HWND, BOOL&)
{
    std::wstring errorMessage;
    if (!DesktopInterop::CreateDesktopHostTestWindow(DesktopInterop::DesktopHostBackend::WinRTComposition, &errorMessage))
    {
        const std::wstring message = L"Failed to create WinRT composition host window.\n" + errorMessage;
        AtlMessageBox(m_hWnd, message.c_str(), IDS_APP_TITLE, MB_OK | MB_ICONERROR);
    }

    return 0;
}

LRESULT CMainFrame::OnTestOpenHostPanel(WORD, WORD, HWND, BOOL&)
{
    DesktopInterop::ShowDesktopHostTestPanel(m_hWnd);
    return 0;
}

bool CMainFrame::OpenDocument(std::wstring_view path)
{
    std::wstring errorMessage;
    if (!document.LoadFromFile(path, &errorMessage))
    {
        AtlMessageBox(m_hWnd, errorMessage.c_str(), IDS_APP_TITLE, MB_OK | MB_ICONERROR);
        return false;
    }

    ApplyDocument();
    return true;
}

void CMainFrame::ApplyDocument()
{
    view.RefreshDocument();
    UpdateWindowTitle();
}

void CMainFrame::InitializeToolBar()
{
    if (!CreateSimpleToolBar(IDR_MAINFRAME))
    {
        return;
    }

    ::SendMessage(m_hWndToolBar, TB_AUTOSIZE, 0, 0);
    ::ShowWindow(m_hWndToolBar, SW_SHOW);
}

void CMainFrame::InitializeSystemMenu()
{
    HMENU systemMenu = GetSystemMenu(FALSE);
    if (systemMenu == nullptr)
    {
        return;
    }

    if (systemMenuHost.Items().empty())
    {
        std::wstring errorMessage;

        SystemMenu::MenuItemSpec topMostItem{};
        topMostItem.id = SystemMenu::kCommandWindowTopMost;
        topMostItem.text = L"\u7A97\u53E3\u7F6E\u9876";
        topMostItem.shortcut = SystemMenu::ShortcutBinding{};
        topMostItem.shortcut->virtualKey = 'T';
        topMostItem.shortcut->ctrl = true;
        topMostItem.shortcut->alt = true;
        topMostItem.onInvoke = [this](HWND windowHandle)
        {
            isWindowTopMost = !isWindowTopMost;
            ::SetWindowPos(
                windowHandle,
                isWindowTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        };
        topMostItem.isChecked = [this]()
        { return isWindowTopMost; };
        topMostItem.isEnabled = []()
        { return true; };

        SystemMenu::MenuItemSpec separator{};
        separator.separator = true;

        SystemMenu::MenuItemSpec aboutItem{};
        aboutItem.id = SystemMenu::kCommandAbout;
        aboutItem.text = L"\u6253\u5F00\u5173\u4E8E";
        aboutItem.shortcut = SystemMenu::ShortcutBinding{};
        aboutItem.shortcut->virtualKey = VK_F1;
        aboutItem.shortcut->alt = true;
        aboutItem.onInvoke = [](HWND windowHandle)
        { ShowAboutDialog(windowHandle); };
        aboutItem.isEnabled = []()
        { return true; };

        (void)systemMenuHost.AddItem(separator, &errorMessage);
        (void)systemMenuHost.AddItem(std::move(topMostItem), &errorMessage);
        (void)systemMenuHost.AddItem(std::move(aboutItem), &errorMessage);
    }

    std::wstring errorMessage;
    if (!systemMenuHost.Install(systemMenu, &errorMessage))
    {
        AtlMessageBox(m_hWnd, errorMessage.c_str(), IDS_APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }

    UpdateSystemMenuState(systemMenu);
}

void CMainFrame::EnableWindowBackdrop()
{
    const BOOL darkMode = TRUE;
    ::DwmSetWindowAttribute(m_hWnd, kDwmAttrImmersiveDarkMode, &darkMode, sizeof(darkMode));
    const BOOL useHostBackdropBrush = TRUE;
    ::DwmSetWindowAttribute(m_hWnd, kDwmAttrUseHostBackdropBrush, &useHostBackdropBrush, sizeof(useHostBackdropBrush));

    // Prefer desktop acrylic; fallback to legacy blur-behind.
    const DWORD backdropType = kDwmBackdropTransientWindow;
    if (FAILED(::DwmSetWindowAttribute(m_hWnd, kDwmAttrSystemBackdropType, &backdropType, sizeof(backdropType))))
    {
        DWM_BLURBEHIND blurBehind{};
        blurBehind.dwFlags = DWM_BB_ENABLE;
        blurBehind.fEnable = TRUE;
        ::DwmEnableBlurBehindWindow(m_hWnd, &blurBehind);
    }
}

void CMainFrame::UpdateWindowTitle()
{
    std::wstring title = LoadAppString(IDS_APP_TITLE);
    if (!document.GetPath().empty())
    {
        title += L" - ";
        title += std::wstring{document.GetPath()};
    }

    SetWindowTextW(title.c_str());
}

void CMainFrame::UpdateLayerMenuState()
{
    HMENU menuHandle = GetMenu();
    if (menuHandle == nullptr)
    {
        return;
    }

    const UINT selectedId = LayerModeToCommandId(view.GetRenderLayerMode());
    ::CheckMenuRadioItem(
        menuHandle,
        ID_VIEW_LAYER_EFFECT_OVER_SVG,
        ID_VIEW_LAYER_EFFECT_ONLY,
        selectedId,
        MF_BYCOMMAND);

    consoleDebugEnabled = view.IsConsoleDebugEnabled();
    ::CheckMenuItem(
        menuHandle,
        ID_VIEW_CONSOLE_DEBUG,
        MF_BYCOMMAND | (consoleDebugEnabled ? MF_CHECKED : MF_UNCHECKED));
}

void CMainFrame::UpdateSystemMenuState(HMENU menuHandle)
{
    HMENU systemMenu = GetSystemMenu(FALSE);
    if (menuHandle == nullptr || systemMenu == nullptr || menuHandle != systemMenu)
    {
        return;
    }

    systemMenuHost.RefreshState(systemMenu);
}
