# Changelog

All notable project changes are documented here. The project follows semantic versioning after the first generally available release.

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
