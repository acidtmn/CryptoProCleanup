# Release test matrix

Do not label a build as generally available until each mandatory scenario has a recorded result and the before/after private-key-container and certificate-store inventories match.

## Operating systems

| OS | Architecture | Required |
|---|---:|:---:|
| Windows 7 SP1 | x86, x64 | Yes |
| Windows 8 | x86, x64 | Yes |
| Windows 8.1 | x86, x64 | Yes |
| Windows 10 | x86, x64 | Yes |
| Windows 11 | x64 | Yes |
| Windows 11 | ARM64 (x86 emulation) | Yes |

## Product scenarios

- no CryptoPro products installed;
- CSP 3.x/4.x/5.x where a legally obtained installer supports the guest OS;
- CSP plus CryptoPro EDS Browser plug-in;
- multiple client products;
- a high-risk/server product to verify typed confirmation;
- broken MSI registration and missing EXE uninstaller;
- locked CryptoPro process/driver and restart continuation;
- license present, absent, and backup folder unwritable;
- current, loaded secondary, and offline Personal (`My`) certificate stores; selected `.cer` and `.p7b` export; no private-key export;
- one, multiple, loaded, and offline user profiles;
- disconnected Windows on another volume: rescue-only scan, full license extraction, certificate export, same-volume backup rejection, current-Windows rejection, recovery copies, and conservative offline cleanup;
- cancellation at every confirmation screen.

## Acceptance evidence

For each VM retain the initial JSON report, final JSON report, cleanup log, product list, and hashes/inventories of protected certificate and private-key locations. A passing run has no selected installed-product entries or verified executable registrations left. Protected stores are unchanged. Any unknown or locked object must produce a partial result rather than be deleted speculatively.

## Recorded validation

- 2026-08-21: 0.5.3-rc1 local Release builds passed for Win32 Legacy and x64 Modern, including the native x64 resume helper. Native unit tests passed on x86 and x64 with no-side-effect backup inspection, nearest-parent resolution, write-probe cleanup, explicit required-space reporting, offline hive/quarantine size estimation, and `NotBuilt`/`Ready`/`Stale` plan state in addition to the existing safety suite.
- 2026-08-21: the 0.5.3-rc1 theme and layout checks passed: 82 matching Dark/Light/High Contrast keys, 21 resolved references, 15 spacing resources, seven semantic card styles, and the documented zero-padding whitelist. Safe UI automation passed Modern launch, 1024×760 and 1280×720 resize, six pages, Dark/Light/System switching, RU/EN switching, table/card geometry, full-width/height checks for the long certificate actions, and pixel-level Dark-theme checks for compact-navigation backgrounds and icon contrast. Legacy automation verified visible, non-overlapping actions on all six pages at 800×600, 1024×768, and a simulated 768×528 work area; screenshots were visually reviewed. This is not a replacement for the pending Windows 7 VM retest. OS-level High Contrast and 125%, 150%, and 200% DPI sessions were not executed.
- 2026-08-21: no destructive CryptoPro cleanup, disconnected-disk write, real RunOnce registration, reboot, or live resume execution was performed for 0.5.3-rc1. These and the remaining OS/CSP VM matrix are still required before a stable release.
- 2026-08-21: 0.5.2-dev local Release builds passed for Win32 Legacy and x64 Modern, including the native x64 resume helper. The expanded native unit suite passed on x86 and x64, including certificate status/date ordering, filtered selection, selected-product/verified-target counts, operation-gate and plan-revision rules, authorized FORCE/resume state, and tampered copied-helper rejection.
- 2026-08-21: the static theme-resource checker passed with identical 82-key Dark, Light, and High Contrast dictionaries and all 21 literal runtime/XAML references resolved. It also rejected hard-coded hexadecimal MainWindow colors, hard-coded runtime brushes, brush `StaticResource` use in MainWindow, and a forced XAML Dark theme.
- 2026-08-21: safe UI automation smoke passed on the development Windows 11 x64 system for Modern launch, responsive resize at 1024×760 and 1280×720, all six pages, compact empty reports, visible checkbox accessibility state, Dark/Light/System switching, and immediate RU/EN title plus live-scan-log switching. Screenshots were visually reviewed for Dark/Light Settings and the English System-theme About, Offline Windows, and Reports pages. This was not a destructive cleanup test and did not exercise an OS-level High Contrast session or non-100% DPI.
- 2026-08-21: 0.5.1-dev local Release builds passed for Win32 Legacy, x64 Modern, and the native x64 resume helper. Unit tests covered the operation gate, plan revisions, exact `OFFLINE`, result merging, sensitive-log redaction, and isolated versioned resume state/runner/load/cleanup without RunOnce or HKLM writes.
- 2026-08-21: non-destructive integration scans passed in x64 and Win32 test binaries on the development Windows system. Both found 2 confirmed products, 1 regular profile, and 7 public certificates; the architecture-specific plans contained 27 and 26 verified targets. Temporary CER/P7B exports reopened through CryptoAPI, redacted report checks passed, and the running Windows directory was rejected as an offline target.
- 2026-08-21: the x64 resume helper was verified as a native x64 PE with Windows subsystem 6.01, static MSVC runtime (no VCRUNTIME/MSVCP dependency), `requireAdministrator`, and only Windows system DLL dependencies. No destructive cleanup, RunOnce registration, offline-disk write, or reboot was performed during development validation.

