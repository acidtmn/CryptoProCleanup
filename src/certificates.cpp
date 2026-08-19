#include "cleanup.hpp"

#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace cpc {
namespace {

class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY key) : key_(key) {}
    ~RegKey() { if (key_) RegCloseKey(key_); }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    HKEY get() const { return key_; }
    HKEY* put() { if (key_) RegCloseKey(key_); key_ = nullptr; return &key_; }
private:
    HKEY key_ = nullptr;
};

class CertStore {
public:
    explicit CertStore(HCERTSTORE store = nullptr) : store_(store) {}
    ~CertStore() { if (store_) CertCloseStore(store_, 0); }
    CertStore(const CertStore&) = delete;
    CertStore& operator=(const CertStore&) = delete;
    HCERTSTORE get() const { return store_; }
    explicit operator bool() const { return store_ != nullptr; }
private:
    HCERTSTORE store_ = nullptr;
};

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    return (left.back() == L'\\' || left.back() == L'/') ? left + right : left + L"\\" + right;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring CanonicalPath(std::wstring path) {
    path = Trim(ExpandEnvironment(path));
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (size && size < buffer.size()) path.assign(buffer.data(), size);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::wstring CertificateName(PCCERT_CONTEXT context, DWORD flags) {
    DWORD size = CertGetNameStringW(context, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, nullptr, 0);
    if (size > 1) {
        std::vector<wchar_t> value(size, L'\0');
        if (CertGetNameStringW(context, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr,
                               value.data(), static_cast<DWORD>(value.size())) > 1) return value.data();
    }
    CERT_NAME_BLOB name = flags & CERT_NAME_ISSUER_FLAG ? context->pCertInfo->Issuer : context->pCertInfo->Subject;
    size = CertNameToStrW(context->dwCertEncodingType, &name, CERT_X500_NAME_STR, nullptr, 0);
    if (size > 1) {
        std::vector<wchar_t> value(size, L'\0');
        if (CertNameToStrW(context->dwCertEncodingType, &name, CERT_X500_NAME_STR,
                           value.data(), static_cast<DWORD>(value.size())) > 1) return value.data();
    }
    return L"<unknown>";
}

std::wstring FormatFileTime(const FILETIME& value) {
    FILETIME local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&value, &local) || !FileTimeToSystemTime(&local, &time)) return {};
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02u.%02u.%04u", time.wDay, time.wMonth, time.wYear);
    return buffer;
}

std::wstring Thumbprint(PCCERT_CONTEXT context) {
    std::array<BYTE, 64> hash{};
    DWORD size = static_cast<DWORD>(hash.size());
    if (!CertGetCertificateContextProperty(context, CERT_SHA1_HASH_PROP_ID, hash.data(), &size)) return {};
    std::wostringstream out;
    for (DWORD index = 0; index < size; ++index) {
        out << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0')
            << static_cast<unsigned>(hash[index]);
    }
    return out.str();
}

std::wstring SingleLine(std::wstring value) {
    for (wchar_t& character : value) {
        if (character < 0x20 || character == 0x7f) character = L' ';
    }
    return Trim(value);
}

void EnumerateCertificateStore(HCERTSTORE store, const UserProfile& profile,
                               std::vector<CertificateEntry>* certificates,
                               std::unordered_set<std::wstring>* seen) {
    PCCERT_CONTEXT context = nullptr;
    while ((context = CertEnumCertificatesInStore(store, context)) != nullptr) {
        const std::wstring thumbprint = Thumbprint(context);
        const std::wstring identity = ToLower(profile.sid + L"|" + thumbprint);
        if (thumbprint.empty() || !seen->insert(identity).second) continue;
        CertificateEntry item;
        item.profileSid = profile.sid;
        item.profileName = profile.displayName;
        item.subject = CertificateName(context, 0);
        item.issuer = CertificateName(context, CERT_NAME_ISSUER_FLAG);
        item.validFrom = FormatFileTime(context->pCertInfo->NotBefore);
        item.validTo = FormatFileTime(context->pCertInfo->NotAfter);
        item.thumbprint = thumbprint;
        item.encoded.assign(context->pbCertEncoded, context->pbCertEncoded + context->cbCertEncoded);
        DWORD propertySize = 0;
        item.hasPrivateKeyReference = CertGetCertificateContextProperty(
            context, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &propertySize) != FALSE;
        certificates->push_back(std::move(item));
    }
}

