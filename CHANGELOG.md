# Changelog

All notable project changes are documented here. The project follows semantic versioning after the first generally available release.

## [0.5.3-rc1] - 2026-08-21

### Changed

- Introduced shared spacing tokens and semantic content, statistic, table, callout, and navigation card styles across Modern; product and offline-result rows now use dedicated table insets instead of accumulated per-card padding.
- Restored the studio accent palette and fixed button label inheritance in Dark, Light, System, and High Contrast resources.
- Made the page header, backup selector, certificate toolbar, offline selector, Settings rows, Reports actions, About actions, and dialogs adapt to narrow windows without truncating known RU/EN labels.
- Added a dedicated Legacy low-resolution layout: the minimum window is now 640×480, all six navigation entries and page actions remain visible at 800×600/1024×768, and the effective 768×528 work-area layout compacts tables, settings, reports, About links, and Offline Windows controls without overlapping buttons.
- Replaced generic completion text with operation-specific semantic states and preserved the originating operation when asynchronous failures are reported.
- Split backup validation into side-effect-free inspection and a debounced background write probe. The UI now shows free and required space; advanced offline cleanup estimates complete hive and verified quarantine-copy size with a safety margin.
- Kept invalid offline diagnostics accessible, invalidated product-set confirmation on every selection change, corrected live/offline license-dialog context, and made cleanup-result wording explicitly conservative.
- Reports now use actual stage outcomes, compact safe-scan summaries, an optional technical log, complete session-file discovery, and a warning before confidential files are opened.
- Theme refresh updates runtime rows and badges in place without rebuilding their models, selections, or handlers. Dead retained execution state was removed.
- Fixed stale Light-theme brushes on the compact Modern navigation rail: Dark now refreshes every button background, selected state, border, and icon foreground after startup and every theme change.

### Safety and validation

- Added static layout-resource checks and expanded safe native tests for no-side-effect backup inspection, path ancestry, write-probe cleanup, offline space estimation, and `NotBuilt`/`Ready`/`Stale` plan state.
- Modern UI automation now samples compact-navigation pixels in Dark theme and fails if a button keeps a light background or its symbol loses contrast.
- Local Release Win32 Legacy and x64 Modern builds, native x86/x64 unit tests, theme/layout checks, and non-destructive UI automation passed. Modern screenshots cover 1024×760 and 1280×720; Legacy screenshots cover 800×600, 1024×768, and a simulated 768×528 work area at the development system's 100% DPI. OS-level High Contrast and 125–200% DPI sessions were not executed.
- No destructive CryptoPro cleanup, offline-disk write, real RunOnce registration, reboot, or live resume execution was performed during 0.5.3-rc1 validation.

## [0.5.2-dev] - 2026-08-21

### Changed

- Replaced the forced-dark Modern theme with matching Dark, Light, and High Contrast resource dictionaries; System follows Windows at runtime, including dialogs and title bar.
- Added generation-bound asynchronous operations, semantic status preservation, complete plan-input revisions, selected-only plan counts, detailed target categories, and stricter conflict gating.
- Bound every offline result to its scanned path, added explicit product-set confirmation, richer certificate selection/diagnostics, another-volume backup validation, and correct offline redaction context.
- Hardened FORCE/resume authorization, transactional helper deployment, runner identity/hash validation, bounded retries, and retained failed state for manual inspection.
- Added real certificate-date sorting and status filters, filtered-only bulk selection, keyboard/UI Automation rows, session-specific Reports actions/timeline, emergency redacted logs, and executable signature/SHA-256 inspection in About.
- Window placement is monitor-validated and bounded; reduced motion now also honors the Windows animation setting.

### Safety and validation

- Added a theme-resource consistency checker to every local build and expanded non-destructive unit tests for operation generations, plan revisions, redaction boundaries, backup validation, certificate sorting/selection, FORCE authorization, and tampered resume helpers.
- No destructive cleanup, real RunOnce registration, reboot, or live resume execution was performed during 0.5.2-dev development validation.

