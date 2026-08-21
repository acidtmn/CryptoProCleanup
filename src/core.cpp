#include "cleanup.hpp"

#include <msi.h>
#include <setupapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sddl.h>
#include <softpub.h>
#include <taskschd.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace cpc {
namespace {

class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY key) : key_(key) {}
    ~RegKey() { reset(); }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    RegKey(RegKey&& other) noexcept : key_(other.release()) {}
    RegKey& operator=(RegKey&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HKEY get() const { return key_; }
    HKEY* put() { reset(); return &key_; }
    explicit operator bool() const { return key_ != nullptr; }
    HKEY release() { HKEY value = key_; key_ = nullptr; return value; }
    void reset(HKEY key = nullptr) { if (key_) RegCloseKey(key_); key_ = key; }
private:
    HKEY key_ = nullptr;
};

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE value) : value_(value) {}
    ~ScopedHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_ = nullptr;
};

class ScopedScHandle {
public:
    explicit ScopedScHandle(SC_HANDLE value = nullptr) : value_(value) {}
    ~ScopedScHandle() { if (value_) CloseServiceHandle(value_); }
    ScopedScHandle(const ScopedScHandle&) = delete;
    ScopedScHandle& operator=(const ScopedScHandle&) = delete;
    SC_HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
private:
    SC_HANDLE value_ = nullptr;
};

HKEY HiveHandle(RegistryHive hive) {
    switch (hive) {
        case RegistryHive::CurrentUser: return HKEY_CURRENT_USER;
        case RegistryHive::Users: return HKEY_USERS;
        default: return HKEY_LOCAL_MACHINE;
    }
}

std::wstring HiveName(RegistryHive hive) {
    switch (hive) {
        case RegistryHive::CurrentUser: return L"HKCU";
        case RegistryHive::Users: return L"HKU";
        default: return L"HKLM";
    }
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring CanonicalPath(const std::wstring& input) {
    std::wstring value = Trim(ExpandEnvironment(input));
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length && length < buffer.size()) value.assign(buffer.data(), length);
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
    return value;
}

bool PathStartsWith(const std::wstring& candidate, const std::wstring& root) {
    const std::wstring left = ToLower(CanonicalPath(candidate));
    const std::wstring right = ToLower(CanonicalPath(root));
    if (right.empty() || left.size() < right.size() || left.compare(0, right.size(), right) != 0) return false;
    return left.size() == right.size() || left[right.size()] == L'\\';
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (left.back() == L'\\' || left.back() == L'/') return left + right;
    return left + L"\\" + right;
}

std::wstring ParentPath(const std::wstring& path) {
    const size_t index = path.find_last_of(L"\\/");
    return index == std::wstring::npos ? std::wstring() : path.substr(0, index);
}

std::wstring FileName(const std::wstring& path) {
    const size_t index = path.find_last_of(L"\\/");
    return index == std::wstring::npos ? path : path.substr(index + 1);
}

std::wstring ReadRegString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0 || size > 1024 * 1024) return {};
    std::vector<wchar_t> value(size / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) != ERROR_SUCCESS) return {};
    std::wstring result(value.data());
    return type == REG_EXPAND_SZ ? ExpandEnvironment(result) : result;
}

DWORD ReadRegDword(HKEY key, const wchar_t* name, DWORD fallback = 0) {
    DWORD type = 0, size = sizeof(DWORD), value = fallback;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS || type != REG_DWORD) return fallback;
    return value;
}

std::vector<std::wstring> EnumSubkeys(HKEY key) {
    std::vector<std::wstring> result;
    DWORD index = 0;
    for (;;) {
        std::array<wchar_t, 512> name{};
        DWORD length = static_cast<DWORD>(name.size());
        const LONG status = RegEnumKeyExW(key, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS) result.emplace_back(name.data(), length);
        ++index;
    }
    return result;
}

std::wstring NormalizeVendor(std::wstring value) {
    value = ToLower(value);
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return iswspace(ch) || ch == L'-' || ch == L'_' || ch == L'.' || ch == L'"' || ch == L'\'' || ch == 0x00AB || ch == 0x00BB;
    }), value.end());
    return value;
}

std::wstring GetKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) result = raw;
    CoTaskMemFree(raw);
    return result;
}

std::wstring GetProgramData() {
    std::wstring value = GetKnownFolder(FOLDERID_ProgramData);
    if (value.empty()) value = ExpandEnvironment(L"%ProgramData%");
    return value;
}

std::vector<std::wstring> DefaultVendorRoots() {
    std::vector<std::wstring> roots;
    for (const auto& candidate : {
        JoinPath(ExpandEnvironment(L"%ProgramFiles%"), L"Crypto Pro"),
        JoinPath(ExpandEnvironment(L"%ProgramFiles(x86)%"), L"Crypto Pro"),
        JoinPath(GetProgramData(), L"Crypto Pro")
    }) {
        if (!candidate.empty() && std::find_if(roots.begin(), roots.end(), [&](const std::wstring& item) {
            return ToLower(CanonicalPath(item)) == ToLower(CanonicalPath(candidate));
        }) == roots.end()) roots.push_back(candidate);
    }
    return roots;
}

std::wstring FileCompanyName(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Translation { WORD language; WORD codePage; };
    Translation* translations = nullptr;
    UINT translationSize = 0;
    std::vector<Translation> fallback{{0x0419, 0x04b0}, {0x0409, 0x04b0}, {0x0409, 0x04e4}};
    std::vector<Translation> candidates;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationSize) && translationSize >= sizeof(Translation)) {
        candidates.assign(translations, translations + translationSize / sizeof(Translation));
    }
    candidates.insert(candidates.end(), fallback.begin(), fallback.end());
    for (const auto& item : candidates) {
        wchar_t query[80]{};
        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\CompanyName", item.language, item.codePage);
        wchar_t* company = nullptr;
        UINT companySize = 0;
        if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&company), &companySize) && company && companySize) return company;
    }
    return {};
}

std::wstring ExtractExecutable(const std::wstring& command) {
    std::wstring expanded = Trim(ExpandEnvironment(command));
    if (expanded.empty()) return {};
    if (expanded.front() == L'"') {
        const size_t end = expanded.find(L'"', 1);
        if (end != std::wstring::npos) return CanonicalPath(expanded.substr(1, end - 1));
    }
    const std::wstring lower = ToLower(expanded);
    const size_t exe = lower.find(L".exe");
    if (exe != std::wstring::npos) return CanonicalPath(expanded.substr(0, exe + 4));
    const size_t space = expanded.find(L' ');
    return CanonicalPath(space == std::wstring::npos ? expanded : expanded.substr(0, space));
}

bool DirectoryContainsVerifiedBinary(const std::wstring& directory) {
    if (!DirectoryExists(directory)) return false;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring lower = ToLower(data.cFileName);
        if (lower.size() >= 4 && (lower.substr(lower.size() - 4) == L".exe" || lower.substr(lower.size() - 4) == L".dll")) {
            const std::wstring path = JoinPath(directory, data.cFileName);
            if (IsCryptoProPublisher(FileCompanyName(path)) && VerifyCryptoProSignature(path)) { found = true; break; }
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return found;
}

std::wstring Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::wstring Redact(const std::wstring& value, const ScanResult& scan) {
    std::wstring result = value;
    auto replaceInsensitive = [&result](const std::wstring& sensitive, const std::wstring& replacement,
                                        bool tokenBoundaries) {
        if (sensitive.empty()) return;
        const std::wstring needle = ToLower(sensitive);
        size_t at = 0;
        for (;;) {
            const std::wstring lower = ToLower(result);
            at = lower.find(needle, at);
            if (at == std::wstring::npos) break;
            const auto tokenCharacter = [](wchar_t ch) { return iswalnum(ch) || ch == L'_'; };
            const bool leftBoundary = at == 0 || !tokenCharacter(result[at - 1]);
            const size_t after = at + sensitive.size();
            const bool rightBoundary = after >= result.size() || !tokenCharacter(result[after]);
            if (tokenBoundaries && (!leftBoundary || !rightBoundary)) { ++at; continue; }
            result.replace(at, sensitive.size(), replacement);
            at += replacement.size();
        }
    };
    std::vector<std::wstring> exactProfileValues;
    std::vector<std::wstring> profileNames;
    for (const auto& profile : scan.profiles) {
        if (!profile.profilePath.empty()) exactProfileValues.push_back(profile.profilePath);
        if (!profile.sid.empty()) exactProfileValues.push_back(profile.sid);
        if (profile.displayName.size() >= 3) profileNames.push_back(profile.displayName);
    }
    auto longestFirst = [](const std::wstring& left, const std::wstring& right) { return left.size() > right.size(); };
    std::sort(exactProfileValues.begin(), exactProfileValues.end(), longestFirst);
    std::sort(profileNames.begin(), profileNames.end(), longestFirst);
    for (const auto& sensitive : exactProfileValues) replaceInsensitive(sensitive, L"<PROFILE>", false);
    for (const auto& sensitive : profileNames) replaceInsensitive(sensitive, L"<PROFILE>", true);
    for (const auto& license : scan.licenses) {
        if (license.fullValue.empty()) continue;
        size_t at = 0;
        while ((at = result.find(license.fullValue, at)) != std::wstring::npos) {
            result.replace(at, license.fullValue.size(), license.maskedValue);
            at += license.maskedValue.size();
        }
    }
    for (const auto& certificate : scan.certificates) {
        for (const auto& sensitive : {certificate.subject, certificate.issuer}) {
            if (sensitive.empty()) continue;
            replaceInsensitive(sensitive, L"<CERTIFICATE>", false);
        }
    }
    return result;
}

void ReportProgress(const ProgressCallback& callback, const std::wstring& message, int percent) {
    if (callback) callback(message, percent);
}

}  // namespace

Language DetectLanguage() {
    const LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_RUSSIAN ? Language::Russian : Language::English;
}

ThemeMode NormalizeThemeMode(DWORD value) {
    switch (value) {
    case static_cast<DWORD>(ThemeMode::System): return ThemeMode::System;
    case static_cast<DWORD>(ThemeMode::Light): return ThemeMode::Light;
    default: return ThemeMode::Dark;
    }
}

std::wstring Tr(Language language, const wchar_t* russian, const wchar_t* english) {
    return language == Language::Russian ? russian : english;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        if (ch >= L'А' && ch <= L'Я') return static_cast<wchar_t>(ch + (L'а' - L'А'));
        if (ch == L'Ё') return L'ё';
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring Trim(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring ExpandEnvironment(const std::wstring& value) {
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!required) return value;
    std::vector<wchar_t> buffer(required + 1);
    if (!ExpandEnvironmentStringsW(value.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()))) return value;
    return buffer.data();
}

std::wstring MaskLicense(const std::wstring& value) {
    const std::wstring trimmed = Trim(value);
    if (trimmed.empty()) return {};
    if (trimmed.size() <= 4) return std::wstring(trimmed.size(), L'*');
    if (trimmed.size() <= 8) return std::wstring(trimmed.size() - 2, L'*') + trimmed.substr(trimmed.size() - 2);
    return trimmed.substr(0, 2) + std::wstring(trimmed.size() - 6, L'*') + trimmed.substr(trimmed.size() - 4);
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wostringstream out;
    for (const wchar_t ch : value) {
        switch (ch) {
            case L'"': out << L"\\\""; break;
            case L'\\': out << L"\\\\"; break;
            case L'\b': out << L"\\b"; break;
            case L'\f': out << L"\\f"; break;
            case L'\n': out << L"\\n"; break;
            case L'\r': out << L"\\r"; break;
            case L'\t': out << L"\\t"; break;
            default:
                if (ch < 0x20) out << L"\\u" << std::hex << std::setw(4) << std::setfill(L'0') << static_cast<unsigned>(ch) << std::dec;
                else out << ch;
        }
    }
    return out.str();
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

bool IsCryptoProPublisher(const std::wstring& value) {
    std::wstring normalized = NormalizeVendor(value);
    for (const auto* legal : {L"company", L"llc", L"ltd", L"inc", L"corp", L"corporation",
                              L"компания", L"общество", L"ооо", L"зао", L"оао", L"пао"}) {
        size_t at = 0;
        const std::wstring token = legal;
        while ((at = normalized.find(token)) != std::wstring::npos) normalized.erase(at, token.size());
    }
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](wchar_t ch) {
        return ch == L',' || ch == L'(' || ch == L')' || ch == L'«' || ch == L'»';
    }), normalized.end());
    return normalized == L"cryptopro" || normalized == L"криптопро";
}

bool IsCryptoProName(const std::wstring& value) {
    const std::wstring lower = ToLower(value);
    if (lower.find(L"cryptopro") != std::wstring::npos || lower.find(L"crypto-pro") != std::wstring::npos ||
        lower.find(L"криптопро") != std::wstring::npos || lower.find(L"крипто-про") != std::wstring::npos) return true;
    const size_t spaced = lower.find(L"crypto pro");
    return spaced != std::wstring::npos && lower.compare(spaced + 10, 5, L"vider") != 0;
}

bool IsHighRiskProduct(const std::wstring& value) {
    const std::wstring lower = ToLower(value);
    static const std::array<const wchar_t*, 17> terms{
        L"ngate", L"hsm", L"ipsec", L"winlogon", L"eap", L"efs", L"криптоарм", L"cryptoarm",
        L"удостоверяющ", L"certificate authority", L"центр сертификац", L"службы уц", L"dss",
        L"cloud csp", L"tls server", L"шлюз", L"server"
    };
    return std::any_of(terms.begin(), terms.end(), [&](const wchar_t* term) { return lower.find(term) != std::wstring::npos; });
}

bool IsGuid(const std::wstring& value) {
    GUID guid{};
    return SUCCEEDED(CLSIDFromString(value.c_str(), &guid));
}

std::wstring PackMsiProductCode(const std::wstring& productCode) {
    if (!IsGuid(productCode)) return {};
    std::wstring value = productCode;
    value.erase(std::remove(value.begin(), value.end(), L'{'), value.end());
    value.erase(std::remove(value.begin(), value.end(), L'}'), value.end());
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        const size_t separator = value.find(L'-', start);
        parts.push_back(value.substr(start, separator == std::wstring::npos ? separator : separator - start));
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    if (parts.size() != 5 || parts[0].size() != 8 || parts[1].size() != 4 || parts[2].size() != 4 ||
        parts[3].size() != 4 || parts[4].size() != 12) return {};
    std::wstring packed;
    for (size_t index = 0; index < 3; ++index) {
        std::reverse(parts[index].begin(), parts[index].end());
        packed += parts[index];
    }
    for (size_t index = 3; index < parts.size(); ++index) {
        for (size_t character = 0; character < parts[index].size(); character += 2) {
            packed.push_back(parts[index][character + 1]);
            packed.push_back(parts[index][character]);
        }
    }
    std::transform(packed.begin(), packed.end(), packed.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towupper(ch)); });
    return packed;
}

std::wstring UnpackMsiProductCode(const std::wstring& packedProductCode) {
    if (packedProductCode.size() != 32 ||
        !std::all_of(packedProductCode.begin(), packedProductCode.end(),
                     [](wchar_t ch) { return iswxdigit(ch) != 0; })) return {};
    auto reversePart = [&](size_t offset, size_t length) {
        std::wstring part = packedProductCode.substr(offset, length);
        std::reverse(part.begin(), part.end());
        return part;
    };
    auto swapPairs = [&](size_t offset, size_t length) {
        std::wstring part;
        part.reserve(length);
        for (size_t index = 0; index < length; index += 2) {
            part.push_back(packedProductCode[offset + index + 1]);
            part.push_back(packedProductCode[offset + index]);
        }
        return part;
    };
    std::wstring productCode = L"{" + reversePart(0, 8) + L"-" + reversePart(8, 4) + L"-" +
                               reversePart(12, 4) + L"-" + swapPairs(16, 4) + L"-" +
                               swapPairs(20, 12) + L"}";
    std::transform(productCode.begin(), productCode.end(), productCode.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towupper(ch)); });
    return IsGuid(productCode) ? productCode : std::wstring();
}

bool IsProtectedPath(const std::wstring& path) {
    const std::wstring lower = ToLower(CanonicalPath(path));
    const auto containsBoundary = [&](const std::wstring& token) {
        const size_t at = lower.find(token);
        return at != std::wstring::npos && (at + token.size() == lower.size() || lower[at + token.size()] == L'\\');
    };
    return containsBoundary(L"\\programdata\\crypto pro\\crypto") ||
           containsBoundary(L"\\appdata\\local\\crypto pro") ||
           containsBoundary(L"\\appdata\\roaming\\crypto pro") ||
           containsBoundary(L"\\microsoft\\crypto") ||
           containsBoundary(L"\\microsoft\\systemcertificates");
}

bool IsProtectedRegistryPath(const std::wstring& path) {
    const std::wstring lower = ToLower(path);
    if (lower.find(L"\\microsoft\\systemcertificates") != std::wstring::npos) return true;
    if (lower.find(L"\\crypto pro\\settings\\keys") != std::wstring::npos) return true;
    if (lower.find(L"\\crypto pro\\settings\\keydevices") != std::wstring::npos) return true;
    const size_t users = lower.find(L"\\crypto pro\\settings\\users\\");
    if (users != std::wstring::npos) {
        return lower.find(L"\\keys", users) != std::wstring::npos ||
               lower.find(L"\\keydevices", users) != std::wstring::npos ||
               lower.find(L"\\cptools\\containers", users) != std::wstring::npos ||
               lower.find(L"\\random", users) != std::wstring::npos;
    }
    return false;
}

