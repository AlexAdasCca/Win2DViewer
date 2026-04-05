#pragma once

#include "Resource.h"
#include "SvgDocument.h"
#include "SystemMenuExtensions.h"
#include "Win2DView.h"

class CMainFrame : public CFrameWindowImpl<CMainFrame>,
                   public CMessageFilter
{
public:
    DECLARE_FRAME_WND_CLASS(L"Win2DViewer.WTLFrame", IDR_MAINFRAME)

    BOOL PreTranslateMessage(MSG* pMsg) override;

    bool OpenDocument(std::wstring_view path);

    BEGIN_MSG_MAP(CMainFrame)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
        MESSAGE_HANDLER(WM_INITMENUPOPUP, OnInitMenuPopup)
        MESSAGE_HANDLER(WM_SYSCOMMAND, OnSysCommand)
        MESSAGE_HANDLER(kConsoleDebugStateSyncMsg, OnConsoleDebugStateSync)
        COMMAND_ID_HANDLER(ID_FILE_NEW, OnFileNew)
        COMMAND_ID_HANDLER(ID_FILE_OPEN, OnFileOpen)
        COMMAND_ID_HANDLER(ID_APP_EXIT, OnFileExit)
        COMMAND_ID_HANDLER(ID_APP_ABOUT, OnAppAbout)
        COMMAND_ID_HANDLER(ID_VIEW_LAYER_EFFECT_OVER_SVG, OnLayerEffectsOverSvg)
        COMMAND_ID_HANDLER(ID_VIEW_LAYER_SVG_OVER_EFFECT, OnLayerSvgOverEffects)
        COMMAND_ID_HANDLER(ID_VIEW_LAYER_SVG_ONLY, OnLayerSvgOnly)
        COMMAND_ID_HANDLER(ID_VIEW_LAYER_EFFECT_ONLY, OnLayerEffectsOnly)
        COMMAND_ID_HANDLER(ID_VIEW_CONSOLE_DEBUG, OnViewConsoleDebug)
        COMMAND_ID_HANDLER(ID_TEST_CREATE_WINRT_HOST, OnTestCreateWinRtHost)
        COMMAND_ID_HANDLER(ID_TEST_OPEN_HOST_PANEL, OnTestOpenHostPanel)
        CHAIN_MSG_MAP(CFrameWindowImpl<CMainFrame>)
    END_MSG_MAP()

private:
    void ApplyDocument();
    void InitializeToolBar();
    void InitializeSystemMenu();
    void EnableWindowBackdrop();
    void UpdateWindowTitle();
    void UpdateLayerMenuState();
    void UpdateSystemMenuState(HMENU menuHandle);

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDropFiles(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnInitMenuPopup(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSysCommand(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnConsoleDebugStateSync(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnFileNew(WORD, WORD, HWND, BOOL&);
    LRESULT OnFileOpen(WORD, WORD, HWND, BOOL&);
    LRESULT OnFileExit(WORD, WORD, HWND, BOOL&);
    LRESULT OnAppAbout(WORD, WORD, HWND, BOOL&);
    LRESULT OnLayerEffectsOverSvg(WORD, WORD, HWND, BOOL&);
    LRESULT OnLayerSvgOverEffects(WORD, WORD, HWND, BOOL&);
    LRESULT OnLayerSvgOnly(WORD, WORD, HWND, BOOL&);
    LRESULT OnLayerEffectsOnly(WORD, WORD, HWND, BOOL&);
    LRESULT OnViewConsoleDebug(WORD, WORD, HWND, BOOL&);
    LRESULT OnTestCreateWinRtHost(WORD, WORD, HWND, BOOL&);
    LRESULT OnTestOpenHostPanel(WORD, WORD, HWND, BOOL&);

private:
    CSvgDocument document;
    CWin2DView view;
    bool consoleDebugEnabled = false;
    bool isWindowTopMost = false;
    SystemMenu::MenuHost systemMenuHost{ L"MainFrame.SystemMenu" };
};
