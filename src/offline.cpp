#include "cleanup.hpp"

#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_set>

namespace cpc {
namespace {

// RegLoadKey mounts each disconnected hive below a neutral HKEY_USERS subkey. That temporary
// root is outside redirected HKLM\Software, so child paths are already the hive's physical paths.
constexpr REGSAM kOfflineRead = KEY_READ;
constexpr REGSAM kOfflineWrite = KEY_READ | KEY_WRITE;

class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY key) : key_(key) {}
    ~RegKey() { reset(); }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    HKEY get() const { return key_; }
    HKEY* put() { reset(); return &key_; }
    explicit operator bool() const { return key_ != nullptr; }
    void reset(HKEY key = nullptr) { if (key_) RegCloseKey(key_); key_ = key; }
private:
    HKEY key_ = nullptr;
};

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool SameFile(const std::wstring& left, const std::wstring& right) {
    HANDLE leftHandle = CreateFileW(left.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (leftHandle == INVALID_HANDLE_VALUE) return false;
    HANDLE rightHandle = CreateFileW(right.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rightHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(leftHandle);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION leftInfo{};
    BY_HANDLE_FILE_INFORMATION rightInfo{};
    const bool valid = GetFileInformationByHandle(leftHandle, &leftInfo) &&
                       GetFileInformationByHandle(rightHandle, &rightInfo);
    CloseHandle(rightHandle);
    CloseHandle(leftHandle);
    return valid && leftInfo.dwVolumeSerialNumber == rightInfo.dwVolumeSerialNumber &&
           leftInfo.nFileIndexHigh == rightInfo.nFileIndexHigh &&
           leftInfo.nFileIndexLow == rightInfo.nFileIndexLow;
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left.back() == L'\\' || left.back() == L'/' ? left + right : left + L"\\" + right;
}

std::wstring ParentPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring CanonicalPath(std::wstring path) {
    path = Trim(path);
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (size && size < buffer.size()) path.assign(buffer.data(), size);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    return path;
}

bool PathStartsWith(const std::wstring& candidate, const std::wstring& root) {
    const std::wstring left = ToLower(CanonicalPath(candidate));
    const std::wstring right = ToLower(CanonicalPath(root));
    return !right.empty() && left.size() >= right.size() && left.compare(0, right.size(), right) == 0 &&
           (left.size() == right.size() || left[right.size()] == L'\\');
}

std::wstring VolumeRoot(const std::wstring& path) {
    std::vector<wchar_t> root(32768, L'\0');
    if (!GetVolumePathNameW(CanonicalPath(path).c_str(), root.data(), static_cast<DWORD>(root.size()))) return {};
    return CanonicalPath(root.data());
}

std::wstring ReadRegString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || !size || size > 1024 * 1024) return {};
    std::vector<wchar_t> data(size / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size) != ERROR_SUCCESS) return {};
    return data.data();
}

DWORD ReadRegDword(HKEY key, const wchar_t* name, DWORD fallback = 0) {
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD value = fallback;
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
           type == REG_DWORD ? value : fallback;
}

std::vector<std::wstring> EnumSubkeys(HKEY key) {
    std::vector<std::wstring> result;
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 512> name{};
        DWORD size = static_cast<DWORD>(name.size());
        const LONG status = RegEnumKeyExW(key, index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS) result.emplace_back(name.data(), size);
    }
    return result;
}

std::wstring ReplacePrefixInsensitive(std::wstring value, const std::wstring& prefix, const std::wstring& replacement) {
    if (ToLower(value).rfind(ToLower(prefix), 0) == 0) value.replace(0, prefix.size(), replacement);
    return value;
}

std::wstring RemapOfflinePath(std::wstring value, const std::wstring& volumeRoot, const std::wstring& windowsDirectory) {
    value = Trim(value);
    if (value.empty()) return {};
    if (value.front() == L'"') {
        const size_t quote = value.find(L'"', 1);
        if (quote != std::wstring::npos) value = value.substr(1, quote - 1);
    }
    const std::wstring programFiles = JoinPath(volumeRoot, L"Program Files");
    const std::wstring programFilesX86 = JoinPath(volumeRoot, L"Program Files (x86)");
    const std::wstring programData = JoinPath(volumeRoot, L"ProgramData");
    for (const auto& replacement : {
        std::pair<const wchar_t*, std::wstring>{L"%ProgramFiles(x86)%", programFilesX86},
        {L"%ProgramFiles%", programFiles}, {L"%ProgramData%", programData},
        {L"%SystemRoot%", windowsDirectory}, {L"%windir%", windowsDirectory}
    }) value = ReplacePrefixInsensitive(value, replacement.first, replacement.second);
    value = ReplacePrefixInsensitive(value, L"\\SystemRoot", windowsDirectory);
    value = ReplacePrefixInsensitive(value, L"System32", JoinPath(windowsDirectory, L"System32"));
    value = ReplacePrefixInsensitive(value, L"%SystemDrive%", volumeRoot);
    if (value.size() >= 3 && iswalpha(value[0]) && value[1] == L':' && value[2] == L'\\') {
        value = JoinPath(volumeRoot, value.substr(3));
    }
    return CanonicalPath(value);
}

std::wstring Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::wstring UniqueFolder(const std::wstring& parent, const std::wstring& prefix) {
    const std::wstring base = JoinPath(parent, prefix + Timestamp());
    std::wstring candidate = base;
    for (unsigned suffix = 1; GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 1000; ++suffix) {
        candidate = base + L"-" + std::to_wstring(suffix);
    }
    return candidate;
}

bool HasCryptoProPathName(const std::wstring& path) {
    const std::wstring lower = ToLower(path);
    return lower.find(L"\\crypto pro") != std::wstring::npos || lower.find(L"\\cryptopro") != std::wstring::npos ||
           lower.find(L"\\крипто про") != std::wstring::npos || lower.find(L"\\криптопро") != std::wstring::npos;
}

