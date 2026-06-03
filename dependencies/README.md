# Dependencies

mod_http3 uses **git submodules** for all dependencies. The `BUILD_SSL` and `BUILD_HTTPD` CMake options control whether they are built from source or found on the system.

---

## Pinned dependency versions

| Dependency  | Submodule path              | Branch          | Version (current) | Notes                          |
|-------------|-----------------------------|-----------------|-------------------|--------------------------------|
| OpenSSL     | `dependencies/openssl`      | `openssl-3.5`   | 3.5.7-dev         | QUIC support required (≥ 3.5). |
| httpd       | `dependencies/httpd`        | `trunk`         | 2.5.1-dev         | AP25 API; requires MMN ≥ 20211221/30. |
| APR         | `dependencies/apr`          | `1.7.x`         | 1.7.7             | APR v2-dev (trunk) will subsume APR-util 1.x APIs. |
| APR-util    | `dependencies/apr-util`     | `1.6.x`         | 1.6.4             | Legacy companion library; kept for APR 1.x compatibility. |
| nghttp3     | `dependencies/nghttp3`      | `main`          | 1.15.x            | Always built from submodule.   |
| googletest  | `dependencies/googletest`   | `v1.17.x`       | 1.17.x            | Test-only; not shipped.        |

All submodules are shallow (`shallow = true`). Initialise them once:

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

nghttp3 and googletest are *external* dependencies, thus they are always built from their submodules regardless of build options. All others follow the mode below.

---

## Why httpd `trunk`, not `2.4.x`

Apache's **Module Magic Number (MMN)** encodes the ABI version of the httpd module API.

| Source                        | MMN major  | MMN minor | Cookie |
|-------------------------------|------------|-----------|--------|
| httpd `trunk` (2.5.1-dev)     | 20211221   | 30        | AP25   |
| httpd `2.4.x` release         | 20120211   | (varies)  | AP24   |
| Typical OS package (2.4.x)    | 20120211   | (varies)  | AP24   |

mod_http3 uses APR bucket types (`AP_BUCKET_IS_RESPONSE`, etc.) that were introduced in the AP25 API generation. Building against 2.4.x -- whether from the branch or from an OS package -- produces a binary that either fails to link or fails to load at runtime. Targeting `trunk` (AP25) resolves this.

> If this ever stabilises into a 2.5.x release series the submodule branch will be updated accordingly.

---

## Dependency resolution modes

### Mode 1 -- Build from source (`BUILD_SSL=ON`, `BUILD_HTTPD=ON`)

CMake builds OpenSSL, APR, APR-util, and httpd from their respective git submodules at **configure time**, installing each into `dependencies/<dep>-dist/`. A small marker file (`dependencies/<dep>-dist/.done`) is used to skip rebuilding dependencies that are already up to date.

**Build order enforced by CMake:**
1. OpenSSL (`dependencies/openssl`) -> `dependencies/openssl-dist/`
2. APR (`dependencies/apr`) -> `dependencies/apr-dist/`
3. APR-util (`dependencies/apr-util`) -> `dependencies/apr-util-dist/`
4. httpd (`dependencies/httpd`) -> `dependencies/httpd-dist/`

```sh
# default to build from source (first configure is slow; subsequent ones are instant from cache)
cmake -B build
cmake --build build -j$(nproc)
```

This is the recommended mode for development. Everything is self-contained under the repo.

**To force a clean rebuild of a dependency built from source**, delete its `-dist` dir and re-configure:

```sh
rm dependencies/openssl-dist/   # re-build OpenSSL
rm dependencies/httpd-dist/     # re-build httpd
cmake --build build
```

---

### Mode 2 -- System packages (`BUILD_SSL=OFF`, `BUILD_HTTPD=OFF`)

CMake uses `WITH_SSL=/path/to/openssl` and `WITH_HTTPD=/path/to/httpd` to locate system-installed dependencies. All must satisfy the minimum versions.

**Minimum requirements checked at configure time:**

| Tool / package | Minimum required |
|----------------|-----------------|
| OpenSSL        | ≥ 3.5.0         |
| httpd (via apxs) | ≥ 2.4.x AND MMN ≥ 20211221 |
| APR            | ≥ 1.7.0         |
| APU            | ≥ 1.6.0         |

> **OS package caveat:** System httpd packages (Ubuntu, Fedora, etc.) ship the 2.4.x AP24 generation (MMN < 20211221). CMake will `FATAL_ERROR` on the MMN check. Use build-from-source mode or a custom PATH install instead.

---

### Mixed mode (`BUILD_SSL=ON`, `BUILD_HTTPD=OFF` or vice versa)

You can mix building from source and system dependencies. For example, build OpenSSL from source but use a system httpd:

```sh
cmake -B build -DBUILD_SSL=ON -DBUILD_HTTPD=OFF -DWITH_HTTPD=/opt/httpd
```

Or use system OpenSSL but build httpd from source:

```sh
cmake -B build -DBUILD_SSL=OFF -DBUILD_HTTPD=ON -DWITH_SSL=/opt/openssl
```

---

### Patching dependencies built from source

If you need to patch OpenSSL, httpd, or APR/APU, apply the patch to the corresponding submodule and **remove any existing build output** before reconfiguring. Build-from-source mode will then rebuild from the patched sources.

```sh
cd dependencies/openssl
git apply /path/to/my.patch
cd ../..

# ensure the previous build output is discarded
rm -rf dependencies/openssl-dist

cmake -B build # External autotool builds happen at configuration time
```

The `.done` approach means you can also manually build and install or copy into `dependencies/<dep>-dist/` yourself and create the `.done` file; CMake will skip the automated build step and use whatever is there.

---

## Verifying a build-from-source install

```sh
# Confirm httpd version and MMN built from source
dependencies/httpd-dist/bin/apxs -q HTTPD_VERSION
dependencies/httpd-dist/bin/apxs -q HTTPD_MMN       # expect 20211221

# Confirm OpenSSL is the one httpd links
ldd dependencies/httpd-dist/modules/mod_ssl.so | grep ssl
# should show dependencies/openssl-dist/lib64/libssl.so, not /usr/lib/...
```
