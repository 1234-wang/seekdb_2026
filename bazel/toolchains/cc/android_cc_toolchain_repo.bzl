"""Configure the installed Android NDK for arm64, API 28."""

load(":repository_utils.bzl", "host_substitutions")

def _android_cc_toolchain_repository_impl(ctx):
    ndk = ctx.getenv("ANDROID_NDK_HOME")
    if not ndk:
        ctx.file("BUILD.bazel", "# Set ANDROID_NDK_HOME to enable Android builds.\n")
        return
    host = "darwin-x86_64" if ctx.os.name == "mac os x" else "linux-x86_64"
    toolchain = ctx.path(ndk + "/toolchains/llvm/prebuilt/" + host)
    compiler = toolchain.get_child("bin").get_child("clang")
    if not compiler.exists:
        fail("Android NDK compiler is missing: %s" % compiler)
    ctx.symlink(toolchain, "ndk")
    resource = ctx.execute([str(compiler), "-print-resource-dir"])
    if resource.return_code:
        fail("Cannot run Android NDK Clang: %s" % resource.stderr)
    version = resource.stdout.strip().split("/")[-1]
    ctx.template("BUILD.bazel", ctx.attr.build_file, substitutions = host_substitutions(ctx) | {"@CLANG_VERSION@": version})
    ctx.template("toolchain_config.bzl", ctx.attr.config_file, substitutions = {
        "@CLANG_VERSION@": version,
        "@REPOSITORY_ROOT@": "external/" + ctx.name,
    })
    ctx.file("rust_env.sh", "\n".join([
        'export CC_aarch64_linux_android="%s/bin/aarch64-linux-android28-clang"' % toolchain,
        'export CXX_aarch64_linux_android="%s/bin/aarch64-linux-android28-clang++"' % toolchain,
        'export AR_aarch64_linux_android="%s/bin/llvm-ar"' % toolchain,
        'export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$CC_aarch64_linux_android"',
        "",
    ]))

android_cc_toolchain_repository = repository_rule(
    implementation = _android_cc_toolchain_repository_impl,
    attrs = {
        "build_file": attr.label(default = Label("//bazel/toolchains/cc:BUILD.android_cc_toolchain.tpl")),
        "config_file": attr.label(default = Label("//bazel/toolchains/cc:android_toolchain_config.bzl.tpl")),
    },
    configure = True,
    local = True,
)
