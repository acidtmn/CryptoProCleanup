#include "cleanup.hpp"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstring>
#include <iterator>
#include <memory>
#include <numeric>
#include <sstream>
#include <thread>

namespace cpc {
namespace {

constexpr UINT WM_CPC_START = WM_APP + 10;
constexpr UINT WM_CPC_OFFLINE_PROGRESS = WM_APP + 11;
constexpr UINT WM_CPC_OFFLINE_COMPLETE = WM_APP + 12;
constexpr UINT WM_CPC_SCAN_PROGRESS = WM_APP + 13;
constexpr UINT WM_CPC_SCAN_COMPLETE = WM_APP + 14;

constexpr COLORREF kBackground = RGB(7, 7, 8);
constexpr COLORREF kSurface = RGB(16, 16, 18);
constexpr COLORREF kSurfaceSecondary = RGB(21, 19, 24);
constexpr COLORREF kBorder = RGB(41, 37, 45);
constexpr COLORREF kText = RGB(245, 243, 240);
constexpr COLORREF kMutedText = RGB(174, 168, 178);
constexpr COLORREF kAccent = RGB(169, 109, 135);
constexpr COLORREF kSuccess = RGB(105, 188, 136);
constexpr COLORREF kDanger = RGB(239, 106, 102);

enum class Page {
    Overview,
    Certificates,
    Offline,
    Log,
    Settings,
    About
};

struct OfflineProgressMessage {
    std::wstring message;
    int percent = 0;
};

using ScanProgressMessage = OfflineProgressMessage;

enum class SummaryKind {
    None,
    LiveScan,
    OfflineScan
};

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
    std::map<int, std::vector<int>> savedColumnWeights;
    std::map<int, RECT> initialControlRects;
    SIZE initialClient{};
    SIZE minimumWindow{};
    HFONT titleFont = nullptr;
    HFONT countFont = nullptr;
    HFONT brandFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HBRUSH editBrush = nullptr;
    HBRUSH accentBrush = nullptr;
    HIMAGELIST checkImages = nullptr;
    bool busy = false;
    bool offlineScanRunning = false;
    std::thread offlineScanThread;
    bool liveScanRunning = false;
    std::thread liveScanThread;
    SummaryKind summaryKind = SummaryKind::None;
    Language summaryLanguage = Language::English;
    Page currentPage = Page::Overview;
    bool languageExplicit = false;
    bool rememberWindow = true;
    bool useSystemColors = false;
    bool highContrast = false;
    bool hasSavedWindow = false;
    RECT savedWindow{};
    bool refreshingUi = false;
    bool skipSettingsSave = false;
    bool compactLayout = false;
    bool narrowLayout = false;
    bool lowResolutionLayout = false;
};

struct ConfirmState {
    Language language = Language::English;
    std::wstring phrase;
    std::wstring message;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH editBrush = nullptr;
};

struct LicenseDialogState {
    Language language = Language::English;
    std::wstring text;
    std::wstring title;
    std::wstring warning;
    std::wstring copiedMessage;
};

struct PlanDialogState {
    Language language = Language::English;
    const CleanupPlan* plan = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH editBrush = nullptr;
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
    GetClientRect(state.window, &client);
    state.initialClient = {client.right - client.left, client.bottom - client.top};
    // The old hard minimum was wider and taller than an 800x600 work area,
    // which placed the primary action row beyond the visible desktop. Keep the
    // tracking minimum independent from the current monitor so the compact
    // layout can also be exercised on a larger development display.
    state.minimumWindow = {640, 480};
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

void LayoutNarrowActionRows(AppState& state, int clientWidth, int clientHeight);
void LayoutLowResolutionPage(AppState& state, int clientWidth, int clientHeight);

void LayoutMainDialog(AppState& state, int clientWidth, int clientHeight) {
    if (!state.initialClient.cx || !state.initialClient.cy) return;
    const int dx = clientWidth - static_cast<int>(state.initialClient.cx);
    const int dy = clientHeight - static_cast<int>(state.initialClient.cy);
    const int leftDx = (dx * 2) / 3;
    const int rightDx = dx - leftDx;
    const int halfY = dy / 2;
    const int profileGrowth = dy < 0 ? dy / 3 : dy - halfY;
    state.compactLayout = clientHeight < 650;
    state.narrowLayout = clientWidth < 1100 || dx < -240;
    state.lowResolutionLayout = clientWidth < 900 || clientHeight < 620;
    const int compactActionShift = state.compactLayout ? -24 : 0;
    const int offlineCompactShift = state.compactLayout ? -30 : 0;
    HDWP positions = BeginDeferWindowPos(90);

    for (const int id : {IDC_NAV_SETTINGS, IDC_NAV_ABOUT, IDC_SAFETY_STATE})
        PlaceControl(state, &positions, id, 0, dy, 0, 0);
    for (const int id : {IDC_PAGE_EYEBROW, IDC_TITLE, IDC_DISCLAIMER})
        PlaceControl(state, &positions, id, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_HEADER_STATUS, dx, 0, 0, 0);
    PlaceControl(state, &positions, IDC_LANGUAGE, dx, 0, 0, 0);

    // Overview: the product table receives two thirds of additional width while
    // the plan card keeps a useful reading width.
    PlaceControl(state, &positions, IDC_PRODUCTS, 0, 0, leftDx, halfY);
    for (const int id : {IDC_PLAN_LABEL, IDC_PLAN_SUMMARY, IDC_CHECK_PLAN, IDC_PROTECTED_SUMMARY})
        PlaceControl(state, &positions, id, leftDx, 0, rightDx, 0);
    PlaceControl(state, &positions, IDC_PROFILES_LABEL, 0, halfY, dx / 2, 0);
    PlaceControl(state, &positions, IDC_PROFILES, 0, halfY, dx / 2, profileGrowth);
    PlaceControl(state, &positions, IDC_SELECT_ALL_PROFILES, 0, halfY + profileGrowth, dx / 2, 0);
    for (const int id : {IDC_BACKUP_LABEL, IDC_BACKUP_INFO})
        PlaceControl(state, &positions, id, dx / 2, halfY, dx - dx / 2, 0);
    PlaceControl(state, &positions, IDC_SHOW_LICENSES, dx / 2, halfY + compactActionShift,
                 dx - dx / 2, 0);
    PlaceControl(state, &positions, IDC_BACKUP_PATH, dx / 2, halfY, dx - dx / 2, 0);
    PlaceControl(state, &positions, IDC_BROWSE, dx, halfY, 0, 0);
    for (const int id : {IDC_SCAN, IDC_CLEAN})
        PlaceControl(state, &positions, id, dx, halfY + compactActionShift, 0, 0);

    // Certificate inventory and details pane.
    PlaceControl(state, &positions, IDC_CERT_INFO, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_CERT_SEARCH, 0, 0, leftDx, 0);
    PlaceControl(state, &positions, IDC_CERT_FILTER, leftDx, 0, 0, 0);
    PlaceControl(state, &positions, IDC_CERTIFICATES, 0, 0, leftDx, dy);
    PlaceControl(state, &positions, IDC_CERT_DETAILS_TITLE, leftDx, 0, rightDx, 0);
    PlaceControl(state, &positions, IDC_CERT_DETAILS, leftDx, 0, rightDx, dy);
    PlaceControl(state, &positions, IDC_COPY_THUMBPRINT, leftDx, dy, rightDx, 0);
    PlaceControl(state, &positions, IDC_EXPORT_CERTS, leftDx, dy, rightDx, 0);
    PlaceControl(state, &positions, IDC_SELECT_ALL_CERTS, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_CERT_SELECTED_COUNT, leftDx, dy, 0, 0);

    // Disconnected Windows page.
    for (const int id : {IDC_OFFLINE_INFO, IDC_OFFLINE_SUMMARY}) PlaceControl(state, &positions, id, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PATH_LABEL, 0, offlineCompactShift, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PATH, 0, offlineCompactShift, dx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_BROWSE, dx, offlineCompactShift, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_SCAN, dx, offlineCompactShift, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_SUMMARY, 0, offlineCompactShift, dx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PRODUCTS_LABEL, 0, offlineCompactShift, leftDx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_PRODUCTS, 0, offlineCompactShift, leftDx, halfY);
    PlaceControl(state, &positions, IDC_OFFLINE_CERTS_LABEL, 0, halfY + offlineCompactShift, leftDx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_CERTS, 0, halfY + offlineCompactShift, leftDx, dy - halfY);
    PlaceControl(state, &positions, IDC_OFFLINE_SELECT_ALL_CERTS, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_SHOW_LICENSES, leftDx, 0, rightDx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_DIAGNOSTICS, leftDx, 0, rightDx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_SAVE, leftDx,
                 dy + (state.compactLayout ? 80 : 0), rightDx, 0);
    PlaceControl(state, &positions, IDC_OFFLINE_CLEAN, leftDx,
                 dy + (state.compactLayout ? 40 : 0), rightDx, 0);

    // Log, settings and about pages.
    PlaceControl(state, &positions, IDC_LOG_INFO, 0, 0, dx, 0);
    PlaceControl(state, &positions, IDC_LOG, 0, 0, dx, dy);
    PlaceControl(state, &positions, IDC_COPY_LOG, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_OPEN_REPORT_FOLDER, 0, dy, 0, 0);
    PlaceControl(state, &positions, IDC_CLEAR_LOG, dx, dy, 0, 0);
    for (const int id : {IDC_SETTINGS_HIGH_CONTRAST, IDC_SETTINGS_INFO, IDC_ABOUT_TITLE, IDC_ABOUT_TEXT})
        PlaceControl(state, &positions, id, 0, 0, dx, 0);

    PlaceControl(state, &positions, IDC_PROGRESS, 0, dy, dx, 0);
    PlaceControl(state, &positions, IDC_STATUS, 0, dy, dx, 0);
    if (positions) EndDeferWindowPos(positions);
    if (state.narrowLayout) LayoutNarrowActionRows(state, clientWidth, clientHeight);
    LayoutLowResolutionPage(state, clientWidth, clientHeight);
    ShowWindow(GetDlgItem(state.window, IDC_PROTECTED_SUMMARY),
               state.currentPage == Page::Overview && !state.compactLayout ? SW_SHOW : SW_HIDE);
    for (const int id : {IDC_OFFLINE_STEP1, IDC_OFFLINE_STEP2, IDC_OFFLINE_STEP3})
        ShowWindow(GetDlgItem(state.window, id),
                   state.currentPage == Page::Offline && !state.compactLayout ? SW_SHOW : SW_HIDE);
    for (const int id : {IDC_PRODUCTS, IDC_PROFILES, IDC_CERTIFICATES,
                         IDC_OFFLINE_PRODUCTS, IDC_OFFLINE_CERTS}) ResizeListColumns(state, id);
}

void MoveControlTo(AppState& state, int id, int left, int top, int width, int height) {
    HWND control = GetDlgItem(state.window, id);
    if (control) MoveWindow(control, left, top, std::max(1, width), std::max(1, height), TRUE);
}

int OriginalControlHeight(const AppState& state, int id, int fallback = 28) {
    const auto found = state.initialControlRects.find(id);
    return found == state.initialControlRects.end() ? fallback :
        std::max(1, static_cast<int>(found->second.bottom - found->second.top));
}

void LayoutNarrowActionRows(AppState& state, int clientWidth, int clientHeight) {
    const auto content = state.initialControlRects.find(IDC_PAGE_EYEBROW);
    const auto language = state.initialControlRects.find(IDC_LANGUAGE);
    const auto progress = state.initialControlRects.find(IDC_PROGRESS);
    if (content == state.initialControlRects.end() || language == state.initialControlRects.end() ||
        progress == state.initialControlRects.end()) return;
    const int left = content->second.left;
    const int rightMargin = std::max(12, static_cast<int>(state.initialClient.cx - language->second.right));
    const int width = std::max(300, clientWidth - left - rightMargin);
    const int footerTop = std::max(120, clientHeight - static_cast<int>(state.initialClient.cy - progress->second.top));
    const int gap = 8;

    const int overviewHeight = std::max(40, OriginalControlHeight(state, IDC_CLEAN));
    const int overviewTop = footerTop - overviewHeight - gap;
    const int licenseWidth = std::max(170, (width * 40) / 100);
    const int scanWidth = std::max(90, (width * 22) / 100);
    const int cleanWidth = std::max(120, width - licenseWidth - scanWidth - gap * 2);
    MoveControlTo(state, IDC_SHOW_LICENSES, left, overviewTop, licenseWidth, overviewHeight);
    MoveControlTo(state, IDC_SCAN, left + licenseWidth + gap, overviewTop, scanWidth, overviewHeight);
    MoveControlTo(state, IDC_CLEAN, left + licenseWidth + scanWidth + gap * 2,
                  overviewTop, cleanWidth, overviewHeight);

    const int certificateHeight = std::max(40, OriginalControlHeight(state, IDC_EXPORT_CERTS));
    const int certificateTop = footerTop - certificateHeight - gap;
    const int certificateHalf = (width - gap) / 2;
    MoveControlTo(state, IDC_COPY_THUMBPRINT, left, certificateTop, certificateHalf, certificateHeight);
    MoveControlTo(state, IDC_EXPORT_CERTS, left + certificateHalf + gap, certificateTop,
                  width - certificateHalf - gap, certificateHeight);
    const int selectionHeight = std::max(20, OriginalControlHeight(state, IDC_SELECT_ALL_CERTS));
    const int selectionTop = certificateTop - selectionHeight - 5;
    MoveControlTo(state, IDC_SELECT_ALL_CERTS, left, selectionTop, certificateHalf, selectionHeight);
    MoveControlTo(state, IDC_CERT_SELECTED_COUNT, left + certificateHalf + gap, selectionTop,
                  width - certificateHalf - gap, selectionHeight);

    const int offlineHeight = std::max(40, OriginalControlHeight(state, IDC_OFFLINE_CLEAN));
    const int offlineSecondTop = footerTop - offlineHeight - gap;
    const int offlineFirstTop = offlineSecondTop - offlineHeight - gap;
    const int offlineHalf = (width - gap) / 2;
    MoveControlTo(state, IDC_OFFLINE_SHOW_LICENSES, left, offlineFirstTop, offlineHalf, offlineHeight);
    MoveControlTo(state, IDC_OFFLINE_DIAGNOSTICS, left + offlineHalf + gap, offlineFirstTop,
                  width - offlineHalf - gap, offlineHeight);
    MoveControlTo(state, IDC_OFFLINE_SAVE, left, offlineSecondTop, offlineHalf, offlineHeight);
    MoveControlTo(state, IDC_OFFLINE_CLEAN, left + offlineHalf + gap, offlineSecondTop,
                  width - offlineHalf - gap, offlineHeight);
}

void ShowResponsiveControl(AppState& state, int id, bool visible) {
    ShowWindow(GetDlgItem(state.window, id), visible ? SW_SHOW : SW_HIDE);
}

void LayoutLowResolutionPage(AppState& state, int clientWidth, int clientHeight) {
    const bool low = state.lowResolutionLayout;
    const bool overview = state.currentPage == Page::Overview;
    const bool certificates = state.currentPage == Page::Certificates;
    const bool offline = state.currentPage == Page::Offline;
    const bool log = state.currentPage == Page::Log;
    const bool settings = state.currentPage == Page::Settings;
    const bool about = state.currentPage == Page::About;

    ShowResponsiveControl(state, IDC_PAGE_EYEBROW, !low);
    ShowResponsiveControl(state, IDC_DISCLAIMER, !low);
    ShowResponsiveControl(state, IDC_HEADER_STATUS, !low);

    for (const int id : {IDC_STAT_PRODUCTS_CAPTION, IDC_STAT_PRODUCTS_VALUE,
                         IDC_STAT_LICENSES_CAPTION, IDC_STAT_LICENSES_VALUE,
                         IDC_STAT_CERTS_CAPTION, IDC_STAT_CERTS_VALUE,
                         IDC_STAT_PROFILES_CAPTION, IDC_STAT_PROFILES_VALUE,
                         IDC_PLAN_LABEL, IDC_PLAN_SUMMARY, IDC_PROTECTED_SUMMARY,
                         IDC_BACKUP_INFO})
        ShowResponsiveControl(state, id, overview && !low);
    for (const int id : {IDC_CERT_DETAILS_TITLE, IDC_CERT_DETAILS})
        ShowResponsiveControl(state, id, certificates && !low);
    ShowResponsiveControl(state, IDC_OFFLINE_INFO, offline);

    if (!low) return;

    const auto content = state.initialControlRects.find(IDC_PAGE_EYEBROW);
    const auto language = state.initialControlRects.find(IDC_LANGUAGE);
    const auto progress = state.initialControlRects.find(IDC_PROGRESS);
    if (content == state.initialControlRects.end() || language == state.initialControlRects.end() ||
        progress == state.initialControlRects.end()) return;
    const int left = content->second.left;
    const int rightMargin = std::max(12, static_cast<int>(state.initialClient.cx - language->second.right));
    const int width = std::max(300, clientWidth - left - rightMargin);
    const int gap = 8;
    const int languageWidth = static_cast<int>(language->second.right - language->second.left);
    const int languageHeight = OriginalControlHeight(state, IDC_LANGUAGE, 26);
    const int languageLeft = clientWidth - rightMargin - languageWidth;
    MoveControlTo(state, IDC_LANGUAGE, languageLeft, 20, languageWidth, languageHeight);
    MoveControlTo(state, IDC_TITLE, left, 38, std::max(150, languageLeft - left - gap), 52);

    if (state.compactLayout) {
        const auto firstNavigation = state.initialControlRects.find(IDC_NAV_OVERVIEW);
        if (firstNavigation != state.initialControlRects.end()) {
            const int navigationLeft = firstNavigation->second.left;
            const int navigationWidth = static_cast<int>(firstNavigation->second.right - firstNavigation->second.left);
            const int navigationTop = 96;
            const int navigationHeight = 37;
            const int navigationGap = 5;
            int index = 0;
            for (const int id : {IDC_NAV_OVERVIEW, IDC_NAV_CERTIFICATES, IDC_NAV_OFFLINE,
                                 IDC_NAV_LOG, IDC_NAV_SETTINGS, IDC_NAV_ABOUT}) {
                MoveControlTo(state, id, navigationLeft,
                              navigationTop + index * (navigationHeight + navigationGap),
                              navigationWidth, navigationHeight);
                ++index;
            }
            MoveControlTo(state, IDC_SAFETY_STATE, navigationLeft,
                          std::max(navigationTop + index * (navigationHeight + navigationGap) + 4,
                                   clientHeight - 55),
                          navigationWidth, 30);
        }
    }

    const int footerTop = std::max(120, clientHeight -
        static_cast<int>(state.initialClient.cy - progress->second.top));
    const int actionHeight = 40;
    const int actionTop = footerTop - actionHeight - gap;
    const int bodyTop = 98;

    if (overview) {
        const int pathTop = actionTop - 43;
        const int backupLabelTop = pathTop - 20;
        const int selectProfilesTop = backupLabelTop - 25;
        const int profilesLabelTop = selectProfilesTop - 70;
        const int checkPlanTop = profilesLabelTop - 43;
        const int productsListTop = bodyTop + 18;
        MoveControlTo(state, IDC_PRODUCTS_LABEL, left, bodyTop, width, 16);
        MoveControlTo(state, IDC_PRODUCTS, left, productsListTop, width,
                      std::max(42, checkPlanTop - productsListTop - 7));
        MoveControlTo(state, IDC_CHECK_PLAN, left, checkPlanTop, width, 34);
        MoveControlTo(state, IDC_PROFILES_LABEL, left, profilesLabelTop, width, 16);
        MoveControlTo(state, IDC_PROFILES, left, profilesLabelTop + 18, width, 46);
        MoveControlTo(state, IDC_SELECT_ALL_PROFILES, left, selectProfilesTop, width, 19);
        MoveControlTo(state, IDC_BACKUP_LABEL, left, backupLabelTop, width, 16);
        const int browseWidth = std::min(110, std::max(90, width / 4));
        MoveControlTo(state, IDC_BACKUP_PATH, left, pathTop, width - browseWidth - gap, 28);
        MoveControlTo(state, IDC_BROWSE, left + width - browseWidth, pathTop, browseWidth, 28);
    } else if (certificates) {
        MoveControlTo(state, IDC_CERT_INFO, left, bodyTop, width, 42);
        const int filterWidth = std::min(270, std::max(180, width / 2));
        MoveControlTo(state, IDC_CERT_SEARCH, left, bodyTop + 48, width - filterWidth - gap, 28);
        MoveControlTo(state, IDC_CERT_FILTER, left + width - filterWidth, bodyTop + 48, filterWidth, 90);
        const int selectionTop = actionTop - 27;
        MoveControlTo(state, IDC_CERTIFICATES, left, bodyTop + 82, width,
                      std::max(50, selectionTop - bodyTop - 90));
        const int half = (width - gap) / 2;
        MoveControlTo(state, IDC_SELECT_ALL_CERTS, left, selectionTop, half, 20);
        MoveControlTo(state, IDC_CERT_SELECTED_COUNT, left + half + gap, selectionTop,
                      width - half - gap, 20);
        MoveControlTo(state, IDC_COPY_THUMBPRINT, left, actionTop, half, actionHeight);
        MoveControlTo(state, IDC_EXPORT_CERTS, left + half + gap, actionTop,
                      width - half - gap, actionHeight);
    } else if (offline) {
        const bool veryShort = clientHeight < 540;
        ShowResponsiveControl(state, IDC_OFFLINE_INFO, !veryShort);
        if (!veryShort) MoveControlTo(state, IDC_OFFLINE_INFO, left, bodyTop, width, 43);
        const int pathLabelWidth = 92;
        const int browseWidth = 92;
        const int scanWidth = 104;
        const int pathRowTop = veryShort ? bodyTop : bodyTop + 58;
        MoveControlTo(state, IDC_OFFLINE_PATH_LABEL, left, pathRowTop + 6, pathLabelWidth, 18);
        MoveControlTo(state, IDC_OFFLINE_PATH, left + pathLabelWidth + gap, pathRowTop,
                      std::max(50, width - pathLabelWidth - browseWidth - scanWidth - gap * 3), 28);
        MoveControlTo(state, IDC_OFFLINE_BROWSE, left + width - browseWidth - scanWidth - gap,
                      pathRowTop, browseWidth, 28);
        MoveControlTo(state, IDC_OFFLINE_SCAN, left + width - scanWidth, pathRowTop, scanWidth, 28);
        MoveControlTo(state, IDC_OFFLINE_SUMMARY, left, pathRowTop + 35, width, 30);

        const int offlineFirstTop = footerTop - actionHeight * 2 - gap * 2;
        // Keep the checkbox clear of the action-card padding at very small heights.
        const int selectionTop = offlineFirstTop - 38;
        const int listsTop = pathRowTop + 70;
        const int listSpace = std::max(58, selectionTop - listsTop);
        const int productListHeight = std::max(24, (listSpace - 42) / 2);
        const int productsLabelTop = listsTop;
        const int productsTop = productsLabelTop + 17;
        const int certificatesLabelTop = productsTop + productListHeight + 5;
        const int certificatesTop = certificatesLabelTop + 17;
        MoveControlTo(state, IDC_OFFLINE_PRODUCTS_LABEL, left, productsLabelTop, width, 15);
        MoveControlTo(state, IDC_OFFLINE_PRODUCTS, left, productsTop, width, productListHeight);
        MoveControlTo(state, IDC_OFFLINE_CERTS_LABEL, left, certificatesLabelTop, width, 15);
        MoveControlTo(state, IDC_OFFLINE_CERTS, left, certificatesTop, width,
                      std::max(24, selectionTop - certificatesTop - 5));
        MoveControlTo(state, IDC_OFFLINE_SELECT_ALL_CERTS, left, selectionTop, width, 20);
    } else if (log) {
        MoveControlTo(state, IDC_LOG_INFO, left, bodyTop, width, 38);
        MoveControlTo(state, IDC_LOG, left, bodyTop + 44, width,
                      std::max(60, actionTop - bodyTop - 52));
        const int firstWidth = (width * 28) / 100;
        const int secondWidth = (width * 40) / 100;
        MoveControlTo(state, IDC_COPY_LOG, left, actionTop, firstWidth, actionHeight);
        MoveControlTo(state, IDC_OPEN_REPORT_FOLDER, left + firstWidth + gap, actionTop,
                      secondWidth, actionHeight);
        MoveControlTo(state, IDC_CLEAR_LOG, left + firstWidth + secondWidth + gap * 2, actionTop,
                      width - firstWidth - secondWidth - gap * 2, actionHeight);
    } else if (settings) {
        const int labelWidth = std::min(165, width / 3);
        const int comboLeft = left + labelWidth + gap;
        const int comboWidth = std::min(250, width - labelWidth - gap);
        MoveControlTo(state, IDC_SETTINGS_THEME_LABEL, left, bodyTop + 7, labelWidth, 18);
        MoveControlTo(state, IDC_SETTINGS_THEME, comboLeft, bodyTop, comboWidth, 90);
        MoveControlTo(state, IDC_SETTINGS_LANGUAGE_LABEL, left, bodyTop + 49, labelWidth, 18);
        MoveControlTo(state, IDC_SETTINGS_LANGUAGE, comboLeft, bodyTop + 42, comboWidth, 90);
        MoveControlTo(state, IDC_SETTINGS_REMEMBER_WINDOW, left, bodyTop + 86, width, 24);
        MoveControlTo(state, IDC_SETTINGS_HIGH_CONTRAST, left, bodyTop + 120, width, 36);
        MoveControlTo(state, IDC_SETTINGS_RESET, left, bodyTop + 164,
                      std::min(300, width), actionHeight);
        MoveControlTo(state, IDC_SETTINGS_INFO, left, bodyTop + 214, width,
                      std::max(45, footerTop - bodyTop - 224));
    } else if (about) {
        MoveControlTo(state, IDC_ABOUT_TITLE, left, bodyTop, width, 34);
        MoveControlTo(state, IDC_ABOUT_TEXT, left, bodyTop + 40, width,
                      std::max(115, actionTop - bodyTop - 125));
        MoveControlTo(state, IDC_ABOUT_COPY_VERSION, left, actionTop - 74,
                      std::min(300, width), actionHeight);
        const int linksTop = actionTop - 27;
        const int githubWidth = std::min(80, width / 5);
        const int websiteWidth = std::min(150, (width * 35) / 100);
        MoveControlTo(state, IDC_ABOUT_GITHUB, left, linksTop, githubWidth, 24);
        MoveControlTo(state, IDC_ABOUT_WEBSITE, left + githubWidth + gap, linksTop, websiteWidth, 24);
        MoveControlTo(state, IDC_ABOUT_SUPPORT, left + githubWidth + websiteWidth + gap * 2,
                      linksTop, width - githubWidth - websiteWidth - gap * 2, 24);
    }
}

COLORREF UiColor(const AppState& state, COLORREF darkColor, int systemColor) {
    return (state.highContrast || state.useSystemColors) ? GetSysColor(systemColor) : darkColor;
}

LRESULT CALLBACK HeaderSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR subclassId, DWORD_PTR reference) {
    auto* state = reinterpret_cast<AppState*>(reference);
    if (message == WM_PAINT && state && !state->highContrast && !state->useSystemColors) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(kSurfaceSecondary);
        FillRect(device, &client, background);
        DeleteObject(background);
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, kMutedText);
        const int count = Header_GetItemCount(window);
        for (int index = 0; index < count; ++index) {
            RECT itemRectangle{};
            if (!Header_GetItemRect(window, index, &itemRectangle)) continue;
            wchar_t caption[256]{};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = caption;
            item.cchTextMax = static_cast<int>(std::size(caption));
            Header_GetItem(window, index, &item);
            RECT textRectangle = itemRectangle;
            textRectangle.left += 7;
            textRectangle.right -= 7;
            DrawTextW(device, caption, -1, &textRectangle,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            HPEN divider = CreatePen(PS_SOLID, 1, kBorder);
            HGDIOBJ oldPen = SelectObject(device, divider);
            MoveToEx(device, itemRectangle.right - 1, itemRectangle.top + 3, nullptr);
            LineTo(device, itemRectangle.right - 1, itemRectangle.bottom - 3);
            SelectObject(device, oldPen);
            DeleteObject(divider);
        }
        HPEN bottom = CreatePen(PS_SOLID, 1, kBorder);
        HGDIOBJ oldPen = SelectObject(device, bottom);
        MoveToEx(device, client.left, client.bottom - 1, nullptr);
        LineTo(device, client.right, client.bottom - 1);
        SelectObject(device, oldPen);
        DeleteObject(bottom);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, HeaderSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK CheckboxSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR subclassId, DWORD_PTR reference) {
#ifdef CPC_MODERN_UI
    auto* state = reinterpret_cast<AppState*>(reference);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && state) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const bool onCard = GetDlgCtrlID(window) == IDC_SETTINGS_REMEMBER_WINDOW;
        FillRect(device, &client, onCard ? state->surfaceBrush : state->backgroundBrush);
        const int size = std::max(13, std::min(17, static_cast<int>(client.bottom - client.top - 2)));
        RECT box{1, (client.bottom - size) / 2, 1 + size, (client.bottom - size) / 2 + size};
        const bool checked = Button_GetCheck(window) == BST_CHECKED;
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const COLORREF accent = enabled ? kAccent : RGB(91, 82, 91);
        HBRUSH brush = CreateSolidBrush(checked ? accent : (onCard ? kSurface : kBackground));
        HPEN pen = CreatePen(PS_SOLID, 1, checked ? accent : (enabled ? kMutedText : RGB(91, 82, 91)));
        HGDIOBJ oldBrush = SelectObject(device, brush);
        HGDIOBJ oldPen = SelectObject(device, pen);
        RoundRect(device, box.left, box.top, box.right, box.bottom, 5, 5);
        SelectObject(device, oldBrush);
        SelectObject(device, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        if (checked) {
            HPEN check = CreatePen(PS_SOLID, 2, enabled ? kText : kMutedText);
            oldPen = SelectObject(device, check);
            MoveToEx(device, box.left + size / 4, box.top + size / 2, nullptr);
            LineTo(device, box.left + size * 5 / 12, box.top + size * 2 / 3);
            LineTo(device, box.left + size * 3 / 4, box.top + size / 3);
            SelectObject(device, oldPen);
            DeleteObject(check);
        }
        wchar_t caption[384]{};
        GetWindowTextW(window, caption, static_cast<int>(std::size(caption)));
        RECT textRectangle = client;
        textRectangle.left = box.right + 7;
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, enabled ? kText : RGB(112, 106, 116));
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = font ? SelectObject(device, font) : nullptr;
        DrawTextW(device, caption, -1, &textRectangle,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (oldFont) SelectObject(device, oldFont);
        if (GetFocus() == window) {
            RECT focus = textRectangle;
            DrawFocusRect(device, &focus);
        }
        EndPaint(window, &paint);
        return 0;
    }
#else
    (void)reference;
#endif
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, CheckboxSubclassProc, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

HFONT CreateUiFont(HWND window, int points, int weight) {
    HDC device = GetDC(window);
    const int dpi = device ? GetDeviceCaps(device, LOGPIXELSY) : 96;
    if (device) ReleaseDC(window, device);
    LOGFONTW font{};
    font.lfHeight = -MulDiv(points, dpi, 72);
    font.lfWeight = weight;
#ifdef CPC_MODERN_UI
    wcscpy_s(font.lfFaceName, L"Segoe UI Variable Text");
#else
    wcscpy_s(font.lfFaceName, L"Segoe UI");
#endif
    return CreateFontIndirectW(&font);
}

void EnableModernWindowEffects(HWND window) {
#ifdef CPC_MODERN_UI
    using SetPreferredAppMode = int (WINAPI*)(int);
    HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
    if (theme) {
        auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(theme, MAKEINTRESOURCEA(135)));
        if (setMode) setMode(2);  // AllowDark / ForceDark on supported Windows 10/11 builds.
    }
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using DwmSetWindowAttributeFunction = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFunction>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setAttribute) {
        const BOOL enabled = TRUE;
        if (FAILED(setAttribute(window, 20, &enabled, sizeof(enabled))))
            setAttribute(window, 19, &enabled, sizeof(enabled));
        const COLORREF caption = kBackground;
        const COLORREF captionText = kText;
        setAttribute(window, 35, &caption, sizeof(caption));
        setAttribute(window, 36, &captionText, sizeof(captionText));
        const DWORD roundedCorners = 2;
        setAttribute(window, 33, &roundedCorners, sizeof(roundedCorners));
        const DWORD mainWindowBackdrop = 2;
        setAttribute(window, 38, &mainWindowBackdrop, sizeof(mainWindowBackdrop));
    }
    FreeLibrary(dwm);
