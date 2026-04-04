#include "pch.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <DispatcherQueue.h>
#include <ShellScalingAPI.h>
#include <Windows.Graphics.Interop.h>
#include <windows.ui.composition.interop.h>
#include <cstdio>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include "Win2DViewer.h"
#include "Win2DView.h"
#include "ConsoleMenuInjection.h"
#include "ConsoleHookIpc.h"
#include "ConsoleTextWriter.h"
#include "DisplaySyncHelper.h"

namespace ns
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
}

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr UINT_PTR kInertiaTimerId = 456;
    constexpr UINT kRenderTickMsg = WM_APP + 1;
    constexpr int kMaxBitmapResolution = 8000;
    constexpr float kPixelsDpi = 96.0f;

    DisplaySyncHelper displaySyncHelper;
    bool gDampScrolling = true;
    bool gConsoleAllocated = false;
    bool gConsoleOwnedByApp = false;
    bool gConsoleCtrlHandlerInstalled = false;
    HANDLE gConsoleCloseNotifyEvent = nullptr;
    HANDLE gConsoleCtrlFallbackEvent = nullptr;
    HANDLE gConsoleMonitorStopEvent = nullptr;
    HANDLE gConsoleMonitorThread = nullptr;
    HWND gConsoleStateSyncTargetWindow = nullptr;
    std::atomic_bool gConsoleDetachInProgress = false;
    void StopConsoleCloseMonitor();

    void DetachDebugConsoleAfterCloseSignal(std::wstring const& source)
    {
        if (!gConsoleAllocated)
        {
            return;
        }

        bool expected = false;
        if (!gConsoleDetachInProgress.compare_exchange_strong(expected, true))
        {
            return;
        }

        diagnosticconsole::LineBuilder line;
        line << L"[ConsoleDebug] close-signal source=" << source << L", detaching console.";
        diagnosticconsole::WriteLine(line.str());

        if (gConsoleOwnedByApp)
        {
            (void)::FreeConsole();
        }

        FILE* stream = nullptr;
        freopen_s(&stream, "NUL:", "w", stdout);
        freopen_s(&stream, "NUL:", "w", stderr);
        freopen_s(&stream, "NUL:", "r", stdin);

        gConsoleAllocated = false;
        gConsoleOwnedByApp = false;
        consolemenu::SetInjectionDiagnosticsEnabled(false);
        consolemenu::ResetConsoleHookState();
        if (gConsoleStateSyncTargetWindow != nullptr && ::IsWindow(gConsoleStateSyncTargetWindow))
        {
            ::PostMessageW(gConsoleStateSyncTargetWindow, kConsoleDebugStateSyncMsg, FALSE, 0);
        }
        gConsoleDetachInProgress.store(false);
    }

    BOOL WINAPI DebugConsoleCtrlHandler(DWORD ctrlType)
    {
        switch (ctrlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (gConsoleCtrlFallbackEvent != nullptr)
            {
                ::SetEvent(gConsoleCtrlFallbackEvent);
            }
            return TRUE;
        default:
            return FALSE;
        }
    }

    DWORD WINAPI ConsoleCloseMonitorThreadProc(LPVOID)
    {
        HANDLE waits[] = { gConsoleMonitorStopEvent, gConsoleCloseNotifyEvent, gConsoleCtrlFallbackEvent };
        for (;;)
        {
            const DWORD waitResult = ::WaitForMultipleObjects(_countof(waits), waits, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0)
            {
                return 0;
            }
            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                DetachDebugConsoleAfterCloseSignal(L"ConhostSubclassNotify");
                continue;
            }
            if (waitResult == WAIT_OBJECT_0 + 2)
            {
                DetachDebugConsoleAfterCloseSignal(L"ConsoleCtrlHandlerFallback");
                continue;
            }
            return 1;
        }
    }

    bool StartConsoleCloseMonitor()
    {
        if (gConsoleMonitorThread != nullptr)
        {
            return true;
        }

        const std::wstring notifyEventName = consolehookipc::BuildConsoleCloseNotifyEventName(::GetCurrentProcessId());
        gConsoleCloseNotifyEvent = ::CreateEventW(nullptr, FALSE, FALSE, notifyEventName.c_str());
        gConsoleCtrlFallbackEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        gConsoleMonitorStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (gConsoleCloseNotifyEvent == nullptr || gConsoleCtrlFallbackEvent == nullptr || gConsoleMonitorStopEvent == nullptr)
        {
            StopConsoleCloseMonitor();
            return false;
        }

        if (!gConsoleCtrlHandlerInstalled)
        {
            gConsoleCtrlHandlerInstalled = !!::SetConsoleCtrlHandler(&DebugConsoleCtrlHandler, TRUE);
        }

        gConsoleMonitorThread = ::CreateThread(nullptr, 0, &ConsoleCloseMonitorThreadProc, nullptr, 0, nullptr);
        if (gConsoleMonitorThread == nullptr)
        {
            StopConsoleCloseMonitor();
            return false;
        }

        return true;
    }

    void StopConsoleCloseMonitor()
    {
        if (gConsoleCtrlHandlerInstalled)
        {
            (void)::SetConsoleCtrlHandler(&DebugConsoleCtrlHandler, FALSE);
            gConsoleCtrlHandlerInstalled = false;
        }

        if (gConsoleMonitorStopEvent != nullptr)
        {
            ::SetEvent(gConsoleMonitorStopEvent);
        }

        if (gConsoleMonitorThread != nullptr)
        {
            if (::GetCurrentThreadId() != ::GetThreadId(gConsoleMonitorThread))
            {
                (void)::WaitForSingleObject(gConsoleMonitorThread, 2000);
            }
            ::CloseHandle(gConsoleMonitorThread);
            gConsoleMonitorThread = nullptr;
        }

        if (gConsoleMonitorStopEvent != nullptr)
        {
            ::CloseHandle(gConsoleMonitorStopEvent);
            gConsoleMonitorStopEvent = nullptr;
        }
        if (gConsoleCloseNotifyEvent != nullptr)
        {
            ::CloseHandle(gConsoleCloseNotifyEvent);
            gConsoleCloseNotifyEvent = nullptr;
        }
        if (gConsoleCtrlFallbackEvent != nullptr)
        {
            ::CloseHandle(gConsoleCtrlFallbackEvent);
            gConsoleCtrlFallbackEvent = nullptr;
        }
    }

    ns::wfn::float3x2 IdentityTransform()
    {
        return ns::wfn::float3x2{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    }

    int RoundToInt(double value)
    {
        return static_cast<int>(value + ((value < 0.0) ? -0.5 : 0.5));
    }

    std::wstring ReplaceString(std::wstring subject, std::wstring const& search, std::wstring const& replace)
    {
        size_t pos = 0;
        while ((pos = subject.find(search, pos)) != std::wstring::npos)
        {
            subject.replace(pos, search.length(), replace);
            pos += replace.length();
        }

        return subject;
    }

    std::wstring Trim(std::wstring value)
    {
        auto notSpace = [](wchar_t ch) { return !iswspace(ch); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    std::wstring NormalizeFontFamilyName(std::wstring fontFamily)
    {
        fontFamily = Trim(std::move(fontFamily));
        if (fontFamily.empty())
        {
            return L"Segoe UI";
        }

        const size_t commaPos = fontFamily.find(L',');
        if (commaPos != std::wstring::npos)
        {
            fontFamily = fontFamily.substr(0, commaPos);
        }

        fontFamily = Trim(std::move(fontFamily));
        if (!fontFamily.empty() && fontFamily.back() == L';')
        {
            fontFamily.pop_back();
            fontFamily = Trim(std::move(fontFamily));
        }

        if (fontFamily.size() >= 2 &&
            ((fontFamily.front() == L'"' && fontFamily.back() == L'"') ||
             (fontFamily.front() == L'\'' && fontFamily.back() == L'\'')))
        {
            fontFamily = fontFamily.substr(1, fontFamily.size() - 2);
        }

        fontFamily = Trim(std::move(fontFamily));
        if (fontFamily.empty())
        {
            return L"Segoe UI";
        }
        return fontFamily;
    }

    void EnsureDebugConsole()
    {
        if (gConsoleAllocated)
        {
            return;
        }

        if (!::AllocConsole())
        {
            return;
        }

        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
        diagnosticconsole::ConfigureUnicodeConsole();
        gConsoleAllocated = true;
        gConsoleOwnedByApp = true;
        consolemenu::SetInjectionDiagnosticsEnabled(true);
        diagnosticconsole::WriteLine(L"[ConsoleDebug] Console attached.");
        (void)StartConsoleCloseMonitor();
        consolemenu::BeginConsoleHookInjectionAsync();
    }

    void ReleaseDebugConsole()
    {
        StopConsoleCloseMonitor();

        if (gConsoleAllocated)
        {
            fflush(stdout);
            fflush(stderr);

            FILE* stream = nullptr;
            freopen_s(&stream, "NUL:", "w", stdout);
            freopen_s(&stream, "NUL:", "w", stderr);
            freopen_s(&stream, "NUL:", "r", stdin);

            if (gConsoleOwnedByApp)
            {
                ::FreeConsole();
            }
        }

        gConsoleAllocated = false;
        gConsoleOwnedByApp = false;
        consolemenu::SetInjectionDiagnosticsEnabled(false);
        consolemenu::ResetConsoleHookState();
    }

    void DebugPrintLine(std::wstring const& line)
    {
        if (!gConsoleAllocated)
        {
            return;
        }

        diagnosticconsole::WriteLine(line);
    }

    std::wstring InlineSvgClassStyles(std::wstring svgText)
    {
        const std::wstring styleOpen = L"<style>";
        const std::wstring styleClose = L"</style>";
        const size_t styleStart = svgText.find(styleOpen);
        if (styleStart == std::wstring::npos)
        {
            return svgText;
        }

        const size_t cssStart = styleStart + styleOpen.size();
        const size_t styleEnd = svgText.find(styleClose, cssStart);
        if (styleEnd == std::wstring::npos)
        {
            return svgText;
        }

        const std::wstring css = svgText.substr(cssStart, styleEnd - cssStart);
        std::unordered_map<std::wstring, std::wstring> classStyles;

        const std::wregex classRule(LR"(.([A-Za-z0-9_-]+)\s*\{([^}]*)\})");
        for (std::wsregex_iterator it(css.begin(), css.end(), classRule), end; it != end; ++it)
        {
            const std::wstring className = (*it)[1].str();
            std::wstring declarations = Trim((*it)[2].str());
            if (!declarations.empty() && declarations.back() != L';')
            {
                declarations.push_back(L';');
            }
            classStyles[className] = declarations;
        }

        if (classStyles.empty())
        {
            return svgText;
        }

        std::wstring output;
        output.reserve(svgText.size() + 256);
        output.append(svgText.substr(0, styleStart));
        output.append(svgText.substr(styleEnd + styleClose.size()));

        const std::wregex classAttr(L"class\\s*=\\s*\"([^\"]*)\"");
        const std::wregex styleAttr(L"style\\s*=\\s*\"([^\"]*)\"");
        auto EscapeAttributeValue = [](std::wstring value)
        {
            value = ReplaceString(std::move(value), L"&", L"&amp;");
            value = ReplaceString(std::move(value), L"\"", L"&quot;");
            value = ReplaceString(std::move(value), L"<", L"&lt;");
            value = ReplaceString(std::move(value), L">", L"&gt;");
            return value;
        };

        std::wstring rebuilt;
        rebuilt.reserve(output.size() + 256);

        size_t pos = 0;
        while (true)
        {
            const size_t tagStart = output.find(L'<', pos);
            if (tagStart == std::wstring::npos)
            {
                rebuilt.append(output.substr(pos));
                break;
            }

            rebuilt.append(output.substr(pos, tagStart - pos));
            const size_t tagEnd = output.find(L'>', tagStart);
            if (tagEnd == std::wstring::npos)
            {
                rebuilt.append(output.substr(tagStart));
                break;
            }

            std::wstring tag = output.substr(tagStart, tagEnd - tagStart + 1);
            std::wsmatch classMatch;
            if (std::regex_search(tag, classMatch, classAttr))
            {
                std::wstring mergedStyle;
                std::wstringstream classes(classMatch[1].str());
                std::wstring cls;
                while (classes >> cls)
                {
                    auto it = classStyles.find(cls);
                    if (it != classStyles.end())
                    {
                        mergedStyle += it->second;
                    }
                }

                if (!mergedStyle.empty())
                {
                    tag = std::regex_replace(tag, classAttr, L"");

                    std::wsmatch styleMatch;
                    if (std::regex_search(tag, styleMatch, styleAttr))
                    {
                        std::wstring currentStyle = Trim(styleMatch[1].str());
                        if (!currentStyle.empty() && currentStyle.back() != L';')
                        {
                            currentStyle.push_back(L';');
                        }
                        const std::wstring escapedStyle = EscapeAttributeValue(currentStyle + mergedStyle);
                        const std::wstring newStyle = L"style=\"" + escapedStyle + L"\"";
                        tag = std::regex_replace(tag, styleAttr, newStyle, std::regex_constants::format_first_only);
                    }
                    else
                    {
                        size_t insertPos = tag.size() > 1 ? tag.size() - 1 : tag.size();
                        if (tag.size() >= 2 && tag[tag.size() - 2] == L'/' && tag[tag.size() - 1] == L'>')
                        {
                            insertPos = tag.size() - 2;
                        }
                        tag.insert(insertPos, L" style=\"" + EscapeAttributeValue(mergedStyle) + L"\"");
                    }
                }
            }

            rebuilt.append(tag);
            pos = tagEnd + 1;
        }

        return rebuilt;
    }

    std::unordered_map<std::wstring, std::wstring> ParseCssDeclarations(std::wstring const& declarationBlock)
    {
        std::unordered_map<std::wstring, std::wstring> declarations;
        size_t pos = 0;
        while (pos < declarationBlock.size())
        {
            const size_t semi = declarationBlock.find(L';', pos);
            std::wstring item = declarationBlock.substr(pos, semi == std::wstring::npos ? std::wstring::npos : semi - pos);
            item = Trim(std::move(item));
            if (!item.empty())
            {
                const size_t colon = item.find(L':');
                if (colon != std::wstring::npos)
                {
                    std::wstring key = Trim(item.substr(0, colon));
                    std::wstring value = Trim(item.substr(colon + 1));
                    if (!key.empty() && !value.empty())
                    {
                        declarations[key] = value;
                    }
                }
            }

            if (semi == std::wstring::npos)
            {
                break;
            }
            pos = semi + 1;
        }
        return declarations;
    }

    std::unordered_map<std::wstring, std::wstring> ParseXmlAttributes(std::wstring const& attrText)
    {
        std::unordered_map<std::wstring, std::wstring> attrs;
        const std::wregex attrPatternDouble(LR"attr(([A-Za-z_:][A-Za-z0-9_.:-]*)\s*=\s*"([^"]*)")attr");
        for (std::wsregex_iterator it(attrText.begin(), attrText.end(), attrPatternDouble), end; it != end; ++it)
        {
            attrs[(*it)[1].str()] = (*it)[2].str();
        }

        const std::wregex attrPatternSingle(LR"attr(([A-Za-z_:][A-Za-z0-9_.:-]*)\s*=\s*'([^']*)')attr");
        for (std::wsregex_iterator it(attrText.begin(), attrText.end(), attrPatternSingle), end; it != end; ++it)
        {
            attrs[(*it)[1].str()] = (*it)[2].str();
        }
        return attrs;
    }

    std::wstring DecodeXmlEntities(std::wstring text)
    {
        text = ReplaceString(std::move(text), L"&lt;", L"<");
        text = ReplaceString(std::move(text), L"&gt;", L">");
        text = ReplaceString(std::move(text), L"&quot;", L"\"");
        text = ReplaceString(std::move(text), L"&apos;", L"'");
        text = ReplaceString(std::move(text), L"&amp;", L"&");
        return text;
    }

    ns::wui::Color ParseSvgColor(std::wstring const& value, ns::wui::Color fallback)
    {
        std::wstring color = Trim(value);
        if (color.empty())
        {
            return fallback;
        }
        if (color[0] == L'#')
        {
            if (color.size() == 4)
            {
                auto hex = [](wchar_t c) -> uint8_t
                {
                    if (c >= L'0' && c <= L'9') return static_cast<uint8_t>(c - L'0');
                    if (c >= L'a' && c <= L'f') return static_cast<uint8_t>(10 + c - L'a');
                    if (c >= L'A' && c <= L'F') return static_cast<uint8_t>(10 + c - L'A');
                    return 0;
                };
                return ns::wui::Color{
                    255,
                    static_cast<uint8_t>(hex(color[1]) * 17),
                    static_cast<uint8_t>(hex(color[2]) * 17),
                    static_cast<uint8_t>(hex(color[3]) * 17)
                };
            }
            if (color.size() == 7)
            {
                const auto toByte = [&](size_t i) -> uint8_t
                {
                    return static_cast<uint8_t>(std::wcstol(color.substr(i, 2).c_str(), nullptr, 16));
                };
                return ns::wui::Color{ 255, toByte(1), toByte(3), toByte(5) };
            }
        }

        if (color == L"black")
        {
            return ns::wui::Color{ 255, 0, 0, 0 };
        }
        if (color == L"white")
        {
            return ns::wui::Color{ 255, 255, 255, 255 };
        }
        return fallback;
    }

    void ApplyFontShorthand(std::wstring const& fontValue, float& fontSize, bool& bold, std::wstring& fontFamily)
    {
        const std::wregex sizePattern(LR"(([0-9]+(?:.[0-9]+)?)px)");
        std::wsmatch sizeMatch;
        if (std::regex_search(fontValue, sizeMatch, sizePattern))
        {
            fontSize = static_cast<float>(std::wcstod(sizeMatch[1].str().c_str(), nullptr));
            size_t familyPos = static_cast<size_t>(sizeMatch.position() + sizeMatch.length());
            if (familyPos < fontValue.size())
            {
                std::wstring family = Trim(fontValue.substr(familyPos));
                if (!family.empty())
                {
                    fontFamily = family;
                }
            }
        }

        if (fontValue.find(L"bold") != std::wstring::npos ||
            std::regex_search(fontValue, std::wregex(LR"((^|\s)([6-9]00)(\s|$))")))
        {
            bold = true;
        }
    }

    std::vector<CWin2DView::SvgTextOverlayItem> ParseSvgTextOverlays(std::wstring const& svgText)
    {
        std::vector<CWin2DView::SvgTextOverlayItem> overlays;

        std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> classDeclarations;
        const std::wregex styleBlock(LR"(<style[^>]*>([\s\S]*?)</style>)");
        std::wsmatch styleMatch;
        if (std::regex_search(svgText, styleMatch, styleBlock))
        {
            const std::wstring css = styleMatch[1].str();
            const std::wregex classRule(LR"(.([A-Za-z0-9_-]+)\s*\{([^}]*)\})");
            for (std::wsregex_iterator it(css.begin(), css.end(), classRule), end; it != end; ++it)
            {
                classDeclarations[(*it)[1].str()] = ParseCssDeclarations((*it)[2].str());
            }
        }

        const std::wregex textElement(LR"(<\s*text\b([^>]*)>([\w\W]*?)<\s*/\s*text\s*>)", std::regex_constants::icase);
        for (std::wsregex_iterator it(svgText.begin(), svgText.end(), textElement), end; it != end; ++it)
        {
            CWin2DView::SvgTextOverlayItem item;

            const std::wstring attrText = (*it)[1].str();
            std::wstring content = (*it)[2].str();
            content = std::regex_replace(content, std::wregex(LR"(<\s*/\s*tspan\s*>)", std::regex_constants::icase), L"");
            content = std::regex_replace(content, std::wregex(LR"(<\s*tspan\b[^>]*>)", std::regex_constants::icase), L"");
            content = std::regex_replace(content, std::wregex(LR"(<[^>]+>)"), L"");
            item.text = Trim(DecodeXmlEntities(content));
            if (item.text.empty())
            {
                continue;
            }

            auto attrs = ParseXmlAttributes(attrText);
            std::unordered_map<std::wstring, std::wstring> mergedStyle;

            if (auto classIt = attrs.find(L"class"); classIt != attrs.end())
            {
                std::wstringstream classStream(classIt->second);
                std::wstring className;
                while (classStream >> className)
                {
                    if (auto declIt = classDeclarations.find(className); declIt != classDeclarations.end())
                    {
                        mergedStyle.insert(declIt->second.begin(), declIt->second.end());
                    }
                }
            }

            if (auto styleIt = attrs.find(L"style"); styleIt != attrs.end())
            {
                auto inlineStyle = ParseCssDeclarations(styleIt->second);
                mergedStyle.insert(inlineStyle.begin(), inlineStyle.end());
            }

            if (auto xIt = attrs.find(L"x"); xIt != attrs.end())
            {
                item.x = static_cast<float>(std::wcstod(xIt->second.c_str(), nullptr));
            }
            if (auto yIt = attrs.find(L"y"); yIt != attrs.end())
            {
                item.y = static_cast<float>(std::wcstod(yIt->second.c_str(), nullptr));
            }

            if (auto fillIt = attrs.find(L"fill"); fillIt != attrs.end())
            {
                item.color = ParseSvgColor(fillIt->second, item.color);
            }
            if (auto fillIt = mergedStyle.find(L"fill"); fillIt != mergedStyle.end())
            {
                item.color = ParseSvgColor(fillIt->second, item.color);
            }

            if (auto fsIt = attrs.find(L"font-size"); fsIt != attrs.end())
            {
                item.fontSize = static_cast<float>(std::wcstod(fsIt->second.c_str(), nullptr));
            }
            if (auto fsIt = mergedStyle.find(L"font-size"); fsIt != mergedStyle.end())
            {
                item.fontSize = static_cast<float>(std::wcstod(fsIt->second.c_str(), nullptr));
            }

            if (auto ffIt = attrs.find(L"font-family"); ffIt != attrs.end())
            {
                item.fontFamily = ffIt->second;
            }
            if (auto ffIt = mergedStyle.find(L"font-family"); ffIt != mergedStyle.end())
            {
                item.fontFamily = ffIt->second;
            }

            if (auto fwIt = attrs.find(L"font-weight"); fwIt != attrs.end())
            {
                item.bold = (fwIt->second == L"bold" || fwIt->second == L"700");
            }
            if (auto fwIt = mergedStyle.find(L"font-weight"); fwIt != mergedStyle.end())
            {
                item.bold = (fwIt->second == L"bold" || fwIt->second == L"700");
            }

            if (auto fontIt = mergedStyle.find(L"font"); fontIt != mergedStyle.end())
            {
                ApplyFontShorthand(fontIt->second, item.fontSize, item.bold, item.fontFamily);
            }

            if (auto taIt = attrs.find(L"text-anchor"); taIt != attrs.end())
            {
                if (taIt->second == L"middle")
                {
                    item.textAlignment = ns::mgct::CanvasHorizontalAlignment::Center;
                }
                else if (taIt->second == L"end")
                {
                    item.textAlignment = ns::mgct::CanvasHorizontalAlignment::Right;
                }
            }
            if (auto taIt = mergedStyle.find(L"text-anchor"); taIt != mergedStyle.end())
            {
                if (taIt->second == L"middle")
                {
                    item.textAlignment = ns::mgct::CanvasHorizontalAlignment::Center;
                }
                else if (taIt->second == L"end")
                {
                    item.textAlignment = ns::mgct::CanvasHorizontalAlignment::Right;
                }
            }

            item.fontFamily = NormalizeFontFamilyName(std::move(item.fontFamily));

            overlays.push_back(std::move(item));
        }

        return overlays;
    }

    std::wstring UTF8ToWide(char const* input)
    {
        if (input == nullptr || *input == '\0')
        {
            return {};
        }

        int length = ::MultiByteToWideChar(CP_UTF8, 0, input, -1, nullptr, 0);
        if (length <= 0)
        {
            return {};
        }

        std::wstring output(static_cast<size_t>(length - 1), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, input, -1, output.data(), length);
        return output;
    }

    float GetFontSize(float width)
    {
        constexpr float kMaxFontSize = 72.0f;
        constexpr float kScaleFactor = 12.0f;
        return std::min(width / kScaleFactor, kMaxFontSize);
    }

    ns::wr::com_ptr<::IDXGIDevice> GetDXGIDevice(ns::mgc::CanvasDevice& device)
    {
        ns::wr::com_ptr<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative> nativeDeviceWrapper =
            device.as<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>();

        ns::wr::com_ptr<ID2D1Device2> d2dDevice{ nullptr };
        ns::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(nullptr, 0.0f, ns::wr::guid_of<ID2D1Device2>(), d2dDevice.put_void()));

        IDXGIDevice* dxgiDeviceRaw = nullptr;
        ns::wr::check_hresult(d2dDevice->GetDxgiDevice(&dxgiDeviceRaw));

        ns::wr::com_ptr<::IDXGIDevice> dxgiDevice;
        dxgiDevice.attach(dxgiDeviceRaw);
        return dxgiDevice;
    }
}

CWin2DView::CWin2DView() noexcept
{
    ppmBitmapResolution = 72;
    transformMatrix = IdentityTransform();
    displaySyncHelper.Initialize();
    displayFrequency = displaySyncHelper.GetFrequency();
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
    KillTimer(kInertiaTimerId);

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
        transformMatrix = IdentityTransform();
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
        Redraw(
            clientWidth / 4.0f,
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

void CWin2DView::SetConsoleDebugEnabled(bool enabled) noexcept
{
    consoleDebugEnabled = enabled;
    if (::IsWindow(m_hWnd))
    {
        gConsoleStateSyncTargetWindow = ::GetAncestor(m_hWnd, GA_ROOT);
    }
    if (consoleDebugEnabled)
    {
        EnsureDebugConsole();
        std::wstringstream ss;
        ss << L"[ConsoleDebug] enabled=1 overlays=" << svgTextOverlays.size()
           << L" svgDoc=" << (svgDocument != nullptr ? 1 : 0)
           << L" dpi=" << currentDpi
           << L" viewSize=" << width << L"x" << height;
        DebugPrintLine(ss.str());
    }
    else
    {
        DebugPrintLine(L"[ConsoleDebug] enabled=0");
        ReleaseDebugConsole();
        gConsoleStateSyncTargetWindow = nullptr;
    }
}

bool CWin2DView::ShouldAnimateEffects() const noexcept
{
    return renderLayerMode != RenderLayerMode::SvgOnly;
}

void CWin2DView::DrawSvgTextOverlay(ns::mgc::CanvasDrawingSession const& session, ns::wfn::float3x2 const& transform)
{
    if (svgTextOverlays.empty())
    {
        if (consoleDebugEnabled && lastLoggedTextOverlayCount != 0)
        {
            lastLoggedTextOverlayCount = 0;
            lastLoggedTextOverlayFailures = 0;
            DebugPrintLine(L"[SvgTextOverlay] total=0 drawn=0 failed=0");
        }
        return;
    }

    session.Transform(transform);
    auto resourceCreator = session.as<ns::mgc::ICanvasResourceCreator>();
    int drawnCount = 0;
    int failedCount = 0;

    for (auto const& item : svgTextOverlays)
    {
        auto drawWithFont = [&](std::wstring const& fontFamily) -> bool
        {
            try
            {
                ns::mgct::CanvasTextFormat textFormat;
                textFormat.FontFamily(ns::wr::hstring(fontFamily));
                textFormat.FontSize(item.fontSize);
                textFormat.HorizontalAlignment(ns::mgct::CanvasHorizontalAlignment::Left);
                if (item.bold)
                {
                    textFormat.FontWeight(ns::wut::FontWeights::Bold());
                }

                constexpr float kMaxLayout = 10000.0f;
                ns::mgct::CanvasTextLayout textLayout(resourceCreator, ns::wr::hstring(item.text), textFormat, kMaxLayout, kMaxLayout);
                auto bounds = textLayout.LayoutBounds();

                float drawX = item.x;
                if (item.textAlignment == ns::mgct::CanvasHorizontalAlignment::Center)
                {
                    drawX -= bounds.Width / 2.0f;
                }
                else if (item.textAlignment == ns::mgct::CanvasHorizontalAlignment::Right)
                {
                    drawX -= bounds.Width;
                }

                const float drawY = item.y - (bounds.Y + bounds.Height);
                session.DrawText(ns::wr::hstring(item.text), ns::wfn::float2(drawX, drawY), item.color, textFormat);
                return true;
            }
            catch (ns::wr::hresult_error const&)
            {
                return false;
            }
        };

        if (drawWithFont(item.fontFamily))
        {
            ++drawnCount;
            continue;
        }
        if (drawWithFont(L"Microsoft YaHei"))
        {
            ++drawnCount;
            continue;
        }
        if (drawWithFont(L"Segoe UI"))
        {
            ++drawnCount;
        }
        else
        {
            ++failedCount;
        }
    }

    if (consoleDebugEnabled)
    {
        const int totalCount = static_cast<int>(svgTextOverlays.size());
        if (totalCount != lastLoggedTextOverlayCount || failedCount != lastLoggedTextOverlayFailures)
        {
            lastLoggedTextOverlayCount = totalCount;
            lastLoggedTextOverlayFailures = failedCount;
            std::wstringstream ss;
            ss << L"[SvgTextOverlay] total=" << totalCount
               << L" drawn=" << drawnCount
               << L" failed=" << failedCount
               << L" transform=[" << transform.m11 << L"," << transform.m12 << L"," << transform.m21
               << L"," << transform.m22 << L"," << transform.m31 << L"," << transform.m32 << L"]";
            DebugPrintLine(ss.str());
        }
    }
}

ns::wud::DesktopWindowTarget CWin2DView::CreateDesktopWindowTarget(ns::wuc::Compositor const& compositor, HWND window)
{
    namespace abi = ABI::Windows::UI::Composition::Desktop;

    auto interop = compositor.as<abi::ICompositorDesktopInterop>();
    ns::wud::DesktopWindowTarget target{ nullptr };
    ns::wr::check_hresult(interop->CreateDesktopWindowTarget(window, true, reinterpret_cast<abi::IDesktopWindowTarget**>(ns::wr::put_abi(target))));
    return target;
}

void CWin2DView::PrepareVisuals(ns::wuc::Compositor const& compositor)
{
    target = CreateDesktopWindowTarget(compositor, m_hWnd);

    root = compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({ 1.0f, 1.0f });

    contentVisual = compositor.CreateSpriteVisual();
    contentVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });

    root.Children().InsertAtTop(contentVisual);

    target.Root(root);
}

void CWin2DView::OnDirect3DDeviceLost(DeviceLostHelper const*, DeviceLostEventArgs const&)
{
    inDeviceLost = true;
    renderTimer.stop();

    auto canvasDevice = ns::mgc::CanvasDevice::GetSharedDevice();
    ns::wr::com_ptr<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop> graphicsDeviceInterop{
        graphicsDevice.as<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>()
    };

    ns::wr::com_ptr<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative> nativeDeviceWrapper =
        canvasDevice.as<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>();
    ns::wr::com_ptr<ID2D1Device2> d2dDevice{ nullptr };
    ns::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(nullptr, 0.0f, ns::wr::guid_of<ID2D1Device2>(), d2dDevice.put_void()));
    ns::wr::check_hresult(graphicsDeviceInterop->SetRenderingDevice(d2dDevice.get()));

    drawingSurface = nullptr;
    sceneBitmap = nullptr;
    trailBitmap = nullptr;
    if (svgDocument != nullptr)
    {
        svgDocument.Close();
        svgDocument = nullptr;
    }

    inDeviceLost = false;
    ScenarioWin2D(compositor, root, currentDpi, width, height);
}

void CWin2DView::ScenarioWin2D(ns::wuc::Compositor const& compositor, ns::wuc::ContainerVisual const& root, UINT dpi, int cx, int cy)
{
    if (inDeviceLost || cx <= 0 || cy <= 0)
    {
        return;
    }

    if (width != cx || height != cy || drawingSurface == nullptr)
    {
        renderTimer.stop();
        width = cx;
        height = cy;

        try
        {
            if (contentVisual == nullptr)
            {
                contentVisual = compositor.CreateSpriteVisual();
                contentVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
                root.Children().InsertAtTop(contentVisual);
            }

            if (drawingSurface != nullptr)
            {
                drawingSurface.Resize(ns::wg::SizeInt32{ width, height });
                contentVisual.Brush(compositor.CreateSurfaceBrush(drawingSurface));
            }

            if (drawingSurface == nullptr)
            {
                canvasDevice = ns::mgc::CanvasDevice::GetSharedDevice();
                auto dxgiDevice = GetDXGIDevice(canvasDevice);
                deviceLostHelper.WatchDevice(dxgiDevice);
                deviceLostHelper.DeviceLost({ this, &CWin2DView::OnDirect3DDeviceLost });

                if (graphicsDevice == nullptr)
                {
                    graphicsDevice = ns::mgcu::CanvasComposition::CreateCompositionGraphicsDevice(compositor, canvasDevice);
                }

                drawingSurface = graphicsDevice.CreateDrawingSurface(
                    ns::wf::Size(static_cast<float>(width), static_cast<float>(height)),
                    ns::wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    ns::wgd::DirectXAlphaMode::Premultiplied);

                contentVisual.Brush(compositor.CreateSurfaceBrush(drawingSurface));
            }

            sceneBitmap = nullptr;
            trailBitmap = nullptr;
            CreateFlameEffect();
            displayText.clear();

            Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f, static_cast<float>(width), static_cast<float>(height), dpi);
        }
        catch (ns::wr::hresult_error const&)
        {
            renderTimer.stop();
        }
    }

    if (ShouldAnimateEffects())
    {
        renderTimer.start(1000.0 / 60.0, [this, dpi]()
        {
            if (!::IsWindow(m_hWnd))
            {
                return false;
            }
            if (!renderTickQueued.exchange(true, std::memory_order_acq_rel))
            {
                ::PostMessage(m_hWnd, kRenderTickMsg, static_cast<WPARAM>(dpi), 0);
            }
            return true;
        });
    }
    else
    {
        renderTimer.stop();
    }
}

bool CWin2DView::Redraw(float cx, float cy, float wx, float wy, float width, float height, UINT dpi, CRect* clipRect)
{
    (void)cx;
    (void)cy;
    (void)wx;
    (void)wy;
    (void)dpi;

    if (drawingSurface == nullptr)
    {
        return false;
    }

    try
    {
        auto clearColor = ns::wui::Colors::Black();
        clearColor.A = 0;

        auto renderEffectsFrame = [&](ns::mgc::ICanvasResourceCreator const& resourceCreator, float sceneWidth, float sceneHeight)
        {
            bool newBitmap = false;
            bool recreateBitmap = sceneBitmap == nullptr;
            if (!recreateBitmap)
            {
                const auto size = sceneBitmap.Size();
                if (std::fabs(size.Width - sceneWidth) > 0.5f ||
                    std::fabs(size.Height - sceneHeight) > 0.5f)
                {
                    recreateBitmap = true;
                    sceneBitmap = nullptr;
                }
            }
            if (recreateBitmap)
            {
                sceneBitmap = ns::mgc::CanvasRenderTarget(resourceCreator, sceneWidth, sceneHeight, kPixelsDpi);
                trailBitmap = ns::mgc::CanvasRenderTarget(resourceCreator, sceneWidth, sceneHeight, kPixelsDpi);
                newBitmap = true;
            }
            else if (trailBitmap == nullptr)
            {
                trailBitmap = ns::mgc::CanvasRenderTarget(resourceCreator, sceneWidth, sceneHeight, kPixelsDpi);
                newBitmap = true;
            }

            auto drawingSession = sceneBitmap.CreateDrawingSession();
            drawingSession.Clear(clearColor);
            if (!newBitmap && trailBitmap != nullptr)
            {
                ns::mgce::OpacityEffect fadedTrail;
                fadedTrail.Source(trailBitmap);
                fadedTrail.Opacity(0.90f);
                drawingSession.DrawImage(fadedTrail);
            }

            const float blockW = std::min(300.0f, sceneWidth * 0.35f);
            const float blockH = std::min(300.0f, sceneHeight * 0.35f);
            const float blockX = sceneWidth * 0.18f;
            const float blockY = sceneHeight * 0.20f;

            ns::wfn::float2 center(sceneWidth * 0.5f, sceneHeight * 0.5f);
            drawingSession.Transform(ns::wfn::make_float3x2_rotation(static_cast<float>(angle * kPi / 180.0), center) * drawingSession.Transform());

            drawingSession.FillRectangle(ns::wf::Rect{ blockX, blockY, blockW, blockH }, ns::wui::Colors::Red());
            drawingSession.FillRectangle(ns::wf::Rect{ blockX + blockW, blockY + blockH, blockW, blockH }, ns::wui::Colors::Green());

            ns::mgct::CanvasTextFormat textFormat;
            textFormat.FontSize(angle / 2 + 1);

            const ns::wr::hstring message{ L"Hello Win2D in WTL!" };
            const ns::wf::Rect textRect{ 0, 0, sceneWidth, sceneHeight };
            ns::mgc::CanvasRenderTarget textBitmap(resourceCreator, sceneWidth, sceneHeight, kPixelsDpi);
            auto textSession = textBitmap.CreateDrawingSession();
            auto textClear = ns::wui::Colors::Black();
            textClear.A = 0;
            textSession.Clear(textClear);
            textSession.DrawText(message, textRect, ns::wui::Colors::Blue(), textFormat);
            textSession.Close();

            ns::mgce::GaussianBlurEffect blur;
            blur.BlurAmount(5);
            blur.Source(textBitmap);
            drawingSession.DrawImage(blur);

            const auto newFontSize = GetFontSize(sceneWidth);
            if (pendingText != displayText || newFontSize != fontSize)
            {
                displayText = pendingText;
                fontSize = newFontSize;
                SetupText(resourceCreator);
            }

            ConfigureEffect();
            drawingSession.DrawImage(compositeEffect, ns::wfn::float2(sceneWidth / 2.0f, sceneHeight / 2.0f));
            drawingSession.Close();

            if (trailBitmap != nullptr)
            {
                auto trailSession = trailBitmap.CreateDrawingSession();
                trailSession.Clear(clearColor);
                trailSession.DrawImage(sceneBitmap);
                trailSession.Close();
            }
        };

        const bool hasSvg = (svgDocument != nullptr);
        const bool renderEffectsOnly = renderLayerMode == RenderLayerMode::EffectsOnly;
        if (document == nullptr || document->Empty() || renderEffectsOnly)
        {
            ns::mgc::CanvasDrawingSession rootSession{ nullptr };
            auto outputTransform = IdentityTransform();
            if (clipRect == nullptr)
            {
                ns::wf::Rect updateRect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
                rootSession = ns::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface, updateRect);
                rootSession.Clear(clearColor);
            }
            else
            {
                ns::wf::Rect updateRect(
                    static_cast<float>(clipRect->left),
                    static_cast<float>(clipRect->top),
                    static_cast<float>(clipRect->Width()),
                    static_cast<float>(clipRect->Height()));
                rootSession = ns::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface, updateRect);
                if (!(hasSvg && renderEffectsOnly))
                {
                    outputTransform.m31 -= static_cast<float>(clipRect->left);
                    outputTransform.m32 -= static_cast<float>(clipRect->top);
                }
            }

            float sceneWidth = std::max(1.0f, width);
            float sceneHeight = std::max(1.0f, height);
            if (!(hasSvg && renderEffectsOnly))
            {
                outputTransform.m31 += transformMatrix.m31;
                outputTransform.m32 += transformMatrix.m32;

                const auto total = GetTotalSize();
                sceneWidth = std::max<float>(static_cast<float>(total.cx), width);
                sceneHeight = std::max<float>(static_cast<float>(total.cy), height);
            }

            auto resourceCreator = rootSession.as<ns::mgc::ICanvasResourceCreator>();
            renderEffectsFrame(resourceCreator, sceneWidth, sceneHeight);

            rootSession.Transform(outputTransform);
            rootSession.DrawImage(sceneBitmap);
            rootSession.Close();
            return true;
        }

        if (svgDocument != nullptr)
        {
            ns::mgc::CanvasDrawingSession session = nullptr;
            auto transform = transformMatrix;

            if (clipRect == nullptr)
            {
                ns::wf::Rect updateRect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
                session = ns::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface, updateRect);
            }
            else
            {
                ns::wf::Rect updateRect(
                    static_cast<float>(clipRect->left),
                    static_cast<float>(clipRect->top),
                    static_cast<float>(clipRect->Width()),
                    static_cast<float>(clipRect->Height()));
                session = ns::mgcu::CanvasComposition::CreateDrawingSession(drawingSurface, updateRect);
                transform.m31 -= static_cast<float>(clipRect->left);
                transform.m32 -= static_cast<float>(clipRect->top);
            }

            session.Antialiasing(ns::mgc::CanvasAntialiasing::Antialiased);

            ns::wr::com_ptr<ID2D1RenderTarget> target{ nullptr };
            if (clipRect != nullptr)
            {
                ns::wr::com_ptr<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative> nativeDeviceWrapper =
                    session.as<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>();
                ns::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(nullptr, 0.0f, ns::wr::guid_of<ID2D1RenderTarget>(), target.put_void()));

                D2D1_RECT_F clip{};
                clip.left = 0;
                clip.top = 0;
                clip.right = static_cast<float>(clipRect->Width());
                clip.bottom = static_cast<float>(clipRect->Height());
                target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            }

            auto transparent = ns::wui::Colors::Black();
            transparent.A = 0;
            session.Clear(transparent);

            const bool drawSvg = renderLayerMode != RenderLayerMode::EffectsOnly;
            const bool drawEffects = renderLayerMode != RenderLayerMode::SvgOnly;
            const float effectWidth = std::max(1.0f, width);
            const float effectHeight = std::max(1.0f, height);

            if (drawEffects)
            {
                auto resourceCreator = session.as<ns::mgc::ICanvasResourceCreator>();
                renderEffectsFrame(resourceCreator, effectWidth, effectHeight);
            }

            if (renderLayerMode == RenderLayerMode::SvgOverEffects && drawEffects)
            {
                session.Transform(IdentityTransform());
                session.DrawImage(sceneBitmap);
            }

            if (drawSvg)
            {
                session.Transform(transform);
                session.DrawSvg(svgDocument, ns::wf::Size(static_cast<float>(width), static_cast<float>(height)));
                DrawSvgTextOverlay(session, transform);
            }

            if (renderLayerMode == RenderLayerMode::EffectsOverSvg && drawEffects)
            {
                session.Transform(IdentityTransform());
                session.DrawImage(sceneBitmap);
            }

            if (clipRect != nullptr)
            {
                target->PopAxisAlignedClip();
            }

            session.Close();
        }
    }
    catch (ns::wr::hresult_error const&)
    {
        return false;
    }

    return true;
}

