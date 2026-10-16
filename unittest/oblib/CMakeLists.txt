# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0.

seekdb_add_module_tests(oblib_tests
  lib/alloc/test_jemalloc_hook.cpp
  lib/allocator/test_allocator_facade.cpp
  lib/resource/test_memory_quota.cpp)

seekdb_add_module_benchmark(memory_quota_benchmark
  "${CMAKE_SOURCE_DIR}/tools/testing/memory_quota_benchmark.cpp")
