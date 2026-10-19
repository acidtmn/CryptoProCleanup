#include "folder_picker.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

namespace cpc {
HRESULT SelectDirectory(HWND owner, const std::wstring& current,
                        const std::wstring& title, std::wstring* selected) {
    if (!selected) return E_INVALIDARG;
    selected->clear();

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(result)) return result;
    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (FAILED(result)) return result;
    result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(result)) return result;
    if (!title.empty()) {
        result = dialog->SetTitle(title.c_str());
        if (FAILED(result)) return result;
    }

    // Начинаем с существующей папки, сохраняя обычный выбор дисков и каталогов.
    if (!current.empty()) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr,
                                                  IID_PPV_ARGS(folder.GetAddressOf()))))
            dialog->SetFolder(folder.Get());
    }
    result = dialog->Show(owner);
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<IShellItem> item;
    result = dialog->GetResult(item.GetAddressOf());
    if (FAILED(result)) return result;
    PWSTR path = nullptr;
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(result)) return result;
    try { *selected = path ? path : L""; }
    catch (...) { CoTaskMemFree(path); throw; }
    CoTaskMemFree(path);
    return selected->empty() ? E_FAIL : S_OK;
}
}
