#include <windows.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr UINT kDefaultDoubleTapMs = 300;
constexpr UINT kMinDoubleTapMs = 50;
constexpr UINT kMaxDoubleTapMs = 2000;
constexpr UINT_PTR kRepeatTimerId = 1;
constexpr UINT_PTR kDetectTimerId = 2;
constexpr UINT kInitialRepeatDelayMs = 400;
constexpr UINT kRepeatIntervalMs = 33;
constexpr UINT kRepeatPumpIntervalMs = 10;
constexpr UINT kDetectCountdownMs = 1000;
constexpr int kDetectTimeoutSeconds = 5;
constexpr ULONG_PTR kSyntheticInputTag = 0x4B545447;
constexpr wchar_t kWindowClassName[] = L"KeyTogglerWindow";
constexpr wchar_t kOverlayWindowClassName[] = L"KeyTogglerOverlayWindow";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayIconCallbackMessage = WM_APP + 1;

enum ControlId : int {
    IDC_KEY_LABEL = 1001,
    IDC_ADD_BUTTON = 1003,
    IDC_STATUS_LABEL = 1004,
    IDC_DOUBLE_TAP_LABEL = 1006,
    IDC_DOUBLE_TAP_EDIT = 1007,
    IDC_BINDINGS_LIST = 1008,
    IDC_REMOVE_BUTTON = 1009,
    IDC_OVERLAY_CHECKBOX = 1010,
    IDC_OVERLAY_FONT_LABEL = 1011,
    IDC_OVERLAY_FONT_EDIT = 1012,
    IDC_OVERLAY_CORNER_LABEL = 1013,
    IDC_OVERLAY_CORNER_COMBO = 1014,
    IDC_OVERLAY_COLOR_BUTTON = 1015,
    IDC_OVERLAY_MONITOR_LABEL = 1016,
    IDC_OVERLAY_MONITOR_COMBO = 1017,
    IDC_OVERLAY_COLOR_PREVIEW = 1018,
    IDC_MINIMIZE_TO_TRAY_CHECKBOX = 1019,
};

enum class InputKind {
    Keyboard,
    MouseButton,
};

struct TargetInput {
    InputKind kind{InputKind::Keyboard};
    UINT code{0};
};

struct ToggleBinding {
    TargetInput target{};
    UINT doubleTapMs = kDefaultDoubleTapMs;

    bool keyLatched = false;
    bool releaseLatchedOnNextPhysicalUp = false;
    bool suppressPhysicalUntilUp = false;
    bool physicalInputIsDown = false;
    std::chrono::steady_clock::time_point lastPhysicalDown{};
    bool hasFirstTap = false;

    std::chrono::steady_clock::time_point nextRepeatAt{};
};

enum class OverlayCorner {
    TopLeft,
    TopRight,
};

struct MonitorEntry {
    HMONITOR handle{};
    RECT bounds{};
    std::wstring label{};
    bool isPrimary = false;
};

struct AppState {
    HINSTANCE instance{};
    HWND window{};
    HWND doubleTapEdit{};
    HWND statusLabel{};
    HWND addButton{};
    HWND bindingsList{};
    HWND removeButton{};
    HWND overlayCheckbox{};
    HWND overlayFontEdit{};
    HWND overlayCornerCombo{};
    HWND overlayColorPreview{};
    HWND overlayColorButton{};
    HWND overlayMonitorCombo{};
    HWND minimizeToTrayCheckbox{};
    HWND overlayWindow{};
    HFONT uiFont{};
    HFONT overlayFont{};
    HBRUSH backgroundBrush{};
    HBRUSH controlBrush{};

    bool isAwaitingNewBinding = false;
    bool overlayEnabled = true;
    int overlayFontSizePx = 16;
    OverlayCorner overlayCorner = OverlayCorner::TopLeft;
    COLORREF overlayTextColor = RGB(244, 244, 245);
    std::vector<MonitorEntry> monitors{};
    NOTIFYICONDATAW trayIconData{};
    bool trayIconVisible = false;
    bool minimizeToTrayEnabled = true;

    HHOOK keyboardHook{};
    HHOOK mouseHook{};

    std::vector<ToggleBinding> bindings{};

    bool isDetectingInput = false;
    int detectSecondsRemaining = 0;
};

AppState gState;

constexpr COLORREF kDarkBackgroundColor = RGB(24, 24, 27);
constexpr COLORREF kDarkSurfaceColor = RGB(39, 39, 42);
constexpr COLORREF kDarkTextColor = RGB(244, 244, 245);
constexpr COLORREF kDarkButtonColor = RGB(63, 63, 70);
constexpr COLORREF kDarkButtonHoverColor = RGB(82, 82, 91);
constexpr COLORREF kDarkButtonDisabledColor = RGB(51, 51, 56);
constexpr COLORREF kOverlayTransparentColorKey = RGB(255, 0, 255);
constexpr int kMinOverlayFontSizePx = 10;
constexpr int kMaxOverlayFontSizePx = 48;

void SetOverlayVisible(bool visible);
void UpdateOverlay();
void RepositionOverlayWindow();
void UpdateRemoveButtonEnabled();

