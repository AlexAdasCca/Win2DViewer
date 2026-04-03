#pragma once

#include "resource.h"

#include <DispatcherQueue.h>
#include "WinrtNsAliases.h"

wna::wd::sys::DispatcherQueueController CreateDispatcherQueueController();
std::wstring LoadAppString(UINT stringId);

