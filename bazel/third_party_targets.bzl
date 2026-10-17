"""Shared declarations for SDK targets and their platform-selected aliases."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_import.bzl", "cc_import")

_HEADER_EXTENSIONS = ["def", "h", "hh", "hpp", "hxx", "inc", "inl", "ipp", "tcc"]

def _headers(directory, includes = ["include"]):
    return {"hdrs": ["include/" + directory + "/**/*." + extension for extension in _HEADER_EXTENSIONS], "includes": includes}

SDK_HEADERS = {
    "gtest": _headers("gtest") | {"deps": [":gtest_archive"]},
    "gmock": _headers("gmock") | {"deps": [":gmock_archive", ":gtest"]},
    "sanity_headers": {"hdrs": ["devtools/include/sanity/**/*.h"], "includes": ["devtools/include"]},
    "jemalloc_headers": _headers("jemalloc"),
    "absl_headers": _headers("absl"),
    "boost_headers": _headers("boost"),
    "curl_headers": _headers("curl"),
    "fast_float_headers": _headers("fast_float", ["include", "include/fast_float"]),
    "icu_headers": _headers("icu", ["include/icu", "include/icu/common"]),
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
    "gmock_archive": {"static_library": "lib/libgmock.a"},
    "sanity_runtime": {"static_library": "devtools/lib64/libsanity.a"},
    "libaio": {"static_library": "lib/libaio.a", "hdrs": ["include/libaio.h"], "includes": ["include"]},
    "libcrypto": {"static_library": "lib/libcrypto.a"},
    "libssl": {"static_library": "lib/libssl.a"},
    "libunwind": {"static_library": "lib/libunwind.a"},
    "zlib": {"static_library": "lib/libz.a"},
}
SDK_FILEGROUPS = {
    "bison_runtime": {"srcs": ["devtools/share/bison/**/*"]},
    "wasm_static_libraries": {"srcs": ["lib/*.a"]},
    "ios_static_libraries": {"srcs": ["lib/*.a", "lib/vsag_lib/*.a"]},
    "sanity_pass": {"srcs": ["devtools/lib64/libsanitypass.so"]},
}

HOST_FILES = [
    "devtools/bin/bison", "devtools/bin/flex", "devtools/bin/llvm-objcopy",
    "devtools/lib64/libsanitypass.so", "devtools/share/bison/README",
]
NATIVE_LINK_FILES = ["lib/libroaring.a", "lib/libaio.so.1", "lib/libaio.so.1.0.1"]

def sdk_targets():
    native.exports_files(HOST_FILES + native.glob(["lib/**/*.a", "lib/**/*.so", "lib/**/*.so.*", "lib64/**/*.a", "lib64/**/*.so", "lib64/**/*.so.*"], allow_empty = True))
    for name, attributes in SDK_HEADERS.items():
        cc_library(name = name, **(attributes | {"hdrs": native.glob(attributes["hdrs"], allow_empty = True)}))
    for name, attributes in SDK_IMPORTS.items():
        cc_import(name = name, **attributes)
    for name, attributes in SDK_FILEGROUPS.items():
        native.filegroup(name = name, **(attributes | {"srcs": native.glob(attributes["srcs"], allow_empty = True)}))

def sdk_aliases():
    for name in list(SDK_HEADERS) + list(SDK_IMPORTS) + list(SDK_FILEGROUPS) + HOST_FILES + NATIVE_LINK_FILES:
        if name in HOST_FILES + ["bison_runtime", "sanity_pass", "sanity_runtime", "sanity_headers"]:
            actual = "@seekdb_3rd_native//:" + name
        else:
            actual = select({
                "@platforms//os:android": "@seekdb_3rd_android//:" + name,
                "@platforms//os:emscripten": "@seekdb_3rd_wasm//:" + name,
                "@platforms//os:ios": "@seekdb_3rd_ios//:" + name,
                Label("//bazel:ios_simulator"): "@seekdb_3rd_ios_simulator//:" + name,
                "//conditions:default": "@seekdb_3rd_native//:" + name,
            })
        native.alias(name = name, actual = actual)
