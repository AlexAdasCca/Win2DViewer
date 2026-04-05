#pragma once

#include <winrt/Microsoft.Graphics.Canvas.Effects.h>
#include <winrt/Microsoft.Graphics.Canvas.Svg.h>
#include <winrt/Microsoft.Graphics.Canvas.Text.h>
#include <winrt/Microsoft.Graphics.Canvas.UI.Composition.h>
#include <winrt/Microsoft.Graphics.Canvas.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

namespace wna
{
    namespace rt = winrt;

    namespace wd
    {
        namespace fnd = winrt::Windows::Foundation;
        namespace num = winrt::Windows::Foundation::Numerics;
        namespace gfx = winrt::Windows::Graphics;
        namespace sys = winrt::Windows::System;
        namespace ui = winrt::Windows::UI;
        namespace uic = winrt::Windows::UI::Composition;
        namespace uid = winrt::Windows::UI::Composition::Desktop;
        namespace uit = winrt::Windows::UI::Text;
        namespace gdx = winrt::Windows::Graphics::DirectX;
        namespace gd3 = winrt::Windows::Graphics::DirectX::Direct3D11;
    } // namespace wd

    namespace cv
    {
        namespace core = winrt::Microsoft::Graphics::Canvas;
        namespace eff = winrt::Microsoft::Graphics::Canvas::Effects;
        namespace svg = winrt::Microsoft::Graphics::Canvas::Svg;
        namespace txt = winrt::Microsoft::Graphics::Canvas::Text;
        namespace uic = winrt::Microsoft::Graphics::Canvas::UI::Composition;
    } // namespace cv
} // namespace wna
