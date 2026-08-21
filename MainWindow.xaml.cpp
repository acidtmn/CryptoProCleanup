#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <sstream>

namespace winrt
{
    using namespace Windows::ApplicationModel::DataTransfer;
    using namespace Windows::Foundation;
    using namespace Windows::Storage::Pickers;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Microsoft::UI::Xaml::Media;
}

namespace
{
    constexpr int kIconResource = 201;
    constexpr wchar_t kSettingsKey[] = L"Software\\CodeAlexandrov\\CryptoProCleanup\\ModernWinUI";
    std::wstring gRuntimeThemeKey = L"Light";

    bool ReadSettingDword(wchar_t const* name, DWORD* value)
    {
        DWORD size = sizeof(*value), type = 0;
        return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD,
                            &type, value, &size) == ERROR_SUCCESS;
    }

    std::wstring ReadSettingString(wchar_t const* name)
    {
        DWORD size = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ,
                         nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return {};
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ,
                         nullptr, value.data(), &size) != ERROR_SUCCESS) return {};
        while (!value.empty() && value.back() == L'\0') value.pop_back();
        return value;
    }

    void WriteSettingDword(HKEY key, wchar_t const* name, DWORD value)
    {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&value), sizeof(value));
    }

    void WriteSettingString(HKEY key, wchar_t const* name, std::wstring const& value)
    {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    winrt::TextBlock Text(std::wstring const& value, double size = 12.0)
    {
        winrt::TextBlock text;
        text.Text(value);
        text.FontSize(size);
        text.TextWrapping(winrt::TextWrapping::Wrap);
        return text;
    }

    winrt::ColumnDefinition Column(double value, winrt::GridUnitType unit)
    {
        winrt::ColumnDefinition column;
        column.Width(winrt::GridLength{value, unit});
        return column;
    }

    winrt::Brush ThemeBrush(wchar_t const* key)
    {
        auto resources = winrt::Application::Current().Resources();
        auto themes = resources.ThemeDictionaries();
        auto themeKey = winrt::box_value(gRuntimeThemeKey);
        if (themes.HasKey(themeKey))
        {
            auto dictionary = themes.Lookup(themeKey).as<winrt::ResourceDictionary>();
            auto resourceKey = winrt::box_value(key);
            if (dictionary.HasKey(resourceKey)) return dictionary.Lookup(resourceKey).as<winrt::Brush>();
        }
        return resources.Lookup(winrt::box_value(key)).as<winrt::Brush>();
    }

    winrt::Border PaddedDialogContent(winrt::UIElement const& child)
    {
        winrt::Border host;
        auto resources = winrt::Application::Current().Resources();
        host.Padding(winrt::unbox_value<winrt::Thickness>(
            resources.Lookup(winrt::box_value(L"DialogContentPadding"))));
        host.Child(child);
        return host;
    }

    enum class BadgeTone { Neutral, Success, Warning, Danger };

    void ApplyBadgeTheme(winrt::Border const& badge, BadgeTone tone)
    {
        wchar_t const* background = tone == BadgeTone::Success ? L"BadgeSuccessBackgroundBrush" :
                                    tone == BadgeTone::Warning ? L"BadgeWarningBackgroundBrush" :
                                    tone == BadgeTone::Danger ? L"BadgeDangerBackgroundBrush" : L"BadgeNeutralBackgroundBrush";
        wchar_t const* foreground = tone == BadgeTone::Success ? L"BadgeSuccessForegroundBrush" :
                                    tone == BadgeTone::Warning ? L"BadgeWarningForegroundBrush" :
                                    tone == BadgeTone::Danger ? L"BadgeDangerForegroundBrush" : L"BadgeNeutralForegroundBrush";
        badge.Background(ThemeBrush(background));
        badge.BorderBrush(ThemeBrush(tone == BadgeTone::Danger ? L"DangerBorderBrush" :
                                    tone == BadgeTone::Warning ? L"WarningBorderBrush" : L"SubtleBorderBrush"));
        if (auto label = badge.Child().try_as<winrt::TextBlock>()) label.Foreground(ThemeBrush(foreground));
    }

    winrt::Border Badge(std::wstring const& value, BadgeTone tone)
    {
        winrt::Border badge;
        badge.HorizontalAlignment(winrt::HorizontalAlignment::Left);
        badge.Padding(winrt::Thickness{9, 5, 9, 5});
        badge.CornerRadius(winrt::CornerRadius{8});
        badge.BorderThickness(winrt::Thickness{1});
        badge.Tag(winrt::box_value(static_cast<int32_t>(tone)));
        auto label = Text(value, 10);
        badge.Child(label);
        ApplyBadgeTheme(badge, tone);
        return badge;
    }

    void RefreshBadgeThemes(winrt::DependencyObject const& root)
    {
        if (!root) return;
        if (auto border = root.try_as<winrt::Border>())
        {
            const int32_t tone = winrt::unbox_value_or<int32_t>(border.Tag(), -1);
            if (tone >= static_cast<int32_t>(BadgeTone::Neutral) &&
                tone <= static_cast<int32_t>(BadgeTone::Danger))
                ApplyBadgeTheme(border, static_cast<BadgeTone>(tone));
        }
        const int children = winrt::VisualTreeHelper::GetChildrenCount(root);
        for (int index = 0; index < children; ++index)
            RefreshBadgeThemes(winrt::VisualTreeHelper::GetChild(root, index));
    }

    std::wstring FormatByteCount(unsigned long long bytes)
    {
        constexpr unsigned long long mib = 1024ull * 1024;
        constexpr unsigned long long gib = 1024ull * mib;
        wchar_t buffer[64]{};
        if (bytes >= gib) swprintf_s(buffer, L"%.1f GB", static_cast<double>(bytes) / gib);
        else swprintf_s(buffer, L"%llu MB", (bytes + mib - 1) / mib);
        return buffer;
    }

    std::wstring ParentDirectory(std::wstring path)
    {
        if (path.empty()) return {};
        std::vector<wchar_t> buffer(path.begin(), path.end());
        buffer.push_back(L'\0');
        if (PathRemoveFileSpecW(buffer.data())) return buffer.data();
        return path;
    }

    std::wstring NormalizeUiPath(std::wstring path)
    {
        path = cpc::Trim(path);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        while (path.size() > 3 && path.back() == L'\\') path.pop_back();
        return cpc::ToLower(path);
    }

    std::wstring VolumeRootOf(std::wstring const& path)
    {
        std::array<wchar_t, MAX_PATH> root{};
        return GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size())) ? root.data() : L"";
    }

}

namespace winrt::CryptoProCleanupModern::implementation
{
    MainWindow::MainWindow()
    {
    }

