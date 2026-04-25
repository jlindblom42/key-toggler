# key-toggler

A lightweight Windows GUI app that latches a configured key or mouse button as "held down" when you double tap it.

## Behavior

- Configure a trigger input (default: `T`).
- Supported typed names include keyboard keys like `SHIFT`, `ALT`, `CTRL`, arrows, and mouse buttons like `MOUSE1`-`MOUSE5`.
- You can also click **Detect (5s)** and press a key/button once within 5 seconds to configure it.
- Double tap the configured input quickly (within the configured window, default 300 ms) to latch it down.
- Tap or hold the configured input once to release the latch.

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