#else
    (void)window;
#endif
}

HIMAGELIST CreateModernCheckImages(const AppState& state) {
#ifdef CPC_MODERN_UI
    if (state.highContrast || state.useSystemColors) return nullptr;
    HDC screen = GetDC(state.window);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    const int size = std::max(14, MulDiv(16, dpi, 96));
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, size, size);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR24 | ILC_MASK, 2, 0);
    const COLORREF maskColor = RGB(255, 0, 255);
    for (int checked = 0; checked < 2; ++checked) {
        RECT rectangle{0, 0, size, size};
        HBRUSH mask = CreateSolidBrush(maskColor);
        FillRect(memory, &rectangle, mask);
        DeleteObject(mask);
        RECT box{1, 1, size - 1, size - 1};
        HBRUSH fill = CreateSolidBrush(checked ? kAccent : kSurfaceSecondary);
        HPEN border = CreatePen(PS_SOLID, std::max(1, size / 10), checked ? kAccent : kMutedText);
        HGDIOBJ oldBrush = SelectObject(memory, fill);
        HGDIOBJ oldPen = SelectObject(memory, border);
        RoundRect(memory, box.left, box.top, box.right, box.bottom, std::max(4, size / 3), std::max(4, size / 3));
        SelectObject(memory, oldBrush);
        SelectObject(memory, oldPen);
        DeleteObject(fill);
        DeleteObject(border);
        if (checked) {
            HPEN checkPen = CreatePen(PS_SOLID, std::max(2, size / 7), kText);
            oldPen = SelectObject(memory, checkPen);
            MoveToEx(memory, size / 4, size / 2, nullptr);
            LineTo(memory, size * 5 / 12, size * 2 / 3);
            LineTo(memory, size * 3 / 4, size / 3);
            SelectObject(memory, oldPen);
            DeleteObject(checkPen);
        }
        ImageList_AddMasked(images, bitmap, maskColor);
    }
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    if (screen) ReleaseDC(state.window, screen);
    return images;
