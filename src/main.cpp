#include <windows.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <string>

namespace {

constexpr UINT kDefaultDoubleTapMs = 300;
constexpr UINT kMinDoubleTapMs = 50;
constexpr UINT kMaxDoubleTapMs = 2000;
constexpr UINT_PTR kRepeatTimerId = 1;
constexpr UINT kInitialRepeatDelayMs = 400;
constexpr UINT kRepeatIntervalMs = 33;
constexpr wchar_t kWindowClassName[] = L"KeyTogglerWindow";

enum ControlId : int {
    IDC_KEY_LABEL = 1001,
    IDC_KEY_EDIT = 1002,
    IDC_APPLY_BUTTON = 1003,
    IDC_STATUS_LABEL = 1004,
    IDC_TIPS_LABEL = 1005,
    IDC_DOUBLE_TAP_LABEL = 1006,
    IDC_DOUBLE_TAP_EDIT = 1007,
};

struct AppState {
    HINSTANCE instance{};
    HWND window{};
    HWND keyEdit{};
    HWND doubleTapEdit{};
    HWND statusLabel{};

    HHOOK keyboardHook{};

    UINT targetVk = 'T';
    UINT doubleTapMs = kDefaultDoubleTapMs;
    bool keyLatched = false;
    bool suppressPhysicalUntilUp = false;
    bool physicalKeyIsDown = false;
    std::chrono::steady_clock::time_point lastPhysicalDown{};

    bool hasFirstTap = false;
};

AppState gState;

std::wstring VkToDisplay(UINT vk) {
    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scanCode == 0) {
        return L"?";
    }

    LONG lParam = static_cast<LONG>(scanCode << 16);
    std::array<wchar_t, 64> buffer{};
    int written = GetKeyNameTextW(lParam, buffer.data(), static_cast<int>(buffer.size()));
    if (written <= 0) {
        wchar_t fallback[2] = {static_cast<wchar_t>(vk), L'\0'};
        return fallback;
    }
    return std::wstring(buffer.data());
}

void UpdateStatus() {
    std::wstring status = L"Status: " + std::wstring(gState.keyLatched ? L"Latched" : L"Not latched") +
                          L" | Configured key: " + VkToDisplay(gState.targetVk) +
                          L" | Double tap window: " + std::to_wstring(gState.doubleTapMs) + L" ms";
    SetWindowTextW(gState.statusLabel, status.c_str());
}

