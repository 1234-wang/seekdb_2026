# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Build and expose the native artifacts from seekdb-jemalloc-sys."""

load(
    "@rules_cc//cc:find_cc_toolchain.bzl",
    "CC_TOOLCHAIN_ATTRS",
    "find_cpp_toolchain",
    "use_cc_toolchain",
)
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@apple_support//lib:apple_support.bzl", "apple_support")

def _seekdb_jemalloc_build_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    archive = ctx.actions.declare_file("jemalloc/lib/libjemalloc_pic.a")
    public_header = ctx.actions.declare_file(
        "jemalloc/include/jemalloc/jemalloc.h",
    )
    cargo_target = ctx.actions.declare_directory("jemalloc/cargo-target")

    if ctx.attr.ios_sdk:
        # Cargo and its C build script must both target iOS. Resolve Xcode's
        # SDK at execution time, keeping host macOS headers out of the build.
        apple_support.run_shell(
            actions = ctx.actions,
            xcode_config = ctx.attr._xcode_config[apple_common.XcodeVersionConfig],
            apple_platform_info = apple_support.platform_info_from_rule_ctx(ctx),
            inputs = [ctx.file.external_src, ctx.file.lockfile, ctx.file.manifest, ctx.file._rust_toolchain],
            outputs = [archive, public_header, cargo_target],
            arguments = [ctx.attr.ios_sdk, ctx.attr.rust_target, ctx.file.manifest.path,
                         cargo_target.path, archive.dirname + "/..", ctx.file._rust_toolchain.path],
            command = """set -eu
export SDKROOT="$(xcrun --sdk "$1" --show-sdk-path)"
export CC="$(xcrun --sdk "$1" --find clang)"
export AR="$(xcrun --sdk "$1" --find ar)"
export IPHONEOS_DEPLOYMENT_TARGET=18.0
target=arm64-apple-ios18.0
if [ "$1" = iphonesimulator ]; then target="$target-simulator"; fi
export CFLAGS="-O2 -fPIC -target $target -isysroot $SDKROOT"
export CPPFLAGS= LDFLAGS= MAKEFLAGS=
manifest="$PWD/$3"
export CARGO_TARGET_DIR="$PWD/$4"
export JEMALLOC_SYS_OUTPUT_DIR="$PWD/$5"
export JEMALLOC_SYS_CONFIGURE_ARGS='--with-jemalloc-prefix=je_
--host=aarch64-apple-ios
--with-lg-page=14
--disable-zone-allocator'
# Resolve rustup's toolchain from the same pin as the main Rust workspace.
cd "$(dirname "$6")"
cargo build --locked --release --jobs 4 --target "$2" --manifest-path "$manifest"
""",
            execution_requirements = {"no-remote": "1", "no-sandbox": "1"},
            mnemonic = "SeekdbJemallocBuild",
            progress_message = "Building seekdb-jemalloc-sys for iOS",
            use_default_shell_env = True,
        )
        return [
            DefaultInfo(files = depset([archive, public_header])),
            OutputGroupInfo(jemalloc_archive = depset([archive]), jemalloc_header = depset([public_header])),
        ]

    ctx.actions.run(
        inputs = depset(
            direct = [
                ctx.file.external_src,
                ctx.file.lockfile,
                ctx.file.manifest,
            ],
            transitive = [cc_toolchain.all_files],
        ),
        outputs = [archive, public_header, cargo_target],
        arguments = [
            "build",
            "--locked",
            "--release",
            "--jobs",
            "4",
            "--manifest-path",
            ctx.file.manifest.path,
        ],
        env = {
            "AR": cc_toolchain.ar_executable,
            "CARGO_TARGET_DIR": cargo_target.path,
            "CC": cc_toolchain.compiler_executable,
            "CFLAGS": "-O2 -fPIC",
            "JEMALLOC_SYS_CONFIGURE_ARGS": "--with-jemalloc-prefix=je_",
            "JEMALLOC_SYS_OUTPUT_DIR": archive.dirname + "/..",
            "MAKEFLAGS": "",
        },
        executable = ctx.attr.cargo,
        execution_requirements = {
            "no-remote": "1",
            "no-sandbox": "1",
        },
        mnemonic = "SeekdbJemallocBuild",
        progress_message = "Building seekdb-jemalloc-sys",
        use_default_shell_env = True,
    )

    return [
        DefaultInfo(files = depset([archive, public_header])),
        OutputGroupInfo(
            jemalloc_archive = depset([archive]),
            jemalloc_header = depset([public_header]),
        ),
    ]

_seekdb_jemalloc_build = rule(
    implementation = _seekdb_jemalloc_build_impl,
    attrs = {
        "external_src": attr.label(allow_single_file = True, mandatory = True),
        "lockfile": attr.label(allow_single_file = True, mandatory = True),
        "manifest": attr.label(allow_single_file = True, mandatory = True),
        "cargo": attr.string(default = "cargo"),
        "ios_sdk": attr.string(),
        "rust_target": attr.string(),
        "_rust_toolchain": attr.label(default = Label("//rust:rust-toolchain.toml"), allow_single_file = True),
    } | CC_TOOLCHAIN_ATTRS | apple_support.action_required_attrs() | apple_support.platform_constraint_attrs(),
    fragments = ["cpp", "apple"],
    toolchains = use_cc_toolchain(),
)

def seekdb_jemalloc(name, external_src, lockfile, manifest):
    """Declares the jemalloc Cargo build and its C++ archive/header targets."""
    build_target = "_%s_build" % name
    public_header_target = "%s_public_header" % name

    _seekdb_jemalloc_build(
        name = build_target,
        external_src = external_src,
        lockfile = lockfile,
        manifest = manifest,
        ios_sdk = select({
            "//bazel:ios_simulator": "iphonesimulator",
            "@platforms//os:ios": "iphoneos",
            "//conditions:default": "",
        }),
        rust_target = select({
            "//bazel:ios_simulator": "aarch64-apple-ios-sim",
            "@platforms//os:ios": "aarch64-apple-ios",
            "//conditions:default": "",
        }),
        # Windows and other platforms will be supported later, after their
        # jemalloc toolchains are provisioned and covered by CI.
        target_compatible_with = select({
            "@platforms//os:linux": [],
            "@platforms//os:macos": [],
            "@platforms//os:ios": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )

    native.filegroup(
        name = "%s_archive" % name,
        srcs = [":" + build_target],
        output_group = "jemalloc_archive",
        visibility = ["//src/observer:__pkg__"],
    )

    native.filegroup(
        name = public_header_target,
        srcs = [":" + build_target],
        output_group = "jemalloc_header",
    )

    cc_library(
        name = "%s_headers" % name,
        hdrs = [":" + public_header_target],
        strip_include_prefix = "jemalloc/include",
        visibility = ["//src/oblib:__pkg__", "//src/observer:__pkg__"],
    )