void ShowTrayIcon() {
    if (gState.window == nullptr || gState.trayIconVisible) {
        return;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = gState.window;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayIconCallbackMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(icon.szTip, L"Key Toggler", static_cast<int>(std::size(icon.szTip)));

    if (Shell_NotifyIconW(NIM_ADD, &icon)) {
        gState.trayIconData = icon;
        gState.trayIconVisible = true;
    } else if (icon.hIcon != nullptr) {
        DestroyIcon(icon.hIcon);
    }
}

void HideTrayIcon() {
    if (!gState.trayIconVisible) {
        return;
    }

    Shell_NotifyIconW(NIM_DELETE, &gState.trayIconData);
    if (gState.trayIconData.hIcon != nullptr) {
        DestroyIcon(gState.trayIconData.hIcon);
        gState.trayIconData.hIcon = nullptr;
    }
    gState.trayIconVisible = false;
}

void RestoreFromTray() {
    ShowWindow(gState.window, SW_RESTORE);
    ShowWindow(gState.window, SW_SHOW);
    SetForegroundWindow(gState.window);
    HideTrayIcon();
}

std::wstring KeyboardVkToDisplay(UINT vk) {
    if (vk == VK_SHIFT) {
        return L"SHIFT";
    }
    if (vk == VK_MENU) {
        return L"ALT";
    }
    if (vk == VK_CONTROL) {
        return L"CTRL";
    }
    if (vk == VK_BROWSER_BACK) {
        return L"BROWSER BACK";
    }
    if (vk == VK_BROWSER_FORWARD) {
        return L"BROWSER FORWARD";
    }

    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scanCode == 0) {
        return L"?";
    }

    LONG lParam = static_cast<LONG>(scanCode << 16);
    if (vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN || vk == VK_INSERT || vk == VK_DELETE ||
        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DIVIDE || vk == VK_NUMLOCK) {
        lParam |= (1 << 24);
    }

    std::array<wchar_t, 64> buffer{};
    int written = GetKeyNameTextW(lParam, buffer.data(), static_cast<int>(buffer.size()));
    if (written <= 0) {
        wchar_t fallback[2] = {static_cast<wchar_t>(vk), L'\0'};
        return fallback;
    }
    return std::wstring(buffer.data());
}

std::wstring MouseVkToDisplay(UINT vk) {
    switch (vk) {
        case VK_LBUTTON:
            return L"MOUSE1 (Left Click)";
        case VK_RBUTTON:
            return L"MOUSE2 (Right Click)";
        case VK_MBUTTON:
            return L"MOUSE3 (Middle Click)";
        case VK_XBUTTON1:
            return L"MOUSE4 (X1)";
        case VK_XBUTTON2:
            return L"MOUSE5 (X2)";
        default:
            return L"Mouse Button";
    }
}

std::wstring InputToDisplay(const TargetInput& input) {
    return input.kind == InputKind::Keyboard ? KeyboardVkToDisplay(input.code) : MouseVkToDisplay(input.code);
}

bool InputsEqual(const TargetInput& lhs, const TargetInput& rhs) {
    return lhs.kind == rhs.kind && lhs.code == rhs.code;
}

bool AnyLatched() {
    return std::any_of(gState.bindings.begin(),
                       gState.bindings.end(),
                       [](const ToggleBinding& binding) { return binding.keyLatched; });
}

std::wstring BuildOverlayText() {
    std::wstring text = L"Key Toggler\n";
    if (gState.bindings.empty()) {
        text += L"No configured bindings";
        return text;
    }

    for (const auto& binding : gState.bindings) {
        text += binding.keyLatched ? L"[ON] " : L"[OFF] ";
        text += InputToDisplay(binding.target);
        text += L"\n";
    }

    if (!text.empty() && text.back() == L'\n') {
        text.pop_back();
    }
    return text;
}

void RebuildOverlayFont() {
    if (gState.overlayFont != nullptr) {
        DeleteObject(gState.overlayFont);
        gState.overlayFont = nullptr;
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return;
    }

    metrics.lfMessageFont.lfHeight = -gState.overlayFontSizePx;
    metrics.lfMessageFont.lfWeight = FW_NORMAL;
    gState.overlayFont = CreateFontIndirectW(&metrics.lfMessageFont);
}

bool ParseOverlayFontSize(int& outSize) {
    if (gState.overlayFontEdit == nullptr) {
        return false;
    }

    wchar_t text[16]{};
    GetWindowTextW(gState.overlayFontEdit, text, static_cast<int>(std::size(text)));
    if (text[0] == L'\0') {
        return false;
    }

    wchar_t* end = nullptr;
    long value = std::wcstol(text, &end, 10);
    if (end == text || *end != L'\0') {
        return false;
    }
    if (value < kMinOverlayFontSizePx || value > kMaxOverlayFontSizePx) {
        return false;
    }

    outSize = static_cast<int>(value);
    return true;
}

void ApplyOverlayFontSizeFromUi() {
    int parsedSize = 0;
    if (!ParseOverlayFontSize(parsedSize) || parsedSize == gState.overlayFontSizePx) {
        return;
    }

    gState.overlayFontSizePx = parsedSize;
    RebuildOverlayFont();
    UpdateOverlay();
}

void SetOverlayVisible(bool visible) {
    if (gState.overlayWindow == nullptr) {
        return;
    }
    ShowWindow(gState.overlayWindow, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void UpdateOverlay() {
    if (gState.overlayWindow == nullptr) {
        return;
    }

    std::wstring text = BuildOverlayText();
    SetWindowTextW(gState.overlayWindow, text.c_str());
    RepositionOverlayWindow();
    if (gState.overlayEnabled) {
        SetOverlayVisible(true);
    }
    InvalidateRect(gState.overlayWindow, nullptr, TRUE);
}

BOOL CALLBACK CollectMonitorProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) {
    auto* entries = reinterpret_cast<std::vector<MonitorEntry>*>(lParam);
    if (entries == nullptr) {
        return FALSE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMonitor, &info)) {
        return TRUE;
    }

    MonitorEntry entry{};
    entry.handle = hMonitor;
    entry.bounds = info.rcMonitor;
    entry.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    entry.label = info.szDevice;
    if (entry.isPrimary) {
        entry.label += L" (Primary)";
    }
    entries->push_back(entry);
    return TRUE;
}