#else
    (void)state;
    return nullptr;
#endif
}

void ResetThemeObjects(AppState& state) {
    for (HFONT* font : {&state.titleFont, &state.countFont, &state.brandFont}) {
        if (*font) { DeleteObject(*font); *font = nullptr; }
    }
    for (HBRUSH* brush : {&state.backgroundBrush, &state.surfaceBrush, &state.editBrush, &state.accentBrush}) {
        if (*brush) { DeleteObject(*brush); *brush = nullptr; }
    }
    if (state.checkImages) {
        for (const int id : {IDC_PRODUCTS, IDC_PROFILES, IDC_CERTIFICATES,
                             IDC_OFFLINE_PRODUCTS, IDC_OFFLINE_CERTS})
            ListView_SetImageList(GetDlgItem(state.window, id), nullptr, LVSIL_STATE);
        ImageList_Destroy(state.checkImages);
        state.checkImages = nullptr;
    }
}

void ApplyModernTheme(AppState& state) {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    state.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                         (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    ResetThemeObjects(state);
    state.backgroundBrush = CreateSolidBrush(UiColor(state, kBackground, COLOR_WINDOW));
    state.surfaceBrush = CreateSolidBrush(UiColor(state, kSurface, COLOR_BTNFACE));
    state.editBrush = CreateSolidBrush(UiColor(state, kSurfaceSecondary, COLOR_WINDOW));
    state.accentBrush = CreateSolidBrush(UiColor(state, kAccent, COLOR_HIGHLIGHT));
    state.checkImages = CreateModernCheckImages(state);
    state.titleFont = CreateUiFont(state.window, 18, FW_SEMIBOLD);
    state.countFont = CreateUiFont(state.window, 16, FW_SEMIBOLD);
    state.brandFont = CreateUiFont(state.window, 10, FW_BOLD);
    if (state.titleFont) SendDlgItemMessageW(state.window, IDC_TITLE, WM_SETFONT,
                                             reinterpret_cast<WPARAM>(state.titleFont), TRUE);
    for (const int id : {IDC_STAT_PRODUCTS_VALUE, IDC_STAT_LICENSES_VALUE,
                         IDC_STAT_CERTS_VALUE, IDC_STAT_PROFILES_VALUE, IDC_ABOUT_TITLE})
        if (state.countFont) SendDlgItemMessageW(state.window, id, WM_SETFONT,
                                                 reinterpret_cast<WPARAM>(state.countFont), TRUE);
    for (const int id : {IDC_BRAND, IDC_BRAND_VERSION, IDC_PAGE_EYEBROW})
        if (state.brandFont) SendDlgItemMessageW(state.window, id, WM_SETFONT,
                                                 reinterpret_cast<WPARAM>(state.brandFont), TRUE);
    for (const int id : {IDC_PRODUCTS, IDC_PROFILES, IDC_CERTIFICATES,
                         IDC_OFFLINE_PRODUCTS, IDC_OFFLINE_CERTS}) {
        HWND list = GetDlgItem(state.window, id);
        SetWindowTheme(list, state.highContrast || state.useSystemColors ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(list, UiColor(state, kSurface, COLOR_WINDOW));
        ListView_SetTextBkColor(list, UiColor(state, kSurface, COLOR_WINDOW));
        ListView_SetTextColor(list, UiColor(state, kText, COLOR_WINDOWTEXT));
        if (state.checkImages) ListView_SetImageList(list, state.checkImages, LVSIL_STATE);
        HWND header = ListView_GetHeader(list);
        if (header) {
            SetWindowSubclass(header, HeaderSubclassProc, static_cast<UINT_PTR>(id),
                              reinterpret_cast<DWORD_PTR>(&state));
            InvalidateRect(header, nullptr, TRUE);
        }
    }
    for (const int id : {IDC_SELECT_ALL_PROFILES, IDC_SELECT_ALL_CERTS, IDC_OFFLINE_SELECT_ALL_CERTS,
                         IDC_SETTINGS_REMEMBER_WINDOW}) {
        SetWindowTheme(GetDlgItem(state.window, id), state.highContrast || state.useSystemColors ? L"Explorer" : L"", L"");
        HWND checkbox = GetDlgItem(state.window, id);
#ifdef CPC_MODERN_UI
        if (checkbox) SetWindowSubclass(checkbox, CheckboxSubclassProc, static_cast<UINT_PTR>(id),
                                        reinterpret_cast<DWORD_PTR>(&state));
#endif
        InvalidateRect(checkbox, nullptr, TRUE);
    }
    for (const int id : {IDC_BACKUP_PATH, IDC_OFFLINE_PATH, IDC_CERT_SEARCH, IDC_LOG})
        SendDlgItemMessageW(state.window, id, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    for (const int id : {IDC_LANGUAGE, IDC_CERT_FILTER, IDC_SETTINGS_THEME, IDC_SETTINGS_LANGUAGE})
        SetWindowTheme(GetDlgItem(state.window, id), state.highContrast || state.useSystemColors ? L"Explorer" : L"DarkMode_CFD", nullptr);
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETSTATE, PBST_NORMAL, 0);
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETBARCOLOR, 0,
                        UiColor(state, kSuccess, COLOR_HIGHLIGHT));
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETBKCOLOR, 0,
                        UiColor(state, kSurfaceSecondary, COLOR_BTNFACE));
    InvalidateRect(state.window, nullptr, TRUE);
}

void OpenProjectLink(const AppState& state, int controlId) {
    const wchar_t* url = L"https://yoomoney.ru/to/4100119195083142";
    if (controlId == IDC_LINK_GITHUB || controlId == IDC_ABOUT_GITHUB)
        url = L"https://github.com/acidtmn/CryptoProCleanup";
    else if (controlId == IDC_LINK_WEBSITE || controlId == IDC_ABOUT_WEBSITE)
        url = L"https://kodalexandrova.ru";
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

std::wstring LiveScanSummary(const AppState& state) {
    std::wostringstream summary;
    summary << Tr(state.language, L"Найдено продуктов: ", L"Products found: ") << state.scan.products.size()
            << Tr(state.language, L", лицензий: ", L", licenses: ") << state.scan.licenses.size()
            << Tr(state.language, L", открытых сертификатов: ", L", public certificates: ")
            << state.scan.certificates.size();
    return summary.str();
}

std::wstring OfflineScanSummary(const AppState& state) {
    if (!state.offline.valid)
        return Tr(state.language, L"Офлайн-система не распознана.",
                                  L"Offline system was not recognized.");
    std::wostringstream summary;
    summary << Tr(state.language, L"Офлайн: продуктов ", L"Offline: products ") << state.offline.scan.products.size()
            << Tr(state.language, L", лицензий ", L", licenses ") << state.offline.scan.licenses.size()
            << Tr(state.language, L", профилей ", L", profiles ") << state.offline.scan.profiles.size()
            << Tr(state.language, L", открытых сертификатов ", L", public certificates ") << state.offline.scan.certificates.size()
            << Tr(state.language, L", подтверждённых целей ", L", verified targets ") << state.offline.targets.size()
            << (state.offline.cleanupCapable ? Tr(state.language, L", очистка доступна", L", cleanup available")
                                             : Tr(state.language, L", только спасение данных", L", rescue only"));
    return summary.str();
}

void RefreshLocalizedSummary(AppState& state) {
    if (state.summaryKind == SummaryKind::None || !state.logPath.empty()) return;
    const bool live = state.summaryKind == SummaryKind::LiveScan;
    const std::wstring summary = live ? LiveScanSummary(state) : OfflineScanSummary(state);
    const auto& warnings = live ? state.scan.warnings : state.offline.scan.warnings;
    std::wstring logText = summary;
    if (!warnings.empty() && state.summaryLanguage != state.language) {
        logText += L"\r\n" + Tr(state.language,
            L"Примечание: предупреждения ниже сформированы до переключения языка. Для их перевода повторите сканирование.",
            L"Note: the warnings below were generated before the language switch. Scan again to translate them.");
    }
    for (const auto& warning : warnings) logText += L"\r\n" + warning;
    SetWindowTextW(GetDlgItem(state.window, IDC_LOG), logText.c_str());
    if (live) {
        SetText(state.window, IDC_STATUS,
                Tr(state.language, L"Готово. Изменения не выполнялись.",
                                   L"Ready. No changes were made."));
    } else if (state.offline.valid) {
        SetText(state.window, IDC_STATUS,
                Tr(state.language, L"Офлайн-сканирование завершено. Изменения не выполнялись.",
                                   L"Offline scan completed. No changes were made."));
    } else {
        SetText(state.window, IDC_STATUS, summary);
    }
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

#ifdef CPC_MODERN_UI
constexpr wchar_t kUiSettingsKey[] = L"Software\\CodeAlexandrov\\CryptoProCleanup\\Modern";
#else
constexpr wchar_t kUiSettingsKey[] = L"Software\\CodeAlexandrov\\CryptoProCleanup\\Legacy";
#endif

bool ReadSettingDword(HKEY key, const wchar_t* name, DWORD* value) {
    DWORD type = 0;
    DWORD size = sizeof(*value);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS &&
           type == REG_DWORD && size == sizeof(*value);
}

void LoadUiSettings(AppState& state) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUiSettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
    DWORD value = 0;
    if (ReadSettingDword(key, L"RememberWindow", &value)) state.rememberWindow = value != 0;
    if (ReadSettingDword(key, L"UseSystemColors", &value)) state.useSystemColors = value != 0;
    if (!state.languageExplicit && ReadSettingDword(key, L"Language", &value) && value <= 1)
        state.language = value == 0 ? Language::Russian : Language::English;
    if (state.rememberWindow && ReadSettingDword(key, L"Page", &value) && value <= static_cast<DWORD>(Page::About))
        state.currentPage = static_cast<Page>(value);
    for (const auto& definition : std::array<std::pair<int, int>, 5>{{
             {IDC_PRODUCTS, 4}, {IDC_PROFILES, 2}, {IDC_CERTIFICATES, 6},
             {IDC_OFFLINE_PRODUCTS, 4}, {IDC_OFFLINE_CERTS, 5}}}) {
        std::vector<int> widths;
        for (int column = 0; column < definition.second; ++column) {
            const std::wstring name = L"Column" + std::to_wstring(definition.first) + L"_" + std::to_wstring(column);
            if (!ReadSettingDword(key, name.c_str(), &value) || value < 20 || value > 4000) {
                widths.clear();
                break;
            }
            widths.push_back(static_cast<int>(value));
        }
        if (!widths.empty()) state.savedColumnWeights[definition.first] = std::move(widths);
    }
    DWORD left = 0, top = 0, width = 0, height = 0;
    if (state.rememberWindow && ReadSettingDword(key, L"WindowLeft", &left) &&
        ReadSettingDword(key, L"WindowTop", &top) && ReadSettingDword(key, L"WindowWidth", &width) &&
        ReadSettingDword(key, L"WindowHeight", &height) && width >= 700 && height >= 500 &&
        width < 10000 && height < 10000) {
        state.savedWindow = {static_cast<LONG>(left), static_cast<LONG>(top),
                             static_cast<LONG>(left + width), static_cast<LONG>(top + height)};
        state.hasSavedWindow = true;
    }
    RegCloseKey(key);
}

void WriteSettingDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void SaveUiSettings(const AppState& state) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUiSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    WriteSettingDword(key, L"RememberWindow", state.rememberWindow ? 1 : 0);
    WriteSettingDword(key, L"UseSystemColors", state.useSystemColors ? 1 : 0);
    WriteSettingDword(key, L"Language", state.language == Language::Russian ? 0 : 1);
    if (state.rememberWindow) {
        WriteSettingDword(key, L"Page", static_cast<DWORD>(state.currentPage));
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(state.window, &placement)) {
            const RECT& rectangle = placement.rcNormalPosition;
            WriteSettingDword(key, L"WindowLeft", static_cast<DWORD>(rectangle.left));
            WriteSettingDword(key, L"WindowTop", static_cast<DWORD>(rectangle.top));
            WriteSettingDword(key, L"WindowWidth", static_cast<DWORD>(rectangle.right - rectangle.left));
            WriteSettingDword(key, L"WindowHeight", static_cast<DWORD>(rectangle.bottom - rectangle.top));
        }
    }
    for (const auto& definition : std::array<std::pair<int, int>, 5>{{
             {IDC_PRODUCTS, 4}, {IDC_PROFILES, 2}, {IDC_CERTIFICATES, 6},
             {IDC_OFFLINE_PRODUCTS, 4}, {IDC_OFFLINE_CERTS, 5}}}) {
        HWND list = GetDlgItem(state.window, definition.first);
        if (!list) continue;
        for (int column = 0; column < definition.second; ++column) {
            const int width = ListView_GetColumnWidth(list, column);
            if (width <= 0) continue;
            const std::wstring name = L"Column" + std::to_wstring(definition.first) + L"_" + std::to_wstring(column);
            WriteSettingDword(key, name.c_str(), static_cast<DWORD>(width));
        }
    }
    RegCloseKey(key);
}

