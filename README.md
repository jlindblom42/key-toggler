# key-toggler

A lightweight Windows GUI app that latches configured keys or mouse buttons as "held down" when you double tap them.

## Behavior

- Supported typed names include keyboard keys like `SHIFT`, `ALT`, `CTRL`, arrows, and mouse buttons like `MOUSE1`-`MOUSE5`.
- Click **Add New Key** and then press a key/button once within 5 seconds to capture it.
- While waiting for input, the **Add New Key** button is disabled and shows the remaining countdown.
- Each configured key/button behaves independently:
  - Double tap it quickly (within its configured window, default 300 ms) to latch it down.
  - Tap or hold that same key/button once to release only that key/button latch.
- Configured bindings in the list show `[ON]` when latched and `[OFF]` when idle.
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


## Versioning and releases

- Semantic versioning is tracked in `VERSION` (starting at `1.0.0`).
- On every push to `main` (including merged PRs), GitHub Actions increments the patch version, commits it, builds `key_toggler.exe`, and publishes a GitHub Release with the executable attached.