void RefreshMonitorList() {
    LRESULT previousSelection = CB_ERR;
    if (gState.overlayMonitorCombo != nullptr) {
        previousSelection = SendMessageW(gState.overlayMonitorCombo, CB_GETCURSEL, 0, 0);
    }

    gState.monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc, reinterpret_cast<LPARAM>(&gState.monitors));

    if (gState.overlayMonitorCombo == nullptr) {
        return;
    }

    SendMessageW(gState.overlayMonitorCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& monitor : gState.monitors) {
        SendMessageW(gState.overlayMonitorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(monitor.label.c_str()));
    }

    if (!gState.monitors.empty()) {
        LRESULT selectionToApply = 0;
        if (previousSelection >= 0 && static_cast<size_t>(previousSelection) < gState.monitors.size()) {
            selectionToApply = previousSelection;
        } else {
            const auto primaryIt = std::find_if(
                gState.monitors.begin(), gState.monitors.end(), [](const MonitorEntry& monitor) { return monitor.isPrimary; });
            if (primaryIt != gState.monitors.end()) {
                selectionToApply = static_cast<LRESULT>(std::distance(gState.monitors.begin(), primaryIt));
            }
        }
        SendMessageW(gState.overlayMonitorCombo, CB_SETCURSEL, selectionToApply, 0);
    }
}

RECT GetSelectedMonitorBounds() {
    if (gState.monitors.empty()) {
        RECT fallback{0, 0, 1920, 1080};
        return fallback;
    }

    LRESULT selected = CB_ERR;
    if (gState.overlayMonitorCombo != nullptr) {
        selected = SendMessageW(gState.overlayMonitorCombo, CB_GETCURSEL, 0, 0);
    }
    if (selected == CB_ERR || selected < 0 || static_cast<size_t>(selected) >= gState.monitors.size()) {
        return gState.monitors.front().bounds;
    }
    return gState.monitors[static_cast<size_t>(selected)].bounds;
}

void RepositionOverlayWindow() {
    if (gState.overlayWindow == nullptr) {
        return;
    }

    constexpr int kOverlayWidth = 280;
    constexpr int kOverlayHeight = 240;
    constexpr int kOverlayMargin = 10;

    RECT bounds = GetSelectedMonitorBounds();
    const int x = (gState.overlayCorner == OverlayCorner::TopLeft)
                      ? (bounds.left + kOverlayMargin)
                      : (bounds.right - kOverlayWidth - kOverlayMargin);
    const int y = bounds.top + kOverlayMargin;

    SetWindowPos(
        gState.overlayWindow, HWND_TOPMOST, x, y, kOverlayWidth, kOverlayHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PickOverlayColor() {
    COLORREF customColors[16]{};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = gState.window;
    chooser.rgbResult = gState.overlayTextColor;
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&chooser)) {
        return;
    }

    gState.overlayTextColor = chooser.rgbResult;
    if (gState.overlayColorPreview != nullptr) {
        InvalidateRect(gState.overlayColorPreview, nullptr, TRUE);
    }
    InvalidateRect(gState.overlayWindow, nullptr, TRUE);
}

