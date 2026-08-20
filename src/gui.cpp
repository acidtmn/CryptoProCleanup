#include "cleanup.hpp"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <memory>
#include <numeric>
#include <sstream>

namespace cpc {
namespace {

constexpr UINT WM_CPC_START = WM_APP + 10;

struct AppState {
    HWND window = nullptr;
    Language language = Language::English;
    ScanResult scan;
    OfflineScanResult offline;
    CleanupPlan plan;
    std::wstring backupRoot;
    std::wstring licensesPath;
    std::wstring reportPath;
    std::wstring logPath;
    std::wstring resumeToken;
    std::map<int, std::pair<int, bool>> listSorts;
    std::map<int, std::vector<int>> listColumnWeights;
    std::map<int, RECT> initialControlRects;
    SIZE initialClient{};
    SIZE minimumWindow{};
    HFONT titleFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    bool busy = false;
};

struct ConfirmState {
    Language language = Language::English;
    std::wstring phrase;
    std::wstring message;
};

struct LicenseDialogState {
    Language language = Language::English;
    std::wstring text;
};

void SetText(HWND dialog, int id, const std::wstring& text) { SetWindowTextW(GetDlgItem(dialog, id), text.c_str()); }

BOOL CALLBACK CaptureControlRect(HWND control, LPARAM parameter) {
    auto* state = reinterpret_cast<AppState*>(parameter);
    const int id = GetDlgCtrlID(control);
    if (!state || id <= 0) return TRUE;
    RECT rectangle{};
    GetWindowRect(control, &rectangle);
    MapWindowPoints(HWND_DESKTOP, state->window, reinterpret_cast<POINT*>(&rectangle), 2);
    state->initialControlRects[id] = rectangle;
    return TRUE;
}

void CaptureInitialLayout(AppState& state) {
    RECT client{};
    RECT window{};
    GetClientRect(state.window, &client);
    GetWindowRect(state.window, &window);
    state.initialClient = {client.right - client.left, client.bottom - client.top};
    state.minimumWindow = {window.right - window.left, window.bottom - window.top};
    state.initialControlRects.clear();
    EnumChildWindows(state.window, CaptureControlRect, reinterpret_cast<LPARAM>(&state));
}

void PlaceControl(AppState& state, HDWP* positions, int id,
                  int moveX, int moveY, int growX, int growY) {
    const auto found = state.initialControlRects.find(id);
    HWND control = GetDlgItem(state.window, id);
    if (found == state.initialControlRects.end() || !control) return;
    const RECT& original = found->second;
    const int width = std::max(1, static_cast<int>(original.right - original.left) + growX);
    const int height = std::max(1, static_cast<int>(original.bottom - original.top) + growY);
    if (*positions) {
        *positions = DeferWindowPos(*positions, control, nullptr, original.left + moveX, original.top + moveY,
                                    width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    } else {
        MoveWindow(control, original.left + moveX, original.top + moveY, width, height, TRUE);
    }
}

void ResizeListColumns(AppState& state, int id) {
    const auto found = state.listColumnWeights.find(id);
    HWND list = GetDlgItem(state.window, id);
    if (found == state.listColumnWeights.end() || !list || found->second.empty()) return;
    RECT client{};
    GetClientRect(list, &client);
    const int available = std::max(100, static_cast<int>(client.right - client.left) - GetSystemMetrics(SM_CXVSCROLL) - 4);
    const int totalWeight = std::accumulate(found->second.begin(), found->second.end(), 0);
    if (totalWeight <= 0) return;
    int used = 0;
    for (size_t index = 0; index < found->second.size(); ++index) {
        const int width = index + 1 == found->second.size() ? std::max(40, available - used) :
            std::max(40, (available * found->second[index]) / totalWeight);
        ListView_SetColumnWidth(list, static_cast<int>(index), width);
        used += width;
    }
}

void LayoutMainDialog(AppState& state, int clientWidth, int clientHeight) {
    if (!state.initialClient.cx || !state.initialClient.cy) return;
    const int dx = std::max(0, clientWidth - static_cast<int>(state.initialClient.cx));
    const int dy = std::max(0, clientHeight - static_cast<int>(state.initialClient.cy));
    const int halfY = dy / 2;
    HDWP positions = BeginDeferWindowPos(40);

    PlaceControl(state, &positions, IDC_TITLE, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_LANGUAGE, dx, 0, 0, 0);
    PlaceControl(state, &positions, IDC_DISCLAIMER, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_TAB, 0, 0, dx, dy);

    PlaceControl(state, &positions, IDC_PRODUCTS_LABEL, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_PRODUCTS, 0, 0, dx, halfY);
    PlaceControl(state, &positions, IDC_PROFILES_LABEL, 0, halfY, dx, 0);
    PlaceControl(state, &positions, IDC_PROFILES, 0, halfY, dx, dy - halfY);
    for (const int id : {IDC_SELECT_ALL_PROFILES, IDC_BACKUP_LABEL, IDC_BACKUP_INFO, IDC_SHOW_LICENSES})
        PlaceControl(state, &positions, id, 0, dy, id == IDC_BACKUP_INFO ? dx : 0, 0);
    PlaceControl(state, &positions, IDC_BACKUP_PATH, 0, dy, dx, 0);
    PlaceControl(state, &positions, IDC_BROWSE, dx, dy, 0, 0);
    PlaceControl(state, &positions, IDC_SCAN, dx, dy, 0, 0);
    PlaceControl(state, &positions, IDC_CLEAN, dx, dy, 0, 0);

    PlaceControl(state, &positions, IDC_CERT_INFO, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_CERTIFICATES, 0, 0, dx, dy);
    PlaceControl(state, &positions, IDC_SELECT_ALL_CERTS, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_EXPORT_CERTS, dx, dy, 0, 0);

    PlaceControl(state, &positions, IDC_OFFLINE_INFO, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PATH_LABEL, 0, 0, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PATH, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_BROWSE, dx, 0, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_SCAN, dx, 0, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PRODUCTS_LABEL, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PRODUCTS, 0, 0, dx, halfY);
    PlaceControl(state, &positions, IDC_OFFLINE_CERTS_LABEL, 0, halfY, dx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_CERTS, 0, halfY, dx, dy - halfY);
    PlaceControl(state, &positions, IDC_OFFLINE_SELECT_ALL_CERTS, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_SHOW_LICENSES, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_SAVE, dx, dy, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_CLEAN, dx, dy, 0, 0);

    for (const int id : {IDC_LINK_GITHUB, IDC_LINK_WEBSITE, IDC_LINK_SUPPORT})
        PlaceControl(state, &positions, id, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_PROGRESS, 0, dy, dx, 0);
    PlaceControl(state, &positions, IDC_STATUS, 0, dy, dx, 0);
    PlaceControl(state, &positions, IDC_LOG, 0, dy, dx, 0);
    if (positions) EndDeferWindowPos(positions);
    for (const int id : {IDC_PRODUCTS, IDC_PROFILES, IDC_CERTIFICATES,
                         IDC_OFFLINE_PRODUCTS, IDC_OFFLINE_CERTS}) ResizeListColumns(state, id);
}

void ApplyModernTheme(AppState& state) {
    state.backgroundBrush = CreateSolidBrush(RGB(246, 248, 251));
    HDC device = GetDC(state.window);
    const int dpi = device ? GetDeviceCaps(device, LOGPIXELSY) : 96;
    if (device) ReleaseDC(state.window, device);
    LOGFONTW font{};
    font.lfHeight = -MulDiv(15, dpi, 72);
    font.lfWeight = FW_SEMIBOLD;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    state.titleFont = CreateFontIndirectW(&font);
    if (state.titleFont) SendDlgItemMessageW(state.window, IDC_TITLE, WM_SETFONT,
                                             reinterpret_cast<WPARAM>(state.titleFont), TRUE);
    for (const int id : {IDC_PRODUCTS, IDC_PROFILES, IDC_CERTIFICATES,
                         IDC_OFFLINE_PRODUCTS, IDC_OFFLINE_CERTS}) {
        SetWindowTheme(GetDlgItem(state.window, id), L"Explorer", nullptr);
    }
    SetWindowTheme(GetDlgItem(state.window, IDC_TAB), L"Explorer", nullptr);
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETSTATE, PBST_NORMAL, 0);
}

void OpenProjectLink(const AppState& state, int controlId) {
    const wchar_t* url = L"https://yoomoney.ru/to/4100119195083142";
    if (controlId == IDC_LINK_GITHUB) url = L"https://github.com/acidtmn/CryptoProCleanup";
    else if (controlId == IDC_LINK_WEBSITE) url = L"https://kodalexandrova.ru";
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(state.window, L"open", url, nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        MessageBoxW(state.window,
                    Tr(state.language, L"Не удалось открыть ссылку в браузере.",
                       L"The link could not be opened in your browser.").c_str(),
                    Tr(state.language, L"Ошибка открытия ссылки", L"Link error").c_str(),
                    MB_OK | MB_ICONWARNING);
    }
}

std::wstring GetText(HWND dialog, int id) {
    const int length = GetWindowTextLengthW(GetDlgItem(dialog, id));
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(GetDlgItem(dialog, id), value.data(), static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    return value;
}

void PumpUi() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void UpdateProgress(AppState& state, const std::wstring& message, int percent) {
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETPOS, static_cast<WPARAM>(std::clamp(percent, 0, 100)), 0);
    SetText(state.window, IDC_STATUS, message);
    UpdateWindow(state.window);
    PumpUi();
}

void AddLogLine(AppState& state, const std::wstring& line) {
    HWND log = GetDlgItem(state.window, IDC_LOG);
    const int length = GetWindowTextLengthW(log);
    SendMessageW(log, EM_SETSEL, length, length);
    const std::wstring text = (length ? L"\r\n" : L"") + line;
    SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    if (!state.logPath.empty()) AppendLog(state.logPath, line);
}

std::wstring DefaultBackupFolder() {
    PWSTR raw = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &raw)) && raw) folder = raw;
    CoTaskMemFree(raw);
    if (folder.empty()) {
        std::vector<wchar_t> current(32768, L'\0');
        GetCurrentDirectoryW(static_cast<DWORD>(current.size()), current.data());
        folder = current.data();
    }
    return folder;
}

