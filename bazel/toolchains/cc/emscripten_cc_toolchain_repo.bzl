"""Expose an installed Emscripten SDK as a C++ toolchain."""

load(":repository_utils.bzl", "host_substitutions")

def _emscripten_cc_toolchain_repository_impl(ctx):
    emscripten = ctx.getenv("EMSCRIPTEN")
    if not emscripten:
        ctx.file("BUILD.bazel", "# Set EMSCRIPTEN to the directory containing emcc.\n")
        return
    root = ctx.path(emscripten).dirname
    ctx.symlink(root, "sdk")
    prefix = ctx.getenv("SEEKDB_WASM_DEPS") or str(ctx.workspace_root) + "/build_wasm_deps"
    cache = ctx.path(prefix + "/em-cache")
    # A native build may discover this toolchain before the Wasm SDK has
    # been prepared. Only Wasm compile/link actions require the frozen cache.
    ctx.watch(cache)
    ctx.symlink(cache, "cache")
    resource = ctx.execute([str(root.get_child("bin").get_child("clang")), "-print-resource-dir"])
    if resource.return_code:
        fail("Cannot inspect Emscripten Clang: " + resource.stderr)
    ctx.symlink(resource.stdout.strip() + "/include", "clang_builtin_include")
    includes = ["%crosstool_top%/" + path for path in [
        "cache/sysroot/include/c++/v1", "clang_builtin_include",
        "cache/sysroot/include", "cache/sysroot/include/compat",
    ]] + [str(cache) + "/sysroot/include", resource.stdout.strip() + "/include"]
    ctx.file("emcc.sh", """#!/bin/sh
export EM_FROZEN_CACHE=1
export EM_CACHE="$(cd "$(dirname "$0")" && pwd)/cache"
if [ ! -d "$EM_CACHE/sysroot" ]; then
  echo "Missing Emscripten sysroot; run python3 tools/wasm/build-deps.py for this Wasm SDK" >&2
  exit 1
fi
exec "$(dirname "$0")/sdk/emscripten/emcc" --sysroot="$(dirname "$0")/cache/sysroot" "$@"
""")
    ctx.template("BUILD.bazel", ctx.attr.build_file, substitutions = host_substitutions(ctx))
    ctx.template("toolchain_config.bzl", ctx.attr.config_file, substitutions = {
        "@INCLUDES@": repr(includes),
    })

emscripten_cc_toolchain_repository = repository_rule(
    implementation = _emscripten_cc_toolchain_repository_impl,
    attrs = {
        "build_file": attr.label(default = Label("//bazel/toolchains/cc:BUILD.emscripten_cc_toolchain.tpl")),
        "config_file": attr.label(default = Label("//bazel/toolchains/cc:emscripten_toolchain_config.bzl.tpl")),
    },
    configure = True,
    local = True,
)
