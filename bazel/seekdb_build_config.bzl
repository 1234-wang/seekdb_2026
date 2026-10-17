"""Shared settings for the seekdb Bazel build."""

SEEKDB_VERSION = "1.4.0.0"

SEEKDB_X86_SIMD_COPTS = [
    "-mtune=core-avx2",
    "-mavx2",
    "-mfma",
    "-mbmi2",
    "-mavx512vl",
    "-mavx512bw",
]

# Shared compiler flags; only legacy modules need workspace-relative includes.
_COMMON_COPTS = [
    "-gdwarf-4",
    "-ffunction-sections",
    "-fdata-sections",
    "-fdebug-info-for-profiling",
    "-fmax-type-align=8",
    "-fno-strict-aliasing",
    "-fno-omit-frame-pointer",
    "-g",
    "-Wno-everything",
]
_CXX_WARNINGS = [
    "-Wno-deprecated",
    "-Wno-reserved-user-defined-literal",
    "-Wno-unused-parameter",
]
_MIGRATION_INCLUDES = [
    "-I.",
    "-Isrc",
    "-Isrc/data_plane/api",
    "-Isrc/query/api",
    "-Isrc/objit/include",
    "-Isrc/oblib/easy",
    "-Isrc/oblib/easy/include",
    "-Isrc/oblib",
    "-Isrc/oblib/common",
]
SEEKDB_MIGRATION_CXX_COPTS = ["-std=gnu++20"] + _MIGRATION_INCLUDES + _COMMON_COPTS + _CXX_WARNINGS
SEEKDB_MIGRATION_C_COPTS = ["-std=gnu11"] + _MIGRATION_INCLUDES + _COMMON_COPTS
SEEKDB_MODULE_CXX_COPTS = SEEKDB_MIGRATION_CXX_COPTS + ["-Werror=private-header"]
SEEKDB_MODULE_C_COPTS = ["-std=gnu11"]
SEEKDB_OBLIB_CXX_COPTS = ["-std=gnu++20"] + _COMMON_COPTS + _CXX_WARNINGS
SEEKDB_OBLIB_C_COPTS = ["-std=gnu11"] + _COMMON_COPTS

SEEKDB_OBLIB_LOCAL_DEFINES = [
    'DEFAULT_LOG_FILE_SIZE_MB=256',
    'DEFAULT_LOG_LEVEL=OB_LOG_LEVEL_ERROR',
    'ENABLE_500_MEMORY_LIMIT',
    'ENABLE_INITIAL_EXEC_TLS_MODEL',
    'FATAL_ERROR_HANG',
    'NDEBUG',
    'OB_BUILD_LITE',
    'OB_BUILD_OBSERVER_LITE',
    'OB_BUILD_SYS_VEC_IDX',
    'OCI_LINK_RUNTIME',
    '_GLIBCXX_USE_CXX11_ABI=1',
    '_NO_EXCEPTION',
    '__STDC_CONSTANT_MACROS',
    '__STDC_LIMIT_MACROS',
]

SEEKDB_RELEASE_LOCAL_DEFINES = SEEKDB_OBLIB_LOCAL_DEFINES + [
    'PACKAGE_NAME=\\"OceanBase\\"',
    'PACKAGE_STRING=\\"OceanBase\\ %s\\"' % SEEKDB_VERSION,
    'PACKAGE_VERSION=\\"%s\\"' % SEEKDB_VERSION,
    'RELEASEID=\\"1\\"',
]

_SEEKDB_SANITY_CONFIG = "//bazel:sanity_enabled"
_SEEKDB_SANITY_PASS = "@seekdb_3rd_headers//:sanity_pass"

_SEEKDB_SANITY_NO_BUILTIN_COPTS = [
    "-fno-builtin-memset",
    "-fno-builtin-bzero",
    "-fno-builtin-memcpy",
    "-fno-builtin-memmove",
    "-fno-builtin-memcmp",
    "-fno-builtin-strlen",
    "-fno-builtin-strnlen",
    "-fno-builtin-strcpy",
    "-fno-builtin-strncpy",
    "-fno-builtin-strcmp",
    "-fno-builtin-strncmp",
    "-fno-builtin-strcasecmp",
    "-fno-builtin-strncasecmp",
    "-fno-builtin-vsprintf",
    "-fno-builtin-vsnprintf",
    "-fno-builtin-sprintf",
    "-fno-builtin-snprintf",
]

def seekdb_sanity_copts(instrument):
    return select({
        _SEEKDB_SANITY_CONFIG: _SEEKDB_SANITY_NO_BUILTIN_COPTS if instrument else [],
        "//conditions:default": [],
    })

def seekdb_sanity_cxxopts(instrument):
    return select({
        _SEEKDB_SANITY_CONFIG: [
            "-fpass-plugin=$(location %s)" % _SEEKDB_SANITY_PASS,
        ] if instrument else [],
        "//conditions:default": [],
    })

def seekdb_sanity_local_defines():
    return select({
        _SEEKDB_SANITY_CONFIG: [
            "ENABLE_SANITY",
            "OB_HAVE_BUNDLED_JEMALLOC=1",
        ],
        "//conditions:default": [],
    })

def seekdb_sanity_target_compatible_with():
    # ENABLE_SANITY is the effective source-level capability.  Mark first-party
    # targets incompatible instead of silently compiling a non-Sanity target
    # when a caller requests Sanity on an unsupported operating system.
    return select({
        _SEEKDB_SANITY_CONFIG: ["@platforms//os:linux"],
        "//conditions:default": [],
    })

def seekdb_sanity_compiler_inputs(instrument):
    return select({
        _SEEKDB_SANITY_CONFIG: [_SEEKDB_SANITY_PASS] if instrument else [],
        "//conditions:default": [],
    })

def seekdb_sanity_implementation_deps():
    return select({
        _SEEKDB_SANITY_CONFIG: ["@seekdb_3rd_headers//:sanity_headers"],
        "//conditions:default": [],
    })