void ConfigureList(AppState& state, HWND list, const std::vector<std::pair<std::wstring, int>>& columns) {
    ListView_DeleteAllItems(list);
    while (ListView_DeleteColumn(list, 0)) {}
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES |
                                           LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    for (size_t index = 0; index < columns.size(); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[index].first.c_str());
        column.cx = columns[index].second;
        column.iSubItem = static_cast<int>(index);
        ListView_InsertColumn(list, static_cast<int>(index), &column);
    }
    std::vector<int> weights;
    weights.reserve(columns.size());
    for (const auto& column : columns) weights.push_back(column.second);
    state.listColumnWeights[GetDlgCtrlID(list)] = std::move(weights);
    ResizeListColumns(state, GetDlgCtrlID(list));
}

void PopulateCertificates(AppState& state) {
    HWND list = GetDlgItem(state.window, IDC_CERTIFICATES);
    ConfigureList(state, list, {
        {Tr(state.language, L"Профиль", L"Profile"), 105},
        {Tr(state.language, L"Кому выдан", L"Issued to"), 170},
        {Tr(state.language, L"Кем выдан", L"Issued by"), 170},
        {Tr(state.language, L"Действует с", L"Valid from"), 88},
        {Tr(state.language, L"Действует по", L"Valid to"), 88},
        {Tr(state.language, L"Закрытый ключ", L"Private key"), 90}
    });
    for (size_t index = 0; index < state.scan.certificates.size(); ++index) {
        const auto& certificate = state.scan.certificates[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(certificate.profileName.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, item.iItem, 1, const_cast<wchar_t*>(certificate.subject.c_str()));
        ListView_SetItemText(list, item.iItem, 2, const_cast<wchar_t*>(certificate.issuer.c_str()));
        ListView_SetItemText(list, item.iItem, 3, const_cast<wchar_t*>(certificate.validFrom.c_str()));
        ListView_SetItemText(list, item.iItem, 4, const_cast<wchar_t*>(certificate.validTo.c_str()));
        const std::wstring key = certificate.hasPrivateKeyReference ? Tr(state.language, L"Есть ссылка", L"Reference") : Tr(state.language, L"Нет", L"No");
        ListView_SetItemText(list, item.iItem, 5, const_cast<wchar_t*>(key.c_str()));
        ListView_SetCheckState(list, item.iItem, certificate.selected ? TRUE : FALSE);
    }
    Button_SetCheck(GetDlgItem(state.window, IDC_SELECT_ALL_CERTS),
                    state.scan.certificates.empty() ? BST_UNCHECKED : BST_CHECKED);
}

void PopulateOfflineLists(AppState& state) {
    HWND products = GetDlgItem(state.window, IDC_OFFLINE_PRODUCTS);
    ConfigureList(state, products, {
        {Tr(state.language, L"Продукт", L"Product"), 315},
        {Tr(state.language, L"Версия", L"Version"), 115},
        {Tr(state.language, L"Архитектура", L"Architecture"), 110},
        {Tr(state.language, L"Риск", L"Risk"), 130}
    });
    for (size_t index = 0; index < state.offline.scan.products.size(); ++index) {
        const auto& product = state.offline.scan.products[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(product.displayName.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(products, &item);
        ListView_SetItemText(products, item.iItem, 1, const_cast<wchar_t*>(product.version.c_str()));
        ListView_SetItemText(products, item.iItem, 2, const_cast<wchar_t*>(product.architecture.c_str()));
        const std::wstring risk = product.risk == RiskLevel::High ? Tr(state.language, L"ВЫСОКИЙ", L"HIGH") : Tr(state.language, L"Обычный", L"Normal");
        ListView_SetItemText(products, item.iItem, 3, const_cast<wchar_t*>(risk.c_str()));
        ListView_SetCheckState(products, item.iItem, product.selected ? TRUE : FALSE);
    }

    HWND certificates = GetDlgItem(state.window, IDC_OFFLINE_CERTS);
    ConfigureList(state, certificates, {
        {Tr(state.language, L"Профиль", L"Profile"), 105},
        {Tr(state.language, L"Кому выдан", L"Issued to"), 185},
        {Tr(state.language, L"Кем выдан", L"Issued by"), 185},
        {Tr(state.language, L"С", L"From"), 88},
        {Tr(state.language, L"По", L"To"), 88}
    });
    for (size_t index = 0; index < state.offline.scan.certificates.size(); ++index) {
        const auto& certificate = state.offline.scan.certificates[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(certificate.profileName.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(certificates, &item);
        ListView_SetItemText(certificates, item.iItem, 1, const_cast<wchar_t*>(certificate.subject.c_str()));
        ListView_SetItemText(certificates, item.iItem, 2, const_cast<wchar_t*>(certificate.issuer.c_str()));
        ListView_SetItemText(certificates, item.iItem, 3, const_cast<wchar_t*>(certificate.validFrom.c_str()));
        ListView_SetItemText(certificates, item.iItem, 4, const_cast<wchar_t*>(certificate.validTo.c_str()));
        ListView_SetCheckState(certificates, item.iItem, certificate.selected ? TRUE : FALSE);
    }
    Button_SetCheck(GetDlgItem(state.window, IDC_OFFLINE_SELECT_ALL_CERTS),
                    state.offline.scan.certificates.empty() ? BST_UNCHECKED : BST_CHECKED);
}

void PopulateLists(AppState& state) {
    HWND products = GetDlgItem(state.window, IDC_PRODUCTS);
    ConfigureList(state, products, {
        {Tr(state.language, L"Продукт", L"Product"), 255},
        {Tr(state.language, L"Версия", L"Version"), 90},
        {Tr(state.language, L"Архитектура", L"Architecture"), 90},
        {Tr(state.language, L"Риск", L"Risk"), 105}
    });
    for (size_t index = 0; index < state.scan.products.size(); ++index) {
        const auto& product = state.scan.products[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(product.displayName.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(products, &item);
        ListView_SetItemText(products, item.iItem, 1, const_cast<wchar_t*>(product.version.c_str()));
        ListView_SetItemText(products, item.iItem, 2, const_cast<wchar_t*>(product.architecture.c_str()));
        const std::wstring risk = product.risk == RiskLevel::High ? Tr(state.language, L"ВЫСОКИЙ", L"HIGH") : Tr(state.language, L"Обычный", L"Normal");
        ListView_SetItemText(products, item.iItem, 3, const_cast<wchar_t*>(risk.c_str()));
        ListView_SetCheckState(products, item.iItem, product.selected ? TRUE : FALSE);
    }

    HWND profiles = GetDlgItem(state.window, IDC_PROFILES);
    ConfigureList(state, profiles, {{Tr(state.language, L"Локальный профиль", L"Local profile"), 350},
                             {Tr(state.language, L"Состояние", L"State"), 190}});
    for (size_t index = 0; index < state.scan.profiles.size(); ++index) {
        const auto& profile = state.scan.profiles[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(profile.displayName.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(profiles, &item);
        std::wstring status = profile.loaded ? Tr(state.language, L"Загружен", L"Loaded") : Tr(state.language, L"Неактивен", L"Offline");
        ListView_SetItemText(profiles, item.iItem, 1, status.data());
        ListView_SetCheckState(profiles, item.iItem, profile.selected ? TRUE : FALSE);
    }
    Button_SetCheck(GetDlgItem(state.window, IDC_SELECT_ALL_PROFILES),
                    state.scan.profiles.empty() ? BST_UNCHECKED : BST_CHECKED);
    PopulateCertificates(state);
}

std::wstring DateSortKey(const std::wstring& value) {
    if (value.size() == 10 && value[2] == L'.' && value[5] == L'.')
        return value.substr(6, 4) + value.substr(3, 2) + value.substr(0, 2);
    return value;
}

std::wstring ListSortValue(const AppState& state, int control, size_t item, int column) {
    if (control == IDC_PRODUCTS && item < state.scan.products.size()) {
        const auto& value = state.scan.products[item];
        if (column == 0) return value.displayName;
        if (column == 1) return value.version;
        if (column == 2) return value.architecture;
        return value.risk == RiskLevel::High ? L"1" : L"0";
    }
    if (control == IDC_CERTIFICATES && item < state.scan.certificates.size()) {
        const auto& value = state.scan.certificates[item];
        if (column == 0) return value.profileName;
        if (column == 1) return value.subject;
        if (column == 2) return value.issuer;
        if (column == 3) return DateSortKey(value.validFrom);
        if (column == 4) return DateSortKey(value.validTo);
        return value.hasPrivateKeyReference ? L"1" : L"0";
    }
    if (control == IDC_OFFLINE_PRODUCTS && item < state.offline.scan.products.size()) {
        const auto& value = state.offline.scan.products[item];
        if (column == 0) return value.displayName;
        if (column == 1) return value.version;
        if (column == 2) return value.architecture;
        return value.risk == RiskLevel::High ? L"1" : L"0";
    }
    if (control == IDC_OFFLINE_CERTS && item < state.offline.scan.certificates.size()) {
        const auto& value = state.offline.scan.certificates[item];
        if (column == 0) return value.profileName;
        if (column == 1) return value.subject;
        if (column == 2) return value.issuer;
        if (column == 3) return DateSortKey(value.validFrom);
        return DateSortKey(value.validTo);
    }
    return {};
}

struct ListSortContext {
    const AppState* state = nullptr;
    int control = 0;
    int column = 0;
    bool ascending = true;
};

int CALLBACK CompareListItems(LPARAM left, LPARAM right, LPARAM contextValue) {
    const auto* context = reinterpret_cast<const ListSortContext*>(contextValue);
    if (!context || !context->state || left < 0 || right < 0) return 0;
    const std::wstring leftValue = ListSortValue(*context->state, context->control, static_cast<size_t>(left), context->column);
    const std::wstring rightValue = ListSortValue(*context->state, context->control, static_cast<size_t>(right), context->column);
    const int compared = _wcsicmp(leftValue.c_str(), rightValue.c_str());
    return context->ascending ? compared : -compared;
}

void SortList(AppState& state, int control, int column) {
    auto& setting = state.listSorts[control];
    if (setting.first == column) setting.second = !setting.second;
    else setting = {column, true};
    ListSortContext context{&state, control, column, setting.second};
    ListView_SortItems(GetDlgItem(state.window, control), CompareListItems, &context);
}

void UpdateTabVisibility(AppState& state) {
    const int selected = TabCtrl_GetCurSel(GetDlgItem(state.window, IDC_TAB));
    const std::array<int, 11> cleanupControls{
        IDC_PRODUCTS_LABEL, IDC_PRODUCTS, IDC_PROFILES_LABEL, IDC_PROFILES, IDC_SELECT_ALL_PROFILES,
        IDC_BACKUP_LABEL, IDC_BACKUP_PATH, IDC_BROWSE, IDC_BACKUP_INFO, IDC_SHOW_LICENSES, IDC_SCAN
    };
    for (const int id : cleanupControls) ShowWindow(GetDlgItem(state.window, id), selected == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state.window, IDC_CLEAN), selected == 0 ? SW_SHOW : SW_HIDE);
    const std::array<int, 4> certificateControls{IDC_CERT_INFO, IDC_CERTIFICATES, IDC_SELECT_ALL_CERTS, IDC_EXPORT_CERTS};
    for (const int id : certificateControls) ShowWindow(GetDlgItem(state.window, id), selected == 1 ? SW_SHOW : SW_HIDE);
    const std::array<int, 13> offlineControls{
        IDC_OFFLINE_INFO, IDC_OFFLINE_PATH_LABEL, IDC_OFFLINE_PATH, IDC_OFFLINE_BROWSE, IDC_OFFLINE_SCAN,
        IDC_OFFLINE_PRODUCTS_LABEL, IDC_OFFLINE_PRODUCTS, IDC_OFFLINE_CERTS_LABEL, IDC_OFFLINE_CERTS,
        IDC_OFFLINE_SELECT_ALL_CERTS, IDC_OFFLINE_SHOW_LICENSES, IDC_OFFLINE_SAVE, IDC_OFFLINE_CLEAN
    };
    for (const int id : offlineControls) ShowWindow(GetDlgItem(state.window, id), selected == 2 ? SW_SHOW : SW_HIDE);
}

void ApplyLanguage(AppState& state) {
    const bool ru = state.language == Language::Russian;
    const std::wstring applicationName = ru ? L"КриптоПро Очистка" : L"CryptoPro Cleanup Utility";
    const std::wstring versionedName = applicationName + L" " + kVersion;
    SetWindowTextW(state.window, versionedName.c_str());
    SetText(state.window, IDC_TITLE, versionedName);
    SetText(state.window, IDC_DISCLAIMER, Tr(state.language,
        L"Неофициальная утилита с открытым исходным кодом. Проект не связан с ООО «КРИПТО-ПРО».",
        L"Unofficial open-source utility. This project is not affiliated with Crypto-Pro LLC."));
    SetText(state.window, IDC_LINK_GITHUB,
            L"<a href=\"https://github.com/acidtmn/CryptoProCleanup\">GitHub</a>");
    SetText(state.window, IDC_LINK_WEBSITE, Tr(state.language,
        L"<a href=\"https://kodalexandrova.ru\">Сайт «Код Александрова»</a>",
        L"<a href=\"https://kodalexandrova.ru\">Code Alexandrov website</a>"));
    SetText(state.window, IDC_LINK_SUPPORT, Tr(state.language,
        L"<a href=\"https://yoomoney.ru/to/4100119195083142\">Поддержать проект</a>",
        L"<a href=\"https://yoomoney.ru/to/4100119195083142\">Support the project</a>"));
    SetText(state.window, IDC_PRODUCTS_LABEL, Tr(state.language, L"Обнаруженные продукты", L"Detected products"));
    SetText(state.window, IDC_PROFILES_LABEL, Tr(state.language, L"Локальные профили для очистки настроек", L"Local profiles whose settings may be cleaned"));
    SetText(state.window, IDC_SELECT_ALL_PROFILES, Tr(state.language, L"Выбрать все профили", L"Select all profiles"));
    SetText(state.window, IDC_BACKUP_LABEL, Tr(state.language, L"Папка резервной копии", L"Backup folder"));
    SetText(state.window, IDC_BROWSE, Tr(state.language, L"Обзор...", L"Browse..."));
    SetText(state.window, IDC_BACKUP_INFO, Tr(state.language,
        L"Будут созданы: licenses.txt (полные лицензии), папка с выбранными .cer/.p7b, summary.txt, report.json и cleanup.log. Закрытые ключи не экспортируются. Сохраните копию вне системного диска.",
        L"Created: licenses.txt (full licenses), selected public .cer/.p7b files, summary.txt, report.json, and cleanup.log. Private keys are not exported. Keep the copy outside the system drive."));
    SetText(state.window, IDC_SCAN, Tr(state.language, L"Сканировать", L"Scan"));
    SetText(state.window, IDC_CLEAN, Tr(state.language, L"Удалить выбранное", L"Remove selected"));
    SetText(state.window, IDC_SHOW_LICENSES, Tr(state.language, L"Показать / копировать лицензию", L"Show / copy license"));
    SetText(state.window, IDC_CERT_INFO, Tr(state.language,
        L"Личное хранилище каждого обычного профиля. Экспортируется только открытая часть; закрытые ключи и токены не копируются.",
        L"Personal store of each regular profile. Only public data is exported; private keys and tokens are not copied."));
    SetText(state.window, IDC_SELECT_ALL_CERTS, Tr(state.language, L"Выбрать все сертификаты", L"Select all certificates"));
    SetText(state.window, IDC_EXPORT_CERTS, Tr(state.language, L"Сохранить выбранные открытые сертификаты...", L"Export selected public certificates..."));
    SetText(state.window, IDC_OFFLINE_INFO, Tr(state.language,
        L"Режим спасения отключённой Windows: безопасное извлечение данных или принудительная очистка после полной резервной копии. Штатный MSI здесь запустить невозможно.",
        L"Disconnected Windows rescue: safely extract data or run forced cleanup after a full recovery backup. Its registered MSI cannot run here."));
    SetText(state.window, IDC_OFFLINE_PATH_LABEL, Tr(state.language, L"Папка Windows", L"Windows folder"));
    SetText(state.window, IDC_OFFLINE_BROWSE, Tr(state.language, L"Обзор...", L"Browse..."));
    SetText(state.window, IDC_OFFLINE_SCAN, Tr(state.language, L"Сканировать", L"Scan"));
    SetText(state.window, IDC_OFFLINE_PRODUCTS_LABEL, Tr(state.language, L"Продукты в отключённой Windows", L"Products in disconnected Windows"));
    SetText(state.window, IDC_OFFLINE_CERTS_LABEL, Tr(state.language, L"Открытые сертификаты профилей и компьютера", L"Public certificates in profiles and local machine"));
    SetText(state.window, IDC_OFFLINE_SELECT_ALL_CERTS, Tr(state.language, L"Выбрать все сертификаты", L"Select all certificates"));
    SetText(state.window, IDC_OFFLINE_SHOW_LICENSES, Tr(state.language, L"Показать / копировать лицензии", L"Show / copy licenses"));
    SetText(state.window, IDC_OFFLINE_SAVE, Tr(state.language, L"Сохранить найденные данные...", L"Save rescued data..."));
    SetText(state.window, IDC_OFFLINE_CLEAN, Tr(state.language, L"Расширенная офлайн-очистка...", L"Advanced offline cleanup..."));
    HWND tabs = GetDlgItem(state.window, IDC_TAB);
    TCITEMW tab{};
    tab.mask = TCIF_TEXT;
    std::wstring caption = Tr(state.language, L"Удаление", L"Cleanup");
    tab.pszText = caption.data();
    TabCtrl_SetItem(tabs, 0, &tab);
    caption = Tr(state.language, L"Сертификаты", L"Certificates");
    tab.pszText = caption.data();
    TabCtrl_SetItem(tabs, 1, &tab);
    caption = Tr(state.language, L"Неисправный диск", L"Disconnected Windows");
    tab.pszText = caption.data();
    TabCtrl_SetItem(tabs, 2, &tab);
    if (!state.scan.products.empty() || !state.scan.profiles.empty() || !state.scan.certificates.empty()) PopulateLists(state);
    if (state.offline.valid) PopulateOfflineLists(state);
    UpdateTabVisibility(state);
}

void SetBusy(AppState& state, bool busy) {
    state.busy = busy;
    for (const int id : {IDC_TAB, IDC_PRODUCTS, IDC_PROFILES, IDC_SELECT_ALL_PROFILES, IDC_BACKUP_PATH,
                         IDC_BROWSE, IDC_LANGUAGE, IDC_SCAN, IDC_CLEAN, IDC_SHOW_LICENSES,
                         IDC_CERTIFICATES, IDC_SELECT_ALL_CERTS, IDC_EXPORT_CERTS,
                         IDC_OFFLINE_PATH, IDC_OFFLINE_BROWSE, IDC_OFFLINE_SCAN, IDC_OFFLINE_PRODUCTS,
                         IDC_OFFLINE_CERTS, IDC_OFFLINE_SELECT_ALL_CERTS, IDC_OFFLINE_SHOW_LICENSES,
                         IDC_OFFLINE_SAVE, IDC_OFFLINE_CLEAN}) EnableWindow(GetDlgItem(state.window, id), !busy);
    if (!busy) {
        EnableWindow(GetDlgItem(state.window, IDC_SHOW_LICENSES), !state.scan.licenses.empty());
        EnableWindow(GetDlgItem(state.window, IDC_EXPORT_CERTS), !state.scan.certificates.empty());
        EnableWindow(GetDlgItem(state.window, IDC_CLEAN), !state.scan.products.empty());
        EnableWindow(GetDlgItem(state.window, IDC_OFFLINE_SHOW_LICENSES), state.offline.valid && !state.offline.scan.licenses.empty());
        EnableWindow(GetDlgItem(state.window, IDC_OFFLINE_SAVE), state.offline.valid);
        EnableWindow(GetDlgItem(state.window, IDC_OFFLINE_CLEAN), state.offline.cleanupCapable && !state.offline.scan.products.empty());
    }
}

bool CopyUnicodeText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool success = false;
    if (memory) {
        void* destination = GlobalLock(memory);
        if (destination) {
            memcpy(destination, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory)) success = true;
        }
        if (!success) GlobalFree(memory);
    }
    CloseClipboard();
    return success;
}

INT_PTR CALLBACK LicensesDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<LicenseDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<LicenseDialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, Tr(state->language, L"Лицензии CryptoPro", L"CryptoPro licenses").c_str());
        SetText(dialog, IDC_LICENSES_WARNING, Tr(state->language,
            L"Полные номера являются конфиденциальными. Перед переустановкой Windows сохраните licenses.txt на внешнем диске или в защищённом облаке.",
            L"Full identifiers are confidential. Before reinstalling Windows, save licenses.txt to external storage or protected cloud storage."));
        SetText(dialog, IDC_LICENSES_TEXT, state->text);
        SetText(dialog, IDC_COPY_LICENSES, Tr(state->language, L"Копировать всё", L"Copy all"));
        SetText(dialog, IDOK, Tr(state->language, L"Закрыть", L"Close"));
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDC_COPY_LICENSES && state) {
        const bool copied = CopyUnicodeText(dialog, state->text);
        MessageBoxW(dialog,
            Tr(state->language, copied ? L"Лицензия скопирована в буфер обмена." : L"Не удалось открыть буфер обмена.",
                               copied ? L"License copied to the clipboard." : L"Could not open the clipboard.").c_str(),
            Tr(state->language, L"Копирование", L"Copy").c_str(), MB_OK | (copied ? MB_ICONINFORMATION : MB_ICONERROR));
        return TRUE;
    }
    if ((message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) || message == WM_CLOSE) {
        EndDialog(dialog, 0);
        return TRUE;
    }
    return FALSE;
}

void ShowLicensesForScan(AppState& state, const ScanResult& scan) {
    if (scan.licenses.empty()) {
        MessageBoxW(state.window,
            Tr(state.language, L"Известные значения лицензии в реестре не найдены.", L"No known license values were found in the registry.").c_str(),
            Tr(state.language, L"Лицензия", L"License").c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wostringstream text;
    for (size_t index = 0; index < scan.licenses.size(); ++index) {
        const auto& license = scan.licenses[index];
        text << L"[" << (index + 1) << L"] " << license.product << L"\r\n"
             << Tr(state.language, L"Источник: ", L"Source: ") << license.registryPath << L" / " << license.valueName << L"\r\n"
             << Tr(state.language, L"Лицензия: ", L"License: ") << license.fullValue << L"\r\n\r\n";
    }
    LicenseDialogState dialogState{state.language, text.str()};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LICENSES), state.window,
                    LicensesDialogProc, reinterpret_cast<LPARAM>(&dialogState));
}

void ShowLicenses(AppState& state) { ShowLicensesForScan(state, state.scan); }

INT_PTR CALLBACK ConfirmDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ConfirmState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<ConfirmState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, Tr(state->language, L"Дополнительное подтверждение", L"Additional confirmation").c_str());
        const std::wstring prompt = state->message + L"\r\n" + Tr(state->language, L"Введите: ", L"Type: ") + state->phrase;
        SetText(dialog, IDC_CONFIRM_TEXT, prompt);
        SetText(dialog, IDOK, L"OK");
        SetText(dialog, IDCANCEL, Tr(state->language, L"Отмена", L"Cancel"));
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDOK && state) {
        if (GetText(dialog, IDC_CONFIRM_EDIT) == state->phrase) EndDialog(dialog, IDOK);
        else MessageBoxW(dialog, Tr(state->language, L"Фраза введена неверно.", L"The confirmation phrase does not match.").c_str(),
                         Tr(state->language, L"Подтверждение", L"Confirmation").c_str(), MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) || message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}

bool TypedConfirm(AppState& state, const std::wstring& phrase, const std::wstring& message) {
    ConfirmState confirm{state.language, phrase, message};
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TYPED_CONFIRM), state.window,
                           ConfirmDialogProc, reinterpret_cast<LPARAM>(&confirm)) == IDOK;
}