void ReadCertificateStore(HKEY profileRoot, const UserProfile& profile,
                          std::vector<CertificateEntry>* certificates,
                          std::unordered_set<std::wstring>* seen,
                          std::vector<std::wstring>* warnings) {
    RegKey registryStore;
    constexpr wchar_t storePath[] = L"SOFTWARE\\Microsoft\\SystemCertificates\\My";
    const LONG opened = RegOpenKeyExW(profileRoot, storePath, 0, KEY_READ, registryStore.put());
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) {
        if (warnings) warnings->push_back(L"Could not read the personal certificate store for profile: " + profile.displayName);
        return;
    }
    CertStore store(CertOpenStore(CERT_STORE_PROV_REG, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                  CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
                                  registryStore.get()));
    if (!store) {
        if (warnings) warnings->push_back(L"CryptoAPI could not open the personal certificate store for profile: " + profile.displayName);
        return;
    }
    EnumerateCertificateStore(store.get(), profile, certificates, seen);
}

bool WriteBinaryFile(const std::wstring& path, const BYTE* data, size_t size, std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastErrorMessage(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool validSize = size <= MAXDWORD;
    const BOOL success = validSize && WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    const DWORD code = success && written == size ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (code != ERROR_SUCCESS) {
        DeleteFileW(path.c_str());
        if (error) *error = GetLastErrorMessage(code ? code : ERROR_WRITE_FAULT);
        return false;
    }
    return true;
}

}  // namespace

void ScanUserCertificates(const std::vector<UserProfile>& profiles,
                          std::vector<CertificateEntry>* certificates,
                          std::vector<std::wstring>* warnings,
                          const ProgressCallback& progress) {
    if (!certificates) return;
    certificates->clear();
    std::unordered_set<std::wstring> seen;
    const std::wstring currentProfile = CanonicalPath(ExpandEnvironment(L"%USERPROFILE%"));
    for (size_t index = 0; index < profiles.size(); ++index) {
        const UserProfile& profile = profiles[index];
        if (progress) progress(L"Scanning public certificates: " + profile.displayName,
                               profiles.empty() ? 100 : static_cast<int>(((index + 1) * 100) / profiles.size()));
        RegKey profileRoot;
        if (!currentProfile.empty() && ToLower(CanonicalPath(profile.profilePath)) == ToLower(currentProfile)) {
            CertStore current(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                            CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG,
                                            L"MY"));
            if (current) EnumerateCertificateStore(current.get(), profile, certificates, &seen);
            else if (warnings) warnings->push_back(L"CryptoAPI could not open the current user's Personal certificate store.");
            continue;
        }
        if (profile.loaded) RegOpenKeyExW(HKEY_USERS, profile.sid.c_str(), 0, KEY_READ, profileRoot.put());
        if (!profileRoot.get()) {
            const std::wstring hive = JoinPath(profile.profilePath, L"NTUSER.DAT");
            if (!FileExists(hive) || RegLoadAppKeyW(hive.c_str(), profileRoot.put(), KEY_READ, 0, 0) != ERROR_SUCCESS) {
                if (warnings) warnings->push_back(L"Could not load certificate registry for profile: " + profile.displayName);
                continue;
            }
        }
        ReadCertificateStore(profileRoot.get(), profile, certificates, &seen, warnings);
    }
    std::stable_sort(certificates->begin(), certificates->end(), [](const CertificateEntry& left, const CertificateEntry& right) {
        if (ToLower(left.profileName) != ToLower(right.profileName)) return ToLower(left.profileName) < ToLower(right.profileName);
        if (ToLower(left.subject) != ToLower(right.subject)) return ToLower(left.subject) < ToLower(right.subject);
        return left.thumbprint < right.thumbprint;
    });
}