bool IsSafeVendorPath(const std::wstring& path, const std::vector<std::wstring>& approvedRoots) {
    if (path.empty() || IsProtectedPath(path)) return false;
    return std::any_of(approvedRoots.begin(), approvedRoots.end(), [&](const std::wstring& root) { return PathStartsWith(path, root); });
}

std::wstring GetLastErrorMessage(DWORD code) {
    wchar_t* raw = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring result = size && raw ? Trim(raw) : L"Error " + std::to_wstring(code);
    LocalFree(raw);
    return result;
}

FileSignatureState InspectFileSignature(const std::wstring& path, std::wstring* signer, LONG* trustStatus) {
    if (signer) signer->clear();
    if (!FileExists(path)) {
        if (trustStatus) *trustStatus = TRUST_E_NOSIGNATURE;
        return FileSignatureState::Error;
    }
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &fileInfo;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_SAFER_FLAG;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &trust);
    if (trustStatus) *trustStatus = status;

    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0, content = 0, format = 0;
    std::wstring signerName;
    if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                         CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format, &store, &message, nullptr)) {
        DWORD size = 0;
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) && size) {
            std::vector<BYTE> buffer(size);
            if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &size)) {
                const auto* info = reinterpret_cast<CMSG_SIGNER_INFO*>(buffer.data());
                CERT_INFO certificate{};
                certificate.Issuer = info->Issuer;
                certificate.SerialNumber = info->SerialNumber;
                PCCERT_CONTEXT context = CertFindCertificateInStore(store, encoding, 0, CERT_FIND_SUBJECT_CERT, &certificate, nullptr);
                if (context) {
                    std::array<wchar_t, 512> name{};
                    if (CertGetNameStringW(context, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), static_cast<DWORD>(name.size()))) signerName = name.data();
                    CertFreeCertificateContext(context);
                }
            }
        }
    }
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust);
    if (signer) *signer = signerName;
    if (status == ERROR_SUCCESS) return FileSignatureState::Valid;
    if (status == TRUST_E_NOSIGNATURE || status == TRUST_E_SUBJECT_FORM_UNKNOWN ||
        status == TRUST_E_PROVIDER_UNKNOWN) return FileSignatureState::Absent;
    if (!signerName.empty()) return FileSignatureState::Invalid;
    return FileSignatureState::Error;
}

bool VerifyCryptoProSignature(const std::wstring& path, std::wstring* signer) {
    std::wstring signerName;
    const auto state = InspectFileSignature(path, &signerName, nullptr);
    if (signer) *signer = signerName;
    return state == FileSignatureState::Valid &&
           (IsCryptoProPublisher(signerName) || IsCryptoProPublisher(FileCompanyName(path)));
}

namespace {

void ScanUninstallView(ScanResult& scan, HKEY hive, RegistryHive hiveId, REGSAM view, const std::wstring& architecture) {
    constexpr wchar_t uninstallRoot[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    RegKey root;
    if (RegOpenKeyExW(hive, uninstallRoot, 0, KEY_READ | view, root.put()) != ERROR_SUCCESS) return;
    for (const auto& subkeyName : EnumSubkeys(root.get())) {
        RegKey key;
        if (RegOpenKeyExW(root.get(), subkeyName.c_str(), 0, KEY_READ | view, key.put()) != ERROR_SUCCESS) continue;
        const std::wstring displayName = ReadRegString(key.get(), L"DisplayName");
        const std::wstring publisher = ReadRegString(key.get(), L"Publisher");
        if (displayName.empty()) continue;
        if (!IsCryptoProPublisher(publisher)) {
            if (IsCryptoProName(displayName) && publisher.empty()) {
                scan.warnings.push_back(L"Unconfirmed product entry: " + displayName);
            }
            continue;
        }
        InstalledProduct product;
        product.displayName = displayName;
        product.version = ReadRegString(key.get(), L"DisplayVersion");
        product.publisher = publisher;
        product.architecture = architecture;
        product.uninstallString = ReadRegString(key.get(), L"UninstallString");
        product.quietUninstallString = ReadRegString(key.get(), L"QuietUninstallString");
        product.installLocation = CanonicalPath(ReadRegString(key.get(), L"InstallLocation"));
        product.registryKey = uninstallRoot + std::wstring(L"\\") + subkeyName;
        product.productCode = ReadRegString(key.get(), L"ProductCode");
        if (product.productCode.empty() && IsGuid(subkeyName)) product.productCode = subkeyName;
        product.msi = ReadRegDword(key.get(), L"WindowsInstaller") == 1 || IsGuid(product.productCode);
        product.registryView = view;
        product.hive = hiveId;
        product.risk = IsHighRiskProduct(product.displayName) ? RiskLevel::High : RiskLevel::Normal;
        const auto duplicate = std::find_if(scan.products.begin(), scan.products.end(), [&](const InstalledProduct& item) {
            return ToLower(item.displayName) == ToLower(product.displayName) &&
                   ToLower(item.version) == ToLower(product.version) &&
                   ToLower(item.registryKey) == ToLower(product.registryKey) && item.hive == product.hive && item.registryView == product.registryView;
        });
        if (duplicate == scan.products.end()) scan.products.push_back(std::move(product));
    }
}

void ScanProfiles(ScanResult& scan) {
    constexpr wchar_t profilesRoot[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
    RegKey root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, profilesRoot, 0, KEY_READ | KEY_WOW64_64KEY, root.put()) != ERROR_SUCCESS) return;
    for (const auto& sid : EnumSubkeys(root.get())) {
        if (sid.rfind(L"S-1-5-21-", 0) != 0) continue;
        RegKey key;
        if (RegOpenKeyExW(root.get(), sid.c_str(), 0, KEY_READ, key.put()) != ERROR_SUCCESS) continue;
        std::wstring path = CanonicalPath(ReadRegString(key.get(), L"ProfileImagePath"));
        if (path.empty() || !DirectoryExists(path)) continue;
        const std::wstring name = FileName(path);
        const std::wstring lowerName = ToLower(name);
        if (lowerName == L"default" || lowerName == L"default user" || lowerName == L"public" || lowerName == L"all users") continue;
        RegKey loaded;
        const bool isLoaded = RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, loaded.put()) == ERROR_SUCCESS;
        scan.profiles.push_back(UserProfile{sid, name, path, isLoaded, true});
    }
    std::sort(scan.profiles.begin(), scan.profiles.end(), [](const UserProfile& a, const UserProfile& b) {
        return ToLower(a.displayName) < ToLower(b.displayName);
    });
}

bool IsLicenseValueName(const std::wstring& name) {
    const std::wstring lower = ToLower(name);
    return lower == L"wlproductid" || lower == L"productid" || lower == L"serialnumber" ||
           lower == L"license" || lower == L"licensekey" || lower == L"licence" || lower == L"licencekey";
}

void ScanLicensesRecursive(ScanResult& scan, HKEY key, const std::wstring& displayPath, int depth,
                           std::unordered_set<std::wstring>& seen) {
    if (depth > 16) return;
    DWORD valueCount = 0, maxName = 0, maxData = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &valueCount, &maxName, &maxData,
                         nullptr, nullptr) == ERROR_SUCCESS) {
        std::vector<wchar_t> name(maxName + 2);
        std::vector<BYTE> data(maxData + sizeof(wchar_t) * 2);
        for (DWORD index = 0; index < valueCount; ++index) {
            DWORD nameSize = static_cast<DWORD>(name.size());
            DWORD dataSize = static_cast<DWORD>(data.size());
            DWORD type = 0;
            if (RegEnumValueW(key, index, name.data(), &nameSize, nullptr, &type, data.data(), &dataSize) != ERROR_SUCCESS) continue;
            const std::wstring valueName(name.data(), nameSize);
            if (!IsLicenseValueName(valueName) || (type != REG_SZ && type != REG_EXPAND_SZ) || dataSize < sizeof(wchar_t)) continue;
            data.resize(dataSize + sizeof(wchar_t) * 2, 0);
            std::wstring value(reinterpret_cast<wchar_t*>(data.data()));
            value = Trim(type == REG_EXPAND_SZ ? ExpandEnvironment(value) : value);
            if (value.size() < 5 || value.size() > 512) continue;
            const std::wstring fingerprint = ToLower(value);
            if (!seen.insert(fingerprint).second) continue;
            const std::wstring product = FileName(displayPath);
            const int priority = ToLower(valueName) == L"wlproductid" ? 30 : 50;
            scan.licenses.push_back(LicenseEntry{product, displayPath, valueName, value, MaskLicense(value), priority});
        }
    }
    for (const auto& child : EnumSubkeys(key)) {
        RegKey subkey;
        if (RegOpenKeyExW(key, child.c_str(), 0, KEY_READ, subkey.put()) == ERROR_SUCCESS) {
            ScanLicensesRecursive(scan, subkey.get(), displayPath + L"\\" + child, depth + 1, seen);
        }
    }
}

void ScanLicenseView(ScanResult& scan, HKEY hive, const std::wstring& hiveName, REGSAM view,
                     std::unordered_set<std::wstring>& seen) {
    RegKey root;
    constexpr wchar_t branch[] = L"SOFTWARE\\Crypto Pro";
    if (RegOpenKeyExW(hive, branch, 0, KEY_READ | view, root.put()) == ERROR_SUCCESS) {
        const std::wstring viewName = view == KEY_WOW64_64KEY ? L"64" : L"32";
        ScanLicensesRecursive(scan, root.get(), hiveName + L"[" + viewName + L"]\\" + branch, 0, seen);
    }
}

void ScanMsiLicenseView(ScanResult& scan, REGSAM view, std::unordered_set<std::wstring>& seen) {
    constexpr wchar_t userDataPath[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData";
    RegKey userData;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, userDataPath, 0, KEY_READ | view, userData.put()) != ERROR_SUCCESS) return;
    const std::wstring viewName = view == KEY_WOW64_64KEY ? L"64" : L"32";
    const auto userSids = EnumSubkeys(userData.get());
    for (const auto& product : scan.products) {
        if (!product.msi || !IsGuid(product.productCode)) continue;
        const std::wstring packed = PackMsiProductCode(product.productCode);
        if (packed.empty()) continue;
        for (const auto& sid : userSids) {
            const std::wstring relative = sid + L"\\Products\\" + packed + L"\\InstallProperties";
            RegKey properties;
            if (RegOpenKeyExW(userData.get(), relative.c_str(), 0, KEY_READ | view, properties.put()) != ERROR_SUCCESS) continue;
            const std::wstring publisher = ReadRegString(properties.get(), L"Publisher");
            if (!IsCryptoProPublisher(publisher)) continue;
            const std::wstring value = Trim(ReadRegString(properties.get(), L"ProductID"));
            if (value.size() < 5 || value.size() > 512) continue;
            if (!seen.insert(ToLower(value)).second) continue;
            std::wstring displayName = ReadRegString(properties.get(), L"DisplayName");
            if (displayName.empty()) displayName = product.displayName;
            const std::wstring registryPath = L"HKLM[" + viewName + L"]\\" + userDataPath + L"\\" + relative;
            scan.licenses.push_back(LicenseEntry{displayName, registryPath, L"ProductID", value, MaskLicense(value), 100});
        }
    }
}

std::wstring NormalizeLicenseForComparison(std::wstring value) {
    value = ToLower(Trim(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return iswspace(ch) || ch == L'-';
    }), value.end());
    return value;
}

void PreferCompleteLicenseValues(ScanResult& scan) {
    std::stable_sort(scan.licenses.begin(), scan.licenses.end(), [](const LicenseEntry& left, const LicenseEntry& right) {
        if (left.sourcePriority != right.sourcePriority) return left.sourcePriority > right.sourcePriority;
        return NormalizeLicenseForComparison(left.fullValue).size() > NormalizeLicenseForComparison(right.fullValue).size();
    });
    std::vector<LicenseEntry> preferred;
    for (const auto& candidate : scan.licenses) {
        const std::wstring candidateValue = NormalizeLicenseForComparison(candidate.fullValue);
        bool redundant = false;
        for (const auto& existing : preferred) {
            const std::wstring existingValue = NormalizeLicenseForComparison(existing.fullValue);
            if (candidateValue == existingValue ||
                (candidateValue.size() < existingValue.size() && existingValue.rfind(candidateValue, 0) == 0 &&
                 candidate.sourcePriority <= existing.sourcePriority)) {
                redundant = true;
                break;
            }
        }
        if (!redundant) preferred.push_back(candidate);
    }
    scan.licenses = std::move(preferred);
}

std::wstring DetectOsName() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto function = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (function) function(&info);
    std::wstring name = L"Windows ";
    if (info.dwMajorVersion == 6 && info.dwMinorVersion == 1) name += L"7 SP1";
    else if (info.dwMajorVersion == 6 && info.dwMinorVersion == 2) name += L"8";
    else if (info.dwMajorVersion == 6 && info.dwMinorVersion == 3) name += L"8.1";
    else if (info.dwMajorVersion >= 10 && info.dwBuildNumber >= 22000) name += L"11";
    else if (info.dwMajorVersion >= 10) name += L"10";
    else name += std::to_wstring(info.dwMajorVersion) + L"." + std::to_wstring(info.dwMinorVersion);
    name += L" (build " + std::to_wstring(info.dwBuildNumber) + L")";
    return name;
}

std::wstring DetectArchitecture() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return L"ARM64";
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
        default: return L"unknown";
    }
}

}  // namespace

ScanResult ScanSystem(Language language, const ProgressCallback& progress) {
    ScanResult scan;
    ReportProgress(progress, Tr(language, L"Определение версии Windows...", L"Detecting Windows version..."), 5);
    scan.osName = DetectOsName();
    scan.osArchitecture = DetectArchitecture();

    ReportProgress(progress, Tr(language, L"Поиск установленных продуктов...", L"Scanning installed products..."), 20);
    const bool native64 = scan.osArchitecture != L"x86";
    ScanUninstallView(scan, HKEY_LOCAL_MACHINE, RegistryHive::LocalMachine, KEY_WOW64_32KEY, L"x86");
    ScanUninstallView(scan, HKEY_CURRENT_USER, RegistryHive::CurrentUser, KEY_WOW64_32KEY, L"x86/user");
    if (native64) {
        ScanUninstallView(scan, HKEY_LOCAL_MACHINE, RegistryHive::LocalMachine, KEY_WOW64_64KEY,
                          scan.osArchitecture == L"ARM64" ? L"x64/ARM64" : L"x64");
        ScanUninstallView(scan, HKEY_CURRENT_USER, RegistryHive::CurrentUser, KEY_WOW64_64KEY, L"x64/user");
    }
    std::sort(scan.products.begin(), scan.products.end(), [](const InstalledProduct& a, const InstalledProduct& b) {
        return ToLower(a.displayName) < ToLower(b.displayName);
    });

    ReportProgress(progress, Tr(language, L"Поиск пользовательских профилей...", L"Scanning user profiles..."), 45);
    ScanProfiles(scan);

    ReportProgress(progress, Tr(language, L"Поиск открытых пользовательских сертификатов...", L"Scanning public user certificates..."), 55);
    ScanUserCertificates(scan.profiles, &scan.certificates, &scan.warnings, [&](const std::wstring& message, int percent) {
        ReportProgress(progress, message, 55 + percent / 10);
    });

    ReportProgress(progress, Tr(language, L"Поиск лицензий (значения не журналируются)...", L"Scanning licenses (values are not logged)..."), 68);
    std::unordered_set<std::wstring> seenLicenses;
    ScanMsiLicenseView(scan, KEY_WOW64_32KEY, seenLicenses);
    if (native64) ScanMsiLicenseView(scan, KEY_WOW64_64KEY, seenLicenses);
    ScanLicenseView(scan, HKEY_LOCAL_MACHINE, L"HKLM", KEY_WOW64_32KEY, seenLicenses);
    ScanLicenseView(scan, HKEY_CURRENT_USER, L"HKCU", KEY_WOW64_32KEY, seenLicenses);
    if (native64) {
        ScanLicenseView(scan, HKEY_LOCAL_MACHINE, L"HKLM", KEY_WOW64_64KEY, seenLicenses);
        ScanLicenseView(scan, HKEY_CURRENT_USER, L"HKCU", KEY_WOW64_64KEY, seenLicenses);
    }
    PreferCompleteLicenseValues(scan);

    scan.protectedItems = {
        L"Windows certificate stores (CurrentUser/LocalMachine)",
        L"Hardware tokens and smart cards",
        L"Crypto Pro\\Settings\\...\\Keys and container metadata",
        L"%ProgramData%\\Crypto Pro\\Crypto",
        L"%LOCALAPPDATA%\\Crypto Pro and %APPDATA%\\Crypto Pro",
        L"Microsoft cryptographic key stores"
    };
    ReportProgress(progress, Tr(language, L"Сканирование завершено.", L"Scan completed."), 100);
    return scan;
}

