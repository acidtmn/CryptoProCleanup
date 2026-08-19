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

- 2026-08-19: Windows 11 x64 build 26100, CryptoPro CSP 5.0.13000 plus CryptoPro EDS Browser plug-in 2.0.15400 — unit tests and non-destructive integration scan passed. The scan found the complete MSI `InstallProperties\ProductID` through the packed ProductCode-derived path and enumerated seven public certificates from the current user's logical Personal store without exposing values or names in test output. Temporary CER/P7B export was reopened through CryptoAPI and removed. No removal was performed.
- The running Windows directory is rejected as an offline target. A destructive disconnected-Windows VM test is still pending.
- 2026-08-19: MSVC C++ static analysis passed with no warnings. Offline scanning also compares the actual `SOFTWARE` and `SYSTEM` hive file identities so a filesystem alias to the running Windows installation is rejected.
- Destructive scenarios and the remaining OS/CSP matrix, including CSP 4.x, are still mandatory before marking a build generally available.
