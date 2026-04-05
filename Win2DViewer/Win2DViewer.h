#pragma once

#include "resource.h"

#include "WinrtNsAliases.h"
#include <DispatcherQueue.h>

wna::wd::sys::DispatcherQueueController CreateDispatcherQueueController();
std::wstring LoadAppString(UINT stringId);
