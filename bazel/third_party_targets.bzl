"""Shared declarations for SDK targets and their platform-selected aliases."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_import.bzl", "cc_import")

_HEADER_EXTENSIONS = ["def", "h", "hh", "hpp", "hxx", "inc", "inl", "ipp", "tcc"]

def _headers(directory, includes = ["include"]):
    return {"hdrs": ["include/" + directory + "/**/*." + extension for extension in _HEADER_EXTENSIONS], "includes": includes}

SDK_HEADERS = {
    "gtest": _headers("gtest") | {
        "deps": [":gtest_archive"],
        "defines": ["_GLIBCXX_USE_CXX11_ABI=1"],
        "linkopts": select({"@platforms//os:linux": ["-pthread"], "//conditions:default": []}),
    },
    "sanity_headers": {"hdrs": ["devtools/include/sanity/**/*.h"], "includes": ["devtools/include"]},
    "jemalloc_headers": _headers("jemalloc"),
    "absl_headers": _headers("absl"),
    "boost_headers": _headers("boost"),
    "curl_headers": _headers("curl"),
    "fast_float_headers": _headers("fast_float", ["include", "include/fast_float"]),
    "icu_headers": {"hdrs": ["include/icu/**/*.h", "include/unicode/*.h"], "includes": ["include", "include/icu", "include/icu/common"]},
    "libaio_headers": {"hdrs": ["include/libaio.h"], "includes": ["include"]},
    "libunwind_headers": {"hdrs": ["include/libunwind*.h", "include/unwind.h"], "includes": ["include"]},
    "libxml2_headers": _headers("libxml2", ["include/libxml2"]),
    "mariadb_headers": _headers("mariadb", ["include", "include/mariadb"]),
    "openssl_headers": _headers("openssl"),
    "protobuf_c_headers": _headers("protobuf-c"),
    "protobuf_cpp_headers": _headers("google/protobuf"),
    "grpc_headers": {"hdrs": ["include/grpc/**/*.h", "include/grpc++/**/*.h", "include/grpcpp/**/*.h"], "includes": ["include"]},
    "rapidjson_headers": _headers("rapidjson"),
    "roaring_headers": _headers("roaring"),
    "s2_headers": _headers("s2"),
    "vsag_headers": _headers("vsag"),
    "zlib_headers": {"hdrs": ["include/zconf.h", "include/zlib.h"], "includes": ["include"]},
}
SDK_IMPORTS = {
    "gtest_archive": {"static_library": "lib/libgtest.a"},
    "libaio": {
        "static_library": "lib/libaio.a",
        "hdrs": ["include/libaio.h"],
        "includes": ["include"],
    },
    "libcrypto": {
        "static_library": "lib/libcrypto.a",
        "hdrs": ["include/openssl/*.h"],
        "includes": ["include"],
    },
    "libssl": {"static_library": "lib/libssl.a"},
    "libunwind": {"static_library": "lib/libunwind.a"},
    "zlib": {"static_library": "lib/libz.a"},
}

SDK_IMPORTS.update({
    name: {"static_library": "lib/" + archive}
    for name, archive in {
        "libxml2": "libxml2.a",
        "protobuf_c": "libprotobuf-c.a",
        "curl": "libcurl.a",
        "lzma": "liblzma.a",
        "icu_common": "libicuuc.a",
        "icu_i18n": "libicui18n.a",
        "icu_data": "libicudata.a",
    }.items()
})

SDK_FILEGROUPS = {
    "ios_base_metadata": {"srcs": ["verified/*.json"]},
    "ios_base_licenses": {"srcs": ["licenses/**/*"]},
    "sanity_pass": {"srcs": ["devtools/lib64/libsanitypass.so"]},
}

HOST_FILES = [
    "devtools/bin/llvm-objcopy",
    "devtools/lib64/libsanitypass.so",
]

NATIVE_LINK_FILES = [
    "lib/libroaring.a",
    "lib/libaio.so.1",
    "lib/libaio.so.1.0.1",
]

def sdk_targets():
    native.exports_files(HOST_FILES + native.glob(["lib/**/*.a", "lib/**/*.so", "lib/**/*.so.*", "lib64/**/*.a", "lib64/**/*.so", "lib64/**/*.so.*"], allow_empty = True))
    for name, attributes in SDK_HEADERS.items():
        cc_library(name = name, **(attributes | {"hdrs": native.glob(attributes["hdrs"], allow_empty = True)}))
    for name, attributes in SDK_IMPORTS.items():
        cc_import(name = name, **(attributes | ({"hdrs": native.glob(attributes["hdrs"], allow_empty = True)} if "hdrs" in attributes else {})))
    for name, attributes in SDK_FILEGROUPS.items():
        native.filegroup(name = name, **(attributes | {"srcs": native.glob(attributes["srcs"], allow_empty = True)}))

_SOURCE_HEADERS = {
    "absl_headers": ("abseil", "abseil"),
    "roaring_headers": ("roaring", "roaring"),
    "s2_headers": ("s2_ios", "s2_wasm"),
    "vsag_headers": ("vsag_ios", "vsag_wasm"),
}

def sdk_aliases():
    cc_library(
        name = "base_libraries",
        deps = [":libcrypto", ":libssl", ":libxml2", ":protobuf_c"] + select({
            "@platforms//os:emscripten": [],
            "//conditions:default": [":curl", ":icu_common", ":icu_data", ":icu_i18n", ":lzma", ":zlib"],
        }),
    )
    for name in list(SDK_HEADERS) + list(SDK_IMPORTS) + list(SDK_FILEGROUPS) + HOST_FILES + NATIVE_LINK_FILES:
        if name in HOST_FILES + ["sanity_pass", "sanity_headers"]:
            actual = "@seekdb_3rd_native//:" + name
        else:
            ios = "@seekdb_3rd_ios//:" + name
            simulator = "@seekdb_3rd_ios_simulator//:" + name
            wasm = "@seekdb_3rd_wasm//:" + name
            if name in _SOURCE_HEADERS:
                ios = "@seekdb_sdk_" + _SOURCE_HEADERS[name][0] + "//:library"
                simulator = ios
                wasm = "@seekdb_sdk_" + _SOURCE_HEADERS[name][1] + "//:library"
            elif name == "boost_headers":
                ios = "@seekdb_sdk_boost//:library"
                simulator = ios
            elif name == "icu_headers":
                wasm = "@seekdb_sdk_icu//:library"
            actual = select({
                "@platforms//os:android": "@seekdb_3rd_android//:" + name,
                "@platforms//os:emscripten": wasm,
                "@platforms//os:ios": ios,
                Label("//bazel:ios_simulator"): simulator,
                "//conditions:default": "@seekdb_3rd_native//:" + name,
            })
        native.alias(name = name, actual = actual)
