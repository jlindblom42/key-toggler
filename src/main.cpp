#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cwctype>
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
constexpr wchar_t kWindowClassName[] = L"KeyTogglerWindow";

enum ControlId : int {
    IDC_KEY_LABEL = 1001,
    IDC_KEY_EDIT = 1002,
    IDC_ADD_BUTTON = 1003,
    IDC_STATUS_LABEL = 1004,
    IDC_TIPS_LABEL = 1005,
    IDC_DOUBLE_TAP_LABEL = 1006,
    IDC_DOUBLE_TAP_EDIT = 1007,
    IDC_DETECT_BUTTON = 1008,
    IDC_BINDINGS_LABEL = 1009,
};

enum class InputKind {
    Keyboard,
    MouseButton,
};

struct TargetInput {
    InputKind kind{InputKind::Keyboard};
    UINT code{'T'};
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

struct AppState {
    HINSTANCE instance{};
    HWND window{};
    HWND keyEdit{};
    HWND doubleTapEdit{};
    HWND statusLabel{};
    HWND detectButton{};
    HWND bindingsLabel{};

    HHOOK keyboardHook{};
    HHOOK mouseHook{};

    std::vector<ToggleBinding> bindings{};

    bool isDetectingInput = false;
    int detectSecondsRemaining = 0;
};

AppState gState;

std::wstring TrimAndUpper(std::wstring text) {
    size_t start = 0;
    while (start < text.size() && std::iswspace(text[start]) != 0) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::iswspace(text[end - 1]) != 0) {
        --end;
    }

    std::wstring trimmed = text.substr(start, end - start);
    for (wchar_t& ch : trimmed) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }

    return trimmed;
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

