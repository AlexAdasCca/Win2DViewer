#include "pch.h"

#include "DesktopCompositionInteropInternal.h"

void DesktopInteropInternal::DesktopHostWindow::InitializeDirectComposition()
{
    CreateD3D11Device();

    auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
    DesktopInteropNs::wr::check_hresult(
        ::DCompositionCreateDevice2(dxgiDevice.get(), __uuidof(IDCompositionDevice), dcompDevice.put_void()));

    DesktopInteropNs::wr::check_hresult(dcompDevice->CreateTargetForHwnd(windowHandle, TRUE, dcompTarget.put()));
    DesktopInteropNs::wr::check_hresult(dcompDevice->CreateVisual(dcompVisual.put()));
    DesktopInteropNs::wr::check_hresult(dcompDevice->CreateVisual(dcompBackgroundVisual.put()));
    DesktopInteropNs::wr::check_hresult(dcompDevice->CreateVisual(dcompOverlayVisual.put()));

    InitializeDirectCompositionPipeline();
    ResizeDirectCompositionSwapChain();
    DesktopInteropNs::wr::check_hresult(dcompBackgroundVisual->SetContent(dcompSwapChain.get()));
    DesktopInteropNs::wr::check_hresult(dcompOverlayVisual->SetContent(dcompOverlaySwapChain.get()));
    DesktopInteropNs::wr::check_hresult(dcompVisual->AddVisual(dcompBackgroundVisual.get(), FALSE, nullptr));
    DesktopInteropNs::wr::check_hresult(
        dcompVisual->AddVisual(dcompOverlayVisual.get(), TRUE, dcompBackgroundVisual.get()));
    DesktopInteropNs::wr::check_hresult(dcompTarget->SetRoot(dcompVisual.get()));
    DesktopInteropNs::wr::check_hresult(dcompDevice->Commit());
}

void DesktopInteropInternal::DesktopHostWindow::CreateDCompCompositionSwapChain(DXGI_ALPHA_MODE alphaMode,
                                                                                UINT width,
                                                                                UINT height,
                                                                                IDXGISwapChain1** swapChain)
{
    auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
    DesktopInteropNs::wr::com_ptr<IDXGIAdapter> adapter;
    DesktopInteropNs::wr::check_hresult(dxgiDevice->GetAdapter(adapter.put()));

    DesktopInteropNs::wr::com_ptr<IDXGIFactory2> dxgiFactory;
    DesktopInteropNs::wr::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), dxgiFactory.put_void()));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.AlphaMode = alphaMode;
    swapChainDesc.Flags = 0;

    DesktopInteropNs::wr::check_hresult(
        dxgiFactory->CreateSwapChainForComposition(d3dDevice.get(), &swapChainDesc, nullptr, swapChain));
}

void DesktopInteropInternal::DesktopHostWindow::CreateD3D11Device()
{
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    HRESULT hr = ::D3D11CreateDevice(nullptr,
                                     D3D_DRIVER_TYPE_HARDWARE,
                                     nullptr,
                                     creationFlags,
                                     featureLevels,
                                     _countof(featureLevels),
                                     D3D11_SDK_VERSION,
                                     d3dDevice.put(),
                                     &selectedFeatureLevel,
                                     d3dContext.put());

#ifdef _DEBUG
    if (FAILED(hr) && (creationFlags & D3D11_CREATE_DEVICE_DEBUG) != 0)
    {
        creationFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = ::D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr,
                                 creationFlags,
                                 featureLevels,
                                 _countof(featureLevels),
                                 D3D11_SDK_VERSION,
                                 d3dDevice.put(),
                                 &selectedFeatureLevel,
                                 d3dContext.put());
    }
#endif

    if (FAILED(hr))
    {
        hr = ::D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_WARP,
                                 nullptr,
                                 creationFlags & ~D3D11_CREATE_DEVICE_DEBUG,
                                 featureLevels,
                                 _countof(featureLevels),
                                 D3D11_SDK_VERSION,
                                 d3dDevice.put(),
                                 &selectedFeatureLevel,
                                 d3dContext.put());
    }

    DesktopInteropNs::wr::check_hresult(hr);
}

