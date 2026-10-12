/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <cstdlib>
#include <filesystem>
#include <string>
#include <gtest/gtest.h>
#include "storage/ob_file_system_router.h"
#include "storage/blocksstable/ob_io_bench_controller.h"
#include "share/ob_device_manager.h"
#include "share/ob_io_device_helper.h"
#include "share/ob_i_local_device_space_provider.h"
#include "share/io/ob_io_manager.h"
#include "share/io/ob_io_calibration.h"
#include "lib/thread/ob_dynamic_thread_pool.h"
#include "lib/task/ob_timer_service.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::share;

namespace
{
class ScopedRouterDirectory
{
public:
  ~ScopedRouterDirectory()
  {
    OB_FILE_SYSTEM_ROUTER.destroy();
    if (!directory.empty()) { std::filesystem::remove_all(directory); }
  }
  std::string directory;
};

class NoReservedSpace : public ObILocalDeviceSpaceProvider
{
public:
  int get_reserved_size(int64_t &bytes) const override
  {
    bytes = 0;
    return OB_SUCCESS;
  }
};

class ScopedDeviceReference
{
public:
  ~ScopedDeviceReference()
  {
    ObIODeviceWrapper::get_instance().destroy();
    if (nullptr != device) {
      if (initialized) { device->destroy(); }
      EXPECT_EQ(OB_SUCCESS, ObDeviceManager::get_instance().release_device(device));
    }
  }
  ObIODevice *device = nullptr;
  bool initialized = false;
};

class ScopedIoManager
{
public:
  ~ScopedIoManager()
  {
    ObIOManager::get_instance().destroy();
    ObSimpleThreadPoolDynamicMgr::get_instance().destroy();
    ObTimerService::get_instance().destroy();
  }
};

class ScopedCalibration
{
public:
  ~ScopedCalibration() { ObIOCalibration::get_instance().destroy(); }
};
}

TEST(IoLifecycle, FileRouterClearsPathsAfterPartialInitAndEveryShutdown)
{
  ScopedRouterDirectory scoped;
  char directory[] = "/tmp/seekdb-io-lifecycle-XXXXXX";
  char *created = mkdtemp(directory);
  ASSERT_NE(nullptr, created);
  scoped.directory = created;
  OB_FILE_SYSTEM_ROUTER.destroy();
  const std::string partial = scoped.directory + "/partial";
  // The same redo/data directory fails after the data and slog paths were set.
  EXPECT_EQ(OB_INVALID_ARGUMENT, OB_FILE_SYSTEM_ROUTER.init(partial.c_str(), partial.c_str()));
  EXPECT_STREQ("", OB_FILE_SYSTEM_ROUTER.get_data_dir());
  EXPECT_STREQ("", OB_FILE_SYSTEM_ROUTER.get_slog_dir());
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    const std::string data = scoped.directory + "/data-" + std::to_string(cycle);
    const std::string redo = scoped.directory + "/redo-" + std::to_string(cycle);
    ASSERT_EQ(OB_SUCCESS, OB_FILE_SYSTEM_ROUTER.init(data.c_str(), redo.c_str()));
    EXPECT_TRUE(std::filesystem::is_directory(data));
    EXPECT_TRUE(std::filesystem::is_directory(data + "/sstable"));
    EXPECT_STREQ(data.c_str(), OB_FILE_SYSTEM_ROUTER.get_data_dir());
    EXPECT_STREQ(redo.c_str(), OB_FILE_SYSTEM_ROUTER.get_clog_dir());
    EXPECT_EQ(OB_INIT_TWICE, OB_FILE_SYSTEM_ROUTER.init(partial.c_str(), partial.c_str()));
    EXPECT_STREQ(data.c_str(), OB_FILE_SYSTEM_ROUTER.get_data_dir());
    OB_FILE_SYSTEM_ROUTER.set_svr_seq(42);
    OB_FILE_SYSTEM_ROUTER.destroy();
    OB_FILE_SYSTEM_ROUTER.destroy();
    EXPECT_STREQ("", OB_FILE_SYSTEM_ROUTER.get_data_dir());
    EXPECT_STREQ("", OB_FILE_SYSTEM_ROUTER.get_sstable_dir());
    EXPECT_STREQ("", OB_FILE_SYSTEM_ROUTER.get_clog_dir());
    EXPECT_EQ(0, OB_FILE_SYSTEM_ROUTER.get_svr_seq());
  }
}