bool ExportPublicCertificates(Language language, const std::vector<CertificateEntry>& certificates,
                              const std::wstring& parentFolder, std::wstring* exportFolder,
                              std::wstring* error) {
    if (parentFolder.empty()) { if (error) *error = L"Certificate export folder is empty."; return false; }
    std::wstring folder = JoinPath(parentFolder, L"CryptoProCertificates-" + Timestamp());
    for (unsigned suffix = 1; GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 1000; ++suffix) {
        folder = JoinPath(parentFolder, L"CryptoProCertificates-" + Timestamp() + L"-" + std::to_wstring(suffix));
    }
    if (!EnsureDirectory(folder, error)) return false;

    CertStore bundle(CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr));
    if (!bundle) { if (error) *error = GetLastErrorMessage(GetLastError()); return false; }
    std::wostringstream catalog;
    catalog << (language == Language::Russian ? L"Экспорт открытых сертификатов" : L"Public certificate export") << L"\r\n"
            << L"WARNING / ВНИМАНИЕ: certificate names contain personal data. No private keys are included.\r\n"
            << L"Имена сертификатов содержат персональные данные. Закрытые ключи не включены.\r\n\r\n";
    size_t exported = 0;
    for (const auto& certificate : certificates) {
        if (!certificate.selected || certificate.encoded.empty()) continue;
        const std::wstring fileName = L"certificate-" + std::to_wstring(exported + 1) + L"-" + certificate.thumbprint + L".cer";
        const std::wstring path = JoinPath(folder, fileName);
        if (!WriteBinaryFile(path, certificate.encoded.data(), certificate.encoded.size(), error)) return false;
        if (certificate.encoded.size() > MAXDWORD) {
            if (error) *error = L"Encoded certificate is too large.";
            return false;
        }
        if (!CertAddEncodedCertificateToStore(bundle.get(), X509_ASN_ENCODING, certificate.encoded.data(),
                                              static_cast<DWORD>(certificate.encoded.size()),
                                              CERT_STORE_ADD_ALWAYS, nullptr)) {
            if (error) *error = GetLastErrorMessage(GetLastError());
            return false;
        }
        catalog << L"[" << (exported + 1) << L"] " << SingleLine(certificate.subject) << L"\r\n"
                << (language == Language::Russian ? L"Профиль: " : L"Profile: ") << SingleLine(certificate.profileName) << L"\r\n"
                << (language == Language::Russian ? L"Кем выдан: " : L"Issuer: ") << SingleLine(certificate.issuer) << L"\r\n"
                << (language == Language::Russian ? L"Действует: " : L"Valid: ") << certificate.validFrom << L" — " << certificate.validTo << L"\r\n"
                << L"SHA-1: " << certificate.thumbprint << L"\r\n"
                << (language == Language::Russian ? L"Ссылка на закрытый ключ: " : L"Private-key reference: ")
                << (certificate.hasPrivateKeyReference ? (language == Language::Russian ? L"есть (ключ не экспортирован)" : L"present (key not exported)")
                                                      : (language == Language::Russian ? L"не обнаружена" : L"not detected")) << L"\r\n"
                << (language == Language::Russian ? L"Файл: " : L"File: ") << fileName << L"\r\n\r\n";
        ++exported;
    }
    catalog << (language == Language::Russian ? L"Экспортировано сертификатов: " : L"Certificates exported: ") << exported << L"\r\n";
    if (!WriteUtf8File(JoinPath(folder, L"certificates.txt"), Utf8(catalog.str()), error)) return false;
    if (exported) {
        const std::wstring bundlePath = JoinPath(folder, L"certificates.p7b");
        if (!CertSaveStore(bundle.get(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, CERT_STORE_SAVE_AS_PKCS7,
                           CERT_STORE_SAVE_TO_FILENAME_W, const_cast<wchar_t*>(bundlePath.c_str()), 0)) {
            if (error) *error = GetLastErrorMessage(GetLastError());
            return false;
        }
    }
    if (exportFolder) *exportFolder = folder;
    return true;
}

}  // namespace cpc
