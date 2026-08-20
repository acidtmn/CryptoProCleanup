#include "cleanup.hpp"

#include <sstream>

namespace cpc {
namespace {

bool EnableTokenPrivilege(const wchar_t* privilege) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) return false;
    TOKEN_PRIVILEGES state{};
    state.PrivilegeCount = 1;
    const bool found = LookupPrivilegeValueW(nullptr, privilege, &state.Privileges[0].Luid) != FALSE;
    if (found) {
        state.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
    }
    const bool adjusted = found && AdjustTokenPrivileges(rawToken, FALSE, &state, sizeof(state), nullptr, nullptr) != FALSE &&
                          GetLastError() == ERROR_SUCCESS;
    CloseHandle(rawToken);
    return adjusted;
}

std::wstring UniqueMountName() {
    static volatile LONG counter = 0;
    std::wostringstream name;
    name << L"CryptoProCleanup_Offline_" << GetCurrentProcessId() << L"_"
         << GetTickCount64() << L"_" << InterlockedIncrement(&counter);
    return name.str();
}

}  // namespace

OfflineRegistryMount::~OfflineRegistryMount() { Close(); }

LONG OfflineRegistryMount::Open(const std::wstring& hivePath, REGSAM access) {
    const LONG previous = Close();
    if (previous != ERROR_SUCCESS) return previous;
    if (hivePath.empty()) return ERROR_INVALID_PARAMETER;
    if (!EnableTokenPrivilege(SE_RESTORE_NAME) || !EnableTokenPrivilege(SE_BACKUP_NAME))
        return ERROR_PRIVILEGE_NOT_HELD;

    LONG loaded = ERROR_ALREADY_EXISTS;
    for (unsigned attempt = 0; attempt < 8 && loaded == ERROR_ALREADY_EXISTS; ++attempt) {
        mountName_ = UniqueMountName();
        loaded = RegLoadKeyW(HKEY_USERS, mountName_.c_str(), hivePath.c_str());
    }
    if (loaded != ERROR_SUCCESS) {
        mountName_.clear();
        return loaded;
    }

    const LONG opened = RegOpenKeyExW(HKEY_USERS, mountName_.c_str(), 0, access, &key_);
    if (opened != ERROR_SUCCESS) {
        const LONG unloaded = RegUnLoadKeyW(HKEY_USERS, mountName_.c_str());
        if (unloaded == ERROR_SUCCESS || unloaded == ERROR_FILE_NOT_FOUND) mountName_.clear();
        return unloaded == ERROR_SUCCESS || unloaded == ERROR_FILE_NOT_FOUND ? opened : unloaded;
    }
    return ERROR_SUCCESS;
}

LONG OfflineRegistryMount::Close() {
    if (key_) {
        RegCloseKey(key_);
        key_ = nullptr;
    }
    if (mountName_.empty()) return ERROR_SUCCESS;

    LONG unloaded = ERROR_BUSY;
    for (unsigned attempt = 0; attempt < 5 && unloaded == ERROR_BUSY; ++attempt) {
        unloaded = RegUnLoadKeyW(HKEY_USERS, mountName_.c_str());
        if (unloaded == ERROR_BUSY) Sleep(20);
    }
    if (unloaded == ERROR_SUCCESS || unloaded == ERROR_FILE_NOT_FOUND) {
        mountName_.clear();
        return ERROR_SUCCESS;
    }
    return unloaded;
}

}  // namespace cpc