TEST(IoLifecycle, DeviceWrapperReturnsBorrowedDeviceOnFailedInitialization)
{
  ScopedDeviceReference scoped;
  ASSERT_EQ(OB_SUCCESS, ObDeviceManager::get_instance().init_devices_env());
  const ObStorageIdMod id(0, ObStorageUsedMod::STORAGE_USED_DATA);
  ASSERT_EQ(OB_SUCCESS, ObDeviceManager::get_local_device(
      ObString(OB_LOCAL_PREFIX), id, scoped.device));
  ASSERT_NE(nullptr, scoped.device);
  const int64_t original_refs = scoped.device->get_ref_cnt();
  ObIODOpts no_block_file_options;
  ASSERT_EQ(OB_SUCCESS, scoped.device->init(no_block_file_options));
  scoped.initialized = true;
  NoReservedSpace space;
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    // The wrapper gets this same real cached device before rejecting null data_dir.
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObIODeviceWrapper::get_instance().init(
        nullptr, "/tmp", OB_DEFAULT_MACRO_BLOCK_SIZE, 0, 128L * 1024 * 1024, space));
    EXPECT_EQ(original_refs, scoped.device->get_ref_cnt());
    // Returning a wrapper reference must preserve the independently owned device.
    ObIOCB *iocb = scoped.device->alloc_iocb();
    EXPECT_NE(nullptr, iocb);
    if (nullptr != iocb) { scoped.device->free_iocb(iocb); }
    ObIODeviceWrapper::get_instance().destroy();
    EXPECT_EQ(original_refs, scoped.device->get_ref_cnt());
  }
}

TEST(IoLifecycle, ManagerStartsAndDestroysItsRealServicesAcrossThreeCycles)
{
  ScopedIoManager scoped;
  ObIOManager &manager = ObIOManager::get_instance();
  manager.destroy();
  ASSERT_EQ(OB_SUCCESS, ObTimerService::get_instance().start());
  EXPECT_EQ(OB_INVALID_ARGUMENT, manager.init(ObIORuntimeOptions(), 0));
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, manager.init(ObIORuntimeOptions(1024L * 1024 * 1024),
        64L * 1024 * 1024, 64, 1));
    ASSERT_EQ(OB_SUCCESS, manager.start());
    {
      ObRefHolder<ObIOService> service;
      ASSERT_EQ(OB_SUCCESS, manager.get_io_service(service));
      ASSERT_NE(nullptr, service.get_ptr());
      EXPECT_TRUE(service.get_ptr()->is_working());
      EXPECT_EQ(OB_INIT_TWICE, manager.init());
      EXPECT_TRUE(service.get_ptr()->is_working());
    } // Return the real service reference before the owner waits for borrowers.
    manager.stop();
    manager.wait();
    manager.destroy();
    manager.destroy();
    EXPECT_FALSE(manager.is_inited());
    EXPECT_EQ(OB_NOT_INIT, manager.start());
  }
}

TEST(IoLifecycle, CalibrationDropsItsBorrowedControllerBetweenCycles)
{
  ObIOBenchController controller;
  ScopedCalibration scoped;
  ObIOCalibration &calibration = ObIOCalibration::get_instance();
  calibration.destroy();
  for (int cycle = 0; cycle < 3; ++cycle) {
    SCOPED_TRACE(cycle);
    ASSERT_EQ(OB_SUCCESS, calibration.init(controller));
    EXPECT_EQ(OB_INIT_TWICE, calibration.init(controller));
    int64_t start = -1;
    int64_t finish = -1;
    int status = -1;
    EXPECT_EQ(OB_SUCCESS, calibration.get_benchmark_status(start, finish, status));
    calibration.destroy();
    calibration.destroy();
    EXPECT_EQ(OB_NOT_INIT, calibration.get_benchmark_status(start, finish, status));
    EXPECT_EQ(OB_NOT_INIT, calibration.reset_io_ability());
  }
}
