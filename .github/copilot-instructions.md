# Copilot Instructions for Ananbox Project

This file contains important information for AI agents working on this project.

## Project Structure

- **Main Application**: Android app in `/app` directory
  - Kotlin source: `app/src/main/java/com/github/ananbox/`
  - Native C++ code: `app/src/main/cpp/`
  - Resources: `app/src/main/res/`

- **Vendored Dependencies**:
  - `app/src/main/cpp/anbox/` - Anbox container runtime
  - `app/src/main/cpp/boost-minimal.tar.xz` - Minimal Boost C++ libraries (20MB archive, auto-extracted during build)
  - `app/src/main/cpp/proot/` - PRoot for rootless containers
  - `app/src/main/cpp/talloc/` - Talloc memory allocator
  - `app/src/main/cpp/scrcpy-server/` - scrcpy server for screen mirroring

## Build System

- Uses Gradle for Android builds
- CMake for native C++ compilation
- NDK version: 25.1.8937393
- Target SDK: 28 (lowered to bypass W^X noexec restrictions)

### Boost Libraries

The boost library is vendored as `app/src/main/cpp/boost-minimal.tar.xz` (89MB archive) containing the full boost 1.83.0:
- **Extraction**: Automatic during CMake configuration (via `cmake/Boost.cmake`)
- **Location**: `app/src/main/cpp/boost-minimal.tar.xz` → extracted to `app/src/main/cpp/boost/`
- **Size**: 626MB extracted, 89MB compressed
- **Modifications**: Any custom changes should be committed separately; build extracts archive first, then overlays git changes

See `.github/copilot-instructions-boost.md` for details on boost vendoring strategy.

### Building the Standalone Server for Termux

The server (`libanbox.so`) is built as a Position Independent Executable (PIE) using the Android NDK toolchain. Requirements:
- Android NDK 25.1+
- CMake 3.22+

Build steps:

```bash
cd app/src/main/cpp

# Set up NDK environment
export ANDROID_NDK=/path/to/android-ndk
export ANDROID_ABI=arm64-v8a  # or x86_64

mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=$ANDROID_ABI \
      -DANDROID_PLATFORM=android-24 \
      -DBUILD_ANANBOX_SERVER=ON \
      ..
make anbox
```

This builds `libanbox.so` (the server) and `proot` binaries.

### Installing in Termux

```bash
# On development machine
adb push build/libanbox.so /data/local/tmp/
adb push build/proot /data/local/tmp/

# In Termux
cp /data/local/tmp/libanbox.so $PREFIX/bin/
cp /data/local/tmp/proot $PREFIX/bin/
chmod +x $PREFIX/bin/libanbox.so $PREFIX/bin/proot
```

## Server Options

Full list of `./libanbox.so` command-line options:

- `-a, --address <ip>`: Listen address (default: 0.0.0.0)
- `-p, --port <port>`: Listen port (default: 5558)
- `-w, --width <pixels>`: Display width (default: 1280)
- `-h, --height <pixels>`: Display height (default: 720)
- `-d, --dpi <dpi>`: Display DPI (default: 160)
- `-b, --base <path>`: Base path (parent of rootfs)
- `-P, --proot <path>`: Path to proot binary
- `-A, --adb-address <ip>`: ADB listen address (default: same as --address)
- `-D, --adb-port <port>`: ADB listen port (default: 5555, 0 to disable)
- `-S, --adb-socket <path>`: ADB socket path in rootfs (default: /dev/socket/adbd)
- `-l, --logcat <dest>`: Container logcat output (file path, stdout, stderr, /dev/null)
- `-v, --verbose`: Enable verbose logging

## Streaming Protocol

The server uses a custom TCP streaming protocol:
- **Graphics**: Raw RGBA8888 frame data
- **Audio**: PCM audio samples
- **Input**: Touch, keyboard, and mouse events
- **Control**: Handshake, ping/pong, disconnect

## Debugging

### Log File Locations

