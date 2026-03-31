The vendored `libwdi` payload is intentionally minimal:

- `include/` contains upstream `v1.5.1` headers.
- `lib/x64/libwdi.lib` is the existing Windows x64 static library.
- `lib/arm64/libwdi.lib` should be generated from upstream `v1.5.1` for native Windows ARM64 helper builds.

To regenerate the ARM64 library on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\packaging\windows\build-libwdi-arm64.ps1
```

That script:

- clones official `https://github.com/pbatard/libwdi.git` at tag `v1.5.1`
- writes a minimal MSVC `config.h` that enables only the ARM64 WinUSB path Xune uses
- patches the temporary `embedder.h` and `libwdi_i.h` checkout so upstream's MSVC code accepts an ARM64-only build
- copies `installer_arm64.exe` into the `arm64\Release\helper\` layout expected by upstream `embedder`
- builds `installer_arm64.exe` and `embedder.exe`
- generates `embedded.h`
- builds an ARM64 static `libwdi.lib`
- copies the result into `XuneSyncLibrary/vendor/libwdi/lib/arm64/libwdi.lib`

The ARM64 build is intentionally narrower than upstream's full Windows matrix because Xune only uses `WDI_WINUSB` from `xune-driver-setup.exe`.
