#include "pch.h"

#include <D2d1_1.h>
#include <D3d11_4.h>
#include <Dwrite.h>
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>
#include <Windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.Composition.h>

#include "DeviceLostHelper.h"

namespace DeviceLostNs
{
    namespace wr = wna::rt;
    namespace wgi = wna::wd::gd3;
} // namespace DeviceLostNs

DeviceLostHelper::~DeviceLostHelper()
{
    StopWatchingCurrentDevice();
    onDeviceLostHandler = nullptr;
}

void DeviceLostHelper::WatchDevice(
    DeviceLostNs::wr::com_ptr<::IDXGIDevice> const& newDxgiDevice)
{
    // If we're currently listening to a device, then stop.
    StopWatchingCurrentDevice();

    // Set the current device to the new device.
    device = nullptr;
    DeviceLostNs::wr::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(
        newDxgiDevice.get(),
        reinterpret_cast<::IInspectable**>(DeviceLostNs::wr::put_abi(device))));

    // Get the DXGI Device.
    dxgiDevice = newDxgiDevice;

    // QI For the ID3D11Device4 interface.
    DeviceLostNs::wr::com_ptr<::ID3D11Device4> d3dDevice{
        dxgiDevice.as<::ID3D11Device4>()};

    // Create a wait struct.
    onDeviceLostHandler = nullptr;
    onDeviceLostHandler = ::CreateThreadpoolWait(DeviceLostHelper::OnDeviceLost,
                                                 (PVOID)this, nullptr);

    // Create a handle and a cookie.
    eventHandle.attach(::CreateEvent(nullptr, false, false, nullptr));
    DeviceLostNs::wr::check_bool(bool{eventHandle});
    cookie = 0;

    // Register for device lost.
    ::SetThreadpoolWait(onDeviceLostHandler, eventHandle.get(), nullptr);
    DeviceLostNs::wr::check_hresult(
        d3dDevice->RegisterDeviceRemovedEvent(eventHandle.get(), &cookie));
}

void DeviceLostHelper::StopWatchingCurrentDevice()
{
    if (dxgiDevice && onDeviceLostHandler != nullptr)
    {
        // QI For the ID3D11Device4 interface.
        auto d3dDevice{dxgiDevice.as<::ID3D11Device4>()};

        // Unregister from the device lost event.
        ::CloseThreadpoolWait(onDeviceLostHandler);
        d3dDevice->UnregisterDeviceRemoved(cookie);

        // Clear member variables.
        onDeviceLostHandler = nullptr;
        eventHandle.close();
        cookie = 0;
        device = nullptr;
    }
}

void DeviceLostHelper::DeviceLost(
    DeviceLostNs::wr::delegate<DeviceLostHelper const*,
                               DeviceLostEventArgs const&> const& handler)
{
    deviceLostHandler = handler;
}

void DeviceLostHelper::RaiseDeviceLostEvent(
    DeviceLostNs::wgi::IDirect3DDevice const& oldDevice)
{
    deviceLostHandler(this, DeviceLostEventArgs::Create(oldDevice));
}

void CALLBACK DeviceLostHelper::OnDeviceLost(
    PTP_CALLBACK_INSTANCE /* instance */, PVOID context, PTP_WAIT /* wait */,
    TP_WAIT_RESULT /* waitResult */)
{
    auto deviceLostHelper = reinterpret_cast<DeviceLostHelper*>(context);
    auto oldDevice = deviceLostHelper->device;
    deviceLostHelper->StopWatchingCurrentDevice();
    deviceLostHelper->RaiseDeviceLostEvent(oldDevice);
}