void UpdateBindingsLabel() {
    if (gState.bindingsList == nullptr) {
        return;
    }

    SendMessageW(gState.bindingsList, LB_RESETCONTENT, 0, 0);
    for (const auto& binding : gState.bindings) {
        std::wstring line =
            std::wstring(binding.keyLatched ? L"[ON] " : L"[OFF] ") + InputToDisplay(binding.target) + L" (" +
            std::to_wstring(binding.doubleTapMs) + L" ms)";
        SendMessageW(gState.bindingsList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }

    UpdateRemoveButtonEnabled();
}

void UpdateRemoveButtonEnabled() {
    if (gState.removeButton == nullptr || gState.bindingsList == nullptr) {
        return;
    }

    const LRESULT selectedIndex = SendMessageW(gState.bindingsList, LB_GETCURSEL, 0, 0);
    const bool hasValidSelection =
        selectedIndex != LB_ERR && selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < gState.bindings.size();
    EnableWindow(gState.removeButton, hasValidSelection ? TRUE : FALSE);
}

void UpdateStatus() {
    const int latchedCount = static_cast<int>(std::count_if(gState.bindings.begin(),
                                                            gState.bindings.end(),
                                                            [](const ToggleBinding& binding) {
                                                                return binding.keyLatched;
                                                            }));

    std::wstring status = L"Status: " + std::to_wstring(latchedCount) + L" latched | Configured keys: " +
                          std::to_wstring(gState.bindings.size());
    SetWindowTextW(gState.statusLabel, status.c_str());
    UpdateBindingsLabel();
    UpdateOverlay();
}

void SendInputDownOrUp(const TargetInput& input, bool down) {
    INPUT nativeInput{};
    if (input.kind == InputKind::Keyboard) {
        nativeInput.type = INPUT_KEYBOARD;
        nativeInput.ki.wVk = static_cast<WORD>(input.code);
        nativeInput.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    } else {
        nativeInput.type = INPUT_MOUSE;
        switch (input.code) {
            case VK_LBUTTON:
                nativeInput.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                break;
            case VK_RBUTTON:
                nativeInput.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                break;
            case VK_MBUTTON:
                nativeInput.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                break;
            case VK_XBUTTON1:
            case VK_XBUTTON2:
                nativeInput.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                nativeInput.mi.mouseData = (input.code == VK_XBUTTON1) ? XBUTTON1 : XBUTTON2;
                break;
            default:
                return;
        }
    }

    if (input.kind == InputKind::Keyboard) {
        nativeInput.ki.dwExtraInfo = kSyntheticInputTag;
    } else {
        nativeInput.mi.dwExtraInfo = kSyntheticInputTag;
    }

    SendInput(1, &nativeInput, sizeof(INPUT));
}

void StopRepeatTimer() {
    if (gState.window != nullptr) {
        KillTimer(gState.window, kRepeatTimerId);
    }
}

void EnsureRepeatTimerRunning() {
    if (gState.window != nullptr && AnyLatched()) {
        SetTimer(gState.window, kRepeatTimerId, kRepeatPumpIntervalMs, nullptr);
    }
}

void StopDetectMode(bool restoreButtonText) {
    gState.isDetectingInput = false;
    gState.detectSecondsRemaining = 0;
    if (gState.window != nullptr) {
        KillTimer(gState.window, kDetectTimerId);
    }

    if (restoreButtonText && gState.addButton != nullptr) {
        SetWindowTextW(gState.addButton, L"Add New Key");
    }
    if (gState.addButton != nullptr) {
        EnableWindow(gState.addButton, TRUE);
    }
}


void SetAwaitingNewBinding(bool awaiting) {
    gState.isAwaitingNewBinding = awaiting;
}

void StopAllLatchedState() {
    for (auto& binding : gState.bindings) {
        if (binding.keyLatched) {
            SendInputDownOrUp(binding.target, false);
        }

        binding.keyLatched = false;
        binding.hasFirstTap = false;
        binding.releaseLatchedOnNextPhysicalUp = false;
        binding.suppressPhysicalUntilUp = false;
        binding.physicalInputIsDown = false;
    }

    StopRepeatTimer();
}

bool ParseDoubleTapMs(UINT& outMs) {
    wchar_t text[32]{};
    GetWindowTextW(gState.doubleTapEdit, text, static_cast<int>(std::size(text)));
    if (text[0] == L'\0') {
        return false;
    }

    wchar_t* end = nullptr;
    unsigned long value = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0') {
        return false;
    }

    if (value < kMinDoubleTapMs || value > kMaxDoubleTapMs) {
        return false;
    }

    outMs = static_cast<UINT>(value);
    return true;
}

void UpsertBinding(const TargetInput& target, UINT doubleTapMs) {
    const auto existing = std::find_if(gState.bindings.begin(),
                                       gState.bindings.end(),
                                       [&](const ToggleBinding& binding) { return InputsEqual(binding.target, target); });

    if (existing != gState.bindings.end()) {
        if (existing->keyLatched) {
            SendInputDownOrUp(existing->target, false);
        }

        existing->doubleTapMs = doubleTapMs;
        existing->keyLatched = false;
        existing->hasFirstTap = false;
        existing->releaseLatchedOnNextPhysicalUp = false;
        existing->suppressPhysicalUntilUp = false;
        existing->physicalInputIsDown = false;
    } else {
        ToggleBinding binding{};
        binding.target = target;
        binding.doubleTapMs = doubleTapMs;
        gState.bindings.push_back(binding);
    }

    if (!AnyLatched()) {
        StopRepeatTimer();
    }
    UpdateStatus();
}

void RemoveSelectedBinding() {
    LRESULT selectedIndex = SendMessageW(gState.bindingsList, LB_GETCURSEL, 0, 0);
    if (selectedIndex == LB_ERR) {
        MessageBoxW(gState.window, L"Select a configured key/button to remove.", L"Nothing selected", MB_OK | MB_ICONINFORMATION);
        return;
    }

    ToggleBinding& binding = gState.bindings[static_cast<size_t>(selectedIndex)];
    if (binding.keyLatched) {
        SendInputDownOrUp(binding.target, false);
    }

    gState.bindings.erase(gState.bindings.begin() + selectedIndex);
    if (!AnyLatched()) {
        StopRepeatTimer();
    }
    UpdateStatus();
}

