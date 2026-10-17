load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load(":toolchain_config.bzl", "emscripten_toolchain_config")

package(default_visibility = ["//visibility:public"])
filegroup(name = "sdk", srcs = glob(["sdk/**", "cache/sysroot/**"], exclude = ["sdk/**/*.pyc"]))
filegroup(name = "empty")
cc_library(
    name = "wasmfs_headers",
    hdrs = glob(["sdk/emscripten/system/lib/wasmfs/*.h"]),
    includes = ["sdk/emscripten/system/lib/wasmfs"],
)
emscripten_toolchain_config(name = "config")
cc_toolchain(
    name = "cc",
    toolchain_identifier = "seekdb-emscripten",
    toolchain_config = ":config",
    all_files = ":sdk",
    compiler_files = ":sdk",
    linker_files = ":sdk",
    ar_files = ":sdk",
    as_files = ":sdk",
    dwp_files = ":empty",
    objcopy_files = ":sdk",
    strip_files = ":sdk",
    supports_param_files = 1,
)
toolchain(
    name = "wasm32",
    toolchain = ":cc",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    exec_compatible_with = ["@platforms//os:@HOST_OS@", "@platforms//cpu:@HOST_CPU@"],
    target_compatible_with = ["@platforms//os:emscripten", "@platforms//cpu:wasm32"],
)