namespace {

std::wstring TargetIdentity(const CleanupTarget& target) {
    return std::to_wstring(static_cast<int>(target.type)) + L"|" + std::to_wstring(static_cast<int>(target.registry.hive)) +
           L"|" + std::to_wstring(target.registry.view) + L"|" + ToLower(target.path.empty() ? target.registry.subkey : CanonicalPath(target.path));
}

void AddTarget(CleanupPlan& plan, CleanupTarget target, std::unordered_set<std::wstring>& identities) {
    if (target.protectedItem || IsProtectedPath(target.path) || IsProtectedRegistryPath(target.registry.subkey)) {
        target.protectedItem = true;
        const std::wstring item = !target.path.empty() ? target.path : HiveName(target.registry.hive) + L"\\" + target.registry.subkey;
        if (std::find(plan.protectedItems.begin(), plan.protectedItems.end(), item) == plan.protectedItems.end()) plan.protectedItems.push_back(item);
        return;
    }
    const std::wstring identity = TargetIdentity(target);
    if (identities.insert(identity).second) plan.targets.push_back(std::move(target));
}

bool BelongsToApprovedProduct(const std::wstring& path, const std::vector<std::wstring>& approvedRoots) {
    if (path.empty() || IsProtectedPath(path)) return false;
    if (IsSafeVendorPath(path, approvedRoots)) return true;
    if (!FileExists(path) || !IsCryptoProPublisher(FileCompanyName(path))) return false;
    static std::map<std::wstring, bool> signatureCache;
    const std::wstring identity = ToLower(CanonicalPath(path));
    const auto cached = signatureCache.find(identity);
    if (cached != signatureCache.end()) return cached->second;
    const bool verified = VerifyCryptoProSignature(path);
    signatureCache.emplace(identity, verified);
    return verified;
}

void ScanServices(CleanupPlan& plan, const std::vector<std::wstring>& approvedRoots,
                  std::unordered_set<std::wstring>& identities) {
    ScopedScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT));
    if (!manager) return;
    DWORD needed = 0, count = 0, resume = 0;
    const BOOL initialEnumeration = EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    if ((!initialEnumeration && GetLastError() != ERROR_MORE_DATA) || !needed) return;
    std::vector<BYTE> buffer(needed + 4096);
    resume = 0;
    if (!EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL,
                               buffer.data(), static_cast<DWORD>(buffer.size()), &needed, &count, &resume, nullptr)) return;
    const auto* services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD index = 0; index < count; ++index) {
        ScopedScHandle service(OpenServiceW(manager.get(), services[index].lpServiceName, SERVICE_QUERY_CONFIG));
        if (!service) continue;
        DWORD configSize = 0;
        const BOOL initialQuery = QueryServiceConfigW(service.get(), nullptr, 0, &configSize);
        if ((!initialQuery && GetLastError() != ERROR_INSUFFICIENT_BUFFER) || !configSize) continue;
        std::vector<BYTE> configBuffer(configSize);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
        if (!QueryServiceConfigW(service.get(), config, configSize, &configSize)) continue;
        const std::wstring executable = ExtractExecutable(config->lpBinaryPathName ? config->lpBinaryPathName : L"");
        if (!BelongsToApprovedProduct(executable, approvedRoots)) continue;
        CleanupTarget target;
        target.type = (config->dwServiceType & SERVICE_DRIVER) ? TargetType::DriverService : TargetType::Service;
        target.category = (config->dwServiceType & SERVICE_DRIVER) ? CleanupTargetCategory::Driver : CleanupTargetCategory::Service;
        target.displayName = services[index].lpDisplayName ? services[index].lpDisplayName : services[index].lpServiceName;
        target.path = services[index].lpServiceName;
        target.reason = L"Service image: " + executable;
        target.verified = true;
        AddTarget(plan, std::move(target), identities);
    }
}

std::wstring ReadDefaultSubkey(HKEY parent, const std::wstring& child, REGSAM view) {
    RegKey key;
    if (RegOpenKeyExW(parent, child.c_str(), 0, KEY_READ | view, key.put()) != ERROR_SUCCESS) return {};
    return ReadRegString(key.get(), nullptr);
}

void ScanComRegistrations(CleanupPlan& plan, HKEY hive, RegistryHive hiveId, REGSAM view,
                          const std::wstring& classesRoot, const std::vector<std::wstring>& approvedRoots,
                          std::unordered_set<std::wstring>& identities) {
    const std::wstring clsidRoot = classesRoot + L"\\CLSID";
    RegKey root;
    if (RegOpenKeyExW(hive, clsidRoot.c_str(), 0, KEY_READ | view, root.put()) != ERROR_SUCCESS) return;
    for (const auto& clsid : EnumSubkeys(root.get())) {
        std::wstring server = ReadDefaultSubkey(root.get(), clsid + L"\\InprocServer32", view);
        if (server.empty()) server = ReadDefaultSubkey(root.get(), clsid + L"\\LocalServer32", view);
        const std::wstring executable = ExtractExecutable(server);
        if (!BelongsToApprovedProduct(executable, approvedRoots)) continue;
        CleanupTarget target;
        target.type = TargetType::RegistryTree;
        target.category = CleanupTargetCategory::Com;
        target.displayName = L"COM " + clsid;
        target.path = HiveName(hiveId) + L"\\" + clsidRoot + L"\\" + clsid;
        target.registry = {hiveId, clsidRoot + L"\\" + clsid, view};
        target.reason = L"COM server: " + executable;
        target.verified = true;
        AddTarget(plan, target, identities);

        for (const auto& relation : {L"ProgID", L"VersionIndependentProgID"}) {
            const std::wstring progId = ReadDefaultSubkey(root.get(), clsid + L"\\" + relation, view);
            if (progId.empty() || progId.find(L'\\') != std::wstring::npos) continue;
            CleanupTarget linked = target;
            linked.displayName = L"COM ProgID " + progId;
            linked.path = HiveName(hiveId) + L"\\" + classesRoot + L"\\" + progId;
            linked.registry.subkey = classesRoot + L"\\" + progId;
            linked.reason = L"Linked to verified CryptoPro CLSID " + clsid;
            AddTarget(plan, std::move(linked), identities);
        }
        const std::wstring typeLib = ReadDefaultSubkey(root.get(), clsid + L"\\TypeLib", view);
        if (IsGuid(typeLib)) {
            CleanupTarget linked = target;
            linked.displayName = L"COM TypeLib " + typeLib;
            linked.path = HiveName(hiveId) + L"\\" + classesRoot + L"\\TypeLib\\" + typeLib;
            linked.registry.subkey = classesRoot + L"\\TypeLib\\" + typeLib;
            linked.reason = L"Linked to verified CryptoPro CLSID " + clsid;
            AddTarget(plan, std::move(linked), identities);
        }
    }
}

void ScanCryptoProviders(CleanupPlan& plan, REGSAM view, std::unordered_set<std::wstring>& identities) {
    constexpr wchar_t providers[] = L"SOFTWARE\\Microsoft\\Cryptography\\Defaults\\Provider";
    RegKey root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, providers, 0, KEY_READ | view, root.put()) != ERROR_SUCCESS) return;
    for (const auto& name : EnumSubkeys(root.get())) {
        if (!IsCryptoProName(name) || ToLower(name).find(L"microsoft") != std::wstring::npos) continue;
        CleanupTarget target;
        target.type = TargetType::RegistryTree;
        target.category = CleanupTargetCategory::Provider;
        target.displayName = name;
        target.path = L"HKLM\\" + std::wstring(providers) + L"\\" + name;
        target.registry = {RegistryHive::LocalMachine, std::wstring(providers) + L"\\" + name, view};
        target.reason = L"Explicit CryptoPro cryptographic provider registration";
        target.verified = true;
        AddTarget(plan, std::move(target), identities);
    }
}

void ScanNativeMessagingRoot(CleanupPlan& plan, HKEY hive, RegistryHive hiveId, REGSAM view,
                             const std::wstring& rootPath, const std::vector<std::wstring>& approvedRoots,
                             std::unordered_set<std::wstring>& identities) {
    RegKey root;
    if (RegOpenKeyExW(hive, rootPath.c_str(), 0, KEY_READ | view, root.put()) != ERROR_SUCCESS) return;
    for (const auto& host : EnumSubkeys(root.get())) {
        const std::wstring manifest = CanonicalPath(ReadDefaultSubkey(root.get(), host, view));
        if (!BelongsToApprovedProduct(manifest, approvedRoots)) continue;
        CleanupTarget target;
        target.type = TargetType::RegistryTree;
        target.category = CleanupTargetCategory::NativeMessagingHost;
        target.displayName = L"Browser native host " + host;
        target.path = HiveName(hiveId) + L"\\" + rootPath + L"\\" + host;
        target.registry = {hiveId, rootPath + L"\\" + host, view};
        target.reason = L"Native messaging manifest: " + manifest;
        target.verified = true;
        AddTarget(plan, std::move(target), identities);
    }
}

std::wstring ReadTextBestEffort(const std::wstring& path, size_t maximumBytes = 512 * 1024) {
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(maximumBytes)) return {};
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read == 0) return {};
    if (read >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        return std::wstring(reinterpret_cast<wchar_t*>(bytes.data() + 2), (read - 2) / sizeof(wchar_t));
    }
    const bool bom = read >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
    const char* start = reinterpret_cast<const char*>(bytes.data()) + (bom ? 3 : 0);
    const int byteCount = static_cast<int>(read - (bom ? 3 : 0));
    UINT codePage = CP_UTF8;
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, byteCount, nullptr, 0);
    if (!chars) {
        codePage = CP_ACP;
        chars = MultiByteToWideChar(codePage, 0, start, byteCount, nullptr, 0);
    }
    if (!chars) return {};
    std::wstring result(chars, L'\0');
    MultiByteToWideChar(codePage, codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, start, byteCount, result.data(), chars);
    return result;
}

void ScanDriverPackages(CleanupPlan& plan, std::unordered_set<std::wstring>& identities) {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return;
    const std::wstring infRoot = JoinPath(windows, L"INF");
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(infRoot, L"oem*.inf").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring text = ToLower(ReadTextBestEffort(JoinPath(infRoot, data.cFileName)));
        std::wistringstream lines(text);
        std::wstring line;
        std::wstring providerExpression;
        std::map<std::wstring, std::wstring> variables;
        bool inVersion = false;
        while (std::getline(lines, line)) {
            line = Trim(line);
            if (line.empty() || line.front() == L';') continue;
            if (line.front() == L'[' && line.back() == L']') {
                inVersion = line == L"[version]";
                continue;
            }
            const size_t equal = line.find(L'=');
            if (equal == std::wstring::npos) continue;
            const std::wstring key = Trim(line.substr(0, equal));
            std::wstring value = Trim(line.substr(equal + 1));
            if (const size_t comment = value.find(L';'); comment != std::wstring::npos) value = Trim(value.substr(0, comment));
            value.erase(std::remove(value.begin(), value.end(), L'"'), value.end());
            variables[key] = value;
            if (inVersion && key == L"provider") providerExpression = value;
        }
        std::wstring provider = providerExpression;
        if (provider.size() >= 3 && provider.front() == L'%' && provider.back() == L'%') {
            const auto found = variables.find(provider.substr(1, provider.size() - 2));
            provider = found == variables.end() ? std::wstring() : found->second;
        }
        const bool vendor = IsCryptoProName(provider);
        const bool driver = text.find(L".sys") != std::wstring::npos || text.find(L"cproctrl") != std::wstring::npos || text.find(L"cpdrv") != std::wstring::npos;
        if (!vendor || !driver) continue;
        CleanupTarget target;
        target.type = TargetType::DriverPackage;
        target.category = CleanupTargetCategory::DriverPackage;
        target.displayName = data.cFileName;
        target.path = data.cFileName;
        target.reason = L"OEM INF declares CryptoPro driver files";
        target.verified = true;
        AddTarget(plan, std::move(target), identities);
    } while (FindNextFileW(search, &data));
    FindClose(search);
}

bool ResolveShortcut(const std::wstring& shortcut, std::wstring* target) {
    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shellLink)))) return false;
    IPersistFile* persist = nullptr;
    bool success = false;
    if (SUCCEEDED(shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist))) &&
        SUCCEEDED(persist->Load(shortcut.c_str(), STGM_READ))) {
        std::vector<wchar_t> path(32768, L'\0');
        WIN32_FIND_DATAW data{};
        if (SUCCEEDED(shellLink->GetPath(path.data(), static_cast<int>(path.size()), &data, SLGP_RAWPATH))) {
            *target = CanonicalPath(path.data());
            success = !target->empty();
        }
    }
    if (persist) persist->Release();
    shellLink->Release();
    return success;
}

void ScanShortcutsRecursive(CleanupPlan& plan, const std::wstring& directory, int depth,
                            const std::vector<std::wstring>& approvedRoots,
                            std::unordered_set<std::wstring>& identities) {
    if (depth > 8 || !DirectoryExists(directory)) return;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = JoinPath(directory, name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ScanShortcutsRecursive(plan, path, depth + 1, approvedRoots, identities);
            continue;
        }
        if (ToLower(name).size() < 4 || ToLower(name).substr(name.size() - 4) != L".lnk") continue;
        std::wstring targetPath;
        if (!ResolveShortcut(path, &targetPath) || !BelongsToApprovedProduct(targetPath, approvedRoots)) continue;
        CleanupTarget target;
        target.type = TargetType::Shortcut;
        target.category = CleanupTargetCategory::Shortcut;
        target.displayName = name;
        target.path = path;
        target.reason = L"Shortcut target: " + targetPath;
        target.verified = true;
        AddTarget(plan, std::move(target), identities);
    } while (FindNextFileW(search, &data));
    FindClose(search);
}

void ScanShortcuts(CleanupPlan& plan, const std::vector<std::wstring>& approvedRoots,
                   std::unordered_set<std::wstring>& identities) {
    const std::wstring common = GetKnownFolder(FOLDERID_CommonPrograms);
    if (!common.empty()) ScanShortcutsRecursive(plan, common, 0, approvedRoots, identities);
    for (const auto& profile : plan.profiles) {
        if (!profile.selected) continue;
        ScanShortcutsRecursive(plan, JoinPath(profile.profilePath, L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs"), 0, approvedRoots, identities);
        ScanShortcutsRecursive(plan, JoinPath(profile.profilePath, L"Desktop"), 0, approvedRoots, identities);
    }
}

void ScanTaskFolder(CleanupPlan& plan, ITaskFolder* folder, const std::vector<std::wstring>& approvedRoots,
                    std::unordered_set<std::wstring>& identities) {
    if (!folder) return;
    IRegisteredTaskCollection* tasks = nullptr;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
        LONG count = 0;
        tasks->get_Count(&count);
        for (LONG index = 1; index <= count; ++index) {
            VARIANT item{}; VariantInit(&item); item.vt = VT_I4; item.lVal = index;
            IRegisteredTask* task = nullptr;
            if (SUCCEEDED(tasks->get_Item(item, &task)) && task) {
                ITaskDefinition* definition = nullptr;
                bool matches = false;
                std::wstring executable;
                if (SUCCEEDED(task->get_Definition(&definition)) && definition) {
                    IActionCollection* actions = nullptr;
                    if (SUCCEEDED(definition->get_Actions(&actions)) && actions) {
                        LONG actionCount = 0; actions->get_Count(&actionCount);
                        for (LONG actionIndex = 1; actionIndex <= actionCount && !matches; ++actionIndex) {
                            IAction* action = nullptr;
                            if (SUCCEEDED(actions->get_Item(actionIndex, &action)) && action) {
                                TASK_ACTION_TYPE type{};
                                if (SUCCEEDED(action->get_Type(&type)) && type == TASK_ACTION_EXEC) {
                                    IExecAction* exec = nullptr;
                                    if (SUCCEEDED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec))) && exec) {
                                        BSTR raw = nullptr;
                                        if (SUCCEEDED(exec->get_Path(&raw)) && raw) { executable = CanonicalPath(raw); SysFreeString(raw); }
                                        matches = BelongsToApprovedProduct(executable, approvedRoots);
                                        exec->Release();
                                    }
                                }
                                action->Release();
                            }
                        }
                        actions->Release();
                    }
                    definition->Release();
                }
                if (matches) {
                    BSTR rawPath = nullptr;
                    if (SUCCEEDED(task->get_Path(&rawPath)) && rawPath) {
                        CleanupTarget target;
                        target.type = TargetType::ScheduledTask;
                        target.category = CleanupTargetCategory::ScheduledTask;
                        target.displayName = rawPath;
                        target.path = rawPath;
                        target.reason = L"Scheduled task executable: " + executable;
                        target.verified = true;
                        AddTarget(plan, std::move(target), identities);
                        SysFreeString(rawPath);
                    }
                }
                task->Release();
            }
        }
        tasks->Release();
    }
    ITaskFolderCollection* folders = nullptr;
    if (SUCCEEDED(folder->GetFolders(0, &folders)) && folders) {
        LONG count = 0; folders->get_Count(&count);
        for (LONG index = 1; index <= count; ++index) {
            VARIANT item{}; VariantInit(&item); item.vt = VT_I4; item.lVal = index;
            ITaskFolder* child = nullptr;
            if (SUCCEEDED(folders->get_Item(item, &child)) && child) { ScanTaskFolder(plan, child, approvedRoots, identities); child->Release(); }
        }
        folders->Release();
    }
}

void ScanScheduledTasks(CleanupPlan& plan, const std::vector<std::wstring>& approvedRoots,
                        std::unordered_set<std::wstring>& identities) {
    ITaskService* service = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(&service))) || !service) return;
    VARIANT empty{}; VariantInit(&empty);
    if (SUCCEEDED(service->Connect(empty, empty, empty, empty))) {
        ITaskFolder* root = nullptr;
        BSTR rootName = SysAllocString(L"\\");
        if (rootName && SUCCEEDED(service->GetFolder(rootName, &root)) && root) { ScanTaskFolder(plan, root, approvedRoots, identities); root->Release(); }
        SysFreeString(rootName);
    }
    service->Release();
}

