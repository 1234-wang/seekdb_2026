# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0.

find_package(Threads REQUIRED)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SeekdbModuleTests.cmake")

set(SEEKDB_PRETEST_MODULES storage PARENT_SCOPE)
set(SEEKDB_PRETEST_STORAGE_TARGET storage_tests PARENT_SCOPE)

add_subdirectory(storage)

add_custom_target(pretest DEPENDS storage_tests)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh.in"
  "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh"
  @ONLY)
file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
              WORLD_READ WORLD_EXECUTE)