void SendVirtualKey(UINT vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void StopRepeatTimer() {
    if (gState.window != nullptr) {
        KillTimer(gState.window, kRepeatTimerId);
    }
}

void StartRepeatTimer() {
    if (gState.window != nullptr) {
        SetTimer(gState.window, kRepeatTimerId, kInitialRepeatDelayMs, nullptr);
    }
}

UINT ParseKeyFromEdit() {
    wchar_t text[32]{};
    GetWindowTextW(gState.keyEdit, text, static_cast<int>(std::size(text)));

    wchar_t ch = text[0];
    if (ch == L'\0') {
        return 0;
    }

    ch = static_cast<wchar_t>(std::towupper(ch));

    SHORT vk = VkKeyScanW(ch);
    if (vk == -1) {
        return 0;
    }

    return LOBYTE(vk);
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

void ApplyConfiguredKey() {
    UINT parsed = ParseKeyFromEdit();
    if (parsed == 0) {
        MessageBoxW(gState.window,
                    L"Please enter a valid single key (for example: T, F, 1).",
                    L"Invalid key",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    UINT parsedDoubleTapMs = 0;
    if (!ParseDoubleTapMs(parsedDoubleTapMs)) {
        MessageBoxW(gState.window,
                    L"Please enter a valid double-tap window in milliseconds (50-2000).",
                    L"Invalid double-tap window",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    if (gState.keyLatched) {
        SendVirtualKey(gState.targetVk, false);
        StopRepeatTimer();
        gState.keyLatched = false;
    }

    gState.targetVk = parsed;
    gState.doubleTapMs = parsedDoubleTapMs;
    gState.hasFirstTap = false;
    gState.suppressPhysicalUntilUp = false;
    gState.physicalKeyIsDown = false;
    UpdateStatus();
}

bool IsInjected(const KBDLLHOOKSTRUCT* data) {
    return (data->flags & LLKHF_INJECTED) != 0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* keyData = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (IsInjected(keyData) || keyData->vkCode != gState.targetVk) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

    if (gState.suppressPhysicalUntilUp) {
        if (isUp) {
            gState.suppressPhysicalUntilUp = false;
            gState.physicalKeyIsDown = false;
            return 1;
        }

        if (isDown) {
            return 1;
        }
    }

    if (gState.keyLatched) {
        if (isDown) {
            SendVirtualKey(gState.targetVk, false);
            StopRepeatTimer();
            gState.keyLatched = false;
            gState.suppressPhysicalUntilUp = true;
            gState.physicalKeyIsDown = true;
            gState.hasFirstTap = false;
            UpdateStatus();
            return 1;
        }

        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (isUp) {
        gState.physicalKeyIsDown = false;
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (isDown) {
        if (gState.physicalKeyIsDown) {
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        gState.physicalKeyIsDown = true;
        auto now = std::chrono::steady_clock::now();
        if (gState.hasFirstTap) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - gState.lastPhysicalDown).count();
            if (elapsedMs <= gState.doubleTapMs) {
                SendVirtualKey(gState.targetVk, true);
                StartRepeatTimer();
                gState.keyLatched = true;
                gState.hasFirstTap = false;
                gState.suppressPhysicalUntilUp = true;
                UpdateStatus();
                return 1;
            }
        }

        gState.lastPhysicalDown = now;
        gState.hasFirstTap = true;
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool InstallHook() {
    gState.keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, gState.instance, 0);
    return gState.keyboardHook != nullptr;
}

void UninstallHook() {
    if (gState.keyboardHook != nullptr) {
        UnhookWindowsHookEx(gState.keyboardHook);
        gState.keyboardHook = nullptr;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            CreateWindowW(L"STATIC",
                          L"Toggle key (single character):",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          20,
                          210,
                          20,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_KEY_LABEL),
                          gState.instance,
                          nullptr);

            gState.keyEdit = CreateWindowW(L"EDIT",
                                           L"T",
                                           WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                           210,
                                           18,
                                           50,
                                           24,
                                           hwnd,
                                           reinterpret_cast<HMENU>(IDC_KEY_EDIT),
                                           gState.instance,
                                           nullptr);

            CreateWindowW(L"BUTTON",
                          L"Apply",
                          WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          270,
                          18,
                          70,
                          24,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_APPLY_BUTTON),
                          gState.instance,
                          nullptr);

            CreateWindowW(L"STATIC",
                          L"Double-tap window (ms):",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          52,
                          180,
                          20,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_DOUBLE_TAP_LABEL),
                          gState.instance,
                          nullptr);

            gState.doubleTapEdit = CreateWindowW(L"EDIT",
                                                 L"300",
                                                 WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                                 210,
                                                 50,
                                                 80,
                                                 24,
                                                 hwnd,
                                                 reinterpret_cast<HMENU>(IDC_DOUBLE_TAP_EDIT),
                                                 gState.instance,
                                                 nullptr);

            gState.statusLabel = CreateWindowW(L"STATIC",
                                               L"",
                                               WS_VISIBLE | WS_CHILD,
                                               20,
                                               84,
                                               420,
                                               20,
                                               hwnd,
                                               reinterpret_cast<HMENU>(IDC_STATUS_LABEL),
                                               gState.instance,
                                               nullptr);

            CreateWindowW(L"STATIC",
                          L"Behavior: double tap configured key to latch and auto-repeat; tap/hold once to release.",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          112,
                          430,
                          40,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_TIPS_LABEL),
                          gState.instance,
                          nullptr);

            UpdateStatus();
            return 0;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_APPLY_BUTTON) {
                ApplyConfiguredKey();
            }
            return 0;
        }

        case WM_DESTROY:
            if (gState.keyLatched) {
                SendVirtualKey(gState.targetVk, false);
                gState.keyLatched = false;
            }
            StopRepeatTimer();
            UninstallHook();
            PostQuitMessage(0);
            return 0;

        case WM_TIMER:
            if (wParam == kRepeatTimerId && gState.keyLatched) {
                SendVirtualKey(gState.targetVk, true);
                SetTimer(gState.window, kRepeatTimerId, kRepeatIntervalMs, nullptr);
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

    gState.window = CreateWindowExW(0,
                                    kWindowClassName,
                                    L"Key Toggler",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    480,
                                    220,
                                    nullptr,
                                    nullptr,
                                    hInstance,
                                    nullptr);

    if (gState.window == nullptr) {
        MessageBoxW(nullptr, L"Failed to create main window.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!InstallHook()) {
        MessageBoxW(gState.window,
                    L"Failed to install keyboard hook. Try running as administrator.",
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