void RestoreSavedWindow(AppState& state) {
    if (!state.hasSavedWindow) return;
    RECT rectangle = state.savedWindow;
    HMONITOR monitor = MonitorFromRect(&rectangle, MONITOR_DEFAULTTONULL);
    if (!monitor) return;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return;
    const int width = std::min(std::max(rectangle.right - rectangle.left, state.minimumWindow.cx),
                               static_cast<LONG>(info.rcWork.right - info.rcWork.left));
    const int height = std::min(std::max(rectangle.bottom - rectangle.top, state.minimumWindow.cy),
                                static_cast<LONG>(info.rcWork.bottom - info.rcWork.top));
    const int left = std::clamp(static_cast<int>(rectangle.left), static_cast<int>(info.rcWork.left),
                                static_cast<int>(info.rcWork.right - width));
    const int top = std::clamp(static_cast<int>(rectangle.top), static_cast<int>(info.rcWork.top),
                               static_cast<int>(info.rcWork.bottom - height));
    SetWindowPos(state.window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void SizeDefaultWindow(AppState& state) {
    RECT current{};
    GetWindowRect(state.window, &current);
    HMONITOR monitor = MonitorFromWindow(state.window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return;
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    const int width = std::min(static_cast<int>(current.right - current.left),
                               std::max(static_cast<int>(state.minimumWindow.cx), std::min(1280, workWidth - 32)));
    const int height = std::min(static_cast<int>(current.bottom - current.top),
                                std::max(static_cast<int>(state.minimumWindow.cy), std::min(850, workHeight - 32)));
    const int left = info.rcWork.left + (workWidth - width) / 2;
    const int top = info.rcWork.top + (workHeight - height) / 2;
    SetWindowPos(state.window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ConfigureList(AppState& state, HWND list, const std::vector<std::pair<std::wstring, int>>& columns) {
    ListView_DeleteAllItems(list);
    while (ListView_DeleteColumn(list, 0)) {}
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                           LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    if (state.checkImages) ListView_SetImageList(list, state.checkImages, LVSIL_STATE);
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
    const auto saved = state.savedColumnWeights.find(GetDlgCtrlID(list));
    if (saved != state.savedColumnWeights.end() && saved->second.size() == weights.size())
        weights = saved->second;
    state.listColumnWeights[GetDlgCtrlID(list)] = std::move(weights);
    ResizeListColumns(state, GetDlgCtrlID(list));
}

std::wstring CertificateDateKey(const std::wstring& value) {
    if (value.size() == 10 && value[2] == L'.' && value[5] == L'.')
        return value.substr(6, 4) + value.substr(3, 2) + value.substr(0, 2);
    if (value.size() >= 10 && value[4] == L'-' && value[7] == L'-')
        return value.substr(0, 4) + value.substr(5, 2) + value.substr(8, 2);
    return value;
}

std::wstring TodayDateKey() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t value[9]{};
    swprintf_s(value, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
    return value;
}

bool CertificateMatchesFilter(const AppState& state, const CertificateEntry& certificate) {
    const std::wstring query = ToLower(Trim(GetText(state.window, IDC_CERT_SEARCH)));
    if (!query.empty()) {
        const std::wstring searchable = ToLower(certificate.profileName + L"\n" + certificate.subject + L"\n" +
                                                certificate.issuer + L"\n" + certificate.thumbprint);
        if (searchable.find(query) == std::wstring::npos) return false;
    }
    const int filter = ComboBox_GetCurSel(GetDlgItem(state.window, IDC_CERT_FILTER));
    const bool expired = !certificate.validTo.empty() && CertificateDateKey(certificate.validTo) < TodayDateKey();
    if (filter == 1 && expired) return false;
    if (filter == 2 && !expired) return false;
    if (filter == 3 && !certificate.hasPrivateKeyReference) return false;
    return true;
}

std::wstring ListPrimaryText(const std::wstring& value) {
#ifdef CPC_MODERN_UI
    return L"      " + value;
#else
    return value;
#endif
}

void UpdateCertificateSelectionSummary(AppState& state) {
    const size_t selected = static_cast<size_t>(std::count_if(
        state.scan.certificates.begin(), state.scan.certificates.end(),
        [](const CertificateEntry& certificate) { return certificate.selected; }));
    SetText(state.window, IDC_CERT_SELECTED_COUNT,
            std::to_wstring(selected) + Tr(state.language, L" выбрано", L" selected"));
}

void ShowSelectedCertificateDetails(AppState& state) {
    HWND list = GetDlgItem(state.window, IDC_CERTIFICATES);
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) {
        SetText(state.window, IDC_CERT_DETAILS, Tr(state.language,
            L"Выберите сертификат в таблице, чтобы увидеть владельца, издателя, срок действия и отпечаток.",
            L"Select a certificate in the table to view its subject, issuer, validity and thumbprint."));
        EnableWindow(GetDlgItem(state.window, IDC_COPY_THUMBPRINT), FALSE);
        return;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(list, &item) || item.lParam < 0 ||
        static_cast<size_t>(item.lParam) >= state.scan.certificates.size()) return;
    const auto& certificate = state.scan.certificates[static_cast<size_t>(item.lParam)];
    std::wostringstream details;
    details << Tr(state.language, L"Кому выдан\r\n", L"Issued to\r\n") << certificate.subject
            << Tr(state.language, L"\r\n\r\nКем выдан\r\n", L"\r\n\r\nIssued by\r\n") << certificate.issuer
            << Tr(state.language, L"\r\n\r\nСрок действия\r\n", L"\r\n\r\nValidity\r\n")
            << certificate.validFrom << L" — " << certificate.validTo
            << Tr(state.language, L"\r\n\r\nОтпечаток\r\n", L"\r\n\r\nThumbprint\r\n") << certificate.thumbprint
            << Tr(state.language, L"\r\n\r\nПрофиль: ", L"\r\n\r\nProfile: ") << certificate.profileName;
    SetText(state.window, IDC_CERT_DETAILS, details.str());
    EnableWindow(GetDlgItem(state.window, IDC_COPY_THUMBPRINT), !certificate.thumbprint.empty());
}

void PopulateCertificates(AppState& state) {
    const bool wasRefreshing = state.refreshingUi;
    state.refreshingUi = true;
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
        if (!CertificateMatchesFilter(state, certificate)) continue;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(list);
        std::wstring primaryText = ListPrimaryText(certificate.profileName);
        item.pszText = primaryText.data();
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
    UpdateCertificateSelectionSummary(state);
    ShowSelectedCertificateDetails(state);
    state.refreshingUi = wasRefreshing;
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
        std::wstring primaryText = ListPrimaryText(product.displayName);
        item.pszText = primaryText.data();
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
        std::wstring primaryText = ListPrimaryText(certificate.profileName);
        item.pszText = primaryText.data();
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
    SetText(state.window, IDC_OFFLINE_SUMMARY, OfflineScanSummary(state));
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
        std::wstring primaryText = ListPrimaryText(product.displayName);
        item.pszText = primaryText.data();
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
        std::wstring primaryText = ListPrimaryText(profile.displayName);
        item.pszText = primaryText.data();
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(profiles, &item);
        std::wstring status = profile.loaded ? Tr(state.language, L"Загружен", L"Loaded") : Tr(state.language, L"Неактивен", L"Offline");
        ListView_SetItemText(profiles, item.iItem, 1, status.data());
        ListView_SetCheckState(profiles, item.iItem, profile.selected ? TRUE : FALSE);
    }
    Button_SetCheck(GetDlgItem(state.window, IDC_SELECT_ALL_PROFILES),
                    state.scan.profiles.empty() ? BST_UNCHECKED : BST_CHECKED);
    PopulateCertificates(state);
    SetText(state.window, IDC_STAT_PRODUCTS_VALUE, std::to_wstring(state.scan.products.size()));
    SetText(state.window, IDC_STAT_LICENSES_VALUE, std::to_wstring(state.scan.licenses.size()));
    SetText(state.window, IDC_STAT_CERTS_VALUE, std::to_wstring(state.scan.certificates.size()));
    SetText(state.window, IDC_STAT_PROFILES_VALUE, std::to_wstring(state.scan.profiles.size()));
}

std::wstring DateSortKey(const std::wstring& value) {
    return CertificateDateKey(value);
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

template <size_t Size>
void ShowPageControls(AppState& state, const std::array<int, Size>& controls, bool visible) {
    for (const int id : controls) ShowWindow(GetDlgItem(state.window, id), visible ? SW_SHOW : SW_HIDE);
}

void UpdatePageHeader(AppState& state) {
    std::wstring eyebrow;
    std::wstring title;
    std::wstring subtitle;
    switch (state.currentPage) {
        case Page::Overview:
            eyebrow = Tr(state.language, L"СИСТЕМА / ОБЗОР", L"SYSTEM / OVERVIEW");
            title = Tr(state.language, L"Обзор", L"Overview");
            subtitle = Tr(state.language, L"Продукты CryptoPro, лицензии и защищённые данные",
                          L"CryptoPro products, licenses and protected data");
            break;
        case Page::Certificates:
            eyebrow = Tr(state.language, L"ДАННЫЕ / СЕРТИФИКАТЫ", L"DATA / CERTIFICATES");
            title = Tr(state.language, L"Открытые сертификаты", L"Public certificates");
            subtitle = Tr(state.language, L"Просмотр и экспорт только открытой части",
                          L"Review and export public data only");
            break;
        case Page::Offline:
            eyebrow = Tr(state.language, L"ВОССТАНОВЛЕНИЕ / ДРУГАЯ WINDOWS", L"RECOVERY / OFFLINE WINDOWS");
            title = Tr(state.language, L"Отключённая Windows", L"Disconnected Windows");
            subtitle = Tr(state.language, L"Спасение лицензий и сертификатов с другого диска",
                          L"Rescue licenses and certificates from another drive");
            break;
        case Page::Log:
            eyebrow = Tr(state.language, L"СЕАНС / ЖУРНАЛ", L"SESSION / LOG");
            title = Tr(state.language, L"Журнал и отчёты", L"Log & reports");
            subtitle = Tr(state.language, L"Ход операций без серийных номеров и содержимого сертификатов",
                          L"Operation history without serial numbers or certificate contents");
            break;
        case Page::Settings:
            eyebrow = Tr(state.language, L"ПРИЛОЖЕНИЕ / НАСТРОЙКИ", L"APPLICATION / SETTINGS");
            title = Tr(state.language, L"Настройки", L"Settings");
            subtitle = Tr(state.language, L"Язык, оформление и сохранение положения окна",
                          L"Language, appearance and window persistence");
            break;
        case Page::About:
            eyebrow = Tr(state.language, L"ПРОЕКТ / СВЕДЕНИЯ", L"PROJECT / ABOUT");
            title = Tr(state.language, L"О программе", L"About");
            subtitle = Tr(state.language, L"Открытый код, контакты и правовая информация",
                          L"Open source, contacts and legal information");
            break;
    }
    SetText(state.window, IDC_PAGE_EYEBROW, eyebrow);
    SetText(state.window, IDC_TITLE, title);
    SetText(state.window, IDC_DISCLAIMER, subtitle);
}

void UpdatePageVisibility(AppState& state) {
    const std::array<int, 22> overviewControls{
        IDC_STAT_PRODUCTS_CAPTION, IDC_STAT_PRODUCTS_VALUE, IDC_STAT_LICENSES_CAPTION, IDC_STAT_LICENSES_VALUE,
        IDC_STAT_CERTS_CAPTION, IDC_STAT_CERTS_VALUE, IDC_STAT_PROFILES_CAPTION, IDC_STAT_PROFILES_VALUE,
        IDC_PRODUCTS_LABEL, IDC_PRODUCTS, IDC_PLAN_LABEL, IDC_PLAN_SUMMARY, IDC_CHECK_PLAN, IDC_PROTECTED_SUMMARY,
        IDC_PROFILES_LABEL, IDC_PROFILES, IDC_SELECT_ALL_PROFILES, IDC_BACKUP_LABEL, IDC_BACKUP_PATH,
        IDC_BROWSE, IDC_BACKUP_INFO, IDC_SHOW_LICENSES
    };
    ShowPageControls(state, overviewControls, state.currentPage == Page::Overview);
    ShowWindow(GetDlgItem(state.window, IDC_PROTECTED_SUMMARY),
               state.currentPage == Page::Overview && !state.compactLayout ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state.window, IDC_SCAN), state.currentPage == Page::Overview ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state.window, IDC_CLEAN), state.currentPage == Page::Overview ? SW_SHOW : SW_HIDE);

    const std::array<int, 10> certificateControls{
        IDC_CERT_INFO, IDC_CERT_SEARCH, IDC_CERT_FILTER, IDC_CERTIFICATES, IDC_CERT_DETAILS_TITLE,
        IDC_CERT_DETAILS, IDC_COPY_THUMBPRINT, IDC_SELECT_ALL_CERTS, IDC_CERT_SELECTED_COUNT, IDC_EXPORT_CERTS
    };
    ShowPageControls(state, certificateControls, state.currentPage == Page::Certificates);

    const std::array<int, 18> offlineControls{
        IDC_OFFLINE_INFO, IDC_OFFLINE_STEP1, IDC_OFFLINE_STEP2, IDC_OFFLINE_STEP3, IDC_OFFLINE_PATH_LABEL,
        IDC_OFFLINE_PATH, IDC_OFFLINE_BROWSE, IDC_OFFLINE_SCAN, IDC_OFFLINE_SUMMARY,
        IDC_OFFLINE_PRODUCTS_LABEL, IDC_OFFLINE_PRODUCTS, IDC_OFFLINE_CERTS_LABEL, IDC_OFFLINE_CERTS,
        IDC_OFFLINE_SELECT_ALL_CERTS, IDC_OFFLINE_SHOW_LICENSES, IDC_OFFLINE_DIAGNOSTICS,
        IDC_OFFLINE_SAVE, IDC_OFFLINE_CLEAN
    };
    ShowPageControls(state, offlineControls, state.currentPage == Page::Offline);
    for (const int id : {IDC_OFFLINE_STEP1, IDC_OFFLINE_STEP2, IDC_OFFLINE_STEP3})
        ShowWindow(GetDlgItem(state.window, id),
                   state.currentPage == Page::Offline && !state.compactLayout ? SW_SHOW : SW_HIDE);

    const std::array<int, 5> logControls{IDC_LOG_INFO, IDC_LOG, IDC_COPY_LOG, IDC_OPEN_REPORT_FOLDER, IDC_CLEAR_LOG};
    ShowPageControls(state, logControls, state.currentPage == Page::Log);
    const std::array<int, 8> settingsControls{
        IDC_SETTINGS_THEME_LABEL, IDC_SETTINGS_THEME, IDC_SETTINGS_LANGUAGE_LABEL, IDC_SETTINGS_LANGUAGE,
        IDC_SETTINGS_REMEMBER_WINDOW, IDC_SETTINGS_HIGH_CONTRAST, IDC_SETTINGS_RESET, IDC_SETTINGS_INFO
    };
    ShowPageControls(state, settingsControls, state.currentPage == Page::Settings);
    const std::array<int, 7> aboutControls{
        IDC_ABOUT_TITLE, IDC_ABOUT_TEXT, IDC_ABOUT_COPY_VERSION, IDC_ABOUT_GITHUB,
        IDC_ABOUT_WEBSITE, IDC_ABOUT_SUPPORT, IDC_LINK_GITHUB
    };
    ShowPageControls(state, aboutControls, state.currentPage == Page::About);
    UpdatePageHeader(state);
    for (const int id : {IDC_NAV_OVERVIEW, IDC_NAV_CERTIFICATES, IDC_NAV_OFFLINE,
                         IDC_NAV_LOG, IDC_NAV_SETTINGS, IDC_NAV_ABOUT})
        InvalidateRect(GetDlgItem(state.window, id), nullptr, TRUE);
    if (state.initialClient.cx && state.initialClient.cy) {
        RECT client{};
        GetClientRect(state.window, &client);
        LayoutMainDialog(state, client.right - client.left, client.bottom - client.top);
    }
}

void SwitchPage(AppState& state, Page page) {
    state.currentPage = page;
    UpdatePageVisibility(state);
    HWND first = nullptr;
    switch (page) {
        case Page::Overview: first = GetDlgItem(state.window, IDC_PRODUCTS); break;
        case Page::Certificates: first = GetDlgItem(state.window, IDC_CERT_SEARCH); break;
        case Page::Offline: first = GetDlgItem(state.window, IDC_OFFLINE_PATH); break;
        case Page::Log: first = GetDlgItem(state.window, IDC_LOG); break;
        case Page::Settings: first = GetDlgItem(state.window, IDC_SETTINGS_THEME); break;
        case Page::About: first = GetDlgItem(state.window, IDC_ABOUT_GITHUB); break;
    }
    if (first && IsWindowVisible(first)) SetFocus(first);
}

void ReadSelections(AppState& state);
void ReadOfflineSelections(AppState& state);

void ApplyLanguage(AppState& state) {
    SendMessageW(state.window, WM_SETREDRAW, FALSE, 0);
    const bool ru = state.language == Language::Russian;
    const std::wstring applicationName = ru ? L"КриптоПро Очистка" : L"CryptoPro Cleanup Utility";
#ifdef CPC_MODERN_UI
    const std::wstring versionedName = applicationName + L" " + kVersion + L" — x64 Modern";
#else
    const std::wstring versionedName = applicationName + L" " + kVersion + L" — Legacy";
#endif
    SetWindowTextW(state.window, versionedName.c_str());
    SetText(state.window, IDC_BRAND, ru ? L"КРИПТО ПРО" : L"CRYPTO PRO");
#ifdef CPC_MODERN_UI
    SetText(state.window, IDC_BRAND_VERSION, L"MODERN x64 / 0.5");
#else
    SetText(state.window, IDC_BRAND_VERSION, L"LEGACY / 0.5");
#endif
    SetText(state.window, IDC_NAV_OVERVIEW, Tr(state.language, L"Обзор", L"Overview"));
    SetText(state.window, IDC_NAV_CERTIFICATES, Tr(state.language, L"Сертификаты", L"Certificates"));
    SetText(state.window, IDC_NAV_OFFLINE, Tr(state.language, L"Другая Windows", L"Offline Windows"));
    SetText(state.window, IDC_NAV_LOG, Tr(state.language, L"Журнал и отчёты", L"Log & reports"));
    SetText(state.window, IDC_NAV_SETTINGS, Tr(state.language, L"Настройки", L"Settings"));
    SetText(state.window, IDC_NAV_ABOUT, Tr(state.language, L"О программе", L"About"));
    SetText(state.window, IDC_SAFETY_STATE, Tr(state.language,
        L"● ЗАЩИЩЁННЫЙ РЕЖИМ", L"● PROTECTED MODE"));
    SetText(state.window, IDC_HEADER_STATUS, Tr(state.language, L"ЗАЩИЩЕНО", L"PROTECTED"));

    SetText(state.window, IDC_STAT_PRODUCTS_CAPTION, Tr(state.language, L"Продукты", L"Products"));
    SetText(state.window, IDC_STAT_LICENSES_CAPTION, Tr(state.language, L"Лицензии", L"Licenses"));
    SetText(state.window, IDC_STAT_CERTS_CAPTION, Tr(state.language, L"Сертификаты", L"Certificates"));
    SetText(state.window, IDC_STAT_PROFILES_CAPTION, Tr(state.language, L"Профили", L"Profiles"));
    SetText(state.window, IDC_STAT_PRODUCTS_VALUE, std::to_wstring(state.scan.products.size()));
    SetText(state.window, IDC_STAT_LICENSES_VALUE, std::to_wstring(state.scan.licenses.size()));
    SetText(state.window, IDC_STAT_CERTS_VALUE, std::to_wstring(state.scan.certificates.size()));
    SetText(state.window, IDC_STAT_PROFILES_VALUE, std::to_wstring(state.scan.profiles.size()));
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
    SetText(state.window, IDC_PLAN_LABEL, Tr(state.language, L"Проверяемый план", L"Verified plan"));
    SetText(state.window, IDC_CHECK_PLAN, Tr(state.language, L"Проверить план", L"Check plan"));
    if (state.plan.targets.empty()) {
        SetText(state.window, IDC_PLAN_SUMMARY, Tr(state.language,
            L"Постройте безопасный план только для выбранных продуктов и профилей.",
            L"Build a safe plan for the selected products and profiles."));
    } else {
        SetText(state.window, IDC_PLAN_SUMMARY,
            Tr(state.language, L"Подтверждённых целей: ", L"Verified targets: ") +
            std::to_wstring(state.plan.targets.size()) + L"\r\n" +
            Tr(state.language, L"Защищённых элементов: ", L"Protected items: ") +
            std::to_wstring(state.plan.protectedItems.size()));
    }
    SetText(state.window, IDC_PROTECTED_SUMMARY, Tr(state.language,
        L"Сертификаты, токены и контейнеры закрытых ключей никогда не удаляются.",
        L"Certificates, tokens and private-key containers are never removed."));
    SetText(state.window, IDC_CERT_INFO, Tr(state.language,
        L"Личное хранилище каждого обычного профиля. Экспортируется только открытая часть; закрытые ключи и токены не копируются.",
        L"Personal store of each regular profile. Only public data is exported; private keys and tokens are not copied."));
    SendDlgItemMessageW(state.window, IDC_CERT_SEARCH, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(Tr(state.language, L"Поиск по владельцу, издателю или отпечатку…",
                                                   L"Search subject, issuer or thumbprint…").c_str()));
    const int certificateFilter = std::max(0, ComboBox_GetCurSel(GetDlgItem(state.window, IDC_CERT_FILTER)));
    ComboBox_ResetContent(GetDlgItem(state.window, IDC_CERT_FILTER));
    for (const auto& item : {
        Tr(state.language, L"Все сертификаты", L"All certificates"),
        Tr(state.language, L"Действующие", L"Currently valid"),
        Tr(state.language, L"Истёкшие", L"Expired"),
        Tr(state.language, L"Со ссылкой на ключ", L"Private-key reference")})
        ComboBox_AddString(GetDlgItem(state.window, IDC_CERT_FILTER), item.c_str());
    ComboBox_SetCurSel(GetDlgItem(state.window, IDC_CERT_FILTER), std::min(certificateFilter, 3));
    SetText(state.window, IDC_CERT_DETAILS_TITLE, Tr(state.language, L"Сведения о сертификате", L"Certificate details"));
    SetText(state.window, IDC_COPY_THUMBPRINT, Tr(state.language, L"Копировать отпечаток", L"Copy thumbprint"));
    SetText(state.window, IDC_SELECT_ALL_CERTS, Tr(state.language, L"Выбрать все сертификаты", L"Select all certificates"));
    SetText(state.window, IDC_EXPORT_CERTS, Tr(state.language, L"Сохранить выбранные открытые сертификаты...", L"Export selected public certificates..."));
    SetText(state.window, IDC_OFFLINE_INFO, Tr(state.language,
        L"Выберите диск с отключённой Windows (например, E:\\) или саму папку Windows (E:\\Windows). Сканирование выполняется только для чтения; на медленном HDD/USB оно может занять несколько минут.",
        L"Choose the drive containing disconnected Windows (for example, E:\\) or its Windows folder (E:\\Windows). The read-only scan may take several minutes on a slow HDD/USB drive."));
    SetText(state.window, IDC_OFFLINE_PATH_LABEL, Tr(state.language, L"Диск / Windows", L"Drive / Windows"));
    SetText(state.window, IDC_OFFLINE_BROWSE, Tr(state.language, L"Обзор...", L"Browse..."));
    SetText(state.window, IDC_OFFLINE_SCAN, Tr(state.language, L"Сканировать", L"Scan"));
    SetText(state.window, IDC_OFFLINE_STEP1, Tr(state.language, L"1  Выбрать диск", L"1  Select disk"));
    SetText(state.window, IDC_OFFLINE_STEP2, Tr(state.language, L"2  Сканировать", L"2  Read-only scan"));
    SetText(state.window, IDC_OFFLINE_STEP3, Tr(state.language, L"3  Сохранить", L"3  Rescue or clean"));
    SetText(state.window, IDC_OFFLINE_PRODUCTS_LABEL, Tr(state.language, L"Продукты в отключённой Windows", L"Products in disconnected Windows"));
    SetText(state.window, IDC_OFFLINE_CERTS_LABEL, Tr(state.language, L"Открытые сертификаты профилей и компьютера", L"Public certificates in profiles and local machine"));
    SetText(state.window, IDC_OFFLINE_SELECT_ALL_CERTS, Tr(state.language, L"Выбрать все сертификаты", L"Select all certificates"));
    SetText(state.window, IDC_OFFLINE_SHOW_LICENSES, Tr(state.language, L"Показать / копировать лицензии", L"Show / copy licenses"));
    SetText(state.window, IDC_OFFLINE_DIAGNOSTICS, Tr(state.language, L"Диагностика...", L"Diagnostics..."));
    SetText(state.window, IDC_OFFLINE_SAVE, Tr(state.language, L"Сохранить найденные данные...", L"Save rescued data..."));
    SetText(state.window, IDC_OFFLINE_CLEAN, Tr(state.language, L"Расширенная офлайн-очистка...", L"Advanced offline cleanup..."));
    SetText(state.window, IDC_OFFLINE_SUMMARY, state.offline.valid ? OfflineScanSummary(state) :
        Tr(state.language, L"Отключённая Windows ещё не сканировалась.", L"No disconnected Windows installation scanned yet."));

    SetText(state.window, IDC_LOG_INFO, Tr(state.language,
        L"Журнал не содержит полных лицензий, закрытых ключей или содержимого сертификатов.",
        L"The log never contains full licenses, private keys or certificate contents."));
    SetText(state.window, IDC_COPY_LOG, Tr(state.language, L"Копировать журнал", L"Copy log"));
    SetText(state.window, IDC_OPEN_REPORT_FOLDER, Tr(state.language, L"Открыть папку отчёта", L"Open report folder"));
    SetText(state.window, IDC_CLEAR_LOG, Tr(state.language, L"Очистить экран", L"Clear view"));

    SetText(state.window, IDC_SETTINGS_THEME_LABEL, Tr(state.language, L"Оформление", L"Theme"));
    SetText(state.window, IDC_SETTINGS_LANGUAGE_LABEL, Tr(state.language, L"Язык интерфейса", L"Interface language"));
    ComboBox_ResetContent(GetDlgItem(state.window, IDC_SETTINGS_THEME));
    ComboBox_AddString(GetDlgItem(state.window, IDC_SETTINGS_THEME), Tr(state.language, L"Тёмная тема", L"Dark theme").c_str());
    ComboBox_AddString(GetDlgItem(state.window, IDC_SETTINGS_THEME), Tr(state.language, L"Системные цвета", L"System colors").c_str());
    ComboBox_SetCurSel(GetDlgItem(state.window, IDC_SETTINGS_THEME), state.useSystemColors ? 1 : 0);
    ComboBox_ResetContent(GetDlgItem(state.window, IDC_SETTINGS_LANGUAGE));
    ComboBox_AddString(GetDlgItem(state.window, IDC_SETTINGS_LANGUAGE), L"Русский");
    ComboBox_AddString(GetDlgItem(state.window, IDC_SETTINGS_LANGUAGE), L"English");
    ComboBox_SetCurSel(GetDlgItem(state.window, IDC_SETTINGS_LANGUAGE), ru ? 0 : 1);
    Button_SetCheck(GetDlgItem(state.window, IDC_SETTINGS_REMEMBER_WINDOW), state.rememberWindow ? BST_CHECKED : BST_UNCHECKED);
    SetText(state.window, IDC_SETTINGS_REMEMBER_WINDOW, Tr(state.language,
        L"Запоминать размер окна и последнюю страницу", L"Remember window size and last page"));
    SetText(state.window, IDC_SETTINGS_HIGH_CONTRAST, state.highContrast ? Tr(state.language,
        L"В Windows включена высокая контрастность — используются системные цвета.",
        L"Windows high contrast is active — system colors are used.") : Tr(state.language,
        L"Высокая контрастность Windows определяется автоматически.",
        L"Windows high contrast is detected automatically."));
    SetText(state.window, IDC_SETTINGS_RESET, Tr(state.language, L"Сбросить настройки интерфейса", L"Reset interface settings"));
    SetText(state.window, IDC_SETTINGS_INFO, Tr(state.language,
        L"Настройки хранятся только для текущего пользователя Windows. В них нет лицензий, сертификатов или целей очистки.",
        L"Settings are stored only for the current Windows user. They contain no licenses, certificates or cleanup targets."));

    SetText(state.window, IDC_ABOUT_TITLE, versionedName);
    SetText(state.window, IDC_ABOUT_TEXT, Tr(state.language,
        L"Неофициальная portable-утилита с открытым исходным кодом для контролируемого удаления продуктов CryptoPro и спасения открытых данных.\r\n\r\nАвтор: Кирилл Александров. Лицензия MIT. Проект не связан с ООО «КРИПТО-ПРО».\r\n\r\nРезервная копия включает licenses.txt с полными лицензиями, выбранные .cer/.p7b, summary.txt, report.json и cleanup.log. Закрытые ключи не экспортируются.",
        L"Unofficial portable open-source utility for controlled CryptoPro product removal and public-data rescue.\r\n\r\nAuthor: Kirill Alexandrov. MIT License. This project is not affiliated with Crypto-Pro LLC.\r\n\r\nA backup contains licenses.txt with full licenses, selected .cer/.p7b files, summary.txt, report.json and cleanup.log. Private keys are not exported."));
    SetText(state.window, IDC_ABOUT_COPY_VERSION, Tr(state.language, L"Копировать сведения о версии", L"Copy version information"));
    SetText(state.window, IDC_ABOUT_GITHUB, L"<a href=\"https://github.com/acidtmn/CryptoProCleanup\">GitHub</a>");
    SetText(state.window, IDC_ABOUT_WEBSITE, L"<a href=\"https://kodalexandrova.ru\">kodalexandrova.ru</a>");
    SetText(state.window, IDC_ABOUT_SUPPORT, Tr(state.language,
        L"<a href=\"https://yoomoney.ru/to/4100119195083142\">Поддержать проект</a>",
        L"<a href=\"https://yoomoney.ru/to/4100119195083142\">Support the project</a>"));
    if (!state.scan.products.empty() || !state.scan.profiles.empty() || !state.scan.certificates.empty()) PopulateLists(state);
    if (state.offline.valid) PopulateOfflineLists(state);
    UpdatePageVisibility(state);
    RefreshLocalizedSummary(state);
    SendMessageW(state.window, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(state.window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SetBusy(AppState& state, bool busy) {
    state.busy = busy;
    for (const int id : {IDC_NAV_OVERVIEW, IDC_NAV_CERTIFICATES, IDC_NAV_OFFLINE, IDC_NAV_LOG,
                         IDC_NAV_SETTINGS, IDC_NAV_ABOUT, IDC_PRODUCTS, IDC_PROFILES, IDC_SELECT_ALL_PROFILES, IDC_BACKUP_PATH,
                         IDC_BROWSE, IDC_LANGUAGE, IDC_SCAN, IDC_CLEAN, IDC_SHOW_LICENSES,
                         IDC_CHECK_PLAN, IDC_CERT_SEARCH, IDC_CERT_FILTER, IDC_CERTIFICATES,
                         IDC_SELECT_ALL_CERTS, IDC_COPY_THUMBPRINT, IDC_EXPORT_CERTS,
                         IDC_OFFLINE_PATH, IDC_OFFLINE_BROWSE, IDC_OFFLINE_SCAN, IDC_OFFLINE_PRODUCTS,
                         IDC_OFFLINE_CERTS, IDC_OFFLINE_SELECT_ALL_CERTS, IDC_OFFLINE_SHOW_LICENSES, IDC_OFFLINE_DIAGNOSTICS,
                         IDC_OFFLINE_SAVE, IDC_OFFLINE_CLEAN, IDC_SETTINGS_THEME, IDC_SETTINGS_LANGUAGE,
                         IDC_SETTINGS_REMEMBER_WINDOW, IDC_SETTINGS_RESET}) EnableWindow(GetDlgItem(state.window, id), !busy);
    SetText(state.window, IDC_HEADER_STATUS, busy ? Tr(state.language, L"ВЫПОЛНЯЕТСЯ", L"WORKING") :
            Tr(state.language, L"ЗАЩИЩЕНО", L"PROTECTED"));
    if (!busy) {
        EnableWindow(GetDlgItem(state.window, IDC_SHOW_LICENSES), !state.scan.licenses.empty());
        EnableWindow(GetDlgItem(state.window, IDC_EXPORT_CERTS), !state.scan.certificates.empty());
        EnableWindow(GetDlgItem(state.window, IDC_CLEAN), !state.scan.products.empty());
        EnableWindow(GetDlgItem(state.window, IDC_OFFLINE_SHOW_LICENSES), state.offline.valid && !state.offline.scan.licenses.empty());
        EnableWindow(GetDlgItem(state.window, IDC_OFFLINE_DIAGNOSTICS), !state.offline.diagnostics.empty());
        EnableWindow(GetDlgItem(state.window, IDC_OFFLINE_SAVE), state.offline.valid);
        EnableWindow(GetDlgItem(state.window, IDC_OFFLINE_CLEAN), state.offline.cleanupCapable && !state.offline.scan.products.empty());
    }
}

void SetOfflineScanAnimation(AppState& state, bool scanning) {
    HWND progress = GetDlgItem(state.window, IDC_PROGRESS);
    if (!progress) return;
    LONG_PTR style = GetWindowLongPtrW(progress, GWL_STYLE);
    if (scanning) {
        SetWindowLongPtrW(progress, GWL_STYLE, style | PBS_MARQUEE);
        SetWindowPos(progress, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(progress, PBM_SETMARQUEE, TRUE, 32);
        SetText(state.window, IDC_OFFLINE_SCAN,
                Tr(state.language, L"Сканирование…", L"Scanning…"));
    } else {
        SendMessageW(progress, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(progress, GWL_STYLE, style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
        SetWindowPos(progress, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SetText(state.window, IDC_OFFLINE_SCAN,
                Tr(state.language, L"Сканировать", L"Scan"));
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

std::wstring TargetTypeText(Language language, TargetType type) {
    switch (type) {
        case TargetType::File: return Tr(language, L"Файл", L"File");
        case TargetType::Directory: return Tr(language, L"Каталог", L"Directory");
        case TargetType::RegistryTree: return Tr(language, L"Реестр", L"Registry");
        case TargetType::Service: return Tr(language, L"Служба", L"Service");
        case TargetType::DriverService: return Tr(language, L"Драйвер", L"Driver service");
        case TargetType::DriverPackage: return Tr(language, L"Пакет драйвера", L"Driver package");
        case TargetType::ScheduledTask: return Tr(language, L"Задача", L"Scheduled task");
        case TargetType::Shortcut: return Tr(language, L"Ярлык", L"Shortcut");
    }
    return {};
}

std::wstring CleanupTargetLocation(const CleanupTarget& target) {
    if (!target.path.empty()) return target.path;
    const wchar_t* hive = target.registry.hive == RegistryHive::LocalMachine ? L"HKLM\\" :
                          target.registry.hive == RegistryHive::CurrentUser ? L"HKCU\\" : L"HKU\\";
    return std::wstring(hive) + target.registry.subkey;
}

void InsertPlanRow(HWND list, int row, const std::wstring& type, const std::wstring& name,
                   const std::wstring& location, const std::wstring& status) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(type.c_str());
    ListView_InsertItem(list, &item);
    ListView_SetItemText(list, row, 1, const_cast<wchar_t*>(name.c_str()));
    ListView_SetItemText(list, row, 2, const_cast<wchar_t*>(location.c_str()));
    ListView_SetItemText(list, row, 3, const_cast<wchar_t*>(status.c_str()));
}

bool PlanTextMatches(const std::wstring& query, const std::wstring& first,
                     const std::wstring& second, const std::wstring& third) {
    return query.empty() || ToLower(first + L"\n" + second + L"\n" + third).find(query) != std::wstring::npos;
}

void PopulatePlanDialog(HWND dialog, PlanDialogState& state) {
    HWND list = GetDlgItem(dialog, IDC_PLAN_LIST);
    ListView_DeleteAllItems(list);
    const std::wstring query = ToLower(Trim(GetText(dialog, IDC_PLAN_SEARCH)));
    const int filter = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_PLAN_FILTER));
    int row = 0;
    if (state.plan && (filter <= 0 || filter == 1)) {
        for (const auto& product : state.plan->products) {
            const std::wstring type = product.msi ? L"MSI" : L"EXE";
            const std::wstring location = product.msi ? product.productCode : product.uninstallString;
            if (!PlanTextMatches(query, product.displayName, location, type)) continue;
            InsertPlanRow(list, row++, Tr(state.language, L"Деинсталлятор ", L"Uninstaller ") + type,
                          product.displayName, location,
                          Tr(state.language, L"Сначала", L"Runs first"));
        }
    }
    if (state.plan) {
        for (const auto& target : state.plan->targets) {
            int category = 2;
            if (target.type == TargetType::RegistryTree) category = 3;
            else if (target.type == TargetType::Service || target.type == TargetType::DriverService ||
                     target.type == TargetType::DriverPackage || target.type == TargetType::ScheduledTask) category = 4;
            if (filter > 0 && filter != category) continue;
            const std::wstring type = TargetTypeText(state.language, target.type);
            const std::wstring location = CleanupTargetLocation(target);
            if (!PlanTextMatches(query, target.displayName, location, target.reason)) continue;
            InsertPlanRow(list, row++, type, target.displayName, location,
                          target.protectedItem ? Tr(state.language, L"Защищено", L"Protected") :
                          target.verified ? Tr(state.language, L"Подтверждено", L"Verified") :
                          Tr(state.language, L"Пропустить", L"Skip"));
        }
        if (filter <= 0 || filter == 5) {
            for (const auto& item : state.plan->protectedItems) {
                if (!PlanTextMatches(query, item, L"", L"")) continue;
                InsertPlanRow(list, row++, Tr(state.language, L"Защищённый элемент", L"Protected item"),
                              item, L"", Tr(state.language, L"Не удаляется", L"Never removed"));
            }
        }
    }
}

std::wstring PlanAsText(const PlanDialogState& state) {
    std::wostringstream text;
    text << L"CryptoPro Cleanup Utility " << kVersion << L"\r\n"
         << Tr(state.language, L"Проверяемый план (только чтение)", L"Verified plan (read-only)") << L"\r\n\r\n";
    if (!state.plan) return text.str();
    for (const auto& product : state.plan->products)
        text << L"[UNINSTALL] " << product.displayName << L" | "
             << (product.msi ? product.productCode : product.uninstallString) << L"\r\n";
    for (const auto& target : state.plan->targets)
        text << L"[" << TargetTypeText(state.language, target.type) << L"] " << target.displayName
             << L" | " << CleanupTargetLocation(target) << L" | " << target.reason << L"\r\n";
    for (const auto& item : state.plan->protectedItems)
        text << L"[PROTECTED] " << item << L"\r\n";
    return text.str();
}

INT_PTR CALLBACK PlanDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PlanDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<PlanDialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        state->backgroundBrush = CreateSolidBrush(kBackground);
        state->editBrush = CreateSolidBrush(kSurfaceSecondary);
        SetWindowTextW(dialog, Tr(state->language, L"Проверяемый план очистки", L"Verified cleanup plan").c_str());
        SetText(dialog, IDC_PLAN_INFO, Tr(state->language,
            L"Предварительный просмотр только для чтения. Защищённые и неизвестные элементы автоматически не удаляются.",
            L"Read-only preview. Protected and unknown items are never removed automatically."));
        SendDlgItemMessageW(dialog, IDC_PLAN_SEARCH, EM_SETCUEBANNER, TRUE,
                            reinterpret_cast<LPARAM>(Tr(state->language, L"Поиск в плане…", L"Search plan…").c_str()));
        HWND filter = GetDlgItem(dialog, IDC_PLAN_FILTER);
        for (const auto& item : {
            Tr(state->language, L"Все категории", L"All categories"),
            Tr(state->language, L"Деинсталляторы", L"Uninstallers"),
            Tr(state->language, L"Файлы и каталоги", L"Files and directories"),
            Tr(state->language, L"Реестр", L"Registry"),
            Tr(state->language, L"Службы и драйверы", L"Services and drivers"),
            Tr(state->language, L"Защищённые", L"Protected")}) ComboBox_AddString(filter, item.c_str());
        ComboBox_SetCurSel(filter, 0);
        HWND list = GetDlgItem(dialog, IDC_PLAN_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const std::array<std::pair<std::wstring, int>, 4> columns{{
            {Tr(state->language, L"Тип", L"Type"), 120},
            {Tr(state->language, L"Название", L"Name"), 190},
            {Tr(state->language, L"Путь / ключ", L"Path / key"), 360},
            {Tr(state->language, L"Статус", L"Status"), 105}}};
        for (size_t index = 0; index < columns.size(); ++index) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(columns[index].first.c_str());
            column.cx = columns[index].second;
            column.iSubItem = static_cast<int>(index);
            ListView_InsertColumn(list, static_cast<int>(index), &column);
        }
        ListView_SetBkColor(list, kSurface);
        ListView_SetTextBkColor(list, kSurface);
        ListView_SetTextColor(list, kText);
        SetText(dialog, IDC_PLAN_COPY, Tr(state->language, L"Копировать план", L"Copy plan"));
        SetText(dialog, IDOK, Tr(state->language, L"Закрыть", L"Close"));
        PopulatePlanDialog(dialog, *state);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_COMMAND) {
        if ((LOWORD(wParam) == IDC_PLAN_SEARCH && HIWORD(wParam) == EN_CHANGE) ||
            (LOWORD(wParam) == IDC_PLAN_FILTER && HIWORD(wParam) == CBN_SELCHANGE)) {
            PopulatePlanDialog(dialog, *state);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_PLAN_COPY) {
            CopyUnicodeText(dialog, PlanAsText(*state));
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, 0); return TRUE; }
    }
    if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    if (message == WM_CTLCOLORSTATIC) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, kText);
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    }
    if (message == WM_CTLCOLOREDIT) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkColor(device, kSurfaceSecondary);
        SetTextColor(device, kText);
        return reinterpret_cast<INT_PTR>(state->editBrush);
    }
    if (message == WM_CLOSE) { EndDialog(dialog, 0); return TRUE; }
    if (message == WM_DESTROY) {
        if (state->backgroundBrush) DeleteObject(state->backgroundBrush);
        if (state->editBrush) DeleteObject(state->editBrush);
        state->backgroundBrush = state->editBrush = nullptr;
        return TRUE;
    }
    return FALSE;
}

void ShowPlanInspector(AppState& state) {
    ReadSelections(state);
    if (state.scan.products.empty()) {
        MessageBoxW(state.window, Tr(state.language, L"Продукты CryptoPro не найдены.", L"No CryptoPro products were found.").c_str(),
                    L"CryptoPro Cleanup Utility", MB_OK | MB_ICONINFORMATION);
        return;
    }
    SetBusy(state, true);
    state.plan = BuildCleanupPlan(state.scan, [&](const std::wstring& message, int percent) {
        UpdateProgress(state, message, percent);
    });
    SetBusy(state, false);
    SetText(state.window, IDC_PLAN_SUMMARY,
        Tr(state.language, L"Подтверждённых целей: ", L"Verified targets: ") +
        std::to_wstring(state.plan.targets.size()) + L"\r\n" +
        Tr(state.language, L"Защищённых элементов: ", L"Protected items: ") +
        std::to_wstring(state.plan.protectedItems.size()));
    PlanDialogState dialogState{state.language, &state.plan};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PLAN), state.window,
                    PlanDialogProc, reinterpret_cast<LPARAM>(&dialogState));
}

