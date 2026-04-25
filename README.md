# key-toggler

A lightweight Windows GUI app that latches configured keys or mouse buttons as "held down" when you double tap them.

## Behavior

- Configure a trigger input (default text: `T`).
- Supported typed names include keyboard keys like `SHIFT`, `ALT`, `CTRL`, arrows, and mouse buttons like `MOUSE1`-`MOUSE5`.
- You can click **Detect (5s)** and press a key/button once within 5 seconds to fill the input box.
- Click **Add New Key** to add that key/button with the configured double-tap window.
- Each configured key/button behaves independently:
  - Double tap it quickly (within its configured window, default 300 ms) to latch it down.
  - Tap or hold that same key/button once to release only that key/button latch.
- Re-adding an existing key/button updates its timing.

## Build locally (Windows)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Executable output:

- `build/Release/key_toggler.exe`

## GitHub Actions build

This repository includes a Windows runner workflow at:

- `.github/workflows/build-windows.yml`

It builds the project and uploads `key_toggler.exe` as an artifact named `key-toggler-windows-exe`.