bool ContainsVerifiedBinary(const std::wstring& directory, unsigned depth = 0) {
    if (!DirectoryExists(directory) || depth > 4) return false;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = JoinPath(directory, name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (ContainsVerifiedBinary(child, depth + 1)) { found = true; break; }
        } else {
            const std::wstring lower = ToLower(name);
            if (lower.size() >= 4 && (lower.substr(lower.size() - 4) == L".exe" ||
                                      lower.substr(lower.size() - 4) == L".dll" ||
                                      lower.substr(lower.size() - 4) == L".sys") &&
                VerifyCryptoProSignature(child)) { found = true; break; }
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return found;
}

void ScanOfflineUninstallRoot(OfflineScanResult& offline, HKEY software, const std::wstring& rootPath,
                              const std::wstring& architecture) {
    RegKey root;
    const LONG opened = RegOpenKeyExW(software, rootPath.c_str(), 0, kOfflineRead, root.put());
    offline.diagnostics.push_back(L"Open uninstall root [" + rootPath + L"]: " + std::to_wstring(opened));
    if (opened != ERROR_SUCCESS) return;
    const size_t before = offline.scan.products.size();
    for (const auto& subkey : EnumSubkeys(root.get())) {
        RegKey key;
        if (RegOpenKeyExW(root.get(), subkey.c_str(), 0, kOfflineRead, key.put()) != ERROR_SUCCESS) continue;
        const std::wstring publisher = ReadRegString(key.get(), L"Publisher");
        const std::wstring displayName = ReadRegString(key.get(), L"DisplayName");
        if (displayName.empty() || !IsCryptoProPublisher(publisher)) continue;
        InstalledProduct product;
        product.displayName = displayName;
        product.version = ReadRegString(key.get(), L"DisplayVersion");
        product.publisher = publisher;
        product.architecture = architecture;
        product.uninstallString = ReadRegString(key.get(), L"UninstallString");
        product.quietUninstallString = ReadRegString(key.get(), L"QuietUninstallString");
        product.installLocation = RemapOfflinePath(ReadRegString(key.get(), L"InstallLocation"), offline.volumeRoot, offline.windowsDirectory);
        product.registryKey = rootPath + L"\\" + subkey;
        product.productCode = ReadRegString(key.get(), L"ProductCode");
        if (product.productCode.empty() && IsGuid(subkey)) product.productCode = subkey;
        product.msi = ReadRegDword(key.get(), L"WindowsInstaller") == 1 || IsGuid(product.productCode);
        product.risk = IsHighRiskProduct(displayName) ? RiskLevel::High : RiskLevel::Normal;
        offline.scan.products.push_back(std::move(product));
    }
    offline.diagnostics.push_back(L"Confirmed products in [" + rootPath + L"]: " +
                                  std::to_wstring(offline.scan.products.size() - before));
}

bool IsLicenseValueName(const std::wstring& name) {
    const std::wstring lower = ToLower(name);
    return lower == L"wlproductid" || lower == L"productid" || lower == L"serialnumber" ||
           lower == L"license" || lower == L"licensekey" || lower == L"licence" || lower == L"licencekey";
}

void ScanOfflineLicenseTree(ScanResult& scan, HKEY key, const std::wstring& displayPath, int depth,
                            std::unordered_set<std::wstring>* seen) {
    if (depth > 16) return;
    DWORD valueCount = 0, maxName = 0, maxData = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &valueCount, &maxName, &maxData,
                         nullptr, nullptr) == ERROR_SUCCESS) {
        std::vector<wchar_t> name(maxName + 2, L'\0');
        std::vector<BYTE> data(maxData + sizeof(wchar_t) * 2, 0);
        for (DWORD index = 0; index < valueCount; ++index) {
            DWORD nameSize = static_cast<DWORD>(name.size());
            DWORD dataSize = static_cast<DWORD>(data.size());
            DWORD type = 0;
            if (RegEnumValueW(key, index, name.data(), &nameSize, nullptr, &type, data.data(), &dataSize) != ERROR_SUCCESS) continue;
            const std::wstring valueName(name.data(), nameSize);
            if (!IsLicenseValueName(valueName) || (type != REG_SZ && type != REG_EXPAND_SZ) || dataSize < sizeof(wchar_t)) continue;
            data.resize(dataSize + sizeof(wchar_t) * 2, 0);
            const std::wstring value = Trim(reinterpret_cast<wchar_t*>(data.data()));
            if (value.size() < 5 || value.size() > 512 || !seen->insert(ToLower(value)).second) continue;
            const int priority = ToLower(valueName) == L"wlproductid" ? 30 : 50;
            scan.licenses.push_back({FileName(displayPath), displayPath, valueName, value, MaskLicense(value), priority});
        }
    }
    for (const auto& child : EnumSubkeys(key)) {
        RegKey subkey;
        if (RegOpenKeyExW(key, child.c_str(), 0, kOfflineRead, subkey.put()) == ERROR_SUCCESS)
            ScanOfflineLicenseTree(scan, subkey.get(), displayPath + L"\\" + child, depth + 1, seen);
    }
}

std::wstring NormalizeLicense(std::wstring value) {
    value = ToLower(Trim(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t character) {
        return iswspace(character) || character == L'-';
    }), value.end());
    return value;
}

void PreferCompleteLicenses(ScanResult& scan) {
    std::stable_sort(scan.licenses.begin(), scan.licenses.end(), [](const LicenseEntry& left, const LicenseEntry& right) {
        if (left.sourcePriority != right.sourcePriority) return left.sourcePriority > right.sourcePriority;
        return NormalizeLicense(left.fullValue).size() > NormalizeLicense(right.fullValue).size();
    });
    std::vector<LicenseEntry> result;
    for (const auto& candidate : scan.licenses) {
        const std::wstring value = NormalizeLicense(candidate.fullValue);
        const bool redundant = std::any_of(result.begin(), result.end(), [&](const LicenseEntry& existing) {
            const std::wstring other = NormalizeLicense(existing.fullValue);
            return value == other || (value.size() < other.size() && other.rfind(value, 0) == 0 &&
                                      candidate.sourcePriority <= existing.sourcePriority);
        });
        if (!redundant) result.push_back(candidate);
    }
    scan.licenses = std::move(result);
}