bool CWin2DView::LoadSvg()
{
    if (document == nullptr || document->Empty())
    {
        return false;
    }

    std::wstring text;
    auto const& bytes = document->GetSvgXml();
    auto const* rawBytes = reinterpret_cast<unsigned char const*>(bytes.data());
    if (bytes.size() >= 2 && rawBytes[0] == 0xFF && rawBytes[1] == 0xFE)
    {
        auto const* wideText = reinterpret_cast<wchar_t const*>(bytes.data() + 2);
        text.assign(wideText, wideText + ((bytes.size() - 2) / sizeof(wchar_t)));
    }
    else
    {
        text = UTF8ToWide(bytes.data());
        text = ReplaceString(text, L"encoding=\"utf-8", L"encoding=\"utf-16");
        text = ReplaceString(text, L"encoding=\"UTF-8", L"encoding=\"UTF-16");
    }

    svgTextOverlays = ParseSvgTextOverlays(text);
    if (consoleDebugEnabled)
    {
        int textTagCount = 0;
        {
            std::wstring lowered = text;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), towlower);
            size_t pos = 0;
            while ((pos = lowered.find(L"<text", pos)) != std::wstring::npos)
            {
                ++textTagCount;
                pos += 5;
            }
        }

        std::wstringstream ss;
        ss << L"[LoadSvg] xmlChars=" << text.size()
           << L" textTags=" << textTagCount
           << L" overlaysParsed=" << svgTextOverlays.size();
        DebugPrintLine(ss.str());

        const size_t previewCount = std::min<size_t>(svgTextOverlays.size(), 5);
        for (size_t i = 0; i < previewCount; ++i)
        {
            auto const& item = svgTextOverlays[i];
            std::wstring previewText = item.text.substr(0, std::min<size_t>(item.text.size(), 32));
            std::wstringstream itemLog;
            itemLog << L"  [Text#" << i << L"] x=" << item.x
                    << L" y=" << item.y
                    << L" font=" << item.fontFamily.c_str()
                    << L" size=" << item.fontSize
                    << L" bold=" << (item.bold ? 1 : 0)
                    << L" text=" << previewText.c_str();
            DebugPrintLine(itemLog.str());
        }
    }
    text = InlineSvgClassStyles(std::move(text));

    svgXml = ns::wr::hstring(text);

    try
    {
        if (svgDocument != nullptr)
        {
            svgDocument.Close();
        }

        auto canvasDevice = ns::mgc::CanvasDevice::GetSharedDevice();
        svgDocument = ns::mgcs::CanvasSvgDocument::LoadFromXml(canvasDevice, svgXml);
    }
    catch (ns::wr::hresult_error const& ex)
    {
        std::wstring message = L"Error loading SVG:\n";
        message += ex.message();
        ::MessageBoxW(m_hWnd, message.c_str(), LoadAppString(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONERROR);

        if (document != nullptr)
        {
            document->Clear();
        }
        return false;
    }

    if (svgDocument == nullptr)
    {
        if (consoleDebugEnabled)
        {
            DebugPrintLine(L"[LoadSvg] svgDocument is null after LoadFromXml.");
        }
        return false;
    }

    auto rootElement = svgDocument.Root();
    if (rootElement)
    {
        try
        {
            svgDocumentWidth = rootElement.GetFloatAttribute(L"width");
            svgDocumentHeight = rootElement.GetFloatAttribute(L"height");
        }
        catch (ns::wr::hresult_error const&)
        {
            try
            {
                auto viewBox = rootElement.GetRectangleAttribute(L"viewBox");
                svgDocumentWidth = viewBox.Width;
                svgDocumentHeight = viewBox.Height;
            }
            catch (ns::wr::hresult_error const&)
            {
                svgDocumentWidth = static_cast<float>(width);
                svgDocumentHeight = static_cast<float>(height);
            }
        }
    }

    SetScrollSizes(MM_TEXT, CSize(static_cast<int>(svgDocumentWidth), static_cast<int>(svgDocumentHeight)));
    transformMatrix = IdentityTransform();
    ScrollToPosition(CPoint(0, 0));
    if (consoleDebugEnabled)
    {
        std::wstringstream ss;
        ss << L"[LoadSvg] docSize=" << svgDocumentWidth << L"x" << svgDocumentHeight
           << L" currentView=" << width << L"x" << height
           << L" dpi=" << currentDpi;
        DebugPrintLine(ss.str());
    }
    return true;
}

