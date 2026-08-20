# Changelog

All notable project changes are documented here. The project follows semantic versioning after the first generally available release.

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
