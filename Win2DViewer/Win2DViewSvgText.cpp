#include "pch.h"

#include "Win2DViewInternal.h"
#include "Win2DViewer.h"

void CWin2DView::DrawSvgTextOverlay(Win2DViewNs::mgc::CanvasDrawingSession const& session,
                                    Win2DViewNs::wfn::float3x2 const& transform)
{
    if (svgTextOverlays.empty())
    {
        if (consoleDebugEnabled && lastLoggedTextOverlayCount != 0)
        {
            lastLoggedTextOverlayCount = 0;
            lastLoggedTextOverlayFailures = 0;
            Win2DViewInternal::DebugPrintLine(L"[SvgTextOverlay] total=0 drawn=0 failed=0");
        }
        return;
    }

    session.Transform(transform);
    auto resourceCreator = session.as<Win2DViewNs::mgc::ICanvasResourceCreator>();
    int drawnCount = 0;
    int failedCount = 0;

    for (auto const& item : svgTextOverlays)
    {
        auto drawWithFont = [&](std::wstring const& fontFamily) -> bool {
            try
            {
                Win2DViewNs::mgct::CanvasTextFormat textFormat;
                textFormat.FontFamily(Win2DViewNs::wr::hstring(fontFamily));
                textFormat.FontSize(item.fontSize);
                textFormat.HorizontalAlignment(Win2DViewNs::mgct::CanvasHorizontalAlignment::Left);
                if (item.bold)
                {
                    textFormat.FontWeight(Win2DViewNs::wut::FontWeights::Bold());
                }

                constexpr float kMaxLayout = 10000.0f;
                Win2DViewNs::mgct::CanvasTextLayout textLayout(
                    resourceCreator, Win2DViewNs::wr::hstring(item.text), textFormat, kMaxLayout, kMaxLayout);
                auto bounds = textLayout.LayoutBounds();

                float drawX = item.x;
                if (item.textAlignment == Win2DViewNs::mgct::CanvasHorizontalAlignment::Center)
                {
                    drawX -= bounds.Width / 2.0f;
                }
                else if (item.textAlignment == Win2DViewNs::mgct::CanvasHorizontalAlignment::Right)
                {
                    drawX -= bounds.Width;
                }

                const float drawY = item.y - (bounds.Y + bounds.Height);
                session.DrawText(Win2DViewNs::wr::hstring(item.text),
                                 Win2DViewNs::wfn::float2(drawX, drawY),
                                 item.color,
                                 textFormat);
                return true;
            }
            catch (Win2DViewNs::wr::hresult_error const&)
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
            ss << L"[SvgTextOverlay] total=" << totalCount << L" drawn=" << drawnCount << L" failed=" << failedCount
               << L" transform=[" << transform.m11 << L"," << transform.m12 << L"," << transform.m21 << L","
               << transform.m22 << L"," << transform.m31 << L"," << transform.m32 << L"]";
            Win2DViewInternal::DebugPrintLine(ss.str());
        }
    }
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
        text = Win2DViewInternal::UTF8ToWide(bytes.data());
        text = Win2DViewInternal::ReplaceString(text, L"encoding=\"utf-8", L"encoding=\"utf-16");
        text = Win2DViewInternal::ReplaceString(text, L"encoding=\"UTF-8", L"encoding=\"UTF-16");
    }

    svgTextOverlays = Win2DViewInternal::ParseSvgTextOverlays(text);
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
        ss << L"[LoadSvg] xmlChars=" << text.size() << L" textTags=" << textTagCount << L" overlaysParsed="
           << svgTextOverlays.size();
        Win2DViewInternal::DebugPrintLine(ss.str());

        const size_t previewCount = std::min<size_t>(svgTextOverlays.size(), 5);
        for (size_t i = 0; i < previewCount; ++i)
        {
            auto const& item = svgTextOverlays[i];
            std::wstring previewText = item.text.substr(0, std::min<size_t>(item.text.size(), 32));
            std::wstringstream itemLog;
            itemLog << L"  [Text#" << i << L"] x=" << item.x << L" y=" << item.y << L" font=" << item.fontFamily.c_str()
                    << L" size=" << item.fontSize << L" bold=" << (item.bold ? 1 : 0) << L" text="
                    << previewText.c_str();
            Win2DViewInternal::DebugPrintLine(itemLog.str());
        }
    }
    text = Win2DViewInternal::InlineSvgClassStyles(std::move(text));

    svgXml = Win2DViewNs::wr::hstring(text);

    try
    {
        if (svgDocument != nullptr)
        {
            svgDocument.Close();
        }

        auto canvasDevice = Win2DViewNs::mgc::CanvasDevice::GetSharedDevice();
        svgDocument = Win2DViewNs::mgcs::CanvasSvgDocument::LoadFromXml(canvasDevice, svgXml);
    }
    catch (Win2DViewNs::wr::hresult_error const& ex)
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
            Win2DViewInternal::DebugPrintLine(L"[LoadSvg] svgDocument is null after LoadFromXml.");
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
        catch (Win2DViewNs::wr::hresult_error const&)
        {
            try
            {
                auto viewBox = rootElement.GetRectangleAttribute(L"viewBox");
                svgDocumentWidth = viewBox.Width;
                svgDocumentHeight = viewBox.Height;
            }
            catch (Win2DViewNs::wr::hresult_error const&)
            {
                svgDocumentWidth = static_cast<float>(width);
                svgDocumentHeight = static_cast<float>(height);
            }
        }
    }

    SetScrollSizes(MM_TEXT, CSize(static_cast<int>(svgDocumentWidth), static_cast<int>(svgDocumentHeight)));
    transformMatrix = Win2DViewInternal::IdentityTransform();
    ScrollToPosition(CPoint(0, 0));
    if (consoleDebugEnabled)
    {
        std::wstringstream ss;
        ss << L"[LoadSvg] docSize=" << svgDocumentWidth << L"x" << svgDocumentHeight << L" currentView=" << width
           << L"x" << height << L" dpi=" << currentDpi;
        Win2DViewInternal::DebugPrintLine(ss.str());
    }
    return true;
}