void CWin2DView::CreateFlameEffect()
{
    morphologyEffect = ns::mgce::MorphologyEffect();
    morphologyEffect.Mode(ns::mgce::MorphologyEffectMode::Dilate);
    morphologyEffect.Width(7);
    morphologyEffect.Height(1);

    auto blur = ns::mgce::GaussianBlurEffect();
    blur.Source(morphologyEffect);
    blur.BlurAmount(3.0f);

    ns::mgce::Matrix5x4 colorMatrix{};
    colorMatrix.M42 = 1.0f;
    colorMatrix.M44 = 1.0f;
    colorMatrix.M51 = 1.0f;
    colorMatrix.M52 = -0.5f;

    auto colorize = ns::mgce::ColorMatrixEffect();
    colorize.Source(blur);
    colorize.ColorMatrix(colorMatrix);

    ns::mgce::TurbulenceEffect turbulence;
    turbulence.Frequency(ns::wfn::float2(0.109f, 0.109f));
    turbulence.Size(ns::wfn::float2(500.0f, 80.0f));

    ns::mgce::BorderEffect border;
    border.Source(turbulence);
    border.ExtendX(ns::mgc::CanvasEdgeBehavior::Mirror);
    border.ExtendY(ns::mgc::CanvasEdgeBehavior::Mirror);

    flameAnimation = ns::mgce::Transform2DEffect();
    flameAnimation.Source(border);

    ns::mgce::DisplacementMapEffect displacement;
    displacement.Source(colorize);
    displacement.Displacement(flameAnimation);
    displacement.Amount(40.0f);

    flamePosition = ns::mgce::Transform2DEffect();
    flamePosition.Source(displacement);

    compositeEffect = ns::mgce::CompositeEffect();
    compositeEffect.Sources().Append(flamePosition);
    compositeEffect.Sources().Append(nullptr);
}

