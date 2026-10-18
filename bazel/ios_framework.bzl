"""Native Bazel entry point for the embedded iOS framework."""

load("//bazel:defs.bzl", "seekdb_final_link", "seekdb_version_source", cc_library = "seekdb_cc_library")
load("//bazel:seekdb_build_config.bzl", "SEEKDB_CXX_COPTS")
load("@apple_support//rules:apple_genrule.bzl", "apple_genrule")

def seekdb_ios_framework():
    """Declare device/simulator framework targets inside the Observer package."""
    seekdb_version_source(
        name = "_ios_build_config",
        template = "ios/driver/build_config.h.in",
        out = "ios-build-config.h",
    )
    cc_library(
        name = "_ios_entry",
        srcs = ["ios/seekdb_ios.cpp", "ios/driver/seekdb_lifecycle.cpp"],
        copts = SEEKDB_CXX_COPTS + ["-include", "$(location :_ios_build_config)"],
        additional_compiler_inputs = [":_ios_build_config"],
        deps = [
            ":observer_internal_interface",
            "//src/oblib:oblib_foundation",
            "//src/oblib:oblib_base",
            "//src/oblib:oblib_io_primitives",
            "//src/oblib:oblib_rpc",
            "//deps/external:jemalloc_headers",
            "@seekdb_3rd_headers//:curl_headers",
        ],
        target_compatible_with = ["@platforms//os:ios"],
        tags = ["manual"],
    )
    seekdb_final_link(
        name = "_ios_framework_link",
        direct_object_deps = [":_ios_entry"],
        group_deps = [":observer_embedded_runtime"],
        sdk_deps = ["//bazel/sdk_sources:source_sdk", "@seekdb_3rd_headers//:base_libraries"],
        ordered_link_files = [
            "//rust:sql_nio_archive",
            "//rust:config_archive",
            "//rust:local_archive",
            "//rust:logger_archive",
            "//deps/external:jemalloc_archive",
        ],
        extra_inputs = ["ios/driver/exports.txt"],
        linkopts = [
            "-Wl,-install_name,@rpath/SeekDB.framework/SeekDB",
            "-Wl,-exported_symbols_list,$(location ios/driver/exports.txt)",
            "-Wl,-framework,Security",
            "-Wl,-framework,CoreFoundation",
            "-Wl,-framework,SystemConfiguration",
            "-Wl,-framework,Accelerate",
            "-liconv",
            "-lresolv",
        ],
        shared = True,
        out = "framework/SeekDB.framework/SeekDB",
        target_compatible_with = ["@platforms//os:ios"],
        tags = ["manual"],
    )
    native.filegroup(
        name = "_ios_framework_link_inputs",
        srcs = [":_ios_framework_link"],
        output_group = "link_inputs",
    )
    native.genrule(
        name = "_ios_framework_link_manifest",
        srcs = [":_ios_framework_link_inputs"],
        outs = ["ios-link-inputs.txt"],
        cmd = "printf '%s\\n' $(locations :_ios_framework_link_inputs) > $@",
        target_compatible_with = ["@platforms//os:ios"],
        tags = ["manual"],
    )
    apple_genrule(
        name = "_ios_framework_metadata",
        srcs = [
            ":_ios_framework_link",
            ":_ios_build_config",
            ":_ios_framework_link_inputs",
            ":_ios_framework_link_manifest",
            "ios/driver/seekdb.h",
            "ios/driver/exports.txt",
            "//:LICENSE",
            "//rust:Cargo.lock",
            "@seekdb_cargo_native_sources//:vendor.tar",
            "//bazel/sdk_sources:ios_source_metadata",
            "//bazel/sdk_sources:ios_licenses",
            "@seekdb_3rd_headers//:ios_base_metadata",
            "@seekdb_3rd_headers//:ios_base_licenses",
        ],
        tools = ["//tools/ios:package_framework.py"],
        outs = [
            "framework/SeekDB.framework/Headers/seekdb.h",
            "framework/SeekDB.framework/Info.plist",
            "framework/SeekDB.framework/build-manifest.json",
            "framework/SeekDB.framework/rust-Cargo.lock",
            "framework/SeekDB.framework/Licenses.tar",
        ],
        cmd = 'python3 "$(location //tools/ios:package_framework.py)"' +
              ' --binary "$(location :_ios_framework_link)" --config "$(location :_ios_build_config)"' +
              ' --header "$(location ios/driver/seekdb.h)" --exports "$(location ios/driver/exports.txt)"' +
              ' --link-inputs "$(location :_ios_framework_link_manifest)"' +
              ' --rust-lock "$(location //rust:Cargo.lock)"' +
              ' --cargo-sources "$(location @seekdb_cargo_native_sources//:vendor.tar)"' +
              ' --source-metadata $(locations //bazel/sdk_sources:ios_source_metadata)' +
              ' $(locations @seekdb_3rd_headers//:ios_base_metadata)' +
              ' --licenses $(location //:LICENSE) $(locations //bazel/sdk_sources:ios_licenses)' +
              ' $(locations @seekdb_3rd_headers//:ios_base_licenses)' +
              ' --output "$(@D)/framework/SeekDB.framework" --platform ' + select({
                  "//bazel:ios_simulator": "IOSSIMULATOR",
                  "//conditions:default": "IOS",
              }),
        target_compatible_with = ["@platforms//os:ios"],
        tags = ["manual"],
    )
    native.filegroup(
        name = "seekdb_ios_framework",
        srcs = [":_ios_framework_link", ":_ios_framework_metadata"],
        target_compatible_with = ["@platforms//os:ios"],
        tags = ["manual"],
        visibility = ["//visibility:public"],
    )