INT_PTR CALLBACK LicensesDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<LicenseDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<LicenseDialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, (state->title.empty() ?
            Tr(state->language, L"Лицензии CryptoPro", L"CryptoPro licenses") : state->title).c_str());
        SetText(dialog, IDC_LICENSES_WARNING, state->warning.empty() ? Tr(state->language,
            L"Полные номера являются конфиденциальными. Перед переустановкой Windows сохраните licenses.txt на внешнем диске или в защищённом облаке.",
            L"Full identifiers are confidential. Before reinstalling Windows, save licenses.txt to external storage or protected cloud storage.") : state->warning);
        SetText(dialog, IDC_LICENSES_TEXT, state->text);
        SetText(dialog, IDC_COPY_LICENSES, Tr(state->language, L"Копировать всё", L"Copy all"));
        SetText(dialog, IDOK, Tr(state->language, L"Закрыть", L"Close"));
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDC_COPY_LICENSES && state) {
        const bool copied = CopyUnicodeText(dialog, state->text);
        MessageBoxW(dialog,
            (copied && !state->copiedMessage.empty() ? state->copiedMessage :
             Tr(state->language, copied ? L"Текст скопирован в буфер обмена." : L"Не удалось открыть буфер обмена.",
                                copied ? L"Text copied to the clipboard." : L"Could not open the clipboard.")).c_str(),
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

void ShowOfflineDiagnostics(AppState& state) {
    std::wostringstream text;
    text << L"CryptoPro Cleanup Utility " << kVersion << L"\r\n"
         << L"Offline scan diagnostics / Диагностика офлайн-сканирования\r\n\r\n";
    for (const auto& line : state.offline.diagnostics) text << line << L"\r\n";
    if (!state.offline.scan.warnings.empty()) {
        text << L"\r\nWarnings / Предупреждения:\r\n";
        for (const auto& warning : state.offline.scan.warnings) text << L"- " << warning << L"\r\n";
    }
    LicenseDialogState dialogState;
    dialogState.language = state.language;
    dialogState.text = text.str();
    dialogState.title = Tr(state.language, L"Диагностика офлайн-сканирования", L"Offline scan diagnostics");
    dialogState.warning = Tr(state.language,
        L"Здесь нет полных лицензий и содержимого сертификатов, но могут быть путь диска и имена локальных профилей. Скопируйте текст для отчёта об ошибке.",
        L"This view contains no full licenses or certificate contents, but may include the disk path and local profile names. Copy it when reporting a scan problem.");
    dialogState.copiedMessage = Tr(state.language, L"Диагностика скопирована.", L"Diagnostics copied.");
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LICENSES), state.window,
                    LicensesDialogProc, reinterpret_cast<LPARAM>(&dialogState));
}

