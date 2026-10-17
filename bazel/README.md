# Bazel build

```sh
./build.py init
./build.py build //src/observer:seekdb
```

On Windows, use `python build.py` with the same commands. `init` selects the
host's dependency initializer; `build`, `test`, `query`, `clean` and other commands
are passed to Bazel. CMake configurations, where still needed, use `cmake`
directly.

`build.py` downloads the version in `.bazelversion`, using the platform URL and
SHA256 in `bazel/binaries.json`. It then runs Bazel. Build flags belong after the
Bazel command; local job concurrency defaults to Bazel's CPU-based value.

```sh
./build.py build --jobs=16 //src/observer:seekdb
./build.py build --config=remote --remote_executor=grpc://HOST:PORT --jobs=100 //src/observer:seekdb
```

The remote configuration sends C++ compilation to REAPI. Cargo, generators and
linking run locally. Set up an SSH tunnel separately if the service requires one.

- `.bazelrc`: shared build settings and named platform configurations.
- `bazel/platforms.bzl`: target platform facts and compilation differences.
- `bazel/toolchains/cc/`: compiler/SDK discovery and toolchain definitions.
- `bazel/cargo.bzl`: Cargo actions and exported C++ archives/headers.
- `bazel/downloader.cfg`: the single dependency download route (GH-Proxy).

Use native Bazel options such as `--disk_cache=PATH`, `--repository_cache=PATH`,
`--registry=URL` and `--downloader_config=PATH` to override defaults. The launcher
only has `--build-dir` and `--launcher-cache-dir`, placed before the command.

Outputs are under `build_bazel/`; the binary is
`build_bazel/bin/src/observer/seekdb`. The launcher cache is `~/.cache/bazel` on
Linux (`XDG_CACHE_HOME` is honored), `~/Library/Caches/bazel` on macOS, and
`%LOCALAPPDATA%/bazel` on Windows. Bazel binaries and the default action disk cache
are shared between checkouts. Bazel manages its own shared repository cache.

Cross compilation uses `--config=android`, `--config=wasm`, `--config=ios` or
`--config=ios-simulator`. See the [Android](../docs/developer-guide/en/android.md),
[WebAssembly](../docs/developer-guide/en/webassembly.md) and
[iOS](../docs/developer-guide/en/ios.md) setup instructions. iOS and WebAssembly
have dedicated framework/shell targets; the server target above is for native
Linux/macOS and Android builds.

Cargo uses the installed Rust version pinned in `rust/rust-toolchain.toml`
(and the separate Wasm pin). Bazel declares the compiler, Python runtime and
locked crate sources as inputs. Dependency fetching uses Cargo's configured
source/cache; compilation runs offline in a sandbox with a private Cargo home.
Commit changes to `rust/Cargo.lock` together with dependency changes.

Each target platform selects its SDK independently: host tools stay in
`deps/3rd`, Android libraries in `deps/android`, and iOS SDKs in `deps/ios`.
The Wasm dependency setup also prepares its Emscripten system libraries under
`build_wasm_deps/em-cache`; Bazel uses that cache read-only.
