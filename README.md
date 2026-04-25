# key-toggler

A lightweight Windows GUI app that latches a configured key as "held down" when you double tap it.

## Behavior

- Configure a single key (default: `T`).
- Double tap the configured key quickly (within 300 ms) to latch it down.
- Tap or hold the configured key once to release the latch.

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
