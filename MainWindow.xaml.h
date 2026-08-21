#pragma once

#include "MainWindow.g.h"
#include "pch.h"
#include "src/cleanup.hpp"

#include <exception>

namespace winrt::CryptoProCleanupModern::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        void InitializeSession(cpc::Language language, std::wstring const& resumeToken,
                               bool languageExplicit = false);

        void Navigation_ItemInvoked(Microsoft::UI::Xaml::Controls::NavigationView const&,
                                    Microsoft::UI::Xaml::Controls::NavigationViewItemInvokedEventArgs const& args);
        void CompactNavigation_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);
        void LanguageCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void SettingsLanguageCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                                    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void ThemeCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                         Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void SettingsToggle_Toggled(winrt::Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ResetSettings_Click(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        void WindowRoot_SizeChanged(winrt::Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
        void Rescan_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ChooseBackup_Click(winrt::Windows::Foundation::IInspectable const&,
                                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget CheckPlan_Click(winrt::Windows::Foundation::IInspectable const&,
                                               Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CertificateFilter_Changed(winrt::Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void CertificateFilter_Changed(winrt::Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void SelectFilteredCertificates_Click(winrt::Windows::Foundation::IInspectable const&,
                                               Microsoft::UI::Xaml::RoutedEventArgs const&);
        void DeselectAllCertificates_Click(winrt::Windows::Foundation::IInspectable const&,
                                           Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ExportCertificates_Click(winrt::Windows::Foundation::IInspectable const&,
                                                        Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CopyThumbprint_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ShowLicenses_Click(winrt::Windows::Foundation::IInspectable const&,
                                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ChooseOffline_Click(winrt::Windows::Foundation::IInspectable const&,
                                                   Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ScanOffline_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OfflineDiagnostics_Click(winrt::Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget SaveOffline_Click(winrt::Windows::Foundation::IInspectable const&,
                                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget CleanOffline_Click(winrt::Windows::Foundation::IInspectable const&,
                                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ToggleTechnicalLog_Click(winrt::Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CopyLog_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenReportFolder_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OpenReportFile_Click(winrt::Windows::Foundation::IInspectable const&,
                                                    Microsoft::UI::Xaml::RoutedEventArgs const&);
        void GitHub_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Website_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Support_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ComputeHash_Click(winrt::Windows::Foundation::IInspectable const&,
                                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ShowLocation_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CopyExecutablePath_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenExecutableFolder_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        enum class UiStatusKind { Idle, Working, ScanComplete, ExportComplete, Cancelled, PlanReady, PlanStale,
                                  Warning, Partial, Success, RestartRequired, Error };
        enum class OutputSessionKind { None, CertificateExport, LiveCleanup, OfflineRescue, OfflineCleanup, ResumeCleanup };
        enum class StageOutcome { NotStarted, Running, Succeeded, Skipped, Failed, RestartDeferred };
        struct OutputSession
        {
            OutputSessionKind kind = OutputSessionKind::None;
            std::wstring folder;
            std::map<std::wstring, std::wstring> files;
            size_t operationCount = 0;
            size_t failureCount = 0;
            size_t residualCount = 0;
            bool restartRequired = false;
            std::array<StageOutcome, 8> stages{};
        };
        cpc::Language language_ = cpc::Language::Russian;
        std::wstring resumeToken_;
        std::wstring currentPage_ = L"overview";
        cpc::ScanResult scan_;
        cpc::CleanupPlan plan_;
        cpc::OfflineScanResult offline_;
        std::wstring offlineInputPath_;
        std::wstring offlineScannedPath_;
        unsigned long long offlineScanRevision_ = 0;
        bool offlinePathStale_ = false;
        bool offlineProductsConfirmed_ = false;
        OutputSession outputSession_;
        std::wstring logText_;
        bool liveScanSummaryOnly_ = false;
        bool technicalLogExpanded_ = false;
        size_t selectedCertificate_ = static_cast<size_t>(-1);
        // XAML controls can raise SelectionChanged while the generated object
        // tree is still being connected. Keep handlers dormant until
        // InitializeSession has configured every named control.
        bool languageSync_ = true;
        bool uiReady_ = false;
        bool compactNavigation_ = false;
        bool busy_ = false;
        bool liveScanRunning_ = false;
        bool offlineScanRunning_ = false;
        bool modalOpen_ = false;
        bool closing_ = false;
        std::deque<std::pair<std::wstring, std::wstring>> dialogQueue_;
        cpc::UiOperationGate operationGate_;
        cpc::PlanRevisionTracker planRevisions_;
        std::vector<size_t> visibleCertificateIndices_;
        Microsoft::UI::Xaml::DispatcherTimer certificateFilterTimer_{nullptr};
        Microsoft::UI::Xaml::DispatcherTimer backupValidationTimer_{nullptr};
        unsigned long long backupValidationRevision_ = 0;
        unsigned long long appliedBackupValidationRevision_ = 0;
        std::wstring backupValidationPath_;
        cpc::BackupFolderValidation backupValidation_;
        bool settingsSync_ = true;
        bool rememberWindow_ = true;
        bool reduceMotion_ = false;
        bool enforcingMinimumSize_ = false;
        cpc::ThemeMode themeMode_ = cpc::ThemeMode::Dark;
        UiStatusKind statusKind_ = UiStatusKind::Idle;
        cpc::UiOperation statusSource_ = cpc::UiOperation::Idle;
        int statusPercent_ = -1;
        std::wstring statusDetail_;
        winrt::Windows::UI::ViewManagement::AccessibilitySettings accessibilitySettings_{nullptr};
        winrt::Windows::UI::ViewManagement::UISettings uiSettings_{nullptr};

        HWND GetWindowHandle();
        void ConfigureWindow();
        void LoadSettings(bool languageExplicit);
        void SaveSettings();
        void ApplyTheme();
        bool ReduceMotionEffective() const;
        void ApplyTitleBarTheme();
        void RefreshThemedVisuals();
        void RefreshCompactNavigationVisuals();
        void Window_Closed(winrt::Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::WindowEventArgs const& args);
        void ApplyAdaptiveLayout(double width);
        void NavigateTo(std::wstring const& page);
        void ApplyLanguage();
        void UpdatePageHeader();
        void SetBusy(bool busy, std::wstring const& message, int percent = -1);
        void SetSemanticStatus(UiStatusKind kind, cpc::UiOperation source = cpc::UiOperation::Idle,
                               int percent = -1);
        void RenderStatus();
        cpc::UiOperationToken BeginOperation(cpc::UiOperation operation, std::wstring const& message, int percent = -1);
        void UpdateOperation(cpc::UiOperationToken token, std::wstring const& message, int percent = -1);
        void EndOperation(cpc::UiOperationToken token, std::wstring const& message,
                          UiStatusKind status = UiStatusKind::Success);
        void RefreshCommandStates();
        void InvalidatePlan();
        bool ValidateBackupPathForUi();
        void ScheduleBackupValidation();
        void RenderBackupValidation();
        winrt::fire_and_forget ProbeBackupPathAsync(unsigned long long revision, std::wstring path);
        void ReportAsyncFailure(std::wstring const& context, std::exception_ptr failure,
                                cpc::UiOperationToken token = {},
                                cpc::ScanResult const* redactionContext = nullptr) noexcept;
        void LogExecution(cpc::ExecutionResult const& execution, cpc::ScanResult const& redactionContext);
        std::wstring LiveScanSummaryText() const;
        void AppendLogLine(std::wstring const& line);
        winrt::fire_and_forget StartLiveScan();
        winrt::fire_and_forget StartOfflineScan(std::wstring path);
        void PopulateLiveScan();
        void PopulateProducts();
        void PopulateProfiles();
        void PopulateCertificates();
        void RefreshCertificateSelectionVisuals();
        void PopulateCertificateFilters();
        void ShowCertificateDetails(size_t index);
        void PopulateOfflineScan();
        void InvalidateOfflinePath();
        bool OfflineStateCurrent();
        winrt::fire_and_forget ShowOfflineDiagnostics();
        void UpdateSelectedCounts();
        void UpdateReportsPage();
        void SetOutputSession(OutputSessionKind kind, std::wstring const& folder,
                              cpc::ExecutionResult const* execution = nullptr,
                              size_t residualCount = 0);
        winrt::fire_and_forget ShowPlanInspector();
        winrt::fire_and_forget ShowRemovalConfirmation();
        winrt::fire_and_forget ExecuteRemoval();
        winrt::fire_and_forget ShowMessage(std::wstring const& title, std::wstring const& message);
        winrt::Windows::Foundation::IAsyncAction PickFolder(std::function<void(std::wstring const&)> completed);
        void OpenUrl(wchar_t const* url);
        void CopyText(std::wstring const& value);
        std::wstring ExecutablePath() const;
        void UpdateAboutSecurityState();
        std::wstring T(wchar_t const* russian, wchar_t const* english) const;
    };
}

namespace winrt::CryptoProCleanupModern::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
