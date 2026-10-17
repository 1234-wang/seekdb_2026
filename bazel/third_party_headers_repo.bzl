"""Expose seekdb's bootstrapped third-party SDK as a Bazel repository."""

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
    directories = {}
    for header in _IOS_BOOST_HEADERS:
        parts = header.split("/")
        for depth in range(1, len(parts)):
            directories["/".join(parts[:depth])] = True
    for directory in directories:
        path = ctx.path(str(source) + directory[len(relative):])
        for child in path.readdir():
            name = directory + "/" + child.basename
            if name in _IOS_BOOST_HEADERS:
                content = ctx.read(child).replace("mpl::integral_c<", "boost::integral_constant<")
                ctx.file("include/" + name, "#include <boost/type_traits/integral_constant.hpp>\n" + content)
            elif name not in directories:
                ctx.symlink(child, "include/" + name)

def _seekdb_third_party_headers_repository_impl(repository_ctx):
    sdk = str(repository_ctx.workspace_root) + "/deps/3rd/usr/local/oceanbase"
    headers = sdk + "/deps/devel/include"
    libraries = sdk + "/deps/devel/lib"
    libraries64 = sdk + "/deps/devel/lib64"
    platform = repository_ctx.attr.platform
    ios_sdk = platform if platform in ["iphoneos", "iphonesimulator"] else ""
    if platform == "android":
        prefix = str(repository_ctx.workspace_root) + "/deps/android/usr/local/oceanbase/deps/devel"
        headers = prefix + "/include"
        libraries = prefix + "/lib"
        libraries64 = prefix + "/lib64"
    if platform == "wasm":
        prefix = repository_ctx.getenv("SEEKDB_WASM_DEPS") or str(repository_ctx.workspace_root) + "/build_wasm_deps"
        headers = prefix + "/include"
        libraries = prefix + "/lib"
        libraries64 = libraries
    if ios_sdk:
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
    repository_ctx.symlink(sdk + "/devtools", "devtools")
    repository_ctx.symlink(libraries, "lib")
    repository_ctx.symlink(libraries64, "lib64")
    repository_ctx.template("BUILD.bazel", repository_ctx.attr.build_file, executable = False)

seekdb_third_party_headers_repository = repository_rule(
    implementation = _seekdb_third_party_headers_repository_impl,
    attrs = {
        "platform": attr.string(default = "native", values = ["native", "android", "iphoneos", "iphonesimulator", "wasm"]),
        "build_file": attr.label(default = Label("//bazel:third_party.BUILD"), allow_single_file = True),
    },
    local = True,
)


def _sdk_aliases_impl(ctx):
    ctx.file("BUILD.bazel", '''load("@seekdb//bazel:third_party_targets.bzl", "sdk_aliases")
package(default_visibility = ["//visibility:public"])
sdk_aliases()
''')

seekdb_sdk_aliases = repository_rule(implementation = _sdk_aliases_impl)
