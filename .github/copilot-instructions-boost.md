# Boost Library Vendoring

## Current State
- **Archive**: `app/src/main/cpp/boost-minimal.tar.xz` (89MB compressed)
- **Contains**: Full boost 1.83.0 distribution (147 libraries)
- **Extracted size**: 626MB

## Why Full Distribution?

Initially attempted to reduce the archive to only the 79 libraries needed at build time:
- algorithm, asio, crc, date_time, dll, filesystem, interprocess, log, program_options, range, regex, scope_exit, serialization, signals2, system, thread, utility, uuid, and 61 transitive dependencies

However, this caused runtime crashes with `boost::wrapexcept` exceptions, indicating that additional libraries are required at runtime beyond the build-time dependencies. To ensure stability, the full boost distribution is now used.

## Build Integration

The `cmake/Boost.cmake` file automatically:
1. Checks if `boost-minimal.tar.xz` exists at `app/src/main/cpp/boost-minimal.tar.xz`
2. Extracts it to `app/src/main/cpp/boost/`
3. Falls back to downloading full boost from GitHub if archive not found

No manual extraction needed - the build system handles it automatically.

## Modifications

Currently, there are no custom modifications to boost. All changes are tracked in git.
If modifications are needed in the future:
1. Extract boost-minimal.tar.xz
2. Make modifications
3. Commit only the changed files (not the entire boost directory)
4. The build will extract the archive first, then git will overlay the modifications
