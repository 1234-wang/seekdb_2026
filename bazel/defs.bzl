"""Stable public entry point for seekdb's first-party Bazel rules.

BUILD files load project rules from here. The implementation files remain free
to evolve without spreading their layout or standard policy across packages.
"""

load("@rules_cc//cc:cc_import.bzl", _cc_import = "cc_import")
load("//bazel:platforms.bzl", _seekdb_openmp_copts = "seekdb_openmp_copts")
load(
    "//bazel:seekdb_build_config.bzl",
    _SEEKDB_MIGRATION_C_COPTS = "SEEKDB_MIGRATION_C_COPTS",
    _SEEKDB_MIGRATION_CXX_COPTS = "SEEKDB_MIGRATION_CXX_COPTS",
    _SEEKDB_OBLIB_CXX_COPTS = "SEEKDB_OBLIB_CXX_COPTS",
    _SEEKDB_OBLIB_C_COPTS = "SEEKDB_OBLIB_C_COPTS",
    _SEEKDB_OBLIB_LOCAL_DEFINES = "SEEKDB_OBLIB_LOCAL_DEFINES",
    _SEEKDB_MODULE_CXX_COPTS = "SEEKDB_MODULE_CXX_COPTS",
    _SEEKDB_X86_SIMD_COPTS = "SEEKDB_X86_SIMD_COPTS",
)
load(
    "//bazel:seekdb_cc_library.bzl",
    _seekdb_cc_library = "seekdb_cc_library",
    _seekdb_generated_unity_cc_library = "seekdb_generated_unity_cc_library",
    _seekdb_generated_unity_source = "seekdb_generated_unity_source",
    _seekdb_semantic_unity_cc_library = "seekdb_semantic_unity_cc_library",
    _seekdb_source_ownership_check = "seekdb_source_ownership_check",
)
load("//bazel:seekdb_final_link.bzl", _seekdb_final_link = "seekdb_final_link")
load("//bazel:seekdb_partial_link.bzl", _seekdb_localized_partial_link = "seekdb_localized_partial_link")
load("//bazel:seekdb_version.bzl", _seekdb_version_source = "seekdb_version_source")

SEEKDB_MIGRATION_C_COPTS = _SEEKDB_MIGRATION_C_COPTS
SEEKDB_MIGRATION_CXX_COPTS = _SEEKDB_MIGRATION_CXX_COPTS
SEEKDB_OBLIB_CXX_COPTS = _SEEKDB_OBLIB_CXX_COPTS
SEEKDB_OBLIB_C_COPTS = _SEEKDB_OBLIB_C_COPTS
SEEKDB_OBLIB_LOCAL_DEFINES = _SEEKDB_OBLIB_LOCAL_DEFINES
SEEKDB_MODULE_CXX_COPTS = _SEEKDB_MODULE_CXX_COPTS
SEEKDB_X86_SIMD_COPTS = _SEEKDB_X86_SIMD_COPTS
seekdb_openmp_copts = _seekdb_openmp_copts

cc_import = _cc_import
seekdb_cc_library = _seekdb_cc_library
seekdb_final_link = _seekdb_final_link
seekdb_generated_unity_cc_library = _seekdb_generated_unity_cc_library
seekdb_generated_unity_source = _seekdb_generated_unity_source
seekdb_localized_partial_link = _seekdb_localized_partial_link
seekdb_semantic_unity_cc_library = _seekdb_semantic_unity_cc_library
seekdb_source_ownership_check = _seekdb_source_ownership_check
seekdb_version_source = _seekdb_version_source