void AddConfiguredInput() {
    if (gState.isAwaitingNewBinding) {
        MessageBoxW(gState.window,
                    L"Press the key/button you want to bind.",
                    L"Waiting for detection",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    SetAwaitingNewBinding(true);
    UINT parsedDoubleTapMs = 0;
    if (!ParseDoubleTapMs(parsedDoubleTapMs)) {
        MessageBoxW(gState.window,
                    L"Please enter a valid double-tap window in milliseconds (50-2000) before detecting.",
                    L"Invalid double-tap window",
                    MB_OK | MB_ICONWARNING);
        SetAwaitingNewBinding(false);
        return;
    }

    StopDetectMode(false);
    gState.isDetectingInput = true;
    gState.detectSecondsRemaining = kDetectTimeoutSeconds;
    SetTimer(gState.window, kDetectTimerId, kDetectCountdownMs, nullptr);
    EnableWindow(gState.addButton, FALSE);
    SetWindowTextW(gState.addButton, L"Press input... (5)");
    SetFocus(gState.window);
}

void CaptureDetectedInput(const TargetInput& detected) {
    UINT parsedDoubleTapMs = 0;
    if (!ParseDoubleTapMs(parsedDoubleTapMs)) {
        MessageBoxW(gState.window,
                    L"Please enter a valid double-tap window in milliseconds (50-2000).",
                    L"Invalid double-tap window",
                    MB_OK | MB_ICONWARNING);
        StopDetectMode(true);
        return;
    }

    UpsertBinding(detected, parsedDoubleTapMs);
    StopDetectMode(true);
    SetAwaitingNewBinding(false);
}

bool IsSyntheticKeyboard(const KBDLLHOOKSTRUCT* data) {
    return (data->flags & LLKHF_INJECTED) != 0 && data->dwExtraInfo == kSyntheticInputTag;
}

bool IsSyntheticMouse(const MSLLHOOKSTRUCT* data) {
    return (data->flags & LLMHF_INJECTED) != 0 && data->dwExtraInfo == kSyntheticInputTag;
}

LRESULT HandleBindingPhysicalEvent(ToggleBinding& binding, bool isDown, bool isUp) {
    if (binding.suppressPhysicalUntilUp) {
        if (isUp) {
            binding.suppressPhysicalUntilUp = false;
            binding.physicalInputIsDown = false;
            return 1;
        }

        if (isDown) {
            return 1;
        }
    }

    if (binding.keyLatched) {
        if (isDown) {
            if (!binding.physicalInputIsDown) {
                binding.physicalInputIsDown = true;
                binding.releaseLatchedOnNextPhysicalUp = true;
            }
            return 1;
        }

        if (isUp) {
            binding.physicalInputIsDown = false;
            if (binding.releaseLatchedOnNextPhysicalUp) {
                SendInputDownOrUp(binding.target, false);
                binding.keyLatched = false;
                binding.releaseLatchedOnNextPhysicalUp = false;
                binding.hasFirstTap = false;
                if (!AnyLatched()) {
                    StopRepeatTimer();
                }
                UpdateStatus();
            }
            return 1;
        }

        return 1;
    }

    if (isUp) {
        binding.physicalInputIsDown = false;
        return 0;
    }

    if (isDown) {
        if (binding.physicalInputIsDown) {
            return 0;
        }

        binding.physicalInputIsDown = true;
        auto now = std::chrono::steady_clock::now();
        if (binding.hasFirstTap) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - binding.lastPhysicalDown).count();
            if (elapsedMs <= binding.doubleTapMs) {
                SendInputDownOrUp(binding.target, true);
                binding.keyLatched = true;
                binding.hasFirstTap = false;
                binding.releaseLatchedOnNextPhysicalUp = false;
                binding.suppressPhysicalUntilUp = true;
                binding.nextRepeatAt = now + std::chrono::milliseconds(kInitialRepeatDelayMs);
                EnsureRepeatTimerRunning();
                UpdateStatus();
                return 1;
            }
        }

        binding.lastPhysicalDown = now;
        binding.hasFirstTap = true;
    }

    return 0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* keyData = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (IsSyntheticKeyboard(keyData)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    if (gState.isDetectingInput && isDown) {
        CaptureDetectedInput({InputKind::Keyboard, keyData->vkCode});
        return 1;
    }

    const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    for (auto& binding : gState.bindings) {
        if (binding.target.kind != InputKind::Keyboard || keyData->vkCode != binding.target.code) {
            continue;
        }

        LRESULT handled = HandleBindingPhysicalEvent(binding, isDown, isUp);
        if (handled != 0) {
            return handled;
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool DecodeMouseEvent(WPARAM wParam, const MSLLHOOKSTRUCT* data, UINT& outVk, bool& isDown, bool& isUp) {
    outVk = 0;
    isDown = false;
    isUp = false;

    switch (wParam) {
        case WM_LBUTTONDOWN:
            outVk = VK_LBUTTON;
            isDown = true;
            return true;
        case WM_LBUTTONUP:
            outVk = VK_LBUTTON;
            isUp = true;
            return true;
        case WM_RBUTTONDOWN:
            outVk = VK_RBUTTON;
            isDown = true;
            return true;
        case WM_RBUTTONUP:
            outVk = VK_RBUTTON;
            isUp = true;
            return true;
        case WM_MBUTTONDOWN:
            outVk = VK_MBUTTON;
            isDown = true;
            return true;
        case WM_MBUTTONUP:
            outVk = VK_MBUTTON;
            isUp = true;
            return true;
        case WM_XBUTTONDOWN:
            outVk = (HIWORD(data->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            isDown = true;
            return true;
        case WM_XBUTTONUP:
            outVk = (HIWORD(data->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            isUp = true;
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* mouseData = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (IsSyntheticMouse(mouseData)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    UINT mouseVk = 0;
    bool isDown = false;
    bool isUp = false;
    if (!DecodeMouseEvent(wParam, mouseData, mouseVk, isDown, isUp)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (gState.isDetectingInput && isDown) {
        CaptureDetectedInput({InputKind::MouseButton, mouseVk});
        return 1;
    }

    for (auto& binding : gState.bindings) {
        if (binding.target.kind != InputKind::MouseButton || mouseVk != binding.target.code) {
            continue;
        }

        LRESULT handled = HandleBindingPhysicalEvent(binding, isDown, isUp);
        if (handled != 0) {
            return handled;
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool InstallHooks() {
    gState.keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, gState.instance, 0);
    gState.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, gState.instance, 0);
    return gState.keyboardHook != nullptr && gState.mouseHook != nullptr;
}

void UninstallHooks() {
    if (gState.keyboardHook != nullptr) {
        UnhookWindowsHookEx(gState.keyboardHook);
        gState.keyboardHook = nullptr;
    }

    if (gState.mouseHook != nullptr) {
        UnhookWindowsHookEx(gState.mouseHook);
        gState.mouseHook = nullptr;
    }
}

void ApplyUiFont(HWND hwnd) {
    if (gState.uiFont != nullptr) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(gState.uiFont), TRUE);
    }
}

void TryEnableDarkTitleBar(HWND hwnd) {
    const BOOL enableDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enableDarkMode, sizeof(enableDarkMode));
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH background = CreateSolidBrush(kOverlayTransparentColorKey);
            FillRect(dc, &client, background);
            DeleteObject(background);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, gState.overlayTextColor);
            if (gState.overlayFont != nullptr) {
                SelectObject(dc, gState.overlayFont);
            } else if (gState.uiFont != nullptr) {
                SelectObject(dc, gState.uiFont);
            }

            wchar_t text[2048]{};
            GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
            RECT textRect = client;
            textRect.left += 8;
            textRect.top += 8;
            textRect.right -= 8;
            textRect.bottom -= 8;
            DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

            EndPaint(hwnd, &ps);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            NONCLIENTMETRICSW metrics{};
            metrics.cbSize = sizeof(metrics);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
            metrics.lfMessageFont.lfHeight = -19;
            metrics.lfMessageFont.lfWeight = FW_MEDIUM;
            gState.uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
            gState.backgroundBrush = CreateSolidBrush(kDarkBackgroundColor);
            gState.controlBrush = CreateSolidBrush(kDarkSurfaceColor);

            gState.minimizeToTrayCheckbox = CreateWindowW(L"BUTTON",
                                                          L"Minimize to tray",
                                                          WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                                          20,
                                                          16,
                                                          220,
                                                          28,
                                                          hwnd,
                                                          reinterpret_cast<HMENU>(IDC_MINIMIZE_TO_TRAY_CHECKBOX),
                                                          gState.instance,
                                                          nullptr);
            SendMessageW(gState.minimizeToTrayCheckbox, BM_SETCHECK, BST_CHECKED, 0);

            CreateWindowW(L"STATIC",
                          L"Toggle input (key or mouse button):",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          56,
                          350,
                          30,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_KEY_LABEL),
                          gState.instance,
                          nullptr);

            gState.addButton = CreateWindowW(L"BUTTON",
                                             L"Add New Key",
                                             WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                                             380,
                                             54,
                                             150,
                                             34,
                                             hwnd,
                                             reinterpret_cast<HMENU>(IDC_ADD_BUTTON),
                                             gState.instance,
                                             nullptr);

            CreateWindowW(L"STATIC",
                          L"Double-tap window (ms):",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          100,
                          220,
                          30,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_DOUBLE_TAP_LABEL),
                          gState.instance,
                          nullptr);

            gState.doubleTapEdit = CreateWindowW(L"EDIT",
                                                 L"300",
                                                 WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                                 245,
                                                 98,
                                                 95,
                                                 32,
                                                 hwnd,
                                                 reinterpret_cast<HMENU>(IDC_DOUBLE_TAP_EDIT),
                                                 gState.instance,
                                                 nullptr);

            gState.statusLabel = CreateWindowW(L"STATIC",
                                               L"",
                                               WS_VISIBLE | WS_CHILD,
                                               20,
                                               140,
                                               520,
                                               28,
                                               hwnd,
                                               reinterpret_cast<HMENU>(IDC_STATUS_LABEL),
                                               gState.instance,
                                               nullptr);

            gState.bindingsList = CreateWindowW(L"LISTBOX",
                                                L"",
                                                WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
                                                20,
                                                176,
                                                640,
                                                170,
                                                hwnd,
                                                reinterpret_cast<HMENU>(IDC_BINDINGS_LIST),
                                                gState.instance,
                                                nullptr);

            gState.removeButton = CreateWindowW(L"BUTTON",
                                                L"Remove Selected",
                                                WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                                                550,
                                                136,
                                                170,
                                                34,
                                                hwnd,
                                                reinterpret_cast<HMENU>(IDC_REMOVE_BUTTON),
                                                gState.instance,
                                                nullptr);

            CreateWindowW(L"STATIC",
                          L"Overlay Settings",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          396,
                          240,
                          28,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_OVERLAY_FONT_LABEL),
                          gState.instance,
                          nullptr);

            gState.overlayCheckbox = CreateWindowW(L"BUTTON",
                                                   L"Enabled",
                                                   WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                                   20,
                                                   430,
                                                   120,
                                                   28,
                                                   hwnd,
                                                   reinterpret_cast<HMENU>(IDC_OVERLAY_CHECKBOX),
                                                   gState.instance,
                                                   nullptr);
            SendMessageW(gState.overlayCheckbox, BM_SETCHECK, BST_CHECKED, 0);

            CreateWindowW(L"STATIC",
                          L"Font:",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          464,
                          120,
                          28,
                          hwnd,
                          nullptr,
                          gState.instance,
                          nullptr);

            gState.overlayFontEdit = CreateWindowW(L"EDIT",
                                                   L"16",
                                                   WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                                   145,
                                                   462,
                                                   50,
                                                   30,
                                                   hwnd,
                                                   reinterpret_cast<HMENU>(IDC_OVERLAY_FONT_EDIT),
                                                   gState.instance,
                                                   nullptr);

            CreateWindowW(L"STATIC",
                          L"px",
                          WS_VISIBLE | WS_CHILD,
                          205,
                          464,
                          35,
                          28,
                          hwnd,
                          nullptr,
                          gState.instance,
                          nullptr);

            gState.overlayColorPreview = CreateWindowW(L"STATIC",
                                                       L"",
                                                       WS_VISIBLE | WS_CHILD | SS_OWNERDRAW,
                                                       245,
                                                       464,
                                                       45,
                                                       28,
                                                       hwnd,
                                                       reinterpret_cast<HMENU>(IDC_OVERLAY_COLOR_PREVIEW),
                                                       gState.instance,
                                                       nullptr);

            CreateWindowW(L"STATIC",
                          L"Location:",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          504,
                          120,
                          28,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_OVERLAY_CORNER_LABEL),
                          gState.instance,
                          nullptr);

            gState.overlayCornerCombo = CreateWindowW(L"COMBOBOX",
                                                      L"",
                                                      WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST,
                                                      145,
                                                      502,
                                                      140,
                                                      220,
                                                      hwnd,
                                                      reinterpret_cast<HMENU>(IDC_OVERLAY_CORNER_COMBO),
                                                      gState.instance,
                                                      nullptr);
            SendMessageW(gState.overlayCornerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-left"));
            SendMessageW(gState.overlayCornerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-right"));
            SendMessageW(gState.overlayCornerCombo, CB_SETCURSEL, 0, 0);

            gState.overlayColorButton = CreateWindowW(L"BUTTON",
                                                      L"Select Color...",
                                                      WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                                                      300,
                                                      462,
                                                      150,
                                                      30,
                                                      hwnd,
                                                      reinterpret_cast<HMENU>(IDC_OVERLAY_COLOR_BUTTON),
                                                      gState.instance,
                                                      nullptr);

            CreateWindowW(L"STATIC",
                          L"Monitor:",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          544,
                          120,
                          28,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_OVERLAY_MONITOR_LABEL),
                          gState.instance,
                          nullptr);

            gState.overlayMonitorCombo = CreateWindowW(L"COMBOBOX",
                                                       L"",
                                                       WS_VISIBLE | WS_CHILD | WS_BORDER | CBS_DROPDOWNLIST,
                                                       145,
                                                       542,
                                                       340,
                                                       220,
                                                       hwnd,
                                                       reinterpret_cast<HMENU>(IDC_OVERLAY_MONITOR_COMBO),
                                                       gState.instance,
                                                       nullptr);
            RefreshMonitorList();

            TryEnableDarkTitleBar(hwnd);

            HWND currentControl = GetWindow(hwnd, GW_CHILD);
            while (currentControl != nullptr) {
                ApplyUiFont(currentControl);
                currentControl = GetWindow(currentControl, GW_HWNDNEXT);
            }

            RebuildOverlayFont();

            gState.overlayWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                                       WS_EX_TRANSPARENT,
                                                   kOverlayWindowClassName,
                                                   L"",
                                                   WS_POPUP,
                                                   10,
                                                   10,
                                                   280,
                                                   240,
                                                   nullptr,
                                                   nullptr,
                                                   gState.instance,
                                                   nullptr);
            if (gState.overlayWindow != nullptr) {
                SetLayeredWindowAttributes(gState.overlayWindow, kOverlayTransparentColorKey, 0, LWA_COLORKEY);
            }
            UpdateOverlay();
            SetOverlayVisible(gState.overlayEnabled);

            SetAwaitingNewBinding(false);
            UpdateStatus();
            UpdateRemoveButtonEnabled();
            return 0;
        }

        case WM_DRAWITEM: {
            const auto* drawItem = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (drawItem == nullptr ||
                (drawItem->CtlID != IDC_ADD_BUTTON && drawItem->CtlID != IDC_REMOVE_BUTTON &&
                 drawItem->CtlID != IDC_OVERLAY_COLOR_BUTTON &&
                 drawItem->CtlID != IDC_OVERLAY_COLOR_PREVIEW)) {
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }

            if (drawItem->CtlID == IDC_OVERLAY_COLOR_PREVIEW) {
                HBRUSH swatchBrush = CreateSolidBrush(gState.overlayTextColor);
                FillRect(drawItem->hDC, &drawItem->rcItem, swatchBrush);
                DeleteObject(swatchBrush);
                FrameRect(drawItem->hDC, &drawItem->rcItem, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                return TRUE;
            }

            COLORREF fillColor = kDarkButtonColor;
            if ((drawItem->itemState & ODS_DISABLED) != 0) {
                fillColor = kDarkButtonDisabledColor;
            } else if ((drawItem->itemState & ODS_HOTLIGHT) != 0 || (drawItem->itemState & ODS_SELECTED) != 0) {
                fillColor = kDarkButtonHoverColor;
            }

            HBRUSH buttonBrush = CreateSolidBrush(fillColor);
            FillRect(drawItem->hDC, &drawItem->rcItem, buttonBrush);
            DeleteObject(buttonBrush);

            SetBkMode(drawItem->hDC, TRANSPARENT);
            SetTextColor(drawItem->hDC, kDarkTextColor);
            if (gState.uiFont != nullptr) {
                SelectObject(drawItem->hDC, gState.uiFont);
            }

            std::array<wchar_t, 64> caption{};
            GetWindowTextW(drawItem->hwndItem, caption.data(), static_cast<int>(caption.size()));
            RECT textRect = drawItem->rcItem;
            DrawTextW(drawItem->hDC, caption.data(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            if ((drawItem->itemState & ODS_FOCUS) != 0) {
                RECT focusRect = drawItem->rcItem;
                InflateRect(&focusRect, -3, -3);
                DrawFocusRect(drawItem->hDC, &focusRect);
            }
            return TRUE;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_ADD_BUTTON) {
                AddConfiguredInput();
            } else if (LOWORD(wParam) == IDC_REMOVE_BUTTON) {
                RemoveSelectedBinding();
            } else if (LOWORD(wParam) == IDC_OVERLAY_CHECKBOX) {
                gState.overlayEnabled = (SendMessageW(gState.overlayCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetOverlayVisible(gState.overlayEnabled);
            } else if (LOWORD(wParam) == IDC_OVERLAY_FONT_EDIT && HIWORD(wParam) == EN_CHANGE) {
                ApplyOverlayFontSizeFromUi();
            } else if (LOWORD(wParam) == IDC_OVERLAY_COLOR_BUTTON) {
                PickOverlayColor();
            } else if (LOWORD(wParam) == IDC_OVERLAY_CORNER_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                LRESULT selected = SendMessageW(gState.overlayCornerCombo, CB_GETCURSEL, 0, 0);
                gState.overlayCorner = (selected == 1) ? OverlayCorner::TopRight : OverlayCorner::TopLeft;
                RepositionOverlayWindow();
            } else if (LOWORD(wParam) == IDC_OVERLAY_MONITOR_COMBO && HIWORD(wParam) == CBN_DROPDOWN) {
                RefreshMonitorList();
            } else if (LOWORD(wParam) == IDC_OVERLAY_MONITOR_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                RepositionOverlayWindow();
            } else if (LOWORD(wParam) == IDC_MINIMIZE_TO_TRAY_CHECKBOX) {
                gState.minimizeToTrayEnabled =
                    (SendMessageW(gState.minimizeToTrayCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
            } else if (LOWORD(wParam) == IDC_BINDINGS_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
                UpdateRemoveButtonEnabled();
            }
            return 0;
        }

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED && gState.minimizeToTrayEnabled) {
                ShowTrayIcon();
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;

        case kTrayIconCallbackMessage:
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                RestoreFromTray();
            }
            return 0;

        case WM_DESTROY:
            StopDetectMode(true);
            StopAllLatchedState();
            UninstallHooks();
            HideTrayIcon();
            if (gState.overlayWindow != nullptr) {
                DestroyWindow(gState.overlayWindow);
                gState.overlayWindow = nullptr;
            }
            if (gState.uiFont != nullptr) {
                DeleteObject(gState.uiFont);
                gState.uiFont = nullptr;
            }
            if (gState.overlayFont != nullptr) {
                DeleteObject(gState.overlayFont);
                gState.overlayFont = nullptr;
            }
            if (gState.backgroundBrush != nullptr) {
                DeleteObject(gState.backgroundBrush);
                gState.backgroundBrush = nullptr;
            }
            if (gState.controlBrush != nullptr) {
                DeleteObject(gState.controlBrush);
                gState.controlBrush = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kDarkTextColor);
            const COLORREF backgroundColor = (msg == WM_CTLCOLORSTATIC) ? kDarkBackgroundColor : kDarkSurfaceColor;
            SetBkColor(dc, backgroundColor);
            HBRUSH brush = (msg == WM_CTLCOLORSTATIC) ? gState.backgroundBrush : gState.controlBrush;
            return reinterpret_cast<LRESULT>(brush != nullptr ? brush : GetStockObject(BLACK_BRUSH));
        }

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, gState.backgroundBrush != nullptr ? gState.backgroundBrush : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }

        case WM_TIMER:
            if (wParam == kRepeatTimerId) {
                bool hasAnyLatched = false;
                const auto now = std::chrono::steady_clock::now();
                for (auto& binding : gState.bindings) {
                    if (!binding.keyLatched) {
                        continue;
                    }

                hasAnyLatched = true;
                    if (binding.target.kind == InputKind::Keyboard && now >= binding.nextRepeatAt) {
                        SendInputDownOrUp(binding.target, true);
                        binding.nextRepeatAt = now + std::chrono::milliseconds(kRepeatIntervalMs);
                    }
                }

                if (!hasAnyLatched) {
                    StopRepeatTimer();
                }
                return 0;
            }

            if (wParam == kDetectTimerId && gState.isDetectingInput) {
                --gState.detectSecondsRemaining;
                if (gState.detectSecondsRemaining <= 0) {
                    StopDetectMode(true);
                    SetAwaitingNewBinding(false);
                } else {
                    std::wstring text = L"Press input... (" + std::to_wstring(gState.detectSecondsRemaining) + L")";
                    SetWindowTextW(gState.addButton, text.c_str());
                }
                return 0;
            }
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    gState.instance = hInstance;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSW overlayWc{};
    overlayWc.lpfnWndProc = OverlayWindowProc;
    overlayWc.hInstance = hInstance;
    overlayWc.lpszClassName = kOverlayWindowClassName;
    overlayWc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    overlayWc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    if (!RegisterClassW(&overlayWc)) {
        MessageBoxW(nullptr, L"Failed to register overlay window class.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    gState.window = CreateWindowExW(0,
                                    kWindowClassName,
                                    L"Key Toggler",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    760,
                                    660,
                                    nullptr,
                                    nullptr,
                                    hInstance,
                                    nullptr);

    if (gState.window == nullptr) {
        MessageBoxW(nullptr, L"Failed to create main window.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!InstallHooks()) {
        MessageBoxW(gState.window,
                    L"Failed to install keyboard/mouse hooks. Try running as administrator.",
                    L"Hook error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(gState.window, nCmdShow);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