std::wstring BrowseForFolder(HWND owner, const std::wstring& current) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog))) || !dialog) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (!current.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) result = raw;
            CoTaskMemFree(raw);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

void ReadSelections(AppState& state) {
    HWND products = GetDlgItem(state.window, IDC_PRODUCTS);
    for (int row = 0; row < ListView_GetItemCount(products); ++row) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(products, &item) && item.lParam >= 0 && static_cast<size_t>(item.lParam) < state.scan.products.size())
            state.scan.products[static_cast<size_t>(item.lParam)].selected = ListView_GetCheckState(products, row) != FALSE;
    }
    HWND profiles = GetDlgItem(state.window, IDC_PROFILES);
    for (int row = 0; row < ListView_GetItemCount(profiles); ++row) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(profiles, &item) && item.lParam >= 0 && static_cast<size_t>(item.lParam) < state.scan.profiles.size())
            state.scan.profiles[static_cast<size_t>(item.lParam)].selected = ListView_GetCheckState(profiles, row) != FALSE;
    }
    HWND certificates = GetDlgItem(state.window, IDC_CERTIFICATES);
    for (int row = 0; row < ListView_GetItemCount(certificates); ++row) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(certificates, &item) && item.lParam >= 0 && static_cast<size_t>(item.lParam) < state.scan.certificates.size())
            state.scan.certificates[static_cast<size_t>(item.lParam)].selected = ListView_GetCheckState(certificates, row) != FALSE;
    }
}