void AddDefaultResidualTargets(CleanupPlan& plan, std::unordered_set<std::wstring>& identities) {
    const auto roots = DefaultVendorRoots();
    for (const auto& root : roots) {
        if (!DirectoryExists(root)) continue;
        CleanupTarget target;
        target.type = TargetType::Directory;
        target.category = CleanupTargetCategory::Directory;
        target.displayName = root;
        target.path = root;
        target.reason = L"Verified default CryptoPro vendor root";
        target.verified = true;
        AddTarget(plan, std::move(target), identities);
    }
    for (const REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        CleanupTarget target;
        target.type = TargetType::RegistryTree;
        target.category = CleanupTargetCategory::Registry;
        target.displayName = L"Crypto Pro registry root";
        target.path = L"HKLM\\SOFTWARE\\Crypto Pro";
        target.registry = {RegistryHive::LocalMachine, L"SOFTWARE\\Crypto Pro", view};
        target.reason = L"Explicit vendor registry root; key-container subtrees are preserved";
        target.verified = true;
        AddTarget(plan, target, identities);
        ScanCryptoProviders(plan, view, identities);
    }
    for (const auto& profile : plan.profiles) {
        if (!profile.selected) continue;
        CleanupTarget target;
        target.type = TargetType::RegistryTree;
        target.category = CleanupTargetCategory::Registry;
        target.displayName = L"Selected user Crypto Pro settings";
        target.path = L"PROFILE:" + profile.sid;
        target.registry = {RegistryHive::Users, L"SOFTWARE\\Crypto Pro", 0};
        target.reason = L"Selected local profile; key-container subtrees are preserved";
        target.verified = true;
        AddTarget(plan, std::move(target), identities);
    }
}

}  // namespace

CleanupPlan BuildCleanupPlan(const ScanResult& scan, const ProgressCallback& progress) {
    CleanupPlan plan;
    plan.products = scan.products;
    plan.profiles = scan.profiles;
    plan.protectedItems = scan.protectedItems;
    const size_t selectedCount = static_cast<size_t>(std::count_if(plan.products.begin(), plan.products.end(), [](const InstalledProduct& item) { return item.selected; }));
    plan.allDetectedProductsSelected = !plan.products.empty() && selectedCount == plan.products.size();
    std::unordered_set<std::wstring> identities;
    std::vector<std::wstring> approvedRoots = DefaultVendorRoots();

    ReportProgress(progress, L"Snapshotting installed product targets...", 10);
    for (const auto& product : plan.products) {
        if (!product.selected) continue;
        if (!product.installLocation.empty() &&
            (std::any_of(approvedRoots.begin(), approvedRoots.end(), [&](const std::wstring& root) { return PathStartsWith(product.installLocation, root); }) ||
             DirectoryContainsVerifiedBinary(product.installLocation))) {
            approvedRoots.push_back(product.installLocation);
            CleanupTarget directory;
            directory.type = TargetType::Directory;
            directory.category = CleanupTargetCategory::Directory;
            directory.displayName = product.displayName;
            directory.path = product.installLocation;
            directory.reason = L"InstallLocation of a confirmed CryptoPro publisher entry";
            directory.verified = true;
            AddTarget(plan, std::move(directory), identities);
        }
        CleanupTarget uninstallKey;
        uninstallKey.type = TargetType::RegistryTree;
        uninstallKey.category = CleanupTargetCategory::Registry;
        uninstallKey.displayName = product.displayName + L" uninstall entry";
        uninstallKey.path = HiveName(product.hive) + L"\\" + product.registryKey;
        uninstallKey.registry = {product.hive, product.registryKey, product.registryView};
        uninstallKey.reason = L"Registered uninstall entry for confirmed publisher";
        uninstallKey.verified = true;
        AddTarget(plan, std::move(uninstallKey), identities);
    }
    if (plan.allDetectedProductsSelected) AddDefaultResidualTargets(plan, identities);

    ReportProgress(progress, L"Snapshotting services and drivers...", 30);
    ScanServices(plan, approvedRoots, identities);
    ScanDriverPackages(plan, identities);

    ReportProgress(progress, L"Snapshotting COM and crypto providers...", 50);
    for (const REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        ScanComRegistrations(plan, HKEY_LOCAL_MACHINE, RegistryHive::LocalMachine, view, L"SOFTWARE\\Classes", approvedRoots, identities);
        ScanNativeMessagingRoot(plan, HKEY_LOCAL_MACHINE, RegistryHive::LocalMachine, view,
                                L"SOFTWARE\\Google\\Chrome\\NativeMessagingHosts", approvedRoots, identities);
        ScanNativeMessagingRoot(plan, HKEY_LOCAL_MACHINE, RegistryHive::LocalMachine, view,
                                L"SOFTWARE\\Microsoft\\Edge\\NativeMessagingHosts", approvedRoots, identities);
        ScanNativeMessagingRoot(plan, HKEY_LOCAL_MACHINE, RegistryHive::LocalMachine, view,
                                L"SOFTWARE\\Mozilla\\NativeMessagingHosts", approvedRoots, identities);
    }
    ScanComRegistrations(plan, HKEY_CURRENT_USER, RegistryHive::CurrentUser, 0, L"SOFTWARE\\Classes", approvedRoots, identities);
    for (const auto& root : {L"SOFTWARE\\Google\\Chrome\\NativeMessagingHosts", L"SOFTWARE\\Microsoft\\Edge\\NativeMessagingHosts", L"SOFTWARE\\Mozilla\\NativeMessagingHosts"}) {
        ScanNativeMessagingRoot(plan, HKEY_CURRENT_USER, RegistryHive::CurrentUser, 0, root, approvedRoots, identities);
    }

    ReportProgress(progress, L"Snapshotting tasks and shortcuts...", 75);
    ScanScheduledTasks(plan, approvedRoots, identities);
    ScanShortcuts(plan, approvedRoots, identities);
    ReportProgress(progress, L"Cleanup plan ready.", 100);
    return plan;
}

namespace {

DWORD RunProcessAndWait(const std::wstring& application, const std::wstring& commandLine) {
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return GetLastError();
    ScopedHandle processHandle(process.hProcess);
    ScopedHandle threadHandle(process.hThread);
    WaitForSingleObject(processHandle.get(), INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(processHandle.get(), &code);
    return code;
}

OperationRecord UninstallProduct(const InstalledProduct& product) {
    OperationRecord record;
    record.action = L"Uninstall";
    record.target = product.displayName;
    DWORD code = ERROR_INVALID_DATA;
    if (product.msi && IsGuid(product.productCode)) {
        INSTALLUILEVEL previous = MsiSetInternalUI(INSTALLUILEVEL_FULL, nullptr);
        code = MsiConfigureProductExW(product.productCode.c_str(), INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT, L"REBOOT=ReallySuppress");
        MsiSetInternalUI(previous, nullptr);
    } else {
        const std::wstring command = product.uninstallString.empty() ? product.quietUninstallString : product.uninstallString;
        const std::wstring executable = ExtractExecutable(command);
        std::vector<std::wstring> approved = DefaultVendorRoots();
        if (!product.installLocation.empty()) approved.push_back(product.installLocation);
        if (command.empty() || !FileExists(executable) || !BelongsToApprovedProduct(executable, approved)) {
            record.outcome = Outcome::Failed;
            record.code = ERROR_ACCESS_DENIED;
            record.message = L"Uninstaller executable could not be verified as a CryptoPro component.";
            return record;
        }
        code = RunProcessAndWait(executable, command);
    }
    record.code = code;
    if (code == ERROR_SUCCESS || code == ERROR_UNKNOWN_PRODUCT || code == ERROR_PRODUCT_UNINSTALLED) {
        record.outcome = Outcome::Succeeded;
        record.message = L"Registered uninstaller completed.";
    } else if (code == ERROR_SUCCESS_REBOOT_REQUIRED || code == ERROR_SUCCESS_REBOOT_INITIATED) {
        record.outcome = Outcome::RebootRequired;
        record.message = L"Registered uninstaller completed and requested a restart.";
    } else {
        record.outcome = Outcome::Failed;
        record.message = L"Registered uninstaller returned: " + GetLastErrorMessage(code);
    }
    return record;
}

bool ScheduleDelete(const std::wstring& path, bool* rebootRequired) {
    if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        if (rebootRequired) *rebootRequired = true;
        return true;
    }
    return false;
}

bool DeleteFileSafe(const std::wstring& path, bool* rebootRequired, DWORD* error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) { if (error) *error = ERROR_DIRECTORY; return false; }
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (DeleteFileW(path.c_str())) return true;
    const DWORD code = GetLastError();
    if ((code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED) && ScheduleDelete(path, rebootRequired)) return true;
    if (error) *error = code;
    return false;
}

bool DeleteDirectoryRecursive(const std::wstring& directory, const std::vector<std::wstring>& approvedRoots,
                              bool* rebootRequired, bool* protectedRetained, DWORD* error) {
    if (!DirectoryExists(directory)) return true;
    if (!IsSafeVendorPath(directory, approvedRoots) &&
        std::none_of(approvedRoots.begin(), approvedRoots.end(), [&](const std::wstring& root) {
            return ToLower(CanonicalPath(directory)) == ToLower(CanonicalPath(root));
        })) {
        if (error) *error = ERROR_ACCESS_DENIED;
        return false;
    }
    if (IsProtectedPath(directory)) {
        if (protectedRetained) *protectedRetained = true;
        return true;
    }
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = JoinPath(directory, name);
            if (IsProtectedPath(child)) {
                if (protectedRetained) *protectedRetained = true;
                continue;
            }
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (!RemoveDirectoryW(child.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND && error) *error = GetLastError();
                } else if (!DeleteDirectoryRecursive(child, approvedRoots, rebootRequired, protectedRetained, error)) {
                    FindClose(search);
                    return false;
                }
            } else if (!DeleteFileSafe(child, rebootRequired, error)) {
                FindClose(search);
                return false;
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(directory.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (RemoveDirectoryW(directory.c_str())) return true;
    const DWORD code = GetLastError();
    if ((code == ERROR_DIR_NOT_EMPTY || code == ERROR_ALREADY_EXISTS) && protectedRetained && *protectedRetained) return true;
    if ((code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED) && ScheduleDelete(directory, rebootRequired)) return true;
    if (code == ERROR_PATH_NOT_FOUND || code == ERROR_FILE_NOT_FOUND) return true;
    if (error) *error = code;
    return false;
}

bool DeleteRegistryBranchRecursive(HKEY parent, const std::wstring& child, REGSAM view,
                                   const std::wstring& displayPath, bool* protectedRetained) {
    if (IsProtectedRegistryPath(displayPath)) {
        if (protectedRetained) *protectedRetained = true;
        return true;
    }
    RegKey key;
    LONG open = RegOpenKeyExW(parent, child.c_str(), 0, KEY_READ | KEY_WRITE | view, key.put());
    if (open == ERROR_FILE_NOT_FOUND) return true;
    if (open != ERROR_SUCCESS) { SetLastError(open); return false; }
    const bool vendorTree = ToLower(displayPath).find(L"\\crypto pro") != std::wstring::npos;
    if (!vendorTree) {
        key.reset();
        const LONG deleted = RegDeleteTreeW(parent, child.c_str());
        if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) { SetLastError(deleted); return false; }
        RegDeleteKeyExW(parent, child.c_str(), view, 0);
        return true;
    }
    const auto children = EnumSubkeys(key.get());
    for (const auto& subkey : children) {
        if (!DeleteRegistryBranchRecursive(key.get(), subkey, 0, displayPath + L"\\" + subkey, protectedRetained)) return false;
    }
    for (;;) {
        std::array<wchar_t, 512> valueName{};
        DWORD length = static_cast<DWORD>(valueName.size());
        const LONG status = RegEnumValueW(key.get(), 0, valueName.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) break;
        RegDeleteValueW(key.get(), valueName.data());
    }
    key.reset();
    const LONG deleted = RegDeleteKeyExW(parent, child.c_str(), view, 0);
    if (deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND ||
        ((deleted == ERROR_ACCESS_DENIED || deleted == ERROR_KEY_HAS_CHILDREN) && protectedRetained && *protectedRetained)) return true;
    SetLastError(deleted);
    return false;
}

bool CleanProfileRegistry(const UserProfile& profile, bool* protectedRetained, DWORD* error) {
    RegKey loaded;
    if (RegOpenKeyExW(HKEY_USERS, profile.sid.c_str(), 0, KEY_READ | KEY_WRITE, loaded.put()) == ERROR_SUCCESS) {
        if (DeleteRegistryBranchRecursive(loaded.get(), L"SOFTWARE\\Crypto Pro", 0,
                                          L"HKU\\" + profile.sid + L"\\SOFTWARE\\Crypto Pro", protectedRetained)) return true;
        if (error) *error = GetLastError();
        return false;
    }
    const std::wstring hiveFile = JoinPath(profile.profilePath, L"NTUSER.DAT");
    if (!FileExists(hiveFile)) { if (error) *error = ERROR_FILE_NOT_FOUND; return false; }
    OfflineRegistryMount offlineHive;
    const LONG status = offlineHive.Open(hiveFile, KEY_READ | KEY_WRITE);
    if (status != ERROR_SUCCESS) { if (error) *error = status; return false; }
    const bool ok = DeleteRegistryBranchRecursive(offlineHive.get(), L"SOFTWARE\\Crypto Pro", 0,
                                                   L"HKU\\<PROFILE>\\SOFTWARE\\Crypto Pro", protectedRetained);
    const DWORD cleanupError = ok ? ERROR_SUCCESS : GetLastError();
    const LONG unloaded = offlineHive.Close();
    if (!ok) { if (error) *error = cleanupError; return false; }
    if (unloaded != ERROR_SUCCESS) { if (error) *error = unloaded; return false; }
    return true;
}

OperationRecord DeleteServiceTarget(const CleanupTarget& target, bool* rebootRequired) {
    OperationRecord record{target.type == TargetType::DriverService ? L"Remove driver service" : L"Remove service", target.displayName};
    ScopedScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) { record.outcome = Outcome::Failed; record.code = GetLastError(); record.message = GetLastErrorMessage(record.code); return record; }
    ScopedScHandle service(OpenServiceW(manager.get(), target.path.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
    if (!service) {
        record.code = GetLastError();
        if (record.code == ERROR_SERVICE_DOES_NOT_EXIST) { record.outcome = Outcome::Succeeded; record.message = L"Already absent."; }
        else { record.outcome = Outcome::Failed; record.message = GetLastErrorMessage(record.code); }
        return record;
    }
    SERVICE_STATUS status{};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
        const DWORD stopError = GetLastError();
        if (stopError != ERROR_SERVICE_NOT_ACTIVE && target.type == TargetType::DriverService) *rebootRequired = true;
    }
    if (!DeleteService(service.get())) {
        record.code = GetLastError();
        if (record.code != ERROR_SERVICE_MARKED_FOR_DELETE) { record.outcome = Outcome::Failed; record.message = GetLastErrorMessage(record.code); return record; }
    }
    record.outcome = target.type == TargetType::DriverService && *rebootRequired ? Outcome::RebootRequired : Outcome::Succeeded;
    record.message = *rebootRequired ? L"Marked for deletion after restart." : L"Removed.";
    return record;
}

bool DeleteScheduledTask(const std::wstring& taskPath, DWORD* error) {
    const size_t slash = taskPath.find_last_of(L'\\');
    const std::wstring folderPath = slash == 0 ? L"\\" : taskPath.substr(0, slash);
    const std::wstring name = slash == std::wstring::npos ? taskPath : taskPath.substr(slash + 1);
    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(&service));
    if (FAILED(hr) || !service) { if (error) *error = static_cast<DWORD>(hr); return false; }
    VARIANT empty{}; VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    ITaskFolder* folder = nullptr;
    if (SUCCEEDED(hr)) {
        BSTR folderName = SysAllocString(folderPath.c_str());
        hr = service->GetFolder(folderName, &folder);
        SysFreeString(folderName);
    }
    if (SUCCEEDED(hr) && folder) {
        BSTR taskName = SysAllocString(name.c_str());
        hr = folder->DeleteTask(taskName, 0);
        SysFreeString(taskName);
    }
    if (folder) folder->Release();
    service->Release();
    if (FAILED(hr) && HRESULT_CODE(hr) != ERROR_FILE_NOT_FOUND) { if (error) *error = static_cast<DWORD>(hr); return false; }
    return true;
}

OperationRecord ExecuteTarget(const CleanupTarget& target, const CleanupPlan& plan, bool* rebootRequired) {
    OperationRecord record;
    record.target = target.displayName.empty() ? target.path : target.displayName;
    if (!target.verified || target.protectedItem) {
        record.action = L"Skip unverified/protected target";
        record.outcome = Outcome::Skipped;
        record.code = ERROR_ACCESS_DENIED;
        record.message = L"Safety policy prevented removal.";
        return record;
    }
    DWORD error = ERROR_SUCCESS;
    bool protectedRetained = false;
    bool ok = false;
    bool targetReboot = false;
    switch (target.type) {
        case TargetType::Service:
        case TargetType::DriverService:
        {
            OperationRecord serviceRecord = DeleteServiceTarget(target, &targetReboot);
            *rebootRequired = *rebootRequired || targetReboot;
            return serviceRecord;
        }
        case TargetType::DriverPackage:
            record.action = L"Remove OEM driver package";
            ok = SetupUninstallOEMInfW(target.path.c_str(), 0, nullptr) != FALSE;
            if (!ok) { error = GetLastError(); if (error == ERROR_FILE_NOT_FOUND) ok = true; }
            break;
        case TargetType::ScheduledTask:
            record.action = L"Remove scheduled task";
            ok = DeleteScheduledTask(target.path, &error);
            break;
        case TargetType::RegistryTree: {
            record.action = L"Remove registry registration";
            if (target.path.rfind(L"PROFILE:", 0) == 0) {
                const std::wstring sid = target.path.substr(8);
                const auto profile = std::find_if(plan.profiles.begin(), plan.profiles.end(), [&](const UserProfile& item) { return item.sid == sid; });
                ok = profile != plan.profiles.end() && CleanProfileRegistry(*profile, &protectedRetained, &error);
            } else {
                ok = DeleteRegistryBranchRecursive(HiveHandle(target.registry.hive), target.registry.subkey,
                                                   target.registry.view,
                                                   HiveName(target.registry.hive) + L"\\" + target.registry.subkey,
                                                   &protectedRetained);
                if (!ok) error = GetLastError();
            }
            break;
        }
        case TargetType::Directory: {
            record.action = L"Remove verified directory";
            std::vector<std::wstring> roots = DefaultVendorRoots();
            for (const auto& product : plan.products) if (product.selected && !product.installLocation.empty()) roots.push_back(product.installLocation);
            ok = DeleteDirectoryRecursive(target.path, roots, &targetReboot, &protectedRetained, &error);
            break;
        }
        case TargetType::File:
        case TargetType::Shortcut:
            record.action = target.type == TargetType::Shortcut ? L"Remove shortcut" : L"Remove file";
            ok = DeleteFileSafe(target.path, &targetReboot, &error);
            break;
    }
    *rebootRequired = *rebootRequired || targetReboot;
    record.code = error;
    if (ok) {
        record.outcome = targetReboot ? Outcome::RebootRequired : (protectedRetained ? Outcome::Skipped : Outcome::Succeeded);
        record.message = protectedRetained ? L"Cleanup completed; protected key material was retained." :
                         (targetReboot ? L"Removal scheduled for restart." : L"Removed or already absent.");
    } else {
        record.outcome = Outcome::Failed;
        record.message = GetLastErrorMessage(error);
    }
    return record;
}

