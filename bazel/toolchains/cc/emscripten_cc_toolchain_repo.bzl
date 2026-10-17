"""Expose an installed Emscripten SDK as a C++ toolchain."""

def _emscripten_cc_toolchain_repository_impl(ctx):
    emscripten = ctx.getenv("EMSCRIPTEN")
    if not emscripten:
        ctx.file("BUILD.bazel", "# Set EMSCRIPTEN to the directory containing emcc.\n")
        return
    root = ctx.path(emscripten).dirname
    ctx.symlink(root, "sdk")
    environment = {}
    if ctx.getenv("EM_CONFIG"):
        environment["EM_CONFIG"] = ctx.getenv("EM_CONFIG")
    if ctx.getenv("EM_CACHE"):
        environment["EM_CACHE"] = ctx.getenv("EM_CACHE")
    # Emscripten builds its sysroot libraries lazily. Prepare the selected ABI
    # before sandboxed actions, which cannot populate an external SDK cache.
    ctx.file("cache_probe.cpp", "int main() { return 0; }\n")
    result = ctx.execute(
        [emscripten + "/emcc", "cache_probe.cpp", "-Oz", "-pthread", "-fexceptions",
         "-msimd128", "-sUSE_ZLIB=1", "-sWASMFS", "-sASSERTIONS=2",
         "-sPROXY_TO_PTHREAD=1", "-sEXIT_RUNTIME=1", "-o", "cache_probe.mjs"],
        environment = environment,
        timeout = 600,
    )
    if result.return_code:
        fail("Cannot prepare Emscripten system libraries: %s" % result.stderr)
    for path in ["cache_probe.cpp", "cache_probe.mjs", "cache_probe.wasm"]:
        ctx.delete(path)
    result = ctx.execute([emscripten + "/em-config", "CACHE"], environment = environment)
    if result.return_code:
        fail("Cannot locate Emscripten cache: %s" % result.stderr)
    ctx.symlink(ctx.path(result.stdout.strip()), "cache")
    result = ctx.execute(
        [emscripten + "/emcc", "-E", "-x", "c++", "/dev/null", "-v", "-pthread"],
        environment = environment,
    )
    if result.return_code:
        fail("Cannot run Emscripten: %s" % result.stderr)
    includes = []
    collecting = False
    for line in result.stderr.splitlines():
        value = line.strip()
        if value == "#include <...> search starts here:":
            collecting = True
        elif value == "End of search list.":
            collecting = False
        elif collecting:
            includes.append(value)
    if not includes:
        fail("Emscripten did not report its builtin include directories")
    ctx.template("BUILD.bazel", ctx.attr.build_file, substitutions = {
        "@HOST_CPU@": "aarch64" if ctx.os.arch in ["aarch64", "arm64"] else "x86_64",
        "@HOST_OS@": "macos" if ctx.os.name == "mac os x" else "linux",
    })
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
