# CryptoPro Cleanup Utility

[![Windows build](https://github.com/acidtmn/CryptoProCleanup/actions/workflows/build.yml/badge.svg)](https://github.com/acidtmn/CryptoProCleanup/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/acidtmn/CryptoProCleanup?include_prereleases)](https://github.com/acidtmn/CryptoProCleanup/releases)
[![MIT License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Windows 7–11](https://img.shields.io/badge/Windows-7%20SP1%E2%80%9311-0078D6.svg)](#system-requirements)

> **Status: 0.4.0 RC4.** A user successfully completed full removal on a live Windows 10 x64 system. Detection of products and the complete license plus public-certificate export are confirmed on a real connected Windows 7 x86 disk. RC4 adds a recognizable application icon and fixes complete repaint plus bottom-summary translation when switching RU/EN.

An unofficial portable utility for backing up license identifiers and public certificates, controlled removal of installed CryptoPro products, and rescue from a disconnected Windows 7 SP1 through Windows 11 installation.

This project is not affiliated with Crypto-Pro LLC. CryptoPro names are used only to describe compatibility. The utility neither downloads nor runs `cspclean.exe`.

**[Download the latest build](https://github.com/acidtmn/CryptoProCleanup/releases)** · [Документация на русском](README.md)

## Important warning

Removing cryptographic software can affect electronic signatures, Windows sign-in, VPN, EFS, TLS, and access to encrypted data. A public release must pass the VM matrix in [docs/TEST_MATRIX.md](docs/TEST_MATRIX.md). An unsigned executable may trigger SmartScreen.

The utility deliberately preserves Windows certificate stores, hardware tokens, private-key containers, and any object whose CryptoPro ownership cannot be verified.

## Features

- native portable Unicode Win32 GUI with RU/EN languages and no .NET dependency;
- original multi-resolution application icon for Explorer, taskbar, window chrome, and UAC;
- resizable/maximizable native window with adaptive tables, system-DPI scaling, and Windows visual styles;
- verified 32/64-bit product discovery by publisher and MSI/EXE metadata;
- complete license display, clipboard copy, and confidential backup;
- selectable public-certificate inventory and CER/P7B export;
- registered MSI/EXE uninstaller first, then only pre-verified residual targets;
- services, drivers, providers, COM, browser, task, shortcut, and profile inventory;
- restart-safe continuation, masked JSON reporting, and a privacy-safe operation log;
- disconnected-Windows rescue for licenses and public certificates;
- separately confirmed, recovery-backed conservative offline cleanup.
- temporary mounting of real system `SOFTWARE`, `SYSTEM`, and `NTUSER.DAT` hives through `RegLoadKey`, followed by mandatory `RegUnLoadKey` cleanup;
- fallback profile discovery from `Users`, plus registry-backed user and local-machine public-certificate stores;
- public-certificate discovery in `AppData\Roaming\Microsoft\SystemCertificates\My\Certificates` without accessing private keys;
- copyable stage-by-stage offline diagnostics and a safe `--offline-scan` command.
- background disconnected-Windows scanning with a responsive window, continuous progress animation, and live stage text.

## System requirements

- Windows 7 SP1 through Windows 11;
- local administrator rights;
- x86, x64, or Windows 11 ARM64 through built-in x86 emulation;
- no installation, .NET runtime, PowerShell, or internet connection is needed to run the utility.

## Usage

Run `CryptoProCleanup.exe`; its manifest requests administrator rights. Use “Show / copy license” to view and copy the full identifier before removal. The Certificates tab lists each regular profile's Personal store with subject, issuer, and validity dates. Select entries to export public `.cer` files and a combined `.p7b`. Before reinstalling Windows, keep the generated backup outside the system drive. Restarts are never initiated without consent.

Each backup session contains `licenses.txt` with full identifiers, a `CryptoProCertificates-*` directory with selected public `.cer` files, `certificates.p7b`, and a confidential `certificates.txt` catalog, plus a human-readable `summary.txt`, structured `report.json`, and `cleanup.log`. Full identifiers and certificate subject/issuer names are never written to the summary, JSON report, or log. For MSI products, the utility derives the dynamic Windows Installer key from ProductCode and prefers the complete `InstallProperties\ProductID`; the shorter `WLProductID` is only a fallback and is hidden when it is a prefix of the complete value.

CER and P7B files contain public certificates only. They can be inspected or imported as public data, but they do not replace a private key and cannot by themselves sign documents.

CryptoPro CSP 4.x and 5.x (and available 3.x installations) are discovered from confirmed publisher and MSI/EXE metadata rather than a hard-coded ProductCode list. The registered MSI/EXE uninstaller always runs before any pre-verified residual target is cleaned. If it requests a restart, residual cleanup is deferred until the protected post-login continuation. A failed uninstaller blocks normal residual cleanup; the separately confirmed advanced path is limited to the same pre-built plan.

## Disconnected Windows rescue

On the Disconnected Windows tab, choose either a drive root such as `E:\` or its `E:\Windows` directory and scan it read-only. The window remains responsive, a continuous indicator moves, and the current stage is shown; a slow HDD/USB scan may take several minutes. Licenses and selected public certificates can be rescued without changing the disk. Advanced offline cleanup cannot run the disconnected installation's registered MSI/EXE uninstaller, so it is explicitly forced and conservative. It requires all detected products, the typed phrase `OFFLINE`, and a backup on another volume. Complete `SOFTWARE` and `SYSTEM` hives plus quarantined file copies and a recovery map are created before writes. User `NTUSER.DAT` files, certificate stores, tokens, and private-key containers remain read-only. Unknown COM/browser remnants are retained and can produce a partial result.

Safe scan-only CLI:

```text
CryptoProCleanup.exe --scan --report C:\Temp\cryptopro-report.json --lang en
```

Safe disconnected-Windows diagnostics without removal:

```text
CryptoProCleanup.exe --offline-scan E:\Windows --report C:\Temp\offline-diagnostic.txt --lang en
```

Version 0.4.0 has no unattended destructive mode.

## Build and package

Visual Studio 2022 with Desktop development with C++ and a Windows SDK is required.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

GitHub Actions builds and tests the executable for every push and pull request. Verified unsigned binaries are published only under [Releases](https://github.com/acidtmn/CryptoProCleanup/releases); compare them with the included SHA-256 list.

## Technology

- C++17, Unicode Win32 API, and Windows Common Controls;
- Windows Installer API, CryptoAPI, Registry API, SCM, and SetupAPI;
- COM, Task Scheduler API, and Shell API for verified integrations;
- MSVC v143 with static CRT (`/MT`), Windows 7 API target and PE subsystem `6.01`;
- `requireAdministrator` UAC manifest, idempotent cleanup planning, and junction/reparse-point protection.

## Author, license, and support

Author: **Kirill Alexandrov** · [kodalexandrova.ru](https://kodalexandrova.ru)

The source code is available under the [MIT License](LICENSE). The software is provided “as is”, without warranty. CryptoPro names belong to their respective owners; this project is unaffiliated.

[![Support via YooMoney](https://img.shields.io/badge/YooMoney-Support%20the%20project-8B3FFD?style=for-the-badge)](https://yoomoney.ru/to/4100119195083142)

Russian documentation: [README.md](README.md).