int TargetPriority(TargetType type) {
    switch (type) {
        case TargetType::ScheduledTask: return 0;
        case TargetType::Service: return 1;
        case TargetType::DriverService: return 2;
        case TargetType::DriverPackage: return 3;
        case TargetType::RegistryTree: return 4;
        case TargetType::Shortcut: return 5;
        case TargetType::File: return 6;
        case TargetType::Directory: return 7;
    }
    return 9;
}

}  // namespace

ExecutionResult ExecuteCleanup(const CleanupPlan& plan, bool allowForcedCleanup, const ProgressCallback& progress) {
    ExecutionResult result;
    if (!allowForcedCleanup) {
        const size_t selectedProducts = static_cast<size_t>(std::count_if(plan.products.begin(), plan.products.end(), [](const InstalledProduct& item) { return item.selected; }));
        size_t completed = 0;
        for (const auto& product : plan.products) {
            if (!product.selected) continue;
            ReportProgress(progress, L"Running registered uninstaller: " + product.displayName,
                           selectedProducts ? static_cast<int>((completed * 35) / selectedProducts) : 0);
            OperationRecord record = UninstallProduct(product);
            if (record.outcome == Outcome::Failed) result.anyFailure = true;
            if (record.outcome == Outcome::RebootRequired) result.rebootRequired = true;
            if (record.outcome == Outcome::Succeeded || record.outcome == Outcome::RebootRequired) result.anyRemoval = true;
            result.operations.push_back(std::move(record));
            ++completed;
        }
        if (result.anyFailure) {
            result.operations.push_back({L"Post-uninstall cleanup", L"All residual targets", Outcome::Skipped, ERROR_CANCELLED,
                                         L"At least one registered uninstaller failed; explicit forced-cleanup confirmation is required."});
            ReportProgress(progress, L"Registered uninstaller failed; forced cleanup was not authorized.", 100);
            return result;
        }
        if (result.rebootRequired) {
            result.residualCleanupDeferred = true;
            result.operations.push_back({L"Post-uninstall cleanup", L"All residual targets", Outcome::Skipped, ERROR_SUCCESS_REBOOT_REQUIRED,
                                         L"A registered uninstaller requested a restart; residual cleanup is deferred until resume."});
            ReportProgress(progress, L"Restart required before residual cleanup.", 100);
            return result;
        }
    }
    std::vector<const CleanupTarget*> ordered;
    ordered.reserve(plan.targets.size());
    for (const auto& target : plan.targets) ordered.push_back(&target);
    std::stable_sort(ordered.begin(), ordered.end(), [](const CleanupTarget* a, const CleanupTarget* b) {
        return TargetPriority(a->type) < TargetPriority(b->type);
    });
    for (size_t index = 0; index < ordered.size(); ++index) {
        ReportProgress(progress, L"Cleaning: " + ordered[index]->displayName,
                       35 + static_cast<int>(((index + 1) * 60) / (ordered.empty() ? 1 : ordered.size())));
        OperationRecord record = ExecuteTarget(*ordered[index], plan, &result.rebootRequired);
        if (record.outcome == Outcome::Failed) result.anyFailure = true;
        if (record.outcome == Outcome::Succeeded || record.outcome == Outcome::RebootRequired) result.anyRemoval = true;
        result.operations.push_back(std::move(record));
    }
    ReportProgress(progress, L"Cleanup pass completed.", 100);
    return result;
}

ScanResult VerifyAfterCleanup(Language language, const ProgressCallback& progress) {
    ScanResult verification = ScanSystem(language, progress);
    for (const auto& root : DefaultVendorRoots()) {
        if (!DirectoryExists(root)) continue;
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(JoinPath(root, L"*").c_str(), &data);
        bool actionable = false;
        if (search != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..") continue;
                const std::wstring child = JoinPath(root, name);
                if (!IsProtectedPath(child)) { actionable = true; break; }
            } while (FindNextFileW(search, &data));
            FindClose(search);
        }
        if (actionable) verification.warnings.push_back(L"Actionable vendor directory remains: " + root);
    }
    CleanupPlan residual = BuildCleanupPlan(verification);
    std::unordered_set<std::wstring> identities;
    for (const auto& target : residual.targets) identities.insert(TargetIdentity(target));
    ScanCryptoProviders(residual, KEY_WOW64_32KEY, identities);
    if (verification.osArchitecture != L"x86") ScanCryptoProviders(residual, KEY_WOW64_64KEY, identities);
    for (const auto& target : residual.targets) {
        if (!target.protectedItem && target.verified) {
            verification.warnings.push_back(L"Verified residual remains: " +
                (target.displayName.empty() ? target.path : target.displayName));
        }
    }
    return verification;
}

bool EnsureDirectory(const std::wstring& path, std::wstring* error) {
    if (DirectoryExists(path)) return true;
    const int status = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_EXISTS || status == ERROR_ALREADY_EXISTS) return true;
    if (error) *error = GetLastErrorMessage(static_cast<DWORD>(status));
    return false;
}

BackupFolderValidation InspectBackupPath(const std::wstring& path,
                                         const std::wstring& sourcePath,
                                         unsigned long long minimumFreeBytes) {
    BackupFolderValidation result;
    result.requiredBytes = minimumFreeBytes;
    const std::wstring trimmed = Trim(path);
    if (trimmed.empty()) { result.detail = L"The backup path is empty."; return result; }
    const std::wstring canonical = CanonicalPath(trimmed);
    result.normalizedPath = canonical;
    std::array<wchar_t, MAX_PATH> backupVolume{};
    if (canonical.empty() || !GetVolumePathNameW(canonical.c_str(), backupVolume.data(),
                                                 static_cast<DWORD>(backupVolume.size()))) {
        result.state = BackupFolderState::NotWritable;
        result.detail = L"The backup volume could not be resolved.";
        return result;
    }
    result.volumeRoot = CanonicalPath(backupVolume.data());
    std::array<wchar_t, MAX_PATH> windowsDirectory{};
    GetWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
    const std::vector<std::wstring> unsuitableRoots{
        windowsDirectory.data(), ExpandEnvironment(L"%ProgramFiles%"),
        ExpandEnvironment(L"%ProgramFiles(x86)%")
    };
    if (IsProtectedPath(canonical) ||
        ToLower(canonical) == ToLower(CanonicalPath(backupVolume.data())) ||
        std::any_of(unsuitableRoots.begin(), unsuitableRoots.end(), [&](const std::wstring& root) {
            return !root.empty() && PathStartsWith(canonical, root);
        })) {
        result.state = BackupFolderState::UnsafeLocation;
        result.detail = L"The selected location is protected or unsuitable for a backup.";
        return result;
    }
    if (!sourcePath.empty()) {
        std::array<wchar_t, MAX_PATH> sourceVolume{};
        if (!GetVolumePathNameW(sourcePath.c_str(), sourceVolume.data(), static_cast<DWORD>(sourceVolume.size())) ||
            ToLower(CanonicalPath(sourceVolume.data())) == ToLower(CanonicalPath(backupVolume.data()))) {
            result.state = BackupFolderState::SameVolume;
            result.detail = L"Offline cleanup requires a backup on another volume.";
            return result;
        }
    }
    const DWORD targetAttributes = GetFileAttributesW(canonical.c_str());
    if (targetAttributes != INVALID_FILE_ATTRIBUTES && !(targetAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.state = BackupFolderState::NotWritable;
        result.detail = L"The backup path points to a file.";
        return result;
    }
    std::wstring existing = canonical;
    while (!DirectoryExists(existing)) {
        std::wstring parent = ParentPath(existing);
        if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
        if (parent.empty() || ToLower(parent) == ToLower(existing)) break;
        existing = std::move(parent);
    }
    if (!DirectoryExists(existing)) existing = backupVolume.data();
    if (!DirectoryExists(existing)) {
        result.state = BackupFolderState::NotWritable;
        result.detail = L"No existing parent folder could be resolved.";
        return result;
    }
    result.nearestExistingParent = CanonicalPath(existing);
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(existing.c_str(), &available, &total, &free)) {
        result.state = BackupFolderState::NotWritable;
        result.detail = GetLastErrorMessage(GetLastError());
        return result;
    }
    result.freeBytes = available.QuadPart;
    if (available.QuadPart < minimumFreeBytes) {
        result.state = BackupFolderState::InsufficientSpace;
        result.detail = L"There is not enough free space for the backup.";
        return result;
    }
    result.state = BackupFolderState::ReadyForProbe;
    result.detail = L"The backup path passed read-only inspection.";
    return result;
}

BackupFolderValidation ProbeBackupFolder(const std::wstring& path,
                                         const std::wstring& sourcePath,
                                         unsigned long long minimumFreeBytes) {
    BackupFolderValidation result = InspectBackupPath(path, sourcePath, minimumFreeBytes);
    if (!result.canProbe()) return result;
    const std::wstring probeParent = DirectoryExists(result.normalizedPath)
        ? result.normalizedPath : result.nearestExistingParent;
    const std::wstring probe = JoinPath(probeParent, L".cryptopro-cleanup-write-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".tmp");
    {
        ScopedHandle file(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        result.probePerformed = true;
        if (!file) {
            result.state = BackupFolderState::NotWritable;
            result.detail = GetLastErrorMessage(GetLastError());
            return result;
        }
        constexpr char marker[] = "CryptoPro Cleanup backup write test\r\n";
        DWORD written = 0;
        if (!WriteFile(file.get(), marker, static_cast<DWORD>(sizeof(marker) - 1), &written, nullptr) ||
            written != sizeof(marker) - 1 || !FlushFileBuffers(file.get())) {
            result.state = BackupFolderState::NotWritable;
            result.detail = GetLastErrorMessage(GetLastError());
            return result;
        }
    }
    result.state = BackupFolderState::Available;
    result.detail = L"The backup folder is writable.";
    return result;
}

BackupFolderValidation ValidateBackupFolder(const std::wstring& path,
                                            const std::wstring& sourcePath,
                                            unsigned long long minimumFreeBytes) {
    return ProbeBackupFolder(path, sourcePath, minimumFreeBytes);
}

bool WriteUtf8File(const std::wstring& path, const std::string& content, std::wstring* error) {
    const std::wstring parent = ParentPath(path);
    if (!parent.empty() && !EnsureDirectory(parent, error)) return false;
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) { if (error) *error = GetLastErrorMessage(GetLastError()); return false; }
    DWORD written = 0;
    if (!content.empty() && (!WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size())) {
        if (error) *error = GetLastErrorMessage(GetLastError());
        return false;
    }
    return true;
}

bool AppendLog(const std::wstring& path, const std::wstring& line) {
    ScopedHandle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;
    const std::string bytes = Utf8(L"[" + Timestamp() + L"] " + line + L"\r\n");
    DWORD written = 0;
    return WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
}

size_t CountSelectedCertificates(const std::vector<CertificateEntry>& certificates) {
    return static_cast<size_t>(std::count_if(certificates.begin(), certificates.end(),
        [](const CertificateEntry& certificate) { return certificate.selected; }));
}

size_t CountSelectedProducts(const std::vector<InstalledProduct>& products) {
    return static_cast<size_t>(std::count_if(products.begin(), products.end(),
        [](const InstalledProduct& product) { return product.selected; }));
}

size_t CountVerifiedTargets(const std::vector<CleanupTarget>& targets) {
    return static_cast<size_t>(std::count_if(targets.begin(), targets.end(),
        [](const CleanupTarget& target) { return target.verified && !target.protectedItem; }));
}

unsigned long long CertificateExpiryKey(const CertificateEntry& certificate) {
    if (!certificate.encoded.empty()) {
        PCCERT_CONTEXT context = CertCreateCertificateContext(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, certificate.encoded.data(),
            static_cast<DWORD>(certificate.encoded.size()));
        if (context) {
            ULARGE_INTEGER value{};
            value.LowPart = context->pCertInfo->NotAfter.dwLowDateTime;
            value.HighPart = context->pCertInfo->NotAfter.dwHighDateTime;
            CertFreeCertificateContext(context);
            return value.QuadPart;
        }
    }
    unsigned day = 0, month = 0, year = 0;
    if (swscanf_s(certificate.validTo.c_str(), L"%u.%u.%u", &day, &month, &year) == 3) {
        SYSTEMTIME time{};
        time.wDay = static_cast<WORD>(day);
        time.wMonth = static_cast<WORD>(month);
        time.wYear = static_cast<WORD>(year);
        FILETIME fileTime{};
        if (SystemTimeToFileTime(&time, &fileTime)) {
            ULARGE_INTEGER value{};
            value.LowPart = fileTime.dwLowDateTime;
            value.HighPart = fileTime.dwHighDateTime;
            return value.QuadPart;
        }
    }
    return 0;
}

CertificateStatus GetCertificateStatus(const CertificateEntry& certificate,
                                       const FILETIME* now, unsigned soonDays) {
    if (certificate.encoded.empty()) return CertificateStatus::Unknown;
    PCCERT_CONTEXT context = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, certificate.encoded.data(),
        static_cast<DWORD>(certificate.encoded.size()));
    if (!context) return CertificateStatus::Unknown;
    FILETIME current{};
    if (now) current = *now;
    else GetSystemTimeAsFileTime(&current);
    const LONG validity = CertVerifyTimeValidity(&current, context->pCertInfo);
    const FILETIME expiry = context->pCertInfo->NotAfter;
    CertFreeCertificateContext(context);
    if (validity > 0) return CertificateStatus::Expired;
    if (validity < 0) return CertificateStatus::NotYetValid;
    ULARGE_INTEGER currentValue{}, expiryValue{};
    currentValue.LowPart = current.dwLowDateTime;
    currentValue.HighPart = current.dwHighDateTime;
    expiryValue.LowPart = expiry.dwLowDateTime;
    expiryValue.HighPart = expiry.dwHighDateTime;
    constexpr unsigned long long ticksPerDay = 864000000000ULL;
    const unsigned long long threshold = static_cast<unsigned long long>(soonDays) * ticksPerDay;
    return expiryValue.QuadPart >= currentValue.QuadPart &&
           expiryValue.QuadPart - currentValue.QuadPart <= threshold
        ? CertificateStatus::ExpiringSoon : CertificateStatus::Valid;
}

void SetCertificateSelection(std::vector<CertificateEntry>& certificates,
                             const std::vector<size_t>& indices, bool selected) {
    for (const size_t index : indices)
        if (index < certificates.size()) certificates[index].selected = selected;
}

bool IsOfflineConfirmation(const std::wstring& phrase) {
    return phrase == L"OFFLINE";
}

void MergeExecutionResults(ExecutionResult* destination, ExecutionResult source) {
    if (!destination) return;
    destination->rebootRequired = destination->rebootRequired || source.rebootRequired;
    destination->residualCleanupDeferred = destination->residualCleanupDeferred || source.residualCleanupDeferred;
    destination->anyFailure = destination->anyFailure || source.anyFailure;
    destination->anyRemoval = destination->anyRemoval || source.anyRemoval;
    destination->operations.insert(destination->operations.end(),
                                   std::make_move_iterator(source.operations.begin()),
                                   std::make_move_iterator(source.operations.end()));
}

std::wstring RedactSensitiveText(const std::wstring& value, const ScanResult& scan) {
    return Redact(value, scan);
}

namespace {

const wchar_t* OutcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::Succeeded: return L"succeeded";
        case Outcome::Skipped: return L"skipped";
        case Outcome::Failed: return L"failed";
        case Outcome::RebootRequired: return L"reboot_required";
    }
    return L"unknown";
}

const wchar_t* TargetTypeName(TargetType type) {
    switch (type) {
        case TargetType::File: return L"file";
        case TargetType::Directory: return L"directory";
        case TargetType::RegistryTree: return L"registry";
        case TargetType::Service: return L"service";
        case TargetType::DriverService: return L"driver_service";
        case TargetType::DriverPackage: return L"driver_package";
        case TargetType::ScheduledTask: return L"scheduled_task";
        case TargetType::Shortcut: return L"shortcut";
    }
    return L"unknown";
}

void JsonString(std::wostringstream& out, const std::wstring& value) { out << L'"' << JsonEscape(value) << L'"'; }

}  // namespace