    void MainWindow::InitializeSession(cpc::Language language, std::wstring const& resumeToken,
                                       bool languageExplicit)
    {
        // C++/WinRT completes XAML connection after the authored constructor
        // returns. Named controls are therefore first accessed here.
        ConfigureWindow();
        uiReady_ = true;
        WindowRoot().ActualThemeChanged([weak = get_weak()](FrameworkElement const&, IInspectable const&)
        {
            if (auto self = weak.get())
            {
                HIGHCONTRASTW contrast{sizeof(contrast)};
                const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                                          (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
                gRuntimeThemeKey = highContrast ? L"HighContrast" :
                    self->themeMode_ == cpc::ThemeMode::Dark ? L"Dark" :
                    self->themeMode_ == cpc::ThemeMode::Light ? L"Light" :
                    self->WindowRoot().ActualTheme() == ElementTheme::Dark ? L"Dark" : L"Light";
                self->ApplyTitleBarTheme();
                self->RefreshThemedVisuals();
            }
        });
        try
        {
            accessibilitySettings_ = winrt::Windows::UI::ViewManagement::AccessibilitySettings();
            accessibilitySettings_.HighContrastChanged([weak = get_weak()](auto const&, auto const&)
            {
                if (auto self = weak.get())
                {
                    self->DispatcherQueue().TryEnqueue([weak]()
                    {
                        if (auto target = weak.get()) target->ApplyTheme();
                    });
                }
            });
            uiSettings_ = winrt::Windows::UI::ViewManagement::UISettings();
            uiSettings_.AnimationsEnabledChanged([weak = get_weak()](auto const&, auto const&)
            {
                if (auto self = weak.get()) self->DispatcherQueue().TryEnqueue([weak]()
                {
                    if (auto target = weak.get()) target->SetBusy(target->busy_, L"", target->statusPercent_);
                });
            });
        }
        catch (...) {}
        language_ = language;
        LoadSettings(languageExplicit);
        ApplyTheme();
        Closed({this, &MainWindow::Window_Closed});
        certificateFilterTimer_ = DispatcherTimer();
        certificateFilterTimer_.Interval(std::chrono::milliseconds(250));
        certificateFilterTimer_.Tick([weak = get_weak()](IInspectable const&, IInspectable const&)
        {
            if (auto self = weak.get())
            {
                self->certificateFilterTimer_.Stop();
                self->PopulateCertificates();
            }
        });
        backupValidationTimer_ = DispatcherTimer();
        backupValidationTimer_.Interval(std::chrono::milliseconds(450));
        backupValidationTimer_.Tick([weak = get_weak()](IInspectable const&, IInspectable const&)
        {
            if (auto self = weak.get())
            {
                self->backupValidationTimer_.Stop();
                self->ProbeBackupPathAsync(self->backupValidationRevision_, self->backupValidationPath_);
            }
        });
        BackupPath().TextChanged([weak = get_weak()](IInspectable const&, TextChangedEventArgs const&)
        {
            if (auto self = weak.get())
            {
                self->planRevisions_.BackupChanged();
                self->InvalidatePlan();
                self->ScheduleBackupValidation();
            }
        });
        OfflinePath().TextChanged([weak = get_weak()](IInspectable const&, TextChangedEventArgs const&)
        {
            if (auto self = weak.get())
            {
                if (self->offlineScanRunning_ || !self->operationGate_.idle()) return;
                const std::wstring entered = self->OfflinePath().Text().c_str();
                if (self->offlineScanRevision_ && NormalizeUiPath(entered) != NormalizeUiPath(self->offlineInputPath_))
                    self->InvalidateOfflinePath();
                self->offlineInputPath_ = entered;
            }
        });
        ApplyAdaptiveLayout(WindowRoot().ActualWidth());
        if (currentPage_ == L"certificates") Navigation().SelectedItem(NavCertificates());
        else if (currentPage_ == L"offline") Navigation().SelectedItem(NavOffline());
        else if (currentPage_ == L"reports") Navigation().SelectedItem(NavReports());
        else if (currentPage_ == L"settings") Navigation().SelectedItem(NavSettings());
        else if (currentPage_ == L"about") Navigation().SelectedItem(NavAbout());
        else { currentPage_ = L"overview"; Navigation().SelectedItem(NavOverview()); }
        NavigateTo(currentPage_);

        resumeToken_ = resumeToken;
        languageSync_ = true;
        LanguageCombo().SelectedIndex(language_ == cpc::Language::Russian ? 0 : 1);
        SettingsLanguageCombo().SelectedIndex(language_ == cpc::Language::Russian ? 0 : 1);
        settingsSync_ = true;
        ThemeCombo().SelectedIndex(std::min(static_cast<int>(themeMode_), 1));
        RememberWindowToggle().IsOn(rememberWindow_);
        ReduceMotionToggle().IsOn(reduceMotion_);
        settingsSync_ = false;
        languageSync_ = false;
        ApplyLanguage();
        ApplyTheme();
        SaveSettings();

        ExecutablePathText().Text(L"");
        ExecutablePathText().Visibility(Visibility::Collapsed);

        PWSTR documents = nullptr;
        if (BackupPath().Text().empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        {
            BackupPath().Text(std::wstring(documents) + L"\\CryptoPro Backup");
            CoTaskMemFree(documents);
        }

        if (!resumeToken_.empty())
        {
            // Defensive fallback for embedders: resume is always an exact
            // residual pass and never enters the normal uninstaller dialog.
            cpc::RunResumeCommand(resumeToken_, true);
            Close();
            return;
        }
        StartLiveScan();
    }

    HWND MainWindow::GetWindowHandle()
    {
        HWND hwnd = nullptr;
        Microsoft::UI::Xaml::Window window = *this;
        check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
        return hwnd;
    }

    void MainWindow::ConfigureWindow()
    {
        Title(std::wstring(L"CryptoPro Cleanup Utility ") + cpc::kVersion);
        const HWND hwnd = GetWindowHandle();
        const UINT dpi = GetDpiForWindow(hwnd);
        const int width = MulDiv(1440, static_cast<int>(dpi), 96);
        const int height = MulDiv(920, static_cast<int>(dpi), 96);
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
        {
            const int availableWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
            const int availableHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
            const int boundedWidth = std::min(width, availableWidth);
            const int boundedHeight = std::min(height, availableHeight);
            const int left = monitorInfo.rcWork.left + (availableWidth - boundedWidth) / 2;
            const int top = monitorInfo.rcWork.top + (availableHeight - boundedHeight) / 2;
            SetWindowPos(hwnd, nullptr, left, top, boundedWidth, boundedHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        const HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconResource),
                                                        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        if (icon)
        {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        }
    }

    void MainWindow::LoadSettings(bool languageExplicit)
    {
        DWORD value = 0;
        if (ReadSettingDword(L"RememberWindow", &value)) rememberWindow_ = value != 0;
        if (ReadSettingDword(L"ReduceMotion", &value)) reduceMotion_ = value != 0;
        if (ReadSettingDword(L"Theme", &value)) themeMode_ = cpc::NormalizeThemeMode(value);
        if (!languageExplicit && ReadSettingDword(L"Language", &value))
            language_ = value == 1 ? cpc::Language::English : cpc::Language::Russian;
        if (rememberWindow_)
        {
            const std::wstring page = ReadSettingString(L"LastPage");
            if (page == L"overview" || page == L"certificates" || page == L"offline" ||
                page == L"reports" || page == L"settings" || page == L"about") currentPage_ = page;
            DWORD left = 0, top = 0, right = 0, bottom = 0;
            if (ReadSettingDword(L"WindowLeft", &left) && ReadSettingDword(L"WindowTop", &top) &&
                ReadSettingDword(L"WindowRight", &right) && ReadSettingDword(L"WindowBottom", &bottom))
            {
                RECT desired{static_cast<LONG>(left), static_cast<LONG>(top),
                             static_cast<LONG>(right), static_cast<LONG>(bottom)};
                HMONITOR monitor = MonitorFromRect(&desired, MONITOR_DEFAULTTONULL);
                MONITORINFO monitorInfo{sizeof(monitorInfo)};
                if (desired.right - desired.left >= 700 && desired.bottom - desired.top >= 500 &&
                    monitor && GetMonitorInfoW(monitor, &monitorInfo))
                {
                    const LONG width = std::min<LONG>(desired.right - desired.left,
                        monitorInfo.rcWork.right - monitorInfo.rcWork.left);
                    const LONG height = std::min<LONG>(desired.bottom - desired.top,
                        monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
                    const LONG boundedLeft = std::clamp<LONG>(desired.left, monitorInfo.rcWork.left,
                        monitorInfo.rcWork.right - width);
                    const LONG boundedTop = std::clamp<LONG>(desired.top, monitorInfo.rcWork.top,
                        monitorInfo.rcWork.bottom - height);
                    SetWindowPos(GetWindowHandle(), nullptr, boundedLeft, boundedTop, width, height,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            else
            {
                DWORD width = 0, height = 0;
                if (ReadSettingDword(L"WindowWidth", &width) && ReadSettingDword(L"WindowHeight", &height) &&
                    width >= 900 && height >= 650 && width <= 4096 && height <= 2160)
                {
                    const UINT dpi = GetDpiForWindow(GetWindowHandle());
                    SetWindowPos(GetWindowHandle(), nullptr, 0, 0,
                        MulDiv(static_cast<int>(width), static_cast<int>(dpi), 96),
                        MulDiv(static_cast<int>(height), static_cast<int>(dpi), 96),
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            if (ReadSettingDword(L"WindowMaximized", &value) && value) ShowWindow(GetWindowHandle(), SW_MAXIMIZE);
        }
    }

    void MainWindow::SaveSettings()
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                            nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        WriteSettingDword(key, L"Language", language_ == cpc::Language::English ? 1 : 0);
        WriteSettingDword(key, L"Theme", static_cast<DWORD>(themeMode_));
        WriteSettingDword(key, L"RememberWindow", rememberWindow_ ? 1 : 0);
        WriteSettingDword(key, L"ReduceMotion", reduceMotion_ ? 1 : 0);
        if (rememberWindow_)
        {
            WINDOWPLACEMENT placement{sizeof(placement)};
            if (GetWindowPlacement(GetWindowHandle(), &placement))
            {
                const RECT rectangle = placement.rcNormalPosition;
                const UINT dpi = GetDpiForWindow(GetWindowHandle());
                WriteSettingDword(key, L"WindowWidth", static_cast<DWORD>(MulDiv(rectangle.right - rectangle.left, 96, dpi)));
                WriteSettingDword(key, L"WindowHeight", static_cast<DWORD>(MulDiv(rectangle.bottom - rectangle.top, 96, dpi)));
                WriteSettingDword(key, L"WindowLeft", static_cast<DWORD>(rectangle.left));
                WriteSettingDword(key, L"WindowTop", static_cast<DWORD>(rectangle.top));
                WriteSettingDword(key, L"WindowRight", static_cast<DWORD>(rectangle.right));
                WriteSettingDword(key, L"WindowBottom", static_cast<DWORD>(rectangle.bottom));
                WriteSettingDword(key, L"WindowMaximized", IsZoomed(GetWindowHandle()) ? 1 : 0);
            }
            WriteSettingString(key, L"LastPage", currentPage_);
        }
        RegCloseKey(key);
    }

    void MainWindow::ApplyTheme()
    {
        switch (themeMode_)
        {
        case cpc::ThemeMode::System: WindowRoot().RequestedTheme(ElementTheme::Default); break;
        case cpc::ThemeMode::Light: WindowRoot().RequestedTheme(ElementTheme::Light); break;
        default: WindowRoot().RequestedTheme(ElementTheme::Dark); break;
        }
        HIGHCONTRASTW contrast{sizeof(contrast)};
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                                   (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        gRuntimeThemeKey = highContrast ? L"HighContrast" :
            themeMode_ == cpc::ThemeMode::Dark ? L"Dark" :
            themeMode_ == cpc::ThemeMode::Light ? L"Light" :
            WindowRoot().ActualTheme() == ElementTheme::Dark ? L"Dark" : L"Light";
        HighContrastStatus().Text(highContrast
            ? T(L"Высокий контраст Windows включён.", L"Windows high contrast is enabled.")
            : T(L"Высокий контраст Windows выключен.", L"Windows high contrast is disabled."));
        ApplyTitleBarTheme();
        RefreshThemedVisuals();
    }

    void MainWindow::ApplyTitleBarTheme()
    {
        const BOOL dark = WindowRoot().ActualTheme() == ElementTheme::Dark ? TRUE : FALSE;
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        {
            using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
            if (auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute")))
            {
                if (FAILED(setAttribute(GetWindowHandle(), 20, &dark, sizeof(dark))))
                    setAttribute(GetWindowHandle(), 19, &dark, sizeof(dark));
            }
            FreeLibrary(dwm);
        }
    }

    void MainWindow::RefreshThemedVisuals()
    {
        if (!uiReady_) return;
        // Static controls use ThemeResource. Runtime rows keep their model,
        // handlers, focus, and selection; only brushes are refreshed here.
        for (auto const& child : ProductsPanel().Children())
            if (auto border = child.try_as<Border>()) border.BorderBrush(ThemeBrush(L"DividerBrush"));
        for (auto const& child : OfflineProductsPanel().Children())
            if (auto border = child.try_as<Border>()) border.BorderBrush(ThemeBrush(L"DividerBrush"));
        for (auto const& item : CertificatesPanel().Items())
            if (auto button = item.try_as<Button>()) button.BorderBrush(ThemeBrush(L"DividerBrush"));
        RefreshBadgeThemes(WindowRoot());
        RefreshCertificateSelectionVisuals();
        RefreshCompactNavigationVisuals();
        RenderStatus();
    }

    void MainWindow::RefreshCompactNavigationVisuals()
    {
        const auto clear = ThemeBrush(L"NavigationBackgroundBrush");
        const auto selected = ThemeBrush(L"SurfaceSelectedBrush");
        const auto foreground = ThemeBrush(L"PrimaryTextBrush");
        auto apply = [&](Button const& button, bool active)
        {
            button.Background(active ? selected : clear);
            button.Foreground(foreground);
            button.BorderBrush(active ? selected : clear);
        };
        apply(CompactOverview(), currentPage_ == L"overview");
        apply(CompactCertificates(), currentPage_ == L"certificates");
        apply(CompactOffline(), currentPage_ == L"offline");
        apply(CompactReports(), currentPage_ == L"reports");
        apply(CompactSettings(), currentPage_ == L"settings");
        apply(CompactAbout(), currentPage_ == L"about");
    }

    void MainWindow::Window_Closed(IInspectable const&, WindowEventArgs const& args)
    {
        if (!operationGate_.idle())
        {
            args.Handled(true);
            ShowMessage(T(L"Операция ещё выполняется", L"An operation is still running"),
                        T(L"Дождитесь завершения текущего этапа. Закрытие сейчас заблокировано.",
                          L"Wait for the current stage to finish. Closing is blocked for now."));
            return;
        }
        closing_ = true;
        dialogQueue_.clear();
        SaveSettings();
    }

    void MainWindow::WindowRoot_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        if (!uiReady_) return;
        if (!enforcingMinimumSize_ && (args.NewSize().Width < 900 || args.NewSize().Height < 650))
        {
            enforcingMinimumSize_ = true;
            RECT rectangle{};
            GetWindowRect(GetWindowHandle(), &rectangle);
            const UINT dpi = GetDpiForWindow(GetWindowHandle());
            SetWindowPos(GetWindowHandle(), nullptr, 0, 0,
                std::max<LONG>(rectangle.right - rectangle.left,
                               static_cast<LONG>(MulDiv(900, static_cast<int>(dpi), 96))),
                std::max<LONG>(rectangle.bottom - rectangle.top,
                               static_cast<LONG>(MulDiv(650, static_cast<int>(dpi), 96))),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            enforcingMinimumSize_ = false;
        }
        ApplyAdaptiveLayout(args.NewSize().Width);
    }

    void MainWindow::ApplyAdaptiveLayout(double width)
    {
        if (width <= 0) width = 1440;
        const bool compact = width < 1180;
        if (compact != compactNavigation_)
        {
            compactNavigation_ = compact;
            Navigation().PaneDisplayMode(compact ? NavigationViewPaneDisplayMode::LeftMinimal
                                                 : NavigationViewPaneDisplayMode::Left);
            Navigation().IsPaneOpen(!compact);
            Navigation().IsPaneToggleButtonVisible(false);
            Navigation().Margin(compact ? Thickness{48, 0, 0, 0} : Thickness{0, 0, 0, 0});
            CompactRail().Visibility(compact ? Visibility::Visible : Visibility::Collapsed);
        }

        const double contentWidth = width - (compact ? 48.0 : 248.0);
        auto setColumn = [](Grid const& grid, uint32_t index, double value, GridUnitType unit)
        {
            grid.ColumnDefinitions().GetAt(index).Width(GridLength{value, unit});
        };

        const bool stackHeader = contentWidth < 900;
        Grid::SetColumn(HeaderActions(), stackHeader ? 0 : 1);
        Grid::SetRow(HeaderActions(), stackHeader ? 1 : 0);
        Grid::SetColumnSpan(HeaderActions(), stackHeader ? 2 : 1);
        HeaderActions().HorizontalAlignment(stackHeader ? HorizontalAlignment::Left : HorizontalAlignment::Right);

        const bool stackBackupPath = contentWidth < 760;
        setColumn(BackupPathGrid(), 0, 1.0, GridUnitType::Star);
        setColumn(BackupPathGrid(), 1, stackBackupPath ? 0.0 : 1.0,
                  stackBackupPath ? GridUnitType::Pixel : GridUnitType::Auto);
        Grid::SetColumn(ChooseBackupButton(), stackBackupPath ? 0 : 1);
        Grid::SetRow(ChooseBackupButton(), stackBackupPath ? 1 : 0);
        ChooseBackupButton().HorizontalAlignment(stackBackupPath ? HorizontalAlignment::Left : HorizontalAlignment::Stretch);

        const bool twoColumnStats = contentWidth < 930;
        for (uint32_t index = 0; index < 4; ++index)
            setColumn(OverviewStatsGrid(), index, index < (twoColumnStats ? 2u : 4u) ? 1.0 : 0.0,
                      index < (twoColumnStats ? 2u : 4u) ? GridUnitType::Star : GridUnitType::Pixel);
        Grid::SetColumn(ProductsStatCard(), 0); Grid::SetRow(ProductsStatCard(), 0);
        Grid::SetColumn(LicensesStatCard(), 1); Grid::SetRow(LicensesStatCard(), 0);
        Grid::SetColumn(CertificatesStatCard(), twoColumnStats ? 0 : 2); Grid::SetRow(CertificatesStatCard(), twoColumnStats ? 1 : 0);
        Grid::SetColumn(RiskStatCard(), twoColumnStats ? 1 : 3); Grid::SetRow(RiskStatCard(), twoColumnStats ? 1 : 0);

        const bool stackOverview = contentWidth < 980;
        setColumn(OverviewContentGrid(), 0, stackOverview ? 1.0 : 2.2, GridUnitType::Star);
        setColumn(OverviewContentGrid(), 1, stackOverview ? 0.0 : 1.0,
                  stackOverview ? GridUnitType::Pixel : GridUnitType::Star);
        Grid::SetColumn(OverviewLeftColumn(), 0); Grid::SetRow(OverviewLeftColumn(), 0);
        Grid::SetColumn(PlanCard(), stackOverview ? 0 : 1); Grid::SetRow(PlanCard(), stackOverview ? 1 : 0);
        PlanCard().Margin(stackOverview ? Thickness{0, 0, 0, 0} : Thickness{0, 0, 0, 0});

        const bool stackBottom = contentWidth < 760;
        setColumn(OverviewBottomGrid(), 0, 1.0, GridUnitType::Star);
        setColumn(OverviewBottomGrid(), 1, stackBottom ? 0.0 : 1.25,
                  stackBottom ? GridUnitType::Pixel : GridUnitType::Star);
        Grid::SetColumn(ProfilesCard(), 0); Grid::SetRow(ProfilesCard(), 0);
        Grid::SetColumn(BackupCard(), stackBottom ? 0 : 1); Grid::SetRow(BackupCard(), stackBottom ? 1 : 0);

        const bool stackCertificates = contentWidth < 1010;
        setColumn(CertificatesContentGrid(), 0, stackCertificates ? 1.0 : 2.3, GridUnitType::Star);
        setColumn(CertificatesContentGrid(), 1, stackCertificates ? 0.0 : 1.0,
                  stackCertificates ? GridUnitType::Pixel : GridUnitType::Star);
        Grid::SetColumn(CertificatesListCard(), 0); Grid::SetRow(CertificatesListCard(), 0);
        Grid::SetColumn(CertificateDetailsCard(), stackCertificates ? 0 : 1);
        Grid::SetRow(CertificateDetailsCard(), stackCertificates ? 1 : 0);

        const bool stackCertificateFilters = contentWidth < 880;
        setColumn(CertificateFilterGrid(), 0, 1.0, GridUnitType::Star);
        setColumn(CertificateFilterGrid(), 1, stackCertificateFilters ? 1.0 : 150.0,
                  stackCertificateFilters ? GridUnitType::Star : GridUnitType::Pixel);
        setColumn(CertificateFilterGrid(), 2, stackCertificateFilters ? 0.0 : 150.0,
                  GridUnitType::Pixel);
        Grid::SetColumn(CertificateProfileFilter(), stackCertificateFilters ? 0 : 1);
        Grid::SetRow(CertificateProfileFilter(), stackCertificateFilters ? 1 : 0);
        Grid::SetColumn(CertificateKeyFilter(), stackCertificateFilters ? 1 : 2);
        Grid::SetRow(CertificateKeyFilter(), stackCertificateFilters ? 1 : 0);
        Grid::SetRow(CertificateBulkActions(), stackCertificateFilters ? 2 : 1);

        for (uint32_t index = 0; index < 4; ++index)
            setColumn(OfflineStatsGrid(), index, index < (twoColumnStats ? 2u : 4u) ? 1.0 : 0.0,
                      index < (twoColumnStats ? 2u : 4u) ? GridUnitType::Star : GridUnitType::Pixel);
        Grid::SetColumn(OfflineProductsStatCard(), 0); Grid::SetRow(OfflineProductsStatCard(), 0);
        Grid::SetColumn(OfflineLicensesStatCard(), 1); Grid::SetRow(OfflineLicensesStatCard(), 0);
        Grid::SetColumn(OfflineCertsStatCard(), twoColumnStats ? 0 : 2); Grid::SetRow(OfflineCertsStatCard(), twoColumnStats ? 1 : 0);
        Grid::SetColumn(OfflineTargetsStatCard(), twoColumnStats ? 1 : 3); Grid::SetRow(OfflineTargetsStatCard(), twoColumnStats ? 1 : 0);

        const bool stackOffline = contentWidth < 980;
        setColumn(OfflineContentGrid(), 0, stackOffline ? 1.0 : 2.2, GridUnitType::Star);
        setColumn(OfflineContentGrid(), 1, stackOffline ? 0.0 : 1.0,
                  stackOffline ? GridUnitType::Pixel : GridUnitType::Star);
        Grid::SetColumn(OfflineResultsCard(), 0); Grid::SetRow(OfflineResultsCard(), 0);
        Grid::SetColumn(OfflineRescueCard(), stackOffline ? 0 : 1); Grid::SetRow(OfflineRescueCard(), stackOffline ? 1 : 0);

        const bool stackSettings = contentWidth < 760;
        for (auto const& row : {SettingsLanguageRow(), SettingsThemeRow()})
        {
            setColumn(row, 0, 1.0, GridUnitType::Star);
            setColumn(row, 1, stackSettings ? 0.0 : 220.0,
                      stackSettings ? GridUnitType::Pixel : GridUnitType::Pixel);
        }
        Grid::SetColumn(SettingsLanguageCombo(), stackSettings ? 0 : 1);
        Grid::SetRow(SettingsLanguageCombo(), stackSettings ? 1 : 0);
        Grid::SetColumn(ThemeCombo(), stackSettings ? 0 : 1);
        Grid::SetRow(ThemeCombo(), stackSettings ? 1 : 0);
        SettingsLanguageCombo().HorizontalAlignment(stackSettings ? HorizontalAlignment::Stretch : HorizontalAlignment::Right);
        ThemeCombo().HorizontalAlignment(stackSettings ? HorizontalAlignment::Stretch : HorizontalAlignment::Right);
    }

    std::wstring MainWindow::T(wchar_t const* russian, wchar_t const* english) const
    {
        return language_ == cpc::Language::Russian ? russian : english;
    }

    void MainWindow::Navigation_ItemInvoked(NavigationView const&, NavigationViewItemInvokedEventArgs const& args)
    {
        const auto item = args.InvokedItemContainer();
        if (!item) return;
        const auto tag = item.Tag();
        if (!tag) return;
        NavigateTo(unbox_value<hstring>(tag).c_str());
    }

    void MainWindow::CompactNavigation_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto button = sender.try_as<Button>();
        if (!button || !button.Tag()) return;
        const std::wstring page = unbox_value<hstring>(button.Tag()).c_str();
        if (page == L"overview") Navigation().SelectedItem(NavOverview());
        else if (page == L"certificates") Navigation().SelectedItem(NavCertificates());
        else if (page == L"offline") Navigation().SelectedItem(NavOffline());
        else if (page == L"reports") Navigation().SelectedItem(NavReports());
        else if (page == L"settings") Navigation().SelectedItem(NavSettings());
        else if (page == L"about") Navigation().SelectedItem(NavAbout());
        else return;
        NavigateTo(page);
    }

    void MainWindow::NavigateTo(std::wstring const& page)
    {
        currentPage_ = page;
        const auto visible = Visibility::Visible;
        const auto collapsed = Visibility::Collapsed;
        OverviewPage().Visibility(page == L"overview" ? visible : collapsed);
        CertificatesPage().Visibility(page == L"certificates" ? visible : collapsed);
        OfflinePage().Visibility(page == L"offline" ? visible : collapsed);
        ReportsPage().Visibility(page == L"reports" ? visible : collapsed);
        SettingsPage().Visibility(page == L"settings" ? visible : collapsed);
        AboutPage().Visibility(page == L"about" ? visible : collapsed);
        RescanButton().Visibility(page == L"overview" || page == L"certificates" ? visible : collapsed);
        RefreshCompactNavigationVisuals();
        UpdatePageHeader();
        if (page == L"reports") UpdateReportsPage();
    }

    void MainWindow::UpdatePageHeader()
    {
        if (currentPage_ == L"overview")
        {
            PageEyebrow().Text(T(L"РАБОТАЮЩАЯ СИСТЕМА · БЕЗОПАСНОЕ СКАНИРОВАНИЕ", L"LIVE SYSTEM · SAFE SCAN"));
            PageTitle().Text(T(L"Обзор системы", L"System overview"));
            PageSubtitle().Text(T(L"Проверь найденные продукты, резервную копию и план операции.",
                                  L"Review detected products, the backup folder, and the operation plan."));
        }
        else if (currentPage_ == L"certificates")
        {
            PageEyebrow().Text(T(L"ОТКРЫТЫЕ СЕРТИФИКАТЫ · ТОЛЬКО ПУБЛИЧНАЯ ЧАСТЬ", L"PUBLIC CERTIFICATES · PUBLIC PART ONLY"));
            PageTitle().Text(T(L"Открытые сертификаты", L"Public certificates"));
            PageSubtitle().Text(T(L"Выберите публичную часть для экспорта в .cer и общий .p7b.",
                                  L"Select public certificates to export as .cer files and one .p7b bundle."));
        }
        else if (currentPage_ == L"offline")
        {
            PageEyebrow().Text(T(L"ОТКЛЮЧЁННАЯ WINDOWS · СНАЧАЛА ТОЛЬКО ЧТЕНИЕ", L"OFFLINE WINDOWS · READ-ONLY FIRST"));
            PageTitle().Text(T(L"Офлайн-Windows", L"Offline Windows"));
            PageSubtitle().Text(T(L"Можно выбрать корень подключённого диска или находящуюся на нём папку Windows.",
                                  L"Select either the attached drive root or its Windows directory."));
        }
        else if (currentPage_ == L"reports")
        {
            PageEyebrow().Text(T(L"ОТЧЁТЫ · БЕЗ ПЕРСОНАЛЬНЫХ ДАННЫХ", L"REPORTS · NO PERSONAL DATA"));
            PageTitle().Text(T(L"Журнал и отчёты", L"Log and reports"));
            PageSubtitle().Text(T(L"Здесь отображается обезличенный ход операций и созданные файлы.",
                                  L"This page shows the redacted operation log and generated files."));
        }
        else if (currentPage_ == L"settings")
        {
            PageEyebrow().Text(T(L"ПРИЛОЖЕНИЕ · ЛОКАЛЬНЫЕ НАСТРОЙКИ", L"APPLICATION · LOCAL SETTINGS"));
            PageTitle().Text(T(L"Настройки", L"Settings"));
            PageSubtitle().Text(T(L"Параметры интерфейса не содержат лицензий, сертификатов и целей очистки.",
                                  L"Interface settings never contain licenses, certificates, or cleanup targets."));
        }
        else
        {
            PageEyebrow().Text(T(L"О ПРОЕКТЕ · ОТКРЫТЫЙ ИСХОДНЫЙ КОД", L"ABOUT · OPEN SOURCE"));
            PageTitle().Text(T(L"О программе", L"About"));
            PageSubtitle().Text(T(L"Версия, автор, лицензия и важная информация о проекте.",
                                  L"Version, author, license, and important project information."));
        }
    }

    void MainWindow::ApplyLanguage()
    {
        // Detailed progress is transient. Re-render it from the semantic
        // operation source so a language switch never leaves stale text.
        statusDetail_.clear();
        Title(T(L"КриптоПро Очистка ", L"CryptoPro Cleanup Utility ") + std::wstring(cpc::kVersion));
        BrandTitle().Text(T(L"КриптоПро\nОчистка", L"CryptoPro\nCleanup"));
        NavOverview().Content(box_value(T(L"Обзор", L"Overview")));
        NavCertificates().Content(box_value(T(L"Открытые сертификаты", L"Public certificates")));
        NavOffline().Content(box_value(T(L"Офлайн-Windows", L"Offline Windows")));
        NavReports().Content(box_value(T(L"Журнал и отчёты", L"Log and reports")));
        NavSettings().Content(box_value(T(L"Настройки", L"Settings")));
        NavAbout().Content(box_value(T(L"О программе", L"About")));
        SafetyCaption().Text(T(L"СОСТОЯНИЕ ЗАЩИТЫ", L"PROTECTION STATUS"));
        SafetyText().Text(T(L"Контейнеры ключей и хранилища сертификатов исключены из очистки",
                            L"Key containers and certificate stores are excluded from cleanup"));
        SafetyActive().Text(T(L"Политика безопасности активна", L"Safety policy is active"));
        WebsiteLink().Content(box_value(T(L"Сайт", L"Website")));
        SupportLink().Content(box_value(T(L"Поддержать", L"Support")));
        RescanButtonText().Text(T(L"Повторить сканирование", L"Scan again"));

        ProductsStatCaption().Text(T(L"ПРОДУКТЫ", L"PRODUCTS"));
        LicensesStatCaption().Text(T(L"ЛИЦЕНЗИИ", L"LICENSES"));
        CertificatesStatCaption().Text(T(L"СЕРТИФИКАТЫ", L"CERTIFICATES"));
        RiskStatCaption().Text(T(L"ВЫСОКИЙ РИСК", L"HIGH RISK"));
        ProductsStatHint().Text(T(L"подтверждённый издатель", L"verified publisher"));
        LicensesStatHint().Text(T(L"полные номера найдены", L"full serials found"));
        CertificatesStatHint().Text(T(L"только открытая часть", L"public part only"));
        RiskStatHint().Text(T(L"критичные продукты", L"critical products"));
        ProductsTitle().Text(T(L"Обнаруженные продукты", L"Detected products"));
        ProductsSubtitle().Text(T(L"Удаляются только записи подтверждённого издателя", L"Only verified-publisher entries can be removed"));
        ProductColumn().Text(T(L"ПРОДУКТ", L"PRODUCT"));
        VersionColumn().Text(T(L"ВЕРСИЯ", L"VERSION"));
        ArchitectureColumn().Text(T(L"АРХИТЕКТУРА", L"ARCHITECTURE"));
        RiskColumn().Text(T(L"РИСК", L"RISK"));
        ProfilesTitle().Text(T(L"Профили для очистки", L"Profiles to clean"));
        ProfilesSubtitle().Text(T(L"Настройки выбранных локальных профилей", L"Settings of selected local profiles"));
        BackupTitle().Text(T(L"Резервная копия", L"Backup"));
        BackupSubtitle().Text(T(L"Создаётся до любых изменений", L"Created before any changes"));
        ChooseBackupText().Text(T(L"Изменить", L"Change"));
        BackupInfo().Text(T(L"Будут созданы licenses.txt, папка certificates, summary.txt, report.json и обезличенный cleanup.log.",
                           L"Creates licenses.txt, a certificates folder, summary.txt, report.json, and a redacted cleanup.log."));
        ValidateBackupPathForUi();
        ShowLicensesText().Text(T(L"Показать и копировать лицензии", L"Show and copy licenses"));
        PlanCaption().Text(T(L"ПЛАН ОПЕРАЦИИ", L"OPERATION PLAN"));
        PlanState().Text(planRevisions_.IsPlanCurrent()
            ? T(L"План проверен", L"Plan reviewed")
            : (planRevisions_.planReady ? T(L"Требуется повторная проверка", L"Review required again")
                                        : T(L"Готово к проверке", L"Ready to review")));
        PlanTargetLabel().Text(T(L"подтверждённых\nцелей очистки", L"verified cleanup\ntargets"));
        PlanStep1().Text(T(L"Создать резервную копию", L"Create a backup"));
        PlanStep1Hint().Text(T(L"лицензии, сертификаты и отчёты", L"licenses, certificates, and reports"));
        PlanStep2().Text(T(L"Запустить штатные деинсталляторы", L"Run registered uninstallers"));
        PlanStep2Hint().Text(T(L"сначала MSI/EXE, без принудительной очистки", L"MSI/EXE first, without forced cleanup"));
        PlanStep3().Text(T(L"Удалить подтверждённые остатки", L"Remove verified residuals"));
        PlanStep3Hint().Text(T(L"только цели проверенного плана", L"verified plan targets only"));
        PlanStep4().Text(T(L"Повторно проверить систему", L"Verify the system again"));
        PlanStep4Hint().Text(T(L"отчёт с точными остатками", L"report with exact residuals"));
        ProtectedTitle().Text(T(L"Защищённые данные не затрагиваются", L"Protected data remains untouched"));
        ProtectedHint().Text(T(L"Закрытые ключи · токены · хранилища Windows", L"Private keys · tokens · Windows stores"));
        CheckPlanText().Text(T(L"Проверить план", L"Review plan"));

        CertSafetyTitle().Text(T(L"Закрытые ключи не экспортируются", L"Private keys are never exported"));
        CertSafetyHint().Text(T(L"Программа показывает только открытую часть сертификатов и наличие ссылки на ключ.",
                                L"The utility shows only the public certificate and whether a private-key reference exists."));
        CertificateSearch().PlaceholderText(T(L"Поиск по владельцу, издателю, профилю или отпечатку", L"Search owner, issuer, profile, or thumbprint"));
        CertSubjectColumn().Text(T(L"КОМУ ВЫДАН", L"ISSUED TO"));
        CertProfileColumn().Text(T(L"ПРОФИЛЬ", L"PROFILE"));
        CertValidColumn().Text(T(L"ДЕЙСТВУЕТ ПО", L"VALID TO"));
        CertKeyColumn().Text(T(L"КЛЮЧ", L"KEY"));
        ExportCertificatesText().Text(T(L"Экспортировать выбранные", L"Export selected"));
        SelectedCertCaption().Text(T(L"ВЫБРАННЫЙ СЕРТИФИКАТ", L"SELECTED CERTIFICATE"));
        CopyThumbprintText().Text(T(L"Копировать отпечаток", L"Copy thumbprint"));

        OfflinePathCaption().Text(T(L"ДИСК ИЛИ ПАПКА WINDOWS", L"DRIVE OR WINDOWS DIRECTORY"));
        OfflinePath().PlaceholderText(T(L"Например, E:\\ или E:\\Windows", L"For example, E:\\ or E:\\Windows"));
        ChooseOfflineText().Text(T(L"Выбрать", L"Browse"));
        ScanOfflineText().Text(T(L"Безопасно сканировать", L"Safe scan"));
        OfflineProductsCaption().Text(T(L"ПРОДУКТЫ", L"PRODUCTS"));
        OfflineLicensesCaption().Text(T(L"ЛИЦЕНЗИИ", L"LICENSES"));
        OfflineCertsCaption().Text(T(L"СЕРТИФИКАТЫ", L"CERTIFICATES"));
        OfflineTargetsCaption().Text(T(L"ЦЕЛИ ОЧИСТКИ", L"CLEANUP TARGETS"));
        OfflineProductsTitle().Text(T(L"Продукты в отключённой Windows", L"Products in offline Windows"));
        OfflineRescueTitle().Text(T(L"Безопасное спасение данных", L"Safe data rescue"));
        OfflineRescueHint().Text(T(L"Сначала сохраняются лицензии и выбранные открытые сертификаты. Отключённая Windows при этом не изменяется.",
                                   L"Licenses and selected public certificates are saved first. The offline Windows installation is not modified."));
        ShowOfflineLicensesText().Text(T(L"Показать и копировать лицензии", L"Show and copy licenses"));
        SaveOfflineText().Text(T(L"Сохранить найденные данные", L"Save rescued data"));
        CleanOfflineText().Text(T(L"Расширенная офлайн-очистка", L"Advanced offline cleanup"));

        ReportPrivacyTitle().Text(T(L"Обезличенный журнал", L"Redacted log"));
        ReportPrivacyHint().Text(T(L"Полные лицензии и персональные имена сертификатов сюда не попадают. Они сохраняются только в отдельных конфиденциальных файлах.",
                                  L"Full licenses and certificate personal names are not shown here. They are written only to separate confidential files."));
        ToggleTechnicalLogText().Text(technicalLogExpanded_
            ? T(L"Скрыть технический журнал", L"Hide technical log")
            : T(L"Показать технический журнал", L"Show technical log"));
        CopyLogText().Text(T(L"Копировать журнал", L"Copy log"));
        OpenReportFolderText().Text(T(L"Открыть папку отчёта", L"Open report folder"));
        OpenJsonText().Text(T(L"Открыть JSON", L"Open JSON"));
        OpenSummaryText().Text(T(L"Открыть сводку", L"Open summary"));
        OpenCleanupLogText().Text(T(L"Открыть cleanup.log", L"Open cleanup.log"));
        OpenOfflineSummaryText().Text(T(L"Открыть offline-summary.txt", L"Open offline-summary.txt"));
        OpenOfflineReportText().Text(T(L"Открыть offline-report.json", L"Open offline-report.json"));
        OpenOfflineDiagnosticsText().Text(T(L"Открыть offline-diagnostics.txt", L"Open offline-diagnostics.txt"));
        OpenOfflineResultText().Text(T(L"Открыть offline-result.txt", L"Open offline-result.txt"));
        OpenOfflineCleanupLogText().Text(T(L"Открыть offline-cleanup.log", L"Open offline-cleanup.log"));
        OpenLicensesText().Text(T(L"Открыть licenses.txt", L"Open licenses.txt"));
        OpenCertificatesTextText().Text(T(L"Открыть certificates.txt", L"Open certificates.txt"));
        OpenCertificatesBundleText().Text(T(L"Открыть certificates.p7b", L"Open certificates.p7b"));
        OpenRecoveryMapText().Text(T(L"Открыть recovery-map.txt", L"Open recovery-map.txt"));
        ReportOperationsCaption().Text(T(L"ОПЕРАЦИИ", L"OPERATIONS"));
        ReportFailuresCaption().Text(T(L"ОШИБКИ", L"FAILURES"));
        ReportResidualsCaption().Text(T(L"ОСТАТКИ", L"RESIDUALS"));
        InterfaceSettingsTitle().Text(T(L"Интерфейс", L"Interface"));
        SettingsLanguageTitle().Text(T(L"Язык интерфейса", L"Interface language"));
        SettingsLanguageHint().Text(T(L"Переключение не сбрасывает результаты сканирования и выбор.",
                                      L"Changing language preserves scan results and selections."));
        RememberWindowToggle().Header(box_value(T(L"Запоминать размер окна и последнюю страницу", L"Remember window size and last page")));
        ReduceMotionToggle().Header(box_value(T(L"Уменьшить анимацию интерфейса", L"Reduce interface motion")));
        ThemeTitle().Text(T(L"Тема", L"Theme"));
        settingsSync_ = true;
        ThemeCombo().Items().Clear();
        for (auto const& label : {T(L"Тёмная", L"Dark"), T(L"Системная", L"System"), T(L"Светлая", L"Light")})
        {
            ComboBoxItem item;
            item.Content(box_value(label));
            ThemeCombo().Items().Append(item);
        }
        ThemeCombo().SelectedIndex(static_cast<int>(themeMode_));
        settingsSync_ = false;
        ThemeHint().Text(T(L"Тёмная, светлая или системная тема; высокий контраст определяется Windows.",
                           L"Dark, light, or system theme; high contrast is detected from Windows."));
        ResetSettingsText().Text(T(L"Сбросить настройки интерфейса", L"Reset interface settings"));
        SelectFilteredCertificatesText().Text(T(L"Выбрать отфильтрованные", L"Select filtered"));
        DeselectAllCertificatesText().Text(T(L"Снять выбор с отфильтрованных", L"Deselect filtered"));
        OfflineCertificatesTitle().Text(T(L"Открытые сертификаты для сохранения", L"Public certificates to save"));
        OfflineDiagnosticsButtonText().Text(T(L"Открыть диагностику", L"Open diagnostics"));
        AboutDescription().Text(T(L"Открытая portable-утилита для контролируемого удаления продуктов CryptoPro, спасения лицензий и экспорта открытой части сертификатов.",
                                  L"Open portable utility for controlled CryptoPro removal, license rescue, and public-certificate export."));
        AboutDisclaimer().Text(T(L"Это неофициальный проект. Он не связан с ООО «КРИПТО-ПРО» и не одобрен правообладателем продуктов CryptoPro.",
                                 L"This is an unofficial project. It is not affiliated with or endorsed by Crypto-Pro LLC."));
        FooterVersion().Text(T(L"КОД АЛЕКСАНДРОВА · ", L"CODE ALEXANDROV · ") + std::wstring(cpc::kVersion));
        AboutVersion().Text(T(L"Версия ", L"Version ") + std::wstring(cpc::kVersion));
        AboutPlatformInfo().Text(T(
            L"Редакция: Modern x64 · C++17 / WinUI 3\nМинимальная ОС: Windows 10 версии 1809\nВ комплекте также доступна отдельная Legacy x86 для Windows 7 SP1–Windows 11.",
            L"Edition: Modern x64 · C++17 / WinUI 3\nMinimum OS: Windows 10 version 1809\nA separate Legacy x86 edition for Windows 7 SP1–Windows 11 is also included."));
        AboutAuthor().Text(T(L"Автор: Кирилл Александров · лицензия MIT", L"Author: Kirill Alexandrov · MIT License"));
        ComputeHashText().Text(T(L"Вычислить SHA-256", L"Compute SHA-256"));
        ShowLocationText().Text(T(L"Показать расположение", L"Show location"));
        CopyExecutablePathText().Text(T(L"Копировать путь", L"Copy path"));
        OpenExecutableFolderText().Text(T(L"Открыть папку программы", L"Open program folder"));
        AboutSupportButton().Content(box_value(T(L"Поддержать проект", L"Support the project")));
        UpdateAboutSecurityState();
        using Microsoft::UI::Xaml::Automation::AutomationProperties;
        AutomationProperties::SetName(CompactOverview(), T(L"Обзор", L"Overview"));
        AutomationProperties::SetName(CompactCertificates(), T(L"Открытые сертификаты", L"Public certificates"));
        AutomationProperties::SetName(CompactOffline(), T(L"Офлайн-Windows", L"Offline Windows"));
        AutomationProperties::SetName(CompactReports(), T(L"Журнал и отчёты", L"Log and reports"));
        AutomationProperties::SetName(CompactSettings(), T(L"Настройки", L"Settings"));
        AutomationProperties::SetName(CompactAbout(), T(L"О программе", L"About"));
        RenderStatus();

        UpdatePageHeader();
        PopulateProducts();
        PopulateProfiles();
        PopulateCertificateFilters();
        PopulateCertificates();
        PopulateOfflineScan();
        UpdateSelectedCounts();
        const bool recognizableScanSummary = logText_.find(L'\n') == std::wstring::npos &&
            (logText_.rfind(L"Безопасное сканирование завершено:", 0) == 0 ||
             logText_.rfind(L"Safe scan completed:", 0) == 0);
        if (liveScanSummaryOnly_ || recognizableScanSummary)
        {
            logText_ = LiveScanSummaryText();
            liveScanSummaryOnly_ = true;
        }
        UpdateReportsPage();
    }

    void MainWindow::LanguageCombo_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (languageSync_) return;
        language_ = LanguageCombo().SelectedIndex() == 0 ? cpc::Language::Russian : cpc::Language::English;
        languageSync_ = true;
        SettingsLanguageCombo().SelectedIndex(LanguageCombo().SelectedIndex());
        languageSync_ = false;
        ApplyLanguage();
        ApplyTheme();
        SaveSettings();
    }

    void MainWindow::ThemeCombo_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (settingsSync_ || !uiReady_) return;
        themeMode_ = cpc::NormalizeThemeMode(static_cast<DWORD>(ThemeCombo().SelectedIndex()));
        ApplyTheme();
        SaveSettings();
    }

    void MainWindow::SettingsToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (settingsSync_ || !uiReady_) return;
        rememberWindow_ = RememberWindowToggle().IsOn();
        reduceMotion_ = ReduceMotionToggle().IsOn();
        ApplyAdaptiveLayout(WindowRoot().ActualWidth());
        SetBusy(busy_, L"", statusPercent_);
        SaveSettings();
    }

    void MainWindow::ResetSettings_Click(IInspectable const&, RoutedEventArgs const&)
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
        rememberWindow_ = true;
        reduceMotion_ = false;
        themeMode_ = cpc::ThemeMode::Dark;
        settingsSync_ = true;
        ThemeCombo().SelectedIndex(0);
        RememberWindowToggle().IsOn(true);
        ReduceMotionToggle().IsOn(false);
        settingsSync_ = false;
        ApplyTheme();
        ApplyAdaptiveLayout(WindowRoot().ActualWidth());
        FooterStatus().Text(T(L"Настройки интерфейса сброшены.", L"Interface settings were reset."));
    }

    void MainWindow::SettingsLanguageCombo_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (languageSync_) return;
        language_ = SettingsLanguageCombo().SelectedIndex() == 0 ? cpc::Language::Russian : cpc::Language::English;
        languageSync_ = true;
        LanguageCombo().SelectedIndex(SettingsLanguageCombo().SelectedIndex());
        languageSync_ = false;
        ApplyLanguage();
        ApplyTheme();
        SaveSettings();
    }

    void MainWindow::SetBusy(bool busy, std::wstring const& message, int percent)
    {
        // Ignore delayed progress callbacks that arrive after their operation
        // has already ended and the command gate returned to Idle.
        if (busy && operationGate_.idle()) return;
        busy_ = busy;
        GlobalProgress().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        const bool animateIndeterminate = percent < 0 && !ReduceMotionEffective();
        GlobalProgress().IsIndeterminate(animateIndeterminate);
        if (percent >= 0) GlobalProgress().Value(percent);
        else if (!animateIndeterminate) GlobalProgress().Value(50);
        if (busy)
        {
            statusDetail_ = message;
            SetSemanticStatus(UiStatusKind::Working, operationGate_.current(), percent);
        }
        RefreshCommandStates();
    }

    bool MainWindow::ReduceMotionEffective() const
    {
        return reduceMotion_ || (uiSettings_ && !uiSettings_.AnimationsEnabled());
    }

    void MainWindow::SetSemanticStatus(UiStatusKind kind, cpc::UiOperation source, int percent)
    {
        statusKind_ = kind;
        statusSource_ = source;
        statusPercent_ = percent;
        RenderStatus();
    }

    void MainWindow::RenderStatus()
    {
        std::wstring header;
        std::wstring footer;
        wchar_t const* brush = L"TertiaryTextBrush";
        switch (statusKind_)
        {
        case UiStatusKind::Working:
            header = T(L"Выполняется", L"Working");
            brush = L"AccentBrush";
            switch (statusSource_)
            {
            case cpc::UiOperation::LiveScan: footer = T(L"Безопасное сканирование системы…", L"Safely scanning the system…"); break;
            case cpc::UiOperation::BuildingPlan: footer = T(L"Построение проверяемого плана…", L"Building a verifiable plan…"); break;
            case cpc::UiOperation::ExportingCertificates: footer = T(L"Экспорт открытых сертификатов…", L"Exporting public certificates…"); break;
            case cpc::UiOperation::OfflineScan: footer = T(L"Сканирование подключённой Windows…", L"Scanning attached Windows…"); break;
            case cpc::UiOperation::SavingOfflineData: footer = T(L"Сохранение найденных данных…", L"Saving rescued data…"); break;
            case cpc::UiOperation::ForcedCleanup: footer = T(L"Ограниченная принудительная очистка…", L"Limited forced cleanup…"); break;
            case cpc::UiOperation::OfflineCleanup: footer = T(L"Офлайн-очистка и проверка…", L"Offline cleanup and verification…"); break;
            case cpc::UiOperation::ComputingHash: footer = T(L"Вычисление SHA-256 текущего EXE…", L"Computing the current EXE SHA-256…"); break;
            default: footer = T(L"Удаление и повторная проверка…", L"Removal and verification…"); break;
            }
            break;
        case UiStatusKind::ScanComplete:
            header = T(L"Готово", L"Ready");
            footer = statusSource_ == cpc::UiOperation::OfflineScan
                ? T(L"Офлайн-сканирование завершено без изменений.", L"Offline scan completed without changes.")
                : T(L"Безопасное сканирование завершено без изменений.", L"Safe scan completed without changes.");
            brush = L"SuccessBrush";
            break;
        case UiStatusKind::ExportComplete:
            header = T(L"Сохранено", L"Saved");
            footer = statusSource_ == cpc::UiOperation::ExportingCertificates
                ? T(L"Сохранена только открытая часть выбранных сертификатов.", L"Only the public part of selected certificates was saved.")
                : T(L"Найденные данные сохранены без изменения исходной системы.", L"Rescued data was saved without changing the source system.");
            brush = L"SuccessBrush";
            break;
        case UiStatusKind::Cancelled:
            header = T(L"Отменено", L"Cancelled");
            footer = T(L"Готово. Изменения не выполнялись.", L"Ready. No changes were made.");
            break;
        case UiStatusKind::PlanReady:
            header = T(L"План готов", L"Plan ready"); footer = T(L"План проверен и соответствует текущему выбору.", L"The reviewed plan matches the current selection."); brush = L"SuccessBrush"; break;
        case UiStatusKind::PlanStale:
            header = T(L"План устарел", L"Plan stale"); footer = T(L"Выбор изменился — проверьте план снова.", L"The selection changed — review the plan again."); brush = L"WarningBrush"; break;
        case UiStatusKind::Warning:
            header = T(L"Внимание", L"Warning"); footer = T(L"Операция завершена с предупреждением.", L"The operation completed with a warning."); brush = L"WarningBrush"; break;
        case UiStatusKind::Partial:
            header = T(L"Частично", L"Partial"); footer = T(L"Операция завершена частично; подробности находятся в отчёте.", L"The operation completed partially; see the report for details."); brush = L"WarningBrush"; break;
        case UiStatusKind::RestartRequired:
            header = T(L"Нужна перезагрузка", L"Restart required"); footer = T(L"Продолжение зарегистрировано; перезагрузите Windows вручную.", L"Continuation is registered; restart Windows manually."); brush = L"WarningBrush"; break;
        case UiStatusKind::Error:
            header = T(L"Ошибка", L"Error"); footer = T(L"Операция не выполнена. Подробности добавлены в журнал.", L"The operation failed. Details were added to the log."); brush = L"DangerBrush"; break;
        case UiStatusKind::Success:
            header = T(L"Готово", L"Done"); footer = T(L"Операция успешно завершена.", L"The operation completed successfully."); brush = L"SuccessBrush"; break;
        default:
            header = T(L"Готово", L"Ready"); footer = T(L"Готово. Изменения не выполняются.", L"Ready. No changes are being made."); break;
        }
        if (!statusDetail_.empty()) footer = statusDetail_;
        HeaderStatus().Text(header);
        FooterStatus().Text(footer);
        const auto statusBrush = ThemeBrush(brush);
        HeaderStatusDot().Fill(statusBrush);
        FooterStatusDot().Fill(statusBrush);
    }

    cpc::UiOperationToken MainWindow::BeginOperation(cpc::UiOperation operation, std::wstring const& message, int percent)
    {
        const auto token = operationGate_.TryBeginToken(operation);
        if (!token) return {};
        SetBusy(true, message, percent);
        return token;
    }

    void MainWindow::UpdateOperation(cpc::UiOperationToken token, std::wstring const& message, int percent)
    {
        if (operationGate_.IsCurrent(token)) SetBusy(true, message, percent);
    }

    void MainWindow::EndOperation(cpc::UiOperationToken token, std::wstring const& message, UiStatusKind status)
    {
        const auto source = operationGate_.current();
        if (!operationGate_.End(token)) return;
        statusDetail_ = message;
        SetBusy(false, message);
        SetSemanticStatus(status, source);
    }

    void MainWindow::RefreshCommandStates()
    {
        const bool idle = operationGate_.idle();
        const auto operation = operationGate_.current();
        const bool destructive = operation == cpc::UiOperation::LiveCleanup ||
                                 operation == cpc::UiOperation::ForcedCleanup ||
                                 operation == cpc::UiOperation::OfflineCleanup ||
                                 operation == cpc::UiOperation::ResumeCleanup;
        const bool navigationAllowed = idle || !destructive;
        const bool offlineCurrent = OfflineStateCurrent();
        const size_t selectedProducts = static_cast<size_t>(std::count_if(
            scan_.products.begin(), scan_.products.end(),
            [](cpc::InstalledProduct const& product) { return product.selected; }));
        RescanButton().IsEnabled(idle);
        BackupPath().IsEnabled(idle);
        ChooseBackupButton().IsEnabled(idle);
        ShowLicensesButton().IsEnabled(idle && !scan_.licenses.empty());
        CheckPlanButton().IsEnabled(idle && selectedProducts > 0);
        CertificateSearch().IsEnabled(idle);
        CertificateProfileFilter().IsEnabled(idle);
        CertificateKeyFilter().IsEnabled(idle);
        CertificateValidityFilter().IsEnabled(idle);
        CertificateSort().IsEnabled(idle);
        SelectFilteredCertificatesButton().IsEnabled(idle);
        DeselectFilteredCertificatesButton().IsEnabled(idle);
        ExportCertificatesButton().IsEnabled(idle && cpc::CountSelectedCertificates(scan_.certificates) > 0);
        ScanOfflineButton().IsEnabled(idle);
        OfflinePath().IsEnabled(idle);
        ChooseOfflineButton().IsEnabled(idle);
        ShowOfflineLicensesButton().IsEnabled(idle && offlineCurrent && !offline_.scan.licenses.empty());
        SaveOfflineButton().IsEnabled(idle && offlineCurrent);
        CleanOfflineButton().IsEnabled(idle && offlineCurrent && offlineProductsConfirmed_ && offline_.cleanupCapable &&
            !offline_.scan.products.empty() && std::all_of(offline_.scan.products.begin(), offline_.scan.products.end(),
                [](cpc::InstalledProduct const& product) { return product.selected; }));
        LanguageCombo().IsEnabled(idle);
        SettingsLanguageCombo().IsEnabled(idle);
        ThemeCombo().IsEnabled(idle);
        RememberWindowToggle().IsEnabled(idle);
        ReduceMotionToggle().IsEnabled(idle);
        ResetSettingsButton().IsEnabled(idle);
        ProductsPanel().IsHitTestVisible(idle);
        ProfilesPanel().IsHitTestVisible(idle);
        CertificatesPanel().IsHitTestVisible(idle);
        OfflineProductsPanel().IsHitTestVisible(idle);
        OfflineCertificatesPanel().IsHitTestVisible(idle);
        ComputeHashButton().IsEnabled(idle);
        ShowLocationButton().IsEnabled(idle);
        CopyExecutablePathButton().IsEnabled(idle);
        OpenExecutableFolderButton().IsEnabled(idle);
        for (auto const& button : {ToggleTechnicalLogButton(), CopyLogButton(), OpenReportFolderButton(), OpenJsonButton(), OpenSummaryButton(),
                                  OpenCleanupLogButton(), OpenOfflineSummaryButton(), OpenOfflineReportButton(),
                                  OpenOfflineDiagnosticsButton(), OpenOfflineResultButton(), OpenOfflineCleanupLogButton(),
                                  OpenLicensesButton(), OpenCertificatesTextButton(), OpenCertificatesBundleButton(), OpenRecoveryMapButton(),
                                  AboutGitHubButton(), AboutWebsiteButton(), AboutSupportButton()})
            button.IsEnabled(idle);
        GitHubLink().IsEnabled(idle);
        WebsiteLink().IsEnabled(idle);
        SupportLink().IsEnabled(idle);
        Navigation().IsEnabled(navigationAllowed);
        for (auto const& button : {CompactOverview(), CompactCertificates(), CompactOffline(), CompactReports(),
                                  CompactSettings(), CompactAbout()})
            button.IsEnabled(navigationAllowed);
    }

    void MainWindow::InvalidatePlan()
    {
        plan_ = {};
        PlanTargetCount().Text(L"—");
        if (planRevisions_.CurrentState() == cpc::PlanRevisionTracker::State::Stale)
        {
            PlanState().Text(T(L"Требуется повторная проверка", L"Review required again"));
            SetSemanticStatus(UiStatusKind::PlanStale);
        }
        else
        {
            PlanState().Text(T(L"Готово к проверке", L"Ready to review"));
            if (!busy_) SetSemanticStatus(UiStatusKind::Idle);
        }
    }

    bool MainWindow::ValidateBackupPathForUi()
    {
        const std::wstring current = BackupPath().Text().c_str();
        if (current != backupValidationPath_) ScheduleBackupValidation();
        RenderBackupValidation();
        return appliedBackupValidationRevision_ == backupValidationRevision_ && backupValidation_.ok();
    }

    void MainWindow::ScheduleBackupValidation()
    {
        backupValidationTimer_.Stop();
        backupValidationPath_ = BackupPath().Text().c_str();
        ++backupValidationRevision_;
        appliedBackupValidationRevision_ = 0;
        backupValidation_ = cpc::InspectBackupPath(backupValidationPath_);
        RenderBackupValidation();
        if (backupValidation_.canProbe()) backupValidationTimer_.Start();
        RefreshCommandStates();
    }

    void MainWindow::RenderBackupValidation()
    {
        const auto& validation = backupValidation_;
        std::wstring text;
        wchar_t const* brush = L"DangerBrush";
        switch (validation.state)
        {
        case cpc::BackupFolderState::Available:
            text = T(L"Папка доступна", L"Folder is available") +
                   T(L"\nСвободно: ", L"\nFree: ") + FormatByteCount(validation.freeBytes) +
                   T(L" · ожидаемый минимум: ", L" · expected minimum: ") + FormatByteCount(validation.requiredBytes);
            brush = L"SuccessBrush";
            break;
        case cpc::BackupFolderState::ReadyForProbe:
            text = T(L"Проверка доступа к папке…", L"Checking folder access…");
            brush = L"SecondaryTextBrush";
            break;
        case cpc::BackupFolderState::InsufficientSpace:
            text = T(L"Недостаточно свободного места", L"Not enough free space") +
                   T(L"\nСвободно: ", L"\nFree: ") + FormatByteCount(validation.freeBytes) +
                   T(L" · ожидаемый минимум: ", L" · expected minimum: ") + FormatByteCount(validation.requiredBytes);
            brush = L"WarningBrush";
            break;
        case cpc::BackupFolderState::SameVolume:
            text = T(L"Для офлайн-очистки нужен другой том", L"Offline cleanup requires another volume");
            break;
        case cpc::BackupFolderState::UnsafeLocation:
            text = T(L"Эта папка не подходит для резервной копии", L"This location is unsuitable for a backup");
            break;
        case cpc::BackupFolderState::EmptyPath:
            text = T(L"Выберите папку резервной копии", L"Choose a backup folder");
            break;
        default:
            text = T(L"Нет доступа на запись", L"The folder is not writable");
            break;
        }
        BackupValidationStatus().Text(text);
        BackupValidationStatus().Foreground(ThemeBrush(brush));
    }

    winrt::fire_and_forget MainWindow::ProbeBackupPathAsync(unsigned long long revision, std::wstring path)
    {
        auto lifetime = get_strong();
        apartment_context uiThread;
        cpc::BackupFolderValidation validation;
        co_await resume_background();
        validation = cpc::ProbeBackupFolder(path);
        co_await uiThread;
        if (revision != backupValidationRevision_ || path != backupValidationPath_) co_return;
        backupValidation_ = std::move(validation);
        appliedBackupValidationRevision_ = revision;
        RenderBackupValidation();
        RefreshCommandStates();
    }

    void MainWindow::ReportAsyncFailure(std::wstring const& context, std::exception_ptr failure,
                                        cpc::UiOperationToken token,
                                        cpc::ScanResult const* redactionContext) noexcept
    {
        std::wstring detail = T(L"Неизвестная ошибка.", L"Unknown error.");
        try
        {
            if (failure) std::rethrow_exception(failure);
        }
        catch (winrt::hresult_error const& error)
        {
            detail = error.message().c_str();
        }
        catch (std::exception const& error)
        {
            detail = cpc::FromUtf8(error.what());
        }
        catch (...) {}
        const cpc::ScanResult redactionScan = redactionContext ? *redactionContext : scan_;
        auto weak = get_weak();
        DispatcherQueue().TryEnqueue([weak, context, detail, token, redactionScan]()
        {
            if (auto self = weak.get())
            {
                if (token && !self->operationGate_.IsCurrent(token)) return;
                if (token)
                {
                    self->liveScanRunning_ = false;
                    self->offlineScanRunning_ = false;
                    self->EndOperation(token, self->T(L"Операция остановлена из-за ошибки.", L"Operation stopped because of an error."),
                                       UiStatusKind::Error);
                }
                else self->SetSemanticStatus(UiStatusKind::Error, cpc::UiOperation::Idle);
                self->AppendLogLine(cpc::RedactSensitiveText(
                    self->T(L"Ошибка: ", L"Error: ") + context + L" — " + detail, redactionScan));
                self->ShowMessage(self->T(L"Операция не выполнена", L"Operation failed"), context + L"\n\n" + detail);
            }
        });
    }

    void MainWindow::LogExecution(cpc::ExecutionResult const& execution, cpc::ScanResult const& redactionContext)
    {
        for (auto const& operation : execution.operations)
        {
            std::wstring prefix;
            switch (operation.outcome)
            {
            case cpc::Outcome::Succeeded: prefix = L"[OK] "; break;
            case cpc::Outcome::RebootRequired: prefix = L"[REBOOT] "; break;
            case cpc::Outcome::Skipped: prefix = L"[SKIP] "; break;
            case cpc::Outcome::Failed: prefix = L"[ERROR] "; break;
            }
            AppendLogLine(cpc::RedactSensitiveText(prefix + operation.action + L": " + operation.target + L" — " + operation.message,
                                                    redactionContext));
        }
    }

    std::wstring MainWindow::LiveScanSummaryText() const
    {
        return T(L"Безопасное сканирование завершено: продуктов — ", L"Safe scan completed: products — ") +
               std::to_wstring(scan_.products.size()) + T(L", лицензий — ", L", licenses — ") +
               std::to_wstring(scan_.licenses.size()) + T(L", открытых сертификатов — ", L", public certificates — ") +
               std::to_wstring(scan_.certificates.size()) + L".";
    }

    void MainWindow::AppendLogLine(std::wstring const& line)
    {
        if (!logText_.empty()) logText_ += L"\r\n";
        logText_ += line;
        liveScanSummaryOnly_ = false;
        LogText().Text(logText_);
        UpdateReportsPage();
    }

    void MainWindow::UpdateReportsPage()
    {
        if (!uiReady_) return;
        const bool hasOutput = outputSession_.kind != OutputSessionKind::None && !outputSession_.folder.empty();
        LogText().Text(logText_);
        ReportOperationsValue().Text(hasOutput ? std::to_wstring(outputSession_.operationCount) : L"—");
        ReportFailuresValue().Text(hasOutput ? std::to_wstring(outputSession_.failureCount) : L"—");
        ReportResidualsValue().Text(hasOutput ? std::to_wstring(outputSession_.residualCount) : L"—");
        const bool rawLogUseful = logText_.find(L'\n') != std::wstring::npos ||
            logText_.find(L"[ERROR]") != std::wstring::npos || logText_.find(L"Ошибка:") != std::wstring::npos ||
            logText_.find(L"Error:") != std::wstring::npos;
        LogText().Visibility((rawLogUseful || technicalLogExpanded_) ? Visibility::Visible : Visibility::Collapsed);
        ToggleTechnicalLogButton().Visibility(logText_.empty() ? Visibility::Collapsed : Visibility::Visible);
        ToggleTechnicalLogText().Text(technicalLogExpanded_
            ? T(L"Скрыть технический журнал", L"Hide technical log")
            : T(L"Показать технический журнал", L"Show technical log"));
        CopyLogButton().Visibility(logText_.empty() ? Visibility::Collapsed : Visibility::Visible);
        OpenReportFolderButton().Visibility(hasOutput ? Visibility::Visible : Visibility::Collapsed);
        auto fileVisible = [this](wchar_t const* name) {
            return outputSession_.files.count(name) ? Visibility::Visible : Visibility::Collapsed;
        };
        OpenJsonButton().Visibility(fileVisible(L"report.json"));
        OpenSummaryButton().Visibility(fileVisible(L"summary.txt"));
        OpenCleanupLogButton().Visibility(fileVisible(L"cleanup.log"));
        OpenOfflineSummaryButton().Visibility(fileVisible(L"offline-summary.txt"));
        OpenOfflineReportButton().Visibility(fileVisible(L"offline-report.json"));
        OpenOfflineDiagnosticsButton().Visibility(fileVisible(L"offline-diagnostics.txt"));
        OpenOfflineResultButton().Visibility(fileVisible(L"offline-result.txt"));
        OpenOfflineCleanupLogButton().Visibility(fileVisible(L"offline-cleanup.log"));
        OpenLicensesButton().Visibility(fileVisible(L"licenses.txt"));
        OpenCertificatesTextButton().Visibility(fileVisible(L"certificates.txt"));
        OpenCertificatesBundleButton().Visibility(fileVisible(L"certificates.p7b"));
        OpenRecoveryMapButton().Visibility(fileVisible(L"recovery-map.txt"));
        if (!hasOutput)
        {
            ReportFilesText().Text(T(L"Отчётов пока нет. Они появятся после сохранения данных или очистки; безопасное сканирование не создаёт конфиденциальные файлы.",
                                     L"No reports yet. They appear after saving data or cleanup; a safe scan creates no confidential files."));
            ReportTimelineText().Text((logText_.empty() ? std::wstring() : logText_ + L"\n") +
                T(L"Сканирование: безопасно · План: ", L"Scan: safe · Plan: ") +
                (planRevisions_.IsPlanCurrent() ? T(L"проверен", L"reviewed") : T(L"не выполнялся или устарел", L"not run or stale")));
            return;
        }
        std::wostringstream files;
        const size_t slash = outputSession_.folder.find_last_of(L"\\/");
        const std::wstring leaf = slash == std::wstring::npos ? outputSession_.folder : outputSession_.folder.substr(slash + 1);
        files << T(L"Сессия: ", L"Session: ") << leaf << L"\n";
        for (auto const& file : outputSession_.files) files << L"• " << file.first << L"\n";
        ReportFilesText().Text(files.str());
        const std::array<std::wstring, 8> stageNames{
            T(L"Сканирование", L"Scan"), T(L"План", L"Plan"), T(L"Резервная копия", L"Backup"),
            T(L"Деинсталляторы", L"Uninstallers"), T(L"Очистка остатков", L"Residual cleanup"),
            T(L"Проверка", L"Verification"), T(L"Отчёт", L"Report"), T(L"Перезагрузка", L"Restart")};
        auto outcomeText = [this](StageOutcome outcome) {
            switch (outcome)
            {
            case StageOutcome::Running: return T(L"выполняется", L"running");
            case StageOutcome::Succeeded: return T(L"готово", L"succeeded");
            case StageOutcome::Skipped: return T(L"пропущено", L"skipped");
            case StageOutcome::Failed: return T(L"ошибка", L"failed");
            case StageOutcome::RestartDeferred: return T(L"ожидается", L"deferred");
            default: return T(L"не запускалось", L"not started");
            }
        };
        std::wstring timeline;
        for (size_t index = 0; index < outputSession_.stages.size(); ++index)
        {
            if (!timeline.empty()) timeline += L" · ";
            timeline += stageNames[index] + L": " + outcomeText(outputSession_.stages[index]);
        }
        ReportTimelineText().Text(timeline);
    }

    void MainWindow::SetOutputSession(OutputSessionKind kind, std::wstring const& folder,
                                      cpc::ExecutionResult const* execution, size_t residualCount)
    {
        outputSession_ = {};
        outputSession_.kind = kind;
        outputSession_.folder = folder;
        outputSession_.residualCount = residualCount;
        outputSession_.stages.fill(StageOutcome::Skipped);
        outputSession_.stages[0] = StageOutcome::Succeeded;
        outputSession_.stages[6] = StageOutcome::Succeeded;
        if (execution)
        {
            outputSession_.operationCount = execution->operations.size();
            outputSession_.failureCount = static_cast<size_t>(std::count_if(execution->operations.begin(), execution->operations.end(),
                [](cpc::OperationRecord const& operation) { return operation.outcome == cpc::Outcome::Failed; }));
            outputSession_.restartRequired = execution->rebootRequired;
        }
        if (kind == OutputSessionKind::LiveCleanup || kind == OutputSessionKind::ResumeCleanup)
        {
            outputSession_.stages[1] = StageOutcome::Succeeded;
            outputSession_.stages[2] = StageOutcome::Succeeded;
            outputSession_.stages[3] = outputSession_.failureCount ? StageOutcome::Failed : StageOutcome::Succeeded;
            outputSession_.stages[4] = execution && execution->residualCleanupDeferred
                ? StageOutcome::RestartDeferred : (outputSession_.failureCount ? StageOutcome::Failed : StageOutcome::Succeeded);
            outputSession_.stages[5] = StageOutcome::Succeeded;
        }
        else if (kind == OutputSessionKind::OfflineCleanup)
        {
            outputSession_.stages[1] = StageOutcome::Succeeded;
            outputSession_.stages[2] = StageOutcome::Succeeded;
            outputSession_.stages[4] = outputSession_.failureCount ? StageOutcome::Failed : StageOutcome::Succeeded;
            outputSession_.stages[5] = StageOutcome::Succeeded;
        }
        else if (kind == OutputSessionKind::OfflineRescue)
            outputSession_.stages[2] = StageOutcome::Succeeded;
        outputSession_.stages[7] = outputSession_.restartRequired ? StageOutcome::RestartDeferred : StageOutcome::Skipped;
        for (auto const* name : {L"licenses.txt", L"certificates.txt", L"certificates.p7b", L"summary.txt", L"report.json", L"cleanup.log", L"offline-summary.txt",
                                L"offline-report.json", L"offline-diagnostics.txt", L"offline-result.txt", L"offline-cleanup.log",
                                L"recovery-map.txt", L"emergency-cleanup.log"})
        {
            const std::wstring path = folder + L"\\" + name;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) outputSession_.files.emplace(name, path);
        }
        UpdateReportsPage();
    }

    void MainWindow::Rescan_Click(IInspectable const&, RoutedEventArgs const&)
    {
        StartLiveScan();
    }

    winrt::fire_and_forget MainWindow::StartLiveScan()
    {
        cpc::UiOperationToken token;
        try
        {
        token = BeginOperation(cpc::UiOperation::LiveScan,
                            T(L"Безопасное сканирование системы…", L"Safely scanning the system…"));
        if (!token) co_return;
        liveScanRunning_ = true;
        ProductsEmpty().Visibility(Visibility::Visible);
        ProductsEmptyText().Text(T(L"Выполняется безопасное сканирование…", L"Running a safe scan…"));
        auto strong = get_strong();
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        apartment_context uiThread;
        const auto language = language_;
        auto result = std::make_shared<cpc::ScanResult>();
        std::exception_ptr failure;
        co_await resume_background();
        try
        {
            *result = cpc::ScanSystem(language,
                [weak, dispatcher, token](std::wstring const& message, int percent)
                {
                    dispatcher.TryEnqueue([weak, message, percent, token]()
                    {
                        if (auto self = weak.get()) self->UpdateOperation(token, message, percent);
                    });
                });
        }
        catch (...) { failure = std::current_exception(); }
        co_await uiThread;
        if (failure)
        {
            ReportAsyncFailure(T(L"Безопасное сканирование", L"Safe scan"), failure, token);
            co_return;
        }
        scan_ = std::move(*result);
        plan_ = {};
        planRevisions_.ScanChanged();
        liveScanRunning_ = false;
        PopulateLiveScan();
        EndOperation(token, T(L"Безопасное сканирование завершено без изменений.",
                              L"Safe scan completed without changes."), UiStatusKind::ScanComplete);
        const std::wstring scanSummary = LiveScanSummaryText();
        if (logText_.empty() || liveScanSummaryOnly_)
        {
            logText_ = scanSummary;
            liveScanSummaryOnly_ = true;
            LogText().Text(logText_);
            UpdateReportsPage();
        }
        else AppendLogLine(scanSummary);
        }
        catch (...) { ReportAsyncFailure(T(L"Безопасное сканирование", L"Safe scan"), std::current_exception(), token); }
    }

    void MainWindow::PopulateLiveScan()
    {
        ProductsStat().Text(std::to_wstring(scan_.products.size()));
        LicensesStat().Text(std::to_wstring(scan_.licenses.size()));
        CertificatesStat().Text(std::to_wstring(scan_.certificates.size()));
        ShowLicensesButton().IsEnabled(!scan_.licenses.empty());
        const size_t risks = static_cast<size_t>(std::count_if(scan_.products.begin(), scan_.products.end(),
            [](cpc::InstalledProduct const& product) { return product.risk == cpc::RiskLevel::High; }));
        RiskStat().Text(std::to_wstring(risks));
        PopulateProducts();
        PopulateProfiles();
        PopulateCertificateFilters();
        PopulateCertificates();
        UpdateSelectedCounts();
    }

    void MainWindow::PopulateProducts()
    {
        ProductsPanel().Children().Clear();
        ProductsEmpty().Visibility(scan_.products.empty() ? Visibility::Visible : Visibility::Collapsed);
        ProductsEmptyText().Text(T(L"Подтверждённые продукты CryptoPro не найдены.", L"No verified CryptoPro products were found."));
        auto weak = get_weak();
        for (size_t index = 0; index < scan_.products.size(); ++index)
        {
            auto const& product = scan_.products[index];
            Grid row;
            row.ColumnSpacing(10);
            row.ColumnDefinitions().Append(Column(32, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(1, GridUnitType::Star));
            row.ColumnDefinitions().Append(Column(110, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(95, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(90, GridUnitType::Pixel));

            CheckBox check;
            check.IsThreeState(false);
            check.IsChecked(box_value(product.selected).as<Windows::Foundation::IReference<bool>>());
            check.VerticalAlignment(VerticalAlignment::Center);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(check,
                T(L"Выбрать офлайн-продукт: ", L"Select offline product: ") + product.displayName);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(check,
                T(L"Любое изменение выбранного набора требует повторного подтверждения.",
                  L"Any selection change requires confirming the product set again."));
            const std::wstring productAutomationName = product.displayName + L"; " + product.version + L"; " +
                product.architecture + L"; " + (product.risk == cpc::RiskLevel::High ? T(L"высокий риск", L"high risk") : T(L"обычный риск", L"normal risk"));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(check,
                T(L"Выбрать продукт: ", L"Select product: ") + productAutomationName);
            check.Click([weak, index](IInspectable const& sender, RoutedEventArgs const&)
            {
                if (auto self = weak.get())
                {
                    if (!self->operationGate_.idle()) { self->PopulateProducts(); return; }
                    auto state = sender.as<CheckBox>().IsChecked();
                    self->scan_.products[index].selected = state && state.Value();
                    self->planRevisions_.ProductsChanged();
                    self->InvalidatePlan();
                    self->UpdateSelectedCounts();
                }
            });
            row.Children().Append(check);

            StackPanel description;
            description.Spacing(2);
            auto name = Text(product.displayName, 13);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            description.Children().Append(name);
            description.Children().Append(Text(product.publisher + (product.msi ? L" · MSI" : L" · EXE"), 10));
            Grid::SetColumn(description, 1);
            row.Children().Append(description);

            auto version = Text(product.version.empty() ? L"—" : product.version, 11);
            version.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(version, 2);
            row.Children().Append(version);
            auto architecture = Badge(product.architecture.empty() ? L"—" : product.architecture, BadgeTone::Neutral);
            architecture.HorizontalAlignment(HorizontalAlignment::Left);
            architecture.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(architecture, 3);
            row.Children().Append(architecture);
            auto risk = Badge(product.risk == cpc::RiskLevel::High ? T(L"Высокий", L"High") : T(L"Обычный", L"Normal"),
                              product.risk == cpc::RiskLevel::High ? BadgeTone::Danger : BadgeTone::Success);
            risk.HorizontalAlignment(HorizontalAlignment::Left);
            risk.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(risk, 4);
            row.Children().Append(risk);

            Border holder;
            holder.Padding(Thickness{16, 12, 16, 12});
            holder.BorderBrush(ThemeBrush(L"DividerBrush"));
            holder.BorderThickness(Thickness{0, 0, 0, 1});
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(holder, productAutomationName);
            holder.Child(row);
            ProductsPanel().Children().Append(holder);
        }
    }

    void MainWindow::PopulateProfiles()
    {
        ProfilesPanel().Children().Clear();
        auto weak = get_weak();
        if (scan_.profiles.empty())
        {
            ProfilesPanel().Children().Append(Text(T(L"Локальные профили не найдены.", L"No local profiles were found."), 11));
            return;
        }
        for (size_t index = 0; index < scan_.profiles.size(); ++index)
        {
            auto const& profile = scan_.profiles[index];
            CheckBox check;
            check.Content(box_value(profile.displayName + (profile.loaded ? T(L" · загружен", L" · loaded") : L"")));
            check.IsThreeState(false);
            check.IsChecked(box_value(profile.selected).as<Windows::Foundation::IReference<bool>>());
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(check,
                T(L"Очистить настройки профиля: ", L"Clean settings for profile: ") + profile.displayName);
            check.Click([weak, index](IInspectable const& sender, RoutedEventArgs const&)
            {
                if (auto self = weak.get())
                {
                    if (!self->operationGate_.idle()) { self->PopulateProfiles(); return; }
                    auto state = sender.as<CheckBox>().IsChecked();
                    self->scan_.profiles[index].selected = state && state.Value();
                    self->planRevisions_.ProfilesChanged();
                    self->InvalidatePlan();
                    self->RefreshCommandStates();
                }
            });
            ProfilesPanel().Children().Append(check);
        }
    }

    void MainWindow::UpdateSelectedCounts()
    {
        const size_t products = static_cast<size_t>(std::count_if(scan_.products.begin(), scan_.products.end(),
            [](cpc::InstalledProduct const& product) { return product.selected; }));
        const size_t certificates = static_cast<size_t>(std::count_if(scan_.certificates.begin(), scan_.certificates.end(),
            [](cpc::CertificateEntry const& certificate) { return certificate.selected; }));
        const size_t visibleSelected = static_cast<size_t>(std::count_if(
            visibleCertificateIndices_.begin(), visibleCertificateIndices_.end(), [this](size_t index) {
                return index < scan_.certificates.size() && scan_.certificates[index].selected;
            }));
        SelectedProductsBadge().Text(T(L"Выбрано: ", L"Selected: ") + std::to_wstring(products));
        CertificateSelectedCount().Text(T(L"Выбрано: ", L"Selected: ") + std::to_wstring(certificates) +
            T(L" · среди отфильтрованных: ", L" · among filtered: ") + std::to_wstring(visibleSelected));
        RefreshCommandStates();
    }

    void MainWindow::PopulateCertificateFilters()
    {
        languageSync_ = true;
        std::wstring selectedProfile;
        if (auto item = CertificateProfileFilter().SelectedItem())
        {
            if (auto combo = item.try_as<ComboBoxItem>(); combo && combo.Tag())
                selectedProfile = unbox_value<hstring>(combo.Tag()).c_str();
        }
        const int keyIndex = std::max(0, CertificateKeyFilter().SelectedIndex());
        const int validityIndex = std::max(0, CertificateValidityFilter().SelectedIndex());
        const int sortIndex = std::max(0, CertificateSort().SelectedIndex());
        CertificateProfileFilter().Items().Clear();
        ComboBoxItem all;
        all.Content(box_value(T(L"Все профили", L"All profiles")));
        CertificateProfileFilter().Items().Append(all);
        std::set<std::wstring> profiles;
        for (auto const& certificate : scan_.certificates) profiles.insert(certificate.profileName);
        int selectedIndex = 0;
        int index = 1;
        for (auto const& profile : profiles)
        {
            ComboBoxItem item;
            item.Content(box_value(profile));
            item.Tag(box_value(profile));
            CertificateProfileFilter().Items().Append(item);
            if (profile == selectedProfile) selectedIndex = index;
            ++index;
        }
        CertificateProfileFilter().SelectedIndex(selectedIndex);
        CertificateKeyFilter().Items().Clear();
        for (auto const& label : {T(L"Любой ключ", L"Any key"), T(L"Есть ссылка", L"Has reference"), T(L"Ссылки нет", L"No reference")})
        {
            ComboBoxItem item;
            item.Content(box_value(label));
            CertificateKeyFilter().Items().Append(item);
        }
        CertificateKeyFilter().SelectedIndex(std::min(keyIndex, 2));
        CertificateValidityFilter().Items().Clear();
        for (auto const& label : {T(L"Любой статус", L"Any status"), T(L"Действует", L"Valid"),
                                  T(L"Скоро истекает", L"Expiring soon"), T(L"Истёк", L"Expired"),
                                  T(L"Срок неизвестен", L"Validity unknown"), T(L"Ещё не действует", L"Not valid yet")})
        {
            ComboBoxItem item;
            item.Content(box_value(label));
            CertificateValidityFilter().Items().Append(item);
        }
        CertificateValidityFilter().SelectedIndex(std::min(validityIndex, 5));
        CertificateSort().Items().Clear();
        for (auto const& label : {T(L"Сначала владелец", L"Owner first"), T(L"Сначала срок", L"Validity first"),
                                  T(L"Сначала профиль", L"Profile first")})
        {
            ComboBoxItem item;
            item.Content(box_value(label));
            CertificateSort().Items().Append(item);
        }
        CertificateSort().SelectedIndex(std::min(sortIndex, 2));
        languageSync_ = false;
    }

    void MainWindow::CertificateFilter_Changed(IInspectable const&, TextChangedEventArgs const&)
    {
        if (!languageSync_ && certificateFilterTimer_)
        {
            certificateFilterTimer_.Stop();
            certificateFilterTimer_.Start();
        }
    }

    void MainWindow::CertificateFilter_Changed(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!languageSync_) PopulateCertificates();
    }

    void MainWindow::SelectFilteredCertificates_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!operationGate_.idle()) return;
        cpc::SetCertificateSelection(scan_.certificates, visibleCertificateIndices_, true);
        planRevisions_.CertificatesChanged();
        InvalidatePlan();
        PopulateCertificates();
        UpdateSelectedCounts();
    }

    void MainWindow::DeselectAllCertificates_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!operationGate_.idle()) return;
        cpc::SetCertificateSelection(scan_.certificates, visibleCertificateIndices_, false);
        planRevisions_.CertificatesChanged();
        InvalidatePlan();
        PopulateCertificates();
        UpdateSelectedCounts();
    }

    void MainWindow::PopulateCertificates()
    {
        CertificatesPanel().Items().Clear();
        const std::wstring search = cpc::ToLower(CertificateSearch().Text().c_str());
        std::wstring profile;
        if (auto selected = CertificateProfileFilter().SelectedItem())
        {
            if (auto item = selected.try_as<ComboBoxItem>(); item && item.Tag())
                profile = unbox_value<hstring>(item.Tag()).c_str();
        }
        const int keyFilter = std::max(0, CertificateKeyFilter().SelectedIndex());
        const int validityFilter = std::max(0, CertificateValidityFilter().SelectedIndex());
        const int sortMode = std::max(0, CertificateSort().SelectedIndex());
        visibleCertificateIndices_.clear();
        for (size_t index = 0; index < scan_.certificates.size(); ++index)
        {
            auto const& certificate = scan_.certificates[index];
            const std::wstring haystack = cpc::ToLower(certificate.subject + L" " + certificate.issuer + L" " +
                                                       certificate.profileName + L" " + certificate.thumbprint);
            if (!search.empty() && haystack.find(search) == std::wstring::npos) continue;
            if (!profile.empty() && certificate.profileName != profile) continue;
            if (keyFilter == 1 && !certificate.hasPrivateKeyReference) continue;
            if (keyFilter == 2 && certificate.hasPrivateKeyReference) continue;
            const auto validity = cpc::GetCertificateStatus(certificate);
            if (validityFilter == 1 && validity != cpc::CertificateStatus::Valid) continue;
            if (validityFilter == 2 && validity != cpc::CertificateStatus::ExpiringSoon) continue;
            if (validityFilter == 3 && validity != cpc::CertificateStatus::Expired) continue;
            if (validityFilter == 4 && validity != cpc::CertificateStatus::Unknown) continue;
            if (validityFilter == 5 && validity != cpc::CertificateStatus::NotYetValid) continue;
            visibleCertificateIndices_.push_back(index);
        }
        std::stable_sort(visibleCertificateIndices_.begin(), visibleCertificateIndices_.end(),
            [this, sortMode](size_t left, size_t right)
            {
                auto const& a = scan_.certificates[left];
                auto const& b = scan_.certificates[right];
                if (sortMode == 1) {
                    const auto aKey = cpc::CertificateExpiryKey(a);
                    const auto bKey = cpc::CertificateExpiryKey(b);
                    if (aKey != bKey) return aKey < bKey;
                    return cpc::ToLower(a.subject) < cpc::ToLower(b.subject);
                }
                if (sortMode == 2) return cpc::ToLower(a.profileName + a.subject) < cpc::ToLower(b.profileName + b.subject);
                return cpc::ToLower(a.subject + a.issuer) < cpc::ToLower(b.subject + b.issuer);
            });
        auto weak = get_weak();
        size_t shown = 0;
        for (size_t index : visibleCertificateIndices_)
        {
            auto const& certificate = scan_.certificates[index];
            ++shown;

            Grid row;
            row.ColumnSpacing(10);
            row.ColumnDefinitions().Append(Column(32, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(1, GridUnitType::Star));
            row.ColumnDefinitions().Append(Column(130, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(120, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(100, GridUnitType::Pixel));
            CheckBox check;
            check.IsThreeState(false);
            check.IsChecked(box_value(certificate.selected).as<Windows::Foundation::IReference<bool>>());
            check.VerticalAlignment(VerticalAlignment::Center);
            check.Click([weak, index](IInspectable const& sender, RoutedEventArgs const&)
            {
                if (auto self = weak.get())
                {
                    if (!self->operationGate_.idle()) { self->PopulateCertificates(); return; }
                    auto state = sender.as<CheckBox>().IsChecked();
                    self->scan_.certificates[index].selected = state && state.Value();
                    self->planRevisions_.CertificatesChanged();
                    self->InvalidatePlan();
                    self->selectedCertificate_ = index;
                    self->ShowCertificateDetails(index);
                    self->RefreshCertificateSelectionVisuals();
                    self->UpdateSelectedCounts();
                }
            });
            row.Children().Append(check);
            StackPanel description;
            description.Spacing(2);
            auto subject = Text(certificate.subject.empty() ? L"—" : certificate.subject, 12);
            subject.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            description.Children().Append(subject);
            description.Children().Append(Text(certificate.issuer, 10));
            Grid::SetColumn(description, 1);
            row.Children().Append(description);
            auto profileText = Text(certificate.profileName, 11);
            profileText.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(profileText, 2);
            row.Children().Append(profileText);
            const auto status = cpc::GetCertificateStatus(certificate);
            std::wstring statusText;
            BadgeTone statusTone = BadgeTone::Neutral;
            switch (status)
            {
            case cpc::CertificateStatus::Valid: statusText = T(L"Действует", L"Valid"); statusTone = BadgeTone::Success; break;
            case cpc::CertificateStatus::ExpiringSoon: statusText = T(L"Скоро истекает", L"Expiring soon"); statusTone = BadgeTone::Warning; break;
            case cpc::CertificateStatus::Expired: statusText = T(L"Истёк", L"Expired"); statusTone = BadgeTone::Danger; break;
            case cpc::CertificateStatus::NotYetValid: statusText = T(L"Ещё не действует", L"Not valid yet"); break;
            default: statusText = T(L"Срок неизвестен", L"Validity unknown"); break;
            }
            StackPanel validity;
            validity.Spacing(3);
            validity.Children().Append(Text(certificate.validTo.empty() ? L"—" : certificate.validTo, 10));
            validity.Children().Append(Badge(statusText, statusTone));
            validity.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(validity, 3);
            row.Children().Append(validity);
            auto key = Badge(certificate.hasPrivateKeyReference ? T(L"Есть ссылка", L"Has link") : T(L"Нет", L"None"),
                             certificate.hasPrivateKeyReference ? BadgeTone::Success : BadgeTone::Neutral);
            key.HorizontalAlignment(HorizontalAlignment::Left);
            key.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(key, 4);
            row.Children().Append(key);
            const std::wstring automationName = (certificate.subject.empty() ? L"—" : certificate.subject) + L"; " +
                certificate.profileName + L"; " + certificate.validTo + L"; " + statusText;
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(check,
                T(L"Выбрать сертификат: ", L"Select certificate: ") + automationName);
            Button holder;
            holder.Padding(Thickness{16, 12, 16, 12});
            holder.BorderBrush(ThemeBrush(L"DividerBrush"));
            holder.BorderThickness(Thickness{0, 0, 0, 1});
            holder.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            holder.HorizontalAlignment(HorizontalAlignment::Stretch);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(holder, automationName);
            if (selectedCertificate_ == index)
                holder.Background(ThemeBrush(L"SurfaceSelectedBrush"));
            else holder.Background(ThemeBrush(L"SurfaceBrush"));
            holder.Content(row);
            holder.Click([weak, index](IInspectable const&, RoutedEventArgs const&)
            {
                if (auto self = weak.get())
                {
                    self->selectedCertificate_ = index;
                    self->ShowCertificateDetails(index);
                    self->RefreshCertificateSelectionVisuals();
                }
            });
            CertificatesPanel().Items().Append(holder);
        }
        if (shown == 0)
        {
            auto empty = Text(T(L"Сертификаты по текущему фильтру не найдены.", L"No certificates match the current filters."), 12);
            empty.HorizontalAlignment(HorizontalAlignment::Center);
            empty.Margin(Thickness{0, 40, 0, 0});
            CertificatesPanel().Items().Append(empty);
        }
        if (!visibleCertificateIndices_.empty())
        {
            if (std::find(visibleCertificateIndices_.begin(), visibleCertificateIndices_.end(), selectedCertificate_) == visibleCertificateIndices_.end())
                selectedCertificate_ = visibleCertificateIndices_.front();
            ShowCertificateDetails(selectedCertificate_);
        }
        else if (!scan_.certificates.empty())
        {
            SelectedCertSubject().Text(T(L"Выбранный сертификат скрыт текущим фильтром", L"The selected certificate is hidden by the current filter"));
            SelectedCertIssuer().Text(T(L"Измените фильтр, чтобы увидеть сведения.", L"Change the filter to view its details."));
            SelectedCertStatus().Text(T(L"Статус: —", L"Status: —"));
            SelectedCertDates().Text(T(L"Срок действия: —", L"Validity: —"));
            SelectedCertKey().Text(T(L"Ссылка на закрытый ключ: —", L"Private-key reference: —"));
            SelectedCertThumbprint().Text(L"SHA-1: —");
        }
        else
        {
            SelectedCertSubject().Text(T(L"Сертификаты не найдены", L"No certificates found"));
            SelectedCertIssuer().Text(T(L"Кем выдан: —", L"Issuer: —"));
            SelectedCertStatus().Text(T(L"Статус: —", L"Status: —"));
            SelectedCertDates().Text(T(L"Срок действия: —", L"Validity: —"));
            SelectedCertKey().Text(T(L"Ссылка на закрытый ключ: —", L"Private-key reference: —"));
            SelectedCertThumbprint().Text(L"SHA-1: —");
        }
        UpdateSelectedCounts();
    }

    void MainWindow::RefreshCertificateSelectionVisuals()
    {
        const auto count = std::min<size_t>(CertificatesPanel().Items().Size(), visibleCertificateIndices_.size());
        for (size_t position = 0; position < count; ++position)
        {
            if (auto holder = CertificatesPanel().Items().GetAt(static_cast<uint32_t>(position)).try_as<Button>())
            {
                holder.Background(ThemeBrush(visibleCertificateIndices_[position] == selectedCertificate_
                    ? L"SurfaceSelectedBrush" : L"SurfaceBrush"));
                holder.BorderBrush(ThemeBrush(L"DividerBrush"));
            }
        }
    }

    void MainWindow::ShowCertificateDetails(size_t index)
    {
        if (index >= scan_.certificates.size()) return;
        selectedCertificate_ = index;
        auto const& certificate = scan_.certificates[index];
        std::wstring initials = certificate.subject.empty() ? L"—" : certificate.subject.substr(0, std::min<size_t>(2, certificate.subject.size()));
        SelectedCertInitials().Text(initials);
        SelectedCertSubject().Text(certificate.subject.empty() ? L"—" : certificate.subject);
        SelectedCertIssuer().Text(T(L"Кем выдан: ", L"Issuer: ") + certificate.issuer + L"\n" +
                                  T(L"Профиль: ", L"Profile: ") + certificate.profileName);
        const auto status = cpc::GetCertificateStatus(certificate);
        std::wstring statusText;
        wchar_t const* statusBrush = L"SecondaryTextBrush";
        switch (status)
        {
        case cpc::CertificateStatus::Valid: statusText = T(L"Действует", L"Valid"); statusBrush = L"SuccessBrush"; break;
        case cpc::CertificateStatus::ExpiringSoon: statusText = T(L"Скоро истекает", L"Expiring soon"); statusBrush = L"WarningBrush"; break;
        case cpc::CertificateStatus::Expired: statusText = T(L"Истёк", L"Expired"); statusBrush = L"DangerBrush"; break;
        case cpc::CertificateStatus::NotYetValid: statusText = T(L"Ещё не действует", L"Not valid yet"); break;
        default: statusText = T(L"Срок неизвестен", L"Validity unknown"); break;
        }
        SelectedCertStatus().Text(T(L"Статус: ", L"Status: ") + statusText);
        SelectedCertStatus().Foreground(ThemeBrush(statusBrush));
        SelectedCertDates().Text(T(L"Действует с: ", L"Valid from: ") + certificate.validFrom + L"\n" +
                                 T(L"Действует по: ", L"Valid to: ") + certificate.validTo);
        SelectedCertKey().Text(certificate.hasPrivateKeyReference
            ? T(L"Есть ссылка на закрытый ключ. Сам ключ не копируется.", L"A private-key reference exists. The key itself is not copied.")
            : T(L"Ссылки на закрытый ключ нет.", L"No private-key reference exists."));
        SelectedCertKey().Foreground(ThemeBrush(certificate.hasPrivateKeyReference
            ? L"SuccessBrush" : L"SecondaryTextBrush"));
        SelectedCertThumbprint().Text(L"SHA-1: " + certificate.thumbprint);
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::PickFolder(std::function<void(std::wstring const&)> completed)
    {
        auto lifetime = get_strong();
        FolderPicker picker;
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L"*");
        check_hresult(picker.as<IInitializeWithWindow>()->Initialize(GetWindowHandle()));
        auto folder = co_await picker.PickSingleFolderAsync();
        if (folder) completed(folder.Path().c_str());
    }

    winrt::fire_and_forget MainWindow::ChooseBackup_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (modalOpen_) co_return;
        try
        {
            co_await PickFolder([weak = get_weak()](std::wstring const& path)
            {
                if (auto self = weak.get()) self->BackupPath().Text(path);
            });
        }
        catch (...) { ReportAsyncFailure(T(L"Выбор папки резервной копии", L"Choosing the backup folder"), std::current_exception()); }
    }

    winrt::fire_and_forget MainWindow::ExportCertificates_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        cpc::UiOperationToken token;
        if (cpc::CountSelectedCertificates(scan_.certificates) == 0)
        {
            ShowMessage(T(L"Не выбран ни один сертификат", L"No certificate selected"),
                        T(L"Выберите хотя бы один открытый сертификат для экспорта.",
                          L"Select at least one public certificate to export."));
            co_return;
        }
        try
        {
        FolderPicker picker;
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L"*");
        check_hresult(picker.as<IInitializeWithWindow>()->Initialize(GetWindowHandle()));
        auto folder = co_await picker.PickSingleFolderAsync();
        if (!folder) co_return;
        token = BeginOperation(cpc::UiOperation::ExportingCertificates,
            T(L"Экспорт открытых сертификатов…", L"Exporting public certificates…"));
        if (!token) co_return;
        std::wstring exportFolder;
        std::wstring error;
        bool exported = false;
        std::exception_ptr failure;
        apartment_context uiThread;
        const auto certificates = scan_.certificates;
        const auto language = language_;
        const std::wstring parent = folder.Path().c_str();
        co_await resume_background();
        try { exported = cpc::ExportPublicCertificates(language, certificates, parent, &exportFolder, &error); }
        catch (...) { failure = std::current_exception(); }
        co_await uiThread;
        if (failure)
        {
            ReportAsyncFailure(T(L"Экспорт сертификатов", L"Certificate export"), failure, token);
            co_return;
        }
        EndOperation(token, exported ? T(L"Открытые сертификаты экспортированы.", L"Public certificates exported.")
                                     : T(L"Экспорт не выполнен.", L"Export failed."),
                     exported ? UiStatusKind::ExportComplete : UiStatusKind::Error);
        if (!exported)
        {
            ShowMessage(T(L"Экспорт не выполнен", L"Export failed"), error);
            co_return;
        }
        SetOutputSession(OutputSessionKind::CertificateExport, exportFolder);
        AppendLogLine(T(L"Выбранные открытые сертификаты экспортированы. Закрытые ключи не экспортировались.",
                        L"Selected public certificates were exported. Private keys were not exported."));
        ShowMessage(T(L"Экспорт завершён", L"Export completed"),
                    T(L"Сохранена только открытая часть сертификатов:\n", L"Only public certificate data was saved:\n") + exportFolder);
        }
        catch (...)
        {
            ReportAsyncFailure(T(L"Экспорт сертификатов", L"Certificate export"), std::current_exception(), token);
        }
    }

    void MainWindow::CopyThumbprint_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (selectedCertificate_ < scan_.certificates.size()) CopyText(scan_.certificates[selectedCertificate_].thumbprint);
    }

    winrt::fire_and_forget MainWindow::ShowLicenses_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (modalOpen_) co_return;
        try
        {
        if (currentPage_ == L"offline" && !OfflineStateCurrent())
        {
            ShowMessage(T(L"Нужно повторное сканирование", L"A new scan is required"),
                        T(L"Путь был изменён. Сначала просканируйте выбранную Windows снова.",
                          L"The path changed. Scan the selected Windows installation again first."));
            co_return;
        }
        auto const& licenses = currentPage_ == L"offline" ? offline_.scan.licenses : scan_.licenses;
        if (licenses.empty())
        {
            ShowMessage(T(L"Лицензии не найдены", L"No licenses found"),
                        T(L"Известные полные значения лицензий в реестре не обнаружены.",
                          L"No known full license values were found in the registry."));
            co_return;
        }

        std::wostringstream output;
        for (size_t index = 0; index < licenses.size(); ++index)
        {
            auto const& license = licenses[index];
            output << L"[" << index + 1 << L"] " << license.product << L"\r\n"
                   << T(L"Источник: ", L"Source: ") << license.registryPath << L" / " << license.valueName << L"\r\n"
                   << T(L"Лицензия: ", L"License: ") << license.fullValue << L"\r\n\r\n";
        }
        const std::wstring text = output.str();

        modalOpen_ = true;
        ContentDialog dialog;
        dialog.XamlRoot(WindowRoot().XamlRoot());
        dialog.RequestedTheme(WindowRoot().RequestedTheme());
        dialog.Title(box_value(T(L"Лицензии CryptoPro", L"CryptoPro licenses")));
        dialog.PrimaryButtonText(T(L"Копировать всё", L"Copy all"));
        dialog.CloseButtonText(T(L"Закрыть", L"Close"));
        dialog.DefaultButton(ContentDialogButton::Close);

        StackPanel content;
        content.Spacing(12);
        const bool offlineContext = currentPage_ == L"offline";
        auto const& contextScan = offlineContext ? offline_.scan : scan_;
        const size_t selectedProductCount = cpc::CountSelectedProducts(contextScan.products);
        std::wstring contextSummary = offlineContext
            ? T(L"Отключённая Windows: ", L"Offline Windows: ") + offlineScannedPath_ + L"\n"
            : T(L"Работающая система", L"Live system");
        contextSummary += L"\n" + T(L"Выбрано продуктов: ", L"Selected products: ") +
            std::to_wstring(selectedProductCount) + T(L" · лицензий: ", L" · licenses: ") +
            std::to_wstring(licenses.size()) + T(L" · открытых сертификатов: ", L" · public certificates: ") +
            std::to_wstring(cpc::CountSelectedCertificates(contextScan.certificates));
        if (!offlineContext && planRevisions_.IsPlanCurrent())
            contextSummary += T(L" · подтверждённых целей: ", L" · verified targets: ") +
                std::to_wstring(cpc::CountVerifiedTargets(plan_.targets));
        else if (!offlineContext)
            contextSummary += T(L" · план ещё не проверен", L" · plan not reviewed yet");
        content.Children().Append(Text(contextSummary, 12));
        Border warning;
        warning.Padding(Thickness{12});
        warning.CornerRadius(CornerRadius{10});
        warning.Background(ThemeBrush(L"WarningSurfaceBrush"));
        warning.BorderBrush(ThemeBrush(L"WarningBorderBrush"));
        warning.BorderThickness(Thickness{1});
        warning.Child(Text(T(L"Полные номера конфиденциальны. Перед переустановкой CryptoPro или Windows сохраните licenses.txt на внешнем диске или в защищённом хранилище.",
                             L"Full serial numbers are confidential. Before reinstalling CryptoPro or Windows, save licenses.txt to external or protected storage."), 11));
        content.Children().Append(warning);
        TextBox values;
        values.Text(text);
        values.IsReadOnly(true);
        values.AcceptsReturn(true);
        values.TextWrapping(TextWrapping::NoWrap);
        values.MinHeight(180);
        values.MaxWidth(720);
        values.MaxHeight(380);
        values.FontFamily(FontFamily{L"Consolas"});
        values.FontSize(12);
        ScrollViewer::SetHorizontalScrollBarVisibility(values, ScrollBarVisibility::Auto);
        ScrollViewer::SetVerticalScrollBarVisibility(values, ScrollBarVisibility::Auto);
        content.Children().Append(values);
        content.Children().Append(Text(T(L"Полные номера не добавляются в report.json и cleanup.log.",
                                         L"Full serial numbers are never written to report.json or cleanup.log."), 10));
        dialog.Content(PaddedDialogContent(content));
        const auto result = co_await dialog.ShowAsync();
        modalOpen_ = false;
        if (result == ContentDialogResult::Primary)
        {
            CopyText(text);
            ShowMessage(T(L"Лицензии скопированы", L"Licenses copied"),
                        T(L"Полные номера помещены в буфер обмена. Не отправляйте их посторонним.",
                          L"Full serial numbers were copied to the clipboard. Do not share them with others."));
        }
        }
        catch (...) { modalOpen_ = false; ReportAsyncFailure(T(L"Просмотр лицензий", L"Viewing licenses"), std::current_exception()); }
    }

