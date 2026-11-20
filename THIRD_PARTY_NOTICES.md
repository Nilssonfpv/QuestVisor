# Third-party components

## In the Quest app (APK)

| Component | How it is included | License |
|---|---|---|
| Meta Spatial SDK 0.15.0 (`com.meta.spatial:*`, incl. its native libraries and bundled zlib) | Maven dependency, compiled into the APK | Meta Platform Technologies SDK License Agreement, https://developer.oculus.com/licenses/oculussdk/ |
| AndroidX, Jetpack Compose, Kotlin standard library, kotlinx.coroutines | Maven dependencies | Apache License 2.0 |
| LLVM libc++ (`libc++_shared.so`, from the Android NDK) | packaged in the APK | Apache License 2.0 with LLVM exceptions |

## In the relay (QuestVisor-Relay.exe)

| Component | License |
|---|---|
| Python 3.11 runtime and standard library (bundled by PyInstaller) | Python Software Foundation License |
| PyInstaller bootloader | GPL 2.0 with the PyInstaller bootloader exception (allows any license for the bundled program) |

## Artwork

The icon, banners and listing artwork in `store-assets/` were drawn for this project; no third-party artwork.
The wordmark text is rendered with the Windows font Segoe UI.
