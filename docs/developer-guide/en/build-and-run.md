# Get the code, build, and run seekdb

## Prerequisites

Install the supported compiler and dependencies described in [Install the toolchain](toolchain.md). A full source build requires substantial disk space and memory.

## Clone the repository

```bash
git clone https://github.com/oceanbase/seekdb.git
cd seekdb
```

## Build the production binary

The repository's `build.py` installs the pinned Bazel and runs the requested command. The default build uses `-O2` and includes debug information.

```bash
./build.py init
./build.py build //src/observer:seekdb
```

`init` prepares platform dependencies; run it on the first build or after dependency metadata changes. For incremental builds, repeat only the `build` command. The resulting binary is `build_bazel/bin/src/observer/seekdb`.

On Windows, use `python build.py` with the same commands. See [Bazel build](../../../bazel/README.md) for concurrency, remote compilation and cross compilation. For C++ test targets, see [Write and run unit tests](unittest.md).

## Build a Sanity binary

On Linux, enable Sanity instrumentation with:

```bash
./build.py build --sanity=true //src/observer:seekdb
```

The binary is written to the same output path. To keep a separate Sanity build directory, place `--build-dir build_sanity` before `build`; its binary will be `build_sanity/bin/src/observer/seekdb`.

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