void CWin2DView::SetupText(ns::mgc::ICanvasResourceCreator resourceCreator)
{
    ns::mgc::CanvasCommandList textCommandList(resourceCreator);
    auto drawingSession = textCommandList.CreateDrawingSession();
    drawingSession.Clear(ns::wui::Color{ 0, 0, 0, 0 });

    ns::mgct::CanvasTextFormat textFormat;
    textFormat.FontFamily(L"Segoe UI");
    textFormat.FontSize(fontSize);
    textFormat.HorizontalAlignment(ns::mgct::CanvasHorizontalAlignment::Center);
    textFormat.VerticalAlignment(ns::mgct::CanvasVerticalAlignment::Top);

    drawingSession.DrawText(ns::wr::to_hstring(displayText), 0, 0, ns::wui::Colors::White(), textFormat);
    drawingSession.Close();

    morphologyEffect.Source(textCommandList);
    compositeEffect.Sources().SetAt(1, textCommandList);
}

void CWin2DView::ConfigureEffect()
{
    flameAnimation.TransformMatrix(ns::wfn::make_float3x2_translation(0, -((60.0f * static_cast<float>(::clock())) / CLOCKS_PER_SEC)));
    const float verticalOffset = fontSize * 1.4f;
    flamePosition.TransformMatrix(ns::wfn::make_float3x2_scale(1, 2, ns::wfn::float2(0, verticalOffset)));
}