bool WriteJsonReport(const std::wstring& path, const ScanResult& scan, const CleanupPlan* plan,
                     const ExecutionResult* execution, const ScanResult* verification, std::wstring* error) {
    std::wostringstream out;
    out << L"{\n  \"schema_version\": 1,\n  \"utility_version\": \"" << kVersion << L"\",\n";
    out << L"  \"generated_at\": "; JsonString(out, Timestamp()); out << L",\n";
    out << L"  \"system\": {\"os\": "; JsonString(out, scan.osName); out << L", \"architecture\": "; JsonString(out, scan.osArchitecture); out << L"},\n";
    out << L"  \"products\": [\n";
    for (size_t index = 0; index < scan.products.size(); ++index) {
        const auto& item = scan.products[index];
        out << L"    {\"name\": "; JsonString(out, item.displayName); out << L", \"version\": "; JsonString(out, item.version);
        out << L", \"publisher\": "; JsonString(out, item.publisher); out << L", \"architecture\": "; JsonString(out, item.architecture);
        out << L", \"installer\": \"" << (item.msi ? L"msi" : L"exe") << L"\", \"risk\": \"" << (item.risk == RiskLevel::High ? L"high" : L"normal") << L"\", \"selected\": " << (item.selected ? L"true" : L"false") << L"}";
        out << (index + 1 == scan.products.size() ? L"\n" : L",\n");
    }
    out << L"  ],\n  \"profiles\": [";
    for (size_t index = 0; index < scan.profiles.size(); ++index) {
        if (index) out << L", ";
        out << L"{\"id\": \"profile-" << (index + 1) << L"\", \"selected\": " << (scan.profiles[index].selected ? L"true" : L"false") << L"}";
    }
    out << L"],\n  \"licenses\": [";
    for (size_t index = 0; index < scan.licenses.size(); ++index) {
        if (index) out << L", ";
        out << L"{\"product\": "; JsonString(out, scan.licenses[index].product);
        out << L", \"value_name\": "; JsonString(out, scan.licenses[index].valueName);
        out << L", \"masked\": "; JsonString(out, scan.licenses[index].maskedValue); out << L"}";
    }
    out << L"],\n  \"certificates\": {\"store\": \"CurrentUser/My\", \"found\": " << scan.certificates.size()
        << L", \"selected_for_public_export\": "
        << std::count_if(scan.certificates.begin(), scan.certificates.end(), [](const CertificateEntry& item) { return item.selected; })
        << L", \"private_keys_exported\": false},\n  \"protected_items\": [";
    const std::vector<std::wstring>& protectedItems = plan ? plan->protectedItems : scan.protectedItems;
    for (size_t index = 0; index < protectedItems.size(); ++index) {
        if (index) out << L", ";
        JsonString(out, Redact(protectedItems[index], scan));
    }
    out << L"],\n  \"warnings\": [";
    for (size_t index = 0; index < scan.warnings.size(); ++index) { if (index) out << L", "; JsonString(out, Redact(scan.warnings[index], scan)); }
    out << L"]";
    if (plan) {
        out << L",\n  \"cleanup_plan\": {\"all_detected_products_selected\": " << (plan->allDetectedProductsSelected ? L"true" : L"false")
            << L", \"selected_products\": " << CountSelectedProducts(plan->products)
            << L", \"verified_targets\": " << CountVerifiedTargets(plan->targets) << L", \"targets\": [\n";
        for (size_t index = 0; index < plan->targets.size(); ++index) {
            const auto& target = plan->targets[index];
            out << L"    {\"type\": \"" << TargetTypeName(target.type) << L"\", \"target\": ";
            JsonString(out, Redact(target.displayName.empty() ? target.path : target.displayName, scan));
            out << L", \"verified\": " << (target.verified ? L"true" : L"false") << L"}";
            out << (index + 1 == plan->targets.size() ? L"\n" : L",\n");
        }
        out << L"  ]}";
    }
    if (execution) {
        out << L",\n  \"execution\": {\"reboot_required\": " << (execution->rebootRequired ? L"true" : L"false")
            << L", \"residual_cleanup_deferred\": " << (execution->residualCleanupDeferred ? L"true" : L"false")
            << L", \"partial\": " << (execution->anyFailure ? L"true" : L"false") << L", \"operations\": [\n";
        for (size_t index = 0; index < execution->operations.size(); ++index) {
            const auto& operation = execution->operations[index];
            out << L"    {\"action\": "; JsonString(out, operation.action); out << L", \"target\": "; JsonString(out, Redact(operation.target, scan));
            out << L", \"outcome\": \"" << OutcomeName(operation.outcome) << L"\", \"code\": " << operation.code << L", \"message\": ";
            JsonString(out, Redact(operation.message, scan)); out << L"}";
            out << (index + 1 == execution->operations.size() ? L"\n" : L",\n");
        }
        out << L"  ]}";
    }
    if (verification) {
        out << L",\n  \"verification\": {\"remaining_products\": " << verification->products.size() << L", \"warnings\": [";
        for (size_t index = 0; index < verification->warnings.size(); ++index) { if (index) out << L", "; JsonString(out, Redact(verification->warnings[index], *verification)); }
        out << L"]}";
    }
    out << L"\n}\n";
    return WriteUtf8File(path, Utf8(out.str()), error);
}

bool WriteTextSummary(Language language, const std::wstring& path, const ScanResult& scan,
                      const CleanupPlan* plan, const ExecutionResult* execution,
                      const ScanResult* verification, std::wstring* error) {
    std::wostringstream out;
    out << (language == Language::Russian ? L"КриптоПро Очистка" : L"CryptoPro Cleanup Utility")
        << L" " << kVersion << L"\r\n"
        << Tr(language, L"Понятный текстовый отчёт", L"Human-readable text report") << L"\r\n"
        << L"============================================================\r\n\r\n";
    out << Tr(language, L"Дата формирования: ", L"Generated: ") << Timestamp() << L"\r\n"
        << Tr(language, L"Операционная система: ", L"Operating system: ") << scan.osName << L" (" << scan.osArchitecture << L")\r\n\r\n";

    out << Tr(language, L"НАЙДЕННЫЕ ПРОДУКТЫ", L"DETECTED PRODUCTS") << L"\r\n";
    if (scan.products.empty()) out << Tr(language, L"  Продукты подтверждённого издателя не найдены.", L"  No confirmed-publisher products were found.") << L"\r\n";
    for (const auto& product : scan.products) {
        out << L"  [" << (product.selected ? L"X" : L" ") << L"] " << product.displayName;
        if (!product.version.empty()) out << L" " << product.version;
        out << L"; " << product.architecture << L"; " << (product.msi ? L"MSI" : L"EXE");
        if (product.risk == RiskLevel::High) out << L"; " << Tr(language, L"ВЫСОКИЙ РИСК", L"HIGH RISK");
        out << L"\r\n";
    }
    out << L"\r\n";

    const size_t selectedProfiles = static_cast<size_t>(std::count_if(scan.profiles.begin(), scan.profiles.end(), [](const UserProfile& profile) { return profile.selected; }));
    out << Tr(language, L"ПРОФИЛИ", L"PROFILES") << L"\r\n"
        << Tr(language, L"  Найдено локальных профилей: ", L"  Local profiles found: ") << scan.profiles.size() << L"\r\n"
        << Tr(language, L"  Выбрано для очистки настроек: ", L"  Selected for settings cleanup: ") << selectedProfiles << L"\r\n"
        << Tr(language, L"  Имена и SID намеренно не включены в отчёт.", L"  Names and SIDs are intentionally omitted from this report.") << L"\r\n\r\n";

    out << Tr(language, L"ЛИЦЕНЗИИ", L"LICENSES") << L"\r\n";
    if (scan.licenses.empty()) out << Tr(language, L"  Известные значения лицензий не найдены.", L"  No known license values were found.") << L"\r\n";
    for (const auto& license : scan.licenses) {
        out << L"  " << license.product << L": " << license.maskedValue << L" (" << license.valueName << L")\r\n";
    }
    out << Tr(language,
              L"  Полные номера сохранены только в licenses.txt и доступны по кнопке программы.",
              L"  Full identifiers are stored only in licenses.txt and available through the program button.") << L"\r\n\r\n";

    const size_t selectedCertificates = static_cast<size_t>(std::count_if(
        scan.certificates.begin(), scan.certificates.end(), [](const CertificateEntry& item) { return item.selected; }));
    out << Tr(language, L"ОТКРЫТЫЕ СЕРТИФИКАТЫ", L"PUBLIC CERTIFICATES") << L"\r\n"
        << Tr(language, L"  Найдено в личных хранилищах пользователей: ", L"  Found in user Personal stores: ")
        << scan.certificates.size() << L"\r\n"
        << Tr(language, L"  Выбрано для экспорта: ", L"  Selected for export: ") << selectedCertificates << L"\r\n"
        << Tr(language,
              L"  В отчёт не включены имена владельцев и издателей. Закрытые ключи не экспортируются.",
              L"  Subject and issuer names are omitted from this report. Private keys are not exported.") << L"\r\n\r\n";

    out << Tr(language, L"ЗАЩИЩЁННЫЕ ДАННЫЕ — НЕ УДАЛЯЮТСЯ", L"PROTECTED DATA — NEVER REMOVED") << L"\r\n";
    const auto& protectedItems = plan ? plan->protectedItems : scan.protectedItems;
    for (const auto& item : protectedItems) out << L"  - " << Redact(item, scan) << L"\r\n";
    out << L"\r\n";

    if (plan) {
        out << Tr(language, L"ПЛАН ОЧИСТКИ", L"CLEANUP PLAN") << L"\r\n"
            << Tr(language, L"  Выбрано продуктов: ", L"  Selected products: ")
            << CountSelectedProducts(plan->products) << L"\r\n"
            << Tr(language, L"  Подтверждённых целей: ", L"  Verified targets: ") << CountVerifiedTargets(plan->targets) << L"\r\n\r\n";
    }

    if (execution) {
        out << Tr(language, L"ВЫПОЛНЕННЫЕ ОПЕРАЦИИ", L"EXECUTED OPERATIONS") << L"\r\n";
        for (const auto& operation : execution->operations) {
            out << L"  [" << OutcomeName(operation.outcome) << L"] " << operation.action << L": "
                << Redact(operation.target, scan) << L"; " << Redact(operation.message, scan);
            if (operation.code) out << L" (code " << operation.code << L")";
            out << L"\r\n";
        }
        out << L"\r\n" << Tr(language, L"  Требуется перезагрузка: ", L"  Restart required: ")
            << (execution->rebootRequired ? Tr(language, L"да", L"yes") : Tr(language, L"нет", L"no")) << L"\r\n"
            << Tr(language, L"  Ошибки или частичный результат: ", L"  Errors or partial result: ")
            << (execution->anyFailure ? Tr(language, L"да", L"yes") : Tr(language, L"нет", L"no")) << L"\r\n\r\n";
    } else {
        out << Tr(language, L"Операции удаления ещё не выполнялись.", L"Removal operations have not run yet.") << L"\r\n\r\n";
    }

    if (verification) {
        out << Tr(language, L"ИТОГОВАЯ ПРОВЕРКА", L"FINAL VERIFICATION") << L"\r\n"
            << Tr(language, L"  Оставшихся зарегистрированных продуктов: ", L"  Remaining registered products: ") << verification->products.size() << L"\r\n";
        if (verification->warnings.empty()) out << Tr(language, L"  Подтверждённые исполняемые остатки не найдены.", L"  No verified executable remnants were found.") << L"\r\n";
        for (const auto& warning : verification->warnings) out << L"  - " << Redact(warning, *verification) << L"\r\n";
        out << L"\r\n";
    }

    out << Tr(language, L"ФАЙЛЫ В ЭТОЙ ПАПКЕ", L"FILES IN THIS FOLDER") << L"\r\n"
        << L"  licenses.txt  — " << Tr(language, L"полные номера лицензий; хранить конфиденциально", L"full license identifiers; keep confidential") << L"\r\n"
        << L"  CryptoProCertificates-* — " << Tr(language, L"выбранные открытые .cer, общий .p7b и конфиденциальный каталог", L"selected public .cer files, a combined .p7b, and a confidential catalog") << L"\r\n"
        << L"  summary.txt   — " << Tr(language, L"этот понятный текстовый отчёт", L"this human-readable text report") << L"\r\n"
        << L"  report.json   — " << Tr(language, L"структурированный технический отчёт без полных лицензий", L"structured technical report without full licenses") << L"\r\n"
        << L"  cleanup.log   — " << Tr(language, L"журнал хода операций без полных лицензий", L"operation log without full licenses") << L"\r\n\r\n"
        << Tr(language,
              L"Перед переустановкой Windows скопируйте всю папку на внешний носитель или в защищённое облако.",
              L"Before reinstalling Windows, copy the entire folder to external storage or protected cloud storage.") << L"\r\n";
    return WriteUtf8File(path, Utf8(out.str()), error);
}

bool WriteEmergencyCleanupLog(const std::wstring& path, const ScanResult& redactionContext,
                              const std::wstring& lastCompletedStage,
                              const ExecutionResult* execution, DWORD errorCode,
                              std::wstring* error) {
    std::wostringstream out;
    out << L"CryptoPro Cleanup Utility " << kVersion << L"\r\n"
        << L"EMERGENCY CLEANUP RECORD\r\n"
        << L"Timestamp: " << Timestamp() << L"\r\n"
        << L"Last completed stage: " << Redact(lastCompletedStage, redactionContext) << L"\r\n"
        << L"Error code: " << errorCode << L"\r\n"
        << L"Manual verification is required.\r\n\r\n";
    if (execution) {
        for (const auto& operation : execution->operations)
            out << L"[" << OutcomeName(operation.outcome) << L"] "
                << Redact(operation.action, redactionContext) << L" | "
                << Redact(operation.target, redactionContext) << L" | "
                << Redact(operation.message, redactionContext) << L"\r\n";
    }
    return WriteUtf8File(path, Utf8(out.str()), error);
}

bool SaveBackup(Language language, const ScanResult& scan, const CleanupPlan& plan, const std::wstring& folder,
                std::wstring* licensesPath, std::wstring* initialReportPath,
                std::wstring* logPath, std::wstring* error) {
    if (folder.empty()) { if (error) *error = L"Backup folder is empty."; return false; }
    const std::wstring sessionFolder = JoinPath(folder, L"CryptoProCleanup-" + Timestamp());
    if (!EnsureDirectory(sessionFolder, error)) return false;
    const std::wstring licenseFile = JoinPath(sessionFolder, L"licenses.txt");
    const std::wstring summaryFile = JoinPath(sessionFolder, L"summary.txt");
    const std::wstring reportFile = JoinPath(sessionFolder, L"report.json");
    const std::wstring logFile = JoinPath(sessionFolder, L"cleanup.log");
    std::wostringstream licenses;
    licenses << L"CryptoPro Cleanup Utility " << kVersion << L"\r\n"
             << L"WARNING / ВНИМАНИЕ: this file contains full license identifiers. Store it securely.\r\n"
             << L"Этот файл содержит полные номера лицензий. Храните его в защищенном месте.\r\n\r\n";
    if (scan.licenses.empty()) licenses << L"No known license values were found / Известные значения лицензий не найдены.\r\n";
    for (size_t index = 0; index < scan.licenses.size(); ++index) {
        const auto& license = scan.licenses[index];
        licenses << L"[" << (index + 1) << L"] " << license.product << L"\r\n"
                 << L"Source: " << license.registryPath << L" / " << license.valueName << L"\r\n"
                 << L"License: " << license.fullValue << L"\r\n\r\n";
    }
    if (!WriteUtf8File(licenseFile, Utf8(licenses.str()), error)) return false;
    if (!ExportPublicCertificates(language, scan.certificates, sessionFolder, nullptr, error)) return false;
    if (!WriteTextSummary(language, summaryFile, scan, &plan, nullptr, nullptr, error)) return false;
    if (!WriteJsonReport(reportFile, scan, &plan, nullptr, nullptr, error)) return false;
    if (!WriteUtf8File(logFile, Utf8(L"CryptoPro Cleanup Utility " + std::wstring(kVersion) + L"\r\nNo license values are written to this log.\r\n"), error)) return false;
    if (licensesPath) *licensesPath = licenseFile;
    if (initialReportPath) *initialReportPath = reportFile;
    if (logPath) *logPath = logFile;
    return true;
}

namespace {

std::wstring Sha256Hex(const std::wstring& value) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::wstring result;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        const std::string bytes = Utf8(value);
        if (CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0)) {
            std::array<BYTE, 32> digest{};
            DWORD size = static_cast<DWORD>(digest.size());
            if (CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &size, 0)) {
                std::wostringstream out;
                for (DWORD index = 0; index < size; ++index) out << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(digest[index]);
                result = out.str();
            }
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return result;
}

std::wstring HexEncode(const std::wstring& value) {
    const std::string bytes = Utf8(value);
    std::wostringstream out;
    for (const unsigned char byte : bytes) out << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(byte);
    return out.str();
}

std::wstring HexDecode(const std::wstring& value) {
    if (value.size() % 2) return {};
    std::string bytes;
    bytes.reserve(value.size() / 2);
    for (size_t index = 0; index < value.size(); index += 2) {
        wchar_t* end = nullptr;
        const std::wstring pair = value.substr(index, 2);
        const long parsed = wcstol(pair.c_str(), &end, 16);
        if (!end || *end) return {};
        bytes.push_back(static_cast<char>(parsed));
    }
    return FromUtf8(bytes);
}

bool ApplyPrivateAcl(const std::wstring& path, bool directory, std::wstring* error) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
        if (error) *error = GetLastErrorMessage(GetLastError());
        return false;
    }
    const bool ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != FALSE;
    if (!ok && error) *error = GetLastErrorMessage(GetLastError());
    LocalFree(descriptor);
    if (ok && directory) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    return ok;
}