void ExportCertificates(AppState& state) {
    ReadSelections(state);
    const size_t selected = static_cast<size_t>(std::count_if(
        state.scan.certificates.begin(), state.scan.certificates.end(), [](const CertificateEntry& item) { return item.selected; }));
    if (!selected) {
        MessageBoxW(state.window, Tr(state.language, L"Не выбран ни один сертификат.", L"No certificate is selected.").c_str(),
                    L"CryptoPro Cleanup Utility", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring parent = BrowseForFolder(state.window, GetText(state.window, IDC_BACKUP_PATH));
    if (parent.empty()) return;
    SetBusy(state, true);
    std::wstring folder, error;
    const bool saved = ExportPublicCertificates(state.language, state.scan.certificates, parent, &folder, &error);
    SetBusy(state, false);
    if (!saved) {
        MessageBoxW(state.window, error.c_str(), Tr(state.language, L"Экспорт не выполнен", L"Export failed").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring message = Tr(state.language,
        L"Открытые сертификаты сохранены. Закрытые ключи не экспортировались.\r\n\r\n",
        L"Public certificates were saved. Private keys were not exported.\r\n\r\n") + folder;
    MessageBoxW(state.window, message.c_str(), Tr(state.language, L"Экспорт завершён", L"Export completed").c_str(), MB_OK | MB_ICONINFORMATION);
}

void ReadOfflineSelections(AppState& state) {
    HWND products = GetDlgItem(state.window, IDC_OFFLINE_PRODUCTS);
    for (int row = 0; row < ListView_GetItemCount(products); ++row) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(products, &item) && item.lParam >= 0 && static_cast<size_t>(item.lParam) < state.offline.scan.products.size())
            state.offline.scan.products[static_cast<size_t>(item.lParam)].selected = ListView_GetCheckState(products, row) != FALSE;
    }
    HWND certificates = GetDlgItem(state.window, IDC_OFFLINE_CERTS);
    for (int row = 0; row < ListView_GetItemCount(certificates); ++row) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(certificates, &item) && item.lParam >= 0 && static_cast<size_t>(item.lParam) < state.offline.scan.certificates.size())
            state.offline.scan.certificates[static_cast<size_t>(item.lParam)].selected = ListView_GetCheckState(certificates, row) != FALSE;
    }
}

