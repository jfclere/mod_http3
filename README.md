# mod_http3 -- HTTP/3 module for Apache httpd

An Apache httpd module that adds HTTP/3 support over QUIC.

Status: **experimental**.


## Table of Contents

- [Dependencies](#dependencies)
- [Quick Start](#quick-start)
- [CMake Options](#cmake-options)
- [Project Layout](#project-layout)
- [Testing with Chrome](#testing-with-chrome)
- [httpd.conf Example](#httpdconf-example)
- [License](#license)



## Dependencies

By default all core dependencies (OpenSSL, httpd, APR/APR-util, nghttp3, googletest) are built from git submodules (`VENDOR_SYSTEM=ON`). 

see [CONFIGURATION.md](CONFIGURATION.md) for using system-installed packages.



## Quick Start

```sh
git clone https://github.com/machine-moon/mod_http3.git
cd mod_http3
git submodule sync --recursive
git submodule update --init --recursive
cmake -B build
cmake --build build
cmake --build build --target tests
```

The first build compiles OpenSSL, APR, APR-util, and httpd from submodules (approximately 10 minutes by default). Subsequent builds reuse the cached result.

To speed up the vendored dependency builds, override the parallel job count:

```sh
cmake -B build -DDEPENDENCIES_PARALLEL=12
```

Output:

- `build/lib/mod_http3.so` -- Apache module
- `build/bin/mod_http3_tests` -- test binary


## CMake Options

Variables are passed with `-D` at configure time:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
```

CMake boolean options accept `ON`/`OFF`, `YES`/`NO`, `TRUE`/`FALSE`, or `1`/`0`.

See the [CMake documentation](https://cmake.org/cmake/help/latest/command/if.html#constant) for the full list of accepted values.

| Option                    | Default   | Description                                              |
|---------------------------|-----------|----------------------------------------------------------|
| `CMAKE_BUILD_TYPE`        | `Release` | `Debug` / `Release` /                                    |
| `BUILD_MODULE`            | `ON`      | Build `mod_http3.so`                                     |
| `BUILD_EXAMPLES`          | `ON`      | Build example programs                                   |
| `BUILD_TESTS`             | `ON`      | Build Google Test suite                                  |
| `ENABLE_ASAN`             | `OFF`     | Address Sanitizer (requires `Debug`)                     |
| `ENABLE_UBSAN`            | `OFF`     | UB Sanitizer (requires `Debug`)                          |
| `ENABLE_WERROR`           | `OFF`     | Treat warnings as errors                                 |
| `VENDOR_SYSTEM`           | `ON`      | Build deps from submodules; `OFF` to use system packages |
| `DEPENDENCIES_PARALLEL`   | `6`       | Parallel jobs for vendored autotools builds              |

See [CONFIGURATION.md](CONFIGURATION.md) for full details, path options,
and examples.



## Project Layout

```
mod_http3/
  include/              Module headers (ossl-nghttp3.h, version.h)
  src/                  Module source (mod_h3.c, ossl-nghttp3.c)
  examples/             Standalone example programs
  tests/                Google Test suite
cmake/
  modules/system/       Finders for OpenSSL, httpd, APR, APU
  modules/external/     Finders for nghttp3, googletest
  utils/                Compiler flags, helpers
dependencies/           Git submodules + vendored build outputs (*-dist/)
scripts/                Helper scripts
```



## Testing with Chrome

```sh
google-chrome \
    --user-data-dir=/tmp/quic_dev \
    --ignore-certificate-errors \
    --origin-to-force-quic-on=localhost:4433 \
    https://localhost:4433
```



## httpd.conf Example

```apache
LoadModule http3_module modules/mod_http3.so

Listen 4433 https
EnableMMAP Off

<VirtualHost *:4433>
    ServerName localhost:4433

    Protocols http/1.1
    ProtocolsHonorOrder on
    SSLEngine on
    SSLCertificateFile    "/path/to/localhost.crt"
    SSLCertificateKeyFile "/path/to/localhost.key"

    H3CertificatePath    "/path/to/localhost.crt"
    H3CertificateKeyPath "/path/to/localhost.key"

    Header set alt-svc "h3=\":4433\"; ma=60; persist=1"
</VirtualHost>
```

See [CONFIGURATION_HTTPD.md](CONFIGURATION_HTTPD.md) for directive
reference and VirtualHost configuration details.



## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