void CWin2DView::SurfaceScroll(CPoint const& newPosition)
{
    const int dx = newPosition.x - static_cast<int>(-transformMatrix.m31);
    const int dy = newPosition.y - static_cast<int>(-transformMatrix.m32);
    if ((dx != 0 || dy != 0) && drawingSurface != nullptr)
    {
        drawingSurface.Scroll(ns::wg::PointInt32{ -dx, -dy });
    }
}

void CWin2DView::Zoom(int zDelta, CPoint screenPoint)
{
    KillTimer(kInertiaTimerId);

    const int dpi25 = std::max(1, (25 * currentDpi) / 96);
    ppmBitmapResolution -= zDelta / dpi25;
    ppmBitmapResolution = std::clamp(
        ppmBitmapResolution,
        std::max(36, (36 * currentDpi) / 96),
        std::max(72, (kMaxBitmapResolution * currentDpi) / 96));

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

    CSize scaledTotal(
        std::clamp(static_cast<int>(svgDocumentWidth * scale), 0, 100000000),
        std::clamp(static_cast<int>(svgDocumentHeight * scale), 0, 100000000));

    CPoint scaledAnchor(
        RoundToInt(fracX * scaledTotal.cx),
        RoundToInt(fracY * scaledTotal.cy));

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

void CWin2DView::DrawClientRect(RECT& rect)
{
    if (svgDocument == nullptr)
    {
        return;
    }

    CRect clip(rect);
    if (!clip.IsRectEmpty() && width > 0 && height > 0)
    {
        Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f, static_cast<float>(width), static_cast<float>(height), currentDpi, &clip);
    }
}

