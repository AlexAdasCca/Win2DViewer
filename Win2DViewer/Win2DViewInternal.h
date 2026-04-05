#pragma once

#include <DispatcherQueue.h>
#include <ShellScalingAPI.h>
#include <Windows.Graphics.Interop.h>
#include <algorithm>
#include <cstdio>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include "ConsoleDebugLifecycle.h"
#include "DisplaySyncHelper.h"
#include "Win2DView.h"

namespace Win2DViewNs
{
    namespace wr = wna::rt;
    namespace wf = wna::wd::fnd;
    namespace wfn = wna::wd::num;
    namespace wui = wna::wd::ui;
    namespace wuc = wna::wd::uic;
    namespace wud = wna::wd::uid;
    namespace wut = wna::wd::uit;
    namespace wg = wna::wd::gfx;
    namespace wgd = wna::wd::gdx;
    namespace wgi = wna::wd::gd3;
    namespace mgc = wna::cv::core;
    namespace mgce = wna::cv::eff;
    namespace mgcs = wna::cv::svg;
    namespace mgct = wna::cv::txt;
    namespace mgcu = wna::cv::uic;
} // namespace Win2DViewNs

namespace Win2DViewInternal
{
    inline constexpr double kPi = 3.14159265358979323846;
    inline constexpr UINT_PTR kInertiaTimerId = 456;
    inline constexpr UINT kRenderTickMsg = WM_APP + 1;
    inline constexpr int kMaxBitmapResolution = 8000;
    inline constexpr float kPixelsDpi = 96.0f;

    extern DisplaySyncHelper gDisplaySyncHelper;
    extern bool gDampScrolling;

    Win2DViewNs::wfn::float3x2 IdentityTransform();
    int RoundToInt(double value);

    std::wstring ReplaceString(std::wstring subject, std::wstring const& search,
                               std::wstring const& replace);
    std::wstring Trim(std::wstring value);
    std::wstring NormalizeFontFamilyName(std::wstring fontFamily);

    void EnsureDebugConsole();
    void ReleaseDebugConsole();
    void DebugPrintLine(std::wstring const& line);

    std::wstring InlineSvgClassStyles(std::wstring svgText);
    std::vector<CWin2DView::SvgTextOverlayItem>
    ParseSvgTextOverlays(std::wstring const& svgText);

    std::wstring UTF8ToWide(char const* input);
    float GetFontSize(float width);
    Win2DViewNs::wr::com_ptr<::IDXGIDevice>
    GetDXGIDevice(Win2DViewNs::mgc::CanvasDevice& device);
} // namespace Win2DViewInternal
