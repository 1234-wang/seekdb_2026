"""Expose seekdb's bootstrapped third-party SDK as a Bazel repository."""

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
    source_headers = [
        "absl", "s2", "roaring", "vsag", "fmt", "cpuinfo.h", "seekdb-vsag",
        "omp.h", "ompx.h", "omp-tools.h", "ompt.h", "lapack.h", "lapacke.h",
        "lapacke_config.h", "lapacke_mangling.h", "lapacke_utils.h", "cblas.h",
    ]
    if ios_sdk:
        prefix = str(repository_ctx.workspace_root) + "/deps/ios/" + ios_sdk + "/devel"
        if not repository_ctx.path(prefix + "/lib/libssl.a").exists:
            fail("Missing iOS dependencies; run ./build.py init --ios" + (" --simulator" if ios_sdk == "iphonesimulator" else ""))
        target_headers = repository_ctx.path(prefix + "/include")
        target_names = [child.basename for child in target_headers.readdir()]
        for child in repository_ctx.path(headers).readdir():
            if child.basename not in target_names and child.basename not in source_headers + ["boost"]:
                repository_ctx.symlink(child, "include/" + child.basename)
        for child in target_headers.readdir():
            if child.basename not in source_headers + ["boost"]:
                repository_ctx.symlink(child, "include/" + child.basename)
        repository_ctx.symlink(prefix + "/verified", "verified")
        repository_ctx.symlink(prefix + "/licenses", "licenses")
        libraries = prefix + "/lib"
        libraries64 = libraries
    elif platform == "wasm":
        for child in repository_ctx.path(headers).readdir():
            if child.basename not in source_headers + ["icu", "unicode"]:
                repository_ctx.symlink(child, "include/" + child.basename)
    else:
        repository_ctx.symlink(headers, "include")
    repository_ctx.symlink(sdk + "/devtools", "devtools")
    repository_ctx.symlink(libraries, "lib")
    repository_ctx.symlink(libraries64, "lib64")
    repository_ctx.template("BUILD.bazel", repository_ctx.attr.build_file, executable = False)

seekdb_third_party_headers_repository = repository_rule(
    attrs = {
        "platform": attr.string(
            default = "native",
            values = [
                "native",
                "android",
                "iphoneos",
                "iphonesimulator",
                "wasm",
            ],
        ),
        "build_file": attr.label(
            allow_single_file = True,
            default = Label("//bazel:third_party.BUILD"),
        ),
    },
    local = True,
    implementation = _seekdb_third_party_headers_repository_impl,
)

def _sdk_aliases_impl(ctx):
    ctx.file("BUILD.bazel", '''load("@seekdb//bazel:third_party_targets.bzl", "sdk_aliases")
package(default_visibility = ["//visibility:public"])
sdk_aliases()
''')

seekdb_sdk_aliases = repository_rule(implementation = _sdk_aliases_impl)
