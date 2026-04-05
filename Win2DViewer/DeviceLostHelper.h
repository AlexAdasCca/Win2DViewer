#pragma once

#include "WinrtNsAliases.h"

struct DeviceLostEventArgs
{
    DeviceLostEventArgs(wna::wd::gd3::IDirect3DDevice const& device) : deviceValue(device) {}
    wna::wd::gd3::IDirect3DDevice Device() { return deviceValue; }
    static DeviceLostEventArgs Create(wna::wd::gd3::IDirect3DDevice const& device)
    {
        return DeviceLostEventArgs{ device };
    }

private:
    wna::wd::gd3::IDirect3DDevice deviceValue;
};

struct DeviceLostHelper
{
    DeviceLostHelper() = default;
    ~DeviceLostHelper();

    wna::wd::gd3::IDirect3DDevice CurrentlyWatchedDevice() { return device; }

    void WatchDevice(wna::rt::com_ptr<::IDXGIDevice> const& dxgiDevice);
    void StopWatchingCurrentDevice();
    void DeviceLost(wna::rt::delegate<DeviceLostHelper const*, DeviceLostEventArgs const&> const& handler);
    wna::rt::delegate<DeviceLostHelper const*, DeviceLostEventArgs const&> deviceLostHandler;

private:
    void RaiseDeviceLostEvent(wna::wd::gd3::IDirect3DDevice const& oldDevice);
    static void CALLBACK OnDeviceLost(PTP_CALLBACK_INSTANCE /* instance */,
                                      PVOID context,
                                      PTP_WAIT /* wait */,
                                      TP_WAIT_RESULT /* waitResult */);

private:
    wna::wd::gd3::IDirect3DDevice device;
    wna::rt::com_ptr<::IDXGIDevice> dxgiDevice;
    PTP_WAIT onDeviceLostHandler{ nullptr };
    wna::rt::handle eventHandle;
    DWORD cookie{ 0 };
};