- 2026-08-20: 0.5.0-dev Modern C++/WinUI 3 x64 self-contained build started successfully on Windows 11 x64. Wide and 1024×760 responsive layouts, live safe scanning, sidebar collapse, immediate RU/EN footer translation, and the confidential license dialog were checked without printing full license values. No destructive operation was run.

- 2026-08-19: Windows 11 x64 build 26100, CryptoPro CSP 5.0.13000 plus CryptoPro EDS Browser plug-in 2.0.15400 — unit tests and non-destructive integration scan passed. The scan found the complete MSI `InstallProperties\ProductID` through the packed ProductCode-derived path and enumerated seven public certificates from the current user's logical Personal store without exposing values or names in test output. Temporary CER/P7B export was reopened through CryptoAPI and removed. No removal was performed.
- 2026-08-20: user field report — full installed-product removal and residual cleanup completed successfully on a live Windows 10 x64 system.
- 2026-08-20: version 0.3.0 RC2 running on Windows 10 x64 returned zero products and certificates for a connected, bootable Windows 7 x86 disk. Version 0.4.0 RC1 added registry-view, MSI Installer UserData, profile-fallback, and certificate-store changes, but a repeat test still returned zero results.
- 2026-08-20: version 0.4.0 unit tests, non-destructive integration scan, and MSVC C++ static analysis passed. The release executable was verified as x86 with OS/subsystem version 6.01, `requireAdministrator`, system-DPI awareness, Common Controls v6, and no dynamic Visual C++ runtime dependency.
- 2026-08-20: user repeat test of 0.4.0 RC1 still returned zero products and certificates on the connected Windows 7 x86 disk. Reproduction showed `RegLoadAppKey` returning `ERROR_BADDB` for a valid saved system `SOFTWARE` hive; `RegLoadKey` opened and unloaded the same hive successfully. RC2 replaces the loader and adds serialized certificate-file discovery.
- 2026-08-20: user testing of 0.4.0 RC2 on the real connected Windows 7 x86 disk confirmed product detection, the correct complete license identifier, and successful public-certificate export. RC3 moves this scan to a responsive background worker with continuous progress indication.
- 2026-08-20: read-only synthetic disconnected-Windows fixture passed with 2 confirmed CryptoPro products, 4 license candidates, 1 inactive profile, and 7 public certificates from `AppData\Roaming\Microsoft\SystemCertificates\My\Certificates`; both `SOFTWARE` and `SYSTEM` temporary mounts unloaded successfully.
- 2026-08-20: RC2 implementation passed unit tests, the non-destructive live integration scan, and MSVC C++ static analysis with no warnings. No `CryptoProCleanup_Offline_*` registry mounts remained after testing; sensitive hive/certificate fixture copies were removed.
- 2026-08-20: RC4 automated GUI probing confirmed immediate RU-to-EN translation of affected labels/buttons, status, and the product/license/certificate summary. Embedded 16- and 32-pixel icon resources loaded successfully from the Release EXE.
- The running Windows directory is rejected as an offline target. A destructive disconnected-Windows VM test is still pending.
- 2026-08-19: MSVC C++ static analysis passed with no warnings. Offline scanning also compares the actual `SOFTWARE` and `SYSTEM` hive file identities so a filesystem alias to the running Windows installation is rejected.
- Destructive scenarios and the remaining OS/CSP matrix, including CSP 4.x, are still mandatory before marking a build generally available.
