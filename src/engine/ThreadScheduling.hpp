#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Windows MMCSS ("Games") thread scheduling and EcoQoS power-throttling opt-out (LATENCY_PLAN 1.3).
namespace ThreadScheduling {

HANDLE enableMmcssGames(DWORD& outTaskIdx);
void disableMmcss(HANDLE avrtHandle);
bool disableProcessEcoQoS();

} // namespace ThreadScheduling
