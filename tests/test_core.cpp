#include "../src/cleanup.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Expect(bool condition, const char* test) {
    if (!condition) { std::cerr << "FAILED: " << test << "\n"; ++failures; }
}

bool DeleteGeneratedTree(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path.c_str()) != FALSE;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(path.c_str()) != FALSE;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        bool success = true;
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            if (!DeleteGeneratedTree(path + L"\\" + name)) { success = false; break; }
        } while (FindNextFileW(search, &data));
        FindClose(search);
        if (!success) return false;
    }
    return RemoveDirectoryW(path.c_str()) != FALSE;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace cpc;
    Expect(IsCryptoProPublisher(L"Компания КриптоПро"), "Russian publisher");
    Expect(IsCryptoProPublisher(L"Crypto-Pro LLC"), "English publisher");
    Expect(IsCryptoProPublisher(L"CRYPTO PRO"), "Normalized publisher");
    Expect(!IsCryptoProPublisher(L"Microsoft Corporation"), "Reject unrelated publisher");
    Expect(!IsCryptoProPublisher(L"My Crypto Provider"), "Reject generic crypto name");
    Expect(IsCryptoProName(L"Crypto-Pro GOST R 34.10-2012 Cryptographic Service Provider"), "Provider name recognized");
    Expect(!IsCryptoProName(L"Microsoft Base Smart Card Crypto Provider"), "Microsoft crypto provider rejected");

    Expect(IsHighRiskProduct(L"КриптоПро NGate"), "NGate is high risk");
    Expect(IsHighRiskProduct(L"CryptoPro Winlogon"), "Winlogon is high risk");
    Expect(IsHighRiskProduct(L"КриптоАРМ ГОСТ"), "CryptoARM is high risk");
    Expect(!IsHighRiskProduct(L"КриптоПро ЭЦП Browser plug-in"), "Browser plug-in is normal risk");

    Expect(MaskLicense(L"1234567890") == L"12****7890", "Long license mask");
    Expect(MaskLicense(L"123456") == L"****56", "Short license mask");
    Expect(MaskLicense(L"123") == L"***", "Tiny license mask");
    Expect(MaskLicense(L"").empty(), "Empty license mask");

    Expect(IsGuid(L"{50F91F80-D397-437C-B0C8-62128DE3B55E}"), "Valid product GUID");
    Expect(!IsGuid(L"not-a-guid"), "Invalid product GUID");
    Expect(PackMsiProductCode(L"{50F91F80-D397-437C-B0C8-62128DE3B55E}") ==
               L"08F19F05793DC7340B8C2621D83E5BE5",
           "MSI packed product code");
    Expect(UnpackMsiProductCode(L"08F19F05793DC7340B8C2621D83E5BE5") ==
               L"{50F91F80-D397-437C-B0C8-62128DE3B55E}",
           "MSI packed product code round-trip");
    Expect(PackMsiProductCode(L"not-a-guid").empty(), "Invalid MSI product code rejected");
    Expect(UnpackMsiProductCode(L"not-a-packed-guid").empty(), "Invalid packed MSI code rejected");

    Expect(IsProtectedPath(L"C:\\ProgramData\\Crypto Pro\\Crypto\\keys"), "ProgramData key store protected");
    Expect(IsProtectedPath(L"C:\\Users\\alice\\AppData\\Local\\Crypto Pro"), "User key directory protected");
    Expect(!IsProtectedPath(L"C:\\Program Files\\Crypto Pro\\CSP"), "Program directory removable");
    Expect(IsProtectedRegistryPath(L"HKLM\\SOFTWARE\\Crypto Pro\\Settings\\Users\\S-1-5-21\\Keys"), "Registry keys protected");
    Expect(IsProtectedRegistryPath(L"HKLM\\SOFTWARE\\Crypto Pro\\Settings\\Users\\S-1-5-21\\cptools\\containers"), "Container metadata protected");
    Expect(!IsProtectedRegistryPath(L"HKLM\\SOFTWARE\\Crypto Pro\\Cryptography"), "Provider settings removable");

    const std::vector<std::wstring> roots{L"C:\\Program Files\\Crypto Pro", L"C:\\ProgramData\\Crypto Pro"};
    Expect(IsSafeVendorPath(L"C:\\Program Files\\Crypto Pro\\CSP\\tool.exe", roots), "Vendor child safe");
    Expect(!IsSafeVendorPath(L"C:\\Program Files\\Other\\tool.exe", roots), "Outside path rejected");
    Expect(!IsSafeVendorPath(L"C:\\ProgramData\\Crypto Pro\\Crypto\\container", roots), "Protected child rejected");

    Expect(JsonEscape(L"a\"b\\c\n") == L"a\\\"b\\\\c\\n", "JSON escaping");
    const std::wstring unicode = L"КриптоПро / CryptoPro";
    Expect(FromUtf8(Utf8(unicode)) == unicode, "UTF-8 round trip");

    wchar_t arg0[] = L"app.exe";
    wchar_t arg1[] = L"--scan";
    wchar_t arg2[] = L"--lang";
    wchar_t arg3[] = L"ru";
    wchar_t* args[] = {arg0, arg1, arg2, arg3};
    const CommandLineOptions options = ParseCommandLine(4, args);
    Expect(options.scanOnly, "Parse scan switch");
    Expect(options.language == Language::Russian && options.languageExplicit, "Parse language switch");

    if (argc > 1 && std::wstring(argv[1]) == L"--integration-scan") {
        const ScanResult scan = ScanSystem(Language::English);
        const CleanupPlan plan = BuildCleanupPlan(scan);
        Expect(std::all_of(scan.products.begin(), scan.products.end(), [](const InstalledProduct& product) {
            return IsCryptoProPublisher(product.publisher);
        }), "Integration scan only returns confirmed publishers");
        Expect(std::none_of(plan.targets.begin(), plan.targets.end(), [](const CleanupTarget& target) {
            return target.protectedItem || IsProtectedPath(target.path) || IsProtectedRegistryPath(target.registry.subkey);
        }), "Integration plan excludes protected targets");
        Expect(std::all_of(scan.licenses.begin(), scan.licenses.end(), [](const LicenseEntry& license) {
            return license.sourcePriority != 100 ||
                   (license.valueName == L"ProductID" &&
                    license.registryPath.find(L"\\Installer\\UserData\\") != std::wstring::npos);
        }), "Preferred full ProductID values have a dynamic Installer UserData source");
        Expect(std::none_of(scan.licenses.begin(), scan.licenses.end(), [&](const LicenseEntry& shorter) {
            return std::any_of(scan.licenses.begin(), scan.licenses.end(), [&](const LicenseEntry& longer) {
                return &shorter != &longer && shorter.fullValue.size() < longer.fullValue.size() &&
                       longer.fullValue.rfind(shorter.fullValue, 0) == 0;
            });
        }), "Truncated license prefix suppressed when a complete value exists");
        Expect(std::all_of(scan.certificates.begin(), scan.certificates.end(), [](const CertificateEntry& certificate) {
            if (certificate.encoded.empty() || certificate.thumbprint.size() != 40 ||
                certificate.subject.empty() || certificate.issuer.empty() ||
                certificate.validFrom.empty() || certificate.validTo.empty()) return false;
            PCCERT_CONTEXT raw = CertCreateCertificateContext(X509_ASN_ENCODING,
                certificate.encoded.data(), static_cast<DWORD>(certificate.encoded.size()));
            std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> context(raw, CertFreeCertificateContext);
            return context != nullptr;
        }), "Enumerated certificates contain valid public DER data and display metadata");
        wchar_t tempDirectory[MAX_PATH]{};
        wchar_t tempReport[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, tempDirectory) && GetTempFileNameW(tempDirectory, L"cpc", 0, tempReport)) {
            std::wstring reportError;
            const auto verifyNoFullLicense = [&](const char* assertion) {
                HANDLE file = CreateFileW(tempReport, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
                if (file == INVALID_HANDLE_VALUE) { Expect(false, assertion); return; }
                LARGE_INTEGER size{};
                bool redacted = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart < 16 * 1024 * 1024;
                std::string bytes;
                if (redacted) {
                    bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
                    DWORD read = 0;
                    redacted = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
                    bytes.resize(read);
                    for (const auto& license : scan.licenses) {
                        if (!license.fullValue.empty() && bytes.find(Utf8(license.fullValue)) != std::string::npos) redacted = false;
                    }
                    for (const auto& certificate : scan.certificates) {
                        if ((!certificate.subject.empty() && bytes.find(Utf8(certificate.subject)) != std::string::npos) ||
                            (!certificate.issuer.empty() && bytes.find(Utf8(certificate.issuer)) != std::string::npos)) redacted = false;
                    }
                }
                CloseHandle(file);
                Expect(redacted, assertion);
            };
            Expect(WriteJsonReport(tempReport, scan, &plan, nullptr, nullptr, &reportError), "Integration JSON report created");
            verifyNoFullLicense("Full license is absent from JSON report");
            Expect(WriteTextSummary(Language::English, tempReport, scan, &plan, nullptr, nullptr, &reportError),
                   "Integration text summary created");
            verifyNoFullLicense("Full license is absent from text summary");
            DeleteFileW(tempReport);
        } else {
            Expect(false, "Temporary integration report path created");
        }
        wchar_t runningWindows[MAX_PATH]{};
        if (GetWindowsDirectoryW(runningWindows, MAX_PATH)) {
            const OfflineScanResult self = ScanOfflineWindows(Language::English, runningWindows);
            Expect(!self.valid, "Running Windows is rejected as an offline cleanup target");
        }
        wchar_t offlineBackupTemp[MAX_PATH]{};
        wchar_t offlineBackupVolume[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, offlineBackupTemp) &&
            GetVolumePathNameW(offlineBackupTemp, offlineBackupVolume, MAX_PATH)) {
            OfflineScanResult sameVolume;
            sameVolume.valid = true;
            sameVolume.volumeRoot = offlineBackupVolume;
            std::wstring session;
            std::wstring error;
            Expect(!SaveOfflineBackup(Language::English, sameVolume, offlineBackupTemp, true, &session, &error),
                   "Offline cleanup recovery backup rejects the source volume");
        }
        wchar_t certificateTempDirectory[MAX_PATH]{};
        wchar_t generatedParent[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, certificateTempDirectory) && GetTempFileNameW(certificateTempDirectory, L"cpc", 0, generatedParent) &&
            DeleteFileW(generatedParent) && CreateDirectoryW(generatedParent, nullptr)) {
            std::wstring exportFolder;
            std::wstring exportError;
            Expect(ExportPublicCertificates(Language::English, scan.certificates, generatedParent,
                                            &exportFolder, &exportError),
                   "Selected public certificates export succeeds");
            if (!scan.certificates.empty() && !exportFolder.empty()) {
                size_t certificateFiles = 0;
                WIN32_FIND_DATAW data{};
                HANDLE files = FindFirstFileW((exportFolder + L"\\*.cer").c_str(), &data);
                if (files != INVALID_HANDLE_VALUE) {
                    do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++certificateFiles; }
                    while (FindNextFileW(files, &data));
                    FindClose(files);
                }
                const size_t selected = static_cast<size_t>(std::count_if(scan.certificates.begin(), scan.certificates.end(),
                    [](const CertificateEntry& certificate) { return certificate.selected; }));
                Expect(certificateFiles == selected, "One CER file is exported for each selected certificate");
                const std::wstring bundlePath = exportFolder + L"\\certificates.p7b";
                HCERTSTORE bundle = CertOpenStore(CERT_STORE_PROV_FILENAME_W, 0, 0,
                                                  CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
                                                  bundlePath.c_str());
                Expect(bundle != nullptr, "Exported P7B bundle can be reopened by CryptoAPI");
                if (bundle) CertCloseStore(bundle, 0);
            }
            Expect(DeleteGeneratedTree(generatedParent), "Temporary certificate export is removed");
        } else {
            Expect(false, "Temporary certificate export folder is created");
        }
        std::wcout << L"Safe integration scan: " << scan.products.size() << L" products, "
                   << scan.profiles.size() << L" profiles, " << scan.certificates.size() << L" public certificates, "
                   << plan.targets.size() << L" verified targets.\n";
    }
    if (failures == 0) std::cout << "All CryptoProCleanup core tests passed.\n";
    return failures == 0 ? 0 : 1;
}