void ScanOfflineLicenses(OfflineScanResult& offline, HKEY software) {
    std::unordered_set<std::wstring> seen;
    RegKey userData;
    constexpr wchar_t installerPath[] = L"Microsoft\\Windows\\CurrentVersion\\Installer\\UserData";
    const LONG installerOpened = RegOpenKeyExW(software, installerPath, 0, kOfflineRead, userData.put());
    offline.diagnostics.push_back(L"Open Installer UserData: " + std::to_wstring(installerOpened));
    if (installerOpened == ERROR_SUCCESS) {
        for (const auto& sid : EnumSubkeys(userData.get())) {
            RegKey products;
            if (RegOpenKeyExW(userData.get(), (sid + L"\\Products").c_str(), 0, kOfflineRead, products.put()) != ERROR_SUCCESS) continue;
            for (const auto& packedProduct : EnumSubkeys(products.get())) {
                RegKey properties;
                const std::wstring relative = sid + L"\\Products\\" + packedProduct + L"\\InstallProperties";
                if (RegOpenKeyExW(userData.get(), relative.c_str(), 0, kOfflineRead, properties.put()) != ERROR_SUCCESS) continue;
                const std::wstring publisher = ReadRegString(properties.get(), L"Publisher");
                if (!IsCryptoProPublisher(publisher)) continue;
                std::wstring productName = ReadRegString(properties.get(), L"DisplayName");
                if (productName.empty()) productName = L"CryptoPro MSI product";
                const std::wstring productCode = UnpackMsiProductCode(packedProduct);
                const std::wstring version = ReadRegString(properties.get(), L"DisplayVersion");
                const bool alreadyDetected = std::any_of(offline.scan.products.begin(), offline.scan.products.end(),
                    [&](const InstalledProduct& product) {
                        return (!productCode.empty() && ToLower(product.productCode) == ToLower(productCode)) ||
                               (ToLower(product.displayName) == ToLower(productName) && product.version == version);
                    });
                if (!alreadyDetected) {
                    InstalledProduct product;
                    product.displayName = productName;
                    product.version = version;
                    product.publisher = publisher;
                    product.productCode = productCode;
                    product.msi = true;
                    product.uninstallString = ReadRegString(properties.get(), L"UninstallString");
                    product.installLocation = RemapOfflinePath(ReadRegString(properties.get(), L"InstallLocation"),
                                                               offline.volumeRoot, offline.windowsDirectory);
                    const std::wstring lowerLocation = ToLower(product.installLocation);
                    product.architecture = offline.scan.osArchitecture == L"x86" ||
                                           lowerLocation.find(L"program files (x86)") != std::wstring::npos ? L"x86" : L"x64";
                    product.risk = IsHighRiskProduct(product.displayName) ? RiskLevel::High : RiskLevel::Normal;
                    if (!productCode.empty()) {
                        for (const wchar_t* uninstallRoot : {
                            L"Microsoft\\Windows\\CurrentVersion\\Uninstall",
                            L"WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"}) {
                            RegKey uninstall;
                            const std::wstring candidate = std::wstring(uninstallRoot) + L"\\" + productCode;
                            if (RegOpenKeyExW(software, candidate.c_str(), 0, kOfflineRead, uninstall.put()) == ERROR_SUCCESS) {
                                product.registryKey = candidate;
                                break;
                            }
                        }
                    }
                    offline.scan.products.push_back(std::move(product));
                }
                const std::wstring value = Trim(ReadRegString(properties.get(), L"ProductID"));
                if (value.size() < 5 || value.size() > 512 || !seen.insert(ToLower(value)).second) continue;
                offline.scan.licenses.push_back({productName, L"OFFLINE_SOFTWARE\\" + std::wstring(installerPath) + L"\\" + relative,
                                                 L"ProductID", value, MaskLicense(value), 100});
            }
        }
    }
    for (const wchar_t* branch : {L"Crypto Pro", L"WOW6432Node\\Crypto Pro"}) {
        RegKey root;
        if (RegOpenKeyExW(software, branch, 0, kOfflineRead, root.put()) == ERROR_SUCCESS)
            ScanOfflineLicenseTree(offline.scan, root.get(), L"OFFLINE_SOFTWARE\\" + std::wstring(branch), 0, &seen);
    }
    PreferCompleteLicenses(offline.scan);
}

bool IsSystemProfileDirectory(const std::wstring& name) {
    const std::wstring lower = ToLower(name);
    return lower == L"default" || lower == L"default user" || lower == L"public" ||
           lower == L"all users" || lower == L"defaultaccount" || lower == L"wdagutilityaccount";
}

bool AddOfflineProfile(OfflineScanResult& offline, const std::wstring& sid, const std::wstring& path) {
    const std::wstring canonical = CanonicalPath(path);
    const std::wstring name = FileName(canonical);
    if (name.empty() || IsSystemProfileDirectory(name) || !DirectoryExists(canonical) ||
        !FileExists(JoinPath(canonical, L"NTUSER.DAT"))) return false;
    const bool duplicate = std::any_of(offline.scan.profiles.begin(), offline.scan.profiles.end(),
        [&](const UserProfile& profile) { return ToLower(CanonicalPath(profile.profilePath)) == ToLower(canonical); });
    if (duplicate) return false;
    offline.scan.profiles.push_back({sid, name, canonical, false, true});
    return true;
}

