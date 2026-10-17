load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")
load(":toolchain_config.bzl", "android_toolchain_config")

package(default_visibility = ["//visibility:public"])
exports_files(["rust_env.sh"])

filegroup(name = "empty")
filegroup(
    name = "compiler_files",
    srcs = glob([
        "ndk/bin/**",
        "ndk/lib/clang/@CLANG_VERSION@/include/**",
        "ndk/lib/*.dylib",
        "ndk/lib/*.so*",
        "ndk/lib64/*.so*",
        "ndk/sysroot/usr/include/**",
    ], allow_empty = True),
)
filegroup(
    name = "linker_files",
    srcs = [":compiler_files"] + glob([
        "ndk/lib/clang/@CLANG_VERSION@/lib/**",
        "ndk/sysroot/usr/lib/aarch64-linux-android/**",
    ]),
)
filegroup(
    name = "static_cpp_runtime",
    srcs = [
        "ndk/sysroot/usr/lib/aarch64-linux-android/libc++_static.a",
        "ndk/sysroot/usr/lib/aarch64-linux-android/libc++abi.a",
    ],
)
android_toolchain_config(name = "config")
cc_toolchain(
    name = "cc",
    toolchain_identifier = "seekdb-android-arm64-api28",
    toolchain_config = ":config",
    all_files = ":linker_files",
    compiler_files = ":compiler_files",
    linker_files = ":linker_files",
    ar_files = ":compiler_files",
    as_files = ":compiler_files",
    dwp_files = ":empty",
    objcopy_files = ":compiler_files",
    strip_files = ":compiler_files",
    static_runtime_lib = ":static_cpp_runtime",
    supports_param_files = 1,
)
toolchain(
    name = "android_arm64",
    toolchain = ":cc",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    exec_compatible_with = ["@platforms//os:@HOST_OS@", "@platforms//cpu:@HOST_CPU@"],
    target_compatible_with = ["@platforms//os:android", "@platforms//cpu:aarch64"],
)
