#pragma once

struct DisplaySyncHelper
{
	DisplaySyncHelper();
	~DisplaySyncHelper();

	void WaitForVSync();
	void Initialize();

	DWORD GetFrequency();

};