## [0.5.1-dev] - 2026-08-21

### Changed

- Stabilized the Modern x64 operation model with one explicit state gate, scan/selection plan revisions, blocked overlapping commands, and guarded window closing.
- Restored the full uninstall-first workflow in Modern: mandatory backup, registered MSI/EXE pass, exact `FORCE` confirmation after uninstaller failure, verified residual pass, verification, redacted logs, and structured reports.
- Added a compact native x64 `CryptoProCleanupResume.exe`. Its protected versioned state contains only the preverified residual plan; the helper never reruns registered uninstallers and never restarts Windows.
- Expanded disconnected-Windows selection, diagnostics, certificate choices, exact `OFFLINE` confirmation, recovery-backed result logging, and post-cleanup rescan.
- Added persisted Modern settings, dark title-bar integration, certificate validity/sort/bulk filters, a searchable categorized plan inspector, and a structured Reports page.

### Safety and validation

- Added isolated tests for operation gating, plan revisions, exact confirmation phrases, execution-result merging, sensitive log redaction, and resume state/runner/load/cleanup without RunOnce or HKLM writes.
- Local Release Win32/x64 builds and non-destructive tests are required by the package script. No destructive CryptoPro, offline-disk, RunOnce, or reboot test is performed on the development machine.

## [0.5.0-dev] - 2026-08-20

### Added

- Separate native C++/WinUI 3 x64 application for Windows 10/11, closely following the supplied dark Fluent mockups; the Windows 7-compatible x86 Win32 edition remains available as Legacy.
- Sidebar navigation, overview/status cards, dedicated certificate, Offline Windows, reports, settings, and about pages, typed destructive confirmations, and a read-only cleanup-plan inspector.
- Responsive layout that collapses the sidebar and stacks cards/details as the window narrows, while retaining native DPI scaling and keyboard-accessible controls.
- Confidential full-license dialog with copy support for both the running and offline Windows scans. Full values remain excluded from JSON and the redacted log.
- Self-contained Windows App SDK 2.4 runtime packaging, including the official runtime resource index extracted reproducibly from Microsoft's NuGet MSIX.

### Fixed

- Prevented early WinUI `SelectionChanged` events from accessing not-yet-connected XAML controls during startup.
- RU/EN switching now refreshes the footer status immediately instead of retaining the language used by the completed scan.
- XAML resources stored in project subdirectories no longer produce mismatched unpackaged `ms-appx` paths.

### Validation

- Modern Release x64 starts successfully in both framework-dependent diagnostic and self-contained portable layouts on Windows 11 x64.
- Automated UI Automation checks confirmed immediate RU-to-EN status translation and opening of the confidential license dialog without emitting license values to test output.
- Wide and 1024×760 layouts were launched and visually checked; no destructive cleanup operation was executed during development.

## [0.4.0-rc4] - 2026-08-20

### Added

- Original blue shield/cleanup application icon embedded at 16–256 pixel sizes for Explorer, the title bar, taskbar, and UAC display.
- The transparent high-resolution icon source and multi-resolution ICO are included in the open-source package.

### Fixed

- RU/EN switching now redraws the dialog atomically, preventing labels, buttons, and some list headers from appearing blank until a later repaint.
- The scan-complete status and count summary at the bottom are immediately regenerated in the selected language.
- Product, profile, and certificate selections are preserved while translated lists are rebuilt.

### Validation

- Automated GUI probing confirmed that all affected captions, the status, and the product/license/certificate summary change from Russian to English immediately.
- Both 16- and 32-pixel icon resources load successfully from the release EXE; core tests and the Release x86 build pass.

## [0.4.0-rc3] - 2026-08-20

### Changed

