/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "sql/dtl/ob_dtl.h"
#include "sql/dtl/ob_dtl_channel.h"

using namespace oceanbase::common;
using namespace oceanbase::sql::dtl;

namespace
{
class ScopedDtl
{
public:
  ~ScopedDtl()
  {
    // An assertion must not strand a remover waiting for its host-owned pin.
    if (nullptr != borrowed) {
      IGNORE_RETURN dtl.release_channel(borrowed);
      borrowed = nullptr;
    }
    if (removing.valid()) { IGNORE_RETURN removing.get(); }
    if (nullptr != detached) { ob_delete(detached); }
    for (uint64_t id : ids) { IGNORE_RETURN dtl.destroy_channel(id); }
    EXPECT_EQ(OB_SUCCESS, dtl.destroy());
  }
  ObDtl dtl;
  ObDtlChannel *borrowed = nullptr;
  ObDtlChannel *detached = nullptr;
  std::future<int> removing;
  std::vector<uint64_t> ids;
};
}

TEST(DtlLifecycle, RegisteredAndDetachedChannelsKeepProvidersAliveAcrossReinitialization)
{
  ScopedDtl scoped;
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, scoped.dtl.init());
    const uint64_t id = ObDtlChannel::generate_id();
    ObDtlChannel *created = nullptr;
    ASSERT_EQ(OB_SUCCESS, scoped.dtl.create_local_channel(id, created));
    ASSERT_NE(nullptr, created);
    scoped.ids.push_back(id);
    EXPECT_EQ(OB_INIT_TWICE, scoped.dtl.init());
    ASSERT_EQ(OB_SUCCESS, scoped.dtl.get_channel(id, scoped.borrowed));
    EXPECT_EQ(created, scoped.borrowed);
    EXPECT_EQ(OB_EAGAIN, scoped.dtl.check_channels_drained());
    EXPECT_EQ(OB_EAGAIN, scoped.dtl.destroy());
    EXPECT_EQ(id, scoped.borrowed->get_id());
    ASSERT_EQ(OB_SUCCESS, scoped.dtl.release_channel(scoped.borrowed));
    scoped.borrowed = nullptr;
    ASSERT_EQ(OB_SUCCESS, scoped.dtl.remove_channel(id, scoped.detached));
    ASSERT_EQ(created, scoped.detached);
    ObDtlChannel *missing = nullptr;
    EXPECT_EQ(OB_HASH_NOT_EXIST, scoped.dtl.get_channel(id, missing));
    // No channel is registered now, but the detached object's queues may still
    // need DFC and Runtime until its derived destructors finish.
    EXPECT_EQ(OB_EAGAIN, scoped.dtl.check_channels_drained());
    EXPECT_EQ(OB_EAGAIN, scoped.dtl.destroy());
    EXPECT_EQ(id, scoped.detached->get_id());
    ob_delete(scoped.detached);
    ASSERT_EQ(OB_SUCCESS, scoped.dtl.check_channels_drained());
    ASSERT_EQ(OB_SUCCESS, scoped.dtl.destroy());
    EXPECT_EQ(OB_SUCCESS, scoped.dtl.destroy());
    EXPECT_EQ(OB_NOT_INIT, scoped.dtl.get_channel(id, missing));
  }
}

TEST(DtlLifecycle, ARemovedChannelWaitingForPinsStillPreventsProviderShutdown)
{
  using namespace std::chrono_literals;
  ScopedDtl scoped;
  ASSERT_EQ(OB_SUCCESS, scoped.dtl.init());
  const uint64_t id = ObDtlChannel::generate_id();
  ObDtlChannel *created = nullptr;
  ASSERT_EQ(OB_SUCCESS, scoped.dtl.create_local_channel(id, created));
  scoped.ids.push_back(id);
  ASSERT_EQ(OB_SUCCESS, scoped.dtl.get_channel(id, scoped.borrowed));
  scoped.removing = std::async(std::launch::async, [&scoped, id] {
    return scoped.dtl.destroy_channel(id);
  });
  int lookup = OB_SUCCESS;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  do {
    ObDtlChannel *transient = nullptr;
    lookup = scoped.dtl.get_channel(id, transient);
    if (OB_SUCCESS == lookup) {
      ASSERT_EQ(OB_SUCCESS, scoped.dtl.release_channel(transient));
      std::this_thread::sleep_for(1ms);
    }
  } while (OB_SUCCESS == lookup && std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(OB_HASH_NOT_EXIST, lookup);
  EXPECT_EQ(std::future_status::timeout, scoped.removing.wait_for(30ms));
  EXPECT_EQ(OB_EAGAIN, scoped.dtl.check_channels_drained());
  EXPECT_EQ(OB_EAGAIN, scoped.dtl.destroy());
  ASSERT_EQ(OB_SUCCESS, scoped.dtl.release_channel(scoped.borrowed));
  scoped.borrowed = nullptr;
  ASSERT_EQ(std::future_status::ready, scoped.removing.wait_for(5s));
  ASSERT_EQ(OB_SUCCESS, scoped.removing.get());
  EXPECT_EQ(OB_SUCCESS, scoped.dtl.destroy());
}
