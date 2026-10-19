#pragma once

#include <windows.h>
#include <string>

namespace cpc {
HRESULT SelectDirectory(HWND owner, const std::wstring& current,
                        const std::wstring& title, std::wstring* selected);
}
