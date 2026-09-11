#pragma once

// Single source of the user-visible and Win32 resource version.
#define CPC_VERSION_TEXT "0.6.0-preview1"
#define CPC_VERSION_WTEXT L"0.6.0-preview1"
#define CPC_VERSION_NUMERIC 0,6,0,0
#define CPC_VERSION_NUMERIC_TEXT "0.6.0.0"

#ifndef RC_INVOKED
namespace cpc {
inline constexpr wchar_t kVersion[] = CPC_VERSION_WTEXT;
inline constexpr wchar_t kModernMinimumWindows[] = L"Windows 10 version 1809";
}
#endif