void UpdateBindingsLabel() {
    std::wstring text = L"Configured keys:";
    if (gState.bindings.empty()) {
        text += L" (none)";
    } else {
        for (const auto& binding : gState.bindings) {
            text += L"\r\n- " + InputToDisplay(binding.target) + L" (" + std::to_wstring(binding.doubleTapMs) + L" ms)";
        }
    }

    SetWindowTextW(gState.bindingsLabel, text.c_str());
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

    if (restoreButtonText && gState.detectButton != nullptr) {
        SetWindowTextW(gState.detectButton, L"Detect (5s)");
    }
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

bool ParseInputToken(const std::wstring& rawText, TargetInput& outInput) {
    std::wstring text = TrimAndUpper(rawText);

    if (text.empty()) {
        return false;
    }

    struct NamedInput {
        const wchar_t* name;
        TargetInput input;
    };

    constexpr NamedInput namedInputs[] = {
        {L"SHIFT", {InputKind::Keyboard, VK_SHIFT}},     {L"ALT", {InputKind::Keyboard, VK_MENU}},
        {L"CTRL", {InputKind::Keyboard, VK_CONTROL}},    {L"CONTROL", {InputKind::Keyboard, VK_CONTROL}},
        {L"SPACE", {InputKind::Keyboard, VK_SPACE}},     {L"TAB", {InputKind::Keyboard, VK_TAB}},
        {L"ENTER", {InputKind::Keyboard, VK_RETURN}},    {L"ESC", {InputKind::Keyboard, VK_ESCAPE}},
        {L"ESCAPE", {InputKind::Keyboard, VK_ESCAPE}},   {L"UP", {InputKind::Keyboard, VK_UP}},
        {L"DOWN", {InputKind::Keyboard, VK_DOWN}},       {L"LEFT", {InputKind::Keyboard, VK_LEFT}},
        {L"RIGHT", {InputKind::Keyboard, VK_RIGHT}},     {L"MOUSE1", {InputKind::MouseButton, VK_LBUTTON}},
        {L"MOUSE2", {InputKind::MouseButton, VK_RBUTTON}},
        {L"MOUSE3", {InputKind::MouseButton, VK_MBUTTON}},
        {L"MOUSE4", {InputKind::MouseButton, VK_XBUTTON1}},
        {L"MOUSE5", {InputKind::MouseButton, VK_XBUTTON2}},
        {L"LEFTCLICK", {InputKind::MouseButton, VK_LBUTTON}},
        {L"RIGHTCLICK", {InputKind::MouseButton, VK_RBUTTON}},
        {L"MIDDLECLICK", {InputKind::MouseButton, VK_MBUTTON}},
        {L"XBUTTON1", {InputKind::MouseButton, VK_XBUTTON1}},
        {L"XBUTTON2", {InputKind::MouseButton, VK_XBUTTON2}},
    };

    for (const auto& namedInput : namedInputs) {
        if (text == namedInput.name) {
            outInput = namedInput.input;
            return true;
        }
    }

    if (text.size() == 1) {
        SHORT vk = VkKeyScanW(text[0]);
        if (vk == -1) {
            return false;
        }

        outInput = {InputKind::Keyboard, static_cast<UINT>(LOBYTE(vk))};
        return true;
    }

    return false;
}

bool ParseInputFromEdit(TargetInput& outInput) {
    wchar_t text[64]{};
    GetWindowTextW(gState.keyEdit, text, static_cast<int>(std::size(text)));
    return ParseInputToken(text, outInput);
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

void AddConfiguredInput() {
    TargetInput parsedInput{};
    if (!ParseInputFromEdit(parsedInput)) {
        MessageBoxW(gState.window,
                    L"Please enter a valid key or button (for example: T, SHIFT, ALT, MOUSE1, MOUSE2).",
                    L"Invalid input",
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

    UpsertBinding(parsedInput, parsedDoubleTapMs);
}

void BeginDetectMode() {
    UINT parsedDoubleTapMs = 0;
    if (!ParseDoubleTapMs(parsedDoubleTapMs)) {
        MessageBoxW(gState.window,
                    L"Please enter a valid double-tap window in milliseconds (50-2000) before detecting.",
                    L"Invalid double-tap window",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    StopDetectMode(false);
    gState.isDetectingInput = true;
    gState.detectSecondsRemaining = kDetectTimeoutSeconds;
    SetTimer(gState.window, kDetectTimerId, kDetectCountdownMs, nullptr);

    SetWindowTextW(gState.detectButton, L"Press input... (5)");
    SetFocus(gState.window);
}

void CaptureDetectedInput(const TargetInput& detected) {
    std::wstring inputName = InputToDisplay(detected);
    SetWindowTextW(gState.keyEdit, inputName.c_str());
    StopDetectMode(true);
}

bool IsInjectedKeyboard(const KBDLLHOOKSTRUCT* data) {
    return (data->flags & LLKHF_INJECTED) != 0;
}

bool IsInjectedMouse(const MSLLHOOKSTRUCT* data) {
    return (data->flags & LLMHF_INJECTED) != 0;
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
    if (IsInjectedKeyboard(keyData)) {
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
    if (IsInjectedMouse(mouseData)) {
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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            CreateWindowW(L"STATIC",
                          L"Toggle input (key or mouse button):",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          20,
                          220,
                          20,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_KEY_LABEL),
                          gState.instance,
                          nullptr);

            gState.keyEdit = CreateWindowW(L"EDIT",
                                           L"T",
                                           WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                           240,
                                           18,
                                           140,
                                           24,
                                           hwnd,
                                           reinterpret_cast<HMENU>(IDC_KEY_EDIT),
                                           gState.instance,
                                           nullptr);

            gState.detectButton = CreateWindowW(L"BUTTON",
                                                L"Detect (5s)",
                                                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                                390,
                                                18,
                                                90,
                                                24,
                                                hwnd,
                                                reinterpret_cast<HMENU>(IDC_DETECT_BUTTON),
                                                gState.instance,
                                                nullptr);

            CreateWindowW(L"BUTTON",
                          L"Add New Key",
                          WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          490,
                          18,
                          95,
                          24,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_ADD_BUTTON),
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
                                               560,
                                               20,
                                               hwnd,
                                               reinterpret_cast<HMENU>(IDC_STATUS_LABEL),
                                               gState.instance,
                                               nullptr);

            gState.bindingsLabel = CreateWindowW(L"STATIC",
                                                 L"",
                                                 WS_VISIBLE | WS_CHILD,
                                                 20,
                                                 110,
                                                 560,
                                                 60,
                                                 hwnd,
                                                 reinterpret_cast<HMENU>(IDC_BINDINGS_LABEL),
                                                 gState.instance,
                                                 nullptr);

            CreateWindowW(L"STATIC",
                          L"Behavior: each configured key/button toggles independently; double tap to latch and auto-repeat,"
                          L" then tap/hold once to release.\nUse Add New Key to append another key, or re-add to update its timing.",
                          WS_VISIBLE | WS_CHILD,
                          20,
                          176,
                          560,
                          40,
                          hwnd,
                          reinterpret_cast<HMENU>(IDC_TIPS_LABEL),
                          gState.instance,
                          nullptr);

            UpdateStatus();
            return 0;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_ADD_BUTTON) {
                AddConfiguredInput();
            } else if (LOWORD(wParam) == IDC_DETECT_BUTTON) {
                BeginDetectMode();
            }
            return 0;
        }

        case WM_DESTROY:
            StopDetectMode(true);
            StopAllLatchedState();
            UninstallHooks();
            PostQuitMessage(0);
            return 0;

        case WM_TIMER:
            if (wParam == kRepeatTimerId) {
                bool hasAnyLatched = false;
                const auto now = std::chrono::steady_clock::now();
                for (auto& binding : gState.bindings) {
                    if (!binding.keyLatched) {
                        continue;
                    }

                    hasAnyLatched = true;
                    if (now >= binding.nextRepeatAt) {
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
                } else {
                    std::wstring text = L"Press input... (" + std::to_wstring(gState.detectSecondsRemaining) + L")";
                    SetWindowTextW(gState.detectButton, text.c_str());
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

    gState.window = CreateWindowExW(0,
                                    kWindowClassName,
                                    L"Key Toggler",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    620,
                                    270,
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
