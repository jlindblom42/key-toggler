#include <windows.h>

#include <array>
#include <chrono>
#include <cwctype>
#include <string>

namespace {

constexpr int kDoubleTapMs = 300;
constexpr wchar_t kWindowClassName[] = L"KeyTogglerWindow";

enum ControlId : int {
    IDC_KEY_LABEL = 1001,
    IDC_KEY_EDIT = 1002,
    IDC_APPLY_BUTTON = 1003,
    IDC_STATUS_LABEL = 1004,
    IDC_TIPS_LABEL = 1005,
};

struct AppState {
    HINSTANCE instance{};
    HWND window{};
    HWND keyEdit{};
    HWND statusLabel{};

    HHOOK keyboardHook{};

    UINT targetVk = 'T';
    bool keyLatched = false;
    bool ignoreNextPhysicalUp = false;
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
                          L" | Configured key: " + VkToDisplay(gState.targetVk);
    SetWindowTextW(gState.statusLabel, status.c_str());
}

void SendVirtualKey(UINT vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
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

void ApplyConfiguredKey() {
    UINT parsed = ParseKeyFromEdit();
    if (parsed == 0) {
        MessageBoxW(gState.window,
                    L"Please enter a valid single key (for example: T, F, 1).",
                    L"Invalid key",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    if (gState.keyLatched) {
        SendVirtualKey(gState.targetVk, false);
        gState.keyLatched = false;
    }

    gState.targetVk = parsed;
    gState.hasFirstTap = false;
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

    if (gState.keyLatched) {
        if (isDown) {
            SendVirtualKey(gState.targetVk, false);
            gState.keyLatched = false;
            gState.ignoreNextPhysicalUp = true;
            gState.hasFirstTap = false;
            UpdateStatus();
            return 1;
        }

        if (isUp && gState.ignoreNextPhysicalUp) {
            gState.ignoreNextPhysicalUp = false;
            return 1;
        }

        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (isDown) {
        auto now = std::chrono::steady_clock::now();
        if (gState.hasFirstTap) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - gState.lastPhysicalDown).count();
            if (elapsedMs <= kDoubleTapMs) {
                SendVirtualKey(gState.targetVk, true);
                gState.keyLatched = true;
                gState.hasFirstTap = false;
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
                                           230,
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
                          290,
                          18,
                          70,
                          24,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_APPLY_BUTTON),
                          gState.instance,
                          nullptr);

            gState.statusLabel = CreateWindowW(L"STATIC",
                                               L"",
                                               WS_VISIBLE | WS_CHILD,
                                               20,
                                               60,
                                               420,
                                               20,
                                               hwnd,
                                               reinterpret_cast<HMENU>(IDC_STATUS_LABEL),
                                               gState.instance,
                                               nullptr);

            CreateWindowW(L"STATIC",
                          L"Behavior: double tap configured key to latch down; tap/hold once to release.",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          90,
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
            UninstallHook();
            PostQuitMessage(0);
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
                                    180,
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
