# XNA Deploy Test CLI

## Prerequisites

**macOS:**
- CMake 3.10+, OpenSSL (`brew install cmake openssl`)
- `cabextract` for .ccgame packages (`brew install cabextract`)

**Linux:**
- CMake 3.10+, OpenSSL, libusb, libudev
- `apt install cmake libssl-dev libusb-1.0-0-dev libudev-dev cabextract`

**Both platforms:**
- MTPZ keys at `~/.mtpz-data`

## Build

```bash
git clone --recursive <repo-url>
cd XuneSyncLibrary
cmake -S . -B build -DZUNE_BUILD_TOOLS=ON
cmake --build build
```

## Runtime DLLs

If the device doesn't already have the XNA runtime installed, you'll
need the Zune runtime DLLs (`xna_zune_runtime.zip`). Unzip them into
any directory and pass it at runtime:

```bash
unzip xna_zune_runtime.zip -d xna_zune_runtime/
./build/xna_deploy_test_cli game.ccgame --runtime-dir xna_zune_runtime/
```

## Run

Run from anywhere — the tool resolves paths relative to its own location:

```bash
./build/xna_deploy_test_cli <app.exe|package.ccgame> [options]
```

| Option | Description |
|---|---|
| `--name <name>` | Game title (default: filename) |
| `--description <desc>` | Game description |
| `--thumbnail <png>` | Thumbnail PNG file |
| `--runtime-dir <dir>` | Runtime DLLs directory override |
| `--launch` | Launch app on device after deploy |
| `--verbose` | Show protocol details |
