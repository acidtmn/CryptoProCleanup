# Contributing

Contributions are welcome, especially conservative detection improvements, translations, tests, and documentation.

## Safety rules

- Never add a global “delete everything containing CryptoPro” search.
- A target must be derived from confirmed publisher, installer, service, driver, provider, or integration metadata.
- Certificate stores, hardware tokens, private-key containers, Windows Installer cache files, and unknown remnants must remain protected.
- Do not commit real licenses, certificate identities, reports, registry hives, or other personal data.
- Destructive tests belong in disposable VMs with before/after inventories of certificates and key containers.

## Build and test

Install Visual Studio 2022 with Desktop development with C++ and a Windows SDK, then run:

```powershell
.\scripts\build.ps1 -Configuration Release
```

The script builds the Win32 GUI and test executable and runs the core test suite. Record manual VM results in `docs/TEST_MATRIX.md`. Keep pull requests focused and explain any newly authorized deletion target and its validation evidence.