std::wstring ResumeRoot(const std::wstring& overrideRoot = {}) {
    return overrideRoot.empty() ? JoinPath(GetProgramData(), L"CryptoProCleanup\\Sessions")
                                : CanonicalPath(overrideRoot);
}

std::wstring ResumeSessionPath(const std::wstring& token, const std::wstring& overrideRoot = {}) {
    return JoinPath(ResumeRoot(overrideRoot), token);
}

bool ReadStateLines(const std::wstring& path, std::multimap<std::wstring, std::wstring>* values, std::wstring* error) {
    const std::wstring text = ReadTextBestEffort(path, 1024 * 1024);
    if (text.empty()) { if (error) *error = L"Resume state is empty or unreadable."; return false; }
    std::wistringstream input(text);
    std::wstring line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const size_t equal = line.find(L'=');
        if (equal == std::wstring::npos) continue;
        values->emplace(line.substr(0, equal), line.substr(equal + 1));
    }
    return true;
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return size ? std::wstring(buffer.data(), size) : std::wstring();
}

std::vector<std::wstring> SplitStateFields(const std::wstring& value) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (;;) {
        const size_t separator = value.find(L'|', start);
        fields.push_back(value.substr(start, separator == std::wstring::npos ? separator : separator - start));
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    return fields;
}

unsigned long ParseStateNumber(const std::wstring& value, unsigned long fallback = 0) {
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
    return end && *end == L'\0' ? parsed : fallback;
}

std::wstring SerializeResumeProduct(const InstalledProduct& product) {
    std::wostringstream line;
    line << (product.selected ? 1 : 0) << L"|" << (product.msi ? 1 : 0) << L"|"
         << static_cast<int>(product.risk) << L"|" << static_cast<int>(product.hive) << L"|"
         << product.registryView << L"|" << HexEncode(product.displayName) << L"|"
         << HexEncode(product.version) << L"|" << HexEncode(product.publisher) << L"|"
         << HexEncode(product.architecture) << L"|" << HexEncode(product.uninstallString) << L"|"
         << HexEncode(product.quietUninstallString) << L"|" << HexEncode(product.installLocation) << L"|"
         << HexEncode(product.registryKey) << L"|" << HexEncode(product.productCode);
    return line.str();
}

bool DeserializeResumeProduct(const std::wstring& value, InstalledProduct* product) {
    if (!product) return false;
    const auto fields = SplitStateFields(value);
    if (fields.size() != 14) return false;
    product->selected = fields[0] == L"1";
    product->msi = fields[1] == L"1";
    product->risk = static_cast<RiskLevel>(ParseStateNumber(fields[2]));
    product->hive = static_cast<RegistryHive>(ParseStateNumber(fields[3]));
    product->registryView = static_cast<REGSAM>(ParseStateNumber(fields[4]));
    product->displayName = HexDecode(fields[5]);
    product->version = HexDecode(fields[6]);
    product->publisher = HexDecode(fields[7]);
    product->architecture = HexDecode(fields[8]);
    product->uninstallString = HexDecode(fields[9]);
    product->quietUninstallString = HexDecode(fields[10]);
    product->installLocation = HexDecode(fields[11]);
    product->registryKey = HexDecode(fields[12]);
    product->productCode = HexDecode(fields[13]);
    return !product->displayName.empty() && IsCryptoProPublisher(product->publisher);
}

std::wstring SerializeResumeTarget(const CleanupTarget& target) {
    std::wostringstream line;
    line << static_cast<int>(target.type) << L"|" << (target.verified ? 1 : 0) << L"|"
         << (target.protectedItem ? 1 : 0) << L"|" << static_cast<int>(target.registry.hive) << L"|"
         << target.registry.view << L"|" << HexEncode(target.displayName) << L"|"
         << HexEncode(target.path) << L"|" << HexEncode(target.reason) << L"|"
         << HexEncode(target.registry.subkey);
    return line.str();
}

bool DeserializeResumeTarget(const std::wstring& value, CleanupTarget* target) {
    if (!target) return false;
    const auto fields = SplitStateFields(value);
    if (fields.size() != 9) return false;
    target->type = static_cast<TargetType>(ParseStateNumber(fields[0]));
    switch (target->type) {
        case TargetType::Directory: target->category = CleanupTargetCategory::Directory; break;
        case TargetType::RegistryTree: target->category = CleanupTargetCategory::Registry; break;
        case TargetType::Service: target->category = CleanupTargetCategory::Service; break;
        case TargetType::DriverService: target->category = CleanupTargetCategory::Driver; break;
        case TargetType::DriverPackage: target->category = CleanupTargetCategory::DriverPackage; break;
        case TargetType::ScheduledTask: target->category = CleanupTargetCategory::ScheduledTask; break;
        case TargetType::Shortcut: target->category = CleanupTargetCategory::Shortcut; break;
        default: target->category = CleanupTargetCategory::File; break;
    }
    target->verified = fields[1] == L"1";
    target->protectedItem = fields[2] == L"1";
    target->registry.hive = static_cast<RegistryHive>(ParseStateNumber(fields[3]));
    target->registry.view = static_cast<REGSAM>(ParseStateNumber(fields[4]));
    target->displayName = HexDecode(fields[5]);
    target->path = HexDecode(fields[6]);
    target->reason = HexDecode(fields[7]);
    target->registry.subkey = HexDecode(fields[8]);
    if (static_cast<unsigned long>(target->type) > static_cast<unsigned long>(TargetType::Shortcut) ||
        static_cast<unsigned long>(target->registry.hive) > static_cast<unsigned long>(RegistryHive::Users)) return false;
    if (!target->verified || target->protectedItem || IsProtectedPath(target->path) ||
        IsProtectedRegistryPath(target->registry.subkey)) return false;
    return !target->displayName.empty() || !target->path.empty() || !target->registry.subkey.empty();
}

bool ResumeRunnerVersionMatches(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return false;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        !info || infoSize < sizeof(VS_FIXEDFILEINFO)) return false;
    return HIWORD(info->dwFileVersionMS) == 0 && LOWORD(info->dwFileVersionMS) == 5 &&
           HIWORD(info->dwFileVersionLS) == 3 && LOWORD(info->dwFileVersionLS) == 0;
}

std::wstring ResumeRunnerArchitecture(DWORD binaryType) {
    return binaryType == SCS_64BIT_BINARY ? L"x64" :
           binaryType == SCS_32BIT_BINARY ? L"x86" : L"unknown";
}

std::wstring FingerprintProducts(const CleanupPlan& plan) {
    std::vector<std::wstring> identities;
    for (const auto& product : plan.products)
        if (product.selected) identities.push_back(ToLower(product.displayName + L"|" + product.version));
    std::sort(identities.begin(), identities.end());
    std::wstring combined;
    for (const auto& identity : identities) combined += identity + L"\n";
    return Sha256Hex(combined);
}

std::wstring FingerprintProfiles(const CleanupPlan& plan) {
    std::vector<std::wstring> identities;
    for (const auto& profile : plan.profiles)
        if (profile.selected) identities.push_back(ToLower(profile.sid));
    std::sort(identities.begin(), identities.end());
    std::wstring combined;
    for (const auto& identity : identities) combined += identity + L"\n";
    return Sha256Hex(combined);
}

bool UpdateResumeStateValue(const std::wstring& statePath, const std::wstring& key,
                            const std::wstring& value, std::wstring* error) {
    std::wstring text = ReadTextBestEffort(statePath, 1024 * 1024);
    if (text.empty()) { if (error) *error = L"Resume state is empty or unreadable."; return false; }
    const std::wstring prefix = key + L"=";
    std::wistringstream input(text);
    std::wostringstream output;
    std::wstring line;
    bool replaced = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) { output << prefix << value << L"\r\n"; replaced = true; }
        else output << line << L"\r\n";
    }
    if (!replaced) output << prefix << value << L"\r\n";
    return WriteUtf8File(statePath, Utf8(output.str()), error);
}

}  // namespace

std::wstring BuildResumeCommand(const std::wstring& runnerPath, const std::wstring& token) {
    if (runnerPath.empty() || !IsGuid(token)) return {};
    return L"\"" + runnerPath + L"\" --resume \"" + token + L"\"";
}

std::wstring ComputeFileSha256(const std::wstring& path, std::wstring* error) {
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) { if (error) *error = GetLastErrorMessage(GetLastError()); return {}; }
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (error) *error = GetLastErrorMessage(GetLastError());
        if (provider) CryptReleaseContext(provider, 0);
        return {};
    }
    std::array<BYTE, 64 * 1024> buffer{};
    DWORD read = 0;
    bool ok = true;
    for (;;) {
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (!read) break;
        if (!CryptHashData(hash, buffer.data(), read, 0)) { ok = false; break; }
    }
    std::array<BYTE, 32> digest{};
    DWORD digestSize = static_cast<DWORD>(digest.size());
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestSize, 0) != FALSE;
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) { if (error) *error = GetLastErrorMessage(GetLastError()); return {}; }
    std::wostringstream output;
    for (DWORD index = 0; index < digestSize; ++index)
        output << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(digest[index]);
    return output.str();
}

bool IsUsableResumeRunner(const std::wstring& runnerPath, std::wstring* error) {
    const std::wstring name = ToLower(FileName(runnerPath));
    DWORD binaryType = 0;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (runnerPath.empty() || !FileExists(runnerPath) || name != L"cryptoprocleanupresume.exe" ||
        !GetBinaryTypeW(runnerPath.c_str(), &binaryType) ||
        (binaryType != SCS_32BIT_BINARY && binaryType != SCS_64BIT_BINARY)) {
        if (error) *error = L"The resume runner is missing, has an unexpected name, or is not a native executable.";
        return false;
    }
    if (!GetFileAttributesExW(runnerPath.c_str(), GetFileExInfoStandard, &attributes)) {
        if (error) *error = GetLastErrorMessage(GetLastError());
        return false;
    }
    ULARGE_INTEGER fileSize{};
    fileSize.HighPart = attributes.nFileSizeHigh;
    fileSize.LowPart = attributes.nFileSizeLow;
    if (fileSize.QuadPart < 64 * 1024 || fileSize.QuadPart > 32ull * 1024 * 1024 ||
        !ResumeRunnerVersionMatches(runnerPath)) {
        if (error) *error = L"The resume runner has an unexpected size or version.";
        return false;
    }
    if (ComputeFileSha256(runnerPath, error).empty()) return false;
    return true;
}

bool PrepareResume(const CleanupPlan& plan, const std::wstring& reportFolder,
                   std::wstring* token, std::wstring* error) {
    const std::wstring runner = JoinPath(ParentPath(CurrentExecutablePath()), L"CryptoProCleanupResume.exe");
    return PrepareResumeWithRunner(plan, reportFolder, runner, DetectLanguage(),
                                   token, error, true, {});
}

bool PrepareResumeWithRunner(const CleanupPlan& plan, const std::wstring& reportFolder,
                             const std::wstring& runnerPath, Language language,
                             std::wstring* token, std::wstring* error,
                              bool registerRunOnce,
                              const std::wstring& sessionsRootOverride) {
    ResumeAuthorization authorization;
    return PrepareResumeAuthorized(plan, reportFolder, runnerPath, language, authorization,
                                   token, error, registerRunOnce, sessionsRootOverride);
}

bool PrepareResumeAuthorized(const CleanupPlan& plan, const std::wstring& reportFolder,
                             const std::wstring& runnerPath, Language language,
                             const ResumeAuthorization& authorization,
                             std::wstring* token, std::wstring* error,
                             bool registerRunOnce,
                             const std::wstring& sessionsRootOverride) {
    if (!authorization.AllowsResidualPass()) {
        if (error) *error = L"The residual cleanup was not explicitly authorized.";
        return false;
    }
    if (!IsUsableResumeRunner(runnerPath, error)) return false;
    const bool productionState = sessionsRootOverride.empty();
    if (productionState && ToLower(CanonicalPath(ParentPath(runnerPath))) !=
                           ToLower(CanonicalPath(ParentPath(CurrentExecutablePath())))) {
        if (error) *error = L"The resume runner must be located next to the Modern executable.";
        return false;
    }
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) { if (error) *error = L"Could not generate a resume token."; return false; }
    std::array<wchar_t, 64> guidText{};
    if (!StringFromGUID2(guid, guidText.data(), static_cast<int>(guidText.size()))) {
        if (error) *error = L"Could not format a resume token.";
        return false;
    }
    const std::wstring generated = guidText.data();
    const std::wstring root = ResumeRoot(sessionsRootOverride);
    const std::wstring session = ResumeSessionPath(generated, sessionsRootOverride);
    const std::wstring runnerName = FileName(runnerPath);
    const std::wstring runner = JoinPath(session, runnerName);
    const std::wstring stateFile = JoinPath(session, L"state.ini");
    const std::wstring runOnceName = L"CryptoProCleanup-" + generated;
    bool runOnceWritten = false;
    auto rollback = [&]() {
        if (runOnceWritten) {
            RegKey runOnce;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0,
                              KEY_SET_VALUE | KEY_WOW64_64KEY, runOnce.put()) == ERROR_SUCCESS)
                RegDeleteValueW(runOnce.get(), runOnceName.c_str());
        }
        DeleteFileW(stateFile.c_str());
        DeleteFileW(runner.c_str());
        RemoveDirectoryW(session.c_str());
    };

    if (!EnsureDirectory(root, error) || !EnsureDirectory(session, error)) { rollback(); return false; }
    if (productionState && (!ApplyPrivateAcl(root, true, error) || !ApplyPrivateAcl(session, true, error))) {
        rollback();
        return false;
    }
    if (!CopyFileW(runnerPath.c_str(), runner.c_str(), FALSE)) {
        if (error) *error = GetLastErrorMessage(GetLastError());
        rollback();
        return false;
    }
    if ((productionState && !ApplyPrivateAcl(runner, false, error)) || !IsUsableResumeRunner(runner, error)) {
        rollback();
        return false;
    }
    const std::wstring sourceHash = ComputeFileSha256(runnerPath, error);
    const std::wstring copiedHash = ComputeFileSha256(runner, error);
    if (sourceHash.empty() || copiedHash.empty() || sourceHash != copiedHash) {
        if (error && error->empty()) *error = L"The copied resume runner hash does not match its source.";
        rollback();
        return false;
    }
    DWORD binaryType = 0;
    WIN32_FILE_ATTRIBUTE_DATA runnerAttributes{};
    if (!GetBinaryTypeW(runner.c_str(), &binaryType) ||
        !GetFileAttributesExW(runner.c_str(), GetFileExInfoStandard, &runnerAttributes)) {
        if (error) *error = L"The copied resume runner metadata could not be read.";
        rollback();
        return false;
    }
    ULARGE_INTEGER runnerSize{};
    runnerSize.HighPart = runnerAttributes.nFileSizeHigh;
    runnerSize.LowPart = runnerAttributes.nFileSizeLow;
    std::wostringstream state;
    state << L"version=3\r\n" << L"token=" << generated << L"\r\n"
           << L"report=" << HexEncode(reportFolder) << L"\r\n"
           << L"language=" << (language == Language::Russian ? L"ru" : L"en") << L"\r\n"
           << L"mode=" << (authorization.mode == ResumeMode::ForcedResidual ? L"forced" : L"deferred") << L"\r\n"
           << L"forceAuthorized=" << (authorization.forceAuthorized ? 1 : 0) << L"\r\n"
           << L"residualCleanupDeferred=" << (authorization.residualCleanupDeferred ? 1 : 0) << L"\r\n"
           << L"uninstallerFailurePresent=" << (authorization.uninstallerFailurePresent ? 1 : 0) << L"\r\n"
           << L"attempts=0\r\nstatus=pending\r\n"
           << L"runner=" << HexEncode(runnerName) << L"\r\n"
           << L"runnerVersion=" << HexEncode(kVersion) << L"\r\n"
           << L"runnerArchitecture=" << ResumeRunnerArchitecture(binaryType) << L"\r\n"
           << L"runnerSize=" << runnerSize.QuadPart << L"\r\n"
           << L"runnerSha256=" << copiedHash << L"\r\n"
           << L"productsFingerprint=" << FingerprintProducts(plan) << L"\r\n"
           << L"profilesFingerprint=" << FingerprintProfiles(plan) << L"\r\n"
           << L"all=" << (plan.allDetectedProductsSelected ? 1 : 0) << L"\r\n";
    for (const auto& product : plan.products) {
        state << L"product2=" << SerializeResumeProduct(product) << L"\r\n";
        if (product.selected) state << L"product=" << Sha256Hex(ToLower(product.displayName + L"|" + product.version)) << L"\r\n";
    }
    for (const auto& profile : plan.profiles) {
        if (profile.selected) state << L"profile=" << Sha256Hex(profile.sid) << L"\r\n";
    }
    for (const auto& target : plan.targets)
        state << L"target2=" << SerializeResumeTarget(target) << L"\r\n";
    for (const auto& item : plan.protectedItems)
        state << L"protected2=" << HexEncode(item) << L"\r\n";
    if (!WriteUtf8File(stateFile, Utf8(state.str()), error) ||
        (productionState && !ApplyPrivateAcl(stateFile, false, error))) {
        rollback();
        return false;
    }

    if (!registerRunOnce) {
        if (token) *token = generated;
        return true;
    }

    RegKey runOnce;
    const LONG open = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0, nullptr, 0,
                                      KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, runOnce.put(), nullptr);
    if (open != ERROR_SUCCESS) { if (error) *error = GetLastErrorMessage(open); rollback(); return false; }
    const std::wstring command = BuildResumeCommand(runner, generated);
    if (command.empty()) { if (error) *error = L"The RunOnce command could not be built."; rollback(); return false; }
    const LONG written = RegSetValueExW(runOnce.get(), runOnceName.c_str(), 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (written != ERROR_SUCCESS) { if (error) *error = GetLastErrorMessage(written); rollback(); return false; }
    runOnceWritten = true;
    if (token) *token = generated;
    return true;
}

