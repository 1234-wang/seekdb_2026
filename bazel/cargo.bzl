"""Expose the Cargo workspace's declared C++ exports to Bazel."""

load("//bazel:defs.bzl", cc_library = "seekdb_cc_library")
load("@apple_support//lib:apple_support.bzl", "apple_support")
load("//bazel:platforms.bzl", "PLATFORM_ATTRS", "target_platform")

CARGO_ACTION_ATTRS = PLATFORM_ATTRS | apple_support.action_required_attrs() | apple_support.platform_constraint_attrs()

def cargo_action(ctx, command, inputs, outputs, **kwargs):
    """Run Cargo locally with the selected Apple SDK when cross compiling."""
    platform = target_platform(ctx)
    environment = {"DEFAULT_LOG_LEVEL": "OB_LOG_LEVEL_ERROR"}
    run_shell = ctx.actions.run_shell
    if platform.os == "ios":
        config = ctx.attr._xcode_config[apple_common.XcodeVersionConfig]
        environment["IPHONEOS_DEPLOYMENT_TARGET"] = str(config.minimum_os_for_platform_type(apple_common.platform_type.ios))
        command = 'export SDKROOT="$(xcrun --sdk %s --show-sdk-path)"\n' % platform.ios_sdk + command
        run_shell = apple_support.run_shell
        kwargs.update(
            actions = ctx.actions,
            xcode_config = config,
            apple_platform_info = apple_support.platform_info_from_rule_ctx(ctx),
        )
    run_shell(
        command = "set -eu\n" + command,
        inputs = inputs,
        outputs = outputs,
        env = environment,
        execution_requirements = {"no-remote": "1", "no-sandbox": "1"},
        use_default_shell_env = True,
        **kwargs
    )


def _cargo_exports_repository_impl(ctx):
    exports = json.decode(ctx.read(ctx.attr.manifest))
    ctx.file("exports.bzl", "EXPORTS = " + repr(exports) + "\n")
    ctx.file("BUILD.bazel", 'exports_files(["exports.bzl"])\n')

cargo_exports_repository = repository_rule(
    implementation = _cargo_exports_repository_impl,
    attrs = {"manifest": attr.label(mandatory = True, allow_single_file = True)},
)

def _cargo_build_impl(ctx):
    exports = json.decode(ctx.attr.exports)
    platform = target_platform(ctx)
    groups = {}
    libraries = {}
    generated_headers = {
        header: ctx.actions.declare_file("cargo/include/" + header)
        for header in {h: True for export in exports.values() for h in export["headers"]}
    }
    for name, export in exports.items():
        library_name = export.get("library", name)
        archive = library_name + ".lib" if platform.os == "windows" else "lib" + library_name + ".a"
        if library_name not in libraries:
            libraries[library_name] = ctx.actions.declare_file("cargo/" + archive)
        library = libraries[library_name]
        headers = [generated_headers[h] for h in export["headers"]]
        groups[name + "_archive"] = depset([library])
        groups[name + "_headers"] = depset(headers)

    outputs = libraries.values() + generated_headers.values()
    directory = ctx.bin_dir.path + "/" + ctx.label.package
    inputs = ctx.files.srcs + ctx.files.platform_inputs
    command = '"%s" --target-dir "%s/cargo-target" --output "%s/cargo" --canonical-archives' % (ctx.file.builder.path, directory, directory)
    if platform.rust_target:
        command += " --target " + platform.rust_target
    if platform.os == "windows":
        # No Bash dependency for the native Windows Cargo build.
        ctx.actions.run(
            executable = "python",
            arguments = [ctx.file.builder.path, "--target-dir", directory + "/cargo-target", "--output", directory + "/cargo", "--canonical-archives"],
            inputs = inputs,
            outputs = outputs,
            env = {"DEFAULT_LOG_LEVEL": "OB_LOG_LEVEL_ERROR"},
            use_default_shell_env = True,
            execution_requirements = {"no-remote": "1", "no-sandbox": "1"},
            mnemonic = "CargoBuild",
        )
    else:
        command = "python3 " + command
        if platform.os == "android":
            command = 'source "%s"\n' % ctx.files.platform_inputs[0].path + command
        elif platform.os == "emscripten":
            # The standalone Wasm bootstrap still exports the per-crate names.
            # Expose each underlying Cargo library only once to the C++ graph.
            command = 'bash "%s" "%s/wasm-rust"\nmkdir -p "%s/cargo"\ncp -R "%s/wasm-rust/exports/include" "%s/cargo/"' % (
                ctx.file._wasm_builder.path, directory, directory, directory, directory,
            )
            copied = {}
            for name, export in exports.items():
                library_name = export.get("library", name)
                if library_name not in copied:
                    command += '\ncp "%s/wasm-rust/exports/lib%s.a" "%s"' % (directory, name, libraries[library_name].path)
                    copied[library_name] = True
        cargo_action(
            ctx,
            command = 'export CXXFLAGS="${CXXFLAGS:-} -D_GLIBCXX_USE_CXX11_ABI=1"\n' + command,
            inputs = inputs,
            outputs = outputs,
            mnemonic = "CargoBuild",
            progress_message = "Building Rust libraries for " + platform.os,
        )
    return [DefaultInfo(files = depset(outputs)), OutputGroupInfo(**groups)]

_cargo_build = rule(
    implementation = _cargo_build_impl,
    attrs = CARGO_ACTION_ATTRS | {
        "exports": attr.string(mandatory = True),
        "srcs": attr.label_list(allow_files = True),
        "builder": attr.label(allow_single_file = True),
        "platform_inputs": attr.label_list(allow_files = True),
        "_wasm_builder": attr.label(default = Label("//tools/wasm:build-rust-nio.sh"), allow_single_file = True),
    },
    fragments = ["apple"],
)

def cargo_workspace(exports, srcs):
    """One build action exports each crate's archive and generated headers."""
    _cargo_build(
        name = "_rust_static",
        exports = json.encode(exports),
        srcs = srcs,
        builder = "build.py",
        platform_inputs = select({
            "@platforms//os:android": ["@seekdb_android_ndk//:rust_env.sh"],
            "@platforms//os:emscripten": ["//tools/wasm:rust_build_tools"],
            "//conditions:default": [],
        }),
    )
    cc_library(
        name = "_rust_runtime",
        linkopts = select({
            "@platforms//os:linux": ["-lpthread", "-ldl", "-lm"],
            "@platforms//os:android": ["-ldl", "-lm"],
            "@platforms//os:windows": ["ntdll.lib", "userenv.lib", "ws2_32.lib", "bcrypt.lib", "advapi32.lib", "synchronization.lib"],
            "//conditions:default": [],
        }),
    )
    for name, export in exports.items():
        for kind in ["archive", "headers"]:
            native.filegroup(
                name = name + ("_archive" if kind == "archive" else "_generated_headers"),
                srcs = [":_rust_static"],
                output_group = name + "_" + kind,
                visibility = ["//visibility:public"] if kind == "archive" else None,
            )
        include_dir = export["crate"] + "/include"
        cc_library(
            name = name + "_headers",
            hdrs = native.glob([include_dir + "/**/*.h"], allow_empty = True) + [":" + name + "_generated_headers"],
            includes = [include_dir, "cargo/include"],
            deps = [":" + dep + "_headers" for dep in export["deps"]],
            visibility = ["//visibility:public"],
        )
        cc_library(
            name = name,
            srcs = [":" + name + "_archive"],
            deps = [":" + name + "_headers", ":_rust_runtime"] + [":" + dep for dep in export["deps"]],
            visibility = ["//visibility:public"],
        )