void CWin2DView::OnScrollPositionChanging(CPoint /*oldPos*/, CPoint newPos)
{
    if (svgDocument != nullptr)
    {
        SurfaceScroll(newPos);
    }

    transformMatrix.m31 = static_cast<float>(-newPos.x);
    transformMatrix.m32 = static_cast<float>(-newPos.y);
}

LRESULT CWin2DView::OnCreate(UINT, WPARAM, LPARAM, BOOL&)
{
    AttachScrollWindow(m_hWnd);
    PrepareVisuals(compositor);

    currentDpi = GetDpiForWindow(m_hWnd);
    ppmBitmapResolution = (72 * currentDpi) / 96;
    pendingText = "Win2D in win32 desktop C++ with WTL";

    CreateFlameEffect();
    return 0;
}

LRESULT CWin2DView::OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
{
    renderTimer.stop();
    renderTickQueued.store(false, std::memory_order_release);
    KillTimer(kInertiaTimerId);
    drawingSurface = nullptr;
    sceneBitmap = nullptr;
    trailBitmap = nullptr;
    if (svgDocument != nullptr)
    {
        svgDocument.Close();
        svgDocument = nullptr;
    }
    consoleDebugEnabled = false;
    ReleaseDebugConsole();
    return 0;
}

LRESULT CWin2DView::OnSize(UINT, WPARAM wParam, LPARAM lParam, BOOL&)
{
    const UINT sizeType = static_cast<UINT>(wParam);
    if (sizeType == SIZE_MINIMIZED)
    {
        renderTimer.stop();
        renderTickQueued.store(false, std::memory_order_release);
        return 0;
    }

    currentDpi = GetDpiForWindow(m_hWnd);

    const int cx = std::max(1, GET_X_LPARAM(lParam));
    const int cy = std::max(1, GET_Y_LPARAM(lParam));
    UpdateScrollMetrics();

    if (svgDocument == nullptr)
    {
        SetScrollSizes(MM_TEXT, CSize(cx, cy));
    }

    if (cx > 0 && cy > 0)
    {
        ScenarioWin2D(compositor, root, currentDpi, cx, cy);
    }

    return 0;
}