bool LoadResumePlan(const std::wstring& token, ScanResult* scan, CleanupPlan* plan,
                    std::wstring* reportFolder, std::wstring* error) {
    return LoadResumePlan(token, scan, plan, reportFolder, nullptr, error, {});
}

bool LoadResumePlan(const std::wstring& token, ScanResult* scan, CleanupPlan* plan,
                    std::wstring* reportFolder, Language* language, std::wstring* error,
                    const std::wstring& sessionsRootOverride) {
    return LoadResumePlanAuthorized(token, scan, plan, reportFolder, language, nullptr,
                                    error, sessionsRootOverride);
}

bool LoadResumePlanAuthorized(const std::wstring& token, ScanResult* scan, CleanupPlan* plan,
                              std::wstring* reportFolder, Language* language,
                              ResumeAuthorization* authorization, std::wstring* error,
                              const std::wstring& sessionsRootOverride) {
    if (!scan || !plan || !IsGuid(token)) { if (error) *error = L"Invalid resume token."; return false; }
    std::multimap<std::wstring, std::wstring> values;
    const std::wstring session = ResumeSessionPath(token, sessionsRootOverride);
    if (!ReadStateLines(JoinPath(session, L"state.ini"), &values, error)) return false;
    const auto storedToken = values.find(L"token");
    if (storedToken == values.end() || ToLower(storedToken->second) != ToLower(token)) { if (error) *error = L"Resume token mismatch."; return false; }
    const auto version = values.find(L"version");
    if (version == values.end() || version->second != L"3") {
        if (error) *error = L"Unsupported or unauthenticated resume-state version.";
        return false;
    }
    ResumeAuthorization storedAuthorization;
    const auto mode = values.find(L"mode");
    storedAuthorization.mode = mode != values.end() && mode->second == L"forced" ?
                               ResumeMode::ForcedResidual : ResumeMode::DeferredResidual;
    const auto forceAuthorized = values.find(L"forceAuthorized");
    const auto residualDeferred = values.find(L"residualCleanupDeferred");
    const auto uninstallerFailure = values.find(L"uninstallerFailurePresent");
    const auto attempts = values.find(L"attempts");
    storedAuthorization.forceAuthorized = forceAuthorized != values.end() && forceAuthorized->second == L"1";
    storedAuthorization.residualCleanupDeferred = residualDeferred != values.end() && residualDeferred->second == L"1";
    storedAuthorization.uninstallerFailurePresent = uninstallerFailure != values.end() && uninstallerFailure->second == L"1";
    storedAuthorization.attempts = attempts == values.end() ? 0 : ParseStateNumber(attempts->second, 99);
    if (!storedAuthorization.AllowsResidualPass() || storedAuthorization.attempts >= 3 ||
        (storedAuthorization.mode == ResumeMode::ForcedResidual && !storedAuthorization.forceAuthorized)) {
        if (error) *error = storedAuthorization.attempts >= 3 ?
            L"The resume retry limit has been reached." : L"The protected state does not authorize residual cleanup.";
        return false;
    }
    if (authorization) *authorization = storedAuthorization;

    const auto runnerValue = values.find(L"runner");
    const auto runnerVersion = values.find(L"runnerVersion");
    const auto runnerArchitecture = values.find(L"runnerArchitecture");
    const auto runnerSizeValue = values.find(L"runnerSize");
    const auto runnerHash = values.find(L"runnerSha256");
    if (runnerValue == values.end() || runnerVersion == values.end() ||
        runnerArchitecture == values.end() || runnerSizeValue == values.end() || runnerHash == values.end()) {
        if (error) *error = L"The resume runner identity is incomplete.";
        return false;
    }
    const std::wstring runnerName = FileName(HexDecode(runnerValue->second));
    const std::wstring runner = JoinPath(session, runnerName);
    DWORD binaryType = 0;
    WIN32_FILE_ATTRIBUTE_DATA runnerAttributes{};
    ULARGE_INTEGER runnerSize{};
    if (!IsUsableResumeRunner(runner, error) || !GetBinaryTypeW(runner.c_str(), &binaryType) ||
        !GetFileAttributesExW(runner.c_str(), GetFileExInfoStandard, &runnerAttributes)) return false;
    runnerSize.HighPart = runnerAttributes.nFileSizeHigh;
    runnerSize.LowPart = runnerAttributes.nFileSizeLow;
    if (HexDecode(runnerVersion->second) != kVersion ||
        runnerArchitecture->second != ResumeRunnerArchitecture(binaryType) ||
        ParseStateNumber(runnerSizeValue->second, 0) != runnerSize.QuadPart ||
        ToLower(runnerHash->second) != ToLower(ComputeFileSha256(runner, error))) {
        if (error && error->empty()) *error = L"The resume runner identity does not match the protected state.";
        return false;
    }
    const auto report = values.find(L"report");
    if (report != values.end() && reportFolder) *reportFolder = HexDecode(report->second);
    std::set<std::wstring> productHashes, profileHashes;
    const auto productRange = values.equal_range(L"product");
    for (auto item = productRange.first; item != productRange.second; ++item) productHashes.insert(item->second);
    const auto profileRange = values.equal_range(L"profile");
    for (auto item = profileRange.first; item != profileRange.second; ++item) profileHashes.insert(item->second);
    const auto storedLanguage = values.find(L"language");
    const Language resumeLanguage = storedLanguage != values.end() && storedLanguage->second == L"ru" ?
                                    Language::Russian : Language::English;
    if (language) *language = resumeLanguage;
    *plan = {};
    const auto all = values.find(L"all");
    const bool originalAll = all != values.end() && all->second == L"1";
    plan->allDetectedProductsSelected = originalAll;
    {
        const auto product2 = values.equal_range(L"product2");
        for (auto item = product2.first; item != product2.second; ++item) {
            InstalledProduct product;
            if (DeserializeResumeProduct(item->second, &product)) plan->products.push_back(std::move(product));
        }
        const auto target2 = values.equal_range(L"target2");
        for (auto item = target2.first; item != target2.second; ++item) {
            CleanupTarget target;
            if (DeserializeResumeTarget(item->second, &target)) plan->targets.push_back(std::move(target));
        }
        const auto protected2 = values.equal_range(L"protected2");
        for (auto item = protected2.first; item != protected2.second; ++item) {
            const std::wstring value = HexDecode(item->second);
            if (!value.empty()) plan->protectedItems.push_back(value);
        }
        // Tests use an isolated sessions root so loading a protected state never
        // needs to inspect the developer machine's HKLM. Production resumes do
        // perform a fresh, read-only scan for an accurate final report.
        if (sessionsRootOverride.empty()) {
            *scan = ScanSystem(resumeLanguage);
            for (auto& product : scan->products)
                product.selected = productHashes.count(Sha256Hex(ToLower(product.displayName + L"|" + product.version))) != 0;
            for (auto& profile : scan->profiles)
                profile.selected = profileHashes.count(Sha256Hex(profile.sid)) != 0;
            plan->profiles = scan->profiles;
        } else {
            scan->products = plan->products;
        }
        const auto productsFingerprint = values.find(L"productsFingerprint");
        const auto profilesFingerprint = values.find(L"profilesFingerprint");
        if (productsFingerprint == values.end() || profilesFingerprint == values.end() ||
            productsFingerprint->second != FingerprintProducts(*plan) ||
            profilesFingerprint->second != FingerprintProfiles(*plan)) {
            if (error) *error = L"The selected product or profile fingerprint does not match the protected state.";
            return false;
        }
        if (plan->targets.empty() && error) *error = L"The protected resume plan contains no verified targets.";
        return !plan->targets.empty();
    }
}

bool CompleteResume(const std::wstring& token, std::wstring* error) {
    return CompleteResume(token, error, true, {});
}

bool CompleteResume(const std::wstring& token, std::wstring* error,
                    bool removeRunOnce, const std::wstring& sessionsRootOverride) {
    if (!IsGuid(token)) { if (error) *error = L"Invalid resume token."; return false; }
    bool success = true;
    auto fail = [&](const std::wstring& message) {
        success = false;
        if (error && error->empty()) *error = message;
    };
    if (removeRunOnce) {
        RegKey runOnce;
        const LONG opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0,
                                          KEY_SET_VALUE | KEY_WOW64_64KEY, runOnce.put());
        if (opened == ERROR_SUCCESS) {
            const LONG deleted = RegDeleteValueW(runOnce.get(), (L"CryptoProCleanup-" + token).c_str());
            if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) fail(GetLastErrorMessage(deleted));
        } else if (opened != ERROR_FILE_NOT_FOUND) fail(GetLastErrorMessage(opened));
    }
    const std::wstring session = ResumeSessionPath(token, sessionsRootOverride);
    if (!PathStartsWith(session, ResumeRoot(sessionsRootOverride))) {
        if (error) *error = L"The resume session path is outside the protected root.";
        return false;
    }
    const std::wstring state = JoinPath(session, L"state.ini");
    std::wstring runnerName;
    std::multimap<std::wstring, std::wstring> values;
    if (ReadStateLines(state, &values, nullptr)) {
        const auto runner = values.find(L"runner");
        if (runner != values.end()) runnerName = FileName(HexDecode(runner->second));
    } else fail(L"The resume state could not be read during final cleanup.");
    const std::wstring executable = CurrentExecutablePath();
    if (!runnerName.empty()) {
        const std::wstring runner = JoinPath(session, runnerName);
        if (ToLower(CanonicalPath(runner)) == ToLower(CanonicalPath(executable))) {
            if (!ScheduleDelete(runner, nullptr)) fail(GetLastErrorMessage(GetLastError()));
        } else if (FileExists(runner) && !DeleteFileW(runner.c_str()) && !ScheduleDelete(runner, nullptr)) {
            fail(GetLastErrorMessage(GetLastError()));
        }
    }
    if (FileExists(state) && !DeleteFileW(state.c_str()) && !ScheduleDelete(state, nullptr))
        fail(GetLastErrorMessage(GetLastError()));
    if (!RemoveDirectoryW(session.c_str())) {
        const DWORD removeError = GetLastError();
        if (removeError != ERROR_FILE_NOT_FOUND && !ScheduleDelete(session, nullptr)) fail(GetLastErrorMessage(removeError));
    }
    return success;
}

int RunResumeCommand(const std::wstring& token, bool showResult, const ProgressCallback& progress) {
    ScanResult scan;
    CleanupPlan plan;
    std::wstring reportFolder;
    std::wstring error;
    Language language = DetectLanguage();
    ResumeAuthorization authorization;
    if (!LoadResumePlanAuthorized(token, &scan, &plan, &reportFolder, &language, &authorization, &error, {})) {
        if (showResult) MessageBoxW(nullptr, error.c_str(),
            Tr(language, L"Продолжение невозможно", L"Cannot resume").c_str(), MB_OK | MB_ICONERROR);
        return 2;
    }
    const std::wstring statePath = JoinPath(ResumeSessionPath(token), L"state.ini");
    const unsigned long currentAttempt = authorization.attempts + 1;
    if (!UpdateResumeStateValue(statePath, L"attempts", std::to_wstring(currentAttempt), &error) ||
        !UpdateResumeStateValue(statePath, L"status", L"running", &error)) {
        if (showResult) MessageBoxW(nullptr, error.c_str(), L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
        return 3;
    }
    ExecutionResult execution;
    ScanResult verification;
    try {
        // Loading version 3 state has already verified that this residual pass
        // was authorized either by a clean reboot deferral or explicit FORCE.
        execution = ExecuteCleanup(plan, true, progress);
        verification = VerifyAfterCleanup(language, progress);
    } catch (...) {
        UpdateResumeStateValue(statePath, L"status", L"failed", nullptr);
        UpdateResumeStateValue(statePath, L"lastError", HexEncode(L"Unhandled exception during resume."), nullptr);
        if (showResult) MessageBoxW(nullptr,
            Tr(language, L"Продолжение завершилось аварийно. Состояние сохранено для ручного повтора.",
                         L"Resume failed unexpectedly. State was retained for a manual retry.").c_str(),
            L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
        return 3;
    }
    if (reportFolder.empty()) {
        error = L"The resume report folder is missing.";
        if (showResult) MessageBoxW(nullptr, error.c_str(), L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
        UpdateResumeStateValue(statePath, L"status", L"failed", nullptr);
        UpdateResumeStateValue(statePath, L"lastError", HexEncode(error), nullptr);
        return 3;
    }
    const std::wstring reportPath = JoinPath(reportFolder, L"report.json");
    const std::wstring summaryPath = JoinPath(reportFolder, L"summary.txt");
    const std::wstring logPath = JoinPath(reportFolder, L"cleanup.log");
    bool reportsWritten = WriteJsonReport(reportPath, scan, &plan, &execution, &verification, &error) &&
                          WriteTextSummary(language, summaryPath, scan, &plan, &execution, &verification, &error);
    for (const auto& operation : execution.operations) {
        const std::wstring line = L"[resume] " + operation.action + L": " + operation.target + L" — " + operation.message;
        reportsWritten = AppendLog(logPath, RedactSensitiveText(line, scan)) && reportsWritten;
    }
    bool partial = execution.anyFailure || execution.rebootRequired || !verification.products.empty() ||
                   !verification.warnings.empty() || !reportsWritten;
    if (partial) {
        UpdateResumeStateValue(statePath, L"status", L"failed", nullptr);
        UpdateResumeStateValue(statePath, L"lastError", HexEncode(error.empty() ? L"Residual cleanup remains partial." : error), nullptr);
    } else {
        UpdateResumeStateValue(statePath, L"status", L"completed", nullptr);
        std::wstring cleanupError;
        if (!CompleteResume(token, &cleanupError)) {
            partial = true;
            error = cleanupError;
        }
    }
    if (showResult) {
        const std::wstring message = partial ?
            Tr(language, L"Продолжение завершено частично. Состояние сохранено для ручного повтора; проверьте обезличенные отчёты.",
                         L"Resume completed partially. State was retained for a manual retry; review the redacted reports.") :
            Tr(language, L"Остаточная очистка после перезагрузки завершена.",
                         L"Post-restart residual cleanup completed.");
        MessageBoxW(nullptr, (message + L"\r\n\r\n" + reportFolder).c_str(),
                    Tr(language, L"Результат продолжения", L"Resume result").c_str(),
                    MB_OK | (partial ? MB_ICONWARNING : MB_ICONINFORMATION));
    }
    return partial ? 1 : 0;
}

CommandLineOptions ParseCommandLine(int argc, wchar_t** argv) {
    CommandLineOptions options;
    options.language = DetectLanguage();
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = ToLower(argv[index]);
        if (argument == L"--scan") options.scanOnly = true;
        else if (argument == L"--offline-scan" && index + 1 < argc) options.offlineWindowsPath = argv[++index];
        else if (argument == L"--help" || argument == L"-h" || argument == L"/?") options.showHelp = true;
        else if (argument == L"--report" && index + 1 < argc) options.reportPath = argv[++index];
        else if (argument == L"--resume" && index + 1 < argc) options.resumeToken = argv[++index];
        else if (argument == L"--lang" && index + 1 < argc) {
            const std::wstring language = ToLower(argv[++index]);
            if (language == L"ru") { options.language = Language::Russian; options.languageExplicit = true; }
            else if (language == L"en") { options.language = Language::English; options.languageExplicit = true; }
        }
    }
    return options;
}

int RunScanCommand(const CommandLineOptions& options) {
    ScanResult scan = ScanSystem(options.language);
    CleanupPlan plan = BuildCleanupPlan(scan);
    std::wstring report = options.reportPath;
    if (report.empty()) {
        std::vector<wchar_t> current(32768, L'\0');
        GetCurrentDirectoryW(static_cast<DWORD>(current.size()), current.data());
        report = JoinPath(current.data(), L"CryptoProCleanup-scan-" + Timestamp() + L".json");
    }
    std::wstring error;
    if (!WriteJsonReport(report, scan, &plan, nullptr, nullptr, &error)) {
        MessageBoxW(nullptr, error.c_str(), L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
        return 2;
    }
    return 0;
}

int RunOfflineScanCommand(const CommandLineOptions& options) {
    const OfflineScanResult offline = ScanOfflineWindows(options.language, options.offlineWindowsPath);
    std::wostringstream text;
    text << L"CryptoPro Cleanup Utility " << kVersion << L"\r\n"
         << L"Offline scan diagnostics / Диагностика офлайн-сканирования\r\n\r\n";
    for (const auto& line : offline.diagnostics) text << line << L"\r\n";
    if (!offline.scan.warnings.empty()) {
        text << L"\r\nWarnings / Предупреждения:\r\n";
        for (const auto& warning : offline.scan.warnings) text << L"- " << warning << L"\r\n";
    }
    std::wstring report = options.reportPath;
    if (report.empty()) {
        std::vector<wchar_t> current(32768, L'\0');
        GetCurrentDirectoryW(static_cast<DWORD>(current.size()), current.data());
        report = JoinPath(current.data(), L"CryptoProCleanup-offline-diagnostic.txt");
    }
    std::wstring error;
    if (!WriteUtf8File(report, Utf8(text.str()), &error)) {
        MessageBoxW(nullptr, error.c_str(), L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
        return 2;
    }
    return offline.valid ? 0 : 1;
}

}  // namespace cpc