    winrt::fire_and_forget MainWindow::ChooseOffline_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (!operationGate_.idle()) co_return;
        try
        {
        FolderPicker picker;
        picker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
        picker.FileTypeFilter().Append(L"*");
        check_hresult(picker.as<IInitializeWithWindow>()->Initialize(GetWindowHandle()));
        auto folder = co_await picker.PickSingleFolderAsync();
        if (folder) OfflinePath().Text(folder.Path());
        }
        catch (...) { ReportAsyncFailure(T(L"Выбор офлайн-Windows", L"Choosing offline Windows"), std::current_exception()); }
    }

    void MainWindow::ScanOffline_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const std::wstring path = OfflinePath().Text().c_str();
        if (path.empty())
        {
            ShowMessage(T(L"Не выбран диск", L"No drive selected"),
                        T(L"Выберите корень подключённого диска или папку Windows на нём.",
                          L"Select the attached drive root or its Windows directory."));
            return;
        }
        StartOfflineScan(path);
    }

    winrt::fire_and_forget MainWindow::StartOfflineScan(std::wstring path)
    {
        cpc::UiOperationToken token;
        try
        {
        offline_ = {};
        offlineInputPath_ = path;
        offlineScannedPath_.clear();
        offlinePathStale_ = false;
        offlineProductsConfirmed_ = false;
        token = BeginOperation(cpc::UiOperation::OfflineScan,
            T(L"Сканирование подключённой Windows…", L"Scanning attached Windows…"), 0);
        if (!token) co_return;
        offlineScanRunning_ = true;
        OfflineProgressCard().Visibility(Visibility::Visible);
        OfflineProgress().IsIndeterminate(false);
        OfflineProgress().Value(0);
        OfflineProgressText().Text(T(L"Определение установленной Windows…", L"Detecting the Windows installation…"));
        OfflineProgressPercent().Text(L"0%");
        auto lifetime = get_strong();
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        apartment_context uiThread;
        const auto language = language_;
        auto result = std::make_shared<cpc::OfflineScanResult>();
        std::exception_ptr failure;
        co_await resume_background();
        try
        {
            *result = cpc::ScanOfflineWindows(language, path,
                [weak, dispatcher, token](std::wstring const& message, int percent)
                {
                    dispatcher.TryEnqueue([weak, message, percent, token]()
                    {
                        if (auto self = weak.get())
                        {
                            if (!self->operationGate_.IsCurrent(token)) return;
                            self->OfflineProgressText().Text(message);
                            self->OfflineProgress().Value(std::clamp(percent, 0, 100));
                            self->OfflineProgressPercent().Text(std::to_wstring(std::clamp(percent, 0, 100)) + L"%");
                            self->UpdateOperation(token, message, percent);
                        }
                    });
                });
        }
        catch (...) { failure = std::current_exception(); }
        co_await uiThread;
        if (failure)
        {
            ReportAsyncFailure(T(L"Офлайн-сканирование", L"Offline scan"), failure, token, &result->scan);
            co_return;
        }
        offline_ = std::move(*result);
        offlineScannedPath_ = offline_.windowsDirectory;
        offlinePathStale_ = false;
        ++offlineScanRevision_;
        offlineScanRunning_ = false;
        OfflineProgress().Value(100);
        OfflineProgressPercent().Text(L"100%");
        OfflineProgressCard().Visibility(Visibility::Collapsed);
        PopulateOfflineScan();
        EndOperation(token, offline_.valid
            ? T(L"Офлайн-сканирование завершено без изменений.", L"Offline scan completed without changes.")
            : T(L"Офлайн-Windows не распознана. См. диагностику.", L"Offline Windows was not recognized. See diagnostics."),
            offline_.valid ? UiStatusKind::ScanComplete : UiStatusKind::Warning);
        AppendLogLine(offline_.valid
            ? T(L"Офлайн-сканирование завершено без изменений: продуктов — ", L"Offline scan completed without changes: products — ") +
                std::to_wstring(offline_.scan.products.size()) + T(L", лицензий — ", L", licenses — ") +
                std::to_wstring(offline_.scan.licenses.size()) + T(L", сертификатов — ", L", certificates — ") +
                std::to_wstring(offline_.scan.certificates.size()) + L"."
            : T(L"Выбранный путь не распознан как отключённая Windows.", L"The selected path was not recognized as an offline Windows installation."));
        }
        catch (...) { ReportAsyncFailure(T(L"Офлайн-сканирование", L"Offline scan"), std::current_exception(), token, &offline_.scan); }
    }

    bool MainWindow::OfflineStateCurrent()
    {
        return offline_.valid && offlineScanRevision_ != 0 && !offlineScannedPath_.empty() &&
               NormalizeUiPath(OfflinePath().Text().c_str()) == NormalizeUiPath(offlineInputPath_);
    }

    void MainWindow::InvalidateOfflinePath()
    {
        offline_ = {};
        offlineScannedPath_.clear();
        offlinePathStale_ = true;
        offlineProductsConfirmed_ = false;
        ++offlineScanRevision_;
        OfflineProgressCard().Visibility(Visibility::Collapsed);
        PopulateOfflineScan();
        OfflineDetectedPath().Text(T(L"Путь изменён — выполните повторное сканирование.",
                                     L"The path changed — run the scan again."));
        OfflineEmptyText().Text(T(L"Старые результаты скрыты, чтобы они не могли быть применены к другой Windows.",
                                  L"Previous results were hidden so they cannot be applied to another Windows installation."));
        RefreshCommandStates();
    }

    void MainWindow::PopulateOfflineScan()
    {
        OfflineProductsPanel().Children().Clear();
        OfflineCertificatesPanel().Children().Clear();
        OfflineCertificatesTitle().Visibility(offline_.valid ? Visibility::Visible : Visibility::Collapsed);
        OfflineProductsStat().Text(offline_.valid ? std::to_wstring(offline_.scan.products.size()) : L"—");
        OfflineLicensesStat().Text(offline_.valid ? std::to_wstring(offline_.scan.licenses.size()) : L"—");
        OfflineCertsStat().Text(offline_.valid ? std::to_wstring(offline_.scan.certificates.size()) : L"—");
        OfflineTargetsStat().Text(offline_.valid ? std::to_wstring(offline_.targets.size()) : L"—");
        const size_t diagnosticCount = offline_.diagnostics.size() + offline_.scan.warnings.size();
        OfflineDiagnostics().Text(diagnosticCount
            ? T(L"Диагностических записей: ", L"Diagnostic entries: ") + std::to_wstring(diagnosticCount)
            : T(L"Диагностика не содержит предупреждений.", L"Diagnostics contain no warnings."));
        OfflineDiagnostics().Visibility(diagnosticCount ? Visibility::Visible : Visibility::Collapsed);
        OfflineDiagnosticsButton().Visibility(diagnosticCount ? Visibility::Visible : Visibility::Collapsed);
        OfflineDiagnosticsButton().IsEnabled(operationGate_.idle() && diagnosticCount != 0);
        if (!offline_.valid)
        {
            OfflineDetectedPath().Text(offlinePathStale_
                ? T(L"Путь изменён — выполните повторное сканирование.", L"The path changed — run the scan again.")
                : T(L"Сканирование ещё не выполнялось или Windows не распознана. Можно выбрать корень диска или папку Windows.",
                    L"No recognized scan yet. Select either the drive root or its Windows directory."));
            OfflineEmptyText().Text(offlinePathStale_
                ? T(L"Старые результаты скрыты, чтобы они не могли быть применены к другой Windows.",
                    L"Previous results were hidden so they cannot be applied to another Windows installation.")
                : T(L"Выберите диск и запустите безопасное сканирование. Во время чтения будет показан текущий этап и процент.",
                    L"Select a drive and start a safe scan. The current stage and percentage are shown while it is being read."));
            OfflineEmptyText().Visibility(Visibility::Visible);
            return;
        }
        OfflineEmptyText().Visibility(Visibility::Collapsed);
        OfflineDetectedPath().Text(offline_.windowsDirectory + L" · " + offline_.scan.osName + L" · " + offline_.scan.osArchitecture +
                                   T(L" · сканирование только для чтения", L" · read-only scan"));
        auto weak = get_weak();
        for (size_t index = 0; index < offline_.scan.products.size(); ++index)
        {
            auto const& product = offline_.scan.products[index];
            Grid row;
            row.ColumnDefinitions().Append(Column(32, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(1, GridUnitType::Star));
            row.ColumnDefinitions().Append(Column(120, GridUnitType::Pixel));
            row.ColumnDefinitions().Append(Column(90, GridUnitType::Pixel));
            CheckBox check;
            check.IsThreeState(false);
            check.IsChecked(box_value(product.selected).as<Windows::Foundation::IReference<bool>>());
            check.VerticalAlignment(VerticalAlignment::Center);
            check.Click([weak, index](IInspectable const& sender, RoutedEventArgs const&)
            {
                if (auto self = weak.get())
                {
                    if (!self->operationGate_.idle()) { self->PopulateOfflineScan(); return; }
                    const auto state = sender.as<CheckBox>().IsChecked();
                    self->offline_.scan.products[index].selected = state && state.Value();
                    self->offlineProductsConfirmed_ = false;
                    self->PopulateOfflineScan();
                }
            });
            row.Children().Append(check);
            StackPanel description;
            auto name = Text(product.displayName, 13);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            description.Children().Append(name);
            description.Children().Append(Text(product.publisher, 10));
            Grid::SetColumn(description, 1);
            row.Children().Append(description);
            auto version = Text(product.version, 11);
            version.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(version, 2);
            row.Children().Append(version);
            auto risk = Badge(product.risk == cpc::RiskLevel::High ? T(L"Высокий", L"High") : T(L"Обычный", L"Normal"),
                              product.risk == cpc::RiskLevel::High ? BadgeTone::Danger : BadgeTone::Success);
            risk.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(risk, 3);
            row.Children().Append(risk);
            Border holder;
            holder.Padding(Thickness{16, 12, 16, 12});
            holder.BorderBrush(ThemeBrush(L"DividerBrush"));
            holder.BorderThickness(Thickness{0, 0, 0, 1});
            holder.Child(row);
            OfflineProductsPanel().Children().Append(holder);
        }
        CheckBox productConfirmation;
        productConfirmation.Margin(Thickness{16, 10, 16, 8});
        productConfirmation.IsThreeState(false);
        productConfirmation.IsChecked(box_value(offlineProductsConfirmed_).as<Windows::Foundation::IReference<bool>>());
        productConfirmation.Content(box_value(T(
            L"Я проверил найденные продукты и подтверждаю выбранный набор для офлайн-очистки",
            L"I reviewed the detected products and confirm the selected offline-cleanup set")));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(productConfirmation,
            T(L"Подтвердить выбранный набор офлайн-продуктов", L"Confirm the selected offline product set"));
        productConfirmation.Click([weak](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto self = weak.get())
            {
                const auto checked = sender.as<CheckBox>().IsChecked();
                self->offlineProductsConfirmed_ = checked && checked.Value();
                self->RefreshCommandStates();
            }
        });
        OfflineProductsPanel().Children().Append(productConfirmation);
        StackPanel certificateToolbar;
        certificateToolbar.Orientation(Orientation::Vertical);
        certificateToolbar.Spacing(8);
        certificateToolbar.Margin(Thickness{16, 0, 16, 4});
        certificateToolbar.Children().Append(Text(
            T(L"Выбрано: ", L"Selected: ") + std::to_wstring(cpc::CountSelectedCertificates(offline_.scan.certificates)), 11));
        Button selectCertificates;
        selectCertificates.Content(box_value(T(L"Выбрать все сертификаты", L"Select all certificates")));
        selectCertificates.Style(Application::Current().Resources().Lookup(box_value(L"RoundedButtonStyle")).as<Style>());
        selectCertificates.Click([weak](IInspectable const&, RoutedEventArgs const&)
        {
            if (auto self = weak.get())
            {
                for (auto& certificate : self->offline_.scan.certificates) certificate.selected = true;
                self->PopulateOfflineScan();
            }
        });
        certificateToolbar.Children().Append(selectCertificates);
        Button clearCertificates;
        clearCertificates.Content(box_value(T(L"Снять выбор со всех сертификатов", L"Deselect all certificates")));
        clearCertificates.Style(Application::Current().Resources().Lookup(box_value(L"RoundedButtonStyle")).as<Style>());
        clearCertificates.Click([weak](IInspectable const&, RoutedEventArgs const&)
        {
            if (auto self = weak.get())
            {
                for (auto& certificate : self->offline_.scan.certificates) certificate.selected = false;
                self->PopulateOfflineScan();
            }
        });
        certificateToolbar.Children().Append(clearCertificates);
        OfflineCertificatesPanel().Children().Append(certificateToolbar);
        for (size_t index = 0; index < offline_.scan.certificates.size(); ++index)
        {
            auto const& certificate = offline_.scan.certificates[index];
            CheckBox check;
            check.Margin(Thickness{16, 4, 16, 4});
            check.IsThreeState(false);
            check.IsChecked(box_value(certificate.selected).as<Windows::Foundation::IReference<bool>>());
            check.Content(box_value((certificate.subject.empty() ? L"—" : certificate.subject) + L"\n" +
                T(L"Издатель: ", L"Issuer: ") + certificate.issuer + L" · " +
                T(L"Профиль: ", L"Profile: ") + certificate.profileName + L" · " +
                certificate.validFrom + L" — " + certificate.validTo + L" · " +
                (certificate.hasPrivateKeyReference ? T(L"есть ссылка на ключ", L"key reference present")
                                                    : T(L"без ссылки на ключ", L"no key reference"))));
            const std::wstring certificateAutomation =
                T(L"Сохранить открытую часть сертификата: ", L"Save public certificate: ") +
                (certificate.subject.empty() ? L"—" : certificate.subject) + L"; " + certificate.validTo;
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(check, certificateAutomation);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(check,
                T(L"Экспортируется только открытая часть; закрытый ключ не копируется.",
                  L"Only the public part is exported; the private key is not copied."));
            check.Click([weak, index](IInspectable const& sender, RoutedEventArgs const&)
            {
                if (auto self = weak.get())
                {
                    if (!self->operationGate_.idle()) { self->PopulateOfflineScan(); return; }
                    const auto state = sender.as<CheckBox>().IsChecked();
                    self->offline_.scan.certificates[index].selected = state && state.Value();
                    self->PopulateOfflineScan();
                }
            });
            OfflineCertificatesPanel().Children().Append(check);
        }
        RefreshCommandStates();
    }

    void MainWindow::OfflineDiagnostics_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (operationGate_.idle()) ShowOfflineDiagnostics();
    }

    winrt::fire_and_forget MainWindow::CheckPlan_Click(IInspectable const&, RoutedEventArgs const&)
    {
        cpc::UiOperationToken token;
        try
        {
        if (cpc::CountSelectedProducts(scan_.products) == 0) co_return;
        token = BeginOperation(cpc::UiOperation::BuildingPlan,
            T(L"Построение проверяемого плана…", L"Building a verifiable plan…"));
        if (!token) co_return;
        auto lifetime = get_strong();
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        apartment_context uiThread;
        auto source = std::make_shared<cpc::ScanResult>(scan_);
        const auto inputRevisions = planRevisions_;
        auto plan = std::make_shared<cpc::CleanupPlan>();
        std::exception_ptr failure;
        co_await resume_background();
        try
        {
            *plan = cpc::BuildCleanupPlan(*source,
                [weak, dispatcher, token](std::wstring const& message, int percent)
                {
                    dispatcher.TryEnqueue([weak, message, percent, token]()
                    {
                        if (auto self = weak.get()) self->UpdateOperation(token, message, percent);
                    });
                });
        }
        catch (...) { failure = std::current_exception(); }
        co_await uiThread;
        if (failure)
        {
            ReportAsyncFailure(T(L"Построение плана", L"Plan construction"), failure, token);
            co_return;
        }
        if (!inputRevisions.SameInputs(planRevisions_))
        {
            EndOperation(token, T(L"Выбор изменился; план нужно построить снова.", L"Selection changed; rebuild the plan."),
                         UiStatusKind::PlanStale);
            co_return;
        }
        plan_ = std::move(*plan);
        planRevisions_.PlanBuilt();
        PlanTargetCount().Text(std::to_wstring(cpc::CountVerifiedTargets(plan_.targets)));
        PlanState().Text(T(L"План проверен", L"Plan reviewed"));
        EndOperation(token, T(L"План построен без изменений системы.", L"Plan built without changing the system."),
                     UiStatusKind::PlanReady);
        AppendLogLine(T(L"Построен проверяемый план: подтверждённых целей — ", L"Verifiable plan built: confirmed targets — ") +
                      PlanTargetCount().Text().c_str() + T(L", защищённых объектов — ", L", protected objects — ") +
                      std::to_wstring(plan_.protectedItems.size()) + L".");
        ShowPlanInspector();
        }
        catch (...) { ReportAsyncFailure(T(L"Построение плана", L"Plan construction"), std::current_exception(), token); }
    }

    winrt::fire_and_forget MainWindow::ShowPlanInspector()
    {
        auto lifetime = get_strong();
        if (!planRevisions_.IsPlanCurrent())
        {
            ShowMessage(T(L"План устарел", L"Plan is stale"),
                        T(L"Повторно проверьте план после изменения выбора или нового сканирования.",
                          L"Review the plan again after changing selections or rescanning."));
            co_return;
        }
        if (modalOpen_) co_return;
        modalOpen_ = true;
        try
        {
        ContentDialog dialog;
        dialog.XamlRoot(WindowRoot().XamlRoot());
        dialog.RequestedTheme(WindowRoot().RequestedTheme());
        dialog.Title(box_value(T(L"Проверенный план очистки", L"Verified cleanup plan")));
        dialog.PrimaryButtonText(T(L"Перейти к подтверждению", L"Continue to confirmation"));
        dialog.CloseButtonText(T(L"Вернуться к плану", L"Return to plan"));
        dialog.DefaultButton(ContentDialogButton::Close);

        StackPanel content;
        content.Spacing(12);
        const size_t selectedProducts = cpc::CountSelectedProducts(plan_.products);
        const bool highRiskPlan = std::any_of(plan_.products.begin(), plan_.products.end(),
            [](cpc::InstalledProduct const& product) { return product.selected && product.risk == cpc::RiskLevel::High; });
        auto summary = Text(T(L"Продуктов: ", L"Products: ") + std::to_wstring(selectedProducts) +
                            T(L" · подтверждённых целей: ", L" · verified targets: ") + std::to_wstring(cpc::CountVerifiedTargets(plan_.targets)) +
                            T(L" · защищено: ", L" · protected: ") + std::to_wstring(plan_.protectedItems.size()) +
                            T(L" · открытых сертификатов: ", L" · public certificates: ") +
                            std::to_wstring(cpc::CountSelectedCertificates(scan_.certificates)), 13);
        summary.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        content.Children().Append(summary);
        content.Children().Append(Text(
            T(L"Резервная копия: ", L"Backup: ") + std::wstring(BackupPath().Text().c_str()) + L"\n" +
            (ValidateBackupPathForUi() ? T(L"Проверка папки: пройдена", L"Folder check: passed")
                                       : T(L"Проверка папки: не завершена", L"Folder check: not complete")) + L" · " +
            (highRiskPlan ? T(L"высокий риск", L"high risk") : T(L"обычный риск", L"normal risk")), 11));
        content.Children().Append(Text(T(L"Инспектор доступен только для чтения. Низкоуровневые цели нельзя произвольно отключать: выбор выполняется на уровне продуктов и профилей.",
                                        L"The inspector is read-only. Low-level targets cannot be arbitrarily disabled; selection is made at product and profile level."), 11));
        Grid toolbar;
        toolbar.ColumnSpacing(8);
        toolbar.RowSpacing(8);
        toolbar.ColumnDefinitions().Append(Column(1, GridUnitType::Star));
        toolbar.ColumnDefinitions().Append(Column(1, GridUnitType::Star));
        toolbar.ColumnDefinitions().Append(Column(135, GridUnitType::Pixel));
        RowDefinition toolbarSearchRow;
        toolbarSearchRow.Height(GridLengthHelper::Auto());
        toolbar.RowDefinitions().Append(toolbarSearchRow);
        RowDefinition toolbarActionsRow;
        toolbarActionsRow.Height(GridLengthHelper::Auto());
        toolbar.RowDefinitions().Append(toolbarActionsRow);
        TextBox search;
        search.PlaceholderText(T(L"Поиск в плане…", L"Search plan…"));
        Grid::SetColumnSpan(search, 3);
        toolbar.Children().Append(search);
        ComboBox category;
        for (auto const& label : {T(L"Все категории", L"All categories"), T(L"Продукты", L"Products"),
                                  T(L"Службы", L"Services"), T(L"Драйверы", L"Drivers"),
                                  T(L"Пакеты драйверов", L"Driver packages"), T(L"Файлы", L"Files"),
                                  T(L"Каталоги", L"Directories"), T(L"Реестр", L"Registry"),
                                  std::wstring(L"COM"), T(L"Криптопровайдеры", L"Cryptographic providers"),
                                  std::wstring(L"Native Messaging Hosts"), T(L"Запланированные задачи", L"Scheduled tasks"),
                                  T(L"Ярлыки", L"Shortcuts"), T(L"Защищённые объекты", L"Protected items")})
        {
            ComboBoxItem item;
            item.Content(box_value(label));
            category.Items().Append(item);
        }
        category.SelectedIndex(0);
        Grid::SetRow(category, 1);
        toolbar.Children().Append(category);
        CheckBox verifiedOnly;
        verifiedOnly.Content(box_value(T(L"Только подтверждённые", L"Verified only")));
        verifiedOnly.IsThreeState(false);
        verifiedOnly.IsChecked(box_value(false).as<Windows::Foundation::IReference<bool>>());
        verifiedOnly.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetRow(verifiedOnly, 1);
        Grid::SetColumn(verifiedOnly, 1);
        toolbar.Children().Append(verifiedOnly);
        Button copyPlan;
        copyPlan.Content(box_value(T(L"Копировать", L"Copy redacted")));
        Grid::SetRow(copyPlan, 1);
        Grid::SetColumn(copyPlan, 2);
        toolbar.Children().Append(copyPlan);
        content.Children().Append(toolbar);
        ScrollViewer scroll;
        scroll.MaxHeight(420);
        StackPanel targets;
        targets.Spacing(6);
        auto rebuild = std::make_shared<std::function<void()>>();
        *rebuild = [this, targets, search, category, verifiedOnly]() mutable
        {
            targets.Children().Clear();
            const std::wstring query = cpc::ToLower(search.Text().c_str());
            const int filter = std::max(0, category.SelectedIndex());
            const auto verified = verifiedOnly.IsChecked();
            const bool requireVerified = verified && verified.Value();
            size_t shown = 0;
            size_t matches = 0;
            auto addRow = [this, &targets, &shown, &matches, &query](std::wstring const& type, std::wstring const& name,
                                                           std::wstring const& location, std::wstring const& status)
            {
                if (!query.empty() && cpc::ToLower(type + L" " + name + L" " + location + L" " + status).find(query) == std::wstring::npos) return;
                ++matches;
                if (shown >= 250) return;
                ++shown;
                Border holder;
                holder.Padding(Thickness{10});
                holder.CornerRadius(CornerRadius{8});
                holder.BorderThickness(Thickness{1});
                holder.BorderBrush(ThemeBrush(L"SubtleBorderBrush"));
                Grid row;
                row.ColumnDefinitions().Append(Column(1, GridUnitType::Star));
                row.ColumnDefinitions().Append(Column(105, GridUnitType::Pixel));
                StackPanel item;
                item.Spacing(3);
                auto title = Text(type + L" · " + name, 11);
                title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                item.Children().Append(title);
                item.Children().Append(Text(location, 10));
                item.Children().Append(Text(status, 9));
                row.Children().Append(item);
                Button copy;
                copy.Content(box_value(T(L"Копировать путь", L"Copy path")));
                copy.HorizontalAlignment(HorizontalAlignment::Right);
                copy.VerticalAlignment(VerticalAlignment::Center);
                copy.Click([this, location](IInspectable const&, RoutedEventArgs const&) { CopyText(location); });
                Grid::SetColumn(copy, 1);
                row.Children().Append(copy);
                holder.Child(row);
                targets.Children().Append(holder);
            };
            if (filter == 0 || filter == 1)
            {
                for (auto const& product : plan_.products)
                    if (product.selected)
                        addRow(product.msi ? L"MSI" : L"EXE", product.displayName,
                               product.msi ? product.productCode : product.uninstallString,
                               T(L"Запускается первым", L"Runs first"));
            }
            for (auto const& target : plan_.targets)
            {
                int targetCategory = 5;
                std::wstring targetType = T(L"Файл", L"File");
                switch (target.category)
                {
                case cpc::CleanupTargetCategory::Directory: targetCategory = 6; targetType = T(L"Каталог", L"Directory"); break;
                case cpc::CleanupTargetCategory::Registry: targetCategory = 7; targetType = T(L"Реестр", L"Registry"); break;
                case cpc::CleanupTargetCategory::Com: targetCategory = 8; targetType = L"COM"; break;
                case cpc::CleanupTargetCategory::Provider: targetCategory = 9; targetType = T(L"Криптопровайдер", L"Cryptographic provider"); break;
                case cpc::CleanupTargetCategory::NativeMessagingHost: targetCategory = 10; targetType = L"Native Messaging Host"; break;
                case cpc::CleanupTargetCategory::Service: targetCategory = 2; targetType = T(L"Служба", L"Service"); break;
                case cpc::CleanupTargetCategory::Driver: targetCategory = 3; targetType = T(L"Драйвер", L"Driver"); break;
                case cpc::CleanupTargetCategory::DriverPackage: targetCategory = 4; targetType = T(L"Пакет драйвера", L"Driver package"); break;
                case cpc::CleanupTargetCategory::ScheduledTask: targetCategory = 11; targetType = T(L"Задача", L"Scheduled task"); break;
                case cpc::CleanupTargetCategory::Shortcut: targetCategory = 12; targetType = T(L"Ярлык", L"Shortcut"); break;
                case cpc::CleanupTargetCategory::Protected: targetCategory = 13; targetType = T(L"Защищено", L"Protected"); break;
                default: break;
                }
                if (filter != 0 && filter != targetCategory) continue;
                if (requireVerified && (!target.verified || target.protectedItem)) continue;
                const std::wstring location = !target.path.empty() ? target.path : target.registry.subkey;
                addRow(target.protectedItem ? T(L"Защищено", L"Protected") : targetType,
                       target.displayName, location,
                       target.reason + (target.verified ? T(L" · подтверждено · будет удалено после деинсталляторов", L" · verified · removed after uninstallers")
                                                        : T(L" · не подтверждено · автоматически не удаляется", L" · unverified · not removed automatically")));
            }
            if (!requireVerified && (filter == 0 || filter == 13))
                for (auto const& item : plan_.protectedItems)
                    addRow(T(L"Защищено", L"Protected"), item, L"", T(L"Никогда не удаляется", L"Never removed"));
            if (shown == 0) targets.Children().Append(Text(T(L"Совпадений нет.", L"No matches."), 11));
            if (matches > shown) targets.Children().Append(Text(
                T(L"Отображены первые ", L"Showing the first ") + std::to_wstring(shown) +
                T(L" из ", L" of ") + std::to_wstring(matches) + T(L" целей", L" targets"), 10));
        };
        search.TextChanged([rebuild](IInspectable const&, TextChangedEventArgs const&) { (*rebuild)(); });
        category.SelectionChanged([rebuild](IInspectable const&, SelectionChangedEventArgs const&) { (*rebuild)(); });
        verifiedOnly.Click([rebuild](IInspectable const&, RoutedEventArgs const&) { (*rebuild)(); });
        copyPlan.Click([this](IInspectable const&, RoutedEventArgs const&)
        {
            std::wostringstream output;
            output << L"CryptoPro Cleanup Utility " << cpc::kVersion << L"\r\n";
            for (auto const& product : plan_.products)
                if (product.selected)
                    output << L"[UNINSTALL] " << product.displayName << L" | "
                           << (product.msi ? product.productCode : product.uninstallString) << L"\r\n";
            for (auto const& target : plan_.targets)
                output << L"[TARGET] " << target.displayName << L" | "
                       << (!target.path.empty() ? target.path : target.registry.subkey) << L" | " << target.reason << L"\r\n";
            for (auto const& item : plan_.protectedItems) output << L"[PROTECTED] " << item << L"\r\n";
            CopyText(cpc::RedactSensitiveText(output.str(), scan_));
        });
        (*rebuild)();
        scroll.Content(targets);
        content.Children().Append(scroll);
        dialog.Content(PaddedDialogContent(content));
        const auto result = co_await dialog.ShowAsync();
        *rebuild = {};
        modalOpen_ = false;
        if (result == ContentDialogResult::Primary) ShowRemovalConfirmation();
        }
        catch (...)
        {
            modalOpen_ = false;
            ReportAsyncFailure(T(L"Просмотр плана", L"Plan review"), std::current_exception());
        }
    }

    winrt::fire_and_forget MainWindow::ShowRemovalConfirmation()
    {
        auto lifetime = get_strong();
        if (cpc::CountSelectedProducts(plan_.products) == 0 || !planRevisions_.IsPlanCurrent())
        {
            ShowMessage(T(L"План нужно проверить снова", L"Review the plan again"),
                        T(L"Сканирование или выбор изменились после построения плана.",
                          L"The scan or selection changed after the plan was built."));
            co_return;
        }
        if (!ValidateBackupPathForUi())
        {
            ShowMessage(T(L"Резервная папка недоступна", L"The backup folder is unavailable"),
                        T(L"Выберите безопасную папку с доступом на запись и достаточным свободным местом.",
                          L"Choose a safe, writable folder with enough free space."));
            co_return;
        }
        if (modalOpen_) co_return;
        modalOpen_ = true;
        try
        {
        const std::wstring phrase = language_ == cpc::Language::Russian ? L"УДАЛИТЬ" : L"DELETE";
        const bool highRisk = std::any_of(plan_.products.begin(), plan_.products.end(),
            [](cpc::InstalledProduct const& product) { return product.selected && product.risk == cpc::RiskLevel::High; });
        if (highRisk)
        {
            const std::wstring riskPhrase = language_ == cpc::Language::Russian ? L"РИСК" : L"RISK";
            ContentDialog warningDialog;
            warningDialog.XamlRoot(WindowRoot().XamlRoot());
            warningDialog.RequestedTheme(WindowRoot().RequestedTheme());
            warningDialog.Title(box_value(T(L"Отдельное подтверждение высокого риска", L"Separate high-risk confirmation")));
            warningDialog.PrimaryButtonText(T(L"Я понимаю риск", L"I understand the risk"));
            warningDialog.CloseButtonText(T(L"Отмена", L"Cancel"));
            warningDialog.DefaultButton(ContentDialogButton::Close);
            warningDialog.IsPrimaryButtonEnabled(false);
            StackPanel warningContent;
            warningContent.Spacing(10);
            warningContent.Children().Append(Text(T(
                L"Выбраны серверные или системно-критичные компоненты. Удаление может повлиять на вход в Windows, VPN, EFS и доступ к зашифрованным данным.",
                L"Server or system-critical components are selected. Removal can affect Windows sign-in, VPN, EFS, and access to encrypted data."), 11));
            warningContent.Children().Append(Text(T(L"Введите РИСК", L"Type RISK"), 11));
            TextBox riskConfirmation;
            riskConfirmation.PlaceholderText(riskPhrase);
            riskConfirmation.TextChanged([warningDialog, riskPhrase](IInspectable const& sender, TextChangedEventArgs const&) mutable
            {
                warningDialog.IsPrimaryButtonEnabled(std::wstring(sender.as<TextBox>().Text().c_str()) == riskPhrase);
            });
            warningContent.Children().Append(riskConfirmation);
            warningDialog.Content(PaddedDialogContent(warningContent));
            if (co_await warningDialog.ShowAsync() != ContentDialogResult::Primary)
            {
                modalOpen_ = false;
                co_return;
            }
        }
        ContentDialog dialog;
        dialog.XamlRoot(WindowRoot().XamlRoot());
        dialog.RequestedTheme(WindowRoot().RequestedTheme());
        dialog.Title(box_value(T(L"Проверьте план перед удалением", L"Review the plan before removal")));
        dialog.PrimaryButtonText(T(L"Удалить выбранное", L"Remove selected"));
        dialog.CloseButtonText(T(L"Вернуться к плану", L"Return to plan"));
        dialog.DefaultButton(ContentDialogButton::Close);
        dialog.IsPrimaryButtonEnabled(false);
        if (auto style = Application::Current().Resources().TryLookup(box_value(L"DangerButtonStyle")))
            dialog.PrimaryButtonStyle(style.as<Style>());

        StackPanel content;
        content.Spacing(12);
        content.Children().Append(Text(
            T(L"Выбрано продуктов: ", L"Selected products: ") + std::to_wstring(cpc::CountSelectedProducts(plan_.products)) +
            T(L" · подтверждённых целей: ", L" · verified targets: ") + std::to_wstring(cpc::CountVerifiedTargets(plan_.targets)) +
            T(L" · защищённых объектов: ", L" · protected objects: ") + std::to_wstring(plan_.protectedItems.size()) +
            T(L" · открытых сертификатов: ", L" · public certificates: ") +
            std::to_wstring(cpc::CountSelectedCertificates(scan_.certificates)), 12));
        content.Children().Append(Text(T(L"Сначала будет создана резервная копия, затем запущены зарегистрированные MSI/EXE-деинсталляторы и только после них — очистка подтверждённых остатков.",
                                        L"A backup is created first, then registered MSI/EXE uninstallers run, and only then are verified residuals cleaned."), 12));
        if (highRisk)
        {
            Border warning;
            warning.Padding(Thickness{12});
            warning.CornerRadius(CornerRadius{10});
            warning.Background(ThemeBrush(L"DangerSurfaceBrush"));
            warning.BorderBrush(ThemeBrush(L"DangerBorderBrush"));
            warning.BorderThickness(Thickness{1});
            warning.Child(Text(T(L"Высокий риск: операция может повлиять на ЭП, VPN, EFS, вход в Windows и зашифрованные данные.",
                                 L"High risk: this operation may affect digital signatures, VPN, EFS, Windows sign-in, and encrypted data."), 11));
            content.Children().Append(warning);
        }
        content.Children().Append(Text(T(L"Защищено: закрытые ключи, аппаратные токены, контейнеры и системные хранилища сертификатов.",
                                        L"Protected: private keys, hardware tokens, containers, and system certificate stores."), 11));
        content.Children().Append(Text(T(L"Папка резервной копии: ", L"Backup folder: ") + std::wstring(BackupPath().Text().c_str()), 11));
        content.Children().Append(Text(T(L"Проверка записи: папка доступна.", L"Write check: folder is available."), 11));
        content.Children().Append(Text(T(L"Для продолжения введите ", L"To continue, type ") + phrase, 11));
        TextBox confirmation;
        confirmation.PlaceholderText(phrase);
        confirmation.TextChanged([dialog, phrase](IInspectable const& sender, TextChangedEventArgs const&) mutable
        {
            dialog.IsPrimaryButtonEnabled(std::wstring(sender.as<TextBox>().Text().c_str()) == phrase);
        });
        content.Children().Append(confirmation);
        dialog.Content(PaddedDialogContent(content));
        const auto result = co_await dialog.ShowAsync();
        modalOpen_ = false;
        if (result == ContentDialogResult::Primary) ExecuteRemoval();
        }
        catch (...)
        {
            modalOpen_ = false;
            ReportAsyncFailure(T(L"Подтверждение удаления", L"Removal confirmation"), std::current_exception());
        }
    }

    winrt::fire_and_forget MainWindow::ExecuteRemoval()
    {
        auto lifetime = get_strong();
        cpc::UiOperationToken token;
        try
        {
            if (!planRevisions_.IsPlanCurrent())
            {
                ShowMessage(T(L"План устарел", L"Plan is stale"),
                            T(L"Перед удалением повторно проверьте план.", L"Review the plan again before removal."));
                co_return;
            }
            const std::wstring backup = BackupPath().Text().c_str();
            if (backup.empty() || !ValidateBackupPathForUi())
            {
                ShowMessage(T(L"Резервная копия обязательна", L"Backup is required"),
                            T(L"Выберите доступную папку. Без успешной записи резервной копии удаление не начнётся.",
                              L"Choose a writable folder. Removal cannot start until the backup is written successfully."));
                co_return;
            }
            token = BeginOperation(cpc::UiOperation::LiveCleanup,
                T(L"Создание обязательной резервной копии…", L"Creating the required backup…"), 0);
            if (!token) co_return;

            auto dispatcher = DispatcherQueue();
            auto weak = get_weak();
            apartment_context uiThread;
            auto scan = std::make_shared<cpc::ScanResult>(scan_);
            auto plan = std::make_shared<cpc::CleanupPlan>(plan_);
            auto execution = std::make_shared<cpc::ExecutionResult>();
            const auto language = language_;
            std::wstring licensesPath, reportPath, logPath, error;
            bool backupSaved = false;
            std::exception_ptr failure;

            co_await resume_background();
            try
            {
                backupSaved = cpc::SaveBackup(language, *scan, *plan, backup,
                    &licensesPath, &reportPath, &logPath, &error);
                if (backupSaved)
                {
                    *execution = cpc::ExecuteCleanup(*plan, false,
                        [weak, dispatcher, token](std::wstring const& message, int percent)
                        {
                            dispatcher.TryEnqueue([weak, message, percent, token]()
                            {
                                if (auto self = weak.get()) self->UpdateOperation(token, message, percent);
                            });
                        });
                }
            }
            catch (...)
            {
                failure = std::current_exception();
                if (backupSaved && !reportPath.empty())
                {
                    std::wstring emergencyError;
                    cpc::WriteEmergencyCleanupLog(ParentDirectory(reportPath) + L"\\emergency-cleanup.log",
                        *scan, L"Backup created; registered removal did not complete", execution.get(),
                        ERROR_UNHANDLED_EXCEPTION, &emergencyError);
                }
            }
            co_await uiThread;
            if (failure)
            {
                if (!reportPath.empty()) SetOutputSession(OutputSessionKind::LiveCleanup,
                    ParentDirectory(reportPath), execution.get());
                ReportAsyncFailure(T(L"Штатное удаление", L"Registered removal"), failure, token);
                co_return;
            }
            if (!backupSaved)
            {
                EndOperation(token, T(L"Удаление не началось: резервная копия не создана.",
                               L"Removal did not start: backup creation failed."), UiStatusKind::Error);
                ShowMessage(T(L"Резервная копия не создана", L"Backup failed"), error);
                co_return;
            }

            const bool uninstallerFailed = std::any_of(execution->operations.begin(), execution->operations.end(),
                [](cpc::OperationRecord const& operation)
                {
                    return operation.action == L"Uninstall" && operation.outcome == cpc::Outcome::Failed;
                });
            bool forceAuthorized = false;
            if (uninstallerFailed)
            {
                modalOpen_ = true;
                ContentDialog forceDialog;
                forceDialog.XamlRoot(WindowRoot().XamlRoot());
                forceDialog.RequestedTheme(WindowRoot().RequestedTheme());
                forceDialog.Title(box_value(T(L"Штатный деинсталлятор завершился ошибкой", L"Registered uninstaller failed")));
                forceDialog.PrimaryButtonText(T(L"Выполнить ограниченную очистку", L"Run limited forced cleanup"));
                forceDialog.CloseButtonText(T(L"Не выполнять", L"Do not force"));
                forceDialog.DefaultButton(ContentDialogButton::Close);
                forceDialog.IsPrimaryButtonEnabled(false);
                StackPanel forceContent;
                forceContent.Spacing(10);
                forceContent.Children().Append(Text(T(
                    L"Принудительная фаза затронет только цели, подтверждённые до запуска деинсталлятора. Введите FORCE.",
                    L"The forced phase will touch only targets verified before the uninstaller ran. Type FORCE."), 11));
                TextBox forceConfirmation;
                forceConfirmation.PlaceholderText(L"FORCE");
                forceConfirmation.TextChanged([forceDialog](IInspectable const& sender, TextChangedEventArgs const&) mutable
                {
                    forceDialog.IsPrimaryButtonEnabled(std::wstring(sender.as<TextBox>().Text().c_str()) == L"FORCE");
                });
                forceContent.Children().Append(forceConfirmation);
                forceDialog.Content(forceContent);
                const bool allowForce = co_await forceDialog.ShowAsync() == ContentDialogResult::Primary;
                modalOpen_ = false;
                if (allowForce)
                {
                    forceAuthorized = true;
                    operationGate_.Transition(token, cpc::UiOperation::ForcedCleanup);
                    UpdateOperation(token, T(L"Ограниченная принудительная очистка подтверждённых целей…",
                                             L"Limited forced cleanup of verified targets…"), 0);
                    auto forced = std::make_shared<cpc::ExecutionResult>();
                    failure = nullptr;
                    co_await resume_background();
                    try
                    {
                        *forced = cpc::ExecuteCleanup(*plan, true,
                            [weak, dispatcher, token](std::wstring const& message, int percent)
                            {
                                dispatcher.TryEnqueue([weak, message, percent, token]()
                                {
                                    if (auto self = weak.get()) self->UpdateOperation(token, message, percent);
                                });
                            });
                    }
                    catch (...)
                    {
                        failure = std::current_exception();
                        std::wstring emergencyError;
                        cpc::WriteEmergencyCleanupLog(ParentDirectory(reportPath) + L"\\emergency-cleanup.log",
                            *scan, L"Registered uninstallers completed; forced residual pass failed", execution.get(),
                            ERROR_UNHANDLED_EXCEPTION, &emergencyError);
                    }
                    co_await uiThread;
                    if (failure)
                    {
                        SetOutputSession(OutputSessionKind::LiveCleanup, ParentDirectory(reportPath), execution.get());
                        ReportAsyncFailure(T(L"Принудительная очистка", L"Forced cleanup"), failure, token);
                        co_return;
                    }
                    cpc::MergeExecutionResults(execution.get(), std::move(*forced));
                }
            }

            bool registerResume = false;
            cpc::ResumeAuthorization resumeAuthorization;
            resumeAuthorization.mode = forceAuthorized ? cpc::ResumeMode::ForcedResidual : cpc::ResumeMode::DeferredResidual;
            resumeAuthorization.forceAuthorized = forceAuthorized;
            resumeAuthorization.residualCleanupDeferred = execution->residualCleanupDeferred;
            resumeAuthorization.uninstallerFailurePresent = uninstallerFailed;
            const bool resumeAllowed = resumeAuthorization.AllowsResidualPass();
            if (execution->rebootRequired && resumeAllowed)
            {
                modalOpen_ = true;
                ContentDialog restartDialog;
                restartDialog.XamlRoot(WindowRoot().XamlRoot());
                restartDialog.RequestedTheme(WindowRoot().RequestedTheme());
                restartDialog.Title(box_value(T(L"Требуется продолжение после перезагрузки", L"Post-restart continuation is required")));
                restartDialog.PrimaryButtonText(T(L"Зарегистрировать продолжение", L"Register continuation"));
                restartDialog.CloseButtonText(T(L"Позже", L"Later"));
                restartDialog.DefaultButton(ContentDialogButton::Close);
                restartDialog.Content(box_value(T(
                    L"Будет создано одноразовое защищённое продолжение. Перезагрузить Windows нужно вручную — утилита сама этого не делает.",
                    L"A protected one-time continuation will be created. Restart Windows manually; the utility never restarts it on its own.")));
                registerResume = co_await restartDialog.ShowAsync() == ContentDialogResult::Primary;
                modalOpen_ = false;
            }
            else if (execution->rebootRequired && !resumeAllowed)
            {
                execution->operations.push_back({L"Register resume", L"One-time continuation",
                    cpc::Outcome::Skipped, ERROR_CANCELLED,
                    L"Resume was not authorized because an uninstaller failed and FORCE was declined."});
            }

            operationGate_.Transition(token, cpc::UiOperation::LiveCleanup);
            UpdateOperation(token, T(L"Повторная проверка и формирование отчётов…", L"Verifying and writing reports…"), 82);
            auto verification = std::make_shared<cpc::ScanResult>();
            bool reportsWritten = false;
            bool resumeRegistered = false;
            std::wstring resumeError;
            const std::wstring session = ParentDirectory(reportPath);
            failure = nullptr;
            co_await resume_background();
            try
            {
                *verification = cpc::VerifyAfterCleanup(language,
                    [weak, dispatcher, token](std::wstring const& message, int percent)
                    {
                        dispatcher.TryEnqueue([weak, message, percent, token]()
                        {
                            if (auto self = weak.get()) self->UpdateOperation(token, message,
                                82 + std::clamp(percent, 0, 100) * 17 / 100);
                        });
                    });
                if (execution->rebootRequired)
                {
                    if (registerResume)
                    {
                        wchar_t executable[32768]{};
                        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
                        const std::wstring runner = ParentDirectory(executable) + L"\\CryptoProCleanupResume.exe";
                        std::wstring resumeTokenValue;
                        resumeRegistered = cpc::PrepareResumeAuthorized(*plan, session, runner, language,
                            resumeAuthorization, &resumeTokenValue, &resumeError, true, {});
                        execution->operations.push_back({L"Register resume", L"One-time continuation",
                            resumeRegistered ? cpc::Outcome::Succeeded : cpc::Outcome::Failed,
                            resumeRegistered ? static_cast<DWORD>(ERROR_SUCCESS) : static_cast<DWORD>(ERROR_FILE_NOT_FOUND),
                            resumeRegistered ? L"Protected one-time post-restart continuation was registered." : resumeError});
                        if (!resumeRegistered) execution->anyFailure = true;
                    }
                    else if (resumeAllowed)
                    {
                        execution->operations.push_back({L"Register resume", L"One-time continuation",
                            cpc::Outcome::Skipped, ERROR_CANCELLED,
                            L"The user declined post-restart continuation registration."});
                    }
                }
                reportsWritten = cpc::WriteTextSummary(language, session + L"\\summary.txt", *scan,
                                      plan.get(), execution.get(), verification.get(), &error) &&
                                 cpc::WriteJsonReport(reportPath, *scan, plan.get(), execution.get(), verification.get(), &error);
                for (auto const& operation : execution->operations)
                {
                    const std::wstring line = operation.action + L": " + operation.target + L" — " + operation.message;
                    reportsWritten = cpc::AppendLog(logPath, cpc::RedactSensitiveText(line, *scan)) && reportsWritten;
                }
            }
            catch (...)
            {
                failure = std::current_exception();
                std::wstring emergencyError;
                cpc::WriteEmergencyCleanupLog(session + L"\\emergency-cleanup.log", *scan,
                    L"Cleanup ran; final verification or report writing failed", execution.get(),
                    ERROR_UNHANDLED_EXCEPTION, &emergencyError);
            }
            co_await uiThread;
            if (failure)
            {
                SetOutputSession(OutputSessionKind::LiveCleanup, session, execution.get());
                ReportAsyncFailure(T(L"Проверка и отчёты", L"Verification and reports"), failure, token);
                co_return;
            }

            SetOutputSession(OutputSessionKind::LiveCleanup, session, execution.get(),
                             verification->products.size() + verification->warnings.size());
            LogExecution(*execution, *scan);
            scan_ = *verification;
            plan_ = {};
            planRevisions_.ScanChanged();
            PopulateLiveScan();
            const bool partial = execution->anyFailure || !verification->products.empty() ||
                                 !verification->warnings.empty() || !reportsWritten ||
                                 (execution->rebootRequired && !resumeRegistered);
            const std::wstring finalStatus = execution->residualCleanupDeferred
                ? T(L"Штатное удаление выполнено; остаточная очистка ожидает перезагрузки.",
                    L"Registered removal completed; residual cleanup is waiting for restart.")
                : (partial ? T(L"Удалено частично. Точные причины сохранены в отчёте.",
                               L"Partially removed. Exact reasons were saved in the report.")
                           : T(L"Удалено полностью и повторно проверено.", L"Fully removed and verified."));
            EndOperation(token, finalStatus, execution->rebootRequired && resumeRegistered
                                           ? UiStatusKind::RestartRequired
                                           : (partial ? UiStatusKind::Partial : UiStatusKind::Success));
            AppendLogLine(finalStatus);
            std::wstring message = finalStatus + T(L"\n\nОтчёты: ", L"\n\nReports: ") + session;
            if (execution->rebootRequired)
                message += resumeRegistered
                    ? T(L"\n\nПродолжение зарегистрировано. Перезагрузите Windows вручную.",
                        L"\n\nContinuation is registered. Restart Windows manually.")
                    : T(L"\n\nПродолжение не зарегистрировано; операция остаётся частичной.",
                        L"\n\nContinuation was not registered; the operation remains partial.");
            ShowMessage(partial ? T(L"Удалено частично", L"Partially removed")
                                : T(L"Удалено полностью", L"Fully removed"), message);
        }
        catch (...)
        {
            modalOpen_ = false;
            ReportAsyncFailure(T(L"Удаление", L"Removal"), std::current_exception(), token);
        }
    }

    winrt::fire_and_forget MainWindow::SaveOffline_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!OfflineStateCurrent())
        {
            ShowMessage(T(L"Результат сканирования устарел", L"The scan result is stale"),
                        T(L"Проверьте путь и выполните повторное сканирование.", L"Verify the path and scan again."));
            co_return;
        }
        auto lifetime = get_strong();
        cpc::UiOperationToken token;
        try
        {
        FolderPicker picker;
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L"*");
        check_hresult(picker.as<IInitializeWithWindow>()->Initialize(GetWindowHandle()));
        auto folder = co_await picker.PickSingleFolderAsync();
        if (!folder) co_return;
        const std::wstring selectedBackupPath = folder.Path().c_str();
        apartment_context validationUiThread;
        cpc::BackupFolderValidation backupValidation;
        co_await resume_background();
        backupValidation = cpc::ProbeBackupFolder(selectedBackupPath);
        co_await validationUiThread;
        if (!backupValidation.ok())
        {
            ShowMessage(T(L"Папка недоступна", L"The folder is unavailable"), backupValidation.detail);
            co_return;
        }
        token = BeginOperation(cpc::UiOperation::SavingOfflineData,
            T(L"Сохранение найденных лицензий и сертификатов…", L"Saving rescued licenses and certificates…"));
        if (!token) co_return;
        auto dispatcher = DispatcherQueue();
        apartment_context uiThread;
        auto offline = std::make_shared<cpc::OfflineScanResult>(offline_);
        const auto language = language_;
        const std::wstring parent = folder.Path().c_str();
        co_await resume_background();
        std::wstring session;
        std::wstring error;
        bool saved = false;
        std::exception_ptr failure;
        try { saved = cpc::SaveOfflineBackup(language, *offline, parent, false, &session, &error); }
        catch (...) { failure = std::current_exception(); }
        co_await uiThread;
        if (failure)
        {
            ReportAsyncFailure(T(L"Сохранение данных офлайн-Windows", L"Saving offline Windows data"), failure, token, &offline->scan);
            co_return;
        }
        EndOperation(token, saved ? T(L"Найденные данные сохранены.", L"Rescued data saved.")
                                  : T(L"Сохранение не выполнено.", L"Rescue save failed."),
                     saved ? UiStatusKind::ExportComplete : UiStatusKind::Error);
        if (saved)
        {
            SetOutputSession(OutputSessionKind::OfflineRescue, session);
            AppendLogLine(T(L"Данные из офлайн-Windows сохранены без изменения исходного диска.",
                            L"Offline Windows data was saved without modifying the source drive."));
            ShowMessage(T(L"Данные сохранены", L"Data saved"), session);
        }
        else ShowMessage(T(L"Сохранение не выполнено", L"Save failed"), error);
        }
        catch (...)
        {
            ReportAsyncFailure(T(L"Сохранение данных офлайн-Windows", L"Saving offline Windows data"), std::current_exception(), token, &offline_.scan);
        }
    }

    winrt::fire_and_forget MainWindow::CleanOffline_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!OfflineStateCurrent() || !offline_.cleanupCapable) co_return;
        auto lifetime = get_strong();
        cpc::UiOperationToken token;
        try
        {
            const bool allProductsSelected = !offline_.scan.products.empty() && std::all_of(
                offline_.scan.products.begin(), offline_.scan.products.end(),
                [](cpc::InstalledProduct const& product) { return product.selected; });
            if (!allProductsSelected || !offlineProductsConfirmed_)
            {
                ShowMessage(T(L"Подтвердите найденные продукты", L"Confirm the detected products"),
                            T(L"Выберите все продукты и установите отдельный флажок, подтверждающий проверку набора.",
                              L"Select every product and tick the separate checkbox confirming that you reviewed the set."));
                co_return;
            }

            FolderPicker picker;
            picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
            picker.FileTypeFilter().Append(L"*");
            check_hresult(picker.as<IInitializeWithWindow>()->Initialize(GetWindowHandle()));
            auto folder = co_await picker.PickSingleFolderAsync();
            if (!folder) co_return;
            const std::wstring selectedBackupPath = folder.Path().c_str();
            const std::wstring selectedOfflineSource = offline_.windowsDirectory;
            apartment_context validationUiThread;
            cpc::BackupFolderValidation backupValidation;
            unsigned long long requiredBackupBytes = 0;
            auto offlineForEstimate = std::make_shared<cpc::OfflineScanResult>(offline_);
            co_await resume_background();
            requiredBackupBytes = cpc::EstimateOfflineBackupMinimum(*offlineForEstimate);
            backupValidation = cpc::ProbeBackupFolder(selectedBackupPath, selectedOfflineSource, requiredBackupBytes);
            co_await validationUiThread;
            const std::wstring backupVolume = VolumeRootOf(folder.Path().c_str());
            const std::wstring sourceVolume = VolumeRootOf(offline_.windowsDirectory);
            if (!backupValidation.ok())
            {
                const bool sameVolume = backupValidation.state == cpc::BackupFolderState::SameVolume;
                ShowMessage(sameVolume
                                ? T(L"Нужен другой том для резервной копии", L"Backup must use another volume")
                                : T(L"Резервная папка недоступна", L"The backup folder is unavailable"),
                            sameVolume
                                ? T(L"Для офлайн-очистки выберите папку на другом диске. Исходный том: ",
                                    L"For offline cleanup, choose a folder on another drive. Source volume: ") + sourceVolume
                                : backupValidation.detail);
                co_return;
            }

            modalOpen_ = true;
            ContentDialog dialog;
            dialog.XamlRoot(WindowRoot().XamlRoot());
            dialog.RequestedTheme(WindowRoot().RequestedTheme());
            dialog.Title(box_value(T(L"Расширенная офлайн-очистка", L"Advanced offline cleanup")));
            dialog.PrimaryButtonText(T(L"Очистить офлайн-Windows", L"Clean offline Windows"));
            dialog.CloseButtonText(T(L"Отмена", L"Cancel"));
            dialog.DefaultButton(ContentDialogButton::Close);
            dialog.IsPrimaryButtonEnabled(false);
            StackPanel content;
            content.Spacing(10);
            std::wostringstream confirmationDetails;
            confirmationDetails
                << T(L"Просканированный путь: ", L"Scanned path: ") << offlineScannedPath_ << L"\n"
                << T(L"Система: ", L"System: ") << offline_.scan.osName << L" · " << offline_.scan.osArchitecture << L"\n"
                << T(L"Продуктов: ", L"Products: ") << offline_.scan.products.size() << L" · "
                << T(L"проверенных целей: ", L"verified targets: ") << offline_.targets.size() << L"\n"
                << T(L"Исходный том: ", L"Source volume: ") << sourceVolume << L"\n"
                << T(L"Резервный том: ", L"Backup volume: ") << backupVolume << L"\n\n"
                << T(L"Свободно в резервном томе: ", L"Free on backup volume: ") << FormatByteCount(backupValidation.freeBytes) << L"\n"
                << T(L"Ожидаемый минимум: ", L"Expected minimum: ") << FormatByteCount(requiredBackupBytes) << L"\n\n"
                << T(L"Тома различаются. Штатные деинсталляторы отключённой Windows запустить невозможно, поэтому очистка консервативна и может завершиться частично. Закрытые ключи, контейнеры и хранилища сертификатов защищены.",
                     L"The volumes differ. Registered uninstallers in offline Windows cannot run, so cleanup is conservative and may remain partial. Private keys, containers, and certificate stores are protected.");
            content.Children().Append(Text(confirmationDetails.str(), 11));
            content.Children().Append(Text(T(L"Для продолжения введите OFFLINE", L"Type OFFLINE to continue"), 11));
            TextBox confirmation;
            confirmation.PlaceholderText(L"OFFLINE");
            confirmation.TextChanged([dialog](IInspectable const& sender, TextChangedEventArgs const&) mutable
            {
                dialog.IsPrimaryButtonEnabled(cpc::IsOfflineConfirmation(sender.as<TextBox>().Text().c_str()));
            });
            content.Children().Append(confirmation);
            dialog.Content(PaddedDialogContent(content));
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary)
            {
                modalOpen_ = false;
                co_return;
            }
            modalOpen_ = false;
            token = BeginOperation(cpc::UiOperation::OfflineCleanup,
                T(L"Создание обязательной офлайн-резервной копии…", L"Creating the required offline backup…"), 0);
            if (!token) co_return;

            auto dispatcher = DispatcherQueue();
            auto weak = get_weak();
            apartment_context uiThread;
            auto source = std::make_shared<cpc::OfflineScanResult>(offline_);
            auto execution = std::make_shared<cpc::ExecutionResult>();
            auto verification = std::make_shared<cpc::OfflineScanResult>();
            const auto language = language_;
            const std::wstring parent = folder.Path().c_str();
            std::wstring session, error;
            bool backupSaved = false;
            bool resultWritten = false;
            std::exception_ptr failure;
            co_await resume_background();
            try
            {
                backupSaved = cpc::SaveOfflineBackup(language, *source, parent, true, &session, &error);
                if (backupSaved)
                {
                    *execution = cpc::ExecuteOfflineCleanup(*source, session,
                        [weak, dispatcher, token](std::wstring const& message, int percent)
                        {
                            dispatcher.TryEnqueue([weak, message, percent, token]()
                            {
                                if (auto self = weak.get()) self->UpdateOperation(token, message, percent);
                            });
                        });
                    for (auto const& operation : execution->operations)
                    {
                        const std::wstring line = operation.action + L": " + operation.target + L" — " + operation.message;
                        cpc::AppendLog(session + L"\\offline-cleanup.log",
                                       cpc::RedactSensitiveText(line, source->scan));
                    }
                    *verification = cpc::ScanOfflineWindows(language, source->windowsDirectory);
                    const bool partial = execution->anyFailure || !verification->scan.products.empty() ||
                                         !verification->targets.empty() || !verification->valid;
                    std::wostringstream result;
                    result << L"CryptoPro Cleanup Utility " << cpc::kVersion << L"\r\n"
                           << (partial ? L"PARTIAL" : L"COMPLETED") << L"\r\n\r\n"
                           << L"Remaining products: " << verification->scan.products.size() << L"\r\n"
                           << L"Remaining verified targets: " << verification->targets.size() << L"\r\n\r\n";
                    auto outcomeName = [language](cpc::Outcome outcome) -> std::wstring {
                        switch (outcome) {
                        case cpc::Outcome::Succeeded: return language == cpc::Language::Russian ? L"успешно" : L"succeeded";
                        case cpc::Outcome::Skipped: return language == cpc::Language::Russian ? L"пропущено" : L"skipped";
                        case cpc::Outcome::Failed: return language == cpc::Language::Russian ? L"ошибка" : L"failed";
                        case cpc::Outcome::RebootRequired: return language == cpc::Language::Russian ? L"нужна перезагрузка" : L"restart required";
                        }
                        return language == cpc::Language::Russian ? L"неизвестно" : L"unknown";
                    };
                    for (auto const& operation : execution->operations)
                        result << L"[" << outcomeName(operation.outcome) << L"] "
                               << operation.action << L" | " << operation.target << L" | " << operation.message << L"\r\n";
                    resultWritten = cpc::WriteUtf8File(session + L"\\offline-result.txt",
                        cpc::Utf8(cpc::RedactSensitiveText(result.str(), source->scan)), &error);
                }
            }
            catch (...)
            {
                failure = std::current_exception();
                if (backupSaved && !session.empty())
                {
                    std::wstring emergencyError;
                    cpc::WriteEmergencyCleanupLog(session + L"\\emergency-cleanup.log", source->scan,
                        L"Offline backup created; offline cleanup or verification failed", execution.get(),
                        ERROR_UNHANDLED_EXCEPTION, &emergencyError);
                }
            }
            co_await uiThread;
            if (failure)
            {
                if (!session.empty()) SetOutputSession(OutputSessionKind::OfflineCleanup, session, execution.get());
                ReportAsyncFailure(T(L"Офлайн-очистка", L"Offline cleanup"), failure, token, &source->scan);
                co_return;
            }
            if (!backupSaved)
            {
                EndOperation(token, T(L"Очистка не началась: резервная копия не создана.", L"Cleanup did not start: backup failed."),
                             UiStatusKind::Error);
                ShowMessage(T(L"Офлайн-очистка не началась", L"Offline cleanup did not start"), error);
                co_return;
            }

            SetOutputSession(OutputSessionKind::OfflineCleanup, session, execution.get(),
                             verification->scan.products.size() + verification->targets.size());
            LogExecution(*execution, source->scan);
            offline_ = *verification;
            offlineInputPath_ = offline_.windowsDirectory;
            offlineScannedPath_ = offline_.windowsDirectory;
            ++offlineScanRevision_;
            offlineProductsConfirmed_ = false;
            PopulateOfflineScan();
            const bool partial = execution->anyFailure || !offline_.scan.products.empty() ||
                                 !offline_.targets.empty() || !offline_.valid || !resultWritten;
            const std::wstring status = partial
                ? T(L"Офлайн-очистка завершена частично; точные остатки сохранены.",
                    L"Offline cleanup completed partially; exact residuals were saved.")
                : T(L"Подтверждённые офлайн-цели обработаны и повторно проверены. Неизвестные остатки автоматически не удалялись.",
                    L"Verified offline targets were processed and checked again. Unknown residuals were not removed automatically.");
            EndOperation(token, status, partial ? UiStatusKind::Partial : UiStatusKind::Success);
            AppendLogLine(status);
            ShowMessage(partial ? T(L"Офлайн-очистка завершена частично", L"Offline cleanup completed partially")
                                : T(L"Офлайн-очистка завершена", L"Offline cleanup completed"),
                        status + T(L"\n\nОтчёты: ", L"\n\nReports: ") + session);
        }
        catch (...)
        {
            modalOpen_ = false;
            ReportAsyncFailure(T(L"Офлайн-очистка", L"Offline cleanup"), std::current_exception(), token, &offline_.scan);
        }
    }

    winrt::fire_and_forget MainWindow::ShowOfflineDiagnostics()
    {
        auto lifetime = get_strong();
        if (modalOpen_ || !operationGate_.idle()) co_return;
        modalOpen_ = true;
        try
        {
            std::wostringstream diagnostics;
            diagnostics << T(L"Просканированный путь: ", L"Scanned path: ") << offlineScannedPath_ << L"\r\n"
                        << L"SOFTWARE: " << offline_.softwareHivePath << L"\r\n"
                        << L"SYSTEM: " << offline_.systemHivePath << L"\r\n"
                        << T(L"Профили: ", L"Profiles: ") << offline_.scan.profiles.size() << L"\r\n"
                        << T(L"Сертификаты: ", L"Certificates: ") << offline_.scan.certificates.size() << L"\r\n\r\n";
            for (const auto& line : offline_.diagnostics) diagnostics << L"• " << line << L"\r\n";
            for (const auto& line : offline_.scan.warnings) diagnostics << L"• " << line << L"\r\n";
            const std::wstring redacted = cpc::RedactSensitiveText(diagnostics.str(), offline_.scan);
            ContentDialog dialog;
            dialog.XamlRoot(WindowRoot().XamlRoot());
            dialog.RequestedTheme(WindowRoot().RequestedTheme());
            dialog.Title(box_value(T(L"Диагностика офлайн-Windows", L"Offline Windows diagnostics")));
            dialog.PrimaryButtonText(T(L"Копировать обезличенный текст", L"Copy redacted text"));
            dialog.CloseButtonText(T(L"Закрыть", L"Close"));
            dialog.DefaultButton(ContentDialogButton::Close);
            TextBox details;
            details.Text(redacted);
            details.IsReadOnly(true);
            details.AcceptsReturn(true);
            details.TextWrapping(TextWrapping::Wrap);
            details.MinHeight(320);
            dialog.Content(PaddedDialogContent(details));
            const auto result = co_await dialog.ShowAsync();
            modalOpen_ = false;
            if (result == ContentDialogResult::Primary) CopyText(redacted);
        }
        catch (...)
        {
            modalOpen_ = false;
            ReportAsyncFailure(T(L"Диагностика офлайн-Windows", L"Offline Windows diagnostics"),
                               std::current_exception(), {}, &offline_.scan);
        }
    }

    winrt::fire_and_forget MainWindow::ShowMessage(std::wstring const& title, std::wstring const& message)
    {
        auto lifetime = get_strong();
        if (closing_) co_return;
        if (modalOpen_)
        {
            dialogQueue_.emplace_back(title, message);
            co_return;
        }
        modalOpen_ = true;
        try
        {
            ContentDialog dialog;
            dialog.XamlRoot(WindowRoot().XamlRoot());
            dialog.RequestedTheme(WindowRoot().RequestedTheme());
            dialog.Title(box_value(title));
            dialog.Content(box_value(message));
            dialog.CloseButtonText(T(L"Закрыть", L"Close"));
            dialog.DefaultButton(ContentDialogButton::Close);
            co_await dialog.ShowAsync();
            modalOpen_ = false;
        }
        catch (...)
        {
            modalOpen_ = false;
            MessageBoxW(GetWindowHandle(), message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        }
        if (!closing_ && !dialogQueue_.empty())
        {
            auto next = std::move(dialogQueue_.front());
            dialogQueue_.pop_front();
            ShowMessage(next.first, next.second);
        }
    }

    void MainWindow::CopyText(std::wstring const& value)
    {
        try
        {
            DataPackage package;
            package.SetText(value);
            Clipboard::SetContent(package);
            Clipboard::Flush();
        }
        catch (...)
        {
            ShowMessage(T(L"Не удалось открыть буфер обмена", L"Could not open the clipboard"),
                        T(L"Содержимое не скопировано. Закройте приложение, использующее буфер обмена, и повторите попытку.",
                          L"Nothing was copied. Close the application using the clipboard and try again."));
        }
    }

    void MainWindow::CopyLog_Click(IInspectable const&, RoutedEventArgs const&)
    {
        CopyText(logText_);
    }

    void MainWindow::ToggleTechnicalLog_Click(IInspectable const&, RoutedEventArgs const&)
    {
        technicalLogExpanded_ = !technicalLogExpanded_;
        UpdateReportsPage();
    }

    void MainWindow::OpenReportFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (outputSession_.folder.empty())
        {
            ShowMessage(T(L"Отчёт ещё не создан", L"No report yet"),
                        T(L"Папка появится после экспорта, резервного копирования или очистки.",
                          L"A report folder appears after export, backup, or cleanup."));
            return;
        }
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            GetWindowHandle(), L"open", outputSession_.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            ShowMessage(T(L"Не удалось открыть папку", L"Could not open the folder"), outputSession_.folder);
    }

    winrt::fire_and_forget MainWindow::OpenReportFile_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (outputSession_.folder.empty())
        {
            ShowMessage(T(L"Отчёт ещё не создан", L"No report yet"),
                        T(L"Папка появится после экспорта, резервного копирования или очистки.",
                          L"A report folder appears after export, backup, or cleanup."));
            co_return;
        }
        const auto button = sender.try_as<Button>();
        if (!button || !button.Tag()) co_return;
        const std::wstring name = unbox_value<hstring>(button.Tag()).c_str();
        const auto found = outputSession_.files.find(name);
        if (found == outputSession_.files.end())
        {
            ShowMessage(T(L"Файл ещё не создан", L"File not created yet"), name);
            co_return;
        }
        const bool confidential = name == L"licenses.txt" || name == L"certificates.txt" ||
                                  name == L"certificates.p7b" || name == L"recovery-map.txt";
        if (confidential)
        {
            if (modalOpen_) co_return;
            modalOpen_ = true;
            ContentDialog warning;
            warning.XamlRoot(WindowRoot().XamlRoot());
            warning.RequestedTheme(WindowRoot().RequestedTheme());
            warning.Title(box_value(T(L"Конфиденциальный файл", L"Confidential file")));
            warning.Content(PaddedDialogContent(Text(T(
                L"Этот файл может содержать полный серийный номер, имена владельцев сертификатов или пути восстановления. Не передавайте его посторонним.",
                L"This file may contain a full serial number, certificate holder names, or recovery paths. Do not share it with others."))));
            warning.PrimaryButtonText(T(L"Открыть файл", L"Open file"));
            warning.CloseButtonText(T(L"Отмена", L"Cancel"));
            warning.DefaultButton(ContentDialogButton::Close);
            ContentDialogResult choice = ContentDialogResult::None;
            try { choice = co_await warning.ShowAsync(); }
            catch (...) { modalOpen_ = false; co_return; }
            modalOpen_ = false;
            if (choice != ContentDialogResult::Primary) co_return;
        }
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            GetWindowHandle(), L"open", found->second.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            ShowMessage(T(L"Не удалось открыть файл", L"Could not open the file"), found->second);
        co_return;
    }

    std::wstring MainWindow::ExecutablePath() const
    {
        std::vector<wchar_t> path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        return length > 0 && static_cast<size_t>(length) < path.size() ? std::wstring(path.data(), length) : std::wstring();
    }

    void MainWindow::UpdateAboutSecurityState()
    {
        const std::wstring path = ExecutablePath();
        std::wstring signer;
        LONG trustStatus = ERROR_SUCCESS;
        const auto state = cpc::InspectFileSignature(path, &signer, &trustStatus);
        std::wstring text;
        wchar_t const* brush = L"SecondaryTextBrush";
        switch (state)
        {
        case cpc::FileSignatureState::Valid:
            text = T(L"Цифровая подпись действительна", L"Digital signature is valid");
            if (!signer.empty()) text += T(L". Подписант: ", L". Signer: ") + signer;
            brush = L"SuccessBrush";
            break;
        case cpc::FileSignatureState::Absent:
            text = T(L"Цифровая подпись: отсутствует", L"Digital signature: absent");
            break;
        case cpc::FileSignatureState::Invalid:
            text = T(L"Цифровая подпись присутствует, но проверку доверия не прошла", L"A digital signature is present but trust verification failed");
            if (!signer.empty()) text += T(L". Подписант: ", L". Signer: ") + signer;
            brush = L"DangerBrush";
            break;
        default:
            text = T(L"Состояние цифровой подписи определить не удалось", L"The digital signature state could not be determined") +
                L" (0x" + [&]() { std::wostringstream value; value << std::hex << static_cast<unsigned long>(trustStatus); return value.str(); }() + L")";
            brush = L"WarningBrush";
            break;
        }
        AboutSignatureStatus().Text(text);
        AboutSignatureStatus().Foreground(ThemeBrush(brush));
    }

    winrt::fire_and_forget MainWindow::ComputeHash_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        cpc::UiOperationToken token;
        try
        {
            token = BeginOperation(cpc::UiOperation::ComputingHash,
                T(L"Вычисление SHA-256 текущего EXE…", L"Computing the current EXE SHA-256…"));
            if (!token) co_return;
            const std::wstring path = ExecutablePath();
            std::wstring hash;
            std::wstring error;
            std::exception_ptr failure;
            apartment_context uiThread;
            co_await resume_background();
            try { hash = cpc::ComputeFileSha256(path, &error); }
            catch (...) { failure = std::current_exception(); }
            co_await uiThread;
            if (failure)
            {
                ReportAsyncFailure(T(L"Вычисление SHA-256", L"SHA-256 calculation"), failure, token);
                co_return;
            }
            if (hash.empty())
            {
                EndOperation(token, T(L"SHA-256 вычислить не удалось.", L"SHA-256 could not be calculated."), UiStatusKind::Error);
                ShowMessage(T(L"Ошибка SHA-256", L"SHA-256 error"), error);
                co_return;
            }
            AboutHashText().Text(L"SHA-256: " + hash);
            AboutHashText().Visibility(Visibility::Visible);
            EndOperation(token, T(L"SHA-256 текущего EXE вычислен.", L"The current EXE SHA-256 was calculated."));
        }
        catch (...)
        {
            ReportAsyncFailure(T(L"Вычисление SHA-256", L"SHA-256 calculation"), std::current_exception(), token);
        }
    }

    void MainWindow::ShowLocation_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ExecutablePathText().Text(ExecutablePath());
        ExecutablePathText().Visibility(Visibility::Visible);
    }

    void MainWindow::CopyExecutablePath_Click(IInspectable const&, RoutedEventArgs const&)
    {
        CopyText(ExecutablePath());
    }

    void MainWindow::OpenExecutableFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const std::wstring folder = ParentDirectory(ExecutablePath());
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            GetWindowHandle(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) ShowMessage(T(L"Не удалось открыть папку", L"Could not open the folder"), folder);
    }

    void MainWindow::OpenUrl(wchar_t const* url)
    {
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(GetWindowHandle(), L"open", url, nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            ShowMessage(T(L"Не удалось открыть ссылку", L"Could not open the link"),
                        T(L"Проверьте приложение браузера по умолчанию и повторите попытку.",
                          L"Check the default browser application and try again."));
    }

    void MainWindow::GitHub_Click(IInspectable const&, RoutedEventArgs const&) { OpenUrl(L"https://github.com/acidtmn/CryptoProCleanup"); }
    void MainWindow::Website_Click(IInspectable const&, RoutedEventArgs const&) { OpenUrl(L"https://kodalexandrova.ru"); }
    void MainWindow::Support_Click(IInspectable const&, RoutedEventArgs const&) { OpenUrl(L"https://yoomoney.ru/to/4100119195083142"); }
}
