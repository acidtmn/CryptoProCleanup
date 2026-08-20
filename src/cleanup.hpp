#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cpc {

constexpr wchar_t kVersion[] = L"0.4.0-rc4";

enum class Language { Russian, English };
enum class RiskLevel { Normal, High };
enum class TargetType {
    File,
    Directory,
    RegistryTree,
    Service,
    DriverService,
    DriverPackage,
    ScheduledTask,
    Shortcut
};
enum class Outcome { Succeeded, Skipped, Failed, RebootRequired };
enum class RegistryHive { LocalMachine, CurrentUser, Users };
enum class OfflineHive { Software, System };

struct RegistryLocation {
    RegistryHive hive = RegistryHive::LocalMachine;
    std::wstring subkey;
    REGSAM view = 0;
};

struct InstalledProduct {
    std::wstring displayName;
    std::wstring version;
    std::wstring publisher;
    std::wstring architecture;
    std::wstring uninstallString;
    std::wstring quietUninstallString;
    std::wstring installLocation;
    std::wstring registryKey;
    std::wstring productCode;
    RegistryHive hive = RegistryHive::LocalMachine;
    REGSAM registryView = 0;
    bool msi = false;
    bool selected = true;
    RiskLevel risk = RiskLevel::Normal;
};

struct UserProfile {
    std::wstring sid;
    std::wstring displayName;
    std::wstring profilePath;
    bool loaded = false;
    bool selected = true;
};

struct LicenseEntry {
    std::wstring product;
    std::wstring registryPath;
    std::wstring valueName;
    std::wstring fullValue;
    std::wstring maskedValue;
    int sourcePriority = 0;
};

struct CertificateEntry {
    std::wstring profileSid;
    std::wstring profileName;
    std::wstring subject;
    std::wstring issuer;
    std::wstring validFrom;
    std::wstring validTo;
    std::wstring thumbprint;
    std::vector<BYTE> encoded;
    bool hasPrivateKeyReference = false;
    bool selected = true;
};

struct ScanResult {
    std::wstring osName;
    std::wstring osArchitecture;
    std::vector<InstalledProduct> products;
    std::vector<UserProfile> profiles;
    std::vector<LicenseEntry> licenses;
    std::vector<CertificateEntry> certificates;
    std::vector<std::wstring> protectedItems;
    std::vector<std::wstring> warnings;
};

struct CleanupTarget {
    TargetType type = TargetType::File;
    std::wstring displayName;
    std::wstring path;
    std::wstring reason;
    RegistryLocation registry;
    bool verified = false;
    bool protectedItem = false;
};

struct OfflineCleanupTarget {
    TargetType type = TargetType::File;
    OfflineHive hive = OfflineHive::Software;
    std::wstring displayName;
    std::wstring path;
    std::wstring registrySubkey;
    std::wstring reason;
    bool verified = false;
    bool protectedItem = false;
};

struct OfflineScanResult {
    std::wstring windowsDirectory;
    std::wstring volumeRoot;
    std::wstring softwareHivePath;
    std::wstring systemHivePath;
    ScanResult scan;
    std::vector<OfflineCleanupTarget> targets;
    std::vector<std::wstring> diagnostics;
    bool valid = false;
    bool cleanupCapable = false;
};

class OfflineRegistryMount {
public:
    OfflineRegistryMount() = default;
    ~OfflineRegistryMount();
    OfflineRegistryMount(const OfflineRegistryMount&) = delete;
    OfflineRegistryMount& operator=(const OfflineRegistryMount&) = delete;
    LONG Open(const std::wstring& hivePath, REGSAM access);
    LONG Close();
    HKEY get() const { return key_; }
    explicit operator bool() const { return key_ != nullptr; }

private:
    HKEY key_ = nullptr;
    std::wstring mountName_;
};

struct CleanupPlan {
    std::vector<InstalledProduct> products;
    std::vector<UserProfile> profiles;
    std::vector<CleanupTarget> targets;
    std::vector<std::wstring> protectedItems;
    bool allDetectedProductsSelected = false;
};

struct OperationRecord {
    std::wstring action;
    std::wstring target;
    Outcome outcome = Outcome::Skipped;
    DWORD code = ERROR_SUCCESS;
    std::wstring message;
};

struct ExecutionResult {
    std::vector<OperationRecord> operations;
    bool rebootRequired = false;
    bool residualCleanupDeferred = false;
    bool anyFailure = false;
    bool anyRemoval = false;
};

struct CommandLineOptions {
    bool scanOnly = false;
    bool showHelp = false;
    Language language = Language::English;
    bool languageExplicit = false;
    std::wstring reportPath;
    std::wstring resumeToken;
    std::wstring offlineWindowsPath;
};

