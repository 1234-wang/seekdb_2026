"""Stable public entry point for seekdb's first-party Bazel rules.

BUILD files load project rules from here. The implementation files remain free
to evolve without spreading their layout or standard policy across packages.
"""

load("@rules_cc//cc:cc_import.bzl", _cc_import = "cc_import")
load("//bazel:platforms.bzl", _openmp_copts = "openmp_copts")
load(
    "//bazel:compile_config.bzl",
    _SEEKDB_C_COPTS = "SEEKDB_C_COPTS",
    _SEEKDB_CXX_COPTS = "SEEKDB_CXX_COPTS",
    _SEEKDB_OBLIB_CXX_COPTS = "SEEKDB_OBLIB_CXX_COPTS",
    _SEEKDB_OBLIB_C_COPTS = "SEEKDB_OBLIB_C_COPTS",
    _SEEKDB_OBLIB_LOCAL_DEFINES = "SEEKDB_OBLIB_LOCAL_DEFINES",
    _SEEKDB_MODULE_CXX_COPTS = "SEEKDB_MODULE_CXX_COPTS",
    _SEEKDB_X86_SIMD_COPTS = "SEEKDB_X86_SIMD_COPTS",
)
load(
    "//bazel:cc_library.bzl",
    _cc_library = "cc_library",
    _unity_cc_library = "unity_cc_library",
    _unity_source = "unity_source",
    _grouped_unity_cc_library = "grouped_unity_cc_library",
)
load("//bazel:final_link.bzl", _final_link = "final_link")
load("//bazel:localized_partial_link.bzl", _localized_partial_link = "localized_partial_link")
load("//bazel:version.bzl", _version_source = "version_source")

SEEKDB_C_COPTS = _SEEKDB_C_COPTS
SEEKDB_CXX_COPTS = _SEEKDB_CXX_COPTS
SEEKDB_OBLIB_CXX_COPTS = _SEEKDB_OBLIB_CXX_COPTS
SEEKDB_OBLIB_C_COPTS = _SEEKDB_OBLIB_C_COPTS
SEEKDB_OBLIB_LOCAL_DEFINES = _SEEKDB_OBLIB_LOCAL_DEFINES
SEEKDB_MODULE_CXX_COPTS = _SEEKDB_MODULE_CXX_COPTS
SEEKDB_X86_SIMD_COPTS = _SEEKDB_X86_SIMD_COPTS
openmp_copts = _openmp_copts

cc_import = _cc_import
cc_library = _cc_library
final_link = _final_link
unity_cc_library = _unity_cc_library
unity_source = _unity_source
localized_partial_link = _localized_partial_link
grouped_unity_cc_library = _grouped_unity_cc_library
version_source = _version_source
