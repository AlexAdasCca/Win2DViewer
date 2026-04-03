
#include <windows.h>
#include <winstring.h>
#include "WinrtNsAliases.h"

namespace ns
{
    namespace wr = wna::rt;
    namespace wf = wna::wd::fnd;
}

#ifdef _M_IX86
#pragma comment(linker, "/alternatename:_OS_RoGetActivationFactory@12=_RoGetActivationFactory@12")
#else
#pragma comment(linker, "/alternatename:OS_RoGetActivationFactory=RoGetActivationFactory")
#endif


extern "C"
{
    HRESULT __stdcall OS_RoGetActivationFactory(HSTRING classId, GUID const& iid, void** factory) noexcept;
}

bool StartsWith(std::wstring_view value, std::wstring_view match) noexcept
{
    return 0 == value.compare(0, match.size(), match);
}
#ifndef _M_IX86
extern "C" HRESULT __stdcall WINRT_RoGetActivationFactory2(HSTRING classId, GUID const& iid, void** factory) noexcept
{
#else
extern "C"  HRESULT WINRT_RoGetActivationFactory2(void* classIdRaw, GUID const& iid, void** factory) noexcept
{
    HSTRING classId = (HSTRING)classIdRaw;
#endif
    *factory = nullptr;
    std::wstring_view name( WindowsGetStringRawBuffer(classId, nullptr), WindowsGetStringLen(classId) );
    HMODULE library{ nullptr };

#if 1
    if (StartsWith(name, L"Microsoft.Graphics.Canvas."))
    {
        library = LoadLibraryW(L"Microsoft.Graphics.Canvas.dll");
    }
    else if (StartsWith(name, L"ComponentB."))
    {
        library = LoadLibraryW(L"ComponentB.dll");
    }
    else
    {
        return OS_RoGetActivationFactory(classId, iid, factory);
    }
#endif
    if (!library)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    using DllGetActivationFactory = HRESULT __stdcall(HSTRING classId, void** factory);
    auto call = reinterpret_cast<DllGetActivationFactory*>(GetProcAddress(library, "DllGetActivationFactory"));

    if (!call)
    {
        HRESULT const hr = HRESULT_FROM_WIN32(GetLastError());
        WINRT_VERIFY(FreeLibrary(library));
        return hr;
    }

    ns::wr::com_ptr<ns::wf::IActivationFactory> activationFactory;
    HRESULT const hr = call(classId, activationFactory.put_void());

    if (FAILED(hr))
    {
        WINRT_VERIFY(FreeLibrary(library));
        return hr;
    }
    GUID activationFactoryIid = ns::wr::guid_of<ns::wf::IActivationFactory>();
    if (iid != activationFactoryIid)
    {
        return activationFactory->QueryInterface(iid, factory);
    }

    *factory = activationFactory.detach();
    return S_OK;
}
