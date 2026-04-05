
// DirectDraw

#define DIRECTDRAW_VERSION 0x0700 // specify version 7.0 of DirectDraw
#define INITGUID
#include <ddraw.h>

#include "DisplaySyncHelper.h"

static bool initDone;
static LPDIRECTDRAW7 directDraw;
static HINSTANCE directDrawModule;
static DWORD monitorFrequency;

DisplaySyncHelper::DisplaySyncHelper()
{
    initDone = false;
    directDraw = 0;
    directDrawModule = 0;
    monitorFrequency = 60;
};

void DisplaySyncHelper::Initialize()
{
    if (!initDone)
    {
        directDrawModule = LoadLibrary(L"ddraw.dll");
        if (directDrawModule != 0)
        {
            typedef HRESULT(WINAPI * DIRECTDRAWCREATEEX)(GUID FAR * lpGuid, LPVOID * lplpDD, REFIID iid, IUnknown FAR * pUnkOuter);
            DIRECTDRAWCREATEEX ddc = (DIRECTDRAWCREATEEX)GetProcAddress(directDrawModule, "DirectDrawCreateEx");
            if (ddc != 0)
            {
                HRESULT r = ddc(0, (LPVOID*)&directDraw, IID_IDirectDraw7, 0);
                if (!FAILED(r))
                {
                    r = directDraw->GetMonitorFrequency(&monitorFrequency);
#if 1
                    if (directDraw != 0)
                    {
                        directDraw->Release();
                        directDraw = 0;
                    }
#endif
                }
            }
        }
        initDone = true;
    }
}

DisplaySyncHelper::~DisplaySyncHelper()
{
    if (directDraw != 0)
        directDraw->Release();
    directDraw = 0;
    if (directDrawModule != 0)
        FreeLibrary(directDrawModule);
    directDrawModule = 0;
};

void DisplaySyncHelper::WaitForVSync()
{
    if (directDraw != 0)
        directDraw->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, 0);
}

DWORD DisplaySyncHelper::GetFrequency()
{
    return monitorFrequency;
}