void DoOfflineScan(AppState& state) {
    const std::wstring path = GetText(state.window, IDC_OFFLINE_PATH);
    if (path.empty()) {
        MessageBoxW(state.window, Tr(state.language, L"Выберите папку Windows на подключённом диске.", L"Select the Windows folder on the connected drive.").c_str(),
                    L"CryptoPro Cleanup Utility", MB_OK | MB_ICONWARNING);
        return;
    }
    SetBusy(state, true);
    state.logPath.clear();
    state.offline = ScanOfflineWindows(state.language, path,
        [&](const std::wstring& message, int percent) { UpdateProgress(state, message, percent); });
    PopulateOfflineLists(state);
    std::wostringstream summary;
    if (state.offline.valid) {
        SetText(state.window, IDC_OFFLINE_PATH, state.offline.windowsDirectory);
        summary << Tr(state.language, L"Офлайн: продуктов ", L"Offline: products ") << state.offline.scan.products.size()
                << Tr(state.language, L", лицензий ", L", licenses ") << state.offline.scan.licenses.size()
                << Tr(state.language, L", профилей ", L", profiles ") << state.offline.scan.profiles.size()
                << Tr(state.language, L", открытых сертификатов ", L", public certificates ") << state.offline.scan.certificates.size()
                << Tr(state.language, L", подтверждённых целей ", L", verified targets ") << state.offline.targets.size()
                << (state.offline.cleanupCapable ? Tr(state.language, L", очистка доступна", L", cleanup available")
                                                 : Tr(state.language, L", только спасение данных", L", rescue only"));
        UpdateProgress(state, Tr(state.language, L"Офлайн-сканирование завершено. Изменения не выполнялись.", L"Offline scan completed. No changes were made."), 100);
    } else {
        summary << Tr(state.language, L"Офлайн-система не распознана.", L"Offline system was not recognized.");
        UpdateProgress(state, summary.str(), 100);
    }
    AddLogLine(state, summary.str());
    for (const auto& warning : state.offline.scan.warnings) AddLogLine(state, warning);
    SetBusy(state, false);
}