void DesktopInteropInternal::DesktopHostWindow::InitializeDirectCompositionPipeline()
{
    static constexpr char kVertexShaderSource[] = R"(
                struct VSOut
                {
                    float4 position : SV_Position;
                    float2 uv : TEXCOORD0;
                };

                VSOut main(uint vertexId : SV_VertexID)
                {
                    float2 pos;
                    if (vertexId == 0) { pos = float2(-1.0, -1.0); }
                    else if (vertexId == 1) { pos = float2(-1.0, 3.0); }
                    else { pos = float2(3.0, -1.0); }

                    VSOut output;
                    output.position = float4(pos, 0.0, 1.0);
                    output.uv = pos * 0.5 + 0.5;
                    return output;
                }
            )";

    static constexpr char kPixelShaderSource[] = R"(
                cbuffer FlowConstants : register(b0)
                {
                    float gTime;
                    float2 gResolution;
                    float gPadding0;
                    float4 gColorDark;
                    float4 gColorGold;
                    float4 gParams;
                };

                float Hash21(float2 p)
                {
                    p = frac(p * float2(123.34, 456.21));
                    p += dot(p, p + 45.32);
                    return frac(p.x * p.y);
                }

                float2 Hash22(float2 p)
                {
                    float n = Hash21(p);
                    return float2(n, Hash21(p + n + 19.19));
                }

                float Noise(float2 p)
                {
                    float2 i = floor(p);
                    float2 f = frac(p);
                    float2 u = f * f * (3.0 - 2.0 * f);

                    float a = Hash21(i);
                    float b = Hash21(i + float2(1.0, 0.0));
                    float c = Hash21(i + float2(0.0, 1.0));
                    float d = Hash21(i + float2(1.0, 1.0));

                    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
                }

                float Fbm(float2 p)
                {
                    float value = 0.0;
                    float amplitude = 0.55;
                    [unroll]
                    for (int i = 0; i < 4; ++i)
                    {
                        value += Noise(p) * amplitude;
                        p = mul(float2x2(1.62, -1.18, 1.18, 1.62), p) + 9.7;
                        amplitude *= 0.52;
                    }
                    return value;
                }

                float2 Rotate(float2 p, float angle)
                {
                    float s = sin(angle);
                    float c = cos(angle);
                    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
                }

                void PickPalette(float t, out float3 colorA, out float3 colorB)
                {
                    static const float3 paletteA[6] =
                    {
                        float3(0.66, 0.13, 0.16),
                        float3(0.70, 0.12, 0.24),
                        float3(0.62, 0.18, 0.14),
                        float3(0.74, 0.14, 0.20),
                        float3(0.68, 0.16, 0.12),
                        float3(0.72, 0.13, 0.28)
                    };
                    static const float3 paletteB[6] =
                    {
                        float3(0.99, 0.36, 0.30),
                        float3(1.00, 0.46, 0.50),
                        float3(1.00, 0.50, 0.34),
                        float3(0.99, 0.42, 0.38),
                        float3(0.98, 0.54, 0.40),
                        float3(1.00, 0.46, 0.58)
                    };

                    const float k = t * 0.11;
                    const float idx = floor(k);
                    const float fracK = smoothstep(0.0, 1.0, frac(k));
                    const int i0 = (int)fmod(idx, 6.0);
                    const int i1 = (i0 + 1) % 6;

                    colorA = lerp(paletteA[i0], paletteA[i1], fracK);
                    colorB = lerp(paletteB[i0], paletteB[i1], fracK);

                    colorA = lerp(colorA, gColorDark.rgb, 0.05);
                    colorB = lerp(colorB, gColorGold.rgb, 0.04);
                }

                void PickBackgroundPalette(float t, out float3 bgA, out float3 bgB)
                {
                    static const float3 paletteBgA[4] =
                    {
                        float3(0.80, 0.72, 0.60),
                        float3(0.82, 0.68, 0.61),
                        float3(0.78, 0.71, 0.64),
                        float3(0.84, 0.70, 0.60)
                    };
                    static const float3 paletteBgB[4] =
                    {
                        float3(0.97, 0.86, 0.76),
                        float3(0.98, 0.80, 0.74),
                        float3(0.95, 0.85, 0.80),
                        float3(0.99, 0.84, 0.73)
                    };

                    const float k = t * 0.035;
                    const float idx = floor(k);
                    const float fracK = smoothstep(0.0, 1.0, frac(k));
                    const int i0 = (int)fmod(idx, 4.0);
                    const int i1 = (i0 + 1) % 4;

                    bgA = lerp(paletteBgA[i0], paletteBgA[i1], fracK);
                    bgB = lerp(paletteBgB[i0], paletteBgB[i1], fracK);
                }
            )"
                                                 R"(
                float MetaballContribution(float2 p, float2 center, float radius, float aspect, float angle)
                {
                    float2 d = Rotate(p - center, angle);
                    d /= float2(
                        max(radius * aspect, 0.001),
                        max(radius / max(aspect, 0.001), 0.001));

                    const float dist2 = dot(d, d);
                    return 1.0 / (1.0 + dist2 * 2.4);
                }

                float SegmentContribution(float2 p, float2 a, float2 b, float radius, float softness)
                {
                    const float2 ab = b - a;
                    const float abLen2 = max(dot(ab, ab), 0.0001);
                    const float t = saturate(dot(p - a, ab) / abLen2);
                    const float2 closest = lerp(a, b, t);
                    const float2 delta = p - closest;
                    const float dist = length(delta) / max(radius, 0.001);
                    return exp(-pow(dist, max(softness, 0.2)));
                }

                float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
                {
                    const float flowSpeed = max(gParams.x, 0.2);
                    const float paletteSpeed = max(gParams.y, 0.2);
                    const float particleGain = max(gParams.z, 0.0);
                    const float swirlGain = max(gParams.w, 0.2);

                    const float time = gTime * flowSpeed;
                    const float paletteTime = gTime * paletteSpeed;
                    const float bgPaletteTime = gTime * max(paletteSpeed * 0.58, 0.18);
                    float2 p = uv - 0.5;
                    p.x *= gResolution.x / max(1.0, gResolution.y);

                    float3 colorA;
                    float3 colorB;
                    PickPalette(paletteTime, colorA, colorB);

                    float3 bgA;
                    float3 bgB;
                    PickBackgroundPalette(bgPaletteTime, bgA, bgB);

                    const float bgFlowA = Fbm(p * 1.05 + float2(time * 0.06, -time * 0.04));
                    const float bgFlowB = Fbm(Rotate(p * 0.88, 0.24) + float2(-time * 0.05, time * 0.03));
                    const float bgAxis = saturate(0.50 + (bgFlowA - 0.5) * 0.34 + (bgFlowB - 0.5) * 0.24 + p.y * 0.11);
                    float3 color = lerp(bgA, bgB, bgAxis);
                    color = lerp(color, lerp(bgA, bgB, 0.62 + 0.12 * sin(p.x * 0.85 + paletteTime * 0.06)), 0.20);
                    const float warmWashA = exp(-pow(length((p - float2(0.38, -0.14)) * float2(0.92, 1.04)) / 1.20, 2.0));
                    const float warmWashB = exp(-pow(length((p - float2(-0.52, 0.24)) * float2(1.04, 0.96)) / 1.08, 2.0));
                    color = lerp(color, lerp(bgA, bgB, 0.76), warmWashA * 0.18);
                    color = lerp(color, lerp(bgA, bgB, 0.22), warmWashB * 0.14);

                    const float focusPulse = 0.5 + 0.5 * sin(time * 0.70 + sin(time * 0.23) * 1.10 + cos(time * 0.11) * 0.72);
                    const float gatherPulse = 0.5 + 0.5 * sin(time * 0.52 + sin(time * 0.17) * 0.92);
                    const float driftPulse = 0.5 + 0.5 * cos(time * 0.40 + sin(time * 0.15) * 0.80);

                    const float2 ingressDir = normalize(float2(
                        cos(time * 0.14 + sin(time * 0.05) * 0.52),
                        sin(time * 0.12 - cos(time * 0.06) * 0.48)));
                    const float2 ingressNrm = float2(-ingressDir.y, ingressDir.x);
                    const float2 sceneDrift = float2(
                        sin(time * 0.045 + sin(time * 0.016) * 1.1),
                        cos(time * 0.038 + cos(time * 0.018) * 0.9)) * float2(0.30, 0.22);
                    const float2 hub0 = float2(
                        1.18 * sin(time * 0.10 + 0.35) + 0.24 * sin(time * 0.24 + 2.10),
                        0.62 * cos(time * 0.08 + 1.10)) + sceneDrift + ingressDir * 0.18;
                    const float2 hub1 = float2(
                        -0.18 + 0.82 * sin(time * 0.09 + 0.80) + 0.26 * sin(time * 0.18 + 1.90),
                        -0.08 + 0.46 * cos(time * 0.07 + 1.30)) - sceneDrift * 0.36;
                    const float2 hub2 = float2(
                        1.02 * cos(time * 0.06 + 2.10) + 0.34 * sin(time * 0.14 + 0.50),
                        -0.26 + 0.52 * sin(time * 0.09 + 1.70)) + sceneDrift * 0.28;
                    const float2 hub3 = float2(
                        -0.74 * cos(time * 0.05 + 0.50) + 0.56 * sin(time * 0.12 + 2.30),
                        0.38 * sin(time * 0.08 + 2.40) + 0.28 * cos(time * 0.10 + 0.90)) - sceneDrift * 0.22;
                    const float2 hubOpp = hub1 - ingressDir * (1.14 + 0.34 * driftPulse) + ingressNrm * (0.28 * sin(time * 0.20 + 0.7));

                    const float2 flowWarp = float2(
                        Fbm(p * 0.86 + float2(time * 0.22, -time * 0.18) + float2(1.6, -2.1)) - 0.5,
                        Fbm(Rotate(p * 0.82, 0.42) + float2(-time * 0.20, time * 0.16) + float2(-1.8, 2.2)) - 0.5);
                    const float2 coarseWarp = float2(
                        Fbm(p * 0.42 + float2(time * 0.08, -time * 0.07) + float2(3.2, -1.4)) - 0.5,
                        Fbm(Rotate(p * 0.38, 0.30) + float2(-time * 0.06, time * 0.05) + float2(-2.8, 2.5)) - 0.5);
                    const float2 q = p + flowWarp * 0.06 + coarseWarp * 0.10;

                    const float irregular = 0.5 + 0.5 * sin(time * 0.94 + sin(time * 0.37) * 1.40 + cos(time * 0.11) * 0.90);
                    const float orbitMixA = 0.5 + 0.5 * sin(time * 1.02 + cos(time * 0.33) * 1.1);
                    const float orbitMixB = 0.5 + 0.5 * sin(time * 0.88 + 1.7 + sin(time * 0.27) * 0.9);
                    const float2 cA = hub1 + float2(-0.18, 0.05) * (0.30 + 0.74 * irregular) + float2(cos(time * 0.74), sin(time * 0.68)) * 0.05;
                    const float2 cB = hub1 + float2(0.16, -0.04) * (0.28 + 0.78 * (1.0 - irregular)) + float2(cos(time * 0.66 + 1.6), sin(time * 0.72 + 0.8)) * 0.05;
                    const float2 cC = hub1 + float2(0.04, 0.14) * (0.16 + 0.66 * focusPulse) + float2(cos(time * 0.82 + 2.2), sin(time * 0.76 + 1.1)) * 0.04;
                    const float2 cD = hub1 + float2(-0.03, -0.16) * (0.16 + 0.62 * driftPulse) + float2(cos(time * 0.78 + 0.5), sin(time * 0.84 + 2.7)) * 0.04;
                    const float2 dA = hub2 + float2(cos(time * 0.46 + 2.0), sin(time * 0.52 + 1.1)) * (0.16 + 0.10 * driftPulse);
                    const float2 dB = hub2 + float2(cos(time * 0.58 + 0.8), sin(time * 0.62 + 2.4)) * (0.12 + 0.08 * focusPulse);
                    const float2 dC = lerp(hub2, hub3, 0.46 + (orbitMixA - 0.5) * 0.18) + float2(cos(time * 0.40 + 0.7), sin(time * 0.44 + 1.5)) * 0.05;
                    const float2 eA = hub3 + float2(cos(time * 0.44 + 1.7), sin(time * 0.50 + 0.3)) * (0.14 + 0.10 * gatherPulse);
                    const float2 eB = hub3 + float2(cos(time * 0.54 + 2.6), sin(time * 0.58 + 1.6)) * (0.10 + 0.08 * focusPulse);
                    const float2 oA = hubOpp + float2(cos(time * 0.42 + 2.7), sin(time * 0.48 + 0.9)) * (0.10 + 0.06 * orbitMixB);
                    const float2 oB = hubOpp + float2(cos(time * 0.50 + 1.4), sin(time * 0.56 + 2.2)) * (0.08 + 0.05 * gatherPulse);

                    float field = 0.0;
                    field += MetaballContribution(q, hub0 + ingressDir * (-0.18 + gatherPulse * 0.08) + ingressNrm * 0.04, 0.24, 1.34, -0.14) * 0.22;
                    field += MetaballContribution(q, hub0 + ingressDir * (0.08 + driftPulse * 0.18) - ingressNrm * (0.05 + 0.04 * focusPulse), 0.20, 1.24, 0.08) * 0.18;
                    field += MetaballContribution(q, hub0 + float2(cos(time * 0.38 + 0.4), sin(time * 0.42 + 1.2)) * (0.10 + 0.05 * gatherPulse), 0.16, 1.18, 0.28) * 0.15;

                    field += MetaballContribution(q, cA, 0.25, 1.18, 0.36) * 0.34;
                    field += MetaballContribution(q, cB, 0.25, 1.20, -0.28) * 0.34;
                    field += MetaballContribution(q, cC, 0.22, 1.14, 0.10) * 0.30;
                    field += MetaballContribution(q, cD, 0.20, 1.08, -0.10) * 0.26;

                    field += MetaballContribution(q, dA, 0.22, 1.20, -0.12) * 0.28;
                    field += MetaballContribution(q, dB, 0.20, 1.16, 0.20) * 0.24;
                    field += MetaballContribution(q, dC, 0.18, 1.12, -0.04) * 0.20;
                    field += MetaballContribution(q, eA, 0.22, 1.18, 0.38) * 0.28;
                    field += MetaballContribution(q, eB, 0.19, 1.12, -0.28) * 0.22;

                    field += MetaballContribution(q, oA, 0.18, 1.18, -0.46) * 0.18;
                    field += MetaballContribution(q, oB, 0.15, 1.12, 0.18) * 0.14;

                    const float trailA = SegmentContribution(q, cA, cB, 0.48, 1.6) * 0.38;
                    const float trailB = SegmentContribution(q, cB, dA, 0.42, 1.5) * 0.30;
                    const float trailC = SegmentContribution(q, dB, eA, 0.46, 1.5) * 0.28;
                    const float trailD = SegmentContribution(q, eB, oA, 0.52, 1.7) * 0.22;
                    const float trailField = trailA + trailB + trailC + trailD;

                    const float flowAxis = dot(q, ingressDir);
                    const float crossAxis = dot(q, ingressNrm);
                    const float sheetWaveA = Fbm(float2(flowAxis * 0.42 - time * 0.12, crossAxis * 0.72 + time * 0.10) + float2(1.4, -2.6));
                    const float sheetWaveB = Fbm(float2(flowAxis * 0.58 + time * 0.14, crossAxis * 0.54 - time * 0.12) + float2(-2.8, 1.7));
                    const float sheetDrift = sin(flowAxis * 1.4 - time * 0.34 + sheetWaveA * 1.8) * 0.24
                        + cos(flowAxis * 0.9 + time * 0.28 + sheetWaveB * 1.5) * 0.18;
                    const float sheetEnvelope = exp(-pow((crossAxis + sheetDrift) / 1.18, 2.0));
                    const float sheetRidgeA = smoothstep(0.30, 0.82, sheetWaveA * 0.64 + sheetWaveB * 0.36);
                    const float sheetRidgeB = smoothstep(0.26, 0.80, sheetWaveB * 0.58 + sheetWaveA * 0.42);
                    const float sheetField = sheetEnvelope * (0.44 + sheetRidgeA * 0.34 + sheetRidgeB * 0.22);

                    const float mistNoiseA = Fbm((q + coarseWarp * 0.48) * 0.72 + float2(-time * 0.16, time * 0.12));
                    const float mistNoiseB = Fbm(Rotate(q * 0.68, -0.34) + float2(time * 0.14, -time * 0.10) + float2(2.8, -1.2));
                    const float mistField = sheetField * 0.58 + field * 0.16 + trailField * 0.18 + mistNoiseA * 0.18 + mistNoiseB * 0.14;

                    const float hazeMask = smoothstep(0.28, 0.86, mistField);
                    const float bodyMask = smoothstep(0.37, 0.92, sheetField * 0.58 + trailField * 0.18 + field * 0.14 + mistNoiseA * 0.12);
                    const float coreMask = smoothstep(0.53, 0.96, sheetField * 0.36 + field * 0.42 + trailField * 0.12);
            )"
                                                 R"(
                    const float2 nkx = float2(0.024, 0.000);
                    const float2 nky = float2(0.000, 0.024);
                    const float n0 = Fbm(q * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n1 = Fbm((q + nkx) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n2 = Fbm((q - nkx) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n3 = Fbm((q + nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n4 = Fbm((q - nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n5 = Fbm((q + nkx + nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n6 = Fbm((q + nkx - nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n7 = Fbm((q - nkx + nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float n8 = Fbm((q - nkx - nky) * 1.28 + float2(time * 0.38, -time * 0.30));
                    const float blurNoise = n0 * 0.30 + (n1 + n2 + n3 + n4) * 0.12 + (n5 + n6 + n7 + n8) * 0.055;
            )"
                                                 R"(
                    const float swirlA = Fbm(Rotate(q * 0.92, 0.74) + float2(-time * 0.58, time * 0.48));
                    const float swirlB = Fbm(q * 1.06 + float2(time * 0.52, -time * 0.46) + float2(2.4, -1.3));
                    const float swirlC = Fbm((q + float2(swirlA - 0.5, swirlB - 0.5) * 0.24) * 1.18 + float2(-time * 0.44, time * 0.40));
                    const float stirField = smoothstep(0.18, 0.82, swirlA * 0.34 + swirlB * 0.30 + swirlC * 0.36);
                    const float2 stirOffset = float2(swirlA - swirlB, swirlC - swirlA) * (0.056 * max(hazeMask, bodyMask));

                    const float motionField = smoothstep(0.28, 0.74, blurNoise * 0.46 + stirField * 0.34 + mistField * 0.20);
                    const float warmShift = 0.5 + 0.5 * sin(time * 0.74 + motionField * 2.4 + q.x * 0.8 + q.y * 0.5);
                    const float3 accentA = lerp(colorA, colorB, 0.48 + 0.14 * warmShift);
                    const float3 accentB = lerp(colorA, colorB, 0.84 - 0.10 * warmShift);
                    const float3 hazeColor = lerp(lerp(bgA, bgB, 0.54), lerp(accentA, accentB, 0.44 + 0.10 * warmShift), 0.48);
                    const float3 edgeTint = lerp(hazeColor, lerp(bgA, bgB, 0.58), 0.18);
                    const float colorField = saturate(0.28 + bodyMask * 0.20 + coreMask * 0.22 + motionField * 0.18 + (stirField - 0.5) * 0.30);
                    float3 waveColor = lerp(edgeTint, lerp(accentA, accentB, colorField), smoothstep(0.20, 0.76, bodyMask + coreMask * 0.36));
                    const float3 warmFocus = lerp(float3(0.99, 0.32, 0.28), float3(1.00, 0.46, 0.40), warmShift);
                    waveColor = lerp(waveColor, warmFocus, 0.16 + coreMask * 0.12 + stirField * 0.06);

                    color = lerp(color, hazeColor, hazeMask * (0.32 + motionField * 0.05));
                    const float foregroundAlpha = saturate(hazeMask * 0.56 + bodyMask * 0.28 + coreMask * 0.10);
                    color = lerp(color, waveColor, foregroundAlpha * 0.88);
                    color += lerp(accentB, float3(1.0, 0.95, 0.92), 0.54) * coreMask * (0.018 + stirField * 0.020);

                    const float2 refractOffset = (flowWarp * 0.012 + stirOffset + ingressDir * 0.008 * sin(time * 0.96 + dot(p, ingressDir) * 2.3)) * foregroundAlpha;
                    const float2 refrP = p + refractOffset;
                    const float refrAxis = saturate(0.60 + refrP.y * 0.08 + refrP.x * 0.05 + (Fbm(refrP * 1.12 + float2(time * 0.24, -time * 0.18)) - 0.5) * 0.10);
                    const float3 refrColor = lerp(bgA, bgB, refrAxis);
                    color = lerp(color, refrColor, foregroundAlpha * 0.015);

                    const float mote = smoothstep(0.978, 1.0, Hash21((uv + flowWarp * 0.06 + stirOffset) * gResolution * 0.11 + time * 1.42));
                    color += lerp(waveColor, float3(1.0, 1.0, 1.0), 0.42) * mote * (0.028 * particleGain);

                    const float grain = Hash21(uv * gResolution * 0.26 + time * 6.0) - 0.5;
                    color += grain * 0.010;

                    const float vignette = 1.0 - smoothstep(1.02, 1.34, length(p));
                    color *= lerp(0.97, 1.0, vignette);

                    color = saturate(color);
                    return float4(color, 1.0);
                }
            )";

    static constexpr char kOverlayPixelShaderSource[] = R"(
                cbuffer OverlayConstants : register(b0)
                {
                    float2 gResolution;
                    float2 gMouse;
                    float2 gHandle;
                    float gTime;
                    float gPadding0;
                    float4 gParams;
                };

                float SdRoundedRect(float2 p, float2 center, float2 halfSize, float radius)
                {
                    float2 d = abs(p - center) - halfSize + radius;
                    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
                }

                float SdSegment(float2 p, float2 a, float2 b)
                {
                    float2 pa = p - a;
                    float2 ba = b - a;
                    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 0.0001));
                    return length(pa - ba * h);
                }

                float FillMask(float sdf, float feather)
                {
                    return 1.0 - smoothstep(0.0, max(feather, 0.0001), sdf);
                }

                void Composite(inout float4 dst, float3 srcColor, float srcAlpha)
                {
                    dst.rgb = srcColor * srcAlpha + dst.rgb * (1.0 - srcAlpha);
                    dst.a = srcAlpha + dst.a * (1.0 - srcAlpha);
                }

                float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
                {
                    const float2 px = position.xy;
                    const float margin = 28.0;
                    const float panelWidth = min(gResolution.x * 0.36, 372.0);
                    const float panelHeight = min(gResolution.y * 0.34, 206.0);
                    const float2 panelHalf = float2(panelWidth * 0.5, panelHeight * 0.5);
                    const float2 panelCenter = float2(gResolution.x - margin - panelHalf.x, margin + panelHalf.y);

                    const float panelSdf = SdRoundedRect(px, panelCenter, panelHalf, 24.0);
                    const float panelMask = FillMask(panelSdf, 2.0);

                    float4 output = float4(0.0, 0.0, 0.0, 0.0);
                    const float3 panelColor = lerp(float3(0.10, 0.11, 0.14), float3(0.15, 0.10, 0.11), 0.5 + 0.5 * sin(gTime * 0.26));
                    Composite(output, panelColor, panelMask * 0.78);

                    const float2 chipSize = float2(panelWidth * 0.28, 34.0);
                    const float chipGap = 14.0;
                    const float2 chip1Center = panelCenter + float2(-panelHalf.x + 28.0 + chipSize.x * 0.5, -panelHalf.y + 30.0);
                    const float2 chip2Center = chip1Center + float2(chipSize.x + chipGap, 0.0);
                    const float chip1Mask = FillMask(SdRoundedRect(px, chip1Center, chipSize * 0.5, 16.0), 1.5);
                    const float chip2Mask = FillMask(SdRoundedRect(px, chip2Center, chipSize * 0.5, 16.0), 1.5);

                    const float hoverPrimary = saturate(gParams.x);
                    const float hoverSecondary = saturate(gParams.y);
                    const float accentToggle = saturate(gParams.z);
                    const float dragging = saturate(gParams.w);

                    const float3 chip1Color = lerp(float3(0.83, 0.31, 0.28), float3(0.98, 0.49, 0.38), 0.45 + hoverPrimary * 0.35 + accentToggle * 0.12);
                    const float3 chip2Color = lerp(float3(0.93, 0.72, 0.58), float3(0.99, 0.82, 0.68), 0.28 + hoverSecondary * 0.34 + (1.0 - accentToggle) * 0.10);
                    Composite(output, chip1Color, chip1Mask * (0.78 + hoverPrimary * 0.12));
                    Composite(output, chip2Color, chip2Mask * (0.66 + hoverSecondary * 0.14));

                    const float2 fieldMin = panelCenter + float2(-panelHalf.x + 22.0, -8.0);
                    const float2 fieldMax = panelCenter + float2(panelHalf.x - 22.0, panelHalf.y - 20.0);
                    const float2 fieldCenter = (fieldMin + fieldMax) * 0.5;
                    const float2 fieldHalf = (fieldMax - fieldMin) * 0.5;
                    const float fieldMask = FillMask(SdRoundedRect(px, fieldCenter, fieldHalf, 18.0), 1.4);

                    const float2 nodeA = float2(fieldMin.x + 26.0, fieldMax.y - 28.0 + sin(gTime * 0.62) * 8.0);
                    const float2 nodeB = gHandle;
                    const float2 nodeC = float2(fieldMax.x - 24.0, fieldMin.y + 24.0 + cos(gTime * 0.74) * 10.0);
                    const float2 nodeD = lerp(nodeA, nodeC, 0.50) + float2(cos(gTime * 0.84), sin(gTime * 0.92)) * 22.0;

                    const float linkAB = FillMask(SdSegment(px, nodeA, nodeB) - 5.0, 1.8);
                    const float linkBC = FillMask(SdSegment(px, nodeB, nodeC) - 4.5, 1.8);
                    const float linkAD = FillMask(SdSegment(px, nodeA, nodeD) - 3.8, 1.6);
                    const float linkDC = FillMask(SdSegment(px, nodeD, nodeC) - 3.8, 1.6);
                    const float linkMix = saturate(linkAB * 0.44 + linkBC * 0.40 + linkAD * 0.28 + linkDC * 0.28) * fieldMask;

                    const float2 flowUv = (px - fieldMin) / max(fieldMax - fieldMin, float2(1.0, 1.0));
                    const float sweep = sin((flowUv.x * 2.8 - flowUv.y * 1.4) * 3.14159 + gTime * 1.4 + accentToggle * 0.9) * 0.5 + 0.5;
                    const float sweep2 = cos((flowUv.x * 1.2 + flowUv.y * 2.1) * 3.14159 - gTime * 1.1) * 0.5 + 0.5;
                    const float3 linkColor = lerp(float3(0.96, 0.42, 0.38), float3(0.99, 0.76, 0.60), sweep * 0.58 + sweep2 * 0.18);
                    Composite(output, linkColor, linkMix * (0.22 + dragging * 0.10));

                    const float nodeMaskA = FillMask(length(px - nodeA) - 9.0, 1.4) * fieldMask;
                    const float nodeMaskB = FillMask(length(px - nodeB) - 11.0, 1.6) * fieldMask;
                    const float nodeMaskC = FillMask(length(px - nodeC) - 8.5, 1.4) * fieldMask;
                    const float nodeMaskD = FillMask(length(px - nodeD) - 7.0, 1.3) * fieldMask;

                    Composite(output, float3(0.98, 0.82, 0.70), nodeMaskA * 0.60);
                    Composite(output, float3(1.00, 0.49, 0.40), nodeMaskB * (0.88 + dragging * 0.08));
                    Composite(output, float3(0.99, 0.70, 0.56), nodeMaskC * 0.56);
                    Composite(output, float3(0.98, 0.58, 0.48), nodeMaskD * 0.46);

                    const float handleHalo = FillMask(length(px - nodeB) - 22.0, 10.0) * fieldMask;
                    Composite(output, float3(1.00, 0.72, 0.62), handleHalo * 0.08);

                    const float mouseHalo = FillMask(length(px - gMouse) - 20.0, 16.0) * panelMask;
                    Composite(output, float3(1.0, 0.96, 0.92), mouseHalo * 0.12);

                    return saturate(output);
                }
            )";

    UINT shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    DesktopInteropNs::wr::com_ptr<ID3DBlob> vertexShaderBlob;
    DesktopInteropNs::wr::com_ptr<ID3DBlob> pixelShaderBlob;
    DesktopInteropNs::wr::com_ptr<ID3DBlob> errorBlob;

    HRESULT hr = ::D3DCompile(kVertexShaderSource,
                              sizeof(kVertexShaderSource) - 1,
                              nullptr,
                              nullptr,
                              nullptr,
                              "main",
                              "vs_5_0",
                              shaderFlags,
                              0,
                              vertexShaderBlob.put(),
                              errorBlob.put());
    DesktopInteropNs::wr::check_hresult(hr);

    errorBlob = nullptr;
    hr = ::D3DCompile(kPixelShaderSource,
                      sizeof(kPixelShaderSource) - 1,
                      nullptr,
                      nullptr,
                      nullptr,
                      "main",
                      "ps_5_0",
                      shaderFlags,
                      0,
                      pixelShaderBlob.put(),
                      errorBlob.put());
    DesktopInteropNs::wr::check_hresult(hr);

    DesktopInteropNs::wr::check_hresult(d3dDevice->CreateVertexShader(
        vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize(), nullptr, dcompVertexShader.put()));

    DesktopInteropNs::wr::check_hresult(d3dDevice->CreatePixelShader(
        pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize(), nullptr, dcompPixelShader.put()));

    errorBlob = nullptr;
    hr = ::D3DCompile(kOverlayPixelShaderSource,
                      sizeof(kOverlayPixelShaderSource) - 1,
                      nullptr,
                      nullptr,
                      nullptr,
                      "main",
                      "ps_5_0",
                      shaderFlags,
                      0,
                      pixelShaderBlob.put(),
                      errorBlob.put());
    DesktopInteropNs::wr::check_hresult(hr);

    DesktopInteropNs::wr::check_hresult(d3dDevice->CreatePixelShader(
        pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize(), nullptr, dcompOverlayPixelShader.put()));

    D3D11_BUFFER_DESC constantBufferDesc{};
    constantBufferDesc.ByteWidth = sizeof(DCompFlowConstants);
    constantBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    constantBufferDesc.MiscFlags = 0;
    constantBufferDesc.StructureByteStride = 0;
    DesktopInteropNs::wr::check_hresult(
        d3dDevice->CreateBuffer(&constantBufferDesc, nullptr, dcompConstantBuffer.put()));

    constantBufferDesc.ByteWidth = sizeof(DCompOverlayConstants);
    DesktopInteropNs::wr::check_hresult(
        d3dDevice->CreateBuffer(&constantBufferDesc, nullptr, dcompOverlayConstantBuffer.put()));
}

void DesktopInteropInternal::DesktopHostWindow::ResizeDirectCompositionSwapChain()
{
    RECT clientRect{};
    ::GetClientRect(windowHandle, &clientRect);

    const UINT width = static_cast<UINT>((std::max<LONG>)(1L, clientRect.right - clientRect.left));
    const UINT height = static_cast<UINT>((std::max<LONG>)(1L, clientRect.bottom - clientRect.top));

    if (dcompSwapChain == nullptr)
    {
        CreateDCompCompositionSwapChain(DXGI_ALPHA_MODE_IGNORE, width, height, dcompSwapChain.put());
        CreateDCompCompositionSwapChain(DXGI_ALPHA_MODE_PREMULTIPLIED, width, height, dcompOverlaySwapChain.put());
    }
    else
    {
        dcompRenderTargetView = nullptr;
        dcompOverlayRenderTargetView = nullptr;
        d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
        d3dContext->ClearState();
        d3dContext->Flush();
        DesktopInteropNs::wr::check_hresult(
            dcompSwapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        DesktopInteropNs::wr::check_hresult(
            dcompOverlaySwapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
    }

    DesktopInteropNs::wr::com_ptr<ID3D11Texture2D> backBuffer;
    DesktopInteropNs::wr::check_hresult(dcompSwapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));
    DesktopInteropNs::wr::check_hresult(
        d3dDevice->CreateRenderTargetView(backBuffer.get(), nullptr, dcompRenderTargetView.put()));

    DesktopInteropNs::wr::com_ptr<ID3D11Texture2D> overlayBackBuffer;
    DesktopInteropNs::wr::check_hresult(dcompOverlaySwapChain->GetBuffer(0, IID_PPV_ARGS(overlayBackBuffer.put())));
    DesktopInteropNs::wr::check_hresult(
        d3dDevice->CreateRenderTargetView(overlayBackBuffer.get(), nullptr, dcompOverlayRenderTargetView.put()));

    dcompPixelWidth = width;
    dcompPixelHeight = height;
    ClampDCompOverlayHandle();
}

RECT DesktopInteropInternal::DesktopHostWindow::GetDCompOverlayPanelRectPixels() const
{
    const LONG panelWidth = static_cast<LONG>((std::min)(dcompPixelWidth * 36 / 100, 372u));
    const LONG panelHeight = static_cast<LONG>((std::min)(dcompPixelHeight * 34 / 100, 206u));
    RECT rect{};
    rect.left = static_cast<LONG>(dcompPixelWidth) - 28 - panelWidth;
    rect.top = 28;
    rect.right = rect.left + panelWidth;
    rect.bottom = rect.top + panelHeight;
    return rect;
}

RECT DesktopInteropInternal::DesktopHostWindow::GetDCompPrimaryChipRectPixels() const
{
    const RECT panel = GetDCompOverlayPanelRectPixels();
    const LONG chipWidth = (panel.right - panel.left) * 28 / 100;
    RECT rect{};
    rect.left = panel.left + 28;
    rect.top = panel.top + 13;
    rect.right = rect.left + chipWidth;
    rect.bottom = rect.top + 34;
    return rect;
}

RECT DesktopInteropInternal::DesktopHostWindow::GetDCompSecondaryChipRectPixels() const
{
    RECT rect = GetDCompPrimaryChipRectPixels();
    const LONG gap = 14;
    const LONG width = rect.right - rect.left;
    rect.left += width + gap;
    rect.right = rect.left + width;
    return rect;
}

RECT DesktopInteropInternal::DesktopHostWindow::GetDCompFieldRectPixels() const
{
    const RECT panel = GetDCompOverlayPanelRectPixels();
    RECT rect{};
    rect.left = panel.left + 22;
    rect.top = panel.top + 58;
    rect.right = panel.right - 22;
    rect.bottom = panel.bottom - 20;
    return rect;
}

bool DesktopInteropInternal::DesktopHostWindow::IsPointInsideRect(const RECT& rect, LONG x, LONG y) const
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

void DesktopInteropInternal::DesktopHostWindow::ClampDCompOverlayHandle()
{
    if (dcompPixelWidth == 0 || dcompPixelHeight == 0)
    {
        return;
    }

    const RECT fieldRect = GetDCompFieldRectPixels();
    if (!dcompOverlayHandleInitialized)
    {
        dcompOverlayHandleX = Lerp(static_cast<float>(fieldRect.left), static_cast<float>(fieldRect.right), 0.58f);
        dcompOverlayHandleY = Lerp(static_cast<float>(fieldRect.bottom), static_cast<float>(fieldRect.top), 0.46f);
        dcompOverlayHandleInitialized = true;
    }

    dcompOverlayHandleX = (std::max)(static_cast<float>(fieldRect.left + 12),
                                     (std::min)(static_cast<float>(fieldRect.right - 12), dcompOverlayHandleX));
    dcompOverlayHandleY = (std::max)(static_cast<float>(fieldRect.top + 12),
                                     (std::min)(static_cast<float>(fieldRect.bottom - 12), dcompOverlayHandleY));
}

void DesktopInteropInternal::DesktopHostWindow::UpdateDCompOverlayHoverState(LONG x, LONG y)
{
    dcompMouseX = static_cast<float>(x);
    dcompMouseY = static_cast<float>(y);
    dcompHoverPrimary = IsPointInsideRect(GetDCompPrimaryChipRectPixels(), x, y);
    dcompHoverSecondary = IsPointInsideRect(GetDCompSecondaryChipRectPixels(), x, y);

    const float dx = dcompOverlayHandleX - static_cast<float>(x);
    const float dy = dcompOverlayHandleY - static_cast<float>(y);
    dcompHoverHandle = ((dx * dx) + (dy * dy)) <= (18.0f * 18.0f);
}

void DesktopInteropInternal::DesktopHostWindow::HandleDirectCompositionMouseMove(LONG x, LONG y)
{
    if (!dcompMouseTracking)
    {
        TRACKMOUSEEVENT trackMouseEvent{};
        trackMouseEvent.cbSize = sizeof(trackMouseEvent);
        trackMouseEvent.dwFlags = TME_LEAVE;
        trackMouseEvent.hwndTrack = windowHandle;
        ::TrackMouseEvent(&trackMouseEvent);
        dcompMouseTracking = true;
    }

    dcompMouseInside = true;
    UpdateDCompOverlayHoverState(x, y);

    if (dcompOverlayDragging)
    {
        dcompOverlayHandleX = static_cast<float>(x);
        dcompOverlayHandleY = static_cast<float>(y);
        ClampDCompOverlayHandle();
    }
}

void DesktopInteropInternal::DesktopHostWindow::HandleDirectCompositionMouseLeave()
{
    dcompMouseTracking = false;
    dcompMouseInside = false;
    dcompHoverPrimary = false;
    dcompHoverSecondary = false;
    dcompHoverHandle = false;
}

void DesktopInteropInternal::DesktopHostWindow::HandleDirectCompositionLButtonDown(LONG x, LONG y)
{
    UpdateDCompOverlayHoverState(x, y);

    if (dcompHoverPrimary)
    {
        dcompOverlayAccentEnabled = !dcompOverlayAccentEnabled;
    }
    else if (dcompHoverSecondary)
    {
        dcompOverlayLinkBoost = !dcompOverlayLinkBoost;
    }
    else if (dcompHoverHandle || IsPointInsideRect(GetDCompFieldRectPixels(), x, y))
    {
        dcompOverlayDragging = true;
        dcompOverlayHandleX = static_cast<float>(x);
        dcompOverlayHandleY = static_cast<float>(y);
        ClampDCompOverlayHandle();
        ::SetCapture(windowHandle);
    }
}

void DesktopInteropInternal::DesktopHostWindow::HandleDirectCompositionLButtonUp(LONG x, LONG y)
{
    UpdateDCompOverlayHoverState(x, y);
    if (dcompOverlayDragging)
    {
        dcompOverlayDragging = false;
        ::ReleaseCapture();
    }
}

void DesktopInteropInternal::DesktopHostWindow::RenderDirectCompositionOverlay()
{
    if (dcompOverlaySwapChain == nullptr || dcompOverlayRenderTargetView == nullptr ||
        dcompOverlayConstantBuffer == nullptr)
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT mapHr = d3dContext->Map(dcompOverlayConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(mapHr))
    {
        return;
    }

    auto* constants = reinterpret_cast<DCompOverlayConstants*>(mapped.pData);
    constants->resolution[0] = static_cast<float>(dcompPixelWidth);
    constants->resolution[1] = static_cast<float>(dcompPixelHeight);
    constants->mouse[0] = dcompMouseX;
    constants->mouse[1] = dcompMouseY;
    constants->handle[0] = dcompOverlayHandleX;
    constants->handle[1] = dcompOverlayHandleY;
    constants->time = animationPhase;
    constants->params[0] = dcompHoverPrimary ? 1.0f : 0.0f;
    constants->params[1] = dcompHoverSecondary ? 1.0f : 0.0f;
    constants->params[2] = dcompOverlayAccentEnabled ? 1.0f : 0.0f;
    constants->params[3] = dcompOverlayDragging ? 1.0f : (dcompOverlayLinkBoost ? 0.55f : 0.0f);
    d3dContext->Unmap(dcompOverlayConstantBuffer.get(), 0);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    d3dContext->ClearRenderTargetView(dcompOverlayRenderTargetView.get(), clearColor);

    ID3D11RenderTargetView* renderTargetViews[] = { dcompOverlayRenderTargetView.get() };
    d3dContext->OMSetRenderTargets(1, renderTargetViews, nullptr);
    d3dContext->IASetInputLayout(nullptr);
    d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dContext->VSSetShader(dcompVertexShader.get(), nullptr, 0);
    d3dContext->PSSetShader(dcompOverlayPixelShader.get(), nullptr, 0);
    ID3D11Buffer* constantBuffers[] = { dcompOverlayConstantBuffer.get() };
    d3dContext->PSSetConstantBuffers(0, 1, constantBuffers);
    d3dContext->Draw(3, 0);

    (void)dcompOverlaySwapChain->Present(1, 0);
}

void DesktopInteropInternal::DesktopHostWindow::RenderDirectCompositionFrame()
{
    if (dcompSwapChain == nullptr || dcompRenderTargetView == nullptr || dcompConstantBuffer == nullptr)
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT mapHr = d3dContext->Map(dcompConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(mapHr))
    {
        return;
    }

    auto* constants = reinterpret_cast<DCompFlowConstants*>(mapped.pData);
    constants->time = animationPhase;
    constants->resolution[0] = static_cast<float>(dcompPixelWidth);
    constants->resolution[1] = static_cast<float>(dcompPixelHeight);
    constants->colorDark[0] = 0.380f;
    constants->colorDark[1] = 0.090f;
    constants->colorDark[2] = 0.090f;
    constants->colorDark[3] = 1.0f;
    constants->colorGold[0] = 1.000f;
    constants->colorGold[1] = 0.420f;
    constants->colorGold[2] = 0.380f;
    constants->colorGold[3] = 1.0f;
    constants->params[0] = 1.78f;
    constants->params[1] = 1.18f;
    constants->params[2] = 0.46f;
    constants->params[3] = 0.72f;
    d3dContext->Unmap(dcompConstantBuffer.get(), 0);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(dcompPixelWidth);
    viewport.Height = static_cast<float>(dcompPixelHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    d3dContext->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView* renderTargetViews[] = { dcompRenderTargetView.get() };
    d3dContext->OMSetRenderTargets(1, renderTargetViews, nullptr);
    d3dContext->IASetInputLayout(nullptr);
    d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dContext->VSSetShader(dcompVertexShader.get(), nullptr, 0);
    d3dContext->PSSetShader(dcompPixelShader.get(), nullptr, 0);
    ID3D11Buffer* constantBuffers[] = { dcompConstantBuffer.get() };
    d3dContext->PSSetConstantBuffers(0, 1, constantBuffers);
    d3dContext->Draw(3, 0);

    (void)dcompSwapChain->Present(1, 0);
    RenderDirectCompositionOverlay();
}