INT_PTR CALLBACK ConfirmDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ConfirmState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<ConfirmState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        state->backgroundBrush = CreateSolidBrush(kSurfaceSecondary);
        state->editBrush = CreateSolidBrush(kBackground);
        SetWindowTextW(dialog, Tr(state->language, L"Дополнительное подтверждение", L"Additional confirmation").c_str());
        const std::wstring prompt = state->message + L"\r\n" + Tr(state->language, L"Введите: ", L"Type: ") + state->phrase;
        SetText(dialog, IDC_CONFIRM_TEXT, prompt);
        SetText(dialog, IDOK, Tr(state->language, L"Удалить", L"Remove"));
        SetText(dialog, IDCANCEL, Tr(state->language, L"Отмена", L"Cancel"));
        SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
        SetFocus(GetDlgItem(dialog, IDC_CONFIRM_EDIT));
        return FALSE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDOK && state) {
        if (GetText(dialog, IDC_CONFIRM_EDIT) == state->phrase) EndDialog(dialog, IDOK);
        else MessageBoxW(dialog, Tr(state->language, L"Фраза введена неверно.", L"The confirmation phrase does not match.").c_str(),
                         Tr(state->language, L"Подтверждение", L"Confirmation").c_str(), MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(state ? state->backgroundBrush : nullptr);
    if (message == WM_CTLCOLORSTATIC && state) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, kText);
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    }
    if (message == WM_CTLCOLOREDIT && state) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkColor(device, kBackground);
        SetTextColor(device, kText);
        return reinterpret_cast<INT_PTR>(state->editBrush);
    }
    if (message == WM_DESTROY && state) {
        if (state->backgroundBrush) DeleteObject(state->backgroundBrush);
        if (state->editBrush) DeleteObject(state->editBrush);
        state->backgroundBrush = state->editBrush = nullptr;
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

std::wstring BrowseForFolder(HWND owner, const std::wstring& current,
                             const std::wstring& title = {}) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog))) || !dialog) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (!title.empty()) dialog->SetTitle(title.c_str());
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

void FinishOfflineScan(AppState& state, OfflineScanResult&& result) {
    state.offline = std::move(result);
    PopulateOfflineLists(state);
    if (state.offline.valid) {
        SetText(state.window, IDC_OFFLINE_PATH, state.offline.windowsDirectory);
    }
    state.summaryKind = SummaryKind::OfflineScan;
    state.summaryLanguage = state.language;
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETPOS, 100, 0);
    RefreshLocalizedSummary(state);
    RedrawWindow(state.window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    SetBusy(state, false);
    if (!state.offline.valid || (state.offline.scan.products.empty() && state.offline.scan.certificates.empty()))
        ShowOfflineDiagnostics(state);
}