void SaveOfflineData(AppState& state) {
    if (!state.offline.valid) {
        MessageBoxW(state.window, Tr(state.language, L"Сначала просканируйте отключённую Windows.", L"Scan the disconnected Windows installation first.").c_str(),
                    L"CryptoPro Cleanup Utility", MB_OK | MB_ICONWARNING);
        return;
    }
    ReadOfflineSelections(state);
    const std::wstring parent = BrowseForFolder(state.window, GetText(state.window, IDC_BACKUP_PATH));
    if (parent.empty()) return;
    SetBusy(state, true);
    std::wstring folder, error;
    const bool saved = SaveOfflineBackup(state.language, state.offline, parent, false, &folder, &error);
    SetBusy(state, false);
    if (!saved) {
        MessageBoxW(state.window, error.c_str(), Tr(state.language, L"Спасение данных не выполнено", L"Data rescue failed").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring message = Tr(state.language,
        L"Лицензии и выбранные открытые сертификаты сохранены. Офлайн-система не изменялась. Закрытые ключи не экспортировались.\r\n\r\n",
        L"Licenses and selected public certificates were saved. The offline system was not changed. Private keys were not exported.\r\n\r\n") + folder;
    MessageBoxW(state.window, message.c_str(), Tr(state.language, L"Данные сохранены", L"Data saved").c_str(), MB_OK | MB_ICONINFORMATION);
}

void StartOfflineCleanup(AppState& state) {
    if (!state.offline.cleanupCapable || state.offline.scan.products.empty()) {
        MessageBoxW(state.window, Tr(state.language, L"Не найдены подтверждённые продукты для офлайн-очистки.", L"No confirmed products were found for offline cleanup.").c_str(),
                    L"CryptoPro Cleanup Utility", MB_OK | MB_ICONWARNING);
        return;
    }
    ReadOfflineSelections(state);
    if (!std::all_of(state.offline.scan.products.begin(), state.offline.scan.products.end(),
                     [](const InstalledProduct& product) { return product.selected; })) {
        MessageBoxW(state.window,
            Tr(state.language,
               L"Офлайн-компоненты могут быть общими для нескольких продуктов. Для безопасного плана выберите все найденные продукты.",
               L"Offline components can be shared by several products. Select every detected product to use the safe plan.").c_str(),
            L"CryptoPro Cleanup Utility", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!TypedConfirm(state, L"OFFLINE", Tr(state.language,
        L"Это не штатная деинсталляция: установщик отключённой Windows запустить невозможно. Будут изменены только подтверждённые цели после копирования SOFTWARE, SYSTEM и файлов на другой том. Введите OFFLINE для продолжения.",
        L"This is not a registered uninstall: the disconnected Windows installer cannot run. Only verified targets will change after SOFTWARE, SYSTEM, and file recovery copies are stored on another volume. Type OFFLINE to continue."))) return;

    const std::wstring parent = BrowseForFolder(state.window, GetText(state.window, IDC_BACKUP_PATH));
    if (parent.empty()) return;
    SetBusy(state, true);
    std::wstring backupSession, error;
    if (!SaveOfflineBackup(state.language, state.offline, parent, true, &backupSession, &error)) {
        SetBusy(state, false);
        MessageBoxW(state.window, error.c_str(), Tr(state.language, L"Резервная копия не создана — офлайн-очистка отменена", L"Backup failed — offline cleanup cancelled").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring offlinePath = state.offline.windowsDirectory;
    ExecutionResult execution = ExecuteOfflineCleanup(state.offline, backupSession,
        [&](const std::wstring& message, int percent) { UpdateProgress(state, message, percent); });
    const std::wstring logPath = backupSession + L"\\offline-cleanup.log";
    WriteUtf8File(logPath, Utf8(L"CryptoPro Cleanup Utility offline cleanup\r\nNo license or certificate values are written to this log.\r\n"), nullptr);
    std::wostringstream resultFile;
    for (const auto& operation : execution.operations) {
        const std::wstring status = operation.outcome == Outcome::Succeeded ? L"OK" :
                                    operation.outcome == Outcome::Failed ? L"ERROR" :
                                    operation.outcome == Outcome::RebootRequired ? L"REBOOT" : L"SKIP";
        const std::wstring line = L"[" + status + L"] " + operation.action + L": " + operation.target + L" — " + operation.message;
        AppendLog(logPath, line);
        resultFile << line << L"\r\n";
    }
    state.offline = ScanOfflineWindows(state.language, offlinePath, {});
    PopulateOfflineLists(state);
    const bool partial = execution.anyFailure || !state.offline.scan.products.empty();
    resultFile << L"\r\nRemaining registered products: " << state.offline.scan.products.size() << L"\r\n"
               << L"Conservative mode: unknown COM/browser remnants may remain.\r\n";
    WriteUtf8File(backupSession + L"\\offline-result.txt", Utf8(resultFile.str()), nullptr);
    const std::wstring result = partial ?
        Tr(state.language, L"Офлайн-очистка завершена частично. Проверьте отчёт и оставшиеся продукты.", L"Offline cleanup completed partially. Review the report and remaining products.") :
        Tr(state.language, L"Подтверждённые офлайн-цели удалены. Неизвестные остатки намеренно могли быть сохранены.", L"Verified offline targets were removed. Unknown remnants may intentionally remain.");
    UpdateProgress(state, result, 100);
    SetBusy(state, false);
    MessageBoxW(state.window, (result + L"\r\n\r\n" + backupSession).c_str(),
                Tr(state.language, L"Результат офлайн-очистки", L"Offline cleanup result").c_str(),
                MB_OK | (partial ? MB_ICONWARNING : MB_ICONINFORMATION));
}

void DoScan(AppState& state) {
    SetBusy(state, true);
    state.logPath.clear();
    SetWindowTextW(GetDlgItem(state.window, IDC_LOG), L"");
    state.scan = ScanSystem(state.language, [&](const std::wstring& message, int percent) { UpdateProgress(state, message, percent); });
    PopulateLists(state);
    std::wostringstream summary;
    summary << Tr(state.language, L"Найдено продуктов: ", L"Products found: ") << state.scan.products.size()
            << Tr(state.language, L", лицензий: ", L", licenses: ") << state.scan.licenses.size()
            << Tr(state.language, L", открытых сертификатов: ", L", public certificates: ") << state.scan.certificates.size();
    AddLogLine(state, summary.str());
    for (const auto& warning : state.scan.warnings) AddLogLine(state, warning);
    UpdateProgress(state, Tr(state.language, L"Готово. Изменения не выполнялись.", L"Ready. No changes were made."), 100);
    SetBusy(state, false);
}

void LogExecution(AppState& state, const ExecutionResult& execution) {
    for (const auto& operation : execution.operations) {
        std::wstring prefix;
        switch (operation.outcome) {
            case Outcome::Succeeded: prefix = L"[OK] "; break;
            case Outcome::RebootRequired: prefix = L"[REBOOT] "; break;
            case Outcome::Skipped: prefix = L"[SKIP] "; break;
            case Outcome::Failed: prefix = L"[ERROR] "; break;
        }
        AddLogLine(state, prefix + operation.action + L": " + operation.target + L" — " + operation.message);
    }
}

std::wstring ReportFolder(const AppState& state) {
    const size_t slash = state.reportPath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? state.backupRoot : state.reportPath.substr(0, slash);
}

void FinalizeExecution(AppState& state, ExecutionResult execution) {
    LogExecution(state, execution);
    ScanResult verification = VerifyAfterCleanup(state.language, [&](const std::wstring& message, int percent) {
        UpdateProgress(state, message, std::min(99, 80 + percent / 5));
    });
    std::wstring reportError;
    WriteJsonReport(state.reportPath, state.scan, &state.plan, &execution, &verification, &reportError);
    WriteTextSummary(state.language, ReportFolder(state) + L"\\summary.txt", state.scan, &state.plan, &execution, &verification, &reportError);
    const bool pendingRestart = execution.rebootRequired;
    const bool partial = execution.anyFailure || !verification.products.empty() || !verification.warnings.empty();
    const std::wstring result = execution.residualCleanupDeferred ?
        Tr(state.language,
           L"Штатное удаление выполнено. Очистка остатков продолжится только после перезагрузки.",
           L"Registered uninstall completed. Residual cleanup will continue only after restart.") :
        (pendingRestart ?
            Tr(state.language,
               L"Очистка выполнена. Отложенные операции завершатся после перезагрузки.",
               L"Cleanup completed. Deferred operations will finish after restart.") :
            (partial ?
                Tr(state.language, L"Удалено частично. Подробности сохранены в отчёте.", L"Partially removed. See the report for details.") :
                Tr(state.language, L"Выбранные продукты и подтверждённые остатки удалены.", L"Selected products and verified remnants were removed.")));
    UpdateProgress(state, result, 100);
    MessageBoxW(state.window, (result + L"\r\n\r\n" + state.reportPath).c_str(),
                Tr(state.language, L"Результат", L"Result").c_str(), MB_OK | ((partial && !pendingRestart) ? MB_ICONWARNING : MB_ICONINFORMATION));

    if (execution.rebootRequired) {
        std::wstring token, error;
        if (PrepareResume(state.plan, ReportFolder(state), &token, &error)) {
            const int answer = MessageBoxW(state.window,
                Tr(state.language,
                   L"Для завершения требуется перезагрузка. Продолжение уже зарегистрировано. Перезагрузить компьютер сейчас?",
                   L"A restart is required. Continuation has been registered. Restart the computer now?").c_str(),
                Tr(state.language, L"Требуется перезагрузка", L"Restart required").c_str(), MB_YESNO | MB_ICONQUESTION);
            if (answer == IDYES && !RequestSystemRestart(&error)) MessageBoxW(state.window, error.c_str(), L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
        } else {
            MessageBoxW(state.window, error.c_str(), Tr(state.language, L"Не удалось зарегистрировать продолжение", L"Could not register continuation").c_str(), MB_OK | MB_ICONERROR);
        }
    }
    SetBusy(state, false);
}

void StartCleanup(AppState& state) {
    ReadSelections(state);
    const size_t selected = static_cast<size_t>(std::count_if(state.scan.products.begin(), state.scan.products.end(), [](const InstalledProduct& item) { return item.selected; }));
    if (!selected) {
        MessageBoxW(state.window, Tr(state.language, L"Не выбран ни один продукт.", L"No product is selected.").c_str(),
                    L"CryptoPro Cleanup Utility", MB_OK | MB_ICONWARNING);
        return;
    }
    const bool highRisk = std::any_of(state.scan.products.begin(), state.scan.products.end(), [](const InstalledProduct& item) { return item.selected && item.risk == RiskLevel::High; });
    if (highRisk && !TypedConfirm(state, state.language == Language::Russian ? L"УДАЛИТЬ" : L"DELETE",
        Tr(state.language,
           L"Выбраны серверные или системно-критичные компоненты. Их удаление может нарушить вход, VPN, EFS или доступ к данным.",
           L"Server or system-critical components are selected. Removal can affect sign-in, VPN, EFS, or access to data."))) return;

    SetBusy(state, true);
    state.plan = BuildCleanupPlan(state.scan, [&](const std::wstring& message, int percent) { UpdateProgress(state, message, percent / 2); });
    const std::wstring backup = GetText(state.window, IDC_BACKUP_PATH);
    std::wstring error;
    if (!SaveBackup(state.language, state.scan, state.plan, backup, &state.licensesPath, &state.reportPath, &state.logPath, &error)) {
        SetBusy(state, false);
        MessageBoxW(state.window, error.c_str(), Tr(state.language, L"Резервная копия не создана — удаление отменено", L"Backup failed — removal cancelled").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    std::wostringstream confirmation;
    confirmation << Tr(state.language, L"Продуктов: ", L"Products: ") << selected
                 << Tr(state.language, L"\r\nПодтверждённых целей очистки: ", L"\r\nVerified cleanup targets: ") << state.plan.targets.size()
                 << Tr(state.language, L"\r\n\r\nСертификаты и контейнеры закрытых ключей будут сохранены. Продолжить?",
                       L"\r\n\r\nCertificates and private-key containers will be preserved. Continue?");
    if (MessageBoxW(state.window, confirmation.str().c_str(),
                    Tr(state.language, L"Подтверждение удаления", L"Confirm removal").c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        SetBusy(state, false);
        return;
    }
    ExecutionResult execution = ExecuteCleanup(state.plan, false, [&](const std::wstring& message, int percent) { UpdateProgress(state, message, percent); });
    const bool uninstallerFailed = std::any_of(execution.operations.begin(), execution.operations.end(), [](const OperationRecord& operation) {
        return operation.action == L"Uninstall" && operation.outcome == Outcome::Failed;
    });
    if (uninstallerFailed) {
        LogExecution(state, execution);
        const bool force = TypedConfirm(state, L"FORCE",
            Tr(state.language,
               L"Один или несколько штатных деинсталляторов завершились ошибкой. Принудительная очистка затронет только ранее подтверждённые цели.",
               L"One or more registered uninstallers failed. Forced cleanup will touch only targets verified before uninstall."));
        if (force) {
            ExecutionResult forced = ExecuteCleanup(state.plan, true, [&](const std::wstring& message, int percent) { UpdateProgress(state, message, percent); });
            execution.rebootRequired = execution.rebootRequired || forced.rebootRequired;
            execution.residualCleanupDeferred = execution.residualCleanupDeferred || forced.residualCleanupDeferred;
            execution.anyFailure = execution.anyFailure || forced.anyFailure;
            execution.anyRemoval = execution.anyRemoval || forced.anyRemoval;
            execution.operations.insert(execution.operations.end(),
                                        std::make_move_iterator(forced.operations.begin()),
                                        std::make_move_iterator(forced.operations.end()));
        }
    }
    FinalizeExecution(state, std::move(execution));
}

void ResumeCleanup(AppState& state) {
    SetBusy(state, true);
    std::wstring reportFolder, error;
    if (!LoadResumePlan(state.resumeToken, &state.scan, &state.plan, &reportFolder, &error)) {
        SetBusy(state, false);
        MessageBoxW(state.window, error.c_str(), Tr(state.language, L"Продолжение невозможно", L"Cannot resume").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    state.backupRoot = reportFolder;
    state.reportPath = reportFolder + L"\\report.json";
    state.logPath = reportFolder + L"\\cleanup.log";
    PopulateLists(state);
    AddLogLine(state, Tr(state.language, L"Продолжение очистки после перезагрузки.", L"Resuming cleanup after restart."));
    ExecutionResult execution = ExecuteCleanup(state.plan, true, [&](const std::wstring& message, int percent) { UpdateProgress(state, message, percent); });
    FinalizeExecution(state, std::move(execution));
    CompleteResume(state.resumeToken, nullptr);
}

INT_PTR CALLBACK MainDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<AppState*>(lParam);
        state->window = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SendDlgItemMessageW(dialog, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        HWND language = GetDlgItem(dialog, IDC_LANGUAGE);
        ComboBox_AddString(language, L"Русский");
        ComboBox_AddString(language, L"English");
        ComboBox_SetCurSel(language, state->language == Language::Russian ? 0 : 1);
        HWND tabs = GetDlgItem(dialog, IDC_TAB);
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        wchar_t cleanup[] = L"Cleanup";
        tab.pszText = cleanup;
        TabCtrl_InsertItem(tabs, 0, &tab);
        wchar_t certificates[] = L"Certificates";
        tab.pszText = certificates;
        TabCtrl_InsertItem(tabs, 1, &tab);
        wchar_t offline[] = L"Disconnected Windows";
        tab.pszText = offline;
        TabCtrl_InsertItem(tabs, 2, &tab);
        TabCtrl_SetCurSel(tabs, 0);
        state->backupRoot = DefaultBackupFolder();
        SetText(dialog, IDC_BACKUP_PATH, state->backupRoot);
        ApplyModernTheme(*state);
        ApplyLanguage(*state);
        CaptureInitialLayout(*state);
        PostMessageW(dialog, WM_CPC_START, 0, 0);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_GETMINMAXINFO && state->minimumWindow.cx && state->minimumWindow.cy) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = state->minimumWindow.cx;
        limits->ptMinTrackSize.y = state->minimumWindow.cy;
        return TRUE;
    }
    if (message == WM_SIZE && wParam != SIZE_MINIMIZED) {
        LayoutMainDialog(*state, LOWORD(lParam), HIWORD(lParam));
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG && state->backgroundBrush)
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    if (message == WM_CTLCOLORSTATIC && state->backgroundBrush) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkMode(device, TRANSPARENT);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        SetTextColor(device, id == IDC_TITLE ? RGB(25, 42, 70) : RGB(67, 78, 96));
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    }
    if (message == WM_CPC_START) {
        if (state->resumeToken.empty()) DoScan(*state); else ResumeCleanup(*state);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
            case IDC_LANGUAGE:
                if (HIWORD(wParam) == CBN_SELCHANGE && !state->busy) {
                    state->language = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_LANGUAGE)) == 0 ? Language::Russian : Language::English;
                    ApplyLanguage(*state);
                }
                return TRUE;
            case IDC_SCAN: if (!state->busy) DoScan(*state); return TRUE;
            case IDC_CLEAN: if (!state->busy) StartCleanup(*state); return TRUE;
            case IDC_SHOW_LICENSES: if (!state->busy) ShowLicenses(*state); return TRUE;
            case IDC_EXPORT_CERTS: if (!state->busy) ExportCertificates(*state); return TRUE;
            case IDC_OFFLINE_SHOW_LICENSES: if (!state->busy) ShowLicensesForScan(*state, state->offline.scan); return TRUE;
            case IDC_OFFLINE_SAVE: if (!state->busy) SaveOfflineData(*state); return TRUE;
            case IDC_OFFLINE_CLEAN: if (!state->busy) StartOfflineCleanup(*state); return TRUE;
            case IDC_OFFLINE_SCAN: if (!state->busy) DoOfflineScan(*state); return TRUE;
            case IDC_BROWSE: if (!state->busy) {
                const std::wstring folder = BrowseForFolder(dialog, GetText(dialog, IDC_BACKUP_PATH));
                if (!folder.empty()) SetText(dialog, IDC_BACKUP_PATH, folder);
            }
                return TRUE;
            case IDC_OFFLINE_BROWSE: if (!state->busy) {
                const std::wstring folder = BrowseForFolder(dialog, GetText(dialog, IDC_OFFLINE_PATH));
                if (!folder.empty()) SetText(dialog, IDC_OFFLINE_PATH, folder);
            }
                return TRUE;
            case IDC_SELECT_ALL_PROFILES: if (!state->busy) {
                const BOOL checked = Button_GetCheck(GetDlgItem(dialog, IDC_SELECT_ALL_PROFILES)) == BST_CHECKED;
                for (int index = 0; index < ListView_GetItemCount(GetDlgItem(dialog, IDC_PROFILES)); ++index)
                    ListView_SetCheckState(GetDlgItem(dialog, IDC_PROFILES), index, checked);
            }
                return TRUE;
            case IDC_SELECT_ALL_CERTS: if (!state->busy) {
                const BOOL checked = Button_GetCheck(GetDlgItem(dialog, IDC_SELECT_ALL_CERTS)) == BST_CHECKED;
                for (int index = 0; index < ListView_GetItemCount(GetDlgItem(dialog, IDC_CERTIFICATES)); ++index)
                    ListView_SetCheckState(GetDlgItem(dialog, IDC_CERTIFICATES), index, checked);
            }
                return TRUE;
            case IDC_OFFLINE_SELECT_ALL_CERTS: if (!state->busy) {
                const BOOL checked = Button_GetCheck(GetDlgItem(dialog, IDC_OFFLINE_SELECT_ALL_CERTS)) == BST_CHECKED;
                for (int index = 0; index < ListView_GetItemCount(GetDlgItem(dialog, IDC_OFFLINE_CERTS)); ++index)
                    ListView_SetCheckState(GetDlgItem(dialog, IDC_OFFLINE_CERTS), index, checked);
            }
                return TRUE;
            case IDCANCEL: if (!state->busy) EndDialog(dialog, 0); return TRUE;
        }
    }
    if (message == WM_NOTIFY) {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && (header->code == NM_CLICK || header->code == NM_RETURN) &&
            (header->idFrom == IDC_LINK_GITHUB || header->idFrom == IDC_LINK_WEBSITE ||
             header->idFrom == IDC_LINK_SUPPORT)) {
            OpenProjectLink(*state, static_cast<int>(header->idFrom));
            return TRUE;
        }
        if (header && header->idFrom == IDC_TAB && header->code == TCN_SELCHANGE && !state->busy) {
            UpdateTabVisibility(*state);
            return TRUE;
        }
        if (header && header->code == LVN_COLUMNCLICK && !state->busy &&
            (header->idFrom == IDC_PRODUCTS || header->idFrom == IDC_CERTIFICATES ||
             header->idFrom == IDC_OFFLINE_PRODUCTS || header->idFrom == IDC_OFFLINE_CERTS)) {
            const auto* click = reinterpret_cast<NMLISTVIEW*>(lParam);
            SortList(*state, static_cast<int>(header->idFrom), click->iSubItem);
            return TRUE;
        }
    }
    if (message == WM_DESTROY) {
        if (state->titleFont) { DeleteObject(state->titleFont); state->titleFont = nullptr; }
        if (state->backgroundBrush) { DeleteObject(state->backgroundBrush); state->backgroundBrush = nullptr; }
        return TRUE;
    }
    if (message == WM_CLOSE) { if (!state->busy) EndDialog(dialog, 0); return TRUE; }
    return FALSE;
}

}  // namespace

int RunGui(HINSTANCE instance, Language language, const std::wstring& resumeToken) {
    AppState state;
    state.language = language;
    state.resumeToken = resumeToken;
    return static_cast<int>(DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainDialogProc,
                                             reinterpret_cast<LPARAM>(&state)));
}

}  // namespace cpc
