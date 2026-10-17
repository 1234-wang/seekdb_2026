"""Emscripten pthread and JavaScript exception ABI."""
load("@rules_cc//cc/private/toolchain:unix_cc_toolchain_config.bzl", "cc_toolchain_config")

def emscripten_toolchain_config(name):
    cc_toolchain_config(
        name = name,
        toolchain_identifier = "seekdb-emscripten",
        host_system_name = "local",
        target_system_name = "wasm32-unknown-emscripten",
        target_libc = "musl",
        cpu = "wasm32",
        compiler = "clang",
        abi_version = "wasm32",
        abi_libc_version = "musl",
        cxx_builtin_include_directories = @INCLUDES@,
        compile_flags = ["-pthread", "-fexceptions", "-msimd128", "-sUSE_ZLIB=1"],
        cxx_flags = ["-std=c++20"],
        link_flags = ["-pthread", "-fexceptions", "-msimd128", "-sUSE_ZLIB=1"],
        opt_compile_flags = ["-Oz"],
        # The browser build uses compact code and symbols from --emit-symbol-map.
        unfiltered_compile_flags = ["-g0"],
        opt_link_flags = ["-Oz"],
        dbg_compile_flags = ["-O0", "-g"],
        tool_paths = {
            "gcc": "emcc.sh",
            "cpp": "emcc.sh",
            "ld": "sdk/bin/wasm-ld",
            "ar": "sdk/emscripten/emar",
            "nm": "sdk/bin/llvm-nm",
            "objcopy": "sdk/bin/llvm-objcopy",
            "objdump": "sdk/bin/llvm-objdump",
            "strip": "sdk/bin/llvm-strip",
            "gcov": "/usr/bin/false",
            "dwp": "/usr/bin/false",
        },
    )
