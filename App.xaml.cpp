#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "src/cleanup.hpp"

#include <shellapi.h>
#include <sstream>

namespace winrt::CryptoProCleanupModern::implementation
{
    App::App()
    {
    }

    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        const cpc::CommandLineOptions options = cpc::ParseCommandLine(argc, argv);
        if (argv) LocalFree(argv);

        if (options.showHelp)
        {
            const std::wstring heading = std::wstring(L"CryptoPro Cleanup Utility ") + cpc::kVersion + L"\r\n\r\n";
            MessageBoxW(nullptr,
                (heading +
                L"--scan                 Safe scan only\r\n"
                L"--offline-scan <path>  Safe disconnected-Windows scan\r\n"
                L"--report <path>        Report path\r\n"
                L"--lang ru|en           Interface/report language\r\n"
                L"--resume <token>       Internal restart continuation").c_str(),
                L"CryptoPro Cleanup Utility", MB_OK | MB_ICONINFORMATION);
            ExitProcess(0);
        }
        if (!options.offlineWindowsPath.empty()) ExitProcess(cpc::RunOfflineScanCommand(options));
        if (options.scanOnly) ExitProcess(cpc::RunScanCommand(options));
        // A resumed cleanup is an exact residual pass over the protected state.
        // It must never fall through to the normal uninstaller confirmation UI.
        if (!options.resumeToken.empty()) ExitProcess(cpc::RunResumeCommand(options.resumeToken, true));

        try
        {
            auto mainWindow = winrt::make_self<MainWindow>();
            mainWindow->InitializeSession(options.language, {}, options.languageExplicit);
            window = *mainWindow;
            window.Activate();
        }
        catch (winrt::hresult_error const& error)
        {
            const std::wstring detail = std::wstring(L"Modern UI initialization failed (0x") +
                [&]() { std::wostringstream value; value << std::hex << static_cast<unsigned long>(error.code().value); return value.str(); }() +
                L"): " + std::wstring(error.message().c_str());
            OutputDebugStringW((detail + L"\r\n").c_str());
            MessageBoxW(nullptr, detail.c_str(), L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
        }
    }
}
