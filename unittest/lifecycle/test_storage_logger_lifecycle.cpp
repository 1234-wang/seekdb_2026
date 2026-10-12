/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "storage/slog/ob_storage_logger.h"
#include "storage/slog/ob_storage_log_entry.h"
#include "storage/slog/ob_storage_log_replayer.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::blocksstable;

namespace
{
class ScopedStorageLoggers
{
public:
  ~ScopedStorageLoggers()
  {
    local.destroy();
    server.destroy();
    if (!directory.empty()) { std::filesystem::remove_all(directory); }
  }
  ObStorageLogger local;
  ObStorageLogger server;
  std::string directory;
};


}

TEST(StorageLoggerLifecycle, BothStreamsResetAcrossThreeInitStartDestroyCycles)
{
  ScopedStorageLoggers scoped;
  char directory[] = "/tmp/seekdb-slogger-lifecycle-XXXXXX";
  char *created = mkdtemp(directory);
  ASSERT_NE(nullptr, created);
  scoped.directory = created;
  ObLogFileSpec spec;
  spec.retry_write_policy_ = "normal";
  spec.log_create_policy_ = "normal";
  spec.log_write_policy_ = "truncate";
  const int64_t file_size = 16L * 1024 * 1024;
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    const std::string root = scoped.directory + "/cycle-" + std::to_string(cycle);
    ObLogFileSpec invalid = spec;
    invalid.retry_write_policy_ = "invalid-policy";
    // Failure happens after base-writer queue initialization, before registration.
    EXPECT_EQ(OB_INVALID_ARGUMENT, scoped.local.init(root.c_str(), file_size, invalid));
    ASSERT_EQ(OB_SUCCESS, scoped.local.init(root.c_str(), file_size, spec));
    ASSERT_EQ(OB_SUCCESS, scoped.server.init(root.c_str(), file_size, spec, true));
    ASSERT_EQ(OB_SUCCESS, scoped.local.start());
    ASSERT_EQ(OB_SUCCESS, scoped.server.start());
    EXPECT_EQ(OB_INIT_TWICE, scoped.local.init(root.c_str(), file_size, spec));
    scoped.local.stop();
    scoped.server.stop();
    scoped.local.wait();
    scoped.server.wait();
    scoped.local.destroy();
    scoped.server.destroy();
    ObLogCursor unavailable;
    EXPECT_EQ(OB_NOT_INIT, scoped.local.get_active_cursor(unavailable));
    EXPECT_FALSE(unavailable.is_valid());
    scoped.local.destroy();
    scoped.server.destroy();
  }
}