LRESULT CWin2DView::OnPaint(UINT, WPARAM, LPARAM, BOOL&)
{
    PAINTSTRUCT ps{};
    BeginPaint(&ps);
    EndPaint(&ps);

    if (svgDocument != nullptr && width > 0 && height > 0 && drawingSurface != nullptr)
    {
        CRect clip(ps.rcPaint);
        Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f, static_cast<float>(width), static_cast<float>(height), currentDpi, &clip);
    }

    return 0;
}

LRESULT CWin2DView::OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&)
{
    return 1;
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
    Zoom((GET_WHEEL_DELTA_WPARAM(wParam) < 0 ? 1 : -1) * ppmBitmapResolution * 25 / 4, CPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
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
            displaySyncHelper.WaitForVSync();
            SetTimer(kInertiaTimerId, interval);
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
    if (wParam != kInertiaTimerId)
    {
        return 0;
    }

    CPoint oldPos = GetScrollPosition();
    CPoint newPos = oldPos + scrollDiff;
    newPos = ClampScrollPosition(newPos);

    if (gDampScrolling)
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

    displaySyncHelper.WaitForVSync();
    ScrollToPosition(newPos);

    if (GetScrollPosition() == oldPos || scrollDiff == CSize(0, 0))
    {
        scrollDiff = CSize(0, 0);
        KillTimer(kInertiaTimerId);
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
                    const double scaledDelta =
                        (delta < 0 ? 1.0 : -1.0) *
                        static_cast<double>(ppmBitmapResolution) *
                        25.0 *
                        (std::abs(delta) / 120.0) / 4.0;
                    Zoom(static_cast<int>(scaledDelta), CPoint(gestureInfo.ptsLocation.x, gestureInfo.ptsLocation.y));
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

LRESULT CWin2DView::OnRenderTick(UINT, WPARAM wParam, LPARAM, BOOL&)
{
    renderTickQueued.store(false, std::memory_order_release);

    const HWND rootWindow = ::GetAncestor(m_hWnd, GA_ROOT);
    if (rootWindow != nullptr && ::IsIconic(rootWindow))
    {
        return 0;
    }

    if (!ShouldAnimateEffects() || drawingSurface == nullptr || width <= 0 || height <= 0)
    {
        return 0;
    }

    const UINT dpi = wParam != 0 ? static_cast<UINT>(wParam) : static_cast<UINT>(currentDpi);
    if (Redraw(width / 4.0f, height / 4.0f, 300.0f, 300.0f, static_cast<float>(width), static_cast<float>(height), dpi))
    {
        angle += 1.0f;
    }

    return 0;
}

