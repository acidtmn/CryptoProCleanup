#pragma once

#include <windows.h>
#include "version.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cpc {

enum class Language { Russian, English };
// Registry values are public compatibility data. Do not reorder these values:
// 0 was the original Dark setting and 1 was the original System setting.
enum class ThemeMode : DWORD { Dark = 0, System = 1, Light = 2 };
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
enum class CleanupTargetCategory {
    File,
    Directory,
    Registry,
    Service,
    Driver,
    DriverPackage,
    Com,
    Provider,
    NativeMessagingHost,
    ScheduledTask,
    Shortcut,
    Protected
};
enum class Outcome { Succeeded, Skipped, Failed, RebootRequired };
enum class RegistryHive { LocalMachine, CurrentUser, Users };
enum class OfflineHive { Software, System };
enum class CertificateStatus { Unknown, Valid, ExpiringSoon, Expired, NotYetValid };
enum class FileSignatureState { Valid, Absent, Invalid, Error };

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
    CleanupTargetCategory category = CleanupTargetCategory::File;
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

enum class ResumeMode { DeferredResidual, ForcedResidual };

struct ResumeAuthorization {
    ResumeMode mode = ResumeMode::DeferredResidual;
    bool forceAuthorized = false;
    bool residualCleanupDeferred = true;
    bool uninstallerFailurePresent = false;
    unsigned long attempts = 0;

    bool AllowsResidualPass() const {
        return forceAuthorized || (residualCleanupDeferred && !uninstallerFailurePresent);
    }
};

enum class BackupFolderState {
    Available,
    ReadyForProbe,
    EmptyPath,
    UnsafeLocation,
    NotWritable,
    InsufficientSpace,
    SameVolume
};

struct BackupFolderValidation {
    BackupFolderState state = BackupFolderState::EmptyPath;
    unsigned long long freeBytes = 0;
    unsigned long long requiredBytes = 0;
    std::wstring normalizedPath;
    std::wstring nearestExistingParent;
    std::wstring volumeRoot;
    std::wstring detail;
    bool probePerformed = false;
    bool ok() const { return state == BackupFolderState::Available; }
    bool canProbe() const { return state == BackupFolderState::ReadyForProbe; }
};

enum class UiOperation {
    Idle,
    LiveScan,
    BuildingPlan,
    ExportingCertificates,
    OfflineScan,
    SavingOfflineData,
    LiveCleanup,
    ForcedCleanup,
    OfflineCleanup,
    ResumeCleanup,
    ComputingHash
};

struct UiOperationToken {
    unsigned long long generation = 0;
    explicit operator bool() const { return generation != 0; }
};

class UiOperationGate {
public:
    UiOperationToken TryBeginToken(UiOperation operation) {
        if (operation == UiOperation::Idle || operation_ != UiOperation::Idle) return {};
        operation_ = operation;
        activeGeneration_ = ++lastGeneration_;
        return {activeGeneration_};
    }
    bool TryBegin(UiOperation operation) {
        return static_cast<bool>(TryBeginToken(operation));
    }
    bool Transition(UiOperationToken token, UiOperation operation) {
        if (!IsCurrent(token) || operation == UiOperation::Idle) return false;
        operation_ = operation;
        return true;
    }
    bool Transition(UiOperation operation) {
        if (operation == UiOperation::Idle || operation_ == UiOperation::Idle) return false;
        operation_ = operation;
        return true;
    }
    bool End(UiOperationToken token) {
        if (!IsCurrent(token)) return false;
        operation_ = UiOperation::Idle;
        activeGeneration_ = 0;
        return true;
    }
    void End() { operation_ = UiOperation::Idle; activeGeneration_ = 0; }
    bool IsCurrent(UiOperationToken token) const {
        return token.generation != 0 && token.generation == activeGeneration_ && operation_ != UiOperation::Idle;
    }
    UiOperationToken currentToken() const { return {activeGeneration_}; }
    UiOperation current() const { return operation_; }
    bool idle() const { return operation_ == UiOperation::Idle; }
private:
    UiOperation operation_ = UiOperation::Idle;
    unsigned long long lastGeneration_ = 0;
    unsigned long long activeGeneration_ = 0;
};

struct PlanRevisionTracker {
    enum class State { NotBuilt, Ready, Stale };
    unsigned long long scanRevision = 0;
    unsigned long long productsRevision = 0;
    unsigned long long profilesRevision = 0;
    unsigned long long certificatesRevision = 0;
    unsigned long long backupRevision = 0;
    unsigned long long planScanRevision = 0;
    unsigned long long planProductsRevision = 0;
    unsigned long long planProfilesRevision = 0;
    unsigned long long planCertificatesRevision = 0;
    unsigned long long planBackupRevision = 0;
    bool planReady = false;
    bool planEverBuilt = false;