using ProgressCallback = std::function<void(const std::wstring&, int)>;

Language DetectLanguage();
std::wstring Tr(Language language, const wchar_t* russian, const wchar_t* english);
std::wstring ToLower(std::wstring value);
std::wstring Trim(const std::wstring& value);
std::wstring ExpandEnvironment(const std::wstring& value);
std::wstring MaskLicense(const std::wstring& value);
std::wstring JsonEscape(const std::wstring& value);
std::string Utf8(const std::wstring& value);
std::wstring FromUtf8(const std::string& value);
bool IsCryptoProPublisher(const std::wstring& value);
bool IsCryptoProName(const std::wstring& value);
bool IsHighRiskProduct(const std::wstring& value);
bool IsGuid(const std::wstring& value);
std::wstring PackMsiProductCode(const std::wstring& productCode);
std::wstring UnpackMsiProductCode(const std::wstring& packedProductCode);
bool IsProtectedPath(const std::wstring& path);
bool IsProtectedRegistryPath(const std::wstring& path);
bool IsSafeVendorPath(const std::wstring& path, const std::vector<std::wstring>& approvedRoots);
bool VerifyCryptoProSignature(const std::wstring& path, std::wstring* signer = nullptr);
std::wstring GetLastErrorMessage(DWORD code);

ScanResult ScanSystem(Language language, const ProgressCallback& progress = {});
void ScanUserCertificates(const std::vector<UserProfile>& profiles,
                          std::vector<CertificateEntry>* certificates,
                          std::vector<std::wstring>* warnings,
                          const ProgressCallback& progress = {});
void ScanOfflineMachineCertificates(Language language, HKEY offlineSoftware,
                                    std::vector<CertificateEntry>* certificates,
                                    std::vector<std::wstring>* warnings);
bool ExportPublicCertificates(Language language, const std::vector<CertificateEntry>& certificates,
                              const std::wstring& parentFolder, std::wstring* exportFolder,
                              std::wstring* error = nullptr);
OfflineScanResult ScanOfflineWindows(Language language, const std::wstring& windowsDirectory,
                                     const ProgressCallback& progress = {});
bool SaveOfflineBackup(Language language, const OfflineScanResult& offline,
                       const std::wstring& parentFolder, bool includeRecoveryCopies,
                       std::wstring* sessionFolder, std::wstring* error = nullptr);
ExecutionResult ExecuteOfflineCleanup(const OfflineScanResult& offline,
                                      const std::wstring& backupSession,
                                      const ProgressCallback& progress = {});
CleanupPlan BuildCleanupPlan(const ScanResult& scan, const ProgressCallback& progress = {});
ExecutionResult ExecuteCleanup(const CleanupPlan& plan, bool allowForcedCleanup,
                               const ProgressCallback& progress = {});
ScanResult VerifyAfterCleanup(Language language, const ProgressCallback& progress = {});

bool EnsureDirectory(const std::wstring& path, std::wstring* error = nullptr);
bool WriteUtf8File(const std::wstring& path, const std::string& content, std::wstring* error = nullptr);
bool SaveBackup(Language language, const ScanResult& scan, const CleanupPlan& plan, const std::wstring& folder,
                std::wstring* licensesPath, std::wstring* initialReportPath,
                std::wstring* logPath, std::wstring* error);
bool WriteTextSummary(Language language, const std::wstring& path, const ScanResult& scan,
                      const CleanupPlan* plan, const ExecutionResult* execution,
                      const ScanResult* verification, std::wstring* error = nullptr);
bool WriteJsonReport(const std::wstring& path, const ScanResult& scan,
                     const CleanupPlan* plan, const ExecutionResult* execution,
                     const ScanResult* verification, std::wstring* error = nullptr);
bool AppendLog(const std::wstring& path, const std::wstring& line);

bool PrepareResume(const CleanupPlan& plan, const std::wstring& reportFolder,
                   std::wstring* token, std::wstring* error);
bool LoadResumePlan(const std::wstring& token, ScanResult* scan, CleanupPlan* plan,
                    std::wstring* reportFolder, std::wstring* error);
bool CompleteResume(const std::wstring& token, std::wstring* error = nullptr);
bool RequestSystemRestart(std::wstring* error = nullptr);

CommandLineOptions ParseCommandLine(int argc, wchar_t** argv);
int RunGui(HINSTANCE instance, Language language, const std::wstring& resumeToken);
int RunScanCommand(const CommandLineOptions& options);
int RunOfflineScanCommand(const CommandLineOptions& options);

}  // namespace cpc
