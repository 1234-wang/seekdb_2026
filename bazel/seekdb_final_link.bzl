"""Ordered final link using the selected C++ toolchain.

Inputs are direct objects, cyclic groups, whole archives, tail dependencies,
then explicitly ordered files and the source SDK closure. Only ordered_link_files
preserves repetition.
"""

load(
    "@rules_cc//cc:action_names.bzl",
    "CPP_LINK_DYNAMIC_LIBRARY_ACTION_NAME",
    "CPP_LINK_EXECUTABLE_ACTION_NAME",
)
load(
    "@rules_cc//cc:find_cc_toolchain.bzl",
    "CC_TOOLCHAIN_ATTRS",
    "find_cpp_toolchain",
    "use_cc_toolchain",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//bazel:platforms.bzl", "PLATFORM_ATTRS", "target_platform")

def _append_unique(result, seen, files):
    for file in files:
        if file.path not in seen:
            seen[file.path] = True
            result.append(file)

def _library_objects(library):
    if library.pic_objects:
        return library.pic_objects
    return library.objects

def _direct_objects(target):
    objects = []
    seen = {}
    for linker_input in target[CcInfo].linking_context.linker_inputs.to_list():
        if linker_input.owner != target.label:
            continue
        for library in linker_input.libraries:
            _append_unique(objects, seen, _library_objects(library))
    if not objects:
        fail(
            (
                "%s contributes no direct C++ objects; pass a source-owning " +
                "target instead of a source-less aggregate"
            ) % target.label,
        )
    return objects

def _selected_static_archive(library):
    if library.pic_static_library != None:
        return library.pic_static_library
    return library.static_library

def _static_link_inputs(target, direct = False):
    inputs = []
    for linker_input in target[CcInfo].linking_context.linker_inputs.to_list():
        if direct and linker_input.owner != target.label:
            continue
        for library in linker_input.libraries:
            archive = _selected_static_archive(library)
            inputs.extend([archive] if archive != None else _library_objects(library))
    return inputs

def _files_from_targets(targets):
    return [file for target in targets for file in target[DefaultInfo].files.to_list()]

def _static_inputs_from_deps(targets):
    inputs = []
    seen = {}
    for target in targets:
        # Different top-level module targets commonly carry overlapping
        # transitive CcInfo closures. Keep the first position of each artifact
        # in this section; explicit positional repetition belongs in
        # ordered_link_files instead.
        _append_unique(inputs, seen, _static_link_inputs(target))
    return inputs

def _whole_archive_inputs(targets):
    direct_inputs = []
    direct_seen = {}
    for target in targets:
        _append_unique(
            direct_inputs,
            direct_seen,
            _static_link_inputs(target, direct = True),
        )

    transitive_inputs = []
    transitive_seen = {}
    for target in targets:
        for file in _static_link_inputs(target):
            if file.path not in direct_seen and file.path not in transitive_seen:
                transitive_seen[file.path] = True
                transitive_inputs.append(file)
    return direct_inputs, transitive_inputs

def _file_paths(files):
    return [file.path for file in files]

def _archive_args(files, apple, alwayslink):
    args = []
    for file in files:
        if file.path in alwayslink:
            args.extend(["-Wl,-force_load," + file.path] if apple else [
                "-Wl,--whole-archive", file.path, "-Wl,--no-whole-archive",
            ])
        else:
            args.append(file.path)
    return args

def _dependency_link_metadata(targets):
    # Merge linker inputs as a depset: shared dependencies contribute flags once,
    # while repetition within one library's flags retains its meaning.
    linker_inputs = depset(transitive = [
        target[CcInfo].linking_context.linker_inputs
        for target in targets
    ]).to_list()
    flags = []
    inputs = []
    alwayslink = {}
    for linker_input in linker_inputs:
        flags.extend(linker_input.user_link_flags)
        inputs.extend(linker_input.additional_inputs)
        for library in linker_input.libraries:
            archive = _selected_static_archive(library)
            if archive != None and library.alwayslink:
                alwayslink[archive.path] = True
    return flags, inputs, alwayslink

def _archive_group(files, apple, whole = False, alwayslink = {}):
    if not files:
        return []
    paths = _file_paths(files)
    if apple:
        # ld64 searches archives repeatedly; force_load retains every object.
        return ["-Wl,-force_load," + path for path in paths] if whole else _archive_args(files, apple, alwayslink)
    start, end = ("--whole-archive", "--no-whole-archive") if whole else ("--start-group", "--end-group")
    return ["-Wl," + start] + (paths if whole else _archive_args(files, apple, alwayslink)) + ["-Wl," + end]

def _seekdb_final_link_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    platform = target_platform(ctx)
    is_apple = platform.apple
    is_wasm = platform.os == "emscripten"
    needs_static_cpp_runtime = not is_apple and not is_wasm
    requested_features = ctx.features
    if needs_static_cpp_runtime and "static_link_cpp_runtimes" not in requested_features:
        requested_features = requested_features + ["static_link_cpp_runtimes"]
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = requested_features,
        unsupported_features = ctx.disabled_features,
    )
    static_cpp_runtime_inputs = []
    if needs_static_cpp_runtime:
        static_cpp_runtime_inputs = cc_toolchain.static_runtime_lib(
            feature_configuration = feature_configuration,
        ).to_list()
    if needs_static_cpp_runtime and not static_cpp_runtime_inputs:
        fail(
            "%s requires a toolchain-provided static C++ runtime" % ctx.label,
        )

    direct_objects = []
    direct_object_seen = {}
    for dep in ctx.attr.direct_object_deps:
        _append_unique(
            direct_objects,
            direct_object_seen,
            _direct_objects(dep),
        )

    group_inputs = _static_inputs_from_deps(ctx.attr.group_deps)
    whole_group_inputs = _static_inputs_from_deps(ctx.attr.whole_group_deps)
    whole_inputs, whole_transitive_inputs = _whole_archive_inputs(
        ctx.attr.whole_archive_deps,
    )
    tail_inputs = []
    tail_seen = {}
    _append_unique(tail_inputs, tail_seen, whole_transitive_inputs)
    _append_unique(
        tail_inputs,
        tail_seen,
        _static_inputs_from_deps(ctx.attr.tail_deps),
    )
    ordered_link_files = _files_from_targets(
        ctx.attr.ordered_link_files,
    )
    sdk_inputs = _static_inputs_from_deps(ctx.attr.sdk_deps)
    sdk_paths = {file.path: True for file in sdk_inputs}
    group_inputs = [file for file in group_inputs if file.path not in sdk_paths]
    whole_group_inputs = [file for file in whole_group_inputs if file.path not in sdk_paths]
    whole_inputs = [file for file in whole_inputs if file.path not in sdk_paths]
    tail_inputs = [file for file in tail_inputs if file.path not in sdk_paths]
    dependency_flags, dependency_inputs, alwayslink = _dependency_link_metadata(
        ctx.attr.direct_object_deps + ctx.attr.group_deps + ctx.attr.whole_group_deps +
        ctx.attr.whole_archive_deps + ctx.attr.tail_deps + ctx.attr.sdk_deps,
    )
    extra_inputs = _files_from_targets(ctx.attr.extra_inputs) + dependency_inputs

    link_inputs = (
        direct_objects +
        group_inputs +
        whole_group_inputs +
        whole_inputs +
        tail_inputs +
        ordered_link_files +
        sdk_inputs +
        static_cpp_runtime_inputs
    )
    if not link_inputs:
        fail("%s has no object or archive inputs" % ctx.label)

    ordered_link_args = _file_paths(direct_objects)
    ordered_link_args.extend(_archive_group(group_inputs, is_apple, alwayslink = alwayslink))
    ordered_link_args.extend(_archive_group(whole_group_inputs, is_apple, whole = True))
    ordered_link_args.extend(_archive_group(whole_inputs, is_apple, whole = True))
    ordered_link_args.extend(_archive_args(tail_inputs, is_apple, alwayslink))
    ordered_link_args.extend(_file_paths(ordered_link_files))
    ordered_link_args.extend(_archive_group(sdk_inputs, is_apple, alwayslink = alwayslink))
    ordered_link_args.extend(_file_paths(static_cpp_runtime_inputs))
    ordered_link_args.extend(dependency_flags)
    ordered_link_args.extend(["-l" + library for library in ctx.attr.system_libs])
    ordered_link_args.extend([
        ctx.expand_location(option, targets = ctx.attr.extra_inputs)
        for option in ctx.attr.linkopts
    ])

    output = ctx.outputs.out
    action_name = (
        CPP_LINK_DYNAMIC_LIBRARY_ACTION_NAME
        if ctx.attr.shared
        else CPP_LINK_EXECUTABLE_ACTION_NAME
    )
    link_variables = cc_common.create_link_variables(
        cc_toolchain = cc_toolchain,
        feature_configuration = feature_configuration,
        output_file = output.path,
        user_link_flags = ordered_link_args,
        is_using_linker = True,
        is_linking_dynamic_library = ctx.attr.shared,
    )
    tool = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = action_name,
    )
    command_line = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = action_name,
        variables = link_variables,
    )
    environment = cc_common.get_environment_variables(
        feature_configuration = feature_configuration,
        action_name = action_name,
        variables = link_variables,
    )
    if is_wasm:
        # The toolchain repository prepares the sysroot. Frozen mode also
        # disables Emscripten's writable JavaScript symbol cache in the sandbox.
        environment = environment | {"EM_FROZEN_CACHE": "1"}

    args = ctx.actions.args()
    args.add_all(command_line)
    ctx.actions.run(
        executable = tool,
        arguments = [args],
        env = environment,
        inputs = depset(
            direct = link_inputs + extra_inputs,
            transitive = [cc_toolchain.all_files],
        ),
        outputs = [output] + ctx.outputs.side_outputs,
        mnemonic = "SeekdbFinalLink",
        progress_message = (
            "Linking seekdb shared library %{output}"
            if ctx.attr.shared
            else "Linking seekdb executable %{output}"
        ),
        use_default_shell_env = True,
    )

    return [
        OutputGroupInfo(link_inputs = depset(link_inputs)),
        DefaultInfo(
            executable = output,
            files = depset([output] + ctx.outputs.side_outputs),
        ),
    ]

seekdb_final_link = rule(
    implementation = _seekdb_final_link_impl,
    attrs = {
        "direct_object_deps": attr.label_list(providers = [CcInfo]),
        "group_deps": attr.label_list(providers = [CcInfo]),
        "whole_group_deps": attr.label_list(providers = [CcInfo]),
        "whole_archive_deps": attr.label_list(providers = [CcInfo]),
        "tail_deps": attr.label_list(providers = [CcInfo]),
        "sdk_deps": attr.label_list(providers = [CcInfo]),
        "ordered_link_files": attr.label_list(allow_files = True),
        "extra_inputs": attr.label_list(allow_files = True),
        "system_libs": attr.string_list(),
        "linkopts": attr.string_list(),
        "out": attr.output(mandatory = True),
        "side_outputs": attr.output_list(),
        "shared": attr.bool(default = False),
    } | CC_TOOLCHAIN_ATTRS | PLATFORM_ATTRS,
    executable = True,
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
