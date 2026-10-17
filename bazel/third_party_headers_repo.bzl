"""Expose seekdb's bootstrapped third-party SDK as a Bazel repository."""

_BUILD_FILE = """
load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

HEADER_EXTENSIONS = [
    "def",
    "h",
    "hh",
    "hpp",
    "hxx",
    "inc",
    "inl",
    "ipp",
    "tcc",
]

exports_files([
    "devtools/bin/bison",
    "devtools/bin/flex",
    "devtools/bin/llvm-objcopy",
    "devtools/lib64/libsanitypass.so",
    "devtools/share/bison/README",
])

# Keep each archive/shared object addressable as its own file label.  The final
# link rule, rather than this repository, owns ordering and linker mode changes.
exports_files(glob([
    "lib/**/*.a",
    "lib/**/*.so",
    "lib/**/*.so.*",
    "lib64/**/*.a",
    "lib64/**/*.so",
    "lib64/**/*.so.*",
], allow_empty = True))

filegroup(
    name = "bison_runtime",
    srcs = glob(["devtools/share/bison/**/*"], allow_empty = True),
)

filegroup(
    name = "wasm_static_libraries",
    srcs = glob(["lib/*.a"], allow_empty = True),
)

filegroup(
    name = "ios_static_libraries",
    srcs = glob(["lib/*.a", "lib/vsag_lib/*.a"], allow_empty = True),
)

cc_import(
    name = "gtest_archive",
    static_library = "lib/libgtest.a",
)

# Keep include propagation in cc_library and use cc_import only for the
# prebuilt archive.  Bazel 8 accepts includes on cc_import but does not add
# that directory to consumers' compile commands.
cc_library(
    name = "gtest",
    hdrs = glob(["include/gtest/**/*.h"], allow_empty = True),
    includes = ["include"],
    deps = [":gtest_archive"],
)

cc_import(
    name = "gmock_archive",
    static_library = "lib/libgmock.a",
)

cc_library(
    name = "gmock",
    hdrs = glob(["include/gmock/**/*.h"], allow_empty = True),
    includes = ["include"],
    deps = [
        ":gmock_archive",
        ":gtest",
    ],
)

cc_library(
    name = "sanity_headers",
    hdrs = glob(["devtools/include/sanity/**/*.h"], allow_empty = True),
    includes = ["devtools/include"],
)

cc_library(
    name = "jemalloc_headers",
    hdrs = glob(["include/jemalloc/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

filegroup(
    name = "sanity_pass",
    srcs = ["devtools/lib64/libsanitypass.so"],
)

cc_import(
    name = "sanity_runtime",
    static_library = "devtools/lib64/libsanity.a",
)

cc_import(
    name = "libaio",
    hdrs = ["include/libaio.h"],
    static_library = "lib/libaio.a",
    includes = ["include"],
)

cc_import(
    name = "libcrypto",
    static_library = "lib/libcrypto.a",
)

cc_import(
    name = "libssl",
    static_library = "lib/libssl.a",
)

cc_import(
    name = "libunwind",
    static_library = "lib/libunwind.a",
)

cc_import(
    name = "zlib",
    static_library = "lib/libz.a",
)

cc_library(
    name = "absl_headers",
    hdrs = glob(["include/absl/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "boost_headers",
    hdrs = glob(["include/boost/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "curl_headers",
    hdrs = glob(["include/curl/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "fast_float_headers",
    hdrs = glob(["include/fast_float/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = [
        "include",
        "include/fast_float",
    ],
)

cc_library(
    name = "icu_headers",
    hdrs = glob(["include/icu/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = [
        "include/icu",
        "include/icu/common",
    ],
)

cc_library(
    name = "libaio_headers",
    # macOS uses seekdb's in-tree asynchronous-I/O compatibility types and
    # therefore has no vendor libaio header.
    hdrs = glob(["include/libaio.h"], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "libunwind_headers",
    hdrs = glob([
        "include/libunwind*.h",
        "include/unwind.h",
    ], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "libxml2_headers",
    hdrs = glob(["include/libxml2/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include/libxml2"],
)

cc_library(
    name = "mariadb_headers",
    hdrs = glob(["include/mariadb/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = [
        "include",
        "include/mariadb",
    ],
)

cc_library(
    name = "openssl_headers",
    hdrs = glob(["include/openssl/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "protobuf_c_headers",
    hdrs = glob(["include/protobuf-c/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "protobuf_cpp_headers",
    hdrs = glob(["include/google/protobuf/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "grpc_headers",
    hdrs = glob([
        "include/grpc/**/*.h",
        "include/grpc++/**/*.h",
        "include/grpcpp/**/*.h",
    ], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "rapidjson_headers",
    hdrs = glob(["include/rapidjson/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "roaring_headers",
    hdrs = glob(["include/roaring/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "s2_headers",
    hdrs = glob(["include/s2/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)


cc_library(
    name = "vsag_headers",
    hdrs = glob(["include/vsag/**/*." + extension for extension in HEADER_EXTENSIONS], allow_empty = True),
    includes = ["include"],
)

cc_library(
    name = "zlib_headers",
    hdrs = glob([
        "include/zconf.h",
        "include/zlib.h",
    ], allow_empty = True),
    includes = ["include"],
)
"""

