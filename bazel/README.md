# Bazel build

```sh
./bazel.py init
./bazel.py build //src/observer:seekdb
```

`bazel.py` downloads the version in `.bazelversion`, using the platform URL and
SHA256 in `bazel/binaries.json`. It then runs Bazel. Build flags belong after the
Bazel command; local job concurrency defaults to Bazel's CPU-based value.

```sh
./bazel.py build --jobs=16 //src/observer:seekdb
./bazel.py build --config=remote --remote_executor=grpc://HOST:PORT --jobs=100 //src/observer:seekdb
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