void ScanOfflineProfiles(Language language, OfflineScanResult& offline, HKEY software) {
    RegKey profiles;
    constexpr wchar_t profileList[] = L"Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
    const LONG profilesOpened = RegOpenKeyExW(software, profileList, 0, kOfflineRead, profiles.put());
    offline.diagnostics.push_back(L"Open ProfileList: " + std::to_wstring(profilesOpened));
    if (profilesOpened == ERROR_SUCCESS) {
        for (const auto& sid : EnumSubkeys(profiles.get())) {
            if (sid.rfind(L"S-1-5-21-", 0) != 0) continue;
            RegKey profile;
            if (RegOpenKeyExW(profiles.get(), sid.c_str(), 0, kOfflineRead, profile.put()) != ERROR_SUCCESS) continue;
            AddOfflineProfile(offline, sid,
                RemapOfflinePath(ReadRegString(profile.get(), L"ProfileImagePath"),
                                 offline.volumeRoot, offline.windowsDirectory));
        }
    }

    // A damaged or redirected ProfileList must not prevent read-only certificate rescue.
    const size_t beforeFallback = offline.scan.profiles.size();
    const std::wstring users = JoinPath(offline.volumeRoot, L"Users");
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(users, L"*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L".." || !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            AddOfflineProfile(offline, L"OFFLINE-FS:" + ToLower(name), JoinPath(users, name));
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    if (offline.scan.profiles.size() > beforeFallback) {
        offline.scan.warnings.push_back(Tr(language,
            L"Часть офлайн-профилей найдена напрямую в папке Users (резервный способ).",
            L"Some offline profiles were recovered directly from the Users directory."));
    }
}

std::wstring TargetIdentity(const OfflineCleanupTarget& target) {
    return std::to_wstring(static_cast<int>(target.type)) + L"|" + std::to_wstring(static_cast<int>(target.hive)) + L"|" +
           ToLower(target.path.empty() ? target.registrySubkey : CanonicalPath(target.path));
}

void AddOfflineTarget(OfflineScanResult& offline, OfflineCleanupTarget target,
                      std::unordered_set<std::wstring>* identities) {
    if (!target.path.empty() && (!PathStartsWith(target.path, offline.volumeRoot) || IsProtectedPath(target.path))) {
        target.protectedItem = true;
        offline.scan.protectedItems.push_back(target.path);
        return;
    }
    const std::wstring displayRegistry = L"HKLM\\SOFTWARE\\" + target.registrySubkey;
    if (!target.registrySubkey.empty() && IsProtectedRegistryPath(displayRegistry)) {
        target.protectedItem = true;
        offline.scan.protectedItems.push_back(displayRegistry);
        return;
    }
    if (target.verified && identities->insert(TargetIdentity(target)).second) offline.targets.push_back(std::move(target));
}

void AddOfflineDirectoryTargets(OfflineScanResult& offline, HKEY software,
                                std::unordered_set<std::wstring>* identities) {
    std::vector<std::wstring> candidates;
    for (const auto& product : offline.scan.products) {
        if (!product.installLocation.empty() && DirectoryExists(product.installLocation) &&
            PathStartsWith(product.installLocation, offline.volumeRoot) && HasCryptoProPathName(product.installLocation)) {
            candidates.push_back(product.installLocation);
        }
    }
    for (const auto& candidate : {
        JoinPath(offline.volumeRoot, L"Program Files\\Crypto Pro"),
        JoinPath(offline.volumeRoot, L"Program Files (x86)\\Crypto Pro"),
        JoinPath(offline.volumeRoot, L"ProgramData\\Crypto Pro")
    }) {
        if (DirectoryExists(candidate) && (ContainsVerifiedBinary(candidate) ||
            std::any_of(candidates.begin(), candidates.end(), [&](const std::wstring& item) {
                return PathStartsWith(item, candidate) || PathStartsWith(candidate, item);
            }))) candidates.push_back(candidate);
    }
    std::sort(candidates.begin(), candidates.end(), [](const std::wstring& left, const std::wstring& right) {
        return CanonicalPath(left).size() < CanonicalPath(right).size();
    });
    std::vector<std::wstring> roots;
    for (const auto& candidate : candidates) {
        if (std::none_of(roots.begin(), roots.end(), [&](const std::wstring& root) { return PathStartsWith(candidate, root); }))
            roots.push_back(candidate);
    }
    for (const auto& root : roots) {
        AddOfflineTarget(offline, {TargetType::Directory, OfflineHive::Software, L"CryptoPro installation directory",
                                   root, {}, L"Confirmed offline product installation path", true, false}, identities);
    }

    for (const auto& product : offline.scan.products) {
        if (product.registryKey.empty()) continue;
        AddOfflineTarget(offline, {TargetType::RegistryTree, OfflineHive::Software, product.displayName,
                                   {}, product.registryKey, L"Confirmed publisher uninstall registration", true, false}, identities);
    }
    for (const wchar_t* branch : {L"Crypto Pro", L"WOW6432Node\\Crypto Pro"}) {
        RegKey key;
        if (RegOpenKeyExW(software, branch, 0, kOfflineRead, key.put()) == ERROR_SUCCESS) {
            AddOfflineTarget(offline, {TargetType::RegistryTree, OfflineHive::Software, L"CryptoPro settings branch",
                                       {}, branch, L"Vendor registry branch with protected key paths excluded", true, false}, identities);
        }
    }
}

void ScanOfflineProviders(OfflineScanResult& offline, HKEY software,
                          const std::vector<std::wstring>& approvedRoots,
                          std::unordered_set<std::wstring>* identities) {
    for (const wchar_t* providerRoot : {
        L"Microsoft\\Cryptography\\Defaults\\Provider",
        L"WOW6432Node\\Microsoft\\Cryptography\\Defaults\\Provider"
    }) {
        RegKey root;
        if (RegOpenKeyExW(software, providerRoot, 0, kOfflineRead, root.put()) != ERROR_SUCCESS) continue;
        for (const auto& name : EnumSubkeys(root.get())) {
            if (!IsCryptoProName(name)) continue;
            RegKey key;
            if (RegOpenKeyExW(root.get(), name.c_str(), 0, kOfflineRead, key.put()) != ERROR_SUCCESS) continue;
            const std::wstring image = RemapOfflinePath(ReadRegString(key.get(), L"Image Path"), offline.volumeRoot, offline.windowsDirectory);
            const bool verified = !image.empty() && FileExists(image) &&
                (VerifyCryptoProSignature(image) || std::any_of(approvedRoots.begin(), approvedRoots.end(),
                    [&](const std::wstring& approved) { return PathStartsWith(image, approved); }));
            if (!verified) continue;
            AddOfflineTarget(offline, {TargetType::RegistryTree, OfflineHive::Software, name, {},
                                       std::wstring(providerRoot) + L"\\" + name,
                                       L"Confirmed CryptoPro provider registration", true, false}, identities);
        }
    }
}

void ScanOfflineServices(OfflineScanResult& offline, HKEY system,
                         const std::vector<std::wstring>& approvedRoots,
                         std::unordered_set<std::wstring>* identities) {
    for (const auto& controlSet : EnumSubkeys(system)) {
        if (ToLower(controlSet).rfind(L"controlset", 0) != 0) continue;
        RegKey services;
        if (RegOpenKeyExW(system, (controlSet + L"\\Services").c_str(), 0, kOfflineRead, services.put()) != ERROR_SUCCESS) continue;
        for (const auto& serviceName : EnumSubkeys(services.get())) {
            RegKey service;
            if (RegOpenKeyExW(services.get(), serviceName.c_str(), 0, kOfflineRead, service.put()) != ERROR_SUCCESS) continue;
            const std::wstring image = RemapOfflinePath(ReadRegString(service.get(), L"ImagePath"), offline.volumeRoot, offline.windowsDirectory);
            if (image.empty() || !FileExists(image)) continue;
            const bool approved = std::any_of(approvedRoots.begin(), approvedRoots.end(),
                [&](const std::wstring& root) { return PathStartsWith(image, root); });
            if (!approved && !VerifyCryptoProSignature(image)) continue;
            const TargetType type = ReadRegDword(service.get(), L"Type") & SERVICE_KERNEL_DRIVER ? TargetType::DriverService : TargetType::Service;
            AddOfflineTarget(offline, {type, OfflineHive::System, serviceName, {},
                                       controlSet + L"\\Services\\" + serviceName,
                                       L"Service image is a verified CryptoPro file", true, false}, identities);
            if (!approved) {
                AddOfflineTarget(offline, {TargetType::File, OfflineHive::System, FileName(image), image, {},
                                           L"Verified CryptoPro service or driver image", true, false}, identities);
            }
        }
    }
}

bool CopyFileSafe(const std::wstring& source, const std::wstring& destination, std::wstring* error) {
    if (!EnsureDirectory(ParentPath(destination), error)) return false;
    if (CopyFileW(source.c_str(), destination.c_str(), TRUE)) return true;
    if (error) *error = GetLastErrorMessage(GetLastError());
    return false;
}

bool CopyTreeSafe(const std::wstring& source, const std::wstring& destination,
                  const std::wstring& offlineRoot, std::wstring* error) {
    if (!PathStartsWith(source, offlineRoot) || IsProtectedPath(source)) return true;
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return CopyFileSafe(source, destination, error);
    if (!EnsureDirectory(destination, error)) return false;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(source, L"*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool success = true;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = JoinPath(source, name);
        if (IsProtectedPath(child) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!CopyTreeSafe(child, JoinPath(destination, name), offlineRoot, error)) { success = false; break; }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return success;
}

bool DeleteTreeSafe(const std::wstring& path, const std::wstring& offlineRoot,
                    bool* retained, DWORD* error) {
    if (!PathStartsWith(path, offlineRoot) || IsProtectedPath(path)) {
        if (retained) *retained = true;
        return true;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (retained) *retained = true;
        return true;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (DeleteFileW(path.c_str())) return true;
        if (error) *error = GetLastError();
        return false;
    }
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        bool success = true;
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            if (!DeleteTreeSafe(JoinPath(path, name), offlineRoot, retained, error)) { success = false; break; }
        } while (FindNextFileW(search, &data));
        FindClose(search);
        if (!success) return false;
    }
    if (RemoveDirectoryW(path.c_str())) return true;
    const DWORD code = GetLastError();
    if ((code == ERROR_DIR_NOT_EMPTY || code == ERROR_ACCESS_DENIED) && retained && *retained) return true;
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    if (error) *error = code;
    return false;
}