_IOS_BOOST_HEADERS = [
    "boost/numeric/conversion/" + name
    for name in ["converter_policies.hpp", "detail/converter.hpp", "detail/int_float_mixture.hpp",
                 "detail/udt_builtin_mixture.hpp", "detail/sign_mixture.hpp"]
]

def _ios_headers(ctx, source, relative):
    # Keep the host SDK read-only. Only the five Boost 1.74 compatibility
    # headers are materialized; all other generic headers remain symlinks.
    if relative != "boost" or "#define BOOST_VERSION 107400" not in ctx.read(source.get_child("version.hpp")):
        ctx.symlink(source, "include/" + relative)
        return
    pending = [(source, relative)]
    for _ in range(5):
        following = []
        for path, name in pending:
            if name in _IOS_BOOST_HEADERS:
                content = ctx.read(path).replace("mpl::integral_c<", "boost::integral_constant<")
                ctx.file("include/" + name, "#include <boost/type_traits/integral_constant.hpp>\n" + content)
            elif any([header.startswith(name + "/") for header in _IOS_BOOST_HEADERS]):
                following.extend([(child, name + "/" + child.basename) for child in path.readdir()])
            else:
                ctx.symlink(path, "include/" + name)
        pending = following

def _seekdb_third_party_headers_repository_impl(repository_ctx):
    headers = str(repository_ctx.workspace_root) + "/" + repository_ctx.attr.path
    devtools = str(repository_ctx.workspace_root) + "/" + repository_ctx.attr.devtools_path
    libraries = str(repository_ctx.workspace_root) + "/" + repository_ctx.attr.libraries_path
    libraries64 = str(repository_ctx.workspace_root) + "/" + repository_ctx.attr.libraries64_path
    wasm_deps = repository_ctx.getenv("SEEKDB_WASM_DEPS")
    ios_sdk = repository_ctx.getenv("SEEKDB_IOS_SDK")
    if wasm_deps and ios_sdk:
        fail("SEEKDB_WASM_DEPS and SEEKDB_IOS_SDK select different dependency platforms")
    if wasm_deps:
        headers = wasm_deps + "/include"
        libraries = wasm_deps + "/lib"
        libraries64 = libraries
    if ios_sdk:
        if ios_sdk not in ["iphoneos", "iphonesimulator"]:
            fail("SEEKDB_IOS_SDK must be iphoneos or iphonesimulator")
        prefix = str(repository_ctx.workspace_root) + "/deps/ios/" + ios_sdk + "/devel"
        if not repository_ctx.path(prefix + "/lib/libssl.a").exists:
            fail("Missing iOS dependencies; run ./bazel.py init --ios" + (" --simulator" if ios_sdk == "iphonesimulator" else ""))
        target_headers = repository_ctx.path(prefix + "/include")
        target_names = [child.basename for child in target_headers.readdir()]
        for child in repository_ctx.path(headers).readdir():
            if child.basename not in target_names:
                _ios_headers(repository_ctx, child, child.basename)
        for child in target_headers.readdir():
            repository_ctx.symlink(child, "include/" + child.basename)
        libraries = prefix + "/lib"
        libraries64 = libraries
    else:
        repository_ctx.symlink(headers, "include")
    repository_ctx.symlink(devtools, "devtools")
    repository_ctx.symlink(libraries, "lib")
    repository_ctx.symlink(libraries64, "lib64")
    repository_ctx.file("BUILD.bazel", _BUILD_FILE)

seekdb_third_party_headers_repository = repository_rule(
    implementation = _seekdb_third_party_headers_repository_impl,
    attrs = {
        "path": attr.string(
            default = "deps/3rd/usr/local/oceanbase/deps/devel/include",
        ),
        "devtools_path": attr.string(
            default = "deps/3rd/usr/local/oceanbase/devtools",
        ),
        "libraries_path": attr.string(
            default = "deps/3rd/usr/local/oceanbase/deps/devel/lib",
        ),
        "libraries64_path": attr.string(
            default = "deps/3rd/usr/local/oceanbase/deps/devel/lib64",
        ),
    },
    local = True,
)
