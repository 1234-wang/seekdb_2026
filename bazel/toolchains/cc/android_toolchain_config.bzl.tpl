"""Android NDK Clang compile and link settings."""
load("@rules_cc//cc/private/toolchain:unix_cc_toolchain_config.bzl", "cc_toolchain_config")

def android_toolchain_config(name):
    cc_toolchain_config(
        name = name,
        toolchain_identifier = "seekdb-android-arm64-api28",
        host_system_name = "local",
        target_system_name = "aarch64-linux-android28",
        target_libc = "bionic",
        cpu = "aarch64",
        compiler = "clang",
        abi_version = "aarch64",
        abi_libc_version = "bionic",
        builtin_sysroot = "@REPOSITORY_ROOT@/ndk/sysroot",
        cxx_builtin_include_directories = [
            "%crosstool_top%/ndk/sysroot/usr/include",
            "%crosstool_top%/ndk/lib/clang/@CLANG_VERSION@/include",
        ],
        compile_flags = [
            "--target=aarch64-linux-android28",
            "-resource-dir=@REPOSITORY_ROOT@/ndk/lib/clang/@CLANG_VERSION@",
        ],
        cxx_flags = ["-std=c++20", "-stdlib=libc++"],
        link_flags = ["--target=aarch64-linux-android28", "-fuse-ld=lld", "-Wl,-z,max-page-size=16384"],
        link_libs = ["-lm", "-ldl"],
        opt_compile_flags = ["-O2"],
        dbg_compile_flags = ["-O0", "-g"],
        supports_start_end_lib = True,
        tool_paths = {
            "gcc": "ndk/bin/clang",
            "cpp": "ndk/bin/clang",
            "ld": "ndk/bin/ld.lld",
            "ar": "ndk/bin/llvm-ar",
            "nm": "ndk/bin/llvm-nm",
            "objcopy": "ndk/bin/llvm-objcopy",
            "objdump": "ndk/bin/llvm-objdump",
            "strip": "ndk/bin/llvm-strip",
            "gcov": "ndk/bin/llvm-cov",
            "dwp": "ndk/bin/llvm-dwp",
        },
    )