- Disconnected-Windows scanning now runs on a background thread, keeping the main window responsive while slow HDD/USB media are read.
- The progress bar uses a continuous marquee during offline scans, the scan button shows an active state, and the status line reports each scan stage.
- Offline path guidance and the folder-picker title now explicitly say that either a drive root such as `E:\` or its `E:\Windows` directory can be selected.

### Validation

- The user confirmed that RC2 detects the products and full license on the real connected Windows 7 x86 disk and successfully exports its public certificates.
- Release x86 build and all core tests pass after the asynchronous GUI change.

## [0.4.0-rc2] - 2026-08-20

### Fixed

- Replaced `RegLoadAppKey` for disconnected Windows system/user hives with privileged temporary `RegLoadKey` mounts and guaranteed `RegUnLoadKey` cleanup. A saved system `SOFTWARE` hive reproducibly returned `ERROR_BADDB` through the former API but opens correctly through the new loader.
- Offline public-certificate discovery now parses serialized certificate files under each profile's `AppData\Roaming\Microsoft\SystemCertificates\My\Certificates`, in addition to registry-backed Personal stores.
- Inactive-profile cleanup on the running system uses the same system-hive-capable loader.

### Added

- Copyable offline diagnostics in the GUI, opened automatically after an unrecognized or completely empty scan.
- Safe `--offline-scan <Windows path> --report <text path>` diagnostic command.
- Stage-by-stage offline diagnostics with resolved paths, API result codes, counts, and mount/unmount results; full licenses and certificate contents are excluded.

### Validation

- A synthetic disconnected-Windows fixture built from saved system hives passed the full read-only pipeline with two CryptoPro products, four license candidates, one inactive profile, and seven file-backed public certificates.
- A repeat test on the user's connected Windows 7 x86 disk remains mandatory.

## [0.4.0-rc1] - 2026-08-20

### Added

- Resizable and maximizable native GUI with adaptive lists, system-DPI awareness, modern system theming, and a larger title treatment.
- Offline profile fallback enumeration from the physical `Users` directory.
- Offline local-machine public-certificate store scan in addition to user Personal stores.
- MSI packed ProductCode decoding so confirmed Installer UserData can recover products missing from the uninstall list.

### Fixed

- Explicit native registry-view access prevents the x86 utility from losing physical offline registry paths on an x64 rescue host.
- `SOFTWARE`, user `NTUSER.DAT`, and `SYSTEM` hives are loaded sequentially, respecting the Windows 7 one-application-hive-per-process limitation.
- Offline data rescue remains available if `SYSTEM` cannot be read; destructive offline cleanup is disabled in that state.

### Validation

- User-reported full uninstall and residual cleanup succeeded on a live Windows 10 x64 installation.
- The Windows 7 x86 disconnected-drive scenario that returned zero products/certificates in RC2 is pending a repeat test with this fix.

## [0.3.0-rc2] - 2026-08-19

### Changed

- Replaced the YooMoney support destination in the Russian and English README.
- Added a localized “Support the project” link to the application footer alongside GitHub and the author website.

## [0.3.0-rc1] - 2026-08-19

### Added

- Native RU/EN Win32 interface for installed-product, profile, certificate, and disconnected-Windows workflows.
- Complete license extraction through dynamically derived Windows Installer metadata, with masked reports and confidential text backup.
- Selectable public-certificate inventory and DER CER/P7B export without private-key export.
- Registered MSI/EXE uninstall-first workflow, verified residual plan, guarded force mode, and restart continuation.
- Read-only disconnected-Windows rescue and separately confirmed recovery-backed conservative offline cleanup.
- Unit and safe integration tests, VM acceptance matrix, reproducible package script, and SHA-256 manifests.

### Validation

- Windows 11 x64 build 26100 with CryptoPro CSP 5.0.13000 and CryptoPro EDS Browser plug-in 2.0.15400: non-destructive integration scan passed.
- Seven public certificates were enumerated and a temporary CER/P7B export was reopened through CryptoAPI.
- MSVC C++ static analysis completed without warnings.

### Known limitations

- The full destructive Windows 7–11 and CryptoPro 3.x/4.x/5.x VM matrix is not complete.
- The executable is not code-signed, so SmartScreen warnings are expected.
- Disconnected Windows cannot run its registered installer; offline cleanup is necessarily conservative and may report partial removal.