void DoOfflineScan(AppState& state) {
    const std::wstring path = GetText(state.window, IDC_OFFLINE_PATH);
    if (path.empty()) {
        MessageBoxW(state.window, Tr(state.language,
                    L"Выберите диск, содержащий отключённую Windows (например, E:\\), или саму папку Windows (E:\\Windows).",
                    L"Select the drive containing disconnected Windows (for example, E:\\), or the Windows folder itself (E:\\Windows).").c_str(),
                    L"CryptoPro Cleanup Utility", MB_OK | MB_ICONWARNING);
        return;
    }
    SetBusy(state, true);
    state.logPath.clear();
    state.offlineScanRunning = true;
    SetOfflineScanAnimation(state, true);
    SetText(state.window, IDC_STATUS, Tr(state.language,
        L"Подготовка сканирования подключённого диска… Не отключайте носитель.",
        L"Preparing to scan the connected drive… Do not disconnect it."));
    const HWND window = state.window;
    const Language language = state.language;
    try {
        state.offlineScanThread = std::thread([window, language, path]() {
            auto result = std::make_unique<OfflineScanResult>();
            try {
                *result = ScanOfflineWindows(language, path,
                    [window](const std::wstring& message, int percent) {
                        auto update = std::make_unique<OfflineProgressMessage>();
                        update->message = message;
                        update->percent = percent;
                        if (PostMessageW(window, WM_CPC_OFFLINE_PROGRESS, 0,
                                         reinterpret_cast<LPARAM>(update.get()))) update.release();
                    });
            } catch (...) {
                result->scan.warnings.push_back(Tr(language,
                    L"Офлайн-сканирование прервано из-за непредвиденной ошибки.",
                    L"The offline scan stopped because of an unexpected error."));
                result->diagnostics.push_back(L"Background offline scan failed with an unexpected exception.");
            }
            if (PostMessageW(window, WM_CPC_OFFLINE_COMPLETE, 0,
                             reinterpret_cast<LPARAM>(result.get()))) result.release();
        });
    } catch (...) {
        state.offlineScanRunning = false;
        SetOfflineScanAnimation(state, false);
        SetBusy(state, false);
        MessageBoxW(state.window, Tr(state.language,
            L"Не удалось запустить фоновое сканирование.",
            L"The background scan could not be started.").c_str(),
            L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
    }
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

void FinishLiveScan(AppState& state, ScanResult&& scan) {
    state.scan = std::move(scan);
    state.plan = {};
    PopulateLists(state);
    state.summaryKind = SummaryKind::LiveScan;
    state.summaryLanguage = state.language;
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETPOS, 100, 0);
    RefreshLocalizedSummary(state);
    RedrawWindow(state.window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    SetBusy(state, false);
}

void DoScan(AppState& state) {
    SetBusy(state, true);
    state.logPath.clear();
    state.liveScanRunning = true;
    SetWindowTextW(GetDlgItem(state.window, IDC_LOG), L"");
    SendDlgItemMessageW(state.window, IDC_PROGRESS, PBM_SETPOS, 2, 0);
    SetText(state.window, IDC_STATUS, Tr(state.language,
        L"Безопасное сканирование системы… Изменения не выполняются.",
        L"Safely scanning the system… No changes are being made."));
    const HWND window = state.window;
    const Language language = state.language;
    try {
        state.liveScanThread = std::thread([window, language]() {
            auto scan = std::make_unique<ScanResult>();
            try {
                *scan = ScanSystem(language, [window](const std::wstring& message, int percent) {
                    auto update = std::make_unique<ScanProgressMessage>();
                    update->message = message;
                    update->percent = percent;
                    if (PostMessageW(window, WM_CPC_SCAN_PROGRESS, 0,
                                     reinterpret_cast<LPARAM>(update.get()))) update.release();
                });
            } catch (...) {
                scan->warnings.push_back(Tr(language,
                    L"Сканирование прервано из-за непредвиденной ошибки.",
                    L"The scan stopped because of an unexpected error."));
            }
            if (PostMessageW(window, WM_CPC_SCAN_COMPLETE, 0,
                             reinterpret_cast<LPARAM>(scan.get()))) scan.release();
        });
    } catch (...) {
        state.liveScanRunning = false;
        SetBusy(state, false);
        MessageBoxW(state.window, Tr(state.language,
            L"Не удалось запустить фоновое сканирование.",
            L"The background scan could not be started.").c_str(),
            L"CryptoPro Cleanup Utility", MB_OK | MB_ICONERROR);
    }
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
            MessageBoxW(state.window,
                Tr(state.language,
                   L"Для завершения требуется перезагрузка. Одноразовое продолжение зарегистрировано. Перезагрузите Windows вручную; утилита сама этого не делает.",
                   L"A restart is required. One-time continuation is registered. Restart Windows manually; the utility never does this itself.").c_str(),
                Tr(state.language, L"Требуется ручная перезагрузка", L"Manual restart required").c_str(),
                MB_OK | MB_ICONINFORMATION);
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
                 << Tr(state.language,
                       L"\r\n\r\nСначала запускаются штатные деинсталляторы, затем только подтверждённые остатки. Сертификаты, токены и контейнеры закрытых ключей сохраняются. В резервной папке уже созданы licenses.txt, summary.txt, report.json и cleanup.log.",
                       L"\r\n\r\nRegistered uninstallers run first, followed only by verified remnants. Certificates, tokens and private-key containers are preserved. The backup folder already contains licenses.txt, summary.txt, report.json and cleanup.log.");
    const std::wstring finalPhrase = state.language == Language::Russian ? L"УДАЛИТЬ" : L"DELETE";
    if (!TypedConfirm(state, finalPhrase, confirmation.str())) {
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

Page PageForNavigationControl(int id) {
    switch (id) {
        case IDC_NAV_CERTIFICATES: return Page::Certificates;
        case IDC_NAV_OFFLINE: return Page::Offline;
        case IDC_NAV_LOG: return Page::Log;
        case IDC_NAV_SETTINGS: return Page::Settings;
        case IDC_NAV_ABOUT: return Page::About;
        default: return Page::Overview;
    }
}

bool IsNavigationControl(int id) {
    return id >= IDC_NAV_OVERVIEW && id <= IDC_NAV_ABOUT;
}

bool IsPrimaryAction(int id) {
    return id == IDC_SCAN || id == IDC_CHECK_PLAN || id == IDC_EXPORT_CERTS ||
           id == IDC_OFFLINE_SCAN || id == IDC_OFFLINE_SAVE;
}

void DrawMainButton(AppState& state, const DRAWITEMSTRUCT& item) {
    const int id = static_cast<int>(item.CtlID);
    RECT rectangle = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool navigation = IsNavigationControl(id);
    const bool active = navigation && PageForNavigationControl(id) == state.currentPage;
    COLORREF fill = UiColor(state, kSurface, COLOR_BTNFACE);
    COLORREF border = UiColor(state, kBorder, COLOR_BTNSHADOW);
    COLORREF textColor = UiColor(state, kText, COLOR_BTNTEXT);
    if (active || IsPrimaryAction(id)) {
        fill = UiColor(state, pressed ? RGB(139, 84, 109) : kAccent, COLOR_HIGHLIGHT);
        border = fill;
        textColor = UiColor(state, kText, COLOR_HIGHLIGHTTEXT);
    } else if (id == IDC_CLEAN || id == IDC_OFFLINE_CLEAN) {
        border = UiColor(state, kDanger, COLOR_HOTLIGHT);
        textColor = UiColor(state, kDanger, COLOR_BTNTEXT);
        if (pressed) fill = RGB(65, 28, 30);
    } else if (pressed) {
        fill = UiColor(state, kSurfaceSecondary, COLOR_3DFACE);
    }
    if (disabled) textColor = UiColor(state, RGB(104, 100, 108), COLOR_GRAYTEXT);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom, 8, 8);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    if (active && navigation) {
        RECT strip = rectangle;
        strip.right = strip.left + 4;
        HBRUSH accent = CreateSolidBrush(UiColor(state, kAccent, COLOR_HIGHLIGHT));
        FillRect(item.hDC, &strip, accent);
        DeleteObject(accent);
    }
    wchar_t caption[256]{};
    GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    RECT textRectangle = rectangle;
    if (navigation) textRectangle.left += 12;
    const bool wrapNarrowAction = state.narrowLayout && !navigation &&
        (id == IDC_SHOW_LICENSES || id == IDC_CLEAN || id == IDC_COPY_THUMBPRINT ||
         id == IDC_EXPORT_CERTS || id == IDC_OFFLINE_SHOW_LICENSES || id == IDC_OFFLINE_DIAGNOSTICS ||
         id == IDC_OFFLINE_SAVE || id == IDC_OFFLINE_CLEAN);
    if (wrapNarrowAction) {
        RECT measured = textRectangle;
        DrawTextW(item.hDC, caption, -1, &measured,
                  DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        const int measuredHeight = measured.bottom - measured.top;
        textRectangle.top += std::max(0, static_cast<int>(textRectangle.bottom - textRectangle.top - measuredHeight) / 2);
        DrawTextW(item.hDC, caption, -1, &textRectangle,
                  DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    } else {
        DrawTextW(item.hDC, caption, -1, &textRectangle,
                  DT_SINGLELINE | DT_VCENTER | (navigation ? DT_LEFT : DT_CENTER) |
                  DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (item.itemState & ODS_FOCUS) {
        InflateRect(&rectangle, -3, -3);
        DrawFocusRect(item.hDC, &rectangle);
    }
}

LRESULT DrawListViewItem(AppState& state, NMLVCUSTOMDRAW* draw) {
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const bool selected = (ListView_GetItemState(draw->nmcd.hdr.hwndFrom,
                                static_cast<int>(draw->nmcd.dwItemSpec), LVIS_FOCUSED) & LVIS_FOCUSED) != 0;
        draw->clrText = selected ? UiColor(state, kText, COLOR_HIGHLIGHTTEXT) :
                                  UiColor(state, kText, COLOR_WINDOWTEXT);
        draw->clrTextBk = selected ? UiColor(state, kAccent, COLOR_HIGHLIGHT) :
                                    UiColor(state, (draw->nmcd.dwItemSpec % 2) ? kSurfaceSecondary : kSurface, COLOR_WINDOW);
        return CDRF_NEWFONT
#ifdef CPC_MODERN_UI
             | CDRF_NOTIFYPOSTPAINT
#endif
             ;
    }
#ifdef CPC_MODERN_UI
    if (draw->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
        RECT row{};
        if (!ListView_GetItemRect(draw->nmcd.hdr.hwndFrom, static_cast<int>(draw->nmcd.dwItemSpec), &row, LVIR_BOUNDS))
            return CDRF_DODEFAULT;
        const int size = std::max(13, std::min(17, static_cast<int>(row.bottom - row.top - 5)));
        RECT box{row.left + 5, row.top + (row.bottom - row.top - size) / 2,
                 row.left + 5 + size, row.top + (row.bottom - row.top - size) / 2 + size};
        const bool checked = ListView_GetCheckState(draw->nmcd.hdr.hwndFrom,
                                                    static_cast<int>(draw->nmcd.dwItemSpec)) != FALSE;
        HBRUSH brush = CreateSolidBrush(checked ? kAccent : kSurfaceSecondary);
        HPEN pen = CreatePen(PS_SOLID, 1, checked ? kAccent : kMutedText);
        HGDIOBJ oldBrush = SelectObject(draw->nmcd.hdc, brush);
        HGDIOBJ oldPen = SelectObject(draw->nmcd.hdc, pen);
        RoundRect(draw->nmcd.hdc, box.left, box.top, box.right, box.bottom, 5, 5);
        SelectObject(draw->nmcd.hdc, oldBrush);
        SelectObject(draw->nmcd.hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        if (checked) {
            HPEN check = CreatePen(PS_SOLID, 2, kText);
            oldPen = SelectObject(draw->nmcd.hdc, check);
            MoveToEx(draw->nmcd.hdc, box.left + size / 4, box.top + size / 2, nullptr);
            LineTo(draw->nmcd.hdc, box.left + size * 5 / 12, box.top + size * 2 / 3);
            LineTo(draw->nmcd.hdc, box.left + size * 3 / 4, box.top + size / 3);
            SelectObject(draw->nmcd.hdc, oldPen);
            DeleteObject(check);
        }
        return CDRF_DODEFAULT;
    }
#endif
    return CDRF_DODEFAULT;
}

void DrawCardGroup(HDC device, AppState& state, std::initializer_list<int> controls, int padding = 10) {
    RECT card{};
    bool first = true;
    for (const int id : controls) {
        HWND control = GetDlgItem(state.window, id);
        if (!control || !IsWindowVisible(control)) continue;
        RECT rectangle{};
        GetWindowRect(control, &rectangle);
        MapWindowPoints(HWND_DESKTOP, state.window, reinterpret_cast<POINT*>(&rectangle), 2);
        if (first) { card = rectangle; first = false; }
        else UnionRect(&card, &card, &rectangle);
    }
    if (first) return;
    InflateRect(&card, padding, padding);
    HBRUSH brush = CreateSolidBrush(UiColor(state, kSurface, COLOR_BTNFACE));
    HPEN pen = CreatePen(PS_SOLID, 1, UiColor(state, kBorder, COLOR_3DSHADOW));
    HGDIOBJ oldBrush = SelectObject(device, brush);
    HGDIOBJ oldPen = SelectObject(device, pen);
    RoundRect(device, card.left, card.top, card.right, card.bottom, 12, 12);
    SelectObject(device, oldBrush);
    SelectObject(device, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void PaintApplicationBackground(AppState& state) {
    PAINTSTRUCT paint{};
    HDC device = BeginPaint(state.window, &paint);
    RECT client{};
    GetClientRect(state.window, &client);
    FillRect(device, &client, state.backgroundBrush);
    HWND eyebrow = GetDlgItem(state.window, IDC_PAGE_EYEBROW);
    RECT boundary{};
    GetWindowRect(eyebrow, &boundary);
    MapWindowPoints(HWND_DESKTOP, state.window, reinterpret_cast<POINT*>(&boundary), 2);
    RECT sidebar{0, 0, std::max(1L, boundary.left - 12), client.bottom};
    FillRect(device, &sidebar, state.surfaceBrush);
    HPEN pen = CreatePen(PS_SOLID, 1, UiColor(state, kBorder, COLOR_3DSHADOW));
    HGDIOBJ oldPen = SelectObject(device, pen);
    MoveToEx(device, sidebar.right, 0, nullptr);
    LineTo(device, sidebar.right, client.bottom);
    SelectObject(device, oldPen);
    DeleteObject(pen);
    if (state.currentPage == Page::Overview) {
        DrawCardGroup(device, state, {IDC_STAT_PRODUCTS_CAPTION, IDC_STAT_PRODUCTS_VALUE}, 9);
        DrawCardGroup(device, state, {IDC_STAT_LICENSES_CAPTION, IDC_STAT_LICENSES_VALUE}, 9);
        DrawCardGroup(device, state, {IDC_STAT_CERTS_CAPTION, IDC_STAT_CERTS_VALUE}, 9);
        DrawCardGroup(device, state, {IDC_STAT_PROFILES_CAPTION, IDC_STAT_PROFILES_VALUE}, 9);
        DrawCardGroup(device, state, {IDC_PLAN_LABEL, IDC_PLAN_SUMMARY, IDC_CHECK_PLAN, IDC_PROTECTED_SUMMARY}, 10);
        DrawCardGroup(device, state, {IDC_BACKUP_LABEL, IDC_BACKUP_PATH, IDC_BROWSE, IDC_BACKUP_INFO,
                                      IDC_SHOW_LICENSES, IDC_SCAN, IDC_CLEAN}, 10);
    } else if (state.currentPage == Page::Certificates) {
        DrawCardGroup(device, state, {IDC_CERT_DETAILS_TITLE, IDC_CERT_DETAILS,
                                      IDC_COPY_THUMBPRINT, IDC_EXPORT_CERTS}, 10);
    } else if (state.currentPage == Page::Offline) {
        DrawCardGroup(device, state, {IDC_OFFLINE_SHOW_LICENSES, IDC_OFFLINE_DIAGNOSTICS,
                                      IDC_OFFLINE_SAVE, IDC_OFFLINE_CLEAN}, 10);
    } else if (state.currentPage == Page::Settings) {
        DrawCardGroup(device, state, {IDC_SETTINGS_THEME_LABEL, IDC_SETTINGS_THEME,
                                      IDC_SETTINGS_LANGUAGE_LABEL, IDC_SETTINGS_LANGUAGE,
                                      IDC_SETTINGS_REMEMBER_WINDOW, IDC_SETTINGS_HIGH_CONTRAST,
                                      IDC_SETTINGS_RESET, IDC_SETTINGS_INFO}, 14);
    } else if (state.currentPage == Page::About) {
        DrawCardGroup(device, state, {IDC_ABOUT_TITLE, IDC_ABOUT_TEXT, IDC_ABOUT_COPY_VERSION,
                                      IDC_ABOUT_GITHUB, IDC_ABOUT_WEBSITE, IDC_ABOUT_SUPPORT}, 16);
    }
    EndPaint(state.window, &paint);
}

INT_PTR CALLBACK MainDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<AppState*>(lParam);
        state->window = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        LoadUiSettings(*state);
        HINSTANCE module = GetModuleHandleW(nullptr);
        HICON largeIcon = reinterpret_cast<HICON>(LoadImageW(
            module, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
        HICON smallIcon = reinterpret_cast<HICON>(LoadImageW(
            module, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
        if (largeIcon) SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
        if (smallIcon) SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        EnableModernWindowEffects(dialog);
        SendDlgItemMessageW(dialog, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        HWND language = GetDlgItem(dialog, IDC_LANGUAGE);
        ComboBox_AddString(language, L"Русский");
        ComboBox_AddString(language, L"English");
        ComboBox_SetCurSel(language, state->language == Language::Russian ? 0 : 1);
        state->backupRoot = DefaultBackupFolder();
        SetText(dialog, IDC_BACKUP_PATH, state->backupRoot);
        ApplyModernTheme(*state);
        ApplyLanguage(*state);
        CaptureInitialLayout(*state);
        if (state->hasSavedWindow) RestoreSavedWindow(*state);
        else SizeDefaultWindow(*state);
        UpdatePageVisibility(*state);
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
    if (message == WM_PAINT) { PaintApplicationBackground(*state); return TRUE; }
    if (message == WM_DRAWITEM) {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && item->CtlType == ODT_BUTTON) { DrawMainButton(*state, *item); return TRUE; }
    }
    if (message == WM_SETTINGCHANGE) {
        ApplyModernTheme(*state);
        ApplyLanguage(*state);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG && state->backgroundBrush)
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    if (message == WM_CTLCOLORSTATIC && state->backgroundBrush) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkMode(device, TRANSPARENT);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        COLORREF color = UiColor(*state, kMutedText, COLOR_WINDOWTEXT);
        if (id == IDC_TITLE || id == IDC_ABOUT_TITLE || id == IDC_BRAND ||
            id == IDC_STAT_PRODUCTS_VALUE || id == IDC_STAT_LICENSES_VALUE ||
            id == IDC_STAT_CERTS_VALUE || id == IDC_STAT_PROFILES_VALUE)
            color = UiColor(*state, kText, COLOR_WINDOWTEXT);
        if (id == IDC_PAGE_EYEBROW) color = UiColor(*state, kAccent, COLOR_HOTLIGHT);
        if (id == IDC_SAFETY_STATE) color = UiColor(*state, kSuccess, COLOR_HIGHLIGHT);
        if (id == IDC_HEADER_STATUS) color = UiColor(*state, kText, COLOR_HIGHLIGHTTEXT);
        SetTextColor(device, color);
        const bool sidebar = id == IDC_BRAND || id == IDC_BRAND_VERSION || id == IDC_SAFETY_STATE;
        const bool card = id == IDC_STAT_PRODUCTS_CAPTION || id == IDC_STAT_PRODUCTS_VALUE ||
                          id == IDC_STAT_LICENSES_CAPTION || id == IDC_STAT_LICENSES_VALUE ||
                          id == IDC_STAT_CERTS_CAPTION || id == IDC_STAT_CERTS_VALUE ||
                          id == IDC_STAT_PROFILES_CAPTION || id == IDC_STAT_PROFILES_VALUE ||
                          id == IDC_PLAN_LABEL || id == IDC_PLAN_SUMMARY || id == IDC_PROTECTED_SUMMARY ||
                          id == IDC_CERT_DETAILS_TITLE || id == IDC_CERT_DETAILS ||
                          id == IDC_SETTINGS_THEME_LABEL || id == IDC_SETTINGS_LANGUAGE_LABEL ||
                          id == IDC_SETTINGS_HIGH_CONTRAST || id == IDC_SETTINGS_INFO ||
                          id == IDC_ABOUT_TITLE || id == IDC_ABOUT_TEXT;
        if (id == IDC_HEADER_STATUS) return reinterpret_cast<INT_PTR>(state->accentBrush);
        return reinterpret_cast<INT_PTR>((sidebar || card) ? state->surfaceBrush : state->backgroundBrush);
    }
    if ((message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) && state->editBrush) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkColor(device, UiColor(*state, kSurfaceSecondary, COLOR_WINDOW));
        SetTextColor(device, UiColor(*state, kText, COLOR_WINDOWTEXT));
        return reinterpret_cast<INT_PTR>(state->editBrush);
    }
    if (message == WM_CTLCOLORBTN && state->backgroundBrush) {
        HDC device = reinterpret_cast<HDC>(wParam);
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, UiColor(*state, kText, COLOR_BTNTEXT));
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    }
    if (message == WM_CPC_START) {
        if (state->resumeToken.empty()) DoScan(*state); else ResumeCleanup(*state);
        return TRUE;
    }
    if (message == WM_CPC_SCAN_PROGRESS) {
        std::unique_ptr<ScanProgressMessage> update(reinterpret_cast<ScanProgressMessage*>(lParam));
        if (update && state->liveScanRunning) {
            SendDlgItemMessageW(state->window, IDC_PROGRESS, PBM_SETPOS,
                                static_cast<WPARAM>(std::clamp(update->percent, 0, 100)), 0);
            SetText(state->window, IDC_STATUS, update->message);
        }
        return TRUE;
    }
    if (message == WM_CPC_SCAN_COMPLETE) {
        std::unique_ptr<ScanResult> scan(reinterpret_cast<ScanResult*>(lParam));
        if (state->liveScanThread.joinable()) state->liveScanThread.join();
        state->liveScanRunning = false;
        if (scan) FinishLiveScan(*state, std::move(*scan));
        else SetBusy(*state, false);
        return TRUE;
    }
    if (message == WM_CPC_OFFLINE_PROGRESS) {
        std::unique_ptr<OfflineProgressMessage> update(
            reinterpret_cast<OfflineProgressMessage*>(lParam));
        if (update && state->offlineScanRunning) {
            SetText(state->window, IDC_STATUS,
                    Tr(state->language, L"Сканирование диска — ", L"Drive scan — ") +
                    std::to_wstring(std::clamp(update->percent, 0, 100)) + L"%: " + update->message);
        }
        return TRUE;
    }
    if (message == WM_CPC_OFFLINE_COMPLETE) {
        std::unique_ptr<OfflineScanResult> result(
            reinterpret_cast<OfflineScanResult*>(lParam));
        if (state->offlineScanThread.joinable()) state->offlineScanThread.join();
        state->offlineScanRunning = false;
        SetOfflineScanAnimation(*state, false);
        if (result) FinishOfflineScan(*state, std::move(*result));
        else SetBusy(*state, false);
        return TRUE;
    }
    if (message == WM_SETCURSOR && (state->offlineScanRunning || state->liveScanRunning) && LOWORD(lParam) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
        return TRUE;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
            case IDC_NAV_OVERVIEW: if (!state->busy) SwitchPage(*state, Page::Overview); return TRUE;
            case IDC_NAV_CERTIFICATES: if (!state->busy) SwitchPage(*state, Page::Certificates); return TRUE;
            case IDC_NAV_OFFLINE: if (!state->busy) SwitchPage(*state, Page::Offline); return TRUE;
            case IDC_NAV_LOG: if (!state->busy) SwitchPage(*state, Page::Log); return TRUE;
            case IDC_NAV_SETTINGS: if (!state->busy) SwitchPage(*state, Page::Settings); return TRUE;
            case IDC_NAV_ABOUT: if (!state->busy) SwitchPage(*state, Page::About); return TRUE;
            case IDC_LANGUAGE:
                if (HIWORD(wParam) == CBN_SELCHANGE && !state->busy) {
                    ReadSelections(*state);
                    ReadOfflineSelections(*state);
                    state->language = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_LANGUAGE)) == 0 ? Language::Russian : Language::English;
                    ApplyLanguage(*state);
                }
                return TRUE;
            case IDC_SCAN: if (!state->busy) DoScan(*state); return TRUE;
            case IDC_CLEAN: if (!state->busy) StartCleanup(*state); return TRUE;
            case IDC_CHECK_PLAN: if (!state->busy) ShowPlanInspector(*state); return TRUE;
            case IDC_SHOW_LICENSES: if (!state->busy) ShowLicenses(*state); return TRUE;
            case IDC_EXPORT_CERTS: if (!state->busy) ExportCertificates(*state); return TRUE;
            case IDC_COPY_THUMBPRINT: if (!state->busy) {
                HWND list = GetDlgItem(dialog, IDC_CERTIFICATES);
                const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                if (row >= 0) {
                    LVITEMW item{};
                    item.mask = LVIF_PARAM;
                    item.iItem = row;
                    if (ListView_GetItem(list, &item) && item.lParam >= 0 &&
                        static_cast<size_t>(item.lParam) < state->scan.certificates.size())
                        CopyUnicodeText(dialog, state->scan.certificates[static_cast<size_t>(item.lParam)].thumbprint);
                }
            }
                return TRUE;
            case IDC_CERT_SEARCH:
                if (HIWORD(wParam) == EN_CHANGE && !state->busy && !state->refreshingUi) {
                    ReadSelections(*state);
                    PopulateCertificates(*state);
                }
                return TRUE;
            case IDC_CERT_FILTER:
                if (HIWORD(wParam) == CBN_SELCHANGE && !state->busy && !state->refreshingUi) {
                    ReadSelections(*state);
                    PopulateCertificates(*state);
                }
                return TRUE;
            case IDC_OFFLINE_SHOW_LICENSES: if (!state->busy) ShowLicensesForScan(*state, state->offline.scan); return TRUE;
            case IDC_OFFLINE_DIAGNOSTICS: if (!state->busy) ShowOfflineDiagnostics(*state); return TRUE;
            case IDC_OFFLINE_SAVE: if (!state->busy) SaveOfflineData(*state); return TRUE;
            case IDC_OFFLINE_CLEAN: if (!state->busy) StartOfflineCleanup(*state); return TRUE;
            case IDC_OFFLINE_SCAN: if (!state->busy) DoOfflineScan(*state); return TRUE;
            case IDC_COPY_LOG: if (!state->busy) {
                CopyUnicodeText(dialog, GetText(dialog, IDC_LOG));
            }
                return TRUE;
            case IDC_OPEN_REPORT_FOLDER: if (!state->busy) {
                const std::wstring folder = state->reportPath.empty() ? state->backupRoot : ReportFolder(*state);
                if (!folder.empty()) ShellExecuteW(dialog, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
                return TRUE;
            case IDC_CLEAR_LOG: if (!state->busy) {
                SetText(dialog, IDC_LOG, L"");
                state->summaryKind = SummaryKind::None;
            }
                return TRUE;
            case IDC_SETTINGS_THEME:
                if (HIWORD(wParam) == CBN_SELCHANGE && !state->busy) {
                    state->useSystemColors = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_SETTINGS_THEME)) == 1;
                    ApplyModernTheme(*state);
                    ApplyLanguage(*state);
                }
                return TRUE;
            case IDC_SETTINGS_LANGUAGE:
                if (HIWORD(wParam) == CBN_SELCHANGE && !state->busy) {
                    ReadSelections(*state);
                    ReadOfflineSelections(*state);
                    state->language = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_SETTINGS_LANGUAGE)) == 0 ?
                                      Language::Russian : Language::English;
                    ComboBox_SetCurSel(GetDlgItem(dialog, IDC_LANGUAGE), state->language == Language::Russian ? 0 : 1);
                    ApplyLanguage(*state);
                }
                return TRUE;
            case IDC_SETTINGS_REMEMBER_WINDOW:
                if (!state->busy)
                    state->rememberWindow = Button_GetCheck(GetDlgItem(dialog, IDC_SETTINGS_REMEMBER_WINDOW)) == BST_CHECKED;
                return TRUE;
            case IDC_SETTINGS_RESET: if (!state->busy) {
                if (MessageBoxW(dialog, Tr(state->language,
                        L"Сбросить сохранённый размер окна, страницу, язык и оформление?",
                        L"Reset saved window size, page, language and appearance?").c_str(),
                        Tr(state->language, L"Сброс настроек", L"Reset settings").c_str(),
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    RegDeleteKeyW(HKEY_CURRENT_USER, kUiSettingsKey);
                    state->skipSettingsSave = true;
                    state->rememberWindow = true;
                    state->useSystemColors = false;
                    ApplyModernTheme(*state);
                    ApplyLanguage(*state);
                    MessageBoxW(dialog, Tr(state->language,
                        L"Настройки сброшены. Стандартный размер окна применится при следующем запуске.",
                        L"Settings were reset. The default window size will apply on the next launch.").c_str(),
                        L"CryptoPro Cleanup Utility", MB_OK | MB_ICONINFORMATION);
                }
            }
                return TRUE;
            case IDC_ABOUT_COPY_VERSION: if (!state->busy) {
                std::wstring versionInfo = std::wstring(L"CryptoPro Cleanup Utility ") + kVersion;
#ifdef CPC_MODERN_UI
                versionInfo += L"\r\nx64 Modern, Windows 10/11, Per-Monitor V2 DPI";
#else
                versionInfo += L"\r\nWin32 Legacy, Windows 7 SP1–Windows 11, subsystem 6.01";
#endif
                versionInfo += L"\r\nC++17, MSVC v143, static CRT\r\nAuthor: Kirill Alexandrov\r\nhttps://github.com/acidtmn/CryptoProCleanup";
                CopyUnicodeText(dialog, versionInfo);
            }
                return TRUE;
            case IDC_BROWSE: if (!state->busy) {
                const std::wstring folder = BrowseForFolder(dialog, GetText(dialog, IDC_BACKUP_PATH));
                if (!folder.empty()) SetText(dialog, IDC_BACKUP_PATH, folder);
            }
                return TRUE;
            case IDC_OFFLINE_BROWSE: if (!state->busy) {
                const std::wstring folder = BrowseForFolder(
                    dialog, GetText(dialog, IDC_OFFLINE_PATH),
                    Tr(state->language,
                       L"Выберите диск с Windows или папку Windows",
                       L"Choose a Windows drive or the Windows folder"));
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
             header->idFrom == IDC_LINK_SUPPORT || header->idFrom == IDC_ABOUT_GITHUB ||
             header->idFrom == IDC_ABOUT_WEBSITE || header->idFrom == IDC_ABOUT_SUPPORT)) {
            OpenProjectLink(*state, static_cast<int>(header->idFrom));
            return TRUE;
        }
        if (header && header->code == NM_CUSTOMDRAW &&
            (header->idFrom == IDC_PRODUCTS || header->idFrom == IDC_PROFILES ||
             header->idFrom == IDC_CERTIFICATES || header->idFrom == IDC_OFFLINE_PRODUCTS ||
             header->idFrom == IDC_OFFLINE_CERTS)) {
            const LRESULT result = DrawListViewItem(*state, reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
            return TRUE;
        }
        if (header && header->idFrom == IDC_CERTIFICATES && header->code == LVN_ITEMCHANGED &&
            !state->busy && !state->refreshingUi) {
            ReadSelections(*state);
            UpdateCertificateSelectionSummary(*state);
            ShowSelectedCertificateDetails(*state);
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
        if (state->offlineScanThread.joinable()) state->offlineScanThread.join();
        if (state->liveScanThread.joinable()) state->liveScanThread.join();
        MSG pending{};
        while (PeekMessageW(&pending, dialog, WM_CPC_OFFLINE_PROGRESS,
                            WM_CPC_SCAN_COMPLETE, PM_REMOVE)) {
            if (pending.message == WM_CPC_OFFLINE_PROGRESS || pending.message == WM_CPC_SCAN_PROGRESS)
                delete reinterpret_cast<OfflineProgressMessage*>(pending.lParam);
            else if (pending.message == WM_CPC_OFFLINE_COMPLETE)
                delete reinterpret_cast<OfflineScanResult*>(pending.lParam);
            else if (pending.message == WM_CPC_SCAN_COMPLETE)
                delete reinterpret_cast<ScanResult*>(pending.lParam);
        }
        if (!state->skipSettingsSave) SaveUiSettings(*state);
        ResetThemeObjects(*state);
        return TRUE;
    }
    if (message == WM_CLOSE) {
        if (!state->busy) EndDialog(dialog, 0);
        else if (state->offlineScanRunning || state->liveScanRunning)
            SetText(state->window, IDC_STATUS, Tr(state->language,
                L"Сканирование ещё выполняется — дождитесь завершения.",
                L"The scan is still running — wait for it to finish."));
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int RunGui(HINSTANCE instance, Language language, const std::wstring& resumeToken,
           bool languageExplicit) {
    AppState state;
    state.language = language;
    state.resumeToken = resumeToken;
    state.languageExplicit = languageExplicit;
    return static_cast<int>(DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainDialogProc,
                                             reinterpret_cast<LPARAM>(&state)));
}

}  // namespace cpc