    void ScanChanged() { ++scanRevision; planReady = false; }
    void ProductsChanged() { ++productsRevision; planReady = false; }
    void ProfilesChanged() { ++profilesRevision; planReady = false; }
    void CertificatesChanged() { ++certificatesRevision; planReady = false; }
    void BackupChanged() { ++backupRevision; planReady = false; }
    // Compatibility alias for older callers: historically all selection
    // changes were stored in one dimension and meant product selection.
    void SelectionChanged() { ProductsChanged(); }
    void PlanBuilt() {
        planScanRevision = scanRevision;
        planProductsRevision = productsRevision;
        planProfilesRevision = profilesRevision;
        planCertificatesRevision = certificatesRevision;
        planBackupRevision = backupRevision;
        planReady = true;
        planEverBuilt = true;
    }
    bool IsPlanCurrent() const {
        return planReady && planScanRevision == scanRevision &&
               planProductsRevision == productsRevision &&
               planProfilesRevision == profilesRevision &&
               planCertificatesRevision == certificatesRevision &&
               planBackupRevision == backupRevision;
    }
    State CurrentState() const {
        if (!planEverBuilt) return State::NotBuilt;
        return IsPlanCurrent() ? State::Ready : State::Stale;
    }
    bool SameInputs(PlanRevisionTracker const& other) const {
        return scanRevision == other.scanRevision &&
               productsRevision == other.productsRevision &&
               profilesRevision == other.profilesRevision &&
               certificatesRevision == other.certificatesRevision &&
               backupRevision == other.backupRevision;
    }
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
ThemeMode NormalizeThemeMode(DWORD value);
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
FileSignatureState InspectFileSignature(const std::wstring& path, std::wstring* signer = nullptr,
                                        LONG* trustStatus = nullptr);
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
BackupFolderValidation ValidateBackupFolder(const std::wstring& path,
                                            const std::wstring& sourcePath = {},
                                            unsigned long long minimumFreeBytes = 64ull * 1024 * 1024);
BackupFolderValidation InspectBackupPath(const std::wstring& path,
                                         const std::wstring& sourcePath = {},
                                         unsigned long long minimumFreeBytes = 64ull * 1024 * 1024);
BackupFolderValidation ProbeBackupFolder(const std::wstring& path,
                                         const std::wstring& sourcePath = {},
                                         unsigned long long minimumFreeBytes = 64ull * 1024 * 1024);
unsigned long long EstimateOfflineBackupMinimum(const OfflineScanResult& offline);
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
bool WriteEmergencyCleanupLog(const std::wstring& path, const ScanResult& redactionContext,
                              const std::wstring& lastCompletedStage,
                              const ExecutionResult* execution, DWORD errorCode,
                              std::wstring* error = nullptr);
bool AppendLog(const std::wstring& path, const std::wstring& line);

size_t CountSelectedCertificates(const std::vector<CertificateEntry>& certificates);
size_t CountSelectedProducts(const std::vector<InstalledProduct>& products);
size_t CountVerifiedTargets(const std::vector<CleanupTarget>& targets);
unsigned long long CertificateExpiryKey(const CertificateEntry& certificate);
CertificateStatus GetCertificateStatus(const CertificateEntry& certificate,
                                       const FILETIME* now = nullptr, unsigned soonDays = 30);
void SetCertificateSelection(std::vector<CertificateEntry>& certificates,
                             const std::vector<size_t>& indices, bool selected);
bool IsOfflineConfirmation(const std::wstring& phrase);
void MergeExecutionResults(ExecutionResult* destination, ExecutionResult source);
std::wstring RedactSensitiveText(const std::wstring& value, const ScanResult& scan);
std::wstring BuildResumeCommand(const std::wstring& runnerPath, const std::wstring& token);
bool IsUsableResumeRunner(const std::wstring& runnerPath, std::wstring* error = nullptr);
std::wstring ComputeFileSha256(const std::wstring& path, std::wstring* error = nullptr);

bool PrepareResume(const CleanupPlan& plan, const std::wstring& reportFolder,
                   std::wstring* token, std::wstring* error);
bool PrepareResumeWithRunner(const CleanupPlan& plan, const std::wstring& reportFolder,
                             const std::wstring& runnerPath, Language language,
                             std::wstring* token, std::wstring* error,
                              bool registerRunOnce = true,
                              const std::wstring& sessionsRootOverride = {});
bool PrepareResumeAuthorized(const CleanupPlan& plan, const std::wstring& reportFolder,
                             const std::wstring& runnerPath, Language language,
                             const ResumeAuthorization& authorization,
                             std::wstring* token, std::wstring* error,
                             bool registerRunOnce = true,
                             const std::wstring& sessionsRootOverride = {});
bool LoadResumePlan(const std::wstring& token, ScanResult* scan, CleanupPlan* plan,
                    std::wstring* reportFolder, std::wstring* error);
bool LoadResumePlan(const std::wstring& token, ScanResult* scan, CleanupPlan* plan,
                    std::wstring* reportFolder, Language* language, std::wstring* error,
                    const std::wstring& sessionsRootOverride);
bool LoadResumePlanAuthorized(const std::wstring& token, ScanResult* scan, CleanupPlan* plan,
                              std::wstring* reportFolder, Language* language,
                              ResumeAuthorization* authorization, std::wstring* error,
                              const std::wstring& sessionsRootOverride = {});
bool CompleteResume(const std::wstring& token, std::wstring* error = nullptr);
bool CompleteResume(const std::wstring& token, std::wstring* error,
                    bool removeRunOnce, const std::wstring& sessionsRootOverride);
int RunResumeCommand(const std::wstring& token, bool showResult,
                     const ProgressCallback& progress = {});

CommandLineOptions ParseCommandLine(int argc, wchar_t** argv);
int RunGui(HINSTANCE instance, Language language, const std::wstring& resumeToken,
           bool languageExplicit = false);
int RunScanCommand(const CommandLineOptions& options);
int RunOfflineScanCommand(const CommandLineOptions& options);

}  // namespace cpc
