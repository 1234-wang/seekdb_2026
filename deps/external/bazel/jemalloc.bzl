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
load("//bazel:cargo.bzl", "CARGO_ACTION_ATTRS", "cargo_action")
load("//bazel:platforms.bzl", "target_platform")

def _seekdb_jemalloc_build_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    archive = ctx.actions.declare_file("jemalloc/lib/libjemalloc_pic.a")
    public_header = ctx.actions.declare_file(
        "jemalloc/include/jemalloc/jemalloc.h",
    )
    cargo_target = ctx.actions.declare_directory("jemalloc/cargo-target")

    platform = target_platform(ctx)
    command = """export CC="$1" AR="$2"
case "$CC" in /*) ;; *) CC="$PWD/$CC" ;; esac
case "$AR" in /*) ;; *) AR="$PWD/$AR" ;; esac
export CFLAGS='-O2 -fPIC'
export MAKEFLAGS=
export CARGO_TARGET_DIR="$PWD/$3"
export JEMALLOC_SYS_OUTPUT_DIR="$PWD/$4"
export JEMALLOC_SYS_CONFIGURE_ARGS='--with-jemalloc-prefix=je_'
manifest="$PWD/$5"
"""
    if platform.os == "ios":
        command += """export CC="$(xcrun --sdk %s --find clang)"
export AR="$(xcrun --sdk %s --find ar)"
target="arm64-apple-ios$IPHONEOS_DEPLOYMENT_TARGET%s"
export CFLAGS="-O2 -fPIC -target $target -isysroot $SDKROOT"
export CPPFLAGS= LDFLAGS=
export JEMALLOC_SYS_CONFIGURE_ARGS='--with-jemalloc-prefix=je_
--host=aarch64-apple-ios
--with-lg-page=14
--disable-zone-allocator'
""" % (platform.ios_sdk, platform.ios_sdk, "-simulator" if platform.ios_sdk == "iphonesimulator" else "")
    # Both builds resolve Cargo from the Rust workspace's pinned toolchain.
    command += 'cd "$(dirname "$6")"\ncargo build --locked --release --jobs 4 --manifest-path "$manifest"'
    if platform.rust_target:
        command += " --target " + platform.rust_target
    cargo_action(
        ctx,
        command = command,
        inputs = depset(
            direct = [ctx.file.external_src, ctx.file.lockfile, ctx.file.manifest, ctx.file._rust_toolchain],
            transitive = [cc_toolchain.all_files],
        ),
        outputs = [archive, public_header, cargo_target],
        arguments = [cc_toolchain.compiler_executable, cc_toolchain.ar_executable,
                     cargo_target.path, archive.dirname + "/..", ctx.file.manifest.path,
                     ctx.file._rust_toolchain.path],
        mnemonic = "SeekdbJemallocBuild",
        progress_message = "Building seekdb-jemalloc-sys for " + platform.os,
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
        "_rust_toolchain": attr.label(default = Label("//rust:rust-toolchain.toml"), allow_single_file = True),
    } | CC_TOOLCHAIN_ATTRS | CARGO_ACTION_ATTRS,
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
