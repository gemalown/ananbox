## Changelog

### Version 0.2.9

#### Activity Renaming
- **MainActivity → RendererActivity**: The container renderer is now clearly named `RendererActivity`
- **SettingsActivity → MainActivity**: The launcher activity is now `MainActivity` (settings/configuration screen)

#### Container Shutdown Improvements
- **Escalating shutdown signals**: Both JNI and local server shutdown use escalating signals (SIGINT → SIGTERM → SIGKILL)
  - First click/toggle: SIGINT (Ctrl+C) - allows graceful cleanup, init can kill child processes
  - Second click/toggle: SIGTERM - stronger termination request
  - Third click/toggle: SIGKILL - forceful immediate termination
- **Shutdown stays in settings**: All shutdown operations keep user in MainActivity (settings) instead of exiting the app
- **Local server toggle feedback**: Toggle stays ON if shutdown signal doesn't terminate the server; shows toast indicating which signal was sent

#### Mode Switching
- **No force stop on mode switch**: When switching between JNI, embedded server, remote streaming, or scrcpy modes, containers stay running until user manually shuts them down
- **Run multiple modes simultaneously**: This allows running multiple containers/connections at the same time

#### Server Improvements
- **`-?` shorthand for `--help`**: Server now accepts `-?` as shorthand for `--help` in command line arguments

#### License and Documentation
- **GPL v2 License**: Added LICENSE file (GPL v2) to root due to PRoot dependency
- **Fork documentation**: Added fork notice in README linking to original [Ananbox/ananbox](https://github.com/Ananbox/ananbox)
- **AI-generated content notice**: Documented that AI-generated content is not subject to copyright protection
- **Comprehensive About dialog**: Lists all dependencies (Anbox, PRoot, Boost, scrcpy-server, AndroidX, Apache Commons Compress, libpng, backward-cpp) with their respective licenses

#### Code Quality Improvements
- **Efficient asset size check**: Use `assetManager.openFd("scrcpy-server").length` instead of reading entire file into memory
- **Version constant**: Extracted `SCRCPY_VERSION = "3.3.3"` as a constant for easier maintenance
- **Proper synchronization**: Replaced `Thread.sleep(500)` with `CountDownLatch` for waiting on scrcpy server readiness
- **Local server toggle sync**: Toggle state syncs with actual server state on settings resume

### Version 0.2.0

#### New Features
- **Standalone Server Mode**: Run containers on a separate machine and stream to clients
- **Remote Client Mode**: Connect to a remote Ananbox server for streaming
- **ADB Forwarding**: Forward ADB connections from TCP port to container's ADB socket
- **scrcpy Support**: Connect via ADB for better performance with scrcpy
- **Log Viewer**: View proot and system logs directly in the app
- **Log Export**: Export diagnostic logs as a compressed tarball for debugging
- **Verbose Mode**: Enable detailed logging for troubleshooting
- **Self-contained Build**: Vendored all dependencies (anbox, boost, proot) for reproducible builds

#### Bug Fixes
- Fixed embedded server mode container startup by using `PROOT_LOADER` to point to pre-built loader binary
- Fixed noexec filesystem issue by bundling proot loader in native library directory (which has exec permission)
- Use symlinks instead of copying binaries from nativeLibraryDir to filesDir/bin (saves disk space)
- Detect and recreate dangling symlinks after app upgrade
- Fixed local JNI mode container startup by properly setting `PROOT_TMP_DIR` environment variable
- Fixed blank black screen issue by ensuring required directories exist before container starts
- Improved symlink handling during rootfs extraction with better validation
- Fixed symlink creation to properly handle relative paths

#### New Features
- Added scrcpy-server v3.3.3 for scrcpy connection support
- Added AdbHelper class for pushing files and executing commands via ADB protocol
- Support for pushing scrcpy-server to container and starting it via adb

#### Build Improvements
- Vendored proot source code to make the project self-contained
- Disabled proot Python extension for Android compatibility
- Suppressed excessive C/C++ warnings during compilation
- Lowered targetSdk to 28 to bypass W^X noexec restriction (like Termux on F-Droid)

### Version 0.1.0 (Original)
- Initial fork from Anbox
- Basic container functionality with proot isolation
- Graphics support via forked anbox renderer

## Preview