bool DeleteRegistryTreeProtected(HKEY root, const std::wstring& subkey, const std::wstring& display,
                                 bool* retained, DWORD* error) {
    if (IsProtectedRegistryPath(display)) {
        if (retained) *retained = true;
        return true;
    }
    RegKey key;
    const LONG opened = RegOpenKeyExW(root, subkey.c_str(), 0, kOfflineWrite, key.put());
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) { if (error) *error = opened; return false; }
    for (const auto& child : EnumSubkeys(key.get())) {
        if (!DeleteRegistryTreeProtected(key.get(), child, display + L"\\" + child, retained, error)) return false;
    }
    for (;;) {
        std::array<wchar_t, 512> name{};
        DWORD size = static_cast<DWORD>(name.size());
        const LONG status = RegEnumValueW(key.get(), 0, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) {
            if (error) *error = status;
            return false;
        }
        const LONG deletedValue = RegDeleteValueW(key.get(), name.data());
        if (deletedValue != ERROR_SUCCESS) {
            if (error) *error = deletedValue;
            return false;
        }
    }
    key.reset();
    const LONG deleted = RegDeleteKeyExW(root, subkey.c_str(), 0, 0);
    if (deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND ||
        ((deleted == ERROR_ACCESS_DENIED || deleted == ERROR_KEY_HAS_CHILDREN) && retained && *retained)) return true;
    if (error) *error = deleted;
    return false;
}

unsigned long long FileLength(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    ULARGE_INTEGER length{};
    length.HighPart = data.nFileSizeHigh;
    length.LowPart = data.nFileSizeLow;
    return length.QuadPart;
}

unsigned long long AddSize(unsigned long long left, unsigned long long right) {
    constexpr unsigned long long cap = 8ull * 1024 * 1024 * 1024 * 1024;
    if (left >= cap || right >= cap - left) return cap;
    return left + right;
}

