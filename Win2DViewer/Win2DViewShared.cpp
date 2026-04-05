#include "pch.h"

#include <cstdio>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <d3d11_4.h>

#include <ShellScalingAPI.h>
#include <DispatcherQueue.h>
#include <Windows.Graphics.Interop.h>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include "Win2DViewInternal.h"

namespace Win2DViewInternal
{
    DisplaySyncHelper gDisplaySyncHelper;
    bool gDampScrolling = true;

    Win2DViewNs::wfn::float3x2 IdentityTransform()
    {
        return Win2DViewNs::wfn::float3x2{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
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

        if (fontFamily.size() >= 2 && ((fontFamily.front() == L'"' && fontFamily.back() == L'"') ||
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
        ConsoleDebugLifecycle::EnsureDebugConsole();
    }

    void ReleaseDebugConsole()
    {
        ConsoleDebugLifecycle::ReleaseDebugConsole();
    }

    void DebugPrintLine(std::wstring const& line)
    {
        ConsoleDebugLifecycle::DebugPrintLine(line);
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
        auto EscapeAttributeValue = [](std::wstring value) {
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
            std::wstring item =
                declarationBlock.substr(pos, semi == std::wstring::npos ? std::wstring::npos : semi - pos);
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

    Win2DViewNs::wui::Color ParseSvgColor(std::wstring const& value, Win2DViewNs::wui::Color fallback)
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
                auto hex = [](wchar_t c) -> uint8_t {
                    if (c >= L'0' && c <= L'9')
                        return static_cast<uint8_t>(c - L'0');
                    if (c >= L'a' && c <= L'f')
                        return static_cast<uint8_t>(10 + c - L'a');
                    if (c >= L'A' && c <= L'F')
                        return static_cast<uint8_t>(10 + c - L'A');
                    return 0;
                };
                return Win2DViewNs::wui::Color{ 255,
                                                static_cast<uint8_t>(hex(color[1]) * 17),
                                                static_cast<uint8_t>(hex(color[2]) * 17),
                                                static_cast<uint8_t>(hex(color[3]) * 17) };
            }
            if (color.size() == 7)
            {
                const auto toByte = [&](size_t i) -> uint8_t {
                    return static_cast<uint8_t>(std::wcstol(color.substr(i, 2).c_str(), nullptr, 16));
                };
                return Win2DViewNs::wui::Color{ 255, toByte(1), toByte(3), toByte(5) };
            }
        }

        if (color == L"black")
        {
            return Win2DViewNs::wui::Color{ 255, 0, 0, 0 };
        }
        if (color == L"white")
        {
            return Win2DViewNs::wui::Color{ 255, 255, 255, 255 };
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

        // Use (.|\\n|\\r) instead of [\\w\\W] for MSVC std::wregex stability with
        // Unicode punctuation in mixed CJK text content.
        const std::wregex textElement(LR"(<\s*text\b([^>]*)>((.|\n|\r)*?)<\s*/\s*text\s*>)",
                                      std::regex_constants::icase);
        for (std::wsregex_iterator it(svgText.begin(), svgText.end(), textElement), end; it != end; ++it)
        {
            CWin2DView::SvgTextOverlayItem item;

            const std::wstring attrText = (*it)[1].str();
            std::wstring content = (*it)[2].str();
            content =
                std::regex_replace(content, std::wregex(LR"(<\s*/\s*tspan\s*>)", std::regex_constants::icase), L"");
            content =
                std::regex_replace(content, std::wregex(LR"(<\s*tspan\b[^>]*>)", std::regex_constants::icase), L"");
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
                    item.textAlignment = Win2DViewNs::mgct::CanvasHorizontalAlignment::Center;
                }
                else if (taIt->second == L"end")
                {
                    item.textAlignment = Win2DViewNs::mgct::CanvasHorizontalAlignment::Right;
                }
            }
            if (auto taIt = mergedStyle.find(L"text-anchor"); taIt != mergedStyle.end())
            {
                if (taIt->second == L"middle")
                {
                    item.textAlignment = Win2DViewNs::mgct::CanvasHorizontalAlignment::Center;
                }
                else if (taIt->second == L"end")
                {
                    item.textAlignment = Win2DViewNs::mgct::CanvasHorizontalAlignment::Right;
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

    Win2DViewNs::wr::com_ptr<::IDXGIDevice> GetDXGIDevice(Win2DViewNs::mgc::CanvasDevice& device)
    {
        Win2DViewNs::wr::com_ptr<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative> nativeDeviceWrapper =
            device.as<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>();

        Win2DViewNs::wr::com_ptr<ID2D1Device2> d2dDevice{ nullptr };
        Win2DViewNs::wr::check_hresult(nativeDeviceWrapper->GetNativeResource(
            nullptr, 0.0f, Win2DViewNs::wr::guid_of<ID2D1Device2>(), d2dDevice.put_void()));

        Win2DViewNs::wr::com_ptr<::IDXGIDevice> dxgiDevice;
        Win2DViewNs::wr::check_hresult(d2dDevice->GetDxgiDevice(dxgiDevice.put()));
        return dxgiDevice;
    }
} // namespace Win2DViewInternal
