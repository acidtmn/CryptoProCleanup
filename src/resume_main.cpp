#include "cleanup.hpp"

#include <shellapi.h>

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ wchar_t*, _In_ int) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const cpc::CommandLineOptions options = cpc::ParseCommandLine(argc, argv);
    if (argv) LocalFree(argv);

    int result = 2;
    if (!options.resumeToken.empty()) {
        result = cpc::RunResumeCommand(options.resumeToken, true);
    } else {
        MessageBoxW(nullptr,
            L"CryptoPro Cleanup Resume Helper\r\n\r\nThis internal helper requires a protected --resume token.",
            L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
