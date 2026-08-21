#include "cleanup.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <memory>

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ wchar_t*, _In_ int) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                                    ICC_PROGRESS_CLASS | ICC_TAB_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&controls);

    int argc = 0;
    wchar_t** rawArguments = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(rawArguments, LocalFree);
    cpc::CommandLineOptions options = cpc::ParseCommandLine(argc, rawArguments);
    int result = 0;
    if (options.showHelp) {
        const std::wstring heading = std::wstring(L"CryptoPro Cleanup Utility ") + cpc::kVersion + L"\r\n\r\n";
        MessageBoxW(nullptr,
            (heading +
            L"--scan                 Safe scan only\r\n"
            L"--offline-scan <path>  Safe disconnected-Windows scan\r\n"
            L"--report <path>        Report path\r\n"
            L"--lang ru|en           Interface/report language\r\n"
            L"--resume <token>       Internal restart continuation").c_str(),
            L"CryptoPro Cleanup Utility", MB_OK | MB_ICONINFORMATION);
    } else if (!options.offlineWindowsPath.empty()) {
        result = cpc::RunOfflineScanCommand(options);
    } else if (options.scanOnly) {
        result = cpc::RunScanCommand(options);
    } else {
        result = cpc::RunGui(instance, options.language, options.resumeToken, options.languageExplicit);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
