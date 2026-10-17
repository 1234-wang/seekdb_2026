# Build seekdb for WebAssembly

Compile seekdb with Emscripten into a WebAssembly engine that runs inside a browser page, together with its JavaScript API and a browser SQL shell.

## Prerequisites

- macOS or Linux host
- Emscripten 6.0.12, the version in `tools/wasm/emscripten-version`, with its `emcc` on `PATH`, for example from `brew install emscripten`
- `rustup`, CMake 3.20 or later, Python 3.12 or later and `curl` on `PATH`
- Node.js 24 or later to run the tests
- At least 20 GB of free disk space recommended for dependencies, the SDK and build directories

## Build

Activate the pinned Emscripten SDK, prepare dependencies, then build with Bazel:

```bash
# emcc must come from the pinned Emscripten SDK.
export EMSCRIPTEN=/path/to/emsdk/upstream/emscripten
export PATH="$EMSCRIPTEN:$PATH"
export SEEKDB_WASM_DEPS="$PWD/build_wasm_deps"

# Reuse deps/3rd if it is already initialized.
python3 bazel.py init
rustup toolchain install "$(cat tools/wasm/rust-toolchain-version)" \
  --profile minimal --component rust-src --target wasm32-unknown-emscripten
bash tools/wasm/build-deps.sh "$SEEKDB_WASM_DEPS"
python3 tools/wasm/prepare-headers.py \
  --source deps/3rd/usr/local/oceanbase/deps/devel/include \
  --include "$SEEKDB_WASM_DEPS/include" --target-prefix "$SEEKDB_WASM_DEPS"
python3 bazel.py build --config=wasm //src/wasm:database
```

`init` prepares host tools and headers; `build-deps.sh` builds Wasm third-party libraries from pinned sources. Once dependencies are ready, repeat only the final Bazel command with these environment variables set. Bazel builds the Rust libraries; Wasm-specific patches apply only to private dependency copies in the build directory. SDK selection follows the Bazel target platform; `SEEKDB_WASM_DEPS` only locates the Wasm SDK and does not affect native or Android builds.

The engine and its JS, HTML and CSS assets are in `build_bazel/bin/src/wasm/dist/`. `python3 bazel.py clean` clears Bazel build outputs and preserves `build_wasm_deps`.

## Build directory contents

- `seekdb_wasm_database.mjs` and `seekdb_wasm_database.wasm`: the engine, its Rust libraries and third-party libraries linked into one module, with Emscripten's JavaScript runtime
- `database.mjs`: the JavaScript API, with the `database-worker.mjs`, `worker-server.mjs`, `runtime-host.mjs`, `storage-cleanup-worker.mjs` and `mysql-*.mjs` modules it loads
- `shell.html`, `shell.css`, `shell.mjs` and `shell-*.mjs`: the browser SQL shell
- `engine-version.mjs`: the engine version shown by the shell

Serve these files from `localhost` or over HTTPS with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`, and `.wasm` as `application/wasm`: the engine's threads need `SharedArrayBuffer`. Opened as `file://` URLs, they do not work.

## Use it in a page

Serve the shell from the build directory and open the URL it prints:

```bash
python3 tools/wasm/serve-shell.py --build-dir build_bazel/bin/src/wasm/dist
```

The shell's commands and storage modes are described in `docs/webassembly-shell.md`.

A page uses the engine through `Database`, which runs it in a Worker and speaks the MySQL protocol to it in memory:

```js
import {Database} from './database.mjs';

const db = await Database.open({
  moduleURL: new URL('./seekdb_wasm_database.mjs', import.meta.url),
  wasmURL: new URL('./seekdb_wasm_database.wasm', import.meta.url),
  storage: 'opfs',  // or 'memory', the default
});
const session = await db.connect();
const text = new TextDecoder();
for await (const event of session.query('SELECT VERSION()')) {
  if (event.kind === 'row') console.log(event.values.map(v => v === null ? null : text.decode(v)));
}
await session.close();
await db.close();
```

- `storage: 'memory'` keeps the data directory in Wasm memory and starts empty on every open. `storage: 'opfs'` keeps it in the `seekdb` directory of the origin private file system, needs OPFS and Web Locks, and can be open in only one page of an origin at a time.
- `query` yields `columns`, `row` and `complete` events. Row values are each column's text bytes as `Uint8Array`, or `null` for SQL NULL.
- Each `Database.open` starts a new Worker and engine. `close()` waits for the engine to shut down and keeps OPFS data; `discard()` terminates the Worker at once and, for OPFS, removes the stored files; `Database.clearPersistentStorage()` removes the stored OPFS files without opening an engine.
- No TCP port is opened: connect through `connect()`.