- `proot.log`: `<filesDir>/proot.log`
- `system.log`: `<filesDir>/rootfs/data/system.log`
- `container.logcat`: `<filesDir>/container.logcat` (when using embedded server)
- `server.log`: `<filesDir>/server.log` (embedded server output)

### Log Export Contents

When exporting logs, the tarball includes:
- proot.log
- system.log
- container.logcat (container's logcat output)
- logcat output (host Android logcat)
- device information
- process list
- settings configuration


## Container Logcat Feature

### Server Implementation (C++)
- File: `app/src/main/cpp/anbox/src/server/main.cpp`
- The server supports `-l` / `--logcat` option to capture container logcat output
- Destinations: file path, `stdout`, `stderr`, or `/dev/null`
- Implementation: Forks a child process that runs `proot -r rootfs /system/bin/logcat`
- The logcat output is redirected to the specified destination using file descriptors

### Usage Examples
```bash
# Write container logcat to a file
./libanbox.so -b ~/ananbox -P proot -l ~/container.logcat

# Display on stderr
./libanbox.so -b ~/ananbox -P proot -l stderr

# Display on stdout (interleaved with normal output)
./libanbox.so -b ~/ananbox -P proot -l stdout

# Discard logcat output
./libanbox.so -b ~/ananbox -P proot -l /dev/null
```

### App Integration (Kotlin)
- File: `app/src/main/java/com/github/ananbox/MainActivity.kt`
- Embedded server mode automatically passes `-l container.logcat` to capture logs
- Output is saved to `filesDir/container.logcat`

### Log Viewing
- File: `app/src/main/java/com/github/ananbox/LogViewActivity.kt`
- Displays `container.logcat` in the log viewer along with other logs
- Container logcat is included in exported log archives

## Container Startup and PROOT_LOADER

### Critical Environment Variables
The container requires the `PROOT_LOADER` environment variable to be set for proper startup on Android systems with noexec restrictions on `/data` partitions.

### Rootfs Structure
- The rootfs is distributed as a separate archive: https://github.com/Ananbox/ananbox/releases
- Contains `run.sh` script at `<base>/rootfs/run.sh`
- The `run.sh` script expects: `$1 = base_path` (parent of rootfs), `$2 = proot_path`

### Server Implementation (C++)
- File: `app/src/main/cpp/anbox/src/server/main.cpp`
- Before calling `run.sh`, the server MUST set:
  - `PROOT_TMP_DIR` - Points to writable temp directory
  - `PROOT_LOADER` - Points to `libproot-loader.so` in the same directory as proot
- The loader path is typically: `<proot_dir>/libproot-loader.so`
- Without PROOT_LOADER, proot will fail on Android due to noexec restrictions

### App Implementation (Kotlin)
- File: `app/src/main/java/com/github/ananbox/MainActivity.kt`
- Sets PROOT_LOADER from `nativeLibraryDir/libproot-loader.so`
- The native library directory has execute permissions, allowing the loader to run

### JNI Implementation (C++)
- File: `app/src/main/cpp/libanbox.cpp`
- Sets PROOT_LOADER before starting container via proot
- Uses the native library directory for the loader path

## Code Style

### AI-Generated File Headers
All files created by Copilot since commit f00dad7 should include this header:

For Kotlin/Java files:
```kotlin
/*
 * This file was generated by AI and is not subject to copyright protection.
 * AI-generated content is not applicable for copyright.
 */
```

For C++ files:
```cpp
/*
 * This file was generated by AI and is not subject to copyright protection.
 * It is dedicated to the public domain.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.
 */
```

For YAML/script files:
```yaml
# This file was generated by AI and is not subject to copyright protection.
# AI-generated content is not applicable for copyright.
```

## Testing

- No comprehensive test suite exists yet
- Manual testing required:
  - Build: `./gradlew assembleDebug`
  - Kotlin compilation: `./gradlew compileDebugKotlin`
  - Native code: `./gradlew :app:externalNativeBuildDebug`

## Important Notes

- This is a fork of Anbox adapted to run on Android rootlessly
- Uses PRoot for storage isolation
- Server can run standalone in Termux or embedded in the app
- Container logcat captures logs from inside the container (not host Android logcat)