unsigned long long BackupTreeLength(const std::wstring& path, const std::wstring& offlineRoot,
                                    unsigned depth = 0) {
    if (depth > 64 || path.empty() || !PathStartsWith(path, offlineRoot) || IsProtectedPath(path)) return 0;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (!(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return FileLength(path);
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return 0;
    unsigned long long total = 0;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        total = AddSize(total, BackupTreeLength(JoinPath(path, name), offlineRoot, depth + 1));
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return total;
}

}  // namespace

unsigned long long EstimateOfflineBackupMinimum(const OfflineScanResult& offline) {
    constexpr unsigned long long mib = 1024ull * 1024;
    unsigned long long payload = AddSize(FileLength(offline.softwareHivePath), FileLength(offline.systemHivePath));
    std::unordered_set<std::wstring> counted;
    for (const auto& target : offline.targets) {
        if (!target.verified || target.protectedItem || target.path.empty()) continue;
        const std::wstring canonical = CanonicalPath(target.path);
        if (canonical.empty() || !counted.insert(ToLower(canonical)).second) continue;
        payload = AddSize(payload, BackupTreeLength(canonical, offline.volumeRoot));
    }
    const unsigned long long safetyMargin = std::max(128ull * mib, payload / 4);
    return std::max(256ull * mib, AddSize(payload, safetyMargin));
}

OfflineScanResult ScanOfflineWindows(Language language, const std::wstring& requestedWindowsDirectory,
                                     const ProgressCallback& progress) {
    OfflineScanResult offline;
    offline.windowsDirectory = CanonicalPath(requestedWindowsDirectory);
    if (ToLower(FileName(offline.windowsDirectory)) != L"windows" &&
        DirectoryExists(JoinPath(offline.windowsDirectory, L"Windows"))) {
        offline.windowsDirectory = JoinPath(offline.windowsDirectory, L"Windows");
    }
    offline.volumeRoot = VolumeRoot(offline.windowsDirectory);
    offline.softwareHivePath = JoinPath(offline.windowsDirectory, L"System32\\Config\\SOFTWARE");
    offline.systemHivePath = JoinPath(offline.windowsDirectory, L"System32\\Config\\SYSTEM");
    offline.diagnostics.push_back(L"Requested path: " + requestedWindowsDirectory);
    offline.diagnostics.push_back(L"Resolved Windows path: " + offline.windowsDirectory);
    offline.diagnostics.push_back(L"Resolved volume root: " + offline.volumeRoot);
    offline.diagnostics.push_back(L"SOFTWARE hive: " + offline.softwareHivePath);
    offline.diagnostics.push_back(L"SYSTEM hive: " + offline.systemHivePath);
    std::vector<wchar_t> currentWindows(32768, L'\0');
    const UINT currentWindowsLength = GetWindowsDirectoryW(currentWindows.data(), static_cast<UINT>(currentWindows.size()));
    const std::wstring currentWindowsPath = currentWindowsLength && currentWindowsLength < currentWindows.size() ?
        CanonicalPath(currentWindows.data()) : std::wstring();
    const std::wstring currentSoftwareHive = JoinPath(currentWindowsPath, L"System32\\Config\\SOFTWARE");
    const std::wstring currentSystemHive = JoinPath(currentWindowsPath, L"System32\\Config\\SYSTEM");
    if (offline.windowsDirectory.empty() ||
        (!currentWindowsPath.empty() && ToLower(currentWindowsPath) == ToLower(offline.windowsDirectory)) ||
        SameFile(offline.softwareHivePath, currentSoftwareHive) || SameFile(offline.systemHivePath, currentSystemHive)) {
        offline.scan.warnings.push_back(Tr(language, L"Нельзя открыть работающую Windows как офлайн-систему.", L"The running Windows installation cannot be opened as an offline system."));
        return offline;
    }
    if (!FileExists(offline.softwareHivePath)) {
        offline.scan.warnings.push_back(Tr(language, L"Не найден офлайн-улей SOFTWARE.", L"The offline SOFTWARE hive was not found."));
        return offline;
    }
    if (!FileExists(offline.systemHivePath))
        offline.scan.warnings.push_back(Tr(language, L"Не найден офлайн-улей SYSTEM: спасение данных доступно, очистка отключена.",
                                                     L"The offline SYSTEM hive was not found: data rescue is available, cleanup is disabled."));
    if (progress) progress(Tr(language, L"Открытие офлайн-реестра только для чтения...", L"Opening offline registry read-only..."), 10);
    OfflineRegistryMount software;
    const LONG softwareStatus = software.Open(offline.softwareHivePath, KEY_READ);
    offline.diagnostics.push_back(L"RegLoadKey SOFTWARE: " + std::to_wstring(softwareStatus) +
                                  L" (" + GetLastErrorMessage(softwareStatus) + L")");
    if (softwareStatus != ERROR_SUCCESS) {
        std::wostringstream details;
        details << Tr(language, L"Не удалось открыть офлайн-улей SOFTWARE, код ",
                                L"Could not open the offline SOFTWARE hive, code ") << softwareStatus;
        offline.scan.warnings.push_back(details.str());
        return offline;
    }
    RegKey version;
    if (RegOpenKeyExW(software.get(), L"Microsoft\\Windows NT\\CurrentVersion", 0, kOfflineRead, version.put()) == ERROR_SUCCESS) {
        offline.scan.osName = ReadRegString(version.get(), L"ProductName");
        const std::wstring build = ReadRegString(version.get(), L"CurrentBuildNumber");
        if (!build.empty()) offline.scan.osName += L" (build " + build + L")";
    }
    RegKey wow;
    offline.scan.osArchitecture = RegOpenKeyExW(software.get(), L"WOW6432Node", 0, kOfflineRead, wow.put()) == ERROR_SUCCESS ? L"x64/ARM64" : L"x86";
    if (progress) progress(Tr(language, L"Поиск офлайн-продуктов и лицензий...", L"Scanning offline products and licenses..."), 30);
    ScanOfflineUninstallRoot(offline, software.get(), L"Microsoft\\Windows\\CurrentVersion\\Uninstall", offline.scan.osArchitecture == L"x86" ? L"x86" : L"x64");
    offline.diagnostics.push_back(L"Products after native uninstall root: " + std::to_wstring(offline.scan.products.size()));
    ScanOfflineUninstallRoot(offline, software.get(), L"WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"x86");
    offline.diagnostics.push_back(L"Products after WOW uninstall root: " + std::to_wstring(offline.scan.products.size()));
    ScanOfflineLicenses(offline, software.get());
    offline.diagnostics.push_back(L"Products after Installer UserData: " + std::to_wstring(offline.scan.products.size()));
    offline.diagnostics.push_back(L"Full license candidates: " + std::to_wstring(offline.scan.licenses.size()));
    ScanOfflineProfiles(language, offline, software.get());
    offline.diagnostics.push_back(L"Offline profiles with NTUSER.DAT: " + std::to_wstring(offline.scan.profiles.size()));
    ScanOfflineMachineCertificates(language, software.get(), &offline.scan.certificates, &offline.scan.warnings);
    offline.diagnostics.push_back(L"Local-machine public certificates: " + std::to_wstring(offline.scan.certificates.size()));

    offline.scan.protectedItems = {
        L"Offline user and local-machine certificate stores (read-only)",
        L"Offline ProgramData\\Crypto Pro\\Crypto private-key containers",
        L"Offline Crypto Pro\\Settings\\...\\Keys and container metadata",
        L"Hardware tokens and smart cards"
    };
    std::unordered_set<std::wstring> identities;
    AddOfflineDirectoryTargets(offline, software.get(), &identities);
    std::vector<std::wstring> approvedRoots;
    for (const auto& target : offline.targets) if (target.type == TargetType::Directory) approvedRoots.push_back(target.path);
    ScanOfflineProviders(offline, software.get(), approvedRoots, &identities);
    version.reset();
    wow.reset();
    const LONG softwareUnload = software.Close();
    offline.diagnostics.push_back(L"RegUnLoadKey SOFTWARE: " + std::to_wstring(softwareUnload) +
                                  L" (" + GetLastErrorMessage(softwareUnload) + L")");
    if (softwareUnload != ERROR_SUCCESS)
        offline.scan.warnings.push_back(Tr(language, L"Не удалось выгрузить временно подключённый улей SOFTWARE, код ",
                                                      L"Could not unload the temporarily mounted SOFTWARE hive, code ") +
                                        std::to_wstring(softwareUnload));

    if (progress) progress(Tr(language, L"Чтение открытых сертификатов офлайн-профилей...", L"Reading public certificates from offline profiles..."), 55);
    std::vector<CertificateEntry> machineCertificates = std::move(offline.scan.certificates);
    ScanUserCertificates(offline.scan.profiles, &offline.scan.certificates, &offline.scan.warnings, {});
    offline.scan.certificates.insert(offline.scan.certificates.end(),
                                     std::make_move_iterator(machineCertificates.begin()),
                                     std::make_move_iterator(machineCertificates.end()));
    std::stable_sort(offline.scan.certificates.begin(), offline.scan.certificates.end(),
        [](const CertificateEntry& left, const CertificateEntry& right) {
            if (ToLower(left.profileName) != ToLower(right.profileName))
                return ToLower(left.profileName) < ToLower(right.profileName);
            if (ToLower(left.subject) != ToLower(right.subject))
                return ToLower(left.subject) < ToLower(right.subject);
            return left.thumbprint < right.thumbprint;
        });
    offline.diagnostics.push_back(L"Public certificates after user profiles: " +
                                  std::to_wstring(offline.scan.certificates.size()));

    if (FileExists(offline.systemHivePath)) {
        OfflineRegistryMount system;
        const LONG systemStatus = system.Open(offline.systemHivePath, KEY_READ);
        offline.diagnostics.push_back(L"RegLoadKey SYSTEM: " + std::to_wstring(systemStatus) +
                                      L" (" + GetLastErrorMessage(systemStatus) + L")");
        if (systemStatus == ERROR_SUCCESS) {
            ScanOfflineServices(offline, system.get(), approvedRoots, &identities);
            const LONG systemUnload = system.Close();
            offline.diagnostics.push_back(L"RegUnLoadKey SYSTEM: " + std::to_wstring(systemUnload) +
                                          L" (" + GetLastErrorMessage(systemUnload) + L")");
            offline.cleanupCapable = systemUnload == ERROR_SUCCESS && softwareUnload == ERROR_SUCCESS;
            if (systemUnload != ERROR_SUCCESS)
                offline.scan.warnings.push_back(Tr(language, L"Не удалось выгрузить временно подключённый улей SYSTEM, код ",
                                                              L"Could not unload the temporarily mounted SYSTEM hive, code ") +
                                                std::to_wstring(systemUnload));
        } else {
            std::wostringstream details;
            details << Tr(language, L"Не удалось открыть офлайн-улей SYSTEM, код ",
                                    L"Could not open the offline SYSTEM hive, code ") << systemStatus
                    << Tr(language, L". Спасение данных доступно, очистка отключена.",
                                    L". Data rescue is available, cleanup is disabled.");
            offline.scan.warnings.push_back(details.str());
        }
    }
    if (offline.scan.products.empty())
        offline.scan.warnings.push_back(Tr(language,
            L"Продукты CryptoPro не найдены в native/WOW64 ветках деинсталляции и MSI Installer UserData.",
            L"No CryptoPro products were found in native/WOW64 uninstall branches or MSI Installer UserData."));
    if (offline.scan.profiles.empty())
        offline.scan.warnings.push_back(Tr(language, L"Обычные профили с NTUSER.DAT не найдены.",
                                                     L"No regular profiles with NTUSER.DAT were found."));
    if (offline.scan.certificates.empty())
        offline.scan.warnings.push_back(Tr(language, L"Открытые сертификаты в пользовательских и машинном хранилищах не найдены.",
                                                     L"No public certificates were found in user or local-machine stores."));
    offline.scan.warnings.push_back(Tr(language,
        L"Офлайн-очистка является принудительной: штатный установщик отключённой Windows запустить невозможно. Неизвестные COM- и браузерные остатки удаляться не будут.",
        L"Offline cleanup is forced: the disconnected Windows installer cannot be run. Unknown COM and browser remnants will not be removed."));
    offline.valid = true;
    offline.diagnostics.push_back(L"Final result: valid=true, cleanupCapable=" +
                                  std::wstring(offline.cleanupCapable ? L"true" : L"false") +
                                  L", products=" + std::to_wstring(offline.scan.products.size()) +
                                  L", licenses=" + std::to_wstring(offline.scan.licenses.size()) +
                                  L", profiles=" + std::to_wstring(offline.scan.profiles.size()) +
                                  L", certificates=" + std::to_wstring(offline.scan.certificates.size()) +
                                  L", targets=" + std::to_wstring(offline.targets.size()));
    if (progress) progress(Tr(language, L"Офлайн-сканирование завершено без изменений.", L"Offline scan completed without changes."), 100);
    return offline;
}

bool SaveOfflineBackup(Language language, const OfflineScanResult& offline,
                       const std::wstring& parentFolder, bool includeRecoveryCopies,
                       std::wstring* sessionFolder, std::wstring* error) {
    if (!offline.valid || parentFolder.empty() || !DirectoryExists(parentFolder)) {
        if (error) *error = L"Offline scan or backup folder is invalid.";
        return false;
    }
    if (includeRecoveryCopies && !offline.cleanupCapable) {
        if (error) *error = Tr(language,
            L"Улей SYSTEM не прошёл проверку: доступно только спасение данных без очистки.",
            L"The SYSTEM hive did not pass validation: only data rescue is available.");
        return false;
    }
    const std::wstring backupVolume = VolumeRoot(parentFolder);
    if (includeRecoveryCopies && (backupVolume.empty() || ToLower(backupVolume) == ToLower(offline.volumeRoot))) {
        if (error) *error = Tr(language,
            L"Для офлайн-очистки резервная папка должна находиться на другом томе.",
            L"Offline cleanup backup must be stored on a different volume.");
        return false;
    }
    const std::wstring folder = UniqueFolder(parentFolder, L"CryptoProOfflineRescue-");
    if (!EnsureDirectory(folder, error)) return false;
    std::wostringstream licenses;
    licenses << L"CryptoPro Offline Rescue " << kVersion << L"\r\n"
             << L"CONFIDENTIAL / КОНФИДЕНЦИАЛЬНО: full license identifiers follow.\r\n\r\n";
    if (offline.scan.licenses.empty()) licenses << L"No license values found / Лицензии не найдены.\r\n";
    for (size_t index = 0; index < offline.scan.licenses.size(); ++index) {
        const auto& license = offline.scan.licenses[index];
        licenses << L"[" << index + 1 << L"] " << license.product << L"\r\nSource: "
                 << license.registryPath << L" / " << license.valueName << L"\r\nLicense: " << license.fullValue << L"\r\n\r\n";
    }
    if (!WriteUtf8File(JoinPath(folder, L"licenses.txt"), Utf8(licenses.str()), error)) return false;
    if (!ExportPublicCertificates(language, offline.scan.certificates, folder, nullptr, error)) return false;

    std::wostringstream summary;
    summary << Tr(language, L"Спасение отключённой Windows", L"Disconnected Windows rescue") << L"\r\n"
            << L"CryptoPro Cleanup Utility " << kVersion << L"\r\n\r\n"
            << Tr(language, L"Продуктов CryptoPro: ", L"CryptoPro products: ") << offline.scan.products.size() << L"\r\n"
            << Tr(language, L"Полных значений лицензий: ", L"Full license values: ") << offline.scan.licenses.size() << L"\r\n"
            << Tr(language, L"Выбранных открытых сертификатов: ", L"Selected public certificates: ") << CountSelectedCertificates(offline.scan.certificates) << L"\r\n"
            << Tr(language, L"Подтверждённых целей офлайн-очистки: ", L"Verified offline cleanup targets: ") << offline.targets.size() << L"\r\n\r\n"
            << Tr(language,
                L"Закрытые ключи, токены, NTUSER.DAT и защищённые контейнеры не изменялись.",
                L"Private keys, tokens, NTUSER.DAT files, and protected containers were not modified.") << L"\r\n";
    if (!WriteUtf8File(JoinPath(folder, L"offline-summary.txt"), Utf8(summary.str()), error)) return false;
    std::wostringstream report;
    report << L"{\n  \"schema_version\": 1,\n  \"utility_version\": \"" << kVersion << L"\",\n"
           << L"  \"mode\": \"offline_rescue\",\n  \"products\": " << offline.scan.products.size()
           << L",\n  \"licenses\": " << offline.scan.licenses.size()
           << L",\n  \"public_certificates\": " << CountSelectedCertificates(offline.scan.certificates)
           << L",\n  \"private_keys_exported\": false,\n  \"verified_cleanup_targets\": " << offline.targets.size()
           << L",\n  \"recovery_copies\": " << (includeRecoveryCopies ? L"true" : L"false") << L"\n}\n";
    if (!WriteUtf8File(JoinPath(folder, L"offline-report.json"), Utf8(report.str()), error)) return false;
    std::wostringstream diagnostics;
    diagnostics << L"CryptoPro Cleanup Utility " << kVersion << L"\r\n"
                << L"Offline scan diagnostics / Диагностика офлайн-сканирования\r\n\r\n";
    for (const auto& line : offline.diagnostics) diagnostics << line << L"\r\n";
    if (!offline.scan.warnings.empty()) {
        diagnostics << L"\r\nWarnings / Предупреждения:\r\n";
        for (const auto& warning : offline.scan.warnings) diagnostics << L"- " << warning << L"\r\n";
    }
    if (!WriteUtf8File(JoinPath(folder, L"offline-diagnostics.txt"), Utf8(diagnostics.str()), error)) return false;

    if (includeRecoveryCopies) {
        const std::wstring hives = JoinPath(folder, L"registry-hives");
        if (!EnsureDirectory(hives, error) ||
            !CopyFileSafe(offline.softwareHivePath, JoinPath(hives, L"SOFTWARE"), error) ||
            !CopyFileSafe(offline.systemHivePath, JoinPath(hives, L"SYSTEM"), error)) return false;
        const std::wstring quarantine = JoinPath(folder, L"quarantine");
        if (!EnsureDirectory(quarantine, error)) return false;
        std::wostringstream recoveryMap;
        recoveryMap << L"CryptoPro Offline Rescue recovery map\r\n"
                    << L"SOFTWARE hive backup: registry-hives\\SOFTWARE\r\n"
                    << L"SYSTEM hive backup: registry-hives\\SYSTEM\r\n\r\n";
        for (size_t index = 0; index < offline.targets.size(); ++index) {
            const auto& target = offline.targets[index];
            if (target.path.empty()) continue;
            const std::wstring targetName = L"target-" + std::to_wstring(index + 1);
            const std::wstring destination = JoinPath(quarantine, targetName);
            if (!CopyTreeSafe(target.path, destination, offline.volumeRoot, error)) return false;
            recoveryMap << targetName << L" => " << target.path << L"\r\n";
        }
        if (!WriteUtf8File(JoinPath(folder, L"recovery-map.txt"), Utf8(recoveryMap.str()), error)) return false;
    }
    if (sessionFolder) *sessionFolder = folder;
    return true;
}

ExecutionResult ExecuteOfflineCleanup(const OfflineScanResult& offline,
                                      const std::wstring& backupSession,
                                      const ProgressCallback& progress) {
    ExecutionResult result;
    result.operations.push_back({L"Registered uninstall", L"Disconnected Windows", Outcome::Skipped, ERROR_NOT_SUPPORTED,
                                 L"A registered installer cannot run for a disconnected Windows installation; advanced offline cleanup was explicitly selected."});
    const bool allSelected = !offline.scan.products.empty() && std::all_of(
        offline.scan.products.begin(), offline.scan.products.end(), [](const InstalledProduct& product) { return product.selected; });
    const std::wstring softwareBackup = JoinPath(backupSession, L"registry-hives\\SOFTWARE");
    const std::wstring systemBackup = JoinPath(backupSession, L"registry-hives\\SYSTEM");
    if (!offline.valid || !offline.cleanupCapable || !allSelected ||
        !FileExists(softwareBackup) || !FileExists(systemBackup)) {
        result.anyFailure = true;
        result.operations.push_back({L"Offline cleanup", L"Safety preconditions", Outcome::Failed, ERROR_INVALID_DATA,
                                     L"All detected products must be selected and verified SOFTWARE/SYSTEM recovery copies must exist."});
        return result;
    }
    const auto probeHive = [](const std::wstring& path) {
        OfflineRegistryMount hive;
        const LONG opened = hive.Open(path, KEY_READ | KEY_WRITE);
        if (opened != ERROR_SUCCESS) return opened;
        return hive.Close();
    };
    const LONG softwareStatus = probeHive(offline.softwareHivePath);
    const LONG systemStatus = probeHive(offline.systemHivePath);
    if (softwareStatus != ERROR_SUCCESS || systemStatus != ERROR_SUCCESS) {
        result.anyFailure = true;
        result.operations.push_back({L"Offline cleanup", L"Offline registry", Outcome::Failed,
                                     softwareStatus != ERROR_SUCCESS ? static_cast<DWORD>(softwareStatus) : static_cast<DWORD>(systemStatus),
                                     L"Could not reopen offline registry hives for controlled write access."});
        return result;
    }
    size_t processed = 0;
    const auto processTarget = [&](const OfflineCleanupTarget& target, HKEY registryHive) {
        if (progress) progress(L"Offline cleanup: " + target.displayName,
                               static_cast<int>(((++processed) * 100) / (offline.targets.empty() ? 1 : offline.targets.size())));
        OperationRecord operation;
        operation.action = L"Offline forced cleanup";
        operation.target = target.displayName;
        bool retained = false;
        DWORD error = ERROR_SUCCESS;
        bool success = false;
        if (target.type == TargetType::RegistryTree || target.type == TargetType::Service || target.type == TargetType::DriverService) {
            const std::wstring display = target.hive == OfflineHive::Software ?
                L"HKLM\\SOFTWARE\\" + target.registrySubkey : L"HKLM\\SYSTEM\\" + target.registrySubkey;
            success = registryHive && DeleteRegistryTreeProtected(registryHive, target.registrySubkey, display, &retained, &error);
            if (!registryHive) error = ERROR_INVALID_HANDLE;
        } else {
            success = DeleteTreeSafe(target.path, offline.volumeRoot, &retained, &error);
        }
        operation.code = error;
        if (success) {
            operation.outcome = Outcome::Succeeded;
            operation.message = retained ? L"Verified removable content was cleaned; protected or reparse-point content was retained."
                                         : L"Verified offline target was removed or already absent.";
            result.anyRemoval = true;
        } else {
            operation.outcome = Outcome::Failed;
            operation.message = GetLastErrorMessage(error);
            result.anyFailure = true;
        }
        result.operations.push_back(std::move(operation));
    };

    const auto processRegistryHive = [&](OfflineHive hiveType, const std::wstring& hivePath) {
        OfflineRegistryMount hive;
        const LONG opened = hive.Open(hivePath, KEY_READ | KEY_WRITE);
        if (opened != ERROR_SUCCESS) {
            result.anyFailure = true;
            result.operations.push_back({L"Offline cleanup", hiveType == OfflineHive::Software ? L"SOFTWARE" : L"SYSTEM",
                                         Outcome::Failed, static_cast<DWORD>(opened),
                                         L"The preflight succeeded but the hive could not be reopened for the write pass."});
            return false;
        }
        for (const auto& target : offline.targets) {
            const bool registryTarget = target.type == TargetType::RegistryTree || target.type == TargetType::Service ||
                                        target.type == TargetType::DriverService;
            if (registryTarget && target.hive == hiveType) processTarget(target, hive.get());
        }
        RegFlushKey(hive.get());
        const LONG unloaded = hive.Close();
        if (unloaded == ERROR_SUCCESS) return true;
        result.anyFailure = true;
        result.operations.push_back({L"Offline cleanup", hiveType == OfflineHive::Software ? L"SOFTWARE" : L"SYSTEM",
                                     Outcome::Failed, static_cast<DWORD>(unloaded),
                                     L"Cleanup operations completed, but the temporary registry mount could not be unloaded."});
        return false;
    };
    if (!processRegistryHive(OfflineHive::Software, offline.softwareHivePath)) return result;
    if (!processRegistryHive(OfflineHive::System, offline.systemHivePath)) return result;
    for (const auto& target : offline.targets) {
        const bool registryTarget = target.type == TargetType::RegistryTree || target.type == TargetType::Service ||
                                    target.type == TargetType::DriverService;
        if (!registryTarget) processTarget(target, nullptr);
    }
    return result;
}

}  // namespace cpc
