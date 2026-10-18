# Get the code, build, and run seekdb

## Prerequisites

Install the supported compiler and dependencies described in [Install the toolchain](toolchain.md). A full source build requires substantial disk space and memory.

## Clone the repository

```bash
git clone https://github.com/oceanbase/seekdb.git
cd seekdb
```

## Build the production binary

The repository's `build.py` downloads the Bazel version pinned in `.bazelversion` and runs the requested command. The default build uses `-O2` and includes debug information.

```bash
./build.py init
./build.py build //src/observer:seekdb
```

`init` prepares platform dependencies; run it on the first build or after dependency metadata changes. For incremental builds, repeat only the `build` command. The resulting binary is `build_bazel/bin/src/observer/seekdb`.

On Windows, use `python build.py` with the same commands. For C++ test targets, see [Write and run unit tests](unittest.md). When you change Rust dependencies, commit the updated `rust/Cargo.lock`.

## Set build concurrency

Bazel defaults to the host's CPU count. Put Bazel options after the command:

```bash
./build.py build --jobs=16 //src/observer:seekdb
```

## Use remote C++ compilation

Specify your REAPI service address and concurrency:

```bash
./build.py build --config=remote --remote_executor=grpc://HOST:PORT --jobs=100 //src/observer:seekdb
```

C++ compilation runs remotely. Cargo, generators, and linking run locally. If the service requires an SSH tunnel, establish the tunnel separately.

## Build a Sanity binary

On Linux, enable Sanity instrumentation with:

```bash
./build.py build --sanity=true //src/observer:seekdb
```

The binary is written to the same output path. To keep a separate Sanity build directory, place `--build-dir build_sanity` before `build`; its binary will be `build_sanity/bin/src/observer/seekdb`.

## Reuse build caches

Bazel binaries and the default action cache are shared between checkouts. Their base directory is `~/.cache/bazel` on Linux, `~/Library/Caches/bazel` on macOS, or `%LOCALAPPDATA%/bazel` on Windows. Linux honors `XDG_CACHE_HOME`. Bazel manages its shared repository download cache separately.

To choose cache locations, put `--launcher-cache-dir PATH` before the command for the Bazel binary cache. Put `--disk_cache=PATH` or `--repository_cache=PATH` after the command for build actions or downloaded dependencies. For example:

```bash
./build.py --launcher-cache-dir /data/bazel build --disk_cache=/data/bazel-actions //src/observer:seekdb
```

To change dependency download settings, use Bazel's `--registry=URL` or `--downloader_config=PATH` after the command.

## Build for another platform

Follow the [Android](android.md), [iOS](ios.md), or [WebAssembly](webassembly.md) guide for dependency setup and target names. Android uses the server target above. iOS builds a framework, and WebAssembly builds an engine and SQL shell.

## Run a local instance

Prepare an isolated deployment and start seekdb through the repository's `obd.sh` wrapper:

```bash
./tools/deploy/obd.sh prepare -p /tmp/obtest
./tools/deploy/obd.sh deploy -c ./tools/deploy/single.yaml
```

Read `mysql_port` from `tools/deploy/single.yaml`. When the generated port is `10000`, connect with either client:

```bash
mysql -h127.0.0.1 -P10000 -uroot
./deps/3rd/u01/obclient/bin/obclient -h127.0.0.1 -P10000 -uroot -Doceanbase -A
```

Use the actual generated port if it differs.

## Stop and remove the local deployment

```bash
./tools/deploy/obd.sh destroy --rm -n single
```

This command stops the local instance and removes the deployment data created by this example. Do not point it at data that must be retained.
