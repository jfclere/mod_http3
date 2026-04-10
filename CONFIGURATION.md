# Build Configuration

This document covers how to configure, build, and test mod_http3.

For httpd runtime directives (`H3CertificatePath`, VirtualHost setup, etc.)
see [CONFIGURATION_HTTPD.md](CONFIGURATION_HTTPD.md).

---

## Table of Contents

- [Quick Start](#quick-start)
- [Build Commands](#build-commands)
- [CMake Options](#cmake-options)
- [Vendored Build](#vendored-build)
- [System Packages Build](#system-packages-build)
- [Sanitizers](#sanitizers)
- [Build Outputs](#build-outputs)

---

## Quick Start

```sh
git clone https://github.com/machine-moon/mod_http3.git
cd mod_http3
git submodule sync --recursive
git submodule update --init --recursive
cmake -B build
cmake --build build -j$(nproc)
cmake --build build -j$(nproc) --target tests
```

The first build with vendored dependencies (the default) compiles OpenSSL,
APR, APR-util, and httpd from submodules. This takes several minutes.
Subsequent builds reuse the cached result and are fast.

Build artifacts are written to `build/`:

- `build/lib/mod_http3.so` -- the Apache module
- `build/bin/mod_http3_tests` -- the test binary
- `build/bin/server`, `build/bin/quic_client_test`, etc. -- example programs

---

## Build Commands

| Command                                        | Description                              |
|------------------------------------------------|------------------------------------------|
| `cmake -B build`                               | Configure only                           |
| `cmake --build build -j$(nproc)`                          | Build everything                         |
| `cmake --build build -j$(nproc) --target tests`           | Build + run the test suite               |
| `cmake --build build -j$(nproc) --target package`         | Create distributable ZIP/TGZ             |
| `cmake -LH -N -B build`                        | Print all CMake cache variables          |

To use a different build directory, pass it to `-B`:

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```

---

## CMake Options

Variables are passed with `-D` at configure time:

```sh
cmake -B build -DVARIABLE=VALUE
```

CMake caches them after the first configure; pass them again to change a value.

CMake boolean options accept `ON`/`OFF`, `YES`/`NO`, `TRUE`/`FALSE`, or `1`/`0`
(all case-insensitive). See the
[CMake documentation](https://cmake.org/cmake/help/latest/command/if.html#constant)
for the full list of accepted values.

### Project Options

| Option            | Default   | Description                                                       |
|-------------------|-----------|-------------------------------------------------------------------|
| `CMAKE_BUILD_TYPE`| `Release` | `Debug`, `Release`                                                |
| `BUILD_MODULE`    | `ON`      | Build `mod_http3.so` Apache DSO                                   |
| `BUILD_EXAMPLES`  | `ON`      | Build example programs                                            |
| `BUILD_TESTS`     | `ON`      | Build Google Test suite                                           |
| `ENABLE_WERROR`   | `OFF`     | Treat compiler warnings as errors                                 |
| `ENABLE_ASAN`     | `OFF`     | Address Sanitizer (requires `Debug`)                              |
| `ENABLE_UBSAN`    | `OFF`     | Undefined Behavior Sanitizer (requires `Debug`)                   |
| `VENDOR_SYSTEM`   | `ON`      | Build OpenSSL, APR, APR-util, and httpd from submodules           |

### Dependencies Options

| Option                          | Default                     | Description                                       |
|---------------------------------|-----------------------------|---------------------------------------------------|
| `DEPENDENCIES_DIRECTORY`        | `dependencies/`             | Root directory for git submodules                 |
| `DEPENDENCIES_OUTPUT_DIRECTORY` | `${DEPENDENCIES_DIRECTORY}` | Install prefix for vendored dependencies          |
| `DEPENDENCIES_PARALLEL`         | `6`                         | Parallel jobs for vendored autotools builds       |

### Examples

```sh
# Debug build with Address Sanitizer
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build -j$(nproc)

# Build tests only (no Apache module, no examples)
cmake -B build -DBUILD_MODULE=OFF -DBUILD_EXAMPLES=OFF
cmake --build build -j$(nproc)

# Use system-installed OpenSSL and httpd instead of submodules
cmake -B build -DVENDOR_SYSTEM=OFF
cmake --build build -j$(nproc)

# Speed up the initial vendored build with more parallel jobs
cmake -B build -DDEPENDENCIES_PARALLEL=12
cmake --build build -j$(nproc)

# Clean rebuild with UBSan
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON
cmake --build build -j$(nproc) --target tests
```

---

## Vendored Build

When `VENDOR_SYSTEM=ON` (the default), CMake builds OpenSSL, APR,
APR-util, and httpd from their respective git submodules at configure
time. Results are installed into `dependencies/<dep>-dist/` and cached
with sentinel files (`dependencies/<dep>-dist/.done`).

Submodules must be initialized first:

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

If you need to reset submodules to a clean state:

```sh
git submodule foreach --recursive 'git reset --hard || :'
git submodule foreach --recursive 'git clean -ffdx || :'
```

Build order enforced by CMake:

1. OpenSSL -> `dependencies/openssl-dist/`
2. APR -> `dependencies/apr-dist/`
3. APR-util -> `dependencies/apr-util-dist/`
4. httpd -> `dependencies/httpd-dist/`

To force a rebuild of a vendored dependency, delete its sentinel:

```sh
rm dependencies/openssl-dist/.done
cmake -B build
```

nghttp3 and googletest are always built from submodules regardless of
`VENDOR_SYSTEM`.

---

## System Packages Build

When `VENDOR_SYSTEM=OFF`, CMake searches `PATH` for the required tools
and uses `find_package(OpenSSL)`.

Required tools on `PATH`:

| Tool               | Minimum                          | Provides               |
|--------------------|----------------------------------|------------------------|
| `apxs` / `apxs2`  | httpd >= 2.4.x, MMN >= 20211221  | httpd headers, config  |
| `apr-1-config`     | APR >= 1.7.0                     | APR headers, link flags|
| `apu-1-config`     | APU >= 1.6.0                     | APU headers, link flags|
| OpenSSL            | >= 3.5.0                         | QUIC support           |

System httpd packages (Ubuntu, Fedora, etc.) ship AP24 (MMN < 20211221)
and will be rejected at configure time. You need httpd built from `trunk`.

If your builds are in non-standard prefixes, export their `bin/` directories:

```sh
export PATH="/opt/httpd/bin:/opt/openssl/bin:$PATH"
cmake -B build -DVENDOR_SYSTEM=OFF
cmake --build build -j$(nproc)
```

---

## Sanitizers

Address Sanitizer and Undefined Behavior Sanitizer both require
`CMAKE_BUILD_TYPE=Debug`. The build will fail if sanitizers are enabled
with a non-Debug build type.

```sh
# ASan
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build -j$(nproc) --target tests

# UBSan
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON
cmake --build build -j$(nproc) --target tests
```

---

## Build Outputs

| Path                          | Contents                             |
|-------------------------------|--------------------------------------|
| `build/lib/mod_http3.so`      | Apache module (`LoadModule`)         |
| `build/bin/mod_http3_tests`   | Google Test binary                   |
| `build/bin/server`            | Standalone QUIC/HTTP3 server         |
| `build/bin/quic_client_test`  | QUIC client test tool                |
| `build/bin/biomemexample`     | OpenSSL BIO memory example           |
| `build/bin/apr_miniserver`    | APR socket server example            |
| `build/compile_commands.json` | Compilation database for editors     |

To install to a prefix after building:

```sh
cmake --install build --prefix /opt/mod_http3
```

The `--prefix` flag sets the installation root at install time and does not
require a reconfigure. Installed layout under the prefix:

```
lib/mod_http3.so
bin/mod_http3_tests
share/mod_http3/docs/    (CHANGES, LICENSE, NOTICE)
```
