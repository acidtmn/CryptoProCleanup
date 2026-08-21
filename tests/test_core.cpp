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

bool DirectoryExistsForTest(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool CreateSizedFileForTest(const std::wstring& path, unsigned long long size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    length.QuadPart = static_cast<LONGLONG>(size);
    const bool success = SetFilePointerEx(file, length, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    CloseHandle(file);
    return success;
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
    if (argc >= 3 && std::wstring(argv[1]) == L"--probe-app-hive") {
        HKEY root = nullptr;
        const LONG loaded = RegLoadAppKeyW(argv[2], &root, KEY_READ, 0, 0);
        std::wcout << L"RegLoadAppKey=" << loaded << L"\n";
        if (loaded == ERROR_SUCCESS) {
            const wchar_t* subkey = argc >= 4 ? argv[3] : L"Microsoft";
            for (const REGSAM access : {KEY_READ, KEY_READ | KEY_WOW64_64KEY,
                                        KEY_READ | KEY_WOW64_32KEY}) {
                HKEY child = nullptr;
                const LONG opened = RegOpenKeyExW(root, subkey, 0, access, &child);
                std::wcout << L"RegOpenKeyEx(" << subkey << L", 0x" << std::hex << access
                           << std::dec << L")=" << opened << L"\n";
                if (child) RegCloseKey(child);
            }
            RegCloseKey(root);
        }
        return loaded == ERROR_SUCCESS ? 0 : static_cast<int>(loaded);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--probe-mounted-hive") {
        OfflineRegistryMount hive;
        const LONG loaded = hive.Open(argv[2], KEY_READ);
        std::wcout << L"RegLoadKey=" << loaded << L"\n";
        LONG opened = loaded;
        if (loaded == ERROR_SUCCESS) {
            const wchar_t* subkey = argc >= 4 ? argv[3] : L"Microsoft";
            HKEY child = nullptr;
            opened = RegOpenKeyExW(hive.get(), subkey, 0, KEY_READ, &child);
            std::wcout << L"RegOpenKeyEx(" << subkey << L")=" << opened << L"\n";
            if (child) RegCloseKey(child);
        }
        const LONG unloaded = hive.Close();
        std::wcout << L"RegUnLoadKey=" << unloaded << L"\n";
        return loaded == ERROR_SUCCESS && opened == ERROR_SUCCESS && unloaded == ERROR_SUCCESS ? 0 : 1;
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--offline-scan") {
        const OfflineScanResult offline = ScanOfflineWindows(Language::English, argv[2]);
        std::cout << "valid=" << offline.valid << " cleanupCapable=" << offline.cleanupCapable
                  << " products=" << offline.scan.products.size()
                  << " licenses=" << offline.scan.licenses.size()
                  << " profiles=" << offline.scan.profiles.size()
                  << " certificates=" << offline.scan.certificates.size()
                  << " targets=" << offline.targets.size() << "\n";
        for (const auto& line : offline.diagnostics) std::cout << "DIAGNOSTIC: " << Utf8(line) << "\n";
        for (const auto& line : offline.scan.warnings) std::cout << "WARNING: " << Utf8(line) << "\n";
        return offline.valid ? 0 : 1;
    }
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

    UiOperationGate operationGate;
    Expect(std::wstring(kVersion) == L"0.5.3-rc1", "Central release-candidate version");
    Expect(NormalizeThemeMode(0) == ThemeMode::Dark &&
           NormalizeThemeMode(1) == ThemeMode::System &&
           NormalizeThemeMode(2) == ThemeMode::Light,
           "Theme registry values remain compatible");
    Expect(NormalizeThemeMode(99) == ThemeMode::Dark, "Unknown theme falls back safely");
    Expect(operationGate.TryBegin(UiOperation::LiveScan), "Operation gate accepts idle transition");
    Expect(!operationGate.TryBegin(UiOperation::LiveCleanup), "Operation gate rejects overlapping work");
    Expect(operationGate.Transition(UiOperation::BuildingPlan) &&
           operationGate.current() == UiOperation::BuildingPlan,
           "Operation gate supports explicit active phase transition");
    operationGate.End();
    const auto firstToken = operationGate.TryBeginToken(UiOperation::LiveScan);
    Expect(firstToken && operationGate.IsCurrent(firstToken), "Operation token identifies active generation");
    Expect(operationGate.End(firstToken), "Current operation token can complete work");
    const auto secondToken = operationGate.TryBeginToken(UiOperation::OfflineScan);
    Expect(secondToken && secondToken.generation != firstToken.generation,
           "Each operation receives a new generation");
    Expect(!operationGate.End(firstToken) && operationGate.IsCurrent(secondToken),
           "Stale callback cannot end a newer operation");
    operationGate.End(secondToken);
    Expect(operationGate.TryBegin(UiOperation::OfflineScan), "Operation gate can be reused after completion");
    operationGate.End();

    PlanRevisionTracker revisions;
    Expect(revisions.CurrentState() == PlanRevisionTracker::State::NotBuilt,
           "A plan is not stale before it has ever been built");
    revisions.ScanChanged();
    Expect(revisions.CurrentState() == PlanRevisionTracker::State::NotBuilt,
           "Initial scan input still leaves the plan in NotBuilt state");
    revisions.PlanBuilt();
    Expect(revisions.IsPlanCurrent(), "Fresh cleanup plan revision is current");
    Expect(revisions.CurrentState() == PlanRevisionTracker::State::Ready,
           "Built current plan reports Ready state");
    revisions.SelectionChanged();
    Expect(!revisions.IsPlanCurrent(), "Selection change invalidates cleanup plan");
    Expect(revisions.CurrentState() == PlanRevisionTracker::State::Stale,
           "Changed inputs report Stale only after a plan existed");
    revisions.PlanBuilt();
    revisions.ProfilesChanged();
    Expect(!revisions.IsPlanCurrent(), "Profile selection invalidates cleanup plan");
    revisions.PlanBuilt();
    revisions.CertificatesChanged();
    Expect(!revisions.IsPlanCurrent(), "Certificate selection invalidates backup plan");
    revisions.PlanBuilt();
    revisions.BackupChanged();
    Expect(!revisions.IsPlanCurrent(), "Backup path invalidates cleanup plan");

    std::vector<CertificateEntry> selectionTest(3);
    selectionTest[0].selected = true;
    selectionTest[1].selected = false;
    selectionTest[2].selected = true;
    Expect(CountSelectedCertificates(selectionTest) == 2, "Selected certificate count");
    SetCertificateSelection(selectionTest, {1, 2}, false);
    Expect(selectionTest[0].selected && !selectionTest[1].selected && !selectionTest[2].selected,
           "Filtered certificate deselection touches only visible indices");
    CertificateEntry olderCertificate;
    olderCertificate.validTo = L"31.12.2024";
    CertificateEntry newerCertificate;
    newerCertificate.validTo = L"01.01.2025";
    Expect(CertificateExpiryKey(olderCertificate) < CertificateExpiryKey(newerCertificate),
           "Certificate dates sort chronologically instead of lexicographically");
    Expect(GetCertificateStatus(CertificateEntry{}) == CertificateStatus::Unknown,
           "Certificate without parseable public data has unknown validity");
    std::vector<InstalledProduct> selectedProductTest(2);
    selectedProductTest[0].selected = true;
    selectedProductTest[1].selected = false;
    std::vector<CleanupTarget> verifiedTargetTest(3);
    verifiedTargetTest[0].verified = true;
    verifiedTargetTest[1].verified = false;
    verifiedTargetTest[2].verified = true;
    verifiedTargetTest[2].protectedItem = true;
    Expect(CountSelectedProducts(selectedProductTest) == 1, "Plan counts selected products only");
    Expect(CountVerifiedTargets(verifiedTargetTest) == 1, "Plan counts only verified removable targets");
    Expect(IsOfflineConfirmation(L"OFFLINE"), "Exact offline confirmation accepted");
    Expect(!IsOfflineConfirmation(L"offline") && !IsOfflineConfirmation(L"OFFLINE "),
           "Inexact offline confirmation rejected");
    Expect(InspectBackupPath(L"").state == BackupFolderState::EmptyPath,
           "Empty backup path is rejected without side effects");
    wchar_t systemWindows[MAX_PATH]{};
    GetWindowsDirectoryW(systemWindows, MAX_PATH);
    Expect(InspectBackupPath(systemWindows).state == BackupFolderState::UnsafeLocation,
           "Windows directory is rejected as a backup location");
    const std::wstring programFiles = ExpandEnvironment(L"%ProgramFiles%");
    if (!programFiles.empty())
        Expect(InspectBackupPath(programFiles).state == BackupFolderState::UnsafeLocation,
               "Program Files is rejected as a backup location");

    ExecutionResult firstExecution;
    firstExecution.anyRemoval = true;
    firstExecution.operations.push_back({L"uninstall", L"product", Outcome::Succeeded, ERROR_SUCCESS, L"ok"});
    ExecutionResult forcedExecution;
    forcedExecution.anyFailure = true;
    forcedExecution.rebootRequired = true;
    forcedExecution.operations.push_back({L"remove", L"service", Outcome::Failed, ERROR_ACCESS_DENIED, L"denied"});
    MergeExecutionResults(&firstExecution, std::move(forcedExecution));
    Expect(firstExecution.anyRemoval && firstExecution.anyFailure && firstExecution.rebootRequired &&
           firstExecution.operations.size() == 2, "Execution passes merge without losing state");

    ScanResult redactionScan;
    redactionScan.licenses.push_back({L"CSP", L"registry", L"ProductID", L"5050N4003001BT72MA83QF3T0", L"50*******************3T0", 100});
    CertificateEntry redactionCertificate;
    redactionCertificate.subject = L"CN=Sensitive User";
    redactionCertificate.issuer = L"CN=Sensitive CA";
    redactionScan.certificates.push_back(redactionCertificate);
    const std::wstring redacted = RedactSensitiveText(
        L"5050N4003001BT72MA83QF3T0 CN=Sensitive User CN=Sensitive CA", redactionScan);
    Expect(redacted.find(L"5050N4003001BT72MA83QF3T0") == std::wstring::npos &&
           redacted.find(L"Sensitive User") == std::wstring::npos &&
           redacted.find(L"Sensitive CA") == std::wstring::npos,
           "Logs redact license and certificate identity");
    ScanResult boundaryRedaction;
    boundaryRedaction.profiles.push_back({L"S-1-5-21-1000", L"user", L"C:\\Users\\user", true, true});
    boundaryRedaction.profiles.push_back({L"S-1-5-21-1001", L"a", L"C:\\Users\\a", false, true});
    boundaryRedaction.profiles.push_back({L"S-1-5-21-1002", L"admin", L"C:\\Users\\admin", false, true});
    const std::wstring technicalText = RedactSensitiveText(
        L"Installer\\UserData C:\\Users\\user\\file S-1-5-21-1000 user admin a data", boundaryRedaction);
    Expect(technicalText.find(L"UserData") != std::wstring::npos,
           "Profile name redaction preserves Installer UserData");
    Expect(technicalText.find(L"C:\\Users\\user") == std::wstring::npos &&
           technicalText.find(L"S-1-5-21-1000") == std::wstring::npos,
           "Full profile path and SID are redacted");
    Expect(technicalText.find(L" user ") == std::wstring::npos && technicalText.find(L" admin ") == std::wstring::npos,
           "Profile names are redacted on token boundaries");
    Expect(technicalText.find(L" a ") != std::wstring::npos,
           "Single-character profile name is not replaced as free text");
    ScanResult offlineRedaction;
    offlineRedaction.profiles.push_back({L"S-1-5-21-2000", L"offline-user", L"E:\\Users\\offline-user", false, true});
    const std::wstring offlineText = RedactSensitiveText(L"E:\\Users\\offline-user S-1-5-21-2000", offlineRedaction);
    Expect(offlineText.find(L"offline-user") == std::wstring::npos && offlineText.find(L"S-1-5-21-2000") == std::wstring::npos,
           "Offline scan uses its own redaction context");

    wchar_t resumeTemp[MAX_PATH]{};
    wchar_t resumeRoot[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, resumeTemp) && GetTempFileNameW(resumeTemp, L"cpr", 0, resumeRoot) &&
        DeleteFileW(resumeRoot) && CreateDirectoryW(resumeRoot, nullptr)) {
        const std::wstring isolatedRoot = resumeRoot;
        const std::wstring backupValidationPath = isolatedRoot + L"\\backup-validation";
        const auto inspectedBackup = InspectBackupPath(backupValidationPath);
        Expect(inspectedBackup.canProbe() && !DirectoryExistsForTest(backupValidationPath),
               "Read-only backup inspection does not create a nonexistent folder");
        Expect(ToLower(inspectedBackup.nearestExistingParent) == ToLower(isolatedRoot),
               "Backup inspection resolves the nearest existing parent");
        const auto writableBackup = ProbeBackupFolder(backupValidationPath);
        Expect(writableBackup.ok() && writableBackup.probePerformed && !DirectoryExistsForTest(backupValidationPath),
               "Background-style probe tests the parent without creating the final folder");
        WIN32_FIND_DATAW probeData{};
        HANDLE probeSearch = FindFirstFileW((isolatedRoot + L"\\.cryptopro-cleanup-write-test-*.tmp").c_str(), &probeData);
        Expect(probeSearch == INVALID_HANDLE_VALUE, "Backup write probe removes its temporary file");
        if (probeSearch != INVALID_HANDLE_VALUE) FindClose(probeSearch);
        const auto sameVolumeBackup = ValidateBackupFolder(backupValidationPath, isolatedRoot);
        Expect(sameVolumeBackup.state == BackupFolderState::SameVolume,
               "Offline backup prevalidation rejects the source volume");
        const auto explicitMinimum = InspectBackupPath(backupValidationPath, {}, 96ull * 1024 * 1024);
        Expect(explicitMinimum.requiredBytes == 96ull * 1024 * 1024,
               "Backup validation exposes the expected minimum without creating a folder");

        const std::wstring offlineFiles = isolatedRoot + L"\\offline-files";
        CreateDirectoryW(offlineFiles.c_str(), nullptr);
        OfflineScanResult offlineEstimate;
        offlineEstimate.volumeRoot = isolatedRoot;
        offlineEstimate.softwareHivePath = offlineFiles + L"\\SOFTWARE";
        offlineEstimate.systemHivePath = offlineFiles + L"\\SYSTEM";
        const std::wstring quarantineDirectory = offlineFiles + L"\\CryptoPro";
        CreateDirectoryW(quarantineDirectory.c_str(), nullptr);
        const bool estimateFilesCreated =
            CreateSizedFileForTest(offlineEstimate.softwareHivePath, 2ull * 1024 * 1024) &&
            CreateSizedFileForTest(offlineEstimate.systemHivePath, 3ull * 1024 * 1024) &&
            CreateSizedFileForTest(quarantineDirectory + L"\\module.dll", 4ull * 1024 * 1024);
        OfflineCleanupTarget estimateTarget;
        estimateTarget.type = TargetType::Directory;
        estimateTarget.path = quarantineDirectory;
        estimateTarget.verified = true;
        offlineEstimate.targets.push_back(estimateTarget);
        Expect(estimateFilesCreated && EstimateOfflineBackupMinimum(offlineEstimate) >= 256ull * 1024 * 1024,
               "Offline cleanup estimates hives, quarantine targets, and a safety reserve");
        const std::wstring runner = isolatedRoot + L"\\CryptoProCleanupResume.exe";
        wchar_t currentExecutable[MAX_PATH]{};
        const bool copied = GetModuleFileNameW(nullptr, currentExecutable, MAX_PATH) != 0 &&
                            CopyFileW(currentExecutable, runner.c_str(), FALSE) != FALSE;
        Expect(copied && IsUsableResumeRunner(runner), "Native resume runner validation");
        Expect(ComputeFileSha256(runner).size() == 64, "Resume runner SHA-256 is available");
        Expect(!IsUsableResumeRunner(isolatedRoot + L"\\missing.exe"), "Missing resume runner rejected");

        CleanupPlan resumeSource;
        InstalledProduct resumeProduct;
        resumeProduct.displayName = L"CryptoPro CSP test metadata";
        resumeProduct.version = L"5.0";
        resumeProduct.publisher = L"Crypto-Pro LLC";
        resumeProduct.selected = true;
        resumeSource.products.push_back(resumeProduct);
        CleanupTarget resumeTarget;
        resumeTarget.type = TargetType::Service;
        resumeTarget.displayName = L"CryptoPro test service";
        resumeTarget.path = L"CryptoProCleanupTestService";
        resumeTarget.reason = L"unit test metadata";
        resumeTarget.verified = true;
        resumeSource.targets.push_back(resumeTarget);
        resumeSource.protectedItems.push_back(L"certificate stores");
        resumeSource.allDetectedProductsSelected = true;

        const std::wstring reportFolder = isolatedRoot + L"\\reports";
        EnsureDirectory(reportFolder);
        std::wstring resumeToken;
        std::wstring resumeError;
        ResumeAuthorization deniedAuthorization;
        deniedAuthorization.residualCleanupDeferred = false;
        deniedAuthorization.uninstallerFailurePresent = true;
        std::wstring deniedToken;
        Expect(!PrepareResumeAuthorized(resumeSource, reportFolder, runner, Language::English,
                                       deniedAuthorization, &deniedToken, &resumeError, false, isolatedRoot) && deniedToken.empty(),
               "Resume state is not created without residual-cleanup authorization");
        ResumeAuthorization forcedAuthorization;
        forcedAuthorization.mode = ResumeMode::ForcedResidual;
        forcedAuthorization.forceAuthorized = true;
        forcedAuthorization.uninstallerFailurePresent = true;
        Expect(forcedAuthorization.AllowsResidualPass(),
               "Explicit FORCE authorization permits the bounded residual pass");
        const bool prepared = copied && PrepareResumeWithRunner(
            resumeSource, reportFolder, runner, Language::English, &resumeToken, &resumeError,
            false, isolatedRoot);
        Expect(prepared && IsGuid(resumeToken), "Resume state prepared without RunOnce");
        Expect(!BuildResumeCommand(runner, resumeToken).empty(), "Quoted resume command created");

        ScanResult loadedScan;
        CleanupPlan loadedPlan;
        std::wstring loadedReport;
        Language loadedLanguage = Language::Russian;
        ResumeAuthorization loadedAuthorization;
        const bool loaded = prepared && LoadResumePlanAuthorized(resumeToken, &loadedScan, &loadedPlan,
            &loadedReport, &loadedLanguage, &loadedAuthorization, &resumeError, isolatedRoot);
        Expect(loaded && loadedLanguage == Language::English && loadedReport == reportFolder &&
               loadedPlan.products.size() == 1 && loadedPlan.targets.size() == 1 &&
               loadedPlan.targets[0].verified && loadedAuthorization.AllowsResidualPass(),
               "Protected resume plan round-trips without HKLM state");
        Expect(!loadedPlan.products.empty() && loadedPlan.products[0].uninstallString.empty(),
               "Resume test metadata does not invent an uninstaller");
        Expect(!CompleteResume(L"not-a-guid", &resumeError, false, isolatedRoot),
               "Invalid resume token is rejected");
        Expect(!prepared || CompleteResume(resumeToken, &resumeError, false, isolatedRoot),
               "Resume state cleanup succeeds without RunOnce");
        const std::wstring tamperRunner = isolatedRoot + L"\\CryptoProCleanupResume.exe";
        Expect(CopyFileW(currentExecutable, tamperRunner.c_str(), FALSE) != FALSE,
               "Fresh resume helper copied for tamper test");
        std::wstring tamperToken;
        const bool tamperPrepared = PrepareResumeWithRunner(resumeSource, reportFolder, tamperRunner,
            Language::English, &tamperToken, &resumeError, false, isolatedRoot);
        const std::wstring authorizedRunner = isolatedRoot + L"\\" + tamperToken + L"\\CryptoProCleanupResume.exe";
        HANDLE tamperFile = CreateFileW(authorizedRunner.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool tampered = false;
        if (tamperFile != INVALID_HANDLE_VALUE) {
            const BYTE marker = 0x5A;
            DWORD written = 0;
            tampered = WriteFile(tamperFile, &marker, 1, &written, nullptr) != FALSE && written == 1;
            CloseHandle(tamperFile);
        }
        ScanResult tamperScan;
        CleanupPlan tamperPlan;
        std::wstring tamperReport;
        Language tamperLanguage{};
        ResumeAuthorization tamperAuthorization;
        Expect(tamperPrepared && tampered && !LoadResumePlanAuthorized(tamperToken, &tamperScan,
            &tamperPlan, &tamperReport, &tamperLanguage, &tamperAuthorization, &resumeError, isolatedRoot),
            "Resume state rejects a helper changed after authorization");
        if (tamperPrepared) CompleteResume(tamperToken, &resumeError, false, isolatedRoot);
        Expect(DeleteGeneratedTree(isolatedRoot), "Temporary resume state is removed");
    } else {
        Expect(false, "Temporary resume test folder is created");
    }

    wchar_t arg0[] = L"app.exe";
    wchar_t arg1[] = L"--scan";
    wchar_t arg2[] = L"--lang";
    wchar_t arg3[] = L"ru";
    wchar_t* args[] = {arg0, arg1, arg2, arg3};
    const CommandLineOptions options = ParseCommandLine(4, args);
    Expect(options.scanOnly, "Parse scan switch");
    Expect(options.language == Language::Russian && options.languageExplicit, "Parse language switch");

    wchar_t offlineArg[] = L"--offline-scan";
    wchar_t offlinePath[] = L"E:\\Windows";
    wchar_t* offlineArgs[] = {arg0, offlineArg, offlinePath};
    const CommandLineOptions offlineOptions = ParseCommandLine(3, offlineArgs);
    Expect(offlineOptions.offlineWindowsPath == offlinePath, "Parse safe offline scan path");

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
